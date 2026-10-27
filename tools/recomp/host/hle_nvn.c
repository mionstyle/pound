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
#include "present.h"
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
    // Values follow the NVN (Switch 1) NVNdeviceInfo order, which NVN2 appears to keep for
    // the low ids. 0/1 are checked by DKB at boot: major must be 231 (0xE7), minor >= 30.
    static const int32_t k_info[] = {
        231,     // 0  API_MAJOR_VERSION
        30,      // 1  API_MINOR_VERSION
        14,      // 2  UNIFORM_BUFFER_BINDINGS_PER_STAGE
        65536,   // 3  MAX_UNIFORM_BUFFER_SIZE
        256,     // 4  UNIFORM_BUFFER_ALIGNMENT
        8,       // 5  COLOR_BUFFER_BINDINGS
        16,      // 6  VERTEX_BUFFER_BINDINGS
        4,       // 7  TRANSFORM_FEEDBACK_BUFFER_BINDINGS
        16,      // 8  SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE
        32,      // 9  TEXTURE_BINDINGS_PER_STAGE
        16,      // 10 COUNTER_ALIGNMENT
        4,       // 11 TRANSFORM_FEEDBACK_BUFFER_ALIGNMENT
        4,       // 12 TRANSFORM_FEEDBACK_CONTROL_ALIGNMENT
        4,       // 13 INDIRECT_DRAW_ALIGNMENT
        16,      // 14 VERTEX_ATTRIBUTES
        32,      // 15 TEXTURE_DESCRIPTOR_SIZE
        32,      // 16 SAMPLER_DESCRIPTOR_SIZE
        256,     // 17 RESERVED_TEXTURE_DESCRIPTORS
        256,     // 18 RESERVED_SAMPLER_DESCRIPTORS
        4,       // 19 COMMAND_BUFFER_COMMAND_ALIGNMENT
        8,       // 20 COMMAND_BUFFER_CONTROL_ALIGNMENT
        4096,    // 21 COMMAND_BUFFER_MIN_COMMAND_SIZE
        1024,    // 22 COMMAND_BUFFER_MIN_CONTROL_SIZE
        128,     // 23 SHADER_SCRATCH_MEMORY_SCALE_FACTOR_MINIMUM
        256,     // 24 SHADER_SCRATCH_MEMORY_SCALE_FACTOR_RECOMMENDED
        4096,    // 25 SHADER_SCRATCH_MEMORY_ALIGNMENT
        0x20000, // 26 SHADER_SCRATCH_MEMORY_GRANULARITY
        16,      // 27 MAX_TEXTURE_ANISOTROPY
        1536,    // 28 MAX_COMPUTE_WORK_GROUP_SIZE_X
        1024,    // 29 MAX_COMPUTE_WORK_GROUP_SIZE_Y
        64,      // 30 MAX_COMPUTE_WORK_GROUP_SIZE_Z
        1536,    // 31 MAX_COMPUTE_WORK_GROUP_SIZE_THREADS
        65535,   // 32 MAX_COMPUTE_DISPATCH_WORK_GROUPS_X
        65535,   // 33 MAX_COMPUTE_DISPATCH_WORK_GROUPS_Y
        65535,   // 34 MAX_COMPUTE_DISPATCH_WORK_GROUPS_Z
        8,       // 35 IMAGE_BINDINGS_PER_STAGE
        1048576, // 36 MAX_TEXTURE_POOL_SIZE
        4096,    // 37 MAX_SAMPLER_POOL_SIZE
        16,      // 38 MAX_VIEWPORTS
        4096,    // 39 MEMPOOL_TEXTURE_OBJECT_PAGE_ALIGNMENT
        1,       // 40 SUPPORTS_MIN_MAX_FILTERING
        1,       // 41 SUPPORTS_STENCIL8_FORMAT
        1,       // 42 SUPPORTS_ASTC_FORMATS
        0x400000,// 43 L2_SIZE
        15,      // 44 MAX_TEXTURE_LEVELS
        2048,    // 45 MAX_TEXTURE_LAYERS
        1,       // 46 GLSLC_MAX_SUPPORTED_GPU_CODE_MAJOR_VERSION
        1,       // 47 GLSLC_MIN_SUPPORTED_GPU_CODE_MAJOR_VERSION
        99,      // 48 GLSLC_MAX_SUPPORTED_GPU_CODE_MINOR_VERSION
        0,       // 49 GLSLC_MIN_SUPPORTED_GPU_CODE_MINOR_VERSION
        1,       // 50 SUPPORTS_CONSERVATIVE_RASTER
        8,       // 51 SUBPIXEL_BITS
        8,       // 52 MAX_SUBPIXEL_BIAS_BITS
        4,       // 53 INDIRECT_DISPATCH_ALIGNMENT
        32,      // 54 ZCULL_SAVE_RESTORE_ALIGNMENT
        256,     // 55 SHADER_SCRATCH_MEMORY_COMPUTE_SCALE_FACTOR_MINIMUM
        32,      // 56 LINEAR_TEXTURE_STRIDE_ALIGNMENT
        128,     // 57 LINEAR_RENDER_TARGET_STRIDE_ALIGNMENT
        4096,    // 58 MEMORY_POOL_PAGE_SIZE
        1,       // 59 SUPPORTS_ZERO_FROM_UNMAPPED_VIRTUAL_POOL_PAGES
        4,       // 60 UNIFORM_BUFFER_UPDATE_ALIGNMENT
        16384,   // 61 MAX_TEXTURE_SIZE
        134217728, // 62 MAX_BUFFER_TEXTURE_SIZE
        2048,    // 63 MAX_3D_TEXTURE_SIZE
        16384,   // 64 MAX_CUBE_MAP_TEXTURE_SIZE
        16384,   // 65 MAX_RECTANGLE_TEXTURE_SIZE
        1,       // 66 SUPPORTS_PASSTHROUGH_GEOMETRY_SHADERS
        1,       // 67 SUPPORTS_VIEWPORT_SWIZZLE
        1,       // 68 SUPPORTS_SPARSE_TILED_PACKAGED_TEXTURES
        1,       // 69 SUPPORTS_ADVANCED_BLEND_MODES
        5,       // 70 MAX_PRESENT_INTERVAL
        1,       // 71 SUPPORTS_DRAW_TEXTURE
        1,       // 72 SUPPORTS_TARGET_INDEPENDENT_RASTERIZATION
        1,       // 73 SUPPORTS_FRAGMENT_COVERAGE_TO_COLOR
        1,       // 74 SUPPORTS_POST_DEPTH_COVERAGE
        1,       // 75 SUPPORTS_IMAGES_USING_TEXTURE_HANDLES
        1,       // 76 SUPPORTS_SAMPLE_LOCATIONS
        16,      // 77 MAX_SAMPLE_LOCATION_TABLE_ENTRIES
        1024,    // 78 SHADER_CODE_MEMORY_POOL_PADDING_SIZE
        32,      // 79 MAX_PATCH_SIZE
        0x1000,  // 80 QUEUE_COMMAND_MEMORY_GRANULARITY
        0x10000, // 81 QUEUE_COMMAND_MEMORY_MIN_SIZE
        0x100000,// 82 QUEUE_COMMAND_MEMORY_DEFAULT_SIZE
        0x1000,  // 83 QUEUE_COMPUTE_MEMORY_GRANULARITY
        0x10000, // 84 QUEUE_COMPUTE_MEMORY_MIN_SIZE
        0x40000, // 85 QUEUE_COMPUTE_MEMORY_DEFAULT_SIZE
        0x2000,  // 86 QUEUE_COMMAND_MEMORY_MIN_FLUSH_THRESHOLD
        1,       // 87 SUPPORTS_FRAGMENT_SHADER_INTERLOCK
        4,       // 88 MAX_TEXTURES_PER_WINDOW
        2,       // 89 MIN_TEXTURES_PER_WINDOW
        0,       // 90 SUPPORTS_DEBUG_LAYER
        0x4000,  // 91 QUEUE_CONTROL_MEMORY_MIN_SIZE
        0x8000,  // 92 QUEUE_CONTROL_MEMORY_DEFAULT_SIZE
        0x1000,  // 93 QUEUE_CONTROL_MEMORY_GRANULARITY
        128,     // 94 SEPARATE_TEXTURE_BINDINGS_PER_STAGE
        32,      // 95 SEPARATE_SAMPLER_BINDINGS_PER_STAGE
        0xFFFF,  // 96 DEBUG_GROUPS_MAX_DOMAIN_ID
        1,       // 97 EVENTS_SUPPORT_REDUCTION_OPERATIONS
    };
    int32_t v = pname < sizeof k_info / sizeof k_info[0] ? k_info[pname] : 0;
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
    static uint64_t total, count;
    uint64_t size = prop_get(A(0), "Storage", 1);
    total += size;
    ++count;
    if (getenv("RECOMP_NVN_TRACE") || size >= (64u << 20))
        host_log("nvn: memory pool #%llu: 0x%llx bytes at 0x%llx flags 0x%llx (total %llu MB)", (unsigned long long)count,
                 (unsigned long long)size, (unsigned long long)prop_get(A(0), "Storage", 0),
                 (unsigned long long)prop_get(A(0), "Flags", 0), (unsigned long long)(total >> 20));
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
static void tex_builder_SetAxis(cpu_t *c, int axis)
{
    uint64_t v[3] = { prop_get(A(0), "Size", 0), prop_get(A(0), "Size", 1), prop_get(A(0), "Size", 2) };
    v[axis]       = A(1);
    prop_set(A(0), "Size", v, 3);
}
static void tex_builder_SetWidth(cpu_t *c) { tex_builder_SetAxis(c, 0); }
static void tex_builder_SetHeight(cpu_t *c) { tex_builder_SetAxis(c, 1); }
static void tex_builder_SetDepth(cpu_t *c) { tex_builder_SetAxis(c, 2); }
static void tex_builder_SetLevels(cpu_t *c) { uint64_t v[1] = { A(1) }; prop_set(A(0), "Levels", v, 1); }

// Bytes per texel of an NVNformat (NVN numbering, which NVN2 keeps: RGBA8 = 37). Block
// compressed formats (BCn, ASTC: id >= 65) use at most one byte per texel.
static uint64_t format_bpp(uint64_t f)
{
    if (f == 0 || f >= 65) return 1;
    if (f <= 4 || f == 49) return 1;                       // R8*, STENCIL8
    if (f <= 9 || (f >= 13 && f <= 16) || f == 50 || (f >= 57 && f <= 60)) return 2;
    if (f <= 12 || (f >= 17 && f <= 21) || (f >= 37 && f <= 40) || f == 51 || f == 52 || f == 53 ||
        (f >= 55 && f <= 56) || (f >= 61 && f <= 64))
        return 4;
    if (f >= 25 && f <= 28) return 3;                      // RGB8*
    if ((f >= 22 && f <= 24) || (f >= 41 && f <= 45) || f == 54 || (f >= 29 && f <= 33)) return 8;
    if (f >= 34 && f <= 36) return 12;
    return 16;                                             // RGBA32*
}

// Block-compressed formats: bytes per 4x4 block (0 = not compressed). ASTC (ids above 80)
// is counted as 4x4 blocks, an upper bound for every ASTC block size.
static uint64_t format_block_bytes(uint64_t f)
{
    if (f == 65 || f == 66 || f == 69 || f == 70 || f == 73 || f == 74) return 8;  // BC1, BC4
    if (f >= 65) return 16;                                                    // BC2/3/5/6/7, ASTC
    return 0;
}

// Storage of a texture: the whole mip chain, packed, rounded to 512 bytes. The real driver
// uses block-linear tiling with similar padding; the game sizes its graphics heaps from
// these values, so they must not be much larger than on hardware.
static void tex_builder_GetStorageSize(cpu_t *c)
{
    uint64_t w = prop_get(A(0), "Size", 0), h = prop_get(A(0), "Size", 1), d = prop_get(A(0), "Size", 2);
    uint64_t levels = prop_get(A(0), "Levels", 0), f = prop_get(A(0), "Format", 0);
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    if (!levels || levels > 16) levels = 1;
    uint64_t size = 0, block = format_block_bytes(f), bpp = format_bpp(f);
    for (uint64_t l = 0; l < levels; ++l) {
        uint64_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        uint64_t bytes = block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block : lw * lh * bpp;
        size += (bytes + 0x1FF) & ~0x1FFull;
    }
    RET(size * d);
}

static void tex_builder_GetStorageAlignment(cpu_t *c) { RET(0x200); }

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

// Command buffers: memory is always "free".
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


// ---- host renderer, step 1: command recording, clears and presentation -----------------
//
// Every texture used as a render target gets a host RGBA8 image ("shadow"). Command
// buffers are recorded into host command lists, executed at nvnQueueSubmitCommands.
// Supported so far: SetRenderTargets, ClearColor, CallCommands. Draws are counted only.

typedef struct
{
    uint64_t  tex;
    int       w, h;
    uint32_t *px;
} shadow_t;

#define MAX_SHADOWS 512
static shadow_t        g_shadows[MAX_SHADOWS];
static int             g_nshadows;
static pthread_mutex_t g_rlock = PTHREAD_MUTEX_INITIALIZER;

static shadow_t *shadow_of(uint64_t tex)
{
    if (!tex)
        return NULL;
    for (int i = 0; i < g_nshadows; ++i)
        if (g_shadows[i].tex == tex)
            return &g_shadows[i];
    int w = (int)prop_get(tex, "Size", 0), h = (int)prop_get(tex, "Size", 1);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || g_nshadows >= MAX_SHADOWS)
        return NULL;
    shadow_t *s = &g_shadows[g_nshadows++];
    s->tex = tex;
    s->w   = w;
    s->h   = h;
    s->px  = calloc((size_t)w * h, 4);
    host_log("render: new target 0x%llx %dx%d format %llu", (unsigned long long)tex, w, h,
             (unsigned long long)prop_get(tex, "Format", 0));
    return s;
}

enum { OP_RT, OP_CLEAR, OP_CALL, OP_DRAW };
typedef struct
{
    int      op;
    uint64_t a[9];
    float    f[4];
} rop_t;

typedef struct
{
    rop_t *ops;
    int    n, cap;
} rlist_t;

typedef struct
{
    uint64_t cmdbuf; // recording command buffer
    rlist_t  cur;
} recorder_t;

#define MAX_REC 256
static recorder_t g_rec[MAX_REC];

#define MAX_LISTS 8192
static struct { uint64_t handle; rlist_t list; } g_lists[MAX_LISTS];
static int g_nlists;

static recorder_t *rec_of(uint64_t cmdbuf, int create)
{
    for (int i = 0; i < MAX_REC; ++i)
        if (g_rec[i].cmdbuf == cmdbuf)
            return &g_rec[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_REC; ++i)
        if (!g_rec[i].cmdbuf) {
            g_rec[i].cmdbuf = cmdbuf;
            return &g_rec[i];
        }
    return NULL;
}

static void rec_push(uint64_t cmdbuf, const rop_t *op)
{
    pthread_mutex_lock(&g_rlock);
    recorder_t *r = rec_of(cmdbuf, 1);
    if (r) {
        if (r->cur.n == r->cur.cap) {
            r->cur.cap = r->cur.cap ? r->cur.cap * 2 : 64;
            r->cur.ops = realloc(r->cur.ops, sizeof(rop_t) * (size_t)r->cur.cap);
        }
        r->cur.ops[r->cur.n++] = *op;
    }
    pthread_mutex_unlock(&g_rlock);
}

static void cmd_BeginRecording(cpu_t *c)
{
    static int logged;
    if (getenv("RECOMP_NVN_TRACE") && logged < 30 && ++logged)
        host_log("render: begin recording 0x%llx (thread 0x%x)", (unsigned long long)A(0), kernel_current_thread()->handle);
    pthread_mutex_lock(&g_rlock);
    recorder_t *r = rec_of(A(0), 1);
    if (r)
        r->cur.n = 0;
    pthread_mutex_unlock(&g_rlock);
}

static uint64_t g_cmd_handle = 0x100000;
static void cmd_EndRecording(cpu_t *c)
{
    uint64_t h = __atomic_add_fetch(&g_cmd_handle, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_rlock);
    recorder_t *r = rec_of(A(0), 0);
    int slot = (int)(h % MAX_LISTS);
    rlist_t *l = &g_lists[slot].list;
    g_lists[slot].handle = h;
    l->n = 0;
    if (r && r->cur.n) {
        if (l->cap < r->cur.n) {
            l->cap = r->cur.n;
            l->ops = realloc(l->ops, sizeof(rop_t) * (size_t)l->cap);
        }
        memcpy(l->ops, r->cur.ops, sizeof(rop_t) * (size_t)r->cur.n);
        l->n = r->cur.n;
        r->cur.n = 0;
    }
    if (g_nlists < MAX_LISTS) ++g_nlists;
    pthread_mutex_unlock(&g_rlock);
    RET(h);
}

// (cmdbuf, int numColors, const NVNtexture* const* colors, const NVNtextureView* const* views,
//  const NVNtexture* depth, const NVNtextureView* depthView)
static void cmd_SetRenderTargets(cpu_t *c)
{
    rop_t op = { OP_RT, { 0 }, { 0 } };
    int   n  = (int)A(1);
    if (n > 8) n = 8;
    op.a[0] = (uint64_t)n;
    for (int i = 0; i < n; ++i)
        op.a[1 + i] = A(2) ? rd64(A(2) + 8ull * i) : 0;
    rec_push(A(0), &op);
}

static void cmd_ClearColor(cpu_t *c) // (cmdbuf, int index, const float* color, int mask)
{
    rop_t op = { OP_CLEAR, { A(1), A(3) }, { 0 } };
    if (A(2))
        for (int i = 0; i < 4; ++i) {
            uint32_t b = rd32(A(2) + 4ull * i);
            memcpy(&op.f[i], &b, 4);
        }
    rec_push(A(0), &op);
}

static void cmd_CallCommands(cpu_t *c) // (cmdbuf, int count, const NVNcommandHandle* handles)
{
    for (uint64_t i = 0; i < A(1); ++i) {
        rop_t op = { OP_CALL, { rd64(A(2) + 8 * i) }, { 0 } };
        rec_push(A(0), &op);
    }
}

static void cmd_Draw(cpu_t *c)
{
    rop_t op = { OP_DRAW, { 0 }, { 0 } };
    rec_push(A(0), &op);
}

static uint64_t g_targets[8]; // currently bound color targets (queue state)
static uint64_t g_draws_this_frame;

static uint8_t to_u8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }

static void run_list(uint64_t handle, int depth)
{
    if (depth > 8)
        return;
    rlist_t *l = &g_lists[handle % MAX_LISTS].list;
    if (g_lists[handle % MAX_LISTS].handle != handle)
        return;
    for (int i = 0; i < l->n; ++i) {
        rop_t *op = &l->ops[i];
        switch (op->op) {
        case OP_RT:
            memset(g_targets, 0, sizeof g_targets);
            for (uint64_t k = 0; k < op->a[0]; ++k) g_targets[k] = op->a[1 + k];
            break;
        case OP_CLEAR: {
            static int logged;
            if (logged < 40 && ++logged)
                host_log("render: clear target[%llu] = 0x%llx to (%.3f %.3f %.3f %.3f) mask 0x%llx",
                         (unsigned long long)op->a[0], (unsigned long long)g_targets[op->a[0] & 7], op->f[0],
                         op->f[1], op->f[2], op->f[3], (unsigned long long)op->a[1]);
            shadow_t *s = shadow_of(g_targets[op->a[0] & 7]);
            if (!s) break;
            uint32_t mask = (uint32_t)op->a[1];
            uint32_t v = (uint32_t)to_u8(op->f[0]) | (uint32_t)to_u8(op->f[1]) << 8 |
                         (uint32_t)to_u8(op->f[2]) << 16 | (uint32_t)to_u8(op->f[3]) << 24;
            uint32_t keep = 0;
            for (int ch = 0; ch < 4; ++ch)
                if (!(mask & (1u << ch))) keep |= 0xFFu << (8 * ch);
            for (size_t p = 0; p < (size_t)s->w * s->h; ++p)
                s->px[p] = (s->px[p] & keep) | (v & ~keep);
            break;
        }
        case OP_CALL: run_list(op->a[0], depth + 1); break;
        case OP_DRAW: ++g_draws_this_frame; break;
        }
    }
}

static void queue_SubmitCommands(cpu_t *c) // (queue, int count, const NVNcommandHandle* handles)
{
    pthread_mutex_lock(&g_rlock);
    static int logged;
    if (getenv("RECOMP_NVN_TRACE") && logged < 6 && ++logged)
        for (uint64_t i = 0; i < A(1); ++i) {
            uint64_t h = rd64(A(2) + 8 * i);
            rlist_t *l = &g_lists[h % MAX_LISTS].list;
            host_log("render: submit queue 0x%llx handle 0x%llx: %d ops (slot owner 0x%llx)", (unsigned long long)A(0),
                     (unsigned long long)h, l->n, (unsigned long long)g_lists[h % MAX_LISTS].handle);
            for (int k = 0; k < l->n && k < 40; ++k)
                host_log("    op %d a0 0x%llx", l->ops[k].op, (unsigned long long)l->ops[k].a[0]);
        }
    for (uint64_t i = 0; i < A(1); ++i)
        run_list(rd64(A(2) + 8 * i), 0);
    pthread_mutex_unlock(&g_rlock);
}

static void queue_Present(cpu_t *c) // (queue, window, int index)
{
    static uint64_t frames;
    ++frames;
    uint64_t arr = prop_get(A(1), "Textures", 1);
    uint64_t tex = arr ? rd64(arr + 8 * (A(2) & 7)) : 0;
    pthread_mutex_lock(&g_rlock);
    shadow_t *s = shadow_of(tex);
    if (frames == 1 || frames % 60 == 0)
        host_log("nvn: present #%llu (window texture %d, %llu draws since last log)", (unsigned long long)frames,
                 (int)A(2), (unsigned long long)g_draws_this_frame);
    if (frames % 60 == 0)
        g_draws_this_frame = 0;
    if (s)
        present_frame(s->px, s->w, s->h, frames);
    pthread_mutex_unlock(&g_rlock);
    if (frames == 120)
        hle_dump_stats(150);
    if (frames % 300 == 0) {
        void exec_dump_profile(int top);
        exec_dump_profile(40);
    }
    RET(0);
}

// ---- shader capture ----------------------------------------------------------------------
//
// nvnProgramSetShaders(program, int count, const NVNshaderData* data): data[i] = {u64 code GPU
// address, const void* control} (+ u64 debug hash for the Ext variant, stride 24). With
// $RECOMP_SHADER_DUMP=<dir>, every new code blob is written as <dir>/<hash>.code (64 KB
// window from the code address) with its control section as <hash>.ctrl (4 KB).

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void dump_blob(const char *dir, uint64_t h, const char *ext, uint64_t addr, size_t n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%016llx.%s", dir, (unsigned long long)h, ext);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(mem_host(addr), 1, n, f);
    fclose(f);
}

static void set_shaders(cpu_t *c, int stride)
{
    const char *dir = getenv("RECOMP_SHADER_DUMP");
    int         n   = (int)A(1);
    for (int i = 0; i < n && i < 8; ++i) {
        uint64_t e = A(2) + (uint64_t)(stride * i);
        uint64_t code = rd64(e), ctrl = rd64(e + 8);
        if (!code)
            continue;
        uint64_t h = fnv64(mem_host(code), 0x400);
        uint64_t v[2] = { code, ctrl };
        char key[16];
        snprintf(key, sizeof key, "Stage%d", i);
        prop_set(A(0), key, v, 2);
        if (dir) {
            dump_blob(dir, h, "code", code, 0x10000);
            if (ctrl) dump_blob(dir, h, "ctrl", ctrl, 0x1000);
        }
    }
    uint64_t v[1] = { (uint64_t)n };
    prop_set(A(0), "NumStages", v, 1);
    RET(1);
}

static void prog_SetShaders(cpu_t *c) { set_shaders(c, 16); }
static void prog_SetShadersExt(cpu_t *c) { set_shaders(c, 24); }

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
    { "nvnTextureBuilderSetWidth", tex_builder_SetWidth },
    { "nvnTextureBuilderSetHeight", tex_builder_SetHeight },
    { "nvnTextureBuilderSetDepth", tex_builder_SetDepth },
    { "nvnCommandBufferBeginRecording", cmd_BeginRecording },
    { "nvnCommandBufferSetRenderTargets", cmd_SetRenderTargets },
    { "nvnCommandBufferClearColor", cmd_ClearColor },
    { "nvnCommandBufferCallCommands", cmd_CallCommands },
    { "nvnCommandBufferDrawElements", cmd_Draw },
    { "nvnCommandBufferDrawElementsBaseVertex", cmd_Draw },
    { "nvnCommandBufferDrawElementsInstanced", cmd_Draw },
    { "nvnCommandBufferDrawArrays", cmd_Draw },
    { "nvnCommandBufferDrawArraysInstanced", cmd_Draw },
    { "nvnQueueSubmitCommands", queue_SubmitCommands },
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
    { "nvnProgramSetShaders", prog_SetShaders },
    { "nvnProgramSetShadersExt", prog_SetShadersExt },
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
