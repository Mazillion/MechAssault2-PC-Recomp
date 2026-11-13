/* ma2_bot_surprise_c.c - surprise set C surprise tricks (#included by ma2_bot_brain.c after ma2_bot_surprise.c; see ma2_bot_surprise_c.h).
 *   7  Level      Elite 'Mech/tank: the enemy ducked behind a destructible building -> shoot the building down (TRAVEL order with `structure`, the Destruction fire path).
 *   8  Stomp      Elite 'Mech: an enemy pilot within 8 m on the ground -> walk onto it (only the game's own contact damage; the gate test logs what it does).
 *   10 Hack cover a teammate BattleArmor is clamped on an enemy 'Mech running the NeuroHack -> keep the enemy's helpers off the BA, never fire on the prize.
 *   11 Peel-off   an enemy BattleArmor is clamped on a teammate -> a careful direct-fire shot at the rider (F4 skips the ridden teammate for that shot only).
 * Every trick: sp_roll() (switch, cooldown, roll, budget, no-repeat, spacing), hard exits on a heavy hit / retreat line / objective lock, [C7] pieces sp_level,
 * sp_stomp, sp_hackcover, sp_peeloff (+ sp_all trick_s), DIAG lines "[SPC] ...". Test hooks: RECOMP_BOT_STOMP_GATE=1 (stomp: any pilot within 14 m, forced, logs the
 * contact result), RECOMP_BOT_SP_FORCE / RECOMP_BOT_SP_SKILL (shared module). */

static struct { uint32_t e; } g_spc_imm[8]; static int g_spc_nimm;                  /* buildings that took no damage from our fire: left alone */
static struct { uint32_t ba, hacked; int ba_nation, seen, covered, guard; double t0; } g_hk[8];   /* hacks in progress (any team), one guard each */
static double g_hk_t = -1.0;
static struct { uint32_t rider; int guard; double t; } g_spc_peel[8];                 /* one shooter per clinging BA */
static int g_spc_contact_dmg, g_spc_regular_banned;                                   /* stomp learning, peel-off Regular ban (friendly fire seen) */

static const char *spc_piece(int act)
{
    switch (act) { case SPC_LEVEL: return "sp_level"; case SPC_STOMP: return "sp_stomp"; case SPC_HACKCOVER: return "sp_hackcover"; default: return "sp_peeloff"; }
}
static void spc_finish(Ma2Brain *b) { b->spc.act = SPC_NONE; b->next_eval = 0.0; b->interrupt = 1; }
#define SPC_END(b, lit) do { c7_add(spc_piece((b)->spc.act), lit, 1); if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d %s end=%s\n", (b)->now, (b)->index, spc_piece((b)->spc.act), lit); spc_finish(b); } while (0)

static int spc_imm(uint32_t e) { for (int i = 0; i < g_spc_nimm; i++) if (g_spc_imm[i].e == e) return 1; return 0; }

/* ---- hack bookkeeping (once a frame): every clamp, with its result and whether a guard covered it ---- */
static int spc_hack_state(const Ma2World *w, uint32_t ba, uint32_t hacked, int ba_nation)
{
    const Ma2PlayerView *bp = player_by_actor(w, ba), *hv;
    if (bp && bp->alive && bp->clamped_to == hacked) return 0;     /* still clamped */
    hv = player_by_actor(w, hacked);
    return hv && hv->alive && hv->nation == ba_nation ? 1 : 2;     /* 1 won (the 'Mech is the BA's side now), 2 lost */
}
static void spc_world(const Ma2World *w)
{
    if (w->time == g_hk_t) return;
    if (w->time < g_hk_t - 1.0) memset(g_hk, 0, sizeof g_hk);
    g_hk_t = w->time;
    for (int i = 0; i < 8; i++) g_hk[i].seen = 0;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        int j, free_j = -1;
        if (p->kind != MA2_KIND_BATTLEARMOR || !p->alive || !p->clamped_to) continue;
        for (j = 0; j < 8; j++) { if (g_hk[j].ba == p->actor && g_hk[j].hacked == p->clamped_to) break; if (!g_hk[j].ba && free_j < 0) free_j = j; }
        if (j == 8 && free_j >= 0) {
            j = free_j; memset(&g_hk[j], 0, sizeof g_hk[j]); g_hk[j].ba = p->actor; g_hk[j].hacked = p->clamped_to; g_hk[j].ba_nation = p->nation; g_hk[j].t0 = w->time;
            c7_add("sp_hackcover", "hacks_all", 1);
        }
        if (j < 8) g_hk[j].seen = 1;
    }
    for (int j = 0; j < 8; j++) {
        int r;
        if (!g_hk[j].ba || g_hk[j].seen) continue;
        r = spc_hack_state(w, g_hk[j].ba, g_hk[j].hacked, g_hk[j].ba_nation);
        if (r == 0) { g_hk[j].seen = 1; continue; }
        c7_add("sp_hackcover", r == 1 ? "won_all" : "lost_all", 1);
        if (g_hk[j].covered) c7_add("sp_hackcover", r == 1 ? "won_covered" : "lost_covered", 1);
        else c7_add("sp_hackcover", r == 1 ? "won_uncovered" : "lost_uncovered", 1);
        memset(&g_hk[j], 0, sizeof g_hk[j]);
    }
}
static int spc_hk_find(uint32_t ba) { for (int j = 0; j < 8; j++) if (g_hk[j].ba == ba) return j; return -1; }

/* ---- choose_target: the guard's weights (spec 10) ---- */
static float spc_target_mult(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Mem *m)
{
    SpcBrain *s = &b->spc;
    const Ma2PlayerView *pv;
    (void)me;
    if (s->act != SPC_HACKCOVER) return 1.0f;
    if (m->actor == s->ent) return 0.0f;                              /* never the prize */
    pv = player_by_actor(w, m->actor);
    if (s->won && b->now < s->win_t + 5.0 && m->kind == MA2_KIND_PILOT && hdist(m->pos, s->bpos) < 40.0f) return 3.0f;   /* the ejected pilot */
    if (pv && pv->attacking == s->aux) return 2.5f;
    if (hdist(m->pos, s->bpos) < 80.0f) return 1.5f;
    return 1.0f;
}

/* ---- Level: the building between us and a target that just ducked out of sight ---- */
static int spc_bld_look(Ma2Brain *b, const Ma2Senses *sn, uint32_t self_actor, uint32_t target, Ma2Vec3 from, Ma2Vec3 to, Ma2BldView *out)
{
    double t0; int r;
    if (!sn || !sn->bld_ray || b->rays < 2.0f) return 0;
    if (raycap_on()) {
        if (b->now != g_ray_now) { g_ray_now = b->now; g_ray_ms = 0.0; }
        if (g_ray_ms > RAY_FRAME_MS) return 0;
    }
    b->rays -= 1.0f; b->rays_used++;
    t0 = c7_t0();
    memset(out, 0, sizeof *out);
    r = sn->bld_ray(sn->ctx, self_actor, target, from, to, out);
    if (raycap_on()) g_ray_ms += (c7_t0() - t0);
    if (r != 1 && out->entity && diag_on()) { static int nl; if (nl++ < 12) fprintf(stderr, "[SPC] t=%.1f bot=%d level: blocked by cls %08X (not a building)\n", b->now, b->index, out->cls); }
    return r == 1;
}
static void spc_level_look(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn)
{
    Mem *m = b->tgt;
    Ma2BldView v;
    float d, dps, reach = my_reach(me);
    (void)w;
    if (!m || !m->valid || m->visible || b->now - m->last_seen > 4.0 || b->now - m->last_seen < 0.4) return;
    if (m->kind == MA2_KIND_VTOL || m->kind == MA2_KIND_FLYER || m->kind == MA2_KIND_TURRET) return;
    d = hdist(me->pos, m->pos);
    if (d < 20.0f || d > reach * 0.9f) return;
    if (!spc_bld_look(b, sn, me->actor, m->actor, eye_of(me->pos, me->kind, 1.0f), eye_of(m->pos, m->kind, 0.6f), &v)) return;
    if (v.hp <= 0.0f || v.hp_max <= 0.0f || spc_imm(v.entity)) return;
    if (v.nation == me->nation) return;                                       /* merge safety: never level a building of our own side (team fire) */
    if (hdist(v.pos, m->pos) > 35.0f || hdist(me->pos, v.pos) > reach * 0.9f || hdist(me->pos, v.pos) >= d) return;   /* it is between us, and the target is close behind it */
    dps = my_effective_dps(me, hdist(me->pos, v.pos));
    c7_add("sp_level", "looks", 1);
    {   /* LevelReach (default on, RECOMP_BOT_LEVEL_REACH / [Bots] LevelReach): 30 of 30 looks were 'too tough' at 8 s of fire (11-02 sweep); the act
         * runs 10 s and gives up on its own when the building does not give, so 14 s of fire is worth a try. Buckets of hp/dps seconds say what is left. */
        float need = dps >= 1.0f ? v.hp / dps : 1e9f, cap = c7_on("levelreach", "RECOMP_BOT_LEVEL_REACH", "LevelReach", 1) ? 14.0f : 8.0f;
        c7_add("sp_level", need <= 8.0f ? "need_le8" : need <= 14.0f ? "need_le14" : need <= 30.0f ? "need_le30" : "need_gt30", 1);
        if (need > cap) { c7_add("sp_level", "too_tough", 1); return; }
    }
    sp_note("hidden behind a building");
    if (!sp_roll(b, SP_LEVEL, m->player)) return;
    {
        SpcBrain *s = &b->spc;
        memset(s, 0, sizeof *s);
        s->act = SPC_LEVEL; s->t0 = b->now; s->until = b->now + (dps >= 1.0f && v.hp / dps > 8.0f ? 17.0 : 10.0); s->ent = v.entity; s->aux = m->actor; s->aux_pl = m->player;
        s->bpos = v.pos; s->hp0 = s->hp_last = v.hp; s->hp_t = b->now;
        if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d sk=%d level: building %08X hp=%.0f/%.0f dps=%.0f d=%.0f target %08X %.0f m behind it\n", b->now, b->index, b->skill, v.entity, v.hp, v.hp_max, dps, hdist(me->pos, v.pos), m->actor, hdist(v.pos, m->pos));
    }
}

/* ---- Stomp ---- */
static int spc_gate(void) { static int g = -1; if (g < 0) { const char *e = getenv("RECOMP_BOT_STOMP_GATE"); g = e && *e && *e != '0'; } return g; }
static void spc_stomp_look(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    int gate = spc_gate();
    /* StompWide (default on, RECOMP_BOT_STOMP_WIDE / [Bots] StompWide): the look never found a pilot in 30 sweep cells (11-02: no sp_stomp counter at all) --
     * it wanted an enemy pilot within 8 m standing nearly still (3 m/s), and a pilot on foot jogs at 8-10 m/s. The action already leads the target
     * (point = pos + vel * 0.4 s), so a pilot within 12 m moving up to 9 m/s qualifies. */
    int wide = c7_on("stompwide", "RECOMP_BOT_STOMP_WIDE", "StompWide", 1);
    float rad = gate ? 14.0f : wide ? 12.0f : 8.0f;
    if (gate) g_sp_forcemask |= 1u << SP_STOMP;
    if (me->kind != MA2_KIND_MECH || me->hp_max <= 0.0f) return;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->alive || !p->actor || p->nation == me->nation || p->kind != MA2_KIND_PILOT) continue;
        if ((w->game_type == MA2_GT_LMS || w->game_type == MA2_GT_TEAM_LMS) && p->deaths >= 1 &&
            c7_on("lmsghostroam", "RECOMP_BOT_LMS_GHOST_ROAM", "LmsGhostRoam", 1)) continue;   /* LmsGhostRoam: an LMS ghost is out, not a stomp */
        if (hdist(p->pos, me->pos) > rad || fabsf(p->pos.y - me->pos.y) > 2.5f) continue;
        if (!gate && hlen(p->vel) > (wide ? 9.0f : 3.0f)) { c7_add("sp_stomp", "skip_fast", 1); continue; }
        if (!ma2_trav_ready() || ma2_trav_clear(p->pos.x, p->pos.z, b->now) < 1.0f) continue;     /* it stands in the open */
        c7_add("sp_stomp", "looks", 1);
        sp_note("pilot at our feet");
        if (!sp_roll(b, SP_STOMP, p->player)) return;
        {
            SpcBrain *s = &b->spc;
            memset(s, 0, sizeof *s);
            s->act = SPC_STOMP; s->t0 = b->now; s->until = b->now + 4.0; s->ent = p->actor; s->aux_pl = p->player; s->vhp0 = p->hp; s->dmin = 1e9f;
            if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d sk=%d stomp: pilot %08X d=%.1f hp=%.0f\n", b->now, b->index, b->skill, p->actor, hdist(p->pos, me->pos), p->hp);
        }
        return;
    }
}

/* ---- Hack cover + Peel-off triggers ---- */
static int spc_free_to_act(Ma2Brain *b, const Ma2PlayerView *me)
{
    int g = b->goal;
    if (me->clamped_to || me->qte_active || me->enemy_rider || me->riding) return 0;
    if (g == G_RETREAT || g == G_GET_VEHICLE || g == G_EJECT || g == G_HACK || g == G_DEFEND_HACK || g == G_SWAP || g == G_LIFT || g == G_RIDE || g == G_REPAIR) return 0;
    if (b->obj_lock || b->dmg_recent > 0.15f || hp_frac(me->hp, me->hp_max) < 0.5f) return 0;
    return me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK || me->kind == MA2_KIND_BATTLEARMOR;
}
static void spc_hack_look(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    if (!spc_free_to_act(b, me)) return;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *ba = &w->players[i], *hv;
        int j, helper = 0;
        if (ba->kind != MA2_KIND_BATTLEARMOR || !ba->alive || ba->nation != me->nation || ba->actor == me->actor || !ba->clamped_to || !ba->qte_active) continue;
        hv = player_by_actor(w, ba->clamped_to);
        if (!hv || !hv->alive || hv->nation == me->nation || hdist(hv->pos, me->pos) > 120.0f) continue;
        j = spc_hk_find(ba->actor);
        if (j >= 0 && g_hk[j].guard) continue;                                              /* one guard per hack */
        for (int k = 0; k < w->nplayers && k < MA2_WORLD_MAX_PLAYERS; k++) {
            const Ma2PlayerView *e = &w->players[k];
            if (e->alive && e->actor && e->nation != me->nation && e->actor != hv->actor && e->kind != MA2_KIND_NONE && hdist(e->pos, hv->pos) < 80.0f) { helper = 1; break; }
        }
        c7_add("sp_hackcover", "looks", 1);
        if (!helper) { c7_add("sp_hackcover", "no_helper", 1); continue; }
        sp_note("teammate hacking");
        if (!sp_roll(b, SP_HACKCOVER, hv->player)) return;
        {
            SpcBrain *s = &b->spc;
            memset(s, 0, sizeof *s);
            s->act = SPC_HACKCOVER; s->t0 = b->now; s->until = b->now + 20.0; s->ent = hv->actor; s->aux = ba->actor; s->aux_pl = hv->player; s->bpos = hv->pos;
            if (j >= 0) { g_hk[j].guard = b->index + 1; g_hk[j].covered = 1; }
            if (ba->is_human) c7_add("sp_hackcover", "for_human", 1);
            if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d sk=%d hackcover: BA %08X hacks %08X d=%.0f\n", b->now, b->index, b->skill, ba->actor, hv->actor, hdist(hv->pos, me->pos));
        }
        return;
    }
}
static void spc_peel_look(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    if (!spc_free_to_act(b, me) || me->kind == MA2_KIND_BATTLEARMOR) return;
    if (g_spc_regular_banned && (b->skill & 3) == MA2_SKILL_REGULAR) return;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *t = &w->players[i], *r;
        Mem *m;
        float d;
        int taken = 0;
        if (t == me || !t->alive || t->nation != me->nation || !t->enemy_rider || (t->kind != MA2_KIND_MECH && t->kind != MA2_KIND_TANK)) continue;
        r = player_by_actor(w, t->enemy_rider);
        if (!r || !r->alive || r->nation == me->nation) continue;
        m = mem_by_actor(b, r->actor);
        d = hdist(me->pos, r->pos);
        if (!m || !m->visible || d < 25.0f || d > 120.0f) continue;
        for (int k = 0; k < 8; k++) if (g_spc_peel[k].rider == r->actor && b->now - g_spc_peel[k].t < 12.0) taken = 1;
        if (taken) continue;                                                               /* one shooter per rider */
        c7_add("sp_peeloff", "looks", 1);
        sp_note("enemy BA clamped on a teammate");
        if (!sp_roll(b, SP_PEELOFF, r->player)) return;
        {
            SpcBrain *s = &b->spc;
            int k, old = 0;
            memset(s, 0, sizeof *s);
            s->act = SPC_PEELOFF; s->t0 = b->now; s->until = b->now + 8.0; s->ent = r->actor; s->aux = t->actor; s->aux_pl = r->player; s->hp0 = t->hp;
            s->fire_t = b->now;
            for (k = 0; k < 8; k++) if (g_spc_peel[k].t < g_spc_peel[old].t) old = k;
            g_spc_peel[old].rider = r->actor; g_spc_peel[old].guard = b->index + 1; g_spc_peel[old].t = b->now;
            if (t->is_human) c7_add("sp_peeloff", "for_human", 1);
            if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d sk=%d peeloff: rider %08X on %08X (hp %.0f) d=%.0f\n", b->now, b->index, b->skill, r->actor, t->actor, t->hp, d);
        }
        return;
    }
}

/* Gate diagnostic for the stomp test: what the pilots at a 'Mech's feet look like (once per 'Mech per 15 s while the hook is on). */
/* ---- the per-tick trigger pass ---- */
static void spc_tick(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, int self, const Ma2Senses *sn)
{
    SpcBrain *s = &b->spc;
    (void)self;
    if (!sp_master_on()) { s->act = SPC_NONE; return; }
    { static int tuned;                                                                     /* lead 10-30: the package shares a ~5 % time budget, task A already uses ~4 %: halve the spec's p */
      if (!tuned) { tuned = 1;
        sp_set_params(SP_LEVEL, 4.0f, 0.0f, 0.0f, 0.0f, 0.25f);
        sp_set_params(SP_STOMP, 2.0f, 0.0f, 0.0f, 0.0f, 0.30f);
        sp_set_params(SP_HACKCOVER, 3.0f, 0.0f, 0.10f, 0.18f, 0.25f);
        sp_set_params(SP_PEELOFF, 3.0f, 0.0f, 0.08f, 0.15f, 0.22f); } }
    spc_world(w);
    if (!me->alive || me->kind == MA2_KIND_NONE || !me->actor) { s->act = SPC_NONE; s->after_t = 0.0; return; }
    if (s->act) c7_add("sp_all", "trick_s", b->dt);
    if (s->after_t > 0.0) {                                                                 /* Level aftermath: was the target exposed / hit? */
        Mem *m = mem_by_actor(b, s->after_tgt);
        if (b->now - s->after_t > 5.0) s->after_t = 0.0;
        else if (m) {
            if (!s->after_exp && b->now - s->after_t <= 3.0 && m->visible && m->last_seen > s->after_t) { s->after_exp = 1; c7_add("sp_level", "exposed", 1); }
            if (!s->after_hit && m->out_t > s->after_t) { s->after_hit = 1; c7_add("sp_level", "hit_after", 1); }
        }
    }
    if (s->act) return;
    if (b->now < s->next_look) return;
    s->next_look = b->now + 0.4 + 0.2 * rng_f(&b->rng);
    {
        int sk = b->skill & 3;
        if ((g_spdef[SP_HACKCOVER].p[sk] > 0.0f || sp_force(SP_HACKCOVER)) && sp_on(SP_HACKCOVER)) { spc_hack_look(b, w, me); if (s->act) return; }
        if ((g_spdef[SP_PEELOFF].p[sk] > 0.0f || sp_force(SP_PEELOFF)) && sp_on(SP_PEELOFF)) { spc_peel_look(b, w, me); if (s->act) return; }
        if ((g_spdef[SP_STOMP].p[sk] > 0.0f || sp_force(SP_STOMP) || spc_gate()) && sp_on(SP_STOMP) && me->kind == MA2_KIND_MECH && !b->obj_lock) { spc_stomp_look(b, w, me); if (s->act) return; }
        if ((g_spdef[SP_LEVEL].p[sk] > 0.0f || sp_force(SP_LEVEL)) && sp_on(SP_LEVEL) && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK) &&
            !b->obj_lock && b->goal != G_RETREAT && b->goal != G_GET_VEHICLE && b->goal != G_EJECT && b->dmg_recent < 0.1f && hp_frac(me->hp, me->hp_max) > 0.5f)
            spc_level_look(b, w, me, sn);
    }
}

/* ---- the running trick overrides this tick's order ---- */
static void spc_begin_order(Ma2Order *o)
{
    int def = o->defend; uint8_t q = o->qte_press;
    memset(o, 0, sizeof *o);
    o->defend = def; o->qte_press = q;
}
static void spc_order(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Senses *sn, const Ma2CtrlStatus *st, Ma2Order *out)
{
    SpcBrain *s = &b->spc;
    if (!s->act) return;
    if (!me->alive) { s->act = SPC_NONE; return; }
    /* common hard exits */
    if (b->dmg_recent > 0.15f && s->act != SPC_STOMP) SPC_END(b, "end_hit");
    else if (hp_frac(me->hp, me->hp_max) < b->sk.retreat_hp || b->goal == G_RETREAT) SPC_END(b, "end_retreat");
    else if (b->obj_lock) SPC_END(b, "end_objective");
    else if (me->clamped_to || me->qte_active || me->enemy_rider || me->riding) SPC_END(b, "end_busy");
    if (!s->act) return;
    /* HackCoverEnd (default on, RECOMP_BOT_HACKCOVER_END / [Bots] HackCoverEnd): a decided hack kept the cover open (to finish the ejected
     * pilot) with no exit, so a LOST hack held it forever: r4_85 b8's tank stood 75-117 s in "hackcover" with no target (the helper search
     * ended at 85.4 and end_no_helper is skipped once decided), enemy 'Mechs at 40-56 m. After the result: only while a won hack still waits
     * on its pilot, and never past 15 s. [C7] sp_hackcover: end_decided. */
    if (s->act == SPC_HACKCOVER && s->win_t && b->now >= s->until && (!s->won || !s->wait_pilot || b->now > s->win_t + 15.0) &&
        c7_on("hackcoverend", "RECOMP_BOT_HACKCOVER_END", "HackCoverEnd", 1)) {
        c7_add("sp_hackcover", "end_decided", 1); SPC_END(b, "end_decided"); return;
    }
    if (b->now >= s->until && !(s->act == SPC_HACKCOVER && s->win_t)) {
        if (s->act == SPC_HACKCOVER) SPC_END(b, "end_time"); else SPC_END(b, "gave_up");
        return;
    }
    switch (s->act) {
    case SPC_LEVEL: {
        Ma2BldView v;
        Mem *m = mem_by_actor(b, s->aux);
        int ok = sn && sn->bld_get && sn->bld_get(sn->ctx, s->ent, &v);
        if (!ok || v.hp <= 0.0f) {                                                          /* it is down */
            double a_t = b->now; uint32_t a_g = s->aux;
            c7_add("sp_level", "levelled", 1);
            if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d level: building %08X down after %.1f s\n", b->now, b->index, s->ent, b->now - s->t0);
            spc_finish(b);
            s->after_t = a_t; s->after_tgt = a_g; s->after_exp = s->after_hit = 0;
            return;
        }
        s->bpos = v.pos;
        if (v.hp < s->hp_last - 0.5f) { s->hurt = 1; s->hp_t = b->now; }
        s->hp_last = v.hp;
        if (!s->hurt && b->now - s->t0 > 3.5) {                                             /* our fire does nothing to it: indestructible or below the damage threshold */
            if (g_spc_nimm < 8) g_spc_imm[g_spc_nimm++].e = s->ent;
            c7_add("sp_level", "immune", 1);
            SPC_END(b, "end_immune");
            return;
        }
        if (m && m->visible && b->now - s->t0 > 0.5) { c7_add("sp_level", "target_back", 1); SPC_END(b, "end_seen"); return; }
        spc_begin_order(out);
        out->type = MA2_ORD_TRAVEL; out->stop = 3.0f; out->hold_fire = 1;
        out->structure = s->ent; out->structure_pos = s->bpos;
        if (b->now >= s->strafe_t) {                                                        /* hold, or step 6 m sideways every few seconds */
            Ma2Vec3 dv = hnorm(vsub(s->bpos, me->pos));
            s->side = s->side ? -s->side : 1; s->strafe_t = b->now + 2.5;
            s->pt = vadd(me->pos, v3(dv.z * (float)s->side * 6.0f, 0.0f, -dv.x * (float)s->side * 6.0f));
            if (!ma2_trav_ready() || ma2_trav_clear(s->pt.x, s->pt.z, b->now) < 4.0f) s->pt = me->pos;
        }
        out->point = s->pt;
        snprintf(b->note, sizeof b->note, "level");
        if (st && st->fired) c7_add("sp_level", "fire_ticks", 1);
        break;
    }
    case SPC_STOMP: {
        const Ma2PlayerView *p = player_by_actor(w, s->ent);
        Mem *m = mem_by_actor(b, s->ent);
        float d;
        if (!p || !p->alive) {                                                              /* dead: crushed, or our guns got it */
            int gun = m && m->out_t > s->t0 && b->now - m->out_t < 1.0;
            c7_add("sp_stomp", gun ? "killed" : "crushed", 1);
            if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d stomp: pilot dead (%s) dmin=%.1f\n", b->now, b->index, gun ? "guns" : "no weapon hit from us = crushed", s->dmin);
            spc_finish(b); return;
        }
        if (p->kind != MA2_KIND_PILOT) { c7_add("sp_stomp", "hijacked_anyway", 1); SPC_END(b, "end_boarded"); return; }
        d = hdist(p->pos, me->pos);
        if (d < s->dmin) s->dmin = d;
        if (d < 3.5f) {
            s->hurt = 1;
            if (p->hp < s->vhp0 - 0.5f) {
                if (!(m && m->out_t > b->now - 0.6)) { c7_add("sp_stomp", "contact_damage", 1); g_spc_contact_dmg++; }
                if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d stomp: pilot hp %.0f -> %.0f at d=%.1f%s\n", b->now, b->index, s->vhp0, p->hp, d, m && m->out_t > b->now - 0.6 ? " (our gun)" : " (CONTACT)");
                s->vhp0 = p->hp;
            }
        }
        if (d > 12.0f) { if (s->hurt) c7_add("sp_stomp", "touched", 1); c7_add("sp_stomp", "escaped", 1); SPC_END(b, "end_escaped"); return; }
        if (b->now >= s->until - 0.05) { if (s->hurt) c7_add("sp_stomp", "touched", 1); }
        spc_begin_order(out);
        out->type = MA2_ORD_TRAVEL; out->stop = 0.4f; out->target = p->actor; out->fire = 1;
        out->point = vadd(p->pos, vscale(p->vel, 0.4f)); out->point.y = p->pos.y;
        snprintf(b->note, sizeof b->note, "stomp");
        break;
    }
    case SPC_HACKCOVER: {
        int r = s->win_t ? 0 : spc_hack_state(w, s->aux, s->ent, me->nation);
        const Ma2PlayerView *hv = player_by_actor(w, s->ent), *best = NULL;
        float bs = -1e9f;
        if (r && !s->win_t) { s->win_t = b->now; s->won = r == 1; c7_add("sp_hackcover", r == 1 ? "hack_won" : "hack_lost", 1); s->until = b->now + 5.0; }
        if (hv) s->bpos = hv->pos;
        for (int k = 0; k < w->nplayers && k < MA2_WORLD_MAX_PLAYERS; k++) {                /* the helper to deal with */
            const Ma2PlayerView *e = &w->players[k];
            float sc;
            if (!e->alive || !e->actor || e->nation == me->nation || e->actor == s->ent || e->kind == MA2_KIND_NONE) continue;
            if (s->won && e->kind == MA2_KIND_PILOT && hdist(e->pos, s->bpos) < 40.0f) sc = 200.0f - hdist(e->pos, me->pos);
            else { float dh = hdist(e->pos, s->bpos); if (dh > 80.0f) continue; sc = (e->attacking == s->aux ? 100.0f : 0.0f) - dh; }
            if (sc > bs) { bs = sc; best = e; }
        }
        if (!best) { if (s->no_helper_t <= 0.0) s->no_helper_t = b->now; else if (b->now - s->no_helper_t > 4.0 && !s->win_t) { SPC_END(b, "end_no_helper"); return; } }
        else s->no_helper_t = 0.0;
        for (int k = 0; k < sn->nout && k < MA2_SENSE_MAX_EVENTS; k++) {                    /* what our hits did */
            const Ma2DmgEvent *ev = &sn->out[k];
            if (ev->actor == s->ent) { c7_add("sp_hackcover", "prize_hits", 1); if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d hackcover: PRIZE HIT %.0f\n", b->now, b->index, ev->amount); }
            else if (!s->helper_hit) {
                const Ma2PlayerView *ev_p = player_by_actor(w, ev->actor);
                if (ev_p && ev_p->attacking == s->aux) { s->helper_hit = 1; c7_add("sp_hackcover", "helper_hit", 1); }
            }
        }
        if (s->won && best && best->kind == MA2_KIND_PILOT) s->wait_pilot = 1;
        if (s->wait_pilot && !best) { c7_add("sp_hackcover", "pilot_killed", 1); s->wait_pilot = 0; }
        spc_begin_order(out);
        if (b->now >= s->strafe_t) {                                                        /* a firing spot 45-65 m from the hacked 'Mech, on the helper's side */
            Ma2Vec3 away = hnorm(vsub(me->pos, s->bpos)), side = best ? hnorm(vsub(best->pos, s->bpos)) : away, mix = hnorm(vadd(vscale(away, 0.5f), vscale(side, 0.5f)));
            float dist = clampf(hdist(me->pos, s->bpos), 45.0f, 65.0f);
            s->strafe_t = b->now + 3.0;
            s->pt = vadd(s->bpos, vscale(hlen(mix) > 0.1f ? mix : away, dist)); s->pt.y = me->pos.y;
            if (!ma2_trav_ready() || ma2_trav_clear(s->pt.x, s->pt.z, b->now) < 3.0f || hdist(me->pos, s->pt) < 6.0f) s->pt = me->pos;
        }
        out->type = MA2_ORD_FIGHT_MOVE; out->point = s->pt; out->stop = 4.0f;
        out->target = best ? best->actor : 0; out->fire = best != NULL;
        snprintf(b->note, sizeof b->note, "hackcover");
        break;
    }
    case SPC_PEELOFF: {
        const Ma2PlayerView *r = player_by_actor(w, s->ent), *t = player_by_actor(w, s->aux);
        if (!r || !r->alive) { c7_add("sp_peeloff", "knocked_off", 1); if (t && t->alive && !t->qte_active) c7_add("sp_peeloff", "hack_saved", 1); SPC_END(b, "end_rider_gone"); return; }
        if (!t || !t->alive || t->enemy_rider != s->ent) {                                  /* off by itself, or the teammate is gone */
            if (t && t->alive) { c7_add("sp_peeloff", "knocked_off", 1); c7_add("sp_peeloff", "hack_saved", 1); }
            SPC_END(b, "end_rider_gone"); return;
        }
        for (int k = 0; k < sn->nout && k < MA2_SENSE_MAX_EVENTS; k++) {
            const Ma2DmgEvent *ev = &sn->out[k];
            if (ev->actor == s->ent) c7_add("sp_peeloff", "hits_on_rider", 1);
            else if (ev->actor == s->aux) {                                                 /* our shot hurt the teammate */
                s->ally_cost += ev->amount / maxf(t->hp_max, 1.0f);
                if (!s->f_ally) { s->f_ally = 1; c7_add("sp_peeloff", "ally_hit", 1); if ((b->skill & 3) == MA2_SKILL_REGULAR) g_spc_regular_banned = 1; }
                if (diag_on()) fprintf(stderr, "[SPC] t=%.1f bot=%d peeloff: ALLY HIT %.0f (cost %.0f%%)\n", b->now, b->index, ev->amount, s->ally_cost * 100.0f);
            }
        }
        if (s->ally_cost > 0.10f) { c7_add("sp_peeloff", "ally_over10", 1); SPC_END(b, "end_ally_cost"); return; }
        if (st && st->fired) { s->fire_t = b->now; c7_add("sp_peeloff", "fire_ticks", 1); }
        else if (b->now - s->fire_t > 4.0) { c7_add("sp_peeloff", "no_shot", 1); SPC_END(b, "gave_up"); return; }   /* no line / no direct-fire group */
        spc_begin_order(out);
        if (b->now >= s->strafe_t) {                                                        /* step toward a line where the rider shows beside the 'Mech */
            Ma2Vec3 dv = hnorm(vsub(t->pos, me->pos)), off = vsub(r->pos, t->pos);
            float lat = off.x * dv.z - off.z * dv.x;
            s->side = lat > 0.5f ? 1 : lat < -0.5f ? -1 : (s->side ? -s->side : 1);
            s->strafe_t = b->now + 2.5;
            s->pt = vadd(me->pos, v3(dv.z * (float)s->side * 12.0f, 0.0f, -dv.x * (float)s->side * 12.0f));
            if (!ma2_trav_ready() || ma2_trav_clear(s->pt.x, s->pt.z, b->now) < 4.0f) s->pt = me->pos;
        }
        out->type = MA2_ORD_FIGHT_MOVE; out->point = s->pt; out->stop = 2.0f; out->target = r->actor; out->fire = 1;
        out->ff_skip = t->actor; out->ff_rider = r->actor;
        snprintf(b->note, sizeof b->note, "peeloff");
        break;
    }
    default: s->act = SPC_NONE; break;
    }
}
