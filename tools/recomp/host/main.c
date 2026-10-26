// recomp_run: boots a title from its exefs with the recompiled modules linked in.
//
//   recomp_run [-v|-vv] <exefs dir>

#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

int host_verbose;
static struct timespec t0;

static void stamp(char *buf, size_t n)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double s = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
    snprintf(buf, n, "%9.4f", s);
}

void host_log(const char *fmt, ...)
{
    char ts[32];
    stamp(ts, sizeof ts);
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] ", ts);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

_Noreturn void host_fatal(const char *fmt, ...)
{
    char ts[32];
    stamp(ts, sizeof ts);
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] FATAL: ", ts);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    kernel_exit(2);
}

static uint64_t read_npdm(const char *dir, uint32_t *stack_size, int *prio, int *core)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/main.npdm", dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        host_fatal("cannot open %s", path);
    static uint8_t d[0x10000];
    size_t         n = fread(d, 1, sizeof d, f);
    fclose(f);
    if (n < 0x80 || memcmp(d, "META", 4) != 0)
        host_fatal("%s: not a META file", path);
    *prio       = d[0x0E];
    *core       = d[0x0F];
    *stack_size = (uint32_t)(d[0x1C] | d[0x1D] << 8 | d[0x1E] << 16 | (uint32_t)d[0x1F] << 24);
    uint32_t aci = (uint32_t)(d[0x70] | d[0x71] << 8 | d[0x72] << 16 | (uint32_t)d[0x73] << 24);
    uint64_t pid = 0;
    memcpy(&pid, d + aci + 0x10, 8);
    char title[0x11] = {0};
    memcpy(title, d + 0x20, 0x10);
    host_log("npdm: '%s' program id %016llx, main thread prio %d core %d stack 0x%x", title,
             (unsigned long long)pid, *prio, *core, *stack_size);
    return pid;
}

static void report_guest_state(void)
{
    guest_thread_t *t = kernel_current_thread();
    if (!t)
        return;
    const cpu_t *c = &t->cpu;
    printf("guest thread 0x%x registers (compiled code keeps them in memory at -O0):\n", t->handle);
    for (int i = 0; i < 31; i += 4)
        printf("  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, (unsigned long long)c->x[i],
               i + 1, (unsigned long long)(i + 1 < 31 ? c->x[i + 1] : 0), i + 2,
               (unsigned long long)(i + 2 < 31 ? c->x[i + 2] : 0), i + 3,
               (unsigned long long)(i + 3 < 31 ? c->x[i + 3] : c->sp));
    guest_backtrace(c);
}

static void report_fault(uint8_t *a)
{
    if (a >= recomp_mem_base && a < recomp_mem_base + (1ull << GUEST_ADDR_BITS))
        printf("FATAL: guest memory fault at guest address 0x%llx (%s)\n",
               (unsigned long long)(a - recomp_mem_base), addr_describe((uint64_t)(a - recomp_mem_base)));
    else
        printf("FATAL: host fault at host address %p\n", (void *)a);
    report_guest_state();
}

#ifdef _WIN32
static LONG WINAPI on_exception(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        report_fault((uint8_t *)e->ExceptionRecord->ExceptionInformation[1]);
    else
        printf("FATAL: host exception 0x%lx\n", (unsigned long)e->ExceptionRecord->ExceptionCode);
    fflush(stdout);
    ExitProcess(4);
}

static void install_fault_handler(void) { SetUnhandledExceptionFilter(on_exception); }
#else
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)uc; (void)sig;
    report_fault(si->si_addr);
    void *bt[24];
    int   n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 1);
    fflush(stdout);
    _exit(4);
}

static void install_fault_handler(void)
{
    static uint8_t   altstack[1 << 16];
    stack_t          ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigaltstack(&ss, NULL);
    sa.sa_sigaction = on_segv;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
#endif

int main(int argc, char **argv)
{
    install_fault_handler();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; ++a)
        host_verbose += (int)strlen(argv[a]) - 1;
    if (a >= argc) {
        fprintf(stderr, "usage: %s [-v|-vv] <exefs dir> [romfs dir ...]\n", argv[0]);
        return 1;
    }
    const char *dir = argv[a];

    // RomFS roots: given explicitly after the exefs, otherwise <exefs>/../romfs and, for an
    // update laid out as <dump>/updates/<v>/exefs, the base RomFS underneath it.
    const char *roots[8];
    int         nroots = 0;
    static char base_romfs[1024], upd_romfs[1024];
    if (a + 1 < argc) {
        for (int i = a + 1; i < argc && nroots < 8; ++i)
            roots[nroots++] = argv[i];
    } else {
        // An update dumped this way carries the complete, already patched RomFS, so it is
        // used on its own; the base RomFS is the fallback.
        snprintf(upd_romfs, sizeof upd_romfs, "%s/../romfs", dir);
        snprintf(base_romfs, sizeof base_romfs, "%s/../../../base/romfs", dir);
        FILE *probe = NULL;
        char  probe_path[1100];
        snprintf(probe_path, sizeof probe_path, "%s/.listing", upd_romfs);
        probe = fopen(probe_path, "rb");
        if (probe) fclose(probe);
        roots[nroots++] = probe ? upd_romfs : base_romfs;
    }

    if (mem_init())
        host_fatal("cannot reserve the guest address space");

    uint32_t stack_size;
    int      prio, core;
    uint64_t pid = read_npdm(dir, &stack_size, &prio, &core);
    kernel_init(pid);
    loader_load_exefs(dir);
    hle_init();
    romfs_build(roots, nroots);

    stack_size        = (stack_size + 0xFFF) & ~0xFFFu;
    uint64_t stack_lo = STACK_REGION_BASE;
    if (mem_map(stack_lo, stack_size, MS_STACK, MP_RW))
        host_fatal("cannot map the main thread stack");

    uint64_t        entry = g_modules[0].base; // rtld
    guest_thread_t *t     = kernel_create_main_thread(entry, stack_lo + stack_size, prio, core);
    t->cpu.x[0] = 0;          // no exception context: normal start
    t->cpu.x[1] = t->handle;  // main thread handle
    if (host_verbose > 1)
        mem_dump_map();
    host_log("starting main thread at %s", addr_describe(entry));
    kernel_run_main_thread(t);
}
