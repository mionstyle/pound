// recomp_run: boots a title from its exefs with the recompiled modules linked in.
//
//   recomp_run [-v|-vv] <exefs dir>

#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

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

static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    uint8_t *a = si->si_addr;
    if (a >= recomp_mem_base && a < recomp_mem_base + (1ull << GUEST_ADDR_BITS))
        printf("FATAL: guest memory fault at guest address 0x%llx (%s)\n",
               (unsigned long long)(a - recomp_mem_base), addr_describe((uint64_t)(a - recomp_mem_base)));
    else
        printf("FATAL: host signal %d at host address %p\n", sig, (void *)a);
    void *bt[24];
    int   n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 1);
    fflush(stdout);
    _exit(4);
}

int main(int argc, char **argv)
{
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
    clock_gettime(CLOCK_MONOTONIC, &t0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; ++a)
        host_verbose += (int)strlen(argv[a]) - 1;
    if (a >= argc) {
        fprintf(stderr, "usage: %s [-v|-vv] <exefs dir>\n", argv[0]);
        return 1;
    }
    const char *dir = argv[a];

    if (mem_init())
        host_fatal("cannot reserve the guest address space");

    uint32_t stack_size;
    int      prio, core;
    uint64_t pid = read_npdm(dir, &stack_size, &prio, &core);
    kernel_init(pid);
    loader_load_exefs(dir);

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
