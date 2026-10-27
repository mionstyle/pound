// Execution: calls go to the recompiled function when the target is a known
// function start of a module, otherwise to the Unicorn fallback interpreter,
// which shares guest memory with the recompiled code.

#include <time.h>
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

uint64_t g_stats_compiled_calls;
uint64_t g_stats_interp_calls;

#define INTERP_RETURN_MAGIC 0x7FFFFFF000ull // never mapped: emulation stops when PC reaches it

// Interpreter engines: a small stack per host thread (Unicorn engines are not thread-safe
// and not re-entrant). A host function called from inside the interpreter (HLE, SVC) may
// call guest code again, which then runs on the next engine of the stack.
#define MAX_DEPTH 160
static __thread uc_engine *g_ucs[MAX_DEPTH];
static __thread cpu_t     *g_cpus[MAX_DEPTH];
static __thread int        g_depth;
static __thread uint64_t   g_until[MAX_DEPTH];

// Unicorn runs its CPU single-threaded, so the atomics and exclusive accesses it emulates
// are not atomic with respect to other host threads. Interpreted code therefore runs
// under one global lock, released whenever it calls out (SVC, HLE, compiled code).
static pthread_mutex_t g_interp_lock = PTHREAD_MUTEX_INITIALIZER;
#define INTERP_UNLOCK() pthread_mutex_unlock(&g_interp_lock)
#define INTERP_LOCK()   pthread_mutex_lock(&g_interp_lock) // address where the current emulation run ends
#define uc     (g_ucs[g_depth - 1])
#define uc_cpu (g_cpus[g_depth - 1])

// All mapped guest regions; engines created later, or touching a region mapped by
// another thread, map them lazily (see on_invalid).
typedef struct { uint64_t addr, size; } map_t;
static map_t           g_maps[4096];
static int             g_map_count;
static pthread_mutex_t g_maps_lock = PTHREAD_MUTEX_INITIALIZER;
static uc_engine      *new_engine(int depth);

static const int xregs[31] = {
    UC_ARM64_REG_X0,  UC_ARM64_REG_X1,  UC_ARM64_REG_X2,  UC_ARM64_REG_X3,  UC_ARM64_REG_X4,  UC_ARM64_REG_X5,
    UC_ARM64_REG_X6,  UC_ARM64_REG_X7,  UC_ARM64_REG_X8,  UC_ARM64_REG_X9,  UC_ARM64_REG_X10, UC_ARM64_REG_X11,
    UC_ARM64_REG_X12, UC_ARM64_REG_X13, UC_ARM64_REG_X14, UC_ARM64_REG_X15, UC_ARM64_REG_X16, UC_ARM64_REG_X17,
    UC_ARM64_REG_X18, UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22, UC_ARM64_REG_X23,
    UC_ARM64_REG_X24, UC_ARM64_REG_X25, UC_ARM64_REG_X26, UC_ARM64_REG_X27, UC_ARM64_REG_X28, UC_ARM64_REG_X29,
    UC_ARM64_REG_X30,
};

static void to_uc(const cpu_t *c)
{
    for (int i = 0; i < 31; ++i)
        uc_reg_write(uc, xregs[i], &c->x[i]);
    uc_reg_write(uc, UC_ARM64_REG_SP, &c->sp);
    uint64_t nzcv = (uint64_t)c->n << 31 | (uint64_t)c->z << 30 | (uint64_t)c->cf << 29 | (uint64_t)c->v << 28;
    uc_reg_write(uc, UC_ARM64_REG_NZCV, &nzcv);
    for (int i = 0; i < 32; ++i)
        uc_reg_write(uc, UC_ARM64_REG_Q0 + i, &c->q[i]);
    uint64_t v = c->fpcr;
    uc_reg_write(uc, UC_ARM64_REG_FPCR, &v);
    v = c->fpsr;
    uc_reg_write(uc, UC_ARM64_REG_FPSR, &v);
    uc_reg_write(uc, UC_ARM64_REG_TPIDR_EL0, &c->tpidr);
    uc_reg_write(uc, UC_ARM64_REG_TPIDRRO_EL0, &c->tpidrro);
}

static void from_uc(cpu_t *c)
{
    for (int i = 0; i < 31; ++i)
        uc_reg_read(uc, xregs[i], &c->x[i]);
    uc_reg_read(uc, UC_ARM64_REG_SP, &c->sp);
    uint64_t nzcv = 0;
    uc_reg_read(uc, UC_ARM64_REG_NZCV, &nzcv);
    c->n = (nzcv >> 31) & 1; c->z = (nzcv >> 30) & 1; c->cf = (nzcv >> 29) & 1; c->v = (nzcv >> 28) & 1;
    for (int i = 0; i < 32; ++i)
        uc_reg_read(uc, UC_ARM64_REG_Q0 + i, &c->q[i]);
    uint64_t v = 0;
    uc_reg_read(uc, UC_ARM64_REG_FPCR, &v);
    c->fpcr = (uint32_t)v;
    uc_reg_read(uc, UC_ARM64_REG_FPSR, &v);
    c->fpsr = (uint32_t)v;
    uc_reg_read(uc, UC_ARM64_REG_TPIDR_EL0, &c->tpidr);
}

static void on_interrupt(uc_engine *u, uint32_t intno, void *ud)
{
    (void)ud;
    uint64_t pc;
    uc_reg_read(u, UC_ARM64_REG_PC, &pc);
    uint32_t insn = rd32(pc - 4);
    if (intno != 2 || (insn & 0xFFE0001F) != 0xD4000001) {
        from_uc(uc_cpu);
        host_fatal("interpreter: exception %u at %s (insn %08x)", intno, addr_describe(pc - 4), insn);
    }
    from_uc(uc_cpu);
    uc_cpu->pc = pc - 4;
    INTERP_UNLOCK();
    kernel_svc(uc_cpu, (insn >> 5) & 0xFFFF);
    INTERP_LOCK();
    to_uc(uc_cpu);
}

static bool on_invalid(uc_engine *u, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud)
{
    (void)size; (void)value; (void)ud;
    // A region mapped after this engine was created (possibly by another thread)?
    pthread_mutex_lock(&g_maps_lock);
    for (int i = 0; i < g_map_count; ++i)
        if (addr >= g_maps[i].addr && addr < g_maps[i].addr + g_maps[i].size) {
            map_t m = g_maps[i];
            pthread_mutex_unlock(&g_maps_lock);
            return uc_mem_map_ptr(u, m.addr, m.size, UC_PROT_ALL, recomp_mem_base + m.addr) == UC_ERR_OK;
        }
    pthread_mutex_unlock(&g_maps_lock);
    uint64_t pc;
    uc_reg_read(u, UC_ARM64_REG_PC, &pc);
    host_log("interpreter: invalid memory %s at 0x%llx, pc %s",
             type == UC_MEM_FETCH_UNMAPPED ? "fetch" : type == UC_MEM_WRITE_UNMAPPED ? "write" : "read",
             (unsigned long long)addr, addr_describe(pc));
    return false;
}

// A replaced function reached inside the interpreter: run the host version and return.
static void on_hle_code(uc_engine *u, uint64_t addr, uint32_t size, void *ud)
{
    (void)size; (void)ud;
    if (!hle_is_hooked(addr))
        return;
    from_uc(uc_cpu);
    uc_cpu->pc = addr;
    INTERP_UNLOCK();
    hle_try(uc_cpu, addr);
    INTERP_LOCK();
    to_uc(uc_cpu);
    uint64_t lr = uc_cpu->x[30];
    uc_reg_write(u, UC_ARM64_REG_PC, &lr);
    if (lr == g_until[g_depth - 1])
        uc_emu_stop(u);
}

static recomp_fn_t lookup(const module_t *m, uint64_t off);

// A basic block that starts a compiled function, reached inside the interpreter (a call or
// tail call from interpreted code): run the compiled version instead and return to LR.
// This keeps the interpreter confined to the functions that really need it.
static void on_block(uc_engine *u, uint64_t addr, uint32_t size, void *ud)
{
    (void)size; (void)ud;
    if (hle_is_hooked(addr))
        return; // replaced function: its code hook runs the host version
    for (int i = 0; i < g_module_count; ++i) {
        const module_t *m = &g_modules[i];
        if (addr < m->base || addr >= m->base + m->size)
            continue;
        static const char *only = (const char *)1;
        if (only == (const char *)1) only = getenv("RECOMP_BLOCK_ONLY");
        if (only && strcmp(only, m->name))
            return;
        recomp_fn_t fn = lookup(m, addr - m->base);
        if (!fn)
            return;
        // Bisection aid: RECOMP_BLOCK_MOD=m:r dispatches only entries with (addr/4) % m == r,
        // RECOMP_BLOCK_LOG logs each newly dispatched entry once.
        static int mod = -1, rem;
        if (mod < 0) {
            const char *e = getenv("RECOMP_BLOCK_MOD");
            mod = 0;
            if (e) sscanf(e, "%d:%d", &mod, &rem);
        }
        if (mod > 0 && (int)((addr >> 2) % (uint64_t)mod) != rem)
            return;
        if (getenv("RECOMP_BLOCK_LOG")) {
            static uint64_t seen[65536];
            uint32_t h = (uint32_t)((addr >> 2) * 2654435761u) & 65535;
            if (seen[h] != addr) { seen[h] = addr; host_log("block-dispatch %s", addr_describe(addr)); }
        }
        cpu_t *c = uc_cpu;
        from_uc(c);
        ++g_stats_compiled_calls;
        INTERP_UNLOCK();
        fn(c);
        INTERP_LOCK();
        to_uc(c);
        uint64_t lr = c->x[30];
        uc_reg_write(u, UC_ARM64_REG_PC, &lr);
        // Unicorn only checks the end address when it starts a block, not after a hook
        // moved PC: returning straight to it must stop the run explicitly.
        if (lr == g_until[g_depth - 1])
            uc_emu_stop(u);
        return;
    }
}

static uc_engine *new_engine(int depth)
{
    uc_engine *e;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &e) != UC_ERR_OK)
        host_fatal("uc_open failed");
    // Deeply nested engines (interpreted <-> compiled recursion) run little code each.
    uc_ctl_set_tcg_buffer_size(e, depth < 2 ? 128u << 20 : 8u << 20);
    uc_ctl_set_cpu_model(e, UC_CPU_ARM64_MAX);
    uc_hook h1, h2;
    uc_hook_add(e, &h1, UC_HOOK_INTR, (void *)on_interrupt, NULL, 1, 0);
    uc_hook_add(e, &h2, UC_HOOK_MEM_INVALID, (void *)on_invalid, NULL, 1, 0);
    uc_hook h3;
    uc_hook_add(e, &h3, UC_HOOK_CODE, (void *)on_hle_code, NULL, HLE_THUNK_BASE, HLE_THUNK_BASE + HLE_THUNK_SIZE - 1);
    if (getenv("RECOMP_BLOCK_DISPATCH") && g_module_count) {
        uc_hook hb;
        uint64_t lo = g_modules[0].base, hi = 0;
        for (int i = 0; i < g_module_count; ++i)
            if (g_modules[i].base + g_modules[i].size > hi) hi = g_modules[i].base + g_modules[i].size;
        uc_hook_add(e, &hb, UC_HOOK_BLOCK, (void *)on_block, NULL, lo, hi - 1);
    }
    static uint64_t hooked[4096];
    int             n = hle_addresses(hooked, 4096);
    for (int i = 0; i < n; ++i) {
        uc_hook h;
        uc_hook_add(e, &h, UC_HOOK_CODE, (void *)on_hle_code, NULL, hooked[i], hooked[i]);
    }
    pthread_mutex_lock(&g_maps_lock);
    for (int i = 0; i < g_map_count; ++i)
        uc_mem_map_ptr(e, g_maps[i].addr, g_maps[i].size, UC_PROT_ALL, recomp_mem_base + g_maps[i].addr);
    pthread_mutex_unlock(&g_maps_lock);
    return e;
}

// Pushes an engine for an interpreted call on cpu c.
static void enter(cpu_t *c)
{
    if (g_depth >= MAX_DEPTH) {
        for (int i = MAX_DEPTH - 6; i < MAX_DEPTH; ++i)
            host_log("  depth %d: until %s", i, addr_describe(g_until[i]));
        guest_backtrace(c);
        host_fatal("interpreter nesting too deep");
    }
    if (!g_ucs[g_depth])
        g_ucs[g_depth] = new_engine(g_depth);
    g_cpus[g_depth] = c;
    ++g_depth;
}

static void leave(void) { --g_depth; }

void exec_init(void) {}

void exec_map_region(uint64_t addr, uint64_t size)
{
    pthread_mutex_lock(&g_maps_lock);
    if (g_map_count >= (int)(sizeof g_maps / sizeof g_maps[0]))
        host_fatal("too many interpreter mappings");
    g_maps[g_map_count++] = (map_t){ addr, size };
    pthread_mutex_unlock(&g_maps_lock);
    for (int i = 0; i < MAX_DEPTH; ++i)
        if (g_ucs[i])
            uc_mem_map_ptr(g_ucs[i], addr, size, UC_PROT_ALL, recomp_mem_base + addr);
}

void exec_unmap_region(uint64_t addr, uint64_t size)
{
    pthread_mutex_lock(&g_maps_lock);
    for (int i = 0; i < g_map_count; ++i)
        if (g_maps[i].addr >= addr && g_maps[i].addr + g_maps[i].size <= addr + size)
            g_maps[i--] = g_maps[--g_map_count];
    pthread_mutex_unlock(&g_maps_lock);
    for (int i = 0; i < MAX_DEPTH; ++i)
        if (g_ucs[i])
            uc_mem_unmap(g_ucs[i], addr, size);
}

// ---- interpreter profile: where the time spent outside compiled code goes -------------

#define PROF_SLOTS 16384
static struct { uint64_t addr, count, ns; } g_prof[PROF_SLOTS];
static pthread_mutex_t g_prof_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_prof_on = -1;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void prof_add(uint64_t addr, uint64_t ns)
{
    pthread_mutex_lock(&g_prof_lock);
    uint32_t h = (uint32_t)((addr >> 2) * 2654435761u) % PROF_SLOTS;
    for (int k = 0; k < PROF_SLOTS; ++k, h = (h + 1) % PROF_SLOTS)
        if (g_prof[h].addr == addr || !g_prof[h].addr) {
            g_prof[h].addr = addr;
            ++g_prof[h].count;
            g_prof[h].ns += ns;
            break;
        }
    pthread_mutex_unlock(&g_prof_lock);
}

static int prof_cmp(const void *a, const void *b)
{
    uint64_t x = ((const uint64_t *)a)[2], y = ((const uint64_t *)b)[2];
    return x < y ? 1 : x > y ? -1 : 0;
}

void exec_dump_profile(int top)
{
    if (g_prof_on != 1)
        return;
    pthread_mutex_lock(&g_prof_lock);
    static uint64_t v[PROF_SLOTS][3];
    int n = 0;
    for (int i = 0; i < PROF_SLOTS; ++i)
        if (g_prof[i].addr) { v[n][0] = g_prof[i].addr; v[n][1] = g_prof[i].count; v[n][2] = g_prof[i].ns; ++n; }
    pthread_mutex_unlock(&g_prof_lock);
    qsort(v, (size_t)n, sizeof v[0], prof_cmp);
    host_log("profile: interpreter entry points by time (inclusive):");
    for (int i = 0; i < n && i < top; ++i)
        host_log("  %8.1f ms %9llu calls  %s", v[i][2] / 1e6, (unsigned long long)v[i][1], addr_describe(v[i][0]));
}

static void interp_call(cpu_t *c, uint64_t target)
{
    if (g_prof_on < 0)
        g_prof_on = getenv("RECOMP_PROFILE") != NULL;
    uint64_t t0 = g_prof_on ? now_ns() : 0;
    enter(c);
    ++g_stats_interp_calls;
    if (host_verbose > 1)
        host_log("interp  -> %s", addr_describe(target));
    uint64_t lr   = c->x[30];
    c->x[30]      = INTERP_RETURN_MAGIC;
    g_until[g_depth - 1] = INTERP_RETURN_MAGIC;
    to_uc(c);
    INTERP_LOCK();
    uc_err e = uc_emu_start(uc, target, INTERP_RETURN_MAGIC, 0, 0);
    INTERP_UNLOCK();
    from_uc(c);
    uint64_t pc;
    uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    if (e != UC_ERR_OK || pc != INTERP_RETURN_MAGIC) {
        guest_thread_t *t = kernel_current_thread();
        host_log("interpreter: thread 0x%x, tls 0x%llx, [tls+0x1f8] = 0x%llx, lr %s", t ? t->handle : 0,
                 (unsigned long long)c->tpidrro, (unsigned long long)rd64(c->tpidrro + 0x1f8),
                 addr_describe(c->x[30]));
        guest_backtrace(c);
        host_fatal("interpreter stopped at %s: %s", addr_describe(pc), uc_strerror(e));
    }
    c->x[30] = lr;
    leave();
    if (g_prof_on)
        prof_add(target, now_ns() - t0);
}

void recomp_resume(cpu_t *c, uint64_t pc, uint64_t lr)
{
    uint64_t t0 = g_prof_on == 1 ? now_ns() : 0;
    enter(c);
    ++g_stats_interp_calls;
    if (host_verbose > 1)
        host_log("resume  -> %s (until %s)", addr_describe(pc), addr_describe(lr));
    g_until[g_depth - 1] = lr;
    to_uc(c);
    INTERP_LOCK();
    uc_err e = uc_emu_start(uc, pc, lr, 0, 0);
    INTERP_UNLOCK();
    from_uc(c);
    uint64_t end;
    uc_reg_read(uc, UC_ARM64_REG_PC, &end);
    if (e != UC_ERR_OK || end != lr) {
        guest_backtrace(c);
        host_fatal("interpreter (resume) stopped at %s: %s", addr_describe(end), uc_strerror(e));
    }
    leave();
    if (g_prof_on == 1)
        prof_add(pc | 1, now_ns() - t0); // odd address marks a resume point
}

uint64_t guest_call(cpu_t *c, uint64_t fn, int nargs, const uint64_t *args)
{
    cpu_t saved = *c;
    for (int i = 0; i < nargs && i < 8; ++i)
        c->x[i] = args[i];
    recomp_dispatch(c, fn);
    uint64_t r = c->x[0];
    *c = saved;
    return r;
}

static recomp_fn_t lookup(const module_t *m, uint64_t off)
{
    size_t lo = 0, hi = m->table_size;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->table[mid].addr < off) lo = mid + 1; else hi = mid;
    }
    return (lo < m->table_size && m->table[lo].addr == off) ? m->table[lo].fn : NULL;
}

void recomp_dispatch(cpu_t *c, uint64_t target)
{
    if (hle_try(c, target))
        return;
    for (int i = 0; i < g_module_count; ++i) {
        const module_t *m = &g_modules[i];
        if (target >= m->base && target < m->base + m->size) {
            recomp_fn_t fn = lookup(m, target - m->base);
            if (fn) {
                ++g_stats_compiled_calls;
                fn(c);
                return;
            }
            break;
        }
    }
    interp_call(c, target);
}

void recomp_trap(cpu_t *c, uint64_t pc, const char *reason)
{
    (void)c;
    host_fatal("trap at %s: %s", addr_describe(pc), reason);
}

void recomp_svc(cpu_t *c, uint32_t imm) { kernel_svc(c, imm); }
