/* ma2_bot_surprise_c.h - surprise set C surprise tricks (SURPRISE-SPEC 7 Level, 8 Stomp, 10 Hack cover, 11 Peel-off).
 * Included by ma2_bot_brain.c after ma2_bot_surprise.h; the implementation ma2_bot_surprise_c.c is #included after the shared module.
 * All four go through sp_roll() (shared rarity gate). The shared module is not edited: this file has its own state.
 * FAIR DAMAGE: decisions, target choice, positioning and aim point only; no damage / armour / health / weapon change. */
#pragma once
#include "ma2_bot_api.h"

enum { SPC_NONE = 0, SPC_LEVEL, SPC_STOMP, SPC_HACKCOVER, SPC_PEELOFF };

typedef struct SpcBrain {
    int act;                     /* SPC_* running, 0 = none */
    double t0, until, next_look, strafe_t, aim_t;
    uint32_t ent;                /* level: the building; stomp: victim actor; hackcover: the hacked 'Mech; peeloff: the rider */
    uint32_t aux;                /* level: the target hidden behind it; hackcover: the hacking BA; peeloff: the ridden teammate */
    uint32_t aux_pl;             /* the victim player of the roll */
    Ma2Vec3 bpos, pt; int side;
    float hp0, hp_last, vhp0, dmin; double hp_t, last_hit_t;
    int hurt, won, wait_pilot, fired_seen, helper_hit, f_ally; float ally_cost;
    double no_helper_t, win_t, fire_t;
    /* level aftermath: the target we hid-hunted; counted once each */
    double after_t; uint32_t after_tgt; int after_exp, after_hit;
} SpcBrain;

struct Ma2Brain; struct Mem;
static float spc_target_mult(struct Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const struct Mem *m);   /* choose_target: x score; <= 0 = not a candidate */
static void  spc_tick(struct Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, int self, const Ma2Senses *sn);   /* triggers + bookkeeping */
static void  spc_order(struct Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, const Ma2CtrlStatus *st, Ma2Order *out);   /* overrides the order while a trick runs */
