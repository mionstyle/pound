#ifndef RECOMP_PRESENT_H
#define RECOMP_PRESENT_H

#include <stdint.h>

// Shows a presented image (RGBA8, row-major, w*h pixels). `frame` counts from 1.
void present_frame(const uint32_t *rgba, int w, int h, uint64_t frame);

#endif
