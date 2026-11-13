/* pc_stick.h -- keyboard diagonal normalisation (FT-RT, R2-INPUT P3-1). Header only; unit-tested by
 * tests/ft_rt_test.c, which also measures the W against W+D stick magnitude through the real
 * xinput_device.c.
 *
 * Two keys on different axes give a left stick of (+-32767, +-32767): magnitude 1.414 of full
 * deflection, which no real stick can produce (its gate is a circle of radius 32767). The guest reads
 * the two axes separately, so a keyboard player moved about 41 % faster on diagonals than a pad
 * player. With the switch on, a deflection beyond the circle is scaled back onto it: W alone is
 * unchanged, W+D becomes (23170, 23170), magnitude 32767.
 *
 * Switch order (pc_switch.h): env RECOMP_KEY_DIAG_NORM, then [Input] DiagonalNormalise, then 1. */
#pragma once
#include <math.h>

/* Scales (x, y) onto the circle of radius 32767 when it lies outside it. Pad-sized values (inside the
 * circle) and single-axis values pass through unchanged. */
static __inline void pc_stick_clamp_circle(int *x, int *y)
{
    double magnitude;
    if (!*x || !*y) return;
    magnitude = sqrt((double)*x * (double)*x + (double)*y * (double)*y);
    if (magnitude <= 32767.0) return;
    {
        double sx = (double)*x * 32767.0 / magnitude, sy = (double)*y * 32767.0 / magnitude;
        *x = (int)(sx + (sx < 0 ? -0.5 : 0.5));
        *y = (int)(sy + (sy < 0 ? -0.5 : 0.5));
    }
}
