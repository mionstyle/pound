// Host input -> one Switch controller (Npad No1, Pro Controller style).
//
// Windows: keyboard (while the game window has focus) and the first XInput gamepad.
//   Keyboard: arrows / WASD = left stick, IJKL = right stick, Enter/Space = A, Backspace = B,
//   X = X, Y = Y, Q/E = L/R, 1/3 = ZL/ZR, Tab = Minus, Esc... is not mapped (window close),
//   P = Plus, F/G/H/T = D-pad left/down/right/up.
// Everywhere: $RECOMP_AUTO_PRESS=<frames> taps A every that many polls (headless testing).

#include "input.h"
#include <stdlib.h>
#include <string.h>

enum {
    B_A = 0, B_B = 1, B_X = 2, B_Y = 3, B_LS = 4, B_RS = 5, B_L = 6, B_R = 7, B_ZL = 8, B_ZR = 9,
    B_PLUS = 10, B_MINUS = 11, B_LEFT = 12, B_UP = 13, B_RIGHT = 14, B_DOWN = 15,
};

#define STICK_MAX 32767

static void auto_press(input_state_t *s)
{
    static int period = -1;
    static uint64_t polls;
    if (period < 0) {
        const char *e = getenv("RECOMP_AUTO_PRESS");
        period = e ? atoi(e) : 0;
    }
    if (period > 0 && (++polls % (uint64_t)period) < 6)
        s->buttons |= 1ull << B_A;
}

#ifdef _WIN32
#include <windows.h>

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xpad_t;
typedef struct { DWORD packet; xpad_t pad; } xstate_t;
typedef DWORD(WINAPI *xget_t)(DWORD, xstate_t *);

static int down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void input_poll(input_state_t *s, int focused)
{
    memset(s, 0, sizeof *s);
    static xget_t xget;
    static int    tried;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) xget = (xget_t)GetProcAddress(m, "XInputGetState");
    }
    if (xget) {
        xstate_t x;
        if (xget(0, &x) == 0) {
            WORD b = x.pad.buttons;
            // Nintendo layout: A is the right face button, B the bottom one.
            if (b & 0x1000) s->buttons |= 1ull << B_B;     // XInput A (bottom)
            if (b & 0x2000) s->buttons |= 1ull << B_A;     // XInput B (right)
            if (b & 0x4000) s->buttons |= 1ull << B_Y;     // XInput X (left)
            if (b & 0x8000) s->buttons |= 1ull << B_X;     // XInput Y (top)
            if (b & 0x0100) s->buttons |= 1ull << B_L;
            if (b & 0x0200) s->buttons |= 1ull << B_R;
            if (b & 0x0040) s->buttons |= 1ull << B_LS;
            if (b & 0x0080) s->buttons |= 1ull << B_RS;
            if (b & 0x0010) s->buttons |= 1ull << B_PLUS;
            if (b & 0x0020) s->buttons |= 1ull << B_MINUS;
            if (b & 0x0001) s->buttons |= 1ull << B_UP;
            if (b & 0x0002) s->buttons |= 1ull << B_DOWN;
            if (b & 0x0004) s->buttons |= 1ull << B_LEFT;
            if (b & 0x0008) s->buttons |= 1ull << B_RIGHT;
            if (x.pad.lt > 64) s->buttons |= 1ull << B_ZL;
            if (x.pad.rt > 64) s->buttons |= 1ull << B_ZR;
            s->lx = x.pad.lx; s->ly = x.pad.ly; s->rx = x.pad.rx; s->ry = x.pad.ry;
        }
    }
    if (focused) {
        static const struct { int vk; int bit; } keys[] = {
            { VK_RETURN, B_A }, { VK_SPACE, B_A }, { VK_BACK, B_B }, { 'X', B_X }, { 'Y', B_Y },
            { 'Q', B_L }, { 'E', B_R }, { '1', B_ZL }, { '3', B_ZR }, { 'P', B_PLUS }, { VK_TAB, B_MINUS },
            { 'F', B_LEFT }, { 'T', B_UP }, { 'H', B_RIGHT }, { 'G', B_DOWN },
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i)
            if (down(keys[i].vk)) s->buttons |= 1ull << keys[i].bit;
        if (down(VK_LEFT) || down('A')) s->lx = -STICK_MAX;
        if (down(VK_RIGHT) || down('D')) s->lx = STICK_MAX;
        if (down(VK_UP) || down('W')) s->ly = STICK_MAX;
        if (down(VK_DOWN) || down('S')) s->ly = -STICK_MAX;
        if (down('J')) s->rx = -STICK_MAX;
        if (down('L')) s->rx = STICK_MAX;
        if (down('I')) s->ry = STICK_MAX;
        if (down('K')) s->ry = -STICK_MAX;
    }
    auto_press(s);
}
#else
void input_poll(input_state_t *s, int focused)
{
    (void)focused;
    memset(s, 0, sizeof *s);
    auto_press(s);
}
#endif
