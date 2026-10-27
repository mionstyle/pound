// Vulkan images for guest textures: NVN formats and one image per guest texture object.
// The format table follows LAS Recomp's runtime/nvn/vulkan/textures.c (NVN numbering, which
// NVN2 keeps for these ids). Contents are not uploaded from guest memory yet: images are
// render targets only, written by GPU commands.
#ifdef RECOMP_VULKAN
#include "vk.h"
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

// ---- image cache: open addressing on the guest object address --------------------------

#define IMG_SLOTS 8192
static vk_image_t *g_images[IMG_SLOTS];

static void destroy(vk_image_t *im)
{
    // Called only between frames for a texture object that was re-initialised with another
    // description; the frame that used the old image may still run, so wait for the GPU.
    vkDeviceWaitIdle(g_vk.dev);
    vkDestroyImageView(g_vk.dev, im->view, NULL);
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
    VkFormatFeatureFlags want = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((fp.optimalTilingFeatures & want) != want) {
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
    uint32_t levels = t->levels ? t->levels : 1;
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = vf,
        .extent = { t->w ? t->w : 1, t->h ? t->h : 1, 1 }, .mipLevels = levels,
        .arrayLayers = t->depth ? t->depth : 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vk_image_t *im = calloc(1, sizeof *im);
    im->key = *t;
    im->format = vf;
    im->aspect = aspect;
    im->blit_src = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
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
    VK_LOG("image for texture 0x%llx: %ux%u x%u levels %u, NVN format 0x%x -> vk %d", (unsigned long long)t->obj,
           t->w, t->h, ci.arrayLayers, levels, t->format, vf);
    return im;
}

static bool same(const rnd_tex_t *a, const rnd_tex_t *b)
{
    return a->w == b->w && a->h == b->h && a->depth == b->depth && a->levels == b->levels && a->format == b->format;
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
#endif // RECOMP_VULKAN
