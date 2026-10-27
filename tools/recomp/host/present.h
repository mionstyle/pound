#ifndef RECOMP_PRESENT_H
#define RECOMP_PRESENT_H

#include <stdint.h>

// Shows a presented image (RGBA8, row-major, w*h pixels). `frame` counts from 1.
void present_frame(const uint32_t *rgba, int w, int h, uint64_t frame);

// Writes every 60th frame to $RECOMP_DUMP_FRAMES (PPM), if set; also used by GPU backends.
void present_dump(const uint32_t *rgba, int w, int h, uint64_t frame);
int  present_dump_wanted(uint64_t frame);

// Windows: the game window (an HWND), created on first use; NULL elsewhere.
void *present_window(void);
// 1: a GPU backend draws into the window (present.c stops painting it with GDI).
void present_set_gpu(int on);

#endif
