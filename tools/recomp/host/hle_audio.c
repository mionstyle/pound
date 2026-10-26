// Audio HLE. The audio renderer is not emulated yet: the SDK still opens and starts a
// renderer through aud:u (see ipc.c), but each update request is answered here, before
// the SDK serialises it, so no renderer output has to be produced.
//
// Next stage: read the AudioRendererConfig (voices, wave buffers, mixes) and mix on the host.

#include "hle.h"

// nn::Result nn::audio::RequestUpdateAudioRenderer(AudioRendererHandle, const AudioRendererConfig*)
static void hle_RequestUpdateAudioRenderer(cpu_t *c)
{
    static int logged;
    if (!logged++)
        host_log("hle: nn::audio::RequestUpdateAudioRenderer -> success (audio output not implemented)");
    c->x[0] = 0;
}

const hle_def_t hle_defs_audio[] = {
    { "_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE",
      hle_RequestUpdateAudioRenderer },
    { NULL, NULL },
};
