// nn::vi (display / layer / native window) implemented on the host, since the
// graphics stack is provided by the host renderer instead of the system's display
// services. Mirrors what the Switch 1 recompilation projects do (LAS Recomp, hle/vi.c).

#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define R_OK 0

static uint64_t g_display, g_layer, g_native_window;
static uint32_t g_vsync_handle;
static void    *g_vsync_event;
static pthread_t g_vsync_thread;
volatile uint64_t g_vsync_count;

uint64_t vi_native_window(void) { return g_native_window; }

static void *vsync_main(void *arg)
{
    (void)arg;
    const int64_t   period = 16666667; // 60 Hz
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
#ifdef _WIN32
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t wait = (next.tv_sec - now.tv_sec) * 1000000000ll + (next.tv_nsec - now.tv_nsec);
        if (wait > 0) {
            struct timespec d = { wait / 1000000000, wait % 1000000000 };
            nanosleep(&d, NULL);
        }
#else
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
#endif
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELEASE);
        kernel_event_signal(g_vsync_event);
    }
    return NULL;
}

static void vi_Initialize(cpu_t *c) { (void)c; }

static void vi_OpenDefaultDisplay(cpu_t *c) // (Display** out)
{
    if (!g_display)
        g_display = hle_alloc(0x100, 16);
    wr64(c->x[0], g_display);
    c->x[0] = R_OK;
}

static void vi_CreateLayer(cpu_t *c) // (Layer** out, Display*)
{
    if (!g_layer)
        g_layer = hle_alloc(0x100, 16);
    wr64(c->x[0], g_layer);
    host_log("vi: layer created");
    c->x[0] = R_OK;
}

static void vi_GetNativeWindow(cpu_t *c) // (void** out, Layer*)
{
    if (!g_native_window)
        g_native_window = hle_alloc(0x1000, 16);
    wr64(c->x[0], g_native_window);
    c->x[0] = R_OK;
}

static void vi_GetDisplayVsyncEvent(cpu_t *c) // (os::SystemEventType*, Display*)
{
    uint64_t event = c->x[0];
    if (!g_vsync_event) {
        g_vsync_handle = kernel_new_event(0, &g_vsync_event);
        pthread_create(&g_vsync_thread, NULL, vsync_main, NULL);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, isManaged=false, AutoClear)
    uint64_t attach = hle_symbol_address("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    uint64_t args[4] = { event, g_vsync_handle, 0, 1 };
    guest_call(c, attach, 4, args);
    host_log("vi: vsync event attached (60 Hz)");
    c->x[0] = R_OK;
}

static void vi_out_bool_false(cpu_t *c) // (bool* out, const Display*)
{
    wr8(c->x[0], 0);
    c->x[0] = R_OK;
}

static void vi_QueryDisplayHdrPaperWhite(cpu_t *c) // (u16* out, const Display*)
{
    wr16(c->x[0], 203);
    c->x[0] = R_OK;
}

static void vi_QueryDisplayHdrStatus(cpu_t *c) // (bool* enabled, HdrOutputFormat*, u16*, const Display*)
{
    wr8(c->x[0], 0);
    wr32(c->x[1], 0);
    wr16(c->x[2], 0);
    c->x[0] = R_OK;
}

static void vi_ok(cpu_t *c) { c->x[0] = R_OK; }

const hle_def_t hle_defs_vi[] = {
    { "_ZN2nn2vi10InitializeEv", vi_Initialize },
    { "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay },
    { "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer },
    { "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow },
    { "_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_GetDisplayVsyncEvent },
    { "_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", vi_out_bool_false },
    { "_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", vi_QueryDisplayHdrPaperWhite },
    { "_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", vi_QueryDisplayHdrStatus },
    { "_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", vi_ok },
    { "_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", vi_ok },
    { "_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", vi_ok },
    { "_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", vi_ok },
    { "_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_ok },
    { "_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_ok },
    { "_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", vi_ok },
    { NULL, NULL },
};
