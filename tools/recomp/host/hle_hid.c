// Input HLE. The controller-support applet is not available, so ShowControllerSupport
// reports that player 1 (Npad id 0) is connected, as when the user confirms the
// controller screen.
//
// Next stage: expose a connected controller through the hid shared memory and feed it
// from the host keyboard / gamepad.

#include "hle.h"
#include "input.h"
#include <string.h>
#include <time.h>

// nn::Result ShowControllerSupport(ControllerSupportResultInfo* out, const ControllerSupportArg&)
static void hle_ShowControllerSupport(cpu_t *c)
{
    host_log("hle: nn::hid::ShowControllerSupport -> player 1 connected");
    if (c->x[0]) {
        wr32(c->x[0], 1);     // playerCount
        wr32(c->x[0] + 4, 0); // selectedId: Npad No1
    }
    c->x[0] = 0;
}

// nn::Result ShowControllerSupport(const ControllerSupportArg&)
static void hle_ShowControllerSupportNoResult(cpu_t *c)
{
    host_log("hle: nn::hid::ShowControllerSupport -> ok");
    c->x[0] = 0;
}

// ---- npad state ----------------------------------------------------------------------
//
// The SDK copies controller states out of the hid shared memory ring buffers. The host
// answers those reads directly: Npad No1 (id 0) is a connected Pro Controller (FullKey
// style) driven by input.c; every other id is disconnected.

#define NPAD_NO1          0u
#define STYLE_FULLKEY     (1u << 0)
#define ATTR_CONNECTED    (1u << 0)
#define ATTR_WIRED        (1u << 1)

static uint64_t sampling(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 200 + (uint64_t)t.tv_nsec / 5000000; // 200 Hz sampling counter
}

// Result nn::hid::detail::GetNpadStyleSet(BitFlagSet<32, NpadStyleTag>* out, const NpadIdType& id)
static void hle_detail_GetNpadStyleSet(cpu_t *c)
{
    uint32_t id = rd32(c->x[1]);
    wr32(c->x[0], id == NPAD_NO1 ? STYLE_FULLKEY : 0);
    c->x[0] = 0;
}

// Result nn::hid::detail::GetNpadStates(int* outCount, State* out, int count, const NpadIdType& id)
// State (0x28 bytes): s64 samplingNumber, u64 buttons, s32 lx, ly, rx, ry, u32 attributes.
static void npad_states(cpu_t *c, int fullkey)
{
    uint64_t out_count = c->x[0], out = c->x[1];
    int      count     = (int)c->x[2];
    uint32_t id        = rd32(c->x[3]);
    int      connected = fullkey && id == NPAD_NO1;
    input_state_t in;
    memset(&in, 0, sizeof in);
    if (connected)
        input_poll(&in, present_has_focus());
    uint64_t now = sampling();
    int      n   = count < 1 ? 0 : count > 16 ? 16 : count;
    for (int i = 0; i < n; ++i) {
        uint64_t e = out + 0x28ull * (uint64_t)i;
        memset(mem_host(e), 0, 0x28);
        wr64(e, now - (uint64_t)i);
        if (connected) {
            wr64(e + 8, in.buttons);
            wr32(e + 0x10, (uint32_t)in.lx);
            wr32(e + 0x14, (uint32_t)in.ly);
            wr32(e + 0x18, (uint32_t)in.rx);
            wr32(e + 0x1C, (uint32_t)in.ry);
            wr32(e + 0x20, ATTR_CONNECTED | ATTR_WIRED);
        }
    }
    wr32(out_count, (uint32_t)n);
    c->x[0] = 0;
}

static void hle_detail_GetNpadStatesFullKey(cpu_t *c) { npad_states(c, 1); }
static void hle_detail_GetNpadStatesOther(cpu_t *c) { npad_states(c, 0); }

// int GetSixAxisSensorStates(SixAxisSensorState* out, int count, const SixAxisSensorHandle&)
// One resting sample: no motion, identity orientation (state is 0x60 bytes).
static void hle_GetSixAxisSensorStates(cpu_t *c)
{
    uint64_t out = c->x[0];
    if ((int)c->x[1] < 1) { c->x[0] = 0; return; }
    memset(mem_host(out), 0, 0x60);
    wr64(out, 5000000);          // delta time (ns)
    wr64(out + 8, sampling());   // sampling number
    float one = 1.0f, g = -1.0f;
    uint32_t bits;
    memcpy(&bits, &g, 4);   wr32(out + 0x18, bits);           // acceleration z = -1 g
    memcpy(&bits, &one, 4);
    wr32(out + 0x34, bits); wr32(out + 0x44, bits); wr32(out + 0x54, bits); // direction = identity
    wr32(out + 0x58, 1);    // attributes: IsConnected
    c->x[0] = 1;
}

const hle_def_t hle_defs_hid[] = {
    { "_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
      hle_GetSixAxisSensorStates },
    { "_ZN2nn3hid6detail15GetNpadStyleSetEPNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEERKj",
      hle_detail_GetNpadStyleSet },
    { "_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadFullKeyStateEiRKj", hle_detail_GetNpadStatesFullKey },
    { "_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_17NpadHandheldStateEiRKj", hle_detail_GetNpadStatesOther },
    { "_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadJoyDualStateEiRKj", hle_detail_GetNpadStatesOther },
    { "_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadJoyLeftStateEiRKj", hle_detail_GetNpadStatesOther },
    { "_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_17NpadJoyRightStateEiRKj", hle_detail_GetNpadStatesOther },
    { "_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
      hle_ShowControllerSupport },
    { "_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", hle_ShowControllerSupportNoResult },
    { NULL, NULL },
};
