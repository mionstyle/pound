// HLE: replace guest functions by host implementations, by symbol name.
//
// Exported symbols of the loaded modules are read from their dynamic symbol tables.
// A replaced function is reached either through recomp_dispatch (indirect calls, PLT
// stubs, calls into code that is not compiled) or, inside the interpreter, through a
// code hook at its address. Functions that only exist on the host (for example the
// procedure addresses handed out by nvnBootstrapLoader) get an address in a reserved
// "thunk" region so the guest can call them like any other function pointer.

#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint64_t    addr;
    hle_fn_t    fn;
    const char *name;
    void       *user;
    uint64_t    calls;
} hle_entry_t;

#define MAX_HLE 8192
static hle_entry_t     g_hle[MAX_HLE]; // sorted by address
static int             g_hle_count;
static pthread_mutex_t g_hle_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_thunk_next = HLE_THUNK_BASE;

// ---- module symbols ------------------------------------------------------------

typedef struct { const char *name; uint64_t addr; } sym_t;
static sym_t *g_syms;
static int    g_nsyms;

static int sym_cmp(const void *a, const void *b) { return strcmp(((const sym_t *)a)->name, ((const sym_t *)b)->name); }

static void read_module_symbols(const module_t *m)
{
    uint64_t base = m->base;
    uint64_t mod0 = base + rd32(base + 4);
    if (rd32(mod0) != 0x30444F4D) // "MOD0"
        return;
    uint64_t dyn = mod0 + (uint64_t)(int64_t)(int32_t)rd32(mod0 + 4);
    uint64_t symtab = 0, strtab = 0, hash = 0;
    for (uint64_t p = dyn;; p += 16) {
        int64_t  tag = (int64_t)rd64(p);
        uint64_t val = rd64(p + 8);
        if (tag == 0) break;
        if (tag == 6) symtab = val;
        if (tag == 5) strtab = val;
        if (tag == 4) hash = val;
    }
    if (!symtab || !strtab)
        return;
    uint64_t count = hash ? rd32(base + hash + 4) : (strtab > symtab ? (strtab - symtab) / 24 : 0);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t e = base + symtab + 24 * i;
        uint32_t name = rd32(e);
        uint16_t shndx = (uint16_t)(rd32(e + 4) >> 16);
        uint64_t value = rd64(e + 8);
        if (!name || !shndx)
            continue; // imports
        g_syms = realloc(g_syms, sizeof *g_syms * (size_t)(g_nsyms + 1));
        g_syms[g_nsyms++] = (sym_t){ (const char *)mem_host(base + strtab + name), base + value };
    }
}

uint64_t hle_symbol_address(const char *name)
{
    sym_t key = { name, 0 };
    sym_t *s  = bsearch(&key, g_syms, (size_t)g_nsyms, sizeof *g_syms, sym_cmp);
    return s ? s->addr : 0;
}

const char *hle_symbol_name(uint64_t addr)
{
    for (int i = 0; i < g_nsyms; ++i)
        if (g_syms[i].addr == addr)
            return g_syms[i].name;
    return NULL;
}

// ---- registry ----------------------------------------------------------------------

static void insert(uint64_t addr, hle_fn_t fn, const char *name, void *user)
{
    pthread_mutex_lock(&g_hle_lock);
    if (g_hle_count >= MAX_HLE)
        host_fatal("too many HLE functions");
    int i = g_hle_count++;
    while (i > 0 && g_hle[i - 1].addr > addr) {
        g_hle[i] = g_hle[i - 1];
        --i;
    }
    g_hle[i] = (hle_entry_t){ addr, fn, name, user, 0 };
    pthread_mutex_unlock(&g_hle_lock);
}

static hle_entry_t *find(uint64_t addr)
{
    int lo = 0, hi = g_hle_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_hle[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < g_hle_count && g_hle[lo].addr == addr) ? &g_hle[lo] : NULL;
}

uint64_t hle_thunk(const char *name, hle_fn_t fn, void *user)
{
    pthread_mutex_lock(&g_hle_lock);
    uint64_t a = g_thunk_next;
    g_thunk_next += 4;
    pthread_mutex_unlock(&g_hle_lock);
    if (a + 4 > HLE_THUNK_BASE + HLE_THUNK_SIZE)
        host_fatal("HLE thunk region full");
    wr32(a, 0xD65F03C0); // ret, in case something executes it directly
    insert(a, fn, name, user);
    return a;
}

void *hle_user(cpu_t *c) { return c->hle_user; }

bool hle_try(cpu_t *c, uint64_t target)
{
    hle_entry_t *e = find(target);
    if (!e)
        return false;
    if (e->calls++ == 0 && host_verbose)
        host_log("hle: %s", e->name);
    void *saved  = c->hle_user;
    c->hle_user  = e->user;
    e->fn(c);
    c->hle_user  = saved;
    return true;
}

bool hle_is_hooked(uint64_t addr) { return find(addr) != NULL; }

int hle_addresses(uint64_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_hle_count && n < max; ++i)
        if (g_hle[i].addr < HLE_THUNK_BASE || g_hle[i].addr >= HLE_THUNK_BASE + HLE_THUNK_SIZE)
            out[n++] = g_hle[i].addr;
    return n;
}

// ---- guest memory for host objects ------------------------------------------------

static uint64_t        g_heap_next = HLE_HEAP_BASE;
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t hle_alloc(uint64_t size, uint64_t align)
{
    if (!align)
        align = 16;
    pthread_mutex_lock(&g_heap_lock);
    uint64_t a = (g_heap_next + align - 1) & ~(align - 1);
    g_heap_next = a + size;
    pthread_mutex_unlock(&g_heap_lock);
    if (g_heap_next > HLE_HEAP_BASE + HLE_HEAP_SIZE)
        host_fatal("HLE heap exhausted");
    memset(mem_host(a), 0, size);
    return a;
}

// ---- definitions ------------------------------------------------------------------

extern const hle_def_t hle_defs_graphics[];
extern const hle_def_t hle_defs_vi[];
extern const hle_def_t hle_defs_audio[];
extern const hle_def_t hle_defs_hid[];

static void register_defs(const hle_def_t *defs)
{
    for (const hle_def_t *d = defs; d->name; ++d) {
        uint64_t a = hle_symbol_address(d->name);
        if (!a) {
            host_log("hle: symbol not found: %s", d->name);
            continue;
        }
        insert(a, d->fn, d->name, NULL);
    }
}

void hle_init(void)
{
    for (int i = 0; i < g_module_count; ++i)
        read_module_symbols(&g_modules[i]);
    qsort(g_syms, (size_t)g_nsyms, sizeof *g_syms, sym_cmp);
    // Not MS_CODE: rtld scans Code regions for modules.
    if (mem_map(HLE_THUNK_BASE, HLE_THUNK_SIZE, MS_STATIC, MP_R))
        host_fatal("cannot map the HLE thunk region");
    if (mem_map(HLE_HEAP_BASE, HLE_HEAP_SIZE, MS_NORMAL, MP_RW))
        host_fatal("cannot map the HLE heap");
    register_defs(hle_defs_graphics);
    register_defs(hle_defs_vi);
    register_defs(hle_defs_audio);
    register_defs(hle_defs_hid);
    host_log("hle: %d module symbols, %d functions replaced", g_nsyms, g_hle_count);
}
