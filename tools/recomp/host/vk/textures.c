// Vulkan images for guest textures: NVN formats and one image per guest texture object.
// The format table follows LAS Recomp's runtime/nvn/vulkan/textures.c (NVN numbering, which
// NVN2 keeps for these ids).
//
// Contents: images the GPU has not written are filled from guest memory (the texture's
// storage in its memory pool), block-linear deswizzled (tegra.c) and ASTC decoded in
// software (astc.c: desktop GPUs have no ASTC). The storage is hashed to notice when the
// game loads or replaces texels: fully until an upload happened, then a sample every few
// frames (a changed sample triggers a full hash).
//
// $RECOMP_VK_NO_UPLOAD=1: no uploads (images start zeroed, as before).
#ifdef RECOMP_VULKAN
#include "vk.h"
#include "astc.h"
#include "tegra.h"
#include <stdlib.h>
#include <string.h>

VkFormat vk_format(uint32_t f)
{
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT;
    case 0x32: return VK_FORMAT_D16_UNORM;
    case 0x33: return VK_FORMAT_D32_SFLOAT; // DEPTH24
    case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT; // NVIDIA has it for everything
    case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case 0x3A: case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM; // RGBX8
    case 0x71: return VK_FORMAT_R16G16B16A16_SFLOAT; // RGBX16F
    case 0x76: return VK_FORMAT_R32G32B32A32_SFLOAT; // RGBX32F
    case 0x79 ... 0x86: return VK_FORMAT_R8G8B8A8_UNORM; // ASTC, decoded on the CPU
    case 0x87 ... 0x94: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x98: case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9A: case 0x9B: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

static VkImageAspectFlags aspect_of(uint32_t f)
{
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f >= 0x32 && f <= 0x34) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool is_compressed(uint32_t f) { return f >= 0x41 && f <= 0x50; }
static bool is_astc(uint32_t f) { return f >= 0x79 && f <= 0x94; }

// Formats whose guest texels can be copied as they are (same bytes per texel / block as the
// Vulkan format vk_format picks), or decoded (ASTC).
static bool uploadable(uint32_t f)
{
    if (is_compressed(f) || is_astc(f)) return true;
    if (f >= 0x01 && f <= 0x18) return true; // R8 .. RG32
    if (f >= 0x25 && f <= 0x30) return true; // RGBA8 .. RGBA32
    if (f >= 0x37 && f <= 0x40) return true; // sRGB RGBA8, packed 16/32-bit formats
    return f == 0x6D || f == 0x71 || f == 0x76 || (f >= 0x98 && f <= 0x9B);
}

// ---- staging buffers, destroyed once the frame slot's GPU work is done ---------------------

typedef struct { VkBuffer buf; VkDeviceMemory mem; } staging_t;
static struct { staging_t *v; int n, cap; } g_garbage[VK_FRAMES];

void vk_textures_frame_begin(int slot)
{
    for (int i = 0; i < g_garbage[slot].n; ++i) {
        vkDestroyBuffer(g_vk.dev, g_garbage[slot].v[i].buf, NULL);
        vkFreeMemory(g_vk.dev, g_garbage[slot].v[i].mem, NULL);
    }
    g_garbage[slot].n = 0;
}

static uint8_t *staging(uint64_t size, VkBuffer *out)
{
    staging_t          s;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size ? size : 16,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &s.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, s.buf, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &s.mem));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, s.buf, s.mem, 0));
    void *p;
    VK_CHECK(vkMapMemory(g_vk.dev, s.mem, 0, VK_WHOLE_SIZE, 0, &p));
    int slot = vk_slot();
    if (g_garbage[slot].n == g_garbage[slot].cap) {
        g_garbage[slot].cap = g_garbage[slot].cap ? 2 * g_garbage[slot].cap : 64;
        g_garbage[slot].v   = realloc(g_garbage[slot].v, sizeof(staging_t) * (size_t)g_garbage[slot].cap);
    }
    g_garbage[slot].v[g_garbage[slot].n++] = s;
    *out = s.buf;
    return p;
}

// ---- image cache: open addressing on the guest object address --------------------------

#define IMG_SLOTS 8192
static vk_image_t *g_images[IMG_SLOTS];

static void destroy(vk_image_t *im)
{
    // Called only between frames for a texture object that was re-initialised with another
    // description; the frame that used the old image may still run, so wait for the GPU.
    vkDeviceWaitIdle(g_vk.dev);
    vkDestroyImageView(g_vk.dev, im->view, NULL);
    for (int i = 0; i < im->nviews; ++i) vkDestroyImageView(g_vk.dev, im->views[i].view, NULL);
    for (int i = 0; i < 3; ++i)
        if (im->sample[i]) vkDestroyImageView(g_vk.dev, im->sample[i], NULL);
    vkDestroyImage(g_vk.dev, im->image, NULL);
    vkFreeMemory(g_vk.dev, im->mem, NULL);
    free(im);
}

static vk_image_t *create(const rnd_tex_t *t)
{
    VkFormat           vf     = vk_format(t->format);
    VkImageAspectFlags aspect = aspect_of(t->format);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, vf, &fp);
    VkFormatFeatureFlags want   = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    bool                 usable = (fp.optimalTilingFeatures & want) == want;
    if (!usable) {
        VK_LOG("format 0x%x (vk %d) unusable, texture 0x%llx as RGBA8", t->format, vf, (unsigned long long)t->obj);
        vf = VK_FORMAT_R8G8B8A8_UNORM;
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, vf, &fp);
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT && !is_compressed(t->format) &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (aspect != VK_IMAGE_ASPECT_COLOR_BIT &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    // 2D arrays and cube maps keep their layers in depth (3D textures are not modelled yet)
    uint32_t w = t->w ? t->w : 1, h = t->h ? t->h : 1, layers = t->depth ? t->depth : 1;
    uint32_t levels = t->levels ? t->levels : 1, max_levels = 1;
    while ((w >> max_levels) || (h >> max_levels)) ++max_levels;
    if (levels > max_levels) levels = max_levels;
    bool cube = layers >= 6 && w == h;
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = vf, .extent = { w, h, 1 }, .mipLevels = levels,
        .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vk_image_t *im = calloc(1, sizeof *im);
    im->key        = *t;
    im->format     = vf;
    im->aspect     = aspect;
    im->levels     = levels;
    im->layers     = layers;
    im->cube       = cube;
    im->can_upload = usable && uploadable(t->format) && aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    im->blit_src   = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
    if (vkCreateImage(g_vk.dev, &ci, NULL, &im->image) != VK_SUCCESS) {
        VK_LOG("cannot create image %ux%u format 0x%x", t->w, t->h, t->format);
        free(im);
        return NULL;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, im->image, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &im->mem));
    VK_CHECK(vkBindImageMemory(g_vk.dev, im->image, im->mem, 0));
    im->attachment = (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
    // Depth/stencil images are viewed through their depth aspect (sampling reads depth).
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vf, .subresourceRange = { aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : aspect, 0, 1, 0, 1 },
    };
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &im->view));

    // UNDEFINED -> GENERAL once, cleared to zero (guest memory starts zeroed too).
    vk_end_rendering();
    VkCommandBuffer         cb = vk_cmd();
    VkImageSubresourceRange rg = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    VkImageMemoryBarrier2   b  = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = im->image,
          .subresourceRange = rg, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                            .pImageMemoryBarriers = &b };
    vkCmdPipelineBarrier2(cb, &di);
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT && !is_compressed(t->format)) {
        // $RECOMP_VK_DEBUG_FILL=1: new images start in a colour picked from their address
        // (shows what is sampled or presented but never written)
        static int fill = -1;
        if (fill < 0)
            fill = getenv("RECOMP_VK_DEBUG_FILL") != NULL;
        uint32_t          k    = (uint32_t)(t->obj >> 5) % 6 + 1;
        VkClearColorValue zero = { { fill ? (float)(k & 1) : 0, fill ? (float)((k >> 1) & 1) : 0,
                                     fill ? (float)((k >> 2) & 1) : 0, fill ? 1.0f : 0 } };
        vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    } else if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearDepthStencilValue z = { 1.0f, 0 };
        vkCmdClearDepthStencilImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    }
    vk_full_barrier(cb);
    VK_LOG("image for texture 0x%llx: %ux%u x%u levels %u target %u, NVN format 0x%x -> vk %d",
           (unsigned long long)t->obj, t->w, t->h, ci.arrayLayers, levels, t->target, t->format, vf);
    return im;
}

static bool same(const rnd_tex_t *a, const rnd_tex_t *b)
{
    return a->w == b->w && a->h == b->h && a->depth == b->depth && a->levels == b->levels &&
           a->format == b->format && a->addr == b->addr;
}

vk_image_t *vk_image_of(const rnd_tex_t *t)
{
    if (!t || !t->obj || !t->w || !t->h || t->w > 16384 || t->h > 16384)
        return NULL;
    uint32_t h = (uint32_t)((t->obj >> 3) * 2654435761u) % IMG_SLOTS;
    for (uint32_t i = 0; i < IMG_SLOTS; ++i) {
        uint32_t s = (h + i) % IMG_SLOTS;
        if (!g_images[s]) {
            g_images[s] = create(t);
            return g_images[s];
        }
        if (g_images[s]->key.obj == t->obj) {
            if (!same(&g_images[s]->key, t)) {
                destroy(g_images[s]);
                g_images[s] = create(t);
            }
            return g_images[s];
        }
    }
    return NULL; // table full
}

// ---- views ---------------------------------------------------------------------------------

VkImageView vk_image_view(vk_image_t *im, uint32_t layer, uint32_t level)
{
    if (layer >= im->layers) layer = 0;
    if (level >= im->levels) level = 0;
    if (!layer && !level)
        return im->view;
    for (int i = 0; i < im->nviews; ++i)
        if (im->views[i].layer == layer && im->views[i].level == level)
            return im->views[i].view;
    if (im->nviews == (int)(sizeof im->views / sizeof im->views[0]))
        return im->view;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = im->format,
        .subresourceRange = { im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect, level, 1,
                              layer, 1 },
    };
    VkImageView v;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &v));
    im->views[im->nviews].layer = layer;
    im->views[im->nviews].level = level;
    im->views[im->nviews].view  = v;
    ++im->nviews;
    return v;
}

VkImageView vk_image_sample_view(vk_image_t *im, int kind)
{
    if (kind < 0 || kind > 2 || (kind == 2 && !im->cube))
        return VK_NULL_HANDLE;
    if (im->sample[kind])
        return im->sample[kind];
    static const VkImageViewType types[3] = { VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                              VK_IMAGE_VIEW_TYPE_CUBE };
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image, .viewType = types[kind],
        .format = im->format,
        .subresourceRange = { im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect, 0,
                              im->levels, 0, kind == 0 ? 1 : kind == 2 ? 6 : im->layers },
    };
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &im->sample[kind]));
    return im->sample[kind];
}

// ---- uploads from guest memory -------------------------------------------------------------

void *mem_host(uint64_t addr);
int   mem_is_mapped(uint64_t addr, uint64_t size);

static uint64_t hash_full(const uint8_t *p, uint64_t n, bool *nonzero)
{
    uint64_t h = 1469598103934665603ull, any = 0, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        any |= v;
        h = (h ^ v) * 1099511628211ull;
    }
    for (; i < n; ++i) {
        any |= p[i];
        h = (h ^ p[i]) * 1099511628211ull;
    }
    *nonzero = any != 0;
    return h;
}

// 64 spans of 64 bytes spread over the storage.
static uint64_t hash_sample(const uint8_t *p, uint64_t n)
{
    bool nz;
    if (n <= 4096)
        return hash_full(p, n, &nz);
    uint64_t h = 1469598103934665603ull;
    for (uint64_t k = 0; k < 64; ++k) {
        uint64_t v[8];
        memcpy(v, p + (n - 64) * k / 63, 64);
        for (int j = 0; j < 8; ++j) h = (h ^ v[j]) * 1099511628211ull;
    }
    return h;
}

static void layout_of(const vk_image_t *im, TexLayout *l)
{
    const rnd_tex_t *t = &im->key;
    memset(l, 0, sizeof *l);
    l->width  = t->w;
    l->height = t->h;
    l->depth  = 1;
    l->layers = im->layers;
    l->levels = im->levels;
    l->fmt    = nvn_format_info(t->format);
    tegra_layout_init(l);
    // Packaged textures (from game files) carry their block height; the low byte is assumed
    // to hold its log2, as on Switch 1 (LAS Recomp).
    uint32_t pl = (uint32_t)(t->packaged_layout & 0xFF);
    if (pl && pl <= 5)
        l->block_height_log2 = pl;
}

static void upload(vk_image_t *im, const uint8_t *mem)
{
    TexLayout L;
    layout_of(im, &L);
    const FormatInfo *fi         = L.fmt;
    bool              astc       = is_astc(im->key.format);
    bool              srgb       = im->key.format >= 0x87;
    uint64_t          layer_size = tegra_layer_size(&L);
    vk_end_rendering();
    VkCommandBuffer cb = vk_cmd();
    for (uint32_t layer = 0; layer < im->layers; ++layer)
        for (uint32_t lv = 0; lv < im->levels; ++lv) {
            LevelLayout       l        = tegra_level(&L, lv);
            const uint8_t    *src      = mem + layer * layer_size + l.offset;
            uint64_t          lin_size = (uint64_t)l.wb * l.hb * fi->bpb;
            VkBuffer          buf;
            VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1 },
                                     .imageExtent      = { l.w, l.h, 1 } };
            if (astc) { // blocks -> linear block rows -> RGBA8 texels
                uint8_t *blocks = malloc(lin_size);
                tegra_deswizzle(src, blocks, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, l.block_depth_log2);
                uint8_t *out = staging((uint64_t)l.w * l.h * 4, &buf);
                astc_decode_ldr(blocks, l.w, l.h, fi->bw, fi->bh, srgb, out);
                free(blocks);
                rg.bufferRowLength   = l.w;
                rg.bufferImageHeight = l.h;
            } else {
                uint8_t *out = staging(lin_size, &buf);
                tegra_deswizzle(src, out, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, l.block_depth_log2);
                rg.bufferRowLength   = l.wb * fi->bw;
                rg.bufferImageHeight = l.hb * fi->bh;
            }
            vkCmdCopyBufferToImage(cb, buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    vk_full_barrier(cb);
    static int logged;
    if (logged < 200 && ++logged)
        VK_LOG("upload texture 0x%llx at 0x%llx: %ux%u x%u levels %u format 0x%x block height %u (packaged layout 0x%llx)",
               (unsigned long long)im->key.obj, (unsigned long long)im->key.addr, im->key.w, im->key.h, im->layers,
               im->levels, im->key.format, L.block_height_log2, (unsigned long long)im->key.packaged_layout);
}

void vk_image_sync(vk_image_t *im)
{
    static int off = -1;
    if (off < 0)
        off = getenv("RECOMP_VK_NO_UPLOAD") != NULL;
    if (off || im->gpu_written || !im->can_upload || !im->key.addr || im->checked_frame == g_vk_frame + 1)
        return;
    // look again only every 8 frames (after an upload: a sample first)
    if (im->checked_frame && ((g_vk_frame + (im->key.obj >> 6)) & 7))
        return;
    im->checked_frame = g_vk_frame + 1;
    TexLayout L;
    layout_of(im, &L);
    uint64_t       total = tegra_total_size(&L);
    if (!total || !mem_is_mapped(im->key.addr, total)) {
        im->can_upload = false; // (a pool whose storage is not known, or a wrong size)
        return;
    }
    const uint8_t *mem = mem_host(im->key.addr);
    if (im->uploaded && hash_sample(mem, total) == im->sample_hash)
        return;
    bool     nonzero;
    uint64_t h      = hash_full(mem, total, &nonzero);
    im->sample_hash = hash_sample(mem, total);
    if (h == im->data_hash || !nonzero)
        return;
    im->data_hash = h;
    im->uploaded  = true;
    upload(im, mem);
}
#endif // RECOMP_VULKAN
