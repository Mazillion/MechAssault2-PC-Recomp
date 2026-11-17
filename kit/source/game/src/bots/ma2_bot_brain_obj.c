/* ma2_bot_brain_obj.c - game-mode objective play (included by ma2_bot_brain.c).
 *
 * Reads Ma2World.objectives / it_player (filled by core, ma2_bot_objective.c)
 * and scores G_OBJECTIVE in evaluate(); obj_order() turns it into a controller order.
 *
 * Roles (per team, by rank of the bot's player among its team's bots, with a
 * per-bot personality "zeal"): ATTACK takes the enemy flag / cores / far
 * checkpoints, DEFEND guards the own flag / base / held checkpoints and hunts
 * carriers, FLEX fights and escorts, taking objectives of opportunity.
 * Anyone carrying something always runs it home; anyone near a dropped own
 * flag returns it.
 */

enum { ROLE_ATTACK = 0, ROLE_DEFEND, ROLE_FLEX };
enum { OP_NONE = 0, OP_CARRY, OP_GRAB, OP_RETURN, OP_HUNT, OP_ESCORT, OP_GUARD, OP_CLAIM, OP_HOLD, OP_CHASE_IT, OP_DESTROY, OP_STAGE, OP_BOMB, OP_COUNT };
static const char *const k_role_name[3] = { "atk", "def", "flex" };
static const char *const k_op_name[OP_COUNT] = { "-", "carry", "grab", "return", "hunt", "escort", "guard", "claim", "hold", "chase-it", "destroy", "stage", "bomb" };

#define MA2B_OBJ_LOCK_RANGE   70.0f   /* grab/return: keep the body (no AI hand-off) inside this */

static int obj_team_rank(const Ma2World *w, const Ma2PlayerView *me, int *count)
{
    int i, rank = 0, n = 0;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->is_bot || p->nation != me->nation || p->player == p->actor) continue;
        n++;
        if (p->player < me->player) rank++;
    }
    if (count) *count = n;
    return rank;
}

/* ATTACK, DEFEND, ATTACK, FLEX, DEFEND, ATTACK, FLEX, ... ; a lone bot attacks. */
static int obj_role(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me)
{
    static const int pattern[6] = { ROLE_ATTACK, ROLE_DEFEND, ROLE_ATTACK, ROLE_FLEX, ROLE_DEFEND, ROLE_FLEX };
    int n = 0, r = obj_team_rank(w, me, &n);
    int role = n <= 1 ? ROLE_ATTACK : pattern[r % 6];
    /* Personality: a very aggressive flex player attacks, a timid one defends. */
    if (role == ROLE_FLEX && b->obj_zeal > 1.2f) role = ROLE_ATTACK;
    else if (role == ROLE_FLEX && b->obj_zeal < 0.8f) role = ROLE_DEFEND;
    b->obj_rank = r;
    return role;
}

static const Ma2ObjectiveView *obj_find(const Ma2World *w, int kind, int nation, int same)
{
    int i;
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        if (o->kind != kind) continue;
        if (same ? o->nation == nation : o->nation != nation) return o;
    }
    return NULL;
}

static int obj_is_teammate(const Ma2World *w, uint32_t player, int nation)
{
    int i = find_player(w, player);
    return i >= 0 && w->players[i].nation == nation;
}

static void obj_set(Ma2Brain *b, int op, Ma2Vec3 pt, float stop, float score, float *s, uint32_t ent)
{
    if (score <= s[G_OBJECTIVE]) return;
    for (int i = 0; i < 4; i++)   /* ft-bots ObjGiveUp: a target this bot found unreachable lately is not offered again */
        if (b->obj_bad[i].until > b->now && b->obj_bad[i].op == op && (ent ? b->obj_bad[i].ent == ent : hdist(b->obj_bad[i].pt, pt) < 12.0f)) {
            c7_add("objgiveup", "skipped", 1);
            return;
        }
    s[G_OBJECTIVE] = score;
    b->obj_op = op; b->obj_pt = pt; b->obj_stop = stop; b->obj_ent = ent;
}

/* F78: whether this bot knows yet that `carrier` (a player) has something that scores, and where it
 * looks for him (*pt). A stolen flag is noticed after a skill delay (Recruit 4-6 s .. Elite 0.3-0.6 s)
 * unless the carrier is in sight or within 40 m; then the bot hunts its memory of him (seen, heard,
 * radar, call-outs), else the carrier's marker as of its last look at it (every 5 s Recruit .. 1.5 s
 * Elite). Off: the live position at once (the old way). */
static const char *const k_f78_n[4] = { "n_rec", "n_reg", "n_vet", "n_eli" }, *const k_f78_s[4] = { "s_rec", "s_reg", "s_vet", "s_eli" };
static int f78_where(Ma2Brain *b, const Ma2PlayerView *me, uint32_t carrier, Ma2Vec3 live, Ma2Vec3 *pt)
{
    static const float dmin[4] = { 4.0f, 2.0f, 1.0f, 0.3f }, dmax[4] = { 6.0f, 3.0f, 1.6f, 0.6f };
    static const float look[4] = { 5.0f, 3.5f, 2.5f, 1.5f };
    int sk = b->skill & 3, i, slot = -1, old = 0;
    Mem *m;
    if (!f78_on()) { *pt = live; return 1; }
    for (i = 0; i < 4; i++) {
        if (b->f78[i].carrier == carrier && b->now >= b->f78[i].last && b->now - b->f78[i].last < 1.5) { slot = i; break; }
        if (b->f78[i].last < b->f78[old].last) old = i;
    }
    if (slot < 0) {   /* a new carrier (or one who dropped it a while ago) */
        slot = old; memset(&b->f78[slot], 0, sizeof b->f78[slot]);
        b->f78[slot].carrier = carrier; b->f78[slot].since = b->now;
        b->f78[slot].notice_at = b->now + rng_range(&b->rng, dmin[sk], dmax[sk]);
    }
    b->f78[slot].last = b->now;
    m = mem_get(b, carrier, 0);
    if (!b->f78[slot].noticed) {
        const char *why = m && m->visible ? "seen" : hdist(me->pos, live) < 40.0f ? "near" : b->now >= b->f78[slot].notice_at ? "delay" : NULL;
        if (!why) return 0;
        b->f78[slot].noticed = 1; b->f78[slot].pos = live; b->f78[slot].pos_t = b->now;
        c7_add("f78", "notices", 1); c7_add("f78", why, 1);
        c7_add("f78", k_f78_n[sk], 1); c7_add("f78", k_f78_s[sk], b->now - b->f78[slot].since);
        if (diag_on()) fprintf(stderr, "[BOT-F78] t=%.1f bot=%d sk=%d notices carrier %08X after %.2f s (%s) d=%.0f\n",
                               b->now, b->index, b->skill, carrier, b->now - b->f78[slot].since, why, hdist(me->pos, live));
    }
    if (m && m->valid && mem_conf(b, m) > 0.5f) { *pt = mem_predict(b, m); return 1; }
    if (b->now - b->f78[slot].pos_t >= look[sk]) { b->f78[slot].pos = live; b->f78[slot].pos_t = b->now; }
    *pt = b->f78[slot].pos;
    return 1;
}
/* F78: the hunt's zeal shrunk so skill decides who goes (zeal 0.75..1.25 was as wide as the whole
 * skill spread). */
static float f78_zeal(const Ma2Brain *b) { return f78_on() ? 1.0f + 0.35f * (b->obj_zeal - 1.0f) : b->obj_zeal; }

/* F46: CTF attackers meet at a staging point short of the enemy flag and go in together (a lone
 * runner dies before the grab or right after it); flex players escort the attacker nearest the
 * flag instead of trickling in one by one. Switch RECOMP_BOT_CTF_GROUP / [Bots] CtfGroup. */
static int ctf_group_on(void) { return c7_on("ctf", "RECOMP_BOT_CTF_GROUP", "CtfGroup", 1); }

static Ma2Vec3 ctf_stage_pt(const Ma2ObjectiveView *mine, const Ma2ObjectiveView *theirs)
{
    Ma2Vec3 d = vsub(mine->home, theirs->home);
    float len = hlen(d);
    if (len < 1.0f) return theirs->home;
    return vadd(theirs->home, vscale(d, minf(170.0f, 0.4f * len) / len));
}

/* 1 = staging (G_OBJECTIVE set to the meeting point), 0 = go for the flag. */
static int ctf_stage(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2ObjectiveView *mine,
                     const Ma2ObjectiveView *theirs, float *s)
{
    static const float wait_s[4] = { 0.0f, 8.0f, 12.0f, 15.0f };   /* by skill: Recruits run in alone */
    float wait = wait_s[b->skill & 3];
    Ma2Vec3 st;
    int i, near = 0, nteam = 0, need;
    if (wait <= 0.0f || hdist(mine->home, theirs->home) < 220.0f) return 0;   /* small map: just go */
    if (b->now < b->ctf_go_until) return 0;                                    /* pushing in now */
    st = ctf_stage_pt(mine, theirs);
    /* StageLatch (default on, RECOMP_BOT_STAGE_LATCH / [Bots] StageLatch): "already past it" had no latch, so a bot on the 15 m line went
     * grab / stage / grab every evaluation: r1_63 (CTF Ill Wind) b5 flipped ~38 times in 27 s at 848,-4,1003 and lost 520 hp. Past it is a
     * push like the staged go: 45 s. [C7] ctf: past_latched. */
    if (hdist(me->pos, theirs->home) < hdist(st, theirs->home) - 15.0f) {
        b->ctf_stage_t = 0.0;
        if (c7_on("stagelatch", "RECOMP_BOT_STAGE_LATCH", "StageLatch", 1)) { b->ctf_go_until = b->now + 45.0; c7_add("ctf", "past_latched", 1); }
        return 0;
    }   /* already past it */
    /* StageComing (default on, RECOMP_BOT_STAGE_COMING / [Bots] StageComing; design 11-14): only a teammate who can get here inside the wait
     * counts (there already, or a ground unit within 300 m of the stage point or of me, not carrying their flag). The idle player at our spawn,
     * a pilot respawning 700 m back and our gunship fighting 250 m off all counted, so a 2v2 attacker stood the whole 12 s at the stage point
     * waiting for nobody (c34bd_a1 bot 1, t=122-134 and 210-222). Nobody coming: go now, latched like the staged go. [C7] ctf: stage_nobody. */
    int scoming = c7_on("stagecoming", "RECOMP_BOT_STAGE_COMING", "StageComing", 1);
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q->player == me->player || q->nation != me->nation || !q->alive || !q->actor) continue;
        int qnear = hdist(q->pos, st) < 70.0f || hdist(q->pos, me->pos) < 50.0f;
        if (scoming && !qnear && ((hdist(q->pos, st) > 300.0f && hdist(q->pos, me->pos) > 300.0f) || (theirs->carrier && theirs->carrier == q->player) ||
                                  kind_ok(q->kind) == MA2_KIND_VTOL)) continue;
        nteam++;
        if (qnear) near++;   /* there, or coming with me */
    }
    if (nteam == 0) {
        if (scoming && hdist(me->pos, st) < 50.0f) { c7_add("ctf", "stage_nobody", 1); b->ctf_go_until = b->now + 45.0; }
        b->ctf_stage_t = 0.0; return 0;
    }
    need = 1;
    /* StageUnderFire (default on, RECOMP_BOT_STAGE_FIRE / [Bots] StageUnderFire): the stage hold (1.02-1.15) out-scored cover and attack,
     * so a bot waiting there stood and died: r1_62 (CTF Going Commando) b2 in the pit at 524,7,735, 850 -> 0 hp twice (24.5-32.8,
     * 182.4-194.5), OUTNUMBERED, goal never changed. Hit in the last 2 s at the stage point: no hold for 6 s (it fights or takes cover).
     * [C7] ctf: stage_fire_breaks. */
    if (hdist(me->pos, st) < 50.0f && b->now - b->last_hit < 2.0 && c7_on("stagefire", "RECOMP_BOT_STAGE_FIRE", "StageUnderFire", 1)) {
        b->ctf_go_until = b->now + 6.0; b->ctf_stage_t = 0.0; c7_add("ctf", "stage_fire_breaks", 1);
        return 0;
    }
    if (hdist(me->pos, st) < 50.0f) {
        if (b->ctf_stage_t <= 0.0) { b->ctf_stage_t = b->now; c7_add("ctf", "stages", 1); }
        if (near >= need || b->now - b->ctf_stage_t >= wait) {
            c7_add("ctf", near >= need ? "go_full" : "go_timeout", 1);
            b->ctf_go_until = b->now + 45.0; b->ctf_stage_t = 0.0;
            return 0;
        }
        c7_add("ctf", "stage_s", w->dt);
    }
    obj_set(b, OP_STAGE, st, 12.0f, 1.15f * b->obj_zeal, s, theirs->entity);
    return 1;
}

/* The teammate nearest the enemy flag (at least 40 m nearer than me), or -1. */
static int ctf_lead(const Ma2World *w, const Ma2PlayerView *me, const Ma2ObjectiveView *theirs)
{
    int i, best = -1;
    float bd = hdist(me->pos, theirs->home) - 40.0f;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        float d;
        if (q->player == me->player || q->nation != me->nation || !q->alive || !q->actor) continue;
        d = hdist(q->pos, theirs->home);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* W3 (push 2026-10-27): CTF capture and grab commit.
 * CtfCapCommit: a flag carrier within 25 m of its own stand (own flag home) goes onto the stand -- the carry order
 *   outranks every fight / cover / power-up score, so combat strafing and standoff never decide the last 25 m
 *   (it still fires while moving). RECOMP_BOT_CTF_CAP_COMMIT / [Bots] CtfCapCommit.
 * CtfGrabCommit: a bot within 20 m of a free enemy flag (home or dropped) walks onto it (the pickup is a touch)
 *   instead of lingering in a fight. RECOMP_BOT_CTF_GRAB_COMMIT / [Bots] CtfGrabCommit.
 * [C7] ctfcap: zone_enter carriers entering 25 m, zone_s carrier seconds inside, cap_n / cap_s captures from inside and
 *   their seconds from entering the zone, orbit_turns / circles = turns around the stand (circles = episodes >= 1 turn),
 *   zone_deaths carriers killed inside, hold_s seconds inside 25 m waiting for our own flag (cannot capture), left_n.
 * [C7] ctfgrab: zone_enter, zone_s bot-seconds within 20 m of a free enemy flag, fight_s of those with a non-objective goal,
 *   grab_n / grab_s pickups from inside and the seconds from entering, lost_n left without the pickup, zone_deaths. */
#define CTF_CAP_R  25.0f
#define CTF_GRAB_R 20.0f
static int ctf_capc_on(void) { return c7_on("ctfcap", "RECOMP_BOT_CTF_CAP_COMMIT", "CtfCapCommit", 1); }
static int ctf_grabc_on(void) { return c7_on("ctfgrab", "RECOMP_BOT_CTF_GRAB_COMMIT", "CtfGrabCommit", 1); }
/* CapAnyway (see obj_ctf): MechAssault 2 scores a capture with the carrier's own flag away; also lifts RunNeedsHome (frun_consider) */
static int cap_anyway_on(void) { return c7_on("capanyway", "RECOMP_BOT_CTF_CAP_ANYWAY", "CapAnyway", 1); }

static void ctf_capz_close(Ma2Brain *b, int how)   /* 1 captured, 2 left the zone / lost the flag, 3 died */
{
    double secs = b->now - b->capz_t0;
    float turns = b->capz_ang / 6.2831853f;
    if (!b->capz_in) return;
    b->capz_in = 0; b->capz_have = 0;
    c7_add("ctfcap", "orbit_turns", turns);
    if (turns >= 1.0f) c7_add("ctfcap", "circles", 1);
    if (how == 1) { c7_add("ctfcap", "cap_n", 1); c7_add("ctfcap", "cap_s", secs); }
    else if (how == 3) c7_add("ctfcap", "zone_deaths", 1);
    else c7_add("ctfcap", "left_n", 1);
}
static void ctf_grbz_close(Ma2Brain *b, int how)   /* 1 picked up, 2 left / lost, 3 died */
{
    double secs = b->now - b->grbz_t0;
    if (!b->grbz_in) return;
    b->grbz_in = 0;
    if (how == 1) { c7_add("ctfgrab", "grab_n", 1); c7_add("ctfgrab", "grab_s", secs); }
    else if (how == 3) c7_add("ctfgrab", "zone_deaths", 1);
    else c7_add("ctfgrab", "lost_n", 1);
}
static void ctf_zone_died(Ma2Brain *b)
{
    if (b->capz_in) ctf_capz_close(b, 3);
    if (b->grbz_in) ctf_grbz_close(b, 3);
}

/* Per-tick bookkeeping (counters only; no decisions). */
static void ctf_zone_tick(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2ObjectiveView *mine, const Ma2ObjectiveView *theirs)
{
    int carrying = theirs->carrier == me->player;
    float dt = (float)(b->now - b->zone_last), ds = hdist(me->pos, mine->home);
    if (dt < 0.0f || dt > 0.5f) dt = 0.25f;   /* evaluated about every MA2B_EVAL_PERIOD, sometimes sooner (interrupts) */
    b->zone_last = b->now; b->zone_dt = dt;
    if (carrying && ds < CTF_CAP_R) {
        float a = atan2f(me->pos.z - mine->home.z, me->pos.x - mine->home.x);
        if (!b->capz_in) { b->capz_in = 1; b->capz_t0 = b->now; b->capz_ang = 0.0f; b->capz_have = 0; c7_add("ctfcap", "zone_enter", 1); }
        c7_add("ctfcap", "zone_s", dt);
        if (mine->state != MA2_OBJS_HOME) c7_add("ctfcap", "hold_s", dt);
        if (ds > 4.0f) {
            if (b->capz_have) { float da = a - b->capz_prev; while (da > 3.14159265f) da -= 6.2831853f; while (da < -3.14159265f) da += 6.2831853f; b->capz_ang += fabsf(da); }
            b->capz_prev = a; b->capz_have = 1;
        } else b->capz_have = 0;
    } else if (b->capz_in) {
        if (!carrying && theirs->state == MA2_OBJS_HOME && ds < 15.0f) ctf_capz_close(b, 1);
        else ctf_capz_close(b, 2);
    }
    {
        int free_ = theirs->state != MA2_OBJS_CARRIED;
        float d = hdist(me->pos, theirs->pos);
        if (free_ && d < CTF_GRAB_R && !carrying) {
            if (!b->grbz_in) { b->grbz_in = 1; b->grbz_t0 = b->now; c7_add("ctfgrab", "zone_enter", 1); }
            c7_add("ctfgrab", "zone_s", dt);
            if (b->goal != G_OBJECTIVE) c7_add("ctfgrab", "fight_s", dt);
        } else if (b->grbz_in) ctf_grbz_close(b, carrying ? 1 : 2);
    }
}

static void obj_ctf(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s, int foot)
{
    const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, me->nation, 1);
    const Ma2ObjectiveView *theirs = obj_find(w, MA2_OBJ_FLAG, me->nation, 0);
    float z = b->obj_zeal;
    if (!mine || !theirs) return;
    ctf_zone_tick(b, w, me, mine, theirs);
    if (theirs->carrier == me->player) {                      /* run it home */
        /* A capture needs our own flag on its stand. If it is gone, wait at
         * home -- or go take it back from a carrier close by.
         * CapAnyway (default on, RECOMP_BOT_CTF_CAP_ANYWAY / [Bots] CapAnyway; design 11-14): NOT in MechAssault 2. A carrier that comes within
         * ~10 m of its own stand scores whether or not its own flag is there (player stat 18 +1 and the team's 10 points: c34bd_b1 t=57.5,
         * c34bd_a1 t=54.6, c34de1 t=143.8, own flag carried by the enemy each time; 11 of 11 'carried -> home' events in 6 recordings came
         * with the carrier 9.4-10.9 m from its own stand). The hold below kept carriers 10-20 m off their stand (the guard patrol), so a
         * capture happened only when the patrol strayed onto it. Now the stand is walked onto as when our flag is home (our flag lying
         * dropped within 120 m is still touched home first: it is on the way). [C7] capanyway: carrier_s (seconds near home with ours away). */
        int wait_home = mine->state != MA2_OBJS_HOME && hdist(me->pos, mine->home) < 60.0f;
        if (wait_home && cap_anyway_on() && !(mine->state == MA2_OBJS_DROPPED && hdist(me->pos, mine->pos) < 120.0f)) {
            wait_home = 0; c7_add("capanyway", "carrier_s", b->zone_dt);
        }
        if (wait_home) {
            if (mine->state == MA2_OBJS_DROPPED && hdist(me->pos, mine->pos) < 120.0f)
                obj_set(b, OP_CARRY, mine->pos, 0.0f, 2.5f, s, theirs->entity);          /* touch = return it */
            else if (mine->carrier && hdist(me->pos, mine->pos) < 150.0f)
                obj_set(b, OP_CARRY, mine->pos, 25.0f, 2.5f, s, theirs->entity);         /* chase it, shooting */
            else obj_set(b, OP_HOLD, mine->home, 20.0f, 2.5f, s, theirs->entity);
        } else if (hdist(me->pos, mine->home) < CTF_CAP_R && ctf_capc_on()) {
            /* W3 CtfCapCommit: 25 m out, the stand outranks fight, cover and power-up scores (all < 2). */
            obj_set(b, OP_CARRY, mine->home, 1.5f, 4.0f, s, theirs->entity);
            c7_add("ctfcap", "commit_s", b->zone_dt);
        } else obj_set(b, OP_CARRY, mine->home, 3.0f, 2.5f, s, theirs->entity);
        return;
    }
    if (mine->state == MA2_OBJS_DROPPED) {
        float d = hdist(me->pos, mine->pos);
        if (b->obj_role != ROLE_ATTACK || d < 80.0f) obj_set(b, OP_RETURN, mine->pos, 0.0f, d < 80.0f ? 1.6f : 1.0f * z, s, mine->entity);
    }
    if (mine->state == MA2_OBJS_CARRIED && mine->carrier) {
        int ti = find_player(w, mine->carrier);
        Ma2Vec3 at;
        if (ti >= 0 && f78_where(b, me, mine->carrier, w->players[ti].pos, &at)) {
            float d = hdist(me->pos, at);
            float sc = b->obj_role == ROLE_ATTACK ? (d < 150.0f ? 1.0f : 0.0f) : 1.3f;
            if (theirs->carrier && b->obj_role == ROLE_ATTACK)   /* our runner needs escorts more -- (F78) unless the thief is right here */
                sc = f78_on() && d < 60.0f ? 1.25f : 0.0f;
            {   /* the carrier of our flag: the better the player, the more readily it goes after him */
                static const float prio[4] = { 0.85f, 1.0f, 1.15f, 1.3f }, prio78[4] = { 0.7f, 0.95f, 1.2f, 1.45f };
                sc *= f78_on() ? prio78[b->skill & 3] : prio[b->skill & 3];
                if (b->obj_role == ROLE_ATTACK && sc == 0.0f && b->skill >= 2 && d < 250.0f && !theirs->carrier) sc = 0.9f;   /* aces turn back for him */
            }
            if (sc > 0.0f) { obj_set(b, OP_HUNT, at, 8.0f, sc * f78_zeal(b), s, mine->entity); b->cd_chase_t = b->now; }
        }
    }
    if (foot) {   /* a defender on foot is off to a ride: where its guard bomb goes (obj_bomb_on_way) */
        if (b->obj_role == ROLE_DEFEND) { b->guard_pt = mine->home; b->guard_t = b->now; }
        return;
    }
    if (theirs->state == MA2_OBJS_CARRIED && theirs->carrier && obj_is_teammate(w, theirs->carrier, me->nation)) {
        if (b->obj_role != ROLE_DEFEND) {
            int ti = find_player(w, theirs->carrier);
            if (ti >= 0) {
                Ma2Vec3 off = vscale(dir_of_heading((float)b->index * 1.7f), 18.0f);
                obj_set(b, OP_ESCORT, vadd(w->players[ti].pos, off), 6.0f, 0.85f * z, s, theirs->entity);
            }
        }
    } else if (theirs->state != MA2_OBJS_CARRIED) {
        float d = hdist(me->pos, theirs->pos);
        /* DropGrab (default on, RECOMP_BOT_DROP_GRAB / [Bots] DropGrab): set down by the player beside their flag, a bot of any
         * role holds the grab out to 60 m for 25 s -- a defender jetted 31 m off went home to guard (a playtester's game 11-03, botrec
         * t=43.0). [C7] dropgrab: hold_s (outside 20 m). */
        int dropped = b->pdrop_t > 0.0 && b->now - b->pdrop_t < 25.0 && b->now >= b->pdrop_t &&
                      c7_on("dropgrab", "RECOMP_BOT_DROP_GRAB", "DropGrab", 1);
        if (getenv("RECOMP_TEST_HUMAN_FERRY") && theirs->state == MA2_OBJS_HOME && !dropped) {   /* test hook (push-y): with the player ferry test running, his team's bots leave their flag to him */
            for (int hi = 0; hi < w->nplayers && hi < MA2_WORLD_MAX_PLAYERS; hi++) if (w->players[hi].is_human && w->players[hi].nation == me->nation) goto defend;
        }
        if (getenv("RECOMP_TEST_BA_CARRIER")) {   /* test hook (LiftCarrier proof): only a BattleArmor goes for their flag at its stand */
            if (kind_ok(me->kind) == MA2_KIND_BATTLEARMOR) { obj_set(b, OP_GRAB, theirs->pos, 0.0f, 2.0f * z, s, theirs->entity); goto defend; }
            if (theirs->state == MA2_OBJS_HOME) goto defend;
        }
        if (theirs->state == MA2_OBJS_HOME && ctf_group_on()) {
            if (b->obj_role == ROLE_ATTACK && ctf_stage(b, w, me, mine, theirs, s)) goto defend;
            if (b->obj_role == ROLE_FLEX && d >= 100.0f && b->skill > 0) {
                int li = ctf_lead(w, me, theirs);
                if (li >= 0) {
                    const Ma2PlayerView *l = &w->players[li];
                    Ma2Vec3 back = vsub(mine->home, l->pos);
                    float bl = hlen(back);
                    Ma2Vec3 off = vscale(dir_of_heading((float)b->index * 1.7f), 10.0f);
                    if (bl > 1.0f) off = vadd(off, vscale(back, 15.0f / bl));
                    obj_set(b, OP_ESCORT, vadd(l->pos, off), 6.0f, 0.8f * z, s, theirs->entity);
                    c7_add("ctf", "lead_escort_s", w->dt);
                }
            }
        }
        float sc = b->obj_role == ROLE_ATTACK ? 1.15f :
                   b->obj_role == ROLE_FLEX ? (theirs->state == MA2_OBJS_DROPPED || d < 150.0f ? 1.0f : 0.55f) :
                   (theirs->state == MA2_OBJS_DROPPED && d < 60.0f ? 1.0f : 0.0f);
        if (sc > 0.0f) obj_set(b, OP_GRAB, theirs->pos, 0.0f, sc * z, s, theirs->entity);
        /* GrabReach (default on, RECOMP_BOT_GRAB_REACH / [Bots] GrabReach): the commit started at 20 m, so 20-30 m out a power-up (1.25)
         * or a retreat (1.20) beat the grab (1.15): bs62a b7 (CTF Going Commando) stood 25 m from the red flag at 52.7 s, took the
         * retreat and its hop; bs62b b0 the same at 25 m from blue (195.9 s). 0 grabs in 480 s. Commit from 30 m. [C7] ctfgrab: commit_s. */
        float commit_r = c7_on("grabreach", "RECOMP_BOT_GRAB_REACH", "GrabReach", 1) ? 30.0f : CTF_GRAB_R;
        if ((d < commit_r || (dropped && d < 60.0f)) && ctf_grabc_on()) {   /* W3 CtfGrabCommit: 20 m from a free flag, walking onto it beats any fight score */
            obj_set(b, OP_GRAB, theirs->pos, 0.0f, 2.6f, s, theirs->entity);
            c7_add("ctfgrab", "commit_s", b->zone_dt);
            if (d >= CTF_GRAB_R) c7_add("dropgrab", "hold_s", b->zone_dt);
        }
    }
defend:
    if (b->obj_role == ROLE_DEFEND) {
        float d = hdist(me->pos, mine->home);
        obj_set(b, OP_GUARD, mine->home, b->obj_guard_r, d > b->obj_guard_r ? 0.8f : 0.5f, s, mine->entity);
    }
}

/* WeakSide (default on, RECOMP_BOT_WEAK_SIDE / [Bots] WeakSide): a team that is behind kept the plan it started with, where a player
 * changes it. Side-balance sweep 11-07, map-design cells: Check It Deliverance team 2 (one terminal at 502 m, the next at 966) spread
 * its attackers over terminals by rank and held 226-290 s against 494-523; Snatch It No Man's Land team 2 lost 1-3 and 1-4. Behind =
 * Check It: the other team holds more terminals; Snatch It: more cores in their base than in ours; Base War: our plant 10 points lower
 * (hp share). The plans: Check It attackers go in pairs to the two nearest terminals; Snatch It attackers go in pairs, and cores in the
 * other base count as near as free ones (taking theirs back is the swing); Base War flex players defend the plant. [C7] weakside: s. */
static int weak_side(const Ma2World *w, const Ma2PlayerView *me)
{
    int i, ours = 0, theirs = 0;
    float our_hp = -1.0f, their_hp = -1.0f, our_max = 0.0f, their_max = 0.0f;   /* Base War: the plant = each side's biggest building */
    if (!c7_on("weakside", "RECOMP_BOT_WEAK_SIDE", "WeakSide", 1)) return 0;
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        if (w->game_type == MA2_GT_SNATCHIT && o->kind == MA2_OBJ_CORE && o->state == MA2_OBJS_IN_BASE) { if (o->nation == me->nation) ours++; else if (o->nation > 0) theirs++; }
        else if (o->kind == MA2_OBJ_CHECKPOINT && o->nation > 0) { if (o->nation == me->nation) ours++; else theirs++; }
        else if (o->kind == MA2_OBJ_TARGET && o->hp_max > 0.0f) {
            float f = maxf(o->hp, 0.0f) / o->hp_max;
            if (o->nation == me->nation) { if (o->hp_max > our_max) { our_max = o->hp_max; our_hp = f; } }
            else if (o->hp_max > their_max) { their_max = o->hp_max; their_hp = f; }
        }
    }
    if (w->game_type == MA2_GT_BASEWAR) return our_hp >= 0.0f && their_hp >= 0.0f && our_hp < their_hp - 0.10f;
    return theirs > ours;
}

/* DeckHold (default on, RECOMP_BOT_DECK_HOLD / [Bots] DeckHold; parity pass 8): the core-above-us tests (DeckSkip 18 m, the halved ground grab 10 m)
 * read this body's own height every evaluation, so a body standing on a slope at the threshold flipped between the grab and the DeckScreen stage
 * point every ~1 s: F_83a (No Man's Land Snatch It, p15) T2 b7 at 1118-1124,875-882 -- ground y 18.4-20.0 under core 2 at y 36.7, i.e. right at
 * 36.7 - 18 = 18.7 -- alternated between core 2 (1018,1003) and its stage point 821,995 (= core + 0.33 x (base 347,978 - core) + 25 m) for 12 s,
 * restarting its lane trip each time. A decision once made for a core holds until the height difference is 4 m past the threshold the other way.
 * [C7] deckhold: held. */
static int deck_above(Ma2Brain *b, int slot, uint32_t ent, float dy, float thr)
{
    static uint32_t d_ent[16][2]; static int d_on[16][2]; static double d_t[16][2];
    int bi = b->index & 15, on = dy > thr;
    if (!c7_on("deckhold", "RECOMP_BOT_DECK_HOLD", "DeckHold", 1)) return on;
    if (d_ent[bi][slot] == ent && b->now >= d_t[bi][slot] && b->now - d_t[bi][slot] < 3.0 && d_on[bi][slot] != on &&
        fabsf(dy - thr) < 4.0f) { on = d_on[bi][slot]; c7_add("deckhold", "held", 1); }
    d_ent[bi][slot] = ent; d_on[bi][slot] = on; d_t[bi][slot] = b->now;
    return on;
}
static void obj_snatch(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s, int foot)
{
    const Ma2ObjectiveView *base = obj_find(w, MA2_OBJ_CORE_BASE, me->nation, 1);
    float z = b->obj_zeal, best = 1e9f;
    const Ma2ObjectiveView *pick = NULL, *hunt = NULL;
    int i, ncand = 0;
    if (!base) return;
    /* SnatchRush (default on, RECOMP_BOT_SNATCH_RUSH / [Bots] SnatchRush; parity pass 11-08): every core starts in the middle, so a guard at a base
     * that holds none guards nothing -- rc10_81b (Ill Wind) 3 of 5 team 1 bots and 2 of 5 team 2 bots stood at an empty base while the first wave
     * met at the pile, a 2-3 bot opening clash instead of the whole team (design note: both teams clash at the objective together). While our base
     * holds no core a guard plays the opening like a flex player (the nearest free core); the first core home sends it back to its post. [C7] snatchrush: s. */
    int rush = 0;
    if (b->obj_role == ROLE_DEFEND && c7_on("snatchrush", "RECOMP_BOT_SNATCH_RUSH", "SnatchRush", 1)) {
        int home = 0;
        for (i = 0; i < w->nobjectives; i++)
            if (w->objectives[i].kind == MA2_OBJ_CORE && w->objectives[i].state == MA2_OBJS_IN_BASE && w->objectives[i].nation == me->nation) home++;
        if (!home) { rush = 1; c7_add("snatchrush", "s", w->dt); }
    }
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        if (o->kind != MA2_OBJ_CORE) continue;
        if (o->carrier == me->player) {
            obj_set(b, OP_CARRY, base->pos, base->radius * 0.4f, 2.5f, s, o->entity);
            return;
        }
        if (o->state == MA2_OBJS_CARRIED) {
            /* HuntDropped (default on, RECOMP_BOT_HUNT_DROPPED / [Bots] HuntDropped): a core dropped by an ejecting carrier stays CARRIED with no
             * carrier for exactly 30 s; it was hunted as an enemy carrier -- d5_82: all 11 bots 'notice carrier 00000000' at 78.5 and 150.7, march
             * out and turn round together 30 s later (b4 541 m out, died in the enemy base). No carrier: nobody to hunt. [C7] snatch: dropped_skipped. */
            if (!o->carrier && c7_on("huntdropped", "RECOMP_BOT_HUNT_DROPPED", "HuntDropped", 1)) { c7_add("snatch", "dropped_skipped", 1); continue; }
            if (o->carrier_nation != me->nation) {
                float d = o->carrier ? hdist(me->pos, o->pos) : 1e9f;
                if (!hunt || d < hdist(me->pos, hunt->pos)) hunt = o;
            }
            continue;
        }
        if (o->state == MA2_OBJS_IN_BASE && o->nation == me->nation) continue;   /* already ours */
        ncand++;
    }
    {
        Ma2Vec3 at;
        if (hunt && f78_where(b, me, hunt->carrier, hunt->pos, &at)) {
            float d = hdist(me->pos, at);
            float sc = b->obj_role == ROLE_ATTACK ? (d < 120.0f ? 1.1f : 0.0f) : 1.3f;
            if (f78_on()) { static const float prio78[4] = { 0.8f, 0.95f, 1.1f, 1.25f }; sc *= prio78[b->skill & 3]; }
            if (sc > 0.0f) obj_set(b, OP_HUNT, at, 8.0f, sc * f78_zeal(b), s, hunt->entity);
        }
    }
    if (foot) return;
    const Ma2ObjectiveView *near_core = NULL; float near_d = 1e9f;   /* CoreNear */
    if (ncand > 0) {
        /* Spread attackers: the k-th nearest candidate, k = rank among the team. WeakSide: in pairs, and their base's cores as near as free ones. */
        int weak = weak_side(w, me);
        int k = (weak && b->obj_role == ROLE_ATTACK ? b->obj_rank / 2 : b->obj_rank) % ncand, j;
        if (weak) c7_add("weakside", "s", w->dt);
        float dist[MA2_BOT_MAX_OBJECTIVES]; int idx[MA2_BOT_MAX_OBJECTIVES], n = 0;
        for (i = 0; i < w->nobjectives; i++) {
            const Ma2ObjectiveView *o = &w->objectives[i];
            if (o->kind != MA2_OBJ_CORE || o->state == MA2_OBJS_CARRIED) continue;
            if (o->state == MA2_OBJS_IN_BASE && o->nation == me->nation) continue;
            dist[n] = hdist(me->pos, o->pos) + (o->state == MA2_OBJS_IN_BASE && !weak ? 60.0f : 0.0f); idx[n++] = i;
        }
        for (i = 1; i < n; i++)                                   /* insertion sort by distance */
            for (j = i; j > 0 && dist[j] < dist[j - 1]; j--) {
                float tf = dist[j]; int ti = idx[j];
                dist[j] = dist[j - 1]; idx[j] = idx[j - 1]; dist[j - 1] = tf; idx[j - 1] = ti;
            }
        if (n > 0) {
            if (w->objectives[idx[0]].state != MA2_OBJS_IN_BASE) { near_core = &w->objectives[idx[0]]; near_d = dist[0]; }
            if (k >= n) k = n - 1;
            int kk = b->obj_role == ROLE_ATTACK ? k : 0;
            /* PickHold (default on, RECOMP_BOT_PICK_HOLD / [Bots] PickHold; parity pass 8): the k-th nearest core was re-ranked every evaluation, so two
             * candidates at about the same distance swapped places as the body moved and the pick (and with it the grab / DeckScreen stage point) flipped:
             * P13_85b1 (Rush Hour Snatch It) T2 b8, rank 2 of 3 candidates, free core 1856 at 264 m vs base core 1168 at 205 + 60 = 265 m -- stage points
             * 1653,1527 / 1364,1400 alternating every 1-2 s, the body at 1436-1446,1443-1454 at 0-8 m/s for 121-135 s. The last pick stays while it is a
             * candidate one place off the k-th and at most 15 % / 30 m further than it. [C7] pickhold: held. */
            {
                static uint32_t pk_ent[16]; static double pk_t[16];
                int bi = b->index & 15;
                if (pk_ent[bi] && b->now >= pk_t[bi] && b->now - pk_t[bi] < 3.0 && w->objectives[idx[kk]].entity != pk_ent[bi] &&
                    c7_on("pickhold", "RECOMP_BOT_PICK_HOLD", "PickHold", 1))
                    for (j = 0; j < n; j++)
                        if (w->objectives[idx[j]].entity == pk_ent[bi] && abs(j - kk) <= 1 && dist[j] <= dist[kk] + maxf(30.0f, 0.15f * dist[kk])) {
                            kk = j; c7_add("pickhold", "held", 1); break;
                        }
                pk_ent[bi] = w->objectives[idx[kk]].entity; pk_t[bi] = b->now;
            }
            pick = &w->objectives[idx[kk]];
            best = hdist(me->pos, pick->pos);
        }
    }
    if (pick) {
        float sc = b->obj_role == ROLE_ATTACK ? 1.15f : b->obj_role == ROLE_FLEX ? (best < 150.0f ? 1.0f : 0.6f) :
                   rush ? 1.0f : (best < 60.0f ? 1.0f : 0.0f);
        /* SnatchDeck: a core on a deck a 'Mech / tank cannot reach is no grab for it (it sat in no_path loops for 8-18 s); it stays on the fight round it
         * (hunt / attack / escort), a BattleArmor or VTOL takes the core */
        int nojets = kind_ok(me->kind) == MA2_KIND_TANK || (kind_ok(me->kind) == MA2_KIND_MECH && !ma2_bot_has_jets(me->actor));
        /* DeckSkip (default on, RECOMP_BOT_DECK_SKIP / [Bots] DeckSkip): halved, the deck core still out-scored everything for a 'Mech with no jets or a tank, which
         * can never get up: p20_85a b5 / b7 (Rush Hour, jetless 'Mechs: no JETGAIN all match) drove round under the y 55 deck 46-60 m off for 5-71 s per
         * try, 7 tries, until ObjGiveUp. Those bodies leave cores 18 m+ above them to BattleArmor, VTOLs and jet 'Mechs -- by height, not by the region map,
         * which links the deck by a pilot stair (rc6_85b b6: skipped once in 240 s, 86 s no route). [C7] snatchdeck: skipped_nojets. */
        /* DeckNear (default on, RECOMP_BOT_DECK_NEAR / [Bots] DeckNear; parity pass 12): far from the pile the height gap is terrain, not a deck -- a
         * jetless 'Mech that stepped off a drop into low ground 300 m out (No Man's Land Snatch It D11_83b T2 b7, the side's fastest 'Mech, y 15 vs the
         * cores' 36.7 at 27.9 s) had its grab zeroed, went 'hunt' / 'support' and never reached the pile (died 57 s); the planner reaches that pile by
         * ground (T1's same chassis did, 34.6 s). The deck test applies within 100 m of the core. deckskip.py: D11_83b 7-28 s of such frames per
         * 'Mech, both teams. [C7] snatchdeck: far_kept. */
        int deck_far = hdist(me->pos, pick->pos) >= 100.0f && c7_on("decknear", "RECOMP_BOT_DECK_NEAR", "DeckNear", 1);
        if (deck_far && nojets && pick->pos.y - me->pos.y > 18.0f) c7_add("snatchdeck", "far_kept", w->dt);
        if (nojets && !deck_far && deck_above(b, 0, pick->entity, pick->pos.y - me->pos.y, 18.0f) && c7_on("deckskip", "RECOMP_BOT_DECK_SKIP", "DeckSkip", 1)) {
            sc = 0.0f; c7_add("snatchdeck", "skipped_nojets", 1);
            /* DeckScreen (default on, RECOMP_BOT_DECK_SCREEN / [Bots] DeckScreen; film review 11-08): a body that cannot get up to the cores
             * screens the way their carriers must take home -- a third of the way from the pile to their base -- instead of hunting at
             * random: p5_85a1 T1 b1 / b4 and T2 b5 / b7 (jetless, Rush Hour) roamed 'hunt' / 'support' for the whole opening. [C7] deckscreen: s. */
            const Ma2ObjectiveView *their = obj_find(w, MA2_OBJ_CORE_BASE, me->nation, 0);
            if (their && b->obj_role != ROLE_DEFEND && c7_on("deckscreen", "RECOMP_BOT_DECK_SCREEN", "DeckScreen", 1)) {
                Ma2Vec3 sp = vadd(pick->pos, vscale(vsub(their->pos, pick->pos), 0.33f));
                sp.x += (float)((b->obj_rank % 3) - 1) * 25.0f;   /* side by side, not in one spot */
                sp.y = me->pos.y;
                obj_set(b, OP_STAGE, sp, 15.0f, 0.95f * z, s, pick->entity); c7_add("deckscreen", "s", w->dt);
            }
        }
        else if (kind_ok(me->kind) != MA2_KIND_BATTLEARMOR && kind_ok(me->kind) != MA2_KIND_VTOL && deck_above(b, 1, pick->entity, pick->pos.y - me->pos.y, 10.0f) && snatch_deck(w, me) &&
            ground_cut(me, pick->pos)) { sc *= 0.5f; c7_add("snatchdeck", "ground_grab_halved", 1); }
        if (sc > 0.0f) obj_set(b, OP_GRAB, pick->pos, 0.0f, sc * z, s, pick->entity);
    }
    /* CoreNear (default on, RECOMP_BOT_CORE_NEAR / [Bots] CoreNear): the hunt (1.3) beat the grab (1.0-1.15) with a free core 13-20 m off --
     * d5_82: 37.6-80.6 b0, b1, b3 chased a carrier with 4 free cores at the pile; from 180.7 a core lay free 60 s while b9 (13 m) and three
     * more left to hunt carriers 500 m away (East scored 0). A free core within 40 m is the play: grab it (1.45). [C7] snatch: core_near. */
    if (near_core && near_d < 40.0f && c7_on("corenear", "RECOMP_BOT_CORE_NEAR", "CoreNear", 1) &&
        !(kind_ok(me->kind) != MA2_KIND_BATTLEARMOR && kind_ok(me->kind) != MA2_KIND_VTOL && near_core->pos.y > me->pos.y + 10.0f)) {
        obj_set(b, OP_GRAB, near_core->pos, 0.0f, 1.45f * z, s, near_core->entity); c7_add("snatch", "core_near", 1);
    }

    if (b->obj_role == ROLE_DEFEND && !rush) {
        float d = hdist(me->pos, base->pos);
        obj_set(b, OP_GUARD, base->pos, b->obj_guard_r, d > b->obj_guard_r ? 0.8f : 0.5f, s, base->entity);
    }
}

/* WP3-C CheckSpread (default on, RECOMP_BOT_CHECK_SPREAD / [Bots] CheckSpread; lead item from Worker A): live teammates (not me) within 25 m of a Check It point.
 * Everyone on foot took the nearest terminal and three pilots stood on one point for 5-8 s (sw70: bots 5/6/9 at (530,1237) t=40-47; sw72, sw73 the same, 2.1 m apart). */
static int check_crowd(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 at)
{
    int j, n = 0;
    for (j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
        const Ma2PlayerView *p = &w->players[j];
        if (p->player == me->player || !p->alive || p->nation != me->nation) continue;
        if (hdist(p->pos, at) < 25.0f) n++;
    }
    return n;
}

static void obj_checkit(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s, int foot)
{
    float dist[MA2_BOT_MAX_OBJECTIVES]; int idx[MA2_BOT_MAX_OBJECTIVES], n = 0, i, j;
    const Ma2ObjectiveView *held = NULL; float hd = 1e9f;
    float z = b->obj_zeal;
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        float d;
        if (o->kind != MA2_OBJ_CHECKPOINT) continue;
        d = hdist(me->pos, o->pos);
        if (o->nation != me->nation && d < maxf(o->radius * 0.8f, 3.0f) && (!(o->flags & MA2_OBJF_FOOT) || foot)) {
            /* HoldTight (default on, RECOMP_BOT_HOLD_TIGHT / [Bots] HoldTight): the hold stop (radius x 0.5 = 3.2 m) was wider than the 3 m
             * capture radius, so a pilot wandered at the edge (r1_72 b9 took 30 s at Kb, the others ~2 s). At most 2 m. */
            obj_set(b, OP_HOLD, o->pos, c7_on("holdtight", "RECOMP_BOT_HOLD_TIGHT", "HoldTight", 1) ? minf(o->radius * 0.5f, 2.0f) : o->radius * 0.5f, 1.4f, s, o->entity);   /* in the zone: finish the claim */
            return;
        }
        if (o->nation == me->nation) { if (d < hd) { hd = d; held = o; } continue; }
        if (foot && !(o->flags & MA2_OBJF_FOOT) ) continue;
        /* FootBodySkip (default on, RECOMP_BOT_FOOT_BODY_SKIP / [Bots] FootBodySkip): FootPrecheck found no pilot walk from this hull to this
         * on-foot terminal (No Man's Land mesa 634,74,1227 / 1402,83,774); the 25 s skip ran out and the same 'Mech drove back to the mesa foot
         * again and again (X_73a2 b1 at 36.7 / 74.6 / 129.7 s, b3 at 27.1 / 103.0 s from 556-563,1142-1148; team 1 had one ground arrival).
         * A player who found he cannot walk up from there does not keep coming back in that 'Mech: skipped while in this body. [C7] footbodyskip. */
        if (!foot && b->nowalk_actor && b->nowalk_actor == me->actor && o->entity == b->nowalk_ent &&
            c7_on("footbodyskip", "RECOMP_BOT_FOOT_BODY_SKIP", "FootBodySkip", 1)) { c7_add("footbodyskip", "skipped", 1); continue; }
        if (foot && d > (b->now < b->obj_walk_until && o->entity == b->obj_walk_ent ? 160.0f : 90.0f)) continue;   /* on foot: only a terminal close by (FootFallback: the one we got out for; p17_72a b0 got out 89-94 m off and boarded again) */
        if (c7_on("checkspread", "RECOMP_BOT_CHECK_SPREAD", "CheckSpread", 1)) {
            int cr = check_crowd(w, me, o->pos);
            if (foot && cr >= 2) { c7_add("checkspread", "foot_skipped", 1); continue; }   /* two teammates already there: another point or the fight */
            if (!foot && cr > 0) { d += 80.0f * (float)(cr > 3 ? 3 : cr); c7_add("checkspread", "veh_pushed", 1); }
        }
        dist[n] = d; idx[n++] = i;
    }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && dist[j] < dist[j - 1]; j--) {
            float tf = dist[j]; int ti = idx[j];
            dist[j] = dist[j - 1]; idx[j] = idx[j - 1]; dist[j - 1] = tf; idx[j - 1] = ti;
        }
    if (n > 0 && (b->obj_role != ROLE_DEFEND || dist[0] < 120.0f || foot)) {
        int k = b->obj_role == ROLE_ATTACK && !foot ? b->obj_rank % n : 0;
        if (b->obj_role == ROLE_ATTACK && !foot && weak_side(w, me)) { k = (b->obj_rank / 2) % (n < 2 ? n : 2); c7_add("weakside", "s", w->dt); }   /* WeakSide: pairs, the two nearest */
        /* CheckSticky (default on, RECOMP_BOT_CHECK_STICKY / [Bots] CheckSticky): the k-th nearest terminal, re-sorted every evaluation from
         * where the bot now stands (plus the crowd push), swapped as it moved: au9_70 (Check It Deliverance, 11-04) b3 / b7 / b10 flipped
         * their terminal 64-86 times, every ~2 s, and circled 200-470 m out from all of them (~550 bot-seconds). The terminal being claimed is
         * kept while it is still a candidate, unless the new pick is 150 m+ nearer. [C7] checksticky: kept / switched. */
        if (b->obj_ent && c7_on("checksticky", "RECOMP_BOT_CHECK_STICKY", "CheckSticky", 1)) {
            for (int q = 0; q < n; q++)
                if (w->objectives[idx[q]].entity == b->obj_ent && q != k) {
                    if (dist[k] > dist[q] - 150.0f) { k = q; c7_add("checksticky", "kept", 1); } else c7_add("checksticky", "switched", 1);
                    break;
                }
        }
        const Ma2ObjectiveView *o = &w->objectives[idx[k]];
        float sc = foot ? 1.3f : (b->obj_role == ROLE_ATTACK ? 1.15f : 0.95f) * z;
        obj_set(b, OP_CLAIM, o->pos, (o->flags & MA2_OBJF_FOOT) ? 0.0f : o->radius * 0.4f, sc, s, o->entity);
        b->obj_need_foot = (o->flags & MA2_OBJF_FOOT) != 0;
    }
    if (held && !foot && (b->obj_role == ROLE_DEFEND || n == 0)) {
        float r = maxf(held->radius, 25.0f);
        obj_set(b, OP_GUARD, held->home, r, hd > r ? 0.8f : 0.5f, s, held->entity);
    }
}

/* Base War: wreck the other team's base buildings, guard our own. */
/* BwGroup (default on, RECOMP_BOT_BW_GROUP / [Bots] BaseWarGroup): Base War attackers crossed the middle one at a time and met the
 * other side's flex pair alone: bs91a (Deliverance) team 1's attackers were alone 85 % of their past-the-middle seconds (rc3_91 100 %),
 * died 6 times at 531-545,1008-1035 to two or three of team 2's bots and never got within 250 m of the plant in 480 s (0 damage).
 * Like the CTF staging (F46): meet at our main lane's 45 % point, go when a team-mate is there or coming with us (or after 12 s; 20 s left bw7b b6 standing 18.6 s), then
 * push for 60 s. Recruits go alone. Hit at the meeting point: no hold for 6 s (fight or cover). [C7] bwgroup: stages, go_full,
 * go_timeout, fire_breaks, past_latched, stage_s. */
static int bw_stage(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2ObjectiveView *tgt, float *s)
{
    const Ma2Lane *ln;
    Ma2Vec3 st;
    int i, k, near = 0;
    if (b->skill <= 0 || w->nlanes < 1 || w->lane_nation_a < 0 || !c7_on("bwgroup", "RECOMP_BOT_BW_GROUP", "BaseWarGroup", 1)) return 0;
    if (b->now < b->ctf_go_until) return 0;                                      /* pushing in now */
    ln = &my_lanes(w, me)[0];
    if (ln->n < 2 || hdist(ln->pt[0], ln->pt[ln->n - 1]) < 600.0f) return 0;      /* a short map: just go */
    k = (int)(0.45f * (float)(ln->n - 1) + 0.5f);
    if (me->nation != w->lane_nation_a) k = ln->n - 1 - k;                       /* our spawn is lane_b's end */
    st = ln->pt[k];
    if (hdist(me->pos, tgt->pos) < hdist(st, tgt->pos) - 15.0f) {                /* already past it: a push */
        b->ctf_stage_t = 0.0; b->ctf_go_until = b->now + 60.0; c7_add("bwgroup", "past_latched", 1);
        return 0;
    }
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q->player == me->player || q->nation != me->nation || !q->alive || !q->actor) continue;   /* a human team-mate counts */
        if (hdist(q->pos, st) < 70.0f || hdist(q->pos, me->pos) < 50.0f) near++;   /* there, or coming with me */
    }
    if (hdist(me->pos, st) < 50.0f) {
        if (b->now - b->last_hit < 2.0) { b->ctf_go_until = b->now + 6.0; b->ctf_stage_t = 0.0; c7_add("bwgroup", "fire_breaks", 1); return 0; }
        if (b->ctf_stage_t <= 0.0) { b->ctf_stage_t = b->now; c7_add("bwgroup", "stages", 1); }
        if (near >= 1 || b->now - b->ctf_stage_t >= 12.0) {
            c7_add("bwgroup", near >= 1 ? "go_full" : "go_timeout", 1);
            if (diag_on()) fprintf(stderr, "[BOT-BWGROUP] t=%.1f bot=%d goes from %.0f,%.0f after %.1f s (%d with it)\n", b->now, b->index, st.x, st.z, b->now - b->ctf_stage_t, near);
            b->ctf_go_until = b->now + 60.0; b->ctf_stage_t = 0.0;
            return 0;
        }
        c7_add("bwgroup", "stage_s", w->dt);
    }
    obj_set(b, OP_STAGE, st, 12.0f, 1.1f * b->obj_zeal, s, tgt->entity);
    return 1;
}

static void obj_basewar(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s, int foot)
{
    const Ma2ObjectiveView *tgt = NULL, *own = NULL;
    float td = 1e9f, od = 1e9f, z = b->obj_zeal, top = 0.0f;
    int i, nt = 0;
    /* the plant first. A base's other team buildings (Bogged Down: a 2000-hp VTOL bay beside the
     * 24000-hp power plant) drew a share of the attackers' rank spread; the biggest enemy building is the one
     * worth the trip, a smaller one only when it is 150 m+ nearer. Switch RECOMP_BOT_BW_PLANT / [Bots] BaseWarPlant. */
    int plant = c7_on("bwplant", "RECOMP_BOT_BW_PLANT", "BaseWarPlant", 1);
    if (foot) return;
    if (plant)
        for (i = 0; i < w->nobjectives; i++)
            if (w->objectives[i].kind == MA2_OBJ_TARGET && w->objectives[i].hp > 0.0f && w->objectives[i].nation != me->nation)
                top = maxf(top, w->objectives[i].hp_max);
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        float d;
        if (o->kind != MA2_OBJ_TARGET || o->hp <= 0.0f) continue;
        d = hdist(me->pos, o->pos);
        /* BaseWarCore (default on, RECOMP_BOT_BW_CORE / [Bots] BaseWarCore): "150 m nearer" was not enough. On Deliverance the bay
         * (2000 hp, saved Indestructible=1) stands about 490 m in front of the 24000-hp plant, so B's attackers (au5_91 bots 5/7/8/10) took
         * the bay as their order point for the whole match and shot it, and B's only defender guarded its own bay roof (y 28,
         * no_path) while bot 1 took 3600 hp off its plant. While a plant-sized building stands, the small ones are no target, and
         * defenders guard their own biggest building. [C7] bwcore: skipped_small. */
        int core = plant && c7_on("bwcore", "RECOMP_BOT_BW_CORE", "BaseWarCore", 1);
        if (o->nation == me->nation) {
            if (core ? (!own || o->hp_max > own->hp_max || (o->hp_max == own->hp_max && d < od)) : d < od) { od = d; own = o; }
            continue;
        }
        nt++;
        if (core && o->hp_max < top * 0.5f) { c7_add("bwcore", "skipped_small", 1); continue; }
        if (plant && o->hp_max < top * 0.5f) d += 150.0f;
        /* attackers spread over the targets by rank; others take the nearest */
        else if (b->obj_role == ROLE_ATTACK) d += (float)(((b->obj_rank + i) % 3) * 60);
        if (d < td) { td = d; tgt = o; }
    }
    if (tgt && b->obj_role == ROLE_ATTACK && bw_stage(b, w, me, tgt, s)) return;
    if (tgt) {
        float d = hdist(me->pos, tgt->pos);
        float sc = b->obj_role == ROLE_ATTACK ? 1.1f : b->obj_role == ROLE_FLEX ? (d < 200.0f ? 0.95f : 0.6f) : (d < 90.0f ? 0.9f : 0.0f);
        if (sc > 0.0f) obj_set(b, OP_DESTROY, tgt->pos, 0.0f, sc * z, s, tgt->entity);
    }
    if (own && b->obj_role == ROLE_DEFEND)
        obj_set(b, OP_GUARD, own->pos, b->obj_guard_r + 20.0f, od > b->obj_guard_r + 20.0f ? 0.8f : 0.5f, s, own->entity);
    else if (own && b->obj_role == ROLE_FLEX && weak_side(w, me)) {                /* WeakSide: flex players defend the plant */
        obj_set(b, OP_GUARD, own->pos, b->obj_guard_r + 40.0f, od > b->obj_guard_r + 40.0f ? 1.0f : 0.85f, s, own->entity);   /* 0.6 inside the ring tied the far destroy (0.6 x zeal up to 1.25): rc4_92a b10 flipped guard / destroy at its plant 149-451 s */
        c7_add("weakside", "s", w->dt);
    }
    (void)nt;
}

static void obj_notit(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s)
{
    int ti;
    /* ItGap (default on, RECOMP_BOT_IT_GAP / [Bots] ItGap): for ~1.6 s after each It change there was no It, so no chase order, and swaps
     * and hacks started in that gap (r1_52 Not It NRC: 184 of 186 chasers, 295 bot-s; 24 of 25 swap-go began there; new Its hacking a live
     * 'Mech died). The killer becomes It where the old one fell: keep chasing that spot up to 3 s. [C7] itgap: s. */
    if (w->it_player == me->player) return;                   /* It: just fight */
    ti = w->it_player ? find_player(w, w->it_player) : -1;
    if (ti < 0 || !w->players[ti].alive) {                      /* no live It: the gap after a change, or nobody yet */
        if (b->it_last_t > 0.0 && b->now - b->it_last_t < 3.0 && b->now >= b->it_last_t && c7_on("itgap", "RECOMP_BOT_IT_GAP", "ItGap", 1)) {
            c7_add("itgap", "s", b->dt);
            obj_set(b, OP_CHASE_IT, b->it_last_pos, 25.0f, 0.95f * f78_zeal(b), s, 0);
        }
        return;
    }
    b->it_last_pos = w->players[ti].pos; b->it_last_t = b->now;
    {   /* only It scores: the better the player, the harder it goes after It */
        static const float prio[4] = { 0.8f, 0.95f, 1.1f, 1.25f }, prio78[4] = { 0.7f, 0.9f, 1.15f, 1.4f };
        Ma2Vec3 at;
        if (!f78_where(b, me, w->it_player, w->players[ti].pos, &at)) return;   /* has not noticed who is It yet */
        obj_set(b, OP_CHASE_IT, at, 25.0f, 0.95f * f78_zeal(b) * (f78_on() ? prio78 : prio)[b->skill & 3], s, w->it_player);
    }
}

/* Pilot bombs at the strategy guide's spots (pp94-95): a pilot on foot within 20 m of a Check It checkpoint
 * its team holds (it just seized it at the terminal), or of a Snatch It core lying free, plants one there
 * on its way to a ride - the enemy comes to that spot; friendlies do not set it off. Opportunistic only
 * (playtest): one try per life on foot, given up after 5 s, never while our bomb is out (a press would
 * set it off); ctrl allows one bomb per life. Switch RECOMP_BOT_BOMB_SPOTS / [Bots] BombSpots. */
static void obj_bomb_mode_spot(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s)
{
    int i, kind = w->game_type == MA2_GT_CHECKIT ? MA2_OBJ_CHECKPOINT : w->game_type == MA2_GT_SNATCHIT ? MA2_OBJ_CORE : -1;
    const Ma2ObjectiveView *pick = NULL; float pd = 0.0f;
    if (me->actor != b->bspot_actor) { b->bspot_actor = me->actor; b->bspot_t0 = 0.0; b->bspot_done = 0; }   /* a new life */
    if (b->bomb_out) b->bspot_done = 1;
    if (kind < 0 || b->bspot_done || !c7_on("pbomb", "RECOMP_BOT_BOMB_SPOTS", "BombSpots", 1)) return;
    for (i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        float d = hdist(me->pos, o->pos);
        if (o->kind != kind) continue;
        if (d > 20.0f || (kind == MA2_OBJ_CHECKPOINT ? o->nation != me->nation : o->state != MA2_OBJS_HOME)) continue;
        if (!pick || d < pd) { pick = o; pd = d; }
    }
    if (!pick) return;
    if (b->bspot_t0 == 0.0) b->bspot_t0 = b->now;
    if (b->now - b->bspot_t0 > 5.0) { b->bspot_done = 1; c7_add("pbomb", "spot_gave_up", 1); return; }   /* not worth more of a life on foot */
    obj_set(b, OP_BOMB, pick->pos, 2.0f, 1.35f, s, pick->entity);
}

/* Scores G_OBJECTIVE for evaluate(). */
static void objective_eval(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, float *s)
{
    int k = kind_ok(me->kind), foot = k == MA2_KIND_PILOT;
    int prev_op = b->obj_op;
    uint32_t prev_ent = b->obj_ent;
    s[G_OBJECTIVE] = 0.0f;
    b->obj_op = OP_NONE; b->obj_need_foot = 0;
    /* VtolFlagReturn ([Bots] VtolFlagReturn / RECOMP_BOT_VTOL_FLAG_RETURN, default on; playtest 10-31): our flag lying dropped
     * within 300 m -- a VTOL flies down low through it to send it home (the fly-through-low pickup move). 20 s without
     * it going home: not again for 45 s. */
    if (k == MA2_KIND_VTOL && w->game_type == MA2_GT_CTF && w->team_mode && c7_on("vtolret", "RECOMP_BOT_VTOL_FLAG_RETURN", "VtolFlagReturn", 1)) {
        const Ma2ObjectiveView *mine = obj_find(w, MA2_OBJ_FLAG, me->nation, 1);
        float d = mine ? hdist(me->pos, mine->pos) : 1e9f;
        if (mine && mine->state == MA2_OBJS_DROPPED && d < 300.0f && b->now >= b->vret_off_until && hp_frac(me->hp, me->hp_max) > 0.35f) {
            if (prev_op != OP_RETURN) { b->vret_t0 = b->now; c7_add("vtolret", "tries", 1);
                if (diag_on()) fprintf(stderr, "[BOT-VRET] t=%.1f bot=%d VTOL goes to return our flag, %.0f m\n", b->now, b->index, d); }
            if (b->now - b->vret_t0 > 20.0) { b->vret_off_until = b->now + 45.0; c7_add("vtolret", "gave_up", 1); }
            else obj_set(b, OP_RETURN, mine->pos, 0.0f, d < 80.0f ? 1.6f : 1.2f, s, mine->entity);
        } else if (prev_op == OP_RETURN && mine && mine->state == MA2_OBJS_HOME) {
            c7_add("vtolret", "returned", 1);
            if (diag_on()) fprintf(stderr, "[BOT-VRET] t=%.1f bot=%d our flag is home\n", b->now, b->index);
        }
        return;
    }
    if (k == MA2_KIND_VTOL || k == MA2_KIND_FLYER || k == MA2_KIND_TURRET) return;
    if (w->game_type == MA2_GT_NOTIT) { obj_notit(b, w, me, s); }
    else if (w->team_mode && w->nobjectives > 0) {
        b->obj_role = obj_role(b, w, me);
        if (w->game_type == MA2_GT_CTF) obj_ctf(b, w, me, s, foot);
        else if (w->game_type == MA2_GT_SNATCHIT) obj_snatch(b, w, me, s, foot);
        else if (w->game_type == MA2_GT_CHECKIT) obj_checkit(b, w, me, s, foot);
        else if (w->game_type == MA2_GT_BASEWAR) obj_basewar(b, w, me, s, foot);
    }
    if (foot && w->team_mode && w->nobjectives > 0) obj_bomb_mode_spot(b, w, me, s);
    /* On foot only urgent objective work beats finding a vehicle. */
    if (foot && b->obj_op != OP_CARRY && b->obj_op != OP_RETURN && b->obj_op != OP_CLAIM && b->obj_op != OP_HOLD && b->obj_op != OP_BOMB) {
        if (b->obj_op == OP_GUARD) { b->guard_pt = b->obj_pt; b->guard_t = b->now; }   /* a guard on its way to a ride: obj_bomb_on_way */
        s[G_OBJECTIVE] = 0.0f; b->obj_op = OP_NONE;
    }
    if (b->obj_op != prev_op || b->obj_ent != prev_ent) {
        snprintf(b->note, sizeof b->note, "obj:%s", k_op_name[b->obj_op]);
        b->obj_wander_until = 0.0;
    }
}

static int obj_locked(const Ma2Brain *b, const Ma2PlayerView *me)
{
    float d;
    if (b->goal != G_OBJECTIVE) return 0;
    if (b->obj_op == OP_CARRY || b->obj_op == OP_HOLD) return 1;
    d = hdist(me->pos, b->obj_pt);
    if (b->obj_op == OP_BOMB) return d < 20.0f;                        /* a few steps from the spot (5 s at most): plant first */
    if (b->obj_role == ROLE_ATTACK && (b->obj_op == OP_GRAB || b->obj_op == OP_CLAIM)) return 1;
    if (b->obj_op == OP_DESTROY && (d < 160.0f || b->obj_role == ROLE_ATTACK)) return 1;   /* runners do not get drawn into fights */
    return (b->obj_op == OP_GRAB || b->obj_op == OP_RETURN || b->obj_op == OP_CLAIM) && d < MA2B_OBJ_LOCK_RANGE;
}

/* Objective order: travel to the objective point (A* + MoveTo in the controller);
 * locked duties (carry, grab, claim, hold) keep the body, the rest may fight. */
static uint32_t obj_enemy_near(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 p, float r)
{
    int i; uint32_t best = 0; float bd = r;
    for (i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *q = &w->players[i];
        float d;
        if (!q->alive || !q->actor || q->nation == me->nation) continue;
        d = hdist(q->pos, p);
        if (d < bd) { bd = d; best = q->actor; }
    }
    return best;
}

static Ma2Vec3 obj_bomb_spot(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 home)
{
    Ma2Vec3 spot = home;
    if (w->nlanes > 0 && w->lane_nation_a >= 0) {
        Ma2Vec3 foe = me->nation == w->lane_nation_a ? w->lane_b : w->lane_a, d = vsub(foe, home);
        float l = hdist(foe, home);
        d.y = 0.f;
        if (l > 60.0f) spot = vadd(home, vscale(d, 30.0f / l));
    }
    return spot;
}
/* A pilot guarding its flag / base walks to a ride first (the guards are in 'Mechs all match): with the
 * bomb spot within 20 m it goes by the spot (5 s at most), plants, then boards - a bomb is never
 * worth more than getting back into the fight (playtest). Once per 30 s, so a plant the ctrl
 * turns down (teammates near, cooldown, skill) costs a short detour, not the ride. */
static void obj_bomb_on_way(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *o)
{
    if (kind_ok(me->kind) != MA2_KIND_PILOT || b->now - b->guard_t > 2.0 || b->now < b->guard_t || o->type != MA2_ORD_GET_VEHICLE || b->bomb_out) return;
    /* BombWayStart (default on, RECOMP_BOT_BOMB_WAY_START / [Bots] BombWayStart): guard_t and bomb_way_t start at 0, so in a match's first
     * seconds every pilot off to a ride read as "a guard 0 s ago" (guard_pt 0,0,0) and as "on a detour that began at 0 s" -- the 20 m test
     * was skipped and the ride order became a walk to 0,0,0, then to the guard's bomb spot 500 m off, until t=5 (vo2/vp2 11-04: all 11 bots
     * 1.7-4.7 s; NRC red's paired BattleArmor pilot circled its suit 5 m away, BaNear dropped it at 8.3 s and red made no opening run).
     * A detour needs a real guard spot and a detour this bot really started. */
    int bws = c7_on("bombwaystart", "RECOMP_BOT_BOMB_WAY_START", "BombWayStart", 1);
    if (bws) {
        if (b->now < b->bomb_way_t) b->bomb_way_t = 0.0;        /* a new match */
        if (b->guard_t <= 0.0) { c7_add("bombwaystart", "no_guard_skips", 1); return; }   /* (a tick the old code went to 0,0,0) */
    }
    Ma2Vec3 spot = obj_bomb_spot(w, me, b->guard_pt);
    if (b->now - b->bomb_way_t > 30.0 || (bws && b->bomb_way_t <= 0.0)) {
        if (hdist(me->pos, spot) > 20.0f) {
            if (bws && b->bomb_way_t <= 0.0 && b->now <= 5.0) c7_add("bombwaystart", "far_skips", 1);   /* (a tick the old code went to the far spot) */
            return;
        }
        b->bomb_way_t = b->now;
        c7_add("pbomb", "way_detours", 1);
        if (diag_on()) fprintf(stderr, "[BOT-BOMB] t=%.1f bot=%d guard detour to %.0f,%.0f (%.0f m) on the way to a ride\n", b->now, b->index, spot.x, spot.z, hdist(me->pos, spot));
    }
    if (b->now - b->bomb_way_t > 5.0) return;                   /* this detour is over: board */
    o->type = MA2_ORD_TRAVEL; o->point = spot; o->stop = 3.0f; o->vehicle = 0;
    o->bomb_here = 1; o->bomb_pt = spot;
}

static void obj_order(Ma2Brain *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Order *o)
{
    int k = kind_ok(me->kind);
    Ma2Vec3 goal = b->obj_pt;
    float stop = maxf(b->obj_stop, 2.0f);
    if (b->obj_op == OP_GUARD || b->obj_op == OP_HOLD) {
        /* Patrol inside the zone instead of parking (keeps claiming, harder to hit). */
        float r = maxf(b->obj_stop, 4.0f);
        /* GuardRing (default on, RECOMP_BOT_GUARD_RING / [Bots] GuardRing): far from the post the waypoint was re-rolled every tick (the order
         * jumped +-40 m twice a second the whole walk) and taken blind: around a Base War plant 64-84 % of the points were wall, roof or the
         * hill behind it -- d5_91: 22 guard stands (52 s) at a route end 4-76 m short, then ObjGiveUp. Far off: keep the point. Each roll:
         * the first of 6 the route reaches (reach set; else open ground 3 m+ clear). [C7] guardring: picks, reached, blind. */
        int gring = c7_on("guardring", "RECOMP_BOT_GUARD_RING", "GuardRing", 1);
        int far_ = hdist(me->pos, b->obj_pt) > r * 1.3f;
        if (far_ && !(gring && b->obj_wander_until > 0.0 && hdist(b->obj_wpt, b->obj_pt) <= r * 1.01f)) b->obj_wander_until = 0.0;
        if ((!far_ || !gring) ? (b->now >= b->obj_wander_until || hdist(me->pos, b->obj_wpt) < 4.0f) : b->obj_wander_until <= 0.0) {
            int tries = gring ? 6 : 1, got = 0;
            for (int q = 0; q < tries && !got; q++) {
                float h = rng_range(&b->rng, -MA2B_PI, MA2B_PI), rf = rng_range(&b->rng, 0.2f, 0.8f);
                /* (v3: on our side of the post -- the open approach a body came in by; a plant's back is wall, roof or hill: g2_91 still 28
                 * stands with points behind / inside it) */
                if (gring && hdist(me->pos, b->obj_pt) > 4.0f) { h = heading_of(vsub(me->pos, b->obj_pt)) + rng_range(&b->rng, -1.05f, 1.05f); rf = rng_range(&b->rng, 0.5f, 0.9f); }
                b->obj_wpt = vadd(b->obj_pt, vscale(dir_of_heading(h), rf * r));
                if (!gring) break;
                int rr = ma2_path_reach_has(b->index, me->pos, b->obj_wpt, b->now);
                if (rr == 1 || (rr < 0 && ma2_trav_ready() && ma2_trav_clear(b->obj_wpt.x, b->obj_wpt.z, b->now) >= 3.0f)) got = 1;
            }
            if (gring) c7_add("guardring", got ? "reached" : "blind", 1);
            b->obj_wander_until = b->now + rng_range(&b->rng, 3.0f, 6.0f);
        }
        goal = b->obj_wpt; stop = 3.0f;
        /* HoldStill (default on, RECOMP_BOT_HOLD_STILL / [Bots] HoldStill): a Check It capture needs ~2 s inside 3 m, but the patrol picked
         * its waypoint 0.2-0.8 x max(stop, 4 m) out, so a pilot ran 5-13 m/s in and out of the circle (r4_73 b7 at T2w: 9 hold/claim flips,
         * no capture in 12 s, killed). On a Check It hold: stand on the point, the last metres walked straight in. [C7] holdstill: frames. */
        if (b->obj_op == OP_HOLD && w->game_type == MA2_GT_CHECKIT && (k == MA2_KIND_PILOT || k == MA2_KIND_BATTLEARMOR) && c7_on("holdstill", "RECOMP_BOT_HOLD_STILL", "HoldStill", 1)) {
            goal = b->obj_pt; stop = 1.0f; c7_add("holdstill", "frames", 1);
        }
    } else if (b->obj_op == OP_DESTROY) {
        stop = minf(k_kind[k].weapon_range * 0.6f, 70.0f);   /* in weapon range of the building */
    }
    if (b->obj_op == OP_DESTROY && b->obj_ent) { o->structure = b->obj_ent; o->structure_pos = b->obj_pt; }   /* and shoot it */
    /* Pilot bombs: a pilot on foot guarding its own flag / base plants one on the enemy's way in, 30 m
     * out toward their spawn (teammates guarding the flag itself keep out of its blast), then goes
     * back to guarding while it is out. No lanes (no enemy spawn known): on the objective itself. */
    if (k == MA2_KIND_PILOT && b->obj_op == OP_BOMB) {
        o->bomb_here = w->game_type == MA2_GT_CHECKIT ? 2 : 3; o->bomb_pt = b->obj_pt; goal = b->obj_pt; stop = 2.0f;
    }
    if (k == MA2_KIND_PILOT && b->obj_op == OP_GUARD) {
        Ma2Vec3 spot = obj_bomb_spot(w, me, b->obj_pt);
        if (!(b->bomb_out && hdist(b->bomb_at, spot) < 15.0f) && !(b->bomb_out && hdist(b->bomb_at, b->obj_pt) < 15.0f)) {
            o->bomb_here = 1; o->bomb_pt = spot; goal = spot; stop = 3.0f;
        }
    }
    o->type = MA2_ORD_TRAVEL; o->point = goal; o->stop = stop;
    o->engage = b->obj_op != OP_CARRY && b->obj_op != OP_HOLD;
    if (b->obj_op == OP_HOLD && w->game_type == MA2_GT_CHECKIT && (k == MA2_KIND_PILOT || k == MA2_KIND_BATTLEARMOR) && c7_on("holdstill", "RECOMP_BOT_HOLD_STILL", "HoldStill", 1)) { o->exact = 1; o->exact_r = 15.f; }
    if (k == MA2_KIND_VTOL) { o->vt_heal = 2; o->stop = 2.0f; snprintf(b->note, sizeof b->note, "vtol-return"); return; }   /* VtolFlagReturn: low through it */
    /* FlagDirect ([Bots] FlagDirect / RECOMP_BOT_FLAG_DIRECT, default on): the flag stand sits in cells the planner keeps
     * a body clear of, so routes ended 14-20 m short and bots waited there 30-70 s (playtest: bots did not go straight for
     * the flag once close). Inside reach, with no wall in between, steer straight at it (ctrl_exact_walk);
     * Recruits only from 15 m, Elites from 25 m. */
    if (b->obj_op == OP_GRAB && c7_on("flagdirect", "RECOMP_BOT_FLAG_DIRECT", "FlagDirect", 1)) {
        static const float reach[4] = { 15.f, 20.f, 25.f, 25.f };
        o->exact = 1; o->exact_r = reach[b->skill & 3];
    }
    /* FlagTouch (default on, RECOMP_BOT_FLAG_TOUCH / [Bots] FlagTouch): the pickup is a touch, closer than the 2 m "arrived"
     * radius -- a BattleArmor walked straight at their flag and stood 2.02 m from it for 15 s (a playtester's game 11-04,
     * botrec t=36.8-52). A grab or a return has no arrival: the bot walks onto the flag's own point until it holds it
     * (the order changes then). [C7] flagtouch: frames, passes. */
    if ((b->obj_op == OP_GRAB || b->obj_op == OP_RETURN) && k != MA2_KIND_VTOL && c7_on("flagtouch", "RECOMP_BOT_FLAG_TOUCH", "FlagTouch", 1)) {
        o->touch = 1; o->stop = 0.5f; o->exact = 1; if (o->exact_r < 15.f) o->exact_r = 15.f;
    }
    /* CoreTouch (default on, RECOMP_BOT_CORE_TOUCH / [Bots] CoreTouch): a Snatch It core counts only within ~11-12 m of the base
     * centre, and MoveTo orbits a point it cannot quite reach: au5_82 bot 5 circled its base at 15-18 m for 12 s at speed (186-198)
     * with the core and was killed; b9 / b10 the same (161-172, 231-240 undelivered). Inside 30 m with no wall between, the carrier
     * walks straight onto the base point until the core is in (the order changes then). [C7] coretouch via flagtouch frames. */
    if (b->obj_op == OP_CARRY && w->game_type == MA2_GT_SNATCHIT && k != MA2_KIND_VTOL && c7_on("coretouch", "RECOMP_BOT_CORE_TOUCH", "CoreTouch", 1)) {
        o->touch = 1; o->stop = 0.5f; o->exact = 1; o->exact_r = 30.f;
    }
    if (b->obj_op == OP_HUNT || b->obj_op == OP_CHASE_IT || b->obj_op == OP_CARRY)
        o->target = obj_enemy_near(w, me, b->obj_pt, 20.0f);           /* the carrier / "It" */
    /* HuntTarget (default on, RECOMP_BOT_HUNT_TARGET / [Bots] HuntTarget; film review 11-08): the hunted carrier itself is the target, not "an enemy
     * within 20 m of where I last looked": A_60a (CTF Bogged Down) 68-92 s, T2 carriers b7 / b8 ran our flag at 15-30 m/s past T1 b4 (17 m) and
     * b2 (37 m), both 'hunt' with NO target -- the looked-at spot (refreshed every 1.5-5 s by skill) was 20 m+ behind. A carrier the bot
     * sees (F78 noticed it, visible now) is named directly. [C7] hunttarget: direct. */
    if (b->obj_op == OP_HUNT && b->obj_ent && c7_on("hunttarget", "RECOMP_BOT_HUNT_TARGET", "HuntTarget", 1)) {
        for (int q = 0; q < w->nobjectives; q++) {
            const Ma2ObjectiveView *ob = &w->objectives[q];
            if (ob->entity != b->obj_ent || !ob->carrier || ob->carrier_nation == me->nation) continue;
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *cq = &w->players[j];
                if (cq->player != ob->carrier || !cq->alive || !cq->actor) continue;
                const Mem *cm = mem_by_actor(b, cq->actor);
                if (cm && cm->visible && o->target != cq->actor) { o->target = cq->actor; c7_add("hunttarget", "direct", 1); }
                break;
            }
            break;
        }
    }
    /* CarrierFire (default on, RECOMP_BOT_CARRIER_FIRE / [Bots] CarrierFire; film review 11-08): a hunter shoots the carrier it hunts the moment
     * it sees it in reach. The travel order fired only at the brain's own threat target: p5_85a1 (Snatch It Rush Hour) 56-63 s, T2 b6 ran a
     * core past four T1 'Mechs at 70-150 m (reach 450-505, line of sight clear) and b1 / b3 hunting it fired 2 shots in 7 s. [C7] carrierfire: s. */
    if ((b->obj_op == OP_HUNT || b->obj_op == OP_CHASE_IT) && o->target && !o->hold_fire && c7_on("carrierfire", "RECOMP_BOT_CARRIER_FIRE", "CarrierFire", 1)) {
        const Mem *cm = mem_by_actor(b, o->target);
        const Ma2PlayerView *cp = player_by_actor(w, o->target);
        if (cm && cm->visible && cp && hdist(me->pos, cp->pos) <= my_reach(me)) { o->fire = 1; c7_add("carrierfire", "s", b->dt); }
    }
    /* Check-It terminals are worked on foot: get out next to it. */
    /* EjectTurret (default on, RECOMP_BOT_EJECT_TURRET / [Bots] EjectTurret): every pilot that got out at an ENEMY-owned terminal with a live
     * turret by it died in the 5-7 s post-eject lock (r1_72 Check It Killing Fields: 9 of 9, 2.4-10.7 s, 0 captures; au9_70 the same at K3).
     * Turrets are not in the world view; core measures them (g_tur). Enemy-owned terminal, turret alive within 50 m: stay in the vehicle
     * (it fights there) rather than get out. [C7] ejectturret: held_s. */
    /* EjectCap (default on, RECOMP_BOT_EJECT_CAP / [Bots] EjectCap): the hold had no exit -- r4_73 b0 parked 2 m from the enemy Tc in its
     * 'Mech 117 s (3421 -> 825 hp; the turret hits stopped at 187 s, the hold ran on to F37 at 25 %), b6 17 s in the cab (4107 -> 1041 hp);
     * the two pilots that got out after it captured in 2-3 s. The hold lasts 8 s from first coming within 40 m of that terminal. [C7] ejectcap: lifted. */
    static double s_ejt_t0[32]; static uint32_t s_ejt_ent[32];
    int ejx = b->index >= 0 && b->index < 32 ? b->index : 0;
    int walking = b->now < b->obj_walk_until && b->obj_walk_ent == b->obj_ent;   /* FootFallback: the cap runs from the fallback too (rc6_72a b0 sat 89 m off in its 'Mech 113-240 s: the turret hold never lifted) */
    if (s_ejt_ent[ejx] != b->obj_ent || b->now < s_ejt_t0[ejx] || (hdist(me->pos, goal) > 60.0f && !walking)) { s_ejt_ent[ejx] = b->obj_ent; s_ejt_t0[ejx] = 0.0; }
    if (b->obj_need_foot && s_ejt_t0[ejx] <= 0.0 && (hdist(me->pos, goal) < 40.0f || walking)) s_ejt_t0[ejx] = b->now;
    int ej_cap = s_ejt_t0[ejx] > 0.0 && b->now - s_ejt_t0[ejx] > 8.0 && c7_on("ejectcap", "RECOMP_BOT_EJECT_CAP", "EjectCap", 1);
    int tur_hold = 0;
    if (b->obj_need_foot && k != MA2_KIND_PILOT && c7_on("ejectturret", "RECOMP_BOT_EJECT_TURRET", "EjectTurret", 1)) {
        for (int q = 0; q < w->nobjectives; q++)
            if (w->objectives[q].entity == b->obj_ent && w->objectives[q].nation > 0 && w->objectives[q].nation != me->nation &&
                ma2_bot_turret_near(w->objectives[q].pos.x, w->objectives[q].pos.z, 50.0f) > 0) { tur_hold = 1; c7_add("ejectturret", "held_s", b->dt); break; }
    }
    if (tur_hold && ej_cap) { tur_hold = 0; c7_add("ejectcap", "lifted", 1); }
    /* FootFallback: the game refuses the exit where there is no room to set the pilot down -- rc7_72a b3's 'Mech pressed against the platform wall at
     * 469,40,1260 asked for it 56-140 s ('obj:eject') and never got out. As G_EJECT does: after 4 s of asking, back 20 m off for 4 s, then again. */
    if (walking && k != MA2_KIND_PILOT && b->now - (b->obj_walk_until - 40.0) > 4.0 && !tur_hold) {
        if (b->now >= b->eject_move_until + 3.0) {
            Ma2Vec3 away = vsub(me->pos, goal); float al = hlen(away);
            if (al < 1.0f) { away.x = 1.0f; away.z = 0.0f; al = 1.0f; }
            b->eject_move_pt = vadd(me->pos, vscale(away, 20.0f / al)); b->eject_move_until = b->now + 4.0; c7_add("footfallback", "eject_moves", 1);
        }
        if (b->now < b->eject_move_until) { o->type = MA2_ORD_TRAVEL; o->point = b->eject_move_pt; o->stop = 3.0f; o->touch = 0; o->exact = 0; snprintf(b->note, sizeof b->note, "obj:eject-move"); return; }
    }
    /* EjectClear (default on, RECOMP_BOT_EJECT_CLEAR / [Bots] EjectClear; film review 11-08): a pilot on foot dies to anything that shoots. A_70a-A_73a
     * (Check It, par10): 13 of 14 bots that got out with an enemy 'Mech / tank / BattleArmor / gunship within 150 m died 2-7 s later, none
     * claimed (A_71a b10 at 54 s: enemy b3 133 m off, dead at 56.3). A human clears the terminal first: while an enemy combat body is within
     * 150 m of us or of the terminal, stay in and fight (the travel order engages). [C7] ejectclear: held_s.
     * EjectClose (default on, RECOMP_BOT_EJECT_CLOSE / [Bots] EjectClose): the get-out came at 40 m, so every pilot walked 36-40 m in the open
     * (A_7x: 30 of 44 ejects at 36-41 m). Drive up to 12 m; inside 40 m get out early only when the vehicle stops gaining (2.5 s without 1 m
     * nearer: a platform or wall it cannot pass). [C7] ejectclose: early / near. */
    int ej_hold = 0;
    if (b->obj_need_foot && k != MA2_KIND_PILOT && !walking && c7_on("ejectclear", "RECOMP_BOT_EJECT_CLEAR", "EjectClear", 1)) {
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
            const Ma2PlayerView *q = &w->players[j];
            if (!q->alive || q->nation == me->nation || q->kind == MA2_KIND_PILOT || q->kind == 0) continue;
            if (hdist(q->pos, me->pos) < 150.0f || hdist(q->pos, goal) < 150.0f) { ej_hold = 1; break; }
        }
        if (ej_hold && hdist(me->pos, goal) < 40.0f) c7_add("ejectclear", "held_s", b->dt);
    }
    int ej_near = hdist(me->pos, goal) < 40.0f;
    if (ej_near && c7_on("ejectclose", "RECOMP_BOT_EJECT_CLOSE", "EjectClose", 1)) {
        static double s_ecl_t[32]; static float s_ecl_d[32]; static uint32_t s_ecl_ent[32];
        float dg = hdist(me->pos, goal);
        if (s_ecl_ent[ejx] != b->obj_ent || dg < s_ecl_d[ejx] - 1.0f || b->now < s_ecl_t[ejx]) { s_ecl_ent[ejx] = b->obj_ent; s_ecl_d[ejx] = dg; s_ecl_t[ejx] = b->now; }
        if (dg >= 12.0f && b->now - s_ecl_t[ejx] < 2.5) ej_near = 0;
        else if (b->obj_need_foot && k != MA2_KIND_PILOT && !ej_hold && b->now >= b->next_eject) c7_add("ejectclose", dg >= 12.0f ? "early" : "near", 1);
    }
    if (b->obj_need_foot && !tur_hold && !ej_hold && k != MA2_KIND_PILOT && !me->qte_active && (ej_near || b->now < b->obj_walk_until) && b->now >= b->next_eject) {
        o->type = MA2_ORD_EJECT;
        b->next_eject = b->now + 2.0;
        b->obj_ejected = 1;
        if (b->now < b->obj_walk_until) { b->left_veh = me->actor; b->left_until = b->now + 60.0; }   /* FootFallback: not back into this hull for the walk */
        snprintf(b->note, sizeof b->note, "obj:eject");
    }
}

int ma2_brain_objective_lock(const Ma2Brain *b)
{
    return b && b->obj_lock;
}
