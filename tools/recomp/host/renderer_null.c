// Renderer stand-in for builds without a GPU backend (Linux test builds): hle_nvn.c keeps
// its CPU path. Builds with -DRECOMP_VULKAN get host/vk/ instead.

#include "renderer.h"

#ifndef RECOMP_VULKAN
int  rnd_init(void) { return 0; }
void rnd_clear_color(const rnd_tex_t *t, uint32_t mask, const float rgba[4]) { (void)t; (void)mask; (void)rgba; }
void rnd_draw(const rnd_state_t *s, const rnd_draw_t *d) { (void)s; (void)d; }
void rnd_present(const rnd_tex_t *t, const int32_t crop[4], uint64_t frame) { (void)t; (void)crop; (void)frame; }
#endif
