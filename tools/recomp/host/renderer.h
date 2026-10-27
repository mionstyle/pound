#ifndef RECOMP_RENDERER_H
#define RECOMP_RENDERER_H

// Host GPU renderer interface, called by hle_nvn.c while it executes NVN command lists
// (always with hle_nvn.c's render lock held: the backend is single-threaded).
//
// Windows: Vulkan (host/vk/). Elsewhere, or when Vulkan cannot start or $RECOMP_GPU=0,
// rnd_init() returns 0 and hle_nvn.c keeps its CPU path (RGBA8 shadows, GDI / PPM output).

#include <stdint.h>

// A guest NVNtexture as the renderer sees it. `obj` (the guest object address) is the
// identity; `addr` is its storage (pool base + offset) in guest memory.
typedef struct
{
    uint64_t obj, addr;
    uint32_t w, h, depth, levels, format; // format: NVNformat (NVN numbering)
} rnd_tex_t;

typedef struct { uint64_t addr, size; } rnd_range_t;
typedef struct { uint32_t format, offset, stream; } rnd_attrib_t; // format: NVNformat (0: unused)
typedef struct { uint32_t stride, divisor; } rnd_stream_t;
// NVNblendFunc / NVNblendEquation values (as recorded by nvnBlendStateSet*)
typedef struct { uint32_t src, dst, src_a, dst_a, op, op_a; } rnd_blend_t;

// NVN shader stages (NVNshaderStage): binding tables are indexed by these.
enum { RND_VERTEX, RND_FRAGMENT, RND_GEOMETRY, RND_TESS_CONTROL, RND_TESS_EVAL, RND_COMPUTE, RND_STAGES };

// GPU state at a draw, as the guest's command buffers set it (queue state: it persists
// across command lists). State objects are resolved when they are bound.
typedef struct
{
    uint64_t    program;                     // guest NVNprogram
    uint64_t    code[RND_STAGES];            // shader code address per stage (0: none)
    rnd_range_t vbuf[16];                    // vertex buffers
    rnd_attrib_t attrib[16];                 // vertex attributes (from the bound NVNvertexAttribStates)
    rnd_stream_t stream[16];                 // vertex streams (from the bound NVNvertexStreamStates)
    int         nattrib, nstream;
    rnd_range_t ubuf[RND_STAGES][18];        // uniform buffers
    rnd_range_t sbuf[RND_STAGES][16];        // storage buffers
    uint64_t    tex[RND_STAGES][32];         // texture handles (NVNtextureHandle)
    uint64_t    tex_pool, smp_pool;          // guest NVNtexturePool / NVNsamplerPool
    float       viewport[4], depth_range[2]; // x, y, w, h; near, far
    int32_t     scissor[4];
    rnd_blend_t blend[8];                    // per render target
    uint32_t    blend_enable;                // bit per target (NVNcolorState)
    uint32_t    channel_mask;                // 4 bits (RGBA) per target (NVNchannelMaskState)
    float       blend_color[4];
    uint32_t    depth_test, depth_write, depth_func; // NVNdepthStencilState (func: NVNdepthFunc)
    uint32_t    cull_face, front_ccw;        // NVNpolygonState (NVNface: 0 none, 1 front, 2 back, 3 both)
    rnd_tex_t   color[8], depth;             // render targets (obj 0: none)
    int         ncolor;
    int         origin_upper_left;           // nvnDeviceSetWindowOriginMode (0: lower left, NVN's default)
    int         depth_zero_to_one;           // nvnDeviceSetDepthMode (0: near is -w, NVN's default)
} rnd_state_t;

typedef struct
{
    int      indexed;
    uint32_t mode;       // NVNdrawPrimitive
    uint32_t index_type; // NVNindexType (indexed draws)
    uint64_t index_addr;
    uint32_t first, count, base_instance, instances;
    int32_t  base_vertex;
} rnd_draw_t;

int  rnd_init(void); // 1: the GPU backend is active (called once, lazily)
void rnd_clear_color(const rnd_tex_t *t, uint32_t mask, const float rgba[4]);
void rnd_draw(const rnd_state_t *s, const rnd_draw_t *d);
// Shows the region crop = { x, y, w, h } of t in the window (w or h 0: all of t); ends the frame.
void rnd_present(const rnd_tex_t *t, const int32_t crop[4], uint64_t frame);

// (hle_nvn.c) The texture registered in the texture pool under a handle's texture id;
// 0 if none.
int nvn_texture_of_handle(uint64_t handle, rnd_tex_t *out);

#endif // RECOMP_RENDERER_H
