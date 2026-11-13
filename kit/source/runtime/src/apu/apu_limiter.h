#ifndef APU_LIMITER_H
#define APU_LIMITER_H

#include <math.h>
#include <string.h>

/* One VP block of lookahead (32 / 48000 = 0.67 ms). The same gain is applied
 * to both channels, preserving stereo position. Ordinary audio below the
 * ceiling is passed unchanged; attenuation only follows an actual overload.
 * The preceding block provides time to ramp down before a loud transient.
 * Recovery is gradual over 80 ms instead of pumping at every sample peak. */
typedef struct MCPXAPUOutputLimiter {
    float previous[32][2];
    float gain, previous_limit;
    int initialized;
} MCPXAPUOutputLimiter;

static inline void apu_limit_block(MCPXAPUOutputLimiter *state,
                                  float samples[32][2], float out[32][2])
{
    float peak = 0.0f;
    for (int i = 0; i < 32; i++)
        for (int ch = 0; ch < 2; ch++) {
            if (!isfinite(samples[i][ch])) samples[i][ch] = 0.0f;
            peak = fmaxf(peak, fabsf(samples[i][ch]));
        }
    float limit = peak > 0.92f ? 0.92f / peak : 1.0f;
    if (!state->initialized) {
        state->gain = state->previous_limit = 1.0f;
        state->initialized = 1;
    }
    float end = state->gain + (1.0f - state->gain) * 0.008298707f;
    end = fminf(end, fminf(limit, state->previous_limit));
    for (int i = 0; i < 32; i++) {
        float gain = state->gain + (end - state->gain) * ((i + 1) / 32.0f);
        out[i][0] = state->previous[i][0] * gain;
        out[i][1] = state->previous[i][1] * gain;
    }
    memcpy(state->previous, samples, sizeof(state->previous));
    state->gain = end;
    state->previous_limit = limit;
}
#endif
