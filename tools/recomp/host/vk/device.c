// Vulkan device, window surface, swapchain and frames (adapted from LAS Recomp's
// runtime/nvn/vulkan/device.c; see vk.h). The window itself is present.c's Win32 window.
//
// Frames: FRAMES command buffers rotate; each one collects everything the guest does
// between two presents and is submitted at present together with the copy of the
// presented texture into the swapchain image.
//
// Environment: RECOMP_GPU=0 (no Vulkan: CPU path), RECOMP_GPU_INDEX=<n> (pick a device),
// RECOMP_VK_VALIDATION=1 (Khronos validation layer, messages in the log),
// RECOMP_VK_PRESENT=fifo|mailbox|immediate (default fifo: the game paces itself at 60 Hz).
#ifdef RECOMP_VULKAN
#include "vk.h"
#include "../present.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

VkG      g_vk;
uint64_t g_vk_frame;

_Noreturn void vk_fatal(void)
{
    host_log("[vk] fatal Vulkan error, exiting");
    fflush(stdout);
    fflush(stderr);
    ExitProcess(3);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user)
{
    (void)type;
    (void)user;
    static int n;
    if (n++ < 500)
        VK_LOG("%s: %s", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "warning", d->pMessage);
    return VK_FALSE;
}

static int create_instance(void)
{
    const char *ext[4] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    uint32_t    n_ext  = 2;
    const char *e        = getenv("RECOMP_VK_VALIDATION");
    int         validate = e && *e && *e != '0';
    if (validate)
        ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char       *layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkApplicationInfo app      = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "recomp_run",
                                   .pEngineName = "pound-recomp", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci    = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                   .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                                   .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers };
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        VK_LOG("validation layer unavailable (%d); continuing without", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = 0;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) {
        VK_LOG("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)", r);
        return 0;
    }
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debug_cb };
        VkDebugUtilsMessengerEXT m;
        if (f && f(g_vk.inst, &mi, NULL, &m) == VK_SUCCESS)
            VK_LOG("validation enabled");
    }
    return 1;
}

// Discrete GPU first ($RECOMP_GPU_INDEX picks one); Vulkan 1.3 and a queue that can present.
static int pick_device(void)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) {
        VK_LOG("no Vulkan device");
        return 0;
    }
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    const char *e    = getenv("RECOMP_GPU_INDEX");
    int         want = e ? atoi(e) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        VK_LOG("gpu %u: %s (api %u.%u.%u, driver 0x%x)", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                           : 1;
        if ((int)i == want)
            score = 100;
        if (score > best_score) {
            best_score = score;
            best       = (int)i;
        }
    }
    if (best < 0) {
        free(pd);
        VK_LOG("no Vulkan 1.3 device");
        return 0;
    }
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.mem);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            g_vk.qfam = i;
            break;
        }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) {
        VK_LOG("no graphics+present queue");
        return 0;
    }
    VK_LOG("using %s", g_vk.props.deviceName);
    return 1;
}

static int create_device(void)
{
    float                   prio = 1.0f;
    VkDeviceQueueCreateInfo qi   = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                     .queueCount = 1, .pQueuePriorities = &prio };
    // Features the renderer will need for the guest's draws (dynamic rendering, sync2,
    // timeline semaphores, buffer device address for the guest heap); all present on any
    // Vulkan 1.3 device.
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .pNext = &f13, .timelineSemaphore = VK_TRUE,
                                             .bufferDeviceAddress = VK_TRUE, .scalarBlockLayout = VK_TRUE };
    VkPhysicalDeviceFeatures2 f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    VkPhysicalDeviceFeatures  have;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    f.features.textureCompressionBC     = have.textureCompressionBC;
    f.features.textureCompressionASTC_LDR = have.textureCompressionASTC_LDR;
    f.features.shaderInt64              = have.shaderInt64;
    f.features.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    f.features.independentBlend         = have.independentBlend;
    const char        *ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci    = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext };
    VkResult r = vkCreateDevice(g_vk.phys, &ci, NULL, &g_vk.dev);
    if (r != VK_SUCCESS) {
        VK_LOG("vkCreateDevice: %d", r);
        return 0;
    }
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam };
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));
    return 1;
}

uint32_t vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g_vk.mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_vk.mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    for (uint32_t i = 0; i < g_vk.mem.memoryTypeCount; i++)
        if (bits & (1u << i))
            return i;
    return 0;
}

// ---- swapchain -------------------------------------------------------------------------

static HWND g_hwnd;

static VkPresentModeKHR present_mode(void)
{
    const char      *e    = getenv("RECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "fifo")  ? VK_PRESENT_MODE_FIFO_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                    : VK_PRESENT_MODE_MAILBOX_KHR;
    uint32_t         n    = 0;
    VkPresentModeKHR m[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++)
        if (m[i] == want)
            return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX
                         ? caps.currentExtent
                         : (VkExtent2D){ (uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top) };
    if (!ext.width || !ext.height)
        return; // minimised
    uint32_t           nf = 0;
    VkSurfaceFormatKHR fmts[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // UNORM: the window shows the bytes the game wrote (it applies its own gamma).
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sf = fmts[i];
            break;
        }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR           old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci  = {
         .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
         .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
         .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
         .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
         .oldSwapchain = old,
    };
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    if (old)
        vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count  = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    VK_LOG("swapchain %ux%u, %u images, format %d, present mode %d", ext.width, ext.height, g_vk.sc_count, sf.format,
           ci.presentMode);
}

static void recreate_swapchain(void)
{
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ---- frames ------------------------------------------------------------------------------

#define FRAMES VK_FRAMES
static struct
{
    VkCommandBuffer cb;
    VkFence         fence;      // signalled when the frame's submission completed
    VkSemaphore     acquired;   // swapchain image acquired
    int             recording;
} g_fr[FRAMES];
static VkSemaphore g_done[8]; // per swapchain image: rendering finished, present may go
static int         g_slot;

static VkSemaphore binary_semaphore(void)
{
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore           s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void init_frames(void)
{
    for (int i = 0; i < FRAMES; i++) {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_fr[i].cb));
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &g_fr[i].fence));
        g_fr[i].acquired = binary_semaphore();
    }
    for (int i = 0; i < 8; i++) g_done[i] = binary_semaphore();
}

VkCommandBuffer vk_cmd(void)
{
    if (!g_fr[g_slot].recording) {
        // The slot's previous submission must be done before its command buffer is reused.
        VK_CHECK(vkWaitForFences(g_vk.dev, 1, &g_fr[g_slot].fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetCommandBuffer(g_fr[g_slot].cb, 0));
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VK_CHECK(vkBeginCommandBuffer(g_fr[g_slot].cb, &bi));
        g_fr[g_slot].recording = 1;
        vk_draw_frame_begin(g_slot);
    }
    return g_fr[g_slot].cb;
}

int vk_slot(void) { return g_slot; }

void vk_full_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier2 mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
    VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
    vkCmdPipelineBarrier2(cb, &di);
}

static void sc_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                            .pImageMemoryBarriers = &b };
    vkCmdPipelineBarrier2(cb, &di);
}

// Submits the current frame's work. With a swapchain image index, waits for its
// acquisition and signals g_done[image] for the present.
static void submit(int image)
{
    VkCommandBuffer cb = vk_cmd();
    VK_CHECK(vkEndCommandBuffer(cb));
    g_fr[g_slot].recording = 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo         si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (image >= 0) {
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &g_fr[g_slot].acquired;
        si.pWaitDstStageMask    = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &g_done[image];
    }
    VK_CHECK(vkResetFences(g_vk.dev, 1, &g_fr[g_slot].fence));
    VK_CHECK(vkQueueSubmit(g_vk.queue, 1, &si, g_fr[g_slot].fence));
}

// $RECOMP_DUMP_FRAMES in GPU mode: the presented texture is copied into a host buffer and
// written as PPM once its frame completed (slow; for checking output headless).
static VkBuffer       g_rb_buf;
static VkDeviceMemory g_rb_mem;
static uint64_t       g_rb_size;
static void          *g_rb_ptr;

static int readback_begin(vk_image_t *src)
{
    uint64_t need = (uint64_t)src->key.w * src->key.h * 4;
    if (need > g_rb_size) {
        if (g_rb_buf) {
            vkDeviceWaitIdle(g_vk.dev);
            vkDestroyBuffer(g_vk.dev, g_rb_buf, NULL);
            vkFreeMemory(g_vk.dev, g_rb_mem, NULL);
        }
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &g_rb_buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g_vk.dev, g_rb_buf, &mr);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                    .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &g_rb_mem));
        VK_CHECK(vkBindBufferMemory(g_vk.dev, g_rb_buf, g_rb_mem, 0));
        VK_CHECK(vkMapMemory(g_vk.dev, g_rb_mem, 0, VK_WHOLE_SIZE, 0, &g_rb_ptr));
        g_rb_size = need;
    }
    VkCommandBuffer   cb = vk_cmd();
    VkBufferImageCopy r  = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { src->key.w, src->key.h, 1 } };
    vk_full_barrier(cb);
    vkCmdCopyImageToBuffer(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, g_rb_buf, 1, &r);
    return 1;
}

static int g_flip = 1; // the window texture's row 0 is the bottom of the screen (lower-left origin)

static void readback_end(vk_image_t *src, uint64_t frame)
{
    VK_CHECK(vkWaitForFences(g_vk.dev, 1, &g_fr[g_slot].fence, VK_TRUE, UINT64_MAX));
    uint32_t  w = src->key.w, h = src->key.h;
    uint32_t *px = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y) // upright: row 0 at the top
        memcpy(px + (size_t)y * w, (uint8_t *)g_rb_ptr + (size_t)(g_flip ? h - 1 - y : y) * w * 4, (size_t)w * 4);
    present_dump(px, (int)w, (int)h, frame);
    free(px);
}

void rnd_present(const rnd_tex_t *t, const int32_t crop[4], uint64_t frame)
{
    void vk_draw_stats(void);
    g_vk_frame = frame;
    if (frame % 300 == 0)
        vk_draw_stats();
    vk_end_rendering();
    vk_image_t *src = vk_image_of(t);
    RECT        rc;
    GetClientRect(g_hwnd, &rc);
    uint32_t ww = (uint32_t)(rc.right - rc.left), wh = (uint32_t)(rc.bottom - rc.top);
    if (ww && wh && (!g_vk.swapchain || ww != g_vk.sc_extent.width || wh != g_vk.sc_extent.height))
        recreate_swapchain();
    int image = -1;
    if (ww && wh && g_vk.swapchain) {
        // The acquire semaphore of this slot is free: its previous submission (which
        // waited on it) completed before vk_cmd() began recording the slot again.
        vk_cmd();
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, g_fr[g_slot].acquired, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, g_fr[g_slot].acquired, VK_NULL_HANDLE, &idx);
        }
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            image = (int)idx;
    }
    if (image >= 0) {
        VkCommandBuffer cb = vk_cmd();
        VkImage         dst = g_vk.sc_images[image];
        vk_full_barrier(cb);
        sc_barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue       black = { { 0, 0, 0, 1 } };
        VkImageSubresourceRange rg    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
        if (src && src->blit_src) {
            // The crop region, letterboxed to the window keeping its aspect ratio.
            int32_t cx = 0, cy = 0, cw = (int32_t)t->w, ch = (int32_t)t->h;
            if (crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= cw && crop[1] + crop[3] <= ch)
                cx = crop[0], cy = crop[1], cw = crop[2], ch = crop[3];
            float    sx = (float)ww / (float)cw, sy = (float)wh / (float)ch, s = sx < sy ? sx : sy;
            int32_t  dw = (int32_t)(cw * s), dh = (int32_t)(ch * s);
            int32_t  dx = ((int32_t)ww - dw) / 2, dy = ((int32_t)wh - dh) / 2;
            VkImageBlit b = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets     = { { cx, g_flip ? cy + ch : cy, 0 }, { cx + cw, g_flip ? cy : cy + ch, 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets     = { { dx, dy, 0 }, { dx + dw, dy + dh, 1 } },
            };
            vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                           VK_FILTER_LINEAR);
        }
        sc_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    int dump = src && present_dump_wanted(frame) && src->format == VK_FORMAT_R8G8B8A8_UNORM && readback_begin(src);
    submit(image);
    if (dump)
        readback_end(src, frame);
    if (image >= 0) {
        uint32_t         idx = (uint32_t)image;
        VkPresentInfoKHR pi  = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &g_done[image], .swapchainCount = 1, .pSwapchains = &g_vk.swapchain,
                                 .pImageIndices = &idx };
        VkResult         r   = vkQueuePresentKHR(g_vk.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            recreate_swapchain();
        else if (r != VK_SUCCESS)
            VK_LOG("present: %d", r);
    }
    g_slot = (g_slot + 1) % FRAMES;
}

void rnd_clear_color(const rnd_tex_t *t, uint32_t mask, const float rgba[4])
{
    vk_image_t *im = vk_image_of(t);
    if (!im || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return;
    vk_end_rendering();
    VkCommandBuffer cb = vk_cmd();
    // Channel masks need a draw; a clear of every channel is the common case.
    if ((mask & 0xF) != 0xF) {
        static int logged;
        if (logged++ < 10)
            VK_LOG("clear with channel mask 0x%x done as a full clear", mask);
    }
    im->gpu_written = true;
    VkClearColorValue       v  = { { rgba[0], rgba[1], rgba[2], rgba[3] } };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg);
    vk_full_barrier(cb);
}

void vk_set_present_flip(int flip) { g_flip = flip; }

int rnd_init(void)
{
    static int state = -1;
    if (state >= 0)
        return state;
    state         = 0;
    const char *e = getenv("RECOMP_GPU");
    if (e && *e == '0') {
        host_log("[vk] disabled by RECOMP_GPU=0");
        return 0;
    }
    g_hwnd = (HWND)present_window();
    if (!g_hwnd || !create_instance())
        return 0;
    VkWin32SurfaceCreateInfoKHR si = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                       .hinstance = GetModuleHandleA(NULL), .hwnd = g_hwnd };
    if (vkCreateWin32SurfaceKHR(g_vk.inst, &si, NULL, &g_vk.surface) != VK_SUCCESS) {
        VK_LOG("cannot create the window surface");
        return 0;
    }
    if (!pick_device() || !create_device())
        return 0;
    init_frames();
    create_swapchain();
    present_set_gpu(1);
    state = 1;
    return 1;
}
#endif // RECOMP_VULKAN
