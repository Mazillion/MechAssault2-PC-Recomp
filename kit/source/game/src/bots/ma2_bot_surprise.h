/* ma2_bot_surprise.h - "surprise" behaviours, shared module (bots push, SURPRISE-SPEC v1 section A).
 *
 * Included by ma2_bot_brain.c (types + prototypes near the top, the implementation file
 * ma2_bot_surprise.c is #included after the brain's helpers, like ma2_bot_brain_obj.c).
 * Every trick (grudge, revenge, mood, rookie mistakes, bait, pincer, power-up timing, level, stomp,
 * hack cover, peel-off) goes through ONE gate, sp_roll(), which applies the rarity model:
 *   switch -> 20 s hold-off -> per-bot cooldown -> per-victim budget (2 per 10 min) -> no same trick
 *   on the same victim back-to-back -> 45 s spacing per victim -> the random roll p[skill].
 * Master switch RECOMP_BOT_SURPRISE=0 / [Bots] Surprises=0; per trick RECOMP_BOT_SP_<NAME> / [Bots] Sp<Name>.
 * Test hooks (default off): RECOMP_BOT_SP_FORCE=<name|all> (p=1, no cooldown/budget/repeat/spacing; the
 * trigger condition still has to hold), RECOMP_BOT_SP_SKILL=0..3 (every brain gets that skill),
 * RECOMP_BOT_SP_DIAG_KILLER=1 (log every death's killer attribution).
 *
 * The other surprise sets (B, C) do not edit these tables: use sp_set_params() from their own code
 * to retune a trick, keep extra state in their own static data. FAIR DAMAGE: only decisions, aim,
 * timing and target choice; nothing here touches damage, armour, health or weapon stats. */
#pragma once
#include "ma2_bot_api.h"

enum SpTrick {
    SP_REVENGE = 0, SP_GRUDGE, SP_MOOD,
    SP_RK_A, SP_RK_B, SP_RK_C, SP_RK_D, SP_RK_E, SP_RK_F,   /* rookie: tunnel / no-flank / straight / out-of-range / VTOL chase / last shooter */
    SP_BAIT, SP_PINCER, SP_POWERUP, SP_LEVEL, SP_STOMP, SP_HACKCOVER, SP_PEELOFF,
    SP_N
};

/* Per-bot state, embedded in Ma2Brain (field `sp`). */
typedef struct SpBrain {
    uint32_t ev_cur;                       /* last death event (rseq) handled */
    int stats_ok, kills0, deaths0;         /* scoreboard baseline */
    int ks, ds;                            /* kills since the last death / deaths since the last kill */
    int was_alive;
    /* mood: 0 none, 1 hot, 2 cold */
    int mood_try;                          /* a streak was just reached: roll once (1 hot, 2 cold) */
    int mood; double mood_until, mood_t0; float m_aggr, m_ret; int m_kills0, m_deaths0;
    /* grudge: killer table (last deaths), armed grudge waiting for the next spawn, running */
    struct { uint32_t player; double t; } kd[6]; int nkd;
    uint32_t gr_player; int gr_arm; double gr_until, gr_t0; int gr_sk_armed;
    /* revenge */
    uint32_t rv_player; double rv_until, rv_t0; Ma2Vec3 rv_pos; int rv_found;
    /* rookie mistakes: 0 none, else SP_RK_A..F */
    int rk; double rk_until, rk_t0; uint32_t rk_tgt, rk_pl; float rk_hp0; int rk_died_counted;
    uint32_t last_shooter; double last_shot_t;   /* actor that hit us last (4f) */
    float hpf;                             /* health fraction at the last tick */
    /* rolls waiting on a condition that holds for a while (4a): tunnel anchor */
    double dbg_t;
    int any;                               /* a target-bias trick (grudge / revenge / rookie) is live: cheap early-out */
} SpBrain;

/* ---- the gate (brain TU; static) ---- */
struct Ma2Brain;
static int   sp_roll(struct Ma2Brain *b, int trick, uint32_t victim_player);
static int   sp_roll_p(struct Ma2Brain *b, int trick, uint32_t victim_player, float p_override);   /* p_override < 0 = table value */
static void  sp_set_params(int trick, float cool_min, float p_recruit, float p_regular, float p_veteran, float p_elite);
static int   sp_on(int trick);                                       /* master + trick switch (cheap after the first call per match) */
static int   sp_force(int trick);

/* ---- hooks the brain calls ---- */
static float sp_target_bias(struct Ma2Brain *b, uint32_t player);    /* choose_target: x score for this enemy (1 = none) */
static float sp_hunt_mult(struct Ma2Brain *b, uint32_t player);      /* hunt pick: x weight */
static float sp_mood_flank(const struct Ma2Brain *b);                /* flank_try: x chance (HOT 1.5) */

/* rookie mistakes (module section 4) */
struct Mem;
static int          sp_rk_noflank(struct Ma2Brain *b, const struct Mem *m, int why);
static struct Mem * sp_rk_target(struct Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, struct Mem *pick, struct Mem *cur);
static int          sp_rk_nopeek(const struct Ma2Brain *b);
static void         sp_rk_order(struct Ma2Brain *b, Ma2Order *out);

/* ---- exported to the core ---- */
#ifdef __cplusplus
extern "C" {
#endif
void ma2_bot_sp_match_begin(void);   /* ma2_bot_player.c: a new match's bots were created */
#ifdef __cplusplus
}
#endif
