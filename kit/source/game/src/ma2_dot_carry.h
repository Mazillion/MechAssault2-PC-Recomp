/* 1.1.8 DotCarry: continuous damage at any frame rate as the game did it at 30 fps (BUG-001).
 *
 * The hit resolver sub_000DC60D turns a continuous damage rate into this frame's whole damage with _ftol(dt * dps) on three
 * paths: collapsing buildings (MABuildingDestroyed +0x1B8 DamageRate), destroyed props (MACorporealDestroyed +0x1B0
 * DamageAmount) and beam weapons (MABarrel +0x148 BarrelDamage via sub_000D16A6, and the beam record REC+0xB0). Nothing
 * carries the fraction, so the result depends on the frame rate: at 30 fps a 44 DPS beam does 1 a frame (30 a second); at
 * 60 fps 0.73 -> 0 and at 144 fps 0.31 -> 0 (the Mission 22 final boss's EyeBeam did nothing at 60 and 144 fps).
 *
 * Here each (source, victim) pair keeps a 30 Hz tick count: a tick is due every 1/30 s of game time and does
 * _ftol(dps / 30), what one 30 fps frame did. The first frame of a contact gets a tick at once, as at 30 fps. So the damage
 * over time is the 30 fps game's at any frame rate: never more than at 30 fps, and nothing that did 0 at 30 fps (20-25 DPS
 * props) starts doing damage. RECOMP_DOT_CARRY=0 = the frame's own _ftol(dt * dps), as up to 1.1.7.
 *
 * Pure logic, no game memory: recomp_manual.c reads the game and calls ma2_dot_carry_step. tests/game/dot_carry_test.c. */
#ifndef MA2_DOT_CARRY_H
#define MA2_DOT_CARRY_H
#include <stdint.h>
#include <math.h>

#define MA2_DOT_HZ 30.0
#define MA2_DOT_SLOTS 256
#define MA2_DOT_STALE 0.25          /* seconds without a hit: a new contact (a tick at once again) */

typedef struct { uint32_t src, victim; double t, acc; int32_t last; int used; } ma2_dot_slot;

/* The damage of this hit. orig = the game's own _ftol(dt * dps) (returned when the inputs look wrong); now = game time of
 * this frame in seconds (a second hit of the same pair in the same frame gets the same damage, without a second tick). */
static int32_t ma2_dot_carry_step(ma2_dot_slot *tab, uint32_t src, uint32_t victim, double now, double dt, double dps, int32_t orig)
{
    if (!tab) return orig;
    if (!(dt > 0.0) || dt > 0.5 || !(dps > 0.0) || dps > 1.0e7 || !src || !victim) return orig;
    int32_t per = (int32_t)(dps / MA2_DOT_HZ + 1e-9);   /* one 30 fps frame's damage, as _ftol truncates it */
    if (per <= 0) return 0;                              /* did nothing at 30 fps either */
    uint64_t x = ((uint64_t)src << 32) | victim;         /* entity addresses are aligned: mix every bit (splitmix64) */
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    unsigned h = (unsigned)(x % MA2_DOT_SLOTS), k, pick = h;
    double oldest = 1e300;
    ma2_dot_slot *s = 0;
    for (k = 0; k < 32; k++) {
        ma2_dot_slot *c = &tab[(h + k) % MA2_DOT_SLOTS];
        if (c->used && c->src == src && c->victim == victim) { s = c; break; }
        double age = c->used ? c->t : -1e300;
        if (age < oldest) { oldest = age; pick = (h + k) % MA2_DOT_SLOTS; }
    }
    if (s && s->t == now) return s->last;                /* same frame: the damage already worked out */
    if (!s || now < s->t || now - s->t > MA2_DOT_STALE) {
        if (!s) { s = &tab[pick]; s->src = src; s->victim = victim; s->used = 1; }
        s->acc = 1.0 - 1e-9;                             /* a new contact: the first frame ticks, as at 30 fps */
    }
    s->acc += dt * MA2_DOT_HZ;
    double n = floor(s->acc);
    if (n > 8.0) n = 8.0;                                /* a long stall: at most 8 ticks at once (30 fps did one frame) */
    s->acc -= floor(s->acc);
    s->t = now;
    s->last = (int32_t)n * per;
    return s->last;
}
#endif
