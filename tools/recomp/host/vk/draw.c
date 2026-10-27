// Draws: the guest's NVN draw state turned into Vulkan pipelines and commands.
//
// Shaders come from the ahead-of-time cache written by tools/recomp/sm86/batch.py
// ($RECOMP_SPV_DIR, default "spv"): <hash>.vert.spv / <hash>.frag.spv and <hash>.res, keyed
// by the same hash as the shader capture (FNV-1a 64 of the code's first 1 KB).
//
// Resource mapping (found with the draw census, see RECOMP_DRAW_LOG):
// - uniform buffer i of a stage = constant buffer bank i + 3 (as NVN1 on Maxwell);
// - bank 2 holds the stage's texture handles: texture i at c[2][0x20 + 8 * i];
// - descriptor set 0 = vertex stage, set 1 = fragment stage; binding = bank (UBOs),
//   32 + k (textures), 17 (guest memory, not bound yet).
//
// Blend, channel masks and depth test/write come from the bound state objects; stencil is
// not applied yet, culling only with $RECOMP_VK_CULL=1 (the winding convention with the
// upper-left window origin is not verified). Still missing: guest texture uploads
// (textures not rendered by the GPU read as zero), guest memory (LDG), compute dispatches.
//
// $RECOMP_DRAW_LOG=<n>: the first n draws' state in full (default 0).
#ifdef RECOMP_VULKAN
#include "vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *mem_host(uint64_t addr);

uint64_t vk_shader_hash(uint64_t code)
{
    const uint8_t *p = mem_host(code);
    uint64_t       h = 1469598103934665603ull;
    for (size_t i = 0; i < 0x400; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint64_t fnv(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t       h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// ---- statistics -------------------------------------------------------------------------

enum { SKIP_NO_SHADER, SKIP_NO_TARGET, SKIP_TOPOLOGY, SKIP_PIPELINE, SKIP_UPLOAD, SKIP_REASONS };
static const char *const k_skip_names[SKIP_REASONS] = { "shader not in cache", "no render target", "topology",
                                                         "pipeline failed", "transient memory full" };
static uint64_t g_drawn, g_skipped[SKIP_REASONS], g_pipelines;

void vk_draw_stats(void)
{
    host_log("[vk] draws: %llu drawn, skipped: %llu (%s) %llu (%s) %llu (%s) %llu (%s) %llu (%s); %llu pipelines",
             (unsigned long long)g_drawn, (unsigned long long)g_skipped[0], k_skip_names[0],
             (unsigned long long)g_skipped[1], k_skip_names[1], (unsigned long long)g_skipped[2], k_skip_names[2],
             (unsigned long long)g_skipped[3], k_skip_names[3], (unsigned long long)g_skipped[4], k_skip_names[4],
             (unsigned long long)g_pipelines);
}

// ---- per-frame transient memory and descriptor pools ---------------------------------------

#define UPLOAD_SIZE (256ull << 20)
static struct
{
    VkBuffer         buf;
    VkDeviceMemory   mem;
    uint8_t         *ptr;
    uint64_t         used;
    VkDescriptorPool pool;
    int              ok;
} g_fs[VK_FRAMES];

typedef struct { VkBuffer buf; uint64_t off; uint8_t *ptr; } span_t;

static void frame_init(int i)
{
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = UPLOAD_SIZE,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &g_fs[i].buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, g_fs[i].buf, &mr);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &g_fs[i].mem));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, g_fs[i].buf, g_fs[i].mem, 0));
    VK_CHECK(vkMapMemory(g_vk.dev, g_fs[i].mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_fs[i].ptr));
    VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536 },
                                  { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536 },
                                  { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 } };
    VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 16384,
                                      .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &g_fs[i].pool));
    g_fs[i].ok = 1;
}

void vk_draw_frame_begin(int slot)
{
    if (!g_fs[slot].ok)
        frame_init(slot);
    g_fs[slot].used = 0;
    VK_CHECK(vkResetDescriptorPool(g_vk.dev, g_fs[slot].pool, 0));
}

static int upload(uint64_t size, span_t *out)
{
    int      i   = vk_slot();
    uint64_t off = (g_fs[i].used + 255) & ~255ull; // minUniformBufferOffsetAlignment <= 256
    if (off + size > UPLOAD_SIZE)
        return 0;
    g_fs[i].used = off + size;
    out->buf     = g_fs[i].buf;
    out->off     = off;
    out->ptr     = g_fs[i].ptr + off;
    return 1;
}

// A copy of guest memory (zero-filled past `avail` bytes).
static int upload_guest(uint64_t addr, uint64_t avail, uint64_t size, span_t *out)
{
    if (!upload(size ? size : 16, out))
        return 0;
    uint64_t n = addr ? (avail < size ? avail : size) : 0;
    if (n)
        memcpy(out->ptr, mem_host(addr), n);
    if (n < size)
        memset(out->ptr + n, 0, size - n);
    return 1;
}

// ---- shader cache -------------------------------------------------------------------------

typedef struct { int bank; uint32_t size; } res_cb_t;
typedef struct { int bank, off, binding; char type[32]; } res_tex_t;
typedef struct
{
    uint64_t       hash;
    int            state; // 1 loaded, -1 not in the cache
    VkShaderModule mod;
    int            ncb, ntex, gmem;
    res_cb_t       cb[18];
    res_tex_t      tex[32];
    uint32_t       in_mask, out_mask;
} shader_t;

#define SHADER_SLOTS 16384
static shader_t *g_shaders[SHADER_SLOTS];

static const char *spv_dir(void)
{
    const char *e = getenv("RECOMP_SPV_DIR");
    return e && *e ? e : "spv";
}

static void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)n + 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    p[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return p;
}

static void parse_res(shader_t *s, char *text)
{
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        int a, b, c;
        char t[32];
        if (sscanf(line, "cbuf %d %d", &a, &b) == 2 && s->ncb < 18)
            s->cb[s->ncb++] = (res_cb_t){ a, (uint32_t)b };
        else if (sscanf(line, "tex %d %d %31s %d", &a, &b, t, &c) == 4 && s->ntex < 32) {
            res_tex_t *x = &s->tex[s->ntex++];
            x->bank = a, x->off = b, x->binding = c;
            snprintf(x->type, sizeof x->type, "%s", t);
        } else if (sscanf(line, "in %d", &a) == 1 && a < 32)
            s->in_mask |= 1u << a;
        else if (sscanf(line, "out %d", &a) == 1 && a < 32)
            s->out_mask |= 1u << a;
        else if (!strncmp(line, "gmem", 4))
            s->gmem = 1;
    }
}

static shader_t *shader_get(uint64_t hash, const char *ext)
{
    uint32_t h = (uint32_t)(hash ^ (hash >> 32)) % SHADER_SLOTS;
    for (uint32_t i = 0; i < SHADER_SLOTS; ++i) {
        shader_t **slot = &g_shaders[(h + i) % SHADER_SLOTS];
        if (*slot && (*slot)->hash == hash)
            return (*slot)->state > 0 ? *slot : NULL;
        if (*slot)
            continue;
        shader_t *s = calloc(1, sizeof *s);
        s->hash     = hash;
        s->state    = -1;
        *slot       = s;
        char   path[1024];
        size_t len;
        snprintf(path, sizeof path, "%s/%016llx.%s.spv", spv_dir(), (unsigned long long)hash, ext);
        uint32_t *code = read_file(path, &len);
        snprintf(path, sizeof path, "%s/%016llx.res", spv_dir(), (unsigned long long)hash);
        size_t rlen;
        char  *res = read_file(path, &rlen);
        if (code && res) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = len,
                                            .pCode = code };
            if (vkCreateShaderModule(g_vk.dev, &ci, NULL, &s->mod) == VK_SUCCESS) {
                parse_res(s, res);
                s->state = 1;
            }
        } else {
            static int logged;
            if (logged++ < 30)
                VK_LOG("shader %016llx.%s not in the cache (%s)", (unsigned long long)hash, ext, spv_dir());
        }
        free(code);
        free(res);
        return s->state > 0 ? s : NULL;
    }
    return NULL;
}

// ---- programs: descriptor set layouts and pipeline layouts -----------------------------------

typedef struct
{
    uint64_t              vs, fs;
    VkDescriptorSetLayout set[2];
    VkPipelineLayout      layout;
} prog_t;

#define PROG_SLOTS 8192
static prog_t *g_progs[PROG_SLOTS];

static VkDescriptorSetLayout set_layout(const shader_t *s, VkShaderStageFlags stage)
{
    VkDescriptorSetLayoutBinding b[64];
    uint32_t                     n = 0;
    for (int i = 0; i < s->ncb; ++i)
        b[n++] = (VkDescriptorSetLayoutBinding){ (uint32_t)s->cb[i].bank, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, NULL };
    for (int i = 0; i < s->ntex; ++i)
        b[n++] = (VkDescriptorSetLayoutBinding){ (uint32_t)s->tex[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                 stage, NULL };
    if (s->gmem)
        b[n++] = (VkDescriptorSetLayoutBinding){ 17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stage, NULL };
    VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = n, .pBindings = b };
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static prog_t *prog_get(const shader_t *vs, const shader_t *fs)
{
    uint64_t h = (vs->hash * 31 + fs->hash) % PROG_SLOTS;
    for (uint32_t i = 0; i < PROG_SLOTS; ++i) {
        prog_t **slot = &g_progs[(h + i) % PROG_SLOTS];
        if (*slot && (*slot)->vs == vs->hash && (*slot)->fs == fs->hash)
            return *slot;
        if (*slot)
            continue;
        prog_t *p = calloc(1, sizeof *p);
        p->vs     = vs->hash;
        p->fs     = fs->hash;
        p->set[0] = set_layout(vs, VK_SHADER_STAGE_VERTEX_BIT);
        p->set[1] = set_layout(fs, VK_SHADER_STAGE_FRAGMENT_BIT);
        VkPushConstantRange        pc = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 };
        VkPipelineLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                          .pSetLayouts = p->set, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc };
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &p->layout));
        *slot = p;
        return p;
    }
    return NULL;
}

// ---- NVN state enums -> Vulkan (values as in LAS Recomp's exec.c) ---------------------------

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_op(uint32_t f)
{
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// ---- pipelines --------------------------------------------------------------------------------

#define ZERO_BINDING 16 // a binding of zeros (stride 0) for shader inputs with no attribute

typedef struct
{
    uint64_t vs, fs;
    VkFormat color[8], depth;
    uint32_t ncolor, topology, depth_minus_one, nattr, nbind;
    uint32_t blend_enable, channel_mask, depth_test, depth_write, depth_func, cull, front_ccw;
    rnd_blend_t blend[8];
    struct { uint32_t loc, binding, fmt, off; } attr[32];
    struct { uint32_t binding, stride, rate; } bind[17];
} pipe_key_t;

typedef struct
{
    pipe_key_t key;
    VkPipeline pipe; // VK_NULL_HANDLE: creation failed
} pipe_t;

#define PIPE_SLOTS 32768
static pipe_t *g_pipes[PIPE_SLOTS];

static VkPipeline pipe_create(const pipe_key_t *k, const shader_t *vs, const shader_t *fs, const prog_t *pr)
{
    VkBool32                 dm1     = k->depth_minus_one;
    VkSpecializationMapEntry me      = { 10, 0, sizeof dm1 };
    VkSpecializationInfo     spec    = { 1, &me, sizeof dm1, &dm1 };
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs->mod, .pName = "main", .pSpecializationInfo = &spec },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs->mod, .pName = "main" },
    };
    VkVertexInputAttributeDescription ad[32];
    VkVertexInputBindingDescription   bd[17];
    for (uint32_t i = 0; i < k->nattr; ++i)
        ad[i] = (VkVertexInputAttributeDescription){ k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].fmt,
                                                     k->attr[i].off };
    for (uint32_t i = 0; i < k->nbind; ++i)
        bd[i] = (VkVertexInputBindingDescription){ k->bind[i].binding, k->bind[i].stride,
                                                   (VkVertexInputRate)k->bind[i].rate };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                .vertexBindingDescriptionCount = k->nbind, .pVertexBindingDescriptions = bd,
                                                .vertexAttributeDescriptionCount = k->nattr,
                                                .pVertexAttributeDescriptions = ad };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = (VkPrimitiveTopology)k->topology };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = k->cull == 1 ? VK_CULL_MODE_FRONT_BIT : k->cull == 2 ? VK_CULL_MODE_BACK_BIT
                  : k->cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE,
        .frontFace = k->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                 .depthTestEnable = k->depth_test != 0, .depthWriteEnable = k->depth_write != 0,
                                                 .depthCompareOp = compare_op(k->depth_func) };
    VkPipelineColorBlendAttachmentState   cb[8];
    for (uint32_t i = 0; i < k->ncolor; ++i) {
        const rnd_blend_t *b = &k->blend[i];
        cb[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = (k->blend_enable >> i) & 1, .srcColorBlendFactor = blend_factor(b->src),
            .dstColorBlendFactor = blend_factor(b->dst), .colorBlendOp = blend_op(b->op),
            .srcAlphaBlendFactor = blend_factor(b->src_a), .dstAlphaBlendFactor = blend_factor(b->dst_a),
            .alphaBlendOp = blend_op(b->op_a),
            // the channel mask; no shader output: keep the attachment's contents
            .colorWriteMask = (fs->out_mask >> i) & 1 ? (k->channel_mask >> (4 * i)) & 0xF : 0,
        };
    }
    VkPipelineColorBlendStateCreateInfo bs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = k->ncolor, .pAttachments = cb };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 3, .pDynamicStates = dyn };
    bool has_stencil = k->depth == VK_FORMAT_D24_UNORM_S8_UINT || k->depth == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                       k->depth == VK_FORMAT_S8_UINT;
    bool has_depth = k->depth != VK_FORMAT_UNDEFINED && k->depth != VK_FORMAT_S8_UINT;
    VkPipelineRenderingCreateInfo rc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->color,
                                         .depthAttachmentFormat = has_depth ? k->depth : VK_FORMAT_UNDEFINED,
                                         .stencilAttachmentFormat = has_stencil ? k->depth : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rc, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &bs, .pDynamicState = &dy,
        .layout = pr->layout,
    };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult   r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r != VK_SUCCESS) {
        VK_LOG("pipeline %016llx/%016llx failed: %d", (unsigned long long)k->vs, (unsigned long long)k->fs, r);
        return VK_NULL_HANDLE;
    }
    ++g_pipelines;
    return p;
}

static VkPipeline pipe_get(const pipe_key_t *k, const shader_t *vs, const shader_t *fs, const prog_t *pr)
{
    uint64_t h = fnv(k, sizeof *k) % PIPE_SLOTS;
    for (uint32_t i = 0; i < PIPE_SLOTS; ++i) {
        pipe_t **slot = &g_pipes[(h + i) % PIPE_SLOTS];
        if (*slot && !memcmp(&(*slot)->key, k, sizeof *k))
            return (*slot)->pipe;
        if (*slot)
            continue;
        pipe_t *p = calloc(1, sizeof *p);
        p->key    = *k;
        p->pipe   = pipe_create(k, vs, fs, pr);
        *slot     = p;
        return p->pipe;
    }
    return VK_NULL_HANDLE;
}

// ---- vertex formats ---------------------------------------------------------------------------

static VkFormat vertex_format(uint32_t f)
{
    switch (f) {
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    // integer-to-float formats (unsigned, signed)
    case 0x51: return VK_FORMAT_R8_USCALED;
    case 0x52: return VK_FORMAT_R8_SSCALED;
    case 0x53: return VK_FORMAT_R16_USCALED;
    case 0x54: return VK_FORMAT_R16_SSCALED;
    case 0x57: return VK_FORMAT_R8G8_USCALED;
    case 0x58: return VK_FORMAT_R8G8_SSCALED;
    case 0x59: return VK_FORMAT_R16G16_USCALED;
    case 0x5A: return VK_FORMAT_R16G16_SSCALED;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED;
    case 0x5E: return VK_FORMAT_R8G8B8_SSCALED;
    case 0x5F: return VK_FORMAT_R16G16B16_USCALED;
    case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED;
    case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED;
    case 0x65: return VK_FORMAT_R16G16B16A16_USCALED;
    case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return vk_format(f);
    }
}

static uint32_t format_size(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_USCALED: case VK_FORMAT_R8_SSCALED: return 1;
    case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SNORM: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT:
    case VK_FORMAT_R8G8B8_USCALED: case VK_FORMAT_R8G8B8_SSCALED: return 3;
    case VK_FORMAT_R16G16B16_SFLOAT: case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SNORM:
    case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16_USCALED:
    case VK_FORMAT_R16G16B16_SSCALED: return 6;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 16;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_USCALED:
    case VK_FORMAT_R16G16B16A16_SSCALED: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: return 8;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8_USCALED: case VK_FORMAT_R8G8_SSCALED: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_USCALED:
    case VK_FORMAT_R16_SSCALED: return 2;
    default: return 4;
    }
}

static bool vertex_format_ok(VkFormat f)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}

static VkPrimitiveTopology topology(uint32_t mode, bool *ok)
{
    *ok = true;
    switch (mode) { // NVNdrawPrimitive
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: *ok = false; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // loops, quads, polygons, patches
    }
}

// ---- dummy textures and the sampler -----------------------------------------------------------

static VkSampler   g_sampler, g_shadow_sampler;
static VkImageView g_dummy[4], g_dummy_depth[3]; // 2D, 2D array, cube, 3D; depth: 2D, 2D array, cube

static VkImageView dummy_image(VkImageViewType type, VkFormat fmt, VkImageAspectFlags aspect)
{
    bool              cube  = type == VK_IMAGE_VIEW_TYPE_CUBE;
    VkImageCreateInfo ci    = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                                .imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                .format = fmt, .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VkImage           img;
    VkDeviceMemory    mem;
    VK_CHECK(vkCreateImage(g_vk.dev, &ci, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &mem));
    VK_CHECK(vkBindImageMemory(g_vk.dev, img, mem, 0));
    vk_end_rendering();
    VkCommandBuffer         cb = vk_cmd();
    VkImageSubresourceRange rg = { aspect, 0, 1, 0, ci.arrayLayers };
    VkImageMemoryBarrier2   b  = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img, .subresourceRange = rg,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED };
    VkDependencyInfo        di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                                   .pImageMemoryBarriers = &b };
    vkCmdPipelineBarrier2(cb, &di);
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v = { { 0, 0, 0, 0 } };
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg);
    } else {
        VkClearDepthStencilValue v = { 1.0f, 0 };
        vkCmdClearDepthStencilImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg);
    }
    vk_full_barrier(cb);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = type,
                                 .format = fmt, .subresourceRange = rg };
    VkImageView v;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &v));
    return v;
}

static void samplers_init(void)
{
    if (g_sampler)
        return;
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE };
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &g_sampler));
    si.compareEnable = VK_TRUE;
    si.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &g_shadow_sampler));
    static const VkImageViewType types[4] = { VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE,
                                              VK_IMAGE_VIEW_TYPE_3D };
    for (int i = 0; i < 4; ++i)
        g_dummy[i] = dummy_image(types[i], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    for (int i = 0; i < 3; ++i)
        g_dummy_depth[i] = dummy_image(types[i], VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

// The image view and sampler for a shader texture binding.
static void texture_binding(const rnd_state_t *s, int stage, const res_tex_t *t, VkDescriptorImageInfo *out)
{
    bool shadow = strstr(t->type, "Shadow") != NULL;
    int  kind   = strstr(t->type, "Cube") ? 2 : strstr(t->type, "Array") ? 1 : strstr(t->type, "3D") ? 3 : 0;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->sampler     = shadow ? g_shadow_sampler : g_sampler;
    out->imageView   = shadow ? g_dummy_depth[kind < 3 ? kind : 0] : g_dummy[kind];
    if (t->bank != 2 || t->off < 0x20 || kind != 0)
        return;
    int       idx = (t->off - 0x20) / 8;
    rnd_tex_t tex;
    if (idx >= 32 || !s->tex[stage][idx] || !nvn_texture_of_handle(s->tex[stage][idx], &tex))
        return;
    vk_image_t *im = vk_image_of(&tex);
    if (!im || !im->view)
        return;
    bool depth = (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    if (!shadow || depth) // (a color image cannot be read with a depth compare)
        out->imageView = im->view;
}

// ---- rendering passes ---------------------------------------------------------------------------

static struct
{
    int     open;
    VkImage color[8], depth;
    int     ncolor;
} g_pass;

void vk_end_rendering(void)
{
    if (!g_pass.open)
        return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    g_pass.open = 0;
}

static void begin_rendering(vk_image_t *const color[8], int ncolor, vk_image_t *depth)
{
    bool same = g_pass.open && g_pass.ncolor == ncolor && g_pass.depth == (depth ? depth->image : VK_NULL_HANDLE);
    for (int i = 0; same && i < ncolor; ++i)
        same = g_pass.color[i] == (color[i] ? color[i]->image : VK_NULL_HANDLE);
    if (same)
        return;
    vk_end_rendering();
    VkRenderingAttachmentInfo ca[8];
    uint32_t                  w = 16384, h = 16384;
    for (int i = 0; i < ncolor; ++i) {
        ca[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                             .imageView = color[i] ? color[i]->view : VK_NULL_HANDLE,
                                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                             .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        g_pass.color[i] = color[i] ? color[i]->image : VK_NULL_HANDLE;
        if (color[i]) {
            if (color[i]->key.w < w) w = color[i]->key.w;
            if (color[i]->key.h < h) h = color[i]->key.h;
        }
    }
    VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = depth ? depth->view : VK_NULL_HANDLE,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    if (depth) {
        if (depth->key.w < w) w = depth->key.w;
        if (depth->key.h < h) h = depth->key.h;
    }
    bool has_depth   = depth && (depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    bool has_stencil = depth && (depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, h } }, .layerCount = 1,
                           .colorAttachmentCount = (uint32_t)ncolor, .pColorAttachments = ca,
                           .pDepthAttachment = has_depth ? &da : NULL, .pStencilAttachment = has_stencil ? &da : NULL };
    vkCmdBeginRendering(vk_cmd(), &ri);
    g_pass.open   = 1;
    g_pass.ncolor = ncolor;
    g_pass.depth  = depth ? depth->image : VK_NULL_HANDLE;
}

// ---- the draw -----------------------------------------------------------------------------------

static void log_draw(const rnd_state_t *s, const rnd_draw_t *d)
{
    static long limit = -1, n;
    if (limit < 0) {
        const char *e = getenv("RECOMP_DRAW_LOG");
        limit         = e ? atol(e) : 0;
    }
    if (n++ >= limit)
        return;
    host_log("[draw %ld] %s mode %u count %u first %u instances %u base vertex %d index type %u at 0x%llx", n,
             d->indexed ? "elements" : "arrays", d->mode, d->count, d->first, d->instances, d->base_vertex,
             d->index_type, (unsigned long long)d->index_addr);
    host_log("    program 0x%llx: vertex %016llx fragment %016llx", (unsigned long long)s->program,
             s->code[RND_VERTEX] ? (unsigned long long)vk_shader_hash(s->code[RND_VERTEX]) : 0ull,
             s->code[RND_FRAGMENT] ? (unsigned long long)vk_shader_hash(s->code[RND_FRAGMENT]) : 0ull);
    for (int k = 0; k < s->ncolor; ++k)
        if (s->color[k].obj)
            host_log("    color[%d] 0x%llx %ux%u format 0x%x", k, (unsigned long long)s->color[k].obj, s->color[k].w,
                     s->color[k].h, s->color[k].format);
    if (s->depth.obj)
        host_log("    depth 0x%llx %ux%u format 0x%x", (unsigned long long)s->depth.obj, s->depth.w, s->depth.h,
                 s->depth.format);
    host_log("    viewport %.0f %.0f %.0f %.0f depth %.2f..%.2f scissor %d %d %d %d", s->viewport[0], s->viewport[1],
             s->viewport[2], s->viewport[3], s->depth_range[0], s->depth_range[1], s->scissor[0], s->scissor[1],
             s->scissor[2], s->scissor[3]);
    for (int i = 0; i < s->nattrib; ++i)
        if (s->attrib[i].format)
            host_log("    attrib[%d] format 0x%x offset 0x%x stream %u", i, s->attrib[i].format, s->attrib[i].offset,
                     s->attrib[i].stream);
    for (int i = 0; i < s->nstream; ++i)
        if (s->vbuf[i].addr || s->stream[i].stride)
            host_log("    stream[%d] stride %u divisor %u, buffer 0x%llx size 0x%llx", i, s->stream[i].stride,
                     s->stream[i].divisor, (unsigned long long)s->vbuf[i].addr, (unsigned long long)s->vbuf[i].size);
    for (int st = 0; st < RND_STAGES; ++st) {
        for (int i = 0; i < 18; ++i)
            if (s->ubuf[st][i].addr)
                host_log("    stage %d ubuf[%d] 0x%llx size 0x%llx", st, i, (unsigned long long)s->ubuf[st][i].addr,
                         (unsigned long long)s->ubuf[st][i].size);
        for (int i = 0; i < 32; ++i)
            if (s->tex[st][i]) {
                rnd_tex_t t = { 0 };
                nvn_texture_of_handle(s->tex[st][i], &t);
                host_log("    stage %d tex[%d] handle 0x%llx: texture 0x%llx %ux%u format 0x%x at 0x%llx", st, i,
                         (unsigned long long)s->tex[st][i], (unsigned long long)t.obj, t.w, t.h, t.format,
                         (unsigned long long)t.addr);
            }
    }
    if (s->vbuf[0].addr) {
        const uint32_t *v = mem_host(s->vbuf[0].addr);
        host_log("    vbuf[0] words: %08x %08x %08x %08x  %08x %08x %08x %08x", v[0], v[1], v[2], v[3], v[4], v[5], v[6],
                 v[7]);
    }
    if (s->ubuf[0][0].addr) {
        const uint32_t *w = mem_host(s->ubuf[0][0].addr);
        uint64_t        nz = 0, first = ~0ull;
        for (uint64_t i = 0; i < s->ubuf[0][0].size / 4; ++i)
            if (w[i]) { ++nz; if (first == ~0ull) first = i * 4; }
        host_log("    vertex ubuf[0]: %llu nonzero words, first at +0x%llx", (unsigned long long)nz, (unsigned long long)first);
        const float *u = mem_host(s->ubuf[0][0].addr);
        host_log("    vertex ubuf[0]: %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g", u[0], u[1], u[2],
                 u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15], u[16], u[17], u[18],
                 u[19]);
    }
}

// The highest index of an indexed draw (for the size of the vertex data to copy).
static uint32_t max_index(const rnd_draw_t *d)
{
    uint32_t       m = 0;
    const uint8_t *p = mem_host(d->index_addr);
    for (uint32_t i = 0; i < d->count; ++i) {
        uint32_t v = d->index_type == 0 ? p[i] : d->index_type == 1 ? ((const uint16_t *)p)[i] : ((const uint32_t *)p)[i];
        if (v > m && v != 0xFFFFFFFFu && !(d->index_type == 1 && v == 0xFFFF))
            m = v;
    }
    return m;
}

void rnd_draw(const rnd_state_t *s, const rnd_draw_t *d)
{
    log_draw(s, d);
    if (!d->count || !d->instances)
        return;
    void vk_set_present_flip(int flip);
    vk_set_present_flip(!s->origin_upper_left);
    vk_cmd(); // (starts the frame: transient memory and descriptor pools)
    samplers_init();
    bool                ok;
    VkPrimitiveTopology topo = topology(d->mode, &ok);
    if (!ok) {
        ++g_skipped[SKIP_TOPOLOGY];
        return;
    }
    shader_t *vs = s->code[RND_VERTEX] ? shader_get(vk_shader_hash(s->code[RND_VERTEX]), "vert") : NULL;
    shader_t *fs = s->code[RND_FRAGMENT] ? shader_get(vk_shader_hash(s->code[RND_FRAGMENT]), "frag") : NULL;
    if (!vs || !fs) {
        ++g_skipped[SKIP_NO_SHADER];
        return;
    }
    prog_t *pr = prog_get(vs, fs);

    // Render targets (resolved before any rendering begins: a new image records commands).
    vk_image_t *color[8] = { 0 }, *depth = NULL;
    int         ncolor   = 0;
    for (int k = 0; k < s->ncolor && k < 8; ++k)
        if (s->color[k].obj && (color[k] = vk_image_of(&s->color[k])) && color[k]->attachment &&
            color[k]->aspect == VK_IMAGE_ASPECT_COLOR_BIT)
            ncolor = k + 1;
        else
            color[k] = NULL;
    if (s->depth.obj && (depth = vk_image_of(&s->depth)) && (!depth->attachment || depth->aspect == VK_IMAGE_ASPECT_COLOR_BIT))
        depth = NULL;
    if (!ncolor && !depth) {
        ++g_skipped[SKIP_NO_TARGET];
        return;
    }

    pipe_key_t key;
    memset(&key, 0, sizeof key);
    key.vs = vs->hash;
    key.fs = fs->hash;
    key.ncolor = (uint32_t)ncolor;
    for (int k = 0; k < ncolor; ++k) key.color[k] = color[k] ? color[k]->format : VK_FORMAT_UNDEFINED;
    key.depth           = depth ? depth->format : VK_FORMAT_UNDEFINED;
    key.topology        = (uint32_t)topo;
    key.depth_minus_one = !s->depth_zero_to_one;
    static int cull = -1;
    if (cull < 0)
        cull = getenv("RECOMP_VK_CULL") && *getenv("RECOMP_VK_CULL") == '1';
    key.cull         = cull ? s->cull_face : 0;
    key.front_ccw    = s->front_ccw;
    key.channel_mask = s->channel_mask;
    key.blend_enable = s->blend_enable & ((1u << ncolor) - 1);
    for (int k = 0; k < ncolor; ++k)
        if ((key.blend_enable >> k) & 1)
            key.blend[k] = s->blend[k];
    if (depth && (depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        key.depth_test  = s->depth_test;
        key.depth_write = s->depth_write;
        key.depth_func  = s->depth_test ? s->depth_func : 0;
    }

    // Vertex input: shader input i <- attribute i; inputs without a usable attribute read
    // a binding of zeros.
    uint32_t used_streams = 0, need_zero = 0;
    for (int i = 0; i < 32; ++i) {
        if (!((vs->in_mask >> i) & 1))
            continue;
        const rnd_attrib_t *a  = i < 16 ? &s->attrib[i] : NULL;
        VkFormat            vf = a && a->format ? vertex_format(a->format) : VK_FORMAT_UNDEFINED;
        if (vf != VK_FORMAT_UNDEFINED && a->stream < 16 && vertex_format_ok(vf)) {
            key.attr[key.nattr++] = (typeof(key.attr[0])){ (uint32_t)i, a->stream, (uint32_t)vf, a->offset };
            used_streams |= 1u << a->stream;
        } else {
            key.attr[key.nattr++] = (typeof(key.attr[0])){ (uint32_t)i, ZERO_BINDING, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
            need_zero = 1;
        }
    }
    for (uint32_t b = 0; b < 16; ++b)
        if ((used_streams >> b) & 1)
            key.bind[key.nbind++] = (typeof(key.bind[0])){ b, s->stream[b].stride,
                                                           s->stream[b].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                : VK_VERTEX_INPUT_RATE_VERTEX };
    if (need_zero)
        key.bind[key.nbind++] = (typeof(key.bind[0])){ ZERO_BINDING, 0, VK_VERTEX_INPUT_RATE_VERTEX };

    // Data: indices (bytes widened to 16 bits), vertices, constant buffers.
    span_t   ib  = { 0 };
    uint32_t top = 0;
    if (d->indexed) {
        uint32_t isz = d->index_type == 0 ? 2 : d->index_type == 1 ? 2 : 4;
        if (!upload((uint64_t)d->count * isz, &ib)) {
            ++g_skipped[SKIP_UPLOAD];
            return;
        }
        const uint8_t *src = mem_host(d->index_addr);
        if (d->index_type == 0)
            for (uint32_t i = 0; i < d->count; ++i) ((uint16_t *)ib.ptr)[i] = src[i] == 0xFF ? 0xFFFF : src[i];
        else
            memcpy(ib.ptr, src, (size_t)d->count * isz);
        top = max_index(d) + (uint32_t)(d->base_vertex > 0 ? d->base_vertex : 0) + 1;
    } else {
        top = d->first + d->count;
    }
    VkBuffer     vbufs[17];
    VkDeviceSize voffs[17];
    for (uint32_t i = 0; i < key.nbind; ++i) {
        uint32_t b = key.bind[i].binding;
        span_t   sp;
        if (b == ZERO_BINDING) {
            if (!upload_guest(0, 0, 16, &sp)) { ++g_skipped[SKIP_UPLOAD]; return; }
        } else {
            uint64_t n = key.bind[i].rate == VK_VERTEX_INPUT_RATE_INSTANCE
                             ? (uint64_t)(d->base_instance + d->instances) * key.bind[i].stride
                             : (uint64_t)top * key.bind[i].stride;
            // attributes reach past the last element's start by their own size
            uint64_t tail = 0;
            for (uint32_t a = 0; a < key.nattr; ++a)
                if (key.attr[a].binding == b) {
                    uint64_t e = key.attr[a].off + format_size((VkFormat)key.attr[a].fmt);
                    if (e > tail) tail = e;
                }
            n += tail;
            if (n > (64ull << 20)) n = 64ull << 20;
            if (!upload_guest(s->vbuf[b].addr, s->vbuf[b].size, n, &sp)) { ++g_skipped[SKIP_UPLOAD]; return; }
        }
        vbufs[i] = sp.buf;
        voffs[i] = sp.off;
    }

    VkPipeline pipe = pipe_get(&key, vs, fs, pr);
    if (!pipe) {
        ++g_skipped[SKIP_PIPELINE];
        return;
    }

    // Descriptor sets: stage set 0 = vertex, 1 = fragment.
    VkDescriptorSet       sets[2];
    const shader_t       *sh[2]     = { vs, fs };
    const int             stage[2]  = { RND_VERTEX, RND_FRAGMENT };
    VkDescriptorSetAllocateInfo ai  = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = g_fs[vk_slot()].pool, .descriptorSetCount = 2,
                                        .pSetLayouts = pr->set };
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, sets) != VK_SUCCESS) {
        ++g_skipped[SKIP_UPLOAD];
        return;
    }
    VkWriteDescriptorSet   w[2 * 64];
    VkDescriptorBufferInfo bi[2 * 20];
    VkDescriptorImageInfo  ii[2 * 32];
    uint32_t               nw = 0, nb = 0, ni = 0;
    for (int k = 0; k < 2; ++k) {
        const shader_t *x = sh[k];
        for (int i = 0; i < x->ncb; ++i) {
            int      bank = x->cb[i].bank;
            uint64_t size = ((x->cb[i].size + 15) / 16) * 16;
            if (size > 65536) size = 65536;
            if (size < 16) size = 16;
            span_t sp;
            bool   up;
            if (bank >= 3 && bank - 3 < 18) {
                const rnd_range_t *r = &s->ubuf[stage[k]][bank - 3];
                up = upload_guest(r->addr, r->size, size, &sp);
            } else if (bank == 2) { // the texture handle table
                up = upload_guest(0, 0, size, &sp);
                if (up)
                    for (int t = 0; t < 32 && 0x20 + 8 * (uint64_t)t + 8 <= size; ++t)
                        memcpy(sp.ptr + 0x20 + 8 * t, &s->tex[stage[k]][t], 8);
            } else { // driver constants: not reproduced yet
                up = upload_guest(0, 0, size, &sp);
            }
            if (!up) { ++g_skipped[SKIP_UPLOAD]; return; }
            bi[nb] = (VkDescriptorBufferInfo){ sp.buf, sp.off, size };
            w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[k],
                                              .dstBinding = (uint32_t)bank, .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nb++] };
        }
        for (int i = 0; i < x->ntex; ++i) {
            texture_binding(s, stage[k], &x->tex[i], &ii[ni]);
            w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[k],
                                              .dstBinding = (uint32_t)x->tex[i].binding, .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              .pImageInfo = &ii[ni++] };
        }
        if (x->gmem) { // guest memory: not mapped yet, a small zero buffer stands in
            span_t sp;
            if (!upload_guest(0, 0, 256, &sp)) { ++g_skipped[SKIP_UPLOAD]; return; }
            bi[nb] = (VkDescriptorBufferInfo){ sp.buf, sp.off, 256 };
            w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[k],
                                              .dstBinding = 17, .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[nb++] };
        }
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, w, 0, NULL);

    // Record (textures were resolved above: nothing below creates images).
    begin_rendering(color, ncolor, depth);
    VkCommandBuffer cb = vk_cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);
    uint64_t gbase = 0;
    vkCmdPushConstants(cb, pr->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, &gbase);
    for (uint32_t i = 0; i < key.nbind; ++i)
        vkCmdBindVertexBuffers(cb, key.bind[i].binding, 1, &vbufs[i], &voffs[i]);
    float      vw = s->viewport[2], vh = s->viewport[3];
    VkViewport vp = { s->viewport[0], s->viewport[1], vw > 0 ? vw : 1, vh > 0 ? vh : 1,
                      s->depth_range[0] < 0 ? 0 : s->depth_range[0] > 1 ? 1 : s->depth_range[0],
                      s->depth_range[1] < 0 ? 0 : s->depth_range[1] > 1 ? 1 : s->depth_range[1] };
    if (vp.minDepth == 0 && vp.maxDepth == 0) vp.maxDepth = 1;
    VkRect2D sc = { { s->scissor[0] > 0 ? s->scissor[0] : 0, s->scissor[1] > 0 ? s->scissor[1] : 0 },
                    { s->scissor[2] > 0 ? (uint32_t)s->scissor[2] : 16384, s->scissor[3] > 0 ? (uint32_t)s->scissor[3] : 16384 } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetBlendConstants(cb, s->blend_color);
    if (d->indexed) {
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, d->index_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, d->count, d->instances, 0, d->base_vertex, d->base_instance);
    } else {
        vkCmdDraw(cb, d->count, d->instances, d->first, d->base_instance);
    }
    ++g_drawn;
}
#endif // RECOMP_VULKAN
