/* ma2_bot_objective.c - game-mode objective state for the bot world.
 * Included from ma2_bots_core.c (uses its static guest helpers).
 * Discovery diagnostics (RECOMP_BOT_OBJDIAG=1). */

/* thiscall with one stack argument (callee pops it), int result. */
static uint32_t vcall1(uint32_t obj, uint32_t slot, uint32_t arg, uint32_t ret_site, int *ok)
{
    if (ok) *ok = 0;
    if (!gva_ok(obj, 4)) return 0;
    uint32_t vt = MEM32(obj);
    if (!gva_ok(vt, slot + 4)) return 0;
    recomp_func_t f = resolve(MEM32(vt + slot));
    if (!f) return 0;
    GuestRegs r; regs_save(&r);
    g_ecx = obj;
    PUSH32(g_esp, arg);
    PUSH32(g_esp, ret_site);
    f();
    uint32_t v = g_eax;
    regs_restore(&r);
    if (ok) *ok = 1;
    return v;
}

/* Player statistic (player+0x2EC stats object, vfunc 0 = get(id); sub_C6935 adds). */
static int player_stat(uint32_t pl, int id)
{
    uint32_t st = MEM32(pl + 0x2EC);
    if (!gva_ok(st, 8)) return 0;
    int ok; uint32_t v = vcall1(st, 0, (uint32_t)id, 0x000C6940u, &ok);
    return ok ? (int)v : 0;
}

static void objdiag_dump(uint32_t e, const char *tag)
{
    uint32_t parent = MEM32(e + 4);
    uint32_t pcls = gva_ok(parent, 0x30) ? MEM32(parent + 0x28) : 0;
    uint32_t node = MEM32(e + 0x94);
    Ma2Vec3 p = gva_ok(node, 0x100) ? read_vec3(node + 0xF0) : read_vec3(e + 0x4C0);
    int nat = -9;
    if (is_kind(e, g_cls.corporeal)) nat = actor_int(e, 0xA8, 0x000EB091u, -9);
    fprintf(stderr, "[OBJ] t=%.1f %s e=%08X cls=%08X par=%08X pcls=%08X nat=%d pos=%.0f,%.0f,%.0f d:", g_rt.world.time, tag, e,
            MEM32(e + 0x28), parent, pcls, nat, p.x, p.y, p.z);
    for (uint32_t o = 0x190; o < 0x2E0; o += 4) fprintf(stderr, " %X", MEM32(e + o));
    fprintf(stderr, "\n");
}

static void objective_diag(void)
{
    static double last; static int hist_done;
    const Ma2World *w = &g_rt.world;
    if (!env_on("RECOMP_BOT_OBJDIAG")) return;
    if (w->time < last) { last = 0; hist_done = 0; }
    if (w->time - last < 3.0) return;
    last = w->time;
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4);
    if (!gva_ok(r1, 8)) return;
    uint32_t root = MEM32(r1 + 4);
    if (!gva_ok(root, 0x30)) return;
    static uint32_t ids[256]; static int cnt[256]; int nids = 0;
    uint32_t want[10]; int nw = 0;
    want[nw++] = class_id_of("MABuilding");
    want[nw++] = class_id_of("MAFlagCTF"); want[nw++] = class_id_of("MASnatchItDataCore");
    want[nw++] = class_id_of("MASnatchItBase"); want[nw++] = class_id_of("MACollectionArea");
    want[nw++] = class_id_of("MACheckpoint"); want[nw++] = class_id_of("MATriggerFlagStatus");
    want[nw++] = class_id_of("MATriggerCheckpoint");
    uint32_t stack[512]; int sp = 0, visited = 0;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192) {
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58)) continue;
        uint32_t id = MEM32(e + 0x28);
        if (!hist_done) {
            int k = 0;
            while (k < nids && ids[k] != id) k++;
            if (k == nids && nids < 256) { ids[nids] = id; cnt[nids] = 0; nids++; }
            if (k < 256) cnt[k]++;
        }
        for (int i = 0; i < nw; i++) if (id == want[i]) { objdiag_dump(e, "ent"); break; }
    }
    if (!hist_done && w->time > 5.0) {
        hist_done = 1;
        fprintf(stderr, "[OBJ-CLS] gt=%d", w->game_type);
        for (int k = 0; k < nids; k++) fprintf(stderr, " %08X:%d", ids[k], cnt[k]);
        fprintf(stderr, "\n");
    }
    /* Players: dump a window of MAPlayer fields to find mode state (It, carried item, score). */
    for (int i = 0; i < w->nplayers && i < MA2_BOT_MAX_PLAYERS; i++) {
        uint32_t pl = w->players[i].player;
        if (!gva_ok(pl, 0x4B0) || MEM32(pl + 0x28) != g_cls.player) continue;
        fprintf(stderr, "[OBJ-PL] t=%.1f i=%d pl=%08X port=%d nat=%d bot=%d hp=%.0f:", w->time, i, pl, (int)MEM32(pl + 0x2AC),
                w->players[i].nation, w->players[i].is_bot, w->players[i].hp);
        fprintf(stderr, " id88=%d stats:", (int)MEM32(pl + 0x88));
        for (int id = 0; id < 0x24; id++) fprintf(stderr, " %d", player_stat(pl, id));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[OBJ-GAME] t=%.1f g48=%d g30:", w->time, (int)MEM32(game + 0x48));
    for (uint32_t o = 0x30; o < 0x80; o += 4) fprintf(stderr, " %X", MEM32(game + o));
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Objective state for Ma2World (reverse engineered)                   */
/* ------------------------------------------------------------------ */
/* MAFlagCTF (size 0x204; flags listed by uid in the vector 0x50E5F4..0x50E5F8):
 *   +0x1A8 carrier actor uid (0 = none)    +0x1B0 carrier player id (player+0x88) or -1
 *   +0x1B4 home position (vec3)            +0x1CC state 0 home, 1 carried, 2/3 dropped, 4 scored
 *   pickup = touch (sub_150E89 -> sub_1513D6): an enemy-nation MAMover with property
 *   bit actor+0x1E8.3; touching your own dropped flag returns it. Capture (sub_150880):
 *   carried flag within 10 m of the other flag's home while that flag is home (state 0).
 * MASnatchItDataCore (0x23C): +0x1AC number, +0x1B8 home, +0x1CC carrier actor uid,
 *   +0x1D0 uid of the MASnatchItBase holding it (0 = none), +0x1E0 carrier player id.
 * MASnatchItBase: nation via vfunc 0xA8, +0x1A8 drop radius (13 m); a carried core is
 *   stored when its carrier (same nation) comes within that radius (sub_135D58).
 * MACheckpoint: owner nation via vfunc 0xA8 (MATriggerCheckpoint sub_14F3D4),
 *   +0x1EC ClaimRadius, +0x1F0 ClaimTime, +0x1F4 ContestRadius, +0x1F8 ContestTime (runtime dump).
 * Not-It: game(0x50C958)+0x48 = player id (+0x88) of "It", -1 = nobody (sub_C9D5E). */
#define OBJ_MAX_ENTS 32
static struct {
    double refresh, update, log; int n;
    uint32_t ent[OBJ_MAX_ENTS]; int kind[OBJ_MAX_ENTS], nation[OBJ_MAX_ENTS];
    uint32_t cls_flag, cls_core, cls_base, cls_cp, cls_trig, cls_bld;
    uint32_t trig[OBJ_MAX_ENTS];          /* checkpoint: its MATriggerMenuActivate terminal */
    double dwell[MAX_BOTS]; uint32_t dwell_trig[MAX_BOTS];
    uint32_t fake_action; unsigned claims;
    int last_state[OBJ_MAX_ENTS]; uint32_t last_carrier[OBJ_MAX_ENTS]; int last_owner[OBJ_MAX_ENTS];
    uint32_t last_it;
    float last_hp[OBJ_MAX_ENTS]; double hp_log[OBJ_MAX_ENTS];   /* Base War targets: damage counted / logged */
} g_obj;

static int obj_on(void) { return env_int("RECOMP_BOT_OBJECTIVES", 1) != 0; }

/* Base War: a team building our fire does not hurt (batch 4, 4D). Bogged Down's VTOL bays (2000 hp, saved
 * Indestructible=1) are team MABuildings too, so the attackers' rank spread sent some of them to shoot a bay
 * for the whole trip (batch-3 review: bays never left 2000 hp; plants took all the damage). After 8 s of bot
 * fire at a target with its hp unchanged it is left out of the objectives for the rest of the match; one that
 * loses hp is never dropped. Switch RECOMP_BOT_BW_IMMUNE / [Bots] BaseWarImmune; [C7] bwimmune. */
static struct { uint32_t e; double fire_s; float hp0; int state; } g_bw[OBJ_MAX_ENTS];   /* state 0 watching, 1 hurt, 2 immune */
static int g_nbw; static double g_bw_t;

static int bw_find(uint32_t e)
{
    for (int i = 0; i < g_nbw; i++) if (g_bw[i].e == e) return i;
    return -1;
}
static void obj_note_fire(uint32_t e, float dt)
{
    if (!e || g_rt.game_type != MA2_GT_BASEWAR || !c7_on("bwimmune", "RECOMP_BOT_BW_IMMUNE", "BaseWarImmune", 1)) return;
    int s = bw_find(e);
    if (s < 0 && g_nbw < OBJ_MAX_ENTS) { s = g_nbw++; g_bw[s].e = e; g_bw[s].fire_s = 0.0; g_bw[s].hp0 = -1.f; g_bw[s].state = 0; }
    if (s >= 0 && g_bw[s].state == 0) g_bw[s].fire_s += dt;
}
static int bw_immune(uint32_t e) { int s = bw_find(e); return s >= 0 && g_bw[s].state == 2; }

static void objective_refresh(void)
{
    int old_n = g_obj.n;
    uint32_t old_ent[OBJ_MAX_ENTS], old_carrier[OBJ_MAX_ENTS]; int old_state[OBJ_MAX_ENTS], old_owner[OBJ_MAX_ENTS];
    memcpy(old_ent, g_obj.ent, sizeof(old_ent)); memcpy(old_carrier, g_obj.last_carrier, sizeof(old_carrier));
    memcpy(old_state, g_obj.last_state, sizeof(old_state)); memcpy(old_owner, g_obj.last_owner, sizeof(old_owner));
    float old_hp[OBJ_MAX_ENTS]; memcpy(old_hp, g_obj.last_hp, sizeof(old_hp));
    g_obj.n = 0;
    g_obj.cls_flag = class_id_of("MAFlagCTF"); g_obj.cls_core = class_id_of("MASnatchItDataCore");
    g_obj.cls_base = class_id_of("MASnatchItBase"); g_obj.cls_cp = class_id_of("MACheckpoint");
    g_obj.cls_trig = class_id_of("MATriggerMenuActivate"); g_obj.cls_bld = class_id_of("MABuilding");
    uint32_t trigs[OBJ_MAX_ENTS]; int ntrig = 0;
    int basewar = g_rt.game_type == MA2_GT_BASEWAR;
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4);
    if (!gva_ok(r1, 8)) return;
    uint32_t root = MEM32(r1 + 4);
    if (!gva_ok(root, 0x30)) return;
    uint32_t stack[512]; int sp = 0, visited = 0;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192) {
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58) || !gva_ok(e, 0x240)) continue;
        uint32_t id = MEM32(e + 0x28);
        if (id == g_obj.cls_trig) { if (ntrig < OBJ_MAX_ENTS) trigs[ntrig++] = e; continue; }
        int k = id == g_obj.cls_flag ? MA2_OBJ_FLAG : id == g_obj.cls_core ? MA2_OBJ_CORE :
                id == g_obj.cls_base ? MA2_OBJ_CORE_BASE : id == g_obj.cls_cp ? MA2_OBJ_CHECKPOINT :
                basewar && id == g_obj.cls_bld ? MA2_OBJ_TARGET : 0;
        if (!k || g_obj.n >= OBJ_MAX_ENTS) continue;
        int nat = actor_int(e, 0xA8, 0x000EB091u, -1);
        if (k == MA2_OBJ_TARGET && nat != 1 && nat != 2) continue;        /* Base War: team buildings only */
        int i = g_obj.n++;
        g_obj.last_state[i] = -1; g_obj.last_carrier[i] = 0; g_obj.last_owner[i] = -9; g_obj.last_hp[i] = 0.f;   /* event tracking follows the entity */
        for (int o = 0; o < old_n; o++)
            if (old_ent[o] == e) { g_obj.last_state[i] = old_state[o]; g_obj.last_carrier[i] = old_carrier[o]; g_obj.last_owner[i] = old_owner[o]; g_obj.last_hp[i] = old_hp[o]; break; }
        g_obj.ent[i] = e; g_obj.kind[i] = k; g_obj.trig[i] = 0;
        g_obj.nation[i] = nat;
    }
    /* Check-It terminals: MATriggerMenuActivate +0xE0 = uid of its MACheckpoint (sub_14A377). */
    for (int t = 0; t < ntrig; t++) {
        uint32_t cp = uid_lookup(MEM32(trigs[t] + 0x60), MEM32(trigs[t] + 0xE0));
        for (int i = 0; i < g_obj.n; i++) if (g_obj.ent[i] == cp && g_obj.kind[i] == MA2_OBJ_CHECKPOINT) g_obj.trig[i] = trigs[t];
    }
}

/* Position of an entity via its vfunc 0x60 (returns a vec3 pointer). */
static int obj_vpos(uint32_t e, Ma2Vec3 *out)
{
    int ok; uint32_t p = vcall0(e, 0x60, 0x0014A57Du, &ok);
    if (!ok || !gva_ok(p, 12)) return 0;
    *out = read_vec3(p);
    return out->x == out->x && out->z == out->z;
}

/* Check-It: the game claims a checkpoint when a local player confirms the
 * terminal's menu (MATriggerMenuActivate -> MAActionCheckItGoal, sub_152868).
 * The trigger only polls human players, so a bot pilot that has stood inside
 * the terminal zone for 2 s gets the same action run for it: a scratch action
 * (+0x60 world, +0x64 checkpoint uid) and the trigger's activator (+0x8C)
 * set to the bot's actor, exactly what the trigger records for a human. */
static void objective_checkit_claims(Ma2World *w)
{
    if (!env_int("RECOMP_BOT_CHECKIT_CLAIM", 1) || MEM8(0x50C948)) return;
    recomp_func_t act = resolve(0x00152868u);
    if (!act) return;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->is_bot || !p->alive || !p->actor || p->player == p->actor) continue;
        int bi = (int)MEM32(p->player + 0x2AC) - MA2_BOT_PORT_BASE;
        if (bi < 0 || bi >= MAX_BOTS) continue;
        uint32_t in_trig = 0, cp = 0;
        for (int k = 0; k < g_obj.n; k++) {
            uint32_t t = g_obj.trig[k];
            if (!t || g_obj.kind[k] != MA2_OBJ_CHECKPOINT || !gva_ok(t, 0xF0)) continue;
            if ((MEM8(t + 0xEC) & 2) && p->kind != MA2_KIND_PILOT) continue;        /* terminal is for pilots on foot */
            if (actor_int(g_obj.ent[k], 0xA8, 0x000EB091u, -1) == p->nation) continue;
            Ma2Vec3 tp;
            if (!obj_vpos(t, &tp)) continue;
            float dx = p->pos.x - tp.x, dy = p->pos.y - tp.y, dz = p->pos.z - tp.z, r = MEMF(t + 0xD4);
            if (r > 0.f && r < 100.f && dx * dx + dy * dy + dz * dz < r * r) { in_trig = t; cp = g_obj.ent[k]; break; }
        }
        if (!in_trig || g_obj.dwell_trig[bi] != in_trig) { g_obj.dwell_trig[bi] = in_trig; g_obj.dwell[bi] = w->time; continue; }
        if (w->time - g_obj.dwell[bi] < 2.0) continue;
        g_obj.dwell[bi] = w->time + 3.0;                                  /* one try per 5 s */
        if (!gva_ok(g_obj.fake_action, 0x80)) {
            g_obj.fake_action = xbox_HeapAlloc(0x80, 16);
            if (!gva_ok(g_obj.fake_action, 0x80)) return;
            for (uint32_t o = 0; o < 0x80; o += 4) MEM32(g_obj.fake_action + o) = 0;
        }
        MEM32(g_obj.fake_action + 0x60) = MEM32(cp + 0x60);
        MEM32(g_obj.fake_action + 0x64) = MEM32(cp + 0x2C);
        MEM32(in_trig + 0x8C) = MEM32(p->actor + 0x2C);
        int before = actor_int(cp, 0xA8, 0x000EB091u, -1);
        GuestRegs r; regs_save(&r);
        g_ecx = g_obj.fake_action;
        PUSH32(g_esp, in_trig);
        PUSH32(g_esp, 0x00152868u);
        act();
        regs_restore(&r);
        int after = actor_int(cp, 0xA8, 0x000EB091u, -1);
        if (after != before) g_obj.claims++;
        if (env_on("RECOMP_BOT_DIAG"))
            fprintf(stderr, "[OBJ-CLAIM] t=%.1f bot=%d port=%d nat=%d checkpoint=%08X owner %d -> %d (claims=%u)\n", w->time, bi,
                    (int)MEM32(p->player + 0x2AC), p->nation, cp, before, after, g_obj.claims);
    }
}

/* Player index whose controlled actor has this uid (actor+0x2C), else -1. */
static int obj_player_by_uid(const Ma2World *w, uint32_t uid)
{
    if (!uid) return -1;
    for (int i = 0; i < w->nplayers; i++) {
        uint32_t a = w->players[i].actor;
        if (a && w->players[i].player != a && gva_ok(a, 0x30) && MEM32(a + 0x2C) == uid) return i;
    }
    return -1;
}

static int obj_port(const Ma2World *w, int pi)
{
    if (pi < 0) return -1;
    uint32_t pl = w->players[pi].player;
    return gva_ok(pl, 0x2B0) && pl != w->players[pi].actor ? (int)MEM32(pl + 0x2AC) : -1;
}

static void objective_update(Ma2World *w)
{
    objective_diag();
    if (w->time < g_obj.update) {                                          /* new match */
        g_obj.refresh = g_obj.log = 0.0; g_obj.n = 0; g_obj.last_it = 0; g_obj.fake_action = 0;
        memset(g_obj.dwell_trig, 0, sizeof(g_obj.dwell_trig));
    }
    g_obj.update = w->time;
    if (!obj_on()) { w->nobjectives = 0; w->it_player = 0; return; }
    int gt = w->game_type;
    int wants = gt == MA2_GT_CTF || gt == MA2_GT_SNATCHIT || gt == MA2_GT_CHECKIT || gt == MA2_GT_BASEWAR;
    /* Not-It: who is It. */
    w->it_player = 0;
    if (gt == MA2_GT_NOTIT) {
        uint32_t game = MEM32(VA_GAME);
        int it = gva_ok(game, 0x50) ? (int)MEM32(game + 0x48) : -1;
        int pi = -1;
        if (it >= 0)
            for (int i = 0; i < w->nplayers; i++) {
                uint32_t pl = w->players[i].player;
                if (pl != w->players[i].actor && gva_ok(pl, 0x90) && (int)MEM32(pl + 0x88) == it) { w->it_player = pl; pi = i; break; }
            }
        if (w->it_player != g_obj.last_it && env_on("RECOMP_BOT_DIAG"))
            fprintf(stderr, "[OBJ-EVT] t=%.1f notit it=%08X port=%d bot=%d\n", w->time, w->it_player, obj_port(w, pi),
                    pi >= 0 ? w->players[pi].is_bot : -1);
        if (w->it_player && w->it_player != g_obj.last_it) c7_add("obj", "it_changes", 1);
        if (w->it_player) c7_add("obj", "it_s", w->dt);
        g_obj.last_it = w->it_player;
    }
    if (wants) {
        if (g_obj.n == 0 ? w->time - g_obj.refresh >= 1.0 : w->time - g_obj.refresh >= 5.0) {
            objective_refresh(); g_obj.refresh = w->time;
        }
    } else g_obj.n = 0;
    if (w->time < g_bw_t - 1.0) g_nbw = 0;                     /* a new match: entities are new */
    g_bw_t = w->time;
    w->nobjectives = 0;
    for (int i = 0; i < g_obj.n && w->nobjectives < MA2_BOT_MAX_OBJECTIVES; i++) {
        uint32_t e = g_obj.ent[i];
        int k = g_obj.kind[i];
        uint32_t want = k == MA2_OBJ_FLAG ? g_obj.cls_flag : k == MA2_OBJ_CORE ? g_obj.cls_core :
                        k == MA2_OBJ_CORE_BASE ? g_obj.cls_base : k == MA2_OBJ_CHECKPOINT ? g_obj.cls_cp : g_obj.cls_bld;
        if (!gva_ok(e, 0x240) || MEM32(e + 0x28) != want) { g_obj.n = 0; w->nobjectives = 0; break; }   /* stale */
        if (k == MA2_OBJ_TARGET && bw_immune(e)) continue;
        Ma2ObjectiveView *o = &w->objectives[w->nobjectives++];
        memset(o, 0, sizeof(*o));
        o->entity = e; o->kind = k; o->nation = g_obj.nation[i]; o->carrier_nation = -1;
        uint32_t node = MEM32(e + 0x94);
        o->pos = gva_ok(node, 0x100) ? read_vec3(node + 0xF0) : read_vec3(e + 0x4C0);
        o->home = o->pos;
        int carrier = -1;
        if (k == MA2_OBJ_FLAG) {
            int st = (int)MEM32(e + 0x1CC);
            o->home = read_vec3(e + 0x1B4);
            carrier = obj_player_by_uid(w, MEM32(e + 0x1A8));
            o->state = MEM32(e + 0x1A8) ? MA2_OBJS_CARRIED : (st == 2 || st == 3) ? MA2_OBJS_DROPPED : MA2_OBJS_HOME;
            o->radius = 10.0f;                                     /* capture distance (0x4B0EA4) */
        } else if (k == MA2_OBJ_CORE) {
            uint32_t base = MEM32(e + 0x1D0);
            o->home = read_vec3(e + 0x1B8);
            carrier = obj_player_by_uid(w, MEM32(e + 0x1CC));
            o->state = MEM32(e + 0x1CC) ? MA2_OBJS_CARRIED : base ? MA2_OBJS_IN_BASE : MA2_OBJS_HOME;
            o->nation = -1;
            if (base) {
                uint32_t bp = uid_lookup(MEM32(e + 0x60), base);
                if (bp && gva_ok(bp, 0x30) && MEM32(bp + 0x28) == g_obj.cls_base) o->nation = actor_int(bp, 0xA8, 0x000EB091u, -1);
            }
            o->radius = 6.0f;
        } else if (k == MA2_OBJ_CORE_BASE) {
            float r = MEMF(e + 0x1A8);
            o->radius = r > 1.f && r < 100.f ? r : 13.f;
        } else if (k == MA2_OBJ_CHECKPOINT) {
            float r = MEMF(e + 0x1EC);                             /* ClaimRadius (30 m on shipped maps) */
            uint32_t t = g_obj.trig[i];
            o->nation = actor_int(e, 0xA8, 0x000EB091u, -1);
            o->radius = r > 1.f && r < 200.f ? r : 20.f;
            Ma2Vec3 tp;
            if (!MEM8(e + 0x200) && t && gva_ok(t, 0xF0) && obj_vpos(t, &tp)) {   /* not ActivatedByProximity: use the terminal */
                float tr = MEMF(t + 0xD4);
                o->home = o->pos; o->pos = tp;
                o->radius = tr > 0.5f && tr < 100.f ? tr : 5.f;
                if (MEM8(t + 0xEC) & 2) o->flags |= MA2_OBJF_FOOT;
            }
        } else {
            o->hp = (float)actor_int(e, 0x74, 0x000C9476u, 0);
            o->hp_max = (float)actor_int(e, 0x6C, 0x000C8C2Au, 0);
            o->state = o->hp > 0.f ? MA2_OBJS_HOME : MA2_OBJS_DROPPED;
            o->radius = 15.f;
            /* Base War (bots F20/batch 2): what the attackers did to each team's buildings. [C7] basewar
             * dmg_red / dmg_blue (hit points lost), destroyed; [OBJ-HP] at most every 5 s while it drops. */
            if (k == MA2_OBJ_TARGET && o->hp_max > 0.f) {
                float was = g_obj.last_hp[i];
                if (was > 0.f && o->hp < was) {
                    c7_add("basewar", o->nation == 1 ? "dmg_red" : "dmg_blue", was - o->hp);
                    if (o->hp <= 0.f) c7_add("basewar", "destroyed", 1);
                    if (env_on("RECOMP_BOT_DIAG") && (w->time - g_obj.hp_log[i] >= 5.0 || o->hp <= 0.f)) {
                        g_obj.hp_log[i] = w->time;
                        fprintf(stderr, "[OBJ-HP] t=%.1f building@%08X nat=%d hp=%.0f/%.0f (%.0f%%)\n", w->time, e, o->nation, o->hp, o->hp_max,
                                100.f * o->hp / o->hp_max);
                    }
                }
                g_obj.last_hp[i] = o->hp;
                int s = bw_find(e);
                if (s >= 0 && g_bw[s].state == 0) {
                    if (g_bw[s].hp0 < 0.f) g_bw[s].hp0 = o->hp;
                    else if (o->hp < g_bw[s].hp0) g_bw[s].state = 1;
                    else if (g_bw[s].fire_s >= 8.0) {
                        g_bw[s].state = 2;
                        c7_add("bwimmune", "skipped", 1);
                        if (env_on("RECOMP_BOT_DIAG"))
                            fprintf(stderr, "[OBJ-BW] t=%.1f building@%08X nat=%d hp=%.0f/%.0f: %.1f s of bot fire, no damage -> left out (immune)\n",
                                    w->time, e, o->nation, o->hp, o->hp_max, g_bw[s].fire_s);
                    }
                }
            }
        }
        if (carrier >= 0) {
            o->carrier = w->players[carrier].player; o->carrier_nation = w->players[carrier].nation;
            o->pos = w->players[carrier].pos;
        }
        if (env_on("RECOMP_BOT_DIAG") && (o->state != g_obj.last_state[i] || o->carrier != g_obj.last_carrier[i] ||
                                          o->nation != g_obj.last_owner[i])) {
            static const char *kn[] = { "?", "flag", "core", "base", "checkpoint", "building" };
            static const char *sn[] = { "home", "carried", "dropped", "in-base" };
            int ls = g_obj.last_state[i];
            fprintf(stderr, "[OBJ-EVT] t=%.1f %s@%08X nat=%d %s -> %s carrier=%08X port=%d bot=%d pos=%.0f,%.0f r=%.0f hp=%.0f/%.0f f=%d\n", w->time,
                    kn[k], e, o->nation, ls >= 0 && ls < 4 ? sn[ls] : "-", sn[o->state & 3], o->carrier,
                    obj_port(w, carrier), carrier >= 0 ? w->players[carrier].is_bot : -1, o->pos.x, o->pos.z, o->radius, o->hp, o->hp_max, o->flags);
        }
        {   /* [C7] objective counters: what each game type's objective actually saw this match */
            int ls = g_obj.last_state[i];
            if (k == MA2_OBJ_FLAG || k == MA2_OBJ_CORE) {
                if (o->state == MA2_OBJS_CARRIED && ls != MA2_OBJS_CARRIED && ls >= 0) {
                    c7_add("obj", k == MA2_OBJ_FLAG ? "ctf_grabs" : "snatch_grabs", 1);
                    if (carrier >= 0 && w->players[carrier].is_bot) c7_add("obj", k == MA2_OBJ_FLAG ? "ctf_grabs_bot" : "snatch_grabs_bot", 1);
                }
                if (o->state == MA2_OBJS_CARRIED) c7_add("obj", k == MA2_OBJ_FLAG ? "ctf_carry_s" : "snatch_carry_s", w->dt);
                if (ls == MA2_OBJS_CARRIED && o->state == MA2_OBJS_DROPPED) c7_add("obj", k == MA2_OBJ_FLAG ? "ctf_drops" : "snatch_drops", 1);
                if (k == MA2_OBJ_FLAG && ls == MA2_OBJS_CARRIED && o->state == MA2_OBJS_HOME) c7_add("obj", "ctf_captures", 1);
                if (k == MA2_OBJ_FLAG && ls == MA2_OBJS_DROPPED && o->state == MA2_OBJS_HOME) c7_add("obj", "ctf_returns", 1);
                if (k == MA2_OBJ_CORE && ls == MA2_OBJS_CARRIED && o->state == MA2_OBJS_IN_BASE) c7_add("obj", "snatch_scores", 1);
            } else if (k == MA2_OBJ_CHECKPOINT) {
                if (g_obj.last_owner[i] != o->nation && ls >= 0 && o->nation >= 0) c7_add("obj", "checkit_claims", 1);
            } else if (k == MA2_OBJ_TARGET) {
                if (ls == MA2_OBJS_HOME && o->state == MA2_OBJS_DROPPED) c7_add("obj", "basewar_destroyed", 1);   /* damage: [C7] basewar */
            }
        }
        g_obj.last_state[i] = o->state; g_obj.last_carrier[i] = o->carrier; g_obj.last_owner[i] = o->nation;
    }
    if (gt == MA2_GT_CHECKIT) objective_checkit_claims(w);
    /* Periodic score line: per-player statistics ids 0..0x1B (player+0x2EC). */
    if (env_on("RECOMP_BOT_DIAG") && w->time - g_obj.log >= 10.0) {
        g_obj.log = w->time;
        if (w->nobjectives > 0) {
            fprintf(stderr, "[OBJ-STATE] t=%.1f gt=%d", w->time, gt);
            for (int i = 0; i < w->nobjectives; i++) {
                const Ma2ObjectiveView *o = &w->objectives[i];
                fprintf(stderr, " %c%d:n%d/s%d", "?FCBKT"[o->kind % 6], i, o->nation, o->state);
                if (o->kind == MA2_OBJ_TARGET) fprintf(stderr, "/hp%.0f", o->hp);
            }
            fprintf(stderr, "\n");
        }
        for (int i = 0; i < w->nplayers && i < MA2_BOT_MAX_PLAYERS; i++) {
            uint32_t pl = w->players[i].player;
            if (pl == w->players[i].actor || !gva_ok(pl, 0x4B0)) continue;
            fprintf(stderr, "[OBJ-SCORE] t=%.1f port=%d nat=%d bot=%d stats:", w->time, (int)MEM32(pl + 0x2AC),
                    w->players[i].nation, w->players[i].is_bot);
            for (int id = 0; id < 0x1C; id++) fprintf(stderr, " %d", player_stat(pl, id));
            fprintf(stderr, "\n");
        }
    }
}

/* Campaign-AI target choice bonus (ma2_bot_ai_select): flag/core carriers of the
 * other team and "It" are worth chasing first. */
static float ma2_objective_target_bonus(const Ma2World *w, const Ma2PlayerView *me, const Ma2PlayerView *e)
{
    if (w->it_player && e->player == w->it_player) return 220.f;
    for (int i = 0; i < w->nobjectives; i++) {
        const Ma2ObjectiveView *o = &w->objectives[i];
        if (o->carrier && o->carrier == e->player && e->nation != me->nation) return 200.f;
    }
    return 0.f;
}

/* Push-s3 stomp gate test (RECOMP_BOT_STOMP_GATE=1): every 20 s, per live body, its kind, hit points and the entity flags word (+0x84):
 * bit 13 CausesCollisionDamage, bit 14 ReceivesCollisionDamage (research: whether pilot / BattleArmor / tank templates carry bit 14 is unchecked). */
static void spc_gate_diag(const Ma2World *w)
{
    static double next; static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BOT_STOMP_GATE"); on = e && *e && *e != '0'; }
    if (!on) return;
    if (w->time < next - 40.0) next = 0.0;
    if (w->time < next) return;
    next = w->time + 20.0;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->alive || !p->actor || !gva_ok(p->actor, 0x90)) continue;
        uint32_t f = MEM32(p->actor + 0x84);
        fprintf(stderr, "[SPC-GATE] t=%.1f i=%d kind=%d nation=%d hp=%.0f/%.0f f84=%08X causes13=%d receives14=%d\n", w->time, i, p->kind, p->nation, p->hp, p->hp_max, f, (f >> 13) & 1, (f >> 14) & 1);
    }
}
