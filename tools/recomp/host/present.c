// Host output of presented frames.
//
// Windows: a window (Win32 + GDI) shows each presented image, scaled to the client area.
// Elsewhere (and additionally when $RECOMP_DUMP_FRAMES is set to a directory), every
// 60th frame is written there as a PPM image, which is how frames are checked headless.

#include "present.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_ppm(const uint32_t *rgba, int w, int h, uint64_t frame)
{
    const char *dir = getenv("RECOMP_DUMP_FRAMES");
    if (!dir || !*dir || frame % 60 != 1)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, (unsigned long long)frame);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t p = rgba[(size_t)y * w + x];
            row[x * 3 + 0] = (uint8_t)p;
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    fclose(f);
    host_log("present: wrote %s", path);
}

#ifdef _WIN32
#include <windows.h>

static HWND             g_wnd;
static CRITICAL_SECTION g_lock;
static uint32_t        *g_pixels; // BGRA copy of the last frame
static int              g_w, g_h;
static volatile LONG    g_started;

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(h, &ps);
        RECT        rc;
        GetClientRect(h, &rc);
        EnterCriticalSection(&g_lock);
        if (g_pixels) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth       = g_w;
            bi.bmiHeader.biHeight      = -g_h; // top-down
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, g_w, g_h, g_pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        LeaveCriticalSection(&g_lock);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        ExitProcess(0);
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID arg)
{
    (void)arg;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "recomp_run";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("recomp_run", "Donkey Kong Bananza (recomp)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL,
                          wc.hInstance, NULL);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

int present_has_focus(void) { return g_wnd && GetForegroundWindow() == g_wnd; }

void present_frame(const uint32_t *rgba, int w, int h, uint64_t frame)
{
    if (InterlockedExchange(&g_started, 1) == 0) {
        InitializeCriticalSection(&g_lock);
        CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    }
    EnterCriticalSection(&g_lock);
    if (w != g_w || h != g_h) {
        free(g_pixels);
        g_pixels = malloc((size_t)w * h * 4);
        g_w = w;
        g_h = h;
    }
    for (size_t i = 0; i < (size_t)w * h; ++i) { // RGBA -> BGRA
        uint32_t p  = rgba[i];
        g_pixels[i] = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
    }
    LeaveCriticalSection(&g_lock);
    if (g_wnd)
        InvalidateRect(g_wnd, NULL, FALSE);
    dump_ppm(rgba, w, h, frame);
}
#else
int  present_has_focus(void) { return 0; }
void present_frame(const uint32_t *rgba, int w, int h, uint64_t frame) { dump_ppm(rgba, w, h, frame); }
#endif
