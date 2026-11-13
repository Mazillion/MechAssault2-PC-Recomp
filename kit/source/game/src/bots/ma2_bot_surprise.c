/* ma2_bot_surprise.c - shared surprise-behaviour module (#included by ma2_bot_brain.c after the brain's helpers;
 * see ma2_bot_surprise.h). Owns: the rarity gate sp_roll(), the per-bot cooldown / per-victim budget /
 * last-trick / spacing tables, the match-wide death + killer table (scoreboard kill within 1 s), and the
 * per-bot tick that hands death events to the tricks.
 *
 * [C7] pieces: sp_all (module-wide: rolls, blocked_cool/budget/repeat/spacing, victim_human, trick_s, bot_s,
 * tick_ms/tick_max), plus one piece per trick (sp_revenge, sp_grudge, sp_mood, sp_rookie, ...) with
 * fired_<skill> (rookie: <sub>_<skill>) added by sp_roll itself. */
#include "ma2_bot_surprise.h"

int ma2_pc_ini_get_int(const char *section, const char *key, int fallback);   /* pc_settings.h */

/* ---- trick table: name, [C7] piece, switch env + ini key, cooldown (min), p per skill (Recruit..Elite) ---- */
typedef struct SpDef {
    const char *name, *piece, *env, *ini, *fired;   /* fired = counter prefix ("fired_<skill>" by default) */
    float cool_min, p[4];
} SpDef;
static SpDef g_spdef[SP_N] = {
    /* SP_REVENGE */ { "revenge",   "sp_revenge", "RECOMP_BOT_SP_REVENGE",   "SpRevenge",   "fired",     4.0f, { 0.10f, 0.15f, 0.20f, 0.25f } },
    /* SP_GRUDGE  */ { "grudge",    "sp_grudge",  "RECOMP_BOT_SP_GRUDGE",    "SpGrudge",    "fired",     4.0f, { 0.15f, 0.20f, 0.25f, 0.30f } },
    /* SP_MOOD    */ { "mood",      "sp_mood",    "RECOMP_BOT_SP_MOOD",      "SpMood",      NULL,        3.0f, { 0.50f, 0.45f, 0.35f, 0.25f } },
    /* SP_RK_A    */ { "tunnel",    "sp_rookie",  "RECOMP_BOT_SP_ROOKIE",    "SpRookie",    "tunnel",    3.0f, { 0.12f, 0.04f, 0.0f, 0.0f } },
    /* SP_RK_B    */ { "noflank",   "sp_rookie",  "RECOMP_BOT_SP_ROOKIE",    "SpRookie",    "noflank",   3.0f, { 0.12f, 0.04f, 0.0f, 0.0f } },
    /* SP_RK_C    */ { "straight",  "sp_rookie",  "RECOMP_BOT_SP_ROOKIE",    "SpRookie",    "straight",  3.0f, { 0.12f, 0.04f, 0.0f, 0.0f } },
    /* SP_RK_D    */ { "outrange",  "sp_rookie",  "RECOMP_BOT_SP_ROOKIE",    "SpRookie",    "outrange",  3.0f, { 0.12f, 0.04f, 0.0f, 0.0f } },
    /* SP_RK_E    */ { "vtolchase", "sp_rookie",  "RECOMP_BOT_SP_ROOKIE",    "SpRookie",    "vtolchase", 3.0f, { 0.12f, 0.04f, 0.0f, 0.0f } },
    /* SP_RK_F    */ { "lastshot",  "sp_rookie",  "RECOMP_BOT_SP_ROOKIE",    "SpRookie",    "lastshot",  3.0f, { 0.12f, 0.04f, 0.0f, 0.0f } },
    /* SP_BAIT    */ { "bait",      "sp_bait",    "RECOMP_BOT_SP_BAIT",      "SpBait",      "fired",     4.0f, { 0.0f, 0.0f, 0.30f, 0.45f } },
    /* SP_PINCER  */ { "pincer",    "sp_pincer",  "RECOMP_BOT_SP_PINCER",    "SpPincer",    "fired",     5.0f, { 0.0f, 0.0f, 0.25f, 0.45f } },
    /* SP_POWERUP */ { "powerup",   "sp_powerup", "RECOMP_BOT_SP_POWERUP",   "SpPowerup",   "fired",     4.0f, { 0.0f, 0.0f, 0.0f, 0.45f } },
    /* SP_LEVEL   */ { "level",     "sp_level",   "RECOMP_BOT_SP_LEVEL",     "SpLevel",     "fired",     4.0f, { 0.0f, 0.0f, 0.0f, 0.50f } },
    /* SP_STOMP   */ { "stomp",     "sp_stomp",   "RECOMP_BOT_SP_STOMP",     "SpStomp",     "fired",     2.0f, { 0.0f, 0.0f, 0.0f, 0.60f } },
    /* SP_HACKCOVER */ { "hackcover", "sp_hackcover", "RECOMP_BOT_SP_HACKCOVER", "SpHackCover", "fired", 3.0f, { 0.0f, 0.20f, 0.35f, 0.50f } },
    /* SP_PEELOFF */ { "peeloff",   "sp_peeloff", "RECOMP_BOT_SP_PEELOFF",   "SpPeelOff",   "fired",     3.0f, { 0.0f, 0.15f, 0.30f, 0.45f } },
};
static const char *const k_sp_sk[4] = { "recruit", "regular", "veteran", "elite" };
static char g_sp_fname[SP_N][4][32];

/* ---- state, reset at every match start ---- */
enum { SP_BOTS = 32, SP_PL = MA2_WORLD_MAX_PLAYERS, SP_BUD = 12, SP_EV = 32, SP_KEV = 16 };
static double g_sp_cool[SP_N][SP_BOTS], g_sp_hold[SP_N][SP_BOTS];
static struct { uint32_t victim; double t[2]; } g_sp_bud[SP_N][SP_BUD];
static struct { uint32_t victim; int trick; double t; } g_sp_last[SP_PL + 8];
static int g_sp_sw[SP_N], g_sp_master, g_sp_inited;
static unsigned g_sp_forcemask; static int g_sp_forceskill = -2, g_sp_dk = -1;
static const char *g_sp_why = "";
/* world snapshot kept between frames (scoreboard deltas, last live position, who is human) */
typedef struct SpPl { uint32_t player, actor; int kills, deaths, nation, human, alive; Ma2Vec3 pos; } SpPl;
static SpPl g_pl[SP_PL]; static int g_pl_n, g_sp_team; static double g_sp_last_time = -1.0;
typedef struct SpKill { double t; int slot, used; } SpKill;
static SpKill g_kev[SP_KEV];
typedef struct SpDeath {
    uint32_t rseq;            /* 0 = empty or still waiting for its killer */
    int used; double t;
    int vslot, kslot;         /* kslot -1 = killer unknown */
    uint32_t vplayer, kplayer, vactor, kactor;
    int vnation, knation, vhuman, khuman;
    Ma2Vec3 vpos, kpos;
    int avenger;              /* revenge: slot of the bot that took this one, -1 */
} SpDeath;
static SpDeath g_spd[SP_EV]; static uint32_t g_sp_rseq;

static int sp_env_on(const char *name) { const char *e = getenv(name); return e && *e && *e != '0'; }

static void sp_reset(void)
{
    memset(g_sp_cool, 0, sizeof g_sp_cool); memset(g_sp_hold, 0, sizeof g_sp_hold);
    for (int t = 0; t < SP_N; t++) for (int i = 0; i < SP_BUD; i++) { g_sp_bud[t][i].victim = 0; g_sp_bud[t][i].t[0] = g_sp_bud[t][i].t[1] = -1e9; }
    memset(g_sp_last, 0, sizeof g_sp_last); memset(g_pl, 0, sizeof g_pl); memset(g_kev, 0, sizeof g_kev); memset(g_spd, 0, sizeof g_spd);
    g_sp_rseq = 0; g_pl_n = 0; g_sp_last_time = -1.0;
    for (int t = 0; t < SP_N; t++) g_sp_sw[t] = -1;
    g_sp_master = -1;
    if (!g_sp_inited) {
        g_sp_inited = 1;
        for (int t = 0; t < SP_N; t++) for (int s = 0; s < 4; s++)
            snprintf(g_sp_fname[t][s], sizeof g_sp_fname[t][s], "%s_%s", g_spdef[t].fired ? g_spdef[t].fired : "fired", k_sp_sk[s]);
    }
    g_sp_forcemask = 0;
    {   /* RECOMP_BOT_SP_FORCE=revenge,grudge | rookie | all */
        const char *e = getenv("RECOMP_BOT_SP_FORCE");
        if (e && *e) {
            char buf[160]; char *tok;
            snprintf(buf, sizeof buf, "%s", e);
            for (tok = strtok(buf, ", ;"); tok; tok = strtok(NULL, ", ;")) {
                for (int t = 0; t < SP_N; t++)
                    if (!strcmp(tok, "all") || !strcmp(tok, g_spdef[t].name) || (!strcmp(tok, "rookie") && t >= SP_RK_A && t <= SP_RK_F)) g_sp_forcemask |= 1u << t;
            }
        }
    }
    g_sp_forceskill = -2; g_sp_dk = -1;
}
void ma2_bot_sp_match_begin(void) { sp_reset(); }

static int sp_force_skill(int skill)
{
    if (g_sp_forceskill == -2) { const char *e = getenv("RECOMP_BOT_SP_SKILL"); g_sp_forceskill = e && *e ? atoi(e) & 3 : -1; }
    return g_sp_forceskill >= 0 ? g_sp_forceskill : skill;
}
static int sp_diag_killer(void) { if (g_sp_dk < 0) g_sp_dk = sp_env_on("RECOMP_BOT_SP_DIAG_KILLER"); return g_sp_dk; }
static int sp_force(int trick) { return (g_sp_forcemask >> trick) & 1u; }

static int sp_master_on(void)
{
    if (g_sp_master < 0) g_sp_master = c7_on("sp_all", "RECOMP_BOT_SURPRISE", "Surprises", 1) ? 1 : 0;
    return g_sp_master;
}
static int sp_on(int trick)
{
    if (!sp_master_on()) return 0;
    if (g_sp_sw[trick] < 0) {
        const SpDef *d = &g_spdef[trick];
        int on = c7_on(d->piece, d->env, d->ini, 1) != 0;
        if (on && trick >= SP_RK_A && trick <= SP_RK_F) {
            char nm[40]; const char *e;
            snprintf(nm, sizeof nm, "RECOMP_BOT_SP_ROOKIE_%c", 'A' + (trick - SP_RK_A));
            e = getenv(nm);
            if (e && *e == '0') on = 0;
        }
        g_sp_sw[trick] = on;
    }
    return g_sp_sw[trick];
}

static void sp_set_params(int trick, float cool_min, float p0, float p1, float p2, float p3)
{
    if (trick < 0 || trick >= SP_N) return;
    g_spdef[trick].cool_min = cool_min; g_spdef[trick].p[0] = p0; g_spdef[trick].p[1] = p1; g_spdef[trick].p[2] = p2; g_spdef[trick].p[3] = p3;
}
static void sp_note(const char *why) { g_sp_why = why ? why : ""; }

static int sp_player_human(uint32_t player)
{
    for (int i = 0; i < g_pl_n; i++) if (g_pl[i].player == player) return g_pl[i].human;
    return 0;
}
static int sp_slot_of(uint32_t player)
{
    for (int i = 0; i < g_pl_n; i++) if (g_pl[i].player == player) return i;
    return -1;
}

static int sp_budget_ok(int tr, uint32_t victim, double now)
{
    for (int i = 0; i < SP_BUD; i++)
        if (g_sp_bud[tr][i].victim == victim)
            return !(g_sp_bud[tr][i].t[0] > now - 600.0 && g_sp_bud[tr][i].t[1] > now - 600.0);
    return 1;
}
static void sp_budget_add(int tr, uint32_t victim, double now)
{
    int slot = -1, old = 0;
    for (int i = 0; i < SP_BUD; i++) {
        if (g_sp_bud[tr][i].victim == victim) { slot = i; break; }
        if (g_sp_bud[tr][i].t[1] < g_sp_bud[tr][old].t[1]) old = i;
    }
    if (slot < 0) { slot = old; g_sp_bud[tr][slot].victim = victim; g_sp_bud[tr][slot].t[0] = g_sp_bud[tr][slot].t[1] = -1e9; }
    g_sp_bud[tr][slot].t[0] = g_sp_bud[tr][slot].t[1]; g_sp_bud[tr][slot].t[1] = now;
}

/* The gate. Returns 1 only when every rarity gate passes; the use is then recorded. victim_player = the player the
 * trick is aimed at (the human or bot target, or a side key for tricks without one: 0xFFFF0000 | nation). */
static int sp_roll_p(Ma2Brain *b, int tr, uint32_t victim, float po)
{
    const SpDef *d;
    int sk = b->skill & 3, bi = b->index & (SP_BOTS - 1), force, human;
    double now = b->now;
    float p;
    if (tr < 0 || tr >= SP_N || !sp_on(tr)) return 0;
    d = &g_spdef[tr]; force = sp_force(tr);
    p = po >= 0.0f ? po : d->p[sk];
    if (p <= 0.0f && !force) return 0;                         /* a skill that never does this trick: silent */
    if (now < g_sp_hold[tr][bi]) return 0;                     /* rolled less than 20 s ago */
    g_sp_hold[tr][bi] = now + (force ? 3.0 : 20.0);
    if (!force) {
        if (now < g_sp_cool[tr][bi]) { c7_add("sp_all", "blocked_cool", 1); return 0; }
        c7_add("sp_all", "rolls", 1);
        if (rng_f(&b->rng) >= p) return 0;
        if (!sp_budget_ok(tr, victim, now)) { c7_add("sp_all", "blocked_budget", 1); return 0; }
        for (int i = 0; i < SP_PL + 8; i++)
            if (g_sp_last[i].victim == victim && g_sp_last[i].t > 0.0) {
                if (g_sp_last[i].trick == tr && (victim & 0xFFFF0000u) != 0xFFFF0000u) { c7_add("sp_all", "blocked_repeat", 1); return 0; }
                if (now - g_sp_last[i].t < 45.0) { c7_add("sp_all", "blocked_spacing", 1); return 0; }
            }
    }
    g_sp_cool[tr][bi] = now + (double)d->cool_min * 60.0 * (0.7 + 0.6 * (double)rng_f(&b->rng));
    sp_budget_add(tr, victim, now);
    {   /* last trick per victim */
        int slot = -1, old = 0;
        for (int i = 0; i < SP_PL + 8; i++) {
            if (g_sp_last[i].victim == victim) { slot = i; break; }
            if (g_sp_last[i].t < g_sp_last[old].t) old = i;
        }
        if (slot < 0) slot = old;
        g_sp_last[slot].victim = victim; g_sp_last[slot].trick = tr; g_sp_last[slot].t = now;
    }
    human = sp_player_human(victim);
    if (d->fired) c7_add(d->piece, g_sp_fname[tr][sk], 1);   /* (mood counts hot_/cold_ itself) */
    if (human) c7_add("sp_all", "victim_human", 1);
    if (diag_on()) fprintf(stderr, "[SURPRISE] t=%.1f bot=%d sk=%d trick=%s victim=%08X%s why=%s\n", now, b->index, sk, d->name, victim, human ? " (HUMAN)" : "", g_sp_why);
    g_sp_why = "";
    return 1;
}
static int sp_roll(Ma2Brain *b, int trick, uint32_t victim_player) { return sp_roll_p(b, trick, victim_player, -1.0f); }

/* ---- the death + killer table ---- */
static int sp_enemy_slots(int a, int b2)
{
    if (a < 0 || b2 < 0 || a == b2) return 0;
    return g_sp_team ? g_pl[a].nation != g_pl[b2].nation : 1;
}
static void sp_death_resolve(SpDeath *e, int kslot)
{
    e->kslot = kslot;
    if (kslot >= 0) {
        e->kplayer = g_pl[kslot].player; e->kactor = g_pl[kslot].actor; e->knation = g_pl[kslot].nation; e->khuman = g_pl[kslot].human; e->kpos = g_pl[kslot].pos;
    }
    e->rseq = ++g_sp_rseq; e->avenger = -1;
    if (sp_diag_killer())
        fprintf(stderr, "[SP-KILL] t=%.1f victim=%d%s killer=%d%s at %.0f,%.0f\n", e->t, e->vslot, e->vhuman ? "(HUMAN)" : "", kslot, kslot >= 0 && e->khuman ? "(HUMAN)" : "", e->vpos.x, e->vpos.z);
}
/* Once a frame (the first brain to run in it): scoreboard deltas -> kill events, death events, killer match. */
static void sp_world_tick(const Ma2World *w)
{
    int n = w->nplayers < SP_PL ? w->nplayers : SP_PL;
    if (w->time == g_sp_last_time) return;
    if (w->time < g_sp_last_time - 1.0) sp_reset();
    g_sp_last_time = w->time; g_sp_team = w->team_mode;
    for (int i = 0; i < n; i++) {
        const Ma2PlayerView *p = &w->players[i];
        SpPl *q = &g_pl[i];
        if (q->player && q->player == p->player) {
            for (int k = q->kills; k < p->kills && k < q->kills + SP_KEV; k++) {   /* (merge safety: a score jump must not spin this) */ SpKill *e = &g_kev[0]; for (int j = 0; j < SP_KEV; j++) if (g_kev[j].t < e->t) e = &g_kev[j]; e->t = w->time; e->slot = i; e->used = 0; }
            if (p->deaths > q->deaths) {
                SpDeath *e = &g_spd[0];
                for (int j = 0; j < SP_EV; j++) if (!g_spd[j].used || g_spd[j].t < e->t) { e = &g_spd[j]; if (!g_spd[j].used) break; }
                memset(e, 0, sizeof *e);
                e->used = 1; e->t = w->time; e->vslot = i; e->kslot = -1; e->vplayer = p->player; e->vactor = q->actor; e->vnation = p->nation; e->vhuman = p->is_human;
                e->vpos = q->pos; e->avenger = -1;
            }
        }
    }
    for (int i = 0; i < n; i++) {   /* refresh the snapshot (after the deltas used the old values) */
        const Ma2PlayerView *p = &w->players[i];
        SpPl *q = &g_pl[i];
        q->player = p->player; q->kills = p->kills; q->deaths = p->deaths; q->nation = p->nation; q->human = p->is_human; q->alive = p->alive;
        if (p->alive && p->actor && (fabsf(p->pos.x) + fabsf(p->pos.z)) > 1.0f) { q->pos = p->pos; q->actor = p->actor; }
    }
    g_pl_n = n;
    for (int j = 0; j < SP_EV; j++) {   /* match each waiting death with an unused enemy kill within 1 s */
        SpDeath *e = &g_spd[j];
        int best = -1; double bd = 1e18;
        if (!e->used || e->rseq) continue;
        for (int k = 0; k < SP_KEV; k++) {
            SpKill *kv = &g_kev[k];
            double d2;
            if (kv->used || kv->t <= 0.0 || fabs(kv->t - e->t) > 1.0 || !sp_enemy_slots(kv->slot, e->vslot)) continue;
            d2 = (double)(g_pl[kv->slot].pos.x - e->vpos.x) * (g_pl[kv->slot].pos.x - e->vpos.x) + (double)(g_pl[kv->slot].pos.z - e->vpos.z) * (g_pl[kv->slot].pos.z - e->vpos.z);
            if (d2 < bd) { bd = d2; best = k; }
        }
        if (best >= 0) { g_kev[best].used = 1; sp_death_resolve(e, g_kev[best].slot); }
        else if (w->time - e->t >= 0.4) sp_death_resolve(e, -1);
    }
}

/* F27 knowledge: what this bot last learnt of a player (an injected sighting never replaces a fresher one). */
static void sp_know(Ma2Brain *b, uint32_t player, uint32_t actor, Ma2Vec3 pos, double t)
{
    int slot = -1, old = 0;
    for (int j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
        if (b->kn[j].player == player) { slot = j; break; }
        if (b->kn[j].t < b->kn[old].t) old = j;
    }
    if (slot < 0) slot = old;
    if (b->kn[slot].player == player && b->kn[slot].actor == actor && b->kn[slot].t >= t) return;
    b->kn[slot].player = player; b->kn[slot].actor = actor; b->kn[slot].pos = pos; b->kn[slot].t = t;
}

/* ---------------------------------------------------------------------------------------------
 * 12. Revenge on a teammate's killer. A teammate dies within 80 m in front of us (its last position inside
 * our view and in the clear), the killer is known (scoreboard kill within 1 s) and alive: each witness rolls,
 * the first to pass is the avenger (one per death). For up to 30 s the killer's target score is x1.3..2.0 by
 * skill and the hunt heads for where the death happened (F27: a spot we learnt, never his live position).
 * Not while on an objective duty, on foot (pilots fetch a ride), or in a vehicle trip.
 * --------------------------------------------------------------------------------------------- */
static const float k_rv_mult[4] = { 1.3f, 1.5f, 1.8f, 2.0f };
static void sp_revenge_witness(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, SpDeath *e)
{
    SpBrain *s = &b->sp;
    float d, ang;
    int k = kind_ok(me->kind);
    if (!w->team_mode || e->kslot < 0 || e->vslot == (int)(me - w->players) || e->vnation != me->nation || e->knation == me->nation) return;
    if (e->avenger >= 0 || s->rv_until > b->now || !me->alive || b->now - e->t > 3.0 || !sp_on(SP_REVENGE)) return;
    if (!g_pl[e->kslot].alive || g_pl[e->kslot].player != e->kplayer) return;               /* the killer is already dead (or gone) */
    if (k == MA2_KIND_PILOT || k == MA2_KIND_NONE || b->obj_lock || b->goal == G_GET_VEHICLE) return;
    d = hdist(me->pos, e->vpos);
    if (d > 80.0f) return;
    ang = fabsf(wrap_pi(heading_of(vsub(e->vpos, me->pos)) - me->yaw));
    if (ang > b->sk.fov_deg * MA2B_DEG && !sp_force(SP_REVENGE)) return;
    if (sn && sn->los && !sp_force(SP_REVENGE)) {
        Ma2Vec3 from = eye_of(me->pos, me->kind, 1.0f), to = e->vpos; to.y += 3.0f;
        if (ray(b, sn, me->actor, e->vactor, from, to) == 0) return;
    }
    c7_add("sp_revenge", "witnessed", 1);
    sp_note("teammate killed in sight");
    if (!sp_roll(b, SP_REVENGE, e->kplayer)) return;
    e->avenger = (int)(me - w->players);
    s->rv_player = e->kplayer; s->rv_t0 = b->now; s->rv_until = b->now + 30.0; s->rv_pos = e->vpos; s->rv_found = 0;
    sp_know(b, e->kplayer, e->kactor, e->vpos, e->t);
    b->next_eval = 0.0;
}

/* ---------------------------------------------------------------------------------------------
 * 1. Grudges. The same enemy kills us twice within 5 min: (rolled) we remember him; for 90 s after the next
 * spawn his target score is x1.3..1.9 by skill and the hunt pick weighs him x2. Ends when we kill him, die
 * again, or after 90 s. No effect on objective duty or a vehicle trip (sp_target_bias).
 * --------------------------------------------------------------------------------------------- */
static const float k_gr_mult[4] = { 1.3f, 1.5f, 1.7f, 1.9f };
static void sp_grudge_note(Ma2Brain *b, const SpDeath *e)
{
    SpBrain *s = &b->sp;
    int again = 0, slot = -1, old = 0;
    if (e->kslot < 0 || !e->kplayer || !sp_on(SP_GRUDGE)) return;
    for (int i = 0; i < 6; i++) {
        if (s->kd[i].player == e->kplayer) { slot = i; if (s->kd[i].t > 0.0 && e->t - s->kd[i].t <= 300.0) again = 1; }
        if (s->kd[i].t < s->kd[old].t) old = i;
    }
    if (slot < 0) slot = old;
    s->kd[slot].player = e->kplayer; s->kd[slot].t = e->t;
    if (!again && !sp_force(SP_GRUDGE)) return;
    c7_add("sp_grudge", "armed", 1);
    sp_note("killed twice within 5 min");
    if (sp_roll(b, SP_GRUDGE, e->kplayer)) { s->gr_arm = 1; s->gr_player = e->kplayer; sp_know(b, e->kplayer, e->kactor, e->kpos, e->t); }
}
static void sp_grudge_end(Ma2Brain *b, const char *counter)
{
    SpBrain *s = &b->sp;
    if (s->gr_t0 > 0.0) { c7_add("sp_grudge", counter, 1); c7_add("sp_grudge", "grudge_s_sum", minf(60.0f, (float)(b->now - s->gr_t0))); }
    s->gr_until = 0.0; s->gr_t0 = 0.0; s->gr_arm = 0;
}

/* ---------------------------------------------------------------------------------------------
 * 2. Momentum / streak mood. 3 kills without dying: HOT; 3 deaths without a kill: COLD (rolled; the side's
 * budget). 120 s, or HOT until the next death / COLD until the next kill. Decision weights only:
 *   HOT:  aggr +0.25, retreat line x0.8, flank chance x1.5, standoff band -15 %
 *   COLD: caution +0.3, retreat line x1.2, takes cover earlier when hurt and shot at (evaluate)
 * --------------------------------------------------------------------------------------------- */
static const char *const k_mood_hot[4] = { "hot_recruit", "hot_regular", "hot_veteran", "hot_elite" };
static const char *const k_mood_cold[4] = { "cold_recruit", "cold_regular", "cold_veteran", "cold_elite" };
static void sp_mood_end(Ma2Brain *b)
{
    SpBrain *s = &b->sp;
    if (!s->mood) return;
    if (s->mood == 1) b->aggr -= s->m_aggr; else b->caution -= s->m_aggr;
    b->sk.retreat_hp -= s->m_ret;
    c7_add("sp_mood", "mood_s_sum", b->now - s->mood_t0);
    s->mood = 0; s->mood_until = 0.0; b->next_eval = 0.0;
}
static void sp_mood_begin(Ma2Brain *b, int kind)
{
    SpBrain *s = &b->sp;
    int sk = b->skill & 3;
    s->mood = kind; s->mood_t0 = b->now; s->mood_until = b->now + 60.0;
    if (kind == 1) { s->m_aggr = minf(0.25f, 1.0f - b->aggr); b->aggr += s->m_aggr; s->m_ret = -0.2f * b->sk.retreat_hp; }
    else { s->m_aggr = 0.3f; b->caution += s->m_aggr; s->m_ret = 0.2f * b->sk.retreat_hp; }
    b->sk.retreat_hp += s->m_ret;
    c7_add("sp_mood", kind == 1 ? k_mood_hot[sk] : k_mood_cold[sk], 1);
    s->ks = s->ds = 0;
    b->next_eval = 0.0;
}
static int sp_streak(void) { static int n = 0; if (!n) { const char *e = getenv("RECOMP_BOT_SP_MOOD_STREAK"); n = e && atoi(e) > 0 ? atoi(e) : 3; } return n; }   /* test hook: streak length (default 3) */
static float sp_mood_flank(const Ma2Brain *b) { return b->sp.mood == 1 ? 1.5f : 1.0f; }

/* ---------------------------------------------------------------------------------------------
 * 4. Rookie mistakes (Recruit p 0.35, Regular 0.12; Veteran and Elite never). Short episodes of a classic beginner's
 * error, one at a time per bot, each its own trick (own cooldown, budget, switch RECOMP_BOT_SP_ROOKIE_<A..F>):
 *   A tunnel vision  20 s keep the target picked first, whoever is closer          (choose_target)
 *   B ignore flanks  15 s a hit from outside the view is not turned to              (look_start)
 *   C straight lines 20 s fight moves go straight at the target: no strafe/dodge/peek (order flag, ctrl_fight)
 *   D out of range    8 s the guns fire at targets up to 1.4x their range           (order flag, ctrl_fire)
 *   E VTOL chase     15 s a ground body goes after a visible enemy VTOL             (choose_target)
 *   F last shooter   20 s every hit re-targets the shooter at once                  (choose_target)
 * Ends early on a heavy hit (B: 25 % of health lost), a retreat / cover / objective goal, or the target gone.
 * --------------------------------------------------------------------------------------------- */
static void sp_rk_end(Ma2Brain *b, const char *why)
{
    SpBrain *s = &b->sp;
    if (!s->rk) return;
    c7_add("sp_rookie", "mistake_s_sum", b->now - s->rk_t0);
    c7_add("sp_rookie", why, 1);
    s->rk = 0; s->rk_until = 0.0;
    b->next_eval = 0.0;
}
static void sp_rk_begin(Ma2Brain *b, int tr, double secs, uint32_t tgt_actor, uint32_t tgt_player)
{
    SpBrain *s = &b->sp;
    s->rk = tr; s->rk_t0 = b->now; s->rk_until = b->now + secs; s->rk_tgt = tgt_actor; s->rk_pl = tgt_player; s->rk_hp0 = s->hpf;
    b->next_eval = 0.0;
}
static int sp_rk_skill_ok(const Ma2Brain *b) { return b->skill <= MA2_SKILL_REGULAR || g_sp_forcemask; }

/* look_start: a hit from outside the view. 1 = the turn to look is skipped (a mistake is on, or starts now). */
static int sp_rk_noflank(Ma2Brain *b, const struct Mem *m, int why)
{
    SpBrain *s = &b->sp;
    if (!sp_master_on() || !sp_rk_skill_ok(b)) return 0;
    if (s->rk == SP_RK_B && b->now < s->rk_until) return 1;
    if (why != 2 || s->rk) return 0;
    sp_note("hit from behind");
    if (sp_roll(b, SP_RK_B, m->player)) { sp_rk_begin(b, SP_RK_B, 10.0, m->actor, m->player); return 1; }
    return 0;
}
/* choose_target: after the pick (and the carrier rule). Returns the pick to use. */
static Mem *sp_rk_target(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Mem *pick, Mem *cur)
{
    SpBrain *s = &b->sp;
    (void)w;
    if (!sp_master_on() || !sp_rk_skill_ok(b)) return pick;
    if (s->rk == SP_RK_A || s->rk == SP_RK_E || s->rk == SP_RK_F) {
        Mem *t = mem_by_actor(b, s->rk == SP_RK_F ? s->last_shooter : s->rk_tgt);
        if (b->now >= s->rk_until) { sp_rk_end(b, "ended_time"); return pick; }
        if (!t || !mem_engageable(b, t) || (s->rk == SP_RK_A && !t->visible && b->now - t->last_seen > 5.0)) {
            if (s->rk != SP_RK_F) { sp_rk_end(b, "ended_target"); return pick; }
            return pick;                                                  /* (F: no shooter to follow right now) */
        }
        if (pick && obj_bonus(w, me, pick->player, b->skill) > 1.0f) return pick;   /* a flag carrier still draws the guns */
        return t;
    }
    if (!s->rk && pick && cur != pick) {      /* A: a new first pick while another enemy is nearer */
        float dp = hdist(me->pos, pick->pos);
        for (int i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
            const Mem *m = &b->mem[i];
            if (!m->valid || m == pick || !mem_engageable(b, m) || k_kind[m->kind].dps <= 0.0f) continue;
            if (hdist(me->pos, m->pos) < dp - 10.0f) {
                sp_note("picked a target with another enemy nearer");
                if (sp_roll(b, SP_RK_A, pick->player)) sp_rk_begin(b, SP_RK_A, 12.0, pick->actor, pick->player);
                break;
            }
        }
    }
    return pick;
}
/* peek_search: C (straight lines) takes no peeks */
static int sp_rk_nopeek(const Ma2Brain *b) { return b->sp.rk == SP_RK_C && b->now < b->sp.rk_until; }
/* end of brain_tick_body: flags the controller reads */
static void sp_rk_order(Ma2Brain *b, Ma2Order *out)
{
    SpBrain *s = &b->sp;
    if (!s->rk || b->now >= s->rk_until) return;
    if (s->rk == SP_RK_C) { out->sp_straight = 1; out->dodge = 0; out->dodge_now = 0; b->dodge_next = b->now + 5.0; }
    else if (s->rk == SP_RK_D) out->sp_far = 1;
}
/* once a tick: end conditions, then the triggers that are not in choose_target / look_start */
static void sp_rk_tick(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, int alive)
{
    SpBrain *s = &b->sp;
    int k = kind_ok(me->kind);
    (void)sn;
    if (!alive) { if (s->rk) { c7_add("sp_rookie", "died_during", 1); sp_rk_end(b, "ended_death"); } return; }
    s->hpf = hp_frac(me->hp, me->hp_max);
    /* the shooter: whoever hit us in the last half second (F65 keeps hit_me_t) */
    {
        double best = b->now - 0.5; uint32_t who = 0;
        for (int i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) if (b->mem[i].valid && b->mem[i].hit_me_t > best) { best = b->mem[i].hit_me_t; who = b->mem[i].actor; }
        if (who && who != s->last_shooter) {
            s->last_shooter = who; s->last_shot_t = best;
            if (s->rk == SP_RK_F) b->next_eval = 0.0;                    /* F: re-target at once */
        }
    }
    if (s->rk) {
        int bad = b->goal == G_RETREAT || b->goal == G_COVER || b->goal == G_OBJECTIVE || b->goal == G_EJECT || b->goal == G_DEFEND_HACK || b->goal == G_GET_VEHICLE;
        if (b->now >= s->rk_until) sp_rk_end(b, "ended_time");
        else if (s->rk == SP_RK_B ? s->hpf < s->rk_hp0 - 0.25f : b->dmg_recent > 0.15f) sp_rk_end(b, "ended_interrupt");
        else if (bad && s->rk != SP_RK_B) sp_rk_end(b, "ended_interrupt");
        else if ((s->rk == SP_RK_C || s->rk == SP_RK_D) && (!b->tgt || !b->tgt->visible)) sp_rk_end(b, "ended_target");
        return;
    }
    if (!sp_rk_skill_ok(b)) return;
    {
        Mem *t = b->tgt;
        int ground = k == MA2_KIND_MECH || k == MA2_KIND_BATTLEARMOR || k == MA2_KIND_TANK;
        if (t && t->visible && b->goal == G_ATTACK && ground) {
            float d = hdist(me->pos, t->pos), reach = my_reach(me);
            if (d < 0.9f * reach && k != MA2_KIND_TANK) { sp_note("in a fight"); if (sp_roll(b, SP_RK_C, t->player)) { sp_rk_begin(b, SP_RK_C, 12.0, t->actor, t->player); return; } }
            float rs = me->reach_short > 10.0f ? minf(me->reach_short, reach) : reach;   /* the shortest gun group: the one a rookie wastes shots with */
            if (d > rs && d <= 1.4f * rs) { sp_note("target just out of range"); if (sp_roll(b, SP_RK_D, t->player)) { sp_rk_begin(b, SP_RK_D, 8.0, t->actor, t->player); return; } }
        }
        if (ground && (!t || t->kind != MA2_KIND_VTOL) && b->goal != G_OBJECTIVE && !b->obj_lock) {
            for (int i = 0; i < MA2_WORLD_MAX_PLAYERS; i++) {
                const Mem *m = &b->mem[i];
                if (!m->valid || !m->visible || m->kind != MA2_KIND_VTOL || hdist(me->pos, m->pos) > 250.0f) continue;
                sp_note("a VTOL in sight");
                if (sp_roll(b, SP_RK_E, m->player)) { sp_rk_begin(b, SP_RK_E, 10.0, m->actor, m->player); return; }
                break;
            }
        }
        if (s->last_shooter && b->now - s->last_shot_t < 0.6 && b->target && s->last_shooter != b->target && ground) {
            const Mem *sh = mem_by_actor(b, s->last_shooter);
            if (sh) { sp_note("hit by someone else"); if (sp_roll(b, SP_RK_F, sh->player)) { sp_rk_begin(b, SP_RK_F, 12.0, sh->actor, sh->player); return; } }
        }
    }
    (void)w;
}

/* Per bot, every tick (before the dead-bot early-out). */
static void sp_brain_tick(Ma2Brain *b, const Ma2World *w, int self, const Ma2PlayerView *me, const Ma2Senses *sn)
{
    SpBrain *s = &b->sp;
    double t0;
    int alive, dk = 0, dd = 0;
    if (!sp_master_on()) return;
    t0 = c7_t0();
    sp_world_tick(w);
    alive = me->alive && me->actor && me->kind != MA2_KIND_NONE;
    if (!s->stats_ok) { s->stats_ok = 1; s->ev_cur = g_sp_rseq; s->kills0 = me->kills; s->deaths0 = me->deaths; s->was_alive = alive; }   /* (old events are not replayed to a new brain) */
    if (alive) c7_add("sp_all", "bot_s", b->dt);
    /* scoreboard: streaks (mood) */
    if (me->kills > s->kills0) { dk = me->kills - s->kills0; s->ks += dk; s->ds = 0; }
    if (me->deaths > s->deaths0) { dd = me->deaths - s->deaths0; s->ds += dd; s->ks = 0; }
    s->kills0 = me->kills; s->deaths0 = me->deaths;
    if (dk) {
        c7_add("sp_mood", s->mood == 1 ? "hot_kills" : s->mood == 2 ? "cold_kills" : "base_kills", dk);
        if (s->gr_until > b->now) c7_add("sp_grudge", "grudge_kills", dk);
    }
    if (dd) c7_add("sp_mood", s->mood == 1 ? "hot_deaths" : s->mood == 2 ? "cold_deaths" : "base_deaths", dd);
    if (s->was_alive && !alive) {                                          /* we just died */
        if (s->mood == 1) sp_mood_end(b);
        s->mood_try = s->mood_try == 2 ? 2 : 0;
        if (s->gr_until > b->now) sp_grudge_end(b, "expired");
        s->gr_arm = 0;
    }
    if (alive && !s->was_alive && s->gr_arm) { s->gr_arm = 0; s->gr_until = b->now + 60.0; s->gr_t0 = b->now; }   /* next spawn */
    /* death events (resolved: killer known or not) */
    for (int j = 0; j < SP_EV; j++) {
        SpDeath *e = &g_spd[j];
        if (!e->used || !e->rseq || e->rseq <= s->ev_cur) continue;
        if (e->vslot == self) {                                             /* we died */
            if (s->rv_until > b->now) { c7_add("sp_revenge", "avenger_died", 1); s->rv_until = 0.0; }
            sp_grudge_note(b, e);
        } else {
            if (alive && sp_on(SP_REVENGE)) sp_revenge_witness(b, w, me, sn, e);
            if (s->rv_until > b->now && e->vplayer == s->rv_player && e->knation == me->nation && e->kslot >= 0) {   /* the killer fell: by us or a teammate */
                c7_add("sp_revenge", "avenged", 1); s->rv_until = 0.0;
            }
            if (s->gr_until > b->now && e->vplayer == s->gr_player && e->kslot == self) sp_grudge_end(b, "paid_back");
        }
    }
    for (int j = 0; j < SP_EV; j++) if (g_spd[j].used && g_spd[j].rseq > s->ev_cur) s->ev_cur = g_spd[j].rseq;
    /* revenge running: did we find him? */
    if (s->rv_until > b->now) {
        if (!alive) { s->rv_until = 0.0; }
        else if (!s->rv_found && b->now - s->rv_t0 <= 15.0) {
            const struct Mem *m = mem_get(b, s->rv_player, 0);
            if (m && m->visible) { s->rv_found = 1; c7_add("sp_revenge", "found", 1); }
        }
    }
    if (s->rv_t0 > 0.0 && s->rv_until <= b->now) { c7_add("sp_revenge", "rv_s_sum", minf(30.0f, (float)(b->now - s->rv_t0))); s->rv_t0 = 0.0; }
    /* grudge expiry */
    if (s->gr_t0 > 0.0 && s->gr_until <= b->now) sp_grudge_end(b, "expired");
    /* mood: end, then start (a streak reached) */
    if (s->mood && (b->now >= s->mood_until || (s->mood == 2 && dk))) sp_mood_end(b);
    /* one roll per streak: the kill / death that makes it (a COLD roll waits for the respawn) */
    if (dk && s->ks == sp_streak()) s->mood_try = 1;
    if (dd && s->ds == sp_streak()) s->mood_try = 2;
    if (s->mood_try && alive && !s->mood && sp_on(SP_MOOD)) {
        int kind = s->mood_try;
        s->mood_try = 0;
        sp_note(kind == 1 ? "kill streak" : "death streak");
        if (sp_roll(b, SP_MOOD, 0xFFFF0100u | (uint32_t)me->nation)) sp_mood_begin(b, kind);
    }
    sp_rk_tick(b, w, me, sn, alive);
    /* time accounting */
    if (alive) {
        if (s->mood == 1) { c7_add("sp_mood", "hot_s", b->dt); if (b->goal == G_ATTACK) c7_add("sp_mood", "hot_attack_s", b->dt); }
        else if (s->mood == 2) { c7_add("sp_mood", "cold_s", b->dt); if (b->goal == G_COVER || b->goal == G_RETREAT) c7_add("sp_mood", "cold_cover_s", b->dt); }
        else { c7_add("sp_mood", "base_s", b->dt); if (b->goal == G_ATTACK) c7_add("sp_mood", "base_attack_s", b->dt); else if (b->goal == G_COVER || b->goal == G_RETREAT) c7_add("sp_mood", "base_cover_s", b->dt); }
        if (s->gr_until > b->now) c7_add("sp_grudge", "grudge_alive_s", b->dt);
        if (s->rv_until > b->now || s->gr_until > b->now || s->mood || s->rk) c7_add("sp_all", "trick_s", b->dt);
    }
    s->any = (s->rv_until > b->now) || (s->gr_until > b->now);
    s->was_alive = alive;
    c7_ms("sp_all", "tick", t0);
}

/* choose_target: x score for this enemy while a revenge / grudge is on it */
static float sp_target_bias(Ma2Brain *b, uint32_t player)
{
    SpBrain *s = &b->sp;
    float m = 1.0f;
    int sk = b->skill & 3;
    if (!s->any || !player || b->obj_lock || b->goal == G_GET_VEHICLE) return 1.0f;
    if (s->rv_until > b->now && player == s->rv_player) m *= k_rv_mult[sk];
    if (s->gr_until > b->now && player == s->gr_player) m *= k_gr_mult[sk];
    return m;
}
/* hunt pick: x weight (a revenge heads for the spot the death happened; a grudge is x2) */
static float sp_hunt_mult(Ma2Brain *b, uint32_t player)
{
    SpBrain *s = &b->sp;
    float m = 1.0f;
    if (!s->any || !player || b->obj_lock) return 1.0f;
    if (s->rv_until > b->now && player == s->rv_player) m *= 8.0f;
    if (s->gr_until > b->now && player == s->gr_player) m *= 2.0f;
    return m;
}
