/* MechAssault 2 offline bots - decision layer ("brain").
 *
 * NATIVE_BOTS_DESIGN.md rev 2, layers 1-4 (the controller ma2_bot_ctrl.c is layer 5):
 *
 *   Perception  sight (sight range, field of view, the game AI's own collision ray),
 *               hearing (weapons fired nearby), damage (who is shooting me), radar,
 *               team call-outs. No live positions of unseen enemies -- ever.
 *   Knowledge   per-enemy memory (last known position/velocity, confidence decaying),
 *               damage rates in both directions, team blackboard (shared sightings).
 *   Decision    utility-scored behaviours with commitment and hysteresis:
 *               GetVehicle (incl. grabbing a free vehicle that passes close), Swap
 *               (trade a dying vehicle / BattleArmor for a better free one), Attack,
 *               Cover (outnumbered: break line of sight while firing), Retreat (losing,
 *               low health: fall back toward cover, allies, armour), PowerUp, Support
 *               (help an outnumbered teammate from a flank), Hack (BattleArmor,
 *               opportunistic), DefendHack, Hunt, game-mode Objective.
 *   Tactics     position queries scored with the AI ray: firing positions (weapon band,
 *               line of sight to the target, exposure to others, spacing, height),
 *               cover and retreat points (line of sight broken to the attackers).
 *
 * Target choice is by threat removed per second of our fire (its damage rate on us /
 * our time to kill it): a nearly dead attacker is finished first; skilled bots commit,
 * green bots react to whoever hurts most. Bot Skill and a per-bot personality drive
 * every threshold.
 *
 * Pure host code: reads Ma2World and asks the game only through Ma2Senses.
 */
#include "ma2_bot_api.h"
#include "ma2_bot_c7.h"   /* C7 per-match counters */
#include "ma2_bot_surprise.h"   /* surprise behaviours: SpBrain + the gate's prototypes (module below) */
#include "ma2_bot_surprise_c.h" /* surprise set C tricks: level / stomp / hack cover / peel-off */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ma2_bot_env.h"   /* getenv, cached (the bot code asks every tick) */

#define MA2B_PI                   3.14159265358979f
#define MA2B_DEG                  (MA2B_PI / 180.0f)
#define MA2B_NKIND                9

#define MA2B_EVAL_PERIOD          0.25    /* behaviour re-evaluation (4 Hz)               */
#define MA2B_HYSTERESIS           0.15f   /* bonus kept by the current behaviour          */
#define MA2B_MIN_DWELL            1.2     /* s before a behaviour may be replaced (no interrupt) */
#define MA2B_DEFAULT_RADAR        200.0f
#define MA2B_MEM_TAU              8.0     /* knowledge confidence decay (s)               */
#define MA2B_MEM_FORGET           15.0    /* forget an enemy unknown for this long (s)    */
#define MA2B_DMG_TAU              4.0     /* damage-rate window (s)                       */
#define MA2B_QTE_NOMINAL_WINDOW   0.8f
#define MA2B_DEFENSE_COOLDOWN     10.0
#define MA2B_SHIELD_RANGE         30.0f
#define MA2B_CALLOUT_DELAY        0.4     /* team sightings reach teammates after (s)    */
#define MA2B_CALLOUT_LIFE         5.0
#define MA2B_RAY_RATE             90.0f   /* AI rays per second per bot (token bucket)    */
#define MA2B_RAY_BURST            40.0f
#define MA2B_NEAR_AWARE           25.0f   /* this close an enemy is noticed without seeing it */

/* ======================================================================
 * Small math
 * ====================================================================== */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static Ma2Vec3 v3(float x, float y, float z) { Ma2Vec3 r; r.x = x; r.y = y; r.z = z; return r; }
static Ma2Vec3 vadd(Ma2Vec3 a, Ma2Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static Ma2Vec3 vsub(Ma2Vec3 a, Ma2Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static Ma2Vec3 vscale(Ma2Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float hlen(Ma2Vec3 a) { return sqrtf(a.x * a.x + a.z * a.z); }
static float hdist(Ma2Vec3 a, Ma2Vec3 b) { return hlen(vsub(a, b)); }
static Ma2Vec3 hnorm(Ma2Vec3 a)
{
    float l = hlen(a);
    return l > 1e-6f ? v3(a.x / l, 0.0f, a.z / l) : v3(0.0f, 0.0f, 0.0f);
}
static Ma2Vec3 dir_of_heading(float h) { return v3(sinf(h), 0.0f, cosf(h)); }
static float heading_of(Ma2Vec3 d) { return atan2f(d.x, d.z); }
static float wrap_pi(float a) { while (a > MA2B_PI) a -= 2.0f * MA2B_PI; while (a < -MA2B_PI) a += 2.0f * MA2B_PI; return a; }

static uint32_t mix32(uint32_t h)
{
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}
static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}
static float rng_f(uint32_t *s) { return (float)(rng_next(s) >> 8) * (1.0f / 16777216.0f); }
static float rng_range(uint32_t *s, float a, float b) { return a + (b - a) * rng_f(s); }

/* ======================================================================
 * Body kinds and skill
 * ====================================================================== */
typedef struct KindInfo {
    float eye, radius;
    float band_min, band_max;   /* preferred fighting distance */
    float weapon_range;         /* effective reach of its guns */
    float threat;               /* fighting strength weight    */
    float dps;                  /* nominal damage rate (HP/s)  */
    float sight;                /* how far it sees (m)         */
    const char *name;
} KindInfo;

static const KindInfo k_kind[MA2B_NKIND] = {
    /* eye  rad  bmin  bmax  wrange threat dps  sight name */
    { 0.0f, 1.0f, 20.f,  60.f, 100.f, 0.20f,  0.f, 150.f, "none"   },
    { 1.7f, 0.6f, 10.f,  35.f,  60.f, 0.15f,  3.f, 170.f, "pilot"  },
    { 7.0f, 4.0f, 60.f, 160.f, 300.f, 1.00f, 45.f, 340.f, "mech"   },
    { 2.5f, 1.6f, 15.f,  40.f, 120.f, 0.45f, 20.f, 230.f, "ba"     },
    { 3.0f, 3.0f, 80.f, 200.f, 300.f, 0.90f, 35.f, 320.f, "tank"   },
    { 0.0f, 3.5f, 80.f, 170.f, 250.f, 0.70f, 25.f, 380.f, "vtol"   },
    { 0.0f, 3.5f, 80.f, 170.f, 250.f, 0.60f, 20.f, 380.f, "flyer"  },
    { 2.5f, 2.5f, 40.f, 120.f, 200.f, 0.50f, 20.f, 280.f, "mover"  },
    { 3.0f, 2.5f, 60.f, 160.f, 220.f, 0.40f, 20.f, 300.f, "turret" },
};

typedef struct Skill {
    float reaction;     /* s a newly seen enemy must stay seen before we engage it        */
    float los_period;   /* s between sight re-checks per enemy                            */
    float fov_deg;      /* half-angle of the field of view                                 */
    float sight_mult;
    float qte_frac;     /* fraction of the NeuroHack response window before answering      */
    float qte_miss;     /* probability of missing a cue                                    */
    float aggression;   /* 0..1                                                            */
    float cover_use;    /* chance to take cover when outnumbered (per episode)             */
    float retreat_hp;   /* health fraction below which a losing fight is left             */
    float dodge;        /* jumpjet hops per 10 s while fighting (more when hit)             */
    int   smart;        /* kill-value reasoning, focus fire, support, finishing kills      */
    /* Camo (F5, t4-sk-B2/B3; user rule: Elites slightly better than the game's AI, ~50 m, the
     * rest much worse). A cloaked enemy is seen only by a roll per sight re-check inside a range,
     * with a clear AI ray; a hit or its own shot reveals it through the game's rules. */
    float cloak_r, cloak_p;     /* cloaked (cloak 1): spotting range (m), chance per roll          */
    float strict_r, strict_p;   /* strictly cloaked (cloak 2)                                      */
    float spot_hold;            /* s a successful roll counts as sight                             */
    float cloak_grace;          /* s it stays a target (fire at the predicted spot) once it cloaks */
    float cloak_forget;         /* s before a cloaked enemy is forgotten                           */
    float cloak_reckon;         /* s of dead reckoning from its last seen motion                   */
} Skill;

/* Recruit, Regular, Veteran, Elite */
static const Skill k_skill[4] = {
    { 0.90f, 0.70f, 55.f, 0.85f, 0.70f, 0.25f, 0.45f, 0.00f, 0.12f, 0.2f, 0, 12.f, 0.15f,  0.f, 0.00f, 1.0f, 0.0f,  4.f, 1.f },
    { 0.55f, 0.50f, 65.f, 0.95f, 0.55f, 0.17f, 0.60f, 0.50f, 0.20f, 0.7f, 0, 20.f, 0.30f,  0.f, 0.00f, 1.0f, 0.0f,  7.f, 2.f },
    { 0.30f, 0.35f, 75.f, 1.05f, 0.40f, 0.10f, 0.75f, 0.85f, 0.27f, 1.2f, 1, 35.f, 0.50f, 10.f, 0.30f, 1.5f, 0.8f, 12.f, 3.f },
    { 0.12f, 0.25f, 85.f, 1.15f, 0.25f, 0.02f, 0.90f, 1.00f, 0.32f, 1.8f, 1, 55.f, 0.80f, 25.f, 0.50f, 1.5f, 1.5f, 20.f, 4.f },
};
/* Dragoon (MA2_SKILL_DRAGOON, experimental): an Elite with every knob past Elite -- reacts in 0.04 s, re-checks sight 4x a
 * second, 100 deg view, 30 % further sight, answers a NeuroHack early and never misses a cue, all-out, dodges 3 a 10 s,
 * spots cloaked enemies like a sensor sweep. */
static const Skill k_dragoon = { 0.04f, 0.15f, 100.f, 1.30f, 0.15f, 0.00f, 1.00f, 1.00f, 0.32f, 3.0f, 1, 80.f, 0.95f, 45.f, 0.80f, 2.0f, 2.5f, 30.f, 6.f };

/* ======================================================================
 * Knowledge
 * ====================================================================== */
typedef struct Mem {
    uint32_t player, actor;
    int valid, kind, nation;
    int visible;                /* in sight cone, in range, AI ray clear -- right now   */
    double vis_since;           /* start of the current continuous sighting             */
    double last_seen;           /* last time actually seen                              */
    double last_known;          /* last time its position was learnt (any sense)        */
    double los_t;               /* last sight check                                     */
    Ma2Vec3 pos, vel;           /* last known                                           */
    float pos_err;              /* metres of uncertainty in pos                         */
    float hp, hp_max;
    float wpn_lvl, reach;       /* F43: its weapon upgrades (0..1, -1 unknown) and gun reach (m), as last seen */
    float in_v;  double in_t;   /* damage it did to us (decayed sum)                    */
    float out_v; double out_t;  /* damage we did to it (decayed sum)                    */
    double hit_me_t;            /* last damage from it                                  */
    int attacking_me;
    double att_t;               /* F3: last time it was attacking us                    */
    int cloak;                  /* its camo rating now (Ma2PlayerView.cloak)             */
    double spot_until;          /* a cloak roll succeeded: seen until then               */
    double cloak_lost_t;        /* when it went from seen to cloaked                     */
    int cloaker; double expect_t; /* F83 (Veteran/Elite): it cloaks; expect it back about then */
    double look_at, look_until; int look_why, look_found; /* F65: turning to look (heard 1 / hit 2): sight outside the view from look_at */
} Mem;

typedef struct QteState {
    uint8_t last_req;
    float last_window;
    int pending, miss, win_unknown;
    double press_at;
    uint8_t mask;
    int cues, answered, missed;
} QteState;

enum Goal { G_NONE = 0, G_ROAM, G_GET_VEHICLE, G_ATTACK, G_DEFEND_HACK, G_EJECT, G_RETREAT, G_OBJECTIVE,
            G_COVER, G_POWERUP, G_SUPPORT, G_HACK, G_SWAP, G_RIDE, G_LIFT, G_REPAIR, G_COUNT };
static const char *const k_goal_name[G_COUNT] = {
    "none", "hunt", "getveh", "attack", "defend", "eject", "retreat", "objective",
    "cover", "powerup", "support", "hack", "swap", "ride", "lift", "repair"
};
/* Lifts: a BattleArmor with a long way to go asks; a teammate's VTOL close by comes down to
 * hover beside it; the BattleArmor jets up and claws on, rides (shooting), and lets go near
 * where it was going -- at once if the VTOL is going down. Requests live a few seconds. */
static struct { int nation; uint32_t ba, vtol; Ma2Vec3 pos, dest; double t, go_t, want_t; int car; } g_lift[12];   /* car: a flag carrier's ride home; want_t: RideIntent */
static struct { uint32_t vtol, ba, ba2; int phase, skill, human, rp; double t0, tp; } g_frun[8];   /* CTF flag run per nation (frun_eval); ba2: VtolTwoRiders */
enum { FR_NONE = 0, FR_PICKUP, FR_INBOUND, FR_GRAB, FR_EXTRACT, FR_COVER };   /* g_frun[].phase */
static const float k_frun_give_up[4] = { 0.5f, 0.42f, 0.35f, 0.28f };   /* flag run: the VTOL's health it gives up at, by pilot skill */
/* PlayerRunHurt: the call a badly damaged VTOL made with the player aboard (1 press on to their flag, 2 set him down at pt);
 * it stands while he hangs on (until = 1e9) and 45 s after he lets go. */
static struct { uint32_t vtol; int mode; double t, until; Ma2Vec3 pt; } g_hurt[8];
static double g_frun_pull[8];   /* PartnerPull: a solo cover run gave way to a BattleArmor partner then */
/* WP2-C GroundReach (default on, RECOMP_BOT_GROUND_REACH / [Bots] GroundReach): 1 = `to` is known to lie on a different walkable region than `me` and is not
 * below it (a way down is a drop link the region map does not show): a pilot, 'Mech or tank cannot get there on foot (ma2_path_same_ground, Worker B). Bodies with
 * jets (BattleArmor) or wings are never judged. A mech parked 19 s under a roof powerup on In Your Face (sw24, 793,805, no_path) is the case. */
static int ground_cut(const Ma2PlayerView *me, Ma2Vec3 to)
{
    int k = kind_ok(me->kind);
    if (k != MA2_KIND_PILOT && k != MA2_KIND_MECH && k != MA2_KIND_TANK) return 0;
    if (to.y < me->pos.y - 4.0f) return 0;
    if (!c7_on("groundreach", "RECOMP_BOT_GROUND_REACH", "GroundReach", 1)) return 0;
    return ma2_path_same_ground(me->pos, to, k != MA2_KIND_PILOT) == 0;
}
/* WP3-C SnatchDeck (default on, RECOMP_BOT_SNATCH_DECK / [Bots] SnatchDeck): 1 = Snatch It with a free core above `me` on another walkable region, i.e. one only a
 * BattleArmor or a VTOL can take (Rush Hour: the cores sit on a y=55 deck over a y=30 floor). All 11 grabs in the three re-sweep Rush Hour runs were made by a
 * BattleArmor, the first only at t=82 (Phase 1) / 184 (Phase 3) while 'Mechs ordered there spent 8-18 s per bot without a route (sw85: 16-21 no_path episodes). */
static int snatch_deck(const Ma2World *w, const Ma2PlayerView *me)
{
    int i;
    if (w->game_type != MA2_GT_SNATCHIT || !c7_on("snatchdeck", "RECOMP_BOT_SNATCH_DECK", "SnatchDeck", 1)) return 0;
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        if (o->kind != MA2_OBJ_CORE || o->state == MA2_OBJS_CARRIED || (o->state == MA2_OBJS_IN_BASE && o->nation == me->nation)) continue;
        if (o->pos.y > me->pos.y + 10.0f && ground_cut(me, o->pos)) return 1;
    }
    return 0;
}
static int g_ba_fail, g_ba_ok;   /* WP2-C BaEnterBan: pilots' BattleArmor boardings that got no prompt / that worked this match */
static double g_frun_want[8];   /* a VTOL of this nation wants a BattleArmor partner until then (vehicle_value) */
static struct { uint32_t who; double until; } g_bapick[8];   /* CtfBaPick: the one pilot per team the BattleArmor bonus is for */
static struct { uint32_t who; double until; } g_vtpick[8];   /* CtfVtolPick: the one pilot per team the VTOL bonus is for */
static int lifts_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_BOT_LIFTS"); v = !(e && *e == '0'); } return v; }
static int lift_find(uint32_t ba) { for (int i = 0; i < 12; i++) if (g_lift[i].ba == ba) return i; return -1; }
/* LiftClaimFix (default on, RECOMP_BOT_LIFT_CLAIM_FIX / [Bots] LiftClaimFix): a VTOL's claim on a request only counts while it
 * keeps pursuing it (go_t is refreshed by the claimant when its goal is the lift, and by order_lift's pickup leg). Before, the claim
 * lived as long as the BattleArmor re-posted, so a VTOL that got into a fight, or died, kept every other VTOL off a flag carrier. */
static int lift_claim_fix_on(void) { return c7_on("liftfix", "RECOMP_BOT_LIFT_CLAIM_FIX", "LiftClaimFix", 1); }
/* LiftBoardFix (default on, RECOMP_BOT_LIFT_BOARD_FIX / [Bots] LiftBoardFix): the ride score a BattleArmor gave a VTOL that came for it was
 * the objective's + 0.05, but the running goal (the objective) keeps +MA2B_HYSTERESIS (0.15), so a flag carrier walked on under its own
 * VTOL (run y3: VTOL 23 m away at t=104, carrier never boarded). A flag carrier now rates the ride above the hysteresis (0.15 + 0.5). */
static int lift_board_fix_on(void) { return c7_on("liftboard", "RECOMP_BOT_LIFT_BOARD_FIX", "LiftBoardFix", 1); }
static float ride_margin(const Ma2Brain *b);
static int lift_claim_live(int i, double now)
{
    if (!g_lift[i].vtol) return 0;
    if (!lift_claim_fix_on()) return 1;
    return now >= g_lift[i].go_t && now - g_lift[i].go_t < 4.0;
}
/* RideIntent (default on, RECOMP_BOT_RIDE_INTENT / [Bots] RideIntent; design 11-12, VTOL pass leftover): a bot BattleArmor's request was
 * re-posted on every evaluation its objective was far and nobody shot at it -- also while its own goal was a fight or a power-up -- so a
 * VTOL kept coming for, then hovering over, a BattleArmor that was not boarding (v29h1 VTOL bot 10 t=101.6-110.7: 258 -> 48 m, then 4 s
 * hovering 70-78 m off while BA bot 8 peeked and shot; v29h2 VTOL bot 2 t=113-119 after BA bot 4 on a power-up run). want_t is the last
 * evaluation the requester's own goal was the ride, or its objective (the lift's destination) without having given up on the VTOL
 * coming for it (lift_off); a pick-up whose bot requester has had neither for 2 s ends (the VTOL takes it again once it does).
 * The player's requests have no brain and always count. [C7] rideintent: ended (pick-ups ended), claims_dropped. */
static int ride_intent_on(void) { return c7_on("rideintent", "RECOMP_BOT_RIDE_INTENT", "RideIntent", 1); }
static int lift_intent_gone(int i, int human, double now)
{
    if (human || !ride_intent_on()) return 0;
    return now < g_lift[i].want_t || now - g_lift[i].want_t > 2.0;
}
static void lift_post(uint32_t ba, int nation, Ma2Vec3 pos, Ma2Vec3 dest, double now)
{
    int i = lift_find(ba), old = 0;
    if (i < 0) { for (int j = 0; j < 12; j++) if (g_lift[j].t < g_lift[old].t || now < g_lift[j].t) old = j; i = old; g_lift[i].vtol = 0; g_lift[i].go_t = 0.0; g_lift[i].want_t = -1e9; }
    g_lift[i].ba = ba; g_lift[i].nation = nation; g_lift[i].pos = pos; g_lift[i].dest = dest; g_lift[i].t = now; g_lift[i].car = 0;
}
/* VTOL carrier lifts (VTOL-CARRIER-SPEC v1). LiftCarrier: a teammate carrying their flag in a BattleArmor
 * is fetched more readily by a better VTOL pilot (decisions only); LiftHuman: the player as that carrier posts a
 * request too (only bot brains post them). [C7] lift counts posted / taken / boarded / home, bot vs player. */
static int lift_carrier_on(void) { return c7_on("liftcarrier", "RECOMP_BOT_LIFT_CARRIER", "LiftCarrier", 1); }
static int lift_human_on(void) { return c7_on("lifthuman", "RECOMP_BOT_LIFT_HUMAN", "LiftHuman", 1); }
static int ctf_ba_pick_on(void) { return c7_on("bapick", "RECOMP_BOT_CTF_BA_PICK", "CtfBaPick", 1); }
/* VtolMuster (default on, RECOMP_BOT_VTOL_MUSTER / [Bots] VtolMuster; design note 10-31): a VTOL with no rider holds
 * 8-11 s for a teammate -- bot or player -- walking to a BattleArmor near it or standing beside it in one, coming
 * down by it so it can claw on, instead of tearing off at the start of a match. g_muster tells the BattleArmor side.
 * BaBoardPlayer (default on, RECOMP_BOT_BA_BOARD_PLAYER / [Bots] BaBoardPlayer): a bot BattleArmor climbs onto the
 * player's VTOL hovering low over it, and lets go where the player sets down, at their flag, or when it is going down. */
static struct { uint32_t vtol, pl; double t; } g_muster[8];
static int vtol_muster_on(void) { return c7_on("muster", "RECOMP_BOT_VTOL_MUSTER", "VtolMuster", 1); }
static int ba_board_player_on(void) { return c7_on("boardpl", "RECOMP_BOT_BA_BOARD_PLAYER", "BaBoardPlayer", 1); }
/* VtolTwoRiders (default on, RECOMP_BOT_VTOL_TWO_RIDERS / [Bots] VtolTwoRiders; playtest 11-01: one run must not override the other: pick up
 * two BattleArmor riders when one is the player and the other a bot):
 * the team VTOL started a flag run with a bot BattleArmor and flew it off while the player sat in a BattleArmor beside it
 * (Deliverance 11-01, all 3 matches) -- VtolMuster stood down once anyone was aboard or any run was on. Now the hold runs
 * while fewer than two are aboard (the player first, never with their flag carrier aboard), also on our own flag run at
 * pickup / inbound 400 m or more from their flag (MusterWhere still keeps it off their base) and over a lift request
 * from a BattleArmor within 50 m; with one aboard it hovers 1.5 m higher (the rider hangs 7-8 m under the skids), with
 * the player aboard it fetches the run's BattleArmor within 150 m, and the second rider joins the run (g_frun.ba2: it
 * drops at their flag and goes for it, and whichever of the two takes the flag is flown home). [C7] tworiders. */
static int vtol_two_riders_on(void) { return c7_on("tworiders", "RECOMP_BOT_VTOL_TWO_RIDERS", "VtolTwoRiders", 1); }
/* TwoWaitCap (default on, RECOMP_BOT_TWO_WAIT_CAP / [Bots] TwoWaitCap; playtest 11-01, no open-ended waits: the Deliverance
 * player ferry stalled at the pickup, Worker B wbdel1): with the player aboard the VTOL held for the run's bot BattleArmor,
 * which chased it jetting for 45 s without once getting under it (the craft, flying with a rider, overshot the moving BA at
 * 20-50 m/s); the hold's 8-11 s limit restarted every time the BA hopped out of 50 m and back, the run then ended "pickup too
 * long" and was rolled again at once with the same BA, the player still hanging (twice, 100 s lost). Now (a) a hold that lapses
 * for under 4 s and comes back keeps its clock, (b) holding or fetching a second rider with one aboard lasts 12 s at most,
 * then that run ends ("did not make it aboard"), the hold is given up for 45 s and the one aboard is flown (the player's
 * VtolWaitPlayer run), no second hold for 30 s, and (c) no new run is rolled while someone hangs on. [C7] twowaitcap
 * capped / resumed / no_run_rider. */
static int two_wait_cap_on(void) { return c7_on("twowaitcap", "RECOMP_BOT_TWO_WAIT_CAP", "TwoWaitCap", 1); }
static int capture_reset_on(void) { return c7_on("capreset", "RECOMP_BOT_CAPTURE_RESET", "CaptureReset", 1); }   /* CaptureReset: see order_lift */
/* BaHackClose (default on, RECOMP_BOT_BA_HACK_CLOSE / [Bots] BaHackClose; playtest 11-01: hack more often at very close
 * range, so a player who comes that close is punished): the NeuroHack pick was a bare utility
 * race (1.0 under 45 m) that an Elite already shooting never lost (attack ~0.87 + 0.15 hysteresis), even point-blank. An
 * engageable enemy 'Mech within 25 m is now rolled for once per encounter (kept 5 s at least; a new roll only after it was
 * out of 25 m for 5 s, or for another 'Mech) at Recruit 45% / Regular 60% / Veteran 75% / Elite 85%, +15 points (cap 95%)
 * against the player's 'Mech; a win makes the hack worth 1.4 (over the fight + hysteresis) and F19's commit sees it
 * through. Not under 0.35 health, nor on a 'Mech hacked in the last 30 s. Decisions only. [C7] bahackclose. */
static int ba_hack_close_on(void) { return c7_on("bahackclose", "RECOMP_BOT_BA_HACK_CLOSE", "BaHackClose", 1); }
static const char *const k_c7_hkc_roll[4] = { "rolls_recruit", "rolls_regular", "rolls_veteran", "rolls_elite" };
static const char *const k_c7_hkc_win[4] = { "wins_recruit", "wins_regular", "wins_veteran", "wins_elite" };
static const char *const k_c7_lift_taken[4] = { "carrier_taken_recruit", "carrier_taken_regular", "carrier_taken_veteran", "carrier_taken_elite" };
static const char *const k_c7_bapick_hit[4] = { "rule_hits_recruit", "rule_hits_regular", "rule_hits_veteran", "rule_hits_elite" };
static const char *const k_c7_bapick_took[4] = { "took_ba_recruit", "took_ba_regular", "took_ba_veteran", "took_ba_elite" };
static void lift_post_carrier(uint32_t ba, int nation, Ma2Vec3 pos, Ma2Vec3 dest, double now, int human)
{
    int i = lift_find(ba);
    if (i < 0 || now - g_lift[i].t > 3.0 || now < g_lift[i].t) c7_add("lift", human ? "carrier_posted_human" : "carrier_posted_bot", 1);
    lift_post(ba, nation, pos, dest, now);
    i = lift_find(ba); if (i >= 0) g_lift[i].car = 1;
}

struct Ma2Brain {
    int index, skill;
    uint32_t rng;
    Skill sk;
    float aggr, caution;        /* personality */
    float marksman;             /* 0..1: likes to fight from range ('Mechs)  */
    float flank;                /* lane: -1 left, 0 centre, +1 right of the way to its destination */
    float band_min, band_max; int standoff;   /* this tick's fighting distance */
    double now; float dt;

    int prev_valid; uint32_t prev_actor; int prev_kind;
    double act_since;

    float hp_prev, dmg_recent; double last_hit;
    int vt_carry;               /* VTOL: a power-up hangs from our tether */
    uint32_t lift_ba, lift_vtol; double ride_since;   /* lifts: the BA we fetch / the VTOL we ride */
    double lift_since, lift_off_until; uint32_t lift_off;   /* a lift that is not working out: given up for a while */
    uint32_t lift_rider; int lift_rider_carrier, lift_rider_human;   /* [C7] lift: who hangs on us now (boarded / home) */
    int bapick_took;            /* CtfBaPick: counted our BattleArmor */
    double fetch_since;         /* VTOL tether fetch under way since (its own clock: lift requests reset lift_since) */
    uint32_t fetch_pk; double fetch_near_t, fetch_off_until;   /* F18: the pickup being fetched, over it since, no fetching until */
    uint32_t muster_pl, muster_ba, muster_off; double muster_t0, muster_off_until, muster_lim; Ma2Vec3 muster_at;   /* VtolMuster: held for whom, since; given up on */
    int muster_human, two_nr;   /* VtolTwoRiders: the hold is for the player; riders aboard last tick */
    uint32_t muster_last; double muster_last_t;   /* TwoWaitCap: the hold that just lapsed (out of 50 m a moment) and when */
    double two_wait_t0, two_wait_last, two_off_until;   /* TwoWaitCap: holding for a second rider since / last tick; no second hold until */
    /* BlastWary: the nearly dead 'Mech kept clear of, first seen dying, noticed (roll), episode start, side to slide; ignored ones */
    uint32_t bw_actor; double bw_seen, bw_t0, bw_dying_t; int bw_go, bw_side; float bw_d0; Ma2Vec3 bw_pos;
    uint32_t bw_roll_actor; double bw_roll_t; int bw_roll_go;   /* BlastWary: the notice roll, once per dying 'Mech */
    double bw_entry_log, bw_elig_t;   /* BlastWary: last [BOT-WARY] 'waits for the blast' line; last tick on a duty it applies to */
    uint32_t capr_rider; double capr_t; int ride_carry;   /* CaptureReset: rider whose carrier request was dropped, when; riding with their flag last tick */
    uint32_t bw_ign[4]; double bw_ign_until[4];
    uint32_t hkc_actor[MA2_WORLD_MAX_PLAYERS]; double hkc_in_t[MA2_WORLD_MAX_PLAYERS], hkc_roll_t[MA2_WORLD_MAX_PLAYERS]; unsigned char hkc_win[MA2_WORLD_MAX_PLAYERS];   /* BaHackClose: per memory slot */
    uint32_t hboard_vtol; double hboard_t; Ma2Vec3 ride_from;   /* BaBoardPlayer: the player's VTOL low over us since; where we got on */
    double pdrop_t;             /* DropGrab: the player set us down by their flag at */
    double pw_since;            /* PartnerWait: waiting for a partner since */
    double lift_drop_t;         /* VTOL with a rider that asked for no place (the player): hovering at the drop since */
    double frun_next;           /* CTF flag run (g_frun): this VTOL's next roll for one */
    int frun_cover; Ma2Vec3 frun_orbit; double frun_orbit_t;   /* flag run, grab phase: the VTOL circles their flag */
    double frun_near_t;         /* flag run, riding in: within 30 m of their flag since */
    uint32_t proj_seen4[16]; unsigned proj_seen_i;   /* the last incoming projectiles decided about */
    int vt_low; double vt_low_until;   /* VTOL: nap-of-the-earth on this leg */
    /* VtolThreatAlt: nap-of-the-earth while shot at / enemies that can hit us are near and in sight. */
    int vt_thr_low, vt_thr_ign; double vt_thr_t0, vt_thr_last, vt_thr_hold, vt_air_t; float vt_thr_delay; int vt_deaths_seen;
    /* Alpha Strike: our own charge (on, since, at whom, next look); cooling off after it; the
     * answer to someone else's (cover until / punish whom, until). */
    int alpha_on; double alpha_t0, alpha_next, alpha_cool_until, alpha_fired_t; uint32_t alpha_tgt;
    /* VTOL repair runs: where this bot started (its base), the run's kind (1 pad, 2 pack) and spot */
    Ma2Vec3 home_pos; int home_set; int repair_kind; Ma2Vec3 repair_pt; uint32_t repair_pack; double repair_since; int repair_swap_rolled;
    int repair_pad; double pad_bad_until[8], repair_block_until;   /* F9: the pad of this run; pads that did not heal us; no repair until */
    Ma2Vec3 swap_lp; uint32_t swap_lp_veh;   /* a gunship changing ride: where it sets down, for which ride */
    double alpha_react_until; uint32_t alpha_seen, alpha_punish; double alpha_punish_until;
    unsigned st_alpha, st_alpha_abort, st_alpha_react;
    int pu_typed;               /* the world tells armour drops from weapon upgrades (some pickup armor > 0 seen) */
    SpBrain sp;                 /* surprise behaviours (ma2_bot_surprise.c) */
    SpcBrain spc;               /* surprise set C tricks (ma2_bot_surprise_c.c) */
    float pu_d;                 /* F6: distance to the power-up evaluate() picked this tick (1e9 = none) */
    uint32_t pu_keep;           /* F26: the power-up we are walking to (an enemy nearer does not drop it at once) */
    uint32_t pu_wref_actor;     /* PuRefusedKind: the body the game refused a weapon upgrade to -- no weapon upgrades for it this life */
    Ma2Vec3 pu_keep_pos; int pu_keep_val;   /* PickupTouch: where it stood; we could use it */
    struct { uint32_t ent; double until; } pu_bad[4]; int pu_bad_next; double pu_np_t0;   /* PuGiveUp: power-ups the planner has no route to for this bot (skipped until), the no-route clock on the one it walks to */
    double pu_on_t; uint32_t pu_on_ent;           /* PuRefused: when we first came within 3 m of the power-up we are fetching (which one) */
    int lms_deaths, lms_out;                      /* LmsNoRide: deaths seen; a pilot respawned after a death in a one-life mode (no ride for it) */
    Ma2Vec3 it_last_pos; double it_last_t;        /* ItGap: where It was last seen, when */
    uint32_t pu_deny;           /* F66: the power-up nearest_powerup() picked to keep from a hurt enemy (0 = none) */

    /* knowledge */
    Mem mem[MA2_WORLD_MAX_PLAYERS];
    /* F27: the last thing learnt of each enemy (any sense), kept after the memory above forgets it */
    struct { uint32_t player, actor; Ma2Vec3 pos; double t; } kn[MA2_WORLD_MAX_PLAYERS];
    float rays; double rays_t;  /* AI-ray token bucket */
    unsigned rays_used;

    /* situation (refreshed each tick) */
    Mem *tgt; uint32_t target;          /* chosen enemy (actor)        */
    double tgt_eval, tgt_since; int tgt_switches, tgt_finish;
    uint32_t tgt_prev; double tgt_prev_t;  /* F3: the target before this one, and when we left it */
    uint32_t f84_fin;                      /* F84: target whose finish hold was last logged       */
    int f42_short; uint32_t f42_actor;     /* F42: levels missing toward Alpha at the last tick, on this body */
    int n_attackers; float in_dps, my_dps, force; int outnumbered, losing;
    Ma2Vec3 threat_c;                    /* centroid of the attackers   */

    /* behaviour */
    int goal; float goal_score; double goal_since, next_eval;
    int interrupt;

    /* tactics */
    Ma2Vec3 tac_pt; int tac_kind; double tac_until; uint32_t tac_tgt; double tac_fail_until; double ba_high_next;
    /* peek and hide: a shooting spot and a hiding spot close by, alternated */
    uint32_t peek_tgt; Ma2Vec3 peek_shoot, peek_hide; int peek_phase; double peek_flip, peek_end, peek_next;
    int peek_slow; double peek_phase_t; double last_fire_t;   /* F79: hide timed to the slow weapon's refire */
    /* F79 hide search spread over frames: its target (0 = idle), next candidate, the start pose, best so far */
    uint32_t ps_tgt; int ps_i; Ma2Vec3 ps_org, ps_hide, ps_shoot; float ps_away, ps_reach, ps_best; double ps_t0;
    int ps_corner, ps_tall, ps_deep;
    int raised; double reperch_until;               /* standing on a roof / ledge; knocked off it */
    Ma2Vec3 perch; float perch_h;                    /* where we last stood up high, and how high */
    /* [C7] high ground (bots batch 5 step 1, counters only): body we track, where we stand (0 ground, 1 roof, 2 hill),
     * last time up high, hill sample, peek watch, a perched death waiting for its killer */
    uint32_t hg_actor; int hg_kind, hg_place, hg_hill, hg_pp, hg_dead_place, hg_dead_kind; float hg_hpf, hg_up_y, hg_dead_y;
    double hg_up_t, hg_hill_t, hg_pend, hg_dead_t; int hg_fb_kind, hg_fb_human; float hg_fb_y;
    float hg_sy; double hg_st; int hg_probe_miss;               /* hg_perch fallback: steady height, since, up without the roof probe */
    Ma2Vec3 hg_hold; double hg_hold_t;                /* (0a) the spot held up high, last tick held */
    double hgm_next;                                  /* (c) next 'Mech high-ground look */
    int hgc_on; Ma2Vec3 hgc_pt; double hgc_until, hgc_next, hgc_top_t, hgc_t0;   /* (a) BA wall climb: running, the roof top, give up at, next look */
    double hgd_next, hgd_t; uint32_t hgd_att; int hgd_checked;   /* (d) next drop allowed, last drop, its main attacker, hidden check done */
    int cover_roll; double cover_roll_t; double los_lost_t;
    double dodge_next;

    /* hunting */
    Ma2Vec3 roam_pt; int roam_valid; double roam_until;
    Ma2Vec3 via, via_dest; int via_state; double lane_t;   /* lane routing: 0 new trip, 1 to via, 2 direct */
    int ck_id; double ck_wait_t, ck_follow_t, ck_relane_until;   /* choke flow (C5): which choke, waiting / filing in since */
    int lane_j, lane_dir, lane_end, lane_paused, lane_prog_i, lane_skip; double lane_keep_until, lane_prog_t; float lane_prog_d;      /* F45: the lane this trip follows, which way, to which point; a choke re-lane holds */
    float lane_join_d;                                         /* F45: nearest this trip has been to its lane (joining from mid-map) */
    double zone_t;                                             /* F45 [C7] lane share / fight zone sampling */
    int flank_on, flank_side; uint32_t flank_tgt, flank_tgt0; double flank_next, flank_t0;   /* flank route */
    Ma2Vec3 flank_tpos;         /* where the enemy we set out against (flank_tgt0) is (batch 4 FlankKeep: a switch to one near it keeps the trip) */
    /* kills: go for what they drop */
    uint32_t kill_watch; Ma2Vec3 kill_watch_pos; double kill_t; Ma2Vec3 kill_pos; int kills;

    /* vehicles */
    uint32_t veh; int veh_fail; double veh_repick;
    double foot_since, foot_until; int fav_kind;
    /* WP2-C FootProgress: a pilot walking for a ride that makes no headway (anchor position / time, last time it was moving) */
    Ma2Vec3 fp_pos; double fp_t0, fp_move_t; int fp_strikes; double fp_strike_t, fp_unreach_t, ba_ban_until;
    /* WP3-C BaNear: a pilot's wait at a BattleArmor's entry (vehicle, first time within 14 m, closest approach to the entry) */
    uint32_t bn_veh; double bn_t0; float bn_dmin;
    double bail_until;          /* climb out of a trapped vehicle */
    int bail_trap;              /* the bail came from a trap: keep at it until out (F10) */
    uint32_t wedge_actor; int wedge_n;   /* WedgeFight: trap bail-outs tried from this hull */
    uint32_t no_board[16]; double no_board_until[16]; int n_no_board;
    uint32_t claim_actor; double post_hack_until;
    uint32_t swap_veh; double swap_until;
    double swap_cool_until;     /* F1: no new swap before then (one just done or given up) */
    double swp_t0, swp_last; Ma2Vec3 swp_pos;   /* push-x SwapUnstick: since when the swap has not moved us 4 m; last call */
    double swh_t; float swh_best;                 /* SwapHeadway: since when the swap has not got 2 m closer (VTOL: height over the ride counts), the best so far */
    uint32_t f37_veh; int f37_go;   /* F37: the vehicle the bail-out roll was made for, and its result */
    float c3_lost_d; double c3_lost_until;   /* C3: a ride lost to a sooner teammate, how far it was; a new one only a little further */
    int veh_wide;               /* F14: a pilot facing only pilots looks for a ride up to 600 m */
    int bomb_out; Ma2Vec3 bomb_at;   /* our pilot bomb is planted there (ctrl status, this tick) */
    uint32_t bspot_actor; double bspot_t0; int bspot_done;   /* Check It / Snatch It bomb spot: one try per life on foot, 5 s at most */
    double bomb_way_t;               /* last detour to a guard bomb spot on the way to a ride */
    Ma2Vec3 guard_pt; double guard_t;  /* on foot, the objective said guard here (then dropped it for a ride) */
    int c3_foot_counted; uint32_t c3_elite_actor, c3_grinder_veh;   /* [C7] c3: counted once */
    uint32_t left_veh; double left_until;   /* the vehicle just left to trade up: not boarded again for a while */

    /* support / hack targets */
    uint32_t support_tgt; Ma2Vec3 support_from; uint32_t hack_tgt;

    /* defence / eject / hack */
    double next_def, next_jump, next_eject, defend_repick; int shield_armed, shield_go; double shield_at;
    double eject_since, eject_move_until; Ma2Vec3 eject_move_pt;   /* an exit the game refuses: move off and try again */
    Ma2Vec3 defend_pt;
    QteState qte;
    int st_shield, st_eject;
    uint32_t hacked_actor; double hacked_t;

    /* game-mode objectives (ma2_bot_brain_obj.c) */
    int obj_role, obj_rank, obj_op, obj_lock, obj_need_foot, obj_ejected; float obj_zeal, obj_guard_r, obj_stop;
    double obj_walk_until;      /* FootFallback: get out and walk to an on-foot terminal the vehicle has no route to */
    uint32_t obj_walk_ent; int obj_walk_n;   /* FootFallback tries for that objective from a vehicle */
    float obj_walk_best; double obj_walk_best_t; Ma2Vec3 obj_walk_anchor;   /* WalkProgress: where the walk last stood 5 m off, and when */
    uint32_t obj_cp_ent; float obj_cp_best; double obj_cp_t, obj_cp_seen;   /* ClaimProgress: the claim's closest approach, when bettered by 10 m, last tick on it */
    Ma2Vec3 obj_pt, obj_wpt; uint32_t obj_ent; double obj_wander_until;
    struct { uint32_t ent; Ma2Vec3 pt; int op; double until; } obj_bad[4];
    uint32_t nowalk_actor, nowalk_ent;   /* FootBodySkip: the body that got FootPrecheck's "no walk" for this on-foot terminal -- skipped while in it */   /* ft-bots ObjGiveUp: objective targets this bot found unreachable (skipped for a while) */
    int obj_nbad_next; double obj_np_t0;
    /* W3 (push): CTF capture / grab commit zones -- carrier within 25 m of its stand, bot within 20 m of a free enemy flag */
    int capz_in, grbz_in, capz_have; double capz_t0, grbz_t0, zone_last; float capz_ang, capz_prev, zone_dt;
    /* F78: scoring carriers (flag / core / It) this bot has or has not noticed yet (ma2_bot_brain_obj.c) */
    struct { uint32_t carrier; double since, notice_at, last, pos_t; Ma2Vec3 pos; int noticed; } f78[4];
    double ctf_stage_t, ctf_go_until;   /* F46 CTF staging (ma2_bot_brain_obj.c) */
    double frun_ready_t;                /* FrunReady: last tick waiting by their flag */
    double vret_t0, vret_off_until;     /* VtolFlagReturn: over our dropped flag since; not again until (ma2_bot_brain_obj.c) */
    double cd_chase_t;                  /* W5: last tick this bot's OP_HUNT was a chase of the enemy flag carrier (ma2_bot_brain_obj.c) */

    char note[32];
};
/* ReachSet (see ma2_bot_path.c): 1 = the last finished search of this bot says `to` is outside everything it can reach (walking, drops, jet legs, its own radius). Walkers only. */
static int reach_cut(const Ma2Brain *b, const Ma2PlayerView *me, Ma2Vec3 to)
{
    int k = kind_ok(me->kind);
    if (k != MA2_KIND_PILOT && k != MA2_KIND_MECH && k != MA2_KIND_TANK && k != MA2_KIND_BATTLEARMOR) return 0;
    return ma2_path_reach_has(b->index, me->pos, to, b->now) == 0;
}


/* ======================================================================
 * Helpers
 * ====================================================================== */
static int kind_ok(int k) { return k >= 0 && k < MA2B_NKIND ? k : 0; }
/* How far this body's guns reach: its longest weapon (read from the arsenal), else the kind's nominal. */
static float my_reach(const Ma2PlayerView *me)
{
    int k = me->kind >= 0 && me->kind < MA2B_NKIND ? me->kind : 0;
    return me->reach > 10.0f ? (me->reach < 600.0f ? me->reach : 600.0f) : k_kind[k].weapon_range;
}
static float hp_frac(float hp, float hpmax) { return hpmax > 0.0f ? clampf(hp / hpmax, 0.0f, 1.0f) : 1.0f; }
static int diag_on(void) { static int d = -1; if (d < 0) { const char *e = getenv("RECOMP_BOT_DIAG"); d = e && *e && *e != '0'; } return d; }
/* FootPrecheck (default on, RECOMP_BOT_FOOT_PRECHECK / [Bots] FootPrecheck): FootFallback got out to walk to a terminal a pilot cannot walk to:
 * Check It No Man's Land 634,74,1227 stands on a mesa whose west face stops pilots (rc10_73a b1/b2, bal38-40 every walk) and whose east face is
 * 39-50 deg (slope 82-117, a wall on the grid); every claim there came from a BattleArmor climbing the east face and getting out on top. Plan
 * the walk as a pilot first (radius 0.6, drop 10, the learned pilot faces): no route to within 8 m, no get-out (the normal give-up follows). */
static int foot_walk_ok(Ma2Brain *b, const Ma2PlayerView *me)
{
    static Ma2Path p;
    /* FootWalkCache: a failed pilot plan expands ~300K cells on the brain thread (No Man's Land mesa, PATHTEST slope 66) and the FootFallback
     * test asks every tick while blocked (W_73a b1: 14 checks in 0.9 s at 113.8 s). Keep the "no walk" answer per bot for 10 s while the
     * terminal and the bot (within 10 m) stay the same; a fresh match (time going back) drops it. [C7] footprecheck cached. */
    static struct { double t; Ma2Vec3 pos, obj; } nw[MA2_WORLD_MAX_PLAYERS];
    if (!c7_on("footprecheck", "RECOMP_BOT_FOOT_PRECHECK", "FootPrecheck", 1)) return 1;
    int bi = b->index >= 0 && b->index < MA2_WORLD_MAX_PLAYERS ? b->index : -1;
    if (bi >= 0 && nw[bi].t > 0.0 && b->now >= nw[bi].t && b->now - nw[bi].t < 10.0 && hdist(nw[bi].obj, b->obj_pt) < 1.0f && hdist(nw[bi].pos, me->pos) < 10.0f) {
        c7_add("footprecheck", "cached", 1);
        b->nowalk_actor = me->actor; b->nowalk_ent = b->obj_ent;
        return 0;
    }
    ma2_path_set_danger(NULL, 0); ma2_path_set_crush(0); ma2_path_set_drop(10.f); ma2_path_set_drop_pref(1.f, 0); ma2_path_set_up(0.f); ma2_path_set_pilot(1);
    /* FootSlope (default on, RECOMP_BOT_FOOT_SLOPE / [Bots] FootSlope; parity pass 11-10): the pilot faces are learned only from a failed walk,
     * so the FIRST get-out of a match walked into the No Man's Land mesa (634,74,1227: west face 43 deg, east face 39-50 deg = slp 82-117):
     * W_73a b1 got out at 554,1148 at 24.1 s, walked 40 s, then went home on foot for a new 'Mech (64-96 s); W_73b b1 / b5 and team 2's mesa
     * (1402,83,774: W_73a b5, W_73b b6) the same -- a 'Mech per side per match out of the middle fight. A human pilot does not try a 40 deg
     * rock face. The precheck shuts cells steeper than slp 66 (33 deg): PATHTEST slope 66 keeps the Killing Fields terminal 2 walk
     * 487,1191 -> 558,1262 (52 m) and shuts both mesas (gap 83 / 126); slope 55 lost the KF walk (gap 76), 78 left the mesa open. [C7] footslope. */
    if (c7_on("footslope", "RECOMP_BOT_FOOT_SLOPE", "FootSlope", 1)) ma2_path_set_slope_max(66);
    int ok = ma2_path_plan(&p, me->pos, b->obj_pt, 0.6f, NULL, 0, b->now);
    ma2_path_set_pilot(0); ma2_path_set_slope_max(0);
    int good = ok && p.ok && p.end_gap < 8.f;
    c7_add("footprecheck", good ? "walk" : "no_walk", 1);
    if (bi >= 0) { nw[bi].t = good ? 0.0 : b->now; nw[bi].pos = me->pos; nw[bi].obj = b->obj_pt; }
    if (!good) { b->nowalk_actor = me->actor; b->nowalk_ent = b->obj_ent; }
    if (!good && diag_on()) fprintf(stderr, "[BOT-OBJ] t=%.1f bot=%d no walk for a pilot to %.0f,%.0f from %.0f,%.0f (ends %.0f m short): staying in\n", b->now, b->index, b->obj_pt.x, b->obj_pt.z, me->pos.x, me->pos.z, p.n ? p.end_gap : -1.f);
    return good;
}
/* F10 trap bail (bots batch 1): a wedged 'Mech/tank keeps the eject goal until it is out, and
 * starts the move-20-m step after 3 s instead of 6 (the 5 s window used to close first).
 * env RECOMP_BOT_TRAP_BAIL=0/1 overrides pc_settings.ini [Bots] TrapBail (default 1);
 * [C7] trapbail traps= moves= out= at match end. */
static int trapbail_on(void) { return c7_on("trapbail", "RECOMP_BOT_TRAP_BAIL", "TrapBail", 1); }

static int find_player(const Ma2World *w, uint32_t player)
{
    int i;
    if (!player) return -1;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++)
        if (w->players[i].player == player) return i;
    return -1;
}
static const Ma2PlayerView *player_by_actor(const Ma2World *w, uint32_t actor)
{
    int i;
    if (!actor) return NULL;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++)
        if (w->players[i].actor == actor) return &w->players[i];
    return NULL;
}
/* ComboAlert (default on, RECOMP_BOT_COMBO_ALERT / [Bots] ComboAlert; playtest 10-31): in CTF an enemy VTOL with a BattleArmor
 * on its claw -- or that BattleArmor -- is a two-for-one that ends their flag run. It is weighted up as a target by skill
 * (choose_target), and a bot that sees it calls it out: free teammates within 450 m go to shoot it (G_SUPPORT on the call-out). */
static struct { uint32_t actor; Ma2Vec3 pos; double t; } g_combo[8];
static int combo_alert_on(void) { return c7_on("combo", "RECOMP_BOT_COMBO_ALERT", "ComboAlert", 1); }
static int combo_of(const Ma2World *w, const Ma2PlayerView *q)
{
    if (!q || !q->alive) return 0;
    if (q->riding) { const Ma2PlayerView *v = player_by_actor(w, q->riding); return v && kind_ok(v->kind) == MA2_KIND_VTOL; }
    if (kind_ok(q->kind) != MA2_KIND_VTOL) return 0;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == q->actor) return 1;
    return 0;
}
static int find_vehicle(const Ma2World *w, uint32_t actor)
{
    int i;
    if (!actor) return -1;
    for (i = 0; i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES; i++)
        if (w->vehicles[i].actor == actor) return i;
    return -1;
}
static int is_enemy(const Ma2World *w, const Ma2PlayerView *me, const Ma2PlayerView *p)
{
    (void)w;
    return p != me && p->alive && p->actor && p->kind != MA2_KIND_NONE && p->nation != me->nation;
}
static Ma2Vec3 eye_of(Ma2Vec3 p, int kind, float frac) { p.y += k_kind[kind_ok(kind)].eye * frac; return p; }

/* Decayed sums: rate = value(now) / tau. */
static float decay_v(float v, double t, double now) { return now > t ? v * (float)exp(-(now - t) / MA2B_DMG_TAU) : v; }
static float rate_of(float v, double t, double now) { return decay_v(v, t, now) / (float)MA2B_DMG_TAU; }

/* AI rays: a token bucket per bot (the ray is not free). Returns 1 clear, 0 blocked, -1 no budget.
 * Brain spike: the 33/66 ms bot frames were one sight ray (the game's raycast) stalling
 * inside game code -- the thread blocked or pre-empted (t3-brainspike 1a90fdf: 8.3 ms wall, 0.3 ms
 * CPU) -- with every other bot's rays still to come that frame. Now all bots' rays share a frame
 * budget of RAY_FRAME_MS wall time: past it, a ray waits for the next frame ("no budget", the
 * callers already handle that), so one stalled call cannot stack with the rest.
 * RECOMP_BOT_RAY_CAP=0 / [Bots] RayFrameCap=0 = off. [C7] brainspike: rays_ms / rays_max per frame,
 * deferred = rays put off to the next frame. */
#define RAY_FRAME_MS 3.0
static int raycap_on(void) { return c7_on("brainspike", "RECOMP_BOT_RAY_CAP", "RayFrameCap", 1); }
static double g_ray_now = -1.0, g_ray_ms;
static int ray(Ma2Brain *b, const Ma2Senses *sn, uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to)
{
    int cap = raycap_on();
    double t0;
    if (!sn || !sn->los) return 1;
    if (b->rays < 1.0f) return -1;
    if (cap) {
        if (b->now != g_ray_now) { g_ray_now = b->now; g_ray_ms = 0.0; }
        if (g_ray_ms > RAY_FRAME_MS) { c7_add("brainspike", "deferred", 1); return -1; }
    }
    b->rays -= 1.0f; b->rays_used++;
    t0 = c7_t0();
    {
        int r = sn->los(sn->ctx, self, target, from, to);
        if (cap) { g_ray_ms += (c7_t0() - t0); c7_ms("brainspike", "rays", t0); }
        return r < 0 ? 1 : r;
    }
}
static float clearance_at(const Ma2Senses *sn, float x, float z) { return sn && sn->clearance ? sn->clearance(sn->ctx, x, z) : 99.0f; }
static float ground_at(const Ma2Senses *sn, float x, float z, float fallback)
{
    int ok = 0; float g;
    if (!sn || !sn->ground) return fallback;
    g = sn->ground(sn->ctx, x, z, &ok);
    return ok ? g : fallback;
}

/* F83 (switch RECOMP_BOT_CLOAK_REACT / ini BotCloakReact): what a bot does when the enemy it sees
 * cloaks. Recruits and Regulars lose it at once and pick another target. Veterans and Elites fire
 * where it should be for the skill's grace (cloak_gate: at the guess, not the body), search along
 * its last motion (the dead reckoning), and remember that it cloaks: about when camo would run
 * out they watch for it (wider, likelier spotting rolls, no field-of-view limit). The game's camo
 * timer is never read. */
static int cloak_react_on(void) { return c7_on("f83", "RECOMP_BOT_CLOAK_REACT", "BotCloakReact", 1); }
static int cloak_watching(const Ma2Brain *b, const struct Mem *m)
{
    return m->cloaker && b->now >= m->expect_t - 2.0 && b->now < m->expect_t + 4.0;
}
static void cloak_react(Ma2Brain *b, struct Mem *m)
{
    static const float back_s[4] = { 0.0f, 0.0f, 12.0f, 10.0f };   /* Veteran, Elite: when it should be back */
    if (!cloak_react_on()) return;
    if (b->skill < MA2_SKILL_VETERAN) {
        m->last_known = m->last_seen = b->now - (double)b->sk.cloak_forget + 0.3;   /* gone in 0.3 s */
        if (b->tgt == m) b->next_eval = 0.0;
        c7_add("f83", "drops", 1);
    } else {
        m->cloaker = 1; m->expect_t = b->now + back_s[b->skill & 3];
        c7_add("f83", "remembers", 1);
    }
    if (diag_on()) fprintf(stderr, "[BOT-F83] t=%.1f bot=%d sk=%d tgt=%08X cloaked: %s\n", b->now, b->index, b->skill, m->actor,
                           b->skill < MA2_SKILL_VETERAN ? "dropped" : "remembered");
}

/* F65 (switch RECOMP_BOT_HEARING / ini BotHearing): hearing was a flat 180 m for any gun, and a
 * sound or a hit from outside the field of view gave sight all round at once. Now how far a shot
 * carries depends on what fires it (the body, and a slow heavy gun: mortar, gauss, crossbow...)
 * and on the listener's skill; a sound or a hit from behind starts a turn to look, and only after
 * the skill's turn time does a sight check outside the view happen (for 1.5 s). An enemy found that
 * way is reacted to by skill: Veterans and Elites re-pick at once (a hit breaks off what they do),
 * Recruits and Regulars after a startle. */
static int hearing_on(void) { return c7_on("f65", "RECOMP_BOT_HEARING", "BotHearing", 1); }
static float hear_range(const Ma2Brain *b, const Ma2PlayerView *p)
{
    /* none, pilot, 'Mech, BattleArmor, tank, VTOL, flyer, mover */
    static const float base[8] = { 120.f, 70.f, 160.f, 100.f, 170.f, 150.f, 150.f, 120.f };
    static const float sk[4] = { 0.75f, 0.9f, 1.0f, 1.15f };
    int k = kind_ok(p->kind);
    float r = base[k < 8 ? k : 0] * sk[b->skill & 3];
    if (p->slow_reload > 0.0f) r *= 1.5f;   /* a heavy gun is heard much further */
    return r;
}
static void look_start(Ma2Brain *b, struct Mem *m, int why)
{
    static const float turn_s[4] = { 0.9f, 0.6f, 0.35f, 0.2f };
    if (b->now < m->look_until + (why == 1 ? 2.0 : 0.0)) return;   /* already turning (a sound again waits a little) */
    if (sp_rk_noflank(b, m, why)) return;   /* surprise: a rookie that ignores flanks */
    m->look_at = b->now + turn_s[b->skill & 3]; m->look_until = m->look_at + 1.5; m->look_why = why; m->look_found = 0;
    m->los_t = -1e9;
    c7_add("f65", why == 2 ? "looks_hit" : "looks_heard", 1);
}
static void look_seen(Ma2Brain *b, struct Mem *m)
{
    static const float react_s[4] = { 0.8f, 0.4f, 0.0f, 0.0f };
    static const float turn_s[4] = { 0.9f, 0.6f, 0.35f, 0.2f };
    double at = b->now + react_s[b->skill & 3];
    m->look_found = 1;
    if (b->next_eval > at) b->next_eval = at;
    if (b->skill >= MA2_SKILL_VETERAN && m->look_why == 2) b->interrupt = 1;
    c7_add("f65", m->look_why == 2 ? "found_hit" : "found_heard", 1);
    c7_add("f65", "found_s_sum", (float)(b->now - m->look_at + turn_s[b->skill & 3]));
    if (diag_on()) fprintf(stderr, "[BOT-F65] t=%.1f bot=%d sk=%d tgt=%08X %s from behind: seen after %.2f s\n", b->now, b->index, b->skill,
                           m->actor, m->look_why == 2 ? "hit" : "heard", b->now - m->look_at + turn_s[b->skill & 3]);
}

static Mem *mem_get(Ma2Brain *b, uint32_t player, int alloc)
{
    int i;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++)
        if (b->mem[i].valid && b->mem[i].player == player) return &b->mem[i];
    if (!alloc) return NULL;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
        if (!b->mem[i].valid) {
            Mem *m = &b->mem[i];
            memset(m, 0, sizeof *m);
            m->valid = 1; m->player = player;
            m->last_seen = -1e9; m->last_known = -1e9; m->vis_since = 1e9; m->los_t = -1e9;
            m->hit_me_t = -1e9;
            return m;
        }
    }
    return NULL;
}
static Mem *mem_by_actor(Ma2Brain *b, uint32_t actor)
{
    int i;
    if (!actor) return NULL;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) if (b->mem[i].valid && b->mem[i].actor == actor) return &b->mem[i];
    return NULL;
}
static float mem_conf(const Ma2Brain *b, const Mem *m)
{
    if (!m || m->last_known < -1e8) return 0.0f;
    if (m->visible) return 1.0f;
    return (float)exp(-(b->now - m->last_known) / MA2B_MEM_TAU);
}
/* Where it probably is now (dead reckoning for a few seconds). */
static Ma2Vec3 mem_predict(const Ma2Brain *b, const Mem *m)
{
    float t = (float)minf((float)(b->now - m->last_known), m->cloak ? b->sk.cloak_reckon : 3.0f);
    return vadd(m->pos, vscale(v3(m->vel.x, 0.0f, m->vel.z), t > 0.0f ? t : 0.0f));
}
static int mem_engageable(const Ma2Brain *b, const Mem *m)
{
    if (m && m->valid && !m->visible && m->cloak)   /* cloaked: only the skill's grace (none for a strict cloak) */
        return m->cloak == 1 && b->now - m->last_seen < (double)b->sk.cloak_grace;
    /* F80: the target of a running peek stays the target while we hide from it (it is out of sight
     * on purpose; we know where it was). */
    if (m && m->valid && !m->visible && m->actor == b->peek_tgt && b->now < b->peek_end && b->now - m->last_seen < 8.0 &&
        c7_on("f80", "RECOMP_BOT_PEEK_HOLD", "BotPeekHold", 1)) return 1;
    return m && m->valid && (m->visible || b->now - m->last_seen < 2.5);
}
static int mem_acquired(const Ma2Brain *b, const Mem *m)
{
    return m && m->visible && b->now - m->vis_since >= (double)b->sk.reaction;
}

int ma2_brain_kind_allowed(int kind, int game_type)
{
    static int allow_ba = -1, allow_air = -1;
    (void)game_type;
    if (allow_ba < 0) {
        const char *e = getenv("RECOMP_BOT_ALLOW_BA"); allow_ba = !(e && *e == '0');
        e = getenv("RECOMP_BOT_ALLOW_VTOL"); allow_air = !(e && *e == '0');
    }
    static int vtol_only = -1;                     /* test hook: every pilot goes for a VTOL (boarding under the nose) */
    if (vtol_only < 0) vtol_only = getenv("RECOMP_TEST_VTOL_PICK") != NULL;
    if (vtol_only && kind != MA2_KIND_VTOL) return 0;
    { static int only_ba = -1; if (only_ba < 0) only_ba = getenv("RECOMP_TEST_ONLY_BA") != NULL; if (only_ba && kind != MA2_KIND_BATTLEARMOR) return 0; }   /* test hook: bots eject and fetch BattleArmor */
    switch (kind) {
    case MA2_KIND_MECH: case MA2_KIND_TANK: return 1;
    case MA2_KIND_BATTLEARMOR: return allow_ba;    /* walks (awake MechAI goto), jets now and then */
    case MA2_KIND_VTOL: return allow_air;          /* FlyerAI MoveTo + altitude (BAVT_SPEC 3)      */
    case MA2_KIND_TURRET: return getenv("RECOMP_TEST_TURRET_PICK") != NULL;   /* test hook: man the gun emplacements */
    default: return 0;
    }
}

/* ======================================================================
 * Team blackboard: sightings called out to teammates
 * ====================================================================== */
#define TB_MAX 48
static struct { int nation; uint32_t player; Ma2Vec3 pos, vel; double t; } g_tb[TB_MAX];
static double g_tb_now = -1.0;
static struct { int nation, bot, lane; double t; } g_lane[TB_MAX];   /* each bot's lane, for spreading */
static struct { int nation, bot; double until; } g_flankers[TB_MAX];  /* bots on a flank route now */
static void flank_release(const Ma2Brain *b);
static double g_fz_t[3];                                            /* F45 [C7]: last fight sample per zone (left / centre / right) */
/* Power-ups someone just collected: the entity stays in the world until it respawns, so for a
 * while nobody heads for that spot again. */
static struct { Ma2Vec3 pos; double until; int nation; } g_pu_taken[16];   /* per team (F6): what one side took, the other does not know */
/* C6: fighting spots (hide / shoot) a bot is using, so a teammate picks another corner. */
static struct { int nation; int bot; Ma2Vec3 pos; double until; } g_spot[32];
static void spot_claim(int nation, int bot, int slot, Ma2Vec3 p, double until)
{
    int i = (bot * 2 + slot) & 31;
    g_spot[i].nation = nation; g_spot[i].bot = bot; g_spot[i].pos = p; g_spot[i].until = until;
}
/* 1 if p is where a teammate is going to fight from or will be in a moment (its claimed spots, its
 * position 1.5 s ahead on its own motion). */
static int spot_taken(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 p, float r)
{
    for (int i = 0; i < 32; i++)
        if (g_spot[i].until > b->now && g_spot[i].nation == me->nation && g_spot[i].bot != b->index && hdist(g_spot[i].pos, p) < r) return 1;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (!q->alive || q->actor == me->actor || q->nation != me->nation || !q->actor) continue;
        if (hdist(vadd(q->pos, vscale(v3(q->vel.x, 0.0f, q->vel.z), 1.5f)), p) < r) return 1;
    }
    return 0;
}
/* Several fights at once (switch RECOMP_BOT_FIGHT_ZONES / [Bots] FightZones, default on):
 * where each bot is fighting or heading to fight, so a hunting bot claims a fight like C3 claims a
 * vehicle -- once a fight already has its share of the team, the next one goes after another enemy
 * instead of the whole team piling into one brawl. */
#define ZONE_R 80.0f
/* [C7] per-skill counter names for review (suffix _recruit.._elite; c7 keeps the pointer). */
static const char *const k_c7_spread_sk[4] = { "spread_hunts_recruit", "spread_hunts_regular", "spread_hunts_veteran", "spread_hunts_elite" };
static const char *const k_c7_f23_sk[4] = { "stronger_picks_recruit", "stronger_picks_regular", "stronger_picks_veteran", "stronger_picks_elite" };
static const char *const k_c7_f37c_sk[4] = { "chances_recruit", "chances_regular", "chances_veteran", "chances_elite" };
static const char *const k_c7_f37b_sk[4] = { "bail_recruit", "bail_regular", "bail_veteran", "bail_elite" };
static const char *const k_c7_f36_sk[4] = { "cover_runs_recruit", "cover_runs_regular", "cover_runs_veteran", "cover_runs_elite" };
static struct { int nation, bot; Ma2Vec3 pos; double until; } g_zone[TB_MAX];
static int zones_on(void) { return c7_on("zones", "RECOMP_BOT_FIGHT_ZONES", "FightZones", 1); }
static void zone_note(const Ma2Brain *b, const Ma2PlayerView *me, Ma2Vec3 p)
{
    int i, slot = -1, old = 0;
    for (i = 0; i < TB_MAX; i++) {
        if (g_zone[i].until > 0.0 && g_zone[i].bot == b->index) { slot = i; break; }
        if (g_zone[i].until < g_zone[old].until) old = i;
    }
    if (slot < 0) slot = old;
    g_zone[slot].nation = me->nation; g_zone[slot].bot = b->index; g_zone[slot].pos = p; g_zone[slot].until = b->now + 6.0;
}
/* Teammates (other bots) fighting at or heading for p. */
static int zone_load(const Ma2Brain *b, const Ma2PlayerView *me, Ma2Vec3 p)
{
    int i, n = 0;
    for (i = 0; i < TB_MAX; i++)
        if (g_zone[i].until > b->now && g_zone[i].bot != b->index && g_zone[i].nation == me->nation && hdist(g_zone[i].pos, p) < ZONE_R) n++;
    return n;
}
/* A fight's share of the team: a third of it, at least 2 (6 a side: 2 per fight, 3 fights). */
static int zone_cap(const Ma2Brain *b, const Ma2PlayerView *me)
{
    int i, mates = 0;
    for (i = 0; i < TB_MAX; i++)
        if (g_zone[i].until > b->now && g_zone[i].bot != b->index && g_zone[i].nation == me->nation) mates++;
    return mates + 1 >= 6 ? (mates + 1 + 2) / 3 : 2;
}
/* [C7] zones: every 5 s, how many separate fights (zones 2 x ZONE_R apart) each side has going. */
static void zone_sample(const Ma2Brain *b)
{
    static double next;
    int i, j, k, nat, nz, nb;
    Ma2Vec3 c[TB_MAX];
    if (b->now < next - 30.0) next = 0.0;   /* new match */
    if (b->now < next) return;
    next = b->now + 5.0;
    for (k = 0; k < TB_MAX; k++) {   /* each side once: at its first row */
        if (g_zone[k].until <= b->now) continue;
        nat = g_zone[k].nation;
        for (j = 0; j < k && !(g_zone[j].until > b->now && g_zone[j].nation == nat); j++) {}
        if (j < k) continue;
        nz = 0; nb = 0;
        for (i = 0; i < TB_MAX; i++) {
            if (g_zone[i].until <= b->now || g_zone[i].nation != nat) continue;
            nb++;
            for (j = 0; j < nz && hdist(c[j], g_zone[i].pos) >= 2.0f * ZONE_R; j++) {}
            if (j == nz) c[nz++] = g_zone[i].pos;
        }
        if (nb < 3) continue;
        c7_add("zones", "samples", 1);
        if (nz >= 2) c7_add("zones", "multi", 1);
        c7_max("zones", "most", (double)nz);
    }
}

/* Roofs and ledges a bot jetted at and could not stand on (sloped, fenced, out of reach): no bot
 * picks a spot near them again this match. */
static Ma2Vec3 g_bad_top[32]; static int g_nbad_top;
static int bad_top(Ma2Vec3 p)
{
    for (int i = 0; i < g_nbad_top; i++) if (hdist(g_bad_top[i], p) < 12.0f) return 1;
    return 0;
}

static void tb_post(int nation, uint32_t player, Ma2Vec3 pos, Ma2Vec3 vel, double now)
{
    int i, slot = -1, old = 0;
    for (i = 0; i < TB_MAX; i++) {
        if (g_tb[i].nation == nation && g_tb[i].player == player) { slot = i; break; }
        if (g_tb[i].t < g_tb[old].t) old = i;
    }
    if (slot < 0) slot = old;
    g_tb[slot].nation = nation; g_tb[slot].player = player; g_tb[slot].pos = pos; g_tb[slot].vel = vel; g_tb[slot].t = now;
}

/* ======================================================================
 * Perception
 * ====================================================================== */
static void perceive(Ma2Brain *b, const Ma2World *w, int self, const Ma2Senses *sn)
{
    const Ma2PlayerView *me = &w->players[self];
    int k = kind_ok(me->kind);
    float sight = maxf(k_kind[k].sight, my_reach(me)) * b->sk.sight_mult;   /* long guns come with the zoom to use them */
    /* (switch RECOMP_BOT_F28_ENGAGED / ini BotSightCapEngaged): the draw-distance cap is for
     * spotting; a target already in sight, or one shooting at us, stays seen out to the bot's own sight (A/B
     * N\bots-4c\ab: the cap alone cost NRC ~0.7 kills/min with the hide fix on). [C7] f28 engaged_keep. */
    float sight_full = sight;
    int f28b = c7_on("f28b", "RECOMP_BOT_F28_ENGAGED", "BotSightCapEngaged", 1);
    if (w->view_range > 1.0f && sight > w->view_range) { sight = w->view_range; c7_add("f28", "capped", 1); }   /* F28: no further than the level draws */
    float radar = w->radar_range > 1.0f ? w->radar_range : MA2B_DEFAULT_RADAR;
    float fov = b->sk.fov_deg * MA2B_DEG;
    float hear = 180.0f;
    int n = w->nplayers < MA2_WORLD_MAX_PLAYERS ? w->nplayers : MA2_WORLD_MAX_PLAYERS;
    int i;

    /* A new match restarts the shared blackboard. */
    if (w->time < g_tb_now) { memset(g_tb, 0, sizeof g_tb); memset(g_lane, 0, sizeof g_lane); memset(g_flankers, 0, sizeof g_flankers); memset(g_fz_t, 0, sizeof g_fz_t); memset(g_pu_taken, 0, sizeof g_pu_taken); memset(g_spot, 0, sizeof g_spot); memset(g_zone, 0, sizeof g_zone); g_nbad_top = 0; }
    g_tb_now = w->time;

    b->rays = minf(b->rays + MA2B_RAY_RATE * b->dt, MA2B_RAY_BURST);

    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++)
        if (b->mem[i].valid && find_player(w, b->mem[i].player) < 0) b->mem[i].valid = 0;

    for (i = 0; i < n; i++) {
        const Ma2PlayerView *p = &w->players[i];
        Mem *m;
        float d, ang;
        int enemy = i != self && is_enemy(w, me, p);
        if (i == self || !p->player) continue;
        m = mem_get(b, p->player, enemy);
        if (!enemy) { if (m) m->valid = 0; continue; }
        if (!m) continue;
        if (m->actor != p->actor) {               /* new body (respawned, boarded): what we knew of it is stale */
            m->actor = p->actor; m->in_v = m->out_v = 0.0f; m->visible = 0; m->vis_since = 1e9; m->wpn_lvl = 0.0f; m->reach = 0.0f;
        }
        m->kind = kind_ok(p->kind); m->nation = p->nation;
        m->attacking_me = p->attacking == me->actor;
        if (m->attacking_me) m->att_t = b->now;
        d = hdist(me->pos, p->pos);
        ang = fabsf(wrap_pi(heading_of(vsub(p->pos, me->pos)) - me->yaw));

        m->cloak = p->cloak;
        /* Camo: unless a roll spotted it a moment ago, a cloaked enemy is seen only by the skill's
         * roll, inside its range, with the AI ray clear -- no field-of-view or near-awareness pass. */
        if (p->cloak && b->now >= m->spot_until) {
            float r = p->cloak == 2 ? b->sk.strict_r : b->sk.cloak_r, pc = p->cloak == 2 ? b->sk.strict_p : b->sk.cloak_p;
            if (p->cloak_shimmer > 0.3f) r *= b->skill >= MA2_SKILL_ELITE ? 1.3f * 1.25f : 1.3f;
            if (cloak_watching(b, m)) { r *= 1.3f; pc = minf(pc * 1.5f, 0.95f); c7_add("f83", "watch_s", b->dt); }
            if (m->visible) { m->cloak_lost_t = b->now; c7_add("cloak", "lost", 1); cloak_react(b, m); }
            m->visible = 0;
            if (r > 0.0f && d <= r && b->now - m->los_t >= (double)b->sk.los_period) {
                int ry = ray(b, sn, me->actor, p->actor, eye_of(me->pos, k, 1.0f), eye_of(p->pos, p->kind, 0.6f));
                if (ry >= 0) {
                    int hit = ry && rng_f(&b->rng) < pc;
                    m->los_t = b->now;
                    c7_add("cloak", "rolls", 1);
                    if (hit) {
                        m->spot_until = b->now + b->sk.spot_hold; m->visible = 1; m->vis_since = b->now;
                        c7_add("cloak", "spotted", 1); c7_max("cloak", "spot_max_m", d);
                    }
                    if (diag_on()) fprintf(stderr, "[BOT-CLOAK] t=%.1f bot=%d sk=%d tgt=%08X d=%.0f cloak=%d shimmer=%.2f range=%.0f ray=%d -> %s\n",
                                           b->now, b->index, b->skill, p->actor, d, p->cloak, p->cloak_shimmer, r, ry, hit ? "spotted" : "missed");
                }
            }
        } else
        /* Sight: in range, in the field of view (or already tracked), and a clear AI ray. */
        if ((d <= sight || (f28b && d <= sight_full && (m->visible || m->attacking_me || b->now - m->hit_me_t < 3.0) &&
                            (c7_add("f28", "engaged_keep", 1), 1)))
            && (ang <= fov || m->visible || d < MA2B_NEAR_AWARE || cloak_watching(b, m)
                           || (hearing_on() ? b->now >= m->look_at && b->now < m->look_until : b->now - m->hit_me_t < 1.0))) {
            if (b->now - m->los_t >= (double)b->sk.los_period * (m->visible ? 1.0 : 0.8)) {
                int r = ray(b, sn, me->actor, p->actor, eye_of(me->pos, k, 1.0f), eye_of(p->pos, p->kind, 0.6f));
                if (r >= 0) {
                    m->los_t = b->now;
                    if (r && !m->visible) m->vis_since = b->now;
                    m->visible = r;
                }
            }
        } else m->visible = 0;
        if (m->visible && !m->look_found && b->now >= m->look_at && b->now < m->look_until && hearing_on()) look_seen(b, m);

        if (m->visible) {
            m->pos = p->pos; m->vel = p->vel; m->pos_err = 0.0f;
            m->hp = p->hp; m->hp_max = p->hp_max;
            m->wpn_lvl = p->wpn_lvl; m->reach = p->reach;
            m->last_seen = b->now; m->last_known = b->now;
            tb_post(me->nation, p->player, p->pos, p->vel, b->now);
        } else {
            /* Senses that give a position without a shot: footsteps/engines close by, gunfire
             * heard, the radar. Each is coarser than sight. */
            float err = -1.0f;
            if (p->cloak) ;   /* camo: no footsteps, no radar return (a firing unit is already revealed) */
            else if (d < MA2B_NEAR_AWARE) err = 3.0f;
            else if (p->firing && d < (hearing_on() ? hear_range(b, p) : hear)) {
                err = 0.08f * d;
                if (hearing_on() && ang > fov && d <= sight) look_start(b, m, 1);
            }
            else if ((c7_on("f39", "RECOMP_BOT_F39", "BotGameRadar", 1) ? sqrtf(d * d + (p->pos.y - me->pos.y) * (p->pos.y - me->pos.y)) : d) < radar)
                err = 0.15f * d;   /* F39: the game's radar test is 3-D */
            if (err >= 0.0f && (b->now - m->last_known > 0.5 || err < m->pos_err)) {
                float a = rng_range(&b->rng, -MA2B_PI, MA2B_PI), r = rng_f(&b->rng) * err;
                m->pos = vadd(p->pos, vscale(dir_of_heading(a), r)); m->vel = p->vel; m->pos_err = err;
                m->last_known = b->now;
                if (m->hp_max <= 0.0f) { m->hp = p->hp; m->hp_max = p->hp_max; }
            }
        }
    }

    /* Damage tells who is shooting at us -- and from where. */
    if (sn) {
        for (i = 0; i < sn->nin && i < MA2_SENSE_MAX_EVENTS; i++) {
            const Ma2PlayerView *src = player_by_actor(w, sn->in[i].actor);
            Mem *m = src ? mem_get(b, src->player, is_enemy(w, me, src)) : NULL;
            if (!m || !src || !is_enemy(w, me, src)) continue;
            m->in_v = decay_v(m->in_v, m->in_t, b->now) + sn->in[i].amount; m->in_t = b->now;
            m->hit_me_t = b->now;
            if (!m->visible) {
                m->pos = src->pos; m->vel = src->vel; m->pos_err = 5.0f; m->last_known = b->now; m->los_t = -1e9;
                if (hearing_on()) look_start(b, m, 2);
            }
        }
        for (i = 0; i < sn->nout && i < MA2_SENSE_MAX_EVENTS; i++) {
            Mem *m = mem_by_actor(b, sn->out[i].actor);
            if (!m) continue;
            m->out_v = decay_v(m->out_v, m->out_t, b->now) + sn->out[i].amount; m->out_t = b->now;
        }
    }

    /* Team call-outs: what teammates saw a moment ago. */
    if (w->team_mode || w->game_type == MA2_GT_GRINDER) {
        for (i = 0; i < TB_MAX; i++) {
            Mem *m; int pi;
            if (g_tb[i].nation != me->nation || !g_tb[i].player) continue;
            if (b->now - g_tb[i].t < MA2B_CALLOUT_DELAY || b->now - g_tb[i].t > MA2B_CALLOUT_LIFE) continue;
            pi = find_player(w, g_tb[i].player);
            if (pi < 0 || !is_enemy(w, me, &w->players[pi])) continue;
            if (w->players[pi].cloak && b->now - g_tb[i].t > MA2B_CALLOUT_DELAY + 1.0) continue;   /* cloaked since: stale */
            m = mem_get(b, g_tb[i].player, 1);
            if (!m || m->visible || g_tb[i].t <= m->last_known) continue;
            m->actor = w->players[pi].actor; m->kind = kind_ok(w->players[pi].kind); m->nation = w->players[pi].nation;
            m->pos = g_tb[i].pos; m->vel = g_tb[i].vel; m->pos_err = 8.0f; m->last_known = g_tb[i].t;
        }
    }

    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
        Mem *m = &b->mem[i];
        double forget = m->cloak ? (double)b->sk.cloak_forget : MA2B_MEM_FORGET;
        if (m->cloaker && b->now < m->expect_t + 4.0) continue;   /* F83: still watching for it */
        if (m->valid && !m->visible && b->now - m->last_known > forget && b->now - m->hit_me_t > forget) m->valid = 0;
    }
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {   /* F27: what we last learnt of each enemy */
        const Mem *m = &b->mem[i];
        int j, slot = -1, old = 0;
        if (!m->valid || m->last_known < -1e8) continue;
        for (j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
            if (b->kn[j].player == m->player) { slot = j; break; }
            if (b->kn[j].t < b->kn[old].t) old = j;
        }
        if (slot < 0) slot = old;
        if (b->kn[slot].player == m->player && m->last_known <= b->kn[slot].t && b->kn[slot].actor == m->actor) continue;
        b->kn[slot].player = m->player; b->kn[slot].actor = m->actor; b->kn[slot].pos = m->pos; b->kn[slot].t = m->last_known;
    }
}

/* F5: the guns follow perception. A cloaked target not seen (nor inside the skill's grace) is
 * neither aimed nor fired at: the order loses it (an engage turns into a move to where it
 * probably is), and `seen` tells the controller, which aims on the live position, to hold. */
static void cloak_gate(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *out)
{
    const Ma2PlayerView *t = out->target ? player_by_actor(w, out->target) : NULL;
    Mem *m = t ? mem_by_actor(b, out->target) : NULL;
    if (!t || !t->cloak) { out->seen = out->target != 0; return; }
    out->seen = m && m->valid && (m->visible || mem_engageable(b, m));
    if (out->seen && m && !m->visible && cloak_react_on()) { out->blind = 1; out->blind_pt = mem_predict(b, m); }   /* F83: the grace fires at a guess */
    if (out->seen) return;
    c7_add("cloak", "orders_held", 1);
    if (out->type == MA2_ORD_ENGAGE || out->type == MA2_ORD_HACK) {
        out->type = MA2_ORD_TRAVEL; out->stop = 8.0f;
        out->point = m && m->valid ? mem_predict(b, m) : me->pos;
    }
    out->target = 0; out->fire = 0; out->standoff = 0;
}

/* ======================================================================
 * Situation: threat, kill value, force ratio
 * ====================================================================== */
static float range_acc(float d) { return d < 150.f ? 1.f : d < 300.f ? 0.6f : 0.25f; }
static int f43_on(void);
static float lvl_mult(float lvl, float weight);
static float my_effective_dps(const Ma2PlayerView *me, float d)
{
    float base = k_kind[kind_ok(me->kind)].dps * range_acc(d) * (f43_on() ? lvl_mult(me->wpn_lvl, 1.0f) : 1.0f);
    return base * hp_frac(me->hp, me->hp_max) * 0.5f + base * 0.5f;
}
/* F3 (target flapping): an enemy's guns swing on and off us every few frames, and its threat swung
 * x3.3 with them; 771 enemy-to-enemy swaps in one match, 354 under 2 s. Now "attacking me" holds
 * 2.5 s after the last time it was true, and a fresh target is kept for a skill dwell (below).
 * RECOMP_BOT_F3_HOLD=0 / ini [Bots] BotTargetHold=0 = the old behaviour. [C7] F3 counters. */
static int f3_on(void) { return c7_on("F3", "RECOMP_BOT_F3_HOLD", "BotTargetHold", 1); }
/* F84 focus fire by skill (bots batch 2; env RECOMP_BOT_F84 / ini [Bots] BotFocusFire, default 1).
 * Before: score switches were ~half of all target changes at every skill, mostly A-B-A ping-pong
 * between far targets with small, jittery scores, and 'finish' could not show (it keeps the target,
 * [THREAT] printed only changes; its 'we are hurting it' test, rate_of(out_v) > 0, was true forever
 * after the first hit). Now: a Recruit has tunnel vision (keeps its target unless another is twice
 * as good, never finishes on purpose); Regular+ stay on a wounded target they hit in the last 3 s
 * (25 / 40 / 50 % health by skill), Veteran+ snap straight to a wounded target in reach and don't
 * flip back to the one they just left (4 / 6 s) unless it is clearly better.
 * [C7] F84 finish_/snap_/aba_ per skill; F84sw new_/lost_/score_ per skill (cause of each switch). */
static int f84_on(void) { return c7_on("F84", "RECOMP_BOT_F84", "BotFocusFire", 1); }
static const char *const k_f84_finish[4] = { "finish_s0", "finish_s1", "finish_s2", "finish_s3" };
static const char *const k_f84_snap[4]   = { "snap_s0", "snap_s1", "snap_s2", "snap_s3" };
static const char *const k_f84_aba[4]    = { "aba_s0", "aba_s1", "aba_s2", "aba_s3" };
static const char *const k_f84_sw[3][4]  = { { "new_s0", "new_s1", "new_s2", "new_s3" },
                                             { "lost_s0", "lost_s1", "lost_s2", "lost_s3" },
                                             { "score_s0", "score_s1", "score_s2", "score_s3" } };
static int f7_on(void) { return c7_on("F7elite", "RECOMP_BOT_F7_ELITE", "BotEliteFight", 1); }
/* F27 (bots batch 2): the hunt goes for enemies this bot has learnt of (seen, heard, radar, hit by,
 * call-outs) at the place it learnt it, not for the live position of anyone on the map (it felt like a
 * wallhack, worst for the scoreboard leader). RECOMP_BOT_F27=0 / ini [Bots] BotHuntKnown=0 = the old
 * live-position hunt. [C7] f27 hunts known stale none. */
static int f27_on(void) { return c7_on("f27", "RECOMP_BOT_F27", "BotHuntKnown", 1); }
/* F78 (bots batch 2): a scoring carrier (enemy with our flag / a core, Not It's "It") is noticed after
 * a skill delay unless seen or close, hunted where the bot believes he is, and in sight draws the guns
 * by skill (ma2_bot_brain_obj.c f78_where, choose_target). RECOMP_BOT_F78=0 / ini [Bots]
 * BotCarrierNotice=0 = the old live-position hunt. [C7] f78 counters. */
static int f78_on(void) { return c7_on("f78", "RECOMP_BOT_F78", "BotCarrierNotice", 1); }
/* F43 threat from the real arsenal (bots batch 2; env RECOMP_BOT_F43 / ini [Bots] BotRealThreat,
 * default 1). Threat used fixed numbers per kind: a 'Mech with every class at its top level counted
 * the same as a fresh one, and range falloff ignored its guns' reach. Now an enemy's damage is
 * scaled by the upgrade levels it showed when last seen (x1 fresh .. x1.8 all at the top), weighed
 * by skill (a Recruit doesn't read the glow: 0 / 0.5 / 1 / 1), and an enemy out of its own guns'
 * reach counts 0.15 of its damage. Own strength uses our own levels at every skill.
 * [C7] F43 upgraded_seen (assess ticks with a seen enemy at lvl >= 0.5), lvl_max. */
static int f43_on(void) { return c7_on("F43", "RECOMP_BOT_F43", "BotRealThreat", 1); }
static float lvl_mult(float lvl, float weight) { return 1.0f + 0.8f * clampf(lvl, 0.0f, 1.0f) * weight; }
static float mem_lvl_mult(const Ma2Brain *b, const Mem *m)
{
    static const float w[4] = { 0.0f, 0.5f, 1.0f, 1.0f };
    return f43_on() ? lvl_mult(m->wpn_lvl, w[b->skill & 3]) : 1.0f;
}
static float mem_pot_dps(const Ma2Brain *b, const Mem *m, float d)
{
    int att = m->attacking_me || (f3_on() && m->att_t > 0.0 && b->now - m->att_t < 2.5);
    float r = range_acc(d);
    if (f43_on() && m->reach > 10.0f && d > 1.1f * m->reach) r *= 0.15f;
    return k_kind[m->kind].dps * r * (att ? 1.0f : 0.3f) * mem_lvl_mult(b, m);
}
/* The one who scores: "It" (only It scores in Not It) and an enemy carrying a flag. The better the
 * player, the more readily it drops what it is shooting at for them. */
static float obj_bonus(const Ma2World *w, const Ma2PlayerView *me, uint32_t player, int skill)
{
    static const float it_b[4] = { 1.25f, 1.55f, 1.85f, 2.2f }, carrier_b[4] = { 1.2f, 1.45f, 1.75f, 2.05f };
    int i;
    if (w->it_player && player == w->it_player) return it_b[skill & 3];
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        if (o->carrier && o->carrier == player && o->carrier_nation != me->nation) return carrier_b[skill & 3];
    }
    return 1.0f;
}

/* F44: one life each (Last Man Standing, Team LMS): a death is final, so bots break off sooner,
 * take lives where they are cheap (a badly hurt enemy) and close in once few are left. The skill
 * sets how much of this a bot thinks of. Switch RECOMP_BOT_LMS / [Bots] LmsLives. */
static int lms_on(const Ma2World *w)
{
    return (w->game_type == MA2_GT_LMS || w->game_type == MA2_GT_TEAM_LMS) && c7_on("lms", "RECOMP_BOT_LMS", "LmsLives", 1);
}
/* Not It: only It scores, a point per kill -- so It goes for the kills it can make quickly
 * (the same pick as F44's). Switch RECOMP_BOT_NOTIT_KILLS / [Bots] NotItKills. */
static int notit_it_on(const Ma2World *w, const Ma2PlayerView *me)
{
    return w->game_type == MA2_GT_NOTIT && w->it_player && w->it_player == me->player &&
           c7_on("notit", "RECOMP_BOT_NOTIT_KILLS", "NotItKills", 1);
}
/* Opponents still alive (the other teams; everyone else in FFA). */
static int lms_opponents(const Ma2World *w, const Ma2PlayerView *me)
{
    int i, n = 0;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (p->player == me->player || !p->alive || !p->actor) continue;
        if (w->team_mode && p->nation == me->nation) continue;
        /* LmsOutCount (default on, RECOMP_BOT_LMS_OUT_COUNT / [Bots] LmsOutCount): a pilot on foot after a death is out (no ride, LmsNoRide),
         * not a rival: counted, blue saw 6 opponents for the whole endgame and the "close in" never fired (au19_41 Team LMS NRC, 158-240 s). */
        if (p->kind == MA2_KIND_PILOT && p->deaths >= 1 && c7_on("lmsoutcount", "RECOMP_BOT_LMS_OUT_COUNT", "LmsOutCount", 1)) continue;
        n++;
    }
    return n;
}

static void assess(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    int i, na = 0;
    float sum = 0.0f, enemy_str = 0.0f, ally_str = 0.0f;
    Ma2Vec3 c = v3(0, 0, 0);
    b->my_dps = k_kind[kind_ok(me->kind)].dps * (f43_on() ? lvl_mult(me->wpn_lvl, 1.0f) : 1.0f);
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
        Mem *m = &b->mem[i];
        float d, in, pot;
        if (!m->valid) continue;
        d = hdist(me->pos, m->pos);
        in = rate_of(m->in_v, m->in_t, b->now);
        pot = mem_pot_dps(b, m, d);
        int atk = b->now - m->hit_me_t < 3.0 || (m->attacking_me && m->visible);
        if (atk) {
            na++; sum += maxf(in, 0.5f * pot); c = vadd(c, m->pos);
        }
        /* ForceAttackers (default on, RECOMP_BOT_FORCE_ATTACKERS / [Bots] ForceAttackers): the odds counted enemies within 150 m only, so a
         * bot shot from further off read force 4.0 ("no enemy") and its attack score took the full x1.2: ad1_20 (TD Bogged Down, Dragoon)
         * b1's tank went 754 -> 154 hp in a duel at range (129-159, force 4.0, 1 attacker) and never broke off. Whoever is hitting us counts
         * at any range. [C7] forceatk: far_s. */
        if ((d < 150.0f || (atk && c7_on("forceatk", "RECOMP_BOT_FORCE_ATTACKERS", "ForceAttackers", 1))) && mem_conf(b, m) > 0.3f) {
            if (d >= 150.0f) c7_add("forceatk", "far_s", b->dt);
            enemy_str += k_kind[m->kind].threat * hp_frac(m->hp, m->hp_max > 0.0f ? m->hp_max : 1.0f) * mem_lvl_mult(b, m);
            if (f43_on() && m->visible && m->wpn_lvl >= 0.5f) c7_add("F43", "upgraded_seen", 1);
            if (f43_on() && m->visible) c7_max("F43", "lvl_max", m->wpn_lvl);
        }
    }
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->alive || !p->actor || p->nation != me->nation) continue;
        if (hdist(p->pos, me->pos) < 150.0f) ally_str += k_kind[kind_ok(p->kind)].threat * hp_frac(p->hp, p->hp_max) * (f43_on() ? lvl_mult(p->wpn_lvl, 1.0f) : 1.0f);
    }
    b->n_attackers = na;
    b->in_dps = sum;
    b->threat_c = na ? vscale(c, 1.0f / (float)na) : me->pos;
    b->force = enemy_str > 0.05f ? ally_str / enemy_str : 4.0f;
    b->outnumbered = (na >= 2 && sum > 1.3f * b->my_dps) || (na >= 3);
    b->losing = b->dmg_recent > 0.12f && sum > 0.8f * b->my_dps;
}

/* How far to fight from: tanks always at range (they snipe with the zoom and die to a 'Mech
 * stepping on them up close); 'Mechs by their weapons' reach and personality -- marksmen with
 * long guns keep their distance, brawlers close in for the campaign AI's duel. */
static void fight_band(Ma2Brain *b, const Ma2PlayerView *me)
{
    int k = kind_ok(me->kind);
    float reach = me->reach > 10.0f ? me->reach : k_kind[k].weapon_range;
    b->band_min = k_kind[k].band_min; b->band_max = k_kind[k].band_max; b->standoff = 0;
    if (k == MA2_KIND_MECH && me->reach_short > 10.0f) {      /* a brawler: where all its guns reach */
        b->band_max = clampf(0.85f * me->reach_short, 70.0f, 160.0f);
        b->band_min = clampf(0.3f * b->band_max, 25.0f, 60.0f);
    }
    if (k == MA2_KIND_BATTLEARMOR) {                          /* on its feet: laser range, mortar further out */
        b->band_max = clampf(0.75f * reach, 40.0f, 150.0f);
        b->band_min = clampf(0.25f * b->band_max, 12.0f, 40.0f);
    }
    if (k == MA2_KIND_TANK) {
        b->band_min = maxf(160.0f, 0.42f * reach); b->band_max = maxf(b->band_min + 90.0f, 0.75f * reach);
        b->standoff = 1;
    } else if (k == MA2_KIND_MECH && reach >= 380.0f && b->marksman > 0.5f) {
        b->band_min = 140.0f; b->band_max = clampf(0.6f * reach, 200.0f, 300.0f);
        b->standoff = 1;
    }
    if (b->sp.mood == 1) { b->band_min *= 0.85f; b->band_max *= 0.85f; }   /* surprise: HOT streak, closes in a little */
}

/* The scoreboard: the field's average kills, and how far an enemy is running away with it --
 * 0 until a few kills clear of the field, 1.5 at most (about 10 clear). A player killing
 * everything draws the other side's attention, as it would among people. */
static float field_kills(const Ma2World *w)
{
    int n = 0, sum = 0;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++)
        if (w->players[j].player && w->players[j].player != w->players[j].actor) { sum += w->players[j].kills; n++; }
    return n ? (float)sum / (float)n : 0.0f;
}
static float menace_of(const Ma2PlayerView *p, float avg_kills)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BOT_LEADER_FOCUS"); on = !(e && *e == '0'); }
    return on ? clampf(((float)p->kills - avg_kills - 2.0f) / 5.0f, 0.0f, 1.5f) : 0.0f;
}
/* LeaderHunt (default on, RECOMP_BOT_LEADER_HUNT / [Bots] LeaderHunt; Elite and Dragoon): a playtester went 15-0 in an Elite TD
 * (11-06 recording) and the bots kept trading shots with each other: menace_of starts 2 kills clear and tops out at 10, and an
 * Elite weighed it x1.45 at most. Elite and Dragoon bots now notice a player pulling ahead from 1 kill clear, rate him up to x2
 * at 4 clear, and weigh it much harder (target x1.9 / x2.5, hunt x5 / x7) -- the side gangs up on whoever is beating it. */
static int leader_hunt_on(void) { return c7_on("leaderhunt", "RECOMP_BOT_LEADER_HUNT", "LeaderHunt", 1); }
static float menace_k(const Ma2Brain *b, const Ma2PlayerView *p, float avg_kills, float w_old, float w_elite, float w_dragoon)
{
    if ((b->skill & 3) == 3 && leader_hunt_on()) {
        float m = clampf(((float)p->kills - avg_kills - 1.0f) / 1.5f, 0.0f, 2.0f);
        return (ma2_bots_dragoon() ? w_dragoon : w_elite) * m;
    }
    return w_old * menace_of(p, avg_kills);
}

/* F31: an enemy pilot on foot scored ~3 against ~20 for a fighting 'Mech, so one walking to the
 * next free 'Mech or tank was left alone, even beside a bot. Within 60 m of a free vehicle (not
 * occupied, alive, with an entry zone) it is worth x1.5 / x2 / x3 / x4 by skill (Recruit .. Elite),
 * x1.5 more when it is nearer that vehicle than we are, scaled down linearly from 20 m to 60 m.
 * RECOMP_BOT_PILOT_DENY / [Bots] PilotDeny. [C7] F31 counters. */
static int pilot_deny_on(void) { return c7_on("F31", "RECOMP_BOT_PILOT_DENY", "PilotDeny", 1); }
static float pilot_deny(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *m)
{
    static const float mult[4] = { 1.5f, 2.0f, 3.0f, 4.0f };
    float bd = 1e9f, x;
    int j, bv = -1;
    for (j = 0; j < w->nvehicles && j < MA2_BOT_MAX_VEHICLES; j++) {
        const Ma2VehicleView *v = &w->vehicles[j];
        float d;
        if (v->occupied || v->hp <= 0.0f || !v->actor || !v->has_entry || v->kind == MA2_KIND_TURRET) continue;
        d = hdist(v->pos, m->pos);
        if (d < bd) { bd = d; bv = j; }
    }
    if (bv < 0 || bd > 60.0f) return 1.0f;
    x = 1.0f + (mult[b->skill & 3] - 1.0f) * clampf((60.0f - bd) / 40.0f, 0.0f, 1.0f);
    if (bd < hdist(me->pos, w->vehicles[bv].pos)) x *= 1.5f;
    c7_add("F31", "boosts", 1); c7_max("F31", "boost_max", x);
    return x;
}

/* Target choice: threat removed per second of our fire. */
static Mem *choose_target(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, int self)
{
    int i;
    int k = kind_ok(me->kind);
    float reach = my_reach(me) * (b->sk.smart ? 1.1f : 1.0f);
    Mem *best = NULL, *cur = mem_by_actor(b, b->target), *carm = NULL;
    float cars = -1e9f, bests = -1e9f, curs = -1e9f, best_thr = 0.0f, cur_thr = 0.0f, best_in = 0.0f, cur_in = 0.0f;
    char lb[480]; int ll = 0; lb[0] = 0;
    float avg_kills = field_kills(w);
    int lms = lms_on(w) || notit_it_on(w, me);
    (void)self;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
        Mem *m = &b->mem[i];
        float d, in, pot, thr, our, ttk, sc;
        if (!m->valid || !mem_engageable(b, m)) continue;
        if (k_kind[m->kind].dps <= 0.0f) continue;
        /* LmsGhost (default on, RECOMP_BOT_LMS_GHOST / [Bots] LmsGhost): in LMS / Team LMS a pilot on foot after a death is out (LmsOutCount);
         * still chosen as a target, live bots hunted ghosts ~230 bot-s (r5_34 LMS Under Cover: b1 after b0 27-76 s, b10, b7; b4, b8 the same,
         * zero hits) and the last two live bots never fought each other. Not a target. [C7] lms: ghost_skipped. */
        if (lms_on(w) && c7_on("lmsghost", "RECOMP_BOT_LMS_GHOST", "LmsGhost", 1)) {
            const Ma2PlayerView *gp = player_by_actor(w, m->actor);
            if (gp && gp->kind == MA2_KIND_PILOT && gp->deaths >= 1) { c7_add("lms", "ghost_skipped", 1); continue; }
        }
        d = hdist(me->pos, m->pos);
        /* Retaliate: one hurting us from beyond our reach (a playtester's TD / Snatch It fights ran at 150-175 m; a bot never even scored a sniper
         * outside 1.3 x its reach) stays a target out to 400 m -- the attack order closes on him */
        int ret_far = (b->skill & 3) >= 2 && rate_of(m->in_v, m->in_t, b->now) > 1.0f && d <= 400.0f && c7_on("retaliate", "RECOMP_BOT_RETALIATE", "Retaliate", 1);
        if (d > reach * 1.3f && !ret_far) continue;
        if (k == MA2_KIND_PILOT && m->kind != MA2_KIND_PILOT && m->kind != MA2_KIND_BATTLEARMOR && d > 60.0f) continue;
        in = rate_of(m->in_v, m->in_t, b->now);
        pot = mem_pot_dps(b, m, d);
        thr = in + (b->sk.smart ? 0.5f : 0.2f) * pot;
        our = rate_of(m->out_v, m->out_t, b->now);
        if (our < 0.5f * my_effective_dps(me, d)) our = my_effective_dps(me, d);
        ttk = (m->hp > 0.0f ? m->hp : 100.0f) / maxf(our, 0.5f);
        if (b->sk.smart) sc = (thr + 0.5f) / maxf(ttk, 3.0f) * 10.0f;
        else sc = thr + 0.5f + 1.0f / (1.0f + d / 60.0f);
        float ob = obj_bonus(w, me, m->player, b->skill);
        sc *= ob;
        if (lms) {   /* F44: a hurt enemy is a life cheaply taken */
            static const float finish[4] = { 0.3f, 0.6f, 0.9f, 1.2f };
            sc *= 1.0f + finish[b->skill & 3] * (1.0f - hp_frac(m->hp, m->hp_max > 0.0f ? m->hp_max : 1.0f));
        }
        if (me->enemy_rider && m->actor == me->enemy_rider) sc *= 3.0f;
        { float sm = spc_target_mult(b, w, me, m); if (sm <= 0.0f) continue; sc *= sm; }   /* surprise set C hack cover */
        if (d > reach) sc *= 0.3f;
        if (!m->visible) sc *= 0.6f;
        else {   /* its defensive system is up: hard (or impossible) to hurt right now */
            const Ma2PlayerView *pv = player_by_actor(w, m->actor);
            if (pv && pv->defense_blocks) sc *= pv->defense_blocks == (MA2_DEF_DIRECT | MA2_DEF_SPLASH) ? 0.3f : 0.8f;
        }
        /* What a sharp player does (a playtester's own play, 2026-10-14: 17 kills to the best bot's 7
         * on Mixed): go for the kill shot on a badly hurt enemy, pile onto one a teammate is
         * already working on, catch one busy fighting someone else -- and keep an eye on whoever
         * is running away with the scoreboard. All of it scaled by skill: Greens barely, Elites
         * a lot. (Before: only Veterans and Elites, and only +15% for a teammate's target under
         * 60% health.) */
        {
            static const float finish_w[4] = { 0.3f, 0.7f, 1.1f, 1.5f }, gang_w[4] = { 0.08f, 0.16f, 0.25f, 0.32f },
                               busy_w[4] = { 0.1f, 0.25f, 0.4f, 0.5f }, lead_w[4] = { 0.1f, 0.2f, 0.32f, 0.45f };
            int sk = b->skill & 3, gang = 0, j;
            float hpf = hp_frac(m->hp, m->hp_max);
            const Ma2PlayerView *pv = player_by_actor(w, m->actor);
            sc *= 1.0f + finish_w[sk] * (1.0f - hpf) * (1.0f - hpf);              /* 20% left: x1.2 .. x1.96 */
            for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *q = &w->players[j];
                if (q != me && q->alive && q->nation == me->nation && q->attacking == m->actor) gang++;
            }
            if (gang) sc *= 1.0f + gang_w[sk] * (float)(gang > 2 ? 2 : gang);
            if (pv && pv->attacking && pv->attacking != me->actor) sc *= 1.0f + busy_w[sk];   /* its guns are on someone else */
            if (pv) sc *= 1.0f + menace_k(b, pv, avg_kills, lead_w[sk], 0.45f, 0.75f);
            sc *= sp_target_bias(b, m->player);   /* surprise: grudge / revenge */
            /* Retaliate (default on, RECOMP_BOT_RETALIATE / [Bots] Retaliate; Veteran and up): under a player's guns (11-06 recording, TD / Base War /
             * Snatch It) a bot targeted him only 10-31 % of the time -- the kill-time score rates a heavy 'Mech low and the keep margin (x1.4 Elite)
             * held it on whatever it had. Whoever is hurting us (in > 1 hp/s) or has us in his sights now weighs x2 Veteran / x3 Elite / x4 Dragoon. */
            if (sk >= 2 && c7_on("retaliate", "RECOMP_BOT_RETALIATE", "Retaliate", 1) && (in > 1.0f || (m->visible && pv && pv->attacking == me->actor))) {   /* a hit tells where it came from */
                sc *= sk == 3 ? (ma2_bots_dragoon() ? 4.0f : 3.0f) : 2.0f;
                c7_add("retaliate", in > 1.0f ? "hit_s" : "aimed_s", b->dt);
            }
        }
        if (m->kind == MA2_KIND_PILOT && k != MA2_KIND_PILOT && pilot_deny_on()) sc *= pilot_deny(b, w, me, m);
        if (w->game_type == MA2_GT_CTF && combo_alert_on()) {   /* ComboAlert: a flag-run pair is worth two kills and their run */
            static const float cm[4] = { 1.5f, 2.0f, 2.6f, 3.0f };
            const Ma2PlayerView *cq = player_by_actor(w, m->actor);
            if (cq && cq->nation != me->nation && combo_of(w, cq)) {
                sc *= cm[b->skill & 3];
                if (m->visible) {
                    int cn = me->nation & 7;
                    uint32_t ca = cq->riding ? cq->riding : m->actor;   /* the pair goes by its VTOL (one call-out, not two flipping) */
                    if (g_combo[cn].actor != ca || b->now - g_combo[cn].t > 5.0) {
                        c7_add("combo", "spotted", 1);
                        if (diag_on()) fprintf(stderr, "[BOT-COMBO] t=%.1f bot=%d spots an enemy VTOL+BattleArmor %08X at %.0f m -- calls it out\n", b->now, b->index, ca, d);
                    }
                    g_combo[cn].actor = ca; g_combo[cn].pos = m->pos; g_combo[cn].t = b->now;
                }
            }
        }
        if (diag_on() && ll < 420)
            ll += snprintf(lb + ll, sizeof lb - ll, " [%s d%.0f in%.0f pot%.0f ttk%.0f s%.2f%s]", k_kind[m->kind].name, d, in, pot, ttk, sc, m == cur ? "*" : "");
        if (m == cur) { curs = sc; cur_thr = thr; cur_in = in; }
        if (ob > 1.0f && m->visible && sc > cars) { cars = sc; carm = m; }
        if (sc > bests) { bests = sc; best = m; best_thr = thr; best_in = in; }
    }
    {
        Mem *pick = best;
        const char *why = "best";
        int sk = b->skill & 3, f84 = f84_on();
        if (f84 && cur && mem_engageable(b, cur) && best && best != cur) {
            static const float fin_hp[4] = { 0.0f, 0.25f, 0.40f, 0.50f }, snap_hp[4] = { 0.0f, 0.0f, 0.30f, 0.40f },
                               keep_k[4] = { 2.0f, 1.15f, 1.3f, 1.4f }, aba_s[4] = { 0.0f, 0.0f, 4.0f, 6.0f };
            static const float dwell[4] = { 3.0f, 2.5f, 2.0f, 1.5f };   /* Recruit .. Elite */
            float cur_hp = hp_frac(cur->hp, cur->hp_max), best_hp = hp_frac(best->hp, best->hp_max);
            int cur_fin = cur_hp < fin_hp[sk] && cur->out_t > 0.0 && b->now - cur->out_t < 3.0;
            int best_snap = best_hp < snap_hp[sk] && best->visible && hdist(me->pos, best->pos) <= reach && !cur_fin;
            if (best_snap && b->now - b->tgt_eval >= 0.5f * b->sk.reaction) { why = "snap"; c7_add("F84", k_f84_snap[sk], 1); }
            else if (b->now - b->tgt_eval < b->sk.reaction) { pick = cur; why = "wait"; }
            else if (f3_on() && cur->visible && b->now - b->tgt_since < dwell[sk] &&
                     best_in < maxf(10.0f, 2.0f * cur_in)) { pick = cur; why = "dwell"; c7_add("F3", "dwell_holds", 1); }
            else if (cur_fin && best_thr < 2.5f * cur_thr + 5.0f) {
                pick = cur; why = "finish"; b->tgt_finish++; c7_add("F84", k_f84_finish[sk], 1);
                if (diag_on() && b->f84_fin != cur->actor)
                    fprintf(stderr, "[THREAT] t=%.1f bot=%d finish %08X hold hp=%.2f sk=%d\n", b->now, b->index, cur->actor, cur_hp, sk);
                b->f84_fin = cur->actor;
            }
            else if (curs * keep_k[sk] >= bests) { pick = cur; why = "keep"; }
            else if (best->actor == b->tgt_prev && b->now - b->tgt_prev_t < aba_s[sk] && curs * 1.6f >= bests) {
                pick = cur; why = "aba"; c7_add("F84", k_f84_aba[sk], 1);
            }
            else why = "switch";
        } else if (cur && mem_engageable(b, cur) && best && best != cur) {
            static const float dwell[4] = { 3.0f, 2.5f, 2.0f, 1.5f };   /* Recruit .. Elite */
            if (b->now - b->tgt_eval < b->sk.reaction) { pick = cur; why = "wait"; }
            else if (f3_on() && cur->visible && b->now - b->tgt_since < dwell[b->skill & 3] &&
                     best_in < maxf(10.0f, 2.0f * cur_in)) { pick = cur; why = "dwell"; c7_add("F3", "dwell_holds", 1); }
            else if (b->sk.smart && hp_frac(cur->hp, cur->hp_max) < 0.35f && rate_of(cur->out_v, cur->out_t, b->now) > 0.0f &&
                     best_thr < 2.5f * cur_thr + 5.0f) { pick = cur; why = "finish"; b->tgt_finish++; }
            else if (curs * (b->sk.smart ? 1.25f : 1.05f) >= bests) { pick = cur; why = "keep"; }
            else why = "switch";
        }
        /* F78: a carrier in sight draws the guns off a plain target: Veterans and Elites at once,
         * Regulars after seeing him 1.5 s, Recruits after 3 s (or once their own target is gone). */
        if (f78_on() && carm && pick != carm && (!pick || obj_bonus(w, me, pick->player, b->skill) <= 1.0f)) {
            static const float wait_s[4] = { 3.0f, 1.5f, 0.0f, 0.0f };
            if (!cur || b->now - carm->vis_since >= wait_s[b->skill & 3]) { pick = carm; why = "carrier"; c7_add("f78", "gun_switch", 1); }
        }
        if (!pick || pick != cur || b->now - b->tgt_eval >= b->sk.reaction) b->tgt_eval = b->now;
        if ((pick ? pick->actor : 0) != b->target) {
            if (f84 && pick) c7_add("F84sw", k_f84_sw[!b->target ? 0 : !strcmp(why, "best") ? 1 : 2][sk], 1);
            if (b->target && pick) {
                b->tgt_switches++; c7_add("F3", "swaps", 1);
                if (b->now - b->tgt_since < 2.0) c7_add("F3", "fast_swaps", 1);
                if (pick->actor == b->tgt_prev && b->now - b->tgt_prev_t < 5.0) c7_add("F3", "aba", 1);
            }
            if (b->target) { b->tgt_prev = b->target; b->tgt_prev_t = b->now; }
            if (diag_on()) {
                const Ma2PlayerView *pv = pick ? player_by_actor(w, pick->actor) : NULL;
                fprintf(stderr, "[THREAT] t=%.1f bot=%d %s %08X -> %08X%s cands:%s\n", b->now, b->index, why, b->target,
                        pick ? pick->actor : 0, pv && pv->is_human ? " (HUMAN)" : "", lb[0] ? lb : " -");
            }
            b->target = pick ? pick->actor : 0;
            b->tgt_since = b->now;
        }
        pick = sp_rk_target(b, w, me, pick, cur);   /* surprise: rookie mistakes A / E / F */
        if ((pick ? pick->actor : 0) != b->target) { if (b->target) { b->tgt_prev = b->target; b->tgt_prev_t = b->now; } b->target = pick ? pick->actor : 0; b->tgt_since = b->now; }
        b->tgt = pick;
        return pick;
    }
}

/* ======================================================================
 * Tactics: position queries
 * ====================================================================== */
static int point_free(const Ma2Senses *sn, Ma2Vec3 p, int kind)
{
    return clearance_at(sn, p.x, p.z) >= k_kind[kind_ok(kind)].radius + 1.0f;
}
static float ally_crowding(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 p)
{
    int i; float pen = 0.0f;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        float d;
        if (q == me || !q->alive || q->nation != me->nation) continue;
        d = hdist(q->pos, p);
        if (d < 15.0f) pen += (15.0f - d) / 15.0f;
    }
    return pen;
}
/* C6 (switch RECOMP_BOT_C6 / ini BotFightSpread): penalty for a fighting spot p against target t that
 * a teammate already uses or is about to stand on (spot_taken), or that lines up on the same bearing
 * from the target as a teammate fighting it (within ~25 degrees, inside 150 m): bearings spread, a
 * pincer instead of a queue. Teammates' positions are taken 1.5 s ahead on their own motion. */
static float fight_spread_pen(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 tpos, Ma2Vec3 p)
{
    float pen = 0.0f, bp;
    if (!c7_on("c6", "RECOMP_BOT_C6", "BotFightSpread", 1)) return 0.0f;
    if (spot_taken(b, w, me, p, 8.0f)) pen += 5.0f;
    bp = heading_of(vsub(p, tpos));
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        Ma2Vec3 qp;
        if (q == me || !q->alive || !q->actor || q->nation != me->nation) continue;
        qp = vadd(q->pos, vscale(v3(q->vel.x, 0.0f, q->vel.z), 1.5f));
        if (hdist(qp, tpos) > 150.0f || hdist(qp, tpos) < 3.0f) continue;
        if (fabsf(wrap_pi(heading_of(vsub(qp, tpos)) - bp)) < 0.45f) pen += 2.0f;
    }
    if (pen > 0.0f) c7_add("c6", "spread_pen", 1);
    return pen;
}

/* (a) A roof too tall for the jets that sees the target: a BattleArmor climbs its face. push-x2: the candidates come
 * from the building list (Ma2Senses.roofs: the roofs of known MABuilding entities within 60 m, probed once each), not
 * from radial samples in open ground (those found no roof in 716 of 720 tries on UC/NRC). A roof counts when it is
 * 10-60 m above us, not a known bad top, within weapon reach of the target (+10 m) and has a line to it. */
static int tac_climb(Ma2Brain *b, const Ma2PlayerView *me, const Mem *t, const Ma2Senses *sn, Ma2Vec3 *out)
{
    Ma2Vec3 roofs[12];
    int k = kind_ok(me->kind), i, n, found = 0;
    float best = -1e9f, reach = my_reach(me) + 10.0f;
    Ma2Vec3 te = eye_of(t->pos, t->kind, 0.6f);
    if (!sn || !sn->roofs) return 0;
    n = sn->roofs(sn->ctx, me->pos, 75.0f, roofs, 12);
    c7_add("hg_climb", n > 0 ? "bld_lists" : "bld_none", 1);
    for (i = 0; i < n; i++) {
        Ma2Vec3 p = roofs[i];
        float rise = p.y - me->pos.y, sc;
        int r;
        c7_add("hg_climb", "cand_roofs", 1);
        if (rise < 10.0f) { c7_add("hg_climb", "cand_low", 1); continue; }
        if (rise > 60.0f) { c7_add("hg_climb", "cand_high", 1); continue; }
        if (bad_top(p)) { c7_add("hg_climb", "cand_badtop", 1); continue; }
        if (hdist(p, t->pos) > reach) { c7_add("hg_climb", "cand_far", 1); continue; }
        sc = rise / 10.0f - hdist(p, me->pos) / 20.0f - 0.5f * fabsf(hdist(p, t->pos) - 0.5f * (b->band_min + b->band_max)) / 50.0f;
        if (sc <= best) continue;
        r = ray(b, sn, me->actor, t->actor, eye_of(p, k, 1.0f), te);
        if (r < 0) { c7_add("hg_climb", "cand_nobudget", 1); break; }
        if (r == 1) { best = sc; *out = p; found = 1; } else c7_add("hg_climb", "cand_nolos", 1);
    }
    return found;
}

/* (d) High ground: step off the far side under heavy fire (bots batch 5, RECOMP_BOT_HG_DROP / [Bots] HighGroundDrop).
 * Up on a roof or a hilltop and taking heavy fire (recent damage over the skill's line, two attackers with a line on
 * us, or a 'Mech below that hit us while we are under half health): look past the edge on the side away from the
 * attackers (weighted by the damage each did) for a spot on the ground the main attacker cannot see, and go there.
 * Better pilots leave sooner; a Recruit notices half the time. Cooldown 10 s. No fall damage in MA2 (research 4).
 * [C7] hg_drop triggers_<skill> ignored dropped no_spot hidden_3s died_5s back_up. 1 = an order was set. */
static int hg_drop_on(void) { return c7_on("hg_drop", "RECOMP_BOT_HG_DROP", "HighGroundDrop",1); }
static int hg_drop(Ma2Brain *b, const Ma2PlayerView *me, const Mem *tgt, const Ma2Senses *sn)
{
    static const float line[4] = { 0.30f, 0.22f, 0.16f, 0.12f }, notice[4] = { 0.5f, 0.7f, 0.85f, 1.0f };
    static const char *const trig[4] = { "triggers_recruit", "triggers_regular", "triggers_veteran", "triggers_elite" };
    static const float da[5] = { 0.0f, 0.61f, -0.61f, 1.22f, -1.22f };
    int k = kind_ok(me->kind), sk = b->skill & 3, natt = 0, below_hit = 0, i, j, roof = b->raised;
    float wx = 0.0f, wz = 0.0f, wbest = -1.0f, away, g0, top;
    const Mem *att = NULL;
    if ((k != MA2_KIND_BATTLEARMOR && k != MA2_KIND_MECH) || !(b->raised || b->hg_hill) || b->now < b->hgd_next || !sn || !sn->surface || !hg_drop_on()) return 0;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Mem *m = &b->mem[i];
        float wt;
        if (!m->valid || !(m->attacking_me || b->now - m->hit_me_t < 2.0)) continue;
        if (m->visible) natt++;
        if (m->kind == MA2_KIND_MECH && m->pos.y < me->pos.y - 3.0f && b->now - m->hit_me_t < 2.0) below_hit = 1;
        wt = rate_of(m->in_v, m->in_t, b->now) + 1.0f;
        { Ma2Vec3 d = vsub(m->pos, me->pos); float l = hlen(d) + 1e-3f; wx += d.x / l * wt; wz += d.z / l * wt; }
        if (wt > wbest) { wbest = wt; att = m; }
    }
    if (!att) return 0;
    if (!(b->dmg_recent >= line[sk] || natt >= 2 || (below_hit && hp_frac(me->hp, me->hp_max) < 0.5f))) return 0;
    b->hgd_next = b->now + 10.0;
    c7_add("hg_drop", trig[sk], 1);
    if (rng_f(&b->rng) >= notice[sk]) { c7_add("hg_drop", "ignored", 1); return 0; }
    away = heading_of((Ma2Vec3){ -wx, 0.0f, -wz });
    { int ok = 0; g0 = sn->ground ? sn->ground(sn->ctx, me->pos.x, me->pos.z, &ok) : me->pos.y; if (!ok) g0 = me->pos.y; }
    top = me->pos.y;
    for (i = 0; i < 5; i++) {
        Ma2Vec3 dir = dir_of_heading(away + da[i]), p = me->pos;
        int found = 0, r;
        for (j = 1; j <= 15 && !found; j++) {                 /* 2 m steps out to 30 m: past the roof edge / down the slope */
            int ok = 0;
            float h;
            p = vadd(me->pos, vscale(dir, 2.0f * (float)j));
            h = roof ? sn->surface(sn->ctx, p.x, p.z, &ok) : ground_at(sn, p.x, p.z, me->pos.y);
            if (roof && !ok) continue;
            if (roof ? h < top - 3.0f : h < g0 - 4.0f) {
                if (roof) p = vadd(p, vscale(dir, 6.0f));     /* clear of the wall */
                else p = vadd(p, vscale(dir, 2.0f));
                found = 1;
            }
        }
        if (!found) continue;
        p.y = ground_at(sn, p.x, p.z, me->pos.y);
        if (!point_free(sn, p, k)) continue;
        r = ray(b, sn, att->actor, me->actor, eye_of(att->pos, att->kind, 1.0f), eye_of(p, k, 1.0f));
        if (r < 0) { b->hgd_next = b->now + 0.5; return 0; }   /* out of rays: try again shortly */
        if (r != 0) continue;
        b->tac_pt = p; b->tac_kind = G_ATTACK; b->tac_tgt = tgt->actor; b->tac_until = b->now + 6.0;
        b->peek_end = 0.0; b->ps_tgt = 0; b->ba_high_next = b->now + 8.0;
        if (k == MA2_KIND_MECH && b->skill >= MA2_SKILL_VETERAN) b->reperch_until = b->now + 16.0;   /* may go back up after */
        b->hgd_t = b->now; b->hgd_att = att->actor; b->hgd_checked = 0;
        c7_add("hg_drop", "dropped", 1);
        if (diag_on()) fprintf(stderr, "[HG-DROP] t=%.1f bot=%d sk=%d kind=%d %s dmg=%.2f att=%d -> %.0f,%.0f,%.0f (from %.0f,%.0f,%.0f)\n", b->now, b->index, sk, k,
                               roof ? "roof" : "hill", b->dmg_recent, natt, p.x, p.y, p.z, me->pos.x, me->pos.y, me->pos.z);
        return 1;
    }
    c7_add("hg_drop", "no_spot", 1);
    return 0;
}
/* (d) after a drop: hidden from that attacker 3 s on? back up high within 20 s? */
static void hg_drop_after(Ma2Brain *b, const Ma2PlayerView *me, const Ma2Senses *sn)
{
    if (b->hgd_t <= 0.0) return;
    if (!b->hgd_checked && b->now - b->hgd_t >= 3.0) {
        const Mem *m = mem_by_actor(b, b->hgd_att);
        int r = m && m->valid ? ray(b, sn, m->actor, me->actor, eye_of(m->pos, m->kind, 1.0f), eye_of(me->pos, me->kind, 1.0f)) : -1;
        if (r >= 0 || !m) { b->hgd_checked = 1; if (r == 0) c7_add("hg_drop", "hidden_3s", 1); }
    }
    if (b->now - b->hgd_t > 3.0 && b->now - b->hgd_t < 20.0 && b->raised) { c7_add("hg_drop", "back_up", 1); b->hgd_t = 0.0; }
    else if (b->now - b->hgd_t >= 20.0) b->hgd_t = 0.0;
}

/* High ground close by: a roof or a hilltop within a short jet of where we are (15-60 m), clearly
 * above us, that sees the target. Small units fight from such spots (peek and hide up there). */
static int roof_flat(const Ma2Senses *sn, Ma2Vec3 p, float s);
static int roof_flat_r(const Ma2Senses *sn, Ma2Vec3 p, float s, float r);
static int tac_high(Ma2Brain *b, const Ma2PlayerView *me, const Mem *t, const Ma2Senses *sn, Ma2Vec3 *out)
{
    static const float rad[3] = { 15.0f, 30.0f, 50.0f };
    int k = kind_ok(me->kind), i, found = 0, rays0 = (int)b->rays;
    float best = -1e9f, toward = heading_of(vsub(t->pos, me->pos));
    Ma2Vec3 te = eye_of(t->pos, t->kind, 0.6f);
    if (!sn || !sn->surface) return 0;
    for (i = 0; i < 24; i++) {
        float a = toward + (float)(i % 8) * (MA2B_PI / 4.0f), sc, g, s;
        int ok = 0, okg = 0, r, roof;
        Ma2Vec3 p = vadd(me->pos, vscale(dir_of_heading(a), rad[i / 8]));
        s = sn->surface(sn->ctx, p.x, p.z, &ok);
        g = sn->ground ? sn->ground(sn->ctx, p.x, p.z, &okg) : s;
        if (!ok || s < me->pos.y + 4.0f) { c7_add("hg_perch", "cand_low", 1); continue; }   /* [C7] why candidates fail */
        if (s > me->pos.y + 22.0f) { c7_add("hg_perch", "cand_high", 1); continue; }      /* clearly higher, within a jet */
        roof = okg && s > g + 3.0f;
        if (roof && k == MA2_KIND_MECH && s > me->pos.y + 12.0f) { c7_add("hg_perch", "cand_high", 1); continue; }   /* (c) a 'Mech's jets: about 12 m */
        if (roof && (!roof_flat(sn, p, s) || bad_top(p) || (k == MA2_KIND_MECH && !roof_flat_r(sn, p, s, 4.5f)))) { c7_add("hg_perch", "cand_notflat", 1); continue; }
        if (!roof && !point_free(sn, p, k)) { c7_add("hg_perch", "cand_notfree", 1); continue; }
        p.y = s;
        sc = (s - me->pos.y) / 5.0f - rad[i / 8] / 30.0f + (roof ? 1.0f : 0.0f) - 0.5f * fabsf(hdist(p, t->pos) - 0.5f * (b->band_min + b->band_max)) / 50.0f;
        if (sc <= best) continue;
        r = ray(b, sn, me->actor, t->actor, eye_of(p, k, 1.0f), te);
        if (r < 0) { c7_add("hg_perch", "cand_nobudget", 1); break; }
        if (r == 1) { best = sc; *out = p; found = 1; } else c7_add("hg_perch", "cand_nolos", 1);
    }
    c7_add("hg_perch", found ? "high_found" : "high_none", 1);
    if (diag_on() && found)
        fprintf(stderr, "[TAC] t=%.1f bot=%d high ground %.0f,%.0f y=%.0f (%.0f m up) (rays %d)\n", b->now, b->index, out->x, out->z, out->y, out->y - me->pos.y, rays0 - (int)b->rays);
    return found;
}

/* The same at radius r (a 'Mech is wide: 4.5 m). */
static int roof_flat_r(const Ma2Senses *sn, Ma2Vec3 p, float s, float r)
{
    int i, n = 0;
    for (i = 0; i < 4; i++) {
        int ok = 0;
        float h = sn->surface(sn->ctx, p.x + (i == 0 ? r : i == 1 ? -r : 0.0f), p.z + (i == 2 ? r : i == 3 ? -r : 0.0f), &ok);
        if (ok && fabsf(h - s) < 1.0f) n++;
    }
    return n >= 3;
}
/* A flat top around p at height s (most of four samples 2.5 m out within a metre): somewhere to stand. */
static int roof_flat(const Ma2Senses *sn, Ma2Vec3 p, float s)
{
    static const float dx[4] = { 2.5f, -2.5f, 0.0f, 0.0f }, dz[4] = { 0.0f, 0.0f, 2.5f, -2.5f };
    int i, n = 0;
    for (i = 0; i < 4; i++) {
        int ok = 0;
        float h = sn->surface(sn->ctx, p.x + dx[i], p.z + dz[i], &ok);
        if (ok && fabsf(h - s) < 1.0f) n++;
    }
    return n >= 3;
}

/* Firing position: inside our weapon band from the target, the target in clear sight, as
 * few other known enemies as possible seeing it, not crowding allies, not far. `around`
 * (radians, or > 9 for "from where we are") orients the candidate fan. */
/* A teammate in the line from `from` to the target: a firing spot there only holds fire. */
static int ally_across(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 from, Ma2Vec3 tp)
{
    float tx = tp.x - from.x, tz = tp.z - from.z, td = sqrtf(tx * tx + tz * tz) + 1e-3f;
    tx /= td; tz /= td;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q == me || !q->alive || q->nation != me->nation) continue;
        float rx = q->pos.x - from.x, rz = q->pos.z - from.z, along = rx * tx + rz * tz, lat = fabsf(rx * tz - rz * tx);
        if (along > 1.0f && along < td + 9.0f && lat < 8.0f) return 1;
    }
    return 0;
}
static int tac_firing(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *t, float around,
                      const Ma2Senses *sn, Ma2Vec3 *out)
{
    static const float offs[9] = { 0.f, 0.44f, -0.44f, 0.87f, -0.87f, 1.4f, -1.4f, 1.92f, -1.92f };
    int k = kind_ok(me->kind), i, found = 0, rays0 = (int)b->rays;
    float band = 0.5f * (b->band_min + b->band_max);
    float a0 = around > 9.0f ? heading_of(vsub(me->pos, t->pos)) : around;
    float best = -1e9f;
    int best_roof = 0;
    Ma2Vec3 te = eye_of(t->pos, t->kind, 0.6f);
    for (i = 0; i < 9; i++) {
        int j, exposed = 0, r;
        float sc;
        float rad = (i & 1) ? band : maxf(b->band_min, band * 0.75f);
        Ma2Vec3 p = vadd(t->pos, vscale(dir_of_heading(a0 + offs[i]), rad));
        int roof = 0;
        p.y = ground_at(sn, p.x, p.z, me->pos.y);
        if ((k == MA2_KIND_BATTLEARMOR || (k == MA2_KIND_MECH && b->now < b->reperch_until)) && sn && sn->surface) {   /* a roof the jets reach */
            int ok = 0;
            float s = sn->surface(sn->ctx, p.x, p.z, &ok);
            if (ok && s > p.y + 3.0f && s < me->pos.y + 22.0f && roof_flat(sn, p, s) && !bad_top(p)) { p.y = s; roof = 1; }
        }
        if (!roof && !point_free(sn, p, k)) continue;
        r = ray(b, sn, me->actor, t->actor, eye_of(p, k, 1.0f), te);
        if (r != 1) continue;
        for (j = 0; j < MA2_WORLD_MAX_PLAYERS && exposed < 3; j++) {
            Mem *o = &b->mem[j];
            if (!o->valid || o == t || mem_conf(b, o) < 0.4f || hdist(o->pos, p) > k_kind[o->kind].weapon_range) continue;
            if (ray(b, sn, o->actor, me->actor, eye_of(o->pos, o->kind, 1.0f), eye_of(p, k, 0.6f)) == 1) exposed++;
            break;   /* one ray for the most relevant other enemy keeps the budget */
        }
        sc = 10.0f - 4.0f * (float)exposed - hdist(me->pos, p) / 40.0f - 2.0f * ally_crowding(w, me, p)
             - (w->team_mode && ally_across(w, me, p, t->pos) ? 6.0f : 0.0f) - fight_spread_pen(b, w, me, t->pos, p)
             + (k == MA2_KIND_BATTLEARMOR ? clampf((p.y - t->pos.y) / 6.0f, -1.0f, 2.5f) + (roof ? 1.0f : 0.0f)
                                          : clampf((p.y - t->pos.y) / 10.0f, -1.0f, 1.5f));
        if (sc > best) { best = sc; *out = p; found = 1; best_roof = roof; }
    }
    if (diag_on())
        fprintf(stderr, "[TAC] t=%.1f bot=%d firing vs %08X band=%.0f -> %s %.0f,%.0f y=%.0f%s (rays %d)\n", b->now, b->index, t->actor, band,
                found ? "ok" : "none", found ? out->x : 0.f, found ? out->z : 0.f, found ? out->y : 0.f, best_roof ? " ROOF" : "", rays0 - (int)b->rays);
    return found;
}

/* Cover / retreat point: line of sight broken to the attackers (weighted by their damage
 * rate), optionally keeping the target in sight (peek), farther from the threats, near an
 * obstacle; retreat also leans toward allies and armour power-ups. */
static int tac_cover(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *keep, int retreat,
                     const Ma2Senses *sn, Ma2Vec3 *out)
{
    const Mem *att[3]; float wgt[3]; int na = 0, i, j, found = 0, rays0 = (int)b->rays;
    int k = kind_ok(me->kind);
    float total = 0.0f, best = -1e9f;
    Ma2Vec3 allies = v3(0, 0, 0); int nal = 0;
    float r_lo = retreat ? 40.0f : 20.0f, r_hi = retreat ? 90.0f : 55.0f;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS && na < 3; i++) {
        Mem *m = &b->mem[i];
        if (!m->valid || mem_conf(b, m) < 0.4f) continue;
        if (!(b->now - m->hit_me_t < 4.0 || m->attacking_me) && !(retreat && hdist(m->pos, me->pos) < 150.0f)) continue;
        if (keep && m == keep && !retreat) continue;
        att[na] = m; wgt[na] = maxf(rate_of(m->in_v, m->in_t, b->now), 0.5f * k_kind[m->kind].dps); total += wgt[na]; na++;
    }
    if (!na && keep && retreat) { att[0] = keep; wgt[0] = k_kind[keep->kind].dps; total = wgt[0]; na = 1; }
    if (!na) return 0;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q == me || !q->alive || q->nation != me->nation) continue;
        if (hdist(q->pos, me->pos) < 300.0f) { allies = vadd(allies, q->pos); nal++; }
    }
    if (nal) allies = vscale(allies, 1.0f / (float)nal);
    for (i = 0; i < 24; i++) {
        float a = (float)(i % 12) * (MA2B_PI / 6.0f) + (i >= 12 ? MA2B_PI / 12.0f : 0.0f);
        float rad = i >= 12 ? r_hi : r_lo;
        Ma2Vec3 p = vadd(me->pos, vscale(dir_of_heading(a), rad));
        float blocked = 0.0f, dgain = 0.0f, sc, cl;
        int budget_out = 0;
        p.y = ground_at(sn, p.x, p.z, me->pos.y);
        cl = clearance_at(sn, p.x, p.z);
        if (cl < k_kind[k].radius + 1.0f) continue;
        /* cheap pre-filter: do not run toward the attackers */
        if (hdist(p, b->threat_c) + 5.0f < hdist(me->pos, b->threat_c) && na) continue;
        for (j = 0; j < na; j++) {
            int r = ray(b, sn, att[j]->actor, me->actor, eye_of(att[j]->pos, att[j]->kind, 1.0f), eye_of(p, k, 0.8f));
            if (r < 0) { budget_out = 1; break; }
            if (r == 0) blocked += wgt[j];
            dgain += hdist(p, att[j]->pos) - hdist(me->pos, att[j]->pos);
        }
        if (budget_out) break;
        sc = 10.0f * blocked / maxf(total, 1.0f) + dgain / (float)na / 20.0f - rad / 60.0f;
        if (cl < k_kind[k].radius + 6.0f) sc += 1.0f;                          /* hugging something solid */
        if (!retreat && keep && blocked > 0.0f) {
            int r = ray(b, sn, me->actor, keep->actor, eye_of(p, k, 1.0f), eye_of(keep->pos, keep->kind, 0.6f));
            if (r == 1) sc += 3.0f;                                            /* can still shoot from there */
        }
        if (retreat && nal) sc += (hdist(me->pos, allies) - hdist(p, allies)) / 30.0f;
        if (retreat) {
            int pi;
            for (pi = 0; pi < w->npickups && pi < MA2_BOT_MAX_PICKUPS; pi++)
                if (w->pickups[pi].kind == MA2_PICKUP_POWERUP && hdist(w->pickups[pi].pos, p) < 25.0f) { sc += 2.0f; break; }
        }
        sc -= 1.5f * ally_crowding(w, me, p);
        if (keep) sc -= fight_spread_pen(b, w, me, keep->pos, p); else if (spot_taken(b, w, me, p, 8.0f)) sc -= 3.0f;
        if (blocked <= 0.0f && dgain / (float)na < (retreat ? 30.0f : 25.0f)) continue;   /* neither hidden nor clear of them */
        if (sc > best) { best = sc; *out = p; found = 1; }
    }
    if (diag_on())
        fprintf(stderr, "[TAC] t=%.1f bot=%d %s from %d attacker(s) in_dps=%.0f -> %s %.0f,%.0f (rays %d)\n", b->now, b->index,
                retreat ? "retreat" : "cover", na, b->in_dps, found ? "ok" : "none", found ? out->x : 0.f, found ? out->z : 0.f, rays0 - (int)b->rays);
    return found;
}

/* ======================================================================
 * Hunting: where to look for a fight
 * ====================================================================== */
/* ======================================================================
 * Lanes
 * Arenas are laid out with a centre route and flanking channels. Each bot keeps a lane
 * (-1 left, 0 centre, +1 right of the straight line to where it is going); teammates spread
 * over the three and a bot changes lane now and then. On a flank lane a long trip goes
 * through the most open ground on that side -- the channel -- so fights spread over the map.
 * ====================================================================== */
#define LANE_MIN_TRIP 150.0f

static void lane_note(Ma2Brain *b, const Ma2PlayerView *me)
{
    int i, slot = -1, old = 0;
    for (i = 0; i < TB_MAX; i++) {
        if (g_lane[i].t > 0.0 && g_lane[i].bot == b->index) { slot = i; break; }
        if (g_lane[i].t < g_lane[old].t) old = i;
    }
    if (slot < 0) slot = old;
    g_lane[slot].nation = me->nation; g_lane[slot].bot = b->index;
    g_lane[slot].lane = (int)b->flank; g_lane[slot].t = b->now > 0.0 ? b->now : 1e-3;
}

/* Take the least used lane of the team (free-for-all: of everyone); the centre takes a bit more. */
static void lane_pick(Ma2Brain *b, const Ma2PlayerView *me)
{
    float team[3] = { 0.0f, -0.35f, 0.0f }, all[3] = { 0.0f, -0.35f, 0.0f }, *load;
    int i, l, best = 1, mates = 0;
    for (i = 0; i < TB_MAX; i++) {
        if (g_lane[i].t <= 0.0 || g_lane[i].bot == b->index || b->now - g_lane[i].t > 30.0) continue;
        if (g_lane[i].lane < -1 || g_lane[i].lane > 1) continue;
        all[g_lane[i].lane + 1] += 1.0f;
        if (g_lane[i].nation == me->nation) { team[g_lane[i].lane + 1] += 1.0f; mates++; }
    }
    load = mates ? team : all;
    for (l = 0; l < 3; l++) load[l] += 0.6f * rng_f(&b->rng);
    for (l = 0; l < 3; l++) if (load[l] < load[best]) best = l;
    b->flank = (float)(best - 1);
    b->via_state = 0;
    b->lane_t = b->now > 0.0 ? b->now : 1e-3;
    lane_note(b, me);
}

/* C5 choke flow. Approaching a choke on the way (a narrow lane stretch, core chokes_find), look at
 * the traffic in it as this team knows it -- teammates always, enemies the team has seen in the
 * last 4 s (blackboard) -- and act like a player: a teammate coming the other way -> keep right
 * through a wide choke, wait at the mouth of a narrow one (known enemies in it are a fight, not
 * traffic: planner cost only); a teammate going the same way just ahead -> file in behind it at the body's spacing
 * (following its point matches its speed; no pushing abreast); the queue long (3+ ahead, or waited
 * 8 s) -> take another lane when its detour costs less than the wait. Occupied chokes also go to the
 * planner as soft costs. env RECOMP_BOT_CHOKES=0/1 / [Bots] Chokes (default 1). */
static float seg_t(Ma2Vec3 a, Ma2Vec3 b, Ma2Vec3 p, float *lat)
{
    float bx = b.x - a.x, bz = b.z - a.z, l2 = bx * bx + bz * bz, t = l2 > 1e-3f ? ((p.x - a.x) * bx + (p.z - a.z) * bz) / l2 : 0.0f;
    float tc = clampf(t, 0.0f, 1.0f), dx = p.x - (a.x + bx * tc), dz = p.z - (a.z + bz * tc);
    *lat = sqrtf(dx * dx + dz * dz);
    return t;
}
static float poly_dist(const Ma2Lane *ln, Ma2Vec3 q)
{
    float best = 1e9f, lat;
    for (int i = 0; i + 1 < ln->n; i++) { seg_t(ln->pt[i], ln->pt[i + 1], q, &lat); if (lat < best) best = lat; }
    return best;
}

/* Lanes spread by team rank and role. The review saw ~70% of the traffic on the
 * centre lane and most bots in one lane all match: each bot's lane was its own random pick with
 * the centre favoured, and a flank bot only aimed at its lane's apex once a trip (past it, or with
 * the apex not on the way, it went straight -- the main route). Now, with the map's lanes known
 * (team modes; found by the planner on any map, editor-made ones with no hints too), each team's
 * bots are ranked -- 'Mechs and tanks first, then by slot -- and dealt over the lanes: a third to
 * the centre, the rest left and right in turn; the deal turns one place every 150 s so no bot holds
 * one lane all match. A flank trip follows its lane's points to where the lane comes nearest the
 * destination, and with no fight to go to a bot pushes up its own lane toward the other team.
 * RECOMP_BOT_LANE_SPREAD=0 / [Bots] LaneSpread=0 = the old picks. Each lane switch has a C7 piece of its own
 * (c7_on caches one switch per piece: sharing "lanes" let the first one read set all five). */
static int lanespread_on(void) { return c7_on("lanes", "RECOMP_BOT_LANE_SPREAD", "LaneSpread", 1); }
/* Batch 3: the lanes as this bot's team walks them. The lane_b team's flanks were planned from its
 * own spawn (lanes_b, core); a side with no flank of its own that way is side 9 there (NRC +1: its
 * east end is a ledge over the east spawn, the way up from below is through the centre). */
static const Ma2Lane *my_lanes(const Ma2World *w, const Ma2PlayerView *me)
{
    return w->lanes_back && w->lane_nation_a >= 0 && me->nation != w->lane_nation_a ? w->lanes_b : w->lanes;
}
static int lane_of_side(const Ma2World *w, const Ma2PlayerView *me, int side)
{
    const Ma2Lane *ls = my_lanes(w, me);
    for (int j = 0; j < w->nlanes && j < 3; j++) if (ls[j].side == side) return j;
    return -1;
}
static int lane_near_idx(const Ma2Lane *ln, Ma2Vec3 q, float *dist)
{
    int bi = 0; float bd = 1e9f;
    for (int i = 0; i < ln->n; i++) { float d = hdist(ln->pt[i], q); if (d < bd) { bd = d; bi = i; } }
    if (dist) *dist = bd;
    return bi;
}
/* Batch 3: the rank by body kind changed whenever a teammate boarded, lost a vehicle or died --
 * on NRC 69 lane flips in 240 s for 11 bots, so a flank bot was dealt another lane before it got
 * to its own. Stable: by team slot only, the deal turns every 150 s and nothing else moves it.
 * RECOMP_BOT_LANE_STABLE=0 / [Bots] LaneStable=0 = heavy bodies first (the batch-2 deal). */
static int lanestable_on(void) { return c7_on("lanestable", "RECOMP_BOT_LANE_STABLE", "LaneStable", 1); }
/* Batch 3: a lane trip with no progress skips ahead (lane_point). RECOMP_BOT_LANE_SKIP=0 / [Bots] LaneSkip=0 = off. */
static int laneskip_on(void) { return c7_on("laneskip", "RECOMP_BOT_LANE_SKIP", "LaneSkip", 1); }
/* Batch 3: lanes with ledge drops are one-way (lane_point). RECOMP_BOT_LANE_DROPS=0 / [Bots] LaneDrops=0 = off. */
static int lanedrops_on(void) { return c7_on("lanedrops", "RECOMP_BOT_LANE_DROPS", "LaneDrops", 1); }
static float lane_assign(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    int n = 0, mine = -1, has[3] = { 0, 0, 0 }, nc, pos, side, stable = lanestable_on();
    const Ma2Lane *ls = my_lanes(w, me);
    for (int j = 0; j < w->nlanes && j < 3; j++) if (ls[j].side >= -1 && ls[j].side <= 1) has[ls[j].side + 1] = 1;
    for (int pass = 0; pass < 2; pass++)                          /* heavy bodies first: they hold the centre */
        for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
            const Ma2PlayerView *p = &w->players[i];
            int heavy = stable || p->kind == MA2_KIND_MECH || p->kind == MA2_KIND_TANK;
            if (!p->is_bot || p->nation != me->nation || heavy != (pass == 0)) continue;
            if (p == me) mine = n;
            n++;
        }
    if (mine < 0 || n == 0) return b->flank;
    nc = has[0] && has[2] ? (n + 2) / 3 : (n + 1) / 2;
    pos = (mine + (int)(w->time / 150.0)) % n;
    if (pos < nc) return 0.0f;
    side = (pos - nc) % 2 ? 1 : -1;
    if (!has[side + 1]) side = -side;
    return has[side + 1] ? (float)side : 0.0f;
}
/* Where q is, across the map (absolute: left / right of the way from lane_a to lane_b): -1, 0, +1,
 * 2 = in a spawn area, 9 = no lanes. With flank lanes known: the nearest lane's side. */
static int zone_of(const Ma2World *w, Ma2Vec3 q)
{
    float dx = w->lane_b.x - w->lane_a.x, dz = w->lane_b.z - w->lane_a.z, len = sqrtf(dx * dx + dz * dz), off;
    if (w->nlanes < 1 || len < 1.0f) return 9;
    if (hdist(q, w->lane_a) < 0.2f * len || hdist(q, w->lane_b) < 0.2f * len) return 2;
    if (w->nlanes > 1) {
        int bj = 0; float bd = 1e9f;
        for (int j = 0; j < w->nlanes && j < 3; j++) { float d = poly_dist(&w->lanes[j], q); if (d < bd) { bd = d; bj = j; } }
        return w->lanes[bj].side;
    }
    off = (dx * (q.z - w->lane_a.z) - dz * (q.x - w->lane_a.x)) / len;   /* the lanes' sign: cross < 0 = side +1 */
    return off < -0.12f * len ? 1 : off > 0.12f * len ? -1 : 0;
}
static void lane_zone_sample(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    static const char *SH[3] = { "share_left", "share_centre", "share_right" }, *FZ[3] = { "fight_left", "fight_centre", "fight_right" };
    int z, k = kind_ok(me->kind);
    if (w->nlanes < 1 || !me->alive || b->now - b->zone_t < 2.0 || k == MA2_KIND_VTOL || k == MA2_KIND_FLYER || k == MA2_KIND_TURRET) return;
    b->zone_t = b->now;
    z = zone_of(w, me->pos);
    if (z >= -1 && z <= 1) c7_add("lanes", SH[z + 1], 1); else if (z == 2) c7_add("lanes", "share_spawn", 1);
    if (b->goal == G_ATTACK && b->tgt && b->tgt->visible && hdist(me->pos, b->tgt->pos) < 200.0f) {
        z = zone_of(w, vscale(vadd(me->pos, b->tgt->pos), 0.5f));
        if (z == 2) c7_add("lanes", "fight_spawn", 1);
        if (z >= -1 && z <= 1) {
            int zones = 0;
            c7_add("lanes", FZ[z + 1], 1);
            g_fz_t[z + 1] = b->now;
            for (int i = 0; i < 3; i++) if (g_fz_t[i] > 0.0 && b->now - g_fz_t[i] < 4.0) zones++;
            if (zones >= 2) c7_add("lanes", "fight_multi", 1);         /* fights in two or three zones at once */
        }
    }
}
/* ChokePlain (default on, RECOMP_BOT_CHOKE_PLAIN / [Bots] ChokePlain): a point choke_flow sets (keep right, wait, file in, FollowHold)
 * kept the objective order's FlagTouch / exact walk, which walks onto the point and on past it: pg81b b8's tank went to its file-in
 * points by the direct walk (W4718) and dithered; iw9b b8's hold point (its own spot) crept it 25 m over the edge at 1178,1414 into a
 * hole at 1167,3,1410 (74-77 s) where no eject could set the pilot down. A choke point is a plain travel stop. [C7] chokes: plain. */
int ma2_bot_leap_near(int index, Ma2Vec3 pos, float r);   /* ma2_bot_ctrl.c: ChokeLeap */
static void choke_plain(Ma2Order *out)
{
    if (!c7_on("chokeplain", "RECOMP_BOT_CHOKE_PLAIN", "ChokePlain", 1)) return;
    if (out->touch || out->exact) c7_add("chokes", "plain", 1);
    out->touch = 0; out->exact = 0; out->exact_r = 0.0f;
}
/* ChokeFileHyst (default on, RECOMP_BOT_CHOKE_FILE_HYST / [Bots] ChokeFileHyst; parity pass 12): the file-in test ran fresh every frame against the
 * nearest team-mate moving 0.5 m/s+ along the choke, so with 3-4 'Mechs leaving a base together it flipped file-in <-> route every few frames
 * (D11_84a T2 48 toggles in 5-40 s, B11_84a1 60) and each flip re-latched the goto (stands of 0.3-1.7 s). A leader counts only while it moves
 * 2 m/s+ along the choke (a crawling or held team-mate is passed by avoidance, not queued behind), and the file-in state changes only after
 * the raw test held 0.4 s. Per-bot state is here, not in Ma2Brain (recording layout). [C7] chokes: filehyst. */
static int g_fh_ck[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, g_fh_state[16], g_fh_raw[16];
static double g_fh_t[16];
static void choke_flow(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *out)
{
    int k = kind_ok(me->kind), best = -1;
    float spacing = k == MA2_KIND_BATTLEARMOR ? 10.0f : 18.0f, bestd = 1e9f;
    Ma2Vec3 dest = out->point, M = { 0, 0, 0 }, E = { 0, 0, 0 }, dir = { 0, 0, 0 };
    int inside = 0;
    for (int c = 0; c < w->nchokes; c++) {
        const Ma2Choke *ch = &w->chokes[c];
        float lat, t = seg_t(ch->a, ch->b, me->pos, &lat), L = hdist(ch->a, ch->b);
        int in = t > 0.0f && t < 1.0f && lat < 0.5f * ch->width + 6.0f;
        Ma2Vec3 m = hdist(me->pos, ch->a) < hdist(me->pos, ch->b) ? ch->a : ch->b, e = m.x == ch->a.x && m.z == ch->a.z ? ch->b : ch->a;
        float dm = hdist(me->pos, m);
        if (L < 1.0f || (!in && dm > 45.0f)) continue;
        if (in) {   /* already in: the end nearer the destination is the way out */
            if (hdist(ch->a, dest) < hdist(ch->b, dest)) { e = ch->a; m = ch->b; } else { e = ch->b; m = ch->a; }
        }
        if (hdist(e, dest) + 10.0f >= hdist(me->pos, dest) || hdist(m, dest) <= hdist(e, dest)) continue;   /* not through it */
        if (dm < bestd) { bestd = dm; best = c; M = m; E = e; inside = in; }
    }
    if (best < 0) { b->ck_id = -1; b->ck_wait_t = b->ck_follow_t = 0.0; g_fh_ck[b->index & 15] = -1; return; }
    /* ChokeLeap (default on, RECOMP_BOT_CHOKE_LEAP / [Bots] ChokeLeap; parity pass 11-10): a body whose route leaves by a jet leap or a step-off at
     * this choke's mouth does not file in behind the bodies there -- it jumps beside them, as a human does. Snatch It Ill Wind choke 2 (team 2's
     * base exit 1135,1479 -> 1165,1487) is the top of BankDive's leap into the basin: P1_81a1 b5 held behind a team-mate 2 m ahead 8.3-21 s
     * (FollowHold, then 'waits, 1 oncoming'), b10 7.2-16.3 s, each one leaping only once the one ahead had gone. [C7] chokes: leaps. */
    if (k != MA2_KIND_TANK && c7_on("chokeleap", "RECOMP_BOT_CHOKE_LEAP", "ChokeLeap", 1) && ma2_bot_leap_near(b->index, me->pos, 60.0f)) {
        b->ck_id = -1; b->ck_wait_t = b->ck_follow_t = 0.0; c7_add("chokes", "leaps", w->dt); return;
    }
    if (best != b->ck_id) { b->ck_id = best; b->ck_wait_t = b->ck_follow_t = 0.0; }
    const Ma2Choke *ch = &w->chokes[best];
    float L = hdist(M, E);
    dir = v3((E.x - M.x) / L, 0.0f, (E.z - M.z) / L);
    int oncoming = 0, foes = 0, ahead = 0; float near_ahead = 1e9f, near_mov = 1e9f; Ma2Vec3 lead = me->pos, lead_mov = me->pos;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        float lat, t;
        if (!p->alive || p->actor == me->actor || p->nation != me->nation) continue;
        t = seg_t(ch->a, ch->b, p->pos, &lat);
        int in = t > -0.1f && t < 1.1f && lat < 0.5f * ch->width + 6.0f;
        float along = p->vel.x * dir.x + p->vel.z * dir.z, ah = (p->pos.x - me->pos.x) * dir.x + (p->pos.z - me->pos.z) * dir.z;
        /* ChokeOncoming (default on, RECOMP_BOT_CHOKE_ONCOMING / [Bots] ChokeOncoming): oncoming = ahead of us and coming through (4 m/s+);
         * any teammate in the choke drifting back at 1.5 m/s counted, so two dithering teammates read each other as traffic and kept
         * dithering 330 m short of their stage (r1_65 CTF On the Rocks b0 176-240, b2 202-240). [C7] chokes: oncoming_ignored. */
        int onc = in && along < -1.5f;
        if (onc && (along > -4.0f || ah <= 0.0f) && c7_on("chokeonc", "RECOMP_BOT_CHOKE_ONCOMING", "ChokeOncoming", 1)) { onc = 0; c7_add("chokes", "oncoming_ignored", 1); }
        if (onc) oncoming++;
        else if ((in || hdist(p->pos, M) < 30.0f) && along > 0.5f && ah > 2.0f) {
            ahead++;
            if (ah < near_ahead) { near_ahead = ah; lead = p->pos; }
            if (along >= 2.0f && ah < near_mov) { near_mov = ah; lead_mov = p->pos; }   /* ChokeFileHyst: a leader that moves */
        }
    }
    for (int i = 0; i < TB_MAX; i++) {   /* enemies the team knows are in it: oncoming as far as we are concerned */
        float lat, t;
        if (g_tb[i].t <= 0.0 || g_tb[i].nation != me->nation || b->now - g_tb[i].t > 4.0) continue;
        t = seg_t(ch->a, ch->b, g_tb[i].pos, &lat);
        if (t > -0.1f && t < 1.1f && lat < 0.5f * ch->width + 6.0f) foes++;   /* a fight, not traffic: the planner cost only */
    }
    if (ahead + oncoming + foes > 0 && out->ndanger < MA2_ORDER_MAX_DANGER) {   /* the planner: a soft cost on the busy choke */
        Ma2PathDanger *d = &out->danger[out->ndanger++];
        d->x = 0.5f * (ch->a.x + ch->b.x); d->z = 0.5f * (ch->a.z + ch->b.z); d->r = 0.5f * L + 10.0f; d->cost = 1.5f * (float)(ahead + oncoming + foes);
    }
    /* The queue is long: another lane, when its detour is shorter than the wait. */
    if (!inside && b->now >= b->ck_relane_until && w->nlanes > 1 &&
        (ahead >= 3 || (b->ck_wait_t > 0.0 && b->now - b->ck_wait_t > 8.0))) {
        float wait_m = (float)(ahead + oncoming) * 4.0f * (k == MA2_KIND_TANK ? 10.0f : 7.0f),   /* metres it would have covered */ base = hdist(me->pos, dest);
        b->ck_relane_until = b->now + 30.0;
        for (int j = 0; j < w->nlanes; j++) {
            const Ma2Lane *ln = &my_lanes(w, me)[j];
            if (j == ch->lane || ln->side < -1 || ln->side > 1 || poly_dist(ln, v3(0.5f * (ch->a.x + ch->b.x), 0.0f, 0.5f * (ch->a.z + ch->b.z))) < ch->width + 20.0f) continue;
            if (hdist(me->pos, ln->apex) + hdist(ln->apex, dest) - base > wait_m) continue;
            b->flank = (float)ln->side; b->via_state = 0; b->ck_wait_t = 0.0; b->lane_keep_until = b->now + 60.0;
            c7_add("chokes", "relanes", 1);
            if (diag_on()) fprintf(stderr, "[BOT-CHOKE] t=%.1f bot=%d choke %d busy (%d ahead, %d oncoming): lane %+d instead\n", b->now, b->index, best, ahead, oncoming, ln->side);
            return;
        }
    }
    Ma2Vec3 right = v3(dir.z, 0.0f, -dir.x);
    /* A wide choke (a valley exit, 40-50 m): room to pass -- keep right through it, no waiting. */
    if (oncoming > 0 && ch->width >= 30.0f && (inside || hdist(me->pos, M) < 30.0f)) {
        out->point = vadd(E, vscale(right, 0.25f * ch->width));
        if (out->stop > 8.0f) out->stop = 8.0f;
        choke_plain(out);
        if (b->ck_wait_t >= 0.0) {
            b->ck_wait_t = -1.0; c7_add("chokes", "keeps_right", 1);
            if (diag_on()) fprintf(stderr, "[BOT-CHOKE] t=%.1f bot=%d keeps right through choke %d (%d oncoming)\n", b->now, b->index, best, oncoming);
        }
        snprintf(b->note, sizeof b->note, "choke-right");
        return;
    }
    if (!inside && oncoming > 0 && hdist(me->pos, M) < 30.0f) {   /* a narrow one: wait at the mouth, keeping right */
        out->point = vadd(vsub(M, vscale(dir, 8.0f)), vscale(right, 0.3f * ch->width + 3.0f));
        out->stop = 3.0f;
        choke_plain(out);
        if (b->ck_wait_t <= 0.0) {
            b->ck_wait_t = b->now; c7_add("chokes", "waits", 1);   /* (-1: keeping right, not waiting) */
            if (diag_on()) fprintf(stderr, "[BOT-CHOKE] t=%.1f bot=%d waits at choke %d (%d oncoming)\n", b->now, b->index, best, oncoming);
        }
        out->yield_wait = 1;   /* ChokeWaitYield (ctrl): the oncoming body does not wait for us in turn */
        snprintf(b->note, sizeof b->note, "choke-wait");
        return;
    }
    b->ck_wait_t = 0.0;
    int file = ahead > 0 && near_ahead < spacing + 6.0f;
    /* ChokeAbreast (default on, RECOMP_BOT_CHOKE_ABREAST / [Bots] ChokeAbreast; parity pass 12): a choke 30 m+ wide (the keep-right width above) has
     * room for two or three 'Mechs side by side, yet every body still filed in 18 m behind the one ahead and held (FollowHold) when that one was
     * close: On the Rocks Snatch It T2's base mouth (choke 1, 40 m) D11_84a 13.5 s of T2 file-in, 5.1 s of it stood; B11_84a1 15.1 / 7.1 s;
     * No Man's Land (choke 3, 44 m) D11_83b T1 16.4 / 8.8 s. Humans leave a wide gap abreast; the avoidance keeps the bodies apart. No file-in
     * there (filesim.py: every wide-choke file-in frame released, Ill Wind's 10-28 m chokes unchanged). [C7] chokes: abreast. */
    if (file && ch->width >= 30.0f && c7_on("chokeabreast", "RECOMP_BOT_CHOKE_ABREAST", "ChokeAbreast", 1)) { c7_add("chokes", "abreast", w->dt); file = 0; }
    else if (c7_on("chokefilehyst", "RECOMP_BOT_CHOKE_FILE_HYST", "ChokeFileHyst", 1)) {
        int bi = b->index & 15, rawm = near_mov < spacing + 6.0f;
        if (g_fh_ck[bi] != best || b->now < g_fh_t[bi]) {   /* another choke, or a new match */ g_fh_ck[bi] = best; g_fh_state[bi] = 0; g_fh_raw[bi] = rawm; g_fh_t[bi] = b->now; }
        if (rawm != g_fh_raw[bi]) { g_fh_raw[bi] = rawm; g_fh_t[bi] = b->now; }
        if (g_fh_state[bi] != rawm && b->now - g_fh_t[bi] >= 0.4) g_fh_state[bi] = rawm;
        int f2 = g_fh_state[bi] && ahead > 0;
        if (f2 && near_mov < 1e8f) { near_ahead = near_mov; lead = lead_mov; }
        if (f2 != file) c7_add("chokes", "filehyst", w->dt);
        file = f2;
    }
    if (file) {   /* file in behind */
        out->point = vsub(lead, vscale(dir, spacing));
        out->stop = 2.0f;
        choke_plain(out);
        /* FollowHold (default on, RECOMP_BOT_FOLLOW_HOLD / [Bots] FollowHold): with the leader only 2-8 m ahead the file-in point (leader
         * minus the spacing) is BEHIND us, so the body turned round to drive back to it, the leader moved on, and it turned again: pg81b b8
         * (Snatch It Ill Wind, team 2's tank) shuttled 1195,1423 <-> 1217,1411 at choke 2 from 63 to 79 s, the order flipping between the
         * route (1187,1394) and the file-in point (1214-1224,1409-1423) every 3-5 s. A file-in point not ahead of us: hold where we are.
         * [C7] chokes: follow_holds. */
        if ((out->point.x - me->pos.x) * dir.x + (out->point.z - me->pos.z) * dir.z < 1.0f && c7_on("followhold", "RECOMP_BOT_FOLLOW_HOLD", "FollowHold", 1)) {
            out->point = me->pos; out->stop = 4.0f; c7_add("chokes", "follow_holds", w->dt);   /* a plain stop: a grab's touch walk stepped past its own spot and crept iw9b b8 over the edge at 1178,1414 into the hole at 1167,3,1410 */
        }
        if (b->ck_follow_t <= 0.0) {
            b->ck_follow_t = b->now; c7_add("chokes", "follows", 1);
            if (diag_on()) fprintf(stderr, "[BOT-CHOKE] t=%.1f bot=%d files in behind at choke %d (%.0f m)\n", b->now, b->index, best, near_ahead);
        }
        snprintf(b->note, sizeof b->note, "choke-follow");
    }
}

/* Why a flank trip along a lane stopped following it (C7 lanes: <left|right>_<why>);
 * "paused": the trip gave way to a fight or another goal (counted once a trip). */
static void lane_trip_end(Ma2Brain *b, int why)
{
    static const char *K[2][8] = { { "left_end_done", "left_end_near", "left_end_off", "left_end_retarget", "left_end_stalled", "left_paused", "left_skips", "left_end_noprog" },
                                   { "right_end_done", "right_end_near", "right_end_off", "right_end_retarget", "right_end_stalled", "right_paused", "right_skips", "right_end_noprog" } };
    if (b->via_state == 1 && b->flank != 0.0f && why >= 0 && why < 8) c7_add("lanes", K[b->flank > 0.0f][why], 1);
}
/* Where a trip to `dest` steers now: through this bot's lane, then straight on. With the map's
 * lanes known (team modes) a flank bot goes by its lane's apex whenever that is on the way
 * (between here and there, at most 1.6x the straight distance); otherwise by the most open
 * ground on its side of the straight line. */
static Ma2Vec3 lane_point(Ma2Brain *b, const Ma2World *w, const Ma2Senses *sn, Ma2Nav *nav, const Ma2PlayerView *me, Ma2Vec3 dest)
{
    static const float T[3] = { 0.5f, 0.38f, 0.62f };
    static const float OFF[6] = { 90.0f, 140.0f, 200.0f, 260.0f, 330.0f, 400.0f };
    float L = hdist(me->pos, dest);
    /* LaneKeep (default on, RECOMP_BOT_LANE_KEEP / [Bots] LaneKeep): a guard point re-rolled round the plant moved the destination 60 m+
     * most frames, and each move restarted the lane trip: au16_90 (Base War Bogged Down) defender b4 stood at 900,1585, 640 m from its
     * plant, 57-145 s with 2568 lane re-joins (left_end_retarget 2701 of 2714 trips); b6 the same 177-185. A trip restarts only when the
     * destination moved 60 m and 30 % of the way left. [C7] lanes: keep_retarget. */
    if (b->via_state && hdist(b->via_dest, dest) > 60.0f) {
        if (c7_on("lanekeep", "RECOMP_BOT_LANE_KEEP", "LaneKeep", 1) && hdist(b->via_dest, dest) <= 0.3f * L) { b->via_dest = dest; c7_add("lanes", "keep_retarget", 1); }
        else { lane_trip_end(b, 3); b->via_state = 0; }   /* somewhere else now */
    }
    /* ChaseDirect (default on, RECOMP_BOT_CHASE_DIRECT / [Bots] ChaseDirect): It moves on every few seconds, so a flank via was re-picked on
     * every 60 m it went (au9_55 Not It Rush Hour, 11-04: 267 re-picks, 10 % of chase orders 75 m+ up on tower roofs; BattleArmor climbing
     * after them got 25 AiKicks / ~200 bot-s). The objective in Not It is the chase: straight at It. [C7] chasedirect: s. */
    if (w->game_type == MA2_GT_NOTIT && b->goal == G_OBJECTIVE && c7_on("chasedirect", "RECOMP_BOT_CHASE_DIRECT", "ChaseDirect", 1)) {
        if (b->via_state == 1) lane_trip_end(b, 3);
        b->via_state = 2; b->via_dest = dest; c7_add("chasedirect", "s", b->dt);
        return dest;
    }
    if (lanespread_on() && w->nlanes > 1) {                                          /* F45: along the lane */
        if (b->via_state == 0) {
            int j = b->flank != 0.0f ? lane_of_side(w, me, (int)b->flank) : -1;
            b->via_state = 2; b->via_dest = dest;
            if (j > 0 && L >= LANE_MIN_TRIP) {
                const Ma2Lane *ln = &my_lanes(w, me)[j];
                float d0, d1, run = 0.0f;
                int i0 = lane_near_idx(ln, me->pos, &d0), i1 = lane_near_idx(ln, dest, &d1);
                /* Batch 3: a lane with ledge drops goes one way (pt[0]'s end down); walked the other way
                 * it starts past its last drop -- NRC +1: the east end is a ledge over the east spawn. */
                if (lanedrops_on() && i1 < i0 && ln->drop_lo > 0 && i0 >= ln->drop_lo) {
                    i0 = ln->drop_lo - 1; d0 = hdist(me->pos, ln->pt[i0]);
                    c7_add("lanes", "drop_joins", 1);
                }
                for (int i = i0 < i1 ? i0 : i1; i < (i0 < i1 ? i1 : i0); i++) run += hdist(ln->pt[i], ln->pt[i + 1]);
                /* joins from up to half the trip away (mid-map bots too, not just from spawn), within the detour cap */
                if (abs(i1 - i0) >= 3 && d0 < maxf(150.0f, minf(0.5f * L, 400.0f)) && d0 + run + d1 <= 1.8f * L + 60.0f) {
                    int same = j == b->lane_j && (i1 > i0 ? 1 : -1) == b->lane_dir && b->now - b->lane_prog_t < 30.0;   /* a new target, same way: keep the skip */
                    b->lane_j = j; b->lane_dir = i1 > i0 ? 1 : -1; b->lane_end = i1; b->via_state = 1; b->lane_join_d = d0; b->lane_paused = 0;
                    if (!same) b->lane_skip = 0;
                    b->lane_prog_i = i0; b->lane_prog_t = b->now; b->lane_prog_d = 1e9f;
                    c7_add("lanes", "flank_trips", 1);
                    if (diag_on()) fprintf(stderr, "[BOT-LANE] t=%.1f bot=%d lane=%+.0f follows points %d..%d to %.0f,%.0f\n", b->now, b->index, b->flank, i0, i1, dest.x, dest.z);
                } else {
                    c7_add("lanes", "flank_off_way", 1);
                    c7_add("lanes", abs(i1 - i0) < 3 ? "off_short" : d0 >= maxf(150.0f, minf(0.5f * L, 400.0f)) ? "off_far" : "off_detour", 1);
                }
            }
        }
        if (b->via_state == 1) {
            if (b->lane_j < 1 || b->lane_j >= w->nlanes) b->via_state = 2;
            else {
                const Ma2Lane *ln = &my_lanes(w, me)[b->lane_j];
                float d; int ic = lane_near_idx(ln, me->pos, &d), k2;
                if (d < b->lane_join_d) b->lane_join_d = d;                              /* still closing on the lane is fine */
                if (L < 100.0f || d > maxf(160.0f, b->lane_join_d + 40.0f) || (b->lane_dir > 0 ? ic >= b->lane_end : ic <= b->lane_end)) {
                    lane_trip_end(b, L < 100.0f ? 1 : d > maxf(160.0f, b->lane_join_d + 40.0f) ? 2 : 0);
                    b->via_state = 2;
                } else {
                    /* Batch 3: no progress along the lane for 10 s (NRC: a tank circled 50 s under the
                     * +1 lane's first points by the east spawn, a gap the lane was planned through at
                     * pilot size): aim 2 points further on, twice, then give the lane up. */
                    int up = lanedrops_on() && b->lane_dir < 0 && ln->drop_lo > 0 && ic >= ln->drop_lo;   /* below the ledge: to its far side first */
                    float dv = up ? hdist(me->pos, ln->pt[ln->drop_lo - 1]) : 0.0f;
                    if ((ic - b->lane_prog_i) * b->lane_dir > 0) { b->lane_prog_i = ic; b->lane_prog_t = b->now; }
                    else if (up && dv < b->lane_prog_d - 10.0f) { b->lane_prog_d = dv; b->lane_prog_t = b->now; }
                    else if (laneskip_on() && b->now - b->lane_prog_t > 10.0) {
                        b->lane_prog_t = b->now;
                        if (b->lane_skip >= 4) { lane_trip_end(b, 7); b->via_state = 2; return dest; }
                        b->lane_skip += 2; lane_trip_end(b, 6);
                        if (diag_on()) fprintf(stderr, "[BOT-LANE] t=%.1f bot=%d lane=%+.0f no progress at point %d: skips to %d\n", b->now, b->index, b->flank, ic, ic + (2 + b->lane_skip) * b->lane_dir);
                    }
                    k2 = up ? ln->drop_lo - 1 : ic + (2 + b->lane_skip) * b->lane_dir;
                    if ((k2 - b->lane_end) * b->lane_dir > 0) k2 = b->lane_end;
                    b->via = ln->pt[k2];
                }
            }
        }
        return b->via_state == 1 ? b->via : dest;
    }
    if (b->via_state == 1 && (hdist(me->pos, b->via) < 30.0f || hdist(me->pos, dest) + 10.0f < hdist(b->via, dest)))
        b->via_state = 2;                                                                /* through the lane */
    if (b->via_state == 0 && b->flank != 0.0f && L >= LANE_MIN_TRIP && w->nlanes > 1) {
        int j;
        b->via_state = 2; b->via_dest = dest;
        for (j = 1; j < w->nlanes; j++) {
            const Ma2Lane *ln = &my_lanes(w, me)[j];
            float d1, d2;
            if (ln->side != (int)b->flank) continue;
            d1 = hdist(me->pos, ln->apex); d2 = hdist(ln->apex, dest);
            if (d1 + d2 > 1.6f * L || d1 < 40.0f) break;                                  /* not on the way / here already */
            if ((ln->apex.x - me->pos.x) * (dest.x - me->pos.x) + (ln->apex.z - me->pos.z) * (dest.z - me->pos.z) <= 0.0f) break;
            b->via = ln->apex; b->via_state = 1;
            if (diag_on()) fprintf(stderr, "[BOT-LANE] t=%.1f bot=%d lane=%+.0f via apex %.0f,%.0f to %.0f,%.0f\n", b->now, b->index, b->flank, b->via.x, b->via.z, dest.x, dest.z);
            break;
        }
        return b->via_state == 1 ? b->via : dest;
    }
    if (b->via_state == 0) {
        b->via_state = 2; b->via_dest = dest;
        if (b->flank != 0.0f && L >= LANE_MIN_TRIP && sn && sn->ground) {
            Ma2Vec3 d = vsub(dest, me->pos);
            Ma2Vec3 side = vscale(v3(d.z / L, 0.0f, -d.x / L), b->flank);
            float bs = -1e9f;
            int ti, oi;
            for (ti = 0; ti < 3; ti++)
                for (oi = 0; oi < 6 && OFF[oi] <= 0.9f * L; oi++) {
                    Ma2Vec3 p = vadd(vadd(me->pos, vscale(d, T[ti])), vscale(side, OFF[oi]));
                    int ok = 0;
                    float cl, sc;
                    p.y = sn->ground(sn->ctx, p.x, p.z, &ok);
                    if (!ok) continue;
                    cl = clearance_at(sn, p.x, p.z);
                    if (cl < 8.0f) continue;                                             /* in the woods / rocks */
                    if (p.y > me->pos.y + 15.0f && c7_on("viaroof", "RECOMP_BOT_VIA_ROOF", "ViaRoof", 1)) { c7_add("viaroof", "skipped", 1); continue; }   /* ViaRoof: a roof / tower top 15 m+ over us is no way round (au9_55) */
                    if (nav && !ma2_nav_connected(nav, me->pos, p)) continue;
                    sc = minf(cl, 30.0f) - 0.025f * OFF[oi] - 8.0f * fabsf(T[ti] - 0.5f);
                    if (sc > bs) { bs = sc; b->via = p; b->via_state = 1; }
                }
            if (diag_on() && b->via_state == 1)
                fprintf(stderr, "[BOT-LANE] t=%.1f bot=%d lane=%+.0f via %.0f,%.0f to %.0f,%.0f\n", b->now, b->index, b->flank, b->via.x, b->via.z, dest.x, dest.z);
        }
    }
    return b->via_state == 1 ? b->via : dest;
}

/* RouteSane (default on, RECOMP_BOT_ROUTE_SANE / [Bots] RouteSane): hunt and lane-push points were chosen by straight-line
 * distance, so a front-line bot could be sent to a point across a ridge whose route runs back through its own base -- d5_25 b3
 * 187-232 'push-left' 340 m straight / 900 m by route, 45 s without contact; b2 43-70 hunt-left into its own base. A point
 * whose graph route is over twice the straight line (and 250 m longer) is a bad pick: the caller tries another. */
static int route_sane_on(void) { static int on = -1; if (on < 0) on = c7_on("routesane", "RECOMP_BOT_ROUTE_SANE", "RouteSane", 1); return on; }
static int route_bad(Ma2Nav *nav, Ma2Vec3 from, Ma2Vec3 to)
{
    if (!nav || !route_sane_on()) return 0;
    float straight = hdist(from, to);
    if (straight < 120.0f) return 0;
    Ma2Vec3 wp[64];
    int n = ma2_nav_path(nav, from, to, wp, 64);
    if (n <= 0) return 0;                                        /* no graph route: the planner decides */
    float len = 0.0f; Ma2Vec3 prev = from;
    for (int i = 0; i < n; i++) { len += hdist(prev, wp[i]); prev = wp[i]; }
    int bad = len > 2.0f * straight && len > straight + 250.0f;
    c7_add("routesane", "checks", 1);
    if (bad) { c7_add("routesane", "rejects", 1);
               if (diag_on()) fprintf(stderr, "[BOT-ROUTESANE] %.0f,%.0f -> %.0f,%.0f: straight %.0f m, route %.0f m: another pick\n", from.x, from.z, to.x, to.z, straight, len); }
    return bad;
}
static void pick_roam_raw(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav)
{
    Mem *m = NULL;
    int i, zon, cap, spread = 0, lane_pull = 0;
    double best = -1e9;
    /* LmsGhostRoam (default on, RECOMP_BOT_LMS_GHOST_ROAM / [Bots] LmsGhostRoam): LmsGhost kept LMS ghosts (pilots on foot after a death: out)
     * off the target list, but the roam still 'investigated' them: A_400a Team LMS b1 sat on T2's parked ghosts 143-180 s (stomped one, fired
     * on them) while the last live T2 pilot was 800 m off. [C7] lms: ghost_roam_skipped. */
    int ghr = lms_on(w) && c7_on("lmsghostroam", "RECOMP_BOT_LMS_GHOST_ROAM", "LmsGhostRoam", 1);
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
        Mem *q = &b->mem[i];
        if (ghr && q->valid) { const Ma2PlayerView *gp = player_by_actor(w, q->actor);
            if (gp && gp->kind == MA2_KIND_PILOT && gp->deaths >= 1) { c7_add("lms", "ghost_roam_skipped", 1); continue; } }
        if (q->valid && q->last_known > best && mem_conf(b, q) > 0.15f) { best = q->last_known; m = q; }
    }
    b->roam_valid = 1;
    b->roam_until = b->now + 20.0;
    zon = zones_on();
    cap = zon ? zone_cap(b, me) : 99;
    if (m && hdist(m->pos, me->pos) > 12.0f) {
        Ma2Vec3 at = mem_predict(b, m);
        int other = 0;
        if (zon && zone_load(b, me, at) >= cap)   /* that fight has its share: another enemy, if there is one to go after */
            for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS && !other; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (is_enemy(w, me, p) && p->alive && fabsf(p->pos.x) + fabsf(p->pos.z) >= 1.0f && hdist(p->pos, at) >= 2.0f * ZONE_R &&
                    zone_load(b, me, p->pos) < cap) other = 1;
            }
        /* F45: a flank bot whose remembered enemy is well off its lane goes up its lane instead, half the time */
        if (!other && lanespread_on() && w->nlanes > 1 && b->flank != 0.0f) {
            int lj = lane_of_side(w, me, (int)b->flank);
            if (lj > 0 && poly_dist(&my_lanes(w, me)[lj], at) > 150.0f && rng_f(&b->rng) < 0.5f) { other = 1; lane_pull = 1; c7_add("lanes", "pulls", 1); }
        }
        if (!other) { b->roam_pt = at; snprintf(b->note, sizeof b->note, "investigate"); return; }
        if (!lane_pull) { c7_add("zones", "skip_full", 1); spread = 1; }
    }
    /* GrinderGuard (default on, RECOMP_BOT_GRINDER_GUARD / [Bots] GrinderGuard): Grinder is the player and his bots against the game's waves,
     * which come for the player; with no enemy to investigate the roam took a random nav node -- r5_01 NRC b0 ran 350 m SE (101.7-121.6) away
     * from the human and the NW wave spawn, and the wave killed the human 1000 m from it. No lead: stay within 15-30 m of the human. */
    if (w->game_type == MA2_GT_GRINDER && c7_on("grinderguard", "RECOMP_BOT_GRINDER_GUARD", "GrinderGuard", 1)) {
        for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
            const Ma2PlayerView *h = &w->players[i];
            if (!h->is_human || !h->alive || h->nation != me->nation || fabsf(h->pos.x) + fabsf(h->pos.z) < 1.0f) continue;
            b->roam_pt = vadd(h->pos, vscale(dir_of_heading(rng_range(&b->rng, -MA2B_PI, MA2B_PI)), rng_range(&b->rng, 15.0f, 30.0f)));
            b->roam_until = b->now + 8.0;
            c7_add("grinderguard", "picks", 1);
            snprintf(b->note, sizeof b->note, "grinder-guard");
            return;
        }
    }
    /* LmsSweep (default on, RECOMP_BOT_LMS_SWEEP / [Bots] LmsSweep): Team LMS endgame with no lead -- red was down to one player on its pad
     * from 136.5 s and blue's three roamed random nodes (b10 never within 584 m, hunting toward its own spawn) to the end (r5_46b). With
     * two or fewer opponents and nothing to investigate, sweep the other team's spawn end of the main lane, as a player would. */
    if (w->game_type == MA2_GT_TEAM_LMS && lms_on(w) && lms_opponents(w, me) <= 2 && w->nlanes >= 1 && w->lane_nation_a >= 0 &&
        c7_on("lmssweep", "RECOMP_BOT_LMS_SWEEP", "LmsSweep", 1)) {
        const Ma2Lane *ln = &my_lanes(w, me)[0];
        if (ln->n >= 2) {
            int ke = me->nation != w->lane_nation_a ? 0 : ln->n - 1;                 /* (our spawn is lane_b's end: theirs is the other one) */
            b->roam_pt = vadd(ln->pt[ke], vscale(dir_of_heading(rng_range(&b->rng, -MA2B_PI, MA2B_PI)), rng_range(&b->rng, 0.0f, 30.0f)));
            b->roam_until = b->now + 15.0;
            c7_add("lms", "sweep_picks", 1);
            snprintf(b->note, sizeof b->note, "lms-sweep");
            return;
        }
    }
    /* F45: up our own lane toward the other team's spawn now and then, so each lane gets its fight. */
    /* OneLanePush (default on, RECOMP_BOT_ONE_LANE_PUSH / [Bots] OneLanePush): the push needed 2+ lanes; on a one-lane map (No Man's Land TD)
     * a bot with no enemy learnt wandered pickups and random nodes -- d5_27: ~245 bot-s before first contact (b3 65 s, b4 55 s). One lane:
     * push up it. [C7] lanes: pushes. */
    int one_lane = w->nlanes == 1 && c7_on("onelanepush", "RECOMP_BOT_ONE_LANE_PUSH", "OneLanePush", 1);
    if (!spread && lanespread_on() && (w->nlanes > 1 || one_lane) && w->lane_nation_a >= 0 && (lane_pull || rng_f(&b->rng) < 0.4f)) {   /* (a full fight already sent us hunting elsewhere: that wins) */
        int j = one_lane ? 0 : lane_of_side(w, me, (int)b->flank);
        if (j >= 0 && my_lanes(w, me)[j].n >= 2) {
            const Ma2Lane *ln = &my_lanes(w, me)[j];
            int kk = (int)(rng_range(&b->rng, 0.55f, 0.85f) * (float)(ln->n - 1) + 0.5f);
            Ma2Vec3 p;
            if (me->nation != w->lane_nation_a) kk = ln->n - 1 - kk;                  /* our spawn is lane_b's end */
            p = ln->pt[kk];
            if (route_bad(nav, me->pos, p)) {                                        /* RouteSane: a point on the lane we can reach from here, further along */
                int found = 0;
                for (int q = 0; q < 5 && !found; q++) {
                    int k2 = (int)((0.55f + 0.1f * (float)q) * (float)(ln->n - 1) + 0.5f);
                    if (me->nation != w->lane_nation_a) k2 = ln->n - 1 - k2;
                    if (k2 != kk && !route_bad(nav, me->pos, ln->pt[k2])) { p = ln->pt[k2]; found = 1; }
                }
                if (!found) p = me->pos;                                             /* nothing on this lane: fall through to the hunt */
            }
            if (hdist(me->pos, p) > 60.0f && (!nav || ma2_nav_connected(nav, me->pos, p))) {
                b->roam_pt = p; b->roam_until = b->now + 25.0;
                c7_add("lanes", "pushes", 1);
                snprintf(b->note, sizeof b->note, b->flank < 0.0f ? "push-left" : b->flank > 0.0f ? "push-right" : "push-centre");
                return;
            }
        }
    }
    /* Like a classic deathmatch bot: head for the rough area of a live opponent (radar-scale
     * knowledge, never the exact spot), re-aimed every few seconds; sometimes via pickups. */
    if (spread || b->sp.rv_until > b->now || rng_f(&b->rng) < 0.6f + 0.3f * b->aggr) {   /* (a revenge always hunts) */
        /* Which one: any of them, the runaway leader more likely (weighted pick; an Elite with
         * someone 10 kills clear of the field goes after him about two times in three). */
        static const float hunt_w[4] = { 0.5f, 1.0f, 2.0f, 3.0f };
        int excl = -1, attempt = 0;
    rehunt:;
        int pick = -1;
        float tw = 0.0f, avg_kills = field_kills(w);
        int f27 = f27_on(), nstale = 0, pick_kn = -1;
        for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
            const Ma2PlayerView *p = &w->players[i];
            int kj = -1;
            if (!is_enemy(w, me, p)) continue;
            if (!p->alive || fabsf(p->pos.x) + fabsf(p->pos.z) < 1.0f) continue;   /* dead / not placed yet: no position */
            if (i == excl) continue;                                                  /* RouteSane: his area is a detour from here */
            if (f27) {   /* only someone we have learnt of, in that body, within the last minute */
                int j;
                for (j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) if (b->kn[j].player == p->player) { kj = j; break; }
                if (kj < 0) continue;
                if (b->kn[kj].actor != p->actor || b->now - b->kn[kj].t > 60.0 || b->now < b->kn[kj].t) { nstale++; continue; }
            }
            float wt = 1.0f + menace_k(b, p, avg_kills, hunt_w[b->skill & 3], 2.0f, 3.0f);
            wt *= sp_hunt_mult(b, p->player);   /* surprise: grudge / revenge */
            if (zon) {   /* a fight that already has its share of the team: rarely; a free one: likelier */
                int ld = zone_load(b, me, p->pos);
                wt *= ld >= cap ? 0.1f : 1.0f / (1.0f + 0.5f * (float)ld);
            }
            if (lanespread_on() && w->nlanes > 1) {                                   /* F45: our lane's enemies first */
                int j = lane_of_side(w, me, (int)b->flank);
                if (j >= 0 && poly_dist(&my_lanes(w, me)[j], p->pos) < 100.0f) wt *= 3.0f;
            }
            tw += wt;
            if (rng_f(&b->rng) * tw < wt) { pick = i; pick_kn = kj; }
        }
        if (f27) c7_add("f27", pick >= 0 ? "known" : nstale ? "stale" : "none", 1);
        if (pick >= 0) {
            float h = rng_range(&b->rng, -MA2B_PI, MA2B_PI);
            if (spread) { c7_add("zones", "spread_hunts", 1); c7_add("zones", k_c7_spread_sk[b->skill & 3], 1); }
            /* F27: around where we last learnt of him, the older the wider (25-70 m fresh, up to +60 m a minute on) */
            Ma2Vec3 at = pick_kn >= 0 ? b->kn[pick_kn].pos : w->players[pick].pos;
            float age = pick_kn >= 0 ? (float)(b->now - b->kn[pick_kn].t) : 0.0f;
            if (diag_on()) fprintf(stderr, "[BOT-HUNT] t=%.1f bot=%d sk=%d hunts %d%s kills=%d menace=%.2f of %d age=%.0f off=%.0f\n", b->now, b->index, b->skill, pick,
                                   w->players[pick].is_human ? " (HUMAN)" : "", w->players[pick].kills, menace_of(&w->players[pick], avg_kills), (int)(tw + 0.5f),
                                   age, hdist(at, w->players[pick].pos));
            if (f27) c7_add("f27", "hunts", 1);
            b->roam_pt = vadd(at, vscale(dir_of_heading(h), rng_range(&b->rng, 25.0f, 70.0f) + age));
            if (route_bad(nav, me->pos, b->roam_pt)) {                               /* RouteSane: another side of his area, else someone else */
                int fixed = 0;
                for (int q = 0; q < 2 && !fixed; q++) {
                    Ma2Vec3 alt = vadd(at, vscale(dir_of_heading(h + 2.094f * (float)(q + 1)), 25.0f + age));
                    if (!route_bad(nav, me->pos, alt)) { b->roam_pt = alt; fixed = 1; }
                }
                if (!fixed && attempt == 0) { excl = pick; attempt = 1; goto rehunt; }
            }
            b->roam_until = b->now + 8.0;
            if (hdist(me->pos, b->roam_pt) > LANE_MIN_TRIP) {
                if (!(lanespread_on() && w->nlanes > 1) && rng_f(&b->rng) < 0.25f) lane_pick(b, me);   /* now and then another channel */
                b->roam_until = b->now + 20.0;                      /* a long trip: do not re-aim it every 8 s */
            }
            snprintf(b->note, sizeof b->note, b->flank < 0.0f ? "hunt-left" : b->flank > 0.0f ? "hunt-right" : "hunt");
            return;
        }
    }
    if (w->npickups > 0 && rng_f(&b->rng) < 0.5f) {
        int n = w->npickups < MA2_BOT_MAX_PICKUPS ? w->npickups : MA2_BOT_MAX_PICKUPS;
        int k = (int)(rng_f(&b->rng) * (float)n) % n;
        if (hdist(w->pickups[k].pos, me->pos) > 12.0f) { b->roam_pt = w->pickups[k].pos; return; }
    }
    if (nav) {
        int nn = ma2_nav_node_count(nav);
        if (nn > 0) {
            int tries;
            for (tries = 0; tries < 4; tries++) {
                int k = (int)(rng_f(&b->rng) * (float)nn) % nn;
                Ma2Vec3 p = ma2_nav_node_pos(nav, k);
                if (hdist(p, me->pos) > 30.0f && ma2_nav_connected(nav, me->pos, p)) { b->roam_pt = p; return; }
            }
        }
    }
    b->roam_pt = vadd(me->pos, vscale(dir_of_heading(rng_range(&b->rng, -MA2B_PI, MA2B_PI)), 60.0f));
}

/* WP3-C HuntReach (default on, RECOMP_BOT_HUNT_REACH / [Bots] HuntReach; lead item from Worker B): a pilot (no jets), 'Mech or tank hunting / roaming rolls again
 * (3 times) when the point lies 6 m+ ABOVE it on another walkable region (ground_cut: GroundReach's test). Pilots in the sw33 re-sweep sat in no_path loops on hunts
 * they could not walk to (35 episodes / 28 s for bot 6). First version also cut same-level points and fell back to a 60 m wander: on Proving Ground's ledges that
 * was 2621 of 2621 rolls (the region map splits the ledges) and put more pilots on foot, so now only points above, and the last roll stands if all are cut. */
static int hunt_cut(const Ma2PlayerView *me, Ma2Vec3 to) { return to.y > me->pos.y + 6.0f && ground_cut(me, to); }
static void pick_roam_hr(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav)
{
    int tries;
    pick_roam_raw(b, w, me, nav);
    if (!c7_on("huntreach", "RECOMP_BOT_HUNT_REACH", "HuntReach", 1) || !hunt_cut(me, b->roam_pt)) return;
    c7_add("huntreach", "cut", 1);
    for (tries = 0; tries < 3; tries++) {
        pick_roam_raw(b, w, me, nav);
        if (!hunt_cut(me, b->roam_pt)) { c7_add("huntreach", "rerolled", 1); return; }
    }
    c7_add("huntreach", "all_cut", 1);   /* (807 of 807 on Proving Ground: the prey sits on a ledge) -- as near as the ground allows */
    if (nav) {
        Ma2Vec3 goal = b->roam_pt, bp = goal; float bd = 1e9f; int nn = ma2_nav_node_count(nav), k;
        for (k = 0; k < 12 && nn > 0; k++) {
            Ma2Vec3 p = ma2_nav_node_pos(nav, (int)(rng_f(&b->rng) * (float)nn) % nn);
            float d = hdist(p, goal);
            if (d < bd && hdist(p, me->pos) > 30.0f && ma2_nav_connected(nav, me->pos, p)) { bd = d; bp = p; }
        }
        if (bd < 1e8f) { b->roam_pt = bp; c7_add("huntreach", "nearest_node", 1); }
    }
}

/* ReachSet: a hunt / roam point outside what the bot's last finished search reached is rolled again (6 times), then a point 30-70 m off inside it is walked to.
 * sw34 bot 9 rolled a new unreachable hunt point every 3 s for 23 s, sw14 bot 8 for 35 s. [C7] reachset: rerolled, wander. */
/* RoamEdge (default on, RECOMP_BOT_ROAM_EDGE / [Bots] RoamEdge): a roam point is re-picked within 10 m of it, but VtolEdgeAll moves a
 * VTOL's goal up to 25 m in from the map edge, so a VTOL 'arrived' (stopped, hovering) 20 m short and waited out the 20 s roam timer
 * (r4_83 b2 14.4-34.4 at 386,984 beside its spawn). A VTOL re-picks within 35 m. */
static int roam_due(const Ma2Brain *b, const Ma2PlayerView *me)
{
    float r = (me->kind == MA2_KIND_VTOL && c7_on("roamedge", "RECOMP_BOT_ROAM_EDGE", "RoamEdge", 1)) ? 35.0f : 10.0f;
    /* RoamArrive (default on, RECOMP_BOT_ROAM_ARRIVE / [Bots] RoamArrive): a 'Mech / tank's goto halts 11-25 m short of a roam point (its stop
     * margin) and the 10 m re-pick never came: d5_27 9 stands, ~27 s (b10 41.4-46.4 until AiKick). They re-pick within 18 m. */
    if ((me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK) && c7_on("roamarrive", "RECOMP_BOT_ROAM_ARRIVE", "RoamArrive", 1)) r = 18.0f;
    if (b->roam_valid && b->now <= b->roam_until && r > 10.0f && hdist(me->pos, b->roam_pt) < r && hdist(me->pos, b->roam_pt) >= 10.0f) c7_add("roamedge", "repicks", 1);
    return !b->roam_valid || b->now > b->roam_until || hdist(me->pos, b->roam_pt) < r;
}
static void pick_roam(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav)
{
    int tries;
    pick_roam_hr(b, w, me, nav);
    if (!reach_cut(b, me, b->roam_pt)) return;
    for (tries = 0; tries < 6; tries++) {
        pick_roam_hr(b, w, me, nav);
        if (!reach_cut(b, me, b->roam_pt)) { c7_add("reachset", "rerolled", 1); return; }
    }
    for (tries = 0; tries < 12; tries++) {
        Ma2Vec3 p = vadd(me->pos, vscale(dir_of_heading(rng_range(&b->rng, -MA2B_PI, MA2B_PI)), rng_range(&b->rng, 30.0f, 70.0f)));
        if (ma2_path_reach_has(b->index, me->pos, p, b->now) == 1) { b->roam_pt = p; b->roam_until = b->now + 6.0; c7_add("reachset", "wander", 1); return; }
    }
}

/* ======================================================================
 * Vehicles
 * ====================================================================== */
static float vehicle_pref(int kind, int skill, int game_type)
{
    if (!ma2_brain_kind_allowed(kind, game_type)) return 0.0f;
    switch (kind) {
    case MA2_KIND_MECH: return 1.0f;
    case MA2_KIND_TANK: return getenv("RECOMP_TEST_PREFER_TANK") ? 2.0f : 0.80f;   /* test hook: a roster of tanks */
    case MA2_KIND_VTOL: return 0.55f + 0.05f * (float)skill;
    case MA2_KIND_BATTLEARMOR: return 0.30f + 0.05f * (float)skill;
    case MA2_KIND_TURRET: return 3.0f;              /* only with RECOMP_TEST_TURRET_PICK (kind_allowed) */
    default: return 0.0f;
    }
}
/* Test hook RECOMP_TEST_ROLES=1 (lift proofs, default off): bot index % 3 == 0 picks a VTOL, == 1 a BattleArmor, so every team has
 * VTOLs and BattleArmor to pair (a plain roster often has one VTOL, busy fighting). */
static float vehicle_pref_b(const Ma2Brain *b, int kind)
{
    static int roles = -1;
    if (roles < 0) roles = getenv("RECOMP_TEST_ROLES") != NULL;
    float p = vehicle_pref(kind, b->skill, 0);
    if (roles && p > 0.0f) {
        if (b->index % 3 == 0 && kind == MA2_KIND_VTOL) p = 3.0f;
        else if (b->index % 3 == 1 && kind == MA2_KIND_BATTLEARMOR) p = 3.0f;
    }
    {   /* RECOMP_TEST_BOT_BA / RECOMP_TEST_BOT_VTOL=<bot index> (test hooks, VtolTwoRiders proof): that bot takes and keeps a BattleArmor / VTOL */
        static int tba = -2, tvt = -2;
        if (tba == -2) { const char *e = getenv("RECOMP_TEST_BOT_BA"); tba = e && *e ? atoi(e) : -1; e = getenv("RECOMP_TEST_BOT_VTOL"); tvt = e && *e ? atoi(e) : -1; }
        if (p > 0.0f && ((tba == b->index && kind == MA2_KIND_BATTLEARMOR) || (tvt == b->index && kind == MA2_KIND_VTOL))) p = 3.0f;
    }
    return p;
}

/* Vehicles that refused us (no prompt, unreachable) are skipped for 30 s, then tried again. */
static int no_board_has(const Ma2Brain *b, uint32_t a)
{
    int i;
    for (i = 0; i < b->n_no_board; i++) if (b->no_board[i] == a && b->now < b->no_board_until[i]) return 1;
    return 0;
}
static void no_board_for(Ma2Brain *b, uint32_t a, double secs)
{
    int i;
    if (!a) return;
    for (i = 0; i < b->n_no_board; i++) if (b->no_board[i] == a) { b->no_board_until[i] = b->now + secs; return; }
    if (b->n_no_board >= 16) { memmove(b->no_board, b->no_board + 1, 15 * sizeof(uint32_t)); memmove(b->no_board_until, b->no_board_until + 1, 15 * sizeof(double)); b->n_no_board = 15; }
    b->no_board[b->n_no_board] = a; b->no_board_until[b->n_no_board] = b->now + secs; b->n_no_board++;
}
/* A vehicle whose zone gave one of us no prompt is passed over by the whole roster: on In Your
 * Face the parked BattleArmor refused every pilot (0 of 17 tries), and each one walked up to it
 * and lost 20 s finding out. Twice refused: left alone for longer. */
static struct { uint32_t actor; double until; int fails; } g_refused[32];
static int refused_has(uint32_t a, double now)
{
    for (int i = 0; i < 32; i++) if (g_refused[i].actor == a && now < g_refused[i].until) return 1;
    return 0;
}
static void refused_add(uint32_t a, double now)
{
    int j = -1, old = 0;
    for (int i = 0; i < 32; i++) { if (g_refused[i].actor == a) j = i; if (g_refused[i].until < g_refused[old].until) old = i; }
    if (j < 0) { j = old; g_refused[j].actor = a; g_refused[j].fails = 0; }
    if (now < g_refused[j].until - 600.0) g_refused[j].fails = 0;      /* a new match: the clock went back */
    g_refused[j].fails++;
    g_refused[j].until = now + (g_refused[j].fails >= 2 ? 240.0 : 90.0);
}
static void no_board_add(Ma2Brain *b, uint32_t a) { no_board_for(b, a, 30.0); refused_add(a, b->now); }
/* StuckHull (default on, RECOMP_BOT_STUCK_HULL / [Bots] StuckHull): a hull a bot bailed out of as trapped stays trapped, and the next pilot
 * walked to it and climbed in (rc14_80a Snatch It Bogged Down: bot 0 bailed at 357,1649 at 115.5 s, bot 3 boarded that tank and was trapped
 * at 147.2 s). Nobody boards it for 10 min. [C7] stuckhull: marked. */
static void stuck_hull_add(uint32_t a, double now)
{
    int j = -1, old = 0;
    for (int i = 0; i < 32; i++) { if (g_refused[i].actor == a) j = i; if (g_refused[i].until < g_refused[old].until) old = i; }
    if (j < 0) { j = old; g_refused[j].actor = a; }
    g_refused[j].fails = 2; g_refused[j].until = now + 600.0;
}
/* Test hooks: keep every bot off a hull for a while; make this bot leave its vehicle now. */
void ma2_brain_test_refuse(uint32_t a, double now, double secs)
{
    refused_add(a, now);
    for (int i = 0; i < 32; i++) if (g_refused[i].actor == a) g_refused[i].until = now + secs;
}
void ma2_brain_test_bail(Ma2Brain *b, double secs) { if (b) { b->bail_until = b->now + secs; b->next_eval = 0.0; } }
/* RECOMP_TEST_HUMAN_FERRY: post a lift for the human (only bot brains post them), or drop it once he hangs on
 * (a rider with no request is the player who clawed on: order_lift takes him to their flag, ctf 2). */
void ma2_brain_test_lift(uint32_t ba, int nation, Ma2Vec3 pos, Ma2Vec3 dest, double now, int clear)
{
    if (!clear) { lift_post(ba, nation, pos, dest, now); return; }
    int i = lift_find(ba);
    if (i >= 0) { g_lift[i].ba = 0; g_lift[i].vtol = 0; }
}
/* A new map: what the roster learned about the last one (refusing vehicles, taken power-ups,
 * lift requests) is keyed by actor addresses and match time, both of which start over. */
void ma2_bot_brain_reset(void)
{
    memset(g_refused, 0, sizeof g_refused);
    g_ba_fail = g_ba_ok = 0;
    memset(g_pu_taken, 0, sizeof g_pu_taken);
    memset(g_lift, 0, sizeof g_lift);
    memset(g_muster, 0, sizeof g_muster);
    memset(g_combo, 0, sizeof g_combo);
    memset(g_frun, 0, sizeof g_frun);
    memset(g_hurt, 0, sizeof g_hurt);
    memset(g_frun_want, 0, sizeof g_frun_want); memset(g_frun_pull, 0, sizeof g_frun_pull);
    memset(g_bapick, 0, sizeof g_bapick);
}
static void no_board_refresh(Ma2Brain *b, const Ma2World *w)
{
    int i;
    for (i = 0; i < b->n_no_board; i++) {
        int vi = find_vehicle(w, b->no_board[i]);
        if (vi < 0 || w->vehicles[vi].occupied || b->now >= b->no_board_until[i]) {
            b->n_no_board--;
            b->no_board[i] = b->no_board[b->n_no_board]; b->no_board_until[i] = b->no_board_until[b->n_no_board]; i--;
        }
    }
}

/* A human teammate on foot is going for this 'Mech (core: human_mech_intent, C2): leave it to
 * him, however much closer we are -- unless we are already climbing in (the game offers it to us).
 * Enemy bots still race him for it; between bots the claims below decide as before. */
static int yields_to_human(const Ma2PlayerView *me, const Ma2VehicleView *v)
{
    return v->human_claim && v->human_claim_nation == me->nation && me->enter_target != v->actor;
}

/* Usable: free, alive, an active entry zone, a kind we can drive, not refused before, not
 * claimed by another bot of our team (FFA: by anyone) -- vehicles are reserved, never raced for. */
/* DyingRide memory: a parked 'Mech's view reads hp 1/1 (r1_81: every 'Mech hull), so its health is the one last seen while someone drove it
 * (players[].hp of that actor). Refreshed from every brain tick; stale after 60 s. */
static struct { uint32_t a; float f; double t; } g_vhp[64];
static void vhp_note(const Ma2World *w, double now)
{
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->alive || !p->actor || p->kind == MA2_KIND_PILOT || p->hp_max <= 0.0f) continue;
        int s = -1, old = 0;
        for (int j = 0; j < 64; j++) { if (g_vhp[j].a == p->actor) { s = j; break; } if (g_vhp[j].t < g_vhp[old].t) old = j; }
        if (s < 0) s = old;
        g_vhp[s].a = p->actor; g_vhp[s].f = p->hp / p->hp_max; g_vhp[s].t = now;
    }
}
static float vhp_known(uint32_t a, double now)
{
    for (int j = 0; j < 64; j++) if (g_vhp[j].a == a && now - g_vhp[j].t < 60.0 && now >= g_vhp[j].t) return g_vhp[j].f;
    return -1.0f;
}
static int vehicle_usable(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2VehicleView *v)
{
    if (v->occupied || v->hp <= 0.0f || !v->actor || !v->has_entry) return 0;
    if (yields_to_human(me, v)) return 0;
    if (vehicle_pref_b(b, v->kind) <= 0.0f) return 0;
    if (no_board_has(b, v->actor) || refused_has(v->actor, b->now)) return 0;
    if (v->actor == b->left_veh && b->now < b->left_until) return 0;
    /* DyingRide (default on, RECOMP_BOT_DYING_RIDE / [Bots] DyingRide): a hull under 15 % is no ride -- au19_211: three ejected pilots took their own
     * dying 'Mech again (b4 120.6, b2 171.4, b6 234.7) and died in it within seconds. [C7] dyingride: skipped. */
    if (c7_on("dyingride", "RECOMP_BOT_DYING_RIDE", "DyingRide", 1)) {
        float f = v->hp_max > 0.0f ? v->hp / v->hp_max : 1.0f, k2 = vhp_known(v->actor, b->now);
        if (k2 >= 0.0f && k2 < f) f = k2;                                       /* a parked 'Mech reads 1/1: what it had when last driven */
        if (f < 0.15f) { c7_add("dyingride", "skipped", 1); return 0; }
    }
    /* WP2-C BaEnterBan (default on, RECOMP_BOT_BA_ENTER_BAN / [Bots] BaEnterBan): a pilot on foot gets no entry prompt at a parked BattleArmor on the
     * Last-Man-Standing maps (sweep 11-02: 0 of 13 tries in sw30/33/34 across the standing walks; wiki: 0 of 17 on In Your Face), while tanks and 'Mechs board in 2.5 s.
     * After one such failure this pilot leaves BattleArmor alone for 150 s; after two with no success in the match nobody on foot walks to one. */
    if (v->kind == MA2_KIND_BATTLEARMOR && me->kind == MA2_KIND_PILOT && c7_on("baenterban", "RECOMP_BOT_BA_ENTER_BAN", "BaEnterBan", 1) &&
        (b->now < b->ba_ban_until || (g_ba_fail >= 2 && g_ba_ok == 0))) return 0;
    if (v->claimed_by && v->claimed_by != me->player && (!w->team_mode || v->claim_nation == me->nation)) return 0;
    if (v->nation > 0 && v->nation != 6 && v->nation != me->nation) return 0;    /* the zone refuses another nation */
    for (int i = 0; i < w->nbombs; i++)                                         /* booby-trapped: a bomb that goes off for us beside its door */
        if (w->bombs[i].owner != me->player && (!w->team_mode || w->bombs[i].nation != me->nation) && hdist(w->bombs[i].pos, v->entry) < 17.0f) return 0;
    return 1;
}

/* Value of taking vehicle v from here: its preference and condition, minus the walk and the
 * danger on the way (known enemy armour near it), with this life's wish and a swap target. */
/* F23 (switch RECOMP_BOT_MECH_RATING / [Bots] MechRating, default on): a 'Mech's strength, 0..1 of the
 * strongest 'Mech on the map -- its full armour (an Atlas carries about twice a Puma's; the game's own
 * battle score ranks them the same way). 0 when unknown. */
static float mech_rating(const Ma2World *w, const Ma2VehicleView *v)
{
    float top = 0.0f;
    int j;
    if (v->kind != MA2_KIND_MECH || v->hp_max <= 0.0f) return 0.0f;
    for (j = 0; j < w->nvehicles && j < MA2_BOT_MAX_VEHICLES; j++)
        if (w->vehicles[j].kind == MA2_KIND_MECH && w->vehicles[j].hp_max > top) top = w->vehicles[j].hp_max;
    for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++)
        if (w->players[j].kind == MA2_KIND_MECH && w->players[j].hp_max > top) top = w->players[j].hp_max;
    return top > 0.0f ? clampf(v->hp_max / top, 0.0f, 1.0f) : 0.0f;
}
static int f23_on(void) { return c7_on("f23-rating", "RECOMP_BOT_MECH_RATING", "MechRating", 1); }

/* CtfPair (default on, RECOMP_BOT_CTF_PAIR / [Bots] CtfPair; playtest 11-04): the opening push of a CTF match is planned, not rolled.
 * Everyone spawns by the team's VTOL and BattleArmor, so in the first seconds each team names ONE pilot for the VTOL and ONE
 * for the BattleArmor nearest it (the two nearest, together); every other bot leaves both alone and takes the other strong
 * 'Mechs (the VTOL / BattleArmor bonuses CtfVtolPick, CtfBaPick and the partner ask are off meanwhile). The VTOL starts the run
 * the moment its partner sits in the BattleArmor (no roll, no lone cover); the BattleArmor holds where it is until the VTOL
 * comes down for it. The player counts: in (or heading for) the VTOL, the bot BattleArmor waits for his VTOL and claws on when
 * it hovers low over it (BaBoardPlayer) -- and if he flies off without it (250 m off, 80 m further than his nearest, or 30 s)
 * it lets the wait go and plays on; in the BattleArmor, the bot VTOL holds for him (VtolMuster, 25 s) and he decides.
 * Only the opening: it ends when the run is on, the player is aboard, a role is lost, or 60 s in -- after that the usual
 * emergent picking. Before, a VTOL and a BattleArmor were each a separate bonus, a 0.8 roll and a 15 s partner wait: runs
 * started late or not at all (a playtester's NRC matches 11-04). [C7] ctfpair: assigned / human_vtol / human_ba / run_started / ended. */
static int ctf_pair_on(void) { return c7_on("ctfpair", "RECOMP_BOT_CTF_PAIR", "CtfPair", 1); }
enum { PAIR_NONE = 0, PAIR_ON, PAIR_DONE };
static struct { int state, human; uint32_t vt_veh, ba_veh, vt_pl, ba_pl, hvt; double t, t_upd, hw_t, hc_t; float hw_min; } g_pair[8];
static int pair_active(int n) { return ctf_pair_on() && g_pair[n & 7].state == PAIR_ON; }
static const Ma2PlayerView *player_by_pl(const Ma2World *w, uint32_t pl)
{
    for (int j = 0; pl && j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].player == pl) return &w->players[j];
    return NULL;
}
static const Ma2VehicleView *veh_by_actor(const Ma2World *w, uint32_t a)
{
    for (int i = 0; a && i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES; i++) if (w->vehicles[i].actor == a) return &w->vehicles[i];
    return NULL;
}
static void pair_end(int n, double now, const char *why)
{
    if (g_pair[n].state == PAIR_ON) {
        c7_add("ctfpair", "ended", 1);
        if (diag_on()) fprintf(stderr, "[BOT-PAIR] t=%.1f nation=%d opening pair ends after %.0f s: %s\n", now, n, now - g_pair[n].t, why);
    }
    g_pair[n].state = PAIR_DONE;
}
static int pair_free_veh(const Ma2VehicleView *v, int kind, int n)
{
    return kind_ok(v->kind) == kind && !v->occupied && (v->nation <= 0 || v->nation == 6 || v->nation == n);
}
static void pair_update(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    if (!ctf_pair_on() || w->game_type != MA2_GT_CTF || !w->team_mode || !lifts_on()) return;
    int n = me->nation & 7;
    if (g_pair[n].t_upd > b->now + 1.0) memset(&g_pair[n], 0, sizeof g_pair[n]);   /* a new match: the clock went back */
    if (g_pair[n].t_upd > 0.0 && b->now - g_pair[n].t_upd < 0.25) return;
    g_pair[n].t_upd = b->now;
    if (g_pair[n].state == PAIR_DONE) return;
    const Ma2PlayerView *hu = NULL;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++)
        if (w->players[j].is_human && w->players[j].alive && (w->players[j].nation & 7) == n) { hu = &w->players[j]; break; }
    if (g_pair[n].state == PAIR_NONE) {
        if (b->now > 20.0) { g_pair[n].state = PAIR_DONE; return; }   /* joined late: no opening to plan */
        const Ma2VehicleView *vt = NULL, *ba = NULL; float bv = 120.0f, bb = 150.0f;
        for (int i = 0; i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES; i++) {   /* the free VTOL nearest a teammate on foot */
            const Ma2VehicleView *v = &w->vehicles[i];
            if (!pair_free_veh(v, MA2_KIND_VTOL, n) && !(hu && kind_ok(hu->kind) == MA2_KIND_VTOL && v->actor == hu->actor)) continue;
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *q = &w->players[j];
                if (!q->alive || (q->nation & 7) != n || (kind_ok(q->kind) != MA2_KIND_PILOT && q->actor != v->actor)) continue;
                float d = hdist(q->pos, v->pos);
                if (d < bv) { bv = d; vt = v; }
            }
        }
        if (vt) for (int i = 0; i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES; i++) {   /* the BattleArmor nearest it */
            const Ma2VehicleView *v = &w->vehicles[i];
            if (!pair_free_veh(v, MA2_KIND_BATTLEARMOR, n) && !(hu && kind_ok(hu->kind) == MA2_KIND_BATTLEARMOR && v->actor == hu->actor)) continue;
            float d = hdist(v->pos, vt->pos);
            if (d < bb) { bb = d; ba = v; }
        }
        if (!vt || !ba) {
            if (b->now < 8.0) return;   /* the world is still filling in (vm2 11-04: no vehicles at t=0.0) -- look again */
            g_pair[n].state = PAIR_DONE;
            if (diag_on()) fprintf(stderr, "[BOT-PAIR] t=%.1f nation=%d no opening pair: no free %s by the spawn\n", b->now, n, vt ? "BattleArmor" : "VTOL");
            return;
        }
        int human = 0;
        if (hu) {
            if (hu->actor == vt->actor || vt->human_claim == hu->player) human = 1;
            else if (hu->actor == ba->actor || ba->human_claim == hu->player) human = 2;
        }
        /* the two bots on foot that get there soonest together (the later of the two arrivals, then the sum) */
        uint32_t pv = 0, pb = 0; float bc = 1e9f;
        for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
            const Ma2PlayerView *qi = &w->players[i];
            if (!qi->is_bot || !qi->alive || (qi->nation & 7) != n || kind_ok(qi->kind) != MA2_KIND_PILOT) continue;
            float di = hdist(qi->pos, vt->pos), ei = hdist(qi->pos, ba->pos);
            if (human == 1 && ei < bc) { bc = ei; pb = qi->player; }
            if (human == 2 && di < bc) { bc = di; pv = qi->player; }
            if (human) continue;
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *qj = &w->players[j];
                if (j == i || !qj->is_bot || !qj->alive || (qj->nation & 7) != n || kind_ok(qj->kind) != MA2_KIND_PILOT) continue;
                float ej = hdist(qj->pos, ba->pos), cost = maxf(di, ej) * 2.0f + di + ej;
                if (cost < bc) { bc = cost; pv = qi->player; pb = qj->player; }
            }
        }
        if ((human != 1 && !pv) || (human != 2 && !pb)) {
            if (b->now < 8.0) return;
            g_pair[n].state = PAIR_DONE;
            if (diag_on()) fprintf(stderr, "[BOT-PAIR] t=%.1f nation=%d no opening pair: too few bots on foot\n", b->now, n);
            return;
        }
        g_pair[n].state = PAIR_ON; g_pair[n].human = human; g_pair[n].t = b->now;
        g_pair[n].vt_veh = vt->actor; g_pair[n].ba_veh = ba->actor; g_pair[n].vt_pl = pv; g_pair[n].ba_pl = pb;
        g_pair[n].hvt = human == 1 && hu && hu->actor == vt->actor ? hu->actor : 0; g_pair[n].hw_t = 0.0; g_pair[n].hc_t = b->now;
        c7_add("ctfpair", "assigned", 1);
        if (human) c7_add("ctfpair", human == 1 ? "human_vtol" : "human_ba", 1);
        if (diag_on()) fprintf(stderr, "[BOT-PAIR] t=%.1f nation=%d opening pair: VTOL %08X -> %s %08X, BattleArmor %08X (%.0f m from it) -> %s %08X\n", b->now, n,
                               vt->actor, human == 1 ? "the player" : "bot", human == 1 ? hu->player : pv, ba->actor, bb, human == 2 ? "the player" : "bot", human == 2 ? hu->player : pb);
        return;
    }
    /* PAIR_ON */
    if (b->now - g_pair[n].t > 60.0 || b->now < g_pair[n].t) { pair_end(n, b->now, "the opening is over"); return; }
    const Ma2PlayerView *pv = player_by_pl(w, g_pair[n].vt_pl), *pb = player_by_pl(w, g_pair[n].ba_pl);
    const Ma2VehicleView *vv = veh_by_actor(w, g_pair[n].vt_veh), *bv = veh_by_actor(w, g_pair[n].ba_veh);
    if ((g_pair[n].vt_pl && (!pv || !pv->alive)) || (g_pair[n].ba_pl && (!pb || !pb->alive))) { pair_end(n, b->now, "a paired bot died"); return; }
    int pv_in = pv && kind_ok(pv->kind) == MA2_KIND_VTOL, pb_in = pb && kind_ok(pb->kind) == MA2_KIND_BATTLEARMOR;
    /* a paired bot in some other ride has left its role (vo2 11-04: the BattleArmor pilot's route circled the suit 5 m away for 8 s, it
     * took a 'Mech at t=15, and its VTOL waited on the pair until 57 s) -- end now, the usual partner logic takes over */
    if ((pv && !pv_in && kind_ok(pv->kind) != MA2_KIND_PILOT) || (pb && !pb_in && kind_ok(pb->kind) != MA2_KIND_PILOT)) { pair_end(n, b->now, "a paired bot took another ride"); return; }
    /* the player takes a role (climbs in, or heads for it, before its bot is in) */
    if (hu && !g_pair[n].human) {
        int hk = kind_ok(hu->kind);
        if (!pv_in && (hk == MA2_KIND_VTOL || (hk == MA2_KIND_PILOT && vv && vv->human_claim == hu->player))) {
            g_pair[n].human = 1; g_pair[n].vt_pl = 0; pv = NULL; g_pair[n].hc_t = b->now; c7_add("ctfpair", "human_vtol", 1);
            if (diag_on()) fprintf(stderr, "[BOT-PAIR] t=%.1f nation=%d the player takes the VTOL role: the bot pilot takes a 'Mech\n", b->now, n);
        } else if (!pb_in && (hk == MA2_KIND_BATTLEARMOR || (hk == MA2_KIND_PILOT && bv && bv->human_claim == hu->player))) {
            g_pair[n].human = 2; g_pair[n].ba_pl = 0; pb = NULL; g_pair[n].hc_t = b->now; c7_add("ctfpair", "human_ba", 1);
            if (diag_on()) fprintf(stderr, "[BOT-PAIR] t=%.1f nation=%d the player takes the BattleArmor role: the bot pilot takes a 'Mech\n", b->now, n);
        }
    }
    if (g_pair[n].human) {
        int want = g_pair[n].human == 1 ? MA2_KIND_VTOL : MA2_KIND_BATTLEARMOR, hk = hu ? kind_ok(hu->kind) : 0;
        if (!hu) { pair_end(n, b->now, "the player died"); return; }
        if (hk == want) { g_pair[n].hc_t = b->now; if (want == MA2_KIND_VTOL) g_pair[n].hvt = hu->actor; }
        else if (b->now - g_pair[n].hc_t > (hk == MA2_KIND_PILOT ? 10.0 : 0.0)) { pair_end(n, b->now, "the player went his own way"); return; }
    }
    /* a paired vehicle taken by somebody else before its bot got in */
    if (g_pair[n].vt_pl && !pv_in && (!vv || (vv->occupied && !(hu && hu->actor == vv->actor)))) { pair_end(n, b->now, "the VTOL was taken"); return; }
    if (g_pair[n].ba_pl && !pb_in && (!bv || (bv->occupied && !(hu && hu->actor == bv->actor)))) { pair_end(n, b->now, "the BattleArmor was taken"); return; }
    if (pv_in && g_frun[n].phase && g_frun[n].phase != FR_COVER && g_frun[n].vtol == pv->actor) { c7_add("ctfpair", "run_started", 1); pair_end(n, b->now, "the run is on"); return; }
    if (g_pair[n].human == 2 && pv_in && hu->riding == pv->actor) { c7_add("ctfpair", "run_started", 1); pair_end(n, b->now, "the player is on the VTOL"); return; }
    if (g_pair[n].human == 1 && pb_in) {   /* the bot BattleArmor waits for the player's VTOL -- not for ever */
        const Ma2PlayerView *hv = player_by_actor(w, g_pair[n].hvt);
        if (pb->riding && hv && pb->riding == hv->actor) { c7_add("ctfpair", "run_started", 1); pair_end(n, b->now, "the BattleArmor is on the player's VTOL"); return; }
        if (hv && hv->alive) {
            float d = hdist(hv->pos, pb->pos);
            if (g_pair[n].hw_t <= 0.0) { g_pair[n].hw_t = b->now; g_pair[n].hw_min = d; }
            g_pair[n].hw_min = minf(g_pair[n].hw_min, d);
            if (d > 250.0f || d > g_pair[n].hw_min + 80.0f || b->now - g_pair[n].hw_t > 30.0) { c7_add("ctfpair", "player_flew_off", 1); pair_end(n, b->now, "the player flew off without the BattleArmor"); return; }
        }
    }
    if (g_pair[n].human == 2 && pv_in && hu && hdist(hu->pos, pv->pos) > 120.0f && b->now - g_pair[n].hc_t < 1.0 && kind_ok(hu->kind) == MA2_KIND_BATTLEARMOR &&
        b->now - g_pair[n].t > 10.0) { pair_end(n, b->now, "the player's BattleArmor walked off"); return; }
}
/* CtfPair: the vehicle steer (the paired bot to its vehicle; everyone else off both) */
static float pair_veh_value(const Ma2World *w, const Ma2PlayerView *me, const Ma2VehicleView *v)
{
    int n = me->nation & 7;
    (void)w;
    if (!pair_active(n) || me->kind != MA2_KIND_PILOT) return 0.0f;
    if (v->actor == g_pair[n].vt_veh) return me->player == g_pair[n].vt_pl ? 6.0f : -6.0f;
    if (v->actor == g_pair[n].ba_veh) return me->player == g_pair[n].ba_pl ? 6.0f : -6.0f;
    if (me->player == g_pair[n].vt_pl || me->player == g_pair[n].ba_pl) return -3.0f;   /* not another ride on the way */
    return 0.0f;
}

/* LoneGround (default on, RECOMP_BOT_LONE_GROUND / [Bots] LoneGround; design 11-14): a VTOL cannot take a flag. A bot with nobody on its team to
 * fly in (no other bot, no teammate player in a BattleArmor) took the VTOL anyway (CtfVtolPick's pull 1.1-2.2 + the 'no gunship up' 0.4) and
 * flew lone cover over their flag for nobody: 1v1 CTF Bogged Down on rc33 (c34bd1v), both bots, 0 flag grabs in 240 s. Alone, those
 * two bonuses and the lone cover are off: it takes a ground ride and goes for the flag itself. [C7] loneground: vtol_pick_skipped, cover_skipped. */
static int lone_ground_on(void) { return c7_on("loneground", "RECOMP_BOT_LONE_GROUND", "LoneGround", 1); }
static int ctf_vtol_partner(const Ma2World *w, const Ma2PlayerView *me)   /* 1 = someone on our team a VTOL can fly in */
{
    if (!lone_ground_on()) return 1;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
        const Ma2PlayerView *q = &w->players[j];
        if (q->player == me->player || q->nation != me->nation) continue;
        if (q->is_bot || (q->is_human && q->alive && kind_ok(q->kind) == MA2_KIND_BATTLEARMOR)) return 1;
    }
    return 0;
}
/* LoneGround: a teammate on the ground who could take their flag while a VTOL covers it (a bot not in a VTOL; a player in a ground ride) */
static int ctf_ground_mate(const Ma2World *w, const Ma2PlayerView *me)
{
    if (!lone_ground_on()) return 1;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
        const Ma2PlayerView *q = &w->players[j];
        int k = kind_ok(q->kind);
        if (q->player == me->player || q->nation != me->nation || !q->alive || k == MA2_KIND_VTOL) continue;
        if (q->is_bot || k != MA2_KIND_PILOT) return 1;
    }
    return 0;
}
static float vehicle_value(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2VehicleView *v)
{
    int j;
    float d = hdist(me->pos, v->pos);
    float s = vehicle_pref_b(b, v->kind) - d / 400.0f - (1.0f - hp_frac(v->hp, v->hp_max)) * 0.4f;
    /* WP2-C FootNear (default on, RECOMP_BOT_FOOT_NEAR / [Bots] FootNear): a walk past 120 m costs more (a pilot is a free kill on the way, and the ride is
     * often taken before it arrives: sweep 11-02, 45 foot spells of 30-200 s, LMS cells worst): the nearest usable ride wins unless a better one is close behind */
    if (ground_cut(me, v->has_entry ? v->entry : v->pos)) { s -= 0.6f; c7_add("groundreach", "veh_penalised", 1); }
    else if (reach_cut(b, me, v->has_entry ? v->entry : v->pos)) { s -= 0.6f; c7_add("reachset", "veh_penalised", 1); }   /* GroundReach: another region, so a same-ground ride first */
    if (me->kind == MA2_KIND_PILOT && d > 120.0f && c7_on("footnear", "RECOMP_BOT_FOOT_NEAR", "FootNear", 1)) s -= (d - 120.0f) / 300.0f;
    if (v->kind == MA2_KIND_MECH && f23_on()) {   /* F23: an Atlas over a nearer Puma -- an Elite walks ~200 m further for it, a Recruit ~80 m */
        static const float rate_w[4] = { 0.35f, 0.55f, 0.75f, 0.9f };
        s += rate_w[b->skill & 3] * mech_rating(w, v);
    }
    s += pair_veh_value(w, me, v);   /* CtfPair: the opening's two roles; the bonuses below are off meanwhile */
    if (v->kind == MA2_KIND_BATTLEARMOR && w->game_type == MA2_GT_CTF && ctf_ba_pick_on() && !pair_active(me->nation)) {
        /* CtfBaPick: a team with a VTOL up and no BattleArmor sends ONE pilot near a free BattleArmor
         * into it, so the VTOL+BattleArmor flag run has a partner (bots almost never chose one: BA 0.30-0.45
         * against a 'Mech's 1.0 + rating). A better pilot plays the team game more (decisions only). The
         * partner ask from a VTOL (g_frun_want) gets the same bonus over 350 m. */
        static const float bonus0[4] = { 0.6f, 0.9f, 1.2f, 1.4f };
        /* PartnerPull (default on, RECOMP_BOT_PARTNER_PULL / [Bots] PartnerPull): the partner ask outbids a 'Mech -- with the
         * bonus above a BattleArmor still lost to an Atlas (0.35 + 1.4 against 1.0 + 0.9) and no bot ever took one: 0 bot
         * VTOL+BattleArmor runs in two laptop matches and a test run (10-31; user: bots should run it as readily as a player) */
        static const float pull[4] = { 1.1f, 1.5f, 1.9f, 2.2f };
        const float *bonus = c7_on("partnerpull", "RECOMP_BOT_PARTNER_PULL", "PartnerPull", 1) ? pull : bonus0;
        int n = me->nation & 7, vt = 0, ba = 0, mates = 0;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Ma2PlayerView *q = &w->players[j];
            if (!q->alive || q->nation != me->nation) continue;
            mates++;
            if (kind_ok(q->kind) == MA2_KIND_VTOL) vt = 1;
            if (kind_ok(q->kind) == MA2_KIND_BATTLEARMOR && q->player != me->player) ba = 1;
        }
        if (g_bapick[n].until - b->now > 61.0) g_bapick[n].until = 0.0;   /* a new match: the clock went back (set at most 60 s ahead) */
        int mine = g_bapick[n].until <= b->now || g_bapick[n].who == me->player;
        /* one pilot answers the ask: six switched to BattleArmor at once and three switched back (laptop 11-01) */
        if (g_frun_want[n] > b->now && d < 350.0f && (mine || !c7_on("partnerone", "RECOMP_BOT_PARTNER_ONE", "PartnerOne", 1))) {
            s += bonus[b->skill & 3];
            if (c7_on("partnerone", "RECOMP_BOT_PARTNER_ONE", "PartnerOne", 1)) { g_bapick[n].who = me->player; g_bapick[n].until = b->now + 20.0; }
        }
        else if (vt && !ba && mates >= 3 && d < 200.0f && mine) {
            s += bonus[b->skill & 3];
            if (g_bapick[n].who != me->player || g_bapick[n].until <= b->now) c7_add("bapick", k_c7_bapick_hit[b->skill & 3], 1);
            g_bapick[n].who = me->player; g_bapick[n].until = b->now + 60.0;
        }
    } else if (v->kind == MA2_KIND_BATTLEARMOR && w->game_type == MA2_GT_CTF && g_frun_want[me->nation & 7] > b->now && d < 250.0f && !pair_active(me->nation)) s += 0.8f;   /* a VTOL wants a flag-run partner */
    /* CtfVtolPick (default on, RECOMP_BOT_CTF_VTOL_PICK / [Bots] CtfVtolPick): the flag run starts only from a bot flying a VTOL,
     * and bots almost never took one (VTOL 0.55-0.70 against a 'Mech's 1.0 + rating): an enemy bot was in a VTOL in 1 of the
     * a playtester's 4 NRC CTF matches (11-03/04), and no bot run started in any (playtest 11-04: bots should use the VTOL strategy too;
     * the flag-run tests forced a VTOL by test hook). Now a team with no bot in a VTOL sends ONE pilot within 300 m of a free
     * VTOL into it (the PartnerPull bonus); CtfBaPick then finds it a BattleArmor partner. [C7] vtpick: picks_<skill>. */
    int lone_vt = v->kind == MA2_KIND_VTOL && w->game_type == MA2_GT_CTF && w->team_mode && !ctf_vtol_partner(w, me);   /* LoneGround */
    if (lone_vt && me->kind == MA2_KIND_PILOT && d < 300.0f) c7_add("loneground", "vtol_pick_skipped", 1);
    if (v->kind == MA2_KIND_VTOL && w->game_type == MA2_GT_CTF && me->kind == MA2_KIND_PILOT && d < 300.0f && !pair_active(me->nation) && !lone_vt &&
        c7_on("vtpick", "RECOMP_BOT_CTF_VTOL_PICK", "CtfVtolPick", 1)) {
        static const float pull[4] = { 1.1f, 1.5f, 1.9f, 2.2f };
        static const char *const k_vp[4] = { "picks_recruit", "picks_regular", "picks_veteran", "picks_elite" };
        int n = me->nation & 7, botvt = 0;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Ma2PlayerView *q = &w->players[j];
            if (q->alive && q->is_bot && q->nation == me->nation && q->player != me->player && kind_ok(q->kind) == MA2_KIND_VTOL) botvt = 1;
        }
        if (g_vtpick[n].until - b->now > 31.0) g_vtpick[n].until = 0.0;   /* a new match: the clock went back (it is set 30 s ahead) */
        int mine = g_vtpick[n].until <= b->now || g_vtpick[n].who == me->player;
        if (!botvt && mine) {
            s += pull[b->skill & 3];
            if (g_vtpick[n].who != me->player || g_vtpick[n].until <= b->now) c7_add("vtpick", k_vp[b->skill & 3], 1);
            g_vtpick[n].who = me->player; g_vtpick[n].until = b->now + 30.0;
        }
    }
    if (v->kind == MA2_KIND_BATTLEARMOR && me->kind == MA2_KIND_PILOT && d < 300.0f && snatch_deck(w, me)) {   /* SnatchDeck: only a BattleArmor can take the cores */
        static const float deck[4] = { 0.7f, 0.9f, 1.1f, 1.3f };
        s += deck[b->skill & 3]; c7_add("snatchdeck", "ba_bonus", 1);
    }
    for (j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
        const Mem *m = &b->mem[j];
        if (m->valid && mem_conf(b, m) > 0.3f && (m->kind == MA2_KIND_MECH || m->kind == MA2_KIND_TANK || m->kind == MA2_KIND_VTOL) &&
            hdist(m->pos, v->pos) < 60.0f) s -= 0.5f;
    }
    for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {     /* a teammate on foot is closer to it */
        const Ma2PlayerView *p = &w->players[j];
        if (p->player == me->player || !p->alive || p->kind != MA2_KIND_PILOT) continue;
        if (w->team_mode && p->nation != me->nation) continue;
        if (hdist(p->pos, v->pos) + 3.0f < d) { s -= 0.25f; break; }
    }
    if (d < 25.0f) s += 0.3f * vehicle_pref_b(b, v->kind);   /* right here: take it */
    if (v->kind == MA2_KIND_VTOL && d < 80.0f && !lone_vt) {
        /* A team wants one gunship up: the first bot near a free VTOL while no teammate flies one
         * takes it (unless a 'Mech is just as close). */
        int flying = 0;
        for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Ma2PlayerView *p = &w->players[j];
            if (p->player != me->player && p->alive && p->kind == MA2_KIND_VTOL && (!w->team_mode || p->nation == me->nation)) flying = 1;
        }
        if (!flying) s += 0.4f;
    }
    if (v->actor == b->veh) s += 0.2f;
    if (v->actor == b->claim_actor && b->now < b->post_hack_until + 20.0) s += 1.5f;
    if (v->actor == b->swap_veh && b->now < b->swap_until) s += 2.0f;
    if (v->kind == b->fav_kind) s += getenv("RECOMP_BOT_FAV_KIND") || getenv("RECOMP_BOT_FAV_KINDS") ? 5.0f : 0.25f;
    return s;
}

static int pick_vehicle(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *score_out)
{
    int i, best = -1;
    float bs = -1e9f;
    int nv = w->nvehicles < MA2_BOT_MAX_VEHICLES ? w->nvehicles : MA2_BOT_MAX_VEHICLES;
    for (i = 0; i < nv; i++) {
        const Ma2VehicleView *v = &w->vehicles[i];
        float s;
        if (!vehicle_usable(b, w, me, v)) continue;
        if (hdist(me->pos, v->pos) > (b->veh_wide ? 600.0f : 300.0f)) continue;   /* not a walk across the map for a ride (F14: unless all we face is pilots) */
        /* C3: the ride we were going for went to a teammate who gets there sooner: another only if it
         * is not much further (a long walk to a far one wedged a pilot in a hole on an editor map) */
        if (b->now < b->c3_lost_until && hdist(me->pos, v->pos) > b->c3_lost_d + 60.0f) continue;
        s = vehicle_value(b, w, me, v);
        if (s > bs) { bs = s; best = i; }
    }
    if (best >= 0 && w->vehicles[best].kind == MA2_KIND_MECH && f23_on()) {
        const Ma2VehicleView *bv = &w->vehicles[best];
        for (i = 0; i < nv; i++) {
            const Ma2VehicleView *v = &w->vehicles[i];
            if (i == best || v->kind != MA2_KIND_MECH || !vehicle_usable(b, w, me, v)) continue;
            if (hdist(me->pos, v->pos) + 20.0f < hdist(me->pos, bv->pos) && v->hp_max < bv->hp_max * 0.9f) {
                static uint32_t counted[64];   /* once per bot and 'Mech: pick_vehicle runs every evaluation */
                if (counted[b->index & 63] != bv->actor) { counted[b->index & 63] = bv->actor; c7_add("f23-rating", "stronger_picks", 1); c7_add("f23-rating", k_c7_f23_sk[b->skill & 3], 1); }
                break;
            }
        }
    }
    if (score_out) *score_out = bs;
    return best;
}

/* F8/F9: our own team's nearest pad (FFA: one with no one near it), leaving out the pads that did
 * not heal us lately (ma2_bot_pad_pick). */
static int pad_usable(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 *out, int *idx)
{
    unsigned skip = 0;
    for (int i = 0; i < 8; i++) if (b->now < b->pad_bad_until[i]) skip |= 1u << i;
    return ma2_bot_pad_pick(w, me, b->home_set ? b->home_pos : me->pos, skip, out, idx);
}

/* A free vehicle clearly better than the one we are in, close by and not under the enemy's guns. */
static int pick_swap(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    int i, best = -1, j;
    int k = kind_ok(me->kind);
    float mine = vehicle_pref_b(b, k) * (0.3f + 0.7f * hp_frac(me->hp, me->hp_max)), bs = mine + 0.25f;
    int ba = k == MA2_KIND_BATTLEARMOR;
    /* A gunship sets down with the descend control held (RS click / Ctrl) and its pilot gets out; an
     * abandoned VTOL respawns at its base. Where the map has helipads the pad's repair (A) beats a
     * change of ride, so a VTOL swaps only where there is none. */
    /* F1 (switch SwapCool): a swap just done or given up waits 20 s / 10 s before the next, and an
     * Elite keeps its 'Mech until it is near death (20 %, not 35 %). F12 (GrinderSwap): BattleArmor in
     * Grinder takes a 'Mech up to 150 m off, under fire or not -- the 'Mech is what wins Grinder. */
    int cool = ma2_bot_switch_on(MA2_SW_SWAP_COOL), grind = ba && w->game_type == MA2_GT_GRINDER && ma2_bot_switch_on(MA2_SW_GRINDER_SWAP);
    float hpf = hp_frac(me->hp, me->hp_max);
    if (k == MA2_KIND_VTOL && (ma2_bot_vtol_pads_on() ? pad_usable(b, w, me, NULL, NULL) : ma2_bot_pad_near(me->pos, NULL))) return -1;   /* the helipad repairs it instead */
    if (!ba && hpf > 0.35f) return -1;
    /* PairKeep (default on, RECOMP_BOT_PAIR_KEEP / [Bots] PairKeep): the CtfPair's BattleArmor keeps its suit while the pair is on.
     * When its VTOL came within reach the hold let go and the swap (1.25: a free 'Mech 40 m off, nobody shooting) outbid the ride, so
     * the paired BattleArmor ran off up a ramp, stood there, and ran back (playtest rec 2026-11-04 bot 4, t=7.0-16.9). */
    if (ba && pair_active(me->nation) && me->player == g_pair[me->nation & 7].ba_pl && c7_on("pairkeep", "RECOMP_BOT_PAIR_KEEP", "PairKeep", 1)) {
        c7_add("pairkeep", "swaps_held", 1);
        return -1;
    }
    if (cool && k == MA2_KIND_MECH && (b->skill & 3) == 3 && hpf > 0.20f) {
        if (b->c3_elite_actor != me->actor) { b->c3_elite_actor = me->actor; ma2_c3_count(MA2_C3_ELITE_KEEPS); }
        return -1;
    }
    if (cool && b->now < b->swap_cool_until && b->goal != G_SWAP) return -1;
    for (i = 0; i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES; i++) {
        const Ma2VehicleView *v = &w->vehicles[i];
        float d = hdist(me->pos, v->pos), val, enemy_near = 0.0f;
        if (!vehicle_usable(b, w, me, v) || d > (grind ? 150.0f : 70.0f) || v->actor == me->actor) continue;
        if (ba && v->kind == MA2_KIND_BATTLEARMOR) continue;
        val = vehicle_pref_b(b, v->kind) * (0.3f + 0.7f * hp_frac(v->hp, v->hp_max)) - d / 300.0f;
        for (j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Mem *m = &b->mem[j];
            if (m->valid && mem_conf(b, m) > 0.4f && k_kind[m->kind].dps >= 15.0f && hdist(m->pos, v->pos) < 50.0f) enemy_near += 1.0f;
        }
        if (enemy_near > 0.0f && !grind) continue;
        val -= 0.3f * enemy_near;
        if (v->actor == b->claim_actor && b->now < b->post_hack_until + 20.0) val += 1.0f;   /* the 'Mech we just hacked free */
        if (val > bs) { bs = val; best = i; }
    }
    return best;
}

/* ======================================================================
 * NeuroHack answers and defence
 * ====================================================================== */
static uint8_t qte_control(Ma2Brain *b, const Ma2PlayerView *me)
{
    QteState *q = &b->qte;
    uint8_t req, press = 0;
    float win;
    if (!me->qte_active) { q->pending = 0; q->last_req = 0; q->last_window = 0.0f; return 0; }
    req = (uint8_t)(me->qte_required & 0x7F);
    win = me->qte_window;
    if (req && (req != q->last_req || win > q->last_window + 0.05f)) {
        float wwin = win > 0.05f ? win : MA2B_QTE_NOMINAL_WINDOW;
        float delay = b->sk.qte_frac * wwin * rng_range(&b->rng, 0.9f, 1.1f);
        float latest = wwin - 0.08f - 2.0f * b->dt;
        q->win_unknown = win <= 0.05f;
        q->pending = 1;
        q->mask = req;
        q->miss = rng_f(&b->rng) < b->sk.qte_miss;
        {   /* test hooks: RECOMP_BOT_QTE_MISS_BA / _MECH = miss probability override */
            const char *e = getenv(me->kind == MA2_KIND_BATTLEARMOR ? "RECOMP_BOT_QTE_MISS_BA" : "RECOMP_BOT_QTE_MISS_MECH");
            if (e && *e) q->miss = rng_f(&b->rng) < (float)atof(e);
        }
        q->press_at = b->now + clampf(delay, b->dt, maxf(b->dt, latest));
        q->cues++;
    }
    if (!req) q->pending = 0;
    if (q->pending && b->now >= q->press_at) {
        q->pending = 0;
        if (q->miss) q->missed++;
        else if (win > 0.0f || q->win_unknown) { press = (uint8_t)(q->mask & ~me->qte_pressed & 0x7F); if (press) q->answered++; }
        else q->missed++;
    }
    q->last_req = req;
    q->last_window = win;
    return press;
}

/* A 'Mech with a BattleArmor closing in pops its defensive system before the clamp. */
/* Alpha Strike, both sides of it.
 * Ours: every weapon at once after a long charge that leaves the 'Mech unable to shoot (and glowing
 * for everyone to see), then an overheat with a long cool-down. So only with a clear shot at a
 * target worth it, in reach, while nobody much is shooting at us -- the skill decides how often
 * the chance is taken and whether a heavy hit mid-charge calls it off -- and afterwards into cover
 * until the heat is down.
 * Theirs: a charging enemy 'Mech in sight that has us in its sights. Close and healthy: it cannot
 * shoot back while it charges -- go at it; otherwise get out of its line before it lets go. Either
 * way, once it has fired it is overheated: finish it. */
static int f42_on(void);
static void alpha_logic(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *out)
{
    static const float take[4] = { 0.20f, 0.45f, 0.70f, 0.90f }, notice[4] = { 0.15f, 0.45f, 0.80f, 0.95f };
    int k = kind_ok(me->kind), sk = b->skill & 3;
    /* ---- theirs ---- */
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->alive || p->kind != MA2_KIND_MECH || !is_enemy(w, me, p)) continue;
        Mem *m = mem_by_actor(b, p->actor);
        if (!m || !m->visible) continue;
        float d = hdist(me->pos, p->pos);
        if (p->alpha_charge > 0 && d < 320.0f && (p->attacking == me->actor || d < 90.0f) && b->alpha_seen != p->actor) {
            b->alpha_seen = p->actor;
            if (rng_f(&b->rng) < notice[sk]) {
                b->st_alpha_react++;
                if (d < 85.0f && hp_frac(me->hp, me->hp_max) > 0.45f && (k == MA2_KIND_MECH || k == MA2_KIND_BATTLEARMOR || k == MA2_KIND_TANK)) {
                    b->alpha_punish = p->actor; b->alpha_punish_until = b->now + 8.0;
                } else { b->alpha_react_until = b->now + 4.0 + 2.0 * rng_f(&b->rng); b->interrupt = 1; b->next_eval = 0.0; }
                if (diag_on()) fprintf(stderr, "[BOT-ALPHA] t=%.1f bot=%d sees %08X charging (%d%%) d=%.0f -> %s\n", b->now, b->index, p->actor, p->alpha_charge, d,
                                       b->alpha_punish == p->actor ? "rush it" : "break line");
            }
        }
        if (p->alpha_charge == 0 && b->alpha_seen == p->actor && p->heat >= 90.0f && sk >= MA2_SKILL_REGULAR) {
            b->alpha_punish = p->actor; b->alpha_punish_until = b->now + 6.0; b->alpha_react_until = 0.0;   /* it has fired: overheated */
        }
    }
    if (b->alpha_punish && b->now < b->alpha_punish_until && b->now >= b->alpha_react_until) {
        Mem *m = mem_by_actor(b, b->alpha_punish);
        if (m && m->valid && mem_engageable(b, m) && out->type != MA2_ORD_GET_VEHICLE && !out->hold_fire) {
            b->tgt = m; b->target = m->actor; out->target = m->actor;
            if (out->type == MA2_ORD_TRAVEL) out->fire = 1;
        }
    } else b->alpha_punish = 0;
    /* ---- ours ---- */
    if (k != MA2_KIND_MECH) { b->alpha_on = 0; return; }
    if (me->alpha_cap) {
        if (b->f42_actor == me->actor && me->alpha_short < b->f42_short) {
            c7_add("F42", "lvl_up", b->f42_short - me->alpha_short);
            if (diag_on()) fprintf(stderr, "[BOT-ALPHA] t=%.1f bot=%d upgrade: %d level(s) short of Alpha (was %d)\n", b->now, b->index, me->alpha_short, b->f42_short);
        }
        b->f42_actor = me->actor; b->f42_short = me->alpha_short;
        if (me->alpha_ready) c7_add("F42", "ready", 1);
    }
    if (b->alpha_cool_until > b->now && me->heat > 60.0f && b->alpha_cool_until < b->alpha_fired_t + 25.0)
        b->alpha_cool_until = b->now + 1.0;                     /* still too hot to fight: stay in cover */
    if (b->alpha_on) {
        Mem *t = mem_by_actor(b, b->alpha_tgt);
        int vis = t && t->valid && t->visible;
        if (!vis && b->tgt && b->tgt->visible && mem_engageable(b, b->tgt) && hdist(me->pos, b->tgt->pos) < my_reach(me)) {
            t = b->tgt; b->alpha_tgt = t->actor; vis = 1;           /* that one went behind something: another in sight takes it */
        }
        if (me->alpha_charge >= 100 && vis) {                       /* full: let go -- it fires */
            b->alpha_on = 0; b->st_alpha++; c7_add("F42", "fires", 1); b->alpha_cool_until = b->now + 9.0; b->alpha_next = b->now + 20.0; b->alpha_fired_t = b->now;
            if (diag_on()) fprintf(stderr, "[BOT-ALPHA] t=%.1f bot=%d fires at %08X after %.1fs heat=%.0f%%\n", b->now, b->index, b->alpha_tgt, b->now - b->alpha_t0, me->heat);
        } else if ((!me->alpha_ready && me->alpha_charge == 0 && b->now - b->alpha_t0 > 1.0) ||   /* a hit took a level: no Alpha now */
                   b->now - b->alpha_t0 > 9.0 ||                                             /* it will not come: give up */
                   (sk >= MA2_SKILL_VETERAN && b->dmg_recent > 0.22f) ||                      /* too hot to stand and glow */
                   !t || !t->valid) {
            b->alpha_on = 0; b->st_alpha_abort++; c7_add("F42", "aborts", 1); b->alpha_next = b->now + 8.0;
            if (diag_on()) fprintf(stderr, "[BOT-ALPHA] t=%.1f bot=%d calls off the charge (%d%%, vis=%d dmg=%.2f)\n", b->now, b->index, me->alpha_charge, vis, b->dmg_recent);
        }
    } else if (me->alpha_ready && b->now >= b->alpha_next && b->now >= b->alpha_cool_until && b->tgt && b->tgt->visible &&
               mem_engageable(b, b->tgt) && !out->hold_fire &&
               (out->type == MA2_ORD_ENGAGE || out->type == MA2_ORD_FIGHT_MOVE || (out->type == MA2_ORD_TRAVEL && out->fire))) {
        b->alpha_next = b->now + 2.5;
        float d = hdist(me->pos, b->tgt->pos), reach = my_reach(me);
        int worth = b->tgt->kind == MA2_KIND_MECH || b->tgt->kind == MA2_KIND_TANK || b->tgt->kind == MA2_KIND_VTOL ||
                    hp_frac(b->tgt->hp, b->tgt->hp_max) < 0.5f;
        int safe = b->dmg_recent < 0.06f && b->n_attackers <= (sk >= MA2_SKILL_VETERAN ? 1 : 2) && me->heat < 50.0f;
        if (f42_on()) {     /* F42: in a team fight someone is nearly always shooting: the better pilot charges through some of it */
            static const float dmg_ok[4] = { 0.06f, 0.10f, 0.18f, 0.25f }; static const int att_ok[4] = { 1, 2, 2, 3 };
            safe = b->dmg_recent < dmg_ok[sk] && b->n_attackers <= att_ok[sk] && me->heat < 50.0f &&
                   hp_frac(me->hp, me->hp_max) > (sk >= MA2_SKILL_VETERAN ? 0.35f : 0.5f);
            c7_add("F42", safe ? "chance_safe" : "chance_unsafe", 1);
        }
        /* every weapon at once: inside the reach of the short guns too, or most of it misses */
        if (worth && safe && d > 20.0f && d < minf(reach, 220.0f) && rng_f(&b->rng) < take[sk]) {
            b->alpha_on = 1; b->alpha_t0 = b->now; b->alpha_tgt = b->tgt->actor;
            if (diag_on()) fprintf(stderr, "[BOT-ALPHA] t=%.1f bot=%d charges at %08X d=%.0f attackers=%d\n", b->now, b->index, b->alpha_tgt, d, b->n_attackers);
        }
    }
    if (b->alpha_on) { out->alpha = 1; out->target = b->alpha_tgt; }
}
static void alpha_vtol(Ma2Brain *b, const Ma2PlayerView *me, Ma2Order *out)
{
    /* A gunship that sees a charge coming at it: end the firing pass now and duck out of sight. */
    if (kind_ok(me->kind) == MA2_KIND_VTOL && b->now < b->alpha_react_until) { out->vt_cover = 1.0f; if (!out->dodge) out->dodge = 1; }
}

static int shield_logic(Ma2Brain *b, const Ma2PlayerView *me)
{
    int i;
    float bd = 1e9f;
    if (me->qte_active || b->now < b->next_def || !me->defense_ready) return 0;
    /* Use it like a player: absorber / AMS / shield bubble when the fire coming in is heavy
     * (not for a scratch), camouflage when falling back or sitting on a sniping spot. */
    if ((me->defense_type == MA2_DEFT_ABSORB || me->defense_type == MA2_DEFT_AMS || me->defense_type == MA2_DEFT_SHIELD) &&
        b->in_dps >= 12.0f + 6.0f * (float)(3 - b->skill) && b->now - b->last_hit < 1.0) {
        if (rng_f(&b->rng) < 0.25f + 0.2f * (float)b->skill) {      /* skill: noticed in time */
            b->next_def = b->now + MA2B_DEFENSE_COOLDOWN; b->st_shield++;
            return 1;
        }
        b->next_def = b->now + 1.5;
        return 0;
    }
    /* JamUse (default on, RECOMP_BOT_JAM_USE / [Bots] JamUse; film review 11-08): target jamming (type 3) had no rule at all -- p5_85a1 T1 b0 / b1 and
     * T2 b9 carried it all match, never switched on: b1 stood 66-78 s under 2-4 attackers, def-ready, 30 % -> dead. A player jams when lock-on fire
     * pours in, and any defence goes on as a last stand: under 35 % and hit in the last 1.5 s. [C7] jamuse: on, last_stand. */
    if (c7_on("jamuse", "RECOMP_BOT_JAM_USE", "JamUse", 1)) {
        float hpf = hp_frac(me->hp, me->hp_max);
        if (me->defense_type == MA2_DEFT_JAM && b->in_dps >= 10.0f + 5.0f * (float)(3 - b->skill) && b->now - b->last_hit < 1.0) {
            if (rng_f(&b->rng) < 0.35f + 0.2f * (float)b->skill) { b->next_def = b->now + MA2B_DEFENSE_COOLDOWN; b->st_shield++; c7_add("jamuse", "on", 1); return 1; }
            b->next_def = b->now + 1.5;
            return 0;
        }
        if (me->defense_type != MA2_DEFT_CAMO && hpf < 0.35f && b->now - b->last_hit < 1.5 && b->skill >= MA2_SKILL_REGULAR) {
            b->next_def = b->now + MA2B_DEFENSE_COOLDOWN; b->st_shield++; c7_add("jamuse", "last_stand", 1); return 1;
        }
    }
    if (me->defense_type == MA2_DEFT_CAMO && (b->goal == G_RETREAT || b->goal == G_COVER || (b->standoff && b->tgt && b->tgt->visible))) {
        b->next_def = b->now + MA2B_DEFENSE_COOLDOWN; b->st_shield++;
        return 1;
    }
    if (me->kind != MA2_KIND_MECH) return 0;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Mem *m = &b->mem[i];
        float d;
        if (!m->valid || m->kind != MA2_KIND_BATTLEARMOR) continue;
        if (!m->visible && mem_conf(b, m) < 0.4f) continue;
        d = hdist(me->pos, m->pos);
        if (d < bd) bd = d;
    }
    if (bd > MA2B_SHIELD_RANGE && !me->enemy_rider) { b->shield_armed = 0; return 0; }
    if (!b->shield_armed) {                       /* skill decides whether we notice in time */
        b->shield_armed = 1;
        b->shield_go = me->enemy_rider || rng_f(&b->rng) < 0.45f + 0.18f * (float)b->skill;
        b->shield_at = b->now + (double)b->sk.reaction;
    }
    if (b->shield_go && b->now >= b->shield_at) {
        b->next_def = b->now + MA2B_DEFENSE_COOLDOWN;
        b->shield_go = 0;
        b->st_shield++;
        return 1;
    }
    return 0;
}

#include "ma2_bot_surprise.c"     /* surprise behaviours: the rarity gate + shared death/killer table */
static int sp2_pu_take(Ma2Brain *b, uint32_t entity);   /* surprise set B (ma2_bot_surprise_b.c): the power-up this Elite waited for / denied is back */
#include "ma2_bot_surprise_c.c"   /* surprise set C tricks (own state; uses sp_roll) */
#include "ma2_bot_brain_obj.c"   /* G_OBJECTIVE: CTF, Snatch-It, Check-It, Base War, Not-It */

/* ---- Capture the Flag: the VTOL flag run ----
 * A VTOL pilot and a BattleArmor of one team pair up: the VTOL picks the BattleArmor up, flies it
 * to their flag and sets it down right there; while the BattleArmor takes the flag the VTOL circles
 * close and shoots at the defenders; then it comes down for the carrier, picks it up again (under
 * fire too) and flies it home to our stand, where it lets go to score. A VTOL bot rolls for a run
 * every 30 s while the pieces are there (their flag free, a BattleArmor teammate far from it); the
 * better the pilot, the more often it tries and the more damage it takes before it gives up. One
 * run per team at a time; either member may advance it from what the world shows. Built on the
 * lift orders (order_lift / order_ride). RECOMP_BOT_FLAGRUN=0 turns it off;
 * RECOMP_BOT_FLAGRUN_TEST=1 makes every roll succeed. (The FR_ phases are declared with g_frun.) */
static const char *const k_fr_name[6] = { "-", "pickup", "inbound", "grab", "extract", "cover" };
/* F36 (switch RECOMP_BOT_FLAGRUN_SOLO / [Bots] FlagRunSolo, default on): with no BattleArmor to fly in
 * (a map where nobody starts in one), the VTOL still makes its run -- alone, as cover: it flies to
 * their flag and circles it shooting the defenders while our ground units go for it (FR_COVER),
 * until a teammate has the flag or 50 s are up. */
static int frun_solo_on(void) { return c7_on("f36-solo", "RECOMP_BOT_FLAGRUN_SOLO", "FlagRunSolo", 1); }
static int vtol_wait_player_on(void) { return c7_on("frun", "RECOMP_BOT_VTOL_WAIT_PLAYER", "VtolWaitPlayer", 1); }
/* [C7] frun (playtest: the run had no counters): started (paired with a bot
 * BattleArmor) / started_<skill> / started_player (a human clawed on, VtolWaitPlayer); reached_<phase>
 * (+ _player); ends_<reason>; captures / captures_<skill> / captures_player. Cover runs stay in f36-solo. */
static const char *const k_frun_started_sk[4] = { "started_recruit", "started_regular", "started_veteran", "started_elite" };
static const char *const k_frun_cap_sk[4] = { "captures_recruit", "captures_regular", "captures_veteran", "captures_elite" };
static const char *const k_frun_reached[6] = { "-", "reached_pickup", "reached_inbound", "reached_grab", "reached_extract", "reached_cover" };
static const char *const k_frun_reached_pl[6] = { "-", "reached_pickup_player", "reached_inbound_player", "reached_grab_player", "reached_extract_player", "-" };
static const char *frun_end_key(const char *why)
{
    static const char *const m[][2] = { { "captured", "ends_captured" }, { "sets the player down", "ends_damaged_setdown" }, { "damaged", "ends_damaged" },
        { "did not make it aboard", "ends_second_missed" }, { "walked away", "ends_player_left" },
        { "cover time", "ends_cover_time" }, { "too long", "ends_timeout" }, { "someone else", "ends_other_carrier" }, { "a teammate has", "ends_teammate_flag" },
        { "lost the flag", "ends_flag_lost" }, { "lost the", "ends_lost_unit" }, { "let go short", "ends_let_go_short" }, { "could not take", "ends_no_grab" },
        { "no flags", "ends_no_flags" }, { "too far to fetch", "ends_fetch_far" } };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (strstr(why, m[i][0])) return m[i][1];
    return "ends_other";
}
static void frun_end(int n, double now, const char *why)
{
    if (diag_on() && g_frun[n].phase)
        fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d ends in %s after %.0f s: %s%s\n", now, n, k_fr_name[g_frun[n].phase], now - g_frun[n].t0, why,
                g_frun[n].human ? " (player run)" : "");
    if (g_frun[n].phase && g_frun[n].phase != FR_COVER) {
        c7_add("frun", frun_end_key(why), 1);
        if (strstr(why, "captured")) {
            c7_add("frun", "captures", 1);
            c7_add("frun", g_frun[n].human ? "captures_player" : k_frun_cap_sk[g_frun[n].skill & 3], 1);
        }
    }
    memset(&g_frun[n], 0, sizeof g_frun[n]);
}
static void frun_phase(int n, int ph, double now)
{
    if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d %s -> %s (%.0f s in)\n", now, n, k_fr_name[g_frun[n].phase], k_fr_name[ph], now - g_frun[n].t0);
    g_frun[n].phase = ph; g_frun[n].tp = now;
    c7_add("frun", g_frun[n].human ? k_frun_reached_pl[ph % 6] : k_frun_reached[ph % 6], 1);
}
static int frun_member(const Ma2World *w, uint32_t actor, int nation)
{
    (void)w;
    int n = nation & 7;
    return g_frun[n].phase && actor && (g_frun[n].vtol == actor || g_frun[n].ba == actor || g_frun[n].ba2 == actor);
}
/* PlayerRunHurt (default on, RECOMP_BOT_PLAYER_RUN_HURT / [Bots] PlayerRunHurt; playtest 11-01: a badly damaged VTOL
 * carrying the player restarted its flag run again and again, Worker A 2ca7073): frun_step ended the player's run below the
 * give-up health and order_lift's VtolWaitPlayer start, which had no health test, began it again the next tick (wa2r4: 81
 * starts and ends in 10 s, the player flown on to their flag regardless). Now the VTOL makes the call once, with him aboard:
 * within reach of their flag (Recruit 200 / Regular 280 / Veteran 360 / Elite 450 m) it presses on and drops him there;
 * further out it takes him back toward our stand (the stand itself within 350 m, else 150 m back along the way) and comes
 * down low there for him to let go. The call stands while he hangs on and for 45 s after he lets go: no new player run
 * from this VTOL meanwhile. [C7] hurtrun press / setdown / player_off / setdown_s. */
static int player_run_hurt_on(void) { return c7_on("hurtrun", "RECOMP_BOT_PLAYER_RUN_HURT", "PlayerRunHurt", 1); }
static int hurt_mode(int n, uint32_t vtol, double now) { return g_hurt[n].vtol == vtol && vtol && now < g_hurt[n].until ? g_hurt[n].mode : 0; }
static int hurt_decide(const Ma2World *w, int n, const Ma2PlayerView *v, int skill, double now)
{
    static const float press[4] = { 200.0f, 280.0f, 360.0f, 450.0f };
    int m = hurt_mode(n, v->actor, now);
    if (m) return m;
    const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, n, 1), *theirs = obj_find(w, MA2_OBJ_FLAG, n, 0);
    float dgo = theirs ? hdist(v->pos, theirs->pos) : 1e9f;
    g_hurt[n].vtol = v->actor; g_hurt[n].t = now; g_hurt[n].until = 1e9;
    g_hurt[n].mode = dgo <= press[skill & 3] ? 1 : 2;
    g_hurt[n].pt = v->pos;
    if (g_hurt[n].mode == 2 && mine) {
        float dh = hdist(v->pos, mine->home);
        if (dh <= 350.0f || dh < 1.0f) g_hurt[n].pt = mine->home;
        else { g_hurt[n].pt.x += (mine->home.x - v->pos.x) / dh * 150.0f; g_hurt[n].pt.z += (mine->home.z - v->pos.z) / dh * 150.0f; }
    }
    c7_add("hurtrun", g_hurt[n].mode == 1 ? "press" : "setdown", 1);
    if (diag_on()) fprintf(stderr, "[BOT-HURTRUN] t=%.1f nation=%d VTOL %08X at %.0f%% health with the player aboard, %.0f m from their flag: %s (%.0f,%.0f)\n",
                           now, n, v->actor, 100.0f * hp_frac(v->hp, v->hp_max), dgo, g_hurt[n].mode == 1 ? "presses on" : "sets him down", g_hurt[n].pt.x, g_hurt[n].pt.z);
    return g_hurt[n].mode;
}
/* CarryHoldOn (default on, RECOMP_BOT_CARRY_HOLD_ON / [Bots] CarryHoldOn): a carrier riding home with their flag holds on to a damaged VTOL
 * down to the run's own extract give-up (~15 %), not the 30 % a plain lift lets go at -- it let go mid-map 420 m from home and died on
 * foot with the flag (ad1_64 b9 t=46.8, b7 t=221). Used by order_ride's carry branch and ExtractHpSync. [C7] carryhold: held_s. */
static float carry_let_go_hp(void) { return c7_on("carryhold", "RECOMP_BOT_CARRY_HOLD_ON", "CarryHoldOn", 1) ? 0.15f : 0.3f; }
static void frun_step(const Ma2World *w, int n, double now)
{
    const float *give_up_hp = k_frun_give_up;
    if (!g_frun[n].phase) return;
    const Ma2PlayerView *v = player_by_actor(w, g_frun[n].vtol), *ba = player_by_actor(w, g_frun[n].ba);
    const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, n, 1), *theirs = obj_find(w, MA2_OBJ_FLAG, n, 0);
    double in = now - g_frun[n].tp;
    if (!mine || !theirs) { frun_end(n, now, "no flags"); return; }
    if (!v || !v->alive || kind_ok(v->kind) != MA2_KIND_VTOL) { frun_end(n, now, "lost the VTOL"); return; }
    if (g_frun[n].phase == FR_COVER) {   /* F36: no BattleArmor in this run */
        /* PartnerPull: a free bot BattleArmor teammate in reach (frun_consider's test) ends the solo cover, so the run is
         * made with it -- the VTOL stayed on cover 800 m off while the partner it asked for waited (desktop 10-31) */
        int partner = 0;
        if (c7_on("partnerpull", "RECOMP_BOT_PARTNER_PULL", "PartnerPull", 1) && theirs->state != MA2_OBJS_CARRIED)
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS && !partner; j++) {
                const Ma2PlayerView *q = &w->players[j];
                if (!q->is_bot || !q->alive || q->nation != v->nation || kind_ok(q->kind) != MA2_KIND_BATTLEARMOR) continue;
                if (q->riding || q->qte_active || q->clamped_to || hdist(q->pos, theirs->pos) < 200.0f || hp_frac(q->hp, q->hp_max) < 0.5f) continue;
                if (hdist(q->pos, v->pos) < 650.0f) partner = 1;
            }
        if (partner) { c7_add("partnerpull", "cover_to_run", 1); g_frun_pull[n] = now; frun_end(n, now, "a BattleArmor partner is up"); }
        else if (hp_frac(v->hp, v->hp_max) < give_up_hp[g_frun[n].skill & 3]) frun_end(n, now, "the VTOL is too badly damaged");
        else if (theirs->state == MA2_OBJS_CARRIED) { c7_add("f36-solo", "flag_taken", 1); frun_end(n, now, "a teammate has their flag (cover done)"); }
        else if (in > 50.0) frun_end(n, now, "cover time over");
        return;
    }
    if (g_frun[n].ba2) {   /* VtolTwoRiders: the second rider took their flag, or is the one left -- the run is his now */
        const Ma2PlayerView *b2 = player_by_actor(w, g_frun[n].ba2);
        int lost = !ba || !ba->alive || kind_ok(ba->kind) != MA2_KIND_BATTLEARMOR;
        if (!b2 || !b2->alive || kind_ok(b2->kind) != MA2_KIND_BATTLEARMOR) g_frun[n].ba2 = 0;
        else if (lost || (theirs->carrier && theirs->carrier == b2->player)) {
            c7_add("tworiders", lost ? "second_takes_over" : "second_carries", 1);
            if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d second BattleArmor %08X%s %s the run\n", now, n, b2->actor, b2->is_human ? " (player)" : "",
                                   lost ? "takes over" : "carries their flag: flown home in");
            g_frun[n].ba2 = lost ? 0 : g_frun[n].ba; g_frun[n].ba = b2->actor; g_frun[n].human = b2->is_human;
            ba = b2;
        }
    }
    if (!ba || !ba->alive || kind_ok(ba->kind) != MA2_KIND_BATTLEARMOR) { frun_end(n, now, "lost the BattleArmor"); return; }
    int carrying = theirs->carrier && theirs->carrier == ba->player;
    /* Going back in for the carrier is what the run is for: it presses on to half the usual damage
     * (the carrier died 40 m from their stand while its VTOL turned home at 0.28 -- run fr20). */
    float give_up = give_up_hp[g_frun[n].skill & 3] * (g_frun[n].phase == FR_EXTRACT ? 0.5f : 1.0f);
    if (hp_frac(v->hp, v->hp_max) < give_up && !(carrying && ba->riding == v->actor)) {
        /* PlayerRunHurt: the player hangs on -- one call, kept (press on: the run goes on; else he is set down) */
        if (g_frun[n].human && ba->riding == v->actor && player_run_hurt_on()) {
            if (hurt_decide(w, n, v, g_frun[n].skill, now) == 2) { frun_end(n, now, "the VTOL is too badly damaged: it sets the player down"); return; }
        } else { frun_end(n, now, "the VTOL is too badly damaged"); return; }
    }
    switch (g_frun[n].phase) {
    case FR_PICKUP:
        if (ba->riding == v->actor) frun_phase(n, FR_INBOUND, now);
        else if (theirs->state == MA2_OBJS_CARRIED) frun_end(n, now, "someone else has their flag");
        else if (in > 45.0) frun_end(n, now, "the pickup took too long");
        break;
    case FR_INBOUND:
        if (carrying) frun_phase(n, FR_EXTRACT, now);
        else if (ba->riding != v->actor) {
            if (hdist(ba->pos, theirs->pos) < 150.0f) frun_phase(n, FR_GRAB, now);   /* close enough: it jets the rest (shot off 88 m out, run fr14) */
            /* RunRepick (default on, RECOMP_BOT_RUN_REPICK / [Bots] RunRepick): a rider knocked off on the way is fetched again and the run
             * goes on (twice a run) -- the run ended there, the lift then ferried the BattleArmor to its own guard point and the pair turned
             * on the player (playtest 11-04: no side trips on a flag run; Bogged Down rec 2026-11-04 bot 10/9, t=19.5). */
            else if (g_frun[n].rp < 2 && !g_frun[n].human && c7_on("runrepick", "RECOMP_BOT_RUN_REPICK", "RunRepick", 1)) {
                g_frun[n].rp++; c7_add("runrepick", "repicks", 1);
                if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d the BattleArmor came off %.0f m short: fetched again (%d)\n", now, n, hdist(ba->pos, theirs->pos), g_frun[n].rp);
                frun_phase(n, FR_PICKUP, now);
            }
            else frun_end(n, now, "the BattleArmor let go short of the flag");
        } else if (theirs->state == MA2_OBJS_CARRIED) frun_end(n, now, "someone else has their flag");
        else if (in > 90.0) frun_end(n, now, "the flight took too long");
        break;
    case FR_GRAB:
        if (carrying) frun_phase(n, FR_EXTRACT, now);
        else if (theirs->state == MA2_OBJS_CARRIED) frun_end(n, now, "someone else has their flag");
        else if (g_frun[n].human && hdist(ba->pos, theirs->pos) > 200.0f) frun_end(n, now, "the player walked away from their flag");
        else if (in > 40.0) frun_end(n, now, "the BattleArmor could not take the flag");
        break;
    case FR_EXTRACT:
        if (!carrying) frun_end(n, now, theirs->state == MA2_OBJS_HOME ? "their flag is back on its stand (captured)" : "the carrier lost the flag");
        /* ExtractHpSync (default on, RECOMP_BOT_EXTRACT_HP_SYNC / [Bots] ExtractHpSync): the carrier lets go of a VTOL under 30 % (order_ride),
         * but the run held on to 14 % and kept sending it back to ride: it re-boarded and dropped for 25 s, 64 m from its own stand with
         * the flag, while the VTOL hovered over it (au5_60 b1/b2, t=50.3-75.7). Off the claw and the VTOL under the carry limit: the run
         * ends, the carrier walks it home. [C7] frun: ends_carry_hp. */
        else if (ba->riding != v->actor && hp_frac(v->hp, v->hp_max) < carry_let_go_hp() && c7_on("extracthp", "RECOMP_BOT_EXTRACT_HP_SYNC", "ExtractHpSync", 1)) {
            c7_add("frun", "ends_carry_hp", 1); frun_end(n, now, "the VTOL is too damaged to carry the flag home: the carrier walks it");
        }
        else if (in > 100.0) frun_end(n, now, "the extraction took too long");
        break;
    }
}
/* A VTOL bot with nothing better on: roll for a run with the nearest free BattleArmor teammate. */
static void frun_consider(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    static const float p_try[4] = { 0.25f, 0.45f, 0.65f, 0.9f };   /* playtest 10-31: the VTOL+BattleArmor run is a strong play: bots make it as readily as a player; playtest 11-04: Elite 90 %, Dragoon 100 % */
    const float ptry = ma2_bots_dragoon() && (b->skill & 3) == 3 ? 1.0f : p_try[b->skill & 3];
    int n = me->nation & 7;
    int pulled = g_frun_pull[n] > 0.0 && b->now >= g_frun_pull[n] && b->now - g_frun_pull[n] < 3.0;   /* PartnerPull: straight to the run */
    if (g_frun[n].phase || (b->now < b->frun_next && !pulled)) return;
    b->frun_next = b->now + 30.0;
    /* CtfPair: the opening's VTOL starts the run the moment its partner sits in the BattleArmor -- no roll; until then no lone
     * cover and no partner ask (look again in 0.5 s). The player's role (his VTOL / his BattleArmor): the muster and BaBoardPlayer. */
    const Ma2PlayerView *pba = NULL;
    if (pair_active(n) && g_pair[n].vt_pl == me->player) {
        pba = player_by_pl(w, g_pair[n].ba_pl);
        if (!pba || !pba->alive || kind_ok(pba->kind) != MA2_KIND_BATTLEARMOR || (pba->riding && pba->riding != me->actor)) {
            b->frun_next = b->now + 0.5; c7_add("ctfpair", "vtol_waits_n", 1);
            return;
        }
    }
    const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, me->nation, 1), *theirs = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
    /* RunNeedsHome (default on, RECOMP_BOT_RUN_NEEDS_HOME / [Bots] RunNeedsHome): no run while our own flag is carried -- a capture
     * needs it on its stand, so the run could only grab and wait (au16_61 runs 4 and 5: started with nation 1's flag away, no capture
     * possible, run 5's VTOL died re-picking its BattleArmor). No partner ask or lone cover either. [C7] frun: no_run_flag_away. */
    /* CapAnyway (ma2_bot_brain_obj.c): MechAssault 2 scores with our own flag away, so a run is worth making then too -- RunNeedsHome
     * stands only with CapAnyway off. */
    if (mine && mine->state == MA2_OBJS_CARRIED && c7_on("runneedshome", "RECOMP_BOT_RUN_NEEDS_HOME", "RunNeedsHome", 1) && !cap_anyway_on()) {
        c7_add("frun", "no_run_flag_away", 1); b->frun_next = b->now + 5.0;
        if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f bot=%d no run: our flag is carried\n", b->now, b->index);
        return;
    }
    const char *why = !mine || !theirs ? "no flags" : theirs->state == MA2_OBJS_CARRIED ? "their flag is carried" :
                      hp_frac(me->hp, me->hp_max) < 0.6f ? "damaged" : b->vt_carry || me->cargo ? "carrying a power-up" :
                      NULL;
    int hrider = 0;   /* TwoWaitCap (c): the player hangs on -- his ride first (his VtolWaitPlayer run) */
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS && !why; j++) if (w->players[j].alive && w->players[j].is_human && w->players[j].riding == me->actor) hrider = 1;
    if (hrider && two_wait_cap_on()) {   /* no partner ask, no solo cover either: it would block his run */
        c7_add("twowaitcap", "no_run_rider", 1);
        if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f bot=%d no run: the player hangs on\n", b->now, b->index);
        return;
    }
    const Ma2PlayerView *best = NULL; float bd = 650.0f; int nba = 0;   /* a VTOL covers that in ~10 s */
    if (pba && !why) { best = pba; bd = hdist(pba->pos, me->pos); nba = 1; }
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS && !why && !pba; j++) {
        const Ma2PlayerView *q = &w->players[j];
        if (!q->is_bot || !q->alive || q->nation != me->nation || kind_ok(q->kind) != MA2_KIND_BATTLEARMOR) continue;
        nba++;
        /* RunFromLift (default on, RECOMP_BOT_RUN_FROM_LIFT / [Bots] RunFromLift): a BattleArmor already on OUR VTOL (a muster lift)
         * is the run's partner -- the lift took the team's BattleArmor first, so the run never found a free one and the VTOL ferried
         * a defender to the middle instead (a playtester's NRC matches 11-04, match 4: lift t=15, run only at t=44). */
        int aboard_us = q->riding == me->actor && c7_on("runfromlift", "RECOMP_BOT_RUN_FROM_LIFT", "RunFromLift", 1);
        if ((q->riding && !aboard_us) || q->qte_active || q->clamped_to || hdist(q->pos, theirs->pos) < 200.0f) continue;
        if (aboard_us) { best = q; bd = 0.0f; c7_add("runfromlift", "from_lift", 1); break; }
        if (hp_frac(q->hp, q->hp_max) < 0.5f) continue;   /* a BattleArmor at a quarter health was picked, and died on the way to the VTOL */
        float d = hdist(q->pos, me->pos);
        if (d < bd) { bd = d; best = q; }
    }
    if (!why && !best) why = nba ? "no free BattleArmor in reach" : "no BattleArmor teammate";
    /* PartnerWait (default on, RECOMP_BOT_PARTNER_WAIT / [Bots] PartnerWait): at the start the VTOL was up before its partner had
     * climbed into the BattleArmor, read "no BattleArmor teammate" and flew cover over their flag alone -- shot down there, run
     * over (NRC CTF Elite vh1/vh2 11-04: 4 lone covers ended "too badly damaged", 0-2 runs a match; playtest: the VTOL stayed to fight in
     * the middle, an easy target). A teammate pilot within 40 m of a free BattleArmor is a partner on the way: no lone
     * cover, look again in 3 s. [C7] partnerwait: waits. */
    if (why && !best && !b->vt_carry && !me->cargo && mine && theirs && theirs->state != MA2_OBJS_CARRIED && hp_frac(me->hp, me->hp_max) >= 0.6f &&
        c7_on("partnerwait", "RECOMP_BOT_PARTNER_WAIT", "PartnerWait", 1)) {
        int coming = 0;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS && !coming; j++) {
            const Ma2PlayerView *q = &w->players[j];
            if (!q->alive || !q->is_bot || q->nation != me->nation || kind_ok(q->kind) != MA2_KIND_PILOT) continue;   /* (not the player: he stood by one at spawn and the wait never ended, vk2) */
            for (int vi = 0; vi < w->nvehicles && vi < MA2_BOT_MAX_VEHICLES; vi++)
                if (w->vehicles[vi].kind == MA2_KIND_BATTLEARMOR && !w->vehicles[vi].occupied && hdist(w->vehicles[vi].pos, q->pos) < 40.0f) { coming = 1; break; }
        }
        if (!coming || b->pw_since > b->now) b->pw_since = 0.0;
        else if (b->pw_since <= 0.0) b->pw_since = b->now;
        if (coming && b->now - b->pw_since < 15.0) {   /* 15 s at most, then as before */
            b->frun_next = b->now + 3.0; c7_add("partnerwait", "waits", 1);
            if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f bot=%d waits: a teammate is getting into a BattleArmor\n", b->now, b->index);
            return;
        }
    }
    if (why) {
        /* Up for a run but nobody to fly: ask for a partner -- for a while, a teammate choosing
         * a ride takes a free BattleArmor before a 'Mech (vehicle_value). */
        int recruit = !best && mine && theirs && theirs->state != MA2_OBJS_CARRIED && hp_frac(me->hp, me->hp_max) >= 0.6f &&
                      (getenv("RECOMP_BOT_FLAGRUN_TEST") || rng_f(&b->rng) < ptry);
        if (recruit) { g_frun_want[n] = b->now + 45.0; b->frun_next = b->now + (c7_on("runfromlift", "RECOMP_BOT_RUN_FROM_LIFT", "RunFromLift", 1) ? 4.0 : 20.0); }   /* RunFromLift: look again soon -- a partner on board starts it */
        if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f bot=%d no run: %s%s\n", b->now, b->index, why, recruit ? " -- asks for a BattleArmor partner" : "");
        if (recruit && !b->vt_carry && !me->cargo && frun_solo_on() && !ctf_ground_mate(w, me)) { c7_add("loneground", "cover_skipped", 1); recruit = 0; }   /* LoneGround: nobody to cover */
        if (recruit && !b->vt_carry && !me->cargo && frun_solo_on()) {   /* F36: and goes in alone meanwhile, as cover for the ground grab */
            memset(&g_frun[n], 0, sizeof g_frun[n]);
            g_frun[n].vtol = me->actor; g_frun[n].phase = FR_COVER; g_frun[n].skill = b->skill;
            g_frun[n].t0 = g_frun[n].tp = b->now;
            c7_add("f36-solo", "cover_runs", 1); c7_add("f36-solo", k_c7_f36_sk[b->skill & 3], 1);
            if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d bot=%d (skill %d) VTOL %08X flies cover over their flag alone (%.0f m off)\n",
                                   b->now, n, b->index, b->skill, me->actor, hdist(me->pos, theirs->pos));
        }
        return;
    }
    if (!getenv("RECOMP_BOT_FLAGRUN_TEST") && !pulled && !pba && rng_f(&b->rng) >= ptry) return;
    if (pulled) { g_frun_pull[n] = 0.0; c7_add("partnerpull", "runs", 1); }
    memset(&g_frun[n], 0, sizeof g_frun[n]);
    g_frun[n].vtol = me->actor; g_frun[n].ba = best->actor; g_frun[n].phase = FR_PICKUP; g_frun[n].skill = b->skill;
    g_frun[n].t0 = g_frun[n].tp = b->now;
    c7_add("frun", "started", 1); c7_add("frun", k_frun_started_sk[b->skill & 3], 1); c7_add("frun", "reached_pickup", 1);
    if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d bot=%d (skill %d) VTOL %08X takes BattleArmor %08X (%.0f m off, %.0f m from their flag)\n",
                           b->now, n, b->index, b->skill, me->actor, best->actor, bd, hdist(best->pos, theirs->pos));
}
/* RECOMP_BOT_LIFT_TRACE=1 (test hook, default off): once a second, any carrier of a flag and the lift state around it:
 * where it is, who hangs on it, the lift table entry (age, VTOL that claimed it), the claiming VTOL's distance and height
 * and every friendly VTOL's distance. Lines [LIFT-TR] (VTOL-CARRIER-SPEC proof: pickup -> flight -> drop). */
static float ride_margin(const Ma2Brain *b)
{
    if (b->obj_op == OP_CARRY && lift_carrier_on() && lift_board_fix_on()) { c7_add("liftboard", "carrier_margin_evals", 1); return MA2B_HYSTERESIS + 0.5f; }
    return 0.05f;
}
/* [LIFT-WHY]: why a VTOL passes a live flag-carrier request (RECOMP_BOT_LIFT_TRACE), once per 2 s per bot. */
static void lift_why(const Ma2Brain *b, uint32_t ba, const char *why, float dd)
{
    static double last[16]; static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BOT_LIFT_TRACE"); on = e && *e && *e != '0'; }
    int k = b->index & 15;
    if (!on) return;
    if (b->now < last[k]) last[k] = -9.0;
    if (b->now - last[k] < 2.0) return;
    last[k] = b->now;
    fprintf(stderr, "[LIFT-WHY] t=%.1f vtol bot=%d (skill %d) passes carrier %08X: %s (%.0f m, %d attackers, goal %s)\n", b->now, b->index, b->skill, ba, why, dd, b->n_attackers, k_goal_name[b->goal]);
}
/* [LIFT-FLY] (RECOMP_BOT_LIFT_TRACE): the VTOL with a rider, once a second: where, where to, how far, how high the rider hangs. */
static void lift_fly_trace(const Ma2Brain *b, const Ma2PlayerView *me, const Ma2PlayerView *q, Ma2Vec3 dest)
{
    static double last[16]; static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BOT_LIFT_TRACE"); on = e && *e && *e != '0'; }
    int k = b->index & 15;
    if (!on) return;
    if (b->now < last[k]) last[k] = -9.0;
    if (b->now - last[k] < 1.0) return;
    last[k] = b->now;
    fprintf(stderr, "[LIFT-FLY] t=%.1f vtol bot=%d at %.0f,%.0f,%.0f hp=%.2f dest %.0f,%.0f (%.0f m) rider %08X y=%.0f note=%s att=%d\n", b->now, b->index,
            me->pos.x, me->pos.y, me->pos.z, hp_frac(me->hp, me->hp_max), dest.x, dest.z, hdist(me->pos, dest), q->actor, q->pos.y, b->note, b->n_attackers);
}
static void lift_trace(const Ma2World *w, double now)
{
    static double last = -9.0; static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BOT_LIFT_TRACE"); on = e && *e && *e != '0'; }
    if (!on || now - last < 1.0) return;
    if (now < last) last = -9.0;
    last = now;
    for (int o = 0; o < w->nobjectives; o++) {
        const Ma2ObjectiveView *f = &w->objectives[o];
        if (f->kind != MA2_OBJ_FLAG || !f->carrier) continue;
        const Ma2PlayerView *c = NULL;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].player == f->carrier) { c = &w->players[j]; break; }
        if (!c) continue;
        const Ma2ObjectiveView *home = obj_find(w, MA2_OBJ_FLAG, c->nation, 1);
        int li = lift_find(c->actor);
        const Ma2PlayerView *cv = li >= 0 && g_lift[li].vtol ? player_by_actor(w, g_lift[li].vtol) : NULL;
        char vs[160] = "";
        int n = 0;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS && n < 3; j++) {
            const Ma2PlayerView *q = &w->players[j];
            if (!q->alive || q->kind != MA2_KIND_VTOL || q->nation != c->nation) continue;
            size_t l = strlen(vs);
            snprintf(vs + l, sizeof vs - l, " [%08X %.0fm y%.0f hp%.2f rider=%d]", q->actor, hdist(q->pos, c->pos), q->pos.y, hp_frac(q->hp, q->hp_max),
                     c->riding == q->actor);
            n++;
        }
        fprintf(stderr, "[LIFT-TR] t=%.1f carrier %08X%s kind=%d %.0f,%.0f,%.0f hp=%.2f home %.0f m riding=%08X req=%s%s claimed_by=%08X (%.0f m)%s\n", now, c->actor,
                c->is_human ? " HUMAN" : "", c->kind, c->pos.x, c->pos.y, c->pos.z, hp_frac(c->hp, c->hp_max), home ? hdist(c->pos, home->home) : -1.0f, c->riding,
                li >= 0 ? "y" : "n", li >= 0 ? (now - g_lift[li].t < 3.0 ? "(live)" : "(stale)") : "", g_lift[li >= 0 ? li : 0].vtol * (li >= 0),
                cv ? hdist(cv->pos, c->pos) : -1.0f, vs);
    }
}
/* CtfPair: the paired BattleArmor holds where it is until its VTOL comes for it (the run's ride / BaBoardPlayer take over) */
static void pair_eval(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s)
{
    int n = me->nation & 7;
    if (!pair_active(n) || me->player != g_pair[n].ba_pl || kind_ok(me->kind) != MA2_KIND_BATTLEARMOR || me->riding) return;
    /* only until the VTOL is there: the hold (the running goal, with its hysteresis) outbid the muster ride by 0.35 and the
     * BattleArmor stood 9 s beside its VTOL hovering 7.6 m over it (vm1 11-04, t=12.6-21.7) */
    const Ma2PlayerView *pv = g_pair[n].human == 1 ? player_by_actor(w, g_pair[n].hvt) : player_by_pl(w, g_pair[n].vt_pl);
    if (pv && pv->alive && kind_ok(pv->kind) == MA2_KIND_VTOL && hdist(pv->pos, me->pos) < (g_pair[n].human == 1 ? 30.0f : 90.0f)) { c7_add("ctfpair", "ba_hold_released", 1); return; }
    obj_set(b, OP_HOLD, me->pos, 6.0f, 2.2f, s, 0);
    c7_add("ctfpair", "ba_hold_n", 1);
}
static void frun_eval(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BOT_FLAGRUN"); on = !(e && *e == '0'); }
    b->frun_cover = 0;
    if (!on || w->game_type != MA2_GT_CTF || !w->team_mode || !lifts_on()) return;
    int n = me->nation & 7, k = kind_ok(me->kind);
    /* VtolMuster: holding for a BattleArmor teammate -- no flag run roll, and a solo cover run gives way (it tore off at
     * the start of a match before anyone had reached a BattleArmor: playtest 10-31). */
    if (k == MA2_KIND_VTOL && b->muster_pl && vtol_muster_on()) {
        if (b->muster_human && vtol_two_riders_on()) frun_consider(b, w, me);   /* VtolTwoRiders: a bot BattleArmor partner may come along */
        else if (pair_active(n) && g_pair[n].vt_pl == me->player) frun_consider(b, w, me);   /* CtfPair: the run starts now; the muster stands down for it */
        if (g_frun[n].phase == FR_COVER && g_frun[n].vtol == me->actor) { c7_add("muster", "cover_run_ended", 1); frun_end(n, b->now, "holding for a BattleArmor teammate"); }
    } else if (k == MA2_KIND_VTOL) frun_consider(b, w, me);
    frun_step(w, n, b->now);
    if (!g_frun[n].phase) return;
    const Ma2ObjectiveView *theirs = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
    const Ma2PlayerView *v = player_by_actor(w, g_frun[n].vtol), *ba = player_by_actor(w, g_frun[n].ba);
    int ph = g_frun[n].phase;
    if (ph == FR_COVER) {
        if (theirs && v && me->actor == g_frun[n].vtol) { s[G_LIFT] = 2.6f; b->lift_ba = 0; b->frun_cover = 1; }
        return;
    }
    if (!theirs || !v || !ba) return;
    /* RunCommit (default on, RECOMP_BOT_RUN_COMMIT / [Bots] RunCommit): the two on a run take no side trips while it is on the way in --
     * no support / attack / cover / power-up detours (they still fire on the way; damage still ends the run; playtest 11-04). */
    if ((ph == FR_PICKUP || ph == FR_INBOUND) && (me->actor == g_frun[n].vtol || me->actor == g_frun[n].ba) && c7_on("runcommit", "RECOMP_BOT_RUN_COMMIT", "RunCommit", 1)) {
        if (s[G_SUPPORT] > 0.f || s[G_ATTACK] > 0.f || s[G_COVER] > 0.f || s[G_POWERUP] > 0.f) c7_add("runcommit", "side_goals_dropped", 1);
        s[G_SUPPORT] = s[G_ATTACK] = s[G_COVER] = s[G_POWERUP] = 0.f;
    }
    /* ExtractHome (default on, RECOMP_BOT_EXTRACT_HOME / [Bots] ExtractHome; design 11-14, the 2v2 CTF stall): order_ride lets a carrier go
     * within 40 m of our stand (to walk onto it), but the extract kept the BattleArmor's ride (G_RIDE 3.0 whenever its VTOL is within
     * 120 m: "meet it") and the VTOL's fetch (G_LIFT 3.0, lift-pickup over the BattleArmor) -- the carrier clawed back on at once
     * (ReClaw, 2 s) and was let go again, 22 m from its stand, every 2 s for 80 s: rc33 c34bd0 Bogged Down 2v2, both pairs, bots 0-3
     * still 49.7-131.4 until the 100 s limit (on Deliverance the same with obj=def/hold). Off the claw within 40 m of our stand the
     * carrier walks the rest: no ride, no fetch. [C7] extracthome: ride_dropped_s. */
    const Ma2ObjectiveView *mine0 = obj_find(w, MA2_OBJ_FLAG, me->nation, 1);
    int at_home = ph == FR_EXTRACT && mine0 && ba->riding != v->actor && hdist(ba->pos, mine0->home) < 40.0f &&
                  c7_on("extracthome", "RECOMP_BOT_EXTRACT_HOME", "ExtractHome", 1);
    if (at_home && me->actor == g_frun[n].ba) c7_add("extracthome", "ride_dropped_s", b->dt);
    if (at_home && (me->actor == g_frun[n].vtol || me->actor == g_frun[n].ba)) return;
    if (me->actor == g_frun[n].vtol) {
        if (ph == FR_GRAB) { s[G_LIFT] = 2.6f; b->lift_ba = 0; b->frun_cover = 1; }   /* cover the grab */
        else { s[G_LIFT] = 3.0f; b->lift_ba = ba->actor; }                              /* fetch / fly / fetch again */
        /* VtolTwoRiders: holding for another BattleArmor (the player) -- the run's own climbs on meanwhile (order_lift's hold) */
        if ((ph == FR_PICKUP || ph == FR_INBOUND) && b->muster_pl && b->muster_pl != ba->player && vtol_two_riders_on()) b->lift_ba = 0;
        const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, me->nation, 1);
        lift_post(ba->actor, ba->nation, ba->pos, ph == FR_EXTRACT && mine ? mine->home : theirs->pos, b->now);
        { int li = lift_find(ba->actor); if (li >= 0) { g_lift[li].vtol = me->actor; g_lift[li].go_t = b->now; g_lift[li].car = ph == FR_EXTRACT; } }   /* no other VTOL takes it; CaptureReset: the extract's ride home */
    } else if (me->actor == g_frun[n].ba) {
        if (ph == FR_PICKUP || ph == FR_INBOUND) { s[G_RIDE] = 3.0f; b->lift_vtol = v->actor; }
        else if (ph == FR_GRAB) {
            obj_set(b, OP_GRAB, theirs->pos, 0.0f, 2.8f, s, theirs->entity);
            /* GrabNoRide (default on, RECOMP_BOT_GRAB_NO_RIDE / [Bots] GrabNoRide): dropped for the grab, the VTOL hovering low over it is not
             * a ride (muster / BaBoardPlayer / a lift outbid the grab): drop and ride flapped 3 times, 8.5 s to the flag (au16_61 run 3) */
            if (!me->riding && c7_on("grabnoride", "RECOMP_BOT_GRAB_NO_RIDE", "GrabNoRide", 1)) { if (s[G_RIDE] > 0.f) c7_add("grabnoride", "rides_dropped", 1); s[G_RIDE] = 0.f; }
        }
        else if (me->riding == v->actor || hdist(v->pos, me->pos) < 120.0f) { s[G_RIDE] = 3.0f; b->lift_vtol = v->actor; }   /* extract: meet it (else run on) */
    } else if (g_frun[n].ba2 && me->actor == g_frun[n].ba2) {   /* VtolTwoRiders: the second rider rides in and goes for the flag too */
        if ((ph == FR_PICKUP || ph == FR_INBOUND) && me->riding == v->actor) { s[G_RIDE] = 3.0f; b->lift_vtol = v->actor; }
        else if (ph == FR_GRAB) { obj_set(b, OP_GRAB, theirs->pos, 0.0f, 2.8f, s, theirs->entity); c7_add("tworiders", "second_grab_evals", 1); }
    }
}

/* ======================================================================
 * Decision: behaviour utilities
 * ====================================================================== */
static int teammate_in_trouble(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, uint32_t *attacker, Ma2Vec3 *where)
{
    int i, j;
    float bestd = 230.0f; int found = 0;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        int attackers = 0; uint32_t a1 = 0;
        float d;
        if (q == me || !q->alive || q->nation != me->nation || q->kind == MA2_KIND_PILOT) continue;
        d = hdist(q->pos, me->pos);
        if (d > bestd) continue;
        for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Ma2PlayerView *e = &w->players[j];
            if (!is_enemy(w, me, e) || e->attacking != q->actor) continue;
            attackers++; if (!a1) a1 = e->actor;
        }
        if (attackers >= 2 || (attackers >= 1 && hp_frac(q->hp, q->hp_max) < 0.45f)) { bestd = d; *attacker = a1; *where = q->pos; found = 1; }
    }
    return found;
}

/* F6: power-ups by what the game would hand over (its collect check, sub_131C9A): armour only
 * to a damaged body (a 'Mech: one of its damage zones below 100 -- health below full stands in),
 * a weapon upgrade only while one of our weapon classes has a level to go. Anything else stays
 * where it lies, so a trip to it is wasted. Value: armour 0.25 + 1.5 x how hurt we are (x the
 * share of the missing armour it gives back); weapon upgrade 0.5; 0 = cannot take it.
 * Switch: env RECOMP_BOT_PICKUP_NEED=0/1 over pc_settings.ini [Bots] PickupNeed (batch 1: on);
 * off (or a map whose pickups are not typed) = every power-up worth the same, as before. */
static int pickup_need_on(void) { return c7_on("pickups", "RECOMP_BOT_PICKUP_NEED", "PickupNeed", 1); }
/* F25: a power-up the game hands out is parked at the map's edge at once and leaves the world view
 * (core), so the 'collected' mark only has to bridge a moment. It was 25 s: a teammate who passed
 * within 4 m of an armour pack it could use (but the game had not given it) hid the pack from a
 * hurt bot for 25 s. Now 4 s, and per team as F6 made it. RECOMP_BOT_PICKUP_MARK / [Bots] PickupMarkFix.
 * F26: a pickup was dropped whenever a remembered enemy was nearer to it (de < 0.8 d), so a hurt
 * bot's goal flickered and it circled the pack. The one we are walking to is now kept unless an
 * enemy is on it (within 12 m) or much nearer than us (de < 0.4 d).
 * RECOMP_BOT_PICKUP_HOLD / [Bots] PickupHold. */
static int pickup_mark_on(void) { return c7_on("f25", "RECOMP_BOT_PICKUP_MARK", "PickupMarkFix", 1); }
static int pickup_hold_on(void) { return c7_on("f26", "RECOMP_BOT_PICKUP_HOLD", "PickupHold", 1); }
/* F66: bots never took a pickup to keep it from an enemy, so a hurt enemy (or a human) walked to
 * the armour pack unopposed. Veterans and Elites now go for one they can use when a seen enemy
 * below half health (armour), or one level short of Alpha Strike (a weapon upgrade), is within
 * 80 m of it and we are nearer. RECOMP_BOT_PICKUP_DENY / [Bots] PickupDeny.
 * [C7] f66 seen_<sk> (evaluations with a pickup to deny), want_<sk> (evaluations where it raised
 * the power-up goal), taken_<sk> (denied pickups we reached). */
static int pickup_deny_on(void) { return c7_on("f66", "RECOMP_BOT_PICKUP_DENY", "PickupDeny", 1); }
static const char *const k_f66_seen[4] = { "seen_recruit", "seen_regular", "seen_veteran", "seen_elite" };
static const char *const k_f66_want[4] = { "want_recruit", "want_regular", "want_veteran", "want_elite" };
static const char *const k_f66_taken[4] = { "taken_recruit", "taken_regular", "taken_veteran", "taken_elite" };
/* F42 Alpha Strike (bots batch 2; env RECOMP_BOT_F42 / ini [Bots] BotAlphaUpgrades, default 1).
 * 0 Alpha Strikes in 296 archived runs: Alpha is ready only with classes 0-2 all at the arsenal's
 * top level (a hit drops one), and no bot went for weapon upgrades (worth 0.5, an armour pack's
 * worth). Now an Alpha-capable 'Mech values an upgrade by skill (+0 / 0.2 / 0.5 / 0.7, and
 * +0.2 / 0.6 / 0.8 more when one level is missing), and Veterans and Elites go out of their way
 * for one within 250 m when it is quiet. The existing alpha_logic charges once it is ready.
 * [C7] F42 want_s2/s3 (evaluations), lvl_up (levels gained), ready (evaluations with Alpha ready),
 * fires / aborts (charges let go / called off). */
static int f42_on(void) { return c7_on("F42", "RECOMP_BOT_F42", "BotAlphaUpgrades", 1); }
/* PuClass (default on, RECOMP_BOT_PU_CLASS / [Bots] PuClass; design note: never go for a power-up it cannot take): a weapon
 * upgrade raises ONE fire class (Energy / Ballistic / Missile = arsenal class 0-2) and the game hands it only to a body whose class has a
 * level to go. wpn_room is a bit per class and the pickup carries its class, so a bot wants only the upgrades it can take (a BattleArmor
 * with only class 2 ignores class 0/1 crates). Class unknown: any room, as before (PuRefusedKind stays the fallback). [C7] puclass: fits / nofit. */
static int pu_class_on(void) { return c7_on("puclass", "RECOMP_BOT_PU_CLASS", "PuClass", 1); }
static int wpn_fits(const Ma2PlayerView *me, const Ma2PickupView *pk)
{
    if (!me->wpn_room) return 0;
    if (!pu_class_on() || pk->wclass < 0 || pk->wclass > 2) return 1;
    return (me->wpn_room >> pk->wclass) & 1;
}
static float powerup_value(const Ma2Brain *b, const Ma2PlayerView *me, const Ma2PickupView *pk)
{
    if (!pickup_need_on() || !b->pu_typed) return 1.0f;
    if (pk->armor > 0) {
        float hpf = hp_frac(me->hp, me->hp_max), miss = me->hp_max - me->hp;
        if (hpf >= 0.999f || miss <= 0.0f) return 0.0f;
        return 0.25f + 1.5f * (1.0f - hpf) * minf((float)pk->armor / miss, 1.0f);
    }
    if (!wpn_fits(me, pk)) return 0.0f;
    if (pk->wclass < 0 && b->pu_wref_actor && b->pu_wref_actor == me->actor) return 0.0f;   /* PuRefusedKind: refused one already */
    if (f42_on() && me->alpha_cap && me->alpha_short > 0) {      /* F42: levels toward Alpha Strike */
        static const float cap_v[4] = { 0.0f, 0.2f, 0.5f, 0.7f }, last_v[4] = { 0.0f, 0.2f, 0.6f, 0.8f };
        return 0.5f + cap_v[b->skill & 3] + (me->alpha_short == 1 ? last_v[b->skill & 3] : 0.0f);
    }
    return 0.5f;
}

/* F18: what a gunship's tether can reach. The hook takes what lies some 5 m under the hull (at
 * 9 m it found nothing) and the hull goes no lower than 3 m off the ground, so a power-up sunk
 * more than ~1.5 m below the ground beside it (a pit, under a ledge) cannot be hooked; and a spot
 * where a hook was tried and nothing came up is left alone for 5 minutes by the whole team. The
 * first failed hook gives the job up (no 25 s on each pickup in turn) and fetching rests 30 s.
 * Switch: env RECOMP_BOT_VTOL_FETCH_FIX=0/1 over [Bots] VtolFetchFix (batch 1: on). */
static int vtol_fetch_fix_on(void) { return c7_on("f18", "RECOMP_BOT_VTOL_FETCH_FIX", "VtolFetchFix", 1); }
static struct { Ma2Vec3 pos; double until; int nation; } g_vt_noreach[16];
static int vtol_can_hook(const Ma2Brain *b, const Ma2PlayerView *me, const Ma2PickupView *pk)
{
    int ok = 0, j;
    if (!vtol_fetch_fix_on()) return 1;
    for (j = 0; j < 16; j++)
        if (b->now < g_vt_noreach[j].until && g_vt_noreach[j].nation == me->nation && hdist(g_vt_noreach[j].pos, pk->pos) < 4.0f) return 0;
    float g = ma2_bot_ground_y(pk->pos.x, pk->pos.z, &ok);
    if (ok && maxf(5.0f, g + 3.0f - pk->pos.y) > 6.5f) { c7_add("f18", "skipped_low", 1); return 0; }
    return 1;
}
static void vtol_noreach_add(const Ma2Brain *b, const Ma2PlayerView *me, Ma2Vec3 p)
{
    int j, old = 0;
    for (j = 0; j < 16; j++) if (g_vt_noreach[j].until < g_vt_noreach[old].until) old = j;
    g_vt_noreach[old].pos = p; g_vt_noreach[old].until = b->now + 300.0; g_vt_noreach[old].nation = me->nation;
}

/* F66: a seen enemy within 80 m of this pickup, farther from it than we are (d), that wants it badly:
 * below half health for armour, one level short of Alpha Strike for a weapon upgrade. */
static int pickup_denies(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2PickupView *pk, float d)
{
    for (int j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
        const Mem *m = &b->mem[j];
        float de;
        if (!m->valid || m->nation == me->nation || m->kind == MA2_KIND_PILOT || mem_conf(b, m) < 0.6f) continue;
        de = hdist(m->pos, pk->pos);
        if (de > 80.0f || de <= d) continue;
        if (pk->armor > 0) { if (m->hp_max > 0.0f && m->hp < 0.5f * m->hp_max) return 1; continue; }
        for (int q = 0; q < w->nplayers && q < MA2_WORLD_MAX_PLAYERS; q++)
            if (w->players[q].actor == m->actor) { if (w->players[q].alpha_short == 1) return 1; break; }
    }
    return 0;
}

static int nearest_powerup(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *dout)
{
    int i, best = -1, deny = -1;
    float bd = 1e9f, brank = 0.0f, dd = 1e9f;
    int deny_ok = pickup_deny_on() && (b->skill & 3) >= MA2_SKILL_VETERAN && kind_ok(me->kind) != MA2_KIND_VTOL;
    for (i = 0; i < w->npickups && i < MA2_BOT_MAX_PICKUPS; i++) {
        float d, de = 1e9f, val; int j;
        if (w->pickups[i].kind != MA2_PICKUP_POWERUP) continue;
        for (j = 0; j < 16; j++)
            if (b->now < g_pu_taken[j].until && (g_pu_taken[j].nation == me->nation || !pickup_need_on()) && hdist(g_pu_taken[j].pos, w->pickups[i].pos) < 3.0f) break;
        if (j < 16) continue;                                               /* collected, not back yet */
        for (j = 0; j < 4; j++) if (b->pu_bad[j].ent == w->pickups[i].entity && b->now < b->pu_bad[j].until) break;
        if (j < 4) { c7_add("pugiveup", "skipped", 1); continue; }          /* PuGiveUp: the planner found no route there for this bot */
        val = powerup_value(b, me, &w->pickups[i]);
        if (kind_ok(me->kind) == MA2_KIND_VTOL) {          /* a ferry: worth what it is to us or to the teammate we would give it to */
            for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *q = &w->players[j];
                if (q == me || !q->alive || q->nation != me->nation) continue;
                if (q->kind != MA2_KIND_MECH && q->kind != MA2_KIND_TANK && q->kind != MA2_KIND_BATTLEARMOR) continue;
                val = maxf(val, powerup_value(b, q, &w->pickups[i]));
            }
            if (val > 0.0f && !vtol_can_hook(b, me, &w->pickups[i])) continue;
        }
        if (val <= 0.0f) { c7_add("pickups", "skipped_unusable", 1); continue; }   /* the game would not give it to anyone it is for */
        d = hdist(me->pos, w->pickups[i].pos);
        for (j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {        /* not one an enemy is sitting on */
            const Mem *m = &b->mem[j];
            if (m->valid && mem_conf(b, m) > 0.4f) de = minf(de, hdist(m->pos, w->pickups[i].pos));
        }
        if (de < d * 0.8f) {
            if (!pickup_hold_on() || b->goal != G_POWERUP || w->pickups[i].entity != b->pu_keep) continue;
            if (de < 12.0f || de < d * 0.4f) { c7_add("f26", "drops_enemy_on_it", 1); continue; }
            c7_add("f26", "holds", 1);
        }
        if (kind_ok(me->kind) == MA2_KIND_VTOL && ma2_bot_air_blocked(w->pickups[i].pos.x, w->pickups[i].pos.z)) continue;   /* out of the VTOL's reach */
        if (ground_cut(me, w->pickups[i].pos)) { c7_add("groundreach", "pickups_cut", 1); continue; }   /* not reachable on foot from here */
        if (reach_cut(b, me, w->pickups[i].pos)) { c7_add("reachset", "pickups_cut", 1); continue; }    /* ReachSet: outside what this body's last search reached */
        for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {   /* a teammate is closer: his, if he can use it */
            const Ma2PlayerView *q = &w->players[j];
            if (q == me || !q->alive || q->nation != me->nation || q->kind == MA2_KIND_PILOT) continue;
            if (kind_ok(me->kind) == MA2_KIND_VTOL) continue;                 /* a ferry fetches for them anyway */
            if (hdist(q->pos, w->pickups[i].pos) + 10.0f < d && powerup_value(b, q, &w->pickups[i]) > 0.0f) break;
        }
        if (j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS) continue;
        if (deny_ok && d < dd && d < 150.0f && pickup_denies(b, w, me, &w->pickups[i], d)) { dd = d; deny = i; }
        {   /* the one worth most for the trip (all worth the same: the nearest, as before) */
            float rank = val / (1.0f + d / 60.0f);
            if (rank > brank) { brank = rank; bd = d; best = i; }
        }
    }
    b->pu_deny = 0;
    if (deny >= 0) { b->pu_deny = w->pickups[deny].entity; best = deny; bd = dd; c7_add("f66", k_f66_seen[b->skill & 3], 1); }
    if (dout) *dout = bd;
    return best;
}

/* RideIntent: this VTOL leaves request i -- its bot BattleArmor has not wanted the ride for 2 s (lift_intent_gone) */
static void ride_intent_drop(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, int i)
{
    int mine = g_lift[i].vtol == me->actor;
    if (b->goal == G_LIFT && b->lift_ba == g_lift[i].ba) {
        const Ma2PlayerView *q = player_by_actor(w, g_lift[i].ba);
        c7_add("rideintent", "ended", 1);
        if (diag_on()) fprintf(stderr, "[BOT-RIDEINTENT] t=%.1f bot=%d VTOL %08X ends the pick-up of BattleArmor %08X %.0f m off: no ride intent for %.1f s\n",
                               b->now, b->index, me->actor, g_lift[i].ba, q ? hdist(q->pos, me->pos) : -1.0f, g_lift[i].want_t > 0.0 ? b->now - g_lift[i].want_t : -1.0);
        b->lift_ba = 0;
    }
    if (mine) { g_lift[i].vtol = 0; c7_add("rideintent", "claims_dropped", 1); }
}

static void evaluate(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, int self, const Ma2Senses *sn)
{
    float s[G_COUNT];
    int k = kind_ok(me->kind), g, best;
    float hpf = hp_frac(me->hp, me->hp_max);
    Mem *t = b->tgt;
    float dt = t ? hdist(me->pos, t->pos) : 1e9f;
    int drivable = k == MA2_KIND_MECH || k == MA2_KIND_TANK || k == MA2_KIND_BATTLEARMOR || k == MA2_KIND_VTOL;
    (void)self; (void)sn;
    memset(s, 0, sizeof s);
    s[G_ROAM] = 0.2f;
    pair_update(b, w, me);   /* CtfPair: plan / follow the opening VTOL + BattleArmor pair */

    if (k != MA2_KIND_PILOT) b->lms_out = 0;          /* LmsNoRide: in a vehicle after all -- not out */
    if (k == MA2_KIND_PILOT) {
        float vs; int vi;
        if (b->foot_since <= 0.0) {
            /* New spell on foot (match start, respawn, eject): this life's vehicle wish -- a 'Mech,
             * now and then a tank. BattleArmor and VTOLs are what a bot takes when nothing stronger
             * is free close by, never what it sets out for. */
            float f = rng_f(&b->rng);
            b->foot_since = b->now;
            b->c3_foot_counted = 0;
            b->fav_kind = f < 0.15f ? MA2_KIND_TANK : MA2_KIND_NONE;
            { const char *fe = getenv("RECOMP_BOT_FAV_KIND"); if (fe && *fe) b->fav_kind = atoi(fe); }
            {   /* test hook: RECOMP_BOT_FAV_KINDS="5,3,3,0,..." -- a wish per bot index (0 = its own) */
                const char *fl = getenv("RECOMP_BOT_FAV_KINDS");
                for (int fi = 0; fl && *fl; fi++) { if (fi == b->index) { int v = atoi(fl); if (v > 0) b->fav_kind = v; break; } fl = strchr(fl, ','); if (fl) fl++; }
            }
        }
        /* No vehicle for a minute: fight on foot for 20 s before looking again. */
        /* WP2-C FootProgress (default on, RECOMP_BOT_FOOT_PROGRESS / [Bots] FootProgress): not while the walk to a ride is making headway
         * (sweep 11-02: pilots walked 30-40 s to a tank, were cut off at the 60 s mark a step from it and spent the next 20 s hunting on foot) */
        if (b->now >= b->foot_until && b->now - b->foot_since > 60.0 &&
            !(b->goal == G_GET_VEHICLE && b->veh && b->now - b->fp_move_t < 3.0 && c7_on("footprog", "RECOMP_BOT_FOOT_PROGRESS", "FootProgress", 1))) { b->foot_until = b->now + 20.0; b->foot_since = b->now + 20.0; }
        /* F14 (switch FootRide): on foot against nothing but pilots, shooting gets nowhere (a pilot
         * barely hurts a pilot): fetch a ride, from further off, before the fight. */
        b->veh_wide = t && t->kind == MA2_KIND_PILOT && ma2_bot_switch_on(MA2_SW_FOOT_RIDE);
        /* LmsNoRide (default on, RECOMP_BOT_LMS_NO_RIDE / [Bots] LmsNoRide): Last Man Standing is one life -- a pilot back after its
         * vehicle was destroyed is offered no ride (au9_33 LMS Proving Ground, 11-04: respawned pilots 0 prompts in 14 zone tries, 893 s of
         * walks to rides; the 3 boardings that worked were pilots ejected alive; the In Your Face "BA refuses every pilot" is the same).
         * Such a pilot does not go for vehicles; it fights or roams on foot. [C7] lmsnoride: out, skipped_s. */
        if (lms_on(w) && c7_on("lmsnoride", "RECOMP_BOT_LMS_NO_RIDE", "LmsNoRide", 1)) {
            if (me->deaths > b->lms_deaths) { b->lms_deaths = me->deaths; b->lms_out = 1; c7_add("lmsnoride", "out", 1); }
        }
        vi = b->now >= b->foot_until && !b->lms_out ? pick_vehicle(b, w, me, &vs) : -1;
        if (b->lms_out) c7_add("lmsnoride", "skipped_s", b->dt);
        if (vi >= 0) s[G_GET_VEHICLE] = hdist(me->pos, w->vehicles[vi].pos) < 25.0f ? 1.3f : 0.95f;
        if (vi >= 0 && b->veh_wide) {
            s[G_GET_VEHICLE] = maxf(s[G_GET_VEHICLE], 1.0f);
            if (!b->c3_foot_counted) { b->c3_foot_counted = 1; ma2_c3_count(MA2_C3_FOOT_GETVEH); }
        }
        if (t && (t->kind == MA2_KIND_PILOT || t->kind == MA2_KIND_BATTLEARMOR) && dt < 50.0f) s[G_ATTACK] = 0.7f;
        else if (t && vi < 0) s[G_ATTACK] = 0.45f;
        if (b->n_attackers > 0 && vi < 0) s[G_RETREAT] = 0.8f;        /* no ride in reach: get out of their guns */
        /* LmsNoRide, out: no ride is coming, so it does not roam or hunt across the map (au12_33: out pilots on the Proving Ground start ledges
         * walked at edges they cannot get down -- 42 of 72 pilot AiKicks at y 205); it stays hidden (the guide: hiding is viable in LMS) and
         * fights only what comes within 60 m. [C7] lmsnoride: hold_s. */
        if (b->lms_out) { s[G_ROAM] = 0.0f; if (!(t && dt < 60.0f)) s[G_ATTACK] = 0.0f; c7_add("lmsnoride", "hold_s", b->dt); }
    } else if (drivable) {
        int sw;
        float thr = b->sk.retreat_hp + b->caution + (k == MA2_KIND_BATTLEARMOR ? 0.05f : k == MA2_KIND_VTOL ? 0.15f : 0.0f);   /* a gunship breaks off early */
        int lms = lms_on(w), lms_end = lms && lms_opponents(w, me) <= 2;
        if (lms && !lms_end) { static const float safer[4] = { 0.03f, 0.08f, 0.12f, 0.15f }; thr += safer[b->skill & 3]; }   /* F44 */
        b->foot_since = 0.0;
        if (k != MA2_KIND_BATTLEARMOR && (me->qte_active || me->enemy_rider)) s[G_DEFEND_HACK] = 2.2f;
        if (k == MA2_KIND_BATTLEARMOR && me->qte_active) s[G_HACK] = 2.2f;     /* our own hack is running */
        if (!ma2_brain_kind_allowed(k, w->game_type) && !me->qte_active) s[G_EJECT] = 3.0f;
        if (b->now < b->bail_until && !me->qte_active) s[G_EJECT] = 3.0f;          /* trapped: get out */
        /* HackEject (default on, RECOMP_BOT_HACK_EJECT / [Bots] HackEject): a won NeuroHack (sub_C6A62(1)) only SETS the victim pilot's eject
         * request (MAPlayer +0x244 bit 15); what acts on it for a human is not found, and nothing did for a bot: r4_85 all 3 BattleArmor hacks
         * completed (f19 hacks_done=3, [BOT-HACKED] 75.2 / 162.6 / 231.8) and every victim drove on in its 'Mech, the hacker shot off at 9-29 m.
         * The bit lives one frame (the game's handler clears it without ejecting a bot), so core stamps the completion (ma2_bot_hack_eject)
         * and me->hack_eject holds 8 s: the bot ejects then. [C7] hackeject: bot_victims, eject_s. */
        if (me->hack_eject && (k == MA2_KIND_MECH || k == MA2_KIND_TANK) && !me->qte_active && c7_on("hackeject", "RECOMP_BOT_HACK_EJECT", "HackEject", 1)) {
            if (s[G_EJECT] < 3.5f) c7_add("hackeject", "eject_s", b->dt);
            s[G_EJECT] = 3.5f;
        }
        /* F37 (switch RECOMP_BOT_EJECT_SAVE / [Bots] EjectSave, default on): a 'Mech or tank about to
         * blow up under fire -- bail out to save the pilot, as a human does, where the pilot's life is
         * worth it: one-life modes, or a free ride within 150 m to run to. Rolled once per vehicle:
         * an Elite nearly always does it, a Recruit one time in four (the rest ride it to the end).
         * EjectEarly (playtest 11-05): at 25 % now, not 12 %. The pilot is locked 5-7 s on its wreck after the exit and the hull's death blast
         * (2.6 s after 0 hp) went off inside that lock: 8/8 (r2_29), 7/9 (au19_211), 8/8 (au19_41), 12/12 (r3_213) pilots died. */
        if ((k == MA2_KIND_MECH || k == MA2_KIND_TANK) && !me->qte_active && b->n_attackers > 0 &&
            hp_frac(me->hp, me->hp_max) < 0.25f && c7_on("f37-eject", "RECOMP_BOT_EJECT_SAVE", "EjectSave", 1)) {
            /* LmsBailLate (default on, RECOMP_BOT_LMS_BAIL_LATE / [Bots] LmsBailLate): in a one-life mode the bail-out was rolled at 25 % whatever
             * the fire: r5_46b b7 ejected with 754 hp left, 14 dps coming in, the nearest enemy 256 m off, then walked 19 s to a hull that was
             * gone and died on foot; all 7 LMS ejected pilots died 3.5-22.9 s after. There the roll waits until the hull has < 8 s to live. */
            int lms_wait = (w->game_type == MA2_GT_LMS || w->game_type == MA2_GT_TEAM_LMS) && b->f37_veh != me->actor &&
                           !(b->in_dps > 1.0f && me->hp / b->in_dps < 8.0f) && c7_on("lmsbaillate", "RECOMP_BOT_LMS_BAIL_LATE", "LmsBailLate", 1);
            if (lms_wait) c7_add("lms", "bail_wait_s", b->dt);
            if (b->f37_veh != me->actor && !lms_wait) {
                static const float p_sk[4] = { 0.25f, 0.5f, 0.8f, 0.95f };
                int life = w->game_type == MA2_GT_LMS || w->game_type == MA2_GT_TEAM_LMS, ride = 0, j;
                for (j = 0; j < w->nvehicles && j < MA2_BOT_MAX_VEHICLES && !life && !ride; j++) {
                    const Ma2VehicleView *v = &w->vehicles[j];
                    if (v->actor != me->actor && vehicle_usable(b, w, me, v) && hdist(me->pos, v->pos) < 150.0f) ride = 1;
                }
                b->f37_veh = me->actor;
                b->f37_go = (life || ride) && rng_f(&b->rng) < p_sk[b->skill & 3];
                c7_add("f37-eject", b->f37_go ? (life ? "bail_life" : "bail_ride") : (life || ride) ? "rode_on" : "no_reason", 1);
                if (life || ride) c7_add("f37-eject", k_c7_f37c_sk[b->skill & 3], 1);   /* eject rate per skill = bail_<sk> / chances_<sk> */
                if (b->f37_go) c7_add("f37-eject", k_c7_f37b_sk[b->skill & 3], 1);
                if (diag_on()) fprintf(stderr, "[BOT-F37] t=%.1f bot=%d sk=%d hp=%.2f attackers=%d life=%d ride=%d -> %s\n", b->now, b->index,
                                       b->skill, hp_frac(me->hp, me->hp_max), b->n_attackers, life, ride, b->f37_go ? "EJECT" : "stay");
            }
            if (b->f37_go && b->f37_veh == me->actor) {   /* (this vehicle's roll: not a stale one while LmsBailLate waits) */
                s[G_EJECT] = 3.0f;
                /* a 25 % hull stays a usable ride (DyingRide skips under 15 %): not straight back into it, nor out of it again */
                b->left_veh = me->actor; b->left_until = b->now + 60.0;
            }
        }
        /* Trade up: a dying vehicle -- or BattleArmor -- for a better free one close by. */
        sw = me->qte_active || getenv("RECOMP_BOT_NO_SWAP") ? -1 : pick_swap(b, w, me);   /* NO_SWAP: test hook */
        if (sw >= 0) {
            /* BattleArmor next to a free 'Mech with no one shooting at it: go and take it now
             * (Grinder starts everyone in BattleArmor with the 'Mechs parked at the spawn). */
            float sd = hdist(me->pos, w->vehicles[sw].pos);
            s[G_SWAP] = k == MA2_KIND_BATTLEARMOR ? (sd < 60.0f && b->n_attackers == 0 ? 1.25f : 0.95f) : 1.05f;
            /* NotItSwap (default on, RECOMP_BOT_NOTIT_SWAP / [Bots] NotItSwap): Not It is one long chase; a swap there walked away from It and was
             * dropped within 1-3 s (d5_55: 25 of 30 swap-go spans aborted, 5 ejects, 4 boardings; b8 as It swapped and died a 10-hp pilot). Only a
             * ride within 40 m, and never for It. [C7] notit: swap_skipped. */
            if (w->game_type == MA2_GT_NOTIT && (sd > 40.0f || notit_it_on(w, me)) && c7_on("notitswap", "RECOMP_BOT_NOTIT_SWAP", "NotItSwap", 1)) {
                s[G_SWAP] = 0.0f; c7_add("notit", "swap_skipped", 1);
            }
            if (k == MA2_KIND_BATTLEARMOR && w->game_type == MA2_GT_GRINDER && ma2_bot_switch_on(MA2_SW_GRINDER_SWAP)) {
                s[G_SWAP] = 1.4f;   /* F12: in Grinder the 'Mech first, whoever is shooting */
                if (b->c3_grinder_veh != w->vehicles[sw].actor) { b->c3_grinder_veh = w->vehicles[sw].actor; ma2_c3_count(MA2_C3_GRINDER_SWAPS); }
            }
            b->swap_veh = w->vehicles[sw].actor; b->swap_until = b->now + 25.0;
        }
        if (t) {
            float fr = clampf(0.6f + 0.4f * b->force, 0.5f, 1.2f);
            s[G_ATTACK] = (mem_acquired(b, t) || b->goal == G_ATTACK ? 0.55f + 0.35f * b->aggr : 0.35f) * fr;
            if (reach_cut(b, me, t->pos)) { s[G_ATTACK] *= 0.35f; c7_add("reachset", "attack_damped", 1); }   /* ReachSet: cannot walk there from here, hunt / roam first */
            /* FarAir (default on, RECOMP_BOT_FAR_AIR / [Bots] FarAir; parity pass 8): a ground body cannot close on a gunship, so an attack goal on a VTOL
             * far off was a stand-off duel from where it stood -- Q_73a (No Man's Land Check It) T2 b5 / b6 / b9 parked 300-460 m from T1's VTOLs over
             * the middle (32-53 s, VTOLs not on them, ~9 s each under 3 m/s) in the pass under the mesa, and b7 on its way to the terminal replanned round
             * them 10 times (T2's ~32 s loss there; T1 did the same on W_73a). A player keeps going and shoots it on the move (the travel / roam legs
             * fire at the target in reach). Still a duel when it is within 150 m or on us (attacking us, or hit us in the last 5 s). [C7] farair: s. */
            if (t->kind == MA2_KIND_VTOL && (k == MA2_KIND_MECH || k == MA2_KIND_TANK || k == MA2_KIND_BATTLEARMOR) && dt > 150.0f &&
                !t->attacking_me && b->now - t->hit_me_t > 5.0 && c7_on("farair", "RECOMP_BOT_FAR_AIR", "FarAir", 1)) {
                s[G_ATTACK] = 0.0f; c7_add("farair", "s", MA2B_EVAL_PERIOD);
            }
            /* F80: a running peek-and-hide is a fight in progress: other errands don't pull us out of it. */
            if (b->goal == G_ATTACK && t->actor == b->peek_tgt && b->now < b->peek_end && c7_on("f80", "RECOMP_BOT_PEEK_HOLD", "BotPeekHold", 1)) {
                s[G_ATTACK] += 0.3f; c7_add("f80", "hold_s", b->dt);
            }
        }
        /* Outnumbered: take cover (skill decides whether the bot thinks of it). */
        /* surprise (COLD streak): a hurt bot under fire takes cover earlier */
        if (b->sp.mood == 2 && t && k != MA2_KIND_VTOL && b->n_attackers > 0 && hpf < 0.75f && b->now >= b->tac_fail_until) s[G_COVER] = maxf(s[G_COVER], 0.92f);
        if (b->outnumbered && k != MA2_KIND_VTOL && b->now >= b->tac_fail_until &&
            (!f7_on() || b->n_attackers >= 3 || b->in_dps > 2.0f * b->my_dps)) {
            if (f7_on() && b->now - b->cover_roll_t > 8.0) c7_add("F7elite", "cover_rolls", 1);
            if (b->now - b->cover_roll_t > 8.0) { b->cover_roll = rng_f(&b->rng) < b->sk.cover_use; b->cover_roll_t = b->now; }
            if (b->cover_roll) s[G_COVER] = 0.95f;
        }
        /* A 'Mech up on a building under fire: drop off the far side, the building between it and
         * the guns, rather than trade shots from the skyline. */
        if (k == MA2_KIND_MECH && b->raised && b->in_dps > 10.0f && b->now - b->last_hit < 1.5 && b->now >= b->tac_fail_until)
            s[G_COVER] = maxf(s[G_COVER], 1.0f);
        /* Losing with low health: fall back (VTOLs climb away, the rest break line of sight). */
        /* RetreatHold (default on, RECOMP_BOT_RETREAT_HOLD / [Bots] RetreatHold): the low-hp fall-back held only while an attacker was on it, so
         * a lull turned it back to attack and the next look round again -- d5_90 b3 128.5-163.4 14 flips in 35 s at 18-31 % (n_attackers 0 <-> 1,
         * hits 8-19 s old), b4 4, b7 3. Once falling back, it keeps going 4 s past the last moment an attacker was on it. [C7] retreathold: s. */
        static double rh_att_t[16];
        int rhx = b->index & 15;
        if (b->n_attackers > 0 || b->losing || b->now < rh_att_t[rhx]) rh_att_t[rhx] = b->now;
        if (hpf < thr && (b->n_attackers > 0 || b->losing) && b->now >= b->tac_fail_until) {
            s[G_RETREAT] = 1.1f;
            if (lms) c7_add("lms", "retreat_s", b->dt);
        }
        else if (hpf < thr && b->goal == G_RETREAT && rh_att_t[rhx] > 0.0 && b->now - rh_att_t[rhx] < 4.0 && b->now >= b->tac_fail_until &&
                 c7_on("retreathold", "RECOMP_BOT_RETREAT_HOLD", "RetreatHold", 1)) {
            s[G_RETREAT] = 1.1f; c7_add("retreathold", "s", b->dt);
        }
        /* SizeUp (default on, RECOMP_BOT_SIZE_UP / [Bots] SizeUp; playtest 11-05): a player who rounds a corner onto five enemies with no one
         * beside him turns back and picks a better place to fight -- before the first hit. The bots only broke off on their own health
         * (a VTOL at ~31 %), so under five guns they died first: a playtester's TD game 11-05, b7's VTOL 790 -> 0 hp in 10 s at 4-6 attackers,
         * b5's 690 -> 0 in 10 s. Now the odds are sized up from what the bot sees or saw in the last 10 s within 200 m (their threat x
         * health) against itself and the teammates within 120 m: outnumbered by two or more bodies and under 0.55 of their strength, it
         * falls back from their centre (held 8 s while they are still there; one fall-back 10 s at most, then 12 s back in with whoever
         * came up before the odds can turn it again), a VTOL under 90 % home to its repairs. Backstop for what it did not
         * size up: hit and losing with under ~6 s to live at this rate. The skill sets how surely it notices (a Recruit charges in more
         * often). Not with an objective, a hack on us, or two left in LMS. [C7] sizeup: odds_<sk> / ttl / s. */
        int sz_flee = 0;   /* (the gunship below: home to its repairs rather than a 60 m hop it dies in -- sz2_20 b1 760 -> 10 hp "retreating") */
        if (c7_on("sizeup", "RECOMP_BOT_SIZE_UP", "SizeUp", 1) && !lms_end && b->goal != G_OBJECTIVE && !me->qte_active && !me->enemy_rider) {
            static double flee_until[16], roll_t[16], flee_start[16], cool_until[16], seen_now[16]; static int roll_ok[16];
            int bi = b->index & 15, ne = 0, nf = 1;
            if (b->now < seen_now[bi]) { flee_until[bi] = roll_t[bi] = flee_start[bi] = cool_until[bi] = 0.0; roll_ok[bi] = 0; }   /* a new match: the clock started over */
            seen_now[bi] = b->now;
            float est = 0.0f, fst = k_kind[kind_ok(k)].threat * hpf, rng_ = k == MA2_KIND_VTOL ? 220.0f : 200.0f;
            Ma2Vec3 ec = v3(0, 0, 0);
            for (int j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Mem *m = &b->mem[j];
                if (!m->valid || m->kind == MA2_KIND_PILOT || k_kind[m->kind].threat <= 0.0f || b->now - m->last_seen > 10.0 || mem_conf(b, m) < 0.4f) continue;
                if (hdist(me->pos, m->pos) > rng_) continue;
                /* SizeUpFFA: in a free-for-all every other player is an 'enemy seen', so the odds were always bad (audit r4_50c: 88 fall-backs, b10 at
                 * 87 % with nothing on it) -- there only the ones with us in their sights count */
                if (!w->team_mode) { const Ma2PlayerView *q = player_by_actor(w, m->actor); if (!q || q->attacking != me->actor) continue; }
                ne++; ec = vadd(ec, m->pos);
                est += k_kind[m->kind].threat * hp_frac(m->hp, m->hp_max > 0.0f ? m->hp_max : 1.0f) * mem_lvl_mult(b, m);
            }
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *p = &w->players[j];
                if (p == me || !p->alive || p->nation != me->nation || p->kind == MA2_KIND_PILOT || hdist(p->pos, me->pos) > 120.0f) continue;
                nf++; fst += k_kind[kind_ok(p->kind)].threat * hp_frac(p->hp, p->hp_max);
            }
            int bad = ne >= nf + 2 && est > 0.0f && fst < 0.55f * est;
            if (bad && b->now - roll_t[bi] > 6.0) { static const float notice[4] = { 0.35f, 0.65f, 0.9f, 1.0f }; roll_t[bi] = b->now; roll_ok[bi] = rng_f(&b->rng) < notice[b->skill & 3]; }
            float ttl = b->in_dps > 1.0f ? me->hp / b->in_dps : 1e9f;
            static const float ttl_sk[4] = { 3.0f, 4.5f, 5.5f, 6.5f };
            int dying = b->n_attackers > 0 && (b->outnumbered || b->losing) && ttl < ttl_sk[b->skill & 3] + (k == MA2_KIND_VTOL ? 2.5f : 0.0f);
            /* one fall-back is 10 s at most; then 12 s back in it (with whoever came up) before the odds turn it round again -- holding off
             * while the crowd stays in memory kept full-health bots out of the game for 20 s+ (sz1_20). The ttl backstop is never held off. */
            if (b->now < flee_until[bi] && b->now - flee_start[bi] > 10.0 && !dying) { flee_until[bi] = 0.0; cool_until[bi] = b->now + 12.0; c7_add("sizeup", "back_in", 1); }
            if ((bad && roll_ok[bi] && b->now >= cool_until[bi]) || dying) {
                static const char *const odds_sk[4] = { "odds_s0", "odds_s1", "odds_s2", "odds_s3" };
                if (b->now >= flee_until[bi]) { flee_start[bi] = b->now; c7_add("sizeup", dying && !(bad && roll_ok[bi]) ? "ttl" : odds_sk[b->skill & 3], 1); }
                flee_until[bi] = b->now + 8.0;
            }
            /* SizeUpHold (default on, RECOMP_BOT_SIZEUP_HOLD / [Bots] SizeUpHold): the fall-back held only while the odds still read bad this
             * step, and a teammate stepping in and out of the 120 m ring flipped it: d5_25 b7 83-124 retreat <-> attack 5 times at 94-86 %
             * (-700 hp, 0 dealt), b3 4 flips in 7 s. Once falling back it stays 2.5 s past the last bad reading (the 8 s / 10 s caps above stand). */
            static double sz_bad_t[16];
            int sz_bad = ne >= nf + 1 || b->n_attackers > 0;
            if (sz_bad || b->now < sz_bad_t[bi]) sz_bad_t[bi] = b->now;
            int sz_hold = !sz_bad && b->now - sz_bad_t[bi] < 2.5 && c7_on("sizeuphold", "RECOMP_BOT_SIZEUP_HOLD", "SizeUpHold", 1);
            if (b->now < flee_until[bi] && sz_hold) c7_add("sizeup", "hold_s", b->dt);
            if (b->now < flee_until[bi] && (sz_bad || sz_hold)) {
                s[G_RETREAT] = maxf(s[G_RETREAT], 1.2f);
                sz_flee = 1;
                if (ne && b->n_attackers == 0) {   /* not hit yet: no rays to hide from -- straight back away from the crowd we saw */
                    b->threat_c = vscale(ec, 1.0f / (float)ne);
                    if (b->tac_kind != G_RETREAT || b->now >= b->tac_until) {
                        Ma2Vec3 away = hnorm(vsub(me->pos, b->threat_c));
                        b->tac_pt = vadd(me->pos, vscale(hlen(away) > 0.1f ? away : dir_of_heading(me->yaw + MA2B_PI), 70.0f));
                        /* SizeUpReach (default on, RECOMP_BOT_SIZEUP_REACH / [Bots] SizeUpReach): the point straight away from the crowd was taken
                         * blind -- r4_50 b7 (It) jetted at the pit's NE wall at speed 0 for 6.6 s toward a point 87 m behind it, b9 the same on the
                         * east wall: both died there. Take the first of away, +-35, +-70 deg whose point the route can reach (reach set), else
                         * one in the open; failing all, the old point. [C7] sizeup: reach_turned, reach_none. */
                        if (c7_on("sizeupreach", "RECOMP_BOT_SIZEUP_REACH", "SizeUpReach", 1)) {
                            static const float turn[5] = { 0.0f, 35.0f, -35.0f, 70.0f, -70.0f };
                            float h0 = hlen(away) > 0.1f ? atan2f(away.x, away.z) : me->yaw + MA2B_PI;
                            int got = -1;
                            for (int q = 0; q < 5 && got < 0; q++) {
                                Ma2Vec3 p = vadd(me->pos, vscale(dir_of_heading(h0 + turn[q] * MA2B_DEG), 70.0f));
                                int r = ma2_path_reach_has(b->index, me->pos, p, b->now);
                                if (r == 1 || (r < 0 && ma2_trav_ready() && ma2_trav_clear(p.x, p.z, b->now) >= 4.0f)) { got = q; b->tac_pt = p; }
                            }
                            c7_add("sizeup", got > 0 ? "reach_turned" : got < 0 ? "reach_none" : "reach_ok", 1);
                        }
                        b->tac_kind = G_RETREAT; b->tac_until = b->now + 4.0;
                    }
                }
                c7_add("sizeup", "s", b->dt);
            }
        }
        if (t && s[G_ATTACK] > 0.0f && notit_it_on(w, me)) { s[G_ATTACK] += 0.25f; c7_add("notit", "it_hunt_s", b->dt); }
        /* F44: two or fewer left standing: go and finish it. */
        if (lms_end && t && s[G_ATTACK] > 0.0f) { s[G_ATTACK] += 0.2f; c7_add("lms", "endgame_s", b->dt); }
        /* Alpha Strike: after our own the 'Mech is overheated and cannot shoot back -- out of their
         * sight until it cools; someone else's charging at us -- out of its line before it lets go. */
        if (k == MA2_KIND_MECH && b->now < b->alpha_cool_until && b->now >= b->tac_fail_until) s[G_RETREAT] = maxf(s[G_RETREAT], 1.15f);   /* falls back from them when no cover is close */
        /* A badly damaged gunship goes for repairs (the skill sets how soon, and Recruits rarely think of
         * it): home to its base's helipad, where it sets down and the pad's prompt (A) repairs it fully --
         * unless a health pack lies close to the way with no enemy near it: skimmed through low, and if
         * that is enough to fight on, back to the fight. Now and then, coming in to the pad, it changes
         * ride instead: a 'Mech or a tank parked near the pad, if a free one stands there. */
        if (k == MA2_KIND_VTOL) {
            static const float heal_at[4] = { 0.20f, 0.30f, 0.40f, 0.45f }, swap_p[4] = { 0.15f, 0.25f, 0.30f, 0.35f };
            float at = heal_at[b->skill & 3];
            { const char *e = getenv("RECOMP_BOT_VTOL_HEAL_AT"); if (e) at = (float)atof(e); }   /* test hook */
            /* SizeUp: hurt and outgunned -- home to the pad. Was any scratch (90 %): an Elite gunship broke off a duel it was winning at 90 % and
             * flew 400-800 m to a pad (audit r4_83 b2 56.3, r4_85 b4/b7), out of the fight for a minute. Now under 60 % (Dragoon 50 %). */
            /* VtolHome2v1 (audit d5_211): with two or more on it a gunship sat 12-21 s in the 2-on-1 and went home at 20-49 %, dying on the way --
             * outnumbered, it breaks off at 75 % (Dragoon 70 %); one-on-one it fights on to 60 / 50 % */
            float home_at = b->n_attackers >= 2 ? (ma2_bots_dragoon() ? 0.70f : 0.75f) : (ma2_bots_dragoon() ? 0.5f : 0.6f);
            if (sz_flee && hpf < home_at) { at = maxf(at, home_at); c7_add("sizeup", "vtol_home", b->dt); }
            int going = b->goal == G_REPAIR;
            /* a pad run repairs fully; after a pack it is back to the fight once fit enough */
            float until = b->repair_kind == 2 ? minf(at + 0.25f, 0.8f) : 0.97f;
            if (!going) { b->repair_swap_rolled = 0; }
            /* changing ride: see it through -- and once the roll on the way in has picked one, keep to it
             * (scored for that one evaluation only, the run to the pad held on and the roll was lost) */
            if ((b->goal == G_SWAP || (going && b->repair_swap_rolled == 2)) && b->swap_veh && b->now < b->swap_until) s[G_SWAP] = maxf(s[G_SWAP], 1.5f);   /* over the repair run and its hysteresis (1.25 + 0.15) */
            int pads_on = ma2_bot_vtol_pads_on(), padi = -1;
            if ((hpf < at || (going && hpf < until)) && !(b->skill == 0 && !going && rng_f(&b->rng) < 0.5f) && !(pads_on && b->now < b->repair_block_until)) {
                Ma2Vec3 pad; int havepad = pads_on ? pad_usable(b, w, me, &pad, &padi) : ma2_bot_pad_near(w->team_mode && b->home_set ? b->home_pos : me->pos, &pad);
                float padd = havepad ? hdist(me->pos, pad) : 1e9f, bd = 1e9f; int bi = -1;
                for (int i = 0; i < w->npickups && i < MA2_BOT_MAX_PICKUPS; i++) {
                    const Ma2PickupView *pk = &w->pickups[i];
                    if (pk->kind != MA2_PICKUP_POWERUP || pk->armor <= 0) continue;
                    float d = hdist(me->pos, pk->pos);
                    /* on the way: near, and not much of a detour from the road home */
                    if (d > 300.0f || ma2_bot_air_blocked(pk->pos.x, pk->pos.z)) continue;
                    if (havepad && d + hdist(pk->pos, pad) > padd + 250.0f) continue;
                    int hot = 0;
                    for (int j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
                        const Mem *m = &b->mem[j];
                        if (m->valid && mem_conf(b, m) > 0.4f && k_kind[m->kind].dps >= 15.0f && hdist(m->pos, pk->pos) < 120.0f) hot = 1;
                    }
                    if (!hot && d < bd) { bd = d; bi = i; }
                }
                if (bi >= 0) { b->repair_kind = 2; b->repair_pt = w->pickups[bi].pos; b->repair_pack = w->pickups[bi].entity; s[G_REPAIR] = 1.25f; }
                else if (havepad && padd < 2500.0f) { b->repair_kind = 1; b->repair_pt = pad; b->repair_pad = padi; b->repair_pack = 0; s[G_REPAIR] = 1.25f; }
                /* Coming in to the pad: once per run, a chance to change ride instead. */
                if (going && b->repair_kind == 1 && padd < 250.0f && !b->repair_swap_rolled) {
                    b->repair_swap_rolled = 1;
                    float roll = rng_f(&b->rng);
                    { const char *e = getenv("RECOMP_BOT_VTOL_SWAP_P"); if (e) roll = roll < (float)atof(e) ? -1.0f : 2.0f; }   /* test hook */
                    if (diag_on()) fprintf(stderr, "[BOT-REPAIR] t=%.1f bot=%d nearing the pad: change-ride roll %.2f vs %.2f\n", b->now, b->index, roll, swap_p[b->skill & 3]);
                    if (roll < swap_p[b->skill & 3]) {
                        int best = -1; float bv = 0.0f;
                        for (int i = 0; i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES; i++) {
                            const Ma2VehicleView *v = &w->vehicles[i];
                            if ((v->kind != MA2_KIND_MECH && v->kind != MA2_KIND_TANK) || hdist(v->pos, pad) > 150.0f || !vehicle_usable(b, w, me, v)) continue;
                            float val = vehicle_pref_b(b, v->kind) * hp_frac(v->hp, v->hp_max);
                            if (val > bv) { bv = val; best = i; }
                        }
                        if (best >= 0) {
                            b->swap_veh = w->vehicles[best].actor; b->swap_until = b->now + 40.0; s[G_SWAP] = 1.5f; b->repair_swap_rolled = 2;
                            if (diag_on()) fprintf(stderr, "[BOT-REPAIR] t=%.1f bot=%d changes ride at the base: %s %08X\n", b->now, b->index,
                                                   w->vehicles[best].kind == MA2_KIND_MECH ? "'Mech" : "tank", w->vehicles[best].actor);
                        }
                    }
                }
            }
        }
        if (b->now < b->alpha_react_until && k != MA2_KIND_VTOL && b->now >= b->tac_fail_until) s[G_COVER] = maxf(s[G_COVER], 1.12f);
        /* Lifts (see g_lift). */
        if (k == MA2_KIND_BATTLEARMOR && lifts_on()) {
            if (me->riding) { s[G_RIDE] = 1.5f; b->lift_vtol = me->riding; }
            else {
                Ma2Vec3 dest = b->roam_valid ? b->roam_pt : me->pos;
                int far_trip = b->roam_valid && hdist(me->pos, dest) > 250.0f && b->n_attackers == 0 && !t;
                if (getenv("RECOMP_BOT_LIFT_TEST") && b->roam_valid && hdist(me->pos, dest) > 100.0f) far_trip = 1;   /* test hook */
                if (far_trip) {
                    lift_post(me->actor, me->nation, me->pos, dest, b->now);
                    int li = lift_find(me->actor);       /* a VTOL is coming for us: meet it */
                    const Ma2PlayerView *v = li >= 0 && lift_claim_live(li, b->now) ? player_by_actor(w, g_lift[li].vtol) : NULL;
                    if (v && v->alive && hdist(v->pos, me->pos) < 260.0f && !(v->actor == b->lift_off && b->now < b->lift_off_until)) {
                        if (b->goal != G_RIDE) b->lift_since = b->now;
                        if (b->now - b->lift_since > 20.0) { b->lift_off = v->actor; b->lift_off_until = b->now + 30.0; }
                        else { s[G_RIDE] = 1.0f; b->lift_vtol = v->actor; }
                    }
                }
                for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS && far_trip; j++) {
                    const Ma2PlayerView *q = &w->players[j];
                    int ok = 0;
                    /* Already on our way up to this one: looser limits, or a VTOL hovering at about the
                     * height limit made the goal flip ride/hunt every evaluation (27 flips in 9 s). */
                    int going = b->goal == G_RIDE && b->lift_vtol == q->actor;
                    if (!q->alive || q->kind != MA2_KIND_VTOL || q->nation != me->nation || hdist(q->pos, me->pos) > (going ? 90.0f : 60.0f)) continue;
                    float g = sn && sn->ground ? sn->ground(sn->ctx, q->pos.x, q->pos.z, &ok) : q->pos.y;
                    if (q->actor == b->lift_off && b->now < b->lift_off_until) continue;   /* tried that one: no */
                    if (ok && q->pos.y - g < (going ? 40.0f : 25.0f) && hp_frac(q->hp, q->hp_max) > 0.4f) {     /* it is down: climb on */
                        if (b->goal != G_RIDE) b->lift_since = b->now;
                        if (b->now - b->lift_since > 12.0) { b->lift_off = q->actor; b->lift_off_until = b->now + 30.0; break; }
                        s[G_RIDE] = 1.0f; b->lift_vtol = q->actor; break;
                    }
                }
            }
        }
        if (k != MA2_KIND_BATTLEARMOR) b->bapick_took = 0;
        else if (!b->bapick_took && g_bapick[me->nation & 7].who == me->player && g_bapick[me->nation & 7].until > b->now) {
            b->bapick_took = 1; c7_add("bapick", k_c7_bapick_took[b->skill & 3], 1);
        }
        if (k == MA2_KIND_VTOL && lifts_on()) {
            int rider = 0, nr = 0, two = vtol_two_riders_on(), rcar = 0; const Ma2PlayerView *rq2[2] = { NULL, NULL };
            const Ma2ObjectiveView *tf2 = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == me->actor) {
                rider = 1; if (nr < 2) rq2[nr] = &w->players[j];
                nr++; if (tf2 && tf2->carrier && tf2->carrier == w->players[j].player) rcar = 1;
            }
            b->lift_ba = 0;
            {   /* VtolTwoRiders: [C7] two aboard (player + bot); on our own flag run the second rider is the run's second BattleArmor */
                int fn = me->nation & 7;
                if (two && nr >= 2 && b->two_nr < 2) {
                    int hum = rq2[0]->is_human || rq2[1]->is_human;
                    c7_add("tworiders", hum ? "two_aboard_player_bot" : "two_aboard_bots", 1);
                    if (diag_on()) fprintf(stderr, "[BOT-TWORIDERS] t=%.1f bot=%d VTOL %08X carries two: %08X%s + %08X%s (run %s)\n", b->now, b->index, me->actor,
                                           rq2[0]->actor, rq2[0]->is_human ? " (player)" : "", rq2[1]->actor, rq2[1]->is_human ? " (player)" : "", k_fr_name[g_frun[fn].phase % 6]);
                }
                b->two_nr = nr;
                if (two && g_frun[fn].phase && g_frun[fn].phase != FR_COVER && g_frun[fn].vtol == me->actor)
                    for (int r = 0; r < nr && r < 2; r++)
                        if (rq2[r]->actor != g_frun[fn].ba && rq2[r]->actor != g_frun[fn].ba2) {
                            g_frun[fn].ba2 = rq2[r]->actor; c7_add("tworiders", rq2[r]->is_human ? "second_joined_player" : "second_joined_bot", 1);
                            if (diag_on()) fprintf(stderr, "[BOT-TWORIDERS] t=%.1f nation=%d %08X%s joins the flag run (%s) as its second BattleArmor\n", b->now, fn, rq2[r]->actor,
                                                   rq2[r]->is_human ? " (player)" : "", k_fr_name[g_frun[fn].phase % 6]);
                        }
            }
            {   /* [C7] lift: a rider came on / let go */
                const Ma2ObjectiveView *mf = obj_find(w, MA2_OBJ_FLAG, me->nation, 1), *tf = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
                uint32_t ra = 0; const Ma2PlayerView *rq = NULL;
                for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == me->actor) { rq = &w->players[j]; ra = rq->actor; break; }
                if (ra != b->lift_rider) {
                    if (b->lift_rider && b->lift_rider_carrier) {
                        const Ma2PlayerView *was = player_by_actor(w, b->lift_rider);
                        if (was && mf && hdist(was->pos, mf->home) < 60.0f) c7_add("lift", b->lift_rider_human ? "carrier_home_human" : "carrier_home_bot", 1);
                    }
                    b->lift_rider = ra; b->lift_rider_carrier = rq && tf && tf->carrier == rq->player; b->lift_rider_human = rq && rq->is_human;
                    if (ra) c7_add("lift", b->lift_rider_carrier ? (b->lift_rider_human ? "carrier_boarded_human" : "carrier_boarded_bot") : (rq->is_human ? "boarded_human" : "boarded_bot"), 1);
                }
                /* LiftHuman: the player carrying their flag in a BattleArmor far from our stand asks for the ride. */
                if (lift_carrier_on() && lift_human_on() && mf && tf && tf->carrier)
                    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                        const Ma2PlayerView *q = &w->players[j];
                        if (!q->is_human || !q->alive || q->nation != me->nation || q->player != tf->carrier || kind_ok(q->kind) != MA2_KIND_BATTLEARMOR || q->riding) continue;
                        if (hdist(q->pos, mf->home) > 150.0f) lift_post_carrier(q->actor, q->nation, q->pos, mf->home, b->now, 1);
                    }
            }
            int hurt_hum = 0;
            {   /* PlayerRunHurt: the player let go of a VTOL that made its call -- the call stands 45 s more */
                int hn = me->nation & 7;
                for (int r = 0; r < nr && r < 2; r++) if (rq2[r]->is_human) hurt_hum = 1;
                if (g_hurt[hn].vtol == me->actor && g_hurt[hn].until > b->now + 1000.0 && !hurt_hum) {
                    g_hurt[hn].until = b->now + 45.0; c7_add("hurtrun", "player_off", 1);
                    if (diag_on()) fprintf(stderr, "[BOT-HURTRUN] t=%.1f nation=%d VTOL %08X: the player is off (%s call, %.0f s after it); no new run from it for 45 s\n",
                                           b->now, hn, me->actor, g_hurt[hn].mode == 1 ? "press-on" : "set-down", b->now - g_hurt[hn].t);
                }
                hurt_hum = hurt_hum && hurt_mode(hn, me->actor, b->now) && player_run_hurt_on();
            }
            /* someone on board: take them where they are going (PlayerRunHurt: the call is carried out even under 30 % health --
             * the lift goal used to drop there and the player was flown wherever the retreat went) */
            if (rider) s[G_LIFT] = hpf > (hurt_hum ? 0.08f : 0.3f) ? 1.2f : 0.0f;
            else if (lift_carrier_on()) {
                /* LiftCarrier: a flag carrier's request first, taken farther, under some fire and longer by a
                 * better pilot (VTOL-CARRIER-SPEC table); other requests as before. */
                static const float cmax[4] = { 260.0f, 350.0f, 450.0f, 550.0f }, chp[4] = { 0.5f, 0.5f, 0.45f, 0.4f }, chp_fire[4] = { 2.0f, 0.7f, 0.6f, 0.55f };
                static const int catk[4] = { 0, 1, 2, 2 };
                static const double cwait[4] = { 25.0, 30.0, 35.0, 40.0 };
                const Ma2ObjectiveView *tf = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
                int sk = b->skill & 3, best = -1, bestc = 0;
                for (int i = 0; i < 12; i++) {
                    if (b->now - g_lift[i].t > 3.0 || b->now < g_lift[i].t || g_lift[i].nation != me->nation || !g_lift[i].ba) continue;
                    if (g_lift[i].vtol && g_lift[i].vtol != me->actor && lift_claim_live(i, b->now)) continue;
                    if (g_lift[i].ba == b->lift_off && b->now < b->lift_off_until) continue;
                    const Ma2PlayerView *q = player_by_actor(w, g_lift[i].ba);
                    if (lift_intent_gone(i, q && q->is_human, b->now)) { ride_intent_drop(b, w, me, i); continue; }   /* RideIntent */
                    int car = q && tf && tf->carrier && tf->carrier == q->player;
                    float dd = hdist(g_lift[i].pos, me->pos);
                    if (car) {
                        const char *why = dd > cmax[sk] ? "far" : hpf < chp[sk] ? "hp" : b->n_attackers > catk[sk] ? "attackers" : (b->n_attackers > 0 && hpf < chp_fire[sk]) ? "hp-under-fire" : NULL;
                        if (why) { lift_why(b, g_lift[i].ba, why, dd); continue; }
                    } else if (b->n_attackers > 0 || hpf <= 0.5f || dd > 260.0f) continue;
                    if (best < 0 || (car && !bestc)) { best = i; bestc = car; }
                    if (car) break;
                }
                if (best >= 0) {
                    int i = best;
                    if (b->goal != G_LIFT || b->lift_ba != g_lift[i].ba) b->lift_since = b->now;
                    if (b->now - b->lift_since > (bestc ? cwait[sk] : 25.0)) {
                        if (bestc) c7_add("lift", "carrier_gave_up", 1);
                        b->lift_off = g_lift[i].ba; b->lift_off_until = b->now + 30.0; g_lift[i].vtol = 0;
                    } else {
                        if (bestc && g_lift[i].vtol != me->actor) {
                            c7_add("lift", k_c7_lift_taken[sk], 1);
                            if (diag_on()) { const Ma2PlayerView *q = player_by_actor(w, g_lift[i].ba);
                                fprintf(stderr, "[BOT-LIFT] t=%.1f bot=%d (skill %d) VTOL takes the flag carrier %08X%s %.0f m off, %d attackers\n", b->now, b->index, sk,
                                        g_lift[i].ba, q && q->is_human ? " (HUMAN)" : "", hdist(g_lift[i].pos, me->pos), b->n_attackers); }
                        }
                        s[G_LIFT] = bestc ? 1.4f : 1.0f; b->lift_ba = g_lift[i].ba; if (g_lift[i].vtol != me->actor || b->goal == G_LIFT) g_lift[i].go_t = b->now; g_lift[i].vtol = me->actor;
                    }
                }
            } else if (b->n_attackers == 0 && hpf > 0.5f) {
                for (int i = 0; i < 12; i++) {
                    if (b->now - g_lift[i].t > 3.0 || b->now < g_lift[i].t || g_lift[i].nation != me->nation || !g_lift[i].ba) continue;
                    if (g_lift[i].vtol && g_lift[i].vtol != me->actor && lift_claim_live(i, b->now)) continue;   /* another VTOL has it */
                    if (hdist(g_lift[i].pos, me->pos) > 260.0f) continue;
                    if (g_lift[i].ba == b->lift_off && b->now < b->lift_off_until) continue;   /* waited for that one already */
                    { const Ma2PlayerView *q = player_by_actor(w, g_lift[i].ba);
                      if (lift_intent_gone(i, q && q->is_human, b->now)) { ride_intent_drop(b, w, me, i); continue; } }   /* RideIntent */
                    if (b->goal != G_LIFT) b->lift_since = b->now;
                    if (b->now - b->lift_since > 25.0) { b->lift_off = g_lift[i].ba; b->lift_off_until = b->now + 30.0; g_lift[i].vtol = 0; break; }
                    s[G_LIFT] = 1.0f; b->lift_ba = g_lift[i].ba; if (g_lift[i].vtol != me->actor || b->goal == G_LIFT) g_lift[i].go_t = b->now; g_lift[i].vtol = me->actor;
                    break;
                }
            }
            /* VtolMuster: no rider and no request taken -- a teammate getting into a BattleArmor near us (the
             * vehicle's boarding claim), or standing beside us in one, gets a hold of 8-11 s. */
            {
                uint32_t pl = 0, ba = 0; int human = 0; Ma2Vec3 at = me->pos; float bd = 1e9f;
                int mboard = 0;
                for (int r = 0; r < nr && r < 2; r++) if (rq2[r]->player == b->muster_pl) mboard = 1;
                if (b->muster_pl && (two ? mboard || nr >= 2 : rider)) { c7_add("muster", "boarded", 1); if (two && nr >= 2) c7_add("tworiders", "hold_filled", 1); b->muster_pl = 0; }
                /* MusterWhere (default on, RECOMP_BOT_VTOL_MUSTER_WHERE / [Bots] MusterWhere): not mid flag run, nor near their
                 * flag -- having let the player off there it held over him, still, and the base's guns took it 92% -> 0 in 6 s
                 * instead of the run's cover circle 55-95 m out (desktop ferry 10-31) */
                int mwhere = 1;
                if (c7_on("musterwhere", "RECOMP_BOT_VTOL_MUSTER_WHERE", "MusterWhere", 1)) {
                    const Ma2ObjectiveView *tfm = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
                    int fp = g_frun[me->nation & 7].phase;
                    /* VtolTwoRiders: our own run still at pickup / inbound, well short of their flag, may hold for a second rider */
                    int fok = two && (fp == FR_PICKUP || fp == FR_INBOUND) && g_frun[me->nation & 7].vtol == me->actor && tfm && hdist(tfm->pos, me->pos) >= 400.0f;
                    if ((fp && !fok) || (tfm && hdist(tfm->pos, me->pos) < 200.0f)) mwhere = 0;
                }
                /* VtolTwoRiders: a lift request taken from a BattleArmor within 50 m (not their flag's carrier) is held for like a muster */
                const Ma2PlayerView *lbq = two && b->lift_ba ? player_by_actor(w, b->lift_ba) : NULL;
                int lb_near = lbq && hdist(lbq->pos, me->pos) < 50.0f && !(tf2 && tf2->carrier && tf2->carrier == lbq->player);
                int two_off = nr && two_wait_cap_on() && b->now < b->two_off_until && b->now + 60.0 > b->two_off_until;   /* TwoWaitCap (b) */
                /* TwoQuick (default on, RECOMP_BOT_VTOL_TWO_QUICK / [Bots] TwoQuick; playtest 11-12 slow to leave once a BattleArmor
                 * latches on): with one rider aboard the VTOL held 8-11 s for a second -- even a pilot still walking to a BattleArmor
                 * 70 m off (playtest botrec 11-12 t=10.0-18.9: blue VTOL bot 5, rider aboard at 9.9, hovered at its pad 8.9 s for
                 * bot 7 boarding a suit). Now, aboard: only a teammate already in a BattleArmor within 35 m is held for, 4 s at most
                 * (the player keeps his 8-11 s). [C7] muster.held_bot / gave_up as before. */
                int two_quick = two && c7_on("twoquick", "RECOMP_BOT_VTOL_TWO_QUICK", "TwoQuick", 1);
                if ((two ? nr < 2 && !rcar && !two_off : !rider) && (!b->lift_ba || lb_near) && mwhere && vtol_muster_on() && b->n_attackers <= 1 && hpf > 0.35f) {
                    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                        const Ma2PlayerView *q = &w->players[j];
                        if (q == me || !q->alive || q->nation != me->nation || q->riding || kind_ok(q->kind) != MA2_KIND_BATTLEARMOR) continue;
                        float d = hdist(q->pos, me->pos);
                        int better = d < bd;
                        if (two && q->is_human != human) better = q->is_human;   /* VtolTwoRiders: the player first */
                        if (d > (two_quick && nr ? 35.0f : 50.0f) || !better) continue;
                        bd = d; pl = q->player; ba = q->actor; at = q->pos; human = q->is_human;
                    }
                    for (int i = 0; i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES && !(two_quick && nr); i++) {   /* TwoQuick: aboard, no wait for a pilot still boarding */
                        const Ma2VehicleView *v = &w->vehicles[i];
                        uint32_t who = v->claimed_by && v->claim_nation == me->nation ? v->claimed_by :
                                       v->human_claim && v->human_claim_nation == me->nation ? v->human_claim : 0;
                        if (!who || v->occupied || kind_ok(v->kind) != MA2_KIND_BATTLEARMOR) continue;
                        float d = hdist(v->pos, me->pos);
                        int vh = who == v->human_claim, better = d < bd;
                        if (two && vh != human) better = vh;
                        if (d > 70.0f || !better) continue;
                        bd = d; pl = who; ba = 0; at = v->pos; human = vh;
                    }
                    if (pl && pl == b->muster_off && b->now < b->muster_off_until) pl = 0;
                }
                if (!pl) { if (b->muster_pl) { b->muster_last = b->muster_pl; b->muster_last_t = b->now; } b->muster_pl = 0; b->muster_human = 0; }
                else {
                    /* TwoWaitCap (a): the same one back within 4 s (a BattleArmor hopping out of 50 m and in) keeps the hold's clock */
                    if (pl != b->muster_pl && pl == b->muster_last && b->now >= b->muster_last_t && b->now - b->muster_last_t < 4.0 && two_wait_cap_on()) {
                        b->muster_pl = pl; c7_add("twowaitcap", "resumed", 1);
                    }
                    if (pl != b->muster_pl) {
                        if (two && nr) c7_add("tworiders", human ? "hold_second_player" : "hold_second_bot", 1);
                        b->muster_pl = pl; b->muster_t0 = b->now; b->muster_lim = rng_range(&b->rng, 8.0f, 11.0f);   /* a player waits a moment, then gives up (playtest 10-31) */
                        if (two_quick && nr && !human) b->muster_lim = 4.0f;   /* TwoQuick: a second bot gets 4 s */
                        if (human && pair_active(me->nation) && g_pair[me->nation & 7].human == 2 && g_pair[me->nation & 7].vt_pl == me->player) b->muster_lim = 25.0f;   /* CtfPair: the opening's partner */
                        c7_add("muster", human ? "held_player" : "held_bot", 1);
                        if (diag_on()) fprintf(stderr, "[BOT-MUSTER] t=%.1f bot=%d VTOL holds for %s %08X %s, %.0f m (%d aboard, run %s)\n", b->now, b->index,
                                               human ? "the player" : "bot", pl, ba ? "in a BattleArmor" : "boarding a BattleArmor", bd, nr, k_fr_name[g_frun[me->nation & 7].phase % 6]);
                    }
                    if (b->now - b->muster_t0 > b->muster_lim) {
                        c7_add("muster", "gave_up", 1);
                        b->muster_off = pl; b->muster_off_until = b->now + 45.0; b->muster_pl = 0;
                    } else {
                        b->muster_ba = ba; b->muster_at = at; b->muster_human = human;
                        if (lb_near) b->lift_ba = 0;   /* VtolTwoRiders: the hold, not the request's pickup (it boards the hold) */
                        s[G_LIFT] = maxf(s[G_LIFT], 2.0f);   /* over hunting + its hysteresis / dwell (it hunted off at the start) */
                        g_muster[me->nation & 7].vtol = me->actor; g_muster[me->nation & 7].pl = pl; g_muster[me->nation & 7].t = b->now;
                    }
                }
            }
        }
        /* Power-ups: grab one we pass, collect them while roaming, go for what a kill dropped,
         * and head for armour when hurt and it is quiet. */
        {
            float pd; int pi = k == MA2_KIND_VTOL ? -1 : nearest_powerup(b, w, me, &pd);   /* a gunship cannot pick anything up */
            b->pu_d = k == MA2_KIND_VTOL ? 0.0f : pi >= 0 ? pd : 1e9f;   /* (a ferry is not held to the 30 m rule) */
            if (k == MA2_KIND_VTOL) {
                /* ...but it can carry one on its tether to a teammate: when quiet, fetch the
                 * nearest; carrying one, deliver it before anything but a fight for our life. */
                float vd; int vi = -1;
                int was = b->vt_carry;
                b->vt_carry = vtol_carrying(w, me, sn);
                if (!was && b->vt_carry) c7_add("f18", "hooked", 1);
                if (was && !b->vt_carry) {        /* just let it go over a teammate: that one is his now */
                    c7_add("f18", "delivered", 1);
                    for (int i = 0; i < w->npickups && i < MA2_BOT_MAX_PICKUPS; i++) {
                        if (w->pickups[i].kind != MA2_PICKUP_POWERUP || hdist(w->pickups[i].pos, me->pos) > 30.0f) continue;
                        int j, old = 0;
                        for (j = 0; j < 16; j++) if (g_pu_taken[j].until < g_pu_taken[old].until) old = j;
                        g_pu_taken[old].pos = w->pickups[i].pos; g_pu_taken[old].until = b->now + 20.0; g_pu_taken[old].nation = me->nation;
                    }
                }
                if (b->vt_carry) { if (cargo_mate(w, me) >= 0) s[G_POWERUP] = maxf(s[G_POWERUP], 1.1f); }
                else if (b->now < b->fetch_off_until) vi = -1;                    /* (F18) gave a fetch up: rest */
                else if ((vi = nearest_powerup(b, w, me, &vd)) >= 0 && vd < 350.0f && b->n_attackers == 0)
                    s[G_POWERUP] = maxf(s[G_POWERUP], t && mem_acquired(b, t) ? 0.35f : 0.65f);
                /* started a fetch: finish it -- while there is still something to do (a VTOL with a power-up
                 * on its tether and no teammate in reach to give it to hovered on the spot for 20 s) */
                if (b->goal == G_POWERUP && b->n_attackers == 0 && (b->vt_carry ? cargo_mate(w, me) >= 0 : vi >= 0))
                    s[G_POWERUP] = maxf(s[G_POWERUP], 0.95f);
                if (getenv("RECOMP_BOT_FORCE_FERRY") && (b->vt_carry || vi >= 0)) s[G_POWERUP] = 5.0f;           /* test hook */
            }
            if (pi >= 0) {
                int quiet = !t || !t->visible || hdist(me->pos, t->pos) > 140.0f;
                if (hpf < 0.6f && b->now - b->last_hit > 3.0) s[G_POWERUP] = 0.55f + 0.6f * (1.0f - hpf) - pd / 600.0f;
                if (pd < 35.0f && quiet) s[G_POWERUP] = maxf(s[G_POWERUP], 0.9f);
                else if (pd < 120.0f && !t) s[G_POWERUP] = maxf(s[G_POWERUP], 0.45f);
                if (b->now - b->kill_t < 15.0 && hdist(w->pickups[pi].pos, b->kill_pos) < 70.0f && quiet) s[G_POWERUP] = maxf(s[G_POWERUP], 1.0f);
                if (hpf < thr && pd < 120.0f) s[G_POWERUP] = maxf(s[G_POWERUP], 1.15f);
                if (b->pu_deny && w->pickups[pi].entity == b->pu_deny && b->n_attackers == 0) {   /* F66: keep it from a hurt enemy */
                    float dv = (b->skill & 3) == MA2_SKILL_ELITE ? 0.9f : 0.8f;
                    if (dv > s[G_POWERUP]) { s[G_POWERUP] = dv; c7_add("f66", k_f66_want[b->skill & 3], 1); }
                }
                if (f42_on() && k == MA2_KIND_MECH && me->alpha_cap && me->alpha_short > 0 && (b->skill & 3) >= MA2_SKILL_VETERAN &&
                    w->pickups[pi].armor == 0 && pd < 250.0f && quiet && b->n_attackers == 0) {
                    s[G_POWERUP] = maxf(s[G_POWERUP], me->alpha_short == 1 ? 1.0f : 0.75f);
                    c7_add("F42", (b->skill & 3) == MA2_SKILL_ELITE ? "want_s3" : "want_s2", 1);
                }
                /* Nearly there: grab it, then back out -- stopping at its edge because the enemy
                 * came into view only trades the shots for nothing. */
                if (b->goal == G_POWERUP && pd < 25.0f) s[G_POWERUP] = maxf(s[G_POWERUP], 1.6f);
                if (sp2_pu_take(b, w->pickups[pi].entity)) s[G_POWERUP] = maxf(s[G_POWERUP], 1.6f);   /* surprise set B: the Elite that waited for it takes it now */
                /* Badly hurt: armour lying close by is the way out -- worth more than running. */
                if (hpf < 0.4f && pd < 45.0f && (w->pickups[pi].armor > 0 || !b->pu_typed)) s[G_POWERUP] = maxf(s[G_POWERUP], 1.7f);
                /* PuGreedy (default on, RECOMP_BOT_PU_GREEDY / [Bots] PuGreedy; playtest 11-05): a player takes every power-up in reach without a
                 * second thought, fighting on the way -- the power-up goal shoots on the move. The bots only went for one when quiet, hurt and
                 * unhit for 3 s, or on special errands, so any enemy in sight held them off it (a playtester's TD game 11-05: power-up trips only in
                 * lulls). Now one this body can use is worth more than standing to fight: within 40 m it beats the attack (1.25), out to
                 * 80 m (armour while hurt: 150 m) it competes with it, less with distance. Recruits look less far. [C7] pugreedy: near / far. */
                if (k != MA2_KIND_VTOL && c7_on("pugreedy", "RECOMP_BOT_PU_GREEDY", "PuGreedy", 1) && powerup_value(b, me, &w->pickups[pi]) > 0.0f) {
                    int arm = w->pickups[pi].armor > 0;
                    static const float reach_sk[4] = { 0.6f, 0.8f, 1.0f, 1.0f };
                    float far_ = (arm ? (hpf < 0.6f ? 150.0f : 90.0f) : 80.0f) * reach_sk[b->skill & 3];
                    if (!(arm && hpf > 0.9f)) {
                        if (pd < 40.0f * reach_sk[b->skill & 3]) { if (s[G_POWERUP] < 1.25f) c7_add("pugreedy", "near", b->dt); s[G_POWERUP] = maxf(s[G_POWERUP], 1.25f); }
                        else if (pd < far_) {
                            float v = 1.05f + (arm ? 0.5f * (1.0f - hpf) : 0.0f) - pd / 500.0f;
                            if (v > s[G_POWERUP]) { s[G_POWERUP] = v; c7_add("pugreedy", "far", b->dt); }
                        }
                    }
                }
            }
        }
        /* Help a teammate who is outnumbered or going down (Regular and up). */
        if (b->skill >= MA2_SKILL_REGULAR && k != MA2_KIND_PILOT && (!t || !t->visible)) {
            uint32_t att = 0; Ma2Vec3 wpos;
            if (teammate_in_trouble(b, w, me, &att, &wpos)) { s[G_SUPPORT] = 0.7f; b->support_tgt = att; b->support_from = wpos; }
            /* ComboAlert: a teammate has an enemy VTOL+BattleArmor in sight -- go and shoot it (not the carrier: he runs). */
            int cn = me->nation & 7;
            const Ma2PlayerView *cv = combo_alert_on() && w->game_type == MA2_GT_CTF && g_combo[cn].actor && b->now - g_combo[cn].t < 3.0 && b->now >= g_combo[cn].t ?
                                      player_by_actor(w, g_combo[cn].actor) : NULL;
            if (cv && combo_of(w, cv) && b->obj_op != OP_CARRY && hdist(me->pos, cv->pos) < 450.0f) {
                static const float rs[4] = { 0.8f, 1.05f, 1.3f, 1.5f };
                if (rs[b->skill & 3] > s[G_SUPPORT]) {
                    if (b->goal != G_SUPPORT || b->support_tgt != cv->actor) c7_add("combo", "responders", 1);
                    s[G_SUPPORT] = rs[b->skill & 3]; b->support_tgt = cv->actor; b->support_from = g_combo[cn].pos;
                }
            }
        }
        /* BattleArmor: aggressive with its guns, NeuroHacks opportunistically. */
        if (k == MA2_KIND_BATTLEARMOR && !me->qte_active) {
            int i;
            for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
                Mem *m = &b->mem[i];
                float d;
                const Ma2PlayerView *pv;
                if (!m->valid || m->kind != MA2_KIND_MECH || !mem_engageable(b, m)) continue;
                if (m->actor == b->hacked_actor && b->now - b->hacked_t < 30.0) continue;
                d = hdist(me->pos, m->pos);
                pv = player_by_actor(w, m->actor);
                if (d < 25.0f && hpf >= 0.35f && ba_hack_close_on()) {   /* BaHackClose: one roll per close encounter */
                    int sk = b->skill & 3, hum = pv && pv->is_human;
                    int same = m->actor == b->hkc_actor[i], fresh = !same || b->now - b->hkc_in_t[i] > 5.0 || b->now < b->hkc_in_t[i];
                    if (fresh && (!same || b->now - b->hkc_roll_t[i] >= 5.0 || b->now < b->hkc_roll_t[i])) {
                        static const float odds[4] = { 0.45f, 0.60f, 0.75f, 0.85f };
                        float p = odds[sk] + (hum ? 0.15f : 0.0f);
                        if (p > 0.95f) p = 0.95f;
                        b->hkc_actor[i] = m->actor; b->hkc_roll_t[i] = b->now; b->hkc_win[i] = rng_f(&b->rng) < p;
                        c7_add("bahackclose", k_c7_hkc_roll[sk], 1); if (b->hkc_win[i]) c7_add("bahackclose", k_c7_hkc_win[sk], 1);
                        if (hum) { c7_add("bahackclose", "rolls_vs_player", 1); if (b->hkc_win[i]) c7_add("bahackclose", "wins_vs_player", 1); }
                        if (diag_on()) fprintf(stderr, "[BOT-HACKROLL] t=%.1f bot=%d (skill %d) 'Mech %08X%s %.0f m: %.0f%% -> %s\n", b->now, b->index, sk, m->actor,
                                               hum ? " (player)" : "", d, p * 100.0f, b->hkc_win[i] ? "hack" : "no");
                    }
                    b->hkc_in_t[i] = b->now;
                    if (b->hkc_win[i] && 1.4f > s[G_HACK]) { s[G_HACK] = 1.4f; b->hack_tgt = m->actor; }
                }
                if (d < 45.0f || (d < 120.0f && pv && pv->attacking && pv->attacking != me->actor)) {
                    float u = d < 45.0f ? 1.0f : 0.85f;
                    if (u > s[G_HACK]) { s[G_HACK] = u; b->hack_tgt = m->actor; }
                }
                /* F19b (batch 2: 8 hack picks in 8.5 min): a BA fights from 40-150 m and the 'Mechs it meets are
                 * mostly shooting at it, so neither test above held. By skill, a 'Mech that is on us (attacking
                 * us, or hit us in the last 3 s: a bot's shots do not set its 'attacking' field) and in reach is
                 * worth closing on and clamping: Regular < 70 m (0.75, loses to most fights), Veteran < 100 m (0.95),
                 * Elite < 120 m (1.05, beats the gun fight); Recruits only hack what they bump into. Not when
                 * badly hurt. RECOMP_BOT_F19_SEEK / [Bots] BaHackSeek. [C7] f19 hack_seek. */
                else if ((m->attacking_me || b->now - m->hit_me_t < 3.0) && hpf >= 0.4f && c7_on("f19b", "RECOMP_BOT_F19_SEEK", "BaHackSeek", 1)) {
                    static const float seek_r[4] = { 0.0f, 70.0f, 100.0f, 120.0f }, seek_u[4] = { 0.0f, 0.75f, 0.95f, 1.05f };
                    int sk = b->skill & 3;
                    if (d < seek_r[sk] && seek_u[sk] > s[G_HACK]) { s[G_HACK] = seek_u[sk]; b->hack_tgt = m->actor; c7_add("f19", "hack_seek", 1); }
                }
            }
            /* F19: once it has picked a 'Mech it sees the hack through (up to 12 s) rather than
             * dropping it the moment the 'Mech turns on it -- the utility above falls to 0 then and
             * cover/powerup won within half a second (13 picks in 442 s, median dwell 0.5 s, no
             * clamp). Off when badly hurt or under heavy fire. RECOMP_BOT_F19 / [Bots] BaHackCommit. */
            if (b->goal == G_HACK && c7_on("f19", "RECOMP_BOT_F19", "BaHackCommit", 1)) {
                const Ma2PlayerView *e = player_by_actor(w, b->hack_tgt);
                if (e && e->alive && e->kind == MA2_KIND_MECH && hdist(me->pos, e->pos) < 120.0f && b->now - b->goal_since < 12.0 &&
                    hpf >= 0.35f && b->in_dps < 25.0f) {
                    if (s[G_HACK] < 1.2f) { s[G_HACK] = 1.2f; c7_add("f19", "commit_holds", 1); }
                } else if (b->now - b->goal_since >= 12.0) c7_add("f19", "commit_timeouts", 1);
            }
        }
    } else {
        /* Other bodies (turrets, movers): not drivable here -> get out. */
        b->foot_since = 0.0;
        if (!me->qte_active) s[G_EJECT] = 3.0f;
    }
    objective_eval(b, w, me, s);
    pair_eval(b, w, me, s);   /* CtfPair: the paired BattleArmor holds for its VTOL */
    if (getenv("RECOMP_BOT_FORCE_RIDE") && lifts_on()) {     /* test hook: every BA rides, every VTOL fetches one */
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Ma2PlayerView *q = &w->players[j];
            if (q == me || !q->alive || q->nation != me->nation || hdist(q->pos, me->pos) > 400.0f) continue;
            if (k == MA2_KIND_BATTLEARMOR && !me->riding && q->kind == MA2_KIND_VTOL) { s[G_RIDE] = 5.0f; b->lift_vtol = q->actor; break; }
            if (k == MA2_KIND_VTOL && q->kind == MA2_KIND_BATTLEARMOR && q->riding != me->actor && !q->riding) {
                s[G_LIFT] = 5.0f; b->lift_ba = q->actor; break;
            }
        }
    }
    /* A BattleArmor with a long way to its objective (a flag across the map, a base to hit) asks
     * for a lift there; a teammate's VTOL down beside it is boarded as part of the objective. */
    if (k == MA2_KIND_BATTLEARMOR && lifts_on() && !me->riding && s[G_OBJECTIVE] > 0.0f && b->obj_op != OP_NONE &&
        hdist(me->pos, b->obj_pt) > (getenv("RECOMP_BOT_LIFT_TEST") ? 100.0f : 250.0f) &&
        (b->n_attackers == 0 || b->obj_op == OP_CARRY || getenv("RECOMP_BOT_LIFT_TEST"))) {   /* a flag carrier wants the ride home most */
        if (b->obj_op == OP_CARRY && lift_carrier_on()) lift_post_carrier(me->actor, me->nation, me->pos, b->obj_pt, b->now, 0);
        else lift_post(me->actor, me->nation, me->pos, b->obj_pt, b->now);
        {
            /* A VTOL has taken the request and is on its way: go to meet it rather than walk on
             * (a BattleArmor outruns a VTOL coming down for it, which then gave up). */
            int li = lift_find(me->actor);
            const Ma2PlayerView *v = li >= 0 && lift_claim_live(li, b->now) ? player_by_actor(w, g_lift[li].vtol) : NULL;
            /* ExtractHpSync: not a VTOL under the 30 % the ride lets go at (it boarded, dropped, boarded: au5_60 b1) */
            if (v && v->alive && !(hp_frac(v->hp, v->hp_max) < carry_let_go_hp() && c7_on("extracthp", "RECOMP_BOT_EXTRACT_HP_SYNC", "ExtractHpSync", 1)) &&
                hdist(v->pos, me->pos) < (b->obj_op == OP_CARRY && lift_carrier_on() ? 560.0f : 260.0f) && !(v->actor == b->lift_off && b->now < b->lift_off_until)) {
                if (b->goal != G_RIDE) b->lift_since = b->now;
                if (b->now - b->lift_since > 20.0) { b->lift_off = v->actor; b->lift_off_until = b->now + 30.0; }
                else { s[G_RIDE] = maxf(s[G_RIDE], s[G_OBJECTIVE] + ride_margin(b)); b->lift_vtol = v->actor; }
            }
        }
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Ma2PlayerView *q = &w->players[j];
            int ok = 0;
            if (!q->alive || q->kind != MA2_KIND_VTOL || q->nation != me->nation || hdist(q->pos, me->pos) > 60.0f) continue;
            if (q->actor == b->lift_off && b->now < b->lift_off_until) continue;
            float g = sn && sn->ground ? sn->ground(sn->ctx, q->pos.x, q->pos.z, &ok) : q->pos.y;
            if (ok && q->pos.y - g < 25.0f && hp_frac(q->hp, q->hp_max) > 0.4f) {
                if (b->goal != G_RIDE) b->lift_since = b->now;
                if (b->now - b->lift_since > 12.0) { b->lift_off = q->actor; b->lift_off_until = b->now + 30.0; break; }
                s[G_RIDE] = maxf(s[G_RIDE], s[G_OBJECTIVE] + ride_margin(b)); b->lift_vtol = q->actor; break;
            }
        }
    }

    if (k == MA2_KIND_BATTLEARMOR && lifts_on() && !me->riding) {
        float want = maxf(1.6f, s[G_OBJECTIVE] + ride_margin(b) + 0.3f);
        /* VtolMuster: a teammate VTOL holding for us -- climb on (it is near and coming down). */
        int n = me->nation & 7;
        const Ma2PlayerView *mv = vtol_muster_on() && g_muster[n].pl == me->player && b->now - g_muster[n].t < 2.0 && b->now >= g_muster[n].t ?
                                  player_by_actor(w, g_muster[n].vtol) : NULL;
        /* VtolTwoRiders: not for a hop -- going nowhere 150 m off, the ride lets go at once (order_ride) and the claw went
         * on and off every 2 s beside the VTOL at our stand after a capture (test wa2r3 11-01) */
        if (mv && vtol_two_riders_on()) {
            int li0 = lift_find(me->actor);
            Ma2Vec3 dd = li0 >= 0 ? g_lift[li0].dest : b->obj_op != OP_NONE ? b->obj_pt : b->roam_valid ? b->roam_pt : me->pos;
            const Ma2ObjectiveView *tfx = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
            if (hdist(dd, me->pos) < 150.0f && !(tfx && tfx->carrier && tfx->carrier == me->player)) { c7_add("tworiders", "muster_short_hop_skipped", 1); mv = NULL; }
        }
        if (mv && mv->alive && hdist(mv->pos, me->pos) < 90.0f && !(mv->actor == b->lift_off && b->now < b->lift_off_until)) {
            if (lift_find(me->actor) < 0) lift_post(me->actor, me->nation, me->pos, b->obj_op != OP_NONE ? b->obj_pt : b->roam_valid ? b->roam_pt : me->pos, b->now);
            { int li = lift_find(me->actor); if (li >= 0) { g_lift[li].vtol = mv->actor; g_lift[li].go_t = b->now; } }
            s[G_RIDE] = maxf(s[G_RIDE], want); b->lift_vtol = mv->actor;
        }
        /* BaBoardPlayer: the player's VTOL hovering low right over us for 1.5 s: claw on (12 s to make it, then not for 30 s). */
        if (ba_board_player_on()) {
            const Ma2PlayerView *hv = NULL;
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *q = &w->players[j];
                if (!q->alive || !q->is_human || q->nation != me->nation || kind_ok(q->kind) != MA2_KIND_VTOL) continue;
                if (hdist(q->pos, me->pos) > (b->hboard_vtol == q->actor ? 30.0f : 18.0f)) continue;
                if (q->pos.y - ground_at(sn, q->pos.x, q->pos.z, q->pos.y - 30.0f) > (b->hboard_vtol == q->actor ? 28.0f : 20.0f)) continue;
                if (q->actor == b->lift_off && b->now < b->lift_off_until) continue;
                hv = q; break;
            }
            /* ReboardFix: dropped by their flag to grab it, the player's VTOL still low overhead is not a ride yet -- the bot
             * climbed straight back on without the flag. Once it holds the flag the VTOL is its ride home. */
            if (hv && b->obj_op == OP_GRAB && b->pdrop_t > 0.0 && b->now - b->pdrop_t < 25.0 && b->now >= b->pdrop_t) { hv = NULL; c7_add("boardpl", "grab_first", 1); }
            if (!hv) b->hboard_vtol = 0;
            else {
                if (b->hboard_vtol != hv->actor) { b->hboard_vtol = hv->actor; b->hboard_t = b->now; }
                if (b->now - b->hboard_t > 13.5) { b->lift_off = hv->actor; b->lift_off_until = b->now + 30.0; b->hboard_vtol = 0; c7_add("boardpl", "gave_up", 1); }
                else if (b->now - b->hboard_t >= 1.5) {
                    if (b->goal != G_RIDE || b->lift_vtol != hv->actor) c7_add("boardpl", "wants", 1);
                    s[G_RIDE] = maxf(s[G_RIDE], want); b->lift_vtol = hv->actor;
                }
            }
        }
    }
    /* Riding home with their flag: the ride wins outright, and the "VTOL never came" give-up (20 s / 12 s from
     * lift_since) does not run once aboard -- it fired 20 s into the ride, the carry goal took over and jetted off,
     * and the lift latched it again: hop off / hop on all the way home (playtest, Bogged Down, 2026-10-31). order_ride lets
     * go near home, when the VTOL is going down, or after 120 s. */
    if (me->riding && b->obj_op == OP_CARRY && k == MA2_KIND_BATTLEARMOR) {
        s[G_RIDE] = maxf(s[G_RIDE], s[G_OBJECTIVE] + ride_margin(b) + 0.5f); b->lift_vtol = me->riding; b->lift_since = b->now;
        if (b->lift_off == me->riding) b->lift_off_until = 0.0;
        c7_add("carryride", "held", 1);
    }
    if (me->riding && k == MA2_KIND_BATTLEARMOR && ba_board_player_on()) {   /* BaBoardPlayer: on the player's VTOL, order_ride says when to let go */
        const Ma2PlayerView *rv = player_by_actor(w, me->riding);
        if (rv && rv->is_human) { s[G_RIDE] = maxf(s[G_RIDE], s[G_OBJECTIVE] + ride_margin(b) + 0.5f); b->lift_vtol = me->riding; b->lift_since = b->now; }
    }
    /* RiderStay (default on, RECOMP_BOT_RIDER_STAY / [Bots] RiderStay; playtest 11-12 a bot rider let go mid-flight): on a bot's
     * VTOL the ride scored a flat 1.5, so any objective / hunt / retreat score over it took the rider's goal while it hung there -- its
     * order left IDLE and the claw let go mid-air (playtest botrec 11-12: red BattleArmor bot 3 latched at t=164.8, goal 'objective'
     * def/hunt from that tick, off at 166.6, re-clawed, off again at 167.6 for good, 570 m from anywhere; 115.4 / 130.2 likewise).
     * Aboard a bot's VTOL the ride now wins outright; order_ride alone says when to let go (VTOL hurt, there, 60 s, cornered). [C7]
     * riderstay: held_s (a score over the ride's was beaten). */
    if (me->riding && k == MA2_KIND_BATTLEARMOR && c7_on("riderstay", "RECOMP_BOT_RIDER_STAY", "RiderStay", 1)) {
        const Ma2PlayerView *rv = player_by_actor(w, me->riding);
        if (rv && rv->alive && !rv->is_human) {
            float top = 0.0f;
            for (int g = G_ROAM; g < G_COUNT; g++) if (g != G_RIDE && g != G_DEFEND_HACK && g != G_EJECT && s[g] > top) top = s[g];
            if (top >= s[G_RIDE]) { s[G_RIDE] = top + 0.1f; c7_add("riderstay", "held_s", b->dt); }
            b->lift_vtol = me->riding; b->lift_since = b->now;
        }
    }
    /* CarryCommit (default on, RECOMP_BOT_VTOL_CARRY_COMMIT / [Bots] CarryCommit; playtest 11-12 a VTOL fought instead of flying to
     * the flag, and its bot rider let go): a VTOL with a rider aboard scored its lift a flat 1.2, so support /
     * attack / hunt took it off the run with the rider hanging on (v29b3 rc30 NRC CTF, red bot 3 t=88-97: lift-fly / support / lift-fly /
     * support, the rider let go 574 m from anywhere at 97.2; playtest botrec 11-12 bot 4 t=105-115 lift-fly -> repair-pad). Now with a rider
     * aboard and over 30 % health the lift wins outright (order_lift fights on the way only); under 30 % the old scores stand. [C7]
     * carrycommit: held_s. */
    if (k == MA2_KIND_VTOL && hp_frac(me->hp, me->hp_max) > 0.3f && c7_on("carrycommit", "RECOMP_BOT_VTOL_CARRY_COMMIT", "CarryCommit", 1)) {
        int aboard = 0;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == me->actor) { aboard = 1; break; }
        if (aboard) {
            float top = 0.0f;
            for (int g = G_ROAM; g < G_COUNT; g++) if (g != G_LIFT && g != G_DEFEND_HACK && g != G_EJECT && s[g] > top) top = s[g];
            if (top >= s[G_LIFT]) { s[G_LIFT] = top + 0.1f; c7_add("carrycommit", "held_s", b->dt); }
        }
    }
    lift_trace(w, b->now);
    frun_eval(b, w, me, s);   /* CTF flag run: overrides the lift / ride / objective scores of its two members */

    /* Hysteresis and commitment: the running behaviour keeps a bonus and a minimum time
     * unless an interrupt (hack, heavy damage, lost vehicle) forces a re-think. */
    /* (F6) A flag guard or carrier leaves its job for a power-up only within 30 m. */
    if (pickup_need_on() && b->obj_op != OP_NONE && s[G_OBJECTIVE] > 0.0f && b->pu_d > 30.0f && s[G_POWERUP] >= s[G_OBJECTIVE]) {
        s[G_POWERUP] = s[G_OBJECTIVE] - 0.05f; c7_add("pickups", "guard_capped", 1);
    }
    /* RunFocus (default on, RECOMP_BOT_RUN_FOCUS / [Bots] RunFocus; film review 11-08): a bot running for a free objective (grab) or after its carrier
     * (hunt) is not pulled off by a far fight -- p5_85a1 T1 b3 (Snatch It Rush Hour) left the core run 3 times in 15 s (objective 0.82 vs attack
     * 1.06) for a VTOL 200-330 m away. The far target is still shot on the move (travel fires at it); a near one (< 120 m), a hurt body (< 40 %)
     * or one taking fire (> 15 dps) still turns to fight. [C7] runfocus: held. */
    if (s[G_OBJECTIVE] > 0.0f && (b->obj_op == OP_GRAB || b->obj_op == OP_HUNT || b->obj_op == OP_CHASE_IT || b->obj_op == OP_CARRY) &&
        hpf > 0.4f && b->in_dps < 15.0f && (!t || dt > 120.0f) && c7_on("runfocus", "RECOMP_BOT_RUN_FOCUS", "RunFocus", 1)) {
        float cap = s[G_OBJECTIVE] - 0.05f;
        if (s[G_ATTACK] > cap || s[G_SUPPORT] > cap || s[G_ROAM] > cap) {
            c7_add("runfocus", "held", b->dt);
            if (s[G_ATTACK] > cap) s[G_ATTACK] = cap;
            if (s[G_SUPPORT] > cap) s[G_SUPPORT] = cap;
            if (s[G_ROAM] > cap) s[G_ROAM] = cap;
        }
    }
    if (b->goal > G_NONE && b->goal < G_COUNT && s[b->goal] > 0.0f) {
        s[b->goal] += MA2B_HYSTERESIS;
        if (!b->interrupt && b->now - b->goal_since < MA2B_MIN_DWELL) s[b->goal] += 0.5f;
    }
    best = G_ROAM;
    for (g = 1; g < G_COUNT; g++) if (s[g] > s[best]) best = g;
    if (best != b->goal) {
        if (diag_on())
            fprintf(stderr, "[BOT-DEC] t=%.1f bot=%d %s -> %s (%.2f) hp=%.2f att=%d in_dps=%.0f force=%.1f out=%d tgt=%08X%s\n", b->now, b->index,
                    k_goal_name[b->goal % G_COUNT], k_goal_name[best], s[best], hpf, b->n_attackers, b->in_dps, b->force, b->outnumbered,
                    b->target, b->interrupt ? " (interrupt)" : "");
        if (best == G_POWERUP) {   /* [C7] pickups: trips started, by type */
            int pi = nearest_powerup(b, w, me, NULL);
            c7_add("pickups", "picks", 1);
            if (pi >= 0 && b->pu_typed) c7_add("pickups", w->pickups[pi].armor > 0 ? "armor" : "weapon", 1);
        }
        if (best == G_HACK) c7_add("f19", "hack_picks", 1);
        if (b->flank_on && best != G_ATTACK) { b->flank_on = 0; c7_add("flanks", "dropped", 1); c7_add("flanks", "drop_goal", 1); flank_release(b); }
        if (b->goal != best) b->note[0] = 0;   /* NoteReset: a goal whose orders set no note showed the last goal's (au3_64 bots 8/10: 'powerup' 26-29 s into objective) */
        b->goal = best;
        b->goal_since = b->now;
        b->roam_valid = 0;
        b->tac_until = 0.0;
    }
    b->goal_score = s[best];
    b->interrupt = 0;
    /* RideIntent: our request counts while we ride, or go on to the objective it is for, and have not given up on the VTOL coming */
    if (k == MA2_KIND_BATTLEARMOR && !me->riding) {
        int li = lift_find(me->actor);
        if (li >= 0 && (b->goal == G_RIDE || (b->goal == G_OBJECTIVE && !(g_lift[li].vtol && g_lift[li].vtol == b->lift_off && b->now < b->lift_off_until))))
            g_lift[li].want_t = b->now;
    }
}

/* ======================================================================
 * Orders
 * ====================================================================== */
static uint32_t actor_of(const Ma2World *w, uint32_t player)
{
    int i = find_player(w, player);
    return i >= 0 ? w->players[i].actor : 0;
}

static void order_travel(Ma2Order *o, Ma2Vec3 p, float stop, int engage)
{
    o->type = MA2_ORD_TRAVEL; o->point = p; o->stop = stop; o->engage = engage;
}

/* Known enemy guns as soft path costs (on foot, retreating, swapping). */
static void add_danger(Ma2Brain *b, const Ma2PlayerView *me, Ma2Order *o)
{
    int i;
    o->ndanger = 0;
    for (i = 0; i < MA2_WORLD_MAX_PLAYERS && o->ndanger < MA2_ORDER_MAX_DANGER; i++) {
        const Mem *m = &b->mem[i];
        float r;
        if (!m->valid || mem_conf(b, m) < 0.3f || k_kind[m->kind].dps < 15.0f) continue;
        if (hdist(m->pos, me->pos) > 350.0f) continue;
        r = minf(k_kind[m->kind].weapon_range * 0.4f, 120.0f);
        o->danger[o->ndanger].x = m->pos.x; o->danger[o->ndanger].z = m->pos.z;
        o->danger[o->ndanger].r = r; o->danger[o->ndanger].cost = 4.0f * mem_conf(b, m);
        o->ndanger++;
    }
}

static void order_get_vehicle(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav, Ma2Order *o)
{
    int vi = find_vehicle(w, b->veh);
    /* Reconsider now and then (vehicle_value favours the current choice): a free vehicle
     * that has come close, or a better one that became free. */
    if (vi >= 0 && b->now - b->veh_repick > 1.0) {
        float ns;
        int ni = pick_vehicle(b, w, me, &ns);
        b->veh_repick = b->now;
        if (ni >= 0 && w->vehicles[ni].actor != b->veh) {
            /* F24 (switch VehCommit): once set off, keep this one unless another is clearly better
             * (+0.3) -- re-picking on every small gain left bots walking between 'Mechs. */
            if (ma2_bot_switch_on(MA2_SW_VEH_COMMIT) && vehicle_usable(b, w, me, &w->vehicles[vi]) &&
                ns < vehicle_value(b, w, me, &w->vehicles[vi]) + 0.3f) ma2_c3_count(MA2_C3_COMMIT_HOLDS);
            else vi = -1;
        }
    }
    if (vi >= 0 && yields_to_human(me, &w->vehicles[vi])) { extern int g_c7_yields; g_c7_yields++; }
    if (vi >= 0 && yields_to_human(me, &w->vehicles[vi]) && diag_on())
        fprintf(stderr, "[BOT-YIELD] t=%.2f bot=%d leaves 'Mech %08X to human %08X (bot %.0f m, human heading for it)\n", b->now, b->index,
                w->vehicles[vi].actor, w->vehicles[vi].human_claim, hdist(me->pos, w->vehicles[vi].pos));
    if (vi >= 0 && ma2_bot_switch_on(MA2_SW_VEH_ASSIGN) && w->vehicles[vi].claimed_by && w->vehicles[vi].claimed_by != me->player &&
        (!w->team_mode || w->vehicles[vi].claim_nation == me->nation)) {
        b->c3_lost_d = hdist(me->pos, w->vehicles[vi].pos); b->c3_lost_until = b->now + 20.0;   /* lost it to a teammate */
    }
    if (vi < 0 || !vehicle_usable(b, w, me, &w->vehicles[vi])) {
        int ni = pick_vehicle(b, w, me, NULL);
        if (ni < 0) {
            b->veh = 0;
            if (roam_due(b, me)) pick_roam(b, w, me, nav);
            order_travel(o, b->roam_pt, 6.0f, 0);
            return;
        }
        if (w->vehicles[ni].actor != b->veh) {
            if (diag_on()) fprintf(stderr, "[BOT-VEHPICK] t=%.1f bot=%d -> %08X kind=%s d=%.0f\n", b->now, b->index, w->vehicles[ni].actor,
                                   k_kind[kind_ok(w->vehicles[ni].kind)].name, hdist(me->pos, w->vehicles[ni].pos));
            if (b->veh) ma2_c3_count(MA2_C3_SWITCHES);
            b->veh = w->vehicles[ni].actor; b->veh_fail = 0;
            /* VTOL+BattleArmor at once (playtest 10-31): a bot taking a VTOL in CTF asks for a BattleArmor partner straight
             * away (teammates choosing a ride then favour a BattleArmor, vehicle_value), not only once airborne. */
            if (w->game_type == MA2_GT_CTF && w->vehicles[ni].kind == MA2_KIND_VTOL && lifts_on()) {
                static const float pp[4] = { 0.3f, 0.5f, 0.7f, 0.85f };
                if (rng_f(&b->rng) < pp[b->skill & 3]) { g_frun_want[me->nation & 7] = b->now + 30.0; c7_add("frun", "partner_asked_at_pick", 1); }
            }
        }
        vi = ni;
    }
    o->type = MA2_ORD_GET_VEHICLE;
    o->vehicle = b->veh;
    o->want_vehicle = b->veh;
    o->point = w->vehicles[vi].has_entry ? w->vehicles[vi].entry : w->vehicles[vi].pos;
    add_danger(b, me, o);
}

/* ======================================================================
 * Peek and hide
 * A small unit -- and a careful 'Mech pilot -- fights from a hidden spot: out to fire, back before
 * the answer lands. Up on a roof, back from the edge (the roof itself hides it); on the ground,
 * behind a building. The pair: where we stand now (it sees the target) and the nearest spot a few
 * metres away, roughly away from the target, that the target cannot see.
 * ====================================================================== */
/* F79 hide reach: how far a hide spot may be so the trip there and back fits in the slow weapon's
 * refire (m; 0 = it does not fit, peek the old way). */
static float peek_slow_reach(const Ma2PlayerView *me, int k)
{
    float speed = k == MA2_KIND_BATTLEARMOR ? 6.0f : 8.0f, r;
    if (me->slow_reload <= 0.0f || (k != MA2_KIND_BATTLEARMOR && k != MA2_KIND_MECH)) return 0.0f;
    r = (me->slow_reload - 0.4f) * speed * 0.5f;
    return r < 6.0f ? 0.0f : minf(r, 35.0f);
}
static int peek_pair(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *t, const Ma2Senses *sn, float reach, Ma2Vec3 *hide)
{
    int k = kind_ok(me->kind), i, j, found = 0, nring, nang;
    float away = heading_of(vsub(me->pos, t->pos)), spread = k == MA2_KIND_BATTLEARMOR ? 4.0f : 7.0f;
    /* Old: two rings 5/10 m (BA) or 9/18 m, +-1.8 rad. Slow weapon (F79): rings out to what the
     * refire allows (6-35 m), +-150 degrees; nearest ring first, the first ring with a hidden spot wins. */
    static const float da_old[6] = { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 1.8f };
    static const float da_wide[9] = { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 1.8f, -1.8f, 2.6f, -2.6f };
    static const float ring_wide[6] = { 6.0f, 10.0f, 15.0f, 22.0f, 30.0f, 35.0f };
    float rings[6];
    const float *da = reach > 0.0f ? da_wide : da_old;
    Ma2Vec3 te = eye_of(t->pos, t->kind, 1.0f);
    if (reach > 0.0f) { nring = 0; for (i = 0; i < 6 && ring_wide[i] <= reach + 0.1f; i++) rings[nring++] = ring_wide[i]; nang = 9; }
    else { rings[0] = k == MA2_KIND_BATTLEARMOR ? 5.0f : 9.0f; rings[1] = k == MA2_KIND_BATTLEARMOR ? 10.0f : 18.0f; nring = 2; nang = 6; }
    for (j = 0; j < nring && !found; j++)
        for (i = 0; i < nang; i++) {
            Ma2Vec3 p = vadd(me->pos, vscale(dir_of_heading(away + da[i]), rings[j]));
            int r;
            if (b->raised) {                                  /* stay on this roof */
                int ok = 0;
                float s = sn && sn->surface ? sn->surface(sn->ctx, p.x, p.z, &ok) : -1e9f;
                if (!ok || fabsf(s - me->pos.y) > 1.5f) continue;
                p.y = s;
            } else {
                p.y = ground_at(sn, p.x, p.z, me->pos.y);
                if (!point_free(sn, p, k)) continue;
            }
            if (spot_taken(b, w, me, p, spread)) { c7_add("c6", "spot_taken", 1); continue; }   /* C6: a teammate's corner */
            r = ray(b, sn, t->actor, me->actor, te, eye_of(p, k, 1.0f));
            if (r < 0) return found;                          /* out of ray budget for now */
            if (r == 0) {
                /* Hidden means all of us: the rays to both our flanks blocked too, not just our middle. */
                Ma2Vec3 dd = vsub(p, t->pos), pe = eye_of(p, k, 0.8f);
                float l = hlen(dd) + 1e-3f, half = k_kind[k].radius;
                int s, hid = 1;
                for (s = -1; s <= 1 && hid; s += 2) {
                    Ma2Vec3 q = pe; q.x += dd.z / l * half * (float)s; q.z -= dd.x / l * half * (float)s;
                    r = ray(b, sn, t->actor, me->actor, te, q);
                    if (r != 0) hid = 0;
                }
                if (hid) { *hide = p; found = 1; break; }
            }
        }
    return found;
}
/* F79 hide search (switch RECOMP_BOT_HIDE_SEARCH / ini BotHideSearch): every ring 6-32 m, +-150 deg,
 * scored instead of nearest-first, and spread over frames so it does not starve sight of rays.
 * A spot the target cannot see (our middle and both flanks) scores for: a firing spot a short step
 * from it that does see the target (a corner: out, fire, back), cover taller than us (a building,
 * not a crest), 12-25 m from where we started, and not losing range. 1 = done, 0 = still going. */
#define PS_NRING 6
#define PS_NANG  9
static int ps_place(Ma2Brain *b, const Ma2Senses *sn, const Ma2PlayerView *me, int k, Ma2Vec3 *p)
{
    if (b->raised) {                                          /* stay on this roof */
        int ok = 0;
        float s = sn && sn->surface ? sn->surface(sn->ctx, p->x, p->z, &ok) : -1e9f;
        if (!ok || fabsf(s - me->pos.y) > 1.5f) return 0;
        p->y = s;
        return 1;
    }
    p->y = ground_at(sn, p->x, p->z, me->pos.y);
    return point_free(sn, *p, k);
}
static int peek_search(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *t, const Ma2Senses *sn)
{
    if (sp_rk_nopeek(b)) return 0;   /* surprise: rookie, straight lines */
    static const float da[PS_NANG] = { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 1.8f, -1.8f, 2.6f, -2.6f };
    static const float ring[PS_NRING] = { 6.0f, 10.0f, 14.0f, 19.0f, 25.0f, 32.0f };
    static const float step[3] = { 2.5f, 4.0f, 6.0f };
    int k = kind_ok(me->kind);
    float spread = k == MA2_KIND_BATTLEARMOR ? 4.0f : 7.0f, half = k_kind[k].radius, range0 = hdist(b->ps_org, t->pos);
    Ma2Vec3 te = eye_of(t->pos, t->kind, 1.0f);
    unsigned used0 = b->rays_used;
    for (; b->ps_i < PS_NRING * PS_NANG; b->ps_i++) {
        float dm = ring[b->ps_i / PS_NANG], sc, l;
        Ma2Vec3 p = vadd(b->ps_org, vscale(dir_of_heading(b->ps_away + da[b->ps_i % PS_NANG]), dm)), dd, pe, hi, sh = b->ps_org;
        int s, j, hid = 1, tall, corner = 0;
        if (b->rays < 26.0f || b->rays_used - used0 >= 16u) return 0;   /* a candidate costs up to 13: keep the rest for sight, <= ~16 a frame */
        if (!ps_place(b, sn, me, k, &p)) continue;
        if (spot_taken(b, w, me, p, spread)) { c7_add("c6", "spot_taken", 1); continue; }   /* C6: a teammate's corner */
        sc = dm < 12.0f ? (dm - 6.0f) / 6.0f : dm <= 25.0f ? 1.0f : 1.0f - (dm - 25.0f) / 10.0f;
        if (sc + 4.5f <= b->ps_best) continue;                /* cannot win even as a tall corner */
        if (ray(b, sn, t->actor, me->actor, te, eye_of(p, k, 1.0f)) != 0) continue;
        dd = vsub(p, t->pos); l = hlen(dd) + 1e-3f; pe = eye_of(p, k, 0.8f);
        for (s = -1; s <= 1 && hid; s += 2) {                 /* hidden means all of us, both flanks too */
            Ma2Vec3 q = pe; q.x += dd.z / l * half * (float)s; q.z -= dd.x / l * half * (float)s;
            if (ray(b, sn, t->actor, me->actor, te, q) != 0) hid = 0;
        }
        if (!hid) continue;
        hi = eye_of(p, k, 1.0f); hi.y += 2.0f;
        tall = ray(b, sn, t->actor, me->actor, te, hi) == 0;
        if (tall) sc += 1.5f;
        /* the firing spot: the shortest step sideways (either way) or toward the target that sees it */
        for (j = 0; j < 3 && !corner && step[j] <= b->ps_reach; j++)
            for (s = 0; s < 3 && !corner; s++) {
                Ma2Vec3 q = p;
                if (s < 2) { float sd = s ? -1.0f : 1.0f; q.x += dd.z / l * step[j] * sd; q.z -= dd.x / l * step[j] * sd; }
                else { q.x -= dd.x / l * step[j]; q.z -= dd.z / l * step[j]; }
                if (!ps_place(b, sn, me, k, &q)) continue;
                if (ray(b, sn, t->actor, me->actor, te, eye_of(q, k, 1.0f)) == 1) { sh = q; corner = 1; }
            }
        if (corner) sc += 3.0f;
        else if (dm > maxf(b->ps_reach, 6.0f)) continue;      /* no corner: shoot from where we stand, if near */
        sc -= 0.05f * maxf(hdist(sh, t->pos) - range0, 0.0f);
        if (sc > b->ps_best) { b->ps_best = sc; b->ps_hide = p; b->ps_shoot = sh; b->ps_corner = corner; b->ps_tall = tall; }
    }
    return 1;
}
/* Hide cost (switch RECOMP_BOT_HIDE_COST / ini BotHideCost): the earlier A/B review
 * found's slow-weapon hide cost the fights: with it on, kills/min UC 3.9 vs 5.9 off and Elite shots per
 * bot-minute 27 vs 55 (Elite hid on 90 % of slow-weapon peeks, each 18-30 s, so it fired less than a Veteran).
 * Hiding pays only while the target shoots at us: a slow-weapon hide starts only when it is attacking us or hit
 * us in the last 3 s, ends once it has left us alone for 5 s, and the per-skill wish is flatter (Elite 0.65,
 * not 0.90). [C7] hidecost skip_calm calm_end. */
static int hidecost_on(void) { return c7_on("hidecost", "RECOMP_BOT_HIDE_COST", "BotHideCost", 1); }
static int threatens_me(const Ma2Brain *b, const Mem *m, double within)
{
    return m->attacking_me || b->now - m->hit_me_t < within || b->now - m->att_t < within;
}
static int peek_order(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Mem *m, const Ma2Senses *sn, const Ma2CtrlStatus *st, Ma2Order *o)
{
    int k = kind_ok(me->kind), sk = b->skill & 3;
    if (b->peek_tgt != m->actor || b->now >= b->peek_end) {
        float want, reach;
        static const float slow_want[4] = { 0.10f, 0.35f, 0.65f, 0.90f };
        static const float slow_want_hc[4] = { 0.10f, 0.35f, 0.55f, 0.65f };   /* hide cost: flatter top end */
        int wide = 0;
        b->peek_end = 0.0;
        if (b->ps_tgt) {                                      /* F79: a hide search is running */
            if (b->ps_tgt != m->actor || b->now - b->ps_t0 > 3.0) { b->ps_tgt = 0; c7_add("f79", "timeouts", 1); return 0; }
            if (!peek_search(b, w, me, m, sn)) return 0;
            b->ps_tgt = 0; wide = 1; reach = b->ps_reach;
        } else {
            if (!m->visible || b->now < b->peek_next || k == MA2_KIND_TANK || k == MA2_KIND_VTOL || k == MA2_KIND_PILOT) return 0;
            b->peek_next = b->now + 6.0;
            /* A slow weapon (F79, switch RECOMP_BOT_PEEK_RELOAD / ini BotPeekReload): BattleArmor and 'Mechs
             * at any health, skill sets how often. Otherwise: BattleArmor most of all; a 'Mech when hurt or
             * outnumbered; skill: how often. With the wide search (RECOMP_BOT_HIDE_SEARCH) every slow weapon
             * counts, the mortar too: the corner step, not the walk to cover, must fit in the refire. */
            reach = c7_on("peek", "RECOMP_BOT_PEEK_RELOAD", "BotPeekReload", 1) ? peek_slow_reach(me, k) : 0.0f;
            if (c7_on("peek", "RECOMP_BOT_PEEK_RELOAD", "BotPeekReload", 1) && me->slow_reload > 0.0f &&
                (k == MA2_KIND_BATTLEARMOR || k == MA2_KIND_MECH) && c7_on("f79", "RECOMP_BOT_HIDE_SEARCH", "BotHideSearch", 1)) {
                float speed = k == MA2_KIND_BATTLEARMOR ? 6.0f : 8.0f;
                reach = clampf((me->slow_reload - 0.4f) * speed * 0.5f, k == MA2_KIND_BATTLEARMOR ? 3.0f : 4.0f, 6.0f);
                wide = 1;
            }
            if (reach > 0.0f) {
                want = hidecost_on() ? slow_want_hc[sk] : slow_want[sk];
                if (hidecost_on() && !threatens_me(b, m, 3.0)) { want = 0.0f; c7_add("hidecost", "skip_calm", 1); }
            }
            else want = k == MA2_KIND_BATTLEARMOR ? 0.3f + 0.2f * (float)b->skill
                      : (hp_frac(me->hp, me->hp_max) < 0.75f || b->outnumbered) ? 0.05f + 0.12f * (float)b->skill : 0.0f;
            if (rng_f(&b->rng) >= want) return 0;
            if (wide) {
                b->ps_tgt = m->actor; b->ps_i = 0; b->ps_org = me->pos; b->ps_away = heading_of(vsub(me->pos, m->pos));
                b->ps_reach = reach; b->ps_best = -1e9f; b->ps_t0 = b->now;
                c7_add("f79", "searches", 1);
                if (!peek_search(b, w, me, m, sn)) return 0;
                b->ps_tgt = 0;
            }
        }
        if (wide) {
            if (b->ps_best < -1e8f) { c7_add("peek", "no_hide", 1); return 0; }
            b->peek_hide = b->ps_hide; b->ps_deep = 0;
            c7_add("f79", "found", 1); c7_add("f79", "corner", b->ps_corner); c7_add("f79", "tall", b->ps_tall);
            c7_add("f79", "step_m_sum", hdist(b->ps_hide, b->ps_shoot)); c7_add("f79", "search_s_sum", b->now - b->ps_t0);
        } else if (!peek_pair(b, w, me, m, sn, reach, &b->peek_hide)) { if (reach > 0.0f) c7_add("peek", "no_hide", 1); return 0; }
        if (!wide) b->ps_corner = 0;
        b->peek_tgt = m->actor; b->peek_shoot = wide ? b->ps_shoot : me->pos; b->peek_phase = 0; b->peek_slow = reach > 0.0f; b->peek_phase_t = b->now;
        b->peek_flip = b->now + rng_range(&b->rng, 1.5f, 2.5f) - 0.15f * (float)b->skill;
        b->peek_end = b->now + rng_range(&b->rng, 18.0f, 30.0f);
        if (wide) {                                           /* first into cover (a fight-move that fires stands and fights: walk), the walk on top of the peek */
            b->peek_phase = 2;
            b->peek_end += hdist(me->pos, b->peek_hide) / (k == MA2_KIND_BATTLEARMOR ? 6.0f : 8.0f);
        }
        spot_claim(me->nation, b->index, 0, b->peek_shoot, b->peek_end);
        spot_claim(me->nation, b->index, 1, b->peek_hide, b->peek_end);
        if (b->peek_slow) { c7_add("peek", "slow_starts", 1); c7_add("peek", "hide_m_sum", hdist(me->pos, b->peek_hide)); }
        else c7_add("peek", "starts", 1);
        if (diag_on()) fprintf(stderr, "[BOT-PEEK] t=%.1f bot=%d kind=%d sk=%d %s shoot %.0f,%.0f hide %.0f,%.0f (%.0f m) reload=%.2f%s\n", b->now, b->index, k, sk,
                               b->raised ? "roof" : "ground", b->peek_shoot.x, b->peek_shoot.z, b->peek_hide.x, b->peek_hide.z,
                               hdist(me->pos, b->peek_hide), me->slow_reload, b->peek_slow ? " timed" : "");
    }
    if (b->peek_slow && hidecost_on() && !threatens_me(b, m, 5.0)) {   /* hide cost: it left us alone, fight in the open */
        b->peek_end = b->now; c7_add("hidecost", "calm_end", 1);
        return 0;
    }
    if (b->peek_slow) {
        /* F79: out at the shooting spot until the slow weapon fires, then behind cover for its
         * refire less the walk back (Elites to within ~0.2 s, Recruits a second either way). */
        static const float jit[4] = { 1.0f, 0.6f, 0.35f, 0.2f };
        float speed = k == MA2_KIND_BATTLEARMOR ? 6.0f : 8.0f;
        if (b->peek_phase == 2) {                             /* F79: on the way into cover */
            /* in cover: close (the steering swerves round teammates), or near and out of its sight */
            float dh = hdist(me->pos, b->peek_hide);
            float in_r = c7_on("f79b", "RECOMP_BOT_HIDE_EXACT", "BotHideExact", 1) ? 1.8f : 4.0f;   /* F79b: walked into the corner itself */
            if (dh < in_r || (dh < 8.0f && !m->visible) || b->now - b->peek_phase_t > hdist(b->ps_org, b->peek_hide) / speed + 6.0) {
                c7_add("f79", dh < 8.0f ? "in_cover" : "cover_timeouts", 1);
                b->peek_phase = 1; b->peek_phase_t = b->now; b->peek_flip = b->now + 0.4;
            }
        } else if (!b->peek_phase && st && st->fired) {
            float back = hdist(b->peek_hide, b->peek_shoot) / speed;
            b->peek_phase = 1; b->peek_phase_t = b->now;
            b->peek_flip = b->now + maxf(me->slow_reload - back + rng_range(&b->rng, -jit[sk], jit[sk]) * 0.5f, 0.3f);
            c7_add("peek", "cycles", 1);
        } else if (!b->peek_phase && !m->visible && b->ps_corner && hdist(me->pos, b->peek_shoot) < 4.0f && b->now - b->peek_phase_t > 2.5) {
            /* F79: out at the corner and the target is not in sight from it (it moved): this peek is over */
            b->peek_end = b->now; c7_add("f79", "lost_line", 1);
            return 0;
        } else if (!b->peek_phase && b->now - b->peek_phase_t > 4.0) {   /* no shot (no line, out of ammo): duck anyway */
            b->peek_phase = 1; b->peek_phase_t = b->now; b->peek_flip = b->now + 1.5;
        } else if (b->peek_phase == 1 && m->visible && b->ps_deep < 2 && b->now - b->peek_phase_t > 0.8 && hdist(me->pos, b->peek_hide) < 6.0f) {
            /* F79: "in cover" and it still sees us (stopped short, or it moved): 3 m deeper, away from it */
            Ma2Vec3 dd = vsub(b->peek_hide, m->pos); float l = hlen(dd) + 1e-3f;
            b->peek_hide.x += dd.x / l * 3.0f; b->peek_hide.z += dd.z / l * 3.0f; b->ps_deep++; b->peek_phase_t = b->now;
            c7_add("f79", "deepen", 1);
        } else if (b->peek_phase && b->now >= b->peek_flip) {
            b->peek_phase = 0; b->peek_phase_t = b->now;
        }
    } else if (b->now >= b->peek_flip) {
        b->peek_phase ^= 1;
        b->peek_flip = b->now + (b->peek_phase ? rng_range(&b->rng, 1.8f, 3.0f) : rng_range(&b->rng, 1.5f, 2.5f) - 0.15f * (float)b->skill);
    }
    o->type = MA2_ORD_FIGHT_MOVE; o->target = m->actor; o->stop = 1.5f;
    if (b->peek_phase == 2) { o->point = b->peek_hide; o->fire = 0; o->hold_fire = 1; snprintf(b->note, sizeof b->note, "peek-to-cover"); }
    else if (b->peek_phase) { o->point = b->peek_hide; o->fire = 0; o->hold_fire = 1; snprintf(b->note, sizeof b->note, "peek-hide"); }
    else { o->point = b->peek_shoot; o->fire = 1; snprintf(b->note, sizeof b->note, "peek-shoot"); }
    if (b->peek_slow && c7_on("f79b", "RECOMP_BOT_HIDE_EXACT", "BotHideExact", 1)) { o->exact = 1; o->stop = 1.0f; }   /* F79b: right into the corner, right out to the step */
    return 1;
}

/* F79 proof metric: of the time a slow weapon is refiring during a fight, how much the bot spends
 * out of its target's sight (per skill; the design target: Elites high, Recruits low). */
static void reload_cover_metric(Ma2Brain *b, const Ma2PlayerView *me, const Mem *m, const Ma2CtrlStatus *st)
{
    static const char *const rl[4] = { "reload_s_rc", "reload_s_rg", "reload_s_vt", "reload_s_el" };
    static const char *const hd[4] = { "hidden_s_rc", "hidden_s_rg", "hidden_s_vt", "hidden_s_el" };
    if (me->slow_reload <= 0.0f || !m) return;
    if (st && st->fired) b->last_fire_t = b->now;
    if (b->last_fire_t > 0.0 && b->now - b->last_fire_t < (double)me->slow_reload && b->now >= b->last_fire_t) {
        c7_add("peek", rl[b->skill & 3], b->dt);
        if (!m->visible) c7_add("peek", hd[b->skill & 3], b->dt);
    }
}

/* Flank routes. In a group fight -- a teammate within 110 m shooting too, and one of us
 * nearer the enemy holding the front -- a bot now and then works round to the enemy's side
 * instead of trading fire head-on: a firing spot ~80-140 m to the side of its target, across the
 * line from our group to it (its own side of that line first), open ground the planner can reach.
 * It goes there shooting (the firing-position move), then fights from there. At most one flanker
 * a team at a time (two when four or more of us are in the fight). Skill-scaled: Recruits never
 * do it, Elites about one roll in three (every 3 s), the aggressive ones more.
 * RECOMP_BOT_FLANKS=0 / [Bots] Flanks=0 = off. */
static int flanks_on(void) { return c7_on("flanks", "RECOMP_BOT_FLANKS", "Flanks", 1); }
/* RECOMP_BOT_FLANK_KEEP=0 / [Bots] FlankKeep=0 = a target switch ends a flank trip again (batch 2-3). */
static int flankkeep_on(void) { return c7_on("flankkeep", "RECOMP_BOT_FLANK_KEEP", "FlankKeep", 1); }
/* Batch 4 FlankCap: a flank spot at most 150 m from us. Trips that arrived started a median 141 m
 * out, dropped ones 182 m (batch 2-3 logs); the old bound (1.6 x target distance + 60 m) allowed
 * ~440 m. DEFAULT OFF: in the UC Elite A/B (240 s each) the share of trips that arrive was the same
 * (3/11 cap on, 5/19 off); the cap only cut the flanks started. RECOMP_BOT_FLANK_CAP=1 / [Bots] FlankCap=1 = on. */
static int flankcap_on(void) { return c7_on("flankcap", "RECOMP_BOT_FLANK_CAP", "FlankCap", 0); }
static void flank_release(const Ma2Brain *b)
{
    for (int i = 0; i < TB_MAX; i++) if (g_flankers[i].until > 0.0 && g_flankers[i].bot == b->index) g_flankers[i].until = 0.0;
}
static void flank_try(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav, const Ma2Senses *sn, const Mem *m)
{
    static const float FLANK_P[4] = { 0.0f, 0.12f, 0.22f, 0.33f };
    static const char *SK[4] = { "started_recruit", "started_regular", "started_veteran", "started_elite" };
    static const float D[3] = { 80.0f, 110.0f, 140.0f }, BK[2] = { -20.0f, 20.0f };
    int k = kind_ok(me->kind), mates = 0, closer = 0, team_on = 0, slot = 0, cap = flankcap_on(), capped = 0;
    float dm = hdist(me->pos, m->pos), bs = -1e9f;
    Ma2Vec3 g = me->pos, u, nrm, best = { 0, 0, 0 };
    if (b->now < b->flank_next) return;
    b->flank_next = b->now + 3.0;
    if (k != MA2_KIND_MECH && k != MA2_KIND_BATTLEARMOR && k != MA2_KIND_TANK) return;
    if (!m->visible || dm < 50.0f || dm > 260.0f || hp_frac(me->hp, me->hp_max) < 0.5f || !sn || !sn->ground) return;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q == me || !q->alive || q->nation != me->nation || hdist(q->pos, me->pos) > 110.0f) continue;
        if (!q->attacking && !q->firing) continue;
        mates++; g = vadd(g, q->pos);
        if (hdist(q->pos, m->pos) < dm) closer++;
    }
    if (mates < 1 || closer < 1) return;                       /* a group fight, someone else holds the front */
    for (int i = 0; i < TB_MAX; i++) {
        if (g_flankers[i].until > b->now) { if (g_flankers[i].nation == me->nation && g_flankers[i].bot != b->index) team_on++; }
        else slot = i;
    }
    if (team_on >= (mates >= 3 ? 2 : 1)) return;
    if (rng_f(&b->rng) >= FLANK_P[b->skill & 3] * (0.6f + 0.8f * b->aggr) * sp_mood_flank(b)) return;
    g = vscale(g, 1.0f / (float)(mates + 1));
    u = hnorm(vsub(m->pos, g));
    if (hlen(u) < 0.5f) return;
    nrm = v3(u.z, 0.0f, -u.x);
    {
        float s0 = (me->pos.x - g.x) * nrm.x + (me->pos.z - g.z) * nrm.z >= 0.0f ? 1.0f : -1.0f;
        for (int si = 0; si < 2; si++)
            for (int di = 0; di < 3; di++)
                for (int bi = 0; bi < 2; bi++) {
                    float side = si ? -s0 : s0, cl, sc;
                    int ok = 0;
                    Ma2Vec3 p = vadd(vadd(m->pos, vscale(nrm, side * D[di])), vscale(u, BK[bi]));
                    if (hdist(me->pos, p) > 1.6f * dm + 60.0f) continue;
                    if (cap && hdist(me->pos, p) > 150.0f) { capped = 1; continue; }
                    p.y = sn->ground(sn->ctx, p.x, p.z, &ok);
                    if (!ok) continue;
                    cl = clearance_at(sn, p.x, p.z);
                    if (cl < 8.0f) continue;
                    if (nav && !ma2_nav_connected(nav, me->pos, p)) continue;
                    if (spot_taken(b, w, me, p, 15.0f)) continue;
                    sc = minf(cl, 30.0f) - 0.05f * hdist(me->pos, p) - (si ? 10.0f : 0.0f);
                    if (sc > bs) { bs = sc; best = p; }
                }
    }
    if (bs <= -1e8f) { c7_add("flanks", "no_spot", 1); if (capped) c7_add("flanks", "no_spot_cap", 1); return; }
    b->tac_pt = best; b->tac_kind = G_ATTACK; b->tac_tgt = m->actor;
    b->tac_until = b->now + clampf(hdist(me->pos, best) / 5.0f + 6.0f, 10.0f, 35.0f);
    b->flank_on = 1; b->flank_tgt = b->flank_tgt0 = m->actor; b->flank_t0 = b->now; b->flank_tpos = m->pos;
    g_flankers[slot].nation = me->nation; g_flankers[slot].bot = b->index; g_flankers[slot].until = b->tac_until;
    spot_claim(me->nation, b->index, 0, best, b->tac_until);
    c7_add("flanks", "started", 1); c7_add("flanks", SK[b->skill & 3], 1);
    if (diag_on()) fprintf(stderr, "[BOT-FLANK] t=%.1f bot=%d sk=%d flanks %08X from %.0f,%.0f to %.0f,%.0f (%d mates, target %.0f m)\n",
                           b->now, b->index, b->skill, m->actor, me->pos.x, me->pos.z, best.x, best.z, mates, dm);
}

/* (a) The climb order ended (over the top, or failed): book it. A failed face joins the bad tops; a topped one is logged
 * as a roof reached and checked a moment later (hg_place) as a roof stood on. */
static void hgc_end(Ma2Brain *b, const Ma2CtrlStatus *st)
{
    if (st->climb_state == 3 && g_nbad_top < 32) g_bad_top[g_nbad_top++] = b->hgc_pt;
    if (st->climb_state == 2) {
        b->hgc_top_t = b->now; c7_add("hg_climb", "roof_reached", 1); c7_add("hg_climb", "climb_s_sum", b->now - b->hgc_t0);
        fprintf(stderr, "[HG-CLIMB] t=%.1f bot=%d sk=%d REACHED the roof at %.0f,%.0f y=%.0f after %.1f s\n", b->now, b->index, b->skill, b->hgc_pt.x, b->hgc_pt.z, b->hgc_pt.y, b->now - b->hgc_t0);
    } else if (diag_on()) fprintf(stderr, "[HG-CLIMB] t=%.1f bot=%d sk=%d climb to %.0f,%.0f failed after %.1f s\n", b->now, b->index, b->skill, b->hgc_pt.x, b->hgc_pt.z, b->now - b->hgc_t0);
}

static void order_attack(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav, const Ma2Senses *sn, Ma2Order *o,
                         const Ma2CtrlStatus *st)
{
    Mem *m = b->tgt;
    int k = kind_ok(me->kind), peeking;
    if (!m && b->hgc_on && k == MA2_KIND_BATTLEARMOR) {      /* (a) the climb outlives its target: carry on up (push-x2) */
        int ended = st && (st->climb_state == 2 || st->climb_state == 3);
        if (ended) hgc_end(b, st);
        if (!ended && !b->raised && b->now <= b->hgc_until) {
            o->type = MA2_ORD_FIGHT_MOVE; o->point = b->hgc_pt; o->stop = 3.0f; o->target = 0; o->fire = 0;
            o->climb = 1; o->climb_pt = b->hgc_pt;
            snprintf(b->note, sizeof b->note, "climb");
            c7_add("hg_climb", "kept_no_target", b->dt);
            return;
        }
        b->hgc_on = 0; b->hgc_next = b->now + 20.0;
    }
    if (!m) {
        if (roam_due(b, me)) pick_roam(b, w, me, nav);
        order_travel(o, b->roam_pt, 8.0f, 1);
        return;
    }
    /* Out of sight for a while, or the attack state is stuck: work to a firing position. */
    if (m->visible) b->los_lost_t = 0.0; else if (b->los_lost_t <= 0.0) b->los_lost_t = b->now;
    /* F79: a running peek on it is the firing position: hidden on purpose, no detour to look for another. */
    peeking = b->peek_tgt == m->actor && b->now < b->peek_end && b->peek_slow && c7_on("f79", "RECOMP_BOT_HIDE_SEARCH", "BotHideSearch", 1);
    if (peeking) b->los_lost_t = 0.0;
    /* (c) High ground for 'Mechs (bots batch 5, RECOMP_BOT_HG_MECH / [Bots] MechHighGround): out of sight of the target
     * for 1.5 s, now and then a 'Mech jets onto a low roof (<= 12 m, a wide flat top) or walks up a hilltop that sees it,
     * instead of the ground firing spot. Chance per 6 s look 0.10/0.20/0.30/0.40 by skill, x1..1.5 for a marksman; never
     * under 40 % health or holding an objective. Up there it holds the spot ((0a)) until (d) or the target moves on.
     * [C7] hg_mech looks rolls_<sk> tries arrived (counted by hg_perch time). */
    if (!peeking && k == MA2_KIND_MECH && b->now >= b->tac_until && b->now >= b->hgm_next && !b->raised && !b->hg_hill &&
        b->los_lost_t > 0.0 && b->now - b->los_lost_t > 1.5 && hp_frac(me->hp, me->hp_max) >= 0.4f && !b->obj_lock &&
        c7_on("hg_mech", "RECOMP_BOT_HG_MECH", "MechHighGround",1)) {
        static const float pm[4] = { 0.10f, 0.20f, 0.30f, 0.40f };
        static const char *const rolls[4] = { "rolls_recruit", "rolls_regular", "rolls_veteran", "rolls_elite" };
        Ma2Vec3 p;
        b->hgm_next = b->now + 6.0;
        c7_add("hg_mech", "looks", 1);
        if (rng_f(&b->rng) < pm[b->skill & 3] * (1.0f + 0.5f * b->marksman)) {
            c7_add("hg_mech", rolls[b->skill & 3], 1);
            if (tac_high(b, me, m, sn, &p)) {
                b->tac_pt = p; b->tac_kind = G_ATTACK; b->tac_tgt = m->actor; b->tac_until = b->now + 10.0;
                b->reperch_until = b->now + 12.0;                 /* tac_firing / the ledge code treat it as wanting up */
                c7_add("hg_mech", "tries", 1);
                if (diag_on()) fprintf(stderr, "[HG-MECH] t=%.1f bot=%d sk=%d up to %.0f,%.0f y=%.0f (%.0f m up)\n", b->now, b->index, b->skill, p.x, p.z, p.y, p.y - me->pos.y);
            }
        }
    }
    if (!peeking && k != MA2_KIND_VTOL && b->now >= b->tac_until &&
        ((b->los_lost_t > 0.0 && b->now - b->los_lost_t > 1.5 && hdist(me->pos, m->pos) < my_reach(me) * 1.2f) ||
         (st && st->engage_wedged))) {
        Ma2Vec3 p;
        if (tac_firing(b, w, me, m, 99.0f, sn, &p)) { b->tac_pt = p; b->tac_kind = G_ATTACK; b->tac_tgt = m->actor; b->tac_until = b->now + 6.0; spot_claim(me->nation, b->index, 0, p, b->tac_until); }
        else b->tac_until = b->now + 2.0;
    }
    /* BattleArmor fights from high ground when it can: now and then it looks for a firing spot
     * well above where it stands -- a hilltop, a rooftop its jets reach -- and goes up there. */
    if (!peeking && (k == MA2_KIND_BATTLEARMOR || (k == MA2_KIND_MECH && b->now < b->reperch_until)) && m->visible && b->now >= b->tac_until && b->now >= b->ba_high_next) {
        Ma2Vec3 p;
        b->ba_high_next = b->now + 6.0;
        if (!b->raised && tac_high(b, me, m, sn, &p) && !(k == MA2_KIND_BATTLEARMOR && st && st->roof_failed && c7_on("hg_climb", "RECOMP_BOT_BA_CLIMB", "BaWallClimb", 1))) {
            b->tac_pt = p; b->tac_kind = G_ATTACK; b->tac_tgt = m->actor; b->tac_until = b->now + 8.0;
            c7_add("hg_perch", k == MA2_KIND_MECH ? "high_trips_mech" : "high_trips_ba", 1);
        } else if (!b->raised && k == MA2_KIND_BATTLEARMOR && !b->hgc_on && b->now >= b->hgc_next && hp_frac(me->hp, me->hp_max) >= 0.5f &&
                   !b->obj_lock && c7_on("hg_climb", "RECOMP_BOT_BA_CLIMB", "BaWallClimb",1)) {
            /* (a) Nothing the jets reach: now and then climb a taller building's face (Recruit never .. Elite 0.6),
             * not with an enemy 'Mech close by that sees us (one hit of 110 knocks a climber off). */
            static const float pc[4] = { 0.0f, 0.2f, 0.4f, 0.6f };
            static const char *const rolls[4] = { "rolls_recruit", "rolls_regular", "rolls_veteran", "rolls_elite" };
            int i, near_mech = 0;
            b->hgc_next = b->now + 12.0;
            for (i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
                const Mem *q = &b->mem[i];
                if (q->valid && q->visible && q->kind == MA2_KIND_MECH && hdist(q->pos, me->pos) < 40.0f) near_mech = 1;
            }
            static int force = -1; if (force < 0) force = getenv("RECOMP_BOT_HG_FORCE_CLIMB") != NULL;   /* test hook, env only: every BA that gets here rolls a climb */
            if ((force || !near_mech) && (force || rng_f(&b->rng) < pc[b->skill & 3])) {
                c7_add("hg_climb", rolls[b->skill & 3], 1);
                if (tac_climb(b, me, m, sn, &p)) {
                    b->hgc_on = 1; b->hgc_pt = p; b->hgc_until = b->now + 25.0; b->hgc_t0 = b->now;
                    if (diag_on()) fprintf(stderr, "[HG-CLIMB] t=%.1f bot=%d sk=%d climbs to %.0f,%.0f y=%.0f (%.0f m up)\n", b->now, b->index, b->skill, p.x, p.z, p.y, p.y - me->pos.y);
                } else c7_add("hg_climb", "no_roof", 1);
            }
        }
    }
    hg_drop_after(b, me, sn);
    if (hg_drop(b, me, m, sn)) peeking = 0;
    if (b->hgc_on) {                                          /* (a) climbing: until over the top, failed, too long, or heavy fire */
        static const float line[4] = { 0.30f, 0.22f, 0.16f, 0.12f };
        int ended = st && (st->climb_state == 2 || st->climb_state == 3);
        if (ended) hgc_end(b, st);
        if (ended || b->raised || b->now > b->hgc_until || b->dmg_recent >= line[b->skill & 3] || k != MA2_KIND_BATTLEARMOR) {
            if (!ended && !b->raised) c7_add("hg_climb", "called_off", 1);
            b->hgc_on = 0; b->hgc_next = b->now + (st && st->climb_state == 2 ? 45.0 : 20.0);
        } else {
            o->type = MA2_ORD_FIGHT_MOVE; o->point = b->hgc_pt; o->stop = 3.0f; o->target = m->actor; o->fire = 1;
            o->climb = 1; o->climb_pt = b->hgc_pt;
            snprintf(b->note, sizeof b->note, "climb");
            return;
        }
    }
    if (b->flank_on) {
        /* Batch 4 FlankKeep: in a group fight the threat pick flips between the enemies every half
         * second or so, and each flip ended the trip (156 of 280 flanks in batch 2-3 logs, ~4 s in;
         * 55 arrived). A switch to an enemy within 120 m of the one we set out against keeps the
         * trip: the spot is still on that group's side (70 m kept only 3 of 12 switches in the first smoke).
         * Distances are from the first enemy, so a switch back to it is always kept. */
        if (b->tac_kind == G_ATTACK && b->tac_tgt == b->flank_tgt && m->actor != b->flank_tgt && flankkeep_on() &&
            hdist(m->pos, b->flank_tpos) < 120.0f) {
            if (diag_on()) fprintf(stderr, "[BOT-FLANK] t=%.1f bot=%d keeps the trip: target %08X -> %08X (%.0f m apart)\n",
                                   b->now, b->index, b->flank_tgt, m->actor, hdist(m->pos, b->flank_tpos));
            b->flank_tgt = b->tac_tgt = m->actor; c7_add("flanks", "kept_switch", 1);
        }
        if (b->tac_kind != G_ATTACK || b->tac_tgt != b->flank_tgt || m->actor != b->flank_tgt) {
            b->flank_on = 0; c7_add("flanks", "dropped", 1); c7_add("flanks", b->tac_kind != G_ATTACK ? "drop_other" : "drop_switch", 1);
            if (diag_on() && b->tac_kind == G_ATTACK) fprintf(stderr, "[BOT-FLANK] t=%.1f bot=%d drops the trip: target %08X -> %08X (%.0f m apart)\n", b->now, b->index, b->flank_tgt, m->actor, hdist(m->pos, b->flank_tpos));
        } else {
            if (m->actor == b->flank_tgt0) b->flank_tpos = m->pos;   /* the anchor stays on the first enemy: no drift along a chain of switches */
            if (hdist(me->pos, b->tac_pt) < 25.0f) {
                b->flank_on = 0; b->tac_until = b->now; c7_add("flanks", "arrived", 1); c7_add("flanks", "arrive_s", b->now - b->flank_t0);
                if (diag_on()) fprintf(stderr, "[BOT-FLANK] t=%.1f bot=%d arrived beside %08X after %.0f s\n", b->now, b->index, m->actor, b->now - b->flank_t0);
            }
            else if (b->now >= b->tac_until) { b->flank_on = 0; c7_add("flanks", "timed_out", 1); }
        }
        if (!b->flank_on) flank_release(b);
    }
    if (!b->flank_on && b->now >= b->tac_until && flanks_on()) flank_try(b, w, me, nav, sn, m);
    if (b->tac_kind == G_ATTACK && b->tac_tgt == m->actor && b->now < b->tac_until && hdist(me->pos, b->tac_pt) > 5.0f) {
        o->type = MA2_ORD_FIGHT_MOVE; o->point = b->tac_pt; o->stop = 4.0f; o->target = m->actor; o->fire = 1;
        snprintf(b->note, sizeof b->note, b->flank_on ? "flank" : "to-firing-pos");
        return;
    }
    reload_cover_metric(b, me, m, st);
    if (peek_order(b, w, me, m, sn, st, o)) return;
    /* (0a) High ground, stay up there (bots batch 5, RECOMP_BOT_HG_STAY / [Bots] HighGroundStay): the plain engage
     * strafes and keeps its band, which walked a BattleArmor off its roof within 2 s of arriving (NRC run hg-nrc1:
     * perch time 0 on a67af79). Up on a roof or a hilltop with the target in sight and in reach, hold the spot we
     * stand on and fire from it. Off: the old engage. [C7] hg_stay holds_ / hold_s_<ba|mech>. */
    if ((k == MA2_KIND_BATTLEARMOR || k == MA2_KIND_MECH) && (b->raised || b->hg_hill) && m->visible &&
        hdist(me->pos, m->pos) <= my_reach(me) && c7_on("hg_stay", "RECOMP_BOT_HG_STAY", "HighGroundStay", 1)) {
        int kb = k == MA2_KIND_MECH;
        if (b->now - b->hg_hold_t > 0.5 || hdist(me->pos, b->hg_hold) > 6.0f) {   /* a new stretch up here: hold where we stand */
            b->hg_hold = me->pos; c7_add("hg_stay", kb ? "holds_mech" : "holds_ba", 1);
        }
        b->hg_hold_t = b->now;
        c7_add("hg_stay", kb ? "hold_s_mech" : "hold_s_ba", b->dt);
        {   /* HoldStep (default on, RECOMP_BOT_HOLD_STEP / [Bots] HoldStep; playtest 11-06 keep moving like a player does): a 'Mech holding a
             * hill-top with its target in sight but no shot stood there -- d5_25: 28 s of hold-high stands >= 1.5 s (b4 6.1 s at 1166,1439, b3
             * 3.6 s x2 at 854,1276). A player sidesteps along the crest for a line. No shot for 2 s: the hold moves 5 m across the line to the
             * target, sides alternating, onto ground at the same height (hills only: a roof is not terrain), every 2.5 s at most. [C7] hg_stay: steps. */
            static double hs_t[16], hs_since[16]; static int hs_side[16];
            int hx = b->index & 15;
            if (b->now < hs_t[hx] - 10.0) hs_t[hx] = 0.0;                                 /* a new match */
            if (hdist(me->pos, b->hg_hold) > 6.0f || b->now - hs_since[hx] > 30.0 || b->now < hs_since[hx]) hs_since[hx] = b->now;
            double quiet = b->now - (b->last_fire_t > hs_since[hx] ? b->last_fire_t : hs_since[hx]);
            if (kb && b->hg_hill && !b->raised && quiet > 2.0 && b->now >= hs_t[hx] && c7_on("holdstep", "RECOMP_BOT_HOLD_STEP", "HoldStep", 1)) {
                Ma2Vec3 tl = hnorm(vsub(m->pos, me->pos)), pp = v3(tl.z, 0.0f, -tl.x);
                int ok0 = 0; float g0 = ma2_bot_ground_y(me->pos.x, me->pos.z, &ok0);
                hs_t[hx] = b->now + 2.5;
                for (int tries = 0; tries < 2; tries++) {
                    int side = hs_side[hx] ? 1 : -1;
                    Ma2Vec3 cand = vadd(b->hg_hold, vscale(pp, 5.0f * (float)side));
                    int ok1 = 0; float g1 = ma2_bot_ground_y(cand.x, cand.z, &ok1);
                    hs_side[hx] = !hs_side[hx];
                    if (ok0 && ok1 && fabsf(g1 - g0) < 1.5f && ma2_trav_ready() && ma2_trav_clear(cand.x, cand.z, b->now) >= 2.0f) {
                        cand.y = g1; b->hg_hold = cand; c7_add("hg_stay", "steps", 1); break;
                    }
                }
            }
        }
        o->type = MA2_ORD_FIGHT_MOVE; o->point = b->hg_hold; o->stop = 2.0f; o->exact = 1; o->target = m->actor; o->fire = 1;
        snprintf(b->note, sizeof b->note, "hold-high");
        return;
    }
    o->type = MA2_ORD_ENGAGE;
    o->target = m->actor;
    o->point = mem_predict(b, m);
    o->standoff = b->standoff; o->band_min = b->band_min; o->band_max = b->band_max;
    o->stop = b->band_max * (b->standoff ? 1.0f : 0.9f);
    if (!m->visible && hdist(me->pos, m->pos) < 12.0f) {   /* searched its last-known spot: it is gone */
        m->last_known -= MA2B_MEM_FORGET; m->last_seen -= MA2B_MEM_FORGET;
        b->next_eval = 0.0;
    }
}

static void order_cover(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, Ma2Order *o, int retreat)
{
    Mem *t = b->tgt;
    int want = retreat ? G_RETREAT : G_COVER;
    /* RetreatOn (default on, RECOMP_BOT_RETREAT_ON / [Bots] RetreatOn; playtest 11-06 keep moving): the retreat point was held its full
     * 6 s, so a bot that got there stood still while still being hit (d5_25: 10 s of retreat stands >= 1.5 s, b7 3.2 s at 999,1503). At the
     * point 1 s and still hit: that cover is no cover -- on to the next one now. Safe behind it, it holds. [C7] retreaton: next. */
    static double ro_at[16];
    int rx = b->index & 15;
    if (retreat && b->tac_kind == want && b->now < b->tac_until && hdist(me->pos, b->tac_pt) < 6.0f) {
        if (ro_at[rx] <= 0.0 || b->now < ro_at[rx]) ro_at[rx] = b->now;
        if (b->now - ro_at[rx] > 1.0 && b->n_attackers > 0 && b->dmg_recent > 0.0f && c7_on("retreaton", "RECOMP_BOT_RETREAT_ON", "RetreatOn", 1)) {
            b->tac_until = b->now; ro_at[rx] = 0.0; c7_add("retreaton", "next", 1);
        }
    } else ro_at[rx] = 0.0;
    if (b->tac_kind != want || b->now >= b->tac_until) {
        Ma2Vec3 p;
        if (tac_cover(b, w, me, t, retreat, sn, &p)) { b->tac_pt = p; b->tac_kind = want; b->tac_until = b->now + (retreat ? 6.0 : 4.0); spot_claim(me->nation, b->index, 0, p, b->tac_until); }
        else {
            /* No hiding place: back off from the attackers (retreat) or keep fighting (cover). */
            b->tac_fail_until = b->now + 3.0;
            if (retreat) {
                Ma2Vec3 away = hnorm(vsub(me->pos, b->threat_c));
                b->tac_pt = vadd(me->pos, vscale(hlen(away) > 0.1f ? away : dir_of_heading(me->yaw + MA2B_PI), 60.0f));
                b->tac_kind = want; b->tac_until = b->now + 4.0;
            } else { b->tac_kind = 0; b->next_eval = 0.0; b->interrupt = 1; }
        }
    }
    if (b->tac_kind != want) { o->type = MA2_ORD_IDLE; return; }
    o->type = MA2_ORD_FIGHT_MOVE;
    o->point = b->tac_pt; o->stop = 4.0f;
    o->target = t ? t->actor : 0;
    o->fire = t != NULL;
    if (retreat) add_danger(b, me, o);
    snprintf(b->note, sizeof b->note, retreat ? "retreat" : "cover");
}

/* VTOL tether (the gunship cannot take a power-up itself): something hooked on it. */
static int vtol_carrying(const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn)
{
    (void)w; (void)sn;
    return me->cargo > 0;   /* the tether's own count: a power-up on a roof beside us looked "hooked" */
}
/* Who gets the power-up we carry: the teammate in a 'Mech, tank or BattleArmor who needs it
 * most (worst damage), not too far. */
static int cargo_mate(const Ma2World *w, const Ma2PlayerView *me)
{
    int best = -1; float bs = -1e9f;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q == me || !q->alive || q->nation != me->nation) continue;
        if (q->kind != MA2_KIND_MECH && q->kind != MA2_KIND_TANK && q->kind != MA2_KIND_BATTLEARMOR) continue;
        float d = hdist(q->pos, me->pos);
        if (d > 600.0f || ma2_bot_air_blocked(q->pos.x, q->pos.z)) continue;
        float s = 2.0f * (1.0f - hp_frac(q->hp, q->hp_max)) - d / 600.0f + (q->is_human ? 0.15f : 0.0f);
        if (s > bs) { bs = s; best = i; }
    }
    return best;
}
static void order_powerup(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *o)
{
    if (kind_ok(me->kind) == MA2_KIND_VTOL) {
        /* Ferry: hook a power-up with the tether, fly it to a teammate, drop it over him. */
        if (b->vt_carry) {
            int m = cargo_mate(w, me);
            if (m < 0) { b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return; }
            order_travel(o, w->players[m].pos, 4.0f, 1); o->cargo = 2;
            snprintf(b->note, sizeof b->note, "ferry");
            return;
        }
        {
            float pd; int pi = nearest_powerup(b, w, me, &pd);
            if (pi < 0) { b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return; }
            if (b->goal != G_POWERUP || b->now < b->fetch_since) b->fetch_since = b->now;
            if (w->pickups[pi].entity != b->fetch_pk) {
                b->fetch_pk = w->pickups[pi].entity; b->fetch_near_t = 0.0;
                c7_add("f18", "fetches", 1);
            }
            {   /* (F18) over it, low, and still nothing on the tether after 6 s (several hook taps): it
                 * cannot be hooked from here -- the team leaves that spot, we rest from fetching */
                int over = hdist(me->pos, w->pickups[pi].pos) < 14.0f;
                if (!over) b->fetch_near_t = 0.0;
                else if (b->fetch_near_t <= 0.0 || b->now < b->fetch_near_t) b->fetch_near_t = b->now;
                if (vtol_fetch_fix_on() && over && b->now - b->fetch_near_t > 6.0) {
                    vtol_noreach_add(b, me, w->pickups[pi].pos);
                    c7_add("f18", "give_up_hook", 1);
                    if (diag_on()) fprintf(stderr, "[BOT-FETCH] t=%.1f bot=%d gives up %08X at %.0f,%.0f,%.0f (no hook in 6 s, VTOL y=%.1f) -- no fetching 30 s\n",
                                           b->now, b->index, w->pickups[pi].entity, w->pickups[pi].pos.x, w->pickups[pi].pos.y, w->pickups[pi].pos.z, me->pos.y);
                    b->fetch_off_until = b->now + 30.0; b->fetch_near_t = 0.0;
                    b->fetch_since = b->now; b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return;
                }
            }
            if (b->now - b->fetch_since > 25.0) {          /* the hook will not take it: leave that one */
                int j, old = 0;
                for (j = 0; j < 16; j++) if (g_pu_taken[j].until < g_pu_taken[old].until) old = j;
                g_pu_taken[old].pos = w->pickups[pi].pos; g_pu_taken[old].until = b->now + 60.0; g_pu_taken[old].nation = me->nation;
                if (vtol_fetch_fix_on()) { b->fetch_off_until = b->now + 30.0; c7_add("f18", "give_up_time", 1); }   /* no chain onto the next one */
                b->fetch_since = b->now; b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return;
            }
            /* Flown straight through low (no tether) a power-up is the gunship's own: a health pack repairs
             * it, a weapon upgrade raises its missiles. It keeps what it can use -- a pack while damaged, an
             * upgrade while its weapons have a level to go -- and tethers the rest to a teammate. */
            const Ma2PickupView *pk = &w->pickups[pi];
            int self = pk->armor > 0 ? hp_frac(me->hp, me->hp_max) < 0.9f : (wpn_fits(me, pk) && (b->pu_typed || pk->armor == 0));
            if (self) {
                order_travel(o, pk->pos, 2.0f, 1); o->vt_heal = 2;
                snprintf(b->note, sizeof b->note, pk->armor > 0 ? "self-pack" : "self-upgrade");
                return;
            }
            order_travel(o, w->pickups[pi].pos, 4.0f, 1); o->cargo = 1;
            snprintf(b->note, sizeof b->note, "fetch");
            return;
        }
    }
    /* PickupTouch (default on, RECOMP_BOT_PICKUP_TOUCH / [Bots] PickupTouch): "within 4 m" counted as collected, so a bot
     * turned away 4 m short of power-ups it never got (playtest 11-04: walk onto the object's own coordinates, and only the game
     * handing it over says it worked). Now the power-up we walked to is collected when the game takes it off the map
     * while we stand within 6 m of where it was; until then the order is a touch onto its exact point (ctrl_exact_walk).
     * [C7] putouch: got (gone with us on it), gone_far (someone else took it). Off: the old 4 m rule. */
    int touch = c7_on("putouch", "RECOMP_BOT_PICKUP_TOUCH", "PickupTouch", 1);
    if (touch && b->pu_keep) {
        int still = 0;
        for (int i = 0; i < w->npickups && i < MA2_BOT_MAX_PICKUPS; i++) if (w->pickups[i].entity == b->pu_keep) still = 1;
        if (!still) {
            int got = hdist(me->pos, b->pu_keep_pos) < 6.0f + 0.5f * hypotf(me->vel.x, me->vel.z);   /* TouchStride: run over it at speed, seen gone up to ~0.5 s later */
            c7_add("putouch", got ? "got" : "gone_far", 1);
            if (got && b->pu_keep_val) {   /* (F6) mark the spot taken for the team */
                int j, old = 0;
                for (j = 0; j < 16; j++) if (g_pu_taken[j].until < g_pu_taken[old].until) old = j;
                g_pu_taken[old].pos = b->pu_keep_pos; g_pu_taken[old].until = b->now + (pickup_mark_on() ? 4.0 : 25.0); g_pu_taken[old].nation = me->nation;
                c7_add("f25", "marks", 1);
                if (b->pu_deny == b->pu_keep) c7_add("f66", k_f66_taken[b->skill & 3], 1);
            }
            b->pu_keep = 0; b->next_eval = 0.0; o->type = MA2_ORD_IDLE;
            return;
        }
        /* PuRefused (default on, RECOMP_BOT_PU_REFUSED / [Bots] PuRefused): the game hands a weapon upgrade over only to a body that can
         * raise THAT weapon; wpn_room says only that some class has a level to go. au3_64 bot 7 (CTF NRC, 11-04) walked over upgrade 01910290 at
         * 1095,1026 every 1.5 s for 13 s (t=154.4-167.5; bots 8 and 10 the same, 26-29 s) and the game never took it off the map. Still there 4 s after it first came within 3 m (the touch walk
         * passes over it): this one is not ours -- skipped 60 s (pu_bad), the goal looks again. [C7] purefused: n. */
        if (still && c7_on("purefused", "RECOMP_BOT_PU_REFUSED", "PuRefused", 1)) {
            if (b->pu_on_ent != b->pu_keep) { b->pu_on_ent = b->pu_keep; b->pu_on_t = 0.0; }
            if (b->pu_on_t <= 0.0 && hdist(me->pos, b->pu_keep_pos) < 3.0f) b->pu_on_t = b->now;
            /* PuRefusedKind (default on, RECOMP_BOT_PU_REFUSED_KIND / [Bots] PuRefusedKind; film review 11-08): 2 s, and a refused WEAPON upgrade
             * rules out every weapon upgrade for this body (A_61a BA b9 circled 1-4 m from two weapon upgrades 140-143 s and again 147-154 s). */
            int prk = c7_on("purefusedkind", "RECOMP_BOT_PU_REFUSED_KIND", "PuRefusedKind", 1);
            if (b->pu_on_t > 0.0 && b->now - b->pu_on_t > (prk ? 2.0 : 4.0)) {
                int slot = b->pu_bad_next++ & 3;
                b->pu_bad[slot].ent = b->pu_keep; b->pu_bad[slot].until = b->now + 60.0; c7_add("purefused", "n", 1);
                if (prk) for (int q = 0; q < w->npickups && q < MA2_BOT_MAX_PICKUPS; q++)
                    if (w->pickups[q].entity == b->pu_keep && w->pickups[q].armor == 0 && w->pickups[q].wclass < 0) { b->pu_wref_actor = me->actor; c7_add("purefusedkind", "weapon_off", 1); }
                if (diag_on()) fprintf(stderr, "[BOT-PUREFUSED] t=%.1f bot=%d power-up %08X at %.0f,%.0f: 4 s at it and the game did not hand it over, skipped 60 s\n", b->now, b->index, b->pu_keep, b->pu_keep_pos.x, b->pu_keep_pos.z);
                b->pu_keep = 0; b->pu_on_ent = 0; b->pu_on_t = 0.0; b->next_eval = 0.0; o->type = MA2_ORD_IDLE;
                return;
            }
        }
    }
    float pd; int pi = nearest_powerup(b, w, me, &pd);
    if (pi < 0) { b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return; }
    b->pu_keep = w->pickups[pi].entity; b->pu_keep_pos = w->pickups[pi].pos; b->pu_keep_val = powerup_value(b, me, &w->pickups[pi]) > 0.0f;
    if (!touch && pd < 4.0f) {                                                /* on it: collected (old rule) */
        int j, old = 0;
        /* (F6) only one the game hands us is gone: a pack we cannot take stays for a teammate */
        if (powerup_value(b, me, &w->pickups[pi]) > 0.0f) {
            for (j = 0; j < 16; j++) if (g_pu_taken[j].until < g_pu_taken[old].until) old = j;
            g_pu_taken[old].pos = w->pickups[pi].pos; g_pu_taken[old].until = b->now + (pickup_mark_on() ? 4.0 : 25.0); g_pu_taken[old].nation = me->nation;
            c7_add("f25", "marks", 1);
            if (b->pu_deny == w->pickups[pi].entity) c7_add("f66", k_f66_taken[b->skill & 3], 1);
        }
        b->pu_keep = 0;
        b->next_eval = 0.0; o->type = MA2_ORD_IDLE;
        return;
    }
    order_travel(o, w->pickups[pi].pos, 1.0f, 1);
    /* PuReach (default on, RECOMP_BOT_PU_REACH / [Bots] PuReach): a 'Mech at 18 m/s turns on a ~22 m circle, so it never got inside the 15 m
     * exact-walk reach and orbited the pack (r1_66 CTF Witch's Cauldron b4 71.5-82 at 1385,960, 15-28 m off, -1770 hp): 30 m for 'Mechs / tanks. */
    if (touch) { o->touch = 1; o->stop = 0.5f; o->exact = 1;
                 o->exact_r = (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK) && c7_on("pureach", "RECOMP_BOT_PU_REACH", "PuReach", 1) ? (me->kind == MA2_KIND_MECH ? 40.f : 30.f) : 15.f; }   /* (d5_66 b9: a 'Mech at 30 m/s orbited the pack on a ~37 m circle, 161.6-171.4) */
    if (pd > 25.0f) add_danger(b, me, o);                       /* the last few metres: straight in */
    snprintf(b->note, sizeof b->note, "powerup");
}

static void order_support(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, Ma2Order *o)
{
    Mem *m = mem_by_actor(b, b->support_tgt);
    const Ma2PlayerView *e = player_by_actor(w, b->support_tgt);
    if (!e || !e->alive) { b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return; }
    if (!m) {   /* a teammate's attacker we have not seen: go by the call-out */
        order_travel(o, e->pos, 30.0f, 1); o->target = e->actor;
        return;
    }
    if (b->tac_kind != G_SUPPORT || b->now >= b->tac_until || b->tac_tgt != m->actor) {
        /* Flank: 60-110 degrees off the line from our teammate to its attacker. */
        float base = heading_of(vsub(b->support_from, m->pos));
        Ma2Vec3 p;
        float side = (b->index & 1) ? 1.4f : -1.4f;
        if (tac_firing(b, w, me, m, base + side, sn, &p)) { b->tac_pt = p; b->tac_kind = G_SUPPORT; b->tac_tgt = m->actor; b->tac_until = b->now + 8.0; }
        else { b->tac_pt = mem_predict(b, m); b->tac_kind = G_SUPPORT; b->tac_tgt = m->actor; b->tac_until = b->now + 3.0; }
    }
    o->type = MA2_ORD_FIGHT_MOVE; o->point = b->tac_pt; o->stop = 6.0f; o->target = m->actor; o->fire = 1;
    snprintf(b->note, sizeof b->note, "support");
}

static void order_hack(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *o)
{
    const Ma2PlayerView *e = player_by_actor(w, b->hack_tgt);
    if (me->qte_active) { o->type = MA2_ORD_HACK; o->target = me->clamped_to ? me->clamped_to : b->hack_tgt; return; }
    if (!e || !e->alive || e->kind != MA2_KIND_MECH) { b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return; }
    o->type = MA2_ORD_HACK; o->target = e->actor; o->point = e->pos; o->stop = 2.0f; o->fire = 1;
    snprintf(b->note, sizeof b->note, "hack");
}

/* Where a gunship sets down to change ride: open ground (6 m clear on the walk grid) 30-40 m off the
 * ride, outside the no-fly patches, the nearest such spot; the pilot walks the rest. Set down any
 * closer it came to rest on the 'Mech's edge, 4-10 m up, and could not get out; a spot inside the
 * base buildings left it with no route, hanging. */
static int vtol_land_spot(const Ma2Senses *sn, const Ma2PlayerView *me, Ma2Vec3 c0, Ma2Vec3 *out)
{
    int found = 0; float best = 1e9f;
    for (int k = 0; k < 16; k++) {
        Ma2Vec3 p = vadd(c0, vscale(dir_of_heading((float)(k & 7) * MA2B_PI / 4.0f), k < 8 ? 30.0f : 40.0f));
        if (clearance_at(sn, p.x, p.z) < 6.0f || ma2_bot_air_blocked(p.x, p.z)) continue;
        float d = hdist(me->pos, p);
        if (d < best) { best = d; *out = p; found = 1; }
    }
    return found;
}

static void order_swap(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, Ma2Order *o)
{
    int vi = find_vehicle(w, b->swap_veh);
    if (vi < 0 || !vehicle_usable(b, w, me, &w->vehicles[vi])) {
        if (b->swap_veh && ma2_bot_switch_on(MA2_SW_SWAP_COOL)) { b->swap_cool_until = b->now + 10.0; ma2_c3_count(MA2_C3_SWAP_COOL_BLOCKS); }   /* F1: given up */
        b->swap_veh = 0; b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return;
    }
    o->want_vehicle = b->swap_veh;   /* C3: the swap target is claimed like any ride */
    /* push-x SwapUnstick (RECOMP_BOT_SWAP_UNSTICK / [Bots] SwapUnstick): a swap that has not moved the bot 4 m in 10 s (a BattleArmor
     * pinned short of the entry at the NRC spawn cliff, 290-400,1180-1285; a landed gunship whose pilot never gets out) is given up:
     * that ride is not offered again for a minute and the bot carries on. [C7] swapfix: tries, giveups_<kind>, unstuck_s. */
    if (c7_on("swapfix", "RECOMP_BOT_SWAP_UNSTICK", "SwapUnstick", 1)) {
        if (b->swp_t0 <= 0.0 || b->now - b->swp_last > 2.0 || hdist(me->pos, b->swp_pos) > 4.0f) {
            if (b->swp_t0 <= 0.0 || b->now - b->swp_last > 2.0) c7_add("swapfix", "tries", 1);
            b->swp_t0 = b->now; b->swp_pos = me->pos;
        }
        /* SwapHeadway (default on, RECOMP_BOT_SWAP_HEADWAY / [Bots] SwapHeadway): moving is not getting there. The au3_60 VTOL (bot 10) circled
         * its ride for 116 s, 10-40 m a leg, so "not moved 4 m in 10 s" never held. Headway = metres to the ride's entry, plus a VTOL's height
         * over the ride: not 2 m better in 20 s is a stuck swap too. */
        {
            const Ma2VehicleView *sv = &w->vehicles[vi]; Ma2Vec3 sat = sv->has_entry ? sv->entry : sv->pos;
            float m = hdist(me->pos, sat) + (me->kind == MA2_KIND_VTOL ? fmaxf(0.0f, me->pos.y - sv->pos.y) : 0.0f);
            if (b->swh_t <= 0.0 || b->now - b->swp_last > 2.0 || m < b->swh_best - 2.0f) { b->swh_t = b->now; b->swh_best = m; }
            else if (b->now - b->swh_t > 20.0 && c7_on("swapheadway", "RECOMP_BOT_SWAP_HEADWAY", "SwapHeadway", 1)) { c7_add("swapheadway", "giveups", 1); b->swp_t0 = b->now - 11.0; b->swh_t = 0.0; }
        }
        b->swp_last = b->now;
        if (b->now - b->swp_t0 > 10.0) {
            c7_add("swapfix", me->kind == MA2_KIND_BATTLEARMOR ? "giveups_ba" : me->kind == MA2_KIND_VTOL ? "giveups_vtol" : "giveups_other", 1);
            c7_add("swapfix", "unstuck_s", b->now - b->swp_t0);
            if (diag_on()) fprintf(stderr, "[SWAP-UNSTICK] t=%.1f bot=%d kind=%d gave up swapping at %.0f,%.0f (not moved 4 m in 10 s)\n", b->now, b->index, me->kind, me->pos.x, me->pos.z);
            no_board_for(b, b->swap_veh, 60.0); b->swap_veh = 0; b->swap_cool_until = b->now + 10.0; b->next_eval = 0.0; b->swp_t0 = 0.0;
            snprintf(b->note, sizeof b->note, "swap-stuck");
            o->type = MA2_ORD_IDLE; return;
        }
    }
    {
        const Ma2VehicleView *v = &w->vehicles[vi];
        Ma2Vec3 at = v->has_entry ? v->entry : v->pos;
        if (me->kind == MA2_KIND_VTOL && b->now >= b->next_eject) {   /* once setting down, it carries on */
            /* A gunship sets down clear of the ride -- 18 m off, on the side it comes in from -- and only
             * once it is there and slow: it began setting down at speed near the entry point, coasted on
             * and came to rest on top of the 'Mech, 10 m up, Y held for good. */
            float dx = me->pos.x - v->pos.x, dz = me->pos.z - v->pos.z, l = sqrtf(dx * dx + dz * dz);
            Ma2Vec3 lp = v->pos; int far;
            if (b->swap_lp_veh != v->actor || hdist(b->swap_lp, v->pos) > 45.0f) b->swap_lp_veh = vtol_land_spot(sn, me, v->pos, &b->swap_lp) ? v->actor : 0;
            if (b->swap_lp_veh == v->actor) { lp = b->swap_lp; far = hdist(me->pos, lp) > 9.0f; }
            else {   /* no open spot found: straight out from the hull, 30 m */
                if (l > 1.0f) { lp.x += dx / l * 30.0f; lp.z += dz / l * 30.0f; } else lp.x += 30.0f;
                far = l < 22.0f || l > 45.0f;
            }
            /* ...or anywhere on the way in that will do: 18-60 m from the ride, open ground, slowed
             * down -- the pilot walks the rest (making for the exact spot it circled the base) */
            if (l > 18.0f && l < 60.0f && clearance_at(sn, me->pos.x, me->pos.z) >= 6.0f && !ma2_bot_air_blocked(me->pos.x, me->pos.z)) far = 0;
            if (far || hypotf(me->vel.x, me->vel.z) > 8.0f) {
                order_travel(o, lp, 4.0f, 1); add_danger(b, me, o); snprintf(b->note, sizeof b->note, "swap-land");
                return;
            }
        /* SwapHold (default on, RECOMP_BOT_SWAP_HOLD / [Bots] SwapHold): a VTOL in the 2 s after an eject order fell into this walk-to-the-entry branch
         * and was flown back at the ride with exit let go, so its set-down never ran on: au3_60 bot 10 (CTF Bogged Down, 11-04) swap-land / swap-go /
         * swap-eject about 50 times over its base, t=73.6-189.7, until shot down. A gunship setting down carries on (the exit stays held). [C7] swaphold: frames. */
        } else if (me->kind == MA2_KIND_VTOL && c7_on("swaphold", "RECOMP_BOT_SWAP_HOLD", "SwapHold", 1)) { c7_add("swaphold", "frames", 1); }
        else if (hdist(me->pos, at) > 12.0f) { order_travel(o, at, 8.0f, 1); add_danger(b, me, o); snprintf(b->note, sizeof b->note, "swap-go"); return; }
        if (b->now >= b->next_eject || me->kind == MA2_KIND_VTOL) {   /* a VTOL's exit is held (Y), not tapped */
            if (b->now >= b->next_eject) b->st_eject++;
            o->type = MA2_ORD_EJECT; b->next_eject = b->now + 2.0;
            b->veh = b->swap_veh;                 /* on foot next: board it at once */
            b->left_veh = me->actor; b->left_until = b->now + 20.0;   /* not straight back into the one we are leaving */
            b->swap_cool_until = b->now + 20.0;   /* F1: done -- the next swap waits (SwapCool) */
            snprintf(b->note, sizeof b->note, "swap-eject");
        } else o->type = MA2_ORD_IDLE;
    }
}

/* BattleArmor: climb onto the VTOL that came down for us; once on, ride shooting what we see,
 * and let go near where we were going -- or when the VTOL is going down, or after a minute. */
static void order_ride(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, Ma2Order *o)
{
    const Ma2PlayerView *v = player_by_actor(w, me->riding ? me->riding : b->lift_vtol);
    if (!v || !v->alive) { b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return; }
    if (me->riding) {
        int li = lift_find(me->actor);
        if (b->ride_since <= 0.0) {
            b->ride_since = b->now; b->ride_from = me->pos;
            if (v->is_human && ba_board_player_on()) c7_add("boardpl", "boarded", 1);
        }
        /* ReboardFix (playtest 11-04 a bot would not board the player's VTOL again): the boarding clock (hboard_t, 13.5 s to make it)
         * kept running through the ride, so once set down it read 20 s, gave the player's VTOL up and refused it 30 s
         * (botrec t=32.8 boardpl.gave_up). Aboard, the clock and any refusal of this VTOL are cleared. */
        b->hboard_vtol = 0; if (b->lift_off == v->actor) b->lift_off_until = 0.0;
        o->type = MA2_ORD_IDLE;
        if (b->tgt && b->tgt->visible) { o->target = b->tgt->actor; o->fire = 1; }
        /* Carrying their flag: ride it all the way home to our stand (the VTOL is the fastest way
         * there), letting go only there or if the VTOL is going down. */
        const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, me->nation, 1), *theirs = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
        int carrying = mine && theirs && theirs->carrier && theirs->carrier == me->player;
        /* CaptureReset: scored aboard -- the ride starts over (its 60 / 90 s limits ran from the pickup at their flag) */
        if (b->ride_carry && !carrying && capture_reset_on()) { b->ride_since = b->now; b->ride_from = me->pos; c7_add("capreset", "rider_clock_reset", 1); }
        b->ride_carry = carrying;
        int run = frun_member(w, me->actor, me->nation) && frun_member(w, v->actor, me->nation) && theirs;
        if (run && !carrying) {
            /* Flag run, inbound: off right over their flag once the VTOL has sunk to it (let go at
             * 49 m and 27 m out, it landed on a roof by the flag and stayed there -- run fr9); held
             * near it 10 s without getting low, off anyway. */
            float hd = hdist(me->pos, theirs->pos);
            if (hd >= 30.0f) b->frun_near_t = 0.0; else if (b->frun_near_t <= 0.0) b->frun_near_t = b->now;
            if (hp_frac(v->hp, v->hp_max) < 0.25f || (hd < 14.0f && me->pos.y - theirs->pos.y < 10.0f) ||
                /* DropSooner (default on, RECOMP_BOT_DROP_SOONER / [Bots] DropSooner): held within 30 m of their flag 4 s (was 10) without
                 * getting low, off anyway -- the VTOL hovered 11-12 s over their stand under fire, 1000 -> 400 hp (au5_60 b2, t=24.8-36.2) */
                (b->frun_near_t > 0.0 && b->now - b->frun_near_t > (c7_on("dropsooner", "RECOMP_BOT_DROP_SOONER", "DropSooner", 1) ? 4.0 : 10.0)) || b->now - b->ride_since > 90.0) {
                o->detach = 1; snprintf(b->note, sizeof b->note, "flagrun-drop");
            } else snprintf(b->note, sizeof b->note, "flagrun-ride");
            return;
        }
        if (v->is_human && ba_board_player_on() && !carrying) {
            /* On the player's VTOL: he decides where. Off when he sets down (60 m or more from where we got
             * on), over their flag, when his VTOL is going down, or after 90 s. */
            float agl = v->pos.y - ground_at(sn, v->pos.x, v->pos.z, v->pos.y - 30.0f);
            int flag = theirs && theirs->state != MA2_OBJS_CARRIED && hdist(me->pos, theirs->pos) < 40.0f;
            if (hp_frac(v->hp, v->hp_max) < 0.3f || flag || b->now - b->ride_since > 90.0 ||
                (agl < 7.0f && hdist(me->pos, b->ride_from) > 60.0f)) {
                o->detach = 1; c7_add("boardpl", flag ? "drop_flag" : agl < 7.0f ? "drop_landed" : "drop_other", 1);
                if (flag) b->pdrop_t = b->now;   /* DropGrab (obj_ctf) */
                snprintf(b->note, sizeof b->note, "ride-off-player");
            } else snprintf(b->note, sizeof b->note, "ride-player");
            return;
        }
        if (carrying && hp_frac(v->hp, v->hp_max) < 0.3f && hp_frac(v->hp, v->hp_max) >= carry_let_go_hp()) c7_add("carryhold", "held_s", b->dt);
        if (carrying ? (hp_frac(v->hp, v->hp_max) < carry_let_go_hp() || hdist(me->pos, mine->home) < 40.0f || b->now - b->ride_since > 120.0) :
            (hp_frac(v->hp, v->hp_max) < 0.3f || b->now - b->ride_since > 60.0 ||
             /* RiderStay: near where it asked to go -- off once the VTOL has come down there (rider under 14 m up) or within 40 m; it let
              * go 150 m out at 34 m up while the VTOL still flew on in (v29f3 NRC, blue bot 7 t=137.3 at 1262,34,962) */
             (li >= 0 && hdist(me->pos, g_lift[li].dest) < 150.0f &&
              (!c7_on("riderstay", "RECOMP_BOT_RIDER_STAY", "RiderStay", 1) || hdist(me->pos, g_lift[li].dest) < 40.0f ||
               me->pos.y - ground_at(sn, me->pos.x, me->pos.z, me->pos.y - 30.0f) < 14.0f)) ||
             /* RiderStay: cornered aboard -- off only once the VTOL is hanging about (under 10 m/s); flying on is the way out */
             (b->n_attackers > 1 && !(c7_on("riderstay", "RECOMP_BOT_RIDER_STAY", "RiderStay", 1) && hypotf(v->vel.x, v->vel.z) >= 10.0f)))) {
            o->detach = 1;
            snprintf(b->note, sizeof b->note, "ride-off");
        } else snprintf(b->note, sizeof b->note, "ride");
        return;
    }
    b->ride_since = 0.0;
    o->type = MA2_ORD_HACK; o->target = v->actor; o->point = v->pos; o->stop = 2.0f;
    snprintf(b->note, sizeof b->note, "ride-on");
}
/* VTOL: come down beside the BattleArmor that asked; with it on board, fly it to where it was
 * going and come down there so it can drop off. */
static int lift_hold(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *o, uint32_t ba_actor, Ma2Vec3 at0, int aboard);
/* TwoWaitCap (b): one aboard and holding / fetching a second rider -- the clock (a gap over 2 s starts it again); past 12 s
 * the second is given up: our pickup run ends, the hold is off for 45 s, no second hold for 30 s. 1 = given up now. */
static int two_wait_over(Ma2Brain *b, const Ma2PlayerView *me, const Ma2PlayerView *aboard, int fn)
{
    if (b->two_wait_t0 <= 0.0 || b->now - b->two_wait_last > 2.0 || b->now < b->two_wait_t0) b->two_wait_t0 = b->now;
    b->two_wait_last = b->now;
    if (b->now - b->two_wait_t0 <= 12.0) return 0;
    c7_add("twowaitcap", aboard->is_human ? "capped_player_aboard" : "capped_bot_aboard", 1);
    if (diag_on()) fprintf(stderr, "[BOT-TWORIDERS] t=%.1f bot=%d VTOL %08X stops waiting for a second rider after %.0f s: flies %08X%s\n", b->now, b->index, me->actor,
                           b->now - b->two_wait_t0, aboard->actor, aboard->is_human ? " (player)" : "");
    if (g_frun[fn].phase == FR_PICKUP && g_frun[fn].vtol == me->actor && g_frun[fn].ba != aboard->actor) frun_end(fn, b->now, "the second BattleArmor did not make it aboard");
    if (b->muster_pl && b->muster_pl != aboard->player) { b->muster_off = b->muster_pl; b->muster_off_until = b->now + 45.0; b->muster_pl = 0; b->muster_human = 0; }
    b->two_off_until = b->now + 30.0; b->two_wait_t0 = 0.0;
    return 1;
}
static void order_lift(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav, const Ma2Senses *sn, Ma2Order *o)
{
    const Ma2PlayerView *rq[2] = { NULL, NULL }; int nr = 0, two = vtol_two_riders_on();
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++)
        if (w->players[j].alive && w->players[j].riding == me->actor) { if (nr < 2) rq[nr] = &w->players[j]; nr++; }
    if (nr && two) {
        /* VtolTwoRiders: their flag's carrier is flown first, else the player (his ride makes the VtolWaitPlayer run). With one
         * aboard: still holding for a second BattleArmor -- hover for it; or our run's BattleArmor (the player boarded
         * first) is fetched within 150 m, else that run ends and the player's is flown. */
        const Ma2ObjectiveView *tf = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
        int car = tf && tf->carrier && (rq[0]->player == tf->carrier || (nr > 1 && rq[1]->player == tf->carrier));
        if (nr > 1 && ((car && rq[1]->player == tf->carrier) || (!car && rq[1]->is_human && !rq[0]->is_human))) { const Ma2PlayerView *t = rq[0]; rq[0] = rq[1]; rq[1] = t; }
        int fn = me->nation & 7;
        int cap = two_wait_cap_on();
        if (nr == 1 && !car && !(cap && b->now < b->two_off_until && b->now + 60.0 > b->two_off_until)) {   /* TwoWaitCap (b): given up on a second */
            if (b->muster_pl && b->muster_pl != rq[0]->player) {
                if (!(cap && two_wait_over(b, me, rq[0], fn)) && lift_hold(b, w, me, o, b->muster_ba, b->muster_at, 1)) return;
            } else if (g_frun[fn].phase == FR_PICKUP && g_frun[fn].vtol == me->actor && g_frun[fn].ba != rq[0]->actor) {
                const Ma2PlayerView *rb = player_by_actor(w, g_frun[fn].ba);
                if (rb && rb->alive && !rb->riding) {
                    if (hdist(rb->pos, me->pos) < 150.0f) {
                        if (b->now - b->frun_ready_t > 5.0) c7_add("tworiders", "fetch_second", 1);
                        b->frun_ready_t = b->now;
                        if (!(cap && two_wait_over(b, me, rq[0], fn)) && lift_hold(b, w, me, o, rb->actor, rb->pos, 1)) { snprintf(b->note, sizeof b->note, "lift-pickup2"); return; }
                    } else frun_end(fn, b->now, "the run's BattleArmor is too far to fetch with a rider aboard");
                }
            }
        }
    }
    if (nr) {
        const Ma2PlayerView *q = rq[0];
        int li = lift_find(q->actor);
        Ma2Vec3 dest;
        /* Capture the Flag: a rider carrying their flag goes straight home to our stand; a rider who
         * asked for nowhere (the player clawed on) is taken to their flag. Either way it comes down
         * right there, not 160 m short. (Carrying a player who had just taken their flag, it flew
         * on deeper into their side.) */
        const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, me->nation, 1), *theirs = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
        /* CaptureReset (default on, RECOMP_BOT_CAPTURE_RESET / [Bots] CaptureReset; playtest 11-01: after a capture by a BattleArmor rider the
         * ride state must reset, for bots too, so the pair can turn round
         * for another flag run): the ride-home request a flag carrier posts (LiftHuman / LiftCarrier, dest our stand) was found
         * by lift_find long after it went stale -- nothing cleared it -- so after the capture the player, clawed on again, was flown
         * back to our own stand ("lift-fly" to 1364,861) and hovered there for 30 s and more (playtest log, match 2, t=76-106),
         * and no new VtolWaitPlayer run could start (it needs no request). Now a carrier's request ends once its rider no longer
         * carries their flag, and a bot rider that has just scored and stays aboard gets a fresh run (inbound, like the player's
         * VtolWaitPlayer run). [C7] capreset. */
        if (li >= 0 && g_lift[li].car && !(theirs && theirs->carrier && theirs->carrier == q->player) && capture_reset_on()) {
            c7_add("capreset", q->is_human ? "carrier_lift_cleared_player" : "carrier_lift_cleared_bot", 1);
            if (diag_on()) fprintf(stderr, "[BOT-CAPRESET] t=%.1f bot=%d VTOL %08X: rider %08X%s no longer carries their flag -- its ride-home request (to %.0f,%.0f, %.0f s old) is dropped\n",
                                   b->now, b->index, me->actor, q->actor, q->is_human ? " (player)" : "", g_lift[li].dest.x, g_lift[li].dest.z, b->now - g_lift[li].t);
            g_lift[li].ba = 0; g_lift[li].vtol = 0; g_lift[li].car = 0; li = -1;
            b->capr_rider = q->actor; b->capr_t = b->now;
        }
        int ctf = 0;
        if (mine && theirs && theirs->carrier && theirs->carrier == q->player) { dest = mine->home; ctf = 1; }
        else if (mine && theirs && (li < 0 || frun_member(w, q->actor, me->nation)) && theirs->state != MA2_OBJS_CARRIED) { dest = theirs->pos; ctf = 2; }
        else if (li >= 0) dest = g_lift[li].dest;
        else {
            /* A rider who asked for nowhere (the player clawed on): take him out where we were
             * heading -- toward the fight -- come down there, and if he stays on, fly on to the
             * next spot after a while. */
            if (!b->roam_valid || (b->lift_drop_t > 0.0 && b->now - b->lift_drop_t > 10.0)) { pick_roam(b, w, me, nav); b->lift_drop_t = 0.0; }
            dest = b->roam_pt;
        }
        /* PlayerRunHurt: too hurt to start his run (or the call already made): set down where the call said, kept while he
         * hangs on. Their flag carried home (ctf 1) is never turned back. */
        int hurt = 0, hn = me->nation & 7;
        if (q->is_human && ctf != 1 && player_run_hurt_on()) {
            int hm = hurt_mode(hn, me->actor, b->now);
            if (!hm && ctf == 2 && !g_frun[hn].phase && hp_frac(me->hp, me->hp_max) < k_frun_give_up[b->skill & 3]) hm = hurt_decide(w, hn, me, b->skill, b->now);
            if (hm == 2) { dest = g_hurt[hn].pt; dest.y = ground_at(sn, dest.x, dest.z, me->pos.y - 30.0f); ctf = 0; hurt = 1; c7_add("hurtrun", "setdown_s", b->dt); }
        }
        /* Up first: the BattleArmor hangs under the skids, and at pickup height it dragged on the
         * ground and the first bump knocked it off (run fr10, 2 s after the pickup). Climb, edging
         * toward the way out, until it hangs 12 m clear. (A point straight overhead counts as
         * reached -- travel goes by the ground distance -- and the VTOL sat where it was.) */
        if (q->pos.y - ground_at(sn, q->pos.x, q->pos.z, q->pos.y - 20.0f) < 12.0f && hdist(me->pos, dest) > 60.0f) {
            float dd = hdist(me->pos, dest);
            Ma2Vec3 up = { me->pos.x + (dest.x - me->pos.x) / dd * 25.0f, me->pos.y + 25.0f, me->pos.z + (dest.z - me->pos.z) / dd * 25.0f };
            order_travel(o, up, 2.0f, 1);
            snprintf(b->note, sizeof b->note, "lift-climb");
            return;
        }
        /* VtolWaitPlayer (switch RECOMP_BOT_VTOL_WAIT_PLAYER / [Bots] VtolWaitPlayer, default on; design
         * note): a player clawed on and flown to their flag makes this a flag run with him as the BattleArmor --
         * the VTOL circles their flag as cover while he takes it (FR_GRAB, up to 40 s; ends if he walks 200 m off),
         * then comes down beside the carrier for him to claw back on and flies him home (FR_EXTRACT, ctf 1 above).
         * Before, once he let go the VTOL went back to its own business. */
        /* TwoWaitCap (c): our own solo cover run (rolled the tick he clawed on) gives way to his run -- it blocked the
         * VtolWaitPlayer start, so he was flown out with no wait or pick-up at their flag (wd_cap1) */
        if (ctf == 2 && q->is_human && g_frun[me->nation & 7].phase == FR_COVER && g_frun[me->nation & 7].vtol == me->actor && two_wait_cap_on()) {
            c7_add("twowaitcap", "cover_gave_way", 1); frun_end(me->nation & 7, b->now, "the player clawed on (cover gives way)");
        }
        if (ctf == 2 && q->is_human && !g_frun[me->nation & 7].phase && vtol_wait_player_on()) {
            int n = me->nation & 7;
            memset(&g_frun[n], 0, sizeof g_frun[n]);
            g_frun[n].vtol = me->actor; g_frun[n].ba = q->actor; g_frun[n].phase = FR_INBOUND; g_frun[n].skill = b->skill; g_frun[n].human = 1;
            g_frun[n].t0 = g_frun[n].tp = b->now;
            c7_add("frun", "started_player", 1); c7_add("frun", "reached_inbound_player", 1);
            if (q->actor == b->capr_rider && b->now - b->capr_t < 10.0 && b->now >= b->capr_t) c7_add("capreset", "new_run_player", 1);
            if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d bot=%d VTOL %08X flies the player %08X to their flag and will wait for him\n",
                                   b->now, n, b->index, me->actor, q->actor);
        } else if (ctf == 2 && !q->is_human && !hurt && !g_frun[me->nation & 7].phase && q->actor == b->capr_rider && b->now - b->capr_t < 10.0 && b->now >= b->capr_t &&
                   hp_frac(me->hp, me->hp_max) >= k_frun_give_up[b->skill & 3] && vtol_wait_player_on() && capture_reset_on()) {
            /* CaptureReset: a bot that scored aboard stays on for another run, as the player's does */
            int n = me->nation & 7;
            memset(&g_frun[n], 0, sizeof g_frun[n]);
            g_frun[n].vtol = me->actor; g_frun[n].ba = q->actor; g_frun[n].phase = FR_INBOUND; g_frun[n].skill = b->skill;
            g_frun[n].t0 = g_frun[n].tp = b->now; b->capr_rider = 0;
            c7_add("capreset", "new_run_bot", 1); c7_add("frun", "started", 1); c7_add("frun", "reached_inbound", 1);
            if (diag_on()) fprintf(stderr, "[BOT-FLAGRUN] t=%.1f nation=%d bot=%d VTOL %08X flies BattleArmor %08X (scored, still aboard) on another run\n",
                                   b->now, n, b->index, me->actor, q->actor);
        }
        order_travel(o, dest, ctf || hurt ? 3.0f : 4.0f, 1);
        o->beeline = ctf && !hurt;   /* FlagBeeline (ctrl): to their flag / home with it, straight */
        if (hdist(me->pos, dest) < (ctf || hurt ? 45.0f : 160.0f)) {   /* there: come down, let it off */
            o->cargo = 3;
            if (li < 0 && !ctf && !hurt && hdist(me->pos, dest) < 30.0f && b->lift_drop_t <= 0.0) b->lift_drop_t = b->now;
        }
        snprintf(b->note, sizeof b->note, hurt ? "lift-hurt-setdown" : ctf == 1 ? "lift-flag-home" : ctf == 2 ? "lift-to-flag" : "lift-fly");
        lift_fly_trace(b, me, q, dest);
        return;
    }
    b->lift_drop_t = 0.0;
    if (b->frun_cover) {
        const Ma2ObjectiveView *theirs = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
        if (theirs) {
            /* FrunReady (default on, RECOMP_BOT_FRUN_READY / [Bots] FrunReady): our BattleArmor is at their flag (25 m): stop
             * circling 55-95 m out and wait low 18 m from it on our side, so the carrier claws straight back on. It had to chase
             * the VTOL for 6 s with the flag under two defenders' fire, and died (run frun 10-31). */
            const Ma2PlayerView *fba = g_frun[me->nation & 7].ba ? player_by_actor(w, g_frun[me->nation & 7].ba) : NULL;
            const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, me->nation, 1);
            if (fba && fba->alive && mine && hdist(fba->pos, theirs->pos) < 25.0f && c7_on("frunready", "RECOMP_BOT_FRUN_READY", "FrunReady", 1)) {
                float hl = hdist(mine->home, theirs->pos);
                Ma2Vec3 at = theirs->pos;
                if (hl > 1.0f) { at.x += (mine->home.x - theirs->pos.x) / hl * 18.0f; at.z += (mine->home.z - theirs->pos.z) / hl * 18.0f; }
                order_travel(o, at, 4.0f, 1); o->vt_low = 1;
                /* HoverFix (default on, RECOMP_BOT_VTOL_HOVER_FIX / [Bots] HoverFix): wait at claw height over the flag's ground,
                 * not at nap-of-the-earth height over the base's buildings (26 m over the carrier: out of reach, desktop 10-31) */
                if (c7_on("hoverfix", "RECOMP_BOT_VTOL_HOVER_FIX", "HoverFix", 1)) { o->point.y = theirs->pos.y; o->cargo = 3; }
                if (b->now - b->frun_ready_t > 5.0) c7_add("frunready", "waits", 1);
                b->frun_ready_t = b->now;
                snprintf(b->note, sizeof b->note, "flagrun-ready");
                return;
            }
            if (b->now >= b->frun_orbit_t || hdist(me->pos, b->frun_orbit) < 12.0f) {
                b->frun_orbit = vadd(theirs->pos, vscale(dir_of_heading(rng_range(&b->rng, -MA2B_PI, MA2B_PI)), rng_range(&b->rng, 55.0f, 95.0f)));   /* not over the defenders' heads */
                b->frun_orbit_t = b->now + rng_range(&b->rng, 4.0f, 7.0f);
            }
            order_travel(o, b->frun_orbit, 8.0f, 1);
            snprintf(b->note, sizeof b->note, "flagrun-cover");
            return;
        }
    }
    if (!b->lift_ba && b->muster_pl && lift_hold(b, w, me, o, b->muster_ba, b->muster_at, 0)) return;
    {
        const Ma2PlayerView *ba = player_by_actor(w, b->lift_ba);
        if (!ba || !ba->alive) { b->next_eval = 0.0; o->type = MA2_ORD_IDLE; return; }
        order_travel(o, ba->pos, 4.0f, 0); o->cargo = 3;
        { int li = lift_find(ba->actor); if (li >= 0 && g_lift[li].vtol == me->actor) g_lift[li].go_t = b->now; }   /* LiftClaimFix: still on it */
        snprintf(b->note, sizeof b->note, "lift-pickup");
    }
}
/* VtolMuster: down beside a BattleArmor teammate (or off to the side of one still being boarded) for it to claw on. aboard
 * (VtolTwoRiders): a rider already hangs 7-8 m under the skids -- 1.5 m higher over a bot, so he is not dragged. */
static int lift_hold(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *o, uint32_t ba_actor, Ma2Vec3 at0, int aboard)
{
    {
        const Ma2PlayerView *ba = ba_actor ? player_by_actor(w, ba_actor) : NULL;
        Ma2Vec3 at = ba && ba->alive ? ba->pos : at0;
        float d = hdist(me->pos, at);
        if (!ba && d > 1.0f) { at.x += (me->pos.x - at.x) / d * 14.0f; at.z += (me->pos.z - at.z) / d * 14.0f; }
        if (ba && ba->alive && ba->is_human) {
            /* The player: hover a little ahead of him (toward their flag, else the way we were heading) and up at
             * nap-of-the-earth height for him to jet to -- not down on top of him (playtest 10-31: far too close). */
            const Ma2ObjectiveView *tf = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
            Ma2Vec3 to = tf ? tf->pos : b->roam_valid ? b->roam_pt : me->pos;
            float tl = hdist(to, ba->pos);
            if (tl < 5.0f) { to = me->pos; tl = hdist(to, ba->pos); }
            if (tl > 1.0f) { at.x = ba->pos.x + (to.x - ba->pos.x) / tl * 12.0f; at.z = ba->pos.z + (to.z - ba->pos.z) / tl * 12.0f; }
            order_travel(o, at, 4.0f, 0); o->vt_low = 1;
            if (c7_on("hoverfix", "RECOMP_BOT_VTOL_HOVER_FIX", "HoverFix", 1)) { o->point.y = ba->pos.y; o->cargo = 4; }   /* HoverFix: 12 m over him */
            snprintf(b->note, sizeof b->note, aboard ? "muster2-player" : "muster-player");
            return 1;
        }
        order_travel(o, at, 4.0f, 0); o->cargo = 3;
        if (aboard) o->point.y += 1.5f;
        snprintf(b->note, sizeof b->note, aboard ? "muster2" : "muster");
        return 1;
    }
}

/* 'Mech being NeuroHacked: keep moving, jump, answer the minigame, pop the defence. */
static void order_defend(Ma2Brain *b, const Ma2PlayerView *me, Ma2Order *o)
{
    if (b->now >= b->defend_repick || hdist(me->pos, b->defend_pt) < 6.0f) {
        b->defend_pt = vadd(me->pos, vscale(dir_of_heading(rng_range(&b->rng, -MA2B_PI, MA2B_PI)), 30.0f));
        b->defend_repick = b->now + rng_range(&b->rng, 0.8f, 1.6f);
    }
    order_travel(o, b->defend_pt, 2.0f, 0);
    o->hold_fire = 1;
    if (me->kind == MA2_KIND_MECH && b->now >= b->next_jump) { o->jump = 1; b->next_jump = b->now + rng_range(&b->rng, 1.2f, 2.0f); }
}

static void on_actor_change(Ma2Brain *b, const Ma2PlayerView *me)
{
    int k = kind_ok(me->kind);
    if (b->prev_valid && b->prev_kind == MA2_KIND_PILOT && k != MA2_KIND_PILOT) {
        snprintf(b->note, sizeof b->note, "boarded-%s", k_kind[k].name);
        if (k == MA2_KIND_BATTLEARMOR) { g_ba_ok++; c7_add("baenterban", "ba_boarded", 1); }
        b->veh = 0; b->veh_fail = 0; b->swap_veh = 0;
    }
    b->obj_ejected = 0;
    b->hp_prev = me->hp; b->dmg_recent = 0.0f;
    b->next_eval = 0.0; b->interrupt = 1;
    b->tac_kind = 0; b->tac_until = 0.0; b->los_lost_t = 0.0;
}

/* ======================================================================
 * Public API
 * ====================================================================== */
Ma2Brain *ma2_brain_create(int bot_index, int skill, uint32_t seed)
{
    Ma2Brain *b = (Ma2Brain *)calloc(1, sizeof *b);
    uint32_t r;
    const Skill *base;
    if (!b) return NULL;
    skill = sp_force_skill(skill);   /* test hook RECOMP_BOT_SP_SKILL */
    if (skill < MA2_SKILL_RECRUIT) skill = MA2_SKILL_RECRUIT;
    if (skill > MA2_SKILL_ELITE) skill = MA2_SKILL_ELITE;
    b->index = bot_index; b->skill = skill;
    r = mix32(seed ^ mix32((uint32_t)bot_index * 0x9E3779B9u + 0x632BE5ABu));
    base = skill == MA2_SKILL_ELITE && ma2_bots_dragoon() ? &k_dragoon : &k_skill[skill];
    b->sk = *base;
    /* F7: the retreat point rose with skill (Elites left at 32 %, Recruits at 12 %), and cover when
     * outnumbered was a skill roll (Recruits never). Now a falling retreat point and the same cover
     * chance for all, only when badly outnumbered (below). RECOMP_BOT_F7_ELITE=0 / BotEliteFight=0 = old. */
    if (f7_on()) { c7_add("F7elite", "brains", 1); }
    /* DragoonKnobs (playtest 11-05): Dragoon keeps its own retreat point and cover chance (0.32 / 1.0); the F7 line had overwritten them with Elite's 0.16 / 0.85 */
    if (f7_on() && base != &k_dragoon) { static const float rhp[4] = { 0.22f, 0.20f, 0.18f, 0.16f }; b->sk.retreat_hp = rhp[skill]; b->sk.cover_use = 0.85f; }
    /* +-15% personal variance per parameter, plus a personality (aggressive <-> cautious). */
#define VAR(x) ((x) * (1.0f + 0.15f * (2.0f * rng_f(&r) - 1.0f)))
    b->sk.reaction   = VAR(base->reaction);
    b->sk.qte_frac   = clampf(VAR(base->qte_frac), 0.10f, 0.85f);
    b->sk.qte_miss   = clampf(VAR(base->qte_miss), 0.0f, 0.5f);
#undef VAR
    b->aggr    = clampf(base->aggression + 0.30f * (rng_f(&r) - 0.5f), 0.1f, 1.0f);
    b->caution = 0.16f * (rng_f(&r) - 0.5f) - 0.10f * (b->aggr - base->aggression);
    b->marksman = clampf(0.5f + 0.8f * (base->aggression - b->aggr) + 0.6f * (rng_f(&r) - 0.5f), 0.0f, 1.0f);
    b->flank = (float)((int)(rng_f(&r) * 3.0f) - 1);
    b->obj_zeal = 0.75f + 0.5f * rng_f(&r);
    b->obj_guard_r = 25.0f + 30.0f * rng_f(&r);
    b->rays = MA2B_RAY_BURST;
    b->rng = mix32(r ^ 0xA511E9B3u);
    if (diag_on()) fprintf(stderr, "[BOT-PERSONA] bot=%d skill=%d aggr=%.2f caution=%+.2f marksman=%.2f flank=%+.0f reaction=%.2f zeal=%.2f\n",
                           bot_index, skill, b->aggr, b->caution, b->marksman, b->flank, b->sk.reaction, b->obj_zeal);
    return b;
}

void ma2_brain_free(Ma2Brain *b) { free(b); }

/* ======================================================================
 * [C7] High ground (bots batch 5 step 1: counters only, no behaviour change). One piece each:
 *   hg_perch  roof_s_/hill_s_/lives_<ba|mech|tank>_<skill>: seconds stood on a roof (b->raised) or a
 *             hill (on the ground, >= 4 m above the ground 20 m around), and bodies (lives) of that kind;
 *             high_trips_*: tac_high sent a bot up; reperch_*: chose to go back up after its roof fell.
 *   hg_peek   starts_/outs_<ground|roof|hill>_<ba|mech>: peeks begun, pop-outs to the shooting spot;
 *             outs_up_<skill>: roof + hill pop-outs by skill.
 *   hg_mortar shots_/mortar_<ground|roof|hill>_<ba|mech> (ctrl_fire), shots_up_/mortar_up_<skill>.
 *   hg_death  died_roof_/died_hill_/died_all_<ba|mech|tank>_<skill>: bodies lost (killed) up high (now or
 *             within 1 s) and in all; killer_<kind>, killer_below/level/above (killer y vs our perch,
 *             +-3 m), killer_human. Killer = a scoreboard kill (stat 2) that went up within 1 s, else
 *             the nearest enemy aiming at us when we died. RECOMP_BOT_DIAG: [HG-DEATH].
 * ====================================================================== */
#define HG_SK(p) p "_recruit", p "_regular", p "_veteran", p "_elite"
#define HG_K3(p) HG_SK(p "_ba"), HG_SK(p "_mech"), HG_SK(p "_tank")
static const char *const k_hg_roof_s[12] = { HG_K3("roof_s") }, *const k_hg_hill_s[12] = { HG_K3("hill_s") }, *const k_hg_lives[12] = { HG_K3("lives") };
static const char *const k_hg_died[3][12] = { { HG_K3("died_all") }, { HG_K3("died_roof") }, { HG_K3("died_hill") } };
static const char *const k_hg_starts[3][2] = { { "starts_ground_ba", "starts_ground_mech" }, { "starts_roof_ba", "starts_roof_mech" }, { "starts_hill_ba", "starts_hill_mech" } };
static const char *const k_hg_outs[3][2] = { { "outs_ground_ba", "outs_ground_mech" }, { "outs_roof_ba", "outs_roof_mech" }, { "outs_hill_ba", "outs_hill_mech" } };
static const char *const k_hg_outs_up[4] = { HG_SK("outs_up") };
static const char *const k_hg_killer[9] = { "killer_unknown", "killer_pilot", "killer_mech", "killer_ba", "killer_tank", "killer_vtol", "killer_flyer", "killer_mover", "killer_turret" };
static int g_hg_place[MA2_WORLD_MAX_PLAYERS];                 /* per player slot, for ctrl_fire */
static struct { double t; int who; } g_hg_kill[16]; static int g_hg_nk;
static int hg_k3(int k) { return k == MA2_KIND_BATTLEARMOR ? 0 : k == MA2_KIND_MECH ? 1 : k == MA2_KIND_TANK ? 2 : -1; }
int ma2_brain_hg_place(int self) { return self >= 0 && self < MA2_WORLD_MAX_PLAYERS ? g_hg_place[self] : 0; }
/* Once a frame: scoreboard kills that went up (who), for the killer of a perched death. */
static void hg_kills(const Ma2World *w)
{
    static double last = -1.0; static int kills[MA2_WORLD_MAX_PLAYERS]; static uint32_t pl[MA2_WORLD_MAX_PLAYERS];
    if (w->time == last) return;
    if (w->time < last) { memset(kills, 0, sizeof kills); memset(pl, 0, sizeof pl); g_hg_nk = 0; memset(g_hg_place, 0, sizeof g_hg_place); }
    last = w->time;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (pl[i] == p->player && p->kills > kills[i]) { g_hg_kill[g_hg_nk & 15].t = w->time; g_hg_kill[g_hg_nk & 15].who = i; g_hg_nk++; }
        pl[i] = p->player; kills[i] = p->kills;
    }
}
static void hg_tick(Ma2Brain *b, const Ma2World *w, int self, const Ma2PlayerView *me, const Ma2Senses *sn)
{
    int sk = b->skill & 3, k = me->alive && me->actor ? kind_ok(me->kind) : MA2_KIND_NONE, k3 = hg_k3(k);
    hg_kills(w);
    if (b->hg_dead_t > 0.0 && b->now - b->hg_dead_t >= 1.0) {   /* name the killer of a perched death */
        int kk = b->hg_fb_kind, hum = b->hg_fb_human, i;
        float ky = b->hg_fb_y;
        for (i = 0; i < 16 && i < g_hg_nk; i++) {
            int j = g_hg_kill[i].who;
            const Ma2PlayerView *q = j < w->nplayers ? &w->players[j] : NULL;
            if (!q || j == self || fabs(g_hg_kill[i].t - b->hg_dead_t) > 1.0 || (w->team_mode && q->nation == me->nation)) continue;
            kk = q->kind; hum = q->is_human; ky = q->pos.y; break;
        }
        kk = kk >= 0 && kk < 9 ? kk : 0;
        c7_add("hg_death", k_hg_killer[kk], 1);
        if (kk) {
            c7_add("hg_death", ky < b->hg_dead_y - 3.0f ? "killer_below" : ky > b->hg_dead_y + 3.0f ? "killer_above" : "killer_level", 1);
            if (hum) c7_add("hg_death", "killer_human", 1);
        }
        if (diag_on()) fprintf(stderr, "[HG-DEATH] t=%.1f bot=%d sk=%d kind=%d %s y=%.0f killer_kind=%d human=%d dy=%.0f\n", b->hg_dead_t, b->index, sk,
                               b->hg_dead_kind, b->hg_dead_place == 1 ? "roof" : "hill", b->hg_dead_y, kk, hum, kk ? ky - b->hg_dead_y : 0.0f);
        b->hg_dead_t = 0.0;
    }
    if (b->hg_actor && (!me->alive || me->actor != b->hg_actor)) {   /* the body we tracked is gone */
        int ok3 = hg_k3(b->hg_kind);
        if (ok3 >= 0 && (!me->alive || (b->hg_hpf < 0.3f && b->now - b->last_hit < 1.5))) {   /* killed, not climbed out */
            int up = b->hg_place ? b->hg_place : (b->now - b->hg_up_t < 1.0 ? (b->raised ? 1 : 2) : 0);
            c7_add("hg_death", k_hg_died[0][ok3 * 4 + sk], 1);
            if (b->hgd_t > 0.0 && b->now - b->hgd_t < 5.0) c7_add("hg_drop", "died_5s", 1);
            if (up) {
                float best = 1e9f;
                c7_add("hg_death", k_hg_died[up][ok3 * 4 + sk], 1);
                b->hg_dead_t = b->now; b->hg_dead_place = up; b->hg_dead_kind = b->hg_kind; b->hg_dead_y = b->hg_up_y;
                b->hg_fb_kind = 0; b->hg_fb_human = 0; b->hg_fb_y = 0.0f;
                for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {   /* fallback: the nearest enemy aiming at us */
                    const Ma2PlayerView *q = &w->players[i];
                    float d;
                    if (i == self || !q->alive || q->attacking != b->hg_actor || (w->team_mode && q->nation == me->nation)) continue;
                    d = hdist(q->pos, b->perch);
                    if (d < best) { best = d; b->hg_fb_kind = q->kind; b->hg_fb_human = q->is_human; b->hg_fb_y = q->pos.y; }
                }
            }
        }
        b->hg_actor = 0; b->hg_place = 0;
    }
    if (k3 < 0) { g_hg_place[self] = 0; return; }
    if (me->actor != b->hg_actor) {
        b->hg_actor = me->actor; b->hg_kind = k; b->hg_place = 0; b->hg_hill = 0; b->hg_hill_t = 0.0; b->hg_up_t = -1e9; b->hg_pp = -1;
        c7_add("hg_perch", k_hg_lives[k3 * 4 + sk], 1);
    }
    b->hg_hpf = hp_frac(me->hp, me->hp_max);
    if (b->now >= b->hg_hill_t && sn && sn->ground) {          /* hill: on the ground, 4 m over the ground 20 m around */
        int ok = 0, n = 0, i;
        float g0 = sn->ground(sn->ctx, me->pos.x, me->pos.z, &ok), sum = 0.0f;
        b->hg_hill_t = b->now + 0.5;
        b->hg_hill = 0;
        if (ok && me->pos.y < g0 + 1.5f) {
            for (i = 0; i < 8; i++) {
                int okr = 0;
                Ma2Vec3 p = vadd(me->pos, vscale(dir_of_heading((float)i * (MA2B_PI / 4.0f)), 20.0f));
                float g = sn->ground(sn->ctx, p.x, p.z, &okr);
                if (okr) { sum += g; n++; }
            }
            b->hg_hill = n >= 6 && g0 - sum / (float)n >= 4.0f;
        }
    }
    {   /* roofs the surface probe misses (it hits no building mesh there): >= 3 m over the terrain and steady (not
         * airborne: height within 0.4 m for 1 s; 4-40 m: not steps, not a BattleArmor riding a VTOL) counts as a roof perch. Counter only; b->raised is unchanged. */
        int okg = 0, was = b->hg_probe_miss;
        float gg = sn && sn->ground ? sn->ground(sn->ctx, me->pos.x, me->pos.z, &okg) : 0.0f;
        if (fabsf(me->pos.y - b->hg_sy) > 0.4f) { b->hg_sy = me->pos.y; b->hg_st = b->now; }
        b->hg_probe_miss = !b->raised && !b->hg_hill && okg && me->pos.y > gg + 4.0f && me->pos.y < gg + 40.0f && b->now - b->hg_st >= 1.0;
        if (b->hg_probe_miss) c7_add("hg_perch", "probe_miss_s", b->dt);
        if (b->hg_probe_miss && !was && diag_on()) {
            int oks = 0; float ss = sn->surface ? sn->surface(sn->ctx, me->pos.x, me->pos.z, &oks) : 0.0f;
            fprintf(stderr, "[HG-UP] t=%.1f bot=%d kind=%d y=%.1f ground=%.1f surface=%.1f ok=%d (roof the probe missed)\n", b->now, b->index, k, me->pos.y, gg, ss, oks);
        }
    }
    b->hg_place = b->raised ? 1 : b->hg_hill ? 2 : b->hg_probe_miss ? 1 : 0;
    g_hg_place[self] = b->hg_place;
    if (b->hgc_top_t > 0.0) {                                /* (a) over the top a moment ago: did it end up standing on the roof? */
        if (b->hg_place == 1) { c7_add("hg_climb", "roof_stood", 1); fprintf(stderr, "[HG-CLIMB] t=%.1f bot=%d sk=%d STOOD on the roof %.1f s after the top, y=%.0f\n", b->now, b->index, b->skill, b->now - b->hgc_top_t, me->pos.y); b->hgc_top_t = 0.0; }
        else if (b->now - b->hgc_top_t > 5.0) { c7_add("hg_climb", "top_missed", 1); b->hgc_top_t = 0.0; }
    }
    if (b->hg_place) {
        b->hg_up_t = b->now; b->hg_up_y = me->pos.y;
        c7_add("hg_perch", b->hg_place == 1 ? k_hg_roof_s[k3 * 4 + sk] : k_hg_hill_s[k3 * 4 + sk], b->dt);
    }
    if (k3 < 2) {                                            /* peeks: begun, and each step out to the shooting spot */
        int on = b->peek_tgt && b->now < b->peek_end;
        if (on && b->peek_end != b->hg_pend) { b->hg_pend = b->peek_end; c7_add("hg_peek", k_hg_starts[b->hg_place][k3], 1); b->hg_pp = -1; }
        if (on && b->peek_phase == 0 && b->hg_pp != 0) {
            c7_add("hg_peek", k_hg_outs[b->hg_place][k3], 1);
            if (b->hg_place) c7_add("hg_peek", k_hg_outs_up[sk], 1);
        }
        b->hg_pp = on ? b->peek_phase : -1;
    }
}

#include "ma2_bot_surprise_b.c"   /* surprise set B: bait, pincer, Elite power-up timing + denial (after every helper it uses) */

/* VtolThreatAlt (playtest 10-30, default on: [Bots] VtolThreatAlt / RECOMP_BOT_VTOL_THREAT_ALT; 0 = the old per-leg dice roll):
 * a VTOL drops to nap-of-the-earth when it is hit, a missile/shell is coming (noticed), or an enemy that can hit it is in
 * range and in sight; it flies at the cruise height (hi + 20 m, over hills as before) when the route is clear. Hysteresis:
 * down after a reaction delay, up only after a hold with no threat. Skill changes decisions and timing only:
 *   reaction delay  Recruit 2.2 s / Regular 1.0 / Veteran 0.45 / Elite 0.2 (x0.7..1.4),
 *   ignores the episode  35 % / 15 % / 3 % / 0 %,   stays low after the threat  2.5 s / 4 / 6 / 8 (x0.8..1.3).
 * [C7] vtthreat per skill: s_vtol, s_threat, s_clear, agl_threat, agl_clear (metre-seconds over the ground), low_s, react_n,
 * react_s (first threat -> low), ignored, dmg (hp fractions taken), deaths. */
#define VT_SK(p) p "_recruit", p "_regular", p "_veteran", p "_elite"
static const char *const k_vt_s_vtol[4] = { VT_SK("s_vtol") }, *const k_vt_s_thr[4] = { VT_SK("s_threat") }, *const k_vt_s_clr[4] = { VT_SK("s_clear") };
static const char *const k_vt_agl_thr[4] = { VT_SK("agl_threat") }, *const k_vt_agl_clr[4] = { VT_SK("agl_clear") }, *const k_vt_low_s[4] = { VT_SK("low_s") };
static const char *const k_vt_react_n[4] = { VT_SK("react_n") }, *const k_vt_react_s[4] = { VT_SK("react_s") }, *const k_vt_ignored[4] = { VT_SK("ignored") };
static const char *const k_vt_dmg[4] = { VT_SK("dmg") }, *const k_vt_deaths[4] = { VT_SK("deaths") };
static const char *const k_vt_s_rider[4] = { VT_SK("s_rider") }, *const k_vt_s_lowm[4] = { VT_SK("s_lowmode") }, *const k_vt_s_highm[4] = { VT_SK("s_highmode") };
static const char *const k_vt_agl_lowm[4] = { VT_SK("agl_lowmode") }, *const k_vt_agl_highm[4] = { VT_SK("agl_highmode") };
static int vt_threat_on(void) { return c7_on("vtthreat", "RECOMP_BOT_VTOL_THREAT_ALT", "VtolThreatAlt", 1); }
static int vt_threat_alt(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, const Ma2Order *out, int on)
{
    static const float dly[4] = { 2.2f, 1.0f, 0.45f, 0.2f }, ign[4] = { 0.35f, 0.15f, 0.03f, 0.0f }, hold[4] = { 2.5f, 4.0f, 6.0f, 8.0f };
    int sk = b->skill & 3, hit, inc, near = 0;
    double now = b->now, dt = b->dt;
    (void)w;
    hit = b->last_hit > 0.0 && now - b->last_hit < 1.5;
    inc = out->dodge_now;
    for (int i = 0; i < MA2_WORLD_MAX_PLAYERS && !near; i++) {
        const Mem *m = &b->mem[i];
        if (!m->valid || !m->visible || m->nation == me->nation || mem_conf(b, m) < 0.3f || k_kind[m->kind].dps < 15.0f) continue;
        float reach = m->reach > 10.0f ? m->reach : k_kind[m->kind].weapon_range;
        if (hdist(m->pos, me->pos) < reach) near = 1;
    }
    int threat = hit || inc || near;
    float g = ground_at(sn, me->pos.x, me->pos.z, -1e9f), agl = g > -1e8f ? me->pos.y - g : -1.0f;
    if (threat) b->vt_thr_last = now;
    if (!on) b->vt_thr_low = 0;
    else if (!b->vt_thr_low) {
        if (threat) {
            if (b->vt_thr_t0 <= 0.0 || now < b->vt_thr_t0) {     /* a new episode: react, or (the green ones) not at all */
                b->vt_thr_t0 = now; b->vt_thr_ign = rng_f(&b->rng) < ign[sk];
                b->vt_thr_delay = dly[sk] * rng_range(&b->rng, 0.7f, 1.4f);
                if (b->vt_thr_ign) c7_add("vtthreat", k_vt_ignored[sk], 1);
            }
            if (!b->vt_thr_ign && now - b->vt_thr_t0 >= (double)b->vt_thr_delay) {
                b->vt_thr_low = 1; b->vt_thr_hold = hold[sk] * rng_range(&b->rng, 0.8f, 1.3f);
                c7_add("vtthreat", k_vt_react_n[sk], 1); c7_add("vtthreat", k_vt_react_s[sk], now - b->vt_thr_t0);
            }
        } else if (b->vt_thr_t0 > 0.0 && now - b->vt_thr_last > 1.5) b->vt_thr_t0 = 0.0;   /* it was nothing */
    } else if (!threat && now - b->vt_thr_last > b->vt_thr_hold) { b->vt_thr_low = 0; b->vt_thr_t0 = 0.0; }
    c7_add("vtthreat", k_vt_s_vtol[sk], dt);
    {   /* a rider hangs under the skids: those seconds fly 12 m higher by design, so they are counted apart */
        int rider = 0;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == me->actor) { rider = 1; break; }
        if (rider) { c7_add("vtthreat", k_vt_s_rider[sk], dt); return b->vt_thr_low; }
    }
    if (threat) c7_add("vtthreat", k_vt_s_thr[sk], dt), c7_add("vtthreat", k_vt_agl_thr[sk], agl >= 0.0f ? (double)agl * dt : 0.0);
    else c7_add("vtthreat", k_vt_s_clr[sk], dt), c7_add("vtthreat", k_vt_agl_clr[sk], agl >= 0.0f ? (double)agl * dt : 0.0);
    if (on ? b->vt_thr_low : out->vt_low) c7_add("vtthreat", k_vt_s_lowm[sk], dt), c7_add("vtthreat", k_vt_agl_lowm[sk], agl >= 0.0f ? (double)agl * dt : 0.0);
    else c7_add("vtthreat", k_vt_s_highm[sk], dt), c7_add("vtthreat", k_vt_agl_highm[sk], agl >= 0.0f ? (double)agl * dt : 0.0);
    ((Ma2Order *)out)->vt_sk = sk; ((Ma2Order *)out)->vt_thr = threat;
    return b->vt_thr_low;
}

/* BlastWary (default on, RECOMP_BOT_BLAST_WARY / [Bots] BlastWary; playtest 11-01: fleeing at hp 0 may be too late
 * to get clear of the blast, so a bot
 * anticipates it): a 'Mech blows up 2.2 s after its health reaches 0 and kills out to ~45-50 m (269 runs, 11-01); fleeing at
 * hp 0 got only 60 % of the 'Mechs inside it out in time (bR2), 2 of 6 from inside 30 m. So a bot in a 'Mech or a tank that has a
 * 'Mech within 55 m nearly dead -- either side, the player's too: health at or under Recruit 5 / Regular 8 / Veteran 12 /
 * Elite 15 % while it is being hit, or gone within Recruit 1.0 / 1.5 / 2.0 / Elite 2.5 s at the damage it is taking -- stops
 * closing on it. Noticed after the bot's reaction time, with a chance by skill (35 / 60 / 80 / 95 %): a fight with that
 * 'Mech keeps 40 m at least (inside it the band steps out while it shoots); inside 30 m of any other dying 'Mech it moves out
 * to ~48 m while it shoots; an order that would take it closer than 40 m slides round at its distance instead. Turn and
 * walk only (never reverse). Capped at 8 s per 'Mech, then that one is ignored for 20 s: a low-health player is not shunned
 * for good. A BattleArmor does not claw onto it (its hop gets it clear of the blast; a claw would not). The objective races
 * (carrying, grabbing or returning a flag, chasing It, bombing) are left alone. [C7] blastwary; [BOT-WARY] lines. */
static int blast_wary_on(void) { return c7_on("blastwary", "RECOMP_BOT_BLAST_WARY", "BlastWary", 1); }
/* Every 'Mech's health over the last ~2 s (8 samples 0.25 s apart), shared by all bots: how fast it is going down. */
static struct { uint32_t actor; float f[8]; double t[8]; int n, i; double hit_t; } g_mhp[MA2_WORLD_MAX_PLAYERS];
static double g_mhp_t = -1.0;
static void mhp_update(const Ma2World *w, double now)
{
    if (now < g_mhp_t) memset(g_mhp, 0, sizeof g_mhp);   /* a new match */
    if (g_mhp_t >= 0.0 && now - g_mhp_t < 0.25) return;
    g_mhp_t = now;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
        const Ma2PlayerView *p = &w->players[j];
        if (!p->actor || kind_ok(p->kind) != MA2_KIND_MECH) { g_mhp[j].actor = 0; continue; }
        if (g_mhp[j].actor != p->actor) { memset(&g_mhp[j], 0, sizeof g_mhp[j]); g_mhp[j].actor = p->actor; g_mhp[j].hit_t = -99.0; }
        float f = hp_frac(p->hp, p->hp_max);
        if (g_mhp[j].n && f < g_mhp[j].f[(g_mhp[j].i + 7) & 7] - 0.002f) g_mhp[j].hit_t = now;
        g_mhp[j].f[g_mhp[j].i] = f; g_mhp[j].t[g_mhp[j].i] = now; g_mhp[j].i = (g_mhp[j].i + 1) & 7;
        if (g_mhp[j].n < 8) g_mhp[j].n++;
    }
}
/* Seconds until player slot j's 'Mech is at 0 at the damage rate of the last ~1.75 s (1e9: not going down). */
static float mhp_ttd(int j, double now)
{
    if (j < 0 || j >= MA2_WORLD_MAX_PLAYERS || g_mhp[j].n < 2) return 1e9f;
    int last = (g_mhp[j].i + 7) & 7, old = last;
    for (int q = 1; q < g_mhp[j].n; q++) { int s = (last - q) & 7; if (now - g_mhp[j].t[s] > 1.8) break; old = s; }
    double dt = g_mhp[j].t[last] - g_mhp[j].t[old];
    float drop = g_mhp[j].f[old] - g_mhp[j].f[last];
    if (dt < 0.4 || drop <= 0.0f) return 1e9f;
    return g_mhp[j].f[last] / (float)(drop / dt);
}
static int bw_ignored(const Ma2Brain *b, uint32_t a)
{
    for (int i = 0; i < 4; i++) if (b->bw_ign[i] == a && b->now < b->bw_ign_until[i] && b->now + 60.0 > b->bw_ign_until[i]) return 1;
    return 0;
}
static void bw_end(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const char *why)
{
    if (b->bw_t0 > 0.0) {
        const Ma2PlayerView *h = player_by_actor(w, b->bw_actor);
        float d = hdist(h ? h->pos : b->bw_pos, me->pos);
        int blast = !h || hp_frac(h->hp, h->hp_max) <= 0.005f;   /* down (or gone): its blast is on the way */
        c7_add("blastwary", blast ? "ended_blast" : why, 1);
        if (blast) { c7_add("blastwary", "blast_d_sum", d); c7_add("blastwary", d > 45.0f ? "blast_out45" : "blast_in45", 1); }
        if (diag_on()) fprintf(stderr, "[BOT-WARY] t=%.1f bot=%d sk=%d ends (%s) after %.1f s, %08X %.0f m off (was %.0f)\n", b->now, b->index, b->skill,
                               blast ? "it went down" : why, b->now - b->bw_t0, b->bw_actor, d, b->bw_d0);
    }
    b->bw_actor = 0; b->bw_t0 = 0.0; b->bw_go = 0;
}
static void blast_wary(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *o)
{
    static const float lo[4] = { 0.05f, 0.08f, 0.12f, 0.15f }, ttd_s[4] = { 1.0f, 1.5f, 2.0f, 2.5f }, notice[4] = { 0.35f, 0.60f, 0.80f, 0.95f };
    static const char *const k_noticed[4] = { "noticed_recruit", "noticed_regular", "noticed_veteran", "noticed_elite" };
    static const char *const k_missed[4] = { "missed_recruit", "missed_regular", "missed_veteran", "missed_elite" };
    const float keep_r = 40.0f, out_r = 30.0f, look_r = 55.0f;
    int k = kind_ok(me->kind), sk = b->skill & 3;
    int hack = k == MA2_KIND_BATTLEARMOR && o->type == MA2_ORD_HACK;
    if (!blast_wary_on()) return;
    mhp_update(w, b->now);
    /* And one already down (health 0, its blast due within 2.6 s) is not walked into: just outside the hp-0 escape's reach
     * (45 m + hull) an order that takes us nearer holds where we stand until it has gone off -- a 'Mech going for the flag
     * the dying carrier dropped walked from 78 m to 53 m and died in that blast (wd_on3). Any duty, races too: the flag
     * is not going anywhere for 2 s. */
    if (k == MA2_KIND_MECH || k == MA2_KIND_TANK || k == MA2_KIND_BATTLEARMOR || k == MA2_KIND_PILOT) {
        static const float react[4] = { 0.4f, 0.3f, 0.2f, 0.1f };   /* as the hp-0 escape */
        float hull = k == MA2_KIND_MECH || k == MA2_KIND_TANK ? 4.0f : k == MA2_KIND_PILOT ? 1.0f : 2.5f;
        for (int i = 0; i < w->nblasts && i < 8; i++) {
            if (w->blasts[i].actor == me->actor || b->now - w->blasts[i].t0 < react[sk] || b->now < w->blasts[i].t0) continue;
            float d = hdist(w->blasts[i].pos, me->pos), r = w->blasts[i].r + hull + 2.0f;
            if (d < r || d > r + 25.0f) continue;                       /* inside: the escape's; far: no matter */
            if (k != MA2_KIND_PILOT && o->keep_r <= 0.0f) { o->keep_pt = w->blasts[i].pos; o->keep_r = r; }   /* BlastBrake (ctrl): no coasting in */
            Ma2Vec3 to = o->point;
            if (o->type == MA2_ORD_ENGAGE || o->type == MA2_ORD_HACK) { const Ma2PlayerView *t = player_by_actor(w, o->target); if (t) to = t->pos; }
            else if (o->type != MA2_ORD_TRAVEL && o->type != MA2_ORD_FIGHT_MOVE) continue;
            float dt = hdist(to, w->blasts[i].pos);
            if (dt >= r + 5.0f || dt >= d - 2.0f) continue;               /* not in toward it */
            int near_dying = 0;   /* a dying 'Mech inside 30 m comes first (the anticipation below moves us out): standing still to wait
                                   * for one blast 22 m from the next one killed a Regular (wd_on7s1) */
            if (k == MA2_KIND_MECH || k == MA2_KIND_TANK)
                for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS && !near_dying; j++) {
                    const Ma2PlayerView *q = &w->players[j];
                    float f = hp_frac(q->hp, q->hp_max);
                    if (q->alive && q->actor != me->actor && kind_ok(q->kind) == MA2_KIND_MECH && f > 0.005f && hdist(q->pos, me->pos) < 30.0f &&
                        (f <= lo[sk] * 1.5f || mhp_ttd(j, b->now) <= ttd_s[sk])) near_dying = 1;
                }
            if (near_dying) { c7_add("blastwary", "entry_hold_skipped", 1); break; }
            uint32_t tg = o->target ? o->target : b->tgt ? b->tgt->actor : 0;
            o->type = MA2_ORD_FIGHT_MOVE; o->point = me->pos; o->stop = 2.0f; o->exact = 0; o->climb = 0; o->exact_r = 0.0f;
            o->target = tg; o->fire = tg != 0 && !o->hold_fire;
            c7_add("blastwary", "entry_hold_s", b->dt); snprintf(b->note, sizeof b->note, "wary-blast-wait");
            if (b->now - b->bw_entry_log > 3.0 || b->now < b->bw_entry_log) {
                b->bw_entry_log = b->now; c7_add("blastwary", "entry_holds", 1);
                if (diag_on()) fprintf(stderr, "[BOT-WARY] t=%.1f bot=%d sk=%d kind=%d waits %.0f m out for the blast of %08X (goal %s, it was going %.0f m from it)\n",
                                       b->now, b->index, b->skill, k, d, w->blasts[i].actor, k_goal_name[b->goal], dt);
            }
            return;
        }
    }
    /* Off combat duty for a moment (a goal flicker, a hack on us): no action, the episode carries on (its grace below ends it) */
    /* CTF fights are mostly objective duty: hunting their carrier, escorting ours, guarding (wd_on1: the two 'Mechs 21-26 m from
     * a dying carrier were escort and hunt). Only the races are left alone: carrying, grabbing, returning, chasing It, bombing. */
    int duty = b->goal == G_OBJECTIVE && !b->obj_lock && b->obj_op != OP_CARRY && b->obj_op != OP_GRAB && b->obj_op != OP_RETURN &&
               b->obj_op != OP_CHASE_IT && b->obj_op != OP_BOMB;
    if ((k != MA2_KIND_MECH && k != MA2_KIND_TANK && !hack) || me->qte_active || me->enemy_rider ||
        !(b->goal == G_ATTACK || b->goal == G_SUPPORT || b->goal == G_HACK || b->goal == G_ROAM || b->goal == G_NONE || duty)) {
        if (b->bw_actor && (b->now - b->bw_elig_t > 2.0 || b->now < b->bw_elig_t)) bw_end(b, w, me, "ended_duty");   /* off duty 2 s: over */
        return;
    }
    b->bw_elig_t = b->now;
    /* The nearest dying 'Mech; the one already kept clear of stays it for 2 s after it last looked dying (its damage comes
     * in bursts: the first runs started and ended an episode every 0.1 s, 'wcbog1'), and unless another is 10 m nearer. */
    const Ma2PlayerView *h = NULL; float hbest = 1e9f, hd = 0.0f, httd = 1e9f;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
        const Ma2PlayerView *q = &w->players[j];
        if (!q->alive || !q->actor || q->actor == me->actor || kind_ok(q->kind) != MA2_KIND_MECH) continue;
        int cur = q->actor == b->bw_actor;
        float f = hp_frac(q->hp, q->hp_max), d = hdist(q->pos, me->pos);
        if (f <= 0.005f || d >= look_r + (cur ? 5.0f : 0.0f) || bw_ignored(b, q->actor)) continue;   /* at 0 it is the death-blast escape's */
        float ttd = mhp_ttd(j, b->now);
        int hit = g_mhp[j].actor == q->actor && b->now - g_mhp[j].hit_t < 2.0;
        int dying = (f <= lo[sk] && hit) || ttd <= ttd_s[sk];
        if (dying && cur) b->bw_dying_t = b->now;
        if (!dying && !(cur && b->now - b->bw_dying_t < 2.0)) continue;
        float score = d - (cur ? 10.0f : 0.0f);
        if (score < hbest) { hbest = score; hd = d; h = q; httd = ttd; }
    }
    if (!h) { if (b->bw_actor) bw_end(b, w, me, b->bw_t0 > 0.0 ? "ended_safe" : "ended_unseen"); return; }
    if (h->actor != b->bw_actor) {
        if (b->bw_actor) bw_end(b, w, me, "ended_switch");
        b->bw_actor = h->actor; b->bw_seen = b->now; b->bw_t0 = 0.0; b->bw_dying_t = b->now;
        if (h->actor == b->bw_roll_actor && b->now >= b->bw_roll_t && b->now - b->bw_roll_t < 15.0) b->bw_go = b->bw_roll_go;   /* noticed once per 'Mech */
        else {
            b->bw_go = getenv("RECOMP_TEST_BLAST_WARY_ALL") || rng_f(&b->rng) < notice[sk];   /* test hook: every bot notices */
            b->bw_roll_actor = h->actor; b->bw_roll_t = b->now; b->bw_roll_go = b->bw_go;
            c7_add("blastwary", b->bw_go ? k_noticed[sk] : k_missed[sk], 1);
        }
        b->bw_side = rng_f(&b->rng) < 0.5f ? -1 : 1;
    }
    if (!b->bw_go || b->now - b->bw_seen < (double)b->sk.reaction) return;
    if (b->bw_t0 <= 0.0) {
        b->bw_t0 = b->now; b->bw_d0 = hd;
        c7_add("blastwary", "episodes", 1);
        if (diag_on()) fprintf(stderr, "[BOT-WARY] t=%.1f bot=%d sk=%d kind=%d keeps clear of %s 'Mech %08X%s: %.0f%% health, %s, %.0f m off (goal %s, order %d)\n",
                               b->now, b->index, b->skill, k, h->nation == me->nation ? "a friendly" : "an enemy", h->actor, h->is_human ? " (player)" : "",
                               100.0f * hp_frac(h->hp, h->hp_max), httd < 1e8f ? "dying fast" : "under fire", hd, k_goal_name[b->goal], o->type);
    }
    b->bw_pos = h->pos;
    /* BlastBrake: the ctrl stops momentum (a jet hop, a run) carrying us inside its blast -- r3_213 b1 flew 51 -> 17 m on a hop with forward demand 0 */
    if (o->keep_r <= 0.0f) { o->keep_pt = h->pos; o->keep_r = keep_r + 8.0f; }
    if (hack) {   /* no claw onto it: shoot it from where we stand (not capped: shooting it is no retreat) */
        if (o->target == h->actor) {
            o->type = MA2_ORD_ENGAGE; o->point = h->pos; o->standoff = 0; o->band_min = b->band_min; o->band_max = b->band_max; o->stop = b->band_max * 0.9f;
            c7_add("blastwary", "hack_skip_s", b->dt); snprintf(b->note, sizeof b->note, "wary-nohack");
        }
        return;
    }
    if (b->now - b->bw_t0 > 8.0) {   /* capped: carry on as before against this one for 20 s */
        int slot = 0;
        for (int i = 0; i < 4; i++) if (b->bw_ign_until[i] < b->bw_ign_until[slot]) slot = i;
        b->bw_ign[slot] = h->actor; b->bw_ign_until[slot] = b->now + 20.0;
        bw_end(b, w, me, "ended_capped");
        return;
    }
    float d = hd;
    Ma2Vec3 away = vsub(me->pos, h->pos); away.y = 0.0f;
    { float l = hlen(away); if (l < 0.5f) away = dir_of_heading(me->yaw + MA2B_PI); else away = vscale(away, 1.0f / l); }
    Ma2Vec3 side = v3(away.z * (float)b->bw_side, 0.0f, -away.x * (float)b->bw_side);
    uint32_t tg = o->target ? o->target : b->tgt ? b->tgt->actor : 0;
    if (o->type == MA2_ORD_ENGAGE && o->target == h->actor) {   /* fighting it: the band starts at 40 m (ctrl_fight steps out inside it) */
        o->band_min = maxf(o->band_min, keep_r); o->band_max = maxf(o->band_max, o->band_min + 30.0f); o->stop = maxf(o->stop, 0.9f * o->band_max);
        c7_add("blastwary", "band_s", b->dt); snprintf(b->note, sizeof b->note, "wary-band");
        return;
    }
    int approach = 0;
    if (o->type == MA2_ORD_TRAVEL || o->type == MA2_ORD_FIGHT_MOVE) approach = hdist(o->point, h->pos) < keep_r && hdist(o->point, h->pos) < d;
    else if (o->type == MA2_ORD_ENGAGE) { const Ma2PlayerView *t = player_by_actor(w, o->target); approach = !t || hdist(t->pos, h->pos) < keep_r + 5.0f; }
    if (d >= out_r && !approach) return;
    Ma2Vec3 p;
    if (d < out_r) { p = vadd(h->pos, vadd(vscale(away, 48.0f), vscale(side, 10.0f))); c7_add("blastwary", "out_s", b->dt); snprintf(b->note, sizeof b->note, "wary-out"); }
    else {   /* slide round at this distance (40 m at least) rather than in */
        Ma2Vec3 s = vadd(me->pos, vscale(side, 12.0f)), u = vsub(s, h->pos); u.y = 0.0f;
        float l = hlen(u); if (l < 0.5f) l = 0.5f;
        p = vadd(h->pos, vscale(u, maxf(d, keep_r + 2.0f) / l));
        c7_add("blastwary", "slide_s", b->dt); snprintf(b->note, sizeof b->note, "wary-slide");
    }
    p.y = me->pos.y;
    o->type = MA2_ORD_FIGHT_MOVE; o->point = p; o->stop = 3.0f; o->exact = 0; o->standoff = 0; o->climb = 0;
    o->target = tg; o->fire = tg != 0;
}
static void brain_tick_body(Ma2Brain *b, const Ma2World *w, int self, Ma2Nav *nav,
                            const Ma2CtrlStatus *st, const Ma2Senses *sn, Ma2Order *out)
{
    const Ma2PlayerView *me;
    int k, changed;
    float hpf_prev;
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!b || !w || self < 0 || self >= w->nplayers || self >= MA2_WORLD_MAX_PLAYERS) return;
    me = &w->players[self];
    b->bomb_out = st ? st->bomb_out : 0; if (st) b->bomb_at = st->bomb_at;
    b->now = w->time;
    b->dt = (w->dt > 0.0f && w->dt < 0.25f) ? w->dt : (1.0f / 60.0f);
    hg_tick(b, w, self, me, sn);
    sp_brain_tick(b, w, self, me, sn);
    if (me->alive && me->kind == MA2_KIND_VTOL) b->vt_air_t = b->now;
    if (me->deaths > b->vt_deaths_seen) { if (b->vt_air_t > 0.0 && b->now - b->vt_air_t < 3.0) c7_add("vtthreat", k_vt_deaths[b->skill & 3], 1); b->vt_deaths_seen = me->deaths; }

    if (!me->alive || me->kind == MA2_KIND_NONE || !me->actor) {
        b->prev_valid = 0; b->goal = G_NONE; b->qte.pending = 0; b->obj_lock = 0;
        sp2_dead(b);
        ctf_zone_died(b);
        return;
    }
    k = kind_ok(me->kind);
    changed = !b->prev_valid || me->actor != b->prev_actor || k != b->prev_kind;
    if (changed) { on_actor_change(b, me); b->act_since = b->now; }
    if (!b->home_set) { b->home_pos = me->pos; b->home_set = 1; ma2_bot_pad_home(me->nation, me->pos); for (int i = 0; i < 8; i++) b->pad_bad_until[i] = 0.0; b->repair_pad = -1; b->repair_block_until = 0.0; }
    ma2_bot_pad_c7(b->now);

    hpf_prev = hp_frac(b->hp_prev, me->hp_max);
    if (me->hp < b->hp_prev - 0.01f) {
        b->dmg_recent += (b->hp_prev - me->hp) / maxf(me->hp_max, 1.0f);
        if (k == MA2_KIND_VTOL) c7_add("vtthreat", k_vt_dmg[b->skill & 3], (b->hp_prev - me->hp) / maxf(me->hp_max, 1.0f));
        b->last_hit = b->now;
        /* A heavy hit or dropping under the retreat line forces a re-think now. */
        if (b->dmg_recent > 0.15f || (hpf_prev >= b->sk.retreat_hp && hp_frac(me->hp, me->hp_max) < b->sk.retreat_hp)) { b->interrupt = 1; b->next_eval = 0.0; }
    }
    b->hp_prev = me->hp;
    b->dmg_recent *= expf(-b->dt / 1.5f);
    if (me->clamped_to) {                         /* our NeuroHack: that 'Mech is ours to take once it is free */
        b->hacked_actor = me->clamped_to; b->hacked_t = b->now;
        b->claim_actor = me->clamped_to; b->post_hack_until = b->now + 5.0;
    }

    /* Controller feedback: a vehicle whose zone gave no prompt, or that cannot be reached,
     * is dropped at once and the next best one chosen -- no retries at the hull. */
    no_board_refresh(b, w);
    if (st && st->enter_failed) {
        no_board_add(b, st->enter_failed);
        snprintf(b->note, sizeof b->note, "no-prompt");
        { int fvi = find_vehicle(w, st->enter_failed);
          if (fvi >= 0 && w->vehicles[fvi].kind == MA2_KIND_BATTLEARMOR) { g_ba_fail++; b->ba_ban_until = b->now + 150.0; c7_add("baenterban", "ba_fails", 1); } }
        if (st->enter_failed == b->veh) { b->veh = 0; b->veh_fail = 0; }
    }
    if (st && (st->no_path || st->stalled) && (b->goal == G_ROAM || b->goal == G_ATTACK || b->goal == G_SUPPORT || b->goal == G_POWERUP)) {
        b->roam_valid = 0; b->tac_until = 0.0;
        if (b->via_state == 1) { lane_trip_end(b, 4); b->via_state = 2; }   /* the lane is not getting us there */
        if (st->no_path) snprintf(b->note, sizeof b->note, "no-path");
    }
    {   /* ft-bots ObjGiveUp: an objective target the route cannot reach (a flag on a roof, a base behind a gap): mark it unreachable for
         * this bot for a while and pick another task now, instead of waiting for the trapped-bot rescue to eject it. */
        static int on = -1;
        if (on < 0) on = c7_on("objgiveup", "RECOMP_BOT_OBJ_GIVEUP", "ObjGiveUp", 1);
        /* ObjRouteFails (default on, RECOMP_BOT_OBJ_ROUTE / [Bots] ObjRouteFails): status.no_path is cleared every step and set only on the
         * step a plan finishes, so a body that keeps moving (a tank circling the cliff foot under a Snatch pile 25 m up) never held it for
         * 3 s: r4_85 b4 59 s / b8 39 s with 3-42 short plans in a row and no give-up. The count of short plans in a row counts too. */
        static int rf_on = -1;
        if (rf_on < 0) rf_on = c7_on("objroute", "RECOMP_BOT_OBJ_ROUTE", "ObjRouteFails", 1);
        int np = st && (st->no_path || (rf_on && k == MA2_KIND_TANK && st->route_fails >= 3));   /* tanks only: a 'Mech / BattleArmor jets the last ledge (r4_85 b3/b9 'Mechs 400 m out) */
        if (on && np && b->goal == G_OBJECTIVE && b->obj_op != OP_NONE && b->obj_op != OP_CARRY) {
            if (b->obj_np_t0 <= 0.0) b->obj_np_t0 = b->now;
            else if (b->obj_op == OP_GUARD && b->now - b->obj_np_t0 >= 0.6 && c7_on("guardring", "RECOMP_BOT_GUARD_RING", "GuardRing", 1)) {
                /* GuardRing: a guard's unreachable patrol point is that point, not the post -- ObjGiveUp banned the whole plant 25 s and sent
                 * defenders 1000-1500 m off and back (d5_91: 11 give-ups). Roll another point after 0.6 s (not 3 s: around a 100 x 40 m plant even
                 * reach-set points fall inside the walls -- g1_91 30 stands of 3 s at route ends 25-47 m short). */
                b->obj_np_t0 = 0.0; b->obj_wander_until = 0.0; c7_add("guardring", "rerolls", 1);
            }
            /* FootFallback (default on, RECOMP_BOT_FOOT_FALLBACK / [Bots] FootFallback): a Check It terminal is worked on foot, but the get-out
             * waited for 40 m and the vehicle's route ended under the platform: Killing Fields terminal 2 (558,52,1262) stands 12 m over the
             * ground on its west side, and 'Mechs circled 466-498,1196-1255 at y 40 until ObjGiveUp banned it (rc4_72a b10 100-110 s, b0, rc4_72b
             * b3 / b8, bs72a b7 / b8, bs72b b0 / b2: 51-145 s each, both teams, both builds). A pilot walks up from there in 52 m (PATHTEST).
             * No route to an on-foot objective within 150 m: get out and walk (no ban). [C7] footfallback: n. */
            else if (b->now - b->obj_np_t0 >= 2.0 && b->obj_need_foot && k != MA2_KIND_PILOT && hdist(me->pos, b->obj_pt) < 150.0f && b->now >= b->obj_walk_until &&
                     (b->obj_walk_ent != b->obj_ent || b->obj_walk_n < 2) &&   /* a hull pinned where the game refuses the exit (p17_72b b3): after two tries, the normal give-up */
                     foot_walk_ok(b, me) &&   /* FootPrecheck */
                     c7_on("footfallback", "RECOMP_BOT_FOOT_FALLBACK", "FootFallback", 1)) {
                if (b->obj_walk_ent != b->obj_ent) { b->obj_walk_ent = b->obj_ent; b->obj_walk_n = 0; }
                b->obj_walk_n++;
                b->obj_walk_until = b->now + 40.0; b->obj_np_t0 = 0.0; c7_add("footfallback", "n", 1);
                if (diag_on()) fprintf(stderr, "[BOT-OBJ] t=%.1f bot=%d no route to on-foot %s %.0f,%.0f from %.0f,%.0f (%.0f m): getting out to walk\n", b->now, b->index, k_op_name[b->obj_op], b->obj_pt.x, b->obj_pt.z, me->pos.x, me->pos.z, hdist(me->pos, b->obj_pt));
            }
            else if (b->now - b->obj_np_t0 >= 3.0 && !(b->now < b->obj_walk_until && b->obj_walk_ent == b->obj_ent)) {   /* not while getting out to walk (p18_72a b3: banned 3 s after the fallback, 124.7 s) */
                int slot = b->obj_nbad_next++ & 3;
                b->obj_bad[slot].ent = b->obj_ent; b->obj_bad[slot].pt = b->obj_pt; b->obj_bad[slot].op = b->obj_op; b->obj_bad[slot].until = b->now + (st->no_path ? 25.0 : 60.0);   /* ObjRouteFails: a route that keeps ending short (a plateau) stays so: 60 s */
                b->obj_np_t0 = 0.0; b->next_eval = 0.0; b->interrupt = 1; b->roam_valid = 0; b->tac_until = 0.0;
                c7_add("objgiveup", "giveups", 1); if (!st->no_path) c7_add("objroute", "giveups", 1);
                snprintf(b->note, sizeof b->note, "obj-unreachable");
                if (diag_on()) fprintf(stderr, "[BOT-OBJ] t=%.1f bot=%d op=%s target %.0f,%.0f unreachable: skipped for %.0f s\n", b->now, b->index, k_op_name[b->obj_op], b->obj_pt.x, b->obj_pt.z, b->obj_bad[slot].until - b->now);
            }
        } else b->obj_np_t0 = 0.0;
    }
    /* WalkProgress (default on, RECOMP_BOT_WALK_PROGRESS / [Bots] WalkProgress): the FootFallback walk held off ObjGiveUp for its whole 40 s, and the
     * pilot's route can go up a face a pilot cannot walk: Check It No Man's Land terminal 634,74,1227 (on foot, from the west: 30 deg at 505,1240,
     * then 43 deg), rc10_73a b1 stood at 505-509,1237-1242 from 48 to 70 s with a route and no progress, b2 109-159 s, rc6_73b b1 the same spot.
     * A walking pilot that has not moved 5 m in 12 s (from 4 s after the get-out: the exit itself holds it ~2.5 s) ends the walk and skips the
     * terminal for 25 s (the normal give-up). Moved, not closed: the walk to 1402,774 starts 135-141 m out on a route that goes round first,
     * and "5 m closer in 10 s" ended good walks (bal37). The 12 s leave the stall learning (PilotFace) its 6 s to send the route round. */
    if (b->goal == G_OBJECTIVE && k == MA2_KIND_PILOT && b->now < b->obj_walk_until && b->obj_walk_ent == b->obj_ent &&
        c7_on("walkprogress", "RECOMP_BOT_WALK_PROGRESS", "WalkProgress", 1)) {
        float d = hdist(me->pos, b->obj_pt);
        if (b->now < b->obj_walk_until - 36.0 || b->obj_walk_best_t < b->obj_walk_until - 40.0 || hdist(me->pos, b->obj_walk_anchor) > 5.0f) { b->obj_walk_anchor = me->pos; b->obj_walk_best = d; b->obj_walk_best_t = b->now; }
        else if (b->now - b->obj_walk_best_t > 12.0 && d > 8.0f) {
            int slot = b->obj_nbad_next++ & 3;
            b->obj_bad[slot].ent = b->obj_ent; b->obj_bad[slot].pt = b->obj_pt; b->obj_bad[slot].op = b->obj_op; b->obj_bad[slot].until = b->now + 25.0;
            b->obj_walk_until = b->now; b->obj_np_t0 = 0.0; b->next_eval = 0.0; b->interrupt = 1; b->roam_valid = 0; b->tac_until = 0.0;
            c7_add("walkprogress", "giveups", 1);
            snprintf(b->note, sizeof b->note, "walk-stuck");
            if (diag_on()) fprintf(stderr, "[BOT-OBJ] t=%.1f bot=%d walk to %s %.0f,%.0f did not move 5 m in 12 s at %.0f,%.0f (%.0f m): skipped for 25 s\n", b->now, b->index, k_op_name[b->obj_op], b->obj_pt.x, b->obj_pt.z, me->pos.x, me->pos.z, d);
        }
    }
    /* ClaimProgress (default on, RECOMP_BOT_CLAIM_PROGRESS / [Bots] ClaimProgress): ObjGiveUp needs the planner's "no route"; a body with a route
     * that never gets there kept the claim forever. Check It No Man's Land terminal 634,74,1227 sits on a mesa reached only up its east face:
     * a BattleArmor jetted and churned at its foot 530-560,1170-1250 from 87 s to the end (p30_73a b1, 153 s), p30_73b b9 109 s on the hill
     * at 1439,111,647 under 1402,774. A claim that has not closed 10 m in 30 s (fights in between restart the count) skips that terminal 25 s. */
    if (b->goal == G_OBJECTIVE && b->obj_op == OP_CLAIM && c7_on("claimprogress", "RECOMP_BOT_CLAIM_PROGRESS", "ClaimProgress", 1)) {
        float d = hdist(me->pos, b->obj_pt);
        if (b->obj_cp_ent != b->obj_ent || b->now - b->obj_cp_seen > 2.0 || d < b->obj_cp_best - 10.0f) { b->obj_cp_ent = b->obj_ent; b->obj_cp_best = d; b->obj_cp_t = b->now; }
        else if (b->now - b->obj_cp_t > 30.0 && d > 15.0f) {
            int slot = b->obj_nbad_next++ & 3;
            b->obj_bad[slot].ent = b->obj_ent; b->obj_bad[slot].pt = b->obj_pt; b->obj_bad[slot].op = b->obj_op; b->obj_bad[slot].until = b->now + 25.0;
            b->obj_walk_until = 0.0; b->obj_cp_ent = 0; b->obj_np_t0 = 0.0; b->next_eval = 0.0; b->interrupt = 1; b->roam_valid = 0; b->tac_until = 0.0;
            c7_add("claimprogress", "giveups", 1);
            snprintf(b->note, sizeof b->note, "claim-stuck");
            if (diag_on()) fprintf(stderr, "[BOT-OBJ] t=%.1f bot=%d claim %.0f,%.0f not 10 m closer in 30 s at %.0f,%.0f (%.0f m): skipped for 25 s\n", b->now, b->index, b->obj_pt.x, b->obj_pt.z, me->pos.x, me->pos.z, d);
        }
        b->obj_cp_seen = b->now;
    }
    {   /* PuGiveUp (default on, RECOMP_BOT_PU_GIVEUP / [Bots] PuGiveUp; 1.0 pass fix round 4, Worker D): the planner's "no route" verdict (status.no_path: the route ends short and the body holds
         * at its end, or 3 plans failed) was read for objective targets (ObjGiveUp), vehicles (UnreachOnce, swap) and roam goals, but never for a power-up: the goal stayed,
         * the body held at the route's dry end and the brain re-picked the same pickup every evaluation (sw46 bot 4: 48 s at 1250,757 beside a hill-top power-up at 1218,750,
         * status.no_path from t=43; sw14 r6 bot 7: 47.6 s). Like ObjGiveUp, 2 s of the verdict on the walk to a power-up skips that pickup for this bot for 45 s and picks again at once.
         * [C7] pugiveup: giveups, skipped. */
        static int on = -1;
        if (on < 0) on = c7_on("pugiveup", "RECOMP_BOT_PU_GIVEUP", "PuGiveUp", 1);
        if (on && st && st->no_path && b->goal == G_POWERUP && b->pu_keep) {
            if (b->pu_np_t0 <= 0.0 || b->now < b->pu_np_t0) b->pu_np_t0 = b->now;
            else if (b->now - b->pu_np_t0 >= 2.0) {
                int slot = b->pu_bad_next++ & 3;
                b->pu_bad[slot].ent = b->pu_keep; b->pu_bad[slot].until = b->now + 45.0;
                b->pu_np_t0 = 0.0; b->pu_keep = 0; b->next_eval = 0.0; b->interrupt = 1; b->roam_valid = 0; b->tac_until = 0.0;
                c7_add("pugiveup", "giveups", 1);
                snprintf(b->note, sizeof b->note, "powerup-unreachable");
                if (diag_on()) fprintf(stderr, "[BOT-PUGIVEUP] t=%.1f bot=%d power-up %08X: no route, skipped for 45 s\n", b->now, b->index, b->pu_bad[slot].ent);
            }
        } else b->pu_np_t0 = 0.0;
    }
    if (st && (st->no_path || st->stalled) && b->goal == G_SWAP && b->swap_veh && c7_on("swapfix", "RECOMP_BOT_SWAP_UNSTICK", "SwapUnstick", 1)) {
        c7_add("swapfix", "giveups_nopath", 1);
        no_board_for(b, b->swap_veh, 60.0); b->swap_veh = 0; b->swap_cool_until = b->now + 10.0; b->next_eval = 0.0; b->swp_t0 = 0.0;
        snprintf(b->note, sizeof b->note, "swap-unreachable");
    }
    if (st && (st->no_path || st->stalled) && b->goal == G_GET_VEHICLE && b->veh) {
        /* UnreachOnce (default on, RECOMP_BOT_UNREACH_ONCE / [Bots] UnreachOnce): the status still describes the order of the ride just dropped, so
         * the next ride was dropped at once on the same stale flag -- three rides in one tick (sweep 11-02, [BOT-NOBOARD] 3x veh-unreachable at t=26.6)
         * and the bot was left with none. One drop per 2 s: the new ride gets a fresh status before it is judged. */
        if (b->now - b->fp_unreach_t < 2.0 && c7_on("unreachonce", "RECOMP_BOT_UNREACH_ONCE", "UnreachOnce", 1)) c7_add("unreachonce", "stale_ignored", 1);
        /* FootLock, the stall verdict too: a pilot locked 5-7 s on its wreck reads "stalled" in ctrl, and EjectGrace's 5 s ended inside the
         * lock -- r5_13 b7 70.1, b5 120.8 / 223.0, b6 173.3 and r5_34 b4 33.2, b8 62.5 dropped a ride 12-16 m away for one 107-279 m off. Not
         * in the first 8 s on foot when the ride is within 30 m. [C7] footlock: stall_held. */
        else if (k == MA2_KIND_PILOT && b->foot_since > 0.0 && b->now >= b->foot_since && b->now - b->foot_since < 8.0 &&
                 find_vehicle(w, b->veh) >= 0 && hdist(me->pos, w->vehicles[find_vehicle(w, b->veh)].pos) < 30.0f &&
                 c7_on("footlock", "RECOMP_BOT_FOOT_LOCK", "FootLock", 1)) c7_add("footlock", "stall_held", b->dt);
        else {
            b->fp_unreach_t = b->now;
            no_board_for(b, b->veh, 30.0);      /* out of reach for us, not refused: ours alone */
            b->veh = 0; b->veh_fail = 0;
            snprintf(b->note, sizeof b->note, "veh-unreachable");
        }
    }
    {   /* WP2-C FootProgress (default on, RECOMP_BOT_FOOT_PROGRESS / [Bots] FootProgress): a pilot ordered to a ride that has not moved 3 m in 7 s
         * (the controller's own stall detector never fired on 18 such spans in the 11-02 sweep: Proving Ground ledge spawns, 8-43 s each) drops that
         * ride for 45 s and picks again at once, from where it stands; the second strike in 40 s also hands the wish to a different kind. */
        int fi = (k == MA2_KIND_PILOT && b->goal == G_GET_VEHICLE && b->veh) ? find_vehicle(w, b->veh) : -1;
        const Ma2VehicleView *fv = fi >= 0 ? &w->vehicles[fi] : NULL;
        if (fv && hdist(me->pos, fv->has_entry ? fv->entry : fv->pos) > 8.0f && c7_on("footprog", "RECOMP_BOT_FOOT_PROGRESS", "FootProgress", 1)) {
            /* WP3-C FootJitter: the anchor test was 3 m, so a pilot shuffling 4-7 m on the spot (sw73 bot 0: 35 s at (505,1240) ordered to a ride 135 m away)
             * kept resetting it and never counted as stuck; 6 m (a walking pilot leaves that in under a second). */
            /* FootLock (default on, RECOMP_BOT_FOOT_LOCK / [Bots] FootLock): a pilot that just got out sits 5-7 s locked on its wreck; that
             * counted as "no headway", so the 7 s ran out as the lock ended and the ride 13 m away was dropped for one 150-299 m off -- r4_14
             * b1 12.9, b7 201.9, b2 149.8, b0 42.5: all four died 6-15 s later. The clock starts 8 s after the pilot came on foot. */
            int locked = b->foot_since > 0.0 && b->now >= b->foot_since && b->now - b->foot_since < 8.0 &&
                         c7_on("footlock", "RECOMP_BOT_FOOT_LOCK", "FootLock", 1);
            if (locked) c7_add("footlock", "held_s", b->dt);
            if (locked || b->fp_t0 <= 0.0 || hdist(me->pos, b->fp_pos) > (c7_on("footjitter", "RECOMP_BOT_FOOT_JITTER", "FootJitter", 1) ? 6.0f : 3.0f) || b->now < b->fp_t0) { b->fp_pos = me->pos; b->fp_t0 = b->now; b->fp_move_t = b->now; }
            else if (b->now - b->fp_t0 > 7.0) {
                c7_add("footprog", "stuck_giveups", 1);
                no_board_for(b, b->veh, 45.0);
                b->veh = 0; b->veh_fail = 0; b->next_eval = 0.0; b->interrupt = 1;
                if (b->now - b->fp_strike_t < 40.0) b->fp_strikes++; else b->fp_strikes = 1;
                b->fp_strike_t = b->now; b->fp_t0 = 0.0;
                if (b->fp_strikes >= 2) { c7_add("footprog", "second_strike", 1); b->fav_kind = MA2_KIND_NONE; }
                snprintf(b->note, sizeof b->note, "foot-stuck");
                if (diag_on()) fprintf(stderr, "[BOT-FOOTPROG] t=%.1f bot=%d no headway for 7 s at %.0f,%.0f,%.0f: ride dropped\n", b->now, b->index, me->pos.x, me->pos.y, me->pos.z);
            }
        } else b->fp_t0 = 0.0;
    }
    {   /* WP3-C BaNear (default on, RECOMP_BOT_BA_NEAR / [Bots] BaNear): a pilot boards a BattleArmor within 7 s of coming within 14 m of it, and always from under
         * 1.7 m of its centre (53 boardings in the 11-02 re-sweep: 3.7-6.6 s, closest 0.3-1.6 m); the 11 that failed stood at the zone rim (3.8-4.9 m) for 6-24 s
         * before the no-prompt note, which put pilots on foot 30+ s in CTF / Base War / Check It cells (sw60 bot 5, sw66 bot 3, sw73 bot 1, sw93 bots 3 and 10).
         * 8 s within 14 m and never closer than 2.5 m: drop that BattleArmor for 150 s (this pilot only) and take the next ride at once. */
        int bi = (k == MA2_KIND_PILOT && b->goal == G_GET_VEHICLE && b->veh) ? find_vehicle(w, b->veh) : -1;
        const Ma2VehicleView *bv = bi >= 0 && w->vehicles[bi].kind == MA2_KIND_BATTLEARMOR && w->vehicles[bi].has_entry ? &w->vehicles[bi] : NULL;
        float bd = bv ? hdist(me->pos, bv->entry) : 1e9f;
        if (bv && bd <= 14.0f && c7_on("banear", "RECOMP_BOT_BA_NEAR", "BaNear", 1)) {
            if (b->bn_veh != b->veh) { b->bn_veh = b->veh; b->bn_t0 = b->now; b->bn_dmin = bd; }
            if (bd < b->bn_dmin) b->bn_dmin = bd;
            if (b->now - b->bn_t0 > 8.0 && b->bn_dmin > 2.5f) {
                c7_add("banear", "giveups", 1);
                no_board_for(b, b->veh, 150.0);
                b->ba_ban_until = b->now + 150.0;
                b->veh = 0; b->veh_fail = 0; b->next_eval = 0.0; b->interrupt = 1; b->bn_veh = 0;
                snprintf(b->note, sizeof b->note, "ba-rim");
                if (diag_on()) fprintf(stderr, "[BOT-BANEAR] t=%.1f bot=%d 8 s at the BattleArmor's rim (closest %.1f m): dropped for 150 s\n", b->now, b->index, b->bn_dmin);
            }
        } else if (!bv || bd > 20.0f) b->bn_veh = 0;
    }
    if (st && (st->no_path || st->stalled) && (b->goal == G_COVER || b->goal == G_RETREAT)) b->tac_until = 0.0;
    if (st && st->roof_failed) {
        int i;
        for (i = 0; i < g_nbad_top; i++) if (hdist(g_bad_top[i], st->roof_pt) < 6.0f) break;
        if (i == g_nbad_top && g_nbad_top < 32) g_bad_top[g_nbad_top++] = st->roof_pt;
        b->tac_until = 0.0; b->peek_end = 0.0; b->ps_tgt = 0;
        if (diag_on()) fprintf(stderr, "[BOT-PERCH] t=%.1f bot=%d could not stand on the top at %.0f,%.0f -- skipped from now on\n", b->now, b->index, st->roof_pt.x, st->roof_pt.z);
    }
    /* A tank down a hollow it cannot climb out of (or a 'Mech wedged with no jets): what a player
     * does -- climb out, find another ride, and leave that hull alone for two minutes. */
    if (trapbail_on() && b->bail_trap && (k == MA2_KIND_PILOT || b->now >= b->bail_until)) {   /* (asked every step: the [C7] switch line) */
        if (k == MA2_KIND_PILOT) { c7_add("trapbail", "out", 1); b->bail_until = b->now; }   /* out: no more eject goal */
        b->bail_trap = 0;
    }
    int test_trap = 0;
    {   /* test hook RECOMP_BOT_TEST_TRAP=<s>: from then on, the first bot in a 'Mech/tank counts as wedged (once a match) */
        static double done_t = -1.0; const char *e = getenv("RECOMP_BOT_TEST_TRAP");
        if (done_t > b->now) done_t = -1.0;
        if (e && done_t < 0.0 && b->now >= atof(e) && (k == MA2_KIND_TANK || k == MA2_KIND_MECH) && !me->qte_active) { test_trap = 1; done_t = b->now; }
    }
    /* WedgeFight (default on, RECOMP_BOT_WEDGE_FIGHT / [Bots] WedgeFight): a hull wedged where the game refuses the eject (no room to set
     * the pilot down) asked for the bail-out every 15 s for the rest of the match and never fought: rc5_63a b4 (CTF Ill Wind, a tank in
     * the dip at 338,1006 by team 1's base) 102-240 s, rc4_65b b6 (CTF On the Rocks, 1509,1503) 121-240 s, iw9b b8 92-240 s. After two
     * bail-outs from the same hull it stays in and fights from there (the turret still turns). [C7] trapbail: wedge_fight. */
    int wedge_fight = 0;
    if (((st && st->trapped) || test_trap) && (k == MA2_KIND_TANK || k == MA2_KIND_MECH) && !me->qte_active && b->now >= b->bail_until) {
        if (b->wedge_actor != me->actor) { b->wedge_actor = me->actor; b->wedge_n = 0; }
        if (b->wedge_n >= 2 && c7_on("wedgefight", "RECOMP_BOT_WEDGE_FIGHT", "WedgeFight", 1)) { wedge_fight = 1; c7_add("trapbail", "wedge_fight", 1); b->bail_trap = 0; }
        else b->wedge_n++;
    }
    if (!wedge_fight && ((st && st->trapped) || test_trap) && (k == MA2_KIND_TANK || k == MA2_KIND_MECH) && !me->qte_active && b->now >= b->bail_until) {
        no_board_for(b, me->actor, 120.0);
        b->bail_until = b->now + (trapbail_on() ? 15.0 : 5.0); b->next_eval = 0.0;
        if (trapbail_on()) {
            /* no_board_refresh drops the entry above at once (the hull is occupied -- by us), so the bot
             * climbed straight back into the wedged 'Mech (smoke t3a-f10): keep it as the hull just left. */
            b->left_veh = me->actor; b->left_until = b->now + 120.0;
            b->bail_trap = 1; c7_add("trapbail", "traps", 1);
            if (c7_on("stuckhull", "RECOMP_BOT_STUCK_HULL", "StuckHull", 1)) { stuck_hull_add(me->actor, b->now); c7_add("stuckhull", "marked", 1); }
        }
        snprintf(b->note, sizeof b->note, "trapped");
        if (diag_on()) fprintf(stderr, "[BOT-TRAPPED] t=%.1f bot=%d kind=%d at %.0f,%.0f: bailing out\n", b->now, b->index, k, me->pos.x, me->pos.z);
    }

    perceive(b, w, self, sn);
    assess(b, w, me);
    vhp_note(w, b->now);                     /* DyingRide memory: hp of every driven hull */
    fight_band(b, me);
    if (!b->pu_typed) {                 /* armour drops can be told from weapon upgrades on this map */
        int i;
        for (i = 0; i < w->npickups && i < MA2_BOT_MAX_PICKUPS; i++) if (w->pickups[i].kind == MA2_PICKUP_POWERUP && w->pickups[i].armor > 0) b->pu_typed = 1;
    }
    {   /* Up on a roof or a ledge? Dropped off one we did not mean to leave (it was destroyed under
         * us, or we were knocked off): the position is gone -- re-think, and look for other high
         * ground (BattleArmor always, a 'Mech now and then). */
        int ok = 0, oks = 0;
        float g = sn && sn->ground ? sn->ground(sn->ctx, me->pos.x, me->pos.z, &ok) : 0.0f;
        float s = sn && sn->surface ? sn->surface(sn->ctx, me->pos.x, me->pos.z, &oks) : g;
        int was = b->raised;
        b->raised = ok && oks && s > g + 3.0f && fabsf(me->pos.y - s) < 1.5f;     /* standing on it, not flying over it */
        if (b->raised) { b->perch = me->pos; b->perch_h = s; }
        else if (was && ok && me->pos.y < g + 1.5f) {
            /* Down on the ground now. Did we step off, or is the roof gone? Look where we stood. */
            int okp = 0;
            float sp = sn && sn->surface ? sn->surface(sn->ctx, b->perch.x, b->perch.z, &okp) : b->perch_h;
            if (okp && sp < b->perch_h - 3.0f) {
                b->peek_end = 0.0; b->ps_tgt = 0; b->tac_until = 0.0; b->next_eval = 0.0; b->ba_high_next = 0.0;
                if (k == MA2_KIND_BATTLEARMOR || (k == MA2_KIND_MECH && rng_f(&b->rng) < 0.5f)) { b->reperch_until = b->now + 8.0; c7_add("hg_perch", k == MA2_KIND_MECH ? "reperch_mech" : "reperch_ba", 1); }
                if (diag_on()) fprintf(stderr, "[BOT-PERCH] t=%.1f bot=%d its building at %.0f,%.0f is gone\n", b->now, b->index, b->perch.x, b->perch.z);
            }
        }
    }
    choose_target(b, w, me, self);
    spc_tick(b, w, me, self, sn);   /* surprise set C tricks: triggers */
    if (zones_on()) {
        if (b->tgt) zone_note(b, me, b->tgt->pos);
        else if (b->roam_valid) zone_note(b, me, b->roam_pt);
        zone_sample(b);
    }
    /* Kills: an enemy we were finishing off is gone -> remember where (it drops power-ups). */
    if (b->kill_watch) {
        const Ma2PlayerView *kv = player_by_actor(w, b->kill_watch);
        if (!kv || !kv->alive) {
            b->kill_t = b->now; b->kill_pos = b->kill_watch_pos; b->kills++; b->kill_watch = 0;
            if (diag_on()) fprintf(stderr, "[BOT-KILL] t=%.1f bot=%d at %.0f,%.0f\n", b->now, b->index, b->kill_pos.x, b->kill_pos.z);
        }
    }
    if (b->tgt && rate_of(b->tgt->out_v, b->tgt->out_t, b->now) > 0.0f && hp_frac(b->tgt->hp, b->tgt->hp_max) < 0.4f) {
        b->kill_watch = b->tgt->actor; b->kill_watch_pos = b->tgt->pos;
    }
    if (b->now >= b->next_eval || ((me->qte_active || me->enemy_rider) && k != MA2_KIND_BATTLEARMOR && k != MA2_KIND_PILOT && b->goal != G_DEFEND_HACK)) {
        evaluate(b, w, me, self, sn);
        b->next_eval = b->now + MA2B_EVAL_PERIOD;
    }

    sp2_tick(b, w, me, nav, sn);     /* surprise set B: bait / pincer set the tactical point (tac_*), power-up trips flag an order override */

    switch (b->goal) {
    case G_GET_VEHICLE: order_get_vehicle(b, w, me, nav, out); break;
    case G_ATTACK:      order_attack(b, w, me, nav, sn, out, st); break;
    case G_COVER:       order_cover(b, w, me, sn, out, 0); break;
    case G_RETREAT:     order_cover(b, w, me, sn, out, 1); break;
    case G_POWERUP:     order_powerup(b, w, me, out); break;
    case G_REPAIR:
        if (b->goal_since > b->repair_since || b->now < b->repair_since) b->repair_since = b->now;
        if (b->repair_kind == 2) {                  /* the pack is gone (taken, by us or anyone): think again */
            int still = 0;
            for (int i = 0; i < w->npickups && i < MA2_BOT_MAX_PICKUPS; i++) if (w->pickups[i].entity == b->repair_pack) still = 1;
            if (!still) { b->next_eval = 0.0; b->repair_kind = 0; }
        }
        order_travel(out, b->repair_pt, b->repair_kind == 1 ? 4.0f : 2.0f, 1);
        if (b->repair_kind == 2 && k != MA2_KIND_VTOL && c7_on("putouch", "RECOMP_BOT_PICKUP_TOUCH", "PickupTouch", 1)) { out->touch = 1; out->stop = 0.5f; out->exact = 1; out->exact_r = 15.f; }   /* PickupTouch: onto the pack until it is gone */
        out->vt_heal = b->repair_kind ? b->repair_kind : 1;
        if (b->now - b->repair_since > 45.0) {      /* not working out */
            b->next_eval = 0.0; b->tac_fail_until = b->now + 20.0;
            if (ma2_bot_vtol_pads_on()) {           /* F9: and really stop: no repair for 20 s; a pad we sat at that did not heal us, not for 120 s */
                extern int g_c7_pad_giveups; g_c7_pad_giveups++;
                b->repair_block_until = b->now + 20.0;
                int at_pad = b->repair_kind == 1 && b->repair_pad >= 0 && b->repair_pad < 8 && hdist(me->pos, b->repair_pt) < 60.0f;
                if (at_pad) b->pad_bad_until[b->repair_pad] = b->now + 120.0;
                if (diag_on()) fprintf(stderr, "[BOT-REPAIR] t=%.1f bot=%d gives up the %s run after 45 s%s\n", b->now, b->index,
                                       b->repair_kind == 2 ? "pack" : "pad", at_pad ? ": that pad is skipped for 120 s" : "");
                b->repair_since = b->now;
            }
        }
        snprintf(b->note, sizeof b->note, b->repair_kind == 2 ? "repair-pack" : "repair-pad");
        break;
    case G_SUPPORT:     order_support(b, w, me, sn, out); break;
    case G_HACK:        order_hack(b, w, me, out); break;
    case G_SWAP:        order_swap(b, w, me, sn, out); break;
    case G_RIDE:        order_ride(b, w, me, sn, out); break;
    case G_LIFT:        order_lift(b, w, me, nav, sn, out); break;
    case G_DEFEND_HACK: order_defend(b, me, out); break;
    case G_OBJECTIVE:   obj_order(b, w, me, out); break;
    case G_EJECT:
        /* The game refuses the exit where there is no room to set the pilot down (a tank on a rock
         * edge pressed eject for 40 s): after 6 s (3 s when wedged), drive 20 m somewhere else for 4 s, then again. */
        if (b->goal_since > b->eject_since || b->now < b->eject_since) b->eject_since = b->now;
        if (k != MA2_KIND_PILOT && k != MA2_KIND_VTOL && k != MA2_KIND_TURRET && b->now - b->eject_since > (b->bail_trap ? 3.0 : 6.0) && b->now >= b->eject_move_until + 3.0) {   /* a VTOL keeps setting down */
            b->eject_move_pt = vadd(me->pos, vscale(dir_of_heading(rng_range(&b->rng, -MA2B_PI, MA2B_PI)), 20.0f));
            b->eject_move_until = b->now + 4.0;
            if (b->bail_trap) c7_add("trapbail", "moves", 1);
        }
        if (b->now < b->eject_move_until) { order_travel(out, b->eject_move_pt, 3.0f, 0); snprintf(b->note, sizeof b->note, "eject-move"); break; }
        if (!me->qte_active && (b->now >= b->next_eject || k == MA2_KIND_VTOL)) {   /* a VTOL's exit is held (Y): it sets down, then out */
            if (b->now >= b->next_eject) b->st_eject++;
            out->type = MA2_ORD_EJECT; b->next_eject = b->now + 2.0;
            snprintf(b->note, sizeof b->note, "eject");
        } else out->type = MA2_ORD_IDLE;
        break;
    default:
        if (b->lms_out && k == MA2_KIND_PILOT) { out->type = MA2_ORD_IDLE; snprintf(b->note, sizeof b->note, "lms-hide"); break; }   /* LmsNoRide: out -- no roaming */
        if (roam_due(b, me)) pick_roam(b, w, me, nav);
        order_travel(out, b->roam_pt, 8.0f, 1);
        break;
    }
    /* LmsOutIdle (default on, RECOMP_BOT_LMS_OUT_IDLE / [Bots] LmsOutIdle): an out LMS pilot was idle only on the roam goal; an attack goal
     * still sent it out (d5_33: b4 3 attack sorties trailing b0 82 m, b9 2; 3 bomb presses that failed). Out = idle, whatever the goal. */
    if (b->lms_out && k == MA2_KIND_PILOT && c7_on("lmsoutidle", "RECOMP_BOT_LMS_OUT_IDLE", "LmsOutIdle", 1)) {
        memset(out, 0, sizeof *out); out->type = MA2_ORD_IDLE; snprintf(b->note, sizeof b->note, "lms-hide"); c7_add("lms", "out_idle_s", b->dt);
    }
    sp2_order(b, w, me, sn, out);    /* surprise set B: the Elite waits at its power-up spot / holds its denial spot */
    blast_wary(b, w, me, out);       /* BlastWary: not in close to a 'Mech about to blow up */
    /* Long trips (hunting, objectives) go by this bot's lane; on foot heading for a vehicle,
     * in the air, or falling back it goes straight. */
    obj_bomb_on_way(b, w, me, out);
    if (lanespread_on() && w->nlanes > 1) {
        float f = b->now >= b->lane_keep_until ? lane_assign(b, w, me) : b->flank;
        if (k == MA2_KIND_TANK && f != 0.0f && c7_on("tankmid", "RECOMP_BOT_TANK_MID", "TankMid", 1)) { f = 0.0f; if (b->flank != 0.0f) c7_add("tankmid", "n", 1); }   /* TankMid, below */
        if (f != b->flank) { b->flank = f; c7_add("lanes", "lane_changes", 1); }
        if (b->lane_t <= 0.0) b->lane_t = b->now > 0.0 ? b->now : 1e-3;
        lane_note(b, me);
    } else if (b->lane_t <= 0.0) lane_pick(b, me);
    /* TankMid (default on, RECOMP_BOT_TANK_MID / [Bots] TankMid; playtest 11-05): a tank on a flank lane went down into the low ground by
     * its own base and stayed there (a playtester's TD game 11-05: b3, 48-95 s at 810,63,1005, hunt-left, 0-20 m/s in a 20 m patch, never in
     * sight of an enemy). A tank is no flanker -- it goes up the main channel with the team, where it can see and support. */
    if (k == MA2_KIND_TANK && b->flank != 0.0f && !(lanespread_on() && w->nlanes > 1) && c7_on("tankmid", "RECOMP_BOT_TANK_MID", "TankMid", 1)) {
        b->flank = 0.0f; b->via_state = 0; lane_note(b, me); c7_add("tankmid", "n", 1);
    }
    else lane_note(b, me);
    /* Batch 4: a flag run is a race -- grabbing, carrying, returning a flag and chasing It go straight
     * (NRC CTF: lanes cost a 1.8x detour and a ~20 s stall at the first lane point; all lanes off won
     * back 40% of the batch-3 capture drop, 4D). RECOMP_BOT_LANE_OBJ=0 / [Bots] LaneObjective=0 = by lane too. */
    if (b->goal == G_OBJECTIVE && (b->obj_op == OP_GRAB || b->obj_op == OP_CARRY || b->obj_op == OP_RETURN || b->obj_op == OP_CHASE_IT) &&
        out->type == MA2_ORD_TRAVEL && lanespread_on() && w->nlanes > 1 && c7_on("laneobj", "RECOMP_BOT_LANE_OBJ", "LaneObjective", 1)) {
        if (b->via_state == 1) lane_trip_end(b, 3);
        b->via_state = 0;
        c7_add("lanes", "obj_straight", 1);
    } else if (out->type == MA2_ORD_TRAVEL && k != MA2_KIND_VTOL && k != MA2_KIND_FLYER &&
        (b->goal == G_ROAM || b->goal == G_ATTACK || b->goal == G_OBJECTIVE))
    {
        Ma2Vec3 real = out->point;
        out->point = lane_point(b, w, sn, nav, me, out->point);
        /* Going by a lane's way point: arrive at it, not "within weapon range" of it -- with a
         * Destruction order's 70 m stop the bot halted short of the way point, which only counts
         * as passed inside 30 m, and stood there for the rest of the match. */
        if (hdist(out->point, real) > 1.0f && out->stop > 15.0f) out->stop = 15.0f;
    }
    else if (b->via_state == 1 && !b->lane_paused) { b->lane_paused = 1; lane_trip_end(b, 5); }
    if (out->type == MA2_ORD_TRAVEL && k != MA2_KIND_VTOL && k != MA2_KIND_FLYER && k != MA2_KIND_PILOT && w->nchokes > 0 &&
        c7_on("chokes", "RECOMP_BOT_CHOKES", "Chokes", 1))
        choke_flow(b, w, me, out);
    lane_zone_sample(b, w, me);
    b->obj_lock = obj_locked(b, me);
    /* Stepping off a ledge (the planner's one-way drops). No fall hurts; what a drop can cost is the
     * position (playtest 2026-10-14). On a trip -- the flag run home, a hunt, a vehicle, a
     * retreat far from any objective -- any drop. In a fight, at a post (cover, firing spots,
     * escorting, guarding or holding an objective), on the last stretch to an objective, or falling
     * back near one, only a drop it can jet straight back up: a 'Mech without jets, or above a cliff
     * higher than its jets climb, holds the high ground, takes the flag and fights its way out --
     * and drops off on the way home. */
    {
        int near_obj = b->obj_op != OP_NONE && hdist(me->pos, b->obj_pt) < 150.0f;
        int carrying = b->goal == G_OBJECTIVE && b->obj_op == OP_CARRY;
        /* W5 CarrierDrops: a carrier going home and a chaser of the enemy's carrier take any drop (a one-way
         * shortcut is what they want), also near the objective, where a tactical move would be limited to
         * drops the jets climb back up. Defenders and everyone else keep the rule above. */
        int chasing = b->goal == G_OBJECTIVE && b->obj_op == OP_HUNT && b->now - b->cd_chase_t < 0.6;
        int pref = (carrying || chasing) && c7_on("carrierdrops", "RECOMP_BOT_CARRIER_DROPS", "CarrierDrops", 1);
        out->drop_pref = pref;
        if (pref) c7_add("carrierdrops", carrying ? "orders_carry" : "orders_chase", 1);
        /* The carrier also keeps clear of the enemies it knows about (guns' soft costs), until the last 40 m:
         * the shortest way home ran through the enemy spawn's traffic (playtest, Bogged Down CTF). */
        /* push-final (design decision): CarrierDanger default OFF -- not asked for in playtests; W5 runs showed +10-25% longer carries, no survival gain. [Bots] CarrierDanger=1 turns it on. */
        if (carrying && out->type == MA2_ORD_TRAVEL && hdist(me->pos, out->point) > 40.0f && c7_on("carrierdanger", "RECOMP_BOT_CARRIER_DANGER", "CarrierDanger", 0)) {
            add_danger(b, me, out);
            c7_add("carrierdanger", "orders", 1);
            if (out->ndanger > 0) { c7_add("carrierdanger", "with_danger", 1); c7_add("carrierdanger", "circles", out->ndanger); }
        }
        out->drop_mode = !carrying && !pref &&
            (b->goal == G_ATTACK || b->goal == G_COVER || b->goal == G_SUPPORT || (b->goal == G_RETREAT && near_obj) ||
             (b->goal == G_OBJECTIVE && (b->obj_op == OP_GUARD || b->obj_op == OP_HOLD || b->obj_op == OP_CLAIM ||
                                         b->obj_op == OP_ESCORT || near_obj))) ? 1 : 0;
    }
    /* Objective duty keeps the body -- a flag runner or a zone holder never stops to fight (engage
     * stays off) -- but not the guns: it shoots what it meets on the move, as a player running a
     * flag shoots back. (It used to hold fire while carrying, holding, and in the last 40 m to the
     * objective: every carrier and every attacker at the stand was a free kill -- a playtester's 17
     * kills to the best bot's 7 in Capture the Flag, 2026-10-14.) */
    if (out->type == MA2_ORD_TRAVEL && !out->hold_fire && b->tgt && mem_engageable(b, b->tgt) &&
        hdist(me->pos, b->tgt->pos) <= my_reach(me) &&
        (out->engage || b->goal == G_OBJECTIVE || b->goal == G_POWERUP || b->goal == G_ROAM || b->goal == G_GET_VEHICLE) &&
        !(out->fire && out->target && b->goal == G_OBJECTIVE && (b->obj_op == OP_HUNT || b->obj_op == OP_CHASE_IT))) {   /* CarrierFire: the carrier stays the target */
        out->fire = 1; out->target = b->tgt->actor;
    }
    /* TravelFire (default on, RECOMP_BOT_TRAVEL_FIRE / [Bots] TravelFire; film review 11-08): an objective / power-up / roam leg that names a target
     * (the hunted carrier, the enemy at the stand, the one by the escort) shoots it when it is seen and in reach -- [BOT-NOFIRE] "order fire=0" was the
     * most common silence after heat in the par9 CTF matches (guards 40, stagers 30, escorts 19 per 5 matches: walking with an enemy in sight and
     * reach, no shot). The F29 reaction delay still applies. [C7] travelfire: s. */
    if (out->type == MA2_ORD_TRAVEL && !out->hold_fire && !out->fire && out->target && b->goal != G_GET_VEHICLE && c7_on("travelfire", "RECOMP_BOT_TRAVEL_FIRE", "TravelFire", 1)) {
        Mem *tm = mem_by_actor(b, out->target);
        if (tm && tm->visible && mem_engageable(b, tm) && hdist(me->pos, tm->pos) <= my_reach(me)) { out->fire = 1; c7_add("travelfire", "s", b->dt); }
    }
    if (!out->target && b->tgt && out->type != MA2_ORD_GET_VEHICLE) out->target = b->tgt->actor;
    /* Jumpjet hops while fighting in a 'Mech with jets: a harder target (skill sets how often;
     * twice as often while being hit). */
    /* A projectile on its way in (a missile homing, a shell on line): seen in time -- a reaction
     * the skill allows, and the skill's chance of noticing at all -- step out of its line, to the
     * side away from it. Recruits mostly eat it; Elites rarely do. */
    {
        static const float notice[4] = { 0.20f, 0.45f, 0.75f, 0.92f };
        float r = k == MA2_KIND_VTOL ? 6.0f : k == MA2_KIND_MECH || k == MA2_KIND_TANK ? 5.0f : 3.0f;
        for (int i = 0; i < w->nproj && i < 32 && (k == MA2_KIND_VTOL || k == MA2_KIND_MECH || k == MA2_KIND_BATTLEARMOR); i++) {
            Ma2Vec3 p = w->proj[i].pos, v = w->proj[i].vel;
            float sp2 = v.x * v.x + v.y * v.y + v.z * v.z;
            if (sp2 < 25.0f * 25.0f) continue;                                 /* not a missile or a shell */
            float rx = me->pos.x - p.x, ry = me->pos.y + 2.0f - p.y, rz = me->pos.z - p.z;
            float tca = (rx * v.x + ry * v.y + rz * v.z) / sp2;
            if (tca <= 0.0f || tca > 2.0f) continue;                           /* going away, or still far off */
            float cx = p.x + v.x * tca - me->pos.x, cy = p.y + v.y * tca - (me->pos.y + 2.0f), cz = p.z + v.z * tca - me->pos.z;
            if (cx * cx + cy * cy + cz * cz > r * r) continue;                 /* it misses anyway */
            uint32_t key = w->proj[i].ent;
            int seen = 0;
            for (int q = 0; q < 16; q++) if (b->proj_seen4[q] == key) seen = 1;
            if (seen) continue;                                                /* already decided about this one */
            b->proj_seen4[b->proj_seen_i++ & 15] = key;
            if (tca < b->sk.reaction * 0.6f || rng_f(&b->rng) > notice[b->skill & 3]) break;
            /* away from its line: the side of the line we stand on (cross of its course and us) */
            float side = v.x * rz - v.z * rx, vl = sqrtf(v.x * v.x + v.z * v.z) + 1e-3f;
            out->dodge = side >= 0.0f ? 1 : -1; out->dodge_now = 1;
            /* perpendicular to its course, on our side of it */
            out->dodge_dx = -v.z / vl * (float)out->dodge; out->dodge_dz = v.x / vl * (float)out->dodge;
            if (out->dodge_dx * rx + out->dodge_dz * rz < 0.0f) { out->dodge_dx = -out->dodge_dx; out->dodge_dz = -out->dodge_dz; }
            /* DodgeFlee (default on, RECOMP_BOT_DODGE_FLEE / [Bots] DodgeFlee): falling back, the side to dodge to is the side it is going: either
             * one takes it off the missile's course (they home), but "our side" of a shot from the flank points back the way it came. au3_20 bot 9
             * (TD Bogged Down, 11-04) retreating south-east from a 'Mech south-west of it dodged 5 times in 17 s (t=209.9-226.7), each jet throwing
             * it north-west, and died 19 m from where it started (hp 456 -> 0). [C7] dodgeflee: flips. */
            if (b->goal == G_RETREAT && c7_on("dodgeflee", "RECOMP_BOT_DODGE_FLEE", "DodgeFlee", 1)) {
                float tx = out->point.x - me->pos.x, tz = out->point.z - me->pos.z;
                if (tx * tx + tz * tz > 25.0f && out->dodge_dx * tx + out->dodge_dz * tz < 0.0f) {
                    out->dodge_dx = -out->dodge_dx; out->dodge_dz = -out->dodge_dz; out->dodge = -out->dodge; c7_add("dodgeflee", "flips", 1);
                }
            }
            if (diag_on()) fprintf(stderr, "[BOT-INCOMING] t=%.1f bot=%d kind=%d proj=%08X in %.2fs miss=%.1f side=%d\n", b->now, b->index, k, key, tca,
                                   sqrtf(cx * cx + cy * cy + cz * cz), out->dodge);
            break;
        }
    }
    if (k == MA2_KIND_VTOL) {
        /* Veterans and Elites hug the ground (less sky behind them, more cover under them); the
         * green ones fly high more often. A chance per leg, kept for the leg. */
        static const float low[4] = { 0.15f, 0.40f, 0.70f, 0.90f }, cover[4] = { 0.10f, 0.35f, 0.65f, 0.85f };
        if (b->now >= b->vt_low_until) { b->vt_low = rng_f(&b->rng) < low[b->skill & 3]; b->vt_low_until = b->now + rng_range(&b->rng, 8.0f, 16.0f); }
        out->vt_low = b->vt_low; out->vt_cover = cover[b->skill & 3];
        if (vt_threat_alt(b, w, me, sn, out, vt_threat_on())) out->vt_low = 1;
        else if (vt_threat_on()) out->vt_low = 0;     /* clear: the cruise height, over hills as always */
        /* A BattleArmor hanging under us: never nap-of-the-earth -- it hangs below the skids, and at
         * 8 m over the rooftops it was scraped off onto one 140 m short of their flag (run fr3). */
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++)
            if (w->players[j].alive && w->players[j].riding == me->actor) { out->vt_low = 0; break; }
        if (out->vt_low) c7_add("vtthreat", k_vt_low_s[b->skill & 3], b->dt);
    }
    if ((out->type == MA2_ORD_ENGAGE || out->type == MA2_ORD_FIGHT_MOVE) && (k == MA2_KIND_MECH || k == MA2_KIND_BATTLEARMOR) && b->sk.dodge > 0.0f && !out->dodge_now &&
        b->tgt && b->tgt->visible) {
        if (b->dodge_next <= 0.0) b->dodge_next = b->now + rng_range(&b->rng, 1.0f, 4.0f);
        else if (b->now >= b->dodge_next) {
            float rate = b->sk.dodge * (b->now - b->last_hit < 1.5 ? 2.0f : 1.0f);
            b->dodge_next = b->now + 10.0 / rate * rng_range(&b->rng, 0.6f, 1.4f);
            out->dodge = rng_f(&b->rng) < 0.5f ? -1 : 1;
        }
    } else b->dodge_next = 0.0;
    sp_rk_order(b, out);   /* surprise: rookie C / D flags */
    alpha_logic(b, w, me, out);
    alpha_vtol(b, me, out);
    if (shield_logic(b, me)) out->defend = 1;
    out->qte_press = qte_control(b, me);
    cloak_gate(b, w, me, out);
    spc_order(b, w, me, sn, st, out);   /* surprise set C tricks: the running trick overrides the order */

    b->prev_valid = 1;
    b->prev_actor = me->actor;
    b->prev_kind = k;
}

/* F16 (bots batch 1): a pilot or BattleArmor that gives up the ride it was going for says why, once
 * per drop -- on editor maps pilots stopped 5-12 m from a 'Mech and dropped it with nothing logged. */
static const char *nob_reason(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2VehicleView *v)
{
    if (!v) return "gone";
    if (v->occupied) return "occupied";
    if (v->hp <= 0.0f || !v->actor) return "destroyed";
    if (!v->has_entry) return "no-entry-zone";
    if (yields_to_human(me, v)) return "yield-human";
    if (v->claimed_by && v->claimed_by != me->player && (!w->team_mode || v->claim_nation == me->nation)) return "claimed";
    if (v->nation > 0 && v->nation != 6 && v->nation != me->nation) return "other-nation";
    if (refused_has(v->actor, b->now)) return "refused";
    if (no_board_has(b, v->actor)) return b->note[0] ? b->note : "no-board";
    return "rescored";
}

/* F29 first-shot delay (bots batch 2; env RECOMP_BOT_F29 / ini [Bots] BotFirstShot, default 1).
 * The skill's reaction time (Recruit 0.90 .. Elite 0.12 s) gated only the Attack goal and target
 * switches; travel, hunt, power-up and vehicle orders fired the moment the target came into sight,
 * so a Recruit rounding a corner shot as fast as an Elite. Now no order fires at a target seen for
 * less than the reaction time -- except straight back at one that hit us in the last second. Fire
 * at a target out of sight (cloak grace, a peek) is left alone. [C7] F29 holds_s0..3 (ticks held). */
static void f29_first_shot(Ma2Brain *b, Ma2Order *out)
{
    static const char *const holds[4] = { "holds_s0", "holds_s1", "holds_s2", "holds_s3" };
    const Mem *m;
    if (!out->fire || !out->target || !c7_on("F29", "RECOMP_BOT_F29", "BotFirstShot", 1)) return;
    m = mem_by_actor(b, out->target);
    if (!m || !m->visible || b->now - m->vis_since >= (double)b->sk.reaction || b->now - m->hit_me_t < 1.0) return;
    out->fire = 0;
    c7_add("F29", holds[b->skill & 3], 1);
}

void ma2_brain_tick(Ma2Brain *b, const Ma2World *w, int self, Ma2Nav *nav,
                    const Ma2CtrlStatus *st, const Ma2Senses *sn, Ma2Order *out)
{
    uint32_t veh0 = b ? b->veh : 0, actor0 = 0;
    int goal0 = b ? b->goal : 0;
    if (b && w && self >= 0 && self < w->nplayers && self < MA2_WORLD_MAX_PLAYERS) actor0 = w->players[self].actor;
    brain_tick_body(b, w, self, nav, st, sn, out);
    if (b && out) f29_first_shot(b, out);
    if (!veh0 || !actor0 || b->veh == veh0 || !ma2_bot_entry_fix_on()) return;
    const Ma2PlayerView *me = &w->players[self];
    if (me->actor != actor0 || !me->alive || (me->kind != MA2_KIND_PILOT && me->kind != MA2_KIND_BATTLEARMOR)) return;   /* boarded or died */
    const Ma2VehicleView *v = NULL;
    for (int i = 0; i < w->nvehicles && i < MA2_BOT_MAX_VEHICLES; i++) if (w->vehicles[i].actor == veh0) v = &w->vehicles[i];
    c7_add("entry", "drops", 1);
    if (diag_on()) fprintf(stderr, "[BOT-NOBOARD] bot=%d veh=%08X kind=%d reason=%s note=%s goal=%s->%s d_hull=%.1f d_entry=%.1f r=%.1f next=%08X t=%.1f\n",
            self, veh0, v ? v->kind : -1, nob_reason(b, w, me, v), b->note, k_goal_name[goal0 >= 0 && goal0 < G_COUNT ? goal0 : 0],
            k_goal_name[b->goal >= 0 && b->goal < G_COUNT ? b->goal : 0], v ? hdist(me->pos, v->pos) : -1.0f,
            v && v->has_entry ? hdist(me->pos, v->entry) : -1.0f, v ? v->entry_r : 0.0f, b->veh, w->time);
}

void ma2_brain_describe(const Ma2Brain *b, char *buf, size_t len)
{
    int g;
    if (!buf || !len) return;
    if (!b) { snprintf(buf, len, "brain=null"); return; }
    g = b->goal >= 0 && b->goal < G_COUNT ? b->goal : 0;
    snprintf(buf, len, "bot%d sk=%d goal=%s kind=%s tgt=%08X att=%d in=%.0f force=%.1f%s veh=%08X fav=%d rays=%u qte=%d/%d/%d shield=%d %s obj=%s/%s%s",
             b->index, b->skill, k_goal_name[g], k_kind[kind_ok(b->prev_kind)].name,
             (unsigned)b->target, b->n_attackers, b->in_dps, b->force, b->outnumbered ? " OUTNUMBERED" : "",
             (unsigned)b->veh, b->fav_kind, b->rays_used,
             b->qte.answered, b->qte.cues, b->qte.missed, b->st_shield,
             b->note, k_role_name[b->obj_role % 3], k_op_name[b->obj_op % OP_COUNT], b->obj_lock ? "!" : "");
}

/* Bot recorder (ma2_bot_rec.c): the brain's size, and the shared tables every bot reads and writes
 * (the team's lifts, flag runs, musters, lanes ...), kept raw each recorded frame. */
size_t ma2_brain_size(void) { return sizeof(struct Ma2Brain); }
int ma2_brain_rec_tables(const void **ptr, size_t *size, const char **name, int max)
{
    int n = 0;
#define BRT(x) if (n < max) { ptr[n] = &(x); size[n] = sizeof(x); name[n] = #x; n++; }
    BRT(g_lift) BRT(g_frun) BRT(g_hurt) BRT(g_frun_pull) BRT(g_frun_want) BRT(g_bapick) BRT(g_muster) BRT(g_combo)
    BRT(g_tb) BRT(g_lane) BRT(g_flankers) BRT(g_pu_taken) BRT(g_spot) BRT(g_zone) BRT(g_refused) BRT(g_vt_noreach)
    BRT(g_hg_place) BRT(g_hg_kill) BRT(g_mhp) BRT(g_pair)
#undef BRT
    return n;
}
