#ifndef MA2_SPRING_RATE_H
#define MA2_SPRING_RATE_H
#include <math.h>
#include <string.h>
#include <stdint.h>
/* Experimental shared spring clock. The original 30 Hz discrete map can have
 * a negative unstable eigenvalue; subdividing its Euler step changes when its
 * authored sign-crossing clamp fires. Keep that map and schedule its ticks by
 * elapsed time instead. No frame limiter, actor ID, mission or cinema filter. */
typedef struct Ma2SpringClock {
    uint32_t key, vtable, frame, valid;
    double remainder;
    double last_seen;
} Ma2SpringClock;
#define MA2_SPRING_CLOCK_SLOTS 2048u
static RECOMP_TLS Ma2SpringClock ma2_spring_clocks[MA2_SPRING_CLOCK_SLOTS];
static RECOMP_TLS double ma2_spring_last_time;
static RECOMP_TLS uint32_t ma2_spring_last_frame, ma2_spring_epoch_valid;
static void ma2_spring_rate_reset(void) {
    memset(ma2_spring_clocks, 0, sizeof ma2_spring_clocks);
    ma2_spring_epoch_valid = 0;
}
static void ma2_spring_axis_tick(float *position, float *velocity, float k, float d) {
    const float h = (float)(1.0 / 30.0);
    float old = *position;
    float a = 0.0f - old * k;
    a = a - *velocity * d;
    float next_v = a * h;
    next_v = next_v + *velocity;
    float next_p = h * next_v;
    next_p = next_p + old;
    if ((old < 0.0f && next_p > 0.0f) || (old > 0.0f && next_p < 0.0f)) {
        next_p = 0.0f; next_v = 0.0f;
    }
    *position = next_p; *velocity = next_v;
}
static int ma2_spring_rate_step(uint32_t key, uint32_t vtable, uint32_t frame,
                               double dt, double total_time, float k, float d,
                               float target_p, float target_r,
                               float *p, float *r, float *vp, float *vr) {
    const double h = 1.0 / 30.0;
    if (!(dt >= 0.0 && dt <= 0.2) || !isfinite(total_time) || !isfinite(k) || !isfinite(d) ||
        !isfinite(*p) || !isfinite(*r) || !isfinite(*vp) || !isfinite(*vr) ||
        !isfinite(target_p) || !isfinite(target_r)) return 0;
    if(ma2_tank_step_scope.active||ma2_tank_step_swapped)return 0;
    int _tank_tick=ma2_tank_response_tick(key,vtable);
    if(_tank_tick>=0){
        if(!_tank_tick)return 2;
        ma2_spring_axis_tick(p,vp,k,d);ma2_spring_axis_tick(r,vr,k,d);
        return 1;
    }
    Ma2SpringClock *s = NULL;
    if (ma2_spring_epoch_valid && (total_time < ma2_spring_last_time || frame < ma2_spring_last_frame))
        ma2_spring_rate_reset();
    ma2_spring_last_time = total_time; ma2_spring_last_frame = frame;
    ma2_spring_epoch_valid = 1;
    Ma2SpringClock *inactive = NULL;
    unsigned start = (key >> 4) & (MA2_SPRING_CLOCK_SLOTS - 1u);
    for (unsigned probe = 0; probe < MA2_SPRING_CLOCK_SLOTS; ++probe) {
        Ma2SpringClock *slot = &ma2_spring_clocks[(start + probe) & (MA2_SPRING_CLOCK_SLOTS - 1u)];
        if (slot->valid && slot->key == key) { s = slot; break; }
        if (!slot->valid) { s = inactive ? inactive : slot; s->valid = 0; break; }
        if (!inactive && total_time - slot->last_seen > 1.0) inactive = slot;
    }
    /* Capacity exhaustion keeps original integration, never replaces another
     * active clock and repeatedly discards its elapsed time. */
    if (!s && inactive) { s = inactive; s->valid = 0; }
    if (!s) return 0;
    /* Only phase is retained: every tick reads current pose, velocities and
     * caller coefficients. External edits cannot restore cached physics state.
     * Reclaimed/rebound transforms and counter resets start a fresh local phase.
     * A still-bound sparse callback retains phase even after a long gap. */
    if (!s->valid || s->vtable != vtable || frame < s->frame) {
        s->remainder = 0.0;
    }
    s->key = key; s->vtable = vtable; s->frame = frame; s->valid = 1;
    s->last_seen = total_time;
    double elapsed = s->remainder + dt;
    unsigned ticks = (unsigned)floor(elapsed / h + 1.0e-10);
    s->remainder = elapsed - ticks * h;
    if (s->remainder < 0.0) s->remainder = 0.0;
    for (unsigned i = 0; i < ticks; ++i) {
        ma2_spring_axis_tick(p, vp, k, d);
        ma2_spring_axis_tick(r, vr, k, d);
        if (i + 1u < ticks) {
            *p = (*p + target_p) - target_p;
            *r = (*r + target_r) - target_r;
        }
    }
    /* Run the untouched original integration at the exact reference step,
     * preserving its guest register effects as well as its float operations. */
    if (dt == h || dt == 0.0) return 0;
    return ticks ? 1 : 2;
}
#endif
