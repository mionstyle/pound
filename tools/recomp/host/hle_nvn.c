// NVN2 on the host, stage 1: a "null" device. Objects are accepted and tracked, memory
// pools / buffers / command buffers / windows behave consistently, nothing is drawn.
// The goal is to let the game's main loop run frame after frame; a renderer replaces
// the draw/submit paths later.
//
// Conventions: NVN functions take the object pointer in x0 and further arguments in
// x1..x7 (floats in q0..q7); NVNboolean results are 0/1 in x0. GPU addresses are guest
// virtual addresses (identity mapping), which keeps buffer/texture lookups trivial.
//
// Procedures without an explicit implementation fall back to name-based rules:
//   *Initialize           -> true, and the builder's recorded settings are copied to the object
//   *Set<Prop>(obj, v...) -> recorded per object
//   *Get<Prop>(obj)       -> the recorded value (0 if never set)
//   anything else         -> 0

#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- per-object property store ---------------------------------------------------

typedef struct
{
    uint64_t obj;
    uint32_t key;
    uint64_t v[4];
} prop_t;

#define PROP_BUCKETS 65536
static prop_t         *g_props[PROP_BUCKETS];
static int             g_prop_cnt[PROP_BUCKETS], g_prop_cap[PROP_BUCKETS];
static pthread_mutex_t g_props_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t name_key(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static prop_t *prop_find(uint64_t obj, uint32_t key, int create)
{
    uint32_t b = (uint32_t)((obj >> 3) * 2654435761u) % PROP_BUCKETS;
    for (int i = 0; i < g_prop_cnt[b]; ++i)
        if (g_props[b][i].obj == obj && g_props[b][i].key == key)
            return &g_props[b][i];
    if (!create)
        return NULL;
    if (g_prop_cnt[b] == g_prop_cap[b]) {
        g_prop_cap[b] = g_prop_cap[b] ? g_prop_cap[b] * 2 : 4;
        g_props[b]    = realloc(g_props[b], sizeof(prop_t) * (size_t)g_prop_cap[b]);
    }
    prop_t *p = &g_props[b][g_prop_cnt[b]++];
    memset(p, 0, sizeof *p);
    p->obj = obj;
    p->key = key;
    return p;
}

static void prop_set(uint64_t obj, const char *prop, const uint64_t *v, int n)
{
    pthread_mutex_lock(&g_props_lock);
    prop_t *p = prop_find(obj, name_key(prop), 1);
    for (int i = 0; i < n && i < 4; ++i) p->v[i] = v[i];
    pthread_mutex_unlock(&g_props_lock);
}

static uint64_t prop_get(uint64_t obj, const char *prop, int idx)
{
    pthread_mutex_lock(&g_props_lock);
    prop_t  *p = prop_find(obj, name_key(prop), 0);
    uint64_t r = p ? p->v[idx] : 0;
    pthread_mutex_unlock(&g_props_lock);
    return r;
}

// Copies every property recorded on `from` to `to` (builder -> object at Initialize).
static void prop_copy_all(uint64_t from, uint64_t to)
{
    pthread_mutex_lock(&g_props_lock);
    for (int b = 0; b < PROP_BUCKETS; ++b)
        for (int i = 0; i < g_prop_cnt[b]; ++i)
            if (g_props[b][i].obj == from) {
                prop_t src = g_props[b][i];
                prop_t *d  = prop_find(to, src.key, 1);
                memcpy(d->v, src.v, sizeof d->v);
            }
    pthread_mutex_unlock(&g_props_lock);
}

// Each builder/object remembers which builder initialised it (props are copied anyway).
#define A(i) (c->x[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))

// ---- explicit implementations ----------------------------------------------------------

typedef struct { const char *name; hle_fn_t fn; } impl_t;

static void dev_GetInteger(cpu_t *c) // (device, NVNdeviceInfo pname, int* value)
{
    uint32_t pname = (uint32_t)A(1);
    int32_t  v     = 0;
    // Values are filled in as the game's needs become known (logged once per pname).
    static uint8_t seen[1024];
    if (pname < 1024 && !seen[pname]++)
        host_log("nvn: nvnDeviceGetInteger(pname %u) -> %d", pname, v);
    wr32(A(2), (uint32_t)v);
}

static void dev_GetTimestampNs(cpu_t *c) { RET(recomp_cntpct() * 1000000000ull / recomp_cntfrq()); }

static void dev_GetTextureHandle(cpu_t *c) // (device, textureID, samplerID) -> NVNtextureHandle
{
    RET((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20));
}

// Memory pools: storage is guest memory; GPU addresses == CPU addresses.
static void pool_builder_SetStorage(cpu_t *c) // (builder, void* memory, size_t size)
{
    uint64_t v[2] = { A(1), A(2) };
    prop_set(A(0), "Storage", v, 2);
}

static void pool_Initialize(cpu_t *c) // (pool, builder) -> bool
{
    prop_copy_all(A(1), A(0));
    RET(1);
}

static void pool_Map(cpu_t *c) { RET(prop_get(A(0), "Storage", 0)); }
static void pool_GetBufferAddress(cpu_t *c) { RET(prop_get(A(0), "Storage", 0)); }
static void pool_GetSize(cpu_t *c) { RET(prop_get(A(0), "Storage", 1)); }

// Buffers: (builder, pool, offset, size)
static void buf_builder_SetStorage(cpu_t *c)
{
    uint64_t v[3] = { A(1), A(2), A(3) };
    prop_set(A(0), "Storage", v, 3);
}

static void buf_Initialize(cpu_t *c)
{
    prop_copy_all(A(1), A(0));
    RET(1);
}

static uint64_t buf_base(uint64_t buf)
{
    uint64_t pool = prop_get(buf, "Storage", 0), off = prop_get(buf, "Storage", 1);
    return prop_get(pool, "Storage", 0) + off;
}

static void buf_GetAddress(cpu_t *c) { RET(buf_base(A(0))); }
static void buf_Map(cpu_t *c) { RET(buf_base(A(0))); }
static void buf_GetSize(cpu_t *c) { RET(prop_get(A(0), "Storage", 2)); }

// Textures: storage size / alignment estimates large enough for any format.
static void tex_builder_SetSize2D(cpu_t *c) { uint64_t v[3] = { A(1), A(2), 1 }; prop_set(A(0), "Size", v, 3); }
static void tex_builder_SetSize3D(cpu_t *c) { uint64_t v[3] = { A(1), A(2), A(3) }; prop_set(A(0), "Size", v, 3); }
static void tex_builder_SetSize1D(cpu_t *c) { uint64_t v[3] = { A(1), 1, 1 }; prop_set(A(0), "Size", v, 3); }
static void tex_builder_SetLevels(cpu_t *c) { uint64_t v[1] = { A(1) }; prop_set(A(0), "Levels", v, 1); }

static void tex_builder_GetStorageSize(cpu_t *c)
{
    uint64_t w = prop_get(A(0), "Size", 0), h = prop_get(A(0), "Size", 1), d = prop_get(A(0), "Size", 2);
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint64_t size = w * h * d * 16; // 16 bytes per texel: >= every format
    size = size + size / 3;          // mip chain
    RET((size + 0xFFFF) & ~0xFFFFull);
}

static void tex_builder_GetStorageAlignment(cpu_t *c) { RET(0x10000); }

static void tex_builder_SetStorage(cpu_t *c) // (builder, pool, offset)
{
    uint64_t v[2] = { A(1), A(2) };
    prop_set(A(0), "Storage", v, 2);
}

static void tex_Initialize(cpu_t *c)
{
    prop_copy_all(A(1), A(0));
    RET(1);
}

static void tex_GetWidth(cpu_t *c) { RET(prop_get(A(0), "Size", 0)); }
static void tex_GetHeight(cpu_t *c) { RET(prop_get(A(0), "Size", 1)); }
static void tex_GetDepth(cpu_t *c) { RET(prop_get(A(0), "Size", 2)); }

// Command buffers: recording returns a non-zero handle; memory is always "free".
static uint64_t g_cmd_handle = 0x100000;
static void cmd_EndRecording(cpu_t *c) { RET(__atomic_add_fetch(&g_cmd_handle, 1, __ATOMIC_RELAXED)); }
static void cmd_GetMemoryFree(cpu_t *c) { (void)c; RET(0x100000); }

// Window: textures are acquired round-robin.
static void win_builder_SetTextures(cpu_t *c) // (builder, int count, NVNtexture* const* textures)
{
    uint64_t v[2] = { A(1), A(2) };
    prop_set(A(0), "Textures", v, 2);
}

static void win_Initialize(cpu_t *c)
{
    prop_copy_all(A(1), A(0));
    RET(1);
}

static void win_AcquireTexture(cpu_t *c) // (window, sync, int* index) -> NVNwindowAcquireTextureResult
{
    uint64_t n    = prop_get(A(0), "Textures", 0);
    uint64_t next = prop_get(A(0), "NextIndex", 0);
    wr32(A(2), (uint32_t)(n ? next % n : 0));
    uint64_t v[1] = { next + 1 };
    prop_set(A(0), "NextIndex", v, 1);
    RET(0); // success
}

static void win_GetNumTextures(cpu_t *c) { RET(prop_get(A(0), "Textures", 0)); }

static void queue_Present(cpu_t *c) // (queue, window, int index)
{
    static uint64_t frames;
    ++frames;
    if (frames == 1 || frames % 60 == 0)
        host_log("nvn: present #%llu (window texture %d)", (unsigned long long)frames, (int)A(2));
    RET(0);
}

static void sync_Wait(cpu_t *c) { RET(0); } // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
static void ret_true(cpu_t *c) { RET(1); }
static void ret_zero(cpu_t *c) { RET(0); }

static const impl_t k_impl[] = {
    { "nvnDeviceGetInteger", dev_GetInteger },
    { "nvnDeviceGetCurrentTimestampInNanoseconds", dev_GetTimestampNs },
    { "nvnDeviceGetTimestampInNanoseconds", dev_GetTimestampNs },
    { "nvnDeviceGetTextureHandle", dev_GetTextureHandle },
    { "nvnDeviceGetTexelFetchHandle", dev_GetTextureHandle },
    { "nvnDeviceGetImageHandle", dev_GetTextureHandle },
    { "nvnDeviceGetSeparateTextureHandle", dev_GetTextureHandle },
    { "nvnDeviceGetSeparateSamplerHandle", dev_GetTextureHandle },
    { "nvnMemoryPoolBuilderSetStorage", pool_builder_SetStorage },
    { "nvnMemoryPoolInitialize", pool_Initialize },
    { "nvnMemoryPoolMap", pool_Map },
    { "nvnMemoryPoolGetBufferAddress", pool_GetBufferAddress },
    { "nvnMemoryPoolGetSize", pool_GetSize },
    { "nvnBufferBuilderSetStorage", buf_builder_SetStorage },
    { "nvnBufferInitialize", buf_Initialize },
    { "nvnBufferGetAddress", buf_GetAddress },
    { "nvnBufferMap", buf_Map },
    { "nvnBufferGetSize", buf_GetSize },
    { "nvnTextureBuilderSetSize1D", tex_builder_SetSize1D },
    { "nvnTextureBuilderSetSize2D", tex_builder_SetSize2D },
    { "nvnTextureBuilderSetSize3D", tex_builder_SetSize3D },
    { "nvnTextureBuilderSetLevels", tex_builder_SetLevels },
    { "nvnTextureBuilderGetStorageSize", tex_builder_GetStorageSize },
    { "nvnTextureBuilderGetStorageAlignment", tex_builder_GetStorageAlignment },
    { "nvnTextureBuilderSetStorage", tex_builder_SetStorage },
    { "nvnTextureInitialize", tex_Initialize },
    { "nvnTextureGetWidth", tex_GetWidth },
    { "nvnTextureGetHeight", tex_GetHeight },
    { "nvnTextureGetDepth", tex_GetDepth },
    { "nvnCommandBufferEndRecording", cmd_EndRecording },
    { "nvnCommandBufferGetCommandMemoryFree", cmd_GetMemoryFree },
    { "nvnCommandBufferGetControlMemoryFree", cmd_GetMemoryFree },
    { "nvnWindowBuilderSetTextures", win_builder_SetTextures },
    { "nvnWindowInitialize", win_Initialize },
    { "nvnWindowAcquireTexture", win_AcquireTexture },
    { "nvnWindowGetNumTextures", win_GetNumTextures },
    { "nvnQueuePresentTexture", queue_Present },
    { "nvnSyncWait", sync_Wait },
    { "nvnQueueGetError", ret_zero },
    { "nvnProgramSetShaders", ret_true },
};

// ---- name-based fallback ---------------------------------------------------------------

typedef struct
{
    char        name[96];
    char        prop[64];
    int         kind; // 0 other, 1 initialize, 2 set, 3 get
    uint64_t    calls;
} generic_t;

static void generic_proc(cpu_t *c)
{
    generic_t *g = hle_user(c);
    uint64_t   n = __atomic_fetch_add(&g->calls, 1, __ATOMIC_RELAXED);
    switch (g->kind) {
    case 1:
        prop_copy_all(A(1), A(0));
        RET(1);
        break;
    case 2: {
        uint64_t v[4] = { A(1), A(2), A(3), A(4) };
        prop_set(A(0), g->prop, v, 4);
        break;
    }
    case 3:
        RET(prop_get(A(0), g->prop, 0));
        break;
    default:
        RET(0);
    }
    if (n == 0 && host_verbose)
        host_log("nvn: %s(0x%llx, 0x%llx, 0x%llx) [generic %s]", g->name, (unsigned long long)A(0),
                 (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                 g->kind == 1 ? "init" : g->kind == 2 ? "set" : g->kind == 3 ? "get" : "no-op");
}

// Returns the host implementation for an NVN procedure name, or NULL.
hle_fn_t nvn_lookup(const char *name, void **user)
{
    for (size_t i = 0; i < sizeof k_impl / sizeof k_impl[0]; ++i)
        if (!strcmp(k_impl[i].name, name)) {
            *user = NULL;
            return k_impl[i].fn;
        }
    generic_t *g = calloc(1, sizeof *g);
    snprintf(g->name, sizeof g->name, "%s", name);
    const char *set = strstr(name, "Set"), *get = strstr(name, "Get");
    size_t      len = strlen(name);
    if (len > 10 && !strcmp(name + len - 10, "Initialize")) {
        g->kind = 1;
    } else if (set && (!get || set < get)) {
        g->kind = 2;
        snprintf(g->prop, sizeof g->prop, "%s", set + 3);
    } else if (get) {
        g->kind = 3;
        snprintf(g->prop, sizeof g->prop, "%s", get + 3);
    }
    *user = g;
    return generic_proc;
}
