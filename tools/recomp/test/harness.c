// Test host for recompiled code: flat guest memory reservation, fault/trap
// recovery and function lookup. Built together with the generated sources into a
// shared library driven by difftest.py.

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "test_config.h"
#include "recomp_cpu.h"

typedef struct { uint64_t addr; recomp_fn_t fn; } recomp_entry_t;
extern const recomp_entry_t recomp_table[];
extern const size_t         recomp_table_size;

uint8_t *recomp_mem_base;
uint64_t recomp_module_base;

// Calls into [h_stub_base, h_stub_base + 0x1000) behave like a function that
// immediately returns (stand-in for imports that are not part of the build).
uint64_t h_stub_base;

enum { H_OK = 0, H_TRAP = 1, H_SEGV = 2, H_UNKNOWN_TARGET = 3, H_SVC = 4, H_TIMEOUT = 5 };

static sigjmp_buf  jb;
static char        last_msg[256];
static uint64_t    last_addr;

const char *h_last_msg(void) { return last_msg; }
uint64_t    h_last_addr(void) { return last_addr; }

void recomp_fault(uint64_t addr)
{
    last_addr = addr;
    snprintf(last_msg, sizeof last_msg, "address out of range 0x%llx", (unsigned long long)addr);
    siglongjmp(jb, H_SEGV);
}

void recomp_trap(cpu_t *c, uint64_t pc, const char *reason)
{
    (void)c;
    last_addr = pc;
    snprintf(last_msg, sizeof last_msg, "%s", reason);
    siglongjmp(jb, H_TRAP);
}

void recomp_svc(cpu_t *c, uint32_t imm)
{
    last_addr = c->pc;
    snprintf(last_msg, sizeof last_msg, "svc 0x%x", imm);
    siglongjmp(jb, H_SVC);
}

static recomp_fn_t lookup(uint64_t off)
{
    size_t lo = 0, hi = recomp_table_size;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (recomp_table[mid].addr < off) lo = mid + 1; else hi = mid;
    }
    return (lo < recomp_table_size && recomp_table[lo].addr == off) ? recomp_table[lo].fn : NULL;
}

void recomp_dispatch(cpu_t *c, uint64_t target)
{
    if (h_stub_base && target >= h_stub_base && target < h_stub_base + 0x1000)
        return;
    recomp_fn_t fn = lookup(target - recomp_module_base);
    if (!fn) {
        last_addr = target;
        snprintf(last_msg, sizeof last_msg, "call to function not in this build 0x%llx", (unsigned long long)target);
        siglongjmp(jb, H_UNKNOWN_TARGET);
    }
    fn(c);
}

static void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    if (sig == SIGALRM) {
        snprintf(last_msg, sizeof last_msg, "timeout");
        siglongjmp(jb, H_TIMEOUT);
    }
    last_addr = (uint64_t)((uint8_t *)si->si_addr - recomp_mem_base);
    snprintf(last_msg, sizeof last_msg, "segfault at guest 0x%llx", (unsigned long long)last_addr);
    siglongjmp(jb, H_SEGV);
}

int h_init(void)
{
    void *p = mmap(NULL, 1ull << RECOMP_GUEST_BITS, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    recomp_mem_base = p;

    static uint8_t altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    return 0;
}

// Makes [addr, addr+size) readable/writable and returns the host pointer.
void *h_map(uint64_t addr, uint64_t size)
{
    uint8_t *p = recomp_mem_base + addr;
    if (mprotect(p, size, PROT_READ | PROT_WRITE) != 0)
        return NULL;
    return p;
}

int h_run(uint64_t fn_off, cpu_t *c, int timeout_ms)
{
    recomp_fn_t fn = lookup(fn_off);
    if (!fn) {
        snprintf(last_msg, sizeof last_msg, "function not in build");
        return H_UNKNOWN_TARGET;
    }
    last_msg[0] = 0;
    struct itimerval t = { {0, 0}, {timeout_ms / 1000, (timeout_ms % 1000) * 1000} };
    int rc = sigsetjmp(jb, 1);
    if (rc == 0) {
        setitimer(ITIMER_REAL, &t, NULL);
        fn(c);
    }
    struct itimerval off = { {0, 0}, {0, 0} };
    setitimer(ITIMER_REAL, &off, NULL);
    return rc;
}

size_t h_cpu_size(void) { return sizeof(cpu_t); }
