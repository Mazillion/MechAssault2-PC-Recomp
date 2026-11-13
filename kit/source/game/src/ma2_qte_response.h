#pragma once
#include "platform/pc_frontend.h"
#include <stdint.h>
#include <string.h>

/* Called only after the original new-cue timer write in 0018F27B.
 * Keep the authored float and mode snapshot for the entire response window. */
static inline void ma2_qte_response_window(float *remaining)
{
    uint32_t bits;
    float scaled;
    const enum PcInputDevice device = pc_input_device();
    if (device != PC_INPUT_KEYBOARD_MOUSE) return;
    memcpy(&bits, remaining, sizeof bits);
    /* Reject zero, negative and nonfinite values without float arithmetic. */
    if (bits == 0 || bits >= 0x7f800000u) return;
    scaled = *remaining * 1.25f;
    memcpy(&bits, &scaled, sizeof bits);
    if (bits < 0x7f800000u) memcpy(remaining, &scaled, sizeof scaled);
}
