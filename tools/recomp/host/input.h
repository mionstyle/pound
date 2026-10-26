#ifndef RECOMP_INPUT_H
#define RECOMP_INPUT_H

#include <stdint.h>

typedef struct
{
    uint64_t buttons;        // nn::hid::NpadButton bits
    int32_t  lx, ly, rx, ry; // sticks, -32767..32767 (up is positive)
} input_state_t;

// Reads the host controls. `focused`: the game window has keyboard focus.
void input_poll(input_state_t *s, int focused);

// Implemented by present.c: whether the game window is the foreground window.
int present_has_focus(void);

#endif
