// Input HLE. The controller-support applet is not available, so ShowControllerSupport
// reports that player 1 (Npad id 0) is connected, as when the user confirms the
// controller screen.
//
// Next stage: expose a connected controller through the hid shared memory and feed it
// from the host keyboard / gamepad.

#include "hle.h"

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

const hle_def_t hle_defs_hid[] = {
    { "_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
      hle_ShowControllerSupport },
    { "_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", hle_ShowControllerSupportNoResult },
    { NULL, NULL },
};
