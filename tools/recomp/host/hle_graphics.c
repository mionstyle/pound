// Graphics HLE. Like the Switch 1 recompilation projects (LAS Recomp), the GPU and its
// driver are not emulated: nv::InitializeGraphics becomes a no-op, so the SDK never boots
// the graphics supervisor / nvsrv, and the NVN2 API is provided by the host through
// nvnBootstrapLoader.
//
// Stage 1 (this file): every NVN2 procedure the game asks for gets a host thunk that
// logs the name and the arguments of its first calls and returns 0. This produces the
// exact list of NVN2 functions the title uses, which the host implementation then fills in.

#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char     name[96];
    uint64_t addr;
    uint64_t calls;
} nvn_proc_t;

#define MAX_PROCS 4096
static nvn_proc_t      g_procs[MAX_PROCS];
static int             g_nprocs;
static pthread_mutex_t g_procs_lock = PTHREAD_MUTEX_INITIALIZER;

static void nvn_unimplemented(cpu_t *c)
{
    nvn_proc_t *p = hle_user(c);
    uint64_t    n = __atomic_fetch_add(&p->calls, 1, __ATOMIC_RELAXED);
    if (n < 3)
        host_log("nvn: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) -> 0 [not implemented]", p->name,
                 (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                 (unsigned long long)c->x[3]);
    c->x[0] = 0;
}

static void hle_nvnDeviceGetProcAddress(cpu_t *c);

// NVN2 procedures implemented on the host (the rest log and return 0).
static const struct { const char *name; hle_fn_t fn; } k_nvn_impl[] = {
    { "nvnDeviceGetProcAddress", hle_nvnDeviceGetProcAddress },
};

static FILE *g_proc_list;

// Returns the guest address of the host procedure `name` (creating its thunk once).
static uint64_t nvn_proc(const char *name)
{
    if (!name || !*name)
        return 0;
    pthread_mutex_lock(&g_procs_lock);
    for (int i = 0; i < g_nprocs; ++i)
        if (!strcmp(g_procs[i].name, name)) {
            pthread_mutex_unlock(&g_procs_lock);
            return g_procs[i].addr;
        }
    if (g_nprocs >= MAX_PROCS)
        host_fatal("too many NVN procedures");
    nvn_proc_t *p = &g_procs[g_nprocs++];
    snprintf(p->name, sizeof p->name, "%s", name);
    pthread_mutex_unlock(&g_procs_lock);
    hle_fn_t fn = nvn_unimplemented;
    for (size_t i = 0; i < sizeof k_nvn_impl / sizeof k_nvn_impl[0]; ++i)
        if (!strcmp(k_nvn_impl[i].name, name))
            fn = k_nvn_impl[i].fn;
    p->addr = hle_thunk(p->name, fn, p);
    if (!g_proc_list) {
        const char *path = getenv("RECOMP_NVN_LIST");
        g_proc_list = fopen(path ? path : "nvn_procs.txt", "w");
    }
    if (g_proc_list) {
        fprintf(g_proc_list, "%s%s\n", name, fn == nvn_unimplemented ? "" : "  [implemented]");
        fflush(g_proc_list);
    }
    if (host_verbose)
        host_log("nvn: procedure %s -> 0x%llx", name, (unsigned long long)p->addr);
    return p->addr;
}

int nvn_proc_count(void) { return g_nprocs; }

// void* nvnBootstrapLoader(const char* name)
static void hle_nvnBootstrapLoader(cpu_t *c)
{
    c->x[0] = nvn_proc(g_str(c->x[0]));
}

// void* nvnDeviceGetProcAddress(const NVNdevice*, const char* name)
static void hle_nvnDeviceGetProcAddress(cpu_t *c)
{
    c->x[0] = nvn_proc(g_str(c->x[1]));
}

// nv::InitializeGraphics(void* memory, size_t size): the graphics driver is not booted.
static void hle_nv_InitializeGraphics(cpu_t *c)
{
    host_log("hle: nv::InitializeGraphics(0x%llx, 0x%llx) skipped (graphics are provided by the host)",
             (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
}

static void hle_nop(cpu_t *c) { (void)c; }

const hle_def_t hle_defs_graphics[] = {
    { "nvnBootstrapLoader", hle_nvnBootstrapLoader },
    { "nvnDeviceGetProcAddress", hle_nvnDeviceGetProcAddress },
    { "_ZN2nv18InitializeGraphicsEPvm", hle_nv_InitializeGraphics },
    { "_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", hle_nop },
    { "_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", hle_nop },
    { NULL, NULL },
};
