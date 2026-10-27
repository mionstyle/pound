// Renderer debugging: $RECOMP_VK_DUMP_TARGETS=<frame> writes every colour image rendered
// during that frame (as it is at the end of the frame) to targets_<frame>/NN_<texture>.ppm,
// NN = order of first use. Float formats are converted by a blit to RGBA8 (clamped to 0..1).
#ifdef RECOMP_VULKAN
#include "vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define MAX_DUMP 96
static struct
{
    vk_image_t    *im;
    VkImage        tmp;
    VkDeviceMemory tmp_mem, buf_mem;
    VkBuffer       buf;
    void          *ptr;
    uint32_t       w, h;
} g_dump[MAX_DUMP];
static int  g_ndump;
static long g_want = -2;

static long want_frame(void)
{
    if (g_want == -2) {
        const char *e = getenv("RECOMP_VK_DUMP_TARGETS");
        g_want        = e ? atol(e) : -1;
    }
    return g_want;
}

// (draw.c) a colour target was rendered or cleared
void vk_debug_target_written(vk_image_t *im)
{
    if (want_frame() < 0 || (long)g_vk_frame + 1 != g_want || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return;
    for (int i = 0; i < g_ndump; ++i)
        if (g_dump[i].im == im)
            return;
    if (g_ndump < MAX_DUMP)
        g_dump[g_ndump++].im = im;
}

static VkDeviceMemory alloc_for(VkMemoryRequirements mr, VkMemoryPropertyFlags f)
{
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, f) };
    VkDeviceMemory       m;
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &m));
    return m;
}

// (device.c, before the frame's submit) copies of every target into host memory
void vk_debug_frame_end(VkCommandBuffer cb)
{
    if (!g_ndump)
        return;
    vk_full_barrier(cb);
    for (int i = 0; i < g_ndump; ++i) {
        vk_image_t *im = g_dump[i].im;
        uint32_t    w = im->key.w, h = im->key.h;
        g_dump[i].w = w;
        g_dump[i].h = h;
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1,
                                 .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VK_CHECK(vkCreateImage(g_vk.dev, &ci, NULL, &g_dump[i].tmp));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_vk.dev, g_dump[i].tmp, &mr);
        g_dump[i].tmp_mem = alloc_for(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindImageMemory(g_vk.dev, g_dump[i].tmp, g_dump[i].tmp_mem, 0));
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (uint64_t)w * h * 4,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &g_dump[i].buf));
        vkGetBufferMemoryRequirements(g_vk.dev, g_dump[i].buf, &mr);
        g_dump[i].buf_mem = alloc_for(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkBindBufferMemory(g_vk.dev, g_dump[i].buf, g_dump[i].buf_mem, 0));
        VK_CHECK(vkMapMemory(g_vk.dev, g_dump[i].buf_mem, 0, VK_WHOLE_SIZE, 0, &g_dump[i].ptr));

        VkImageMemoryBarrier2 b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .image = g_dump[i].tmp, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED };
        VkDependencyInfo      di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                                     .pImageMemoryBarriers = &b };
        vkCmdPipelineBarrier2(cb, &di);
        VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .srcOffsets     = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .dstOffsets     = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } } };
        if (im->blit_src)
            vkCmdBlitImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, g_dump[i].tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                           VK_FILTER_NEAREST);
        vk_full_barrier(cb);
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
        vkCmdCopyImageToBuffer(cb, g_dump[i].tmp, VK_IMAGE_LAYOUT_GENERAL, g_dump[i].buf, 1, &r);
    }
}

// (device.c, after the frame's submit) waits for the GPU and writes the files
void vk_debug_frame_done(void)
{
    if (!g_ndump)
        return;
    vkDeviceWaitIdle(g_vk.dev);
    char dir[64];
    snprintf(dir, sizeof dir, "targets_%ld", g_want);
#ifdef _WIN32
    _mkdir(dir);
#endif
    for (int i = 0; i < g_ndump; ++i) {
        char path[160];
        snprintf(path, sizeof path, "%s/%02d_%llx_%ux%u_f%x.ppm", dir, i, (unsigned long long)g_dump[i].im->key.obj,
                 g_dump[i].w, g_dump[i].h, g_dump[i].im->key.format);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", g_dump[i].w, g_dump[i].h);
            const uint8_t *p = g_dump[i].ptr;
            for (uint64_t k = 0; k < (uint64_t)g_dump[i].w * g_dump[i].h; ++k) fwrite(p + 4 * k, 1, 3, f);
            fclose(f);
        }
        vkDestroyImage(g_vk.dev, g_dump[i].tmp, NULL);
        vkFreeMemory(g_vk.dev, g_dump[i].tmp_mem, NULL);
        vkDestroyBuffer(g_vk.dev, g_dump[i].buf, NULL);
        vkFreeMemory(g_vk.dev, g_dump[i].buf_mem, NULL);
    }
    host_log("[vk] frame %ld: %d render targets written to %s/", g_want, g_ndump, dir);
    g_ndump = 0;
    g_want  = -1;
}
#endif // RECOMP_VULKAN
