// Vulkan backend of the host renderer (Windows).
//
// Adapted from the Vulkan backend of LAS Recomp (github.com/peiikai/LASRecomp,
// runtime/nvn/vulkan/, MIT licence, (c) 2026 Peiikai), reduced to what this runtime uses
// so far: device.c (instance, Win32 surface, device, swapchain, frames, present) and
// textures.c (NVN formats, one image per guest texture).
//
// Synchronisation model (as in LAS Recomp): every guest image lives in
// VK_IMAGE_LAYOUT_GENERAL and a full memory barrier separates operations. All work of a
// frame goes into one command buffer, submitted at present.
#ifndef RECOMP_VK_H
#define RECOMP_VK_H

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#include "../renderer.h"

void host_log(const char *fmt, ...);
#define VK_LOG(...) host_log("[vk] " __VA_ARGS__)
#define VK_CHECK(x)                                                                    \
    do {                                                                               \
        VkResult r_ = (x);                                                             \
        if (r_ != VK_SUCCESS) {                                                        \
            VK_LOG("%s failed: %d (%s:%d)", #x, r_, __FILE__, __LINE__);               \
            vk_fatal();                                                                \
        }                                                                              \
    } while (0)

typedef struct
{
    VkInstance                       inst;
    VkPhysicalDevice                 phys;
    VkDevice                         dev;
    VkQueue                          queue;
    uint32_t                         qfam;
    VkPhysicalDeviceProperties       props;
    VkPhysicalDeviceMemoryProperties mem;
    VkCommandPool                    pool;
    // window
    VkSurfaceKHR   surface;
    VkSwapchainKHR swapchain;
    VkFormat       sc_format;
    VkExtent2D     sc_extent;
    uint32_t       sc_count;
    VkImage        sc_images[8];
} VkG;
extern VkG g_vk;

_Noreturn void vk_fatal(void);

// ---- device.c
#define VK_FRAMES 3
VkCommandBuffer vk_cmd(void);                  // the current frame's command buffer (recording)
int             vk_slot(void);
extern uint64_t g_vk_frame;                    // presents so far                 // the current frame slot (0..VK_FRAMES-1)
void            vk_end_rendering(void);        // (draw.c) close an open render pass before other commands
void            vk_full_barrier(VkCommandBuffer cb);
uint32_t        vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);

// ---- textures.c
typedef struct
{
    rnd_tex_t          key; // guest description it was created for
    VkImage            image;
    VkDeviceMemory     mem;
    VkFormat           format;
    VkImageAspectFlags aspect;
    bool               blit_src;   // the format supports vkCmdBlitImage as a source
    bool               attachment; // usable as a color / depth attachment
    VkImageView        view;       // level 0, layer 0 (rendering and 2D sampling)
    bool               gpu_written; // rendered into or cleared by the GPU at least once
} vk_image_t;

VkFormat    vk_format(uint32_t nvn_format);
vk_image_t *vk_image_of(const rnd_tex_t *t); // created on first use (NULL: unsupported)

// ---- draw.c
void     vk_draw_frame_begin(int slot); // the slot's previous frame completed: its transient memory is free
uint64_t vk_shader_hash(uint64_t code);

#endif // RECOMP_VK_H
