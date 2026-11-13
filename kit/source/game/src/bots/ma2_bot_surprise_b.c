/* ma2_bot_surprise_b.c - positioning surprises (surprise set B, SURPRISE-SPEC v1 sections 5, 9, 6):
 *   5  Veteran baiting       (SP_BAIT)    a hurt / out-gunned Veteran or Elite falls back toward a teammate or a corner
 *                                         with the chaser behind it, firing at it, and the chaser runs into a crossfire.
 *   9  Crossfire / pincer    (SP_PINCER)  two Veterans / Elites on one target split and come at it ~90 deg apart.
 *   6  Elite power-up timing + denial (SP_POWERUP): 6a waits (10-20 m off) for a power-up it knows is about to return;
 *                                         6b holds a spot with sight of a pickup a hurt enemy is walking onto.
 * #included by ma2_bot_brain.c just before brain_tick_body (after every helper it uses). Does NOT edit the shared module
 * (ma2_bot_surprise.c / .h): it uses sp_roll_p / sp_on / sp_note and reads the module's cooldown table, and keeps all of
 * its own state in static data here. Entry points (one line each in the brain):
 *   sp2_dead(b)                          dead bot: end its tricks (counters)
 *   sp2_tick(b,w,me,nav,sn)              once per live tick before the goal switch: state machines, tac_* fields (bait, pincer)
 *   sp2_order(b,w,me,sn,out)             after the goal switch: the power-up hold orders
 *   sp2_pu_take(b, entity)               evaluate: the power-up this bot was waiting for / denying is back -> take it (1.6)
 * Everything is decisions / position / timing: nothing here touches damage, armour, health or weapon stats (FAIR DAMAGE).
 * [C7] pieces: sp_bait, sp_pincer, sp_powerup (spec) and sp_b (time in tricks, tick cost, registry facts).
 * Test hooks (all default off): RECOMP_BOT_SP_FORCE=bait,pincer,powerup (module), RECOMP_BOT_SP_SKILL=2|3 (module),
 * RECOMP_BOT_PU_RESPAWN_S=<s> (the respawn the bots assume until they have seen a cycle; the real one is learned). */

enum { S2_PU = 24, S2_NAT = 8, S2_PIN = 6, S2_SHD = 8 };

/* ---- power-up registry: when a power-up goes (parked at the map edge = out of the view) and when it is due back ---- */
typedef struct S2Pu {
    uint32_t ent; Ma2Vec3 pos; int armor; int wclass;   /* PuClass */
    int used, in_view, gen;            /* gen: a generator crate (first seen in the first 45 s, or it has come back once) */
    double first_seen, gone_t, ret_t, back_t, near_t[S2_NAT], occ_t;
    int known[S2_NAT];                 /* nation n had a bot within 60 m of it in the 2 s before it went */
    uint32_t taker; int taker_nation;  /* who stood on it when it went (within 8 m), 0 = nobody */
    unsigned ev;                       /* gone-event counter (bots compare it to the one they started under) */
} S2Pu;
static S2Pu g_s2pu[S2_PU];
static double g_s2_resp = -1.0;        /* respawn seconds learned from a full cycle (or the test hook / 30) */

/* ---- one record per bot, written every tick: what the others need to know about it ---- */
typedef struct S2Info { int valid; double t; int nation, skill, kind, carrying, busy; uint32_t actor, tgt, player; Ma2Vec3 pos; float hpf; } S2Info;
static S2Info g_s2i[SP_BOTS];

/* ---- per-bot trick state ---- */
typedef struct S2Bot {
    /* bait */
    int bait;                          /* 0 off, 1 running (steering), 2 monitoring (released, counting sprung) */
    double bt0, bt_follow, bt_prog, bt_next, bt_mon_end, bt_corner_next, bt_dbg; uint32_t b_chaser, b_chaser_pl; Ma2Vec3 b_pt, b_mate; float b_min_d, b_hp0; int b_followed, b_sprung, b_corner;
    /* pincer */
    int pin_rec;                       /* index+1 into g_s2pin while taking part */
    double pin_next, pin_arr_t;
    /* power-up */
    int pu_mode;                       /* 0 off, 1 timed wait, 2 deny hold, 3 take it now */
    int pu_kind;                       /* 1 time, 2 deny */
    int pu_idx; uint32_t pu_ent; Ma2Vec3 pu_pos, pu_spot; unsigned pu_ev; double pu_t0, pu_back_t, pu_next, pu_wait_t, pu_take_t, pu_reach_t; uint32_t pu_victim;
    int pu_arrived, pu_hit, pu_set;
} S2Bot;
static S2Bot g_s2b[SP_BOTS];

typedef struct S2Pin {
    int active, watch;                 /* active: steering; watch: target-killed window still open */
    int a, b;                          /* bot indices (a owns the roll) */
    uint32_t tgt, tgt_pl; Ma2Vec3 tpos0, spot[2]; double t0, until; int arrived[2], both, ended, replans;
    int skills;                        /* pair kind for the counter: 0 vet+vet, 1 vet+elite, 2 elite+elite */
    uint32_t actor[2];
} S2Pin;
static S2Pin g_s2pin[S2_PIN];
static struct { int used; uint32_t tgt; double t0, until; int done; } g_s2shd[S2_SHD];    /* plain gang-up baseline */

static double g_s2_last = -1.0;
static float s2_know_r(void)     /* how close a team bot had to be when a power-up went for the team to know when it is due back (spec: 60 m) */
{
    static float r = -1.0f;
    if (r < 0.0f) { const char *e = getenv("RECOMP_BOT_SP_PU_KNOW_R"); r = e && *e && atof(e) > 0.0 ? (float)atof(e) : 60.0f; }
    return r;
}
static char g_s2_note[96];

/* c7 keeps the counter-name pointer: names built at run time must be interned (static, never on the stack) */
static const char *s2_ended(const char *why)
{
    static char pool[64][32]; static int n;
    char tmp[32]; int i;
    snprintf(tmp, sizeof tmp, "ended_%s", why);
    for (i = 0; i < n; i++) if (!strcmp(pool[i], tmp)) return pool[i];
    if (n >= 64) return "ended_other";
    memcpy(pool[n], tmp, sizeof tmp);
    return pool[n++];
}
static const char *const k_s2_pair[3] = { "fired_vv", "fired_ve", "fired_ee" };

static void s2_reset(void)
{
    memset(g_s2pu, 0, sizeof g_s2pu); memset(g_s2i, 0, sizeof g_s2i); memset(g_s2b, 0, sizeof g_s2b);
    memset(g_s2pin, 0, sizeof g_s2pin); memset(g_s2shd, 0, sizeof g_s2shd);
    g_s2_resp = -1.0;
}
static float s2_resp_default(void)
{
    if (g_s2_resp > 0.0) return (float)g_s2_resp;
    {
        const char *e = getenv("RECOMP_BOT_PU_RESPAWN_S");
        if (e && *e && atof(e) > 0.0) return (float)atof(e);
    }
    return 30.0f;
}
static float s2_ang_diff(float a, float b2)   /* a - b2 wrapped to -pi..pi */
{
    float d = a - b2;
    while (d > MA2B_PI) d -= 2.0f * MA2B_PI;
    while (d < -MA2B_PI) d += 2.0f * MA2B_PI;
    return d;
}
static float s2_speed(int k) { return k == MA2_KIND_BATTLEARMOR ? 6.0f : 8.0f; }
/* The trick's target stays the bot's target for its length: choose_target flips between enemies in a group fight, which ended
 * most first-version tricks within seconds. Applied after choose_target ran (this file never edits choose_target). */
static int s2_hold_target(Ma2Brain *b, uint32_t actor)
{
    Mem *m = mem_by_actor(b, actor);
    if (!m || !m->valid || !mem_engageable(b, m)) return 0;
    if (b->tgt != m) { b->tgt = m; b->target = actor; c7_add("sp_b", "target_held", 1); }
    return 1;
}
static int s2_ground_kind(int k) { return k == MA2_KIND_MECH || k == MA2_KIND_BATTLEARMOR || k == MA2_KIND_TANK; }

/* ---- world tick, once per frame (the first bot to run in it) ---- */
static void s2_world_tick(const Ma2World *w)
{
    int i, j, n;
    if (w->time == g_s2_last) return;
    if (w->time < g_s2_last - 1.0) s2_reset();
    g_s2_last = w->time;
    n = w->nplayers < MA2_WORLD_MAX_PLAYERS ? w->nplayers : MA2_WORLD_MAX_PLAYERS;
    /* power-up registry */
    {
        unsigned char seen[S2_PU]; memset(seen, 0, sizeof seen);
        for (i = 0; i < w->npickups && i < MA2_BOT_MAX_PICKUPS; i++) {
            const Ma2PickupView *pk = &w->pickups[i];
            int e = -1, free_i = -1;
            if (pk->kind != MA2_PICKUP_POWERUP) continue;
            for (j = 0; j < S2_PU; j++) {
                if (!g_s2pu[j].used) { if (free_i < 0) free_i = j; continue; }
                if (g_s2pu[j].ent == pk->entity) { e = j; break; }
            }
            if (e >= 0 && hdist(g_s2pu[e].pos, pk->pos) > 3.0f) {   /* a pooled crate reused for another drop: not the same pickup */
                if (!g_s2pu[e].in_view) c7_add("sp_b", "reused_ent", 1);
                memset(&g_s2pu[e], 0, sizeof g_s2pu[e]); g_s2pu[e].used = 1; g_s2pu[e].ent = pk->entity; g_s2pu[e].first_seen = w->time; g_s2pu[e].in_view = 1; g_s2pu[e].gen = 0;
            }
            if (e < 0) {   /* a new entity: the same generator spot as one that went (a respawn may make a new crate) */
                for (j = 0; j < S2_PU; j++)
                    if (g_s2pu[j].used && !g_s2pu[j].in_view && g_s2pu[j].gone_t > 0.0 && hdist(g_s2pu[j].pos, pk->pos) < 3.0f) { e = j; g_s2pu[j].ent = pk->entity; break; }
            }
            if (e < 0) {
                if (free_i < 0) {   /* table full: reuse the oldest gone one */
                    free_i = 0;
                    for (j = 0; j < S2_PU; j++) if (!g_s2pu[j].in_view && g_s2pu[j].gone_t < g_s2pu[free_i].gone_t) free_i = j;
                }
                e = free_i; memset(&g_s2pu[e], 0, sizeof g_s2pu[e]);
                g_s2pu[e].used = 1; g_s2pu[e].ent = pk->entity; g_s2pu[e].first_seen = w->time; g_s2pu[e].in_view = 1;
                g_s2pu[e].gen = w->time < 45.0f;
            }
            {
                S2Pu *u = &g_s2pu[e];
                seen[e] = 1; u->pos = pk->pos; u->armor = pk->armor; u->wclass = pk->wclass;
                if (!u->in_view) {   /* it is back */
                    double dur = w->time - u->gone_t;
                    u->in_view = 1; u->ret_t = w->time; u->gen = 1;
                    if (dur > 3.0 && dur < 200.0) {
                        int occupied = w->time - u->occ_t < 5.0;
                        if (g_s2_resp < 0.0 || fabs(dur - g_s2_resp) > 0.5) c7_add("sp_b", "resp_learned", 1);
                        g_s2_resp = dur;
                        c7_add("sp_b", "cycles", 1); c7_add("sp_b", occupied ? "cycle_s_occupied" : "cycle_s_free", dur); c7_add("sp_b", occupied ? "cycles_occupied" : "cycles_free", 1);
                        if (diag_on()) fprintf(stderr, "[SP-PU] t=%.1f pu=%08X back at %.0f,%.0f after %.1f s (stood on: %d) resp=%.1f\n", w->time, u->ent, u->pos.x, u->pos.z, dur, occupied, g_s2_resp);
                    }
                }
                for (j = 0; j < n; j++) {
                    const Ma2PlayerView *p = &w->players[j];
                    float d;
                    if (!p->alive || !p->actor) continue;
                    d = hdist(p->pos, pk->pos);
                    if (d < s2_know_r()) u->near_t[p->nation & (S2_NAT - 1)] = w->time;
                    if (d < 10.0f) u->occ_t = w->time;
                }
            }
        }
        for (i = 0; i < S2_PU; i++) {
            S2Pu *u = &g_s2pu[i];
            if (!u->used || !u->in_view || seen[i]) continue;
            /* it just went: who was standing on it, who of each team had seen it */
            u->in_view = 0; u->gone_t = g_s2_last; u->ev++; u->taker = 0; u->taker_nation = -1;
            {
                float bd = 8.0f;
                for (j = 0; j < n; j++) {
                    const Ma2PlayerView *p = &w->players[j];
                    float d;
                    if (!p->alive || !p->actor) continue;
                    d = hdist(p->pos, u->pos);
                    if (d < bd) { bd = d; u->taker = p->actor; u->taker_nation = p->nation; }
                }
            }
            for (j = 0; j < S2_NAT; j++) u->known[j] = g_s2_last - u->near_t[j] <= 2.0 && u->near_t[j] > 0.0;
            u->back_t = u->gone_t + s2_resp_default();
            c7_add("sp_b", "goes", 1);
            if (u->gen) {
                int kn = 0; for (j = 0; j < S2_NAT; j++) kn += u->known[j];
                c7_add("sp_b", "gen_goes", 1); if (kn) c7_add("sp_b", "gen_goes_known", 1); if (u->taker) c7_add("sp_b", "gen_goes_taken", 1);
            }
            if (diag_on()) {
                float nd[2] = { 9999.0f, 9999.0f };
                for (j = 0; j < n; j++) { const Ma2PlayerView *p = &w->players[j]; float d; if (!p->alive || !p->actor || (p->nation & ~1)) continue; d = hdist(p->pos, u->pos); if (d < nd[p->nation & 1]) nd[p->nation & 1] = d; }
                fprintf(stderr, "[SP-PU] t=%.1f pu=%08X gone at %.0f,%.0f taker=%08X back_t=%.1f gen=%d near=%.0f/%.0f\n", g_s2_last, u->ent, u->pos.x, u->pos.z, u->taker, u->back_t, u->gen, nd[0], nd[1]);
            }
        }
        for (i = 0; i < S2_PU; i++) {   /* never came back (a destroyed unit's drop): forget it */
            S2Pu *u = &g_s2pu[i];
            if (u->used && !u->in_view && g_s2_last - u->gone_t > 4.0 * s2_resp_default() + 60.0) u->used = 0;
        }
    }
    /* pincer baseline: how often does the same kind of pair kill its target within 20 s without the trick */
    for (i = 0; i < S2_SHD; i++) {
        const Ma2PlayerView *tv;
        if (!g_s2shd[i].used) continue;
        tv = player_by_actor(w, g_s2shd[i].tgt);
        if (!g_s2shd[i].done && (!tv || !tv->alive)) { g_s2shd[i].done = 1; c7_add("sp_pincer", "plain_killed_20s", 1); }
        if (w->time >= g_s2shd[i].until) g_s2shd[i].used = 0;
    }
}

/* ---- common ---- */
static int s2_hard_exit(const Ma2Brain *b, const Ma2PlayerView *me)
{
    return b->dmg_recent > 0.15f || hp_frac(me->hp, me->hp_max) < b->sk.retreat_hp;
}
static void s2_trick_time(const Ma2Brain *b) { c7_add("sp_all", "trick_s", b->dt); }

/* ======================================================================
 * 5. Veteran baiting
 * ====================================================================== */
/* Was parked OFF (S2-RESULT.md: sprung 0 in ~35 fires). design note: every approved behaviour ships ON, so the gate now defaults to 1;
 * RECOMP_BOT_SP_BAIT_ENABLE=0 / [Bots] SpBaitEnable=0 (or SpBait=0 / the master) turns it off. */
static int s2_bait_enabled(void) { return c7_on("sp_bait_gate", "RECOMP_BOT_SP_BAIT_ENABLE", "SpBaitEnable", 1) || sp_force(SP_BAIT); }
static void s2_bait_end(Ma2Brain *b, int idx, const char *why, int died)
{
    S2Bot *s = &g_s2b[idx];
    if (!s->bait) return;
    if (died) c7_add("sp_bait", "bait_died", 1);
    c7_add("sp_bait", s2_ended(why), 1);
    if (diag_on()) fprintf(stderr, "[SP-BAIT] t=%.1f bot=%d end=%s after %.1f s followed=%d sprung=%d%s\n", g_s2_last, idx, why, g_s2_last - s->bt0, s->b_followed, s->b_sprung, died ? " (bait died)" : "");
    if (s->bait == 1 && b) { b->tac_until = b->now; b->next_eval = 0.0; }
    s->bait = 0;
}
/* a teammate worth falling back to: fighting, 30-90 m from us, roughly behind us (away from the chaser) */
static int s2_bait_mate(const Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *t, Ma2Vec3 *mate_pos)
{
    int i, best = -1; float ba = 1e9f;
    Ma2Vec3 away = hnorm(vsub(me->pos, t->pos));
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        Ma2Vec3 to; float d, c;
        if (q == me || !q->alive || q->nation != me->nation || !q->actor) continue;
        if (!s2_ground_kind(kind_ok(q->kind))) continue;
        if (hp_frac(q->hp, q->hp_max) < 0.4f) continue;
        d = hdist(q->pos, me->pos);
        if (d < 30.0f || d > 90.0f) continue;
        if (!q->attacking && !q->firing && !(q->is_bot == 0)) continue;     /* a teammate in the fight (a human counts) */
        to = hnorm(vsub(q->pos, me->pos));
        c = to.x * away.x + to.z * away.z;
        if (c < 0.34f) continue;                                          /* within ~70 deg of straight away from the chaser */
        if (hdist(q->pos, t->pos) < 45.0f) continue;                      /* not one the chaser is already on */
        if (acosf(clampf(c, -1.0f, 1.0f)) < ba) { ba = acosf(clampf(c, -1.0f, 1.0f)); best = i; }
    }
    if (best < 0) return 0;
    *mate_pos = w->players[best].pos;
    return 1;
}
static void s2_bait_tick(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, int k, float hpf)
{
    S2Bot *s = &g_s2b[b->index & (SP_BOTS - 1)];
    int idx = b->index & (SP_BOTS - 1);
    Mem *t = b->tgt;
    if (s->bait == 1) {
        const Ma2PlayerView *cv = player_by_actor(w, s->b_chaser);
        float dpt;
        s2_trick_time(b);
        if (!cv || !cv->alive) { c7_add("sp_bait", "chaser_killed", 1); s2_bait_end(b, idx, "chaser_dead", 0); return; }
        if (s2_hard_exit(b, me)) { s2_bait_end(b, idx, "hurt", 0); return; }
        if (b->goal != G_ATTACK && b->goal != G_COVER && b->goal != G_RETREAT) { s2_bait_end(b, idx, "goal", 0); return; }
        if (!s2_hold_target(b, s->b_chaser)) { s2_bait_end(b, idx, "target", 0); return; }
        t = b->tgt;
        if (b->now - s->bt0 > 20.0) { s2_bait_end(b, idx, "time", 0); return; }
        dpt = hdist(cv->pos, s->b_pt);
        if (diag_on() && b->now - s->bt_dbg >= 2.0) {
            s->bt_dbg = b->now;
            fprintf(stderr, "[SP-BAIT] t=%.1f bot=%d run: me->pt %.0f, chaser->pt %.0f, chaser->me %.0f, chaser->mate %.0f, goal=%s\n", b->now, idx, hdist(me->pos, s->b_pt), dpt, hdist(cv->pos, me->pos), hdist(cv->pos, s->b_mate), k_goal_name[b->goal % G_COUNT]);
        }
        if (dpt < s->b_min_d - 2.0f) { s->b_min_d = dpt; s->bt_prog = b->now; }
        if (!s->b_followed && hdist(cv->pos, s->b_corner ? s->b_pt : s->b_mate) < 55.0f) {
            s->b_followed = 1; s->bt_follow = b->now; s->b_hp0 = cv->hp / maxf(cv->hp_max, 1.0f);
            c7_add("sp_bait", "followed", 1);
        }
        if (b->now - s->bt_prog > 8.0 && !s->b_followed) { s2_bait_end(b, idx, "nofollow", 0); return; }
        if (hdist(me->pos, s->b_pt) < 6.0f || s->b_followed) {
            /* arrived (or the chaser is in): stop steering, fight normally here and watch for the crossfire */
            b->tac_until = b->now; b->next_eval = 0.0;
            s->bait = 2; s->bt_mon_end = b->now + 10.0;
            return;
        }
        b->tac_pt = s->b_pt; b->tac_tgt = s->b_chaser; b->tac_until = b->now + 1.0; b->tac_kind = b->goal;
        b->flank_on = 0;
        spot_claim(me->nation, b->index, 1, s->b_pt, b->now + 2.0);
        return;
    }
    if (s->bait == 2) {
        const Ma2PlayerView *cv = player_by_actor(w, s->b_chaser);
        if (cv && cv->alive && !s->b_followed && hdist(cv->pos, s->b_corner ? s->b_pt : s->b_mate) < 55.0f) {
            s->b_followed = 1; s->bt_follow = b->now; s->b_hp0 = cv->hp / maxf(cv->hp_max, 1.0f);
            c7_add("sp_bait", "followed", 1);
        }
        if (cv && cv->alive && !s->b_sprung && s->b_followed) {
            float hpn = cv->hp / maxf(cv->hp_max, 1.0f);
            if (s->b_hp0 - hpn >= 0.08f) {   /* the chaser lost health while a teammate near it was shooting */
                int i, fire = 0;
                for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
                    const Ma2PlayerView *q = &w->players[i];
                    if (q == me || !q->alive || q->nation != me->nation || hdist(q->pos, cv->pos) > 90.0f) continue;
                    if (q->firing) fire = 1;
                }
                if (fire) { s->b_sprung = 1; c7_add("sp_bait", "sprung", 1); }
            }
        }
        if (cv && !cv->alive) { c7_add("sp_bait", "chaser_killed", 1); s2_bait_end(b, idx, "chaser_dead", 0); return; }
        if (b->now >= s->bt_mon_end || s2_hard_exit(b, me)) { s2_bait_end(b, idx, "done", 0); return; }
        return;
    }
    /* idle: look for a chance every half second */
    if (b->now < s->bt_next) return;
    s->bt_next = b->now + 0.5;
    if (!sp_on(SP_BAIT) || !s2_bait_enabled() || (b->skill & 3) < MA2_SKILL_VETERAN || !t || b->flank_on || b->obj_lock) return;
    if ((b->goal != G_ATTACK && b->goal != G_COVER) || !s2_ground_kind(k)) return;
    if (!mem_engageable(b, t) || t->kind == MA2_KIND_PILOT || !t->visible || b->n_attackers > 2) return;
    {
        float d = hdist(me->pos, t->pos), closing, tthreat, mthreat, vl;
        Ma2Vec3 toward;
        if (d < 25.0f || d > 110.0f) return;
        if (hpf < b->sk.retreat_hp + 0.12f) return;                       /* it would retreat anyway */
        if (!threatens_me(b, t, 3.0)) return;
        toward = hnorm(vsub(me->pos, t->pos)); vl = hlen(t->vel);
        closing = vl > 0.1f ? (t->vel.x * toward.x + t->vel.z * toward.z) : 0.0f;
        if (closing < 2.0f) return;                                       /* chasing us, closing */
        tthreat = k_kind[kind_ok(t->kind)].threat * hp_frac(t->hp, t->hp_max);
        mthreat = k_kind[k].threat * hpf;
        if (!(hpf < 0.6f || tthreat > 1.5f * mthreat)) return;
        {
            Ma2Vec3 mate, pt; int corner = 0, ok = 0, have_mate;
            double st0; unsigned sr0;
            have_mate = s2_bait_mate(b, w, me, t, &mate);                 /* positions only: cheap */
            if (!have_mate && !(b->now >= s->bt_corner_next && b->rays >= 14.0f)) return;
            snprintf(g_s2_note, sizeof g_s2_note, "bait to %s d=%.0f hp=%.2f", have_mate ? "mate" : "corner", d, hpf);
            sp_note(g_s2_note);
            if (!sp_roll(b, SP_BAIT, t->player)) return;                   /* the 20 s hold bounds every search below to one per 20 s per bot */
            st0 = c7_t0(); sr0 = b->rays_used;
            if (have_mate) {
                Ma2Vec3 u = hnorm(vsub(mate, t->pos));
                pt = vadd(mate, vscale(u, 8.0f));                         /* just behind the teammate, the chaser has to pass it */
                pt.y = ground_at(sn, pt.x, pt.z, mate.y);
                ok = point_free(sn, pt, k);
                if (!ok) { pt = mate; pt.x += u.z * 10.0f; pt.z -= u.x * 10.0f; pt.y = ground_at(sn, pt.x, pt.z, mate.y); ok = point_free(sn, pt, k); }
            } else {
                s->bt_corner_next = b->now + 6.0;
                if (tac_cover(b, w, me, t, 1, sn, &pt) && hdist(pt, me->pos) >= 30.0f && hdist(pt, t->pos) > hdist(me->pos, t->pos)) { corner = 1; ok = 1; mate = pt; }
            }
            c7_ms("sp_b", "bait_search", st0); c7_add("sp_b", "bait_search_n", 1); c7_add("sp_b", "bait_search_rays", (double)(b->rays_used - sr0));
            if (!ok) { c7_add("sp_bait", "no_dest", 1); return; }
            s->bait = 1; s->bt0 = b->now; s->bt_prog = b->now; s->b_chaser = t->actor; s->b_chaser_pl = t->player;
            s->b_pt = pt; s->b_mate = mate; s->b_min_d = hdist(t->pos, pt); s->b_followed = 0; s->b_sprung = 0; s->b_corner = corner;
            b->tac_pt = pt; b->tac_tgt = t->actor; b->tac_until = b->now + 1.0; b->tac_kind = b->goal;
            c7_add("sp_bait", corner ? "to_corner" : "to_mate", 1);
        }
    }
}

/* ======================================================================
 * 9. Crossfire / pincer
 * ====================================================================== */
static void s2_pin_end(int r, const char *why, int died)
{
    S2Pin *p = &g_s2pin[r];
    if (!p->active) return;
    if (diag_on()) {
        const S2Info *ia = &g_s2i[p->a & (SP_BOTS - 1)], *ib = &g_s2i[p->b & (SP_BOTS - 1)];
        fprintf(stderr, "[SP-PINCER] t=%.1f bots %d,%d end=%s after %.1f s, arrived %d/%d, left to spots %.0f / %.0f m\n", g_s2_last, p->a, p->b, why, g_s2_last - p->t0,
                p->arrived[0], p->arrived[1], hdist(ia->pos, p->spot[0]), hdist(ib->pos, p->spot[1]));
    }
    p->active = 0; p->ended = 1;
    c7_add("sp_pincer", s2_ended(why), 1);
    if (died) c7_add("sp_pincer", "pincer_died", 1);
    if (strcmp(why, "dead") != 0 && strcmp(why, "time") != 0 && !died) c7_add("sp_pincer", "broke_off", 1);
    for (int j = 0; j < 2; j++) { int bi = (j ? p->b : p->a) & (SP_BOTS - 1); g_s2b[bi].pin_rec = 0; g_s2b[bi].pin_next = g_s2_last + 5.0; }
}
static int s2_pin_find(uint32_t tgt)
{
    for (int i = 0; i < S2_PIN; i++) if ((g_s2pin[i].active || g_s2pin[i].watch) && g_s2pin[i].tgt == tgt) return i;
    return -1;
}
/* A firing spot on the wanted bearing from the target: free, in clear sight, not claimed by another teammate. */
static int s2_pin_spot(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *t, float bearing, float rad, const Ma2Senses *sn,
                       Ma2Nav *nav, Ma2Vec3 from, uint32_t for_actor, int k, Ma2Vec3 *out)
{
    static const float offs[5] = { 0.0f, 0.25f, -0.25f, 0.5f, -0.5f };
    Ma2Vec3 te = eye_of(t->pos, t->kind, 0.6f);
    for (int i = 0; i < 5; i++) {
        Ma2Vec3 p = vadd(t->pos, vscale(dir_of_heading(bearing + offs[i]), rad));
        int r;
        p.y = ground_at(sn, p.x, p.z, me->pos.y);
        if (!point_free(sn, p, k)) continue;
        if (nav && !ma2_nav_connected(nav, from, p)) continue;
        if (spot_taken(b, w, me, p, 12.0f)) continue;
        r = ray(b, sn, for_actor, t->actor, eye_of(p, k, 1.0f), te);
        if (r != 1) continue;
        *out = p;
        return 1;
    }
    return 0;
}
/* The two spots: ~90 deg apart around the target, centred on where the pair stands now, nearer than the bots are. */
static int s2_pin_plan_i(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *t, Ma2Nav *nav, const Ma2Senses *sn, int k,
                       Ma2Vec3 pos_a, uint32_t act_a, Ma2Vec3 pos_b, uint32_t act_b, int kind_b, Ma2Vec3 *spA, Ma2Vec3 *spB)
{
    float ha = heading_of(vsub(pos_a, t->pos)), hb = heading_of(vsub(pos_b, t->pos)), base, rad, d1, d2, dt = hdist(pos_a, t->pos);
    Ma2Vec3 sp_ph, sp_mh;
    float ph, mh;
    base = heading_of(vadd(dir_of_heading(ha), dir_of_heading(hb)));
    if (hlen(vadd(dir_of_heading(ha), dir_of_heading(hb))) < 0.2f) base = ha;
    rad = clampf(minf(0.5f * (b->band_min + b->band_max), 0.7f * dt), 40.0f, 130.0f);
    rad = minf(rad, 0.8f * my_reach(me));
    ph = base + 0.785f; mh = base - 0.785f;
    if (!s2_pin_spot(b, w, me, t, ph, rad, sn, nav, pos_a, act_a, k, &sp_ph)) return 0;
    if (!s2_pin_spot(b, w, me, t, mh, rad, sn, nav, pos_b, act_b, kind_b, &sp_mh)) return 0;
    d1 = hdist(pos_a, sp_ph) + hdist(pos_b, sp_mh); d2 = hdist(pos_a, sp_mh) + hdist(pos_b, sp_ph);
    if (d1 <= d2) { *spA = sp_ph; *spB = sp_mh; } else { *spA = sp_mh; *spB = sp_ph; }
    return 1;
}
static int s2_pin_plan(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *t, Ma2Nav *nav, const Ma2Senses *sn, int k,
                       Ma2Vec3 pos_a, uint32_t act_a, Ma2Vec3 pos_b, uint32_t act_b, int kind_b, Ma2Vec3 *spA, Ma2Vec3 *spB)
{
    double st0 = c7_t0(); unsigned sr0 = b->rays_used;
    int ok = s2_pin_plan_i(b, w, me, t, nav, sn, k, pos_a, act_a, pos_b, act_b, kind_b, spA, spB);
    c7_ms("sp_b", "pin_search", st0); c7_add("sp_b", "pin_search_n", 1); c7_add("sp_b", "pin_search_rays", (double)(b->rays_used - sr0));
    return ok;
}
static void s2_pin_tick(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav, const Ma2Senses *sn, int k, float hpf)
{
    int idx = b->index & (SP_BOTS - 1), r;
    S2Bot *s = &g_s2b[idx];
    Mem *t = b->tgt;
    if (s->pin_rec) {   /* taking part */
        S2Pin *p = &g_s2pin[s->pin_rec - 1];
        int me_slot = p->a == idx ? 0 : 1, owner = me_slot == 0;
        const Ma2PlayerView *tv;
        r = s->pin_rec - 1;
        if (!p->active) { s->pin_rec = 0; return; }
        s2_trick_time(b);
        tv = player_by_actor(w, p->tgt);
        if (!tv || !tv->alive) { if (owner) c7_add("sp_pincer", "target_dead_in_trick", 1); s2_pin_end(r, "dead", 0); return; }
        if (!s2_hold_target(b, p->tgt)) { s2_pin_end(r, "target", 0); return; }
        t = b->tgt;
        if (owner) {   /* the owner watches the pair */
            const S2Info *mi = &g_s2i[p->b & (SP_BOTS - 1)];
            if (b->now >= p->until) { s2_pin_end(r, "time", 0); return; }
            if (b->now - p->t0 > 2.0 && hdist(tv->pos, p->tpos0) > 40.0f) {   /* it moved: plan the spots again (twice), else the other drops back to the core fight */
                Ma2Vec3 nA, nB;
                if (p->replans < 2 && b->rays >= 12.0f && mi->valid && s2_pin_plan(b, w, me, t, nav, sn, k, me->pos, me->actor, mi->pos, mi->actor, kind_ok(mi->kind), &nA, &nB)) {
                    p->spot[0] = nA; p->spot[1] = nB; p->tpos0 = tv->pos; p->replans++; p->arrived[0] = p->arrived[1] = 0; p->until += 6.0;
                    c7_add("sp_pincer", "replans", 1);
                } else if (hdist(tv->pos, p->tpos0) > 60.0f) { s2_pin_end(r, "moved", 0); return; }
            }
            if (!mi->valid || b->now - mi->t > 1.0) { s2_pin_end(r, "mate_dead", 1); return; }
            if (mi->hpf < 0.35f) { s2_pin_end(r, "mate_hurt", 0); return; }
            if (!p->both && p->arrived[0] && p->arrived[1]) {
                float ha = heading_of(vsub(me->pos, tv->pos)), hb = heading_of(vsub(mi->pos, tv->pos));
                p->both = 1; c7_add("sp_pincer", "both_arrived", 1);
                c7_add("sp_pincer", "angle_sum", fabsf(s2_ang_diff(ha, hb)) * (180.0f / MA2B_PI)); c7_add("sp_pincer", "angle_n", 1);
                if (diag_on()) fprintf(stderr, "[SP-PINCER] t=%.1f bots %d,%d both at the target %08X, %.0f deg apart\n", b->now, p->a, p->b, p->tgt, fabsf(s2_ang_diff(ha, hb)) * (180.0f / MA2B_PI));
            }
        }
        if (hpf < 0.35f) { s2_pin_end(r, "hurt", 0); return; }
        if (s2_hard_exit(b, me)) { s2_pin_end(r, "hard", 0); return; }
        if (b->goal != G_ATTACK) { if (b->now - s->pin_arr_t > 3.0) { s2_pin_end(r, "goal", 0); return; } }
        else s->pin_arr_t = b->now;
        b->tac_pt = p->spot[me_slot]; b->tac_tgt = p->tgt; b->tac_until = b->now + 1.0; b->tac_kind = G_ATTACK; b->flank_on = 0;
        spot_claim(me->nation, b->index, 1, p->spot[me_slot], b->now + 2.0);
        if (hdist(me->pos, p->spot[me_slot]) < 20.0f) p->arrived[me_slot] = 1;
        return;
    }
    /* idle: only the lower-index bot of a pair rolls, every 1.5 s */
    if (b->now < s->pin_next) return;
    s->pin_next = b->now + 1.5;
    if (!sp_on(SP_PINCER) || (b->skill & 3) < MA2_SKILL_VETERAN || !t || b->flank_on || b->obj_lock || b->goal != G_ATTACK || !s2_ground_kind(k)) return;
    if (!t->visible || !mem_engageable(b, t) || !s2_ground_kind(kind_ok(t->kind)) || hpf <= 0.5f || b->now < g_sp_cool[SP_PINCER][idx]) return;
    {
        float dt = hdist(me->pos, t->pos);
        int j, mate = -1;
        if (dt < 50.0f || dt > 200.0f) return;
        for (j = idx + 1; j < SP_BOTS; j++) {   /* the lower index owns it */
            const S2Info *mi = &g_s2i[j];
            if (!mi->valid || b->now - mi->t > 0.6 || mi->nation != me->nation || mi->skill < MA2_SKILL_VETERAN || mi->tgt != t->actor) continue;
            if (mi->busy || mi->carrying || mi->hpf <= 0.5f || !s2_ground_kind(mi->kind)) continue;
            if (hdist(mi->pos, me->pos) > 60.0f || b->now < g_sp_cool[SP_PINCER][j]) continue;
            mate = j; break;
        }
        if (mate < 0) return;
        if (s2_pin_find(t->actor) >= 0) return;
        {
            const S2Info *mi = &g_s2i[mate];
            int sk_pair = ((b->skill & 3) == MA2_SKILL_ELITE) + (mi->skill == MA2_SKILL_ELITE);   /* 0 vv, 1 ve, 2 ee */
            static const float pp[3] = { 0.25f, 0.35f, 0.45f };
            Ma2Vec3 spA, spB;
            int slot = -1, i;
            if (b->rays < 12.0f) return;
            for (i = 0; i < S2_PIN; i++) if (!g_s2pin[i].active && !g_s2pin[i].watch) { slot = i; break; }
            snprintf(g_s2_note, sizeof g_s2_note, "pincer %d+%d on %08X d=%.0f", idx, mate, t->actor, dt);
            sp_note(g_s2_note);
            if (slot < 0) return;
            /* the roll (shadow baseline first: this pair is a candidate whether it rolls in or not) */
            if (!sp_roll_p(b, SP_PINCER, t->player, pp[sk_pair])) {
                int q;
                for (q = 0; q < S2_SHD; q++) if (g_s2shd[q].used && g_s2shd[q].tgt == t->actor) break;
                if (q == S2_SHD && sp_on(SP_PINCER)) {
                    for (q = 0; q < S2_SHD; q++) if (!g_s2shd[q].used) break;
                    if (q < S2_SHD) { g_s2shd[q].used = 1; g_s2shd[q].tgt = t->actor; g_s2shd[q].t0 = b->now; g_s2shd[q].until = b->now + 20.0; g_s2shd[q].done = 0; c7_add("sp_pincer", "plain_n", 1); }
                }
                return;
            }
            g_sp_cool[SP_PINCER][mate] = b->now + 5.0 * 60.0 * (0.7 + 0.6 * (double)rng_f(&b->rng));
            if (!s2_pin_plan(b, w, me, t, nav, sn, k, me->pos, me->actor, mi->pos, mi->actor, kind_ok(mi->kind), &spA, &spB)) { c7_add("sp_pincer", "no_spot", 1); return; }
            memset(&g_s2pin[slot], 0, sizeof g_s2pin[slot]);
            g_s2pin[slot].active = 1; g_s2pin[slot].watch = 1; g_s2pin[slot].a = idx; g_s2pin[slot].b = mate; g_s2pin[slot].tgt = t->actor; g_s2pin[slot].tgt_pl = t->player;
            g_s2pin[slot].tpos0 = (player_by_actor(w, t->actor) ? player_by_actor(w, t->actor)->pos : t->pos); g_s2pin[slot].spot[0] = spA; g_s2pin[slot].spot[1] = spB; g_s2pin[slot].t0 = b->now; g_s2pin[slot].until = b->now + 25.0;
            g_s2pin[slot].skills = sk_pair; g_s2pin[slot].actor[0] = me->actor; g_s2pin[slot].actor[1] = mi->actor;
            s->pin_rec = slot + 1; g_s2b[mate].pin_rec = slot + 1; s->pin_arr_t = b->now; g_s2b[mate].pin_arr_t = b->now;
            c7_add("sp_pincer", k_s2_pair[sk_pair], 1);
            if (diag_on()) fprintf(stderr, "[SP-PINCER] t=%.1f bots %d,%d (pair %d) on %08X d=%.0f: spots %.0f,%.0f and %.0f,%.0f\n", b->now, idx, mate, sk_pair, t->actor, dt, spA.x, spA.z, spB.x, spB.z);
        }
    }
}
/* the target-killed window of a finished pincer: closes 20 s after it began, counted once */
static void s2_pin_watch(const Ma2World *w)
{
    for (int i = 0; i < S2_PIN; i++) {
        S2Pin *p = &g_s2pin[i];
        const Ma2PlayerView *tv;
        if (!p->watch) continue;
        tv = player_by_actor(w, p->tgt);
        if (tv && !tv->alive) { c7_add("sp_pincer", "target_killed_20s", 1); p->watch = 0; if (p->active) s2_pin_end(i, "dead", 0); continue; }
        if (!tv) { p->watch = 0; continue; }
        if (w->time >= p->t0 + 20.0) p->watch = 0;
    }
}

/* ======================================================================
 * 6. Elite power-up timing + denial
 * ====================================================================== */
static S2Pu *s2_pu_by_ent(uint32_t ent)
{
    for (int i = 0; i < S2_PU; i++) if (g_s2pu[i].used && g_s2pu[i].ent == ent) return &g_s2pu[i];
    return NULL;
}
static void s2_pu_end(Ma2Brain *b, int idx, const char *why)
{
    S2Bot *s = &g_s2b[idx];
    if (!s->pu_mode) return;
    c7_add("sp_powerup", s2_ended(why), 1);
    if (b) { b->next_eval = 0.0; b->tac_until = b->now; }
    s->pu_mode = 0; s->pu_set = 0;
}
/* pickup taken by...: resolve a gone event for a bot that was waiting for / denying it */
static void s2_pu_resolve(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, int idx, S2Pu *u)
{
    S2Bot *s = &g_s2b[idx];
    int mine = u->taker == me->actor, mates = u->taker && !mine && u->taker_nation == me->nation;
    if (s->pu_kind == 1) {
        if (mine && b->now - u->ret_t <= 3.5) { c7_add("sp_powerup", "timed_take", 1); s2_pu_end(b, idx, "taken"); }
        else if (mine) { c7_add("sp_powerup", "took_late", 1); s2_pu_end(b, idx, "taken"); }
        else if (mates) { c7_add("sp_powerup", "timed_mate", 1); s2_pu_end(b, idx, "mate_took"); }
        else { c7_add("sp_powerup", "timed_late", 1); s2_pu_end(b, idx, "lost"); }
    } else {
        if (mine || mates) { c7_add("sp_powerup", "deny_kept", 1); s2_pu_end(b, idx, "kept"); }
        else if (u->taker && u->taker == s->pu_victim) { c7_add("sp_powerup", "deny_taken", 1); s2_pu_end(b, idx, "victim_took"); }
        else { c7_add("sp_powerup", "deny_taken", 1); s2_pu_end(b, idx, "other_took"); }
    }
    (void)w;
}
static void s2_pu_tick(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav, const Ma2Senses *sn, int k, float hpf)
{
    int idx = b->index & (SP_BOTS - 1);
    S2Bot *s = &g_s2b[idx];
    int i;
    if (s->pu_mode) {
        S2Pu *u = s2_pu_by_ent(s->pu_ent);
        if (!u) { s2_pu_end(b, idx, "lost_pu"); return; }
        if (u->ev != s->pu_ev && !u->in_view) { s2_pu_resolve(b, w, me, idx, u); return; }   /* it went (while we were out there) */
        if (s2_hard_exit(b, me)) { s2_pu_end(b, idx, "hurt"); return; }
        if (s->pu_mode == 3) { if (b->now - s->pu_take_t > 8.0 + hdist(me->pos, u->pos) / s2_speed(kind_ok(me->kind))) { s2_pu_end(b, idx, "take_timeout"); return; } }
        else if (b->now - s->pu_t0 > (s->pu_kind == 1 ? 25.0 : 12.0)) { s2_pu_end(b, idx, "time"); return; }
        if (s->pu_mode != 3) s2_trick_time(b);
        if (s->pu_kind == 1) {
            if (b->goal != G_ROAM && b->goal != G_ATTACK && b->goal != G_POWERUP) { s2_pu_end(b, idx, "goal"); return; }
            if (s->pu_mode == 1) {
                c7_add("sp_powerup", "wait_s_sum", b->dt);
                if (u->in_view) { s->pu_mode = 3; s->pu_take_t = b->now; b->next_eval = 0.0; }
                else if (b->now > s->pu_back_t + 8.0) { c7_add("sp_powerup", "not_back", 1); s2_pu_end(b, idx, "not_back"); return; }
                else if (b->n_attackers > 0) { s2_pu_end(b, idx, "attacked"); return; }
            } else if (s->pu_mode == 3) {
                if (u->in_view && hdist(me->pos, u->pos) < 4.0f) { /* about to take it: the registry resolves it when the crate goes */ }
            }
        } else if (s->pu_mode == 2) {
            const Ma2PlayerView *vv = player_by_actor(w, s->pu_victim);
            float dv;
            if (b->goal != G_ROAM && b->goal != G_ATTACK && b->goal != G_POWERUP && b->goal != G_COVER) { s2_pu_end(b, idx, "goal"); return; }
            if (!vv || !vv->alive) { c7_add("sp_powerup", "deny_victim_dead", 1); s2_pu_end(b, idx, "victim_dead"); if (u->in_view) s->pu_mode = 3, s->pu_take_t = b->now; return; }
            dv = hdist(vv->pos, u->pos);
            if (s->pu_mode == 2) {
                Mem *m = mem_by_actor(b, s->pu_victim);
                s2_trick_time(b);
                if (dv < 8.0f && s->pu_reach_t <= 0.0) s->pu_reach_t = b->now;
                if (s->pu_reach_t > 0.0 && !s->pu_hit && b->now - s->pu_reach_t <= 3.0 && m && rate_of(m->out_v, m->out_t, b->now) > 0.0f) { s->pu_hit = 1; c7_add("sp_powerup", "deny_hit", 1); }
                if (dv > 60.0f && s->pu_reach_t <= 0.0) { s2_pu_end(b, idx, "turned_away"); if (u->in_view) s->pu_mode = 3, s->pu_take_t = b->now; return; }
                if (b->now - s->pu_t0 > 12.0) { s2_pu_end(b, idx, "time"); return; }
            }
        }
        return;
    }
    /* idle: Elites only, looking twice a second */
    if (b->now < s->pu_next) return;
    s->pu_next = b->now + 0.5;
    if ((b->skill & 3) != MA2_SKILL_ELITE || !sp_on(SP_POWERUP) || !s2_ground_kind(k) || b->obj_lock || hpf < 0.5f) return;
    if (b->now < g_sp_cool[SP_POWERUP][idx]) return;
    /* 6a: timing */
    if ((b->goal == G_ROAM || (b->goal == G_ATTACK && (!b->tgt || !b->tgt->visible || hdist(me->pos, b->tgt->pos) > 120.0f))) && b->n_attackers == 0) {
        for (i = 0; i < S2_PU; i++) {
            S2Pu *u = &g_s2pu[i];
            Ma2PickupView pv; float d, tt, rem; int j, taken_by_mate = 0;
            if (!u->used || u->in_view || !u->gen || u->gone_t <= 0.0 || !u->known[me->nation & (S2_NAT - 1)]) continue;
            memset(&pv, 0, sizeof pv); pv.entity = u->ent; pv.kind = MA2_PICKUP_POWERUP; pv.pos = u->pos; pv.armor = u->armor; pv.wclass = u->wclass; pv.nation = -1;
            if (powerup_value(b, me, &pv) <= 0.0f) continue;
            d = hdist(me->pos, u->pos);
            if (d > 150.0f || d < 12.0f) continue;
            tt = d / s2_speed(k); rem = (float)(u->back_t - b->now);
            if (rem < tt + 2.0f || rem > tt + 12.0f) continue;
            /* PuWaitShort (default on, RECOMP_BOT_PU_WAIT_SHORT / [Bots] PuWaitShort; playtest 11-06 no standing about): s2_speed is about half a
             * 'Mech's real pace, so the bot got there early and stood out the rest -- d5_41 b2 22.7 s at full hp by the crate while b1 fought
             * alone 250 m off. Only when the crate is back within 6 s of the real arrival. [C7] sp_powerup: wait_too_long. */
            if (rem > d / (2.0f * s2_speed(k)) + 6.0f && c7_on("puwaitshort", "RECOMP_BOT_PU_WAIT_SHORT", "PuWaitShort", 1)) { c7_add("sp_powerup", "wait_too_long", 1); continue; }
            for (j = 0; j < SP_BOTS; j++) if (j != idx && g_s2b[j].pu_mode && g_s2b[j].pu_ent == u->ent) taken_by_mate = 1;
            if (taken_by_mate) continue;
            snprintf(g_s2_note, sizeof g_s2_note, "time pu=%08X back_in=%.1f d=%.0f", u->ent, rem, d);
            sp_note(g_s2_note);
            if (!sp_roll_p(b, SP_POWERUP, 0xFFFF0000u | (uint32_t)(me->nation & 0xFFFF), 0.40f)) return;
            {
                /* the waiting spot: 10-20 m off the crate (standing on it may hold the respawn), sight of it, out of sight of the nearest known enemy */
                const Mem *en = NULL; float ed = 1e9f, bs = -1e9f; double st0 = c7_t0(); unsigned sr0 = b->rays_used;
                Ma2Vec3 best = u->pos, p;
                int found = 0, a;
                for (j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
                    const Mem *m = &b->mem[j];
                    if (!m->valid || m->nation == me->nation || mem_conf(b, m) < 0.4f) continue;
                    if (hdist(m->pos, u->pos) < ed) { ed = hdist(m->pos, u->pos); en = m; }
                }
                for (a = 0; a < 8; a++) {
                    float rad = (a & 1) ? 17.0f : 13.0f, sc;
                    int rs, ok = 1;
                    p = vadd(u->pos, vscale(dir_of_heading((float)a * (MA2B_PI / 4.0f) + 0.3f), rad));
                    p.y = ground_at(sn, p.x, p.z, u->pos.y);
                    if (!point_free(sn, p, k)) continue;
                    if (nav && !ma2_nav_connected(nav, me->pos, p)) continue;
                    if (spot_taken(b, w, me, p, 8.0f)) continue;
                    rs = ray(b, sn, me->actor, 0, eye_of(p, k, 1.0f), vadd(u->pos, v3(0, 1.0f, 0)));
                    if (rs == 0) continue;
                    sc = 4.0f - hdist(me->pos, p) / 40.0f;
                    if (en && ok) { int re = ray(b, sn, en->actor, me->actor, eye_of(en->pos, en->kind, 1.0f), eye_of(p, k, 0.8f)); if (re == 0) sc += 3.0f; }
                    if (sc > bs) { bs = sc; best = p; found = 1; }
                }
                c7_ms("sp_b", "pu_search", st0); c7_add("sp_b", "pu_search_n", 1); c7_add("sp_b", "pu_search_rays", (double)(b->rays_used - sr0));
                if (!found) { c7_add("sp_powerup", "no_spot", 1); return; }
                c7_add("sp_powerup", "rolls", 1); c7_add("sp_powerup", "fired_time_elite", 1);
                s->pu_mode = 1; s->pu_kind = 1; s->pu_ent = u->ent; s->pu_pos = u->pos; s->pu_spot = best; s->pu_ev = u->ev; s->pu_t0 = b->now; s->pu_back_t = u->back_t; s->pu_set = 1; s->pu_victim = 0; s->pu_reach_t = 0.0; s->pu_hit = 0;
                spot_claim(me->nation, b->index, 1, best, b->now + 30.0);
                if (diag_on()) fprintf(stderr, "[SURPRISE] trick=powerup kind=time pu=%08X back_in=%.1f d=%.0f bot=%d spot=%.0f,%.0f\n", u->ent, rem, d, idx, best.x, best.z);
                return;
            }
        }
    }
    /* 6b: denial. A hurt enemy walking onto a pickup it will reach before us: hold a spot with sight of it. */
    if ((b->goal == G_ROAM || b->goal == G_ATTACK) && b->n_attackers <= 1) {
        int j;
        for (j = 0; j < MA2_WORLD_MAX_PLAYERS; j++) {
            Mem *m = &b->mem[j];
            const Ma2PlayerView *mv;
            if (!m->valid || m->nation == me->nation || m->kind == MA2_KIND_PILOT || mem_conf(b, m) < 0.6f || !m->visible) continue;
            mv = player_by_actor(w, m->actor);
            if (!mv || !mv->alive) continue;
            for (i = 0; i < S2_PU; i++) {
                S2Pu *u = &g_s2pu[i];
                Ma2PickupView pv; float de, dme, vl, heading_ok = 0.0f;
                int back_soon;
                if (!u->used) continue;
                back_soon = u->gen && !u->in_view && u->gone_t > 0.0 && u->back_t - b->now > -2.0 && u->back_t - b->now < 10.0;
                if (!u->in_view && !back_soon) continue;
                de = hdist(m->pos, u->pos); dme = hdist(me->pos, u->pos);
                if (de > 80.0f || de > dme || dme < 30.0f || dme > 120.0f) continue;
                memset(&pv, 0, sizeof pv); pv.entity = u->ent; pv.kind = MA2_PICKUP_POWERUP; pv.pos = u->pos; pv.armor = u->armor; pv.wclass = u->wclass;
                if (u->armor > 0) { if (!(m->hp_max > 0.0f && m->hp < 0.5f * m->hp_max)) continue; }
                else if (mv->alpha_short != 1) continue;
                vl = hlen(m->vel);
                if (vl > 1.0f) {
                    Ma2Vec3 dir = hnorm(vsub(u->pos, m->pos));
                    heading_ok = (m->vel.x * dir.x + m->vel.z * dir.z) / vl;
                }
                if (!(heading_ok >= 0.707f || (back_soon && de < 40.0f))) continue;
                if (my_reach(me) < 40.0f) continue;
                snprintf(g_s2_note, sizeof g_s2_note, "deny pu=%08X back_in=%.1f d=%.0f victim=%08X", u->ent, (float)(u->back_t - b->now), dme, m->actor);
                sp_note(g_s2_note);
                if (!sp_roll_p(b, SP_POWERUP, m->player, 0.50f)) return;
                {
                    Ma2Vec3 best = me->pos, p; float bs = -1e9f; int found = 0, a; double st0 = c7_t0(); unsigned sr0 = b->rays_used;
                    for (a = 0; a < 9; a++) {
                        float ang = (float)a * (MA2B_PI / 4.0f), rad = a == 8 ? 0.0f : (a & 1 ? 8.0f : 15.0f), dd, sc;
                        int rs;
                        if (a == 8) { ang = 0.0f; rad = 0.0f; }
                        p = vadd(me->pos, vscale(dir_of_heading(ang), rad));
                        p.y = ground_at(sn, p.x, p.z, me->pos.y);
                        dd = hdist(p, u->pos);
                        if (dd > 0.85f * my_reach(me) || dd < 25.0f || !point_free(sn, p, k)) continue;
                        if (nav && rad > 0.0f && !ma2_nav_connected(nav, me->pos, p)) continue;
                        rs = ray(b, sn, me->actor, 0, eye_of(p, k, 1.0f), vadd(u->pos, v3(0, 1.0f, 0)));
                        if (rs != 1) continue;
                        sc = 5.0f - rad / 10.0f - ally_crowding(w, me, p);
                        if (sc > bs) { bs = sc; best = p; found = 1; }
                    }
                    c7_ms("sp_b", "pu_search", st0); c7_add("sp_b", "pu_search_n", 1); c7_add("sp_b", "pu_search_rays", (double)(b->rays_used - sr0));
                    if (!found) { c7_add("sp_powerup", "no_spot", 1); return; }
                    c7_add("sp_powerup", "rolls", 1); c7_add("sp_powerup", "fired_deny_elite", 1);
                    s->pu_mode = 2; s->pu_kind = 2; s->pu_ent = u->ent; s->pu_pos = u->pos; s->pu_spot = best; s->pu_ev = u->ev; s->pu_t0 = b->now; s->pu_victim = m->actor; s->pu_set = 1; s->pu_reach_t = 0.0; s->pu_hit = 0;
                    spot_claim(me->nation, b->index, 1, best, b->now + 14.0);
                    if (diag_on()) fprintf(stderr, "[SURPRISE] trick=powerup kind=deny pu=%08X back_in=%.1f d=%.0f bot=%d victim=%08X spot=%.0f,%.0f\n", u->ent, (float)(u->back_t - b->now), dme, idx, m->actor, best.x, best.z);
                    return;
                }
            }
        }
    }
}

/* ======================================================================
 * entry points
 * ====================================================================== */
static void sp2_dead(Ma2Brain *b)
{
    int idx = b->index & (SP_BOTS - 1);
    S2Bot *s = &g_s2b[idx];
    g_s2i[idx].valid = 0;
    if (s->bait) s2_bait_end(NULL, idx, "died", 1);
    if (s->pin_rec) { int r = s->pin_rec - 1; if (g_s2pin[r].active) s2_pin_end(r, "died", 1); s->pin_rec = 0; }
    if (s->pu_mode) { s2_pu_end(NULL, idx, "died"); }
}
static void sp2_tick(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Nav *nav, const Ma2Senses *sn)
{
    double t0;
    int idx = b->index & (SP_BOTS - 1), k = kind_ok(me->kind);
    float hpf = hp_frac(me->hp, me->hp_max);
    S2Info *in = &g_s2i[idx];
    S2Bot *s = &g_s2b[idx];
    if (!sp_master_on()) return;
    t0 = c7_t0();
    s2_world_tick(w);
    s2_pin_watch(w);
    in->valid = 1; in->t = b->now; in->nation = me->nation; in->skill = b->skill & 3; in->kind = k; in->actor = me->actor; in->player = me->player;
    in->tgt = b->tgt ? b->tgt->actor : 0; in->pos = me->pos; in->hpf = hpf;
    in->carrying = b->goal == G_OBJECTIVE && b->obj_op == OP_CARRY; in->busy = s->bait != 0 || s->pu_mode != 0 || b->flank_on || b->obj_lock;
    if (s->bait || s->pin_rec || s->pu_mode) {   /* a changed body ends everything */
        if (b->prev_valid && (me->actor != b->prev_actor || k != b->prev_kind)) {
            if (s->bait) s2_bait_end(b, idx, "body", 0);
            if (s->pin_rec) { int r = s->pin_rec - 1; if (g_s2pin[r].active) s2_pin_end(r, "body", 0); s->pin_rec = 0; }
            if (s->pu_mode) s2_pu_end(b, idx, "body");
        }
    }
    s2_bait_tick(b, w, me, sn, k, hpf);
    s2_pin_tick(b, w, me, nav, sn, k, hpf);
    s2_pu_tick(b, w, me, nav, sn, k, hpf);
    c7_ms("sp_b", "tick", t0);
}
/* After the goal switch: the power-up hold orders (the spot, or the pickup when it is back). */
static void sp2_order(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, Ma2Order *o)
{
    S2Bot *s = &g_s2b[b->index & (SP_BOTS - 1)];
    (void)w; (void)sn;
    if (!s->pu_mode || !s->pu_set) return;
    if (s->pu_mode == 3) return;                       /* evaluate's G_POWERUP boost takes it */
    if (hdist(me->pos, s->pu_spot) < 3.0f && s->pu_kind == 1 && !s->pu_arrived) { s->pu_arrived = 1; c7_add("sp_powerup", "at_spot", 1); }
    o->type = MA2_ORD_FIGHT_MOVE; o->point = s->pu_spot; o->stop = s->pu_kind == 1 ? 3.0f : 2.0f;
    if (s->pu_kind == 2) { o->target = s->pu_victim; o->fire = 1; }
    else if (b->tgt && mem_engageable(b, b->tgt)) { o->target = b->tgt->actor; o->fire = 1; }
    else { o->target = 0; o->fire = 0; }
    o->engage = 0; o->hold_fire = 0;
    snprintf(b->note, sizeof b->note, s->pu_kind == 1 ? "pu-wait" : "pu-deny");
}
static int sp2_pu_take(Ma2Brain *b, uint32_t entity)
{
    S2Bot *s = &g_s2b[b->index & (SP_BOTS - 1)];
    return s->pu_mode == 3 && s->pu_ent == entity;
}
