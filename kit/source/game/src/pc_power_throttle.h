/* 1.1.8: the process's power-throttling (EcoQoS) opt-out, with a Windows 10 fallback.
 *
 * Windows 11 knows two controls: EXECUTION_SPEED (no efficiency cores at low clocks) and IGNORE_TIMER_RESOLUTION (keep the
 * timer resolution while the window is in the background). Windows 10 knows only EXECUTION_SPEED and refuses a call that
 * names the other one with ERROR_INVALID_PARAMETER (87), so up to 1.1.7 a Windows 10 PC got no opt-out at all
 * ([LAUNCH] power throttling opt-out failed (87), report MA2-2LXVOZ). Now: both, else speed only.
 *
 * pc_power_optout(set) tries the masks in that order through set(mask) (which returns nonzero on success and leaves the error
 * in *err otherwise) and returns 2 = both, 1 = speed only, 0 = neither. tests/game/power_throttle_test.c checks the order. */
#ifndef PC_POWER_THROTTLE_H
#define PC_POWER_THROTTLE_H

#define PC_PT_EXECUTION_SPEED 0x1u
#define PC_PT_IGNORE_TIMER_RESOLUTION 0x4u

typedef int (*pc_power_set_fn)(unsigned long mask, void *ctx, unsigned long *err);

static int pc_power_optout(pc_power_set_fn set, void *ctx, unsigned long *first_err, unsigned long *last_err)
{
    unsigned long e = 0;
    *first_err = *last_err = 0;
    if (set(PC_PT_EXECUTION_SPEED | PC_PT_IGNORE_TIMER_RESOLUTION, ctx, &e)) return 2;
    *first_err = e;
    e = 0;
    if (set(PC_PT_EXECUTION_SPEED, ctx, &e)) return 1;
    *last_err = e;
    return 0;
}

/* the [LAUNCH] line for each result */
static const char *pc_power_optout_text(int r)
{
    return r == 2 ? "speed and timer resolution" : r == 1 ? "speed only (Windows 10)" : "failed";
}
#endif
