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
#include "renderer.h"
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

// Recorded operations. Most keep the call's integer arguments after the command buffer
// (a[i] = x[i + 1]), its first float arguments (f[i] = s<i>) and, for arguments that point
// to guest data (state objects, arrays), a copy of that data in p.
enum
{
    OP_RT, OP_CLEAR, OP_CALL, OP_DRAW,
    OP_PROGRAM, OP_VBUF, OP_VBUFS, OP_ATTRIB, OP_STREAM, OP_UBUF, OP_UBUFS, OP_SBUF, OP_SBUFS, OP_TEX, OP_TEXS,
    OP_TEX_POOL, OP_SMP_POOL, OP_VIEWPORT, OP_VIEWPORTS, OP_SCISSOR, OP_SCISSORS, OP_DEPTH_RANGE, OP_BLEND,
    OP_COLOR, OP_CHANNEL_MASK, OP_DEPTH_STENCIL, OP_POLYGON,
    OP_DRAW_ARRAYS, OP_DRAW_ARRAYS_INST, OP_DRAW_ELEMENTS, OP_DRAW_ELEMENTS_BV, OP_DRAW_ELEMENTS_INST,
    OP_UPDATE_UBO, OP_BLEND_COLOR,
};
#define ROP_PAYLOAD 256
typedef struct
{
    int      op;
    uint64_t a[9];
    float    f[4];
    uint32_t n;              // payload bytes
    uint8_t  p[ROP_PAYLOAD]; // copied guest data
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
    static int warned;
    if (!warned++)
        host_log("render: more than %d command buffers recording: commands are dropped", MAX_REC);
    return NULL;
}

// nvnCommandBufferFinalize(cmdbuf): its recorder slot is free again (games create and
// finalize command buffers every frame).
static void cmd_Finalize(cpu_t *c)
{
    pthread_mutex_lock(&g_rlock);
    recorder_t *r = rec_of(A(0), 0);
    if (r) {
        r->cmdbuf = 0;
        r->cur.n  = 0;
    }
    pthread_mutex_unlock(&g_rlock);
}

// Recording statistics (logged with the presents): draws recorded into command buffers,
// draws in ended lists, draws executed, commands lost for want of a recorder.
static uint64_t g_stat_rec_draws, g_stat_list_draws, g_stat_exec_draws, g_stat_lost;

static int is_draw_op(int op) { return op >= OP_DRAW_ARRAYS && op <= OP_DRAW_ELEMENTS_INST; }

static void rec_push(uint64_t cmdbuf, const rop_t *op)
{
    if (is_draw_op(op->op))
        ++g_stat_rec_draws;
    pthread_mutex_lock(&g_rlock);
    recorder_t *r = rec_of(cmdbuf, 1);
    if (r) {
        if (r->cur.n == r->cur.cap) {
            r->cur.cap = r->cur.cap ? r->cur.cap * 2 : 64;
            r->cur.ops = realloc(r->cur.ops, sizeof(rop_t) * (size_t)r->cur.cap);
        }
        r->cur.ops[r->cur.n++] = *op;
    } else {
        ++g_stat_lost;
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
    static uint64_t traced;
    recorder_t     *rr = getenv("RECOMP_LIST_TRACE") ? rec_of(A(0), 0) : NULL;
    int             nd = 0;
    for (int i = 0; rr && i < rr->cur.n; ++i) nd += is_draw_op(rr->cur.ops[i].op);
    if (rr && rr->cur.n > 1 && nd && traced++ < 300) { // (lists with state and draws)
        host_log("render: cmdbuf 0x%llx ends list 0x%llx: %d ops, %d draws (thread 0x%x)", (unsigned long long)A(0),
                 (unsigned long long)h, rr ? rr->cur.n : -1, nd, kernel_current_thread()->handle);
    }
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
        for (int i = 0; i < l->n; ++i) g_stat_list_draws += is_draw_op(l->ops[i].op);
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
    uint64_t depth = A(4); // (payload: the depth target)
    memcpy(op.p, &depth, 8);
    op.n = 8;
    rec_push(A(0), &op);
}

// Generic recording: arguments, float arguments and `len` bytes from the guest pointer in
// argument `arg` (0: none).
static void rec_generic(cpu_t *c, int code, int arg, uint32_t len)
{
    rop_t op;
    memset(&op, 0, offsetof(rop_t, p));
    op.op = code;
    for (int i = 0; i < 9; ++i) op.a[i] = c->x[i + 1];
    for (int i = 0; i < 4; ++i) op.f[i] = c->q[i].f32[0];
    static int logged = -1;
    if (logged < 0)
        logged = getenv("RECOMP_NVN_LOG_BINDS") ? 0 : 1 << 30;
    if (arg && logged < 200 && ++logged)
        host_log("nvn: bind op %d: x1 0x%llx x2 0x%llx x3 0x%llx (pointer argument %d, %u bytes)", code,
                 (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], arg, len);
    if (arg && c->x[arg] && len) {
        if (len > ROP_PAYLOAD) len = ROP_PAYLOAD;
        memcpy(op.p, mem_host(c->x[arg]), len);
        op.n = len;
    }
    rec_push(A(0), &op);
}

#define REC(name, code) static void name(cpu_t *c) { rec_generic(c, code, 0, 0); }
#define REC_COPY(name, code, arg, len) static void name(cpu_t *c) { rec_generic(c, code, arg, (uint32_t)(len)); }
// NVN signatures (the NVN1 ones, which NVN2 keeps for these procs):
REC(cmd_BindProgram, OP_PROGRAM)                                 // (cmd, program, stageBits)
REC(cmd_BindVertexBuffer, OP_VBUF)                               // (cmd, index, addr, size)
REC_COPY(cmd_BindVertexBuffers, OP_VBUFS, 3, A(2) * 16)          // (cmd, first, count, const NVNbufferRange*)

// Vertex attribute / stream states are configured through procedures the generic fallback
// records as properties of the state's address (nothing is written into the guest struct),
// so a bind resolves them into the payload: attributes as rnd_attrib_t, streams as
// rnd_stream_t. Struct sizes in guest memory: 4 bytes per attribute, 8 per stream (as NVN1).
static void cmd_BindVertexAttribState(cpu_t *c) // (cmd, count, const NVNvertexAttribState*)
{
    rop_t op;
    memset(&op, 0, offsetof(rop_t, p));
    op.op   = OP_ATTRIB;
    op.a[0] = A(1) < 16 ? A(1) : 16;
    for (uint64_t i = 0; i < op.a[0]; ++i) {
        uint64_t     s  = A(2) + 4 * i;
        rnd_attrib_t at = { (uint32_t)prop_get(s, "Format", 0), (uint32_t)prop_get(s, "Format", 1),
                            (uint32_t)prop_get(s, "StreamIndex", 0) };
        memcpy(op.p + sizeof at * i, &at, sizeof at);
    }
    op.n = (uint32_t)(sizeof(rnd_attrib_t) * op.a[0]);
    rec_push(A(0), &op);
}

static void cmd_BindVertexStreamState(cpu_t *c) // (cmd, count, const NVNvertexStreamState*)
{
    rop_t op;
    memset(&op, 0, offsetof(rop_t, p));
    op.op   = OP_STREAM;
    op.a[0] = A(1) < 16 ? A(1) : 16;
    for (uint64_t i = 0; i < op.a[0]; ++i) {
        uint64_t     s  = A(2) + 8 * i;
        rnd_stream_t st = { (uint32_t)prop_get(s, "Stride", 0), (uint32_t)prop_get(s, "Divisor", 0) };
        memcpy(op.p + sizeof st * i, &st, sizeof st);
    }
    op.n = (uint32_t)(sizeof(rnd_stream_t) * op.a[0]);
    rec_push(A(0), &op);
}
REC(cmd_BindUniformBuffer, OP_UBUF)                              // (cmd, stage, index, addr, size)
REC_COPY(cmd_BindUniformBuffers, OP_UBUFS, 4, A(3) * 16)         // (cmd, stage, first, count, const NVNbufferRange*)
REC(cmd_BindStorageBuffer, OP_SBUF)                              // (cmd, stage, index, addr, size)
REC_COPY(cmd_BindStorageBuffers, OP_SBUFS, 4, A(3) * 16)
REC(cmd_BindTexture, OP_TEX)                                     // (cmd, stage, index, handle)
REC_COPY(cmd_BindTextures, OP_TEXS, 4, A(3) * 8)                 // (cmd, stage, first, count, const handle*)
REC(cmd_SetTexturePool, OP_TEX_POOL)                             // (cmd, const NVNtexturePool*)
REC(cmd_SetSamplerPool, OP_SMP_POOL)
REC(cmd_SetViewport, OP_VIEWPORT)                                // (cmd, x, y, w, h)
REC_COPY(cmd_SetViewports, OP_VIEWPORTS, 3, A(2) * 16)           // (cmd, first, count, const float[4]*)
REC(cmd_SetScissor, OP_SCISSOR)                                  // (cmd, x, y, w, h)
REC_COPY(cmd_SetScissors, OP_SCISSORS, 3, A(2) * 16)
REC(cmd_SetDepthRange, OP_DEPTH_RANGE)                           // (cmd, float n, float f)
REC_COPY(cmd_SetBlendColor, OP_BLEND_COLOR, 1, 16)               // (cmd, const float[4])
REC(cmd_DrawArrays, OP_DRAW_ARRAYS)                              // (cmd, mode, first, count)
REC(cmd_DrawArraysInstanced, OP_DRAW_ARRAYS_INST)                // (cmd, mode, first, count, baseInstance, instances)
REC(cmd_DrawElements, OP_DRAW_ELEMENTS)                          // (cmd, mode, type, count, indexAddr)
REC(cmd_DrawElementsBaseVertex, OP_DRAW_ELEMENTS_BV)             // (... , baseVertex)
REC(cmd_DrawElementsInstanced, OP_DRAW_ELEMENTS_INST)            // (... , baseVertex, baseInstance, instances)

// Data of nvnCommandBufferUpdateUniformBuffer, copied at record time into a ring (it is
// consumed when the list executes, at most a few frames later).
#define UPDATE_RING (32u << 20)
static uint8_t *g_update_ring;
static uint64_t g_update_pos;

// (cmd, NVNbufferAddress buffer, size_t bufferSize, ptrdiff_t updateOffset, size_t updateSize, const void* data):
// the GPU writes the data into the buffer in command order.
static void cmd_UpdateUniformBuffer(cpu_t *c)
{
    uint64_t size = A(4);
    static int logged;
    if (logged < 5 && ++logged)
        host_log("nvn: UpdateUniformBuffer(buffer 0x%llx size 0x%llx, offset 0x%llx, 0x%llx bytes from 0x%llx)",
                 (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3), (unsigned long long)A(4),
                 (unsigned long long)A(5));
    if (!A(1) || !A(5) || !size || size > 0x10000 || A(3) + size > A(2))
        return;
    pthread_mutex_lock(&g_rlock);
    if (!g_update_ring)
        g_update_ring = malloc(UPDATE_RING);
    if (g_update_pos + size > UPDATE_RING)
        g_update_pos = 0;
    uint64_t at = g_update_pos;
    g_update_pos = (g_update_pos + size + 15) & ~15ull;
    memcpy(g_update_ring + at, mem_host(A(5)), size);
    pthread_mutex_unlock(&g_rlock);
    rop_t op = { OP_UPDATE_UBO, { A(1) + A(3), size, at }, { 0 } };
    rec_push(A(0), &op);
}

// ---- state objects ---------------------------------------------------------------------
//
// Blend, color, channel mask, depth/stencil and polygon states are configured through
// procedures recorded as properties of the state's address; binds resolve them (a[0..]).
// SetDefaults gives every property NVN's default value. Per-target settings
// (SetBlendEnable, SetChannelMask) are explicit so each target keeps its own property.

static void set1(uint64_t obj, const char *prop, uint64_t v) { prop_set(obj, prop, &v, 1); }

static void blend_SetDefaults(cpu_t *c)
{
    uint64_t f[4] = { 2, 1, 2, 1 }, e[2] = { 1, 1 }; // ONE, ZERO; ADD
    set1(A(0), "BlendTarget", 0);
    prop_set(A(0), "BlendFunc", f, 4);
    prop_set(A(0), "BlendEquation", e, 2);
}

static void color_SetDefaults(cpu_t *c)
{
    for (int i = 0; i < 8; ++i) {
        char k[24];
        snprintf(k, sizeof k, "BlendEnable%d", i);
        set1(A(0), k, 0);
    }
}

static void color_SetBlendEnable(cpu_t *c) // (state, int index, bool enable)
{
    char k[24];
    snprintf(k, sizeof k, "BlendEnable%d", (int)(A(1) & 7));
    set1(A(0), k, A(2) & 1);
}

static void mask_SetDefaults(cpu_t *c)
{
    for (int i = 0; i < 8; ++i) {
        char     k[24];
        uint64_t v[4] = { 1, 1, 1, 1 };
        snprintf(k, sizeof k, "ChannelMask%d", i);
        prop_set(A(0), k, v, 4);
    }
}

static void mask_SetChannelMask(cpu_t *c) // (state, int index, bool r, bool g, bool b, bool a)
{
    char     k[24];
    uint64_t v[4] = { A(2) & 1, A(3) & 1, A(4) & 1, A(5) & 1 };
    snprintf(k, sizeof k, "ChannelMask%d", (int)(A(1) & 7));
    prop_set(A(0), k, v, 4);
}

static void depth_SetDefaults(cpu_t *c)
{
    set1(A(0), "DepthTestEnable", 0);
    set1(A(0), "DepthWriteEnable", 0);
    set1(A(0), "DepthFunc", 2); // LESS
    set1(A(0), "StencilTestEnable", 0);
}

static void polygon_SetDefaults(cpu_t *c)
{
    set1(A(0), "CullFace", 0);
    set1(A(0), "FrontFace", 1); // CCW
}

static void rec_state(cpu_t *c, int code, const uint64_t *v, int n)
{
    rop_t op;
    memset(&op, 0, offsetof(rop_t, p));
    op.op = code;
    for (int i = 0; i < n && i < 9; ++i) op.a[i] = v[i];
    rec_push(A(0), &op);
}

static void cmd_BindBlendState(cpu_t *c) // (cmd, const NVNblendState*)
{
    uint64_t s    = A(1);
    uint64_t v[7] = { prop_get(s, "BlendTarget", 0), prop_get(s, "BlendFunc", 0), prop_get(s, "BlendFunc", 1),
                      prop_get(s, "BlendFunc", 2), prop_get(s, "BlendFunc", 3), prop_get(s, "BlendEquation", 0),
                      prop_get(s, "BlendEquation", 1) };
    rec_state(c, OP_BLEND, v, 7);
}

static void cmd_BindColorState(cpu_t *c)
{
    uint64_t v[1] = { 0 };
    for (int i = 0; i < 8; ++i) {
        char k[24];
        snprintf(k, sizeof k, "BlendEnable%d", i);
        v[0] |= (prop_get(A(1), k, 0) & 1) << i;
    }
    rec_state(c, OP_COLOR, v, 1);
}

static void cmd_BindChannelMaskState(cpu_t *c)
{
    uint64_t v[1] = { 0 };
    for (int i = 0; i < 8; ++i) {
        char k[24];
        snprintf(k, sizeof k, "ChannelMask%d", i);
        pthread_mutex_lock(&g_props_lock);
        prop_t  *p = prop_find(A(1), name_key(k), 0);
        uint64_t m = p ? (p->v[0] & 1) | (p->v[1] & 1) << 1 | (p->v[2] & 1) << 2 | (p->v[3] & 1) << 3 : 0xF;
        pthread_mutex_unlock(&g_props_lock);
        v[0] |= m << (4 * i);
    }
    rec_state(c, OP_CHANNEL_MASK, v, 1);
}

static void cmd_BindDepthStencilState(cpu_t *c)
{
    uint64_t s    = A(1);
    uint64_t v[3] = { prop_get(s, "DepthTestEnable", 0), prop_get(s, "DepthWriteEnable", 0), prop_get(s, "DepthFunc", 0) };
    rec_state(c, OP_DEPTH_STENCIL, v, 3);
}

static void cmd_BindPolygonState(cpu_t *c)
{
    uint64_t v[2] = { prop_get(A(1), "CullFace", 0), prop_get(A(1), "FrontFace", 0) };
    rec_state(c, OP_POLYGON, v, 2);
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
    static uint64_t traced;
    if (getenv("RECOMP_LIST_TRACE") && traced++ < 100)
        host_log("render: cmdbuf 0x%llx calls %llu lists, first 0x%llx", (unsigned long long)A(0), (unsigned long long)A(1),
                 (unsigned long long)(A(1) ? rd64(A(2)) : 0));
    for (uint64_t i = 0; i < A(1); ++i) {
        rop_t op = { OP_CALL, { rd64(A(2) + 8 * i) }, { 0 } };
        rec_push(A(0), &op);
    }
}

static uint64_t    g_targets[8]; // currently bound color targets (queue state)
static uint64_t    g_depth_target;
static uint64_t    g_draws_this_frame;
static rnd_state_t g_st;         // queue state seen by draws
static int      g_gpu = -1;   // the host GPU renderer is active (renderer.h), decided at the first submit

static int gpu_on(void)
{
    if (g_gpu < 0)
        g_gpu = rnd_init();
    return g_gpu;
}

// The renderer's description of a guest NVNtexture (recorded builder properties).
static void tex_desc(uint64_t tex, rnd_tex_t *d)
{
    uint64_t pool = prop_get(tex, "Storage", 0);
    d->obj    = tex;
    d->addr   = pool ? prop_get(pool, "Storage", 0) + prop_get(tex, "Storage", 1) : 0;
    d->w      = (uint32_t)prop_get(tex, "Size", 0);
    d->h      = (uint32_t)prop_get(tex, "Size", 1);
    d->depth  = (uint32_t)prop_get(tex, "Size", 2);
    d->levels = (uint32_t)prop_get(tex, "Levels", 0);
    d->format = (uint32_t)prop_get(tex, "Format", 0);
    if (!d->depth) d->depth = 1;
    if (!d->levels || d->levels > 16) d->levels = 1;
}

// Texture pool registrations: texture id -> guest NVNtexture (one pool assumed; DKB has one).
#define POOL_IDS (1u << 20)
static uint64_t *g_pool_tex;

static void texpool_RegisterTexture(cpu_t *c) // (pool, int id, const NVNtexture*, const NVNtextureView*)
{
    if (!g_pool_tex)
        g_pool_tex = calloc(POOL_IDS, sizeof *g_pool_tex);
    if (A(1) < POOL_IDS)
        g_pool_tex[A(1)] = A(2);
}

int nvn_texture_of_handle(uint64_t handle, rnd_tex_t *out)
{
    uint64_t tex = g_pool_tex ? g_pool_tex[handle & (POOL_IDS - 1)] : 0;
    if (!tex)
        return 0;
    tex_desc(tex, out);
    return 1;
}

static uint8_t to_u8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }

// Shader code address per stage of a program (set_shaders records Stage<i> = { code, ctrl }).
// The stage of a blob comes from its header: byte 0 = 0x81 vertex, 0x82 fragment; code
// without a header is a compute shader.
static void program_stages(uint64_t prog, uint64_t code[RND_STAGES])
{
    memset(code, 0, sizeof(uint64_t) * RND_STAGES);
    int n = (int)prop_get(prog, "NumStages", 0);
    for (int i = 0; i < n && i < 8; ++i) {
        char key[16];
        snprintf(key, sizeof key, "Stage%d", i);
        uint64_t a = prop_get(prog, key, 0);
        if (!a) continue;
        uint8_t tag = *(const uint8_t *)mem_host(a);
        int     st  = tag == 0x81 ? RND_VERTEX : tag == 0x82 ? RND_FRAGMENT : RND_COMPUTE;
        code[st]    = a;
    }
}

static void set_range(rnd_range_t *r, const uint8_t *p) // NVNbufferRange = { u64 address, u64 size }
{
    memcpy(&r->addr, p, 8);
    memcpy(&r->size, p + 8, 8);
}

// Applies a state operation to g_st; returns 1 for a draw (filled into *d).
static int apply_state(const rop_t *op, rnd_draw_t *d)
{
    const uint64_t *a = op->a;
    switch (op->op) {
    case OP_PROGRAM:
        g_st.program = a[0];
        program_stages(a[0], g_st.code);
        break;
    case OP_VBUF:
        if (a[0] < 16) g_st.vbuf[a[0]] = (rnd_range_t){ a[1], a[2] };
        break;
    case OP_VBUFS:
        for (uint64_t i = 0; i < a[1] && a[0] + i < 16 && 16 * i + 16 <= op->n; ++i) set_range(&g_st.vbuf[a[0] + i], op->p + 16 * i);
        break;
    case OP_ATTRIB:
        g_st.nattrib = (int)a[0];
        memset(g_st.attrib, 0, sizeof g_st.attrib);
        memcpy(g_st.attrib, op->p, op->n);
        break;
    case OP_STREAM:
        g_st.nstream = (int)a[0];
        memset(g_st.stream, 0, sizeof g_st.stream);
        memcpy(g_st.stream, op->p, op->n);
        break;
    case OP_UBUF:
        if (a[0] < RND_STAGES && a[1] < 18) g_st.ubuf[a[0]][a[1]] = (rnd_range_t){ a[2], a[3] };
        break;
    case OP_UBUFS:
        for (uint64_t i = 0; a[0] < RND_STAGES && i < a[2] && a[1] + i < 18 && 16 * i + 16 <= op->n; ++i)
            set_range(&g_st.ubuf[a[0]][a[1] + i], op->p + 16 * i);
        break;
    case OP_SBUF:
        if (a[0] < RND_STAGES && a[1] < 16) g_st.sbuf[a[0]][a[1]] = (rnd_range_t){ a[2], a[3] };
        break;
    case OP_SBUFS:
        for (uint64_t i = 0; a[0] < RND_STAGES && i < a[2] && a[1] + i < 16 && 16 * i + 16 <= op->n; ++i)
            set_range(&g_st.sbuf[a[0]][a[1] + i], op->p + 16 * i);
        break;
    case OP_TEX:
        if (a[0] < RND_STAGES && a[1] < 32) g_st.tex[a[0]][a[1]] = a[2];
        break;
    case OP_TEXS:
        for (uint64_t i = 0; a[0] < RND_STAGES && i < a[2] && a[1] + i < 32 && 8 * i + 8 <= op->n; ++i)
            memcpy(&g_st.tex[a[0]][a[1] + i], op->p + 8 * i, 8);
        break;
    case OP_TEX_POOL: g_st.tex_pool = a[0]; break;
    case OP_SMP_POOL: g_st.smp_pool = a[0]; break;
    case OP_VIEWPORT:
        for (int i = 0; i < 4; ++i) g_st.viewport[i] = (float)(int32_t)a[i];
        break;
    case OP_VIEWPORTS:
        if (a[0] == 0 && op->n >= 16) memcpy(g_st.viewport, op->p, 16);
        break;
    case OP_SCISSOR:
        for (int i = 0; i < 4; ++i) g_st.scissor[i] = (int32_t)a[i];
        break;
    case OP_SCISSORS:
        if (a[0] == 0 && op->n >= 16) memcpy(g_st.scissor, op->p, 16);
        break;
    case OP_DEPTH_RANGE:
        g_st.depth_range[0] = op->f[0];
        g_st.depth_range[1] = op->f[1];
        break;
    case OP_BLEND:
        g_st.blend[a[0] & 7] = (rnd_blend_t){ (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4],
                                               (uint32_t)a[5], (uint32_t)a[6] };
        break;
    case OP_BLEND_COLOR:
        if (op->n >= 16) memcpy(g_st.blend_color, op->p, 16);
        break;
    case OP_UPDATE_UBO:
        memcpy(mem_host(a[0]), g_update_ring + a[2], a[1]);
        break;
    case OP_COLOR: g_st.blend_enable = (uint32_t)a[0]; break;
    case OP_CHANNEL_MASK: g_st.channel_mask = (uint32_t)a[0]; break;
    case OP_DEPTH_STENCIL:
        g_st.depth_test  = (uint32_t)a[0];
        g_st.depth_write = (uint32_t)a[1];
        g_st.depth_func  = (uint32_t)a[2];
        break;
    case OP_POLYGON:
        g_st.cull_face = (uint32_t)a[0];
        g_st.front_ccw = (uint32_t)a[1];
        break;
    case OP_DRAW_ARRAYS:
    case OP_DRAW_ARRAYS_INST:
        memset(d, 0, sizeof *d);
        d->mode  = (uint32_t)a[0];
        d->first = (uint32_t)a[1];
        d->count = (uint32_t)a[2];
        d->base_instance = op->op == OP_DRAW_ARRAYS_INST ? (uint32_t)a[3] : 0;
        d->instances     = op->op == OP_DRAW_ARRAYS_INST ? (uint32_t)a[4] : 1;
        return 1;
    case OP_DRAW_ELEMENTS:
    case OP_DRAW_ELEMENTS_BV:
    case OP_DRAW_ELEMENTS_INST:
        memset(d, 0, sizeof *d);
        d->indexed     = 1;
        d->mode        = (uint32_t)a[0];
        d->index_type  = (uint32_t)a[1];
        d->count       = (uint32_t)a[2];
        d->index_addr  = a[3];
        d->base_vertex = op->op == OP_DRAW_ELEMENTS ? 0 : (int32_t)a[4];
        d->base_instance = op->op == OP_DRAW_ELEMENTS_INST ? (uint32_t)a[5] : 0;
        d->instances     = op->op == OP_DRAW_ELEMENTS_INST ? (uint32_t)a[6] : 1;
        return 1;
    }
    return 0;
}

static void draw(const rnd_draw_t *d)
{
    ++g_draws_this_frame;
    ++g_stat_exec_draws;
    if (!gpu_on())
        return;
    g_st.ncolor = 0;
    for (int k = 0; k < 8; ++k) {
        if (g_targets[k]) {
            tex_desc(g_targets[k], &g_st.color[k]);
            g_st.ncolor = k + 1;
        } else {
            memset(&g_st.color[k], 0, sizeof g_st.color[k]);
        }
    }
    if (g_depth_target)
        tex_desc(g_depth_target, &g_st.depth);
    else
        memset(&g_st.depth, 0, sizeof g_st.depth);
    rnd_draw(&g_st, d);
}

static void run_list(uint64_t handle, int depth)
{
    if (depth > 8)
        return;
    rlist_t *l = &g_lists[handle % MAX_LISTS].list;
    if (g_lists[handle % MAX_LISTS].handle != handle) {
        static int logged;
        if (logged++ < 20)
            host_log("render: list handle 0x%llx not found (depth %d, slot holds 0x%llx)", (unsigned long long)handle, depth,
                     (unsigned long long)g_lists[handle % MAX_LISTS].handle);
        return;
    }
    static uint64_t runs;
    if (getenv("RECOMP_LIST_TRACE") && runs++ < 200)
        host_log("render: run list 0x%llx depth %d: %d ops", (unsigned long long)handle, depth, l->n);
    for (int i = 0; i < l->n; ++i) {
        rop_t *op = &l->ops[i];
        switch (op->op) {
        case OP_RT:
            memset(g_targets, 0, sizeof g_targets);
            for (uint64_t k = 0; k < op->a[0]; ++k) g_targets[k] = op->a[1 + k];
            memcpy(&g_depth_target, op->p, 8);
            break;
        case OP_CLEAR: {
            static int logged;
            if (logged < 40 && ++logged)
                host_log("render: clear target[%llu] = 0x%llx to (%.3f %.3f %.3f %.3f) mask 0x%llx",
                         (unsigned long long)op->a[0], (unsigned long long)g_targets[op->a[0] & 7], op->f[0],
                         op->f[1], op->f[2], op->f[3], (unsigned long long)op->a[1]);
            if (gpu_on()) {
                rnd_tex_t d;
                if (g_targets[op->a[0] & 7]) {
                    tex_desc(g_targets[op->a[0] & 7], &d);
                    rnd_clear_color(&d, (uint32_t)op->a[1], op->f);
                }
                break;
            }
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
        default: {
            rnd_draw_t d;
            if (apply_state(op, &d))
                draw(&d);
        }
        }
    }
}

static uint64_t *g_pending; // submitted command handles not executed yet
static int       g_npending, g_cap_pending;

static void run_pending(void) // (render lock held)
{
    for (int i = 0; i < g_npending; ++i)
        run_list(g_pending[i], 0);
    g_npending = 0;
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
    // Execution is deferred to the next present / finish, like a GPU running behind the
    // CPU: games write the constants of a frame's draws after submitting its commands.
    static uint64_t traced;
    if (getenv("RECOMP_LIST_TRACE") && traced++ < 50)
        host_log("render: submit %llu lists, first 0x%llx", (unsigned long long)A(1), (unsigned long long)rd64(A(2)));
    for (uint64_t i = 0; i < A(1); ++i) {
        if (g_npending == g_cap_pending) {
            g_cap_pending = g_cap_pending ? 2 * g_cap_pending : 256;
            g_pending     = realloc(g_pending, sizeof *g_pending * (size_t)g_cap_pending);
        }
        g_pending[g_npending++] = rd64(A(2) + 8 * i);
    }
    if (g_npending > MAX_LISTS / 2) // (the list ring must not wrap over pending lists)
        run_pending();
    pthread_mutex_unlock(&g_rlock);
}

static void queue_Finish(cpu_t *c) // (queue)
{
    (void)c;
    pthread_mutex_lock(&g_rlock);
    run_pending();
    pthread_mutex_unlock(&g_rlock);
}

static void queue_Present(cpu_t *c) // (queue, window, int index)
{
    static uint64_t frames;
    ++frames;
    uint64_t arr = prop_get(A(1), "Textures", 1);
    uint64_t tex = arr ? rd64(arr + 8 * (A(2) & 7)) : 0;
    pthread_mutex_lock(&g_rlock);
    run_pending();
    shadow_t *s = gpu_on() ? NULL : shadow_of(tex);
    if (g_gpu && tex) {
        rnd_tex_t d;
        tex_desc(tex, &d);
        int32_t crop[4]; // nvnWindowSetCrop(window, x, y, w, h)
        for (int i = 0; i < 4; ++i) crop[i] = (int32_t)prop_get(A(1), "Crop", i);
        rnd_present(&d, crop, frames);
    }
    if (frames == 1 || frames % 60 == 0)
        host_log("nvn: present #%llu (window texture %d, %llu draws since last log)", (unsigned long long)frames,
                 (int)A(2), (unsigned long long)g_draws_this_frame);
    if (frames % 300 == 0)
        host_log("render: draws recorded %llu, in ended lists %llu, executed %llu; commands lost %llu",
                 (unsigned long long)g_stat_rec_draws, (unsigned long long)g_stat_list_draws,
                 (unsigned long long)g_stat_exec_draws, (unsigned long long)g_stat_lost);
    if (frames % 60 == 0)
        g_draws_this_frame = 0;
    if (s)
        present_frame(s->px, s->w, s->h, frames);
    pthread_mutex_unlock(&g_rlock);
    if (frames == 120)
        hle_dump_stats(getenv("RECOMP_HLE_STATS") ? atoi(getenv("RECOMP_HLE_STATS")) : 150);
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

// Device-wide conventions the renderer needs (NVNwindowOriginMode, NVNdepthMode).
static void dev_SetWindowOriginMode(cpu_t *c)
{
    g_st.origin_upper_left = (int)A(1);
    host_log("nvn: window origin mode %d", (int)A(1));
}
static void dev_SetDepthMode(cpu_t *c)
{
    g_st.depth_zero_to_one = (int)A(1);
    host_log("nvn: depth mode %d", (int)A(1));
}
static void dev_GetWindowOriginMode(cpu_t *c) { RET(g_st.origin_upper_left); }
static void dev_GetDepthMode(cpu_t *c) { RET(g_st.depth_zero_to_one); }

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
    { "nvnCommandBufferDrawElements", cmd_DrawElements },
    { "nvnCommandBufferDrawElementsBaseVertex", cmd_DrawElementsBaseVertex },
    { "nvnCommandBufferDrawElementsInstanced", cmd_DrawElementsInstanced },
    { "nvnCommandBufferDrawArrays", cmd_DrawArrays },
    { "nvnCommandBufferDrawArraysInstanced", cmd_DrawArraysInstanced },
    { "nvnCommandBufferBindProgram", cmd_BindProgram },
    { "nvnTexturePoolRegisterTexture", texpool_RegisterTexture },
    { "nvnDeviceSetWindowOriginMode", dev_SetWindowOriginMode },
    { "nvnDeviceSetDepthMode", dev_SetDepthMode },
    { "nvnDeviceGetWindowOriginMode", dev_GetWindowOriginMode },
    { "nvnDeviceGetDepthMode", dev_GetDepthMode },
    { "nvnCommandBufferBindVertexBuffer", cmd_BindVertexBuffer },
    { "nvnCommandBufferBindVertexBuffers", cmd_BindVertexBuffers },
    { "nvnCommandBufferBindVertexAttribState", cmd_BindVertexAttribState },
    { "nvnCommandBufferBindVertexStreamState", cmd_BindVertexStreamState },
    { "nvnCommandBufferBindUniformBuffer", cmd_BindUniformBuffer },
    { "nvnCommandBufferBindUniformBuffers", cmd_BindUniformBuffers },
    { "nvnCommandBufferBindStorageBuffer", cmd_BindStorageBuffer },
    { "nvnCommandBufferBindStorageBuffers", cmd_BindStorageBuffers },
    { "nvnCommandBufferBindTexture", cmd_BindTexture },
    { "nvnCommandBufferBindTextures", cmd_BindTextures },
    { "nvnCommandBufferSetTexturePool", cmd_SetTexturePool },
    { "nvnCommandBufferSetSamplerPool", cmd_SetSamplerPool },
    { "nvnCommandBufferSetViewport", cmd_SetViewport },
    { "nvnCommandBufferSetViewports", cmd_SetViewports },
    { "nvnCommandBufferSetScissor", cmd_SetScissor },
    { "nvnCommandBufferSetScissors", cmd_SetScissors },
    { "nvnCommandBufferSetDepthRange", cmd_SetDepthRange },
    { "nvnCommandBufferBindBlendState", cmd_BindBlendState },
    { "nvnCommandBufferBindColorState", cmd_BindColorState },
    { "nvnCommandBufferBindChannelMaskState", cmd_BindChannelMaskState },
    { "nvnCommandBufferBindDepthStencilState", cmd_BindDepthStencilState },
    { "nvnCommandBufferBindPolygonState", cmd_BindPolygonState },
    { "nvnCommandBufferSetBlendColor", cmd_SetBlendColor },
    { "nvnBlendStateSetDefaults", blend_SetDefaults },
    { "nvnColorStateSetDefaults", color_SetDefaults },
    { "nvnColorStateSetBlendEnable", color_SetBlendEnable },
    { "nvnChannelMaskStateSetDefaults", mask_SetDefaults },
    { "nvnChannelMaskStateSetChannelMask", mask_SetChannelMask },
    { "nvnDepthStencilStateSetDefaults", depth_SetDefaults },
    { "nvnPolygonStateSetDefaults", polygon_SetDefaults },
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
    { "nvnQueueFinish", queue_Finish },
    { "nvnCommandBufferFinalize", cmd_Finalize },
    { "nvnCommandBufferUpdateUniformBuffer", cmd_UpdateUniformBuffer },
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
    // $RECOMP_NVN_LOG=<text>: every call of a generic procedure whose name contains <text>
    // (first 400), e.g. RECOMP_NVN_LOG=VertexAttribState.
    static const char *want = (const char *)1;
    if (want == (const char *)1)
        want = getenv("RECOMP_NVN_LOG");
    static int logged;
    if (want && strstr(g->name, want) && logged < 400 && ++logged)
        host_log("nvn: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)", g->name, (unsigned long long)A(0),
                 (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3), (unsigned long long)A(4));
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
