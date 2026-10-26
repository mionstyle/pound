// Execution: calls go to the recompiled function when the target is a known
// function start of a module, otherwise to the Unicorn fallback interpreter,
// which shares guest memory with the recompiled code.

#include "host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

uint64_t g_stats_compiled_calls;
uint64_t g_stats_interp_calls;

#define INTERP_RETURN_MAGIC 0x7FFFFFF000ull // never mapped: emulation stops when PC reaches it

// One interpreter instance per host thread (Unicorn engines are not thread-safe).
static __thread uc_engine *uc;
static __thread cpu_t     *uc_cpu; // cpu_t of the interpreted call in progress

// All mapped guest regions; engines created later, or touching a region mapped by
// another thread, map them lazily (see on_invalid).
typedef struct { uint64_t addr, size; } map_t;
static map_t           g_maps[4096];
static int             g_map_count;
static pthread_mutex_t g_maps_lock = PTHREAD_MUTEX_INITIALIZER;
static void            ensure_uc(void);

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
    kernel_svc(uc_cpu, (insn >> 5) & 0xFFFF);
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

static void ensure_uc(void)
{
    if (uc)
        return;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc) != UC_ERR_OK)
        host_fatal("uc_open failed");
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_MAX);
    uc_hook h1, h2;
    uc_hook_add(uc, &h1, UC_HOOK_INTR, (void *)on_interrupt, NULL, 1, 0);
    uc_hook_add(uc, &h2, UC_HOOK_MEM_INVALID, (void *)on_invalid, NULL, 1, 0);
    pthread_mutex_lock(&g_maps_lock);
    for (int i = 0; i < g_map_count; ++i)
        uc_mem_map_ptr(uc, g_maps[i].addr, g_maps[i].size, UC_PROT_ALL, recomp_mem_base + g_maps[i].addr);
    pthread_mutex_unlock(&g_maps_lock);
}

void exec_init(void) {}

void exec_map_region(uint64_t addr, uint64_t size)
{
    pthread_mutex_lock(&g_maps_lock);
    if (g_map_count >= (int)(sizeof g_maps / sizeof g_maps[0]))
        host_fatal("too many interpreter mappings");
    g_maps[g_map_count++] = (map_t){ addr, size };
    pthread_mutex_unlock(&g_maps_lock);
    if (uc)
        uc_mem_map_ptr(uc, addr, size, UC_PROT_ALL, recomp_mem_base + addr);
}

void exec_unmap_region(uint64_t addr, uint64_t size)
{
    pthread_mutex_lock(&g_maps_lock);
    for (int i = 0; i < g_map_count; ++i)
        if (g_maps[i].addr >= addr && g_maps[i].addr + g_maps[i].size <= addr + size)
            g_maps[i--] = g_maps[--g_map_count];
    pthread_mutex_unlock(&g_maps_lock);
    if (uc)
        uc_mem_unmap(uc, addr, size);
}

static void interp_call(cpu_t *c, uint64_t target)
{
    ensure_uc();
    ++g_stats_interp_calls;
    if (host_verbose > 1)
        host_log("interp  -> %s", addr_describe(target));
    cpu_t *saved = uc_cpu;
    uc_cpu        = c;
    uint64_t lr   = c->x[30];
    c->x[30]      = INTERP_RETURN_MAGIC;
    to_uc(c);
    uc_err e = uc_emu_start(uc, target, INTERP_RETURN_MAGIC, 0, 0);
    from_uc(c);
    uint64_t pc;
    uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    if (e != UC_ERR_OK || pc != INTERP_RETURN_MAGIC)
        host_fatal("interpreter stopped at %s: %s", addr_describe(pc), uc_strerror(e));
    c->x[30] = lr;
    uc_cpu   = saved;
}

void recomp_resume(cpu_t *c, uint64_t pc, uint64_t lr)
{
    ensure_uc();
    ++g_stats_interp_calls;
    if (host_verbose > 1)
        host_log("resume  -> %s (until %s)", addr_describe(pc), addr_describe(lr));
    cpu_t *saved = uc_cpu;
    uc_cpu        = c;
    to_uc(c);
    uc_err e = uc_emu_start(uc, pc, lr, 0, 0);
    from_uc(c);
    uint64_t end;
    uc_reg_read(uc, UC_ARM64_REG_PC, &end);
    if (e != UC_ERR_OK || end != lr)
        host_fatal("interpreter (resume) stopped at %s: %s", addr_describe(end), uc_strerror(e));
    uc_cpu = saved;
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
