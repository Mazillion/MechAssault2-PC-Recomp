/* MechAssault 2 offline bots - BotController (native interfaces only).
 *
 * NOT a standalone translation unit: #included by ma2_bots_core.c after the
 * world snapshot, the threat pick and the AI line-of-sight wrapper.
 *
 * One controller per BotPlayer, ticked once per simulation step from the bot
 * player's own tick (sub_C80DB, after its control behaviour is re-bound). It
 * executes the brain's Ma2Order (travel, board, eject, engage, fight on the move,
 * hack) and decides nothing tactical itself:
 *   - locomotion: A* corner path (ma2_bot_path.c) -> look-ahead point -> the
 *     body's campaign AI: ai+0x130 destination, mode 6, MoveTo (ai vt+0x38)
 *     every step, AI asleep (LOCOMOTION_SPEC.md "method 2"). Pilot, 'Mech, tank.
 *   - fighting: AI target slot (+0xB4/+0xB8, owned here, never a dead actor) +
 *     attack mode 4, only while the target is inside engage range AND the AI's
 *     own collision ray sub_17AD5C reports a clear line (PLAYER_SPEC.md / spec 4).
 *   - actions with no AI call: one type-0x67 command frame per step straight to
 *     sub_C8012 (the frame MAInput would send): 39 enter/exit, 3 jumpjet,
 *     52 defensive system, 49+bit NeuroHack answers.
 * No pad, no camera, no view, no stick.
 */

/* F11 drop give-up (bots batch 1): env RECOMP_BOT_DROP_GIVEUP=0/1 overrides pc_settings.ini
 * [Bots] DropGiveUp (default 1); [C7] dropgiveup tries= gaveup= at match end. */
static int drop_giveup_on(void) { return c7_on("dropgiveup", "RECOMP_BOT_DROP_GIVEUP", "DropGiveUp", 1); }
static uint32_t g_frame_msg[MAX_BOTS], g_frame_cmd[MAX_BOTS];

static void ctrl_reset(BotCtrl *c) { memset(c, 0, sizeof *c); }
/* push-x3 NavFix (RECOMP_BOT_NAV_FIX / [Bots] NavFix, default ON): E8 new-body crumbs, E9 height-aware obstacle learning, E10 no first-stall jet
 * without a route, E11 NoPathHold on a failed start only, and the partial-end hold. [C7] navfix counters. */
static int navfix_on(void) { static int on = -1; if (on < 0) on = c7_on("navfix", "RECOMP_BOT_NAV_FIX", "NavFix", 1); return on; }

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

int ma2_bot_body_ok(uint32_t a);   /* ma2_bots_core.c: a MAMech / MAMover body (push W1 hijack fix) */
static uint32_t actor_ai(uint32_t a)
{
    int ok = 0;
    if (!ma2_bot_body_ok(a)) return 0;   /* no vt+0x200 call, and no +0x1E8 / AI writes behind it */
    uint32_t ai = vcall0(a, 0x200, 0x000E6CA1u, &ok);
    return ok && gva_ok(ai, 0x230) ? ai : 0;
}

/* sub_183B9D: ecx=ai, eax=mode, push arg (the game passes 3), ret 4. */
static void ai_setmode(uint32_t ai, uint32_t mode)
{
    GuestRegs r; regs_save(&r);
    g_ecx = ai; g_eax = mode;
    PUSH32(g_esp, 3u); PUSH32(g_esp, 0x00184137u);
    sub_00183B9D();
    regs_restore(&r);
}

/* MoveTo, ai vt+0x38: thiscall(ai; Vec3 *pt, u8 run, u8 b2, float speed, u8 b4) ret 0x14, al = arrived. */
static int ai_moveto(uint32_t ai, float speed, int run)
{
    recomp_func_t f = resolve(MEM32(MEM32(ai) + 0x38));
    if (!f) return -1;
    GuestRegs r; regs_save(&r);
    g_ecx = ai;
    PUSH32(g_esp, 0u); PUSH32(g_esp, f2u(speed)); PUSH32(g_esp, 0u); PUSH32(g_esp, (uint32_t)run); PUSH32(g_esp, ai + 0x130);
    PUSH32(g_esp, 0x00184143u);
    f();
    int v = (int)(g_eax & 0xFF);
    regs_restore(&r);
    return v;
}

static float kind_plan_radius(int k)
{
    switch (k) { case MA2_KIND_PILOT: return 0.8f; case MA2_KIND_BATTLEARMOR: return 1.4f; default: return 2.6f; }
}
/* The radius a body plans with: 'Mechs and tanks are much bigger than a nominal 2.6 m -- their
 * hulls, measured while parked (vehicle_extent), reach 5-7 m -- so they keep that much clear of
 * walls, turrets and parked hulls instead of clipping them. */
static float plan_radius(const Ma2PlayerView *me)
{
    float r = kind_plan_radius(me->kind);
    if (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK)
        for (int i = 0; i < 48; i++)
            if (g_ext[i].actor == me->actor && g_ext[i].max > 0.5f) { float h = 0.6f * g_ext[i].max; r = h < r ? r : h > 4.0f ? 4.0f : h; break; }
    return r;
}
/* MoveTo slows down toward its point and halts 5-10 m short of it (LOCOMOTION_SPEC 3),
 * so the steering point is kept well ahead of the body. */
static float kind_lookahead(int k) { return k == MA2_KIND_PILOT ? 14.0f : k == MA2_KIND_VTOL ? 100.0f : 32.0f; }
static float kind_stop_margin(int k) { return k == MA2_KIND_PILOT ? 6.0f : 11.0f; }
/* TankOrbit (default on, RECOMP_BOT_TANK_ORBIT / [Bots] TankOrbit): TankPivot turns a STOPPED tank to its carrot; a tank that keeps driving
 * round a point inside its turning circle never stops: iw8a b7 (Snatch It Ill Wind, team 2) drove 131 m in a 15 m patch at its base exit
 * (1195-1209,1411-1424, t=15-56) toward the choke mouth 1188,1395 31 m off, all rays clear, until PocketBail bailed it out; pg81b b8 the
 * same 63-79 s. A shuttle or churn stall of a tank with the carrot 30 deg+ off its nose starts the pivot (turn on the spot, up to 3 s,
 * then MoveTo straight at it). [C7] tankpivot: orbit. */
static void tank_orbit_pivot(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 t)
{
    if (me->kind != MA2_KIND_TANK || c->tp_until > 0.0 || !c7_on("tankorbit", "RECOMP_BOT_TANK_ORBIT", "TankOrbit", 1)) return;
    if (hypotf(t.x - me->pos.x, t.z - me->pos.z) < 8.f || fabsf(wrap_pi(atan2f(t.x - me->pos.x, t.z - me->pos.z) - me->yaw)) < 0.52f) return;
    c->tp_pt = t; c->tp_until = w->time + 3.0; c->tp_t = 0.0; c7_add("tankpivot", "orbit", 1);
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-TANKPIVOT] t=%.1f orbiting at %.1f,%.1f,%.1f, carrot %.0f,%.0f: turning on the spot\n", w->time, me->pos.x, me->pos.y, me->pos.z, t.x, t.z);
}
/* Nearest a steering point may be while under way: MoveTo brakes for a point closer than this,
 * and a body that brakes at every corner walks in little steps instead of a steady stride. */
/* BattleArmor's 8 m sat inside MoveTo's 5-10 m halt band, so a BA braked at every
 * corner and stood still long enough for the progress watchdog (76 of 80 stalls on Killing Field,
 * nearly all single stalls in open ground), and each stall then sent it 4 s backwards. Now 14 m,
 * and a BA's first stall where the map shows open ground ahead replans without the backtrack.
 * RECOMP_BOT_BA_STRIDE=0 / [Bots] BaStride=0 = the old 8 m and backtrack. */
static int ba_stride_on(void) { return c7_on("f35", "RECOMP_BOT_BA_STRIDE", "BaStride", 1); }
/* BaWalk (default on, RECOMP_BOT_BA_WALK_FIRST / [Bots] BaWalk): a BattleArmor walks, as a player does -- its walk is full
 * speed (11-15 m/s under MoveTo, a player 15; measured 11-03 with every jet held off, RECOMP_TEST_NOJET). It was jetting
 * "first, whenever there is fuel": a hop every 2.5 s, a strafe hop every 3-5 s, a dodge hop every 3.5 s under fire, and
 * between them it stood on the spot in its run animation (playtest 11-03 laptop log: BAs running in place; set down at the flag
 * by a VTOL they jetted round it over- and undershooting instead of walking onto it; the standing is BaLand). Now travel hops
 * only on long open legs (90 m+ from the goal, the burn ending 40 m short of it), 4 s of walking after each; fight strafes
 * are walked (a hop now and then, 9-14 s apart); a dodge hop at most every 7 s (a missile dodge is unchanged). Jets for
 * ledges, roofs, BaUnstick / BaDipHop and blasts are unchanged. 0 = the old jets-first. [C7] bawalk: travel_hops, fight_hops. */
static int ba_walk_on(void) { static int on = -1; if (on < 0) on = c7_on("bawalk", "RECOMP_BOT_BA_WALK_FIRST", "BaWalk", 1); return on; }
static float kind_min_carrot(int k)
{
    switch (k) { case MA2_KIND_PILOT: return 8.0f; case MA2_KIND_BATTLEARMOR: return ba_stride_on() ? 14.0f : 8.0f; case MA2_KIND_TANK: return 18.0f; case MA2_KIND_VTOL: return 45.0f; default: return 20.0f; }
}

/* ---- command frames ---- */
static void ctrl_tap(BotCtrl *c, int id, int steps)
{
    if (id < 0 || id >= 60 || c->press[id] || c->val[id] != 0.f) return;   /* a fresh edge only */
    c->press[id] = (uint8_t)steps;
    c->taps[id]++;
}

/* A pad button as a player presses it: one physical button lights up several behaviour actions
 * at once -- one per control context (on foot, 'Mech, VTOL...) -- and each vehicle reads its own.
 * Measured from the keyboard (RECOMP_HUMAN_ACT, 2026-10-14). Pressing only one of them (39 for Y,
 * 37 for A, 12 for the right stick click) left a VTOL's exit, its helipad prompt and its descend
 * unanswered. */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LS, BTN_RS, BTN_COUNT };
static const signed char k_btn_actions[BTN_COUNT][6] = {
    { 19, 37, 51, -1 },             /* A      (Space) */
    { 17, 38, 55, 58, 59, -1 },     /* B      (F)     */
    { 15, 44, 49, 56, -1 },         /* X      (R)     */
    { 36, 39, 45, 50, -1 },         /* Y      (Q)     */
    { 3, 20, 48, -1 },              /* LS click (Shift) */
    { 12, 18, 52, -1 },             /* RS click (Ctrl)  */
};
static void ctrl_button(BotCtrl *c, int btn, int steps)
{
    for (int k = 0; k < 6 && k_btn_actions[btn][k] >= 0; k++) {
        int a = k_btn_actions[btn][k];
        if (c->press[a] < steps) c->press[a] = steps;
    }
}

/* One frame per step (what MAInput's sub_11AD8B builds): 60 command records updated
 * through sub_16F08C (value, held/released timers, edges), all active, then the
 * end-of-frame command 0x3C. Delivered to the player receiver sub_C8012. */
/* The player's trigger, for weapons the AI fire refuses (Alpha Strike). A human's fire input
 * reaches the weapon through the player struct: the behaviour's fire handler (sub_174A48 ->
 * sub_CC06A) copies the trigger record (action 18) to player+0x3C4 and sets +0x3E0, and the
 * player's fire step (sub_CC083 -> sub_DE453 -> the weapon's trigger state machine sub_D3806)
 * charges while it is held and fires on release. For a bot that copy never happens -- +0x3C4
 * stayed all zeros with the flag set, so the fire step saw a released trigger every frame and
 * the charge restarted each frame. Here: the same copy, each frame, from the record just sent. */
static void ctrl_player_trigger(int index, uint32_t player, int act)
{
    uint32_t cmd = g_frame_cmd[index];
    if (!cmd || !gva_ok(player, 0x3E4)) return;
    uint32_t rec = cmd + (uint32_t)act * 0x1Cu;
    for (uint32_t o = 0; o < 0x1C; o += 4) MEM32(player + 0x3C4 + o) = MEM32(rec + o);
    MEM8(player + 0x3E0) = 1;
    extern uint32_t g_ma2_trace_launcher;
    uint32_t L = g_ma2_trace_launcher;
    static int n;
    if (L && gva_ok(L, 0x1C0) && n < 300 && env_on("RECOMP_ALPHA_TRACE")) {
        n++;
        fprintf(stderr, "[TRIG] f=%u rec v=%.2f held=%.2f prev=%.2f fl=%08X r88=%d t84=%u now=%u\n",
                g_rt.bots[index].ctrl.frames, MEMF(rec + 4), MEMF(rec + 8), MEMF(rec + 0x10), MEM32(L + 0x78), (int)MEM32(L + 0x88),
                MEM32(L + 0x84), MEM32(0x004E2430u));
    }
}
static void ctrl_send_frame(int index, uint32_t player)
{
    BotCtrl *c = &g_rt.bots[index].ctrl;
    uint32_t beh = MEM32(player + 0x2F0);
    if (!gva_ok(beh, 0x6F0) || !(MEM32(player + 0x244) & 2)) return;        /* no behaviour / no control yet */
    if (!g_frame_msg[index]) { g_frame_msg[index] = xbox_HeapAlloc(0x740, 16); g_frame_cmd[index] = xbox_HeapAlloc(61 * 0x1C, 16); }
    uint32_t msg = g_frame_msg[index], cmd = g_frame_cmd[index];
    if (!msg || !cmd) return;
    if (!c->frames)
        for (uint32_t i = 0; i < 61; i++) { uint32_t r = cmd + i * 0x1C; for (uint32_t o = 0; o < 0x1C; o += 4) MEM32(r + o) = 0; MEM32(r) = i; }
    uint32_t step = MEM32(VA_STEP_TIME);
    for (uint32_t o = 0; o < 0x740; o += 4) MEM32(msg + o) = 0;
    for (uint32_t i = 0; i < 60; i++) {
        float v = 0.f;
        if (c->press[i]) { v = 1.f; c->press[i]--; }
        if (c->axis[i] != 0.f) { v = c->axis[i] == c->axis[i] ? (c->axis[i] < -1.f ? -1.f : c->axis[i] > 1.f ? 1.f : c->axis[i]) : 0.f; c->axis[i] = 0.f; }   /* ew-b8 B8.3 (R5 E3): never a NaN or a runaway on the pad */
        c->val[i] = v;
        uint32_t r = cmd + i * 0x1C;
        GuestRegs g; regs_save(&g);
        g_ecx = r; g_edx = step; g_xmm1 = XMM_SCALAR(v);
        PUSH32(g_esp, 0x00195C21u); sub_0016F08C();
        regs_restore(&g);
        for (uint32_t o = 0; o < 0x1C; o += 4) MEM32(msg + 0x1C + i * 0x1C + o) = MEM32(r + o);
        MEM8(msg + 0x6C8 + i) = 1;
    }
    MEM32(msg + 0x1C + 60 * 0x1C) = 0x3C;
    MEM8(msg + 0x6C8 + 60) = 1;
    rec_cmds(index, c->val);                          /* bot recorder: every command value sent */
    int trace = c->val[39] != 0.f && env_int("RECOMP_BOT_DIAG", 0) >= 1;
    uint32_t b_e4 = MEM8(beh + 0x6E4), b_cf = MEM8(beh + 0x6CF), b_c = MEM32(beh + 0xC);
    GuestRegs g; regs_save(&g);
    PUSH32(g_esp, msg); PUSH32(g_esp, player); PUSH32(g_esp, 0x000C7F8Bu);
    sub_000C8012();                                   /* stdcall 8 */
    regs_restore(&g);
    /* The trigger record just sent, into the player's fire input, every frame while it is in use. */
    if (c->trig_act > 0 && c->trig_act < 60) ctrl_player_trigger(index, player, c->trig_act);
    if (trace)
        fprintf(stderr, "[BOT-CMD] bot=%d id39 beh=%08X bvt=%08X pre(e4=%u cf=%u bC=%08X) post(e4=%u cf=%u bC=%08X rec v=%.2f prev=%.2f) actor=%08X p244=%08X p438=%08X p448=%08X msg39 v=%.2f prev=%.2f\n",
                index, beh, MEM32(beh), b_e4, b_cf, b_c, MEM8(beh + 0x6E4), MEM8(beh + 0x6CF), MEM32(beh + 0xC),
                MEMF(beh + 0x45C + 4), MEMF(beh + 0x45C + 0x10), player_actor(player), MEM32(player + 0x244), MEM32(player + 0x438), MEM32(player + 0x448),
                MEMF(msg + 0x1C + 39 * 0x1C + 4), MEMF(msg + 0x1C + 39 * 0x1C + 0x10));
    c->frames++;
}

/* ---- body: travel / stop / attack ---- */
/* An asleep campaign AI still "thinks" every ai+0x1C0 x 1.0-1.4 s -- 0.2-0.28 s for a pilot's MechAI
 * (think gate sub_16A806, next think ai+0x1C4 from sub_16A905) -- and each think that leaves it asleep
 * puts its body to sleep: comp+0x144 |= 0x60 (sub_183345, 0x18346A). The body's next update (walker
 * comp vt+0x44 -> sub_2AE860 -> sub_2AD3A0) then zeroes its velocity and forward speed and skips the
 * step. A pilot run by MoveTo or by its own turn + speed with the AI asleep therefore stopped dead 4-5
 * times a second -- two steps still, the run anim (actor+0x86C = 2) dropping to idle (0), a half-speed
 * step after: the stop-start gait (5.1 stops per second of running over 12 traced matches; 'Mechs,
 * driven awake, 0.3). The bot is this body's mind: while it drives the body, the AI's next think is
 * held a second ahead of the game clock (0x4E2440, compared as a float), so nothing puts the body to
 * sleep under it. RECOMP_BOT_PILOT_NOTHINK=0 restores the old behaviour. */
static void ai_hold_think(uint32_t a, uint32_t ai)
{
    if (actor_kind(a) != MA2_KIND_PILOT || !env_int("RECOMP_BOT_PILOT_NOTHINK", 1)) return;
    MEMF(ai + 0x1C4) = (float)(MEMD(VA_GAME_TIME) + 1.0);
}
static void body_moveto(uint32_t a, Ma2Vec3 p)
{
    if (!(p.x == p.x && p.y == p.y && p.z == p.z)) { c7_add("safety", "nan_moveto", 1); return; }   /* ew-b8 B8.4 (R5 E4): a NaN goal would steer the body off the map */
    uint32_t ai = actor_ai(a);
    if (!ai) return;
    ai_drive_body(a);                                 /* comp+0x144 bit0: the AI, not the control tick, moves it */
    ai_hold_think(a, ai);                             /* no asleep-AI think: it would stop the body (pilots) */
    MEM32(a + 0x1E8) |= 3u;
    MEM8(ai + 0x90) = 0;                              /* FSM asleep: no state overrides us */
    MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0;
    MEMF(ai + 0x130) = p.x; MEMF(ai + 0x134) = p.y; MEMF(ai + 0x138) = p.z;
    if (MEM32(ai + 0x12C) != 6u) ai_setmode(ai, 6);
    ai_moveto(ai, 0.f, 1);
}
static void body_stop(uint32_t a)
{
    uint32_t ai = actor_ai(a);
    if (!ai) return;
    ai_drive_body(a);
    MEM32(a + 0x1E8) |= 3u;
    {   /* A 'Mech or BattleArmor stopped in mid-air: an asleep MechAI marks its body "not
         * simulated" most frames, so it hung there sinking a few cm a second -- and a pilot
         * cannot climb out while airborne (a Grinder bot waited 26 s to eject). Kept awake and
         * sent to where it already is, it falls and lands like anyone. */
        int k = actor_kind(a);
        if ((k == MA2_KIND_MECH || k == MA2_KIND_BATTLEARMOR) && env_int("RECOMP_BOT_MECH_AWAKE", 1)) {
            Ma2Vec3 p = actor_pos(a); int ok = 0;
            float g = terrain_height(p.x, p.z, &ok);
            if (ok && p.y - g > 2.0f) {
                MEMF(a + 0x200) = 0.f; MEMF(a + 0x20C) = 0.f;
                MEM8(ai + 0x90) = 1;
                MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0;
                MEMF(ai + 0x130) = p.x; MEMF(ai + 0x134) = p.y; MEMF(ai + 0x138) = p.z;
                if (MEM32(ai + 0x12C) != 6u) ai_setmode(ai, 6);
                return;
            }
        }
    }
    MEM8(ai + 0x90) = 0;
    MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0;
    if (MEM32(ai + 0x12C) != 7u) ai_setmode(ai, 7);
    {   /* forward speed 0 (sub_2AD5C0), also ends a direct walk */
        recomp_func_t speed = resolve(0x002AD5C0u); uint32_t comp = MEM32(a + 0x8C);
        if (speed && gva_ok(comp, 0x150)) { GuestRegs r; regs_save(&r); g_edx = comp; g_eax = 1; PUSH32(g_esp, 0u); PUSH32(g_esp, 0x002AD631u); speed(); regs_restore(&r); }
    }
}
/* Close-range walk without the AI navigator (its local steering keeps a pilot off a
 * parked hull, so it never reaches an entry zone beside it): the body's own
 * TurnToward (sub_2AE360, yaw only) and forward speed (sub_2AD5C0, percent of max). */
static void body_walk_direct(uint32_t a, Ma2Vec3 from, Ma2Vec3 to, int pct)
{
    uint32_t ai = actor_ai(a), comp = MEM32(a + 0x8C);
    recomp_func_t face = resolve(0x002AE360u), speed = resolve(0x002AD5C0u);
    if (!ai || !gva_ok(comp, 0x150) || !face || !speed) return;
    ai_drive_body(a);
    ai_hold_think(a, ai);
    MEM32(a + 0x1E8) |= 3u;
    MEM8(ai + 0x90) = 0; MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0;
    if (MEM32(ai + 0x12C) != 7u) ai_setmode(ai, 7);
    GuestRegs r; regs_save(&r);
    g_esp -= 32; uint32_t v = g_esp & ~15u;
    MEMF(v) = to.x - from.x; MEMF(v + 4) = 0.f; MEMF(v + 8) = to.z - from.z;
    g_esp = v - 16; g_esi = comp; g_ecx = v; g_eax = 0; g_edx = 1;
    PUSH32(g_esp, f2u(4.0f)); PUSH32(g_esp, 0x002AE4E0u); face();
    g_esp = v - 16; g_edx = comp; g_eax = 1;
    PUSH32(g_esp, (uint32_t)pct); PUSH32(g_esp, 0x002AD631u); speed();
    regs_restore(&r);
}
/* TankPivot: a hull turned on the spot. The tank navigator keeps a turn radius in comp+0xD4 (5) during MoveTo, and its yaw rate is
 * speed / radius, so a stopped tank cannot turn (BAVT_SPEC: comp+0xD4 = 0, then the body's TurnToward sub_2AE360 each tick, 90 deg with no
 * error). AI idle (mode 7) so MoveTo does not put the radius back; forward speed 0. */
static void body_turn_in_place(uint32_t a, Ma2Vec3 from, Ma2Vec3 to)
{
    uint32_t ai = actor_ai(a), comp = MEM32(a + 0x8C);
    recomp_func_t face = resolve(0x002AE360u), speed = resolve(0x002AD5C0u);
    if (!ai || !gva_ok(comp, 0x150) || !face || !speed) return;
    ai_drive_body(a);
    MEM32(a + 0x1E8) |= 3u;
    MEM8(ai + 0x90) = 0; MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0;
    if (MEM32(ai + 0x12C) != 7u) ai_setmode(ai, 7);
    MEMF(comp + 0xD4) = 0.f;
    GuestRegs r; regs_save(&r);
    g_esp -= 32; uint32_t v = g_esp & ~15u;
    MEMF(v) = to.x - from.x; MEMF(v + 4) = 0.f; MEMF(v + 8) = to.z - from.z;
    g_esp = v - 16; g_esi = comp; g_ecx = v; g_eax = 0; g_edx = 1;
    PUSH32(g_esp, f2u(4.0f)); PUSH32(g_esp, 0x002AE4E0u); face();
    g_esp = v - 16; g_edx = comp; g_eax = 1;
    PUSH32(g_esp, 0u); PUSH32(g_esp, 0x002AD631u); speed();
    regs_restore(&r);
}
/* RECOMP_BOT_TRACE: which drive call moved the body this step (source line). */
static int g_drv_line; static char g_drv_kind;
#define body_moveto(a, p)            (g_drv_line = __LINE__, g_drv_kind = 'M', body_moveto((a), (p)))
#define body_stop(a)                 (g_drv_line = __LINE__, g_drv_kind = 'S', body_stop((a)))
#define body_walk_direct(a, f, t, q) (g_drv_line = __LINE__, g_drv_kind = 'W', body_walk_direct((a), (f), (t), (q)))
/* Steer a body toward p (the planner's carrot). 'Mechs and BattleArmor (both MechAI bodies): the
 * campaign MechAI awake in its goto
 * mode (6) walks to ai+0x130 at full stride -- an asleep MechAI marks its body "not simulated"
 * every frame (sub_183345: comp+0x144 |= 0x60), so a 'Mech pushed by MoveTo while its AI sleeps
 * only moves on some frames: a third of its speed in short steps (measured 6.6 of 18.8 m/s,
 * awake 18). It starts no fights of its own: EngageRadius / EngageRadiusPilot 0, target slot
 * empty (fire is ours, ctrl_fire). Pilots and tanks run at full speed with MoveTo (a pilot only while
 * its asleep AI's think is held off: ai_hold_think). */
static void body_steer(const Ma2PlayerView *me, Ma2Vec3 p)
{
    if (!(p.x == p.x && p.y == p.y && p.z == p.z)) { c7_add("safety", "nan_steer", 1); return; }   /* ew-b8 B8.4 */
    int walker = me->kind == MA2_KIND_MECH || (me->kind == MA2_KIND_BATTLEARMOR && env_int("RECOMP_BOT_BA_WALK", 1)) ||
                 (me->kind == MA2_KIND_VTOL && env_int("RECOMP_BOT_VTOL_AWAKE", 1));   /* its FlyerAI sleeps the same way */
    uint32_t ai = walker && env_int("RECOMP_BOT_MECH_AWAKE", 1) ? actor_ai(me->actor) : 0;
    if (!ai) { body_moveto(me->actor, p); return; }
    g_drv_line = __LINE__; g_drv_kind = 'A';
    ai_drive_body(me->actor);
    MEM32(me->actor + 0x1E8) |= 3u;
    MEMF(me->actor + 0x200) = 0.f; MEMF(me->actor + 0x20C) = 0.f;
    MEM8(ai + 0x90) = 1;
    MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0;
    MEMF(ai + 0x130) = p.x; MEMF(ai + 0x134) = p.y; MEMF(ai + 0x138) = p.z;
    if (MEM32(ai + 0x12C) != 6u) ai_setmode(ai, 6);
}

/* ---- BAVT_SPEC.md: MechAI jump, native fire, BattleArmor clamp ---- */
/* StartJump sub_18ABB5(ebx=ai; push str, push dur, push Vec3 *dir) ret 0xC -> al started. Needs jet
 * charge actor+0x49C > 1; MoveTo's per-tick sub_18AD06 then flies the jet along dir. */
static int ai_startjump(uint32_t ai, Ma2Vec3 dir, float dur, float str)
{
    recomp_func_t f = resolve(0x0018ABB5u); if (!f) return -1;
    if (env_on("RECOMP_TEST_NOJET")) return 0;   /* diag: walking only (BA walk A/B) */
    GuestRegs r; regs_save(&r);
    g_esp -= 16; uint32_t v = g_esp;
    MEMF(v) = dir.x; MEMF(v + 4) = dir.y; MEMF(v + 8) = dir.z;
    g_ebx = ai;
    PUSH32(g_esp, f2u(str)); PUSH32(g_esp, f2u(dur)); PUSH32(g_esp, v); PUSH32(g_esp, 0x0018A527u); f();
    int ok = (int)(g_eax & 0xFF); regs_restore(&r); return ok;
}
/* Jump toward p while MoveTo runs (walkers with jets: 'Mech, BattleArmor). */
/* Jet fuel: actor+0x49C, 0..2 (2 = full; StartJump needs > 1). */
static int body_jump_fuel_max(uint32_t a, Ma2Vec3 from, Ma2Vec3 p, float fuel, float maxdur)
{
    uint32_t ai = actor_ai(a);
    if (!ai || MEM8(ai + 0x240) || !(MEMF(a + 0x49C) >= fuel)) return 0;
    Ma2Vec3 d = { p.x - from.x, 0.f, p.z - from.z };
    float l = hypotf(d.x, d.z); if (l < 0.5f) return 0;
    d.x /= l; d.z /= l;
    /* The burn lasts as long as the fuel does (about 1 unit a second), never past it. */
    float dur = MEMF(a + 0x49C) - 0.15f;
    if (dur > maxdur) dur = maxdur;
    return ai_startjump(ai, d, dur < 2.0f ? dur : 2.0f, 1.0f) == 1;
}
static int body_jump_fuel(uint32_t a, Ma2Vec3 from, Ma2Vec3 p, float fuel) { return body_jump_fuel_max(a, from, p, fuel, 2.0f); }
/* HeatJet (default on, RECOMP_BOT_HEAT_JET / [Bots] HeatJet): a 'Mech's travel hops (cruise and route hops, not the up-jets, stall or
 * escape hops) wait while it runs hot (heat >= 65 %) with an enemy within 350 m: cu1_bw custom Base War b10 / b1 ran at heat 80-119 for
 * most of the match -- each hop +20-35 % -- and could not fire in the fights they jetted into (dmg 471 / 107 in 150 s). [C7] heatjet: held. */
static int heat_jet_hold(const Ma2World *w, const Ma2PlayerView *me)
{
    if (me->kind != MA2_KIND_MECH || me->heat < 65.f || !c7_on("heatjet", "RECOMP_BOT_HEAT_JET", "HeatJet", 1)) return 0;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->alive || p->player == me->player || (w->team_mode && p->nation == me->nation) || p->kind == MA2_KIND_NONE || hdist2(p->pos, me->pos) > 350.f) continue;
        c7_add("heatjet", "held", 1); return 1;
    }
    return 0;
}
/* W-33 HotUpJet (default on, RECOMP_BOT_HOT_UPJET / [Bots] HotUpJet): a 'Mech at 85% heat or more waits 2.5 s
 * before an up-jet it wants (each new wish waits again) -- the jet adds heat and locks its own guns (D-05: B_802 b3 jetted after a
 * carrier at 52 and 61 s, heat >= 85, and shut down). After 2.5 s the jet is allowed again (an escape
 * still has to happen). 'Mechs only; BattleArmor, stall and escape jets are not affected. [C7] hotupjet held. */
static int hot_upjet_hold(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    if (me->kind != MA2_KIND_MECH || me->heat < 85.f || !c7_on("hotupjet", "RECOMP_BOT_HOT_UPJET", "HotUpJet", 1)) { c->hotup_t = 0.0; return 0; }
    /* a new wish to jet (none asked in the last 0.5 s) starts a new 2.5 s hold: B_802 b3 stayed above 94% from
     * 49 to 63 s and jetted at 52 and 61 s; both must wait */
    if (c->hotup_t <= 0.0 || w->time < c->hotup_t || w->time - c->hotup_seen > 0.5) c->hotup_t = w->time;
    c->hotup_seen = w->time;
    if (w->time - c->hotup_t > 2.5) return 0;
    c7_add("hotupjet", "held", 1);
    return 1;
}
static int body_jump_toward(uint32_t a, Ma2Vec3 from, Ma2Vec3 p) { return body_jump_fuel(a, from, p, 1.9f); }
/* Up onto something (a roof, a ledge): the jet aimed at the point itself, height included, and
 * burned longer (RECOMP_BOT_UPJET_DUR, default 3 s; the jet's own fuel still cuts it). */
static int body_jump_up(uint32_t a, Ma2Vec3 from, Ma2Vec3 p, float fuel)
{
    uint32_t ai = actor_ai(a);
    if (!ai || MEM8(ai + 0x240) || !(MEMF(a + 0x49C) >= fuel)) return 0;
    Ma2Vec3 d = { p.x - from.x, p.y - from.y + 3.f, p.z - from.z };
    float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); if (l < 0.5f) return 0;
    d.x /= l; d.y /= l; d.z /= l;
    float dur = (float)env_int("RECOMP_BOT_UPJET_DUR10", 30) / 10.f, left = MEMF(a + 0x49C) - 0.15f;
    return ai_startjump(ai, d, dur < left ? dur : left, 1.0f) == 1;
}
/* GoalRise (default on, RECOMP_BOT_GOAL_RISE / [Bots] GoalRise): how far a goal is above the body, taking the goal no higher than
 * 1.5 m over the surface under it. A flag's position floats over its stand: read as "above us", a 'Mech by the flag skipped the
 * walk-in and jetted at it with a 3 s burn, landing 14-32 m past and walking back, again and again (laptop log 10-31). */
static float goal_rise(Ma2Vec3 goal, const Ma2PlayerView *me)
{
    float gy = goal.y;
    if (c7_on("goalrise", "RECOMP_BOT_GOAL_RISE", "GoalRise", 1)) { int ok = 0; float s = surface_height(goal.x, goal.z, &ok); if (ok && gy > s + 1.5f) gy = s + 1.5f; }
    return gy - me->pos.y;
}
/* Standing on something above the ground (a roof, a ledge): what a step there must stay on. */
static int on_raised(const Ma2PlayerView *me, float *surf)
{
    int ok = 0, oks = 0;
    float g = terrain_height(me->pos.x, me->pos.z, &ok);
    if (!ok || me->pos.y < g + 3.0f) return 0;
    float s = surface_height(me->pos.x, me->pos.z, &oks);
    if (!oks || s < g + 3.0f || fabsf(me->pos.y - s) > 1.5f) return 0;      /* in the air, not on a top */
    if (surf) *surf = s;
    return 1;
}
/* EdgeStep (default on, RECOMP_BOT_EDGE_STEP / [Bots] EdgeStep): a body stranded on a raised top (a ledge, a roof) whose route down it cannot walk steps off the
 * nearest edge: the first drop of 5 m+ in 16 directions out to 30 m (preferring the way toward the goal), and 3 m past it. Falls do no damage
 * (the planner's drop comment). Proving Ground ledge pilots stood 19-43 s on 174-205 m tops, with a route whose next leg was a drop (WP2-B, 11-02). */
static int edge_step_pt(const Ma2PlayerView *me, Ma2Vec3 goal, Ma2Vec3 *out)
{
    float best = 1e9f; int found = 0;
    float gb = atan2f(goal.x - me->pos.x, goal.z - me->pos.z);
    for (int k = 0; k < 16; k++) {
        float a = (float)k * 0.3927f, sx = sinf(a), cz = cosf(a), da = fabsf(a - gb);
        while (da > 3.14159f) da = fabsf(da - 6.28318f);
        for (float d = 2.f; d <= 30.f; d += 2.f) {
            int okg = 0, oks = 0;
            float px = me->pos.x + sx * d, pz = me->pos.z + cz * d;
            float g = terrain_height(px, pz, &okg), s = surface_height(px, pz, &oks);
            float top = oks && (!okg || s > g) ? s : g;
            if (!okg && !oks) break;
            if (top < me->pos.y - 5.f) {
                float score = d + 6.f * da;
                if (score < best) { best = score; out->x = me->pos.x + sx * (d + 3.f); out->z = me->pos.z + cz * (d + 3.f); out->y = me->pos.y; found = 1; }
                break;
            }
        }
    }
    return found;
}
/* Weapon of a fire group: arsenal actor+0xA4 children of class [0x4EAF4C], weapon+0x80 group,
 * the group's selected id arsenal+0x7C+4*group. */
static uint32_t weapon_of(uint32_t a, int group)
{
    uint32_t ars = MEM32(a + 0xA4), wcls = MEM32(0x004EAF4Cu), any = 0;
    if (!gva_ok(ars, 0x90)) return 0;
    for (uint32_t w = MEM32(ars + 0xC), n = 0; gva_ok(w, 0xA0) && n < 64; w = MEM32(w + 8), n++) {
        if (MEM32(w + 0x28) != wcls || (int)MEM32(w + 0x80) != group) continue;
        if (!any) any = w;
        if (MEM32(w + 0x98) == MEM32(ars + 0x7C + 4u * (uint32_t)group)) return w;
    }
    return any;
}
/* Fire sub_18482E(ai, Vec3 *from, Vec3 *aimPt, weapon, target) ret 0x14 -> al fired (AI inaccuracy and
 * fire-rate gate applied); aimPt = target->vt[0x134](&pt, 0). The projectile flies from the muzzle. */
static int ai_fire_off(uint32_t ai, uint32_t weapon, uint32_t tgt, Ma2Vec3 from, const Ma2Vec3 *off);
static int ai_fire(uint32_t ai, uint32_t weapon, uint32_t tgt, Ma2Vec3 from) { return ai_fire_off(ai, weapon, tgt, from, NULL); }
/* BurstFire (default on, RECOMP_BOT_BURST / [Bots] BotBurstFire; Veteran and up). The AI fire gate sub_1858EF keeps ONE refire time per AI
 * (ai+0x118 = now + the weapon's interval + rand(FireDelay actor+0x218: 1.5 s authored, Elite 1.2)), so a shot from any group shut the other
 * groups out: a bot fired one weapon at a time, about one shot a second, where a player fires all three triggers together (11-06 user
 * recording: 15-0 in TD; Elite 'Mechs 4-95 shots/min, firing 20-60 % of the time they had a shot). With the gate open (ai+0x118 = 0 before
 * the call) each group fires at its own interval (F74 f74_shot, barrel fire_ms) and the heat cycle (heat_holds) sets the pace. */
static int g_open_gate;
static float g_heat_add;   /* BurstFire: heat (percent) of the groups fired this step -- the 'Mech's heat in the world view is the step's start */
static double g_now_t;    /* BurstFire: the world time of the current ctrl_fire (heat_holds has no world) */
static int burst_on(void) { return c7_on("burst", "RECOMP_BOT_BURST", "BotBurstFire", 1); }
static int ai_fire_off(uint32_t ai, uint32_t weapon, uint32_t tgt, Ma2Vec3 from, const Ma2Vec3 *off)
{
    recomp_func_t f = resolve(0x0018482Eu);
    if (g_open_gate && gva_ok(ai, 0x120)) MEMF(ai + 0x118) = 0.f;
    recomp_func_t ap = gva_ok(tgt, 4) && gva_ok(MEM32(tgt), 0x138) ? resolve(MEM32(MEM32(tgt) + 0x134)) : NULL;
    if (!f || !ap || !weapon) return -1;
    GuestRegs r; regs_save(&r);
    g_esp -= 64; uint32_t v = g_esp & ~15u; g_esp = v;
    MEMF(v) = from.x; MEMF(v + 4) = from.y; MEMF(v + 8) = from.z;
    MEMF(v + 16) = 0; MEMF(v + 20) = 0; MEMF(v + 24) = 0;
    uint32_t base = v - 32; g_esp = base;
    g_ecx = tgt; PUSH32(g_esp, 0u); PUSH32(g_esp, v + 16); PUSH32(g_esp, 0x001B43FCu); ap();
    if (off) { MEMF(v + 16) += off->x; MEMF(v + 20) += off->y; MEMF(v + 24) += off->z; }
    g_esp = base;
    PUSH32(g_esp, tgt); PUSH32(g_esp, weapon); PUSH32(g_esp, v + 16); PUSH32(g_esp, v); PUSH32(g_esp, ai);
    PUSH32(g_esp, 0x001B4805u); f();
    int ok = (int)(g_eax & 0xFF); regs_restore(&r);
    if (ok == 1 && g_open_gate) {   /* BurstFire: count this group's heat toward the next group's check in the same step */
        WpnProps p; uint32_t a = MEM32(ai + 0x20);
        if (wpn_props(weapon, &p) && gva_ok(a, 0x870) && MEMF(a + 0x850) >= 1.f)
            g_heat_add += 100.f * (p.heat_shot > 0.f ? p.heat_shot : 0.5f * p.heat_sec) / MEMF(a + 0x850);
    }
    return ok;
}
/* BattleArmor clamp (claw path of the human tick 0x17305C): sub_10239A(eax=BA) "can clamp now",
 * then sub_1019EB(eax=BA) attaches (enemy 'Mech within 11 m). */
static int ba_clamp(uint32_t a)
{
    recomp_func_t can = resolve(0x0010239Au), go = resolve(0x001019EBu);
    if (!can || !go) return -1;
    GuestRegs r; regs_save(&r);
    g_eax = a; PUSH32(g_esp, 0x0017306Eu); can();
    int ok = (int)(g_eax & 0xFF);
    if (ok) { g_esp = r.sp; g_eax = a; PUSH32(g_esp, 0x00173079u); go(); }
    regs_restore(&r); return ok;
}

/* Let go: the attach routine itself with on = 0 (sub_10165B(ecx=BA; u8 on, Entity*, node, x)
 * ret 0x10 -> sub_102102(BA, 0, 0, 0)). The jump command does not loosen the claw on a VTOL. */
static int ba_release(uint32_t a)
{
    recomp_func_t f = resolve(0x0010165Bu);
    if (!f) return -1;
    GuestRegs r; regs_save(&r);
    g_ecx = a;
    PUSH32(g_esp, 0u); PUSH32(g_esp, 0u); PUSH32(g_esp, 0u); PUSH32(g_esp, 0u);   /* x, node, entity, on = 0 */
    PUSH32(g_esp, 0x00173079u); f();
    regs_restore(&r); return 1;
}

/* ---- Fire discipline + native aim and fire (BAVT_SPEC 4) ----
 * Every native shot: the target is alive, the AI's own collision ray from our eye to it is
 * clear (refreshed 4x a second), no teammate stands in the line of fire or the splash
 * around the target, and each weapon group fires only inside its own range. */
static int ctrl_target_alive(const Ma2World *w, uint32_t actor, const Ma2PlayerView **out)
{
    for (int i = 0; i < w->nplayers; i++)
        if (w->players[i].actor == actor && w->players[i].alive) { if (out) *out = &w->players[i]; return 1; }
    return 0;
}
/* F4 (hold-fire too coarse: a flat 8 m lane in 2-D, to 9 m past the target, so a teammate far
 * below a ridge or 7 m to the side held a bot's fire ~30 % of its fighting time). Now 3-D: a
 * teammate holds fire only within its body radius + 2 m of the muzzle-to-target segment, or
 * within 6 m of the target (splash). RECOMP_BOT_F4_FF=0 / ini [Bots] BotFriendlyFire3D=0 = the old lane. */
static float muzzle_h(int k);
static float kind_body_r(int k);
static float body_mid_h(int k)
{
    switch (k) { case MA2_KIND_MECH: return 5.f; case MA2_KIND_TANK: return 1.5f; case MA2_KIND_VTOL: case MA2_KIND_FLYER: return 0.f; default: return 1.f; }
}
static uint32_t g_ff_skip;   /* surprise set C peel-off: teammate actor the next F4 check skips (set by ctrl_fire for the shot at its rider) */
static int ally_in_line_3d(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 tp)
{
    Ma2Vec3 a = me->pos, b = tp;
    a.y += muzzle_h(me->kind); b.y += 3.f;
    float sx = b.x - a.x, sy = b.y - a.y, sz = b.z - a.z, l2 = sx * sx + sy * sy + sz * sz + 1e-3f;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (p == me || !p->alive || !p->actor || p->nation != me->nation) continue;
        if (g_ff_skip && p->actor == g_ff_skip) { c7_add("sp_peeloff", "f4_skips", 1); continue; }
        float px = p->pos.x, py = p->pos.y + body_mid_h(p->kind), pz = p->pos.z;
        float ex = px - b.x, ey = py - b.y, ez = pz - b.z;
        if (ex * ex + ey * ey + ez * ez < 36.f) return 1;                          /* in the splash */
        float s = ((px - a.x) * sx + (py - a.y) * sy + (pz - a.z) * sz) / l2;
        if (s <= 0.f || s >= 1.f) continue;                                         /* behind us / past the target */
        float cx = a.x + sx * s - px, cy = a.y + sy * s - py, cz = a.z + sz * s - pz, r = kind_body_r(p->kind) + 2.f;
        if (cx * cx + cy * cy + cz * cz < r * r) return 1;
    }
    return 0;
}
static int ally_in_line(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 tp)
{
    c7_add("F4", "checks", 1);
    if (c7_on("F4", "RECOMP_BOT_F4_FF", "BotFriendlyFire3D", 1)) { int h = ally_in_line_3d(w, me, tp); if (h) c7_add("F4", "holds", 1); return h; }
    float tx = tp.x - me->pos.x, tz = tp.z - me->pos.z, td = hypotf(tx, tz) + 1e-3f;
    tx /= td; tz /= td;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (p == me || !p->alive || !p->actor || p->nation != me->nation) continue;
        float rx = p->pos.x - me->pos.x, rz = p->pos.z - me->pos.z, along = rx * tx + rz * tz, lat = fabsf(rx * tz - rz * tx);
        if (along > 1.f && along < td + 9.f && lat < 8.f) { c7_add("F4", "holds", 1); return 1; }
    }
    return 0;
}
/* Which side of the line of fire the teammate in it stands (+1 / -1 along (uz, -ux), the
 * strafe axis of ctrl_fight); 0 when the line is clear. */
static int ally_line_side(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 tp)
{
    float tx = tp.x - me->pos.x, tz = tp.z - me->pos.z, td = hypotf(tx, tz) + 1e-3f;
    tx /= td; tz /= td;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (p == me || !p->alive || !p->actor || p->nation != me->nation) continue;
        float rx = p->pos.x - me->pos.x, rz = p->pos.z - me->pos.z, along = rx * tx + rz * tz, lat = rx * tz - rz * tx;
        if (along > 1.f && along < td + 9.f && fabsf(lat) < 8.f) return lat >= 0.f ? 1 : -1;
    }
    return 0;
}
static float weapon_range(uint32_t weapon)
{
    uint32_t def = gva_ok(weapon, 0x10) ? MEM32(weapon + 0xC) : 0;
    float r = gva_ok(def, 0x130) ? MEMF(def + 0x128) : 0.f;
    return r > 10.f && r < 2000.f ? r : 300.f;
}
static float muzzle_h(int k)
{
    switch (k) { case MA2_KIND_MECH: return 8.f; case MA2_KIND_TANK: return 3.f; case MA2_KIND_VTOL: return -1.5f; default: return 1.5f; }
}
/* Fresh line of sight for the shot (cached per target, 4 Hz). */
static float kind_body_r(int k);
static int ctrl_shot_los(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2PlayerView *t)
{
    if (c->fl_tgt != t->actor || w->time - c->fl_t >= 0.25 || w->time < c->fl_t) {
        Ma2Vec3 from = me->pos, to = t->pos;
        uint32_t hit = 0;
        from.y += kind_eye(me->kind); to.y += kind_eye(t->kind) * 0.6f;
        int clear = ai_los(me->actor, t->actor, from, to, &hit) == 1;
        if (!clear) {
            /* A wide body half behind cover: its flanks may still be open. One ray to each side of
             * its centre, half its width out across the line of fire. */
            float dx = to.x - from.x, dz = to.z - from.z, l = hypotf(dx, dz) + 1e-3f, half = kind_body_r(t->kind) * 0.8f;
            for (int sgn = -1; sgn <= 1 && !clear; sgn += 2) {
                Ma2Vec3 e = to; uint32_t h2 = 0;
                e.x += dz / l * half * (float)sgn; e.z -= dx / l * half * (float)sgn;
                clear = ai_los(me->actor, t->actor, from, e, &h2) == 1;
            }
        }
        if (!clear) c->last_hit_cls = gva_ok(hit, 0x30) ? MEM32(hit + 0x28) : 0;
        if (c->fl_tgt != t->actor) c->fl_seen = -100.0;
        c->fl_tgt = t->actor; c->fl_t = w->time; c->fl_clear = clear;
        if (clear) { c->fl_seen = w->time; c->los_clear++; } else c->los_block++;
    }
    return c->fl_clear;
}
/* 1 if this fire group is not worth firing at the target now (DEFENSE_SPEC.md, wpn_value_vs):
 * its running defence soaks most of it (absorber: 75-80 % of the types it covers, AMS: missiles,
 * shield bubble: everything), homing lost to a jammer, or the target fully cloaked -- a player
 * switches to the weapons that still get through. */
static int weapon_defended(const Ma2World *w, const Ma2PlayerView *t, uint32_t wpn, float dist)
{
    if (!t) return 0;
    return wpn_value_vs(t->actor, wpn, dist, w->time) < 0.4f;
}
/* F7 + F17 (Bot Skill did nothing to damage: the game's fire routine ignores the accuracy value
 * skill_tune_mover sets). Skill now lives in our own fire path:
 *  - aim error: the aim point is moved off the target by up to r100 m at 100 m (scaled with
 *    distance, at least the 30 m value), re-rolled every 0.35 s, and halved after 2 s of
 *    tracking the same target -- Recruits spray, Elites settle on you;
 *  - cadence: an extra pause after each shot from a fire group, and attention lapses
 *    (a chance per 3 s of holding fire 0.8 s). Elites: none.
 * RECOMP_BOT_SKILL_AIM=0 / ini [Bots] BotSkillAim=0 = off (the old fire path). [C7] F7 counters by skill. */
static const char *const k_f7_shots[4] = { "shots_recruit", "shots_regular", "shots_veteran", "shots_elite" };
static const char *const k_f7_rolls[4] = { "aim_rolls_recruit", "aim_rolls_regular", "aim_rolls_veteran", "aim_rolls_elite" };
static const char *const k_f7_err[4] = { "aim_err_m_sum_recruit", "aim_err_m_sum_regular", "aim_err_m_sum_veteran", "aim_err_m_sum_elite" };
static int skill_aim_on(void) { return c7_on("F7", "RECOMP_BOT_SKILL_AIM", "BotSkillAim", 1); }
static int skill_k(const BotCtrl *c) { return c->skill < 0 ? 0 : c->skill > 3 ? 3 : c->skill; }
static float aim_rnd(BotCtrl *c)
{
    uint32_t x = c->aim_rng ? c->aim_rng : 0x9E3779B9u ^ (uint32_t)(uintptr_t)c;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5; c->aim_rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}
static Ma2Vec3 skill_aim_off(BotCtrl *c, const Ma2World *w, uint32_t tgt, float d)
{
    static const float r100[4] = { 7.0f, 4.5f, 2.5f, 1.2f };   /* Recruit .. Elite, metres at 100 m */
    int k = skill_k(c);
    if (c->aim_tgt != tgt || w->time < c->aim_since) { c->aim_tgt = tgt; c->aim_since = w->time; c->aim_roll_t = -1.0; }
    if (c->aim_roll_t < 0.0 || w->time - c->aim_roll_t > 0.35 || w->time < c->aim_roll_t) {
        float track = (float)((w->time - c->aim_since) / 2.0); track = track < 0.f ? 0.f : track > 1.f ? 1.f : track;
        float r = (k == 3 && ma2_bots_dragoon() ? 0.4f : r100[k]) * (d > 30.f ? d : 30.f) / 100.f * (1.0f - 0.5f * track);   /* Dragoon: a third of Elite */
        float a = aim_rnd(c) * 6.2831853f, rr = r * sqrtf(aim_rnd(c));
        c->aim_off.x = cosf(a) * rr; c->aim_off.z = sinf(a) * rr; c->aim_off.y = r * 0.4f * (2.0f * aim_rnd(c) - 1.0f);
        c->aim_roll_t = w->time;
        c7_add("F7", k_f7_rolls[k], 1); c7_add("F7", k_f7_err[k], sqrtf(rr * rr + c->aim_off.y * c->aim_off.y));
    }
    return c->aim_off;
}
static int skill_cadence_ok(BotCtrl *c, const Ma2World *w, int g)
{
    static const float lapse_p[4] = { 0.40f, 0.20f, 0.08f, 0.0f };
    int k = skill_k(c);
    if (w->time - c->lapse_roll_t > 3.0 || w->time < c->lapse_roll_t) {
        c->lapse_roll_t = w->time;
        if (aim_rnd(c) < lapse_p[k]) c->lapse_until = w->time + 0.8;
    }
    if (w->time < c->lapse_until - 1.0) c->lapse_until = 0.0;      /* a new match: the clock went back */
    if (w->time < c->grp_next[g] - 2.0) c->grp_next[g] = 0.0;
    if (w->time < c->lapse_until || w->time < c->grp_next[g]) { c7_add("F7", "shots_skipped", 1); return 0; }
    return 1;
}
static void skill_fired(BotCtrl *c, const Ma2World *w, int g)
{
    static const float gap[4] = { 0.55f, 0.30f, 0.12f, 0.0f };     /* extra pause after a shot, s */
    int k = skill_k(c);
    c->grp_next[g] = w->time + gap[k];
    c7_add("F7", k_f7_shots[k], 1);
}
/* F41 (strafing beat every bot's slow shots: no target lead). The aim point moves ahead by the
 * target's velocity times the shot's flight time (distance / projectile speed), times a lead
 * quality by skill (0.30 / 0.55 / 0.80 / 0.95), at most 40 m. No template field for the speed is
 * known, so it is learnt from the projectiles in flight (w->proj, frame-to-frame velocity) keyed
 * by their template uid (projectile +0x30; a fire group's template uid is MAWpnGrp +0x7C).
 * No lead for beams, flamers and homing shots, or before a speed is known.
 * RECOMP_BOT_LEAD=0 / ini [Bots] BotLead=0 = off. [C7] F41 counters; [F41-PSPD] per template (DIAG). */
static struct { uint32_t tmpl; float speed; unsigned n; float avg; unsigned navg; } g_pspd[256];   /* 64 filled up in the smoke */
static int g_npspd;
/* ChargeFire lead: the PPC bolt accelerates (~50 -> 460 m/s, ch8_26), so its instantaneous speed overstates how fast it gets there and the
 * lead fell short by half. Each projectile's launch point and time are kept, and the template also learns its AVERAGE speed over the flight
 * (distance from launch / time since). */
static struct { uint32_t ent; double t0; Ma2Vec3 p0; } g_pfly[64];
static int lead_on(void) { return c7_on("F41", "RECOMP_BOT_LEAD", "BotLead", 1); }
static void lead_learn(const Ma2World *w)
{
    static double last = -1.0;
    if (w->time == last) return;
    if (w->time < last) { g_npspd = 0; memset(g_pspd, 0, sizeof g_pspd); memset(g_pfly, 0, sizeof g_pfly); }    /* a new match */
    last = w->time;
    for (int i = 0; i < w->nproj && i < 32; i++) {
        uint32_t ent = w->proj[i].ent;
        if (!gva_ok(ent, 0x40)) continue;
        Ma2Vec3 v = w->proj[i].vel;
        float sp = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        uint32_t tm = MEM32(ent + 0x30); int j;
        int fi = -1, fo = 0;
        for (int q = 0; q < 64; q++) { if (g_pfly[q].ent == ent) { fi = q; break; } if (g_pfly[q].t0 < g_pfly[fo].t0) fo = q; }
        if (fi < 0) { g_pfly[fo].ent = ent; g_pfly[fo].t0 = w->time; g_pfly[fo].p0 = w->proj[i].pos; }
        if (sp < 20.f || sp > 3000.f) continue;
        for (j = 0; j < g_npspd && g_pspd[j].tmpl != tm; j++) {}
        if (j == g_npspd) { if (g_npspd >= 256) continue; g_pspd[g_npspd].tmpl = tm; g_pspd[g_npspd].speed = sp; g_npspd++; c7_max("F41", "speeds_learnt", g_npspd); }
        else g_pspd[j].speed += (sp - g_pspd[j].speed) * 0.2f;
        g_pspd[j].n++;
        if (fi >= 0 && w->time - g_pfly[fi].t0 >= 0.5 && w->time - g_pfly[fi].t0 <= 3.0) {
            Ma2Vec3 p = w->proj[i].pos, q0 = g_pfly[fi].p0;
            float avg = sqrtf((p.x - q0.x) * (p.x - q0.x) + (p.y - q0.y) * (p.y - q0.y) + (p.z - q0.z) * (p.z - q0.z)) / (float)(w->time - g_pfly[fi].t0);
            if (avg > 20.f && avg < 3000.f) { g_pspd[j].avg = g_pspd[j].navg ? g_pspd[j].avg + (avg - g_pspd[j].avg) * 0.1f : avg; g_pspd[j].navg++; }
        }
    }
}
static float lead_speed(uint32_t wpn)
{
    uint32_t tm = gva_ok(wpn, 0xA0) ? MEM32(wpn + 0x7C) : 0;
    for (int j = 0; j < g_npspd; j++) if (g_pspd[j].tmpl == tm && g_pspd[j].n >= 2) return g_pspd[j].speed;
    return 0.f;
}
static Ma2Vec3 lead_off(const BotCtrl *c, const Ma2PlayerView *t, uint32_t wpn, float d)
{
    static const float q[4] = { 0.30f, 0.55f, 0.80f, 0.95f };
    Ma2Vec3 z = { 0.f, 0.f, 0.f };
    WpnProps p;
    if (!t || !wpn_props(wpn, &p)) return z;
    /* ChargeFire: the PPC is flagged a seeker, but the AI fire hands it no lock (sub_17B222 aims at a point), so its slow bolt flies straight:
     * unled, 21 of 23 charged shots missed a moving target at 60-360 m (ch6_26). A charge weapon is led like any straight shot. */
    if ((p.beam || p.flame || p.seeker || p.swarmer) && !(p.charge && p.wtype != 15)) { c7_add("F41", "skipped_kind", 1); return z; }
    float sp = lead_speed(wpn);
    if (p.charge) {   /* ChargeFire: the bolt's average speed over its flight (it accelerates) */
        uint32_t tm = gva_ok(wpn, 0xA0) ? MEM32(wpn + 0x7C) : 0;
        for (int j = 0; j < g_npspd; j++) if (g_pspd[j].tmpl == tm && g_pspd[j].navg >= 3) { sp = g_pspd[j].avg; c7_add("chargefire", "lead_avg", 1); break; }
    }
    if (sp <= 0.f) { c7_add("F41", "no_speed", 1); return z; }
    float k = d / sp * (skill_k(c) == 3 && ma2_bots_dragoon() ? 1.0f : q[skill_k(c)]);   /* Dragoon: perfect lead */
    Ma2Vec3 o = { t->vel.x * k, t->vel.y * k * 0.5f, t->vel.z * k };
    float m = sqrtf(o.x * o.x + o.y * o.y + o.z * o.z);
    if (m > 40.f) { float s = 40.f / m; o.x *= s; o.y *= s; o.z *= s; m = 40.f; }
    c7_add("F41", "lead_shots", 1); c7_add("F41", "lead_m_sum", m);
    return o;
}
static void lead_level_dump(void)
{
    if (env_on("RECOMP_BOT_DIAG"))
        for (int j = 0; j < g_npspd; j++) fprintf(stderr, "[F41-PSPD] tmpl=%08X speed=%.1f n=%u\n", g_pspd[j].tmpl, g_pspd[j].speed, g_pspd[j].n);
}
/* F30 ('Mech bots ignored heat: they ran into the game's overheat veto mid-fight and lost camo).
 * A heat weapon (barrel +0x168 heat per shot, +0xE8 per second for beams and flamers, in the 'Mech's
 * own units: HeatMax +0x850) holds fire when the shot would take the heat past a ceiling by skill:
 * Recruit 100 % (fires into the lock-out), Regular 95, Veteran 88, Elite 80; cloaked, Veteran and
 * Elite keep under 40 % (the camo shimmers above it). Weapons without heat fire as before.
 * RECOMP_BOT_HEAT=0 / ini [Bots] BotHeat=0 = off. [C7] F30 counters. */
static int heat_on(void) { return c7_on("F30", "RECOMP_BOT_HEAT", "BotHeat", 1); }
/* Batch 4 A/B (Elite 'Mech fires ~27% fewer shots/min than Veteran): EliteHeat88 = the Elite heat
 * ceiling 88 like Veteran, EliteKeep55 = F74 Elite keeps a weapon group at 0.55 of the best like
 * Veteran. A/B 10-27 (all-Elite TD 5v6, UC+NRC): KEEP55 kills/bmin +13/+25 %, deaths -20 %, so it is ON;
 * HEAT88 fired less and killed less, so it stays OFF. Own [C7] pieces. RECOMP_BOT_ELITE_HEAT88 / [Bots] EliteHeat88,
 * RECOMP_BOT_ELITE_KEEP55 / [Bots] EliteKeep55. */
static int eliteheat88_on(void) { return c7_on("eliteheat88", "RECOMP_BOT_ELITE_HEAT88", "EliteHeat88", 0); }
static int elitekeep55_on(void) { return c7_on("elitekeep55", "RECOMP_BOT_ELITE_KEEP55", "EliteKeep55", 1); }
static int vtol_timed_on(void) { return c7_on("f81", "RECOMP_BOT_VTOL_TIMED", "BotVtolTimed", 1); }
/* W6 push 2026-10-27 (playtest: VTOLs hover at a fixed height, snag on cliffs, follow ground ramps; heal landing is slow).
 * VtolTerrainClimb: a VTOL's travel is a straight line at the cruise height that clears the terrain along it
 * (the 'Mech route is only the fallback). VtolFastPad: the helipad run descends while it closes in and touches
 * down without the careful line-up. Own [C7] pieces vtclimb / vtpad. RECOMP_BOT_VTOL_TERRAIN_CLIMB /
 * RECOMP_BOT_VTOL_FAST_PAD over [Bots] VtolTerrainClimb / VtolFastPad (both default on). */
static int vt_climb_on(void) { return c7_on("vtclimb", "RECOMP_BOT_VTOL_TERRAIN_CLIMB", "VtolTerrainClimb", 1); }
static int vt_fastpad_on(void) { return c7_on("vtpad", "RECOMP_BOT_VTOL_FAST_PAD", "VtolFastPad", 1); }
static int vt_padlift_on(void) { return c7_on("vtpadlift", "RECOMP_BOT_VTOL_PAD_LIFT", "VtolPadLift", 1); }
static int ai_kick_on(void) { return c7_on("aikick", "RECOMP_BOT_AI_KICK", "AiKick", 1); }
static int yield_timeout_on(void) { return c7_on("yieldtimeout", "RECOMP_BOT_YIELD_TIMEOUT", "YieldTimeout", 1); }
static int fast_stall_on(void) { return c7_on("faststall", "RECOMP_BOT_FAST_STALL", "FastStall", 1); }
static int tank_drop_on(void) { return c7_on("tankdrop", "RECOMP_BOT_TANK_DROP", "TankDrop", 1); }
static int edge_step_on(void) { return c7_on("edgestep", "RECOMP_BOT_EDGE_STEP", "EdgeStep", 1); }
static int board_deep_on(void) { return c7_on("boarddeep", "RECOMP_BOT_BOARD_DEEP", "BoardDeep", 1); }
static int ba_centre_on(void) { return c7_on("bacentre", "RECOMP_BOT_BA_CENTRE", "BaCentre", 1); }
static int pocket_escape_on(void) { return c7_on("pocketescape", "RECOMP_BOT_POCKET_ESCAPE", "PocketEscape", 1); }
static int vt_padkick_on(void) { return c7_on("vtpadkick", "RECOMP_BOT_VTOL_PAD_KICK", "VtolPadKick", 1); }
static int raised_guard_on(void) { return c7_on("raisedguard", "RECOMP_BOT_RAISED_GUARD", "RaisedGuard", 1); }
static int arrive_spread_on(void) { return c7_on("arrivespread", "RECOMP_BOT_ARRIVE_SPREAD", "ArriveSpread", 1); }
static int frozen_pilot_on(void) { return c7_on("frozenpilot", "RECOMP_BOT_FROZEN_PILOT", "FrozenPilot", 1); }
static const char *const k_f30_holds[4] = { "holds_recruit", "holds_regular", "holds_veteran", "holds_elite" };
static int g_heat_prio;   /* HeatPush: ctrl_fire's target is a carrier or nearly dead */
static int heat_holds(BotCtrl *c, const Ma2PlayerView *me, uint32_t wpn)
{
    static const float ceil_pct[4] = { 100.f, 95.f, 88.f, 80.f };
    WpnProps p;
    if (me->kind != MA2_KIND_MECH || me->heat < 0.f || !gva_ok(me->actor, 0x870) || !wpn_props(wpn, &p)) return 0;
    float hmax = MEMF(me->actor + 0x850), per = p.heat_shot > 0.f ? p.heat_shot : 0.5f * p.heat_sec;
    if (per <= 0.f || hmax < 1.f) return 0;
    int k = skill_k(c);
    float lim = k == 3 && ma2_bots_dragoon() ? 93.f : k == 3 && eliteheat88_on() ? ceil_pct[2] : ceil_pct[k];   /* Dragoon: runs hotter */
    if (me->cloak && k >= 2) lim = 40.f;
    c7_max("F30", "heat_max", me->heat);
    if (!me->cloak && k >= 2 && burst_on()) {
        /* BurstFire heat cycle: everything fires until the 'Mech is hot (vent_at), then the heat weapons hold until it is down to cool_to
         * (the heat-free ones keep firing) -- bursts of every weapon, not a trickle at the ceiling. No single shot past `top` (100 = lock-out). */
        int dr = k == 3 && ma2_bots_dragoon();
        float vent_at = dr ? 88.f : k == 3 ? 85.f : 80.f, cool_to = dr ? 65.f : k == 3 ? 60.f : 55.f, top = dr ? 99.f : k == 3 ? 97.f : 92.f;
        float h = me->heat + g_heat_add;
        /* the vent latches only on our own burst (fired in the last 1.5 s): heat from the jump jets (audit r4_85 b10: 119 % after jetting up the
         * plateau, no shots for 10 s while it cooled to 78) only keeps a shot under `top`, it does not hold every heat weapon down to cool_to */
        int bursting = c->last_fire_t > 0.0 && g_now_t - c->last_fire_t < 1.5 && g_now_t >= c->last_fire_t;
        if (!c->heat_vent && h >= vent_at && bursting) { c->heat_vent = 1; c7_add("burst", "vents", 1); }
        else if (c->heat_vent && h <= cool_to) c->heat_vent = 0;
        if (!c->heat_vent && h + 100.f * per / hmax <= top) return 0;
        if (g_heat_prio && h + 100.f * per / hmax <= top) { c7_add("heatpush", "through_vent", 1); return 0; }   /* HeatPush: no cool-down pause on this one */
        c7_add("F30", k_f30_holds[k], 1);
        return 1;
    }
    if (me->heat + 100.f * per / hmax <= lim) return 0;
    c7_add("F30", k_f30_holds[k], 1);
    return 1;
}
/* F74 (weapon use looked scripted: every group fired whatever was in range, the secondaries on a
 * fixed every-other-half-second beat). Choice and rhythm now come from the weapon and the skill:
 *  - choice: each group in range is scored against the target (splash and flamers vs pilots and
 *    BattleArmor, homing and beams vs VTOLs and fast movers, lobbed shots poor against them, a
 *    long gun at range, a hot weapon when hot, the edge of a weapon's reach). Recruits keep one
 *    favourite group in range (re-picked every 4-8 s), Regulars fire everything in range,
 *    Veterans and Elites only the groups worth 55 % / 70 % of the best one;
 *  - rhythm: the trigger is held in bursts and let go between them (Recruit 1.4-3.4 s bursts,
 *    0.6-1.7 s gaps .. Elite near-continuous, 0.08-0.24 s gaps), and a group fires no faster
 *    than its own interval (barrel fire_ms) with a jitter.
 * RECOMP_BOT_F74=0 / ini [Bots] BotWeaponPlay=0 = off (the old fixed beat). [C7] F74 counters. */
static int f74_on(void) { return c7_on("F74", "RECOMP_BOT_F74", "BotWeaponPlay", 1); }
static const char *const k_f74_choice[4] = { "skip_choice_s0", "skip_choice_s1", "skip_choice_s2", "skip_choice_s3" };
static const char *const k_f74_pause[4] = { "skip_pause_s0", "skip_pause_s1", "skip_pause_s2", "skip_pause_s3" };
static const char *const k_f74_burst[4] = { "bursts_s0", "bursts_s1", "bursts_s2", "bursts_s3" };
static const char *const k_f74_shot[4] = { "shots_s0", "shots_s1", "shots_s2", "shots_s3" };
static float f74_score(const WpnProps *p, const Ma2PlayerView *me, const Ma2PlayerView *t, float d)
{
    float s = 1.f, spd = hypotf(t->vel.x, t->vel.z);
    int soft = t->kind == MA2_KIND_PILOT || t->kind == MA2_KIND_BATTLEARMOR;
    int fast = t->kind == MA2_KIND_VTOL || t->kind == MA2_KIND_FLYER || spd > 20.f;
    if (soft) { if (p->splash || p->flame) s *= 1.6f; else if (p->seeker) s *= 0.6f; }
    if (fast) { if (p->seeker) s *= 1.6f; if (p->beam) s *= 1.2f; if (p->lobbed) s *= 0.4f; }
    if (p->range > 10.f && d > 0.8f * p->range) s *= 0.6f;
    if (d > 200.f && p->range >= 400.f) s *= 1.3f;
    if (me->heat > 60.f && (p->heat_shot > 0.f || p->heat_sec > 0.f)) s *= 0.6f;
    /* ChargeFire: a charged PPC is the 'Mech's heaviest hit -- the first choice against armour (a 'Mech, a tank, BattleArmor) when it is not too hot */
    if (p->charge && p->wtype != 15 && !fast && t->kind != MA2_KIND_PILOT && me->heat <= 60.f && c7_on("chargefire", "RECOMP_BOT_CHARGE_FIRE", "ChargeFire", 1)) s *= 1.5f;
    return s;
}
/* 1 = this group may fire now (inside a burst, past its own interval). */
static int f74_cadence_ok(BotCtrl *c, const Ma2World *w, int g)
{
    static const float burst0[4] = { 2.4f, 1.6f, 1.1f, 3.0f }, gap0[4] = { 1.1f, 0.6f, 0.35f, 0.16f }, burstD[4] = { 2.4f, 1.6f, 1.1f, 5.0f }, gapD[4] = { 1.1f, 0.6f, 0.35f, 0.06f };
    const float *burst = ma2_bots_dragoon() ? burstD : burst0, *gap = ma2_bots_dragoon() ? gapD : gap0;   /* Dragoon: longer bursts, shorter pauses */
    int k = skill_k(c);
    double now = w->time;
    if (now < c->f74_next[g] - 5.0 || now < c->f74_shot[g] - 5.0) c->f74_next[g] = c->f74_burst_end[g] = c->f74_shot[g] = 0.0;   /* a new match */
    if (now >= c->f74_burst_end[g]) {
        if (now < c->f74_next[g]) { c7_add("F74", k_f74_pause[k], 1); return 0; }
        c->f74_burst_end[g] = now + burst[k] * (0.6f + 0.8f * aim_rnd(c));
        c->f74_next[g] = c->f74_burst_end[g] + gap[k] * (0.5f + aim_rnd(c));
        c7_add("F74", k_f74_burst[k], 1);
    }
    return now >= c->f74_shot[g];
}
static void f74_fired(BotCtrl *c, const Ma2World *w, int g, uint32_t wpn)
{
    WpnProps p;
    float iv = wpn_props(wpn, &p) && p.fire_ms > 0.f && p.fire_ms < 10000.f ? p.fire_ms / 1000.f : 0.f;
    c->f74_shot[g] = w->time + iv * (1.0f + 0.3f * aim_rnd(c));
    c7_add("F74", k_f74_shot[skill_k(c)], 1);
}
/* [C7] hg_mortar (high ground, bots batch 5 step 1): shots and mortar shots by where the shooter stands
 * (ground / roof / hill, ma2_brain_hg_place) for BattleArmor and 'Mechs, and up high by skill. */
static void hg_shot(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, uint32_t wpn)
{
    static const char *const shots[3][2] = { { "shots_ground_ba", "shots_ground_mech" }, { "shots_roof_ba", "shots_roof_mech" }, { "shots_hill_ba", "shots_hill_mech" } };
    static const char *const mortar[3][2] = { { "mortar_ground_ba", "mortar_ground_mech" }, { "mortar_roof_ba", "mortar_roof_mech" }, { "mortar_hill_ba", "mortar_hill_mech" } };
    static const char *const shots_up[4] = { "shots_up_recruit", "shots_up_regular", "shots_up_veteran", "shots_up_elite" };
    static const char *const mortar_up[4] = { "mortar_up_recruit", "mortar_up_regular", "mortar_up_veteran", "mortar_up_elite" };
    WpnProps p;
    int b = me->kind == MA2_KIND_BATTLEARMOR ? 0 : me->kind == MA2_KIND_MECH ? 1 : -1, pl = ma2_brain_hg_place((int)(me - w->players));
    int mo = wpn_props(wpn, &p) && p.wtype == 12;              /* k_wtype_name[12] = "mortar" */
    if (b < 0 || pl < 0 || pl > 2) return;
    c7_add("hg_mortar", shots[pl][b], 1);
    if (mo) c7_add("hg_mortar", mortar[pl][b], 1);
    if (pl) { c7_add("hg_mortar", shots_up[skill_k(c)], 1); if (mo) c7_add("hg_mortar", mortar_up[skill_k(c)], 1); }
}
/* VtolNose (default on, RECOMP_BOT_VTOL_NOSE / [Bots] VtolNose): the native fire sends the shot from the muzzle to the aim point whatever
 * way the body faces, so a VTOL flying off sideways or away (repositioning, ducking out, a fight move) launched its missiles out of its
 * flank or tail at a target behind it. A VTOL fires only at what lies within 30 deg of its nose (node forward +0xE0/+0xE8), as a pilot
 * aims by pointing the craft. [C7] vtnose: held. */
static int vtol_nose_ok(const Ma2PlayerView *me, Ma2Vec3 p)
{
    if (me->kind != MA2_KIND_VTOL || !c7_on("vtnose", "RECOMP_BOT_VTOL_NOSE", "VtolNose", 1)) return 1;
    uint32_t node = MEM32(me->actor + 0x94);
    float fx, fz;
    if (gva_ok(node, 0x100)) { fx = MEMF(node + 0xE0); fz = MEMF(node + 0xE8); } else { fx = sinf(me->yaw); fz = cosf(me->yaw); }
    float dx = p.x - me->pos.x, dz = p.z - me->pos.z, fl = hypotf(fx, fz), dl = hypotf(dx, dz);
    if (fl < 1e-3f || dl < 1.f) return 1;
    if ((fx * dx + fz * dz) / (fl * dl) >= 0.866f) return 1;
    c7_add("vtnose", "held", 1);
    return 0;
}
/* RECOMP_BOT_CHARGE_TRACE=1: a charge weapon's barrel state (+0x78 flags, +0x84 start, +0x88 percent) on every change, per bot and group;
 * after a release, that group's projectiles (ent+0x30 == MAWpnGrp+0x7C) and the target, every 0.1 s for 3 s. */
static struct { uint32_t tmpl, tgt; double t0, next; } g_chg_follow[16];
static int s_chg_g[16]; static double s_chg_t[16];   /* ChargeFire: the group (+1) holding a charge per bot, and the last call that held it */
static void charge_follow(int bi, const Ma2World *w)
{
    if (bi < 0 || bi >= 16 || !g_chg_follow[bi].tmpl || w->time > g_chg_follow[bi].t0 + 3.0 || w->time < g_chg_follow[bi].next) return;
    g_chg_follow[bi].next = w->time + 0.03;
    const Ma2PlayerView *t = NULL;
    for (int i = 0; i < w->nplayers; i++) if (w->players[i].actor == g_chg_follow[bi].tgt) t = &w->players[i];
    if (t) fprintf(stderr, "[CHG-TGT] t=%.2f bot=%d hp=%.0f\n", w->time, bi, t->hp);
    for (int i = 0; i < w->nproj && i < 32; i++) {
        uint32_t ent = w->proj[i].ent;
        if (!gva_ok(ent, 0x40) || MEM32(ent + 0x30) != g_chg_follow[bi].tmpl) continue;
        fprintf(stderr, "[CHG-PROJ] t=%.2f bot=%d ent=%08X pos=%.0f,%.0f,%.0f vel=%.0f,%.0f,%.0f tgt=%.0f,%.0f,%.0f miss=%.1f\n", w->time, bi, ent, w->proj[i].pos.x, w->proj[i].pos.y, w->proj[i].pos.z,
                w->proj[i].vel.x, w->proj[i].vel.y, w->proj[i].vel.z, t ? t->pos.x : 0.f, t ? t->pos.y : 0.f, t ? t->pos.z : 0.f,
                t ? sqrtf((w->proj[i].pos.x - t->pos.x) * (w->proj[i].pos.x - t->pos.x) + (w->proj[i].pos.z - t->pos.z) * (w->proj[i].pos.z - t->pos.z)) : -1.f);
    }
}
static void charge_trace(int tag, const BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    static uint32_t last[16][3][3];
    if (env_on("RECOMP_BOT_CHARGE_TRACE")) charge_follow((int)(((char *)c - (char *)&g_rt.bots[0].ctrl) / (ptrdiff_t)sizeof(g_rt.bots[0])), w);
    if (!env_on("RECOMP_BOT_CHARGE_TRACE") || me->kind != MA2_KIND_MECH) return;
    for (int g = 0; g < 3; g++) {
        uint32_t wp = weapon_of(me->actor, g), b = wp ? MEM32(wp + 0xC) : 0;
        if (!gva_ok(b, 0x200) || !(MEM32(b + 0x78) & 8u)) continue;
        int i = (int)(me - w->players) & 15;
        uint32_t f = MEM32(b + 0x78), s = MEM32(b + 0x84), p = MEM32(b + 0x88);
        if (tag || f != last[i][g][0] || s != last[i][g][1] || p != last[i][g][2])
            fprintf(stderr, "[CHG] t=%.3f p%d g%d %s fl=%08X start=%u pct=%d rate=%d min=%d hold=%.2f sel=%u now=%u\n", w->time, i, g, tag == 1 ? "FIRE-CALL" : tag == 2 ? "FIRE-OK" : "tick",
                    f, s, (int)p, (int)MEM32(b + 0x80), (int)MEM32(b + 0x98), MEMF(b + 0xA0), MEM32(me->actor + 0xF8), MEM32(0x004E2430u));
        last[i][g][0] = f; last[i][g][1] = s; last[i][g][2] = p;
    }
}
/* NoFireWhy (diag only, RECOMP_BOT_DIAG): why the last ctrl_fire of each bot did not shoot -- [BOT-NOFIRE] when a target in reach goes 1.5 s unshot. */
static const char *s_nf_why[16]; static double s_nf_t0[16], s_nf_log[16];
#define NF(why) do { int bx_ = (int)(((char *)c - (char *)&g_rt.bots[0].ctrl) / (ptrdiff_t)sizeof(g_rt.bots[0])); if (bx_ >= 0 && bx_ < 16) s_nf_why[bx_] = (why); } while (0)
static int ctrl_fire(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, uint32_t tgt)
{
    const Ma2PlayerView *t = NULL;
    uint32_t a = me->actor, ai = actor_ai(a);
    if (!ai || !tgt || !ctrl_target_alive(w, tgt, &t)) { NF("dead/none"); return 0; }
    if (!vtol_nose_ok(me, t->pos)) { NF("vtol_nose"); return 0; }
    if (c->alpha_hold) { NF("alpha_hold"); return 0; }                    /* charging the Alpha Strike: nothing else fires */
    if (w->time < c->bav_nofire_until && w->time + 1.0 > c->bav_nofire_until) { c7_add("deathblast", "fire_held", 1); NF("blast_hold"); return 0; }   /* BlastHoldFire */
    if (t->cloak && tgt != c->seen_tgt) { c7_add("cloak", "ctrl_holds", 1); NF("cloak"); return 0; }   /* F5: cloaked and not perceived: no aim, no shot */
    if (!ctrl_shot_los(c, w, me, t)) { NF("no_los"); return 0; }
    skill_tune_mover(a, c->skill);                           /* Bot Skill: the AI aim spread and fire delay sub_18482E uses */
    int peel = c->ff_skip && tgt == c->ff_rider;   /* surprise set C peel-off: shooting a clinging BA off a teammate */
    g_ff_skip = peel ? c->ff_skip : 0;
    if (w->team_mode && ally_in_line(w, me, t->pos)) { g_ff_skip = 0; c->ff_holds++; NF("ally_in_line"); return 0; }
    g_ff_skip = 0;
    float d = hdist2(t->pos, me->pos);
    if (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK) {
        /* 'Mech torso (sub_18AFDB) / tank turret + barrel (sub_18F874): eax = ai; push target. */
        recomp_func_t aim = resolve(me->kind == MA2_KIND_TANK ? 0x0018F874u : 0x0018AFDBu);
        if (aim) {
            GuestRegs r; regs_save(&r);
            g_eax = ai; PUSH32(g_esp, tgt); PUSH32(g_esp, me->kind == MA2_KIND_TANK ? 0x0018FAD3u : 0x001B44D9u); aim();
            regs_restore(&r);
        }
    }
    Ma2Vec3 from = me->pos; from.y += muzzle_h(me->kind);
    int fired = 0;
    g_heat_add = 0.f; g_now_t = w->time;
    uint32_t wpn[3]; float rng[3], longest = 0.f;
    for (int g = 0; g < 3; g++) {
        wpn[g] = weapon_of(a, g); rng[g] = wpn[g] ? weapon_range(wpn[g]) : 0.f;
        if (rng[g] > longest) longest = rng[g];
    }
    int bi_chg = -1;   /* ChargeFire: the group holding a charge right now */
    {
        int bx = (int)(((char *)c - (char *)&g_rt.bots[0].ctrl) / (ptrdiff_t)sizeof(g_rt.bots[0]));
        if (bx >= 0 && bx < 16 && s_chg_g[bx] && w->time >= s_chg_t[bx] && w->time - s_chg_t[bx] < 2.5) bi_chg = s_chg_g[bx] - 1;
    }
    /* HeatPush (default on, RECOMP_BOT_HEAT_PUSH / [Bots] HeatPush; film review 11-08): a player pushes the heat to just under the lock-out to stop
     * a carrier or finish a kill; the BurstFire vent (hold every heat gun from 85 % down to 60 %) made heat the commonest [BOT-NOFIRE] reason in
     * the par9 CTF matches (35-111 per match). On a carrier of an objective or a target under 30 %, no vent pause (still never past `top`). */
    g_heat_prio = 0;
    if (c7_on("heatpush", "RECOMP_BOT_HEAT_PUSH", "HeatPush", 1)) {
        if (t->hp_max > 0.f && t->hp < 0.3f * t->hp_max) g_heat_prio = 1;
        for (int q = 0; q < w->nobjectives && !g_heat_prio; q++) if (w->objectives[q].carrier && w->objectives[q].carrier == t->player) g_heat_prio = 1;
    }
    int ok[3], nf_def = 0, nf_heat = 0, nf_any = 0;
    for (int g = 0; g < 3; g++) {
        ok[g] = wpn[g] && d <= rng[g] * (c->sp_far ? 1.4f : 1.0f);   /* (surprise D: rookie fires out of range) */
        /* At range the long guns do the work: a weapon at the edge of its reach mostly misses. */
        if (ok[g] && d > 200.f && longest >= 400.f && rng[g] < 400.f) ok[g] = 0;
        if (ok[g] && weapon_defended(w, t, wpn[g], d)) { ok[g] = 0; nf_def = 1; }            /* the target's defence soaks it up */
        if (ok[g] && heat_on() && heat_holds(c, me, wpn[g]) && !(bi_chg >= 0 && g == bi_chg)) { ok[g] = 0; nf_heat = 1; }   /* F30: too hot for this one (a charge already held is judged at its press) */
        if (ok[g]) nf_any = 1;
    }
    {   /* ChargeFire: no 1-2 s charge at a VTOL while another group can shoot it (its bolt flies straight; a running 'Mech is fair game) */
        int fastt = t->kind == MA2_KIND_VTOL || t->kind == MA2_KIND_FLYER, other = -1;
        for (int g = 0; g < 3; g++) { uint32_t b = ok[g] ? MEM32(wpn[g] + 0xC) : 0; if (ok[g] && !(gva_ok(b, 0x200) && (MEM32(b + 0x78) & 8u))) other = g; }
        if (fastt && other >= 0 && me->kind == MA2_KIND_MECH && c7_on("chargefire", "RECOMP_BOT_CHARGE_FIRE", "ChargeFire", 1))
            for (int g = 0; g < 3; g++) { uint32_t b = ok[g] ? MEM32(wpn[g] + 0xC) : 0; if (gva_ok(b, 0x200) && (MEM32(b + 0x78) & 8u)) { ok[g] = 0; c7_add("chargefire", "skip_fast", 1); } }
    }
    int f74 = f74_on(), k74 = skill_k(c), pick = -1;
    float sc[3] = { 0.f, 0.f, 0.f }, best = 0.f;
    if (f74) {
        int n = 0, inr[3];
        for (int g = 0; g < 3; g++) {
            WpnProps p;
            if (!wpn[g] || d > rng[g] * (c->sp_far ? 1.4f : 1.0f) || !wpn_props(wpn[g], &p)) continue;
            sc[g] = f74_score(&p, me, t, d);
            if (ok[g] && sc[g] > best) best = sc[g];     /* the best group that can fire (a held one sets no floor) */
            inr[n++] = g;
        }
        if (k74 == 0 && n) {                         /* Recruit: one favourite group in range */
            int ok = 0;
            for (int i = 0; i < n; i++) if (inr[i] == c->f74_fav) ok = 1;
            if (!ok || w->time >= c->f74_fav_t || w->time < c->f74_fav_t - 10.0) {
                c->f74_fav = inr[(int)(aim_rnd(c) * (float)n) % n];
                c->f74_fav_t = w->time + 4.0 + 4.0 * aim_rnd(c);
            }
            pick = c->f74_fav;
        }
    }
    /* ChargeFire (default on, RECOMP_BOT_CHARGE_FIRE / [Bots] ChargeFire): a charge weapon (barrel+0x78 bit 3: the PPCs of the Uziel, Blood Asp,
     * Mad Cat...) fired through sub_18482E presses, holds while the charge (barrel+0x88) is under the AI's wanted charge ai+0x120, and lets go at
     * it (sub_185AB3). Only the campaign AI's sub_18AB1B ever sets ai+0x120; for a bot it stayed 0.0, so the second call released at 0 %, under
     * the barrel's minimum (+0x98), and the shot was cancelled (sub_CFE30 -> sub_CF914): the "tap" that never fired. The bot now sets the charge it
     * wants (by skill, at least the minimum, under 100 so it never overcharges) and calls that group every step until it lets go; no other group
     * fires meanwhile (that would clear the hold latch ai+0x96). [C7] chargefire: presses / shots / pct_sum / dropped. */
    int bi = (int)(((char *)c - (char *)&g_rt.bots[0].ctrl) / (ptrdiff_t)sizeof(g_rt.bots[0]));
    int chg_on = bi >= 0 && bi < 16 && me->kind == MA2_KIND_MECH && c7_on("chargefire", "RECOMP_BOT_CHARGE_FIRE", "ChargeFire", 1), cg = -1;
    if (chg_on && s_chg_g[bi]) {
        if (w->time >= s_chg_t[bi] && w->time - s_chg_t[bi] < 2.5) cg = s_chg_g[bi] - 1;
        else { s_chg_g[bi] = 0; MEM8(ai + 0x96) = 0; c7_add("chargefire", "dropped", 1); }   /* the next press starts clean; the trigger goes back to released (step-start rule) */
    }
    for (int g = 0; g < 3; g++) {
        if (!ok[g]) continue;
        if (peel) { WpnProps pp; if (!wpn_props(wpn[g], &pp) || pp.splash || pp.lobbed || pp.seeker || pp.flame) continue; }   /* direct fire only */
        uint32_t cb = chg_on && gva_ok(wpn[g], 0xA0) ? MEM32(wpn[g] + 0xC) : 0;
        int is_chg = gva_ok(cb, 0x200) && (MEM32(cb + 0x78) & 8u), holding = is_chg && g == cg;
        /* ChargeCool (default on, RECOMP_BOT_CHARGE_COOL / [Bots] ChargeCool; film review 11-08): the pause is for heat -- a cool 'Mech keeps every
         * gun going through the charge (A_61a b3, heat 5-8 %, group 1 silent 7-25 s at 44-127 m while the PPC charged). [C7] chargecool: fired_through. */
        int ccool = me->heat >= 0.f && me->heat < 50.f && c7_on("chargecool", "RECOMP_BOT_CHARGE_COOL", "ChargeCool", 1);
        if (cg >= 0 && g != cg && me->kind == MA2_KIND_MECH && ccool) c7_add("chargecool", "fired_through", 1);
        else if (cg >= 0 && g != cg && me->kind == MA2_KIND_MECH) {             /* ChargeFire: through a charge, only the guns that add no heat keep firing */
            WpnProps hp_;
            if (wpn_props(wpn[g], &hp_) && (hp_.heat_shot > 0.f || hp_.heat_sec > 0.f)) { c7_add("chargefire", "heat_wpn_paused", 1); continue; }
        }
        if (holding) { }                                                   /* mid-charge: no choice / cadence gates, keep holding */
        else if (f74) {
            static const float keep[4] = { 0.f, 0.f, 0.55f, 0.70f };
            float kp = k74 == 3 && elitekeep55_on() ? keep[2] : keep[k74];
            if (burst_on() && k74 >= 2) kp = k74 == 3 ? 0.f : 0.35f;   /* BurstFire: Elite / Dragoon fire every group in range, Veteran most */
            if ((pick >= 0 && g != pick) || sc[g] < kp * best) { c7_add("F74", k_f74_choice[k74], 1); continue; }
            if (!f74_cadence_ok(c, w, g)) continue;
        } else if (g == 1 && !(((int)(w->time * 2.0)) & 1)) continue;      /* secondaries every other half second */
        int aim = skill_aim_on(), lead = lead_on();
        if (!holding && aim && !skill_cadence_ok(c, w, g)) continue;
        if (is_chg) {
            static const float want[4] = { 60.f, 75.f, 85.f, 95.f };
            float mn = (float)(int)MEM32(cb + 0x98), p = k74 == 3 && ma2_bots_dragoon() ? 97.f : want[k74];
            if (t->kind == MA2_KIND_PILOT || t->kind == MA2_KIND_VTOL || t->kind == MA2_KIND_FLYER) p = minf2(p, 75.f);   /* a snap shot at small or fast ones */
            p = maxf2(p, mn + 1.f); if (p > 99.f) p = 99.f;
            MEMF(ai + 0x120) = p;
            /* the other groups keep firing through the charge; each such shot clears the hold latch ai+0x96 (sub_185AB3), which would turn
             * this call into a fresh press every step and the charge would never let go. The barrel stays armed, so the latch is put back. */
            if (holding) MEM8(ai + 0x96) = 1;
            if (!holding) c7_add("chargefire", "presses", 1);
        }
        charge_trace(1, c, w, me);
        g_open_gate = burst_on() && k74 >= 2 && !is_chg;   /* BurstFire: this group's own interval, not the AI's shared gate (a charge keeps its proven path) */
        if (g_open_gate && heat_on() && heat_holds(c, me, wpn[g])) { g_open_gate = 0; continue; }   /* with what this step already fired */
        int was_fired = fired;
        if (c->blind && t->cloak) {                 /* F83: at the predicted spot, not the body (no lead: it is a guess) */
            Ma2Vec3 off = { c->blind_pt.x - t->pos.x, 0.f, c->blind_pt.z - t->pos.z };
            if (aim) { Ma2Vec3 s = skill_aim_off(c, w, tgt, d); off.x += s.x; off.y += s.y; off.z += s.z; }
            if (ai_fire_off(ai, wpn[g], tgt, from, &off) == 1) { fired = 1; c->shots++; hg_shot(c, w, me, wpn[g]); if (aim) skill_fired(c, w, g); if (f74) f74_fired(c, w, g, wpn[g]); c7_add("f83", "blind_shots", 1); }
        } else if (aim || lead) {
            Ma2Vec3 off = { 0.f, 0.f, 0.f };
            if (aim) off = skill_aim_off(c, w, tgt, d);
            if (lead) { lead_learn(w); Ma2Vec3 l = lead_off(c, t, wpn[g], d); off.x += l.x; off.y += l.y; off.z += l.z; }
            if (ai_fire_off(ai, wpn[g], tgt, from, &off) == 1) { fired = 1; c->shots++; hg_shot(c, w, me, wpn[g]); if (aim) skill_fired(c, w, g); if (f74) f74_fired(c, w, g, wpn[g]); }
        } else if (ai_fire(ai, wpn[g], tgt, from) == 1) { fired = 1; c->shots++; hg_shot(c, w, me, wpn[g]); if (f74) f74_fired(c, w, g, wpn[g]); }
        if (is_chg && env_on("RECOMP_BOT_CHARGE_TRACE"))
            fprintf(stderr, "[CHG] t=%.3f p%d bot=%d g%d AFTER ret=%d fl=%08X start=%u pct=%d latch=%u want=%.0f hold=%d tgt=%08X kind=%d d=%.0f dmg=%d\n", w->time, (int)(me - w->players), bi, g, fired && !was_fired,
                    MEM32(cb + 0x78), MEM32(cb + 0x84), (int)MEM32(cb + 0x88), MEM8(ai + 0x96), MEMF(ai + 0x120), holding, tgt, t->kind, d, (int)MEM32(cb + 0x148));
        if (is_chg) {
            if (fired && !was_fired && env_on("RECOMP_BOT_CHARGE_TRACE")) {
                g_chg_follow[bi].tmpl = MEM32(wpn[g] + 0x7C); g_chg_follow[bi].tgt = tgt; g_chg_follow[bi].t0 = w->time; g_chg_follow[bi].next = 0.0;
                fprintf(stderr, "[CHG] t=%.3f bot=%d RELEASE tmpl=%08X me=%.0f,%.0f,%.0f yaw=%.0f tgt=%.0f,%.0f,%.0f tvel=%.1f,%.1f lead_speed=%.0f\n", w->time, bi, MEM32(wpn[g] + 0x7C),
                        me->pos.x, me->pos.y, me->pos.z, me->yaw * 57.3f, t->pos.x, t->pos.y, t->pos.z, t->vel.x, t->vel.z, lead_speed(wpn[g]));
            }
            if (fired && !was_fired) { s_chg_g[bi] = 0; c->press[18] = 0; c7_add("chargefire", "shots", 1); continue; }
            s_chg_g[bi] = g + 1; s_chg_t[bi] = w->time; cg = g;            /* pressed / still charging: hold it next step */
            /* The player's own fire step (sub_CC083 -> sub_DE453 -> sub_D3806) runs every frame on the selected class (actor+0xF8) and, with the
             * bot's trigger record idle, reads "released" -- which cancelled the charge each frame (trace: fl 0x229 after the press, 0x009 by the
             * next step, +0x88 stuck at 0). Hold the trigger there too, on this class, as the Alpha Strike does: both paths then hold one charge. */
            if (MEM32(a + 0xF8) != 7u) MEM32(a + 0xF8) = (uint32_t)g;
            c->press[18] = 2; c->trig_act = 18;
        }
    }
    g_open_gate = 0; g_heat_add = 0.f; g_heat_prio = 0;
    if (!fired) NF(nf_any ? "cadence/aim" : nf_heat ? "heat" : nf_def ? "defended" : "range");
    if (fired) c->last_fire_t = w->time;
    if (fired) charge_trace(2, c, w, me);
    if (fired && t->cloak) c7_add("cloak", "shots_at_cloaked", 1);   /* only ever at a perceived one (seen_tgt) */
    return fired;
}

/* An objective building (Destruction modes): the same native aim and fire as at a body, at a
 * structure -- in sight (the AI's own ray, the building itself does not block it) and in range
 * of each fire group; no teammate in the line. */
static int ctrl_fire_structure(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, uint32_t ent, Ma2Vec3 pos)
{
    uint32_t a = me->actor, ai = actor_ai(a);
    if (!ai || !ent || !gva_ok(ent, 0x30)) return 0;
    if (!vtol_nose_ok(me, pos)) return 0;
    float d = hdist2(pos, me->pos);
    Ma2Vec3 from = me->pos, to = pos; uint32_t hit = 0;
    from.y += muzzle_h(me->kind); to.y += 6.f;
    if (ai_los(a, ent, from, to, &hit) != 1 && hit != ent) {
        static double last; if (env_on("RECOMP_BOT_DIAG") && w->time - last > 2.0) { last = w->time;
            fprintf(stderr, "[BOT-OBJFIRE] no line to %08X (cls %08X) at %.0f,%.0f,%.0f from %.0f,%.0f,%.0f: hit %08X cls %08X\n", ent, MEM32(ent + 0x28), pos.x, pos.y, pos.z,
                    me->pos.x, me->pos.y, me->pos.z, hit, gva_ok(hit, 0x30) ? MEM32(hit + 0x28) : 0u); }
        return 0;
    }
    if (w->team_mode && ally_in_line(w, me, pos)) { c->ff_holds++; return 0; }
    skill_tune_mover(a, c->skill);
    if (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK) {
        recomp_func_t aim = resolve(me->kind == MA2_KIND_TANK ? 0x0018F874u : 0x0018AFDBu);
        if (aim) {
            GuestRegs r; regs_save(&r);
            g_eax = ai; PUSH32(g_esp, ent); PUSH32(g_esp, me->kind == MA2_KIND_TANK ? 0x0018FAD3u : 0x001B44D9u); aim();
            regs_restore(&r);
        }
    }
    int fired = 0;
    g_heat_add = 0.f; g_now_t = w->time;
    for (int g = 0; g < 3; g++) {
        uint32_t wpn = weapon_of(a, g);
        if (!wpn || d > weapon_range(wpn)) continue;
        int open = burst_on() && skill_k(c) >= 2;   /* BurstFire: every group at its own interval on an objective building too */
        if (open) { if (w->time < c->f74_shot[g] && w->time > c->f74_shot[g] - 10.0) continue; }
        else if (g == 1 && !(((int)(w->time * 2.0)) & 1)) continue;
        if (open && heat_on() && heat_holds(c, me, wpn)) continue;
        g_open_gate = open;
        if (ai_fire(ai, wpn, ent, from) == 1) { fired = 1; c->shots++; hg_shot(c, w, me, wpn); if (open) f74_fired(c, w, g, wpn); }
        g_open_gate = 0;
    }
    if (fired) c->last_fire_t = w->time;
    return fired;
}
/* Point `ahead` metres along the path from the bot (pure pursuit), kept inside a free
 * corridor of the traversability grid so corners are not cut through walls. */
/* Steering point: the farthest point along the planned path, within `ahead` metres, that is in
 * clear sight from here (the corridor free on the grid and of parked hulls). The path is sampled
 * every 2 m, so the point slides round a corner as soon as the way round can be seen and the body
 * starts its turn early -- an arc past the obstacle, the way a player angles round a turret,
 * instead of a walk to the corner and a hard turn there. */
static int hulls_clear(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 a, Ma2Vec3 b, uint32_t skip);
/* JetTryFuel (default on, RECOMP_BOT_JET_TRY_FUEL / [Bots] JetTryFuel; parity pass 11-10): a jet-up try was judged 6 s after the body reached the
 * leg's foot, jets fired or not -- after a climb burn the fuel is low and the next burn waits, so the try 'failed' unfired and the spot was barred
 * for EVERY bot for 10 min: Ill Wind's terrace jet-up (the only way from the basin to the core pile, ~690,1380) was barred at 44-52 s in 4 of 7
 * runs (P5_81a1 b7, P6_81a1 b5, P6_81a2 b8, P7_81a1 b7). Now 6 s from the first burn of the try (20 s if no burn could fire). [C7] f38: tries, jets. */
static int jet_try_fuel_on(void) { static int on = -1; if (on < 0) on = c7_on("jettryfuel", "RECOMP_BOT_JET_TRY_FUEL", "JetTryFuel", 1); return on; }
static int leap_top_on(void) { static int on = -1; if (on < 0) on = c7_on("leaptop", "RECOMP_BOT_LEAP_TOP", "LeapTop", 1); return on; }
/* LeapStallFar (default on, RECOMP_BOT_LEAP_STALL_FAR / [Bots] LeapStallFar; parity pass 9): a body STANDING up to 24 m short of a leap top (not
 * 18) leaps from there -- the goto's stop-go approach to a carrot pinned at the top: W9_81b1 b8 (Ill Wind Snatch It T2) stood at 1169,14,1510
 * 19 m short at 12.07-12.4 s, walked 8 m, stood again and leapt at 13.4. [C7] leapstall: far. */
static int leap_stall_far_on(void) { static int on = -1; if (on < 0) on = c7_on("leapstallfar", "RECOMP_BOT_LEAP_STALL_FAR", "LeapStallFar", 1); return on; }
static int leap_stall_up_far_on(void) { static int on = -1; if (on < 0) on = c7_on("leapstallupfar", "RECOMP_BOT_LEAP_STALL_UP_FAR", "LeapStallUpFar", 1); return on; }
static int jet_over_yield_on(void) { static int on = -1; if (on < 0) on = c7_on("jetoveryield", "RECOMP_BOT_JET_OVER_YIELD", "JetOverYield", 1); return on; }
static int leap_stall_on(void) { static int on = -1; if (on < 0) on = c7_on("leapstall", "RECOMP_BOT_LEAP_STALL", "LeapStall", 1); return on; }
static Ma2Vec3 path_carrot(const Ma2Path *p, const Ma2World *w, const Ma2PlayerView *me, uint32_t skip,
                           float ahead, float radius, double now, int *at_end)
{
    int i = p->next;
    Ma2Vec3 pos = me->pos, prev = me->pos, best = p->pt[i];
    float walked = 0.f;
    *at_end = 0;
    for (; i < p->n; i++) {
        Ma2Vec3 q = p->pt[i];
        float seg = hypotf(q.x - prev.x, q.z - prev.z);
        int steps = (int)ceilf(seg / 2.f);
        if (steps < 1) steps = 1;
        for (int s = 1; s <= steps; s++) {
            float t = (float)s / (float)steps;
            Ma2Vec3 cand = { prev.x + (q.x - prev.x) * t, q.y, prev.z + (q.z - prev.z) * t };
            if (walked + seg * t > ahead) return best;
            if (ma2_trav_ready() && hdist2(pos, cand) > 3.f &&   /* a VTOL too: MaxVTOLHeight keeps it under the tall buildings */
                (!(p->crush ? ma2_trav_segment_heavy(pos, cand, radius * 0.95f, now) : ma2_trav_segment(pos, cand, radius * 0.95f, now)) ||
                 !hulls_clear(w, me, pos, cand, skip)))
                return best;                                    /* out of sight from here: steer for the last point in sight */
            best = cand;
        }
        /* LeapTop (default on, RECOMP_BOT_LEAP_TOP / [Bots] LeapTop; parity pass 11-10): the carrot stops at the top of a step-off or jet leg -- the
         * leap fires only within 8-12 m of that top, and a carrot run on along the leg pulled the body across the corner to an edge that is not the
         * top: P3_81a1 b8 (Ill Wind team 2) stood at 1162,13,1502 over the basin 11.3-14.5 s steering for 1143,1505 with the plan's leap top 30 m
         * along the rim, until a replan. [C7] leaptop: stop_s. */
        if (i + 1 < p->n && p->drop[i + 1] && leap_top_on()) { c7_add("leaptop", "stop_s", w->dt); return q; }
        walked += seg; prev = q;
    }
    *at_end = 1;
    return best;
}

/* Bodies around us as circles: every parked vehicle, every other unit (any team: pilots, BattleArmor,
 * 'Mechs, tanks, grounded VTOLs) -- sized by kind. `slow` (set per entry) = moving under 2 m/s:
 * those go into the plan; faster ones are left to local avoidance. `skip` = an actor never listed
 * (only used for the vehicle a pilot is about to climb into when it must not block the zone). */
typedef struct Obst { float x, z, r, vx, vz; int slow, team; uint32_t actor; int kind; } Obst;
static float kind_body_r(int k)
{
    switch (k) { case MA2_KIND_PILOT: return 0.6f; case MA2_KIND_BATTLEARMOR: return 1.6f; case MA2_KIND_MECH: case MA2_KIND_TANK: return 3.8f;
                 case MA2_KIND_VTOL: case MA2_KIND_FLYER: return 3.5f; case MA2_KIND_MOVER: case MA2_KIND_TURRET: return 3.0f; default: return 2.0f; }
}
/* How far a body reaches: its kind's nominal radius, or for a 'Mech or tank 0.8 x its hull as
 * measured while it stood parked (arms and guns stick out 5-6 m from a 'Mech's centre). */
static float body_r_of(uint32_t actor, int kind)
{
    float r = kind_body_r(kind);
    if (kind == MA2_KIND_MECH || kind == MA2_KIND_TANK)
        for (int i = 0; i < 48; i++)
            if (g_ext[i].actor == actor && g_ext[i].max > 0.5f) { float h = 0.8f * g_ext[i].max; if (h > r) r = h > 6.0f ? 6.0f : h; break; }
    return r;
}
static float kind_cruise(int k)
{
    switch (k) { case MA2_KIND_PILOT: return 7.f; case MA2_KIND_BATTLEARMOR: return 12.f; case MA2_KIND_TANK: return 14.f; default: return 16.f; }
}
/* Seconds until two discs, relative position p and relative velocity v, come within R (0 = already,
 * < 0 = not on these courses). */
static float time_to_touch(float px, float pz, float vx, float vz, float R)
{
    float c = px * px + pz * pz - R * R, a = vx * vx + vz * vz, b = px * vx + pz * vz;
    if (c <= 0.f) return 0.f;
    if (a < 1e-4f || b >= 0.f) return -1.f;
    float disc = b * b - a * c;
    if (disc < 0.f) return -1.f;
    return (-b - sqrtf(disc)) / a;
}
/* C4 hybrid avoidance ([C7] avoid ...: avoid_ms = the crowd steering's cost per frame over all
 * bots, target < 0.2 ms at 6v6). Kept here only for the budget: over it for a second -> the swerve
 * map checks are reused on alternate frames (cache) until it is well under again. */
static struct { double ft, frame_ms, sec_ms; unsigned frame, sec_frames; int cache; } g_av = { -1.0 };
static void avoid_frame(const Ma2World *w)
{
    if (w->time == g_av.ft) return;
    if (w->time < g_av.ft - 1.0) memset(&g_av, 0, sizeof g_av);    /* the clock went back: a new match */
    else if (g_av.ft >= 0.0) {                                       /* close the frame just finished */
        if (g_av.frame_ms > 0.2) c7_add("avoid", "over02", 1);
        if (g_av.cache) c7_add("avoid", "cache_frames", 1);
        g_av.sec_ms += g_av.frame_ms;
        if (++g_av.sec_frames >= 60) {
            double avg = g_av.sec_ms / g_av.sec_frames;
            if (avg > 0.2) g_av.cache = 1; else if (avg < 0.12) g_av.cache = 0;
            g_av.sec_ms = 0.0; g_av.sec_frames = 0;
        }
    }
    g_av.ft = w->time; g_av.frame_ms = 0.0; g_av.frame++;
}
/* Right of way when boxed in: flag carrier > badly hurt > already rolling (by how fast). */
static float avoid_urgency(const Ma2World *w, const Ma2PlayerView *p)
{
    float u = 0.f;
    for (int i = 0; i < w->nobjectives; i++) if (w->objectives[i].carrier && w->objectives[i].carrier == p->player) { u += 8.f; break; }
    if (p->hp_max > 0.f && p->hp / p->hp_max < 0.35f) u += 4.f;
    return u + 2.f * minf2(hypotf(p->vel.x, p->vel.z) / kind_cruise(p->kind), 1.f);
}
/* Airspace a VTOL could not get into: an invisible barrier (over a hill, round a base) the
 * sight rays pass through. Learned when the awake FlyerAI and then the game's own MoveTo both
 * make no headway toward a point with nothing in the way; shared by every bot on the map. */
#define AIRBLK_MAX 48
static struct { float x, z, r; } g_airblk[AIRBLK_MAX];
static int g_nairblk, g_airblk_next;
static Ma2Vec3 g_pads[16]; static int g_npads;   /* ew-b8 B8.8 (R5 E8): was 8; skip_mask is 32 bits */
static struct { float x, z; int n; } g_team_home[16];
/* VehPad (default on, RECOMP_BOT_VEH_PAD / [Bots] VehPad): where a vehicle stands unoccupied the first time its actor is seen (the
 * map's start lineup, a respawn) is a spawn spot. A VTOL respawned on a tank crossing team 1's VTOL spot on Ill Wind (rc9_63b t=53.3,
 * VTOL 02146130 at 331,18,977, contact d=2.5 m): the tank sat pinned 15 s until PocketBail, twice in that run. An empty spot (no
 * vehicle within 4 m: one may appear) is an obstacle to a tank's plan. */
#define VSPAWN_MAX 32
static struct { float x, z, r; } g_vspawn[VSPAWN_MAX];
static int g_nvspawn;
static uint32_t g_vseen[128]; static int g_nvseen;
static int veh_pad_on(void) { static int on = -1; if (on < 0) on = c7_on("vehpad", "RECOMP_BOT_VEH_PAD", "VehPad", 1); return on; }
static void vspawn_learn(const Ma2World *w)
{
    for (int k = 0; k < w->nvehicles; k++) {
        const Ma2VehicleView *v = &w->vehicles[k];
        int seen = 0;
        for (int i = 0; i < g_nvseen && i < 128; i++) if (g_vseen[i] == v->actor) { seen = 1; break; }
        if (seen) continue;
        g_vseen[g_nvseen++ % 128] = v->actor;
        if (v->occupied || v->kind == MA2_KIND_TURRET) continue;
        int dup = 0;
        for (int i = 0; i < g_nvspawn; i++) if (hypotf(g_vspawn[i].x - v->pos.x, g_vspawn[i].z - v->pos.z) < 6.f) { dup = 1; break; }
        if (dup || g_nvspawn >= VSPAWN_MAX) continue;
        float r = v->ext_max > 5.f ? v->ext_max : v->kind == MA2_KIND_VTOL ? 9.f : 5.f; if (r > 10.f) r = 10.f;   /* a VTOL's wings: 9.3 m measured */
        g_vspawn[g_nvspawn].x = v->pos.x; g_vspawn[g_nvspawn].z = v->pos.z; g_vspawn[g_nvspawn].r = r; g_nvspawn++;
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VEHPAD] spawn spot %d at %.0f,%.0f r=%.1f (kind %d)\n", g_nvspawn - 1, v->pos.x, v->pos.z, r, v->kind);
    }
}
int g_c7_pad_own, g_c7_pad_skip_enemy, g_c7_pad_skip_bad, g_c7_pad_giveups, g_c7_pad_none;
static void ef_free(void);
void ma2_bot_air_reset(void)   /* a new map */
{
    ef_free();                                       /* VtolEdgeAll: the next map's edge field */
    g_nairblk = 0; g_airblk_next = 0; g_npads = 0;
    g_nvspawn = 0; g_nvseen = 0;
    memset(g_team_home, 0, sizeof g_team_home);
    g_c7_pad_own = g_c7_pad_skip_enemy = g_c7_pad_skip_bad = g_c7_pad_giveups = g_c7_pad_none = 0;
}
/* The base helipads: a building that carries an MACollectionArea (the landing zone where a VTOL
 * collects cargo, and where it is repaired: hover low, the prompt, A). Not where the VTOLs stand at
 * the start -- New River City's repair pad is 80 m from its VTOL. Found by the entity scan. */
void ma2_bot_pad_add(Ma2Vec3 p)
{
    for (int k = 0; k < g_npads; k++) if (hdist2(g_pads[k], p) < 15.f) return;
    if (g_npads >= 16) return;
    g_pads[g_npads++] = p;
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-PAD] helipad %d at %.0f,%.0f,%.0f\n", g_npads - 1, p.x, p.y, p.z);
}
static void pads_note(const Ma2World *w) { (void)w; }
int ma2_bot_pad_near(Ma2Vec3 from, Ma2Vec3 *out)
{
    int best = -1; float bd = 1e9f;
    for (int k = 0; k < g_npads; k++) { float d = hdist2(g_pads[k], from); if (d < bd) { bd = d; best = k; } }
    if (best < 0) return 0;
    if (out) *out = g_pads[best];
    return 1;
}
/* F8/F9 (bots batch 1): a damaged VTOL heals only at its own team's pads (FFA: one with no enemy
 * near it), nearest to it; a pad whose repair did not work out (45 s give-up) is skipped by that
 * bot for 120 s. Switch: env RECOMP_BOT_VTOL_PADS=0/1 over pc_settings.ini [Bots] VtolPads (on). */
int ma2_pc_ini_get_int(const char *section, const char *key, int fallback);
int ma2_bot_vtol_pads_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BOT_VTOL_PADS"); on = e && *e ? *e != '0' : ma2_pc_ini_get_int("Bots", "VtolPads", 1) != 0; }
    return on;
}
/* Where each team's bots started: the sum of their homes (team modes: a pad belongs to the team
 * whose start it lies nearest). */
void ma2_bot_pad_home(int nation, Ma2Vec3 p)
{
    if (nation < 0 || nation >= 16) return;
    g_team_home[nation].x += p.x; g_team_home[nation].z += p.z; g_team_home[nation].n++;
}
static float team_home_d(int nation, Ma2Vec3 p)
{
    if (nation < 0 || nation >= 16 || !g_team_home[nation].n) return 1e9f;
    float n = (float)g_team_home[nation].n;
    return hypotf(p.x - g_team_home[nation].x / n, p.z - g_team_home[nation].z / n);
}
int ma2_bot_pad_pick(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 home, unsigned skip_mask, Ma2Vec3 *out, int *idx)
{
    int best = -1, nenemy = 0; float bd = 1e9f;
    for (int k = 0; k < g_npads; k++) {
        Ma2Vec3 p = g_pads[k];
        if (skip_mask & (1u << k)) { g_c7_pad_skip_bad++; continue; }
        if (w->team_mode) {
            float mine = team_home_d(me->nation, p), theirs = 1e9f;
            for (int t = 0; t < 16; t++) if (t != me->nation) theirs = fminf(theirs, team_home_d(t, p));
            /* no enemy start known: only a pad close to our own start */
            if (theirs < 1e8f ? theirs <= mine : hdist2(p, home) > 300.f) { nenemy++; continue; }
        } else {
            int hot = 0;
            for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) {
                const Ma2PlayerView *o = &w->players[j];
                if (o->alive && o->actor != me->actor && hdist2(o->pos, p) < 150.f) hot = 1;
            }
            if (hot) { nenemy++; continue; }
        }
        float d = hdist2(p, me->pos);
        if (d < bd) { bd = d; best = k; }
    }
    if (nenemy) g_c7_pad_skip_enemy++;
    if (best < 0) { g_c7_pad_none++; return 0; }
    g_c7_pad_own++;
    if (out) *out = g_pads[best];
    if (idx) *idx = best;
    return 1;
}
void ma2_bot_pad_c7(double now)
{
    static double next;
    if (now < next - 120.0) next = 0.0;   /* a new match */
    if (now < next) return;
    if (next > 0.0)
        if (c7_log_on()) fprintf(stderr, "[C7] vtolpads picks=%d skip_enemy=%d skip_bad=%d giveups=%d none=%d pads=%d on=%d t=%.0f\n",
                g_c7_pad_own, g_c7_pad_skip_enemy, g_c7_pad_skip_bad, g_c7_pad_giveups, g_c7_pad_none, g_npads, ma2_bot_vtol_pads_on(), now);
    next = now + 60.0;
}
/* DeckSkip: does this body carry jump jets (fuel at actor+0x49C; jetless 'Mechs read 0)? */
/* The fullest tank seen per body: right after a burn the fuel reads ~0 and a jet 'Mech looked jetless (p23_85b: 'Mechs took 1 grab order on
 * Rush Hour in 240 s with 19 jet climbs logged). */
int ma2_bot_has_jets(uint32_t actor)
{
    static uint32_t s_a[64]; static float s_f[64]; static int s_n;
    if (!actor || !gva_ok(actor, 0x4A0)) return 0;
    float f = MEMF(actor + 0x49C); int i;
    for (i = 0; i < s_n; i++) if (s_a[i] == actor) break;
    if (i == s_n) { if (s_n < 64) s_n++; else i = (int)(actor >> 4) & 63; s_a[i] = actor; s_f[i] = 0.f; }
    if (f > s_f[i]) s_f[i] = f;
    return s_f[i] > 0.05f;
}
/* ChokeLeap: the bot's route leaves by a step-off or a jet leg (drop[i] != 0) whose top is within r of pos, among the next four legs. The
 * brain's choke queue (choke_flow) asks it: a body that leaps off a choke's mouth does not file in behind the bodies walking through it. */
int ma2_bot_leap_near(int index, Ma2Vec3 pos, float r)
{
    if (index < 0 || index >= (int)(sizeof g_rt.bots / sizeof g_rt.bots[0])) return 0;
    const BotCtrl *c = &g_rt.bots[index].ctrl;
    if (!c->have_path) return 0;
    /* LeapNearFix (parity pass 9, 11-11): the scan began at path.next and stopped at once while next was 0 (steering for the plan's first corner,
     * i.e. most of the time right after a plan), so ChokeLeap never saw a leap at drop[1]: X_81a1 b8 (Ill Wind Snatch It T2) held behind a
     * team-mate at its leap's choke 10.8-12.8 s with path 0/9 (FollowHold), X_81a2 b9 8.7-11.6 s, X_81a3 b5 8.4-13.0 s. [Bots] LeapNearFix. */
    int i0 = c->path.next > 0 || !c7_on("leapnearfix", "RECOMP_BOT_LEAP_NEAR_FIX", "LeapNearFix", 1) ? c->path.next : 1;
    for (int i = i0; i > 0 && i < c->path.n && i <= c->path.next + 4; i++)
        if (c->path.drop[i] && hdist2(pos, c->path.pt[i - 1]) < r) return 1;
    return 0;
}
/* EjectTurret: an alive turret (measured and stamped by core, g_tur) within r of x,z. */
int ma2_bot_turret_near(float x, float z, float r)
{
    int n = 0;
    for (int i = 0; i < g_ntur; i++) if (g_tur[i].alive && hypotf(g_tur[i].at.x - x, g_tur[i].at.z - z) < r) n++;
    return n;
}
int ma2_bot_air_blocked(float x, float z)
{
    for (int i = 0; i < g_nairblk; i++) if (hypotf(x - g_airblk[i].x, z - g_airblk[i].z) < g_airblk[i].r) return 1;
    return 0;
}
static void airblk_add(float x, float z, float r)
{
    for (int i = 0; i < g_nairblk; i++)
        if (hypotf(x - g_airblk[i].x, z - g_airblk[i].z) < g_airblk[i].r * 0.6f) { g_airblk[i].r = minf2(g_airblk[i].r + 8.f, 60.f); return; }
    int i = g_nairblk < AIRBLK_MAX ? g_nairblk++ : (g_airblk_next++ % AIRBLK_MAX);
    g_airblk[i].x = x; g_airblk[i].z = z; g_airblk[i].r = r;
}
static int ctrl_obstacles(const Ma2World *w, const Ma2PlayerView *me, uint32_t skip, Obst *o, int max)
{
    int n = 0;
    for (int i = 0; i < w->nvehicles && n < max; i++) {                      /* parked (occupied ones come from players) */
        const Ma2VehicleView *v = &w->vehicles[i];
        if (v->occupied || v->actor == skip || v->actor == me->actor || v->kind == MA2_KIND_PILOT) continue;
        if (hdist2(v->pos, me->pos) > 250.f) continue;
        o[n].x = v->pos.x; o[n].z = v->pos.z; o[n].r = maxf2(kind_body_r(v->kind), v->ext_max + 0.3f); o[n].vx = o[n].vz = 0.f; o[n].slow = 1;
        o[n].team = v->nation; o[n].actor = v->actor; o[n].kind = v->kind; n++;
    }
    for (int i = 0; i < w->nplayers && n < max; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q == me || !q->alive || !q->actor || q->actor == me->actor || q->actor == skip) continue;
        if (hdist2(q->pos, me->pos) > 250.f) continue;
        if (q->kind == MA2_KIND_VTOL || q->kind == MA2_KIND_FLYER) {             /* only when on the ground */
            int ok = 0; float g = terrain_height(q->pos.x, q->pos.z, &ok);
            if (!ok || q->pos.y - g > 3.f) continue;
        }
        float sp = hypotf(q->vel.x, q->vel.z);
        o[n].x = q->pos.x; o[n].z = q->pos.z; o[n].r = body_r_of(q->actor, q->kind); o[n].vx = q->vel.x; o[n].vz = q->vel.z;
        if (q->kind == MA2_KIND_VTOL) o[n].r = maxf2(o[n].r, 6.0f);   /* landed, crewed: its wings reach 6 m (the parked outline turns with it) */
        o[n].slow = sp < 2.f; o[n].team = q->nation; o[n].actor = q->actor; o[n].kind = q->kind; n++;
    }
    return n;
}
static float seg_point_dist(float ax, float az, float bx, float bz, float px, float pz, float *tproj)
{
    float vx = bx - ax, vz = bz - az, l2 = vx * vx + vz * vz, t = 0.f;
    if (l2 > 1e-4f) { t = ((px - ax) * vx + (pz - az) * vz) / l2; t = t < 0.f ? 0.f : t > 1.f ? 1.f : t; }
    if (tproj) *tproj = t;
    float qx = ax + vx * t - px, qz = az + vz * t - pz;
    return sqrtf(qx * qx + qz * qz);
}

static float hull_core_r(int k);
static int hull_tall(int kind);
static void hull_star(const Ma2VehicleView *v, int tall, float *out);
/* W2 (start hitching): long A* plans cost ~40 ms each on the game thread and all 11 bots re-planned in the same
 * step (a 450 ms frame, three times in the first 4 s). PlanBudget (default on; [Bots] PlanBudget=0 /
 * RECOMP_BOT_PLAN_BUDGET=0 restores): once the plans of this game step have used RECOMP_BOT_PLAN_MS (default 12)
 * the remaining replans wait for the next step. A waiting bot keeps its old route (or heads for its goal). */

/* W7 PathSlice: see pathslice_on (core). A bot whose plan is queued keeps its old route (or heads for its goal) until the path arrives. */
static void ctrl_plan_done(int index, Bot *b, const Ma2World *w, int ok, Ma2Vec3 goal, Ma2Vec3 from, int nd);
static int plan_slot_ok(const Ma2World *w)
{
    static int on = -1; static double step_t = -1.0, spent_at_start;
    if (on < 0) on = c7_on("planbudget", "RECOMP_BOT_PLAN_BUDGET", "PlanBudget", 1);
    if (!on || pathslice_on()) return 1;
    if (w->time != step_t) { step_t = w->time; spent_at_start = g_perf_plan_ms; }
    if (g_perf_plan_ms - spent_at_start >= (double)env_int("RECOMP_BOT_PLAN_MS", 12)) { c7_add("planbudget", "deferred", 1); return 0; }
    return 1;
}
/* VtolLaneRoute (default on, RECOMP_BOT_VTOL_LANE_ROUTE / [Bots] VtolLaneRoute): a VTOL's long leg is planned through the air
 * on a 20 m height grid (the walk grid's surface, highest per cell), not flown straight over whatever stands between. Playtest
 * 11-01, Deliverance: the VTOL flew high over the middle hill instead of following the main lanes; find a
 * middle ground. Measured (playtest log 2026-11-01): the map is a valley that bends round a 85-100 m
 * hill; the straight leg from the north base to the west flag crossed its corner at 104-107 m (the VTOL's height cap) over
 * a 10 m valley floor, and the west VTOL climbed to 105 m at 590,1112 for the east flag and hung there 17 s with a rider
 * (ground_tall 25 / plans_direct 60 a match). The cost of a cell is its length (x0.9 on a lane, x0.75 on this flight's
 * chosen lane) times 1 + (its height over the start/goal height) / K, plus 2 per metre climbed: a wide high crossing costs
 * what a 1.5x detour along the valley costs, a narrow ridge or a goal up on a plateau costs next to nothing (Bogged Down's
 * flag run still goes straight up the plateau edge). The cell route is then pulled tight wherever the straight cut stands
 * no more than `slack` over the cells it replaces: corners cut, the valley followed loosely. Learned shut airspace, known
 * gun zones and a learned height cap are never planned through. A flight whose route needs a climb past the cap or 90 m
 * up still takes the ground route (as VtolTerrainClimb). [C7] vtlane: plans, straight, bends, ground_ceiling, ground_tall,
 * nogrid, fail, plan_ms.
 * VtolRouteVary (default on, RECOMP_BOT_VTOL_ROUTE_VARY / [Bots] VtolRouteVary): playtest 11-01, the VTOLs took the same route
 * every time; vary it. Each flight (a goal 80 m+ from the last one, or 90 s on)
 * rolls its own K (45-90; a rider caps it at 50), which lane it favours (or none), a smooth +-35% cost field over the map
 * (160 m lattice: which side of a hill, which part of a valley), the corner-cut slack (6-18 m, 6-10 with a rider) and a
 * cruise band (0-12 m over the usual clearance, 0-4 with a rider). Seeded per match from the clock, per bot and per
 * flight, so two VTOLs and two runs differ; the roll holds for the whole flight (replans keep it: no jitter). Off: K 60,
 * lanes x0.85, no field, slack 10, no band. [C7] vtvary: flights, lane_none, lane_0..2, band_hi (band over 6 m), k_lo (K under 60). */
static int vt_lane_route_on(void) { return c7_on("vtlane", "RECOMP_BOT_VTOL_LANE_ROUTE", "VtolLaneRoute", 1); }
static int vt_route_vary_on(void) { return c7_on("vtvary", "RECOMP_BOT_VTOL_ROUTE_VARY", "VtolRouteVary", 1); }
#define AG_CELL 20.f
static struct {
    int w, h, k, nl; const float *src; double t;
    float *hg; unsigned char *ln;            /* height per cell (-1e9 = off the map); lane bits (lane j within 40 m) */
    unsigned char *bd; float *ed; int have_tm; /* VtolEdgeKeep: 2 = the cell has an out-of-bounds sample, 1 = border band only; metres to the nearest out-of-bounds cell */
    float *g; int *par, *cells; unsigned *st, *cl, gen;
    float *hf; int *hi; int nh, cap;         /* open list: a binary heap of (f, cell) */
} g_ag;
static uint32_t g_vr_seed; static double g_vr_seed_t;
static float g_ag_sout = 40.f;                                               /* VtolEdgeKeep: border cells this near the start may be crossed (the way out) */
static void ag_free(void)
{
    free(g_ag.hg); free(g_ag.ln); free(g_ag.bd); free(g_ag.ed); free(g_ag.g); free(g_ag.par); free(g_ag.cells); free(g_ag.st); free(g_ag.cl); free(g_ag.hf); free(g_ag.hi);
    memset(&g_ag, 0, sizeof g_ag);
}
/* VtolEdgeKeep (default on, RECOMP_BOT_VTOL_EDGE_KEEP / [Bots] VtolEdgeKeep): playtest 11-01, the VTOLs fly very high and end
 * up just short of the invisible wall on top of the tall cliffs, which exploits the map: those tops are meant to be out
 * of bounds, on Deliverance and on any map built the same way (custom maps too). Every level says where play is: the
 * MATerrainInfo type map (key 1F1646EA, 512x512, 0 playable, 8/16/32 water, 2 border band, 1 out of bounds; the editor
 * paints it, so custom maps carry it). The invisible wall stands at the 2/1 edge and holds at any height: a VTOL sent
 * east up Deliverance's middle hill stopped at x 590-600 at 100-111 m, where the 2 band meets the 1 cells (desktop
 * 11-01). Band 2 also rims cliffs INSIDE the arena (Bogged Down's plateau edge), so only out-of-bounds (1) cells shut
 * the air route; band cells cost x1.5. A route keeps 25 m off out-of-bounds cells (shut) and pays up to x2.5 per metre
 * within 60 m of them; near its start or goal both are waived (getting out of one / a target by the edge). A goal on an
 * out-of-bounds cell is flown to the nearest cell 20 m+ inside. The fight's hide and slide spots keep 25 m off too.
 * [C7] vtedge: flights, flights_lt20, flights_lt40 (closest approach per flight), near20_s (seconds within 20 m),
 * over_border_s / over_oob_s (seconds over a 2 / 1 cell), spot_moved, nomap. Trace: [BOT-VEDGE] per flight. */
static int vt_edge_keep_on(void) { return c7_on("vtedge", "RECOMP_BOT_VTOL_EDGE_KEEP", "VtolEdgeKeep", 1); }
/* The type map value at (x, z), read the way the game reads it (sub_F7AC7: layer [TI+0x84] (index 0), picture [layer+0xC];
 * sub_189E62: cell = picture size / (terrain samples x cell size) x position; sub_2674A0: base +0x4C, x * bytes per pixel
 * +0x24, z * pitch +0x50, 16-bit when flags +0xC & 7). TI = the active MATerrainInfo [0x50C8E0]. -1 = no map. */
static int ti_type_at(float x, float z)
{
    uint32_t ti = MEM32(0x50C8E0u), t = MEM32(VA_TERRAIN);
    if (!gva_ok(ti, 0x90) || !gva_ok(t, 0x840)) return -1;
    uint32_t lay = MEM32(ti + 0x84);
    if (!gva_ok(lay, 0x10)) return -1;
    uint32_t pic = MEM32(lay + 0xC);
    if (!gva_ok(pic, 0x60) || !MEM32(pic + 0x38)) return -1;
    int pw = (int)MEM32(pic + 0x58), ph = (int)MEM32(pic + 0x5C), tw = (int)MEM32(t + 4), td = (int)MEM32(t + 8);
    float cell = MEMF(t + 0x814);
    if (pw <= 0 || ph <= 0 || tw <= 0 || td <= 0 || !(cell > 0.f)) return -1;
    int ix = (int)((float)pw / ((float)tw * cell) * x), iz = (int)((float)ph / ((float)td * cell) * z);
    if (ix < 0 || iz < 0 || ix >= pw || iz >= ph) return -1;
    uint32_t a = MEM32(pic + 0x4C) + (uint32_t)ix * MEM32(pic + 0x24) + (uint32_t)iz * MEM32(pic + 0x50);
    if (!gva_ok(a, 4)) return -1;
    return (MEM8(pic + 0xC) & 7) ? (int)MEM16(a) : (int)MEM32(a);
}
/* VtolEdgeAll (default on, RECOMP_BOT_VTOL_EDGE_ALL / [Bots] VtolEdgeAll): playtest 11-01, the VTOLs were crashing into the invisible
 * wall at the top of the cliff, exploiting the map; custom maps and
 * other maps with similar features must not be exploited the same way. VtolEdgeKeep kept the long planned legs off the
 * out-of-bounds cells; a leg under 150 m still flew straight, and the flag-run cover orbit (55-95 m round their flag, any bearing),
 * muster, pick-up and drop hovers were flown at whatever point the brain gave. Now EVERY steer target and leg of a VTOL keeps off
 * the map's own out-of-bounds cells (type map bit 1: the editor paints it, so custom maps too) -- the more the higher it flies:
 * 25 m, plus 1 m per metre over 20 m above the ground (to 55 m), so it never hangs high over a cliff top beside the wall. A goal,
 * hover or carrot inside that band moves inward along the edge normal (the slope of a distance field at the type map's own
 * resolution, ~4 m); every route (the air A*, a straight short leg, a ground route) has its corners kept in and gets a bend where
 * a leg dips into the band (not the way out of the band it starts in); tether work (hook / drop, 5-12 m up) keeps 10 m; the missile
 * dodge turns the other way off the edge; a craft that drifts in anyway (carried on past its point, a firing hover where it
 * stopped: under 24 m) is flown back out to 32 m, and one closing on the edge fast is braked on the stick. Measured (5v6 CTF, the player ferried, 240 s, off -> on): Deliverance closest to
 * out of bounds 0.0 -> 34.3 m, 4.0 -> 0 s within 25 m, peak 61 -> 43 m up; Bogged Down 0.0 -> 29.6 m, 8.5 -> 0 s, 6.7 -> 0 s
 * high near it, and its ferry still flew the plateau edge straight (767 m closed / 774 flown, ~42 m/s with the player on).
 * Its edge distances also fix VtolEdgeKeep's grid (each chamfer pass read the neighbours it had not done yet: distances stopped
 * two cells out). Off = VtolEdgeKeep alone, as before. [C7] vtedgeall: goal_in_s, dest_in_s, hover_in_s, pts_in,
 * bends, keep_fail, dodge_flip, recover_s; measured with the switch on or off (in25_<steering> says which steering it was under): air_s, in25_s, in15_s, hi_near_s (within 40 m at over 30 m
 * up), worst_in_m (deepest into the 25 m band). Trace [BOT-VEDGE2] (RECOMP_BOT_VTOL_TRACE). */
static int vt_edge_all_on(void) { return c7_on("vtedgeall", "RECOMP_BOT_VTOL_EDGE_ALL", "VtolEdgeAll", 1); }
static struct { float *d; int w, h; float mx, mz; uint32_t pic, base; int tried; } g_ef;   /* metres to the nearest out-of-bounds pixel */
static void ef_free(void) { free(g_ef.d); memset(&g_ef, 0, sizeof g_ef); }
static int ef_ready(void)
{
    uint32_t ti = MEM32(0x50C8E0u), t = MEM32(VA_TERRAIN);
    if (!gva_ok(ti, 0x90) || !gva_ok(t, 0x840)) return 0;
    uint32_t lay = MEM32(ti + 0x84);
    if (!gva_ok(lay, 0x10)) return 0;
    uint32_t pic = MEM32(lay + 0xC);
    if (!gva_ok(pic, 0x60) || !MEM32(pic + 0x38)) return 0;
    uint32_t base = MEM32(pic + 0x4C);
    if (g_ef.pic == pic && g_ef.base == base && (g_ef.d || g_ef.tried)) return g_ef.d != NULL;
    ef_free(); g_ef.pic = pic; g_ef.base = base; g_ef.tried = 1;
    int pw = (int)MEM32(pic + 0x58), ph = (int)MEM32(pic + 0x5C), tw = (int)MEM32(t + 4), td = (int)MEM32(t + 8);
    float cell = MEMF(t + 0x814);
    uint32_t bpp = MEM32(pic + 0x24), pitch = MEM32(pic + 0x50); int w16 = (MEM8(pic + 0xC) & 7) != 0;
    if (pw <= 0 || ph <= 0 || pw > 4096 || ph > 4096 || tw <= 0 || td <= 0 || !(cell > 0.f) || bpp > 8 || !gva_ok(base, pitch * (uint32_t)(ph - 1) + bpp * (uint32_t)pw)) return 0;
    float *d = (float *)malloc((size_t)pw * ph * sizeof(float));
    if (!d) return 0;
    double t0 = c7_t0(); int nb = 0;
    for (int iz = 0; iz < ph; iz++)
        for (int ix = 0; ix < pw; ix++) {
            uint32_t a = base + (uint32_t)ix * bpp + (uint32_t)iz * pitch;
            int v = w16 ? (int)MEM16(a) : (int)MEM32(a), bad = v > 0 && (v & 1);
            d[iz * pw + ix] = bad ? 0.f : 1e9f; nb += bad;
        }
    const float mx = (float)tw * cell / (float)pw, mz = (float)td * cell / (float)ph, md = hypotf(mx, mz);
    for (int pass = 0; pass < 2; pass++)                                    /* chamfer: forward, then backward */
        for (int q = 0; q < pw * ph; q++) {
            int i = pass ? pw * ph - 1 - q : q, x = i % pw, z = i / pw, s = pass ? -1 : 1;   /* forward: from left / above; backward: right / below */
            float e = d[i];
            if (x - s >= 0 && x - s < pw) e = minf2(e, d[i - s] + mx);
            if (z - s >= 0 && z - s < ph) e = minf2(e, d[i - s * pw] + mz);
            if (x - s >= 0 && x - s < pw && z - s >= 0 && z - s < ph) e = minf2(e, d[i - s - s * pw] + md);
            if (x + s >= 0 && x + s < pw && z - s >= 0 && z - s < ph) e = minf2(e, d[i + s - s * pw] + md);
            d[i] = e;
        }
    g_ef.d = d; g_ef.w = pw; g_ef.h = ph; g_ef.mx = mx; g_ef.mz = mz;
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VEDGE2] edge field %dx%d (%.1f x %.1f m), %d out-of-bounds pixels, %.1f ms\n", pw, ph, mx, mz, nb, (c7_t0() - t0));
    return 1;
}
static float ef_px(int ix, int iz) { return ix < 0 || iz < 0 || ix >= g_ef.w || iz >= g_ef.h ? 0.f : g_ef.d[iz * g_ef.w + ix]; }
static float ef_dist(float x, float z)                                       /* metres to out of bounds, bilinear (off the map = 0) */
{
    if (!g_ef.d) return 1e9f;
    float fx = x / g_ef.mx - 0.5f, fz = z / g_ef.mz - 0.5f;
    int ix = (int)floorf(fx), iz = (int)floorf(fz); float tx = fx - (float)ix, tz = fz - (float)iz;
    float a = ef_px(ix, iz), b = ef_px(ix + 1, iz), c = ef_px(ix, iz + 1), e = ef_px(ix + 1, iz + 1);
    if (a > 1e8f || b > 1e8f || c > 1e8f || e > 1e8f) return maxf2(maxf2(a, b), maxf2(c, e)) > 1e8f && minf2(minf2(a, b), minf2(c, e)) > 1e8f ? 1e9f : minf2(minf2(a, b), minf2(c, e));
    return (a * (1.f - tx) + b * tx) * (1.f - tz) + (c * (1.f - tx) + e * tx) * tz;
}
static float g_vkeep_cap;                                                   /* set round a cargo run-in: ctrl_travel keeps no wider */
static float vt_keep_m(float agl) { return 25.f + minf2(maxf2(agl - 20.f, 0.f), 30.f); }   /* the keep-off for a craft this high over the ground */
/* Move p (x, z) out of the keep-off band: up the distance field's slope (the edge normal), else the nearest ring point clear of
 * it. 1 = moved, 0 = already clear (or no map), -1 = no clear point within 200 m (left as it was). */
static int vt_keep_in(Ma2Vec3 *p, float keep)
{
    if (!ef_ready()) return 0;
    float d = ef_dist(p->x, p->z);
    if (d >= keep) return 0;
    Ma2Vec3 q = *p;
    for (int it = 0; it < 8 && d < keep; it++) {
        float s = maxf2(g_ef.mx, 2.f), gx = ef_dist(q.x + s, q.z) - ef_dist(q.x - s, q.z), gz = ef_dist(q.x, q.z + s) - ef_dist(q.x, q.z - s), gl = hypotf(gx, gz);
        if (gl < 1e-3f || gx != gx || gz != gz) break;                       /* flat: deep in the out-of-bounds area */
        float st = keep - d + 2.f;
        q.x += gx / gl * st; q.z += gz / gl * st;
        d = ef_dist(q.x, q.z);
    }
    if (d < keep)
        for (float r = 10.f; r <= 200.f && d < keep; r += 10.f)
            for (int k = 0; k < 24 && d < keep; k++) {
                float a = (float)k * 0.2618f; Ma2Vec3 c2 = *p; c2.x += sinf(a) * r; c2.z += cosf(a) * r;
                float e = ef_dist(c2.x, c2.z);
                if (e >= keep && e < 1e8f) { q = c2; d = e; }
            }
    if (d < keep) {
        static int nlog; c7_add("vtedgeall", "keep_fail", 1);
        if (env_on("RECOMP_BOT_DIAG") && nlog++ < 8) fprintf(stderr, "[BOT-VEDGE2] no point %.0f m clear of the edge within 200 m of %.0f,%.0f (edge %.1f)\n", keep, p->x, p->z, ef_dist(p->x, p->z));
        return -1;
    }
    p->x = q.x; p->z = q.z;
    return 1;
}
static float vt_edge_dist(float x, float z)                                  /* metres to the nearest border cell (grid), 1e9 = none / no map */
{
    if (vt_edge_all_on() && ef_ready()) return ef_dist(x, z);                /* VtolEdgeAll: the fine field */
    if (!g_ag.ed || !g_ag.have_tm) return 1e9f;
    int cx = (int)(x / AG_CELL), cz = (int)(z / AG_CELL);
    if (cx < 0 || cz < 0 || cx >= g_ag.w || cz >= g_ag.h) return 0.f;
    return g_ag.ed[cz * g_ag.w + cx];
}
static int ag_build(const Ma2World *w)
{
    Ma2TravView v;
    if (!ma2_trav_view(&v) || !v.sf || v.cell <= 0.f) return 0;
    int k = (int)(AG_CELL / v.cell + 0.5f); if (k < 1) k = 1;
    int gw = (v.w + k - 1) / k, gh = (v.h + k - 1) / k;
    if (w->time < g_ag.t) g_ag.src = NULL;                                   /* a new match */
    g_ag.t = w->time;
    if (g_ag.src != v.sf || g_ag.w != gw || g_ag.h != gh) {
        ag_free();
        size_t n = (size_t)gw * gh;
        g_ag.hg = (float *)malloc(n * sizeof(float)); g_ag.ln = (unsigned char *)calloc(n, 1);
        g_ag.g = (float *)malloc(n * sizeof(float)); g_ag.par = (int *)malloc(n * sizeof(int)); g_ag.cells = (int *)malloc(n * sizeof(int));
        g_ag.st = (unsigned *)calloc(n, sizeof(unsigned)); g_ag.cl = (unsigned *)calloc(n, sizeof(unsigned));
        g_ag.cap = (int)n * 8; g_ag.hf = (float *)malloc((size_t)g_ag.cap * sizeof(float)); g_ag.hi = (int *)malloc((size_t)g_ag.cap * sizeof(int));
        if (!g_ag.hg || !g_ag.ln || !g_ag.g || !g_ag.par || !g_ag.cells || !g_ag.st || !g_ag.cl || !g_ag.hf || !g_ag.hi) { ag_free(); return 0; }
        for (int cz = 0; cz < gh; cz++)
            for (int cx = 0; cx < gw; cx++) {
                float m = -1e9f;
                for (int z = cz * k; z < cz * k + k && z < v.h; z++)
                    for (int x = cx * k; x < cx * k + k && x < v.w; x++) {
                        int i = z * v.w + x;
                        if (v.sf[i] > -1e8f && v.sf[i] > m) m = v.sf[i];
                        if (v.th && v.th[i] > -1e8f && v.th[i] > m) m = v.th[i];
                    }
                g_ag.hg[cz * gw + cx] = m;
            }
        /* VtolEdgeKeep: border / out-of-bounds cells from the type map (a sample every 4 m), then a chamfer distance */
        g_ag.bd = (unsigned char *)calloc(n, 1); g_ag.ed = (float *)malloc(n * sizeof(float));
        if (!g_ag.bd || !g_ag.ed) { ag_free(); return 0; }
        g_ag.have_tm = ti_type_at(AG_CELL * gw * 0.5f, AG_CELL * gh * 0.5f) >= 0;
        int nbad = 0;
        for (int cz = 0; cz < gh; cz++)
            for (int cx = 0; cx < gw; cx++) {
                int bad = 0;
                for (int sz = 0; sz < 5 && bad < 2 && g_ag.have_tm; sz++)
                    for (int sx = 0; sx < 5 && bad < 2; sx++) {
                        int tv = ti_type_at(cx * AG_CELL + 2.f + 4.f * sx, cz * AG_CELL + 2.f + 4.f * sz);
                        if (tv > 0 && (tv & 1)) bad = 2; else if (tv > 0 && (tv & 2)) bad = 1;
                    }
                g_ag.bd[cz * gw + cx] = (unsigned char)bad; nbad += bad == 2;
                g_ag.ed[cz * gw + cx] = bad == 2 ? 0.f : 1e9f;
            }
        for (int pass = 0; pass < 2; pass++)
            for (int q = 0; q < (int)n; q++) {
                int i = pass ? (int)n - 1 - q : q, x = i % gw, z = i / gw, s = (pass ? -1 : 1) * (vt_edge_all_on() ? 1 : -1);   /* VtolEdgeAll fix: each pass reads the neighbours it has already done (it read the others: distances stopped 2 cells out, the 60 m cost ramp at 40 m) */
                float e = g_ag.ed[i];
                if (x - s >= 0 && x - s < gw) e = minf2(e, g_ag.ed[i - s] + AG_CELL);
                if (z - s >= 0 && z - s < gh) e = minf2(e, g_ag.ed[i - s * gw] + AG_CELL);
                if (x - s >= 0 && x - s < gw && z - s >= 0 && z - s < gh) e = minf2(e, g_ag.ed[i - s - s * gw] + AG_CELL * 1.41421f);
                if (x + s >= 0 && x + s < gw && z - s >= 0 && z - s < gh) e = minf2(e, g_ag.ed[i + s - s * gw] + AG_CELL * 1.41421f);
                g_ag.ed[i] = e;
            }
        if (!g_ag.have_tm) c7_add("vtedge", "nomap", 1);
        if (env_on("RECOMP_BOT_DIAG")) {
            fprintf(stderr, "[BOT-VEDGE] air grid %dx%d (%.0f m), type map %s, %d out-of-bounds cells\n", gw, gh, AG_CELL, g_ag.have_tm ? "read" : "MISSING", nbad);
            if (env_on("RECOMP_BOT_VTOL_EDGE_DUMP"))   /* the map as the planner sees it: '#' out of bounds, '+' border, '.' play; every 2nd cell */
                for (int cz = gh - 1; cz >= 0; cz -= 2) {
                    char row[512]; int o = 0;
                    for (int cx = 0; cx < gw && o < 510; cx += 2) row[o++] = g_ag.bd[cz * gw + cx] == 2 ? '#' : g_ag.bd[cz * gw + cx] ? '+' : '.';
                    row[o] = 0; fprintf(stderr, "[BOT-VEDGE] z=%4.0f %s\n", cz * AG_CELL, row);
                }
        }
        g_ag.src = v.sf; g_ag.w = gw; g_ag.h = gh; g_ag.k = k; g_ag.nl = -1;
    }
    if (g_ag.nl != w->nlanes) {                                              /* the lanes appear a second or two into a match */
        memset(g_ag.ln, 0, (size_t)gw * gh);
        for (int j = 0; j < w->nlanes && j < 3; j++) {
            const Ma2Lane *L = &w->lanes[j];
            for (int s = 0; s + 1 < L->n; s++) {
                float x0 = minf2(L->pt[s].x, L->pt[s + 1].x) - 40.f, x1 = maxf2(L->pt[s].x, L->pt[s + 1].x) + 40.f;
                float z0 = minf2(L->pt[s].z, L->pt[s + 1].z) - 40.f, z1 = maxf2(L->pt[s].z, L->pt[s + 1].z) + 40.f;
                for (int cz = (int)maxf2(z0 / AG_CELL, 0.f); cz < gh && cz * AG_CELL <= z1; cz++)
                    for (int cx = (int)maxf2(x0 / AG_CELL, 0.f); cx < gw && cx * AG_CELL <= x1; cx++)
                        if (seg_point_dist(L->pt[s].x, L->pt[s].z, L->pt[s + 1].x, L->pt[s + 1].z, (cx + 0.5f) * AG_CELL, (cz + 0.5f) * AG_CELL, NULL) < 40.f)
                            g_ag.ln[cz * gw + cx] |= (unsigned char)(1u << j);
            }
        }
        g_ag.nl = w->nlanes;
    }
    return 1;
}
static uint32_t vr_next(uint32_t *x) { uint32_t v = *x ? *x : 0x6D2B79F5u; v ^= v << 13; v ^= v >> 17; v ^= v << 5; *x = v; return v; }
static float vr_unit(uint32_t *x) { return (float)(vr_next(x) >> 8) * (1.f / 16777216.f); }
static float vr_lattice(uint32_t seed, int ix, int iz)
{
    uint32_t h = seed ^ ((uint32_t)ix * 0x8DA6B343u) ^ ((uint32_t)iz * 0xD8163841u);
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return (float)(h >> 8) * (1.f / 16777216.f);
}
static float vr_field(const BotCtrl *c, float x, float z)                   /* this flight's smooth cost field, 1 +- amp */
{
    if (c->vr_amp <= 0.f) return 1.f;
    float fx = x / 160.f, fz = z / 160.f; int ix = (int)floorf(fx), iz = (int)floorf(fz); float tx = fx - ix, tz = fz - iz;
    float a = vr_lattice(c->vr_seed, ix, iz), b = vr_lattice(c->vr_seed, ix + 1, iz), d = vr_lattice(c->vr_seed, ix, iz + 1), e = vr_lattice(c->vr_seed, ix + 1, iz + 1);
    float n = (a * (1.f - tx) + b * tx) * (1.f - tz) + (d * (1.f - tx) + e * tx) * tz;
    return 1.f + c->vr_amp * (2.f * n - 1.f);
}
/* This flight's roll (VtolRouteVary). */
static void vr_roll(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal)
{
    if (c->vr_actor == me->actor && c->vr_n > 0 && hdist2(goal, c->vr_goal) < 80.f && w->time >= c->vr_t && w->time - c->vr_t < 90.0) return;
    if (!g_vr_seed || w->time < g_vr_seed_t) { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_vr_seed = (uint32_t)q.QuadPart ^ (uint32_t)(q.QuadPart >> 32) ^ 0xA511E9B3u; }
    g_vr_seed_t = w->time;
    if (c->ve_n > 0 && c->vr_actor == me->actor) {                            /* VtolEdgeKeep: the flight that ends here */
        c7_add("vtedge", "flights", 1);
        if (c->ve_min < 20.f) c7_add("vtedge", "flights_lt20", 1);
        if (c->ve_min < 40.f) c7_add("vtedge", "flights_lt40", 1);
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VEDGE] t=%.1f bot=%d flight=%d to %.0f,%.0f: closest to out of bounds %.0f m at %.0f,%.0f,%.0f (type %d under it), peak y %.0f\n",
                                               w->time, index, c->vr_n, c->vr_goal.x, c->vr_goal.z, c->ve_min, c->ve_at.x, c->ve_at.y, c->ve_at.z, c->ve_type, c->ve_ymax);
    }
    c->ve_n = 0; c->ve_min = 1e9f; c->ve_ymax = -1e9f; c->ve_type = -1;
    c->vr_actor = me->actor; c->vr_goal = goal; c->vr_t = w->time; c->vr_n++;
    for (int j = 0; j < 3; j++) c->vr_lane_f[j] = 0.85f;
    c->vr_k = 60.f; c->vr_amp = 0.f; c->vr_slack = 10.f; c->vr_band = 0.f; c->vr_lane = -1;
    if (!vt_route_vary_on()) return;
    uint32_t x = g_vr_seed ^ ((uint32_t)(index + 1) * 0x9E3779B9u) ^ ((uint32_t)c->vr_n * 0x85EBCA6Bu);
    for (int i = 0; i < 4; i++) vr_next(&x);
    c->vr_seed = vr_next(&x);
    c->vr_k = 45.f + 45.f * vr_unit(&x);
    c->vr_amp = 0.35f;
    c->vr_slack = 6.f + 12.f * vr_unit(&x);
    c->vr_band = 12.f * vr_unit(&x);
    int nl = w->nlanes < 3 ? w->nlanes : 3;
    float u = vr_unit(&x);
    if (nl > 0 && u > 0.3f) c->vr_lane = (int)((u - 0.3f) / 0.7f * (float)nl) < nl ? (int)((u - 0.3f) / 0.7f * (float)nl) : nl - 1;   /* 30%: no favourite */
    for (int j = 0; j < 3; j++) c->vr_lane_f[j] = j == c->vr_lane ? 0.75f : 0.9f;
    c7_add("vtvary", "flights", 1);
    { static const char *const ln[4] = { "lane_none", "lane_0", "lane_1", "lane_2" }; c7_add("vtvary", ln[c->vr_lane + 1], 1); }
    if (c->vr_band > 6.f) c7_add("vtvary", "band_hi", 1);
    if (c->vr_k < 60.f) c7_add("vtvary", "k_lo", 1);
}
static void ag_push(float f, int i)
{
    if (g_ag.nh >= g_ag.cap) return;
    int j = g_ag.nh++;
    while (j > 0) { int p = (j - 1) / 2; if (g_ag.hf[p] <= f) break; g_ag.hf[j] = g_ag.hf[p]; g_ag.hi[j] = g_ag.hi[p]; j = p; }
    g_ag.hf[j] = f; g_ag.hi[j] = i;
}
static int ag_pop(void)
{
    int top = g_ag.hi[0]; float lf = g_ag.hf[--g_ag.nh]; int li = g_ag.hi[g_ag.nh], j = 0;
    for (;;) {
        int a = 2 * j + 1, b2 = a + 1, m = j; float mf = lf;
        if (a < g_ag.nh && g_ag.hf[a] < mf) { m = a; mf = g_ag.hf[a]; }
        if (b2 < g_ag.nh && g_ag.hf[b2] < mf) { m = b2; mf = g_ag.hf[b2]; }
        if (m == j) break;
        g_ag.hf[j] = g_ag.hf[m]; g_ag.hi[j] = g_ag.hi[m]; j = m;
    }
    if (g_ag.nh > 0) { g_ag.hf[j] = lf; g_ag.hi[j] = li; }
    return top;
}
/* Shut to this plan: off the map, learned shut airspace, a known gun zone (r x 0.8), over a learned height cap -- except the
 * zones the start or the goal sits in (getting out of one / a target inside one). */
/* StickCeil: the stick ceiling at (x, z). It follows the terrain height map under the craft (MaxVTOLHeight over the terrain, less
 * ~6.6 m): New River City (terrain ~0) 48.0-48.7 m; Bogged Down's bog (terrain -19/-20) 28.2-28.4 m, its plateau ~46 m. Flying
 * off the plateau over the bog the ceiling falls 20 m at once and the game pulls the craft down at 21-27 m/s -- a rider 9 m under it
 * met the plateau rim (playtest rec 2026-11-04 t=18.9-19.7, vu3 t=22.4: both at 916-933,~1240). 0 agl = the absolute ceiling. */
static float vt_ceil_at(const BotCtrl *c, float x, float z)
{
    if (!(c->vt_ceiling > 0.f)) return 0.f;
    if (!(c->vt_ceil_agl > 0.f)) return c->vt_ceiling;
    int ok = 0; float t = terrain_height(x, z, &ok);
    return ok ? t + c->vt_ceil_agl : c->vt_ceiling;
}
static int ag_shut(const BotCtrl *c, const Bot *b, int i, float clr, Ma2Vec3 s, Ma2Vec3 g)
{
    if (g_ag.hg[i] < -1e8f) return 1;
    float x = ((float)(i % g_ag.w) + 0.5f) * AG_CELL, z = ((float)(i / g_ag.w) + 0.5f) * AG_CELL;
    if (vt_edge_keep_on() && g_ag.have_tm) {                                /* VtolEdgeKeep */
        float ds = hypotf(x - s.x, z - s.z), dg = hypotf(x - g.x, z - g.z);
        if (g_ag.bd[i] == 2 && ds > g_ag_sout && dg > 40.f) return 1;     /* over out of bounds now / a goal on it: the way out, the way in */
        if (g_ag.ed[i] < 25.f && ds > 60.f && dg > 60.f) return 1;
    }
    if (c->vt_ceiling > 0.f) {   /* StickCeil: this cell's own ceiling, against the tallest of it and its neighbours (a cliff over a bog) */
        float hm = g_ag.hg[i]; int cx = i % g_ag.w, cz = i / g_ag.w;
        for (int oz = -1; oz <= 1; oz++) for (int ox = -1; ox <= 1; ox++) {
            int nx = cx + ox, nz = cz + oz;
            if (nx >= 0 && nz >= 0 && nx < g_ag.w && nz < g_ag.h && g_ag.hg[nz * g_ag.w + nx] > hm) hm = g_ag.hg[nz * g_ag.w + nx];
        }
        if (hm + clr > vt_ceil_at(c, x, z) + 1.f) return 1;
    }
    for (int k = 0; k < g_nairblk; k++) {
        float r = g_airblk[k].r;
        /* RiderRay: with a rider the shut air reaches most of a cell further -- a 25 m circle tested at 20 m cell centres let the
         * tightened lane route pass 10 m from the obstruction RiderRay had just found at 19 m up (vx2 11-04 bot 7, t=19.6-22.9: it
         * stalled into it at 877,1317 and lost the rider) */
        float rr = r + (c->vt_rider && c7_on("riderray", "RECOMP_BOT_VTOL_RIDER_RAY", "RiderRay", 1) ? 0.35f * AG_CELL : 0.f);
        if (hypotf(x - g_airblk[k].x, z - g_airblk[k].z) < rr && hypotf(s.x - g_airblk[k].x, s.z - g_airblk[k].z) >= r && hypotf(g.x - g_airblk[k].x, g.z - g_airblk[k].z) >= r) return 1;
    }
    for (int k = 0; k < b->order.ndanger; k++) {
        const Ma2PathDanger *d = &b->order.danger[k]; float r = d->r * 0.8f;
        if (hypotf(x - d->x, z - d->z) < r && hypotf(s.x - d->x, s.z - d->z) >= r && hypotf(g.x - d->x, g.z - d->z) >= r) return 1;
    }
    return 0;
}
/* VtolEdgeKeep: a goal over a border / out-of-bounds cell is moved to the nearest cell 20 m+ inside play (within 100 m): a
 * retreat or cover spot the brain put up on an out-of-bounds hill sent the craft up to 111 m against the wall (desktop 11-01,
 * Deliverance 593,1187: type 1 under it). 1 = moved. */
static int vt_goal_inside(Ma2Vec3 *goal)
{
    if (!vt_edge_keep_on() || !g_ag.have_tm || !g_ag.bd) return 0;
    int gx = (int)(goal->x / AG_CELL), gz = (int)(goal->z / AG_CELL), gw = g_ag.w, gh = g_ag.h;
    if (gx < 0 || gz < 0 || gx >= gw || gz >= gh || g_ag.bd[gz * gw + gx] != 2) return 0;
    int bx = -1, bz = -1; float bd2 = 1e9f;
    for (int dz = -5; dz <= 5; dz++)
        for (int dx = -5; dx <= 5; dx++) {
            int X = gx + dx, Z = gz + dz;
            if (X < 0 || Z < 0 || X >= gw || Z >= gh || g_ag.bd[Z * gw + X] == 2 || g_ag.ed[Z * gw + X] < 20.f || g_ag.hg[Z * gw + X] < -1e8f) continue;
            float dd = (float)(dx * dx + dz * dz);
            if (dd < bd2) { bd2 = dd; bx = X; bz = Z; }
        }
    if (bx < 0) return 0;
    goal->x = (bx + 0.5f) * AG_CELL; goal->z = (bz + 0.5f) * AG_CELL;
    c7_add("vtedge", "goal_pulled_in", 1);
    return 1;
}
/* 1 = the leg is set (c->path), 0 = not for this leg (short, no grid: VtolTerrainClimb decides), -1 = take the ground route. */
static int vtol_air_route(int index, BotCtrl *c, Bot *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal)
{
    float l = hdist2(goal, me->pos);
    const Ma2Vec3 goal0 = goal;                                              /* the goal asked for (a goal on a border cell is flown to one inside) */
    if (!vt_lane_route_on() || l < 150.f) return 0;
    if (!ag_build(w)) { c7_add("vtlane", "nogrid", 1); return 0; }
    double t0 = c7_t0();
    vr_roll(index, c, w, me, goal);
    const int gw = g_ag.w, gh = g_ag.h;
    int sx = (int)(me->pos.x / AG_CELL), sz = (int)(me->pos.z / AG_CELL), gx = (int)(goal.x / AG_CELL), gz = (int)(goal.z / AG_CELL);
    if (sx < 0 || sz < 0 || gx < 0 || gz < 0 || sx >= gw || sz >= gh || gx >= gw || gz >= gh) { c7_add("vtlane", "nogrid", 1); return 0; }
    if (vt_goal_inside(&goal)) { gx = (int)(goal.x / AG_CELL); gz = (int)(goal.z / AG_CELL); }
    g_ag_sout = 40.f;                                                        /* over a band: the way out is open as far as the nearest play cell (+30 m) */
    if (vt_edge_keep_on() && g_ag.have_tm && g_ag.bd[sz * gw + sx] == 2) {
        for (int r = 1; r <= 10 && g_ag_sout <= 40.f; r++)
            for (int dz = -r; dz <= r; dz++)
                for (int dx = -r; dx <= r; dx++) {
                    int X = sx + dx, Z = sz + dz;
                    if (X < 0 || Z < 0 || X >= gw || Z >= gh || g_ag.bd[Z * gw + X] == 2) continue;
                    g_ag_sout = maxf2(g_ag_sout, minf2(hypotf((float)dx, (float)dz) * AG_CELL + 30.f, 400.f));
                }
    }
    const int si = sz * gw + sx, gi = gz * gw + gx;
    const float clr = c->vt_rider ? 13.f : 7.f, K = c->vt_rider ? minf2(c->vr_k, 50.f) : c->vr_k, Wc = 2.f, slack = c->vt_rider ? minf2(c->vr_slack, 10.f) : c->vr_slack;
    float hs = g_ag.hg[si] > -1e8f ? g_ag.hg[si] : me->pos.y - clr, hgl = g_ag.hg[gi] > -1e8f ? g_ag.hg[gi] : goal.y;
    float F = maxf2(hs, minf2(me->pos.y - clr, hs + 25.f));                 /* height it has already: no climb paid up to here */
    float ref = maxf2(F, hgl);                                               /* the height the leg needs anyway */
    float fmin = 0.75f * (1.f - c->vr_amp);                                  /* the cheapest a metre gets: the heuristic stays a lower bound */
    if (++g_ag.gen == 0) { memset(g_ag.st, 0, (size_t)gw * gh * sizeof(unsigned)); memset(g_ag.cl, 0, (size_t)gw * gh * sizeof(unsigned)); g_ag.gen = 1; }
    g_ag.nh = 0;
    g_ag.g[si] = 0.f; g_ag.par[si] = -1; g_ag.st[si] = g_ag.gen;
    ag_push(0.f, si);
    int found = 0, nexp = 0;
    while (g_ag.nh > 0) {
        int i = ag_pop();
        if (g_ag.cl[i] == g_ag.gen) continue;
        g_ag.cl[i] = g_ag.gen; nexp++;
        if (i == gi) { found = 1; break; }
        int x = i % gw, z = i / gw;
        float hc = g_ag.hg[i] > -1e8f ? g_ag.hg[i] : hs;
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dz) continue;
                int X = x + dx, Z = z + dz;
                if (X < 0 || Z < 0 || X >= gw || Z >= gh) continue;
                int j = Z * gw + X;
                if (g_ag.cl[j] == g_ag.gen) continue;
                if (j != gi && ag_shut(c, b, j, clr, me->pos, goal)) continue;
                float hn = g_ag.hg[j] > -1e8f ? g_ag.hg[j] : hgl, cx = (X + 0.5f) * AG_CELL, cz = (Z + 0.5f) * AG_CELL;
                float m = 1.f;
                for (int q = 0; q < 3; q++) if ((g_ag.ln[j] >> q) & 1u) m = minf2(m, c->vr_lane_f[q]);
                m *= vr_field(c, cx, cz);
                if (vt_edge_keep_on() && g_ag.have_tm && g_ag.ed[j] < 60.f) m *= g_ag.bd[j] == 2 ? 4.f : 1.f + 1.5f * (60.f - g_ag.ed[j]) / 60.f;   /* VtolEdgeKeep */
                if (vt_edge_keep_on() && g_ag.bd[j] == 1) m *= 1.5f;           /* a border band (the outer rim, or a cliff face inside) */
                float cost = AG_CELL * (dx && dz ? 1.41421f : 1.f) * m * (1.f + maxf2(hn - ref, 0.f) / K) + Wc * maxf2(hn - maxf2(hc, F), 0.f);
                float ng = g_ag.g[i] + cost;
                if (g_ag.st[j] == g_ag.gen && ng >= g_ag.g[j]) continue;
                g_ag.st[j] = g_ag.gen; g_ag.g[j] = ng; g_ag.par[j] = i;
                ag_push(ng + hypotf((float)(X - gx), (float)(Z - gz)) * AG_CELL * fmin, j);
            }
    }
    c7_add("vtlane", "expanded", nexp);
    if (!found) {
        c7_add("vtlane", "fail", 1); c7_ms("vtlane", "plan", t0);
        if (env_on("RECOMP_BOT_DIAG") && (w->time - c->vr_log_t > 5.0 || w->time < c->vr_log_t)) {
            c->vr_log_t = w->time;
            fprintf(stderr, "[BOT-VROUTE] t=%.1f bot=%d no air route %.0f,%.0f -> %.0f,%.0f (start cell border %d edge %.0f, goal cell border %d edge %.0f, %d expanded)\n", w->time, index,
                    me->pos.x, me->pos.z, goal.x, goal.z, g_ag.bd ? g_ag.bd[si] : -1, g_ag.ed ? g_ag.ed[si] : -1.f, g_ag.bd ? g_ag.bd[gi] : -1, g_ag.ed ? g_ag.ed[gi] : -1.f, nexp);
        }
        return 0;
    }
    int nc = 0;
    for (int i = gi; i >= 0 && nc < gw * gh; i = g_ag.par[i]) g_ag.cells[nc++] = i;
    for (int i = 0; i < nc / 2; i++) { int t = g_ag.cells[i]; g_ag.cells[i] = g_ag.cells[nc - 1 - i]; g_ag.cells[nc - 1 - i] = t; }
    /* Pull tight: from each kept cell, on to the farthest cell whose straight cut stands no higher than the cells it replaces
     * (+ slack) and passes nothing shut. */
    int keep[MA2_PATH_MAX]; int nk = 0, i0 = 0;
    keep[nk++] = 0;
    while (i0 < nc - 1 && nk < MA2_PATH_MAX - 2) {
        int best = i0 + 1; float hm = maxf2(g_ag.hg[g_ag.cells[i0]], g_ag.hg[g_ag.cells[i0 + 1]]);
        for (int k2 = i0 + 2; k2 < nc; k2++) {
            hm = maxf2(hm, g_ag.hg[g_ag.cells[k2]]);
            int a = g_ag.cells[i0], e = g_ag.cells[k2];
            float ax = (a % gw) + 0.5f, az = (a / gw) + 0.5f, ex = (e % gw) + 0.5f, ez = (e / gw) + 0.5f;
            int n = (int)(hypotf(ex - ax, ez - az) * AG_CELL / 10.f) + 1, ok = 1;
            for (int s = 1; s < n && ok; s++) {
                int qx = (int)(ax + (ex - ax) * (float)s / (float)n), qz = (int)(az + (ez - az) * (float)s / (float)n), q = qz * gw + qx;
                if (q != gi && (g_ag.hg[q] > hm + slack || ag_shut(c, b, q, clr, me->pos, goal))) ok = 0;
            }
            if (!ok) break;
            best = k2;
        }
        keep[nk++] = best; i0 = best;
    }
    if (keep[nk - 1] != nc - 1) keep[nk++] = nc - 1;
    /* The route, its length, and how high its first 400 m stand (the VtolTerrainClimb checks). */
    Ma2Vec3 pts[MA2_PATH_MAX]; int np = 0;
    pts[np++] = me->pos;
    for (int k2 = 1; k2 < nk - 1; k2++) { int q = g_ag.cells[keep[k2]]; Ma2Vec3 v = { ((q % gw) + 0.5f) * AG_CELL, g_ag.hg[q], ((q / gw) + 0.5f) * AG_CELL }; pts[np++] = v; }
    pts[np++] = goal;
    float rl = 0.f, hi = -1e9f, run = 0.f;
    for (int s = 0; s + 1 < np; s++) {
        float sl = hdist2(pts[s], pts[s + 1]);
        for (float f = 0.f; f <= sl && run + f <= 400.f; f += 12.f) {
            int ok = 0; float h = surface_height(pts[s].x + (pts[s + 1].x - pts[s].x) * f / maxf2(sl, 1e-3f), pts[s].z + (pts[s + 1].z - pts[s].z) * f / maxf2(sl, 1e-3f), &ok);
            if (ok && h > hi) hi = h;
        }
        run += sl; rl += sl;
    }
    c7_ms("vtlane", "plan", t0);
    {
        int okg = 0; float g0 = terrain_height(me->pos.x, me->pos.z, &okg), need = hi + 20.f;
        const char *why = c->vt_ceiling > 0.f && need > c->vt_ceiling + 1.f ? "ground_ceiling" : okg && need - maxf2(g0, me->pos.y - 20.f) > 90.f ? "ground_tall" : NULL;
        if (why) { c7_add("vtlane", why, 1); c->vtk_direct = 0; return -1; }
    }
    Ma2Path *p = &c->path;
    p->n = np; for (int s = 0; s < np; s++) { p->pt[s] = pts[s]; p->drop[s] = 0; }
    p->next = 1; p->goal = goal0; p->radius = plan_radius(me); p->crush = 1;
    p->ok = 1; p->end_gap = 0.f; p->planned = w->time; p->ndyn = 0; p->from = me->pos; p->has_from = 1;
    c->have_path = 1; c->path_fails = 0;
    c->path_t = w->time; c->path_goal = goal0; c->next_plan = w->time + 0.5; c->path_epoch = ma2_trav_epoch();
    c->vtk_direct = np == 2 ? 1 : 2;
    if (c->pq_ticket) c->pq_stale = 1;
    c7_add("vtlane", "plans", 1); c7_add("vtlane", np == 2 ? "straight" : "bends", 1);
    if (env_on("RECOMP_BOT_DIAG") && (c->vr_logged != c->vr_n || w->time - c->vr_log_t > 5.0 || w->time < c->vr_log_t)) {
        char buf[600]; int o = 0;
        for (int s = 1; s < np && o < 560; s++) o += snprintf(buf + o, sizeof buf - (size_t)o, " %.0f,%.0f", pts[s].x, pts[s].z);
        float emin = 1e9f;
        for (int s = 0; s + 1 < np; s++) { float sl = hdist2(pts[s], pts[s + 1]); for (float f = 0.f; f <= sl; f += 10.f) emin = minf2(emin, vt_edge_dist(pts[s].x + (pts[s + 1].x - pts[s].x) * f / maxf2(sl, 1e-3f), pts[s].z + (pts[s + 1].z - pts[s].z) * f / maxf2(sl, 1e-3f))); }
        fprintf(stderr, "[BOT-VROUTE] t=%.1f bot=%d flight=%d from %.0f,%.0f,%.0f straight=%.0f route=%.0f hi400=%.0f edge=%.0f rider=%d K=%.0f lane=%d slack=%.0f band=%.0f exp=%d pts:%s\n",
                w->time, index, c->vr_n, me->pos.x, me->pos.y, me->pos.z, l, rl, hi, emin, c->vt_rider, K, c->vr_lane, slack, c->vt_rider ? minf2(c->vr_band, 4.f) : c->vr_band, nexp, buf);
        c->vr_logged = c->vr_n; c->vr_log_t = w->time;
    }
    return 1;
}
/* Defined before its first use (BeelineVary's bow below): used before its definition it was an implicit int clampf2(),
 * which read its float arguments as garbage and put every flag run's bow point ~35 km off the map (NRC vl2 11-04: bot 5's
 * leg headed for -27174,-21748, VtolEdgeAll piled 12 bends on it, and the craft shuttled 50 m by the edge for 25 s). */
static float clampf2(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* RiderRay (default on, RECOMP_BOT_VTOL_RIDER_RAY / [Bots] RiderRay): with a BattleArmor on the claw, is the way from `a` toward `b` open
 * at the lowest point the rider can be lifted to (this craft's height ceiling, StickCeil, less the 9 m it hangs)? A ray through the
 * game's static collision says (the surface samples the planners use come from the walk grid, which never saw the tops of Bogged
 * Down's middle buildings: the planner thought 25 m stood there, the craft at its 46 m ceiling flew into them, the rider was knocked
 * onto the plateau and the run ended -- playtest 11-04 rec 2026-11-04 bot 10, t=18.9-19.7). A hit is learnt as shut airspace
 * (both planners route round it) and returns 1. [C7] riderray: casts / shut. */
static int vt_rider_ray_shut(BotCtrl *c, double now, const Ma2PlayerView *me, Ma2Vec3 a, Ma2Vec3 b, float span)
{
    if (!c->vt_rider || !(c->vt_ceiling > 0.f) || !c7_on("riderray", "RECOMP_BOT_VTOL_RIDER_RAY", "RiderRay", 1)) return 0;
    /* from 4 m out (was 12: pressed against a tower, the ray began past its wall and saw nothing -- NRC blue sat at 0.5 m/s against
     * one for 16 s, zb2 11-04 bot 4, t=14-30; the craft itself is the filter's self, the rider hangs under it) */
    const float r0 = 4.f;
    float dx = b.x - a.x, dz = b.z - a.z, l = hypotf(dx, dz), len = minf2(l, span) - r0;
    if (len < 10.f) return 0;
    /* in 60 m pieces, each at its own lowest ceiling less the rider's hang: one ray at the lowest ceiling of a 450-600 m leg (28 m over a
     * dip) hit what the craft clears at its far end and shut every way out of New River City's blue base (zc2 11-04 bot 1, 50 s) */
    Ma2Vec3 dir = { dx / l, 0.f, dz / l };
    for (float s0 = r0; s0 < len + r0; s0 += 60.f) {
        float sl = minf2(60.f, len + r0 - s0), cmin = c->vt_ceiling;
        for (float s = s0; s <= s0 + sl + 0.1f; s += 15.f) { float cs = vt_ceil_at(c, a.x + dx / l * s, a.z + dz / l * s); if (cs > 0.f && cs < cmin) cmin = cs; }
        Ma2Vec3 o = { a.x + dx / l * s0, cmin - 9.f, a.z + dz / l * s0 };
        float t = 0.f; uint32_t ob = 0;
        c7_add("riderray", "casts", 1);
        /* both layers: Bogged Down's middle buildings are destructible (dynamic colliders) -- a static-only ray passed under the one the
         * craft flew into at 46 m (vu3 11-04 bot 10, t=22.4, rider knocked off 474 m short; NRC's towers are static and were found) */
        if (!ray_cast_as(me->actor, o, dir, sl, 1, 1, &t, &ob) || t < 0.5f) continue;
        float hx = o.x + dir.x * t, hz = o.z + dir.z * t;
        if (ma2_bot_air_blocked(hx, hz)) return 1;
        airblk_add(hx + dir.x * 8.f, hz + dir.z * 8.f, 18.f);   /* 18 m: 25 m (+ a cell of margin) shut the 30-40 m streets between New River City's towers and boxed the blue base in (za2 11-04 bot 3: 87 s at its base) */
        c7_add("riderray", "shut", 1);
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-RIDERRAY] t=%.1f the way from %.0f,%.0f toward %.0f,%.0f is shut at %.0f m up %.0f m on (%.0f,%.0f, hit %08X): learnt\n",
                                               now, a.x, a.z, b.x, b.z, o.y, s0 + t, hx, hz, gva_ok(ob, 0x30) ? MEM32(ob + 0x28) : 0u);
        return 1;
    }
    return 0;
}
/* VtolTerrainClimb: a straight leg for a VTOL. The ground planner routes round cliffs and up ramps (a 'Mech's
 * way); a VTOL flies over them. The straight line to the goal is looked along for 400 m (a surface sample every
 * 12 m: ground, roofs, rocks): fine unless it runs into learned shut airspace, a known gun, a climb past what the
 * map lets a VTOL reach, or a height this craft already found it cannot climb to. Then the path is the straight
 * line itself (vtol_prepare lifts the cruise height 20 m over the highest surface ahead, 220 m out) and the ground
 * route stays the fallback. Not for the last 60 m: setting down beside a unit or a pad keeps the planner's care. */
static int vtol_direct_path(BotCtrl *c, Bot *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal)
{
    float dx = goal.x - me->pos.x, dz = goal.z - me->pos.z, l = hypotf(dx, dz);
    const char *why = NULL;
    if (l < 30.f) why = "ground_short";
    float hi = -1e9f, span = minf2(l, 400.f), hprev = -1e9f;
    if (!why && ma2_bot_air_blocked(me->pos.x, me->pos.z)) why = "ground_airshut";
    /* every 6 m (was 12): narrow tower tops in the surface grid fell between samples -- New River City's blue straight leg passed with
     * a 74 m top on it against a 48 m ceiling (zg1 11-04, [BOT-VDIRECT]) */
    for (int k = 0, n = (int)(span / 6.f) + 1; !why && k <= n; k++) {
        float f = span * (float)k / (float)n, x = me->pos.x + dx / l * f, z = me->pos.z + dz / l * f;
        int ok = 0; float h = surface_height(x, z, &ok);
        if (ok && h > hi) hi = h;
        if (ok && c->vt_ceil_agl > 0.f) {   /* StickCeil: this spot's ceiling against what stands here and at the last sample */
            if (k == 0) hprev = h;
            if (f > 8.f && maxf2(h, hprev) + 20.f > vt_ceil_at(c, x, z) + 1.f) { why = "ground_ceiling"; c7_add("stickceil", "leg_refused", 1); }
            hprev = h;
        }
        if (k > 0 && ma2_bot_air_blocked(x, z)) why = "ground_airshut";
    }
    if (!why) {
        int okg = 0; float g0 = terrain_height(me->pos.x, me->pos.z, &okg), need = hi + 20.f;
        if (c->vt_ceiling > 0.f && !(c->vt_ceil_agl > 0.f) && need > c->vt_ceiling + 1.f) {   /* (StickCeil over the terrain: per sample, above) */
            why = "ground_ceiling";
            if (env_on("RECOMP_BOT_DIAG") && w->time - c->vtk_diag_t > 5.0) { c->vtk_diag_t = w->time; fprintf(stderr, "[VTCLIMB] t=%.1f ceiling %.0f < need %.0f (surface ahead %.0f, ground here %.0f, y %.0f) at %.0f,%.0f toward %.0f,%.0f\n", w->time, c->vt_ceiling, need, hi, g0, me->pos.y, me->pos.x, me->pos.z, goal.x, goal.z); }
        }
        else if (okg && need - maxf2(g0, me->pos.y - 20.f) > 90.f) why = "ground_tall";
    }
    if (!why && vt_rider_ray_shut(c, w->time, me, me->pos, goal, 400.f)) why = "ground_riderray";   /* RiderRay */
    for (int i = 0; !why && i < b->order.ndanger; i++) {
        const Ma2PathDanger *d = &b->order.danger[i];
        float lim = minf2(l, 250.f);
        if (seg_point_dist(me->pos.x, me->pos.z, me->pos.x + dx / l * lim, me->pos.z + dz / l * lim, d->x, d->z, NULL) < d->r * 0.8f) why = "ground_danger";
    }
    if (why) { c7_add("vtclimb", why, 1); c->vtk_direct = 0; return 0; }
    if (c->vt_rider && env_on("RECOMP_BOT_DIAG")) {   /* diag: a ridered straight leg taken -- what it was judged on */
        float cm = 1e9f, hm = -1e9f, hf = 0.f; for (float f = 0.f; f <= minf2(l, 400.f); f += 6.f) { int ok = 0; float x = me->pos.x + dx / l * f, z = me->pos.z + dz / l * f, h = surface_height(x, z, &ok); if (ok && h > hm) { hm = h; hf = f; } float cc = vt_ceil_at(c, x, z); if (cc > 0.f && cc < cm) cm = cc; }
        fprintf(stderr, "[BOT-VDIRECT] t=%.1f from %.0f,%.0f toward %.0f,%.0f (%.0f m): highest surface %.0f at %.0f m, lowest ceiling %.0f, agl %.1f, ceiling here %.1f\n", w->time, me->pos.x, me->pos.z, goal.x, goal.z, l, hm, hf, cm, c->vt_ceil_agl, c->vt_ceiling);
    }
    Ma2Path *p = &c->path;
    p->n = 2; p->pt[0] = me->pos; p->pt[1] = goal; p->next = 1;       /* pt[0] = the start, so "off the line" is judged against the leg */
    p->goal = goal; p->radius = plan_radius(me); p->crush = 1; p->drop[0] = p->drop[1] = 0;
    p->ok = 1; p->end_gap = 0.f; p->planned = w->time; p->ndyn = 0;
    c->have_path = 1; c->path_fails = 0;
    c->path_t = w->time; c->path_goal = goal; c->next_plan = w->time + 0.5; c->path_epoch = ma2_trav_epoch();
    c->vtk_direct = 1;
    if (c->pq_ticket) c->pq_stale = 1;   /* push-final: a queued ground plan (PathSlice) must not replace this leg when it lands */
    c7_add("vtclimb", "plans_direct", 1);
    return 1;
}
/* VtolEdgeAll: a VTOL route kept off the edge -- its corners kept in (28 m), and a bend (pulled 34 m in) wherever a leg dips
 * into the 25 m band; the first leg's way out of a band it starts in is left alone. Any route: air A*, straight, ground. */
static void vt_route_keep(int index, const Ma2World *w, const Ma2PlayerView *me, Ma2Path *p)
{
    if (me->kind != MA2_KIND_VTOL || !vt_edge_all_on() || !ef_ready() || p->n < 1) return;
    const float keep = 28.f;                                                 /* 3 m over the 25 m band: the flown line strays a few metres off the route */
    int i0 = p->next > 0 ? p->next : 0, nin = 0, nb = 0;
    for (int i = i0; i < p->n; i++) if (vt_keep_in(&p->pt[i], keep) > 0) nin++;
    float e0 = ef_dist(me->pos.x, me->pos.z);
    for (int i = i0; i < p->n && nb < 12; ) {
        Ma2Vec3 a = i == i0 ? me->pos : p->pt[i - 1], bq = p->pt[i];
        float l = hdist2(a, bq), skip = i == i0 && e0 < keep ? keep - e0 + 15.f : 0.f, worst = keep - 3.f, ws = -1.f;
        for (float s = 6.f; s < l - 4.f; s += 6.f) {
            if (s < skip) continue;
            float e = ef_dist(a.x + (bq.x - a.x) * s / l, a.z + (bq.z - a.z) * s / l);
            if (e < worst) { worst = e; ws = s; }
        }
        if (ws < 0.f || p->n >= MA2_PATH_MAX) { i++; continue; }
        Ma2Vec3 q = { a.x + (bq.x - a.x) * ws / l, 0.5f * (a.y + bq.y), a.z + (bq.z - a.z) * ws / l };
        if (vt_keep_in(&q, keep + 6.f) <= 0) { i++; continue; }
        /* EdgeBendAhead (default on, RECOMP_BOT_EDGE_BEND_AHEAD / [Bots] EdgeBendAhead): a bend must lie between the leg's ends. Pushed out
         * along the edge field's slope, a dip near New River City's blue base always landed on the same spot (349.99,1169.79): the leg up
         * to it still dipped, so the same point went in 12 times (path n=15), and once the craft had passed it each 0.5 s replan put it
         * back behind -- the flag-run VTOL with its rider shuttled round its base for 70 s (vm1 11-04 bot 0, t=22-91). Such a leg is
         * left as it is (the 25 m band is a margin; the wall is at the edge itself). [C7] vtedgeall: bend_off_leg. */
        if (c7_on("edgebendahead", "RECOMP_BOT_EDGE_BEND_AHEAD", "EdgeBendAhead", 1)) {
            float dq = hdist2(q, a), pr = ((q.x - a.x) * (bq.x - a.x) + (q.z - a.z) * (bq.z - a.z)) / maxf2(l * l, 1e-3f);
            float cosa = ((q.x - a.x) * (bq.x - a.x) + (q.z - a.z) * (bq.z - a.z)) / maxf2(dq * l, 1e-3f);   /* more than 60 deg off the leg: behind or beside */
            if (cosa < 0.5f || pr > 0.95f || hdist2(q, a) < 10.f || hdist2(q, bq) < 10.f) { c7_add("vtedgeall", "bend_off_leg", 1); i++; continue; }
        }
        for (int k = p->n; k > i; k--) { p->pt[k] = p->pt[k - 1]; p->drop[k] = p->drop[k - 1]; }
        p->pt[i] = q; p->drop[i] = 0; p->n++; nb++;                       /* the new leg a -> q is looked at again */
    }
    if (nin) c7_add("vtedgeall", "pts_in", nin);
    if (nb) {
        c7_add("vtedgeall", "bends", nb);
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VEDGE2] t=%.1f bot=%d route kept off the edge: %d corner(s) in, %d bend(s), %d points\n", w->time, index, nin, nb, p->n);
    }
}
static int vt_mod_climb_on(void); static int stick_ceil_on(void); static float stick_tg(const Ma2PlayerView *me);   /* (defined with ModClimb, below) */
static int ctrl_plan(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal, uint32_t ignore_veh)
{
    BotCtrl *c = &b->ctrl;
    if (me->kind == MA2_KIND_VTOL && vt_climb_on()) {   /* VtolLaneRoute first; a short leg (or no grid) is VtolTerrainClimb's; -1 = the ground route */
        /* StickCeil before the first plan: the ceiling was only set in vtol_prepare's twice-a-second refresh, so a flight's first route
         * could be planned without it -- New River City's blue run took the lane over the towers by its base (a 92 m need) and pressed
         * at its 48 m ceiling for 90 s (ze2 11-04 bot 0, route t=14.8 vs ceiling 21.9; zc2 / zd1 / zb2 alike) */
        if (vt_mod_climb_on() && stick_ceil_on() && (c->vt_sceil_actor != me->actor || !(c->vt_ceil_agl > 0.f))) {
            float cap = MEMF(me->actor + 0x4E0);
            if (cap > 10.f && cap < 3000.f) {
                if (c->vt_sceil_actor != me->actor) { c->vt_sceil_actor = me->actor; c->vt_sceil = 0.f; c->vt_sc_t = 0.0; }
                c->vt_ceil_agl = cap - 6.6f; c->vt_ceiling = stick_tg(me) + c->vt_ceil_agl; c->vt_ceil_t = w->time; c7_add("stickceil", "seeded_at_plan", 1);
            }
        }
        /* FlagBeeline (default on, RECOMP_BOT_FLAG_BEELINE / [Bots] FlagBeeline): a flag run leg flies the straight line
         * (VtolTerrainClimb: over whatever stands in the way, round no hill) -- the lane route (VtolLaneRoute, for the playtest note
         * follow the lanes 11-01) bent the enemy run north and round: 735 m in 24 s at 25-58 m/s, shot down on the way
         * (NRC CTF vj2 11-04, t=10.5-34.8; playtest: the VTOL wandered instead of flying straight to the flag). A gun
         * zone, shut airspace or a height past its reach on the line still sends it by the lane route. [C7] beeline: legs. */
        if (b->order.beeline && c7_on("beeline", "RECOMP_BOT_FLAG_BEELINE", "FlagBeeline", 1)) {
            Ma2Vec3 gin = goal;
            if (vt_lane_route_on() && ag_build(w)) vt_goal_inside(&gin);
            if (vtol_direct_path(c, b, w, me, gin)) {
                c->path.goal = goal; c->path_goal = goal;
                /* BeelineVary (playtest 11-04: keep some path variation on flag runs too): a long leg
                 * bows to one side through a mid point -- up to 9 % of its length (40 m at most), its side and size rolled per
                 * flight (VtolRouteVary's flight count), kept for the whole flight. Not if that point is shut airspace. */
                float lx = gin.x - me->pos.x, lz = gin.z - me->pos.z, ll = hypotf(lx, lz);
                if (ll > 200.f && c->path.n == 2) {
                    uint32_t hsh = (uint32_t)index * 2654435761u ^ (uint32_t)c->vr_n * 40503u ^ 0x9E3779B9u;
                    hsh ^= hsh >> 15; hsh *= 2246822519u; hsh ^= hsh >> 13;
                    float u = (float)(hsh & 0xFFFF) / 65535.f * 2.f - 1.f, f = 0.4f + 0.2f * (float)((hsh >> 16) & 0xFF) / 255.f;
                    float off = clampf2(u * 0.09f * ll, -40.f, 40.f);
                    Ma2Vec3 mid = c->path.pt[0];
                    mid.x += lx * f + (-lz / ll) * off; mid.z += lz * f + (lx / ll) * off;
                    if (fabsf(off) > 5.f && !ma2_bot_air_blocked(mid.x, mid.z)) {
                        c->path.pt[2] = c->path.pt[1]; c->path.pt[1] = mid; c->path.n = 3; c->path.drop[2] = 0; c7_add("beeline", "bowed", 1);
                    }
                }
                vt_route_keep(index, w, me, &c->path); c7_add("beeline", "legs", 1); return 1;
            }
        }
        int ar = vtol_air_route(index, c, b, w, me, goal);
        /* RiderRay over the whole route: with a rider, every leg of a new lane route is checked before it is flown; a leg that is shut
         * at the rider's lowest reachable height is learnt and the route planned again (4 tries). Found one at a time on the next leg,
         * the middle of Bogged Down (ceiling 28 m over the bog, rider at ~19 m, buildings taller) was hit before the craft could turn
         * (vy2 11-04 bot 6, t=14.8 found, 17.6 rider lost at 875,1318). [C7] riderray: route_replans. */
        for (int tries = 0; ar > 0 && c->vt_rider && tries < 4; tries++) {
            int shut = 0; Ma2Vec3 from = me->pos;
            for (int i = c->path.next > 0 ? c->path.next : 1; i < c->path.n && !shut; i++) {
                shut = vt_rider_ray_shut(c, w->time, me, from, c->path.pt[i], hdist2(from, c->path.pt[i]) + 12.f);
                from = c->path.pt[i];
            }
            if (!shut) break;
            c7_add("riderray", "route_replans", 1);
            ar = vtol_air_route(index, c, b, w, me, goal);
        }
        if (ar > 0) { vt_route_keep(index, w, me, &c->path); return 1; }   /* VtolEdgeAll */
        Ma2Vec3 gin = goal;
        if (ar == 0 && vt_lane_route_on() && ag_build(w)) vt_goal_inside(&gin);   /* VtolEdgeKeep: a short leg too */
        if (ar == 0 && vtol_direct_path(c, b, w, me, gin)) { c->path.goal = goal; c->path_goal = goal; vt_route_keep(index, w, me, &c->path); return 1; }   /* VtolEdgeAll: a short leg's bend */
    }
    enum { DYN_MAX = MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS + 8 };
    Ma2PathObstacle dyn[DYN_MAX]; int nd = 0;
    Obst ob[MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS];
    const int sliced = pathslice_on();
    if (sliced && c->pq_ticket && ma2_pathq_state(index) >= 2) { c->next_plan = w->time + 0.25; return c->have_path; }   /* W7: its plan is being searched: let it finish */
    int no = ctrl_obstacles(w, me, ignore_veh, ob, MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS);
    for (int i = 0; i < no; i++) {
        if (!ob[i].slow) continue;                                             /* moving: local avoidance */
        float dme = hypotf(ob[i].x - me->pos.x, ob[i].z - me->pos.z);
        /* A parked hull close to us or to the goal goes in as its measured outline. One circle as
         * wide as its longest arm (a VTOL's tail, a 'Mech's gun) shuts the way past its narrow side
         * and buries an entry zone beside it -- and when we stood inside that circle it was dropped
         * and the plan ran straight through the hull. Standing against it, the plan starts from
         * the nearest free spot outside it. */
        const Ma2VehicleView *hv = NULL;
        for (int k = 0; k < w->nvehicles; k++) if (w->vehicles[k].actor == ob[i].actor && !w->vehicles[k].occupied) hv = &w->vehicles[k];
        if (hv && hv->ext_max > 0.5f && (dme < 40.f || hypotf(ob[i].x - goal.x, ob[i].z - goal.z) < 40.f) && nd < DYN_MAX) {
            Ma2PathObstacle d; memset(&d, 0, sizeof d);
            d.x = hv->pos.x; d.z = hv->pos.z; d.has_star = 1;
            hull_star(hv, hull_tall(me->kind), d.star);
            for (int k = 0; k < MA2_HULL_DIRS; k++) if (d.star[k] > d.r) d.r = d.star[k];
            dyn[nd++] = d;
            continue;
        }
        if (dme < ob[i].r + 0.5f) continue;                                    /* never plan out of the one we touch */
        if (hypotf(ob[i].x - goal.x, ob[i].z - goal.z) < ob[i].r * 0.5f && ob[i].kind == MA2_KIND_PILOT) continue;
        dyn[nd].x = ob[i].x; dyn[nd].z = ob[i].z; dyn[nd].r = ob[i].r; dyn[nd].has_star = 0; nd++;
    }
    /* Where this bot got stuck lately: plan around it (not when it is the goal itself). */
    for (int i = 0; i < c->nstuck && nd < DYN_MAX; i++) {
        if (w->time - c->stuck_t[i] > 90.0 || w->time < c->stuck_t[i]) continue;
        if (hypotf(c->stuck_pt[i].x - goal.x, c->stuck_pt[i].z - goal.z) < 5.f) continue;
        if (hypotf(c->stuck_pt[i].x - me->pos.x, c->stuck_pt[i].z - me->pos.z) < 2.f) continue;
        dyn[nd].x = c->stuck_pt[i].x; dyn[nd].z = c->stuck_pt[i].z; dyn[nd].r = me->kind == MA2_KIND_VTOL ? 20.f : 3.5f; dyn[nd].has_star = 0; nd++;
    }
    if (veh_pad_on()) {                                                      /* VehPad: empty spawn spots, for a tank */
        vspawn_learn(w);
        for (int i = 0; me->kind == MA2_KIND_TANK && i < g_nvspawn && nd < DYN_MAX; i++) {
            float sx = g_vspawn[i].x, sz = g_vspawn[i].z, r = g_vspawn[i].r;
            if (hypotf(sx - me->pos.x, sz - me->pos.z) > 80.f || hypotf(sx - me->pos.x, sz - me->pos.z) < r + 1.f) continue;   /* far, or on it: plan out */
            if (hypotf(sx - goal.x, sz - goal.z) < r + 2.f) continue;
            int full = 0;
            for (int k = 0; k < w->nvehicles; k++) if (hypotf(w->vehicles[k].pos.x - sx, w->vehicles[k].pos.z - sz) < 4.f) { full = 1; break; }
            if (full) continue;                                              /* standing there: its hull is in already */
            dyn[nd].x = sx; dyn[nd].z = sz; dyn[nd].r = r; dyn[nd].has_star = 0; nd++;
            c7_add("vehpad", "plans", 1);
        }
    }
    if (me->kind == MA2_KIND_VTOL)                                           /* learned shut airspace */
        for (int i = 0; i < g_nairblk && nd < DYN_MAX; i++) {
            if (hypotf(g_airblk[i].x - me->pos.x, g_airblk[i].z - me->pos.z) > 500.f) continue;
            if (hypotf(g_airblk[i].x - me->pos.x, g_airblk[i].z - me->pos.z) < g_airblk[i].r * 0.5f) continue;   /* we are in it: get out first */
            dyn[nd].x = g_airblk[i].x; dyn[nd].z = g_airblk[i].z; dyn[nd].r = g_airblk[i].r; dyn[nd].has_star = 0; nd++;
        }
    c->path_t = w->time; c->path_goal = goal; c->next_plan = w->time + 0.5; c->path_epoch = ma2_trav_epoch();
    {   /* known enemy guns: soft costs; known pilot bombs: a steep one over their 25 m blast */
        Ma2PathDanger dz[MA2_ORDER_MAX_DANGER + 16]; int ndz = 0;
        for (int i = 0; i < b->order.ndanger; i++) dz[ndz++] = b->order.danger[i];
        if (me->kind != MA2_KIND_VTOL)
            for (int i = 0; i < w->nbombs; i++) {
                if (w->bombs[i].owner == b->player || hdist2(w->bombs[i].pos, me->pos) > 400.f) continue;
                dz[ndz].x = w->bombs[i].pos.x; dz[ndz].z = w->bombs[i].pos.z; dz[ndz].r = 27.f; dz[ndz].cost = 25.f; ndz++;
            }
        ma2_path_set_danger(dz, ndz);
        c->bav_epoch = w->bomb_epoch;
    }
    ma2_path_set_crush(me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_VTOL);   /* 'Mechs walk through trees; a VTOL flies the 'Mech route */
    /* TankSlope (default on, RECOMP_BOT_TANK_SLOPE / [Bots] TankSlope): tanks planned on the light grid, whose wall starts at 0.8 rise/run (39 deg),
     * and could not drive the slopes between 29 and 39 deg it offered: Snatch It Ill Wind's team-2 exit falls from y 13 at 1194,1408 to y -2 at
     * 1178,1391 (33-38 deg, RECOMP_BOT_HPROBE), where every team-2 tank bounced at 1195-1209,1412-1425 until PocketBail (iw8a, iw9b, iw11b,
     * rc4_81a 10-57 s) and one crept over into a hole. A tank does not enter ground steeper than the 'Mech grid's 0.55 (29 deg). */
    if (me->kind == MA2_KIND_TANK && c7_on("tankslope", "RECOMP_BOT_TANK_SLOPE", "TankSlope", 1)) ma2_path_set_slope_max(55);
    ma2_path_set_pilot(me->kind == MA2_KIND_PILOT);   /* PilotFace */
    ma2_path_set_tank_face(me->kind == MA2_KIND_TANK && c7_on("tankface", "RECOMP_BOT_TANK_FACE", "TankFace", 1));   /* TankFace */
    /* How far down it may step off a ledge. Falls do no damage; the limits only keep routes sane
     * (tanks and pilots stay with modest drops). A tactical move (Ma2Order.drop_mode) only takes a
     * drop its jets climb back up. The VTOL's route never drops. RECOMP_BOT_DROPS=0 turns drops off. */
    {
        static int on = -1;
        if (on < 0) on = env_int("RECOMP_BOT_DROPS", 1);
        float jets = MEMF(me->actor + 0x49C);
        int has_jets = me->kind == MA2_KIND_BATTLEARMOR || (me->kind == MA2_KIND_MECH && jets > 0.05f);
        float body = !on ? 0.f : (me->kind == MA2_KIND_BATTLEARMOR || me->kind == MA2_KIND_MECH) ? 60.f :
                     me->kind == MA2_KIND_TANK ? 6.f : me->kind == MA2_KIND_PILOT ? 10.f : 0.f;   /* a tank has no jets: small ledges only */
        if (b->order.drop_mode == 1) {   /* a tactical move: only down where the jets climb back (2 m to spare) */
            float back = !has_jets ? 0.f : (c->jet_actor == me->actor && c->jet_gain > 0.f ? c->jet_gain : 10.f) - 2.f;
            if (body > back) body = back > 0.f ? back : 0.f;
            /* TankDrop (default on, RECOMP_BOT_TANK_DROP / [Bots] TankDrop): a tank or pilot has no jets, so the tactical limit made its drop 0 -- and the
             * planner's wide start snap (SnapWide2, WP2-B) needs a drop or an up-link above 0, so a tank on a tactical move had no plan when it sat
             * off the grid (sw65 tank 8.5 s no plan, WP2-B note). 1 m: no real drop link (the grid steps are 2 m+), the snap works. */
            if (body < 1.f && (me->kind == MA2_KIND_TANK || me->kind == MA2_KIND_PILOT) && on && tank_drop_on()) { body = 1.f; c7_add("tankdrop", "plans", 1); }
        }
        ma2_path_set_drop(body);
        /* W5 CarrierDrops: carrier / chaser plan with a cheaper drop link and may land in a marsh. */
        if (b->order.drop_pref && body > 0.f) { ma2_path_set_drop_pref(0.85f, 1); c7_add("carrierdrops", "plans", 1); c->cd_plan = 1; }
        else { ma2_path_set_drop_pref(1.0f, 0); c->cd_plan = 0; }
        /* F38: up a ledge where the jets climb it (the measured climb, 2 m to spare). */
        /* JetPrior (default on, RECOMP_BOT_JET_PRIOR / [Bots] JetPrior): until a body had measured its own climb the planner allowed a jet-up of
         * 10 - 2 = 8 m, and one short burn (5.9 m) set it lower still, so a 'Mech never planned the way up to a raised objective: Snatch It Rush
         * Hour's core deck (y 55 over the y 30 street) -- rc4_85a b7 'no route' 9-78 s, its routes ending at 1166,30,1390, then ObjGiveUp; b6 (7
         * fuel, measured 24.1 m) planned the jet-up at 1140,1375 and took the core (rc4_85b). A 'Mech's jets climb about 3.4 m per unit of the
         * fullest tank seen (b6: 7 -> 24 m, b7: 5 -> 15-17 m): that is the prior, a measurement only counts when it is higher. [C7] jetprior: plans. */
        float upm = c->jet_actor == me->actor && c->jet_gain > 0.f ? c->jet_gain : 10.f;
        {
            static float s_fhi[16]; static uint32_t s_fa[16]; int fi = index & 15;
            if (s_fa[fi] != me->actor) { s_fa[fi] = me->actor; s_fhi[fi] = 0.f; }
            if (jets > s_fhi[fi]) s_fhi[fi] = jets;
            if (me->kind == MA2_KIND_MECH && has_jets && c7_on("jetprior", "RECOMP_BOT_JET_PRIOR", "JetPrior", 1)) {
                float prior = minf2(24.f, 3.4f * s_fhi[fi]);
                if (prior > upm) { upm = prior; c7_add("jetprior", "plans", 1); }
            }
        }
        ma2_path_set_up(has_jets && c7_on("f38", "RECOMP_BOT_JET_UP", "JetUpLedges", 1) ? upm - 2.f : 0.f);
    }
    if (sliced) {   /* W7: queue the search; the path arrives through ctrl_plan_take on a later tick */
        int tk = ma2_pathq_submit(index, me->pos, goal, plan_radius(me), dyn, nd, w->time, NULL);
        if (tk > 0) {
            c->pq_ticket = tk; c->pq_actor = me->actor; c->pq_goal = goal; c->pq_from = me->pos; c->pq_nd = nd;
            ma2_path_set_drop_pref(1.0f, 0);
            c7_add("pathslice", "queued", 1);
            return c->have_path;
        }
        if (tk == -2) { ma2_path_set_drop_pref(1.0f, 0); return c->have_path; }
        c7_add("pathslice", "queue_full", 1);   /* no free slot: plan now */
    }
    g_perf_plans++; double pl0 = perf_now_ms();
    ma2_path_set_reach_slot(index);   /* ReachSet: the finished search is kept for this bot's goal picks */
    int ok = ma2_path_plan(&c->path, me->pos, goal, plan_radius(me), dyn, nd, w->time);
    ma2_path_set_reach_slot(-1);
    g_perf_plan_ms += perf_now_ms() - pl0;
    ctrl_plan_done(index, b, w, ok, goal, me->pos, nd);
    if (c->have_path) vt_route_keep(index, w, me, &c->path);           /* VtolEdgeAll: a VTOL's ground route too */
    return c->have_path;
}
/* What follows a finished plan (c->path is filled). */
static void ctrl_plan_done(int index, Bot *b, const Ma2World *w, int ok, Ma2Vec3 goal, Ma2Vec3 from, int nd)
{
    BotCtrl *c = &b->ctrl;
    c->have_path = ok && c->path.n > 0;
    c->nostart = ok ? 0 : c->nostart + 1;
    if (c->cd_plan && c->have_path)
        for (int i = 0; i < c->path.n; i++) if (c->path.drop[i] == 1) { c7_add("carrierdrops", "legs_planned", 1); break; }
    ma2_path_set_drop_pref(1.0f, 0);
    if (!ok || (!c->path.ok && c->path.end_gap > 12.f)) c->path_fails++; else c->path_fails = 0;
    if (c->path_fails >= 3) b->status.no_path = 1;
    if (env_int("RECOMP_BOT_DIAG", 0) >= 2)
        fprintf(stderr, "[BOT-PATH] bot=%d n=%d ok=%d gap=%.1f goal=%.0f,%.0f from=%.0f,%.0f dyn=%d\n", index, c->path.n, c->path.ok, c->path.end_gap, goal.x, goal.z, from.x, from.z, nd);
}
/* ew-b8 (R1 P1-4): the map was committed since this route was planned. It is stale only if a changed cell is near the part
 * still to walk (the bot to the end, each leg padded by the body radius + 4 m); a far-away wreck no longer makes all 12 bots
 * replan. A route judged unaffected takes the current epoch. */
static int ctrl_route_stale(BotCtrl *c, const Ma2PlayerView *me)
{
    unsigned ep = ma2_trav_epoch();
    static int on = -1;
    if (c->path_epoch == ep) return 0;
    if (on < 0) on = c7_on("replan_rect", "RECOMP_BOT_REPLAN_RECT", "ReplanRect", 1);
    if (!on) return 1;
    float pad = plan_radius(me) + 4.f, ax = me->pos.x, az = me->pos.z;
    int n = c->path.n > MA2_PATH_MAX ? MA2_PATH_MAX : c->path.n;
    for (int i = c->path.next < 0 ? 0 : c->path.next; i < n; i++) {
        float bx = c->path.pt[i].x, bz = c->path.pt[i].z;
        if (ma2_trav_changed_since(c->path_epoch, (ax < bx ? ax : bx) - pad, (az < bz ? az : bz) - pad, (ax > bx ? ax : bx) + pad, (az > bz ? az : bz) + pad)) { c7_add("replan_rect", "route_stale", 1); return 1; }
        ax = bx; az = bz;
    }
    if (n <= c->path.next && ma2_trav_changed_since(c->path_epoch, ax - pad, az - pad, ax + pad, az + pad)) { c7_add("replan_rect", "route_stale", 1); return 1; }
    c->path_epoch = ep; c7_add("replan_rect", "route_kept", 1);
    return 0;
}
/* W7: collect a finished queued plan (once per tick, before the replan test). */
static void ctrl_plan_take(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me)
{
    BotCtrl *c = &b->ctrl;
    /* W8: the situation changed after this plan was queued (stall learned a stuck point / jump gave up / entry retarget /
     * escape): the in-flight search (or a finished, untaken path) was made without that knowledge, so it is thrown away and
     * the replan test submits a fresh one. Without this a stalled bot got one stale route straight back through the spot. */
    if (c->pq_stale) {
        c->pq_stale = 0;
        if (c->pq_ticket) { ma2_pathq_cancel(index); c->pq_ticket = 0; c7_add("pathslice", "cancelled", 1); }
    }
    if (!c->pq_ticket) return;
    Ma2Path np; int ret = 0;
    int r = ma2_pathq_take(index, c->pq_ticket, &np, &ret);
    if (r == 0) return;
    c->pq_ticket = 0;
    if (r < 0 || c->pq_actor != me->actor) { c7_add("pathslice", "dropped", 1); return; }
    c->path = np;
    {   /* push-x3 E1: the plan's age is counted from the take, and its epoch is the one it was searched on. A queue wait over 6 s
         * (or any epoch bump while it waited) made the fresh plan 'too old / stale' the same tick and sent the bot straight back to
         * the queue: a backlog never drained (wait_ms_max 15-32 s). */
        static int rf = -1;
        if (rf < 0) rf = c7_on("replanfix", "RECOMP_BOT_REPLAN_FIX", "ReplanFix", 1);
        if (rf) { c->path_t = w->time; if (np.epoch) c->path_epoch = np.epoch; }
    }
    ctrl_plan_done(index, b, w, ret, c->pq_goal, c->pq_from, c->pq_nd);
    if (c->have_path) vt_route_keep(index, w, me, &c->path);           /* VtolEdgeAll */
    c7_add("pathslice", "taken", 1);
    c7_add("pathslice", "wait_ms_sum", (w->time - np.planned) * 1000.0); c7_max("pathslice", "wait_ms_max", (w->time - np.planned) * 1000.0);
}

/* VTOL flight (BAVT_SPEC 3): FlyerAI MoveTo flies toward ai+0x130 at the altitude ai+0x20C
 * (absolute y). A boarded VTOL keeps a "yaw hold" (comp+0x145 bit7, set by sub_113964) that the
 * AI turn step honours; take-off clears it for a player, so clear it once airborne (ai+0x21F = 0). */
static float hull_core_r(int k);
static float hull_r(const Ma2VehicleView *v, int tall);
/* 1 if the straight line a->b keeps clear of every parked vehicle's solid hull (and of bodies
 * standing still); `skip` = a vehicle not counted. */
static float hull_at(const Ma2VehicleView *v, int tall, Ma2Vec3 q, float fallback);
static int seg_hits_hull(const Ma2VehicleView *v, int tall, Ma2Vec3 a, Ma2Vec3 b);
static int hulls_clear(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 a, Ma2Vec3 b, uint32_t skip)
{
    float rad = kind_body_r(me->kind);
    int tall = hull_tall(me->kind);
    for (int i = 0; i < w->nvehicles; i++) {
        const Ma2VehicleView *v = &w->vehicles[i];
        if (v->occupied || v->actor == skip || v->actor == me->actor || v->kind == MA2_KIND_PILOT) continue;
        float r = hull_r(v, tall) + rad;
        if (seg_point_dist(a.x, a.z, b.x, b.z, v->pos.x, v->pos.z, NULL) >= r) continue;   /* nowhere near it */
        /* Within reach of its longest arm: follow its measured outline (a VTOL's tail reaches 7 m
         * one way and 2 m the other), every 0.5 m along the line. */
        float core = hull_core_r(v->kind) + 0.6f;
        float d0 = hypotf(v->pos.x - a.x, v->pos.z - a.z);
        int touching = d0 < hull_at(v, tall, a, core) - 0.6f + rad;
        float l = hypotf(b.x - a.x, b.z - a.z);
        int n = (int)(l / 0.5f) + 1;
        if (touching) {
            /* Already against it (under a VTOL's tail, beside a 'Mech's foot): a way along or away
             * from it is clear, one further in is not. */
            for (int s = 1; s <= n; s++) {
                float t = (float)s / (float)n, px = a.x + (b.x - a.x) * t, pz = a.z + (b.z - a.z) * t;
                if (hypotf(px - v->pos.x, pz - v->pos.z) < d0 - 0.3f) return 0;
            }
            continue;
        }
        for (int s = 0; s <= n; s++) {
            Ma2Vec3 p = a; float t = (float)s / (float)n;
            p.x += (b.x - a.x) * t; p.z += (b.z - a.z) * t;
            float dp = hypotf(p.x - v->pos.x, p.z - v->pos.z);
            if (dp < r && dp < hull_at(v, tall, p, core) - 0.6f + rad) return 0;
        }
    }
    return 1;
}
/* A pilot pressed against a parked vehicle: MoveTo's own avoidance will not move a body that
 * touches a hull, so it backs straight off it (own turn + speed) for a moment, as a player would. */
static int jetup_landed_on(void) { static int on = -1; if (on < 0) on = c7_on("jetuplanded", "RECOMP_BOT_JETUP_LANDED", "JetUpLanded", 1); return on; }
static int pilot_escape(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, float reach)
{
    const Ma2VehicleView *hv = NULL; float bd = 1e9f;
    if (me->kind != MA2_KIND_PILOT) return 0;
    for (int i = 0; i < w->nvehicles; i++) {
        const Ma2VehicleView *v = &w->vehicles[i];
        if (v->occupied || v->kind == MA2_KIND_PILOT) continue;
        float d = hypotf(v->pos.x - me->pos.x, v->pos.z - me->pos.z);
        if (d < bd) { bd = d; hv = v; }
    }
    if (!hv || bd > reach) return 0;
    float ax = me->pos.x - hv->pos.x, az = me->pos.z - hv->pos.z, l = hypotf(ax, az) + 1e-3f;
    c->esc_pt = me->pos; c->esc_pt.x += ax / l * 7.f; c->esc_pt.z += az / l * 7.f;
    c->esc_until = w->time + 1.6; c->escapes++;
    c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 1.6;
    return 1;
}
/* A wall thinner than the map's probe spacing (a fence, a railing, a screen, a thin facade) lets
 * the downward rays pass either side of it, so the map shows open ground. Where it stops a body,
 * sweep it with a fan of chest-high rays toward the way we want to go (+-60 degrees, 10 apart)
 * and block each face they meet -- the wall fills in where it was met, as a player learns it.
 * A hit with the ground under it near the ray's height is a slope, not a wall; one near a
 * body or a parked hull is that, not the map. */
static int stall_batch_on(void)
{
    static int on = -1;
    if (on < 0) on = c7_on("stallbatch", "RECOMP_BOT_STALL_BATCH", "StallBatch", 1);
    return on;
}
static int wall_sweep(int index, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 toward)
{
    if (!g_probe.phys || !ma2_trav_ready()) return 0;
    float bx = toward.x - me->pos.x, bz = toward.z - me->pos.z;
    if (hypotf(bx, bz) < 0.5f) return 0;
    float base = atan2f(bx, bz), y = me->pos.y + 2.0f;
    int n = 0, walls = 0;
    for (int k = -6; k <= 6; k++) {
        float a = base + (float)k * 0.1745f, t = 0.f;
        uint32_t ob = 0;
        Ma2Vec3 o = { me->pos.x, y, me->pos.z }, d = { sinf(a), 0.f, cosf(a) };
        if (!ray_cast(&g_probe, o, d, 18.f, 1, 1, &t, &ob) || t < 0.5f) continue;
        Ma2Vec3 h = { o.x + d.x * t, 0.f, o.z + d.z * t }, foot = { o.x + d.x * (t - 0.4f), 0.f, o.z + d.z * (t - 0.4f) };
        int ok = 0;
        float s = surface_height(foot.x, foot.z, &ok);
        if (!ok || y - s < 1.2f) continue;                                         /* rising ground */
        int body = 0;
        for (int i = 0; i < w->nplayers && !body; i++) if (w->players[i].alive && hdist2(w->players[i].pos, h) < 6.f) body = 1;
        for (int i = 0; i < w->nvehicles && !body; i++) if (hdist2(w->vehicles[i].pos, h) < 8.f) body = 1;
        if (body) continue;
        Ma2Vec3 c = { h.x + d.x * 0.4f, 0.f, h.z + d.z * 0.4f };
        n += ma2_trav_block_disc(c.x, c.z, 0.8f);
        side_prop_add(c, 0.8f);
        walls++;
    }
    if (n && !stall_batch_on()) ma2_trav_commit();     /* ew-b8 B8.17: with StallBatch the stall makes ONE commit (the grid is dirty-tracked) */
    if (walls && env_on("RECOMP_BOT_DIAG"))
        fprintf(stderr, "[BOT-NAV] t=%.1f bot=%d wall sweep at %.0f,%.0f: %d faces, %d cells\n", w->time, index, me->pos.x, me->pos.z, walls, n);
    return walls;
}
/* An up-jump failed near here lately (a rise the jets do not clear): walk round instead of
 * jetting at it again and again. */
static int up_allowed(const BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    return c->upfail_t <= 0.0 || w->time - c->upfail_t > 30.0 || w->time < c->upfail_t || hdist2(me->pos, c->upfail_pt) > 20.f;
}
static float g_vt_ymax = -1e9f; static double g_vt_ymax_t;   /* VtolCeilFix: the highest any bot VTOL has flown this match */
/* RouteLook (default on, RECOMP_BOT_VTOL_ROUTE_LOOK / [Bots] RouteLook): the cruise height is read along the planned route,
 * not a straight line along the heading -- 300 m straight on past a near corner saw the towers the route goes round and
 * sent the ferry up to 132 m over the middle of Bogged Down, a target for the whole map (laptop 10-31). */
static int vt_route_pts(const BotCtrl *c, const Ma2PlayerView *me, Ma2Vec3 dest, Ma2Vec3 *pts)
{
    int np = 0;
    pts[np++] = me->pos; pts[np++] = dest;
    if (c->have_path) for (int i = c->path.next; i < c->path.n && np < MA2_PATH_MAX + 2; i++) pts[np++] = c->path.pt[i];
    return np;
}
static float vt_route_hi(const BotCtrl *c, const Ma2PlayerView *me, Ma2Vec3 dest, float look, int n, float hi)
{
    Ma2Vec3 pts[MA2_PATH_MAX + 2]; int np = vt_route_pts(c, me, dest, pts);
    float step = look / (float)n, d0 = 0.f, at = step;
    for (int s = 0; s + 1 < np && at <= look; s++) {
        float dx = pts[s + 1].x - pts[s].x, dz = pts[s + 1].z - pts[s].z, l = hypotf(dx, dz);
        for (; at <= look && at <= d0 + l; at += step) {
            float f = (at - d0) / maxf2(l, 1e-3f); int ok = 0;
            float h = surface_height(pts[s].x + dx * f, pts[s].z + dz * f, &ok);
            if (ok && h > hi) hi = h;
        }
        d0 += l;
    }
    return hi;
}
/* The point `ahead` metres on along the route (its end if shorter): MoveTo's point while it climbs. MoveTo flies to its point
 * and parks there -- given the carrot 35-45 m off it stopped dead every few seconds of a ferry (laptop 10-31, "stopping
 * randomly"); a point well on keeps it flying while it climbs. */
static Ma2Vec3 vt_route_point(const BotCtrl *c, const Ma2PlayerView *me, Ma2Vec3 dest, float ahead)
{
    Ma2Vec3 pts[MA2_PATH_MAX + 2]; int np = vt_route_pts(c, me, dest, pts);
    float d0 = 0.f;
    for (int s = 0; s + 1 < np; s++) {
        float dx = pts[s + 1].x - pts[s].x, dz = pts[s + 1].z - pts[s].z, l = hypotf(dx, dz);
        if (d0 + l >= ahead) { float f = (ahead - d0) / maxf2(l, 1e-3f); Ma2Vec3 q = { pts[s].x + dx * f, dest.y, pts[s].z + dz * f }; return q; }
        d0 += l;
    }
    { Ma2Vec3 q = pts[np - 1]; q.y = dest.y; return q; }
}
static int vt_route_look_on(void) { return c7_on("routelook", "RECOMP_BOT_VTOL_ROUTE_LOOK", "RouteLook", 1); }

/* NeedClimb (default on, RECOMP_BOT_VTOL_NEED_CLIMB / [Bots] NeedClimb): the craft's own flight has no height control (the
 * VTOL's vertical-axis setter, vtable +0x268, is the empty stub 0x1A3459) and holds its own height over the ground; only MoveTo
 * climbs, at ~8 m/s over the ground. So MoveTo flies only while what is ahead stands higher than the craft can clear -- the
 * cruise height (36 m over everything with a rider) kept it in MoveTo for a whole ferry, 55 s for 840 m (desktop 10-31). In at
 * 2 m short of the need, out once the need has passed (it drops 6 m under the craft). */
static int vt_need_climb_on(void) { return c7_on("needclimb", "RECOMP_BOT_VTOL_NEED_CLIMB", "NeedClimb", 1); }
/* ModClimb (default on, RECOMP_BOT_VTOL_MOD_CLIMB / [Bots] ModClimb): a VTOL climbs the way a player climbs it, flying on.
 * A player holds Shift (the left stick click) to climb and Ctrl to sink; the flight component's vertical demand comp+0x90
 * reads +30 / -30 / 0 (measured with the human in a VTOL, desktop 11-01: W+Shift 4 s = +29 m at full speed). The demand is
 * filled from the player's own input object, which a bot has not -- the same stick-click commands from a bot do nothing,
 * so every bot VTOL held its height until the ground shoved it up and a rider 7-8 m under it met the slope first (playtest
 * 11-01: the VTOL flew into the hill and knocked its rider off). The bot sets that demand itself while the way ahead needs height.
 * No MoveTo climbs any more: MoveTo stopped the craft to climb. */
static int vt_mod_climb_on(void) { return c7_on("modclimb", "RECOMP_BOT_VTOL_MOD_CLIMB", "ModClimb", 1); }
/* StickCeil (default on, RECOMP_BOT_VTOL_STICK_CEIL / [Bots] StickCeil): the height a VTOL flown by the stick (ModClimb) can reach.
 * The player's climb stops short of the map's MaxVTOLHeight (MAFlyer actor+0x4E0, 55 on New River City): with the +30 demand held,
 * bot VTOLs levelled off at 48.0-48.7 m over every cell round NRC's blue base (r1 11-04, 306 samples), MoveTo flights went higher
 * (95 m) but were pulled back down at -30 m/s once the demand was on again. VtolCeilFix never learnt it (a MoveTo flight had been
 * higher, and a learnt cap lapsed in 20 s), so the cruise and the need (83-108 m over the towers by the blue base) were heights it
 * could never reach. Now the ceiling is the cap less 6.5 m (or lower, a held climb that gained nothing for 1.2 s), kept all flight:
 * the straight leg past it is refused (ground_ceiling), the lane route goes round what stands above it, MoveTo is not aimed over it.
 * [C7] stickceil: seeded / learned. */
static int stick_ceil_on(void) { return c7_on("stickceil", "RECOMP_BOT_VTOL_STICK_CEIL", "StickCeil", 1); }
static float stick_tg(const Ma2PlayerView *me) { int ok = 0; float t = terrain_height(me->pos.x, me->pos.z, &ok); return ok ? t : 0.f; }   /* terrain under it */
/* The height the next part of the route needs, just in time: a surface counts only once it is within the distance this
 * craft covers while it climbs to clear it (12 m/s up, 1.2 s spare) -- not a far tower, not a hill off the route. */
static float vt_manual_need(const BotCtrl *c, const Ma2PlayerView *me, Ma2Vec3 dest, float *at_out)
{
    Ma2Vec3 pts[MA2_PATH_MAX + 2]; int np = vt_route_pts(c, me, dest, pts);
    const float clr = (c->vt_rider ? 13.f : 7.f) + (vt_lane_route_on() ? (c->vt_rider ? minf2(c->vr_band, 4.f) : c->vr_band) : 0.f), rate = 7.f, step = 8.f, maxd = 280.f;   /* VtolRouteVary: this flight's cruise band */
    float sp = maxf2(hypotf(me->vel.x, me->vel.z), 15.f), need = -1e9f, d0 = 0.f, at = 0.f;   /* rate: m/s up at the +30 demand */
    if (at_out) *at_out = -1.f;
    for (int s = 0; s + 1 < np && at <= maxd; s++) {
        float dx = pts[s + 1].x - pts[s].x, dz = pts[s + 1].z - pts[s].z, l = hypotf(dx, dz);
        for (; at <= maxd && at <= d0 + l; at += step) {
            float f = (at - d0) / maxf2(l, 1e-3f); int ok = 0;
            float h = surface_height(pts[s].x + dx * f, pts[s].z + dz * f, &ok);
            if (!ok) continue;
            float dh = h + clr - me->pos.y, reach = sp * (maxf2(dh, 0.f) / rate + 1.2f) + 20.f;
            if (at <= reach && h + clr > need) { need = h + clr; if (at_out) *at_out = at; }
        }
        d0 += l;
    }
    return need;
}

static int vt_climb_wanted(const BotCtrl *c, const Ma2PlayerView *me)
{
    if (vt_mod_climb_on()) return 0;   /* the stick climbs (vtol_manual) */
    /* only a rise ahead (a hillside, a building): over a plateau the craft's own flight follows the ground, and MoveTo held
     * there crept at 9 m/s for 40 s (desktop 10-31) */
    /* MoveTo climbs fast (25-50 m/s up) but creeps on once within ~2 m of its height (and sits there): it is aimed 10 m
     * over the need and let go at 3 m over it, before that (desktop 10-31) */
    if (vt_need_climb_on()) return c->vt_need > 0.f && c->vt_rise > (c->vt_climb ? 6.f : 10.f) && me->pos.y < c->vt_need + (c->vt_climb ? 3.f : -1.f);
    return c->vt_alt != 0.f && c->vt_alt > me->pos.y + (c->vt_climb ? 1.5f : 5.f);
}

/* ModClimb hook, called by the flyer tick sub_113964 between its input pass (sub_113478, which sets the flight
 * component's vertical demand comp+0x90 from a human's input object -- 0 for a bot) and the flight step: a bot
 * pilot's climb demand goes in there, as a player's Shift would put it. */
void ma2_bot_vtol_lift(uint32_t actor)
{
    if (!actor || !vt_mod_climb_on()) return;
    for (int i = 0; i < g_rt.nbots; i++) {
        const BotCtrl *c = &g_rt.bots[i].ctrl;
        if (c->vt_lift_actor != actor || c->vt_lift_demand == 0.f) continue;
        uint32_t comp = MEM32(actor + 0x8C);
        if (gva_ok(comp, 0x150)) MEMF(comp + 0x90) = c->vt_lift_demand;
        return;
    }
}

static void vtol_prepare(BotCtrl *c, const Ma2PlayerView *me, Ma2Vec3 dest, double now)
{
    if (now < g_vt_ymax_t) g_vt_ymax = -1e9f;   /* a new match */
    g_vt_ymax_t = now; if (me->pos.y > g_vt_ymax) g_vt_ymax = me->pos.y;
    uint32_t a = me->actor, ai = actor_ai(a), comp = MEM32(a + 0x8C);
    if (!ai || !gva_ok(comp, 0x150)) return;
    /* Cruise height: 20 m over the highest surface on the way ahead (hills, buildings) -- the
     * FlyerAI flies straight at its point and does not climb over what is in between: a VTOL
     * sat pressed against a hillside for a minute. Looked at twice a second; no headway for
     * 4 s while far from the point climbs 15 m more (up to +60). */
    if (now - c->vt_alt_t > 0.5 || now < c->vt_alt_t) {
        /* RiderRay in flight: the next leg at the rider's lowest reachable height -- shut: plan again (round what was learnt) */
        if (c->vt_rider && c->have_path && c->path.next > 0 && c->path.next < c->path.n &&
            vt_rider_ray_shut(c, now, me, me->pos, c->path.pt[c->path.next], maxf2(hypotf(me->vel.x, me->vel.z) * 4.f + 40.f, 80.f))) {
            c->have_path = 0; c->pq_stale = 1; c->next_plan = 0.0; c7_add("riderray", "replans", 1);
        }
        int ok = 0;
        float hi = terrain_height(me->pos.x, me->pos.z, &ok), h;
        if (!ok) hi = me->pos.y - 20.f;
        float g_under = hi;
        float dx = dest.x - me->pos.x, dz = dest.z - me->pos.z, l = hypotf(dx, dz), span = minf2(l, 220.f);
        int nla = vt_climb_on() ? 16 : 12;                       /* VtolTerrainClimb: a sample every ~14 m (a narrow ridge) */
        /* RiderClimb (default on, RECOMP_BOT_VTOL_RIDER_CLIMB / [Bots] RiderClimb): with a BattleArmor on the claw, look 300 m
         * along the heading whatever the next point is, so the climb starts well before a hill -- one met late put the craft
         * down on the slope and knocked the player off (playtest 10-31). */
        if (c->vt_rider && c7_on("riderclimb", "RECOMP_BOT_VTOL_RIDER_CLIMB", "RiderClimb", 1)) { span = 300.f; nla = 24; }
        else if (c7_on("riderclimb", "RECOMP_BOT_VTOL_RIDER_CLIMB", "RiderClimb", 1) && span < 150.f) span = 150.f;   /* a near route point: still see the hill beyond it */
        for (int k = 1; k <= nla && l > 1.f; k++) {
            float f = span * (float)k / (float)nla;
            int oks = 0;
            h = surface_height(me->pos.x + dx / l * f, me->pos.z + dz / l * f, &oks);
            if (oks && h > hi) hi = h;
            if (vt_route_look_on() && f >= 60.f) break;      /* RouteLook: past 60 m the route, below */
        }
        if (vt_route_look_on()) {
            float sp0 = hypotf(me->vel.x, me->vel.z);
            hi = vt_route_hi(c, me, dest, c->vt_rider ? minf2(maxf2(sp0 * 5.f, 150.f), 300.f) : maxf2(span, 150.f), nla, hi);
        }
        /* Climbing to the cruise height is headway as much as flying on: counting it as stuck
         * raised the cruise again, and the VTOL climbed instead of flying (7 m/s across a city). */
        /* VtolHeadway (default on, RECOMP_BOT_VTOL_HEADWAY / [Bots] VtolHeadway; film review 11-08): the watch below counts any 2.2 m of drift as
         * headway, so a craft wobbling +-10 m in place never tripped it. A_64a (CTF, par9) T1 b4 with BA b2 aboard hung at 355-370,1165-1175, y 48
         * (its stick ceiling 48.7) 16-39 s: the route led over a surface it wanted 89 m for. Headway = getting nearer the route's goal: 50 m in
         * 8 s (it cruises 20-100 m/s; v14_64b crept 5 m/s 17-49 s and passed 15 m / 10 s) while 60 m+ from it and not in a firing hover, else the air ahead is shut (as the second stall below) and the route planned
         * round it. [C7] vtheadway: shut. */
        if (c7_on("vtheadway", "RECOMP_BOT_VTOL_HEADWAY", "VtolHeadway", 1)) {
            static struct { uint32_t a; float gx, gz, best; double t; } hw[16];
            int hs = (int)((me->actor >> 4) & 15);
            Ma2Vec3 g = c->have_path ? c->path_goal : dest;
            float dg = hypotf(g.x - me->pos.x, g.z - me->pos.z);
            if (hw[hs].a != me->actor || hypotf(g.x - hw[hs].gx, g.z - hw[hs].gz) > 30.f || dg < hw[hs].best - 50.f || now < hw[hs].t || c->vt_phase == 1 || dg < 60.f) {
                hw[hs].a = me->actor; hw[hs].gx = g.x; hw[hs].gz = g.z; hw[hs].best = dg; hw[hs].t = now;
            } else if (now - hw[hs].t > 8.0) {
                float dx0 = dest.x - me->pos.x, dz0 = dest.z - me->pos.z, l0 = hypotf(dx0, dz0) + 1e-3f;
                airblk_add(me->pos.x + dx0 / l0 * minf2(l0, 30.f), me->pos.z + dz0 / l0 * minf2(l0, 30.f), 30.f);
                c->have_path = 0; c->pq_stale = 1; c->next_plan = 0.0; c7_add("vtheadway", "shut", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VTOL] headway: %.0f m from its goal %.0f,%.0f and no 50 m nearer in 8 s at %.0f,%.0f,%.0f: air ahead shut, replanned\n", dg, g.x, g.z, me->pos.x, me->pos.y, me->pos.z);
                hw[hs].best = dg; hw[hs].t = now;
            }
        }
        if (hdist2(me->pos, c->vt_prog_pos) > 5.f || fabsf(me->pos.y - c->vt_prog_pos.y) > 4.f || c->vt_phase == 1) {   /* a firing hover is not stuck; anything else that sits still is */ c->vt_prog_pos = me->pos; c->vt_prog_t = now; if (c->vt_boost > 0.f) c->vt_boost -= 2.f; }
        else if (now - c->vt_prog_t > 4.0) {
            /* No headway: climb, and let the game's MoveTo fly it for a while -- the awake FlyerAI
             * is fast but on some maps parks in a hover short of its point (New River City). */
            /* The second time running (MoveTo had its turn and got nowhere either): the air ahead
             * is shut. Remember it, so the planner flies round it and nobody sends a VTOL in. */
            if (now < c->vt_moveto_until + 5.0 && c->vt_moveto_until > 0.0) {
                float dx0 = dest.x - me->pos.x, dz0 = dest.z - me->pos.z, l0 = hypotf(dx0, dz0) + 1e-3f;
                airblk_add(me->pos.x + dx0 / l0 * minf2(l0, 30.f), me->pos.z + dz0 / l0 * minf2(l0, 30.f), 30.f);
                c7_add("vtclimb", "airshut_n", 1);
                c->have_path = 0; c->pq_stale = 1; c->next_plan = 0.0;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VTOL] air shut ahead of %.0f,%.0f,%.0f toward %.0f,%.0f (%d learned)\n", me->pos.x, me->pos.y, me->pos.z, dest.x, dest.z, g_nairblk);
            }
            c->vt_boost = minf2(c->vt_boost + 15.f, 60.f); c->vt_prog_t = now;
            c7_add("vtclimb", "stall_n", 1);
            c->vt_moveto_until = now + 8.0;
            if (env_on("RECOMP_BOT_DIAG") && g_probe.phys) {          /* what stands in the way */
                char buf[300]; int o = 0;
                static const float hs[5] = { -20.f, 0.f, 20.f, 50.f, 100.f };
                for (int k = 0; k < 5; k++) {
                    float t = 0.f; uint32_t ob = 0;
                    Ma2Vec3 or0 = { me->pos.x, me->pos.y + hs[k], me->pos.z }, d = { dx / (l + 1e-3f), 0.f, dz / (l + 1e-3f) };
                    int hit = ray_cast(&g_probe, or0, d, 120.f, 1, 1, &t, &ob);
                    o += snprintf(buf + o, sizeof buf - (size_t)o, " h%+.0f=%s%.0f/%08X", hs[k], hit ? "" : "-", hit ? t : 0.f, gva_ok(ob, 0x30) ? MEM32(ob + 0x28) : 0u);
                }
                fprintf(stderr, "[BOT-VTOL] stalled at %.0f,%.0f,%.0f toward %.0f,%.0f:%s\n", me->pos.x, me->pos.y, me->pos.z, dest.x, dest.z, buf);
                { static int dumped; if (!dumped++) {   /* the FlyerAI object and the actor: floats that look like positions or radii */
                    uint32_t aio = actor_ai(a);
                    for (uint32_t base = 0; base < 2; base++) { uint32_t ob = base ? a : aio; char ln[2200]; int q = 0;
                        q += snprintf(ln + q, sizeof ln - (size_t)q, "[BOT-VTOL] %s %08X:", base ? "actor" : "ai", ob);
                        for (uint32_t off = 0; off < (base ? 0x600u : 0x400u) && q < 2150; off += 4) { float f = MEMF(ob + off);
                            if ((f > 50.f && f < 2000.f) || (f < -50.f && f > -2000.f)) q += snprintf(ln + q, sizeof ln - (size_t)q, " %X=%.0f", off, f); }
                        fprintf(stderr, "%s\n", ln); } } }
            }
        }
        if (c->vt_boost < 0.f) c->vt_boost = 0.f;
        {
            /* Up at once for what is ahead; down gently once it is passed (3 m each half second),
             * so a skyline of towers does not send it up and down -- each change of height stops
             * the FlyerAI's run while it climbs or sinks. */
            float want = hi + 20.f + c->vt_boost, obst = hi;
            if (c->vt_low_pref || c->vt_phase == 3 || c->vt_phase == 4) {
                /* nap of the earth: 8 m over what is close ahead, not 20 m over the tallest thing
                 * within 220 m -- terrain and buildings get between it and the guns */
                int okg = 0; float lo = terrain_height(me->pos.x, me->pos.z, &okg), h2;
                if (!okg) lo = me->pos.y - 10.f;
                /* LowLookAhead (default on, RECOMP_BOT_VTOL_LOW_LOOK / [Bots] LowLookAhead): 60 m is one second at a VTOL's
                 * cruise -- too late to climb a hill or a wall: with the player on the claw it flew into both and knocked him
                 * off (laptop 10-31, y held at 12 m). Look 4 s of flight ahead (5 s with a rider), 60-240 m, past a near point. */
                float lspan = minf2(l, 60.f); int lns = 6;
                if (c7_on("lowlook", "RECOMP_BOT_VTOL_LOW_LOOK", "LowLookAhead", 1)) {
                    float sp = hypotf(me->vel.x, me->vel.z);
                    lspan = minf2(maxf2(sp * (c->vt_rider ? 5.f : 4.f), c->vt_rider ? 120.f : 60.f), 240.f); lns = 16;
                }
                for (int k = 1; k <= lns && l > 1.f; k++) {
                    float f = lspan * (float)k / (float)lns; int oks = 0;
                    h2 = surface_height(me->pos.x + dx / l * f, me->pos.z + dz / l * f, &oks);
                    if (oks && h2 > lo) lo = h2;
                    if (vt_route_look_on() && f >= 60.f) break;
                }
                if (vt_route_look_on()) lo = vt_route_hi(c, me, dest, lspan, lns, lo);
                want = lo + 8.f + c->vt_boost * 0.5f; obst = lo;
            }
            /* A BattleArmor on the claw hangs 7-8 m under the skids: at the usual 8-10 m over the
             * ground it dragged along and was knocked off within seconds of the pickup (runs fr10,
             * fr18). Carry it 12 m higher. */
            if (c->vt_rider) want += c7_on("riderclimb", "RECOMP_BOT_VTOL_RIDER_CLIMB", "RiderClimb", 1) ? 16.f : 12.f;
            /* NeedClimb: the height the way ahead calls for -- its top plus room for the craft (and a rider 7-8 m under it) */
            c->vt_need = obst + (c->vt_rider ? 12.f : 6.f) + c->vt_boost * 0.5f;
            c->vt_rise = obst - g_under + c->vt_boost;   /* how far the way ahead stands over the ground below: the craft follows that ground itself */
            c->vt_alt = (want >= c->vt_alt || c->vt_alt <= 0.f || now - c->vt_alt_t > 3.0) ? want : maxf2(want, c->vt_alt - 3.f);
            /* The map caps a VTOL's height (MaxVTOLHeight): a climb that gains nothing for 3 s has
             * found it -- fly at that height, or MoveTo would be asked to climb for ever. */
            if (c->vt_ceil_actor != a) { c->vt_ceil_actor = a; c->vt_ceiling = 0.f; c->vt_climb_t = 0.0; }
            if (c->vt_climb) {
                if (c->vt_climb_t <= 0.0 || me->pos.y > c->vt_climb_y + 1.f || now < c->vt_climb_t) { c->vt_climb_y = me->pos.y; c->vt_climb_t = now; }
                else if (now - c->vt_climb_t > 3.0) {
                    /* VtolCeilFix (default on, RECOMP_BOT_VTOL_CEIL_FIX / [Bots] VtolCeilFix): vt_climb is not re-read while the
                     * MoveTo window is open, so a level cruise counted as a climb that gained nothing and "found" caps 10-20 m
                     * over the ground (laptop log 10-31: 7, 14, 22, 33 m; the same VTOL later flew at 86 m). Each one shut the
                     * straight leg (ground_ceiling: the long way round a cliff) and held the cruise down into buildings. Now a cap
                     * is learnt only in a real climb, at least 35 m over the ground below. */
                    int okt = 0; float gt = terrain_height(me->pos.x, me->pos.z, &okt);
                    int real = !c7_on("vtceilfix", "RECOMP_BOT_VTOL_CEIL_FIX", "VtolCeilFix", 1) ||
                               (c->vt_alt > me->pos.y + 5.f && (!okt || me->pos.y - gt >= 35.f) &&
                                me->pos.y > g_vt_ymax - 3.f);   /* the cap is the map's, absolute: a VTOL has flown higher -- not it (25 m "cap", 47 m flown: laptop 10-31) */
                    if (real) { c->vt_ceiling = me->pos.y + 0.5f; c->vt_ceil_t = now; }
                    else c7_add("vtceilfix", "rejected", 1);
                    c->vt_climb_t = now;
                }
            } else c->vt_climb_t = 0.0;
            if (c->vt_ceiling > 0.f && (now - c->vt_ceil_t > 20.0 || now < c->vt_ceil_t)) c->vt_ceiling = 0.f;   /* it may be terrain-relative: look again */
            if (c->vt_ceiling > 0.f && (me->pos.y > c->vt_ceiling + 2.f || g_vt_ymax > c->vt_ceiling + 3.f)) c->vt_ceiling = 0.f;   /* VtolCeilFix: above it (or another VTOL was) -- no cap there */
            if (vt_mod_climb_on() && stick_ceil_on()) {   /* StickCeil: the stick's ceiling, all flight */
                float cap = MEMF(a + 0x4E0), sc = cap > 10.f && cap < 3000.f ? cap - 6.6f : 0.f;   /* over the terrain */
                if (c->vt_sceil_actor != a) { c->vt_sceil_actor = a; c->vt_sceil = 0.f; c->vt_sc_t = 0.0; c7_add("stickceil", "seeded", 1); }
                if (c->vt_sceil > 0.f && (sc <= 0.f || c->vt_sceil < sc)) sc = c->vt_sceil;
                c->vt_ceil_agl = sc;
                if (sc > 0.f) { c->vt_ceiling = stick_tg(me) + sc; c->vt_ceil_t = now; }
            }
            if (c->vt_ceiling > 0.f && c->vt_alt > c->vt_ceiling) c->vt_alt = c->vt_ceiling;
        }
        c->vt_alt_t = now;
    }
    if (c->vt_alt_force > 0.f) c->vt_alt = c->vt_alt_force;
    if (c->vt_alt == c->vt_alt) MEMF(ai + 0x20C) = c->vt_alt;
    /* NeedClimb: MoveTo climbs only what is needed (it creeps at ~8 m/s while it climbs, and sits 2 m short of its height) */
    if (c->vt_alt_force <= 0.f && vt_need_climb_on() && c->vt_need == c->vt_need && c->vt_need > 0.f)
        MEMF(ai + 0x20C) = c->vt_ceiling > 0.f && stick_ceil_on() ? minf2(c->vt_need + 10.f, c->vt_ceiling) : c->vt_need + 10.f;   /* StickCeil: not over it */   /* ew-b8 B8.4: not a NaN altitude */
    /* The AI yaw controller (sub_193D6D) does nothing while actor+0x248 is 0.0 -- and a VTOL a
     * player climbed into has it 0 (AI-spawned flyers do not). Without it the craft cannot turn
     * under AI control, and the navigator cuts its speed whenever the next point is not ahead:
     * the bot VTOL hung in the air at every bend of its route. The field is only that gate. */
    if (MEMF(a + 0x248) == 0.f) MEMF(a + 0x248) = 1.f;
    if (!MEM8(ai + 0x21F) && (MEM8(comp + 0x145) & 0x80) && !(vt_mod_climb_on() && now - c->vt_manual_t < 0.5 && now >= c->vt_manual_t)) {   /* airborne with the yaw hold set (again): release it -- not under the stick (ModClimb: a player's VTOL has it set) */
        MEM8(comp + 0x145) &= 0x7F;
        if (c->vtol_unhold == a && env_on("RECOMP_BOT_DIAG")) { static int nlog; if (nlog++ < 20) fprintf(stderr, "[BOT-VTOL] yaw hold set again on %08X\n", a); }
        c->vtol_unhold = a;
    }
}

/* StickSpeed0: hand a VTOL from the AI to the stick. The AI's MoveTo (body_moveto) sets the flight component's FORWARD-SPEED
 * target (comp+0x7C state +0x1C = comp+0x98, through sub_2AD5C0; up to +-comp+0xBC = 60 m/s), and the body integrator
 * sub_002AD940 adds that ground term (sub_2AD800: heading x speed ramped toward comp+0x98 at comp+0xC4/s) to the human flight
 * model's velocity (comp+0x10C) every step while comp+0x144 bit 8 is set -- the stick does not clear it. A rider pick-up
 * hover (cargo 3, body_moveto over a point behind the nose) left -53.4 there: the red craft flew 60 m/s forward on the stick
 * minus 53.4 back = a 6 m/s crawl to the flag (playtest botrec 2026-11-10 t=11.9-39.5); a blue ferry kept +43.2 and flew
 * 100-117 m/s. A player's VTOL never has an AI target there. [C7] stickspeed0: zeroed. */
static int stick_speed0_on(void) { static int on = -1; if (on < 0) on = c7_on("stickspeed0", "RECOMP_BOT_VTOL_STICK_SPEED0", "StickSpeed0", 1); return on; }
static void vtol_stick_take(uint32_t a, uint32_t comp)
{
    MEM32(comp + 0x144) &= ~1u;                               /* the stick, not the AI */
    if (MEMF(comp + 0x98) != 0.f && stick_speed0_on()) {
        recomp_func_t speed = resolve(0x002AD5C0u);
        if (env_on("RECOMP_BOT_DIAG")) { static int nlog; if (nlog++ < 40) fprintf(stderr, "[BOT-VTOL] stick takes %08X: AI forward speed %.1f (now %.1f) -> 0\n", a, MEMF(comp + 0x98), MEMF(comp + 0x5C)); }
        if (speed) { GuestRegs r; regs_save(&r); g_edx = comp; g_eax = 1; PUSH32(g_esp, 0u); PUSH32(g_esp, 0x002AD631u); speed(); regs_restore(&r); }
        c7_add("stickspeed0", "zeroed", 1);
    }
}

/* RECOMP_BOT_VTOL_MANUAL: fly the VTOL with the player's own controls (BOTS guide: action 4 LY ->
 * forward thrust vt+0x264, 8 LX -> lateral, 5 RY -> vertical vt+0x268, 9 RX -> yaw vt+0x260; the human
 * flight model runs while comp+0x144 bit0, "AI-driven", is clear). Signs are read from the log. */
static void vtol_manual(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 dest)
{
    uint32_t a = me->actor, comp = MEM32(a + 0x8C), node = MEM32(a + 0x94), ai = actor_ai(a);
    if (!gva_ok(comp, 0x150) || !gva_ok(node, 0x100)) return;
    vtol_stick_take(a, comp);                                 /* the stick, not the AI (and no AI forward speed left on it) */
    c->vt_c90_seen = MEMF(comp + 0x90);                      /* diag: what the climb demand held since our last write */
    if (vt_mod_climb_on()) { MEM32(comp + 0x144) |= 0x8000u; c->vt_manual_t = w->time; }   /* ModClimb: as a player's VTOL (0x0010810E) */
    if (ai) { MEM8(ai + 0x90) = 0; }
    static int ys = 0, ts = 0, vs = 0;
    if (!ys) { const char *e = getenv("RECOMP_BOT_VTOL_SIGNS"); int a1 = 1, a2 = 1, a3 = 1; if (e) sscanf(e, "%d,%d,%d", &a1, &a2, &a3); ys = a1; ts = a2; vs = a3; }
    float fx = MEMF(node + 0xE0), fz = MEMF(node + 0xE8);
    float err = atan2f(dest.x - me->pos.x, dest.z - me->pos.z) - atan2f(fx, fz);
    while (err > 3.14159265f) err -= 6.2831853f;
    while (err < -3.14159265f) err += 6.2831853f;
    float yaw = err * 2.0f; yaw = yaw > 1.f ? 1.f : yaw < -1.f ? -1.f : yaw;
    float thrust = fabsf(err) < 0.5f ? 1.f : fabsf(err) < 1.2f ? 0.4f : 0.f;
    float dy = (c->vt_alt != 0.f ? c->vt_alt : me->pos.y) - me->pos.y, vert = dy * 0.15f;   /* 0 = unset (heights below 0 are real: New River City's ground is at -3) */
    vert = vert > 1.f ? 1.f : vert < -1.f ? -1.f : vert;
    c->axis[9] = yaw * (float)ys; c->axis[4] = thrust * (float)ts; c->axis[5] = vert * (float)vs;
    if (vt_mod_climb_on()) {
        c->axis[5] = 1e-6f;                                 /* RY is the gun's pitch on a VTOL, not height */
        if (w->time - c->vt_mneed_t > 0.2 || w->time < c->vt_mneed_t) { c->vt_mneed_t = w->time; c->vt_mneed = vt_manual_need(c, me, dest, &c->vt_mneed_at); }
        if (c->vt_mneed > -1e8f && me->pos.y + me->vel.y * 0.6f < c->vt_mneed) {
            c->vt_lift_actor = a; c->vt_lift_demand = 30.f;   /* the climb demand a player's Shift sets (comp+0x90: +30 Shift, -30 Ctrl, 0); ma2_bot_vtol_lift writes it */
            if (!c->vt_mclimb) { c->vt_mclimb = 1; c7_add("modclimb", "climbs", 1);
                if (env_on("RECOMP_BOT_DIAG") && (w->time - c->vt_mclimb_log > 2.0 || w->time < c->vt_mclimb_log) && (c->vt_mclimb_log = w->time, 1)) fprintf(stderr, "[BOT-VCLIMB] t=%.1f bot=%d climbs %.0f -> %.0f for a surface %.0f m on (sp %.0f)\n", w->time, index, me->pos.y, c->vt_mneed, c->vt_mneed_at, hypotf(me->vel.x, me->vel.z)); }
            c7_add("modclimb", "climb_s", 1.0 / 60.0);
        } else c->vt_mclimb = 0;
        if (stick_ceil_on()) {   /* StickCeil: the climb held and nothing gained for 1.2 s -- the stick's ceiling here */
            if (c->vt_sceil_actor != a) { c->vt_sceil_actor = a; c->vt_sceil = 0.f; c->vt_sc_t = 0.0; }
            if (c->vt_lift_demand >= 30.f && fabsf(me->vel.y) < 0.5f && me->pos.y > 20.f) {
                if (c->vt_sc_t <= 0.0 || w->time < c->vt_sc_t || fabsf(me->pos.y - c->vt_sc_y) > 1.f) { c->vt_sc_t = w->time; c->vt_sc_y = me->pos.y; }
                /* only near the cap: a held climb also stalls with a rider snagged or pressed under something -- it "learnt" 28.2 and
                 * 35.5 m on Bogged Down (vu1/vu2 11-04), shrank the ceiling and shut air at 20 m; real plateaus: 46.0-48.7 m, cap 55 */
                else if (w->time - c->vt_sc_t > 1.2 && (c->vt_sceil <= 0.f || me->pos.y + 0.5f - stick_tg(me) < c->vt_sceil - 0.5f) &&
                         (!(MEMF(a + 0x4E0) > 10.f && MEMF(a + 0x4E0) < 3000.f) || me->pos.y - stick_tg(me) > MEMF(a + 0x4E0) - 6.6f - 5.f)) {
                    c->vt_sceil = me->pos.y + 0.5f - stick_tg(me); c7_add("stickceil", "learned", 1);   /* over the terrain */
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-STICKCEIL] t=%.1f bot=%d the stick climb stops at %.1f m (cap %.0f) at %.0f,%.0f\n", w->time, index, me->pos.y, MEMF(a + 0x4E0), me->pos.x, me->pos.z);
                }
            } else c->vt_sc_t = 0.0;
        }
    }
    /* StickClimb (default mode 5, RECOMP_BOT_VTOL_CLIMB_MODE / [Bots] StickClimb): the stick's vertical does not move the craft;
     * a player raises it holding a stick click -- RS here (11 -> 60 m in 4 s at 40-110 m/s; LS did nothing: desktop 10-31). It did
     * not always take (8 s at 15 m in a fight), so no metre gained in 1 s of it hands the climb to MoveTo for 3 s (slow, sure).
     * Default now 1 = MoveTo only: RS click is the defensive system, and the craft's own flight holds a height over the ground
     * under it whatever the stick or a click asks (the 11 -> 60 m was the ground rising; laptop 10-31: it sank 40 -> 22 m at
     * speed with RS held, and the fallbacks parked it). 2 = LS, 3 = RS only, 4 = both, 5 = RS + MoveTo fallback (tests). */
    { int cm = env_int("RECOMP_BOT_VTOL_CLIMB_MODE", 1);
      if (!c7_on("stickclimb", "RECOMP_BOT_VTOL_STICK_CLIMB", "StickClimb", 1)) cm = 0;
      if (dy > 2.f && (cm == 2 || cm == 4)) ctrl_button(c, BTN_LS, 2);
      if (dy > 2.f && (cm == 3 || cm == 4 || cm == 5)) ctrl_button(c, BTN_RS, 2);
      if (cm == 5) {
          if (dy <= 2.f || c->rs_t0 <= 0.0 || w->time < c->rs_t0 || me->pos.y > c->rs_y0 + 1.f) { c->rs_t0 = dy > 2.f ? w->time : 0.0; c->rs_y0 = me->pos.y; }
          else if (w->time - c->rs_t0 > 1.0 && dy > 5.f) { c->vt_moveto_until = w->time + 3.0; c->rs_t0 = 0.0; c7_add("stickclimb", "moveto_fallbacks", 1); }
      } }
    if (env_on("RECOMP_BOT_VTOL_PROBE") && (w->time - c->vt_probe_t > 0.5 || w->time < c->vt_probe_t)) {   /* diag: the flight model's state, to find its height target */
        c->vt_probe_t = w->time;
        static const struct { const char *n; uint32_t lo, hi; } rg[3] = { { "c", 0x0, 0x400 }, { "a", 0x200, 0xA00 }, { "i", 0x0, 0x400 } };
        uint32_t base[3] = { comp, a, ai };
        int gok = 0; float gnd = terrain_height(me->pos.x, me->pos.z, &gok);
        fprintf(stderr, "[VPROBE] t=%.1f bot=%d y=%.2f gnd=%.1f sp=%.1f vy=%.2f", w->time, index, me->pos.y, gok ? gnd : -999.f, hypotf(me->vel.x, me->vel.z), me->vel.y);
        for (int r = 0; r < 3; r++) { if (!gva_ok(base[r], rg[r].hi)) continue;
            for (uint32_t off = rg[r].lo; off < rg[r].hi; off += 4) { float f = MEMF(base[r] + off);
                if (f == f && fabsf(f) > 0.05f && fabsf(f) < 3000.f && (MEM32(base[r] + off) & 0x7F800000u) != 0) fprintf(stderr, " %s%X=%.2f", rg[r].n, off, f); } }
        fprintf(stderr, "\n");
    }
    if (c->axis[9] == 0.f) c->axis[9] = 1e-6f;
    if (env_on("RECOMP_BOT_VTOL_TRACE") && (w->time - c->vt_trace_t > 0.5 || w->time < c->vt_trace_t)) {
        c->vt_trace_t = w->time;
        int gok = 0; float gnd = terrain_height(me->pos.x, me->pos.z, &gok);
        fprintf(stderr, "[BOT-VMAN] t=%.1f bot=%d pos=%.0f,%.0f,%.0f sp=%.1f dest=%.0f,%.0f(%.0f) err=%.0f yaw=%.2f thr=%.2f vert=%.2f alt=%.0f c144=%08X gnd=%.0f max494=%.0f def498=%.0f vtol4c0=%.0f mneed=%.0f@%.0f mclimb=%d c90=%.1f/%.1f c144=%08X a260=%08X c7C=%08X\n",
                w->time, index, me->pos.x, me->pos.y, me->pos.z, hypotf(me->vel.x, me->vel.z), dest.x, dest.z, hdist2(dest, me->pos),
                err * 57.3f, c->axis[9], c->axis[4], c->axis[5], c->vt_alt, MEM32(comp + 0x144), gok ? gnd : -999.f,
                MEMF(a + 0x494), MEMF(a + 0x498), MEMF(a + 0x4C0), c->vt_mneed, c->vt_mneed_at, c->vt_mclimb, c->vt_c90_seen, MEMF(comp + 0x90), MEM32(comp + 0x144), MEM32(a + 0x260), MEM32(comp + 0x7C));
    }
}

/* The stick toward `dest` at height `alt`, forward thrust capped at `thrust_max` and cut while far off
 * the height (sink or climb first, then go): setting down beside a ride, a low pass over a power-up.
 * The game's flight AI holds its own height unless MoveTo drives it, so this is the player's stick. */
static void vtol_stick(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 dest, float alt, float thrust_max)
{
    c->vt_alt = alt;
    vtol_manual(index, c, w, me, dest);
    float f = fabsf(alt - me->pos.y) > 4.f ? 0.3f : 1.f;
    c->axis[4] *= thrust_max * f;
    if (c->axis[4] == 0.f) c->axis[4] = 1e-6f;        /* sent as "stick centred", not "no input" */
}

/* [C7] vtpad: one helipad repair run = from the first tick within 70 m of the pad to the first tick it is no longer
 * the run (the brain moved on). runs / aborted (never pressed A), approach_s (70 m -> first A), heal_s (first A ->
 * health full), heal_total_s (70 m -> health full, summed over healed_n runs: the s per heal), post_s (full -> the run ended: the leaving delay), total_s, hp_gain, overshoot_n (settled, then
 * flew past 25 m again). Counted with the switch on or off. */
static void vtpad_close(BotCtrl *c, const Ma2PlayerView *me)
{
    double end = c->hz_last;
    c->hz_in = 0;
    c7_add("vtpad", "runs", 1);
    c7_add("vtpad", "total_s", end - c->hz_t0);
    if (c->hz_a_t > 0.0) {
        double full = c->hz_hp_full ? c->hz_full_t : end;
        c7_add("vtpad", "approach_s", c->hz_a_t - c->hz_t0);
        c7_add("vtpad", "heal_s", full > c->hz_a_t ? full - c->hz_a_t : 0.0);
        if (c->hz_hp_full) { c7_add("vtpad", "post_s", end - full); c7_add("vtpad", "healed_n", 1); c7_add("vtpad", "heal_total_s", full - c->hz_t0); c7_max("vtpad", "heal_total_max", full - c->hz_t0); }
        c7_max("vtpad", "approach_max", c->hz_a_t - c->hz_t0);
    } else c7_add("vtpad", "aborted", 1);
    if (me->hp > c->hz_hp0) c7_add("vtpad", "hp_gain", me->hp - c->hz_hp0);
}

/* [C7] vtclimb: what a VTOL's travel legs did (both switch states, so ON/OFF compare): seconds under way (fly_s),
 * metres closed on the goal (close_m; fly_s/close_m = closing speed, whatever the route), metres actually flown
 * (path_m), seconds within 5 m of what is under it (low_s) or slower than 3 m/s (slow_s), the highest it got. */
static void vtol_track(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal)
{
    float d = hdist2(me->pos, goal);
    if (c->vtk_t > 0.0 && w->time > c->vtk_t && w->time - c->vtk_t < 1.0 && hdist2(goal, c->vtk_goal) < 30.f && d > 50.f && me->pos.y > 6.f) {
        float dt = (float)(w->time - c->vtk_t), step = hdist2(me->pos, c->vtk_pos);
        int ok = 0; float s = surface_height(me->pos.x, me->pos.z, &ok);
        c7_add("vtclimb", "fly_s", dt); c7_add("vtclimb", "close_m", c->vtk_d - d); c7_add("vtclimb", "path_m", step);
        if (c->vtk_direct) c7_add("vtclimb", "direct_s", dt);
        if (c->vtk_direct == 2) c7_add("vtlane", "lane_s", dt);   /* VtolLaneRoute: on a bent air route */
        if (c->vt_rider) { c7_add("vtclimb", "ferry_s", dt); c7_add("vtclimb", "ferry_close_m", c->vtk_d - d); c7_add("vtclimb", "ferry_path_m", step); }   /* a BattleArmor aboard: the flag-run legs */
        if (ok && me->pos.y - s < 5.f) c7_add("vtclimb", "low_s", dt);
        if (hypotf(me->vel.x, me->vel.z) < 3.f) { c7_add("vtclimb", "slow_s", dt); c->vs_dt = dt; }   /* (vt_slow_note says where it came from) */
        c7_max("vtclimb", "max_y", me->pos.y);
    }
    c->vtk_t = w->time; c->vtk_goal = goal; c->vtk_d = d; c->vtk_pos = me->pos;
    {   /* RECOMP_TEST_VTOL_VELWATCH=<n> (test hook): once per run, the first craft that carries a rider 10 m+ up gets its body velocity x
         * (comp+0x50, the integrated state; comp+0x14 is last frame's copy) watched: the next n writes print the writer's guest call chain */
        static int vw = -2; extern void xbox_WatchArmVa(uint32_t va, int max_reports);
        if (vw == -2) vw = env_int("RECOMP_TEST_VTOL_VELWATCH", 0);
        if (vw > 0 && c->vt_rider && me->pos.y > 10.f) {
            uint32_t comp = MEM32(me->actor + 0x8C);
            if (gva_ok(comp, 0x120)) {
                fprintf(stderr, "[BOT-VELWATCH] t=%.1f actor %08X comp %08X vel50=%.3f,%.3f,%.3f v110=%.2f,%.2f\n", w->time, me->actor, comp,
                        MEMF(comp + 0x50), MEMF(comp + 0x54), MEMF(comp + 0x58), MEMF(comp + 0x110), MEMF(comp + 0x118));
                xbox_WatchArmVa(comp + 0x50, vw); vw = 0;
            }
        }
    }
    {   /* diag: a craft with a rider crawling on a long leg (Worker D, wd_cap1: 6.0 m/s for 15 s at 44 m, 900 m from the goal) */
        float sp = hypotf(me->vel.x, me->vel.z);
        c->va_sp_s += (sp - c->va_sp_s) * minf2((float)w->dt / 1.5f, 1.f); sp = c->va_sp_s;   /* smoothed (1.5 s): a crawl jitters 4-16 m/s (weB0) and never held under 9 for 3 s */
        if (!(c->vt_rider && d > 100.f && sp < 9.f && me->pos.y > 8.f)) c->va_crawl_t = 0.0;
        else if (c->va_crawl_t <= 0.0 || w->time < c->va_crawl_t) c->va_crawl_t = w->time;
        else if (w->time - c->va_crawl_t > 3.0) {
            c7_add("vtarrive", "crawl_s", w->dt);
            if (env_on("RECOMP_BOT_DIAG") && (w->time - c->va_crawl_log > 2.0 || w->time < c->va_crawl_log)) {
                uint32_t ai2 = actor_ai(me->actor), comp = MEM32(me->actor + 0x8C);
                c->va_crawl_log = w->time;
                fprintf(stderr, "[BOT-VCRAWL] t=%.1f %.0f m/s %.1f s, %.0f m to go, branch %c moveto_left=%.1f mode=%u awake=%u c144=%08X ax4=%.2f ax9=%.2f lift=%.0f dest=%.0f m alt=%.0f y=%.0f path=%d/%d\n",
                        w->time, sp, w->time - c->va_crawl_t, d, c->va_bprev ? c->va_bprev : '-', c->vt_moveto_until - w->time, ai2 ? MEM32(ai2 + 0x12C) : 99u, ai2 ? (unsigned)MEM8(ai2 + 0x90) : 9u,
                        gva_ok(comp, 0x150) ? MEM32(comp + 0x144) : 0u, c->axis[4], c->axis[9], c->vt_lift_demand, hdist2(c->dest, me->pos), c->vt_alt, me->pos.y, c->path.next, c->have_path ? c->path.n : 0);
            }
        }
    }
    if (g_ag.have_tm && me->pos.y > 6.f && (w->time - c->ve_t > 0.5 || w->time < c->ve_t)) {   /* VtolEdgeKeep: how close to the border it flew */
        float dt = (float)minf2((float)(w->time - c->ve_t), 1.f), e = 1e9f;
        c->ve_t = w->time; c->ve_n++;
        int tu = ti_type_at(me->pos.x, me->pos.z);
        if (vt_edge_dist(me->pos.x, me->pos.z) < 120.f)
            for (float oz = -100.f; oz <= 100.f; oz += 4.f)
                for (float ox = -100.f; ox <= 100.f; ox += 4.f) {
                    float r = hypotf(ox, oz); if (r >= e) continue;
                    int tv = ti_type_at(me->pos.x + ox, me->pos.z + oz);
                    if (tv > 0 && (tv & 1)) e = r;
                }
        if (e < c->ve_min) { c->ve_min = e; c->ve_at = me->pos; c->ve_type = tu; }
        if (me->pos.y > c->ve_ymax) c->ve_ymax = me->pos.y;
        if (dt > 0.f) {
            if (e < 20.f) c7_add("vtedge", "near20_s", dt);
            if (tu > 0 && (tu & 1)) c7_add("vtedge", "over_oob_s", dt); else if (tu > 0 && (tu & 2)) c7_add("vtedge", "over_border_s", dt);
        }
    }
}

/* Walk/drive to goal along the planned path. Returns 1 when within `stop`.
 * overshoot > 0: aim past the final point (MoveTo halts ~5-10 m short of its point). */
/* VtolArrive (default on, RECOMP_BOT_VTOL_ARRIVE / [Bots] VtolArrive): Worker D 11-01 (Deliverance, the player ferried): a VTOL with a
 * rider never came down over their flag -- it crossed their base ~35 m up, passed the flag at up to ~80 m/s, overshot 80-150 m and came
 * back for 25 s, and a hurt Veteran that "pressed on" was shot down doing it. The stick flight holds full forward thrust while its nose
 * is on the point and has no braking; MoveTo's hover takes over only inside 14 m, which it crossed in a fraction of a second. Now a VTOL
 * bound for an exact stop (stop <= 5 m: a drop, a pick-up, a hover; not a point above it) brakes on a curve: forward speed held to
 * 4 + sqrt(2 x 20 m/s2 x the way left along its route), latched once begun, the stick pulled back (it reverses thrust: 94 -> 42 m/s
 * in 0.5 s) when faster, the drift across the
 * nose steered toward the point; with a rider aboard it sinks inside 90 m (the flight's -30 demand, a player's Ctrl) to 12 m over the
 * drop point's ground (10 m over whatever stands under it), so the rider hangs a few metres up; MoveTo's hover does the last 14 m. The
 * goal is already kept off the edge (VtolEdgeAll). Any map: it reads only the route, the ground and the craft. [C7] vtarrive: brakes,
 * brake_s, sink_s, overshoot_n (25 m+ past a goal it had come within 30 m of); measured on or off: t60_n, t60_drop_s / drop_n (first
 * inside 60 m of a rider's stop -> the rider under 8 m over the ground), t60_off_s / off_n (-> the rider let go first). [BOT-VARRIVE]. */
static int vt_arrive_on(void) { return c7_on("vtarrive", "RECOMP_BOT_VTOL_ARRIVE", "VtolArrive", 1); }
/* VtolClimbUp (default on, RECOMP_BOT_VTOL_CLIMB_UP / [Bots] VtolClimbUp): Worker D (wd_cap1) saw a VTOL with a rider crawl ~6 m/s for
 * 15 s. Reproduced worse (Deliverance 11-01, wcar1): a VTOL with its rider 8.6 m up hung still 93 s on the pick-up's climb -- the
 * brain's "up first" point (25 m on and 25 m up, re-set each step) counted as reached by its ground distance alone once VtolEdgeAll
 * had moved it in under the craft, and the stick flight never climbs to a goal's height (only to what the ground ahead needs). Now
 * a point 4 m+ overhead is not reached until the craft is up there, and the craft climbs to it (the +30 demand, a player's Shift),
 * easing its forward thrust to 0.3 within 30 m. [C7] vtclimbup: climb_s. */
static int vt_climb_up_on(void) { return c7_on("vtclimbup", "RECOMP_BOT_VTOL_CLIMB_UP", "VtolClimbUp", 1); }
static float path_length(const Ma2Path *p, Ma2Vec3 from);
static float vt_ground_at(float x, float z)                                  /* what a hovering craft must clear there: ground or what stands on it */
{
    int ok1 = 0, ok2 = 0; float g = terrain_height(x, z, &ok1), s = surface_height(x, z, &ok2);
    return ok1 && ok2 ? maxf2(g, s) : ok1 ? g : ok2 ? s : -1e9f;
}
/* SinkAhead (default on, RECOMP_BOT_VTOL_SINK_AHEAD / [Bots] SinkAhead; playtest 11-12 VTOLs sank until the ground knocked the
 * BattleArmor rider off): the rider's sink to drop height (VtolArrive, < 90 m from the point) asked for max(point's ground + 12, ground under
 * the craft + 10) -- only what lay at the point and straight below. At 50 m/s and -30 m/s sink it flew on over what stood between:
 * playtest botrec 11-12 t=96.8-97.4, blue VTOL (bot 7) 92 m from our flag, ground -0.2 there, sank 27.8 -> 20.3 m onto a 16 m
 * block 76 m short, the craft 4 m over it and its rider (hanging ~4.7 m under the skids) knocked off -- the run ended 70 m short.
 * Now the height asked also clears the highest ground the craft reaches in the next 1.5 s (along its velocity and toward the point,
 * +-4 m abeam): + 11 m with a rider aboard, + 6 m without. [C7] sinkahead: held_s (the ground ahead raised the height asked). */
static int sink_ahead_on(void) { return c7_on("sinkahead", "RECOMP_BOT_VTOL_SINK_AHEAD", "SinkAhead", 1); }
static float vt_ground_ahead(const Ma2PlayerView *me, Ma2Vec3 goal)
{
    float sp = hypotf(me->vel.x, me->vel.z), len = clampf2(sp * 1.5f + 10.f, 15.f, 90.f), mx = -1e9f;
    float gx = goal.x - me->pos.x, gz = goal.z - me->pos.z, gd = hypotf(gx, gz);
    for (int dir = 0; dir < 2; dir++) {
        float ux, uz, l = len;
        if (dir == 0) { if (sp < 2.f) continue; ux = me->vel.x / sp; uz = me->vel.z / sp; }
        else { if (gd < 1.f) continue; ux = gx / gd; uz = gz / gd; l = minf2(len, gd); }
        for (float s = 0.f; s <= l + 0.1f; s += 4.f)
            for (int k = -1; k <= 1; k++) {
                float g = vt_ground_at(me->pos.x + ux * s - uz * 4.f * k, me->pos.z + uz * s + ux * 4.f * k);
                if (g > mx) mx = g;
            }
    }
    return mx;
}
/* PickupGlide (default on, RECOMP_BOT_VTOL_PICKUP_GLIDE / [Bots] PickupGlide; playtest 11-12 pick-ups were slow; picking up
 * teammates on the move should be smooth): a pick-up flew in at cruise height (~24 m over the BattleArmor), braked, then hung
 * still over it and sank 12 m on MoveTo before the claw (playtest botrec 11-12: bot 7 t=81.4-82.4, bot 4 192.6-193.7 / 164.0-164.8:
 * 0.8-1.1 s of vertical sink per pick-up after the braking). Now inside 80 m of a pick-up (cargo 3, nobody aboard) the craft glides
 * down while it brakes, to claw height (the BattleArmor + 11 m, + 0.15 m per m beyond 20 m out), never under the ground ahead + 6 m,
 * so it arrives low and the hover only centres it. [C7] pickupglide: sink_s. */
/* RiderDodge (default on, RECOMP_BOT_VTOL_RIDER_DODGE / [Bots] RiderDodge; playtest 11-12 towers knocked riders off): the
 * stick ceiling is terrain-relative (MaxVTOLHeight - 6.6, ~48 m on NRC) and a rider hangs ~4.7 m under the skids, so a ~42 m tower top
 * can be cleared by the craft but not by its rider. New River City's towers by both bases took riders off three times in 13 matches:
 * playtest botrec 11-12 t=254.4 (blue base tower at 1058,899, rider gone, the craft at 26 m into it), v29a6 t=30.5 (same tower, craft at
 * 53 m, rider at 48 m caught), v29c1 t=33.0 (red base tower 848,1373, rider at 47 m caught). With a rider aboard, the stick's steer
 * point is checked: when the ground on the next 30-100 m toward it stands higher than the ceiling there - 10.5 m, the nearest heading
 * (+-20..90 deg) that is clear is flown instead, for that step. [C7] riderdodge: steer_s, none_s (no clear heading). */
static int rider_dodge_on(void) { return c7_on("riderdodge", "RECOMP_BOT_VTOL_RIDER_DODGE", "RiderDodge", 1); }
static int vt_rider_line_clear(const BotCtrl *c, const Ma2PlayerView *me, float a, float len)
{
    float ux = sinf(a), uz = cosf(a), top = maxf2(me->pos.y, c->vt_ceiling > 0.f ? 0.f : me->pos.y + 8.f);
    for (float s = 6.f; s <= len + 0.1f; s += 4.f)
        for (int k = -1; k <= 1; k++) {
            float x = me->pos.x + ux * s - uz * 5.f * k, z = me->pos.z + uz * s + ux * 5.f * k, g = vt_ground_at(x, z);
            float cl = vt_ceil_at(c, x, z); if (!(cl > 0.f)) cl = top;
            if (g > -1e8f && g > maxf2(cl, me->pos.y) - 10.5f) return 0;
        }
    return 1;
}
static void vt_rider_dodge(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 *dest)
{
    if (!c->vt_rider || !rider_dodge_on()) return;
    float dx = dest->x - me->pos.x, dz = dest->z - me->pos.z, dl = hypotf(dx, dz);
    if (dl < 5.f) return;
    float sp = hypotf(me->vel.x, me->vel.z), len = clampf2(sp * 1.6f + 20.f, 30.f, 100.f), a0 = atan2f(dx, dz);
    if (vt_rider_line_clear(c, me, a0, minf2(len, dl + 10.f))) return;
    /* the side kept 2 s once chosen: alternating +-20 deg step by step it stalled 0 m/s among the blue base's towers (v29g3 red bot 3
     * t=31-46, steer_s 11.5) */
    static float last_side[16]; static double last_t[16];
    int si = index >= 0 && index < 16 ? index : 0;
    float pref = w->time - last_t[si] < 2.0 && w->time >= last_t[si] ? last_side[si] : 0.f;
    static const float offs[4] = { 0.35f, 0.70f, 1.05f, 1.57f };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 2; k++) {
            float sg = (k == 0) == (pref >= 0.f) ? 1.f : -1.f, a = a0 + sg * offs[i];
            if (!vt_rider_line_clear(c, me, a, len)) continue;
            float l2 = maxf2(dl, 40.f);
            dest->x = me->pos.x + sinf(a) * l2; dest->z = me->pos.z + cosf(a) * l2;
            last_side[si] = sg; last_t[si] = w->time;
            c7_add("riderdodge", "steer_s", w->dt); return;
        }
    c7_add("riderdodge", "none_s", w->dt);
}
static int vt_low_stick(const Ma2World *w, const Ma2PlayerView *me)   /* LowStick (see the stick branch in ctrl_travel) */
{
    if (!c7_on("lowstick", "RECOMP_BOT_VTOL_LOW_STICK", "LowStick", 1)) return 0;
    int ok = 0; float g = terrain_height(me->pos.x, me->pos.z, &ok);
    if (!ok || me->pos.y - g <= 6.f) return 0;
    c7_add("lowstick", "n_s", w->dt); return 1;
}
static int pickup_glide_on(void) { return c7_on("pickupglide", "RECOMP_BOT_VTOL_PICKUP_GLIDE", "PickupGlide", 1); }
static int g_vt_pickup;                                                      /* set round a pick-up's run in (ORD_TRAVEL cargo 3, nobody aboard) */
static void vt_arrive(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal, float stop)
{
    if (!vt_arrive_on() || stop > 5.f || goal.y > me->pos.y + 4.f) { c->va_brake = 0; return; }
    uint32_t node = MEM32(me->actor + 0x94);
    if (!gva_ok(node, 0x100)) return;
    float r = hdist2(me->pos, goal);
    if (c->have_path && hdist2(c->path_goal, goal) < 10.f) r = maxf2(r, path_length(&c->path, me->pos));
    float fx = MEMF(node + 0xE0), fz = MEMF(node + 0xE8), fl = hypotf(fx, fz);
    if (fl < 1e-3f) return;
    fx /= fl; fz /= fl;
    float vf = me->vel.x * fx + me->vel.z * fz, vl = me->vel.x * fz - me->vel.z * fx;
    float tv = 4.f + sqrtf(2.f * 20.f * maxf2(r - 6.f, 0.f));
    if (c->va_brake && (r > c->va_r0 + 30.f || hdist2(goal, c->va_bgoal) > 30.f)) c->va_brake = 0;   /* a new goal, or it went off away */
    if (vf > tv - 1.f || c->va_brake) {                                     /* latched: it rides the curve in (let go at once it sped up again: 66 -> 94 m/s) */
        if (!c->va_brake) { c->va_brake = 1; c->va_r0 = r; c->va_bgoal = goal; c7_add("vtarrive", "brakes", 1); }
        float dx = goal.x - me->pos.x, dz = goal.z - me->pos.z, fwd = dx * fx + dz * fz, lat = dx * fz - dz * fx;
        float tvs = fwd < 0.f ? -minf2(tv, 6.f) : minf2(tv, maxf2(fwd, 0.f) * 0.6f + 4.f);   /* past it: back up slowly */
        c->axis[4] = clampf2((tvs - vf) * 0.12f, -1.f, 1.f); if (c->axis[4] == 0.f) c->axis[4] = 1e-6f;
        c->axis[8] = clampf2((clampf2(lat * 0.5f, -12.f, 12.f) - vl) * 0.12f, -1.f, 1.f); if (c->axis[8] == 0.f) c->axis[8] = 1e-6f;
        c7_add("vtarrive", "brake_s", w->dt); c->vs_flags |= 2;
    }
    if (c->vt_rider && r < 90.f && vt_mod_climb_on()) {                     /* down to drop height over the point */
        float gy = vt_ground_at(goal.x, goal.z), under = vt_ground_at(me->pos.x, me->pos.z);
        float want = maxf2(gy > -1e8f ? gy + 12.f : me->pos.y, under > -1e8f ? under + 10.f : -1e9f);
        float floor_y = want;                                               /* SinkAhead: no sink under the ground ahead + 11 (it never climbs for it) */
        if (sink_ahead_on()) {
            float ah = vt_ground_ahead(me, goal);
            if (ah > -1e8f && ah + 11.f > want) { floor_y = ah + 11.f; if (me->pos.y - want > 1.5f) c7_add("sinkahead", "held_s", w->dt); }
        }
        if (me->pos.y - floor_y > 1.5f + maxf2(-me->vel.y, 0.f) * 0.3f) { c->vt_lift_actor = me->actor; c->vt_lift_demand = -30.f; c7_add("vtarrive", "sink_s", w->dt); c->vs_flags |= 4; }
        else if (me->pos.y < want - 2.f) { c->vt_lift_actor = me->actor; c->vt_lift_demand = 30.f; }   /* (under it: up; a prediction from the sink's own speed climbed in bursts) */
        else c->vt_lift_demand = 0.f;
    } else if (!c->vt_rider && g_vt_pickup && r < 80.f && vt_mod_climb_on() && pickup_glide_on()) {   /* PickupGlide: down to claw height on the way in */
        float want = goal.y + 11.f + maxf2(r - 20.f, 0.f) * 0.15f, ah = vt_ground_ahead(me, goal);
        if (ah > -1e8f) want = maxf2(want, ah + 6.f);
        if (me->pos.y - want > 1.5f + maxf2(-me->vel.y, 0.f) * 0.3f) { c->vt_lift_actor = me->actor; c->vt_lift_demand = -30.f; c7_add("pickupglide", "sink_s", w->dt); c->vs_flags |= 4; }
        /* (only sinks: the climb stays the stick's own) */
    }
}
/* VtolArrive measure (switch on or off): a rider's stop -- first inside 60 m, then the rider down (under 8 m over the ground) or off. */
static void vt_arrive_measure(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o)
{
    const Ma2PlayerView *rd = NULL;
    for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == me->actor) { rd = &w->players[j]; break; }
    /* Takeoff after the claw (either switch state; Worker E 11-01: Bogged Down's took ~4 s with 3812551, ~2 s before): a rider newly
     * aboard -> 40 m from where it clawed on. [C7] vttakeoff: n, away_s (summed) / away_max, to20_s (-> first 20 m/s across), clear_s
     * (-> the rider 12 m over the ground), hold_s (meanwhile under a hover / pick-up order: the brain holding for a second rider, not
     * a slow takeoff), timeout_n (not in 30 s), and which branch flew it (S M W A H other, as vt_slow_note). [BOT-VTAKEOFF] (DIAG). */
    if (!rd) c->vto_on = 0;
    else {
        if (!c->vto_on) { c->vto_on = 1; c->vto_t0 = w->time; c->vto_done = 0; c->vto_clear = 0; c->vto_t20 = -1.f; c->vto_hold = 0.f; c->vto_p0 = me->pos; memset(c->vto_br, 0, sizeof c->vto_br); }
        if (!c->vto_done) {
            static const char *const k_tb[6] = { "br_stick_s", "br_moveto_s", "br_window_s", "br_awake_s", "br_hover_s", "br_other_s" };
            char br = c->va_bprev; int bi = br == 'S' ? 0 : br == 'M' ? 1 : br == 'W' ? 2 : br == 'A' ? 3 : br == 'H' ? 4 : 5;
            float sp = hypotf(me->vel.x, me->vel.z), rg0 = vt_ground_at(rd->pos.x, rd->pos.z), up = w->time - c->vto_t0;
            c->vto_br[bi] += w->dt; c7_add("vttakeoff", k_tb[bi], w->dt);
            if (env_on("RECOMP_BOT_VTOL_TRACE") && (w->time - c->vto_tr_t >= 0.2 || w->time < c->vto_tr_t)) {
                uint32_t ai2 = actor_ai(me->actor);
                c->vto_tr_t = w->time;
                fprintf(stderr, "[BOT-VTKTR] t=%.2f bot=%d +%.1f br=%c fl=%d y=%.1f agl=%.1f vy=%.1f sp=%.1f rider_agl=%.1f ord=%d stop=%.0f goal_d=%.0f goal_dy=%.0f mode=%u awake=%u win=%.1f alt=%.0f ai20C=%.0f lift=%.0f ax4=%.2f\n",
                        w->time, index, up, br ? br : '-', c->vs_fprev, me->pos.y, me->pos.y - vt_ground_at(me->pos.x, me->pos.z), me->vel.y, sp, rg0 > -1e8f ? rd->pos.y - rg0 : -1.f, o->type, o->stop,
                        hdist2(me->pos, o->point), o->point.y - me->pos.y, ai2 ? MEM32(ai2 + 0x12C) : 99u, ai2 ? (unsigned)MEM8(ai2 + 0x90) : 9u, c->vt_moveto_until - w->time, c->vt_alt,
                        ai2 ? MEMF(ai2 + 0x20C) : 0.f, c->vt_lift_demand, c->axis[4]);
            }
            if (!c->vto_clear && rg0 > -1e8f && rd->pos.y - rg0 >= 12.f) { c->vto_clear = 1; c7_add("vttakeoff", "clear_s", up); }
            if (c->vto_t20 < 0.f && sp >= 20.f) { c->vto_t20 = up; c7_add("vttakeoff", "to20_s", up); }
            if (o->type == MA2_ORD_TRAVEL && o->cargo) { c->vto_hold += w->dt; c7_add("vttakeoff", "hold_s", w->dt); }
            int away = hdist2(me->pos, c->vto_p0) >= 40.f;
            if (away || up > 30.0) {
                c->vto_done = 1;
                if (away) { c7_add("vttakeoff", "n", 1); c7_add("vttakeoff", "away_s", up); c7_max("vttakeoff", "away_max", up); } else c7_add("vttakeoff", "timeout_n", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VTAKEOFF] t=%.1f bot=%d claw -> %s %.1f s, 20 m/s at %.1f s, held %.1f s (rider %s, %.0f m over the ground now; craft y=%.0f) stick %.1f moveto %.1f window %.1f awake %.1f hover %.1f other %.1f\n",
                                                       c->vto_t0, index, away ? "40 m away" : "not 40 m away", up, c->vto_t20, c->vto_hold, rd->is_human ? "player" : "bot", rg0 > -1e8f ? rd->pos.y - rg0 : -1.f, me->pos.y,
                                                       c->vto_br[0], c->vto_br[1], c->vto_br[2], c->vto_br[3], c->vto_br[4], c->vto_br[5]);
            }
        }
    }
    int active = rd && o->type == MA2_ORD_TRAVEL && o->stop <= 5.f;
    if (c->va_t60 > 0.0 && !c->va_done && !active) {
        c7_add("vtarrive", "t60_off_s", w->time - c->va_t60); c7_add("vtarrive", "off_n", 1); c->va_done = 1;
        c7_add("vtarrive", "off_agl_sum", me->pos.y - vt_ground_at(me->pos.x, me->pos.z)); c7_add("vtarrive", "off_sp_sum", hypotf(me->vel.x, me->vel.z));
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VARRIVE] t=%.1f bot=%d the rider let go (or the stop ended) %.1f s after 60 m (craft %.0f m over the ground, %.0f m/s; the %s rider last %.1f m up, %.0f m from the point)\n", w->time, index, w->time - c->va_t60, me->pos.y - vt_ground_at(me->pos.x, me->pos.z), hypotf(me->vel.x, me->vel.z), c->va_rhum ? "player" : "bot", c->va_ragl, c->va_rr);
    }
    if (!active) { c->va_t60 = 0.0; if (!rd) c->va_set = 0; return; }
    if (!c->va_set || hdist2(o->point, c->va_goal) > 40.f) { c->va_set = 1; c->va_goal = o->point; c->va_t60 = 0.0; c->va_done = 0; c->va_os = 0; c->va_rmin = 1e9f; c->va_far = 0; }
    float r = hdist2(me->pos, o->point), rg = vt_ground_at(rd->pos.x, rd->pos.z), ragl = rg > -1e8f ? rd->pos.y - rg : 99.f;
    c->va_ragl = ragl; c->va_rr = r; c->va_rhum = rd->is_human;           /* (for the let-go line: the rider's height there) */
    if (r > 150.f) c->va_far = 1;                                           /* a flight in, not a hover / pick-up on the spot */
    if (r < 60.f && c->va_far && c->va_t60 <= 0.0 && !c->va_done) { c->va_t60 = w->time; c7_add("vtarrive", "t60_n", 1); }
    if (c->va_t60 > 0.0) {
        if (r < c->va_rmin) c->va_rmin = r;
        if (!c->va_os && c->va_rmin < 30.f && r > c->va_rmin + 25.f) { c->va_os = 1; c7_add("vtarrive", "overshoot_n", 1); }
        if (!c->va_done && r < 30.f && ragl < 8.f) {
            c->va_done = 1; c7_add("vtarrive", "t60_drop_s", w->time - c->va_t60); c7_add("vtarrive", "drop_n", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VARRIVE] t=%.1f bot=%d rider down (%.1f m up) %.1f s after 60 m, %.0f m from the point\n", w->time, index, ragl, w->time - c->va_t60, r);
        }
    }
    if (env_on("RECOMP_BOT_VTOL_TRACE") && r < 300.f && (w->time - c->va_tr_t >= 0.25 || w->time < c->va_tr_t)) {
        c->va_tr_t = w->time;
        fprintf(stderr, "[BOT-VARRIVE] t=%.2f bot=%d r=%.0f sp=%.1f vy=%.1f agl=%.0f rider_agl=%.1f brake=%d lift=%.0f cargo=%d stop=%.0f\n", w->time, index, r, hypotf(me->vel.x, me->vel.z), me->vel.y,
                me->pos.y - vt_ground_at(me->pos.x, me->pos.z), ragl, c->va_brake, c->vt_lift_demand, o->cargo, o->stop);
    }
}
/* VtolLiftOff (default on, RECOMP_BOT_VTOL_LIFT_OFF / [Bots] VtolLiftOff): Worker C (wcarB1, 11-01) saw Bogged Down's pick-up take off
 * about 4 s after the claw, 2 s before. Traced (weBd0, 11-01): not VtolClimbUp or VtolArrive (neither acted). Two older holds did it:
 * (1) ctrl_underfoot -- a vehicle boarded under 6 s ago stands still while a teammate's pilot is within 9 m across the ground --
 * froze the VTOL 7-16 m up, its rider just clawed on, for 1.5-1.6 s while a pilot walked under it to his 'Mech; (2) the parked-drive
 * check (under 2 m/s for 1.5 s, the point 30 m+ off: MoveTo flies it 4 s) kept its clock from an earlier branch or life, so the first
 * slow step of a takeoff from the ground (under 6 m world height the stick does not fly it) opened that window at once, and MoveTo
 * climbed it creeping (0 m/s across for 2.4 s, then 11-15 m/s): 3.1-3.2 s to 20 m/s, against 1.5-2.0 s for a clean takeoff. Now a VTOL
 * 4 m+ off the ground or with a rider aboard is never held for a pilot under it, and the parked clock runs only over consecutive
 * steps and not while the craft is climbing. [C7] vtliftoff: underfoot_skip_s, park_windows, park_stale. */
static int vt_lift_off_on(void) { return c7_on("vtliftoff", "RECOMP_BOT_VTOL_LIFT_OFF", "VtolLiftOff", 1); }
/* ctrl_travel's parked-drive check (see its call): 1 = MoveTo's 4 s window opens now. */
static int vt_park_window(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 dest)
{
    int fix = vt_lift_off_on();
    int parked = hypotf(me->vel.x, me->vel.z) < 2.f && hdist2(dest, me->pos) > 30.f && w->time >= c->vt_moveto_until && !c->vt_climb &&
                 !(fix && fabsf(me->vel.y) > 2.f);                        /* VtolLiftOff: going up is not parked */
    int stale = fix && c->vt_slow_t > 0.0 && (w->time - c->vt_slow_seen > 0.25 || w->time < c->vt_slow_seen);   /* VtolLiftOff: a clock left by another branch or life */
    c->vt_slow_seen = w->time;
    if (!parked) { c->vt_slow_t = 0.0; return 0; }
    if (stale) c7_add("vtliftoff", "park_stale", 1);
    if (c->vt_slow_t <= 0.0 || w->time < c->vt_slow_t || stale) { c->vt_slow_t = w->time; return 0; }
    if (w->time - c->vt_slow_t <= 1.5) return 0;
    c->vt_moveto_until = w->time + 4.0; c->vt_slow_t = 0.0; c7_add("vtliftoff", "park_windows", 1);
    return 1;
}
/* VTOL slow time (measured, either switch state; Worker E 11-01): vtclimb slow_s -- under 3 m/s on a travel leg 50 m+ from a steady
 * goal -- read 16-18 % of fly time in Worker C's runs after 3812551 (5-14 % before). The same exe and method gave 6.6-11.9 % (4 runs),
 * and runs before 3812551 ranged 1.5-20.5 %: it follows the match (how much of the VTOL time is fighting and turning). Broken down
 * (wE3, 4 runs, 80 s of it): VtolArrive's brake / sink, VtolClimbUp and the edge recovery / brake acted in 1-13 % per run (5 % in
 * all), and in the three runs with 19-38 s of it 78-90 % fell on legs with a stop over 5 m, where VtolArrive never acts; most was
 * engage slides from a firing hover and turning on the spot (the stick gives no thrust 1.2 rad+ off the nose; ~70 deg/s at full
 * yaw: up to 2.1 s at 0 m/s after a claw). Counted per slow step: the branch that flew the craft the step before
 * (S stick, M MoveTo, W MoveTo window, A awake drive, H cargo hover, P pad run), what acted on it (bits: 1 VtolClimbUp, 2 VtolArrive
 * brake, 4 its sink, 8 the edge recovery, 16 the edge brake), the order, a rider, the edge, how long since it last went 10 m/s.
 * [C7] vtslow; [BOT-VSLOW] (DIAG) for a spell of 1 s or more. */
static void vt_slow_note(int index, Bot *b, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal, float stop, float d)
{
    float sp = hypotf(me->vel.x, me->vel.z);
    if (sp > 10.f || c->vs_fast_t <= 0.0 || w->time < c->vs_fast_t) c->vs_fast_t = w->time;
    if (c->vs_t0 > 0.0 && (w->time - c->vs_last > 0.3 || w->time < c->vs_last)) {   /* the spell is over */
        if (c->vs_last - c->vs_t0 >= 1.0 && env_on("RECOMP_BOT_DIAG")) {
            char bs[8]; int n = 0; static const char k_br[7] = "SMWAHP";
            for (int i = 0; i < 6; i++) if (c->vs_sbr & (1 << i)) bs[n++] = k_br[i];
            if (c->vs_sbr & 64) bs[n++] = '-';
            bs[n] = 0;
            fprintf(stderr, "[BOT-VSLOW] t=%.1f-%.1f bot=%d %.1f s ord=%d cargo=%d stop=%.0f d=%.0f rider=%d branch=%s acted=%s%s%s%s%s\n", c->vs_t0, c->vs_last, index,
                    c->vs_last - c->vs_t0, c->vs_sord, c->vs_scargo, c->vs_sstop, c->vs_sd, c->vs_srider, bs, (c->vs_sfl & 1) ? "climbup," : "", (c->vs_sfl & 2) ? "brake," : "",
                    (c->vs_sfl & 4) ? "sink," : "", (c->vs_sfl & 8) ? "edgerec," : "", (c->vs_sfl & 16) ? "edgebrake," : "");
        }
        c->vs_t0 = 0.0;
    }
    if (c->vs_dt <= 0.f) return;
    double dt = c->vs_dt; c->vs_dt = 0.f;
    const Ma2Order *o = &b->order;
    char br = c->va_bprev; int fl = c->vs_fprev, bi = br == 'S' ? 0 : br == 'M' ? 1 : br == 'W' ? 2 : br == 'A' ? 3 : br == 'H' ? 4 : br == 'P' ? 5 : 6;
    static const char *const k_bk[7] = { "br_stick_s", "br_moveto_s", "br_window_s", "br_awake_s", "br_hover_s", "br_pad_s", "br_other_s" };
    c7_add("vtslow", "slow_s", dt); c7_add("vtslow", k_bk[bi], dt);
    if (fl & 1) c7_add("vtslow", "climbup_s", dt);
    if (fl & 2) c7_add("vtslow", "brake_s", dt);
    if (fl & 4) c7_add("vtslow", "sink_s", dt);
    if (fl & 8) c7_add("vtslow", "edgerec_s", dt);
    if (fl & 16) c7_add("vtslow", "edgebrake_s", dt);
    if (c->vt_rider) c7_add("vtslow", "rider_s", dt);
    if (stop <= 5.f) c7_add("vtslow", "stop5_s", dt);
    if (ef_ready() && ef_dist(me->pos.x, me->pos.z) < 40.f) c7_add("vtslow", "edge40_s", dt);
    { int okg = 0; float g = terrain_height(me->pos.x, me->pos.z, &okg); if (okg && me->pos.y - g < 8.f) c7_add("vtslow", "low_s", dt); }
    {   /* turning: the point it steers at more than 1.2 rad off the nose (vtol_manual gives no thrust there) */
        uint32_t node = MEM32(me->actor + 0x94);
        if (gva_ok(node, 0x100)) {
            float err = atan2f(c->dest.x - me->pos.x, c->dest.z - me->pos.z) - atan2f(MEMF(node + 0xE0), MEMF(node + 0xE8));
            while (err > 3.14159265f) err -= 6.2831853f;
            while (err < -3.14159265f) err += 6.2831853f;
            if (fabsf(err) > 1.2f) c7_add("vtslow", "turn_s", dt);
        }
    }
    if (w->time < c->vt_moveto_until) c7_add("vtslow", "window_s", dt);
    c7_add("vtslow", o->type == MA2_ORD_TRAVEL ? (o->cargo ? "ord_cargo_s" : o->vt_heal ? "ord_heal_s" : "ord_travel_s") : o->type == MA2_ORD_ENGAGE ? "ord_engage_s" :
                     o->type == MA2_ORD_FIGHT_MOVE ? "ord_fight_s" : "ord_other_s", dt);
    c7_add("vtslow", w->time - c->vs_fast_t > 5.0 ? "parked_s" : "dip_s", dt);   /* sat slow 5 s+, or a dip in a run */
    if (c->vs_t0 <= 0.0) { c->vs_t0 = w->time; c->vs_sfl = 0; c->vs_sbr = 0; c->vs_sord = o->type; c->vs_scargo = o->cargo; c->vs_sstop = stop; c->vs_sd = d; c->vs_srider = c->vt_rider; }
    c->vs_last = w->time; c->vs_sfl |= fl; c->vs_sbr |= 1 << bi;
}
/* ArriveSpread (default on, RECOMP_BOT_ARRIVE_SPREAD / [Bots] ArriveSpread): WP3 fix round (11-02), sweep r3sw70 Check It! Deliverance: the brain sent three pilots to the
 * same objective point (529,1236, stop 2 m) and all three arrived and stood 2-3 m apart for 6-8 s (five bunch-ups in that one cell, one in Phase 1). Players want no
 * contention: a TRAVEL arrival that finds a teammate of its own kind already standing at the spot (nearer the goal, or the same distance and the lower player id)
 * walks to a stand-off point 6.3 m from every standing teammate, on the same ground within 3 m of its own height, on a ring 5-9 m round the goal
 * (nearest to where it is), and holds there while the holder stays; it goes in again as soon as the spot is empty. [C7] arrivespread: n / hold_s / released / none. */
static int g_spread_ctx;
static int spread_count(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 p, float r, Ma2Vec3 goal, int *closer)
{
    int n = 0; float myd = hdist2(me->pos, goal);
    if (closer) *closer = 0;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *o = &w->players[i];
        if (o->actor == me->actor || !o->alive || o->nation != me->nation || o->nation <= 0 || o->kind != me->kind || o->riding || o->clamped_to) continue;
        if (hypotf(o->vel.x, o->vel.z) > 2.5f || fabsf(o->pos.y - me->pos.y) > 5.f || hdist2(o->pos, p) > r) continue;
        n++;
        if (closer) { float od = hdist2(o->pos, goal); if (od < myd - 0.3f || (od <= myd + 0.3f && o->player < me->player)) (*closer)++; }
    }
    return n;
}
static int arrive_spread(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal, float stop, float d)
{
    static const float sep = 6.3f;
    if (!arrive_spread_on() || !ma2_trav_ready() || me->kind == MA2_KIND_VTOL || me->kind == MA2_KIND_FLYER) return 0;
    int closer = 0;
    if (c->as_until > 0.0) {
        int near_goal = spread_count(w, me, goal, maxf2(stop, 1.5f) + 4.f, goal, &closer);
        if (w->time > c->as_until || w->time < c->as_until - 25.0 || hdist2(goal, c->as_goal) > 6.f || near_goal == 0) {
            c7_add("arrivespread", w->time > c->as_until ? "released_time" : hdist2(goal, c->as_goal) > 6.f ? "released_goal" : "released_empty", 1); c->as_until = 0.0; return 0;
        }
        if (hdist2(me->pos, c->as_pt) > 1.2f) { if (me->kind == MA2_KIND_PILOT) body_walk_direct(me->actor, me->pos, c->as_pt, 100); else body_steer(me, c->as_pt);   /* MoveTo's avoidance will not walk a pilot off a teammate's side (sw70: 7 s at speed 0, M304) */
            c->dest = c->as_pt; c->mode = CM_MOVE; c->arrived = 0; c->have_path = 0; return -1; }   /* -1: walking to the spot, this step is done */
        body_stop(me->actor); c->mode = CM_STOP; c->arrived = 1; c7_add("arrivespread", "hold_s", w->dt);
        return 1;
    }
    if (d > maxf2(stop, 1.5f)) return 0;
    if (!spread_count(w, me, me->pos, sep, goal, &closer) || !closer) return 0;
    float best = 1e9f; int found = 0; Ma2Vec3 pick = goal;
    for (int k = 0; k < 16; k++) {
        float a = (float)k * 0.3927f + (float)(me->player & 7u) * 0.05f;
        for (int ri = 0; ri < 3; ri++) {
            float r = (ri == 0 ? 5.f : ri == 1 ? 7.f : 9.f);
            Ma2Vec3 q = { goal.x + sinf(a) * r, me->pos.y, goal.z + cosf(a) * r };
            int oks = 0; float sf = surface_height(q.x, q.z, &oks);
            if (!oks || fabsf(sf - me->pos.y) > 3.f || ma2_trav_clear(q.x, q.z, w->time) <= 1.5f) continue;
            if (spread_count(w, me, q, sep, goal, NULL)) continue;
            float sc = hdist2(me->pos, q) + 0.5f * r;
            if (sc < best) { best = sc; pick = q; pick.y = sf; found = 1; }
        }
    }
    if (!found) { c7_add("arrivespread", "none", 1); return 0; }
    c->as_pt = pick; c->as_goal = goal; c->as_until = w->time + 20.0; c->as_n++;
    c7_add("arrivespread", "n", 1);
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-SPREAD] t=%.1f kind=%d at %.0f,%.0f stands off to %.0f,%.0f (goal %.0f,%.0f, %d holding)\n", w->time, me->kind, me->pos.x, me->pos.z, pick.x, pick.z, goal.x, goal.z, closer);
    return 0;
}
/* FrozenPilot (default on, RECOMP_BOT_FROZEN_PILOT / [Bots] FrozenPilot): WP3 fix round, sweep r3sw33: LMS Proving Ground pilot bot 0 landed from a ledge drop at
 * 741,144,435 (hp 10/10, order TRAVEL) and then stood on the same 0.1 m for 82 s with MoveTo driving it every frame (drive M304, ai mode 6, route 10 m of open
 * ground, rays clear, speed 0.000, comp velocity 0); the 6 s stall ticked 18 times (backtrack to a crumb 30 m above, wall sweep, re-plan) and nothing moved it,
 * PocketEscape / EdgeStep never fired (the ledge is a strip narrower than their clearance test, not a "raised top"). AiKick now covers pilots too (the 0.5 s yield
 * that freed the walkers). A pilot on a travel leg (15 m+ from its goal) that has not moved 0.5 m in 3 s then wiggles: 1.1 s of straight own-drive walking
 * (body_walk_direct, not MoveTo) toward its carrot, then round the other seven headings; if that moves it 1.5 m it is free (sweep r4A-sw33: 5 of 5 freed within 0.3 s
 * each, a pilot MoveTo cannot move on a 205 m ledge top) and keeps its own drive toward the carrot for 4 s. If it still does not, it is set down 4 m on along its own
 * planned route (or at its last walked crumb), the way the game's own teleport sets a body down (position copies in the body component and the actor). Bounded:
 * 3 sets per stand. [C7] frozenpilot: spells / wiggle_s / freed / teleports / no_target. */
static void body_teleport(uint32_t a, Ma2Vec3 p)
{
    uint32_t comp = MEM32(a + 0x8C);
    if (!gva_ok(comp, 0x180)) return;
    static const uint32_t pos3[3] = { 0x08, 0x44, 0x16C };
    for (int i = 0; i < 3; i++) { MEMF(comp + pos3[i]) = p.x; MEMF(comp + pos3[i] + 4) = p.y; MEMF(comp + pos3[i] + 8) = p.z; }
    MEMF(comp + 0x24) = p.y - 0.032f; MEMF(comp + 0x60) = p.y - 0.032f;
    MEMF(comp + 0x14) = 0.f; MEMF(comp + 0x1C) = 0.f; MEMF(comp + 0x20) = 0.f; MEMF(comp + 0x50) = 0.f; MEMF(comp + 0x58) = 0.f; MEMF(comp + 0x5C) = 0.f;
    MEMF(a + 0x4C0) = p.x; MEMF(a + 0x4C4) = p.y; MEMF(a + 0x4C8) = p.z;
}
static int frozen_tele_pt(const BotCtrl *c, const Ma2PlayerView *me, Ma2Vec3 *out)
{
    if (c->have_path && c->path.n > 0) {
        for (int k = 0; k < c->path.n && k < 8; k++) {
            Ma2Vec3 q = c->path.pt[k]; float l = hdist2(q, me->pos);
            if (l < 3.f || fabsf(q.y - me->pos.y) > 4.f) continue;
            float f = l > 4.f ? 4.f / l : 1.f;
            out->x = me->pos.x + (q.x - me->pos.x) * f; out->z = me->pos.z + (q.z - me->pos.z) * f; out->y = q.y + 0.3f;
            return 1;
        }
    }
    for (int k = 1; k <= 8 && k <= c->ncrumb; k++) {
        Ma2Vec3 q = c->crumb[(c->crumb_i - k + 64) % 8];
        if (hdist2(q, me->pos) < 3.f) continue;
        *out = q; out->y += 0.3f; return 1;
    }
    return 0;
}
static int ba_dip_hop_on(void);
static int g_ts_bot = -1; static float g_ts_gx, g_ts_gz;   /* RECOMP_TEST_BOT_SET: the bot that was set down, and the far goal it is sent to ("<s>,<x>,<z>,<kind>,<gx>,<gz>") */
/* LeapNotWalk (default on, RECOMP_BOT_LEAP_NOT_WALK / [Bots] LeapNotWalk; parity pass 9, 11-11): the goto will not walk a jet body down the slope
 * to a leap's top (it turns on the spot with no forward speed -- LeapStall's note), and GotoWalk's direct walk starts after 0.3-1 s, before
 * LeapStall's own 0.4 s clock can run (GotoWalk returns first): T9_81a1 b6 (Ill Wind Snatch It T2) stood / walked / stood at 1168 -> 1160 -> 1154,
 * 1502 from 10.0 to 12.6 s (18 m from its leap top 1150,1504, walks at 10.1 and 11.6) and jetted at 12.6. When GotoWalk would walk a standing
 * jet body whose route leaps DOWN within the next 3 corners, from a top 6-24 m off (landing 5 m+ lower, < 60 m off, nothing rising 2 m over
 * the body between), the leg becomes the leap from here instead (as LeapStall does) and the jet fires this frame. [C7] leapstall: notwalk. */
static int leap_not_walk(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    if (!c->have_path || c->path.n < 2 || !leap_stall_on() || !(me->kind == MA2_KIND_BATTLEARMOR || (me->kind == MA2_KIND_MECH && ma2_bot_has_jets(me->actor))) ||
        hypotf(me->vel.x, me->vel.z) > 1.f || !c7_on("leapnotwalk", "RECOMP_BOT_LEAP_NOT_WALK", "LeapNotWalk", 1)) return 0;
    int lj = -1;
    for (int j = c->path.next > 0 ? c->path.next : 1; j < c->path.n && j <= c->path.next + 3; j++)
        if (c->path.drop[j] == 1 || c->path.drop[j] == 2) { lj = j; break; }
    if (lj <= 0) return 0;
    Ma2Vec3 top = c->path.pt[lj - 1], land = c->path.pt[lj];
    int okl = 0; float lh = terrain_height(land.x, land.z, &okl); if (okl) land.y = lh;
    float dtop = hdist2(me->pos, top);
    /* a jet-up leg too (LeapStallUp's test): landing 5-22 m higher, < 45 m off, 6-14 m from the foot (LeapStallUpFar: < 52 m off, < 24 m from the
     * foot -- pass 11: a body landed 21 m from the foot was walked in first, the walk blocking LeapStall: P13_81b2 b6 41.27 -> jet 43.20) */
    int upfar = leap_stall_up_far_on();
    int up = c->path.drop[lj] == 2 && land.y > me->pos.y + 5.f && land.y < me->pos.y + 22.f && hdist2(me->pos, land) < (upfar ? 52.f : 45.f) && dtop >= 6.f && dtop < (upfar ? 24.f : 14.f) &&
             c7_on("leapstallup", "RECOMP_BOT_LEAP_STALL_UP", "LeapStallUp", 1);
    if (!up && (dtop < 6.f || dtop >= 24.f || !(land.y < me->pos.y - 5.f) || hdist2(me->pos, land) >= 60.f)) return 0;
    int okg = 0; float gh0 = surface_height(me->pos.x, me->pos.z, &okg);
    if (okg && me->pos.y - gh0 >= 1.5f) return 0;
    int okt = 0; float top_g = surface_height(top.x, top.z, &okt); if (!okt) top_g = me->pos.y;
    for (float s = 2.f; s < dtop; s += 2.f) {
        int oks = 0; float px = me->pos.x + (top.x - me->pos.x) * s / dtop, pz = me->pos.z + (top.z - me->pos.z) * s / dtop;
        float sh = surface_height(px, pz, &oks);
        if (oks && sh > (up ? maxf2(me->pos.y, top_g) : me->pos.y) + 2.f) return 0;
    }
    if (up) c7_add("leapstall", "notwalk_up", 1);
    c->path.pt[lj - 1] = me->pos; c->path.drop[lj] = 2; c->path.next = lj;
    c->ls_t = w->time; c->ls_best = 0.f; c7_add("leapstall", "notwalk", 1);
    if (c->jump_next > w->time) c->jump_next = w->time;
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-LEAPSTALL] t=%.1f kind=%d at %.0f,%.0f,%.0f %.0f m short of the leap top %.0f,%.0f: leaps from here (not walked) to %.0f,%.0f,%.0f\n",
                                         w->time, me->kind, me->pos.x, me->pos.y, me->pos.z, dtop, top.x, top.z, land.x, land.y, land.z);
    return 1;
}
/* LandedLeg (default on, RECOMP_BOT_LANDED_LEG / [Bots] LandedLeg; parity pass 11, 11-11): a jet leg the body has flown stayed the route's next leg
 * after the landing when the flight came down a few metres off its planned landing point -- the foot behind (F38 needs 8 m), the landing at
 * the body's own level (LeapStall(Up) sees no rise), so nothing jetted on and the goto walked the body to the old landing point first.
 * P13_81b2 b6 (Ill Wind Snatch It T2) landed at 729,0.7,1401 at 40.97 with path 1/6 = the basin jet leg to 722,1388 (13 m on, same level);
 * stood, walked to it, replanned 'off' and jetted up the band only at 43.20 (P13_81b2 b9 32.3 -> 33.74, P14_81a1 b5 33.7 -> 36.17).
 * On the ground (no burn, |vy| < 1, < 1.5 m over the surface), the next leg a jet leg whose landing is < 16 m off and within -2..+3 m of the body's height,
 * 8 m+ from its foot: that leg is done -- the next corner is steered for, and its jet (LeapStallUp, LeapNotWalk, F38) can follow. [C7] landedleg: n. */
static void landed_leg(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    if (!c->have_path || c->path.next <= 0 || c->path.next >= c->path.n - 1 || c->path.drop[c->path.next] != 2 || fabsf(me->vel.y) >= 1.f ||
        !(me->kind == MA2_KIND_BATTLEARMOR || (me->kind == MA2_KIND_MECH && ma2_bot_has_jets(me->actor)))) return;
    uint32_t lai = actor_ai(me->actor);
    if (lai && MEM8(lai + 0x240)) return;
    Ma2Vec3 land = c->path.pt[c->path.next], foot = c->path.pt[c->path.next - 1];
    if (hdist2(me->pos, land) >= 16.f || hdist2(me->pos, foot) < 8.f) return;
    int okl = 0; float ly = terrain_height(land.x, land.z, &okl); if (!okl) ly = land.y;
    if (me->pos.y <= ly - 2.f || me->pos.y >= ly + 3.f) return;   /* at the landing's level: a body come down above it still has the drop to make */
    int okg = 0; float gh = surface_height(me->pos.x, me->pos.z, &okg);
    if (okg && me->pos.y - gh >= 1.5f) return;
    if (!c7_on("landedleg", "RECOMP_BOT_LANDED_LEG", "LandedLeg", 1)) return;
    c->path.next++; c->lw_until = 0.0; c->lw_t = 0.0; c7_add("landedleg", "n", 1);
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-LANDEDLEG] t=%.1f bot=%d kind=%d landed at %.0f,%.1f,%.0f, %.0f m from the leg's landing %.0f,%.1f,%.0f: next corner %d/%d\n",
                                         w->time, index, me->kind, me->pos.x, me->pos.y, me->pos.z, hdist2(me->pos, land), land.x, ly, land.z, c->path.next, c->path.n);
}

static int ctrl_travel(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 goal, float stop, uint32_t ignore_veh, float overshoot)
{
    BotCtrl *c = &b->ctrl;
    if (index == g_ts_bot) { goal.x = g_ts_gx; goal.z = g_ts_gz; }
    float vkeep = 25.f;                                                      /* VtolEdgeAll: this craft's keep-off (by its height) */
    if (me->kind == MA2_KIND_VTOL && vt_edge_all_on() && ef_ready()) {
        int okg = 0; float g = terrain_height(me->pos.x, me->pos.z, &okg);
        vkeep = vt_keep_m(okg ? me->pos.y - g : 0.f);
        if (g_vkeep_cap > 0.f && vkeep > g_vkeep_cap) vkeep = g_vkeep_cap;   /* the run in to a hover point (ORD_TRAVEL cargo) */
        if (vt_keep_in(&goal, vkeep) > 0) c7_add("vtedgeall", "goal_in_s", w->dt);   /* a hover, cover, muster or drop point by the edge */
    }
    float d = hdist2(me->pos, goal);
    /* Pilot bombs: leaving one's blast is ctrl_bomb_escape's (every mode, after this); while it runs, no route. */
    if (c->bav_until > 0.0) { c->dest = c->bav_pt; c->mode = CM_MOVE; c->have_path = 0; return 0; }
    if (g_spread_ctx) { int sp = arrive_spread(c, w, me, goal, stop, d); if (sp > 0) return 1; if (sp < 0) return 0; }
    if (d <= maxf2(stop, 1.5f) && !((me->kind == MA2_KIND_BATTLEARMOR || me->kind == MA2_KIND_MECH) && goal_rise(goal, me) > 3.f) &&
        !(me->kind == MA2_KIND_VTOL && goal.y > me->pos.y + 4.f && vt_climb_up_on())) {   /* VtolClimbUp: a point overhead is not "there" either */ body_stop(me->actor); c->mode = CM_STOP; c->arrived = 1; return 1; }   /* a roof spot overhead is not "there" */
    c->arrived = 0;
    int air = me->kind == MA2_KIND_VTOL;
    /* EjectStep (default on, RECOMP_BOT_EJECT_STEP / [Bots] EjectStep): a pilot just out of a vehicle stands inside the hull it left (2.6-5.9 s,
     * 24 swap-ejects in au19_211 Rush Hour; kicks change nothing for a pilot, and five gave up the 'Mech 12-14 m off when EjectGrace ran out).
     * In its first 0.5 s on foot with a hull within 4 m it walks straight out (pilot_escape, 7 m), once per body. [C7] ejectstep: n. */
    if (me->kind == MA2_KIND_PILOT && w->time - c->actor_since < 0.5 && c->ejs_actor != me->actor && c7_on("ejectstep", "RECOMP_BOT_EJECT_STEP", "EjectStep", 1)) {
        c->ejs_actor = me->actor;
        if (pilot_escape(c, w, me, 4.f)) c7_add("ejectstep", "n", 1);
    }
    if (me->kind == MA2_KIND_PILOT && w->time < c->esc_until) {
        body_walk_direct(me->actor, me->pos, c->esc_pt, 100); c->dest = c->esc_pt; c->mode = CM_MOVE;
        return 0;
    }
    /* Jetting up onto a roof or ledge: steer at the spot itself until we land (the walk planner
     * cannot put a waypoint up there; its nearest point is on the ground beside the building). */
    if (c->upjet_until > 0.0 && w->time >= c->upjet_until) {
        /* The jump is over and we are not up there: that top cannot be stood on (a sloped or
         * fenced roof, out of reach) -- the brain stops picking it. */
        float top;
        if (!(on_raised(me, &top) && hdist2(me->pos, c->upjet_goal) < 8.f)) {
            b->status.roof_failed = 1; b->status.roof_pt = c->upjet_goal;
            c->upfail_pt = me->pos; c->upfail_t = w->time;
        }
        c->upjet_until = 0.0;
    }
    if (!air && w->time < c->upjet_until) {
        float top;
        if (on_raised(me, &top) && hdist2(me->pos, goal) < 6.f) c->upjet_until = 0.0;   /* up there */
        else { body_steer(me, goal); c->dest = goal; c->mode = CM_MOVE; return 0; }
    }
    /* Up on a roof or ledge the walk planner sees only an obstacle. A goal on the same top is walked
     * to straight; a goal on the ground is walked toward straight off the edge on its side (a 'Mech
     * drops behind the building, out of its attacker's sight); another high spot is jetted to. */
    {
        float top;
        if (!air && w->time >= c->rz_off_until && on_raised(me, &top)) {
            int okg = 0;
            float sg = surface_height(goal.x, goal.z, &okg);
            int same = okg && fabsf(sg - top) < 1.5f, raised_goal = okg && !same && sg > goal.y - 1.f &&
                       sg > terrain_height(goal.x, goal.z, &okg) + 3.f;
            if (raised_goal && (me->kind == MA2_KIND_BATTLEARMOR || me->kind == MA2_KIND_MECH) && up_allowed(c, w, me) &&
                (me->kind != MA2_KIND_MECH || !hot_upjet_hold(c, w, me)) &&
                body_jump_up(me->actor, me->pos, goal, me->kind == MA2_KIND_BATTLEARMOR ? 1.2f : 1.9f)) { c->jumps++; c->upjet_until = w->time + 4.0; c->upjets++; c->upjet_goal = goal; }
            /* RaisedGuard (default on, RECOMP_BOT_RAISED_GUARD / [Bots] RaisedGuard): this straight walk returns before the progress watchdog below, so a body
             * that cannot walk it (a rail, a cliff, a stair it cannot take) was never counted stalled: sw33 Proving Ground pilots stood 20-43 s on the
             * 174-205 m ledges with stalls=0 (sweep WP2 11-02, the "stall detector never fires" of 18 spans). 4 s without 3 m: a stall (status.stalled, the
             * unseen-obstacle learning) and the planner's route for 12 s instead of the straight line. [C7] raisedguard: stalls / planner_s. */
            if (raised_guard_on()) {
                if (c->rz_t <= 0.0 || w->time < c->rz_t || hdist2(me->pos, c->rz_pos) > 3.f) { c->rz_pos = me->pos; c->rz_t = w->time; if (hdist2(me->pos, c->rz_pos0) > 12.f) c->rz_n = 0; }
                else if (w->time - c->rz_t > 4.0 && hdist2(me->pos, goal) > 3.f) {
                    c->rz_t = w->time; c->rz_pos = me->pos; c->rz_pos0 = me->pos; c->rz_n++; c->rz_off_until = w->time + 12.0; c->n_stalls++;
                    if (c->rz_n >= 2) b->status.stalled = 1;
                    if (c->rz_n >= 2 && me->kind == MA2_KIND_PILOT && edge_step_on()) {
                        Ma2Vec3 ep2;
                        if (edge_step_pt(me, goal, &ep2)) {
                            c->esc_pt = ep2; c->esc_until = w->time + 3.5; c->escapes++; c7_add("edgestep", "n", 1);
                            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-EDGESTEP] t=%.1f bot=%d on the top at %.0f,%.0f,%.0f steps off toward %.0f,%.0f\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, ep2.x, ep2.z);
                        }
                    }
                    c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
                    c7_add("raisedguard", "stalls", 1);
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-RAISED] t=%.1f bot=%d kind=%d no headway on the top at %.0f,%.0f,%.0f toward %.0f,%.0f (%d): the planner for 12 s\n", w->time, index, me->kind, me->pos.x, me->pos.y, me->pos.z, goal.x, goal.z, c->rz_n);
                }
            }
            body_steer(me, goal); c->dest = goal; c->mode = CM_MOVE; c->have_path = 0;
            return 0;
        }
    }
    if (!air) landed_leg(index, c, w, me);
    /* Breadcrumbs: where we actually got to, every few metres. */
    if (c->ncrumb == 0 || (hdist2(me->pos, c->crumb[(c->crumb_i + 7) % 8]) > 6.f && w->time - c->crumb_t > 1.0)) {
        c->crumb[c->crumb_i] = me->pos; c->crumb_i = (c->crumb_i + 1) % 8; if (c->ncrumb < 8) c->ncrumb++; c->crumb_t = w->time;
    }
    /* GotoWalk (default on, RECOMP_BOT_GOTO_WALK / [Bots] GotoWalk): the campaign MechAI goto (mode 6, awake) sometimes turns a walker to its
     * carrot in place (anim 18) and then never walks (anim 0, forward speed comp+0x98 0) -- the "stands 6-22 s turning to each new carrot" of
     * sweep WP2, which AiKick only shakes loose (on the second kick, often). 11-04 audit: au3_20 bot 3 (TD Bogged Down) came down from a
     * travel hop on a mound at 327.6,5.1,1766.8 and stood 7 s (t=15.3-22.9) facing its carrot 20 m off and 9-17 m lower, through an AiKick
     * and a 4 s backtrack; only a jet freed it. au3_60 bot 4 (CTF Bogged Down) stood t=126.2-133.0 on level ground at 871.9,4.1,1596.5
     * facing its carrot 20-30 m off. A walker still 1 s, facing (25 deg) what it steers at 8 m+ off, walks there on the body's own turn +
     * speed (no navigator) for up to 2.5 s, ending once it has moved 6 m or come down 2 m; then the planner again. Only while the navigator
     * itself gives no forward speed (comp+0x98 0). [C7] gotowalk: n / moved / failed. */
    /* TankPivot (default on, RECOMP_BOT_TANK_PIVOT / [Bots] TankPivot): a stopped tank whose carrot swung 30 deg+ off its nose sat there --
     * MoveTo gave no speed (comp+0x98 0) and its turn rate is speed / comp+0xD4: au5_91 bot 2 (Base War Deliverance, 11-04) stood 18 s at
     * 1422.6,7.5,369.6 (t=105-123), nose -57 deg, carrot -148 deg, until the trapped bail-out. Still 1 s with the carrot 8 m+ off: the hull
     * turns on the spot to it (up to 3 s, till within 15 deg), then MoveTo again. [C7] tankpivot: n / faced / failed. */
    if (me->kind == MA2_KIND_TANK && !air && c7_on("tankpivot", "RECOMP_BOT_TANK_PIVOT", "TankPivot", 1)) {
        if (c->tp_until > 0.0) {
            float off = fabsf(wrap_pi(atan2f(c->tp_pt.x - me->pos.x, c->tp_pt.z - me->pos.z) - me->yaw));
            if (off < 0.26f) { c7_add("tankpivot", "faced", 1); c->tp_until = 0.0; c->tp_t = 0.0; }
            else if (w->time > c->tp_until || w->time < c->tp_until - 4.0) { c7_add("tankpivot", "failed", 1); c->tp_until = 0.0; c->tp_t = 0.0; }
            else { body_turn_in_place(me->actor, me->pos, c->tp_pt); c->dest = c->tp_pt; c->mode = CM_MOVE; return 0; }
        } else if (hdist2(me->pos, goal) > 10.f && gva_ok(MEM32(me->actor + 0x8C), 0x150) && MEMF(MEM32(me->actor + 0x8C) + 0x98) < 0.5f) {
            Ma2Vec3 t = c->dest;
            float off = fabsf(wrap_pi(atan2f(t.x - me->pos.x, t.z - me->pos.z) - me->yaw));
            if (c->tp_t <= 0.0 || w->time < c->tp_t || hdist2(me->pos, c->tp_pos) > 0.7f || hdist2(me->pos, t) < 8.f || off < 0.52f) { c->tp_pos = me->pos; c->tp_t = w->time; }
            else if (w->time - c->tp_t > 1.0) {
                c->tp_pt = t; c->tp_until = w->time + 3.0; c->tp_t = 0.0; c7_add("tankpivot", "n", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-TANKPIVOT] t=%.1f bot=%d still at %.1f,%.1f,%.1f, carrot %.0f deg off: turning on the spot\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, off * 57.3f);
                body_turn_in_place(me->actor, me->pos, t); c->mode = CM_MOVE;
                return 0;
            }
        }
    }
    if ((me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && !air && c7_on("gotowalk", "RECOMP_BOT_GOTO_WALK", "GotoWalk", 1)) {
        uint32_t lai = actor_ai(me->actor);
        /* GotoWindow (default on, RECOMP_BOT_GOTO_WINDOW / [Bots] GotoWindow; parity pass 7, 11-11): the goto can also stutter -- 0.3-1 s bursts
         * of > 0.7 m, then 0.5-1 s stood turning to the next carrot -- and every burst restarted the 1 s still-timer: Q_81a1 b6 (Ill Wind Snatch
         * It team 2, just boarded) went 1199 -> 1189,1420 in 10.0-14.4 s (2.4 m/s, mech cruise 16) with drive A348 before the walk took over.
         * Judge headway over a 2 s window instead: standing 0.3 s+, carrot 8 m+ off, and under a quarter of the class cruise (0.5 x cruise m)
         * made good in the last 2 s -> the direct walk (turning first when the carrot is off the nose). Positions every 0.25 s on the ground;
         * a failed walk clears them. [C7] gotowin: n. */
        int gwin_on = c7_on("gotowin", "RECOMP_BOT_GOTO_WINDOW", "GotoWindow", 1), gwin = 0;
        if (gwin_on) {
            int last = (c->gw_hi + 11) % 12;
            if (c->gw_ht[last] > w->time) { memset(c->gw_ht, 0, sizeof c->gw_ht); c->gw_st = 0.0; }   /* a new life / match: time went back */
            if (c->gw_ht[last] <= 0.0 || w->time - c->gw_ht[last] >= 0.25) { c->gw_hp[c->gw_hi] = me->pos; c->gw_ht[c->gw_hi] = w->time; c->gw_hi = (c->gw_hi + 1) % 12; }
            if (c->gw_st <= 0.0 || hdist2(me->pos, c->gw_spos) > 0.7f) { c->gw_spos = me->pos; c->gw_st = w->time; }
        }
        if (c->lw_until > 0.0) {
            if (me->pos.y < c->lw_y0 - 2.f || hdist2(me->pos, c->lw_p0) > 6.f) {
                /* GotoChain (default on, RECOMP_BOT_GOTO_CHAIN / [Bots] GotoChain): a walk that moved threw the route away; the fresh plan's
                 * 14 m BattleArmor carrot stood the goto ~1.1 s and the walk fired again -- r4_14 (all-BA Destruction): 74 fires in 15 chains
                 * of up to 4, 124 s stood (b4 66-80 401 -> 207 hp, b0 retreating 172-182). Keep the route unless the walk left it (> 12 m from
                 * its next point), and for 4 s after a walk re-arm at 0.3 s instead of 1 s. [C7] gotowalk: kept_route. */
                int chain = c7_on("gotochain", "RECOMP_BOT_GOTO_CHAIN", "GotoChain", 1);
                int off_route = !c->have_path || c->path.n <= 0 || c->path.next >= c->path.n;
                if (!off_route) {   /* on it: within 12 m of the current leg, or 15 m of any point still ahead (r5_13: the next-corner test kept 15 of 71) */
                    float best = 1e9f;
                    for (int q = c->path.next; q < c->path.n; q++) { float dq = hdist2(me->pos, c->path.pt[q]); if (dq < best) best = dq; }
                    /* FirstLeg (default on, RECOMP_BOT_FIRST_LEG / [Bots] FirstLeg; parity pass 11, 11-11): with path.next == 0 the test saw only corner
                     * distances, so a long string-pulled first leg (corner 20-130 m on) read as 'off route' after every walk and the route was thrown
                     * away and replanned, the goto standing the body again: B11_84a1 b6 (On the Rocks Snatch It T2) down the base's 11 m slope
                     * 1526,62.7,1485 -> 1510,53.9,1478 at 30.2-32.8 s, four 0.6-1 s walks each followed by a 0.1-0.6 s stand (firstleg_proof.py: 15
                     * such T2 / 3 T1 hand-backs in that match, all on their first leg). The first leg runs from where this walk began (lw_p0). [C7] firstleg: kept. */
                    int fl = c->path.next == 0 && c7_on("firstleg", "RECOMP_BOT_FIRST_LEG", "FirstLeg", 1);
                    if (c->path.next > 0 || fl) {
                        Ma2Vec3 a = c->path.next > 0 ? c->path.pt[c->path.next - 1] : c->lw_p0, bq = c->path.pt[c->path.next];
                        float sx = bq.x - a.x, sz = bq.z - a.z, l2 = sx * sx + sz * sz;
                        float u = l2 > 0.01f ? ((me->pos.x - a.x) * sx + (me->pos.z - a.z) * sz) / l2 : 0.f;
                        u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
                        float dx = me->pos.x - (a.x + sx * u), dz = me->pos.z - (a.z + sz * u), ds = sqrtf(dx * dx + dz * dz);
                        if (ds < 12.f) { if (fl && best > 15.f) c7_add("firstleg", "kept", 1); best = 0.f; }
                    }
                    off_route = best > 15.f;
                }
                /* WalkChainHold (default on, RECOMP_BOT_WALK_CHAIN_HOLD / [Bots] WalkChainHold; parity pass 10, 11-11): a walk that moved while the
                 * one before it had moved < 4 s ago (the goto stood the body again in between) hands back to the goto, which stands it a third time:
                 * X9_81b1 b10 (Ill Wind Snatch It T2) on the basin mounds 26.6-29.5 s at 862 -> 849,1427, the goto latching a turn target 90 deg off
                 * the route (comp+0xA0 170 vs bearing -100) on every hand-back, 0.3-0.7 s stood each time; 20-26 such chain stalls per 90 s per match,
                 * both teams (walkchain.py). In a chain, on the route, moving, with the route's next corner 8 m+ on and within 25 deg of the facing
                 * and no leap / jet leg there: walk on to it (twice at most, then the goto again). [C7] walkchain: rearm. */
                int prev_moved = c->lw_moved > 0.0 && w->time >= c->lw_moved && w->time - c->lw_moved < 4.0;
                c7_add("gotowalk", "moved", 1); c->lw_until = 0.0; c->lw_t = 0.0; c->back_until = 0.0; c->lw_moved = w->time;
                if (chain && !off_route && prev_moved && c->lw_chain < 4 && hypotf(me->vel.x, me->vel.z) > 3.f &&
                    c7_on("walkchainhold", "RECOMP_BOT_WALK_CHAIN_HOLD", "WalkChainHold", 1)) {
                    int k2 = -1;   /* the first corner on that is 8 m+ off and ahead (corners walked past during the walk are skipped), before any leap / jet leg */
                    for (int q = c->path.next; q < c->path.n && q <= c->path.next + 4; q++) {
                        if (c->path.drop[q] != 0) break;
                        float qo = fabsf(wrap_pi(atan2f(c->path.pt[q].x - me->pos.x, c->path.pt[q].z - me->pos.z) - me->yaw));
                        if (qo > 1.57f) continue;
                        if (hdist2(me->pos, c->path.pt[q]) >= 8.f) { if (qo < 0.44f) k2 = q; break; }
                    }
                    Ma2Vec3 t2 = k2 >= 0 ? c->path.pt[k2] : me->pos;
                    /* Down a bank (the walk 'moved' by coming down 2 m): the route's next leg may be a jet leg planned in the air (P13_81b2 b6, Ill Wind
                     * Snatch It T2, 13.5-17.0 s down the bank 1139 -> 1117,1499: path 2/10 with drop 2, five 0.7 s walks each followed by a 0.3-0.5 s
                     * goto stand). The walk's own point, still 8 m+ on, within 25 deg and 2 m+ lower, is walked on to (four times at most). [C7] walkchain: down. */
                    int down = 0;
                    if (k2 < 0 && me->pos.y < c->lw_y0 - 2.f && c->lw_chain < 4 && hdist2(me->pos, c->lw_pt) >= 8.f &&
                        fabsf(wrap_pi(atan2f(c->lw_pt.x - me->pos.x, c->lw_pt.z - me->pos.z) - me->yaw)) < 0.44f) {
                        int okd = 0; float dy = surface_height(c->lw_pt.x, c->lw_pt.z, &okd);
                        if (okd && dy < me->pos.y - 2.f) { t2 = c->lw_pt; down = 1; }
                    }
                    if (k2 >= 0 && c->lw_chain >= 2) k2 = -1;
                    if (k2 >= 0 || down) {
                        c->lw_chain++; if (k2 >= 0) c->path.next = k2; c7_add("walkchain", down ? "down" : "rearm", 1); c7_add("gotowalk", "kept_route", 1);
                        c->lw_pt = t2; c->lw_p0 = me->pos; c->lw_y0 = me->pos.y; c->lw_until = w->time + 2.5;
                        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-WALKCHAIN] t=%.1f bot=%d at %.0f,%.0f,%.0f walks on to %.0f,%.0f (%d)\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, t2.x, t2.z, c->lw_chain);
                        body_walk_direct(me->actor, me->pos, t2, 100); c->dest = t2; c->mode = CM_MOVE; return 0;
                    }
                }
                c->lw_chain = 0;
                if (!chain || off_route) { c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time; }
                else c7_add("gotowalk", "kept_route", 1);
            }
            /* WalkWallOut (default on, RECOMP_BOT_WALK_WALL_OUT / [Bots] WalkWallOut): a walk into a bare-terrain face (1 m ahead) ran its whole
             * 2.5-3 s and held off AiKick / backtrack (d5_25: four stands of 4.2-4.5 s, b5 88-92 -1072 hp). Not 0.5 m in the first 1.2-1.7 s:
             * failed now. [C7] gotowalk: failed_early. */
            else if (w->time > c->lw_until || w->time < c->lw_until - 3.5 ||
                     (w->time > c->lw_until - 1.3 && hdist2(me->pos, c->lw_p0) < 0.5f && fabsf(me->pos.y - c->lw_y0) < 0.5f &&
                      fabsf(wrap_pi(atan2f(c->lw_pt.x - me->pos.x, c->lw_pt.z - me->pos.z) - me->yaw)) < 0.6f &&   /* (not while a SlopeTurn still turns) */
                      c7_on("walkwallout", "RECOMP_BOT_WALK_WALL_OUT", "WalkWallOut", 1) && (c7_add("gotowalk", "failed_early", 1), 1))) {
                c7_add("gotowalk", "failed", 1); c->lw_until = 0.0; c->lw_t = 0.0;
                memset(c->gw_ht, 0, sizeof c->gw_ht); c->gw_st = 0.0;   /* GotoWindow: a failed walk starts a fresh window */
            }
            else {
                /* SlopeTurn walk: well off the carrot, a slow step so the body turns onto it rather than arcing past it */
                float loff = fabsf(wrap_pi(atan2f(c->lw_pt.x - me->pos.x, c->lw_pt.z - me->pos.z) - me->yaw));
                body_walk_direct(me->actor, me->pos, c->lw_pt, loff > 0.79f ? 30 : 100); c->dest = c->lw_pt; c->mode = CM_MOVE; return 0;
            }
        } else if (lai && !MEM8(lai + 0x240) && fabsf(me->vel.y) < 1.f && hdist2(me->pos, goal) > 10.f &&
                   gva_ok(MEM32(me->actor + 0x8C), 0x150) && MEMF(MEM32(me->actor + 0x8C) + 0x98) < 0.5f) {   /* the navigator gives no forward speed (comp+0x98 0, anim 0): pressing on a wall (22.5 set, au4_20 bot 0) is the stall watchdog's */

            Ma2Vec3 t = c->dest; int okt = 0; float ty = surface_height(t.x, t.z, &okt);
            float off = fabsf(wrap_pi(atan2f(t.x - me->pos.x, t.z - me->pos.z) - me->yaw));
            /* SlopeTurn (default on, RECOMP_BOT_SLOPE_TURN / [Bots] SlopeTurn): on perches, crater rims and hill tops the goto stands with its carrot
             * 78-104 deg off the facing and its own target yaw (comp+0xA0) held ON the facing -- it neither turns nor walks, and the 25 deg gate above
             * never opened (r3_213 TD Witch's Cauldron: b9 at 915.6,63.8,960.8 t=120.4-123.2, carrot 20 m off at 78 deg; b7 BA at 1027,56.5,881
             * t=155.8-162.2, 551 -> 237 hp, carrot flipping 104 / -18..-33 deg; b6, b4 the same). Still 1 s like that: walk it, turning first. [C7] slopeturn: n. */
            int hold = off > 0.44f && fabsf(wrap_pi(MEMF(MEM32(me->actor + 0x8C) + 0xA0) * 0.0174533f - me->yaw)) < 0.17f &&
                       c7_on("slopeturn", "RECOMP_BOT_SLOPE_TURN", "SlopeTurn", 1);
            /* SlopeStuck (default on, RECOMP_BOT_SLOPE_STUCK / [Bots] SlopeStuck): after a drop the goto can hold its target yaw 17 deg off the facing
             * with no turn rate (comp+0xB0 0) and ai+0x360 0: A_205a TD b6 fell 61 -> 47 m at 71.5 and stood 7.5 s (target yaw -155, facing -138,
             * carrot 120 deg off) until a second AiKick; 30 such post-drop stands of 3-17 s in the par13-15 sweep. Not turning counts as held too. */
            if (!hold && off > 0.44f && fabsf(MEMF(MEM32(me->actor + 0x8C) + 0xB0)) < 1.f && c7_on("slopestuck", "RECOMP_BOT_SLOPE_STUCK", "SlopeStuck", 1)) {
                hold = 1; c7_add("slopestuck", "held_s", w->dt);
            }
            /* StaleTurn (default on, RECOMP_BOT_STALE_TURN / [Bots] StaleTurn; parity pass 9, 11-11): the goto latches its turn target comp+0xA0
             * when a jet burn ends (and at boarding: 0) and does not take a new one until that turn is done -- it then stands at anim 0 (above).
             * X_81a2 b5 (Ill Wind Snatch It T2): an in-flight replan put the carrot behind it for 0.4 s as the burn ended (13.37: A0 = 129 deg);
             * it landed at 1115.7,-14.7,1515.5 with the carrot at -159 deg, turned in place the wrong way to 129 (14.44-15.47) and stood till
             * AiKick's stop at 15.97 -- 1.8 s. A still walker whose turn target is 30 deg+ off the bearing to its carrot (8 m+ away) is walked
             * after 0.25 s (turning first) instead of after the turn plus the 1 s clock. [C7] staleturn: held_s / n. */
            /* SlopeStale (default on, RECOMP_BOT_SLOPE_STALE / [Bots] SlopeStale; parity pass 11, 11-11): a SlopeTurn hold (turn target held ON the
             * facing) whose target is also 30 deg+ off the bearing to the carrot is the same latched turn target -- the test above skipped StaleTurn
             * for it and it waited the 1 s clock: B11_84a1 b6 (On the Rocks Snatch It T2) landed at 1526,62.7,1485 on the base's slope top at 28.54,
             * turned to the stale -26 deg (carrot -127) and stood till 29.54 (slopestale_proof.py: 5 such T2 / 1 T1 spells in that match, 0.5-1.7 s
             * cut each where the clock had run). StaleTurn's 0.25 s for it too. [C7] staleturn: slope. */
            int stale = 0, slope_held = hold && c7_on("slopestale", "RECOMP_BOT_SLOPE_STALE", "SlopeStale", 1);
            if ((!hold || slope_held) && off > 0.44f && hdist2(me->pos, t) >= 8.f && c7_on("staleturn", "RECOMP_BOT_STALE_TURN", "StaleTurn", 1) &&
                fabsf(wrap_pi(MEMF(MEM32(me->actor + 0x8C) + 0xA0) * 0.0174533f - atan2f(t.x - me->pos.x, t.z - me->pos.z))) > 0.52f) {
                if (hold) c7_add("staleturn", "slope", w->dt);
                hold = 1; stale = 1; c7_add("staleturn", "held_s", w->dt);
            }
            if (!okt) ty = t.y;
            if (gwin_on && c->gw_st > 0.0 && w->time - c->gw_st >= 0.3 && hdist2(me->pos, t) >= 8.f) {
                int best = -1;   /* the newest sample at least 2 s old (and not over 3 s: no gaps) */
                for (int q = 0; q < 12; q++)
                    if (c->gw_ht[q] > 0.0 && w->time - c->gw_ht[q] >= 2.0 && w->time - c->gw_ht[q] < 3.0 && (best < 0 || c->gw_ht[q] > c->gw_ht[best])) best = q;
                if (best >= 0 && hdist2(me->pos, c->gw_hp[best]) < 0.5f * kind_cruise(me->kind)) gwin = 1;
            }
            if (!gwin && (c->lw_t <= 0.0 || w->time < c->lw_t || hdist2(me->pos, c->lw_pos) > 0.7f || hdist2(me->pos, t) < 8.f || (off > 0.44f && !hold))) { c->lw_pos = me->pos; c->lw_t = w->time; }
            else if (gwin || w->time - c->lw_t > (stale ? 0.25 : c->lw_moved > 0.0 && w->time >= c->lw_moved && w->time - c->lw_moved < 4.0 &&
                                          c7_on("gotochain", "RECOMP_BOT_GOTO_CHAIN", "GotoChain", 1) ? 0.3 : 1.0)) {
                if (leap_not_walk(c, w, me)) { c->lw_t = 0.0; goto gw_done; }   /* the leap from here, not a walk to its top (F38 jets below) */
                c->lw_pt = t; c->lw_p0 = me->pos; c->lw_y0 = me->pos.y; c->lw_until = w->time + (off > 0.44f ? 3.0 : 2.5); c->lw_t = 0.0; c7_add("gotowalk", "n", 1);
                if (stale) c7_add("staleturn", "n", 1);
                if (gwin) { c7_add("gotowin", "n", 1); memset(c->gw_ht, 0, sizeof c->gw_ht); c->gw_st = 0.0; }
                else if (off > 0.44f) c7_add("slopeturn", "n", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-GOTOWALK] t=%.1f bot=%d kind=%d%s still at %.1f,%.1f,%.1f facing %.0f m to %.0f,%.1f,%.0f (%.1f m lower, %.0f deg off): walking it\n", w->time, index, me->kind, gwin ? " (2 s window)" : "", me->pos.x, me->pos.y, me->pos.z, hdist2(me->pos, t), t.x, ty, t.z, me->pos.y - ty, off * 57.3f);
                body_walk_direct(me->actor, me->pos, t, off > 0.79f ? 30 : 100); c->mode = CM_MOVE;
                return 0;
            }
        }
    }
    gw_done:;
    /* Backing out after a stall: straight to the breadcrumb, then replan. Walked, not jetted --
     * a hop forward over the lip and a hop back out, repeated, is the jump-jet ping-pong a player
     * would never do (BattleArmor still hops: that is how it moves). */
    if (!air && w->time < c->back_until) {
        if (hdist2(me->pos, c->back_pt) < 3.f) { c->back_until = 0.0; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time; }
        else {
            {
                /* BackCarrot (default on, RECOMP_BOT_BACK_CARROT / [Bots] BackCarrot): MoveTo halts 5-10 m short of its point, so a crumb that
                 * close was "arrived at" and the body stood out the whole 4 s: au3_00 bot 0 (Grinder, 11-04) stood t=191.0-194.8 facing a crumb
                 * 6.7 m off, anim 0, AiKick held off by the backtrack. Steered at like any carrot: along the same line, kind_min_carrot out
                 * (the 3 m arrival test stays on the crumb). [C7] backcarrot: pushed. */
                Ma2Vec3 bp = c->back_pt; float bl = hdist2(bp, me->pos), bm = kind_min_carrot(me->kind);
                if (bl < bm && bl > 0.3f && c7_on("backcarrot", "RECOMP_BOT_BACK_CARROT", "BackCarrot", 1)) {
                    bp.x = me->pos.x + (bp.x - me->pos.x) / bl * bm; bp.z = me->pos.z + (bp.z - me->pos.z) / bl * bm; c7_add("backcarrot", "pushed_s", w->dt);
                }
                body_steer(me, bp);
                if (me->kind == MA2_KIND_BATTLEARMOR && body_jump_toward(me->actor, me->pos, c->back_pt)) c->jumps++;
            }
            c->dest = c->back_pt; c->mode = CM_MOVE;
            return 0;
        }
    }
    ctrl_plan_take(index, b, w, me);                                        /* W7: a queued plan may have finished */
    const char *rp_why = !c->have_path ? "none" : hdist2(goal, c->path_goal) > maxf2(6.f, 0.15f * d) ? "goal" : w->time - c->path_t > 6.0 ? "age" :
                         ctrl_route_stale(c, me) ? "epoch" :               /* the map changed near the route left (something destroyed) */
                         ma2_path_off(&c->path, me->pos, 8.f) ? "off" : NULL;
    int replan = rp_why != NULL;
    if (replan) c7_add("replan", rp_why, 1);                                /* push-x3: which trigger drives the replans */
    /* The remaining route newly blocked (a unit stopped on it): replan at once (>= 0.3 s apart). */
    if (!replan && !air && c->have_path && w->time - c->path_t >= 0.3) {
        Obst ob[MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS];
        int no = ctrl_obstacles(w, me, ignore_veh, ob, MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS);
        float rad = plan_radius(me);
        Ma2Vec3 a0 = me->pos;
        for (int sgi = c->path.next; sgi < c->path.n && sgi < c->path.next + 3 && !replan; sgi++) {
            Ma2Vec3 b0 = c->path.pt[sgi];
            for (int i = 0; i < no; i++) {
                if (!ob[i].slow) continue;
                if (ob[i].actor && ob[i].actor == b->order.target) continue;   /* the one we are going for: a route to it ends on it (replanned every 0.3 s) */
                /* BoardNoReplan: nor the vehicle we walk to board -- planned round by its outline, it "blocked" segment 0 every
                 * 0.3 s and the pilot crept at ~1.5 m/s, each new plan queued behind the others (laptop log 10-31: 6 s for 6 m). */
                if (ob[i].actor && b->order.type == MA2_ORD_GET_VEHICLE && (ob[i].actor == b->order.vehicle || ob[i].actor == c->board_veh) &&
                    c7_on("boardnoreplan", "RECOMP_BOT_BOARD_NO_REPLAN", "BoardNoReplan", 1)) continue;
                if (hypotf(ob[i].x - me->pos.x, ob[i].z - me->pos.z) < ob[i].r + rad) continue;   /* the one we already touch */
                if (seg_point_dist(a0.x, a0.z, b0.x, b0.z, ob[i].x, ob[i].z, NULL) < ob[i].r + rad) {
                    /* A parked hull was planned round by its measured outline: the route may pass
                     * its narrow side well inside the circle of its longest arm. */
                    const Ma2VehicleView *hv = NULL;
                    for (int k = 0; k < w->nvehicles; k++) if (w->vehicles[k].actor == ob[i].actor && !w->vehicles[k].occupied) hv = &w->vehicles[k];
                    if (hv && hv->ext_max > 0.5f && !seg_hits_hull(hv, hull_tall(me->kind), a0, b0)) continue;
                    replan = 1; c->replans_blocked++;
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-REPLAN] bot=%d blocked by %08X kind=%d at %.0f,%.0f seg %d t=%.1f\n", index, ob[i].actor, ob[i].kind, ob[i].x, ob[i].z, sgi, w->time);
                    break;
                }
            }
            a0 = b0;
        }
        if (replan) c->next_plan = w->time;
    }
    /* A pilot bomb seen since this route was planned: plan again if what is left of it passes inside the blast. */
    if (!replan && !air && c->have_path && c->bav_epoch != w->bomb_epoch) {
        c->bav_epoch = w->bomb_epoch;
        Ma2Vec3 a0 = me->pos;
        for (int sgi = c->path.next; sgi < c->path.n && !replan; sgi++) {
            for (int i = 0; i < w->nbombs && !replan; i++)
                if (w->bombs[i].owner != b->player && hdist2(w->bombs[i].pos, goal) > 27.f &&
                    seg_point_dist(a0.x, a0.z, c->path.pt[sgi].x, c->path.pt[sgi].z, w->bombs[i].pos.x, w->bombs[i].pos.z, NULL) < 27.f) {
                    replan = 1; c->next_plan = w->time; c7_add("bombavoid", "rerouted", 1);
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-BOMBAV] t=%.1f bot=%d reroute round bomb %08X at %.0f,%.0f\n", w->time, index, w->bombs[i].ent, w->bombs[i].pos.x, w->bombs[i].pos.z);
                }
            a0 = c->path.pt[sgi];
        }
    }
    if (replan && w->time >= c->next_plan && plan_slot_ok(w)) ctrl_plan(index, b, w, me, goal, ignore_veh);
    Ma2Vec3 dest = goal, carrot = goal;
    int at_end = 1;
    if (c->have_path) {
        int arr = 0;
        (void)ma2_path_follow(&c->path, me->pos, me->kind == MA2_KIND_PILOT ? 4.f : air ? 20.f : 9.f, &arr);
        /* LeapTop, the follower: standing at the top of a leap a few corners on (a replan's start snap or a corner short of the rim still ahead) is
         * being at that leap -- skip to it. P5_81a1 b5 (Ill Wind team 2) passed 3 m from its leap top at 1156,1523 at 20.3 s with path 0/9 and
         * milled on the rim until 32.2 s; the executors only look at the leg being steered for. [C7] leaptop: skips. */
        if (!air && leap_top_on())
            for (int j = c->path.next + 1; j > 0 && j < c->path.n && j <= c->path.next + 3; j++)
                if (c->path.drop[j] && (c->path.drop[j] == 1 || c->path.drop[j] == 2) && hdist2(me->pos, c->path.pt[j - 1]) < 6.f) { c->path.next = j; c7_add("leaptop", "skips", 1); break; }
        /* AirAhead (default on, RECOMP_BOT_AIR_AHEAD / [Bots] AirAhead; parity pass 9, 11-11): in a jet flight (burning, or 3 m+ over the surface and
         * rising / falling) at 3 m/s+, route corners BEHIND the flight are passed -- the goto latches its turn target from the carrot when the burn ends
         * and turns to it on landing (StaleTurn). A plan searched from over a rock or an edge starts at the free cell the search snapped to, behind
         * the body: all five T2 'Mechs that leapt into Ill Wind's basin (X_81a1 b6, X_81a2 b5, X_81a3 b7, X_81b1 b9, X_81b3 b7; Snatch It)
         * replanned 'off' at 1131-1132,37,1502 over the rock, got 1142,1494 (behind, 129 deg) as the carrot as the burn ended, and turned round on
         * landing (1.1-1.5 s stood); a corner overflown in the air does the same (X_81a2 T1 b1 12.0). The next corner ahead of the flight is
         * steered for instead (none ahead: unchanged). [C7] airahead: skips. */
        if (!air && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && c->path.next < c->path.n - 1 && hypotf(me->vel.x, me->vel.z) > 3.f &&
            c7_on("airahead", "RECOMP_BOT_AIR_AHEAD", "AirAhead", 1)) {
            uint32_t aai = actor_ai(me->actor); int oks = 0; float sh = surface_height(me->pos.x, me->pos.z, &oks);
            if ((aai && MEM8(aai + 0x240)) || (oks && me->pos.y - sh > 3.f && fabsf(me->vel.y) > 1.f)) {
                int k = c->path.next;
                while (k < c->path.n && (c->path.pt[k].x - me->pos.x) * me->vel.x + (c->path.pt[k].z - me->pos.z) * me->vel.z <= 0.f) k++;
                if (k > c->path.next && k < c->path.n) { c->path.next = k; c7_add("airahead", "skips", 1); }
            }
        }
        /* LeapStall (default on, RECOMP_BOT_LEAP_STALL / [Bots] LeapStall; parity pass 5, 11-11): a jet body whose route leaps DOWN within the next
         * corners, 6-18 m short of the leap's top and not 1 m closer to it for 1.2 s, leaps from where it stands -- as a player jets off the rim
         * rather than shuffling to the planner's exact top cell. The campaign MechAI goto would not walk the last 12 m of a 0.33 grade down to the
         * top (turning on the spot, no forward speed): P8_81b2 b6 (Ill Wind team 2) stood 10.7-17.6 s at 1160-1162,1511-1517 with path 0/9 and its
         * top 1150,1508 12 m on, until a backtrack jetted it over; the executors only run within 6-12 m of the top. The leg becomes a jet leap from
         * here (standing, landing <= 60 m on and 5 m+ lower, nothing rising between). [C7] leapstall: n. */
        if (!air && leap_stall_on() && (me->kind == MA2_KIND_BATTLEARMOR || (me->kind == MA2_KIND_MECH && ma2_bot_has_jets(me->actor)))) {
            int lj = -1;
            for (int j = c->path.next > 0 ? c->path.next : 1; j < c->path.n && j <= c->path.next + 3; j++)
                if (c->path.drop[j] == 1 || c->path.drop[j] == 2) { lj = j; break; }
            if (lj > 0) {
                Ma2Vec3 top = c->path.pt[lj - 1], land = c->path.pt[lj];
                int okl = 0; float lh = terrain_height(land.x, land.z, &okl); if (okl) land.y = lh;
                float dtop = hdist2(me->pos, top);
                /* LeapStallUp (default on, RECOMP_BOT_LEAP_STALL_UP / [Bots] LeapStallUp; parity pass 9, 11-11): the same halt short of the FOOT of a
                 * jet-up leg -- the jet (F38) fires only within 8 m of the foot and the goto stops 5-10 m short of the carrot there: S9_81b1 b7 (Ill Wind
                 * Snatch It T2) stood 31.8-33.9 s at 713,-0.3,1391, 10 m from the foot 718,1382 of the band jet-up to 718,17,1354, until GotoWindow
                 * walked it in and it jetted at 34.4. A jet leg (drop 2) whose landing is 5-22 m higher and < 45 m off, standing (< 1 m/s) 0.4 s
                 * 6-14 m from the foot: the jet goes from here. [C7] leapstall: up. */
                /* LeapStallUpFar (default on, RECOMP_BOT_LEAP_STALL_UP_FAR / [Bots] LeapStallUpFar; parity pass 10, 11-11): standing 14-24 m from the
                 * foot it stood on: X9_81b1 b10 (Ill Wind Snatch It T2) 38.7-39.6 s at 710,-0,1396, foot 15.5 m, landing 42 m off +17 m, jetted at 40.2
                 * once the goto had shuffled in. A 3 s burn carries a 'Mech 50 m+ (air speed 18-22 m/s): reach 24 m to the foot, 52 m to the landing,
                 * the same clear-line test. [C7] leapstall: up_far. */
                int lsupfar = leap_stall_up_far_on();
                int lsup = c->path.drop[lj] == 2 && land.y > me->pos.y + 5.f && land.y < me->pos.y + 22.f && hdist2(me->pos, land) < (lsupfar ? 52.f : 45.f) &&
                           dtop < (lsupfar ? 24.f : 14.f) &&
                           hypotf(me->vel.x, me->vel.z) < 1.f && c7_on("leapstallup", "RECOMP_BOT_LEAP_STALL_UP", "LeapStallUp", 1);
                if (c->ls_t <= 0.0 || w->time < c->ls_t || hdist2(c->ls_top, top) > 10.f || dtop < c->ls_best - 1.f) { c->ls_t = w->time; c->ls_best = dtop; c->ls_top = top; }
                /* LeapStallQuick (default on, RECOMP_BOT_LEAP_STALL_QUICK / [Bots] LeapStallQuick; parity pass 9, 11-11): the goto halts 5-10 m short
                 * of its point and the carrot stops AT the leap top (LeapTop), so a body walking to a leap down comes to a stop 6-12 m short every
                 * time and stood out the whole 1.2 s: X_81b1 b8 (Ill Wind Snatch It T2) stood 13.2-14.1 s at 1159,13,1513, X_81a2 b9 14.6-15.6 s.
                 * Standing (< 1 m/s) is not still closing in: 0.4 s then. Walking on, the 1.2 s as before. [C7] leapstall: quick. */
                else if (w->time - c->ls_t > (hypotf(me->vel.x, me->vel.z) < 1.f && c7_on("leapstallquick", "RECOMP_BOT_LEAP_STALL_QUICK", "LeapStallQuick", 1) ? 0.4 : 1.2) &&
                         dtop >= 6.f && dtop < (hypotf(me->vel.x, me->vel.z) < 1.f && leap_stall_far_on() ? 24.f : 18.f) &&   /* LeapStallFar: standing, as LeapNotWalk */
                         (lsup || (land.y < me->pos.y - 5.f && hdist2(me->pos, land) < 60.f))) {
                    int okg = 0; float gh0 = surface_height(me->pos.x, me->pos.z, &okg);
                    int clear = !okg || me->pos.y - gh0 < 1.5f;   /* standing (not mid-hop), and nothing rises between here and the top */
                    int okt = 0; float top_g = surface_height(top.x, top.z, &okt); if (!okt) top_g = me->pos.y;   /* LeapStallUp: the foot's own ground (corner y = the body's height when planned) */
                    for (float s = 2.f; s < dtop && clear; s += 2.f) {
                        int oks = 0; float px = me->pos.x + (top.x - me->pos.x) * s / dtop, pz = me->pos.z + (top.z - me->pos.z) * s / dtop;
                        float sh = surface_height(px, pz, &oks);
                        if (oks && sh > (lsup ? maxf2(me->pos.y, top_g) : me->pos.y) + 2.f) clear = 0;
                    }
                    if (clear) {
                        c->path.pt[lj - 1] = me->pos; c->path.drop[lj] = 2; c->path.next = lj;
                        if (lsup) c7_add("leapstall", "up", 1);
                        if (lsup && dtop >= 14.f) c7_add("leapstall", "up_far", 1);
                        if (dtop >= 18.f) c7_add("leapstall", "far", 1);
                        if (w->time - c->ls_t <= 1.2) c7_add("leapstall", "quick", 1);
                        c->ls_t = w->time; c->ls_best = 0.f; c7_add("leapstall", "n", 1);
                        if (c->jump_next > w->time) c->jump_next = w->time;   /* the leap now, not after another jet's 3 s spacing (Q_81a1 b3 waited 10.4 -> 15.9) */
                        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-LEAPSTALL] t=%.1f bot=%d kind=%d at %.0f,%.0f,%.0f %.0f m short of the leap top %.0f,%.0f: leaps from here to %.0f,%.0f,%.0f\n",
                                                                 w->time, index, me->kind, me->pos.x, me->pos.y, me->pos.z, dtop, top.x, top.z, land.x, land.y, land.z);
                    }
                }
            } else c->ls_t = 0.0;
        }
        /* A drop leg: from its top, walk straight off the edge at the landing -- no carrot, no
         * steering round the "wall" the grid sees there (as a 'Mech steps off a roof). */
        /* F38: a jet-up leg: at its foot, jet at the landing. Not 1 m higher within 6 s of the first
         * try from here: that ledge does not work (no one plans a jump or drop from here 10 min). */
        if (!air && c->path.next > 0 && c->path.next < c->path.n && c->path.drop[c->path.next] == 2 &&
            hdist2(me->pos, c->path.pt[c->path.next - 1]) < 8.f) {
            Ma2Vec3 land = c->path.pt[c->path.next];
            int okh = 0; float gh = terrain_height(land.x, land.z, &okh);
            if (okh) land.y = gh;
            /* JetUpLanded (default on, RECOMP_BOT_JETUP_LANDED / [Bots] JetUpLanded): a hop that rose 1 m counted as "climbed" and a 2.4 m drift
             * restarted the clock, so a 9 m up-link the body could not make was tried for minutes (rc10_93b b2 Base War Rush Hour: 1047,58,1660 ->
             * 1040,67,1670 at 206, 211, 216 s; 1065,30,1595 -> 1050,41,1608 at 154, 164, 197 s; 285 s on that hill). Landed = within 2 m of the
             * landing height and 8 m of it; the try is keyed on a 6 m circle. */
            int jul = jetup_landed_on();
            if (c->up_t0 <= 0.0 || w->time < c->up_t0 || hdist2(c->up_p0, me->pos) > (jul ? 36.f : 6.f)) {
                c->up_t0 = w->time; c->up_p0 = me->pos; c->up_y0 = me->pos.y; c->up_land = land; c->up_fire = 0.0; c7_add("f38", "tries", 1);
            } else if ((jet_try_fuel_on() ? (c->up_fire > 0.0 ? w->time - c->up_fire > 6.0 : w->time - c->up_t0 > 20.0) : w->time - c->up_t0 > 6.0) && (land.y < c->up_y0 - 3.f ? me->pos.y > c->up_y0 - 2.f : (jul ? me->pos.y < land.y - 2.f : me->pos.y < c->up_y0 + 1.f))) {   /* HopDown: a leap down is judged by the height lost */
                c7_add("f38", "gaveup", 1);
                ma2_path_forbid_drop(me->pos.x, me->pos.z, w->time + 600.0);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-JETUP] t=%.1f bot=%d could not jet up at %.0f,%.0f -- not from here for 10 min\n", w->time, index, me->pos.x, me->pos.z);
                c->up_t0 = 0.0; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
            }
            if (c->up_t0 > 0.0 && w->time >= c->jump_next && body_jump_up(me->actor, me->pos, land, 1.2f)) {
                c->jumps++; c->jump_next = w->time + 3.0; c7_add("f38", "jets", 1); if (c->up_fire <= 0.0) c->up_fire = w->time;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-JETUP] t=%.1f bot=%d jets up from %.0f,%.0f,%.0f to %.0f,%.0f,%.0f\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, land.x, land.y, land.z);
            }
        } else if (c->up_t0 > 0.0 && (jetup_landed_on() ? me->pos.y >= c->up_land.y - 2.f && hdist2(me->pos, c->up_land) < 64.f : me->pos.y >= c->up_y0 + 1.f)) { c7_add("f38", "climbed", 1); c->up_t0 = 0.0; }
        else if (c->up_t0 > 0.0 && me->pos.y <= c->up_y0 - 6.f) { c7_add("f38", "leapt_down", 1); c->up_t0 = 0.0; }
        if (!air && c->path.next > 0 && c->path.next < c->path.n && c->path.drop[c->path.next] == 1 &&
            hdist2(me->pos, c->path.pt[c->path.next - 1]) < 12.f) {
            Ma2Vec3 land = c->path.pt[c->path.next], top = c->path.pt[c->path.next - 1];
            /* One attempt per edge (a replan moves the top a metre or two). Not come down 2 m within
             * 5 s: this drop does not work (an invisible wall, a fence or rail the grid missed, water
             * the body will not enter). No one plans a drop off that edge again for 10 minutes (the ground does not change). (A
             * carrier walked at one edge for 20 s while each replan restarted the clock: run dr6.) */
            /* F11: replans along one edge can move the top 10 m+ while the body stands still (a 'Mech
             * stepped off the same spot 41 times in 120 s, run s13m5): key the attempt on where the
             * body is, not on the plan's top. */
            int fresh = c->drop_t0 <= 0.0 || w->time < c->drop_t0 ||
                        (drop_giveup_on() ? hdist2(c->drop_p0, me->pos) > 6.f : hdist2(c->drop_top, top) > 10.f);
            /* DropLedge (default on, RECOMP_BOT_DROP_LEDGE / [Bots] DropLedge): came down 2 m but stuck there -- a hop had set
             * the start height in the air, or a ledge part way down -- and the 5 s give-up never ran: a BattleArmor ran on the
             * spot at an edge for 13 s (laptop log 10-31, Bogged Down). The new low spot starts the attempt over. */
            int ledge = c7_on("dropledge", "RECOMP_BOT_DROP_LEDGE", "DropLedge", 1);
            /* DropSettle (default on, RECOMP_BOT_DROP_SETTLE / [Bots] DropSettle): WP3 round, sweep r4B-sw65 run 1: a 'Mech at 1498,60,1525 stood 75 s (t=42-116) on a "drop" leg whose
             * landing was level (y 59.7) 21 m away: the attempt began while it was still coming down the slope (y 65 -> 59.7 over 3 m), so it was neither "not come down 2 m"
             * (the give-up) nor within 3 m of the attempt's start (the DropLedge restart): neither ever ran, the leg returned before the stall watchdog, [BOT-DROP]
             * logged every 3 s and the body never moved. Having come down 2 m+ and 2 s passed, the attempt now starts over wherever the body stands (the
             * 6 m move already did), so the 5 s give-up (no drops there for 10 min, replan) finds the standing body. [C7] dropsettle: restarts. */
            int settle = c7_on("dropsettle", "RECOMP_BOT_DROP_SETTLE", "DropSettle", 1);
            /* DropShort (default on, RECOMP_BOT_DROP_SHORT / [Bots] DropShort; parity pass 11-08): the body went over the edge but came to rest well short
             * of the landing (a gap or a deck's underside the one-layer grid does not show) and stands there. DropSettle restarted the attempt from the
             * low spot and the 5 s give-up ran from there: rc10_85a b4 (Snatch It Rush Hour) fell 19 m at 1546,1512 (landing 1530,1512, 16 m on) at
             * 21.1 and stood with no input until 29.4 -- 8 s of team 1's opening. Down 4 m+, 2 s on, standing (< 1 m/s) 8 m+ from the landing: this
             * drop failed now -- no drops from its top for 10 min, replan from where the body is. [C7] dropshort: n. */
            if (!fresh && c->drop_t0 > 0.0 && me->pos.y < c->drop_y0 - 4.f && w->time - c->drop_t0 > 2.0 && hypotf(me->vel.x, me->vel.z) < 1.f &&
                hdist2(me->pos, land) > 8.f && c7_on("dropshort", "RECOMP_BOT_DROP_SHORT", "DropShort", 1)) {
                c7_add("dropshort", "n", 1); c->n_dropfail++;
                ma2_path_forbid_drop(c->drop_top.x, c->drop_top.z, w->time + 600.0);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DROP] t=%.1f bot=%d landed short at %.0f,%.0f,%.0f (landing %.0f,%.0f) -- replan, no drops from %.0f,%.0f for 10 min\n",
                                                         w->time, index, me->pos.x, me->pos.y, me->pos.z, land.x, land.z, c->drop_top.x, c->drop_top.z);
                c->dt_top = c->drop_top; c->dt_land = me->pos; c->dt_t = w->time; c->dt_n = 0;   /* DropTrap remembers it */
                c->drop_t0 = 0.0; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
                body_steer(me, me->pos); c->dest = me->pos; c->mode = CM_MOVE;
                return 0;
            }
            if (!fresh && ledge && me->pos.y < c->drop_y0 - 2.f && w->time - c->drop_t0 > 2.0 && (hdist2(c->drop_p0, me->pos) < 3.f || settle)) {
                fresh = 1; c7_add(hdist2(c->drop_p0, me->pos) < 3.f ? "dropledge" : "dropsettle", "restarts", 1);
            }
            /* BaDipHop: a BattleArmor that came to this edge in the air (a hop landing at the top) has the attempt start on the ground:
             * begun mid-hop, landing 8 m lower read as "came down" and it stood "stepping off" for 49 s (Bogged Down 886,17,1185) */
            if (me->kind == MA2_KIND_BATTLEARMOR && !fresh && c->drop_t0 > 0.0 && ba_dip_hop_on()) {
                uint32_t dai = actor_ai(me->actor); int oks = 0; float sfc = surface_height(me->pos.x, me->pos.z, &oks);
                if ((dai && MEM8(dai + 0x240)) || (oks && me->pos.y - sfc > 1.5f)) { c->drop_t0 = w->time; c->drop_y0 = me->pos.y; c->drop_p0 = me->pos; c->drop_air = 1; }
                else if (c->drop_air) { c->drop_air = 0; c->drop_t0 = w->time; c->drop_y0 = me->pos.y; c->drop_p0 = me->pos; c7_add("badiphop", "drop_reseat", 1); }
            }
            /* DropEdgeRun (default on, RECOMP_BOT_DROP_EDGE_RUN / [Bots] DropEdgeRun): each replan along an edge moved the attempt 8-10 m, and a new
             * attempt restarted the 5 s give-up, so a 'Mech walked a ledge 12 s trying four spots it could not step off (rc10_83b b9 Snatch It
             * No Man's Land 1382,990 -> 1375,983 -> 1367,986 -> 1368,983, 42-54 s). A new attempt within 15 s and 15 m of one that never came down
             * fails that one: no drop from its edge for 10 min. [C7] dropedgerun: n. */
            if (fresh && c->drop_t0 > 0.0 && w->time >= c->drop_t0 + 2.5 && w->time - c->drop_t0 < 15.0 && hdist2(c->drop_p0, me->pos) < 225.f &&   /* 2.5 s at it: a try cut short by a replan proves nothing (bal35: 15-23 a match) */
                me->pos.y > c->drop_y0 - 2.f && c7_on("dropedgerun", "RECOMP_BOT_DROP_EDGE_RUN", "DropEdgeRun", 1)) {
                c7_add("dropedgerun", "n", 1); c->n_dropfail++;
                ma2_path_forbid_drop(c->drop_top.x, c->drop_top.z, w->time + 600.0);
                ma2_path_forbid_drop(c->drop_p0.x, c->drop_p0.z, w->time + 600.0);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DROP] t=%.1f bot=%d edge run: the try at %.0f,%.0f did not come down -- no drops there for 10 min\n", w->time, index, c->drop_top.x, c->drop_top.z);
            }
            if (fresh) { c->drop_t0 = w->time; c->drop_top = top; c->drop_y0 = me->pos.y; c->drop_p0 = me->pos; c7_add("dropgiveup", "tries", 1); if (b->order.drop_pref) c7_add("carrierdrops", "tries", 1); }
            else if (w->time - c->drop_t0 > 5.0 && me->pos.y > c->drop_y0 - 2.f) {
                c7_add("dropgiveup", "gaveup", 1); c->n_dropfail++;
                ma2_path_forbid_drop(c->drop_top.x, c->drop_top.z, w->time + 600.0);
                ma2_path_forbid_drop(me->pos.x, me->pos.z, w->time + 600.0);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DROP] t=%.1f bot=%d could not step off at %.0f,%.0f -- no drops there for 10 min\n", w->time, index, c->drop_top.x, c->drop_top.z);
                c->drop_t0 = 0.0; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
            }
            if (env_on("RECOMP_BOT_DIAG") && w->time - c->drop_log_t > 3.0) {
                c->drop_log_t = w->time;
                fprintf(stderr, "[BOT-DROP] t=%.1f bot=%d kind=%d steps off at %.0f,%.0f,%.0f toward %.0f,%.0f\n", w->time, index, me->kind,
                        me->pos.x, me->pos.y, me->pos.z, land.x, land.z);
            }
            /* DropLedge: a BattleArmor 2 s at the edge without getting anywhere jets over the lip toward the landing. */
            if (ledge && me->kind == MA2_KIND_BATTLEARMOR && w->time - c->drop_t0 > 2.0 && hdist2(c->drop_p0, me->pos) < 2.f &&
                w->time >= c->jump_next && body_jump_up(me->actor, me->pos, land, 1.2f)) {
                c->jumps++; c->jump_next = w->time + 3.0; c7_add("dropledge", "jets", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DROP] t=%.1f bot=%d jets over the edge at %.0f,%.0f toward %.0f,%.0f\n", w->time, index, me->pos.x, me->pos.z, land.x, land.z);
            }
            body_steer(me, land); c->dest = land; c->mode = CM_MOVE;
            return 0;
        }
        if (c->drop_t0 > 0.0 && c->dt_t < c->drop_t0 && me->pos.y < c->drop_y0 - 4.f) {   /* DropTrap: this drop came down: remember it */
            c->dt_top = c->drop_top; c->dt_land = me->pos; c->dt_t = w->time; c->dt_n = 0;
        }
        if (c->drop_t0 > 0.0 && hdist2(me->pos, c->drop_top) > 30.f) c->drop_t0 = 0.0;   /* moved on from that edge (a replan off the leg keeps the attempt) */
        else if (c->drop_t0 > 0.0 && w->time - c->drop_t0 > 6.0 && me->pos.y > c->drop_y0 - 2.f) {   /* milling about at the edge, never down */
            ma2_path_forbid_drop(c->drop_top.x, c->drop_top.z, w->time + 600.0);
            c7_add("dropgiveup", "gaveup", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DROP] t=%.1f bot=%d could not step off at %.0f,%.0f (off the leg) -- no drops there for 10 min\n", w->time, index, c->drop_top.x, c->drop_top.z);
            c->drop_t0 = 0.0; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
        }
        dest = path_carrot(&c->path, w, me, ignore_veh, kind_lookahead(me->kind), plan_radius(me), w->time, &at_end);
        carrot = dest;
        /* A corner closer than the braking distance would slow the body on every turn: aim past it
         * along the same line (the carrot moves round the corner as soon as the next leg is clear). */
        float m = kind_min_carrot(me->kind), dl = hdist2(dest, me->pos);
        if (!at_end && dl < m && dl > 0.3f) { dest.x = me->pos.x + (dest.x - me->pos.x) / dl * m; dest.z = me->pos.z + (dest.z - me->pos.z) / dl * m; }
    }
    int hold_end = 0;                                                        /* push-x3: standing at a partial route's end */
    if (at_end) {
        /* Last leg: aim at the planner's reachable end (a free cell; the raw goal may sit
         * against a hull or wall, where MoveTo's local avoidance would orbit it), pushed on
         * along the final segment by MoveTo's halt margin. */
        float ov = maxf2(overshoot, kind_stop_margin(me->kind) - stop + 1.f);
        /* push-x3 partial-end hold: the route ends well short of the goal (a roof, a deck, another region: the planner ended on our own
         * ground). Pushing past that end drives into the wall / edge, stalls, backtracks to the same end and ends in BOT-TRAPPED (an eject).
         * Standing at the end is the answer: no overshoot, no stall, and the brain is told there is no path (it drops a roam / attack
         * goal; the jets of a 'Mech / BattleArmor within 35 m of a raised goal fire above). */
        /* EndGapStop (default on, RECOMP_BOT_END_GAP_STOP / [Bots] EndGapStop): a route 12 m short of a hunt / roam point with an 8 m stop
         * can never arrive, yet the hold wanted a gap over 12 m and the goal 20 m+ off -- d5_27 b0 stood 5.1 s (58.6-63.7 at 668,991) at
         * its route end 12 m from the point, the goto idle, no "no path" to the brain. For a travel stop of 6 m+ the bars are stop + 3. */
        float eg_gap = 12.f, eg_dist = 20.f;
        if (stop >= 6.f && c7_on("endgapstop", "RECOMP_BOT_END_GAP_STOP", "EndGapStop", 1)) { eg_gap = minf2(12.f, stop + 3.f); eg_dist = minf2(20.f, stop + 3.f); }
        if (navfix_on() && !air && c->have_path && c->path.n > 0 && c->path.end_gap > eg_gap && hdist2(me->pos, goal) > eg_dist &&
            hdist2(me->pos, c->path.pt[c->path.n - 1]) < kind_stop_margin(me->kind) + 3.f) {
            hold_end = 1; ov = 0.f; b->status.no_path = 1; c7_add("navfix", "end_hold_frames", 1);
        }
        Ma2Vec3 base = goal, from = me->pos;
        if (c->have_path && c->path.n > 0) {
            base = c->path.pt[c->path.n - 1];
            if (hdist2(base, me->pos) < 3.f && c->path.n >= 2) from = c->path.pt[c->path.n - 2];
        }
        float dx = base.x - from.x, dz = base.z - from.z, l = hypotf(dx, dz);
        dest = base;
        if (l > 0.5f && ov > 0.f) { dest.x += dx / l * ov; dest.z += dz / l * ov; }
    }
    /* push-x NoPathHold (RECOMP_BOT_NOPATH_HOLD / [Bots] NoPathHold): a 'Mech or BattleArmor with no route (its plan is
     * still queued, or failed: no free cell near it) used to head straight for a goal hundreds of metres off -- and
     * its travel jets burned that way too, up a cliff face into a pocket with no way out (NRC 384,1168 / 452,1325:
     * path=0/0 until the bot ejected). Now: no travel jets without a route; when the planner has failed from here the
     * bot walks back along its own breadcrumbs (ground it was known to stand on) until a plan works again.
     * [C7] nopath_hold: nojet_n (jets withheld), back_s (seconds walking back), failed_n (plan failures with a far goal). */
    int nopath = 0;
    if (!air && !c->have_path && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && hdist2(me->pos, goal) > 40.f &&
        c7_on("nopath_hold", "RECOMP_BOT_NOPATH_HOLD", "NoPathHold", 1)) {
        nopath = 1; c7_add("nopath_hold", "frames", 1);
        if ((navfix_on() ? c->nostart >= 1 : c->path_fails >= 1) && c->ncrumb >= 2) {
            Ma2Vec3 oldest = c->crumb[c->ncrumb >= 8 ? c->crumb_i % 8 : 0];
            if (hdist2(oldest, me->pos) > 4.f) { dest = oldest; c7_add("nopath_hold", "back_s", w->dt); }
        }
    }
    /* Crowd steering. Every unit within 45 m (a parked hull is the planner's business) is a disc
     * moving on its course: a bot's course is where it is steering (we know its intent), anyone
     * else's its velocity. Of the headings 0-70 degrees either side of the way we want to go, the
     * one that keeps clear of all of them for the next 3 s with the least turn wins -- so two bots
     * meeting at a corner each ease aside early and pass, instead of walking into each other and
     * stopping. Ties go right (both easing the same way would cross again). No heading clear and
     * a teammate head-on: the one with the lower priority waits a moment. */
    if (!air) {
        double av_t0 = c7_t0();
        int hyb = ma2_bot_avoid_hybrid_on();
        avoid_frame(w);
        Obst ob[MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS];
        unsigned char recip[MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS];
        int no = ctrl_obstacles(w, me, ignore_veh, ob, MA2_BOT_MAX_VEHICLES + MA2_WORLD_MAX_PLAYERS);
        float rad = body_r_of(me->actor, me->kind), dl = hdist2(dest, me->pos);
        int near_units = 0;
        for (int i = 0; i < no; i++) {
            float dd = hypotf(ob[i].x - me->pos.x, ob[i].z - me->pos.z), sep = dd - ob[i].r - rad;
            recip[i] = 0;
            if (ob[i].actor != ignore_veh && ob[i].actor != c->board_veh) {
                if (sep < c->min_sep) c->min_sep = sep;
                if (sep < 0.f && !c->in_contact) { c->contacts++; c->in_contact = 1;
                    if (env_on("RECOMP_BOT_DIAG")) {
                        float hr = -1.f;   /* a parked hull: its outline toward us */
                        for (int k = 0; k < w->nvehicles; k++)
                            if (w->vehicles[k].actor == ob[i].actor && !w->vehicles[k].occupied) hr = hull_at(&w->vehicles[k], hull_tall(me->kind), me->pos, -0.6f) - 0.6f;
                        fprintf(stderr, "[BOT-CONTACT] bot=%d kind=%d with %08X kind=%d sep=%.1f moving=%d t=%.1f d=%.1f hull=%.1f rad=%.1f\n", index, me->kind, ob[i].actor, ob[i].kind, sep, !ob[i].slow, w->time, dd, hr, rad);
                    } }
            }
            int parked = 0;
            for (int k = 0; k < w->nvehicles; k++) if (w->vehicles[k].actor == ob[i].actor && !w->vehicles[k].occupied) parked = 1;
            if (parked || ob[i].actor == ignore_veh || ob[i].actor == c->board_veh || dd > 45.f) { ob[i].kind = -1; continue; }
            /* Hybrid (C4): a teammate bot's course is where it is steering -- its call-out, "coming
             * through", the steadiest thing we have; anyone else's (humans, enemies) is predicted from
             * what is seen of them (the C1 track), not from the game's velocity. Off: every bot's
             * course is its steering point, everyone else's the raw velocity (as before). */
            const Ma2PlayerView *qv = NULL;
            for (int k = 0; k < w->nplayers; k++) if (w->players[k].actor == ob[i].actor && w->players[k].alive) { qv = &w->players[k]; break; }
            int mate_bot = qv && qv->is_bot && qv->nation == me->nation;
            if (!hyb || mate_bot) {
                for (int k = 0; k < g_rt.nbots; k++) {
                    const BotCtrl *oc = &g_rt.bots[k].ctrl;
                    if (oc->actor != ob[i].actor || oc->mode != CM_MOVE) continue;
                    float ox = oc->dest.x - ob[i].x, oz = oc->dest.z - ob[i].z, ol = hypotf(ox, oz), os = maxf2(hypotf(ob[i].vx, ob[i].vz), 0.6f * kind_cruise(ob[i].kind));
                    if (ol > 1.f) { ob[i].vx = ox / ol * os; ob[i].vz = oz / ol * os; }
                    if (w->time < oc->yield_until) { ob[i].vx = ob[i].vz = 0.f; }
                    else if (hyb && ol > 1.f) recip[i] = 1;          /* it steers round us too: each takes half */
                }
            } else if (qv) {
                Ma2TrackState ts;
                if (ma2_track_get(MA2_TRACK_AVOID, ob[i].actor, w->time, &ts) && ts.valid) {
                    float u = 0.f;
                    (void)ma2_track_predict(&ts, 1.0f, &u);
                    ob[i].vx = ts.vel.x; ob[i].vz = ts.vel.z;
                    ob[i].r += minf2(0.25f * (u - 1.0f), 1.0f);           /* a little room for doubt */
                    c7_add("avoid", "tracked", 1);
                }
                if (qv->is_bot && hypotf(ob[i].vx, ob[i].vz) > 2.f) recip[i] = 1;   /* an enemy bot dodges as we do */
            }
            near_units++;
        }
        if (near_units && dl > 1.f) {
            static const float offs[] = { 0.f, 0.17f, -0.17f, 0.35f, -0.35f, 0.6f, -0.6f, 0.9f, -0.9f, 1.22f, -1.22f };
            float base = atan2f(dest.x - me->pos.x, dest.z - me->pos.z);
            float spd = maxf2(hypotf(me->vel.x, me->vel.z), 0.7f * kind_cruise(me->kind));
            float best = 1e9f, best_off = 0.f, cost0 = 0.f, best_raw = 1e9f, best_raw_off = 0.f;
            int head_on_mate = -1;
            /* Reciprocal split: against a unit that dodges too, a candidate v counts as if we took
             * 2v - v_now (each side takes half the dodge). Standing still we take all of it. */
            float vax = me->vel.x, vaz = me->vel.z;
            int va_ok = hypotf(vax, vaz) > 1.f;
            /* Keep right: someone coming the other way ahead of us -- both pass on their right
             * (positive offset = toward +x of the course = right), so the two never mirror each other. */
            int headon = 0;
            if (hyb) {
                float bx = sinf(base), bz = cosf(base);
                for (int i = 0; i < no && !headon; i++) {
                    if (ob[i].kind < 0) continue;
                    float px = ob[i].x - me->pos.x, pz = ob[i].z - me->pos.z, ov = hypotf(ob[i].vx, ob[i].vz);
                    if (px * bx + pz * bz < 0.f || hypotf(px, pz) > 30.f || ov < 1.f) continue;
                    if ((ob[i].vx * bx + ob[i].vz * bz) / ov < -0.7f && fabsf(px * bz - pz * bx) < ob[i].r + rad + 4.f) headon = 1;
                }
            }
            /* Swerve map checks one frame old are reused on alternate frames when the steering runs
             * over its budget (avoid_frame), if we have hardly moved or turned since. */
            int use_cache = hyb && g_av.cache && (g_av.frame & 1) && c->av_trav_frame + 1 == g_av.frame &&
                            hdist2(me->pos, c->av_trav_pos) < 1.5f && fabsf(wrap_pi(base - c->av_trav_base)) < 0.08f;
            unsigned trav_mask = 0;
            for (int oi = 0; oi < (int)(sizeof offs / sizeof offs[0]); oi++) {
                float a = base + offs[oi], cvx = sinf(a) * spd, cvz = cosf(a) * spd;
                float cost = fabsf(offs[oi]) * 0.8f + (offs[oi] < 0.f ? (headon ? 0.25f + 0.3f * fabsf(offs[oi]) : 0.05f) : 0.f);
                for (int i = 0; i < no; i++) {
                    if (ob[i].kind < 0) continue;
                    float px = ob[i].x - me->pos.x, pz = ob[i].z - me->pos.z, R = ob[i].r + rad + 1.0f;
                    float mvx = cvx, mvz = cvz;
                    if (recip[i] && va_ok) { mvx = 2.f * cvx - vax; mvz = 2.f * cvz - vaz; }
                    float t = time_to_touch(px, pz, ob[i].vx - mvx, ob[i].vz - mvz, R);
                    if (t == 0.f) {                                   /* touching: heading into it is the worst */
                        float pl = hypotf(px, pz) + 1e-3f, into = (px * cvx + pz * cvz) / (pl * spd);
                        cost += into > 0.f ? 3.f * into : 0.f;
                    } else if (t > 0.f && t < 3.0f) cost += 4.f * (3.0f - t) / 3.0f;
                }
                if (oi > 0) {                                         /* a swerve must not run us into the map */
                    int blocked;
                    if (use_cache) blocked = (int)((c->av_trav_mask >> oi) & 1u);
                    else {
                        float look = minf2(dl, spd * 1.5f);
                        Ma2Vec3 q = { me->pos.x + sinf(a) * look, me->pos.y, me->pos.z + cosf(a) * look };
                        blocked = (ma2_trav_ready() && !(me->kind == MA2_KIND_MECH ? ma2_trav_segment_heavy(me->pos, q, rad * 0.7f, w->time)
                                                                                   : ma2_trav_segment(me->pos, q, rad * 0.7f, w->time)))
                                  || !hulls_clear(w, me, me->pos, q, ignore_veh);    /* nor into a parked hull */
                        if (blocked) trav_mask |= 1u << oi;
                    }
                    if (blocked) cost += 5.f;
                }
                if (cost < best_raw) { best_raw = cost; best_raw_off = offs[oi]; }
                /* ~0.5 s against flicking from one side to the other */
                if (hyb && c->av_side && w->time - c->av_side_t < 0.5 && offs[oi] * (float)c->av_side < 0.f) cost += 0.4f;
                if (oi == 0) cost0 = cost;
                if (cost < best) { best = cost; best_off = offs[oi]; }
            }
            if (hyb && !use_cache) { c->av_trav_mask = trav_mask; c->av_trav_frame = g_av.frame; c->av_trav_pos = me->pos; c->av_trav_base = base; }
            if (hyb && use_cache) c7_add("avoid", "cached", 1);
            if (hyb && best_raw_off * best_off < 0.f) c7_add("avoid", "held", 1);
            if (best_off != 0.f && cost0 > best + 0.1f) {
                float a = base + best_off, l = maxf2(dl, kind_min_carrot(me->kind));
                dest.x = me->pos.x + sinf(a) * l; dest.z = me->pos.z + cosf(a) * l;
                c->avoids++; c7_add("avoid", "swerves", 1);
                int side = best_off > 0.f ? 1 : -1;
                if (c->av_side && side != c->av_side && w->time - c->av_side_t < 0.5) c7_add("avoid", "flips", 1);
                c->av_side = side; c->av_side_t = w->time;
                if (headon && side > 0) c7_add("avoid", "keep_right", 1);
            }
            if (best > 3.0f) {                                        /* boxed in: someone waits */
                if (!hyb) {
                    for (int i = 0; i < no; i++) {
                        if (ob[i].kind < 0 || ob[i].team != me->nation) continue;
                        float px = ob[i].x - me->pos.x, pz = ob[i].z - me->pos.z;
                        if (hypotf(px, pz) > ob[i].r + rad + 8.f) continue;
                        for (int k = 0; k < g_rt.nbots; k++) if (g_rt.bots[k].ctrl.actor == ob[i].actor && k < index) head_on_mate = k;
                    }
                    if (head_on_mate >= 0) c->yield_until = w->time + 0.5;
                } else {
                    /* Right of way by urgency, not bot-list order: a human teammate always; then the
                     * flag carrier, the badly hurt, and the one already rolling through the gap. A
                     * teammate already waiting is not waited for (it waits for us). */
                    float myp = avoid_urgency(w, me);
                    uint32_t for_actor = 0;
                    for (int i = 0; i < no; i++) {
                        if (ob[i].kind < 0 || ob[i].team != me->nation) continue;
                        float px = ob[i].x - me->pos.x, pz = ob[i].z - me->pos.z;
                        if (hypotf(px, pz) > ob[i].r + rad + 8.f) continue;
                        const Ma2PlayerView *qv = NULL;
                        for (int k = 0; k < w->nplayers; k++) if (w->players[k].actor == ob[i].actor && w->players[k].alive) { qv = &w->players[k]; break; }
                        if (!qv) continue;
                        float op = 100.f;
                        if (!qv->is_human) {
                            int k = 0;
                            while (k < g_rt.nbots && g_rt.bots[k].ctrl.actor != ob[i].actor) k++;
                            if (k >= g_rt.nbots || w->time < g_rt.bots[k].ctrl.yield_until) continue;
                            /* ChokeWaitYield (default on, RECOMP_BOT_CHOKE_WAIT_YIELD / [Bots] ChokeWaitYield; parity pass 8): a teammate standing at a
                             * choke mouth for us (brain choke-wait, order.yield_wait) is waiting already, but its ctrl.yield_until is not set, so the
                             * oncoming body waited for it in turn: S_71bT (Ill Wind Check It) choke 4, b1 'waits at choke 4 (1 oncoming)' 37.5-44.4 s
                             * for b0 while b0 sat in BOT-WAIT for b1 38.4-43.8 (108 lines, both 'Mechs ~0-9 m/s for 6 s). It goes round it. [C7] chokes: waiter_passed. */
                            if (g_rt.bots[k].order.yield_wait && c7_on("chokewaityield", "RECOMP_BOT_CHOKE_WAIT_YIELD", "ChokeWaitYield", 1)) { c7_add("chokes", "waiter_passed", 1); continue; }
                            op = avoid_urgency(w, qv);
                        }
                        if (op > myp || (op == myp && qv->actor < me->actor)) {
                            if (yield_timeout_on() && qv->actor == c->yw_skip && w->time < c->yw_skip_until) continue;   /* YieldTimeout: already waited it out */
                            for_actor = qv->actor; break;
                        }
                    }
                    /* YieldTimeout (default on, RECOMP_BOT_YIELD_TIMEOUT / [Bots] YieldTimeout): the right-of-way wait is renewed every 0.5 s for as long as the
                     * teammate ahead stays put -- and a teammate that is itself holding a firing stance never moves aside: swA60 CTF, a 'Mech stood 34 s at
                     * 498,911 under fire (BOT-WAIT 1742 lines, urgency 0.00, three mechs in a knot). After 2.5 s of waiting for the same teammate it is
                     * not waited for again for 8 s: the avoidance swerve takes the bot round it. [C7] yieldtimeout: skips. */
                    if (for_actor && yield_timeout_on()) {
                        if (c->yw_actor != for_actor || w->time - c->yw_last > 1.2) { c->yw_actor = for_actor; c->yw_t0 = w->time; }
                        c->yw_last = w->time;
                        if (w->time - c->yw_t0 > 2.5) {
                            c->yw_skip = for_actor; c->yw_skip_until = w->time + 8.0; c->yw_actor = 0; c7_add("yieldtimeout", "skips", 1);
                            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-YIELDTO] t=%.1f bot=%d stops waiting for %08X at %.0f,%.0f\n", w->time, index, for_actor, me->pos.x, me->pos.z);
                            for_actor = 0;
                        }
                    }
                    if (for_actor) {
                        c->yield_until = w->time + 0.5; c7_add("avoid", "waits", 1);
                        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-WAIT] t=%.1f bot=%d for=%08X at=%.0f,%.0f urgency=%.2f\n", w->time, index, for_actor, me->pos.x, me->pos.z, myp);
                    }
                }
            }
        }
        if (c->in_contact) {
            int any = 0;
            for (int i = 0; i < no; i++) if (ob[i].actor != ignore_veh && ob[i].actor != c->board_veh && hypotf(ob[i].x - me->pos.x, ob[i].z - me->pos.z) - ob[i].r - rad < 0.5f) any = 1;
            c->in_contact = any;
        }
        g_av.frame_ms += c7_t0() - av_t0;
        c7_ms("avoid", "avoid", av_t0);                    /* -> avoid_ms / avoid_max per frame, all bots */
    }
    /* A 'Mech walks only while the point it walks to lies within some 15-20 degrees of its course:
     * past that it stops dead, turns on the spot and sets off again (measured: walk, anim 18, idle,
     * start, walk). A steering point that swings round a corner or aside for a teammate made it
     * shuffle. So while it walks the point leads its course by at most 12 degrees -- it keeps
     * striding and curves round; only a real about-turn is made standing. */
    if (me->kind == MA2_KIND_MECH && w->time >= c->upjet_until) {
        float sp = hypotf(me->vel.x, me->vel.z), dl2 = hdist2(dest, me->pos);
        if (sp > 3.f && dl2 > 2.f) {
            float course = atan2f(me->vel.x, me->vel.z), want = atan2f(dest.x - me->pos.x, dest.z - me->pos.z);
            float dlt = want - course;
            while (dlt > 3.14159265f) dlt -= 6.2831853f;
            while (dlt < -3.14159265f) dlt += 6.2831853f;
            if (fabsf(dlt) > 0.21f && fabsf(dlt) < 2.0f) {
                float a = course + (dlt > 0.f ? 0.21f : -0.21f), l = maxf2(dl2, kind_min_carrot(me->kind));
                /* Not where the wide arc runs into a parked hull (round a VTOL's wing the carrot
                 * swings late): there the turn is made, standing if need be. */
                float ln = minf2(l, sp * 2.f + 4.f);
                Ma2Vec3 nq = { me->pos.x + sinf(a) * ln, me->pos.y, me->pos.z + cosf(a) * ln };
                if (hulls_clear(w, me, me->pos, nq, ignore_veh)) { dest.x = me->pos.x + sinf(a) * l; dest.z = me->pos.z + cosf(a) * l; }
            }
        }
    }
    if (air && vt_edge_all_on()) {   /* VtolEdgeAll: the carrot / end overshoot */
        Ma2Vec3 d0 = dest;
        if (vt_keep_in(&dest, vkeep) > 0) {
            c7_add("vtedgeall", "dest_in_s", w->dt);
            /* EdgeBendAhead: the kept-in carrot must still lead the craft. The keep-off widens with height (53 m at 48 m up), and by
             * New River City's blue base (a corner of the map) every carrot ahead was pulled back to 1-4 m from the craft: the flag-run
             * VTOL hovered there with its rider for 90 s and the run timed out (r1 11-04 bot 4, t=18-102, [BOT-VCRAWL] dest=1-4 m). */
            if (c7_on("edgebendahead", "RECOMP_BOT_EDGE_BEND_AHEAD", "EdgeBendAhead", 1) && hdist2(d0, me->pos) > 1.f) {
                float ax = d0.x - me->pos.x, az = d0.z - me->pos.z, bx = dest.x - me->pos.x, bz = dest.z - me->pos.z, la = hypotf(ax, az), lb = hypotf(bx, bz);
                if (lb < minf2(10.f, la) || (ax * bx + az * bz) < 0.5f * la * lb) { dest = d0; c7_add("vtedgeall", "dest_in_refused_s", w->dt); }
            }
        }
    }
    c->dest = dest;
    c->mode = CM_MOVE;
    if (air) {
        c->vt_rider = 0;
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++)
            if (w->players[j].alive && w->players[j].riding == me->actor) { c->vt_rider = 1; break; }
        vtol_prepare(c, me, dest, w->time);
        vtol_track(c, w, me, goal);
        vt_slow_note(index, b, c, w, me, goal, stop, d);
    }
    c->vt_lift_demand = 0.f;                                  /* ModClimb: set again by vtol_manual while it climbs */
    if (air && env_on("RECOMP_BOT_VTOL_TRACE") && (w->time - c->vt_trace_t > 1.0 || w->time < c->vt_trace_t)) {
        uint32_t ai2 = actor_ai(me->actor);
        c->vt_trace_t = w->time;
        fprintf(stderr, "[BOT-VTRACE] t=%.1f bot=%d pos=%.0f,%.0f,%.0f sp=%.1f carrot=%.0f,%.0f(%.0f) goal=%.0f,%.0f(%.0f) alt=%.0f boost=%.0f moveto=%d awake=%u mode=%u path=%d/%d a248=%08X c144=%08X cA0=%.0f cB0=%.1f rel=%.0f ceil=%.1f ymax=%.1f rider=%d low=%d need=%.0f rise=%.0f\n",
                w->time, index, me->pos.x, me->pos.y, me->pos.z, hypotf(me->vel.x, me->vel.z), dest.x, dest.z, hdist2(dest, me->pos), goal.x, goal.z, hdist2(goal, me->pos),
                c->vt_alt, c->vt_boost, w->time < c->vt_moveto_until, ai2 ? MEM8(ai2 + 0x90) : 9u, ai2 ? MEM32(ai2 + 0x12C) : 99u, c->path.next, c->have_path ? c->path.n : 0,
                MEM32(me->actor + 0x248), gva_ok(MEM32(me->actor + 0x8C), 0x150) ? MEM32(MEM32(me->actor + 0x8C) + 0x144) : 0u,
                gva_ok(MEM32(me->actor + 0x8C), 0x150) ? MEMF(MEM32(me->actor + 0x8C) + 0xA0) : 0.f, gva_ok(MEM32(me->actor + 0x8C), 0x150) ? MEMF(MEM32(me->actor + 0x8C) + 0xB0) : 0.f,
                gva_ok(MEM32(me->actor + 0x94), 0x100) ? (atan2f(dest.x - me->pos.x, dest.z - me->pos.z) - atan2f(MEMF(MEM32(me->actor + 0x94) + 0xE0), MEMF(MEM32(me->actor + 0x94) + 0xE8))) * 57.3f : 999.f, c->vt_ceiling, g_vt_ymax, c->vt_rider, c->vt_low_pref, c->vt_need, c->vt_rise);
    }
    /* AiKick (default on, RECOMP_BOT_AI_KICK / [Bots] AiKick): players want no bot standing about. Sweep WP2 (11-02): a 'Mech just boarded stood
     * 6-22 s in its spawn lane with the awake goto drive running (sw44 bot 9 22 s and ejected, sw85 bot 7, sw65 bot 10 at 1536,1448: yaw turned
     * to each new carrot at the 6 s stall tick and then froze, the game's AI mode-6 handler not walking). What freed the ones that got free
     * (sw44 bots 5 and 7) was a 0.5 s yield (body_stop: AI mode 7) before the next goto drive set mode 6 again. So a ground walker that has not
     * moved 1 m in 1.6 s with its carrot 6 m+ off is given that yield on purpose, up to 3 times a spell, and it is checked (moved 3 m within 5 s:
     * freed). [C7] aikick: kicks / freed / failed. */
    {   /* RECOMP_TEST_TELEPORT=<s>: once, the first pilot on a travel leg after that game time is set down 5 m toward its carrot (proves body_teleport; logs before / after) */
        static int tt_done; static double tt_after = -2.0;
        if (tt_after < -1.0) tt_after = env_int("RECOMP_TEST_TELEPORT", 0) > 0 ? (double)env_int("RECOMP_TEST_TELEPORT", 0) : -1.0;
        if (tt_after > 0.0 && !tt_done && me->kind == MA2_KIND_PILOT && !air && w->time > tt_after && hdist2(me->pos, dest) > 8.f) {
            Ma2Vec3 tp = me->pos; float l = hdist2(me->pos, dest);
            tp.x += (dest.x - me->pos.x) / l * 5.f; tp.z += (dest.z - me->pos.z) / l * 5.f; tp.y += 0.3f; tt_done = 1;
            fprintf(stderr, "[TEST-TELEPORT] t=%.2f bot=%d from %.2f,%.2f,%.2f to %.2f,%.2f,%.2f\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, tp.x, tp.y, tp.z);
            body_teleport(me->actor, tp);
            return 0;
        }
    }
    {   /* RECOMP_TEST_BOT_SET="<s>,<x>,<z>[,<kind>]": once, the first bot of that kind (default 'Mech) on a travel leg after that game time is set down at x,z on the surface there
         * (proves HopDown on the Rush Hour deck without waiting for the chance; logs before / after) */
        static int ts_done; static double ts_t = -2.0, ts_x, ts_z; static int ts_kind = MA2_KIND_MECH; static double ts_gx, ts_gz;
        if (ts_t < -1.0) {
            const char *e = getenv("RECOMP_TEST_BOT_SET"); double t = 0, x = 0, z = 0, gx = 0, gz = 0; int k = 0;
            int n = e ? sscanf(e, "%lf,%lf,%lf,%d,%lf,%lf", &t, &x, &z, &k, &gx, &gz) : 0;
            if (n >= 3) { ts_t = t; ts_x = x; ts_z = z; if (n >= 4) ts_kind = k; ts_gx = n >= 6 ? gx : 0.0; ts_gz = gz; } else ts_t = -1.0;
        }
        if (ts_t > 0.0 && !ts_done && me->kind == ts_kind && !air && w->time > ts_t) {
            int oks = 0; float sy = surface_height((float)ts_x, (float)ts_z, &oks);
            if (oks) {
                Ma2Vec3 tp = { (float)ts_x, sy + 0.5f, (float)ts_z }; ts_done = 1; if (ts_gx != 0.0) { g_ts_bot = index; g_ts_gx = (float)ts_gx; g_ts_gz = (float)ts_gz; }
                fprintf(stderr, "[TEST-SET] t=%.2f bot=%d from %.2f,%.2f,%.2f to %.2f,%.2f,%.2f\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, tp.x, tp.y, tp.z);
                body_teleport(me->actor, tp);
                return 0;
            }
        }
    }
    if (me->kind == MA2_KIND_PILOT && !air && !hold_end && frozen_pilot_on() && hdist2(me->pos, goal) >= 15.f) {   /* FrozenPilot, see above ctrl_travel */
        if (c->fz_wig_until > 0.0 && w->time < c->fz_wig_until) {
            if (hdist2(me->pos, c->fz_pos0) > 1.5f) {
                c7_add("frozenpilot", "freed", 1); c->fz_wig_until = 0.0; c->fz_n = 0; c->fz_pos = me->pos; c->fz_t = w->time; c->fz_dir_until = w->time + 4.0;
                c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
            } else {
                int hk = ((int)((w->time - c->fz_wig_t) / 1.1)) & 7;   /* heading 0 = the carrot, then round the compass */
                float ha = atan2f(dest.x - me->pos.x, dest.z - me->pos.z) + (float)hk * 0.785398f; Ma2Vec3 to = me->pos; to.x += sinf(ha) * 10.f; to.z += cosf(ha) * 10.f;
                body_walk_direct(me->actor, me->pos, to, 100); c->dest = to; c->mode = CM_MOVE;
                c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 1.0; c7_add("frozenpilot", "wiggle_s", w->dt);
                return 0;
            }
        } else if (c->fz_wig_until > 0.0) {                                  /* the wiggle is over and it has not moved: set it down on its route */
            c->fz_wig_until = 0.0;
            Ma2Vec3 tp;
            if (c->fz_n <= 3 && frozen_tele_pt(c, me, &tp)) {
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-FROZEN] t=%.1f bot=%d still at %.1f,%.1f,%.1f after the wiggle: set down at %.1f,%.1f,%.1f (%d)\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, tp.x, tp.y, tp.z, c->fz_n);
                body_teleport(me->actor, tp); c7_add("frozenpilot", "teleports", 1);
                c->fz_pos = tp; c->fz_t = w->time; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 0.5;
                return 0;
            }
            c7_add("frozenpilot", "no_target", 1);
            c->fz_pos = me->pos; c->fz_t = w->time;
        }
        if (w->time < c->fz_dir_until && c->fz_wig_until <= 0.0) {          /* freed a moment ago: MoveTo froze it, so it keeps its own drive toward the carrot a few seconds */
            body_walk_direct(me->actor, me->pos, dest, 100); c->mode = CM_MOVE; c7_add("frozenpilot", "direct_s", w->dt);
            return 0;
        }
        /* KickNoReset (default on, RECOMP_BOT_KICK_NO_RESET / [Bots] KickNoReset): an AiKick's own 0.5 s stop (yield_until) restarted this
         * 3 s clock, so a pilot respawned inside the human's BattleArmor (r5_01 Grinder NRC b0 188.2-198.1) stood frozen 10 s through three
         * kicks before the wiggle began. A yield the kick set does not count. */
        int kyield = w->time < c->yield_until && !(w->time - c->kk_last < 0.6 && w->time >= c->kk_last &&
                                                     c7_on("kicknoreset", "RECOMP_BOT_KICK_NO_RESET", "KickNoReset", 1));
        if (c->fz_t <= 0.0 || w->time < c->fz_t || hdist2(me->pos, c->fz_pos) > 0.5f || kyield) {
            if (c->fz_t > 0.0 && hdist2(me->pos, c->fz_pos) > 3.f) c->fz_n = 0;
            c->fz_pos = me->pos; c->fz_t = w->time;
        } else if (w->time - c->fz_t > 3.0) {
            c->fz_n++; c->fz_t = w->time; c->fz_pos0 = me->pos; c->fz_wig_t = w->time; c->fz_wig_until = w->time + 8.8; c->fz_k0 = (c->fz_n + (int)me->player) & 7;
            c7_add("frozenpilot", "spells", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-FROZEN] t=%.1f bot=%d has not moved 0.5 m in 3 s at %.1f,%.1f,%.1f (carrot %.0f,%.0f): wiggling (%d)\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, dest.x, dest.z, c->fz_n);
        }
    }
    if ((me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR || (me->kind == MA2_KIND_PILOT && frozen_pilot_on())) && !air && !hold_end && ai_kick_on()) {
        if (c->kk_chk > 0.0) {
            if (hdist2(me->pos, c->kk_pos0) > 3.f) { c7_add("aikick", "freed", 1); c->kk_chk = 0.0; c->kk_n = 0; }
            else if (w->time > c->kk_chk || w->time < c->kk_chk - 6.0) { c7_add("aikick", "failed", 1); c->kk_chk = 0.0; }
        }
        /* KickFiring (default on, RECOMP_BOT_KICK_FIRING / [Bots] KickFiring): a body standing to fire (at a plant, from a firing spot) is not stuck:
         * r1_93 Base War Rush Hour, all 4 failed kicks were Mechs shooting the enemy plant from 64-69 m, and the 0.5 s stop broke their fire. */
        /* KickPinned: only that case -- an order shooting a structure. Firing on the move hid real pins: in Not It every body fires all match,
         * and r4_50 b8 stood 4.3 s frozen on a wall (850 -> 570 hp), b9 6 s pinned on the pit wall and died, with no kick. */
        int kfire = me->firing && c7_on("kickfiring", "RECOMP_BOT_KICK_FIRING", "KickFiring", 1) &&
                    (b->order.structure || hdist2(me->pos, b->order.point) < 15.f || b->order.type == MA2_ORD_ENGAGE ||
                     !c7_on("kickpinned", "RECOMP_BOT_KICK_PINNED", "KickPinned", 1));
        /* (ENGAGE: the duel's point is the target, not a place to go -- d5_61 b6 82.0 was kicked off its firing stand at an enemy 90 m away) */
        /* KickHold: a hold at its own exact point with a target (hold-high: a mortar post fires once in ~3 s) is a hold between shots too --
         * d5_62c: 5 of 22 kicks broke such posts (b6 131.1 / 139.1 / 142.0 at 527,14,639). */
        if (!kfire && b->order.type == MA2_ORD_FIGHT_MOVE && b->order.target && b->order.exact && hdist2(me->pos, b->order.point) < 6.f &&
            c7_on("kickpinned", "RECOMP_BOT_KICK_PINNED", "KickPinned", 1)) kfire = 1;
        /* (a firing stand is a deliberate hold when its order point is where it stands -- hold-high posts, wary-blast-wait: 0 m in r5_13, where
         * kicks broke 12 of them; the pins were 90-200 m from theirs) */
        if (kfire || c->kk_t <= 0.0 || w->time < c->kk_t || hdist2(me->pos, c->kk_pos) > 1.f || hdist2(me->pos, dest) < (me->kind == MA2_KIND_PILOT ? 3.f : 6.f) || w->time < c->yield_until || w->time < c->back_until) {
            if (c->kk_t > 0.0 && hdist2(me->pos, c->kk_pos) > 3.f) c->kk_n = 0;
            c->kk_pos = me->pos; c->kk_t = w->time;
        } else if (w->time - c->kk_t > 1.6 && w->time - c->kk_last > 2.5 && c->kk_n < 3) {
            c->kk_last = w->time; c->kk_n++; c->kk_t = w->time; c->kk_pos0 = me->pos; if (c->kk_chk <= 0.0) c->kk_chk = w->time + 5.0;
            c->yield_until = w->time + 0.5; c7_add("aikick", "kicks", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-AIKICK] t=%.1f bot=%d kind=%d still at %.0f,%.0f,%.0f, carrot %.0f,%.0f (%d)\n", w->time, index, me->kind, me->pos.x, me->pos.y, me->pos.z, dest.x, dest.z, c->kk_n);
        }
    }
    /* JetOverYield (default on, RECOMP_BOT_JET_OVER_YIELD / [Bots] JetOverYield; parity pass 10, 11-11): a jet started this frame or still armed
     * (MechAI ai+0x240) was held on the ground by the right-of-way stop (body_stop -> mode 7) until the wait ran out: X9_81a1 b5 (Ill Wind
     * Snatch It T2) LeapStall jet at 36.3 s at the exit ledge 713,-1,1394, armed 36.34-37.17 standing behind a teammate, renewed waits; 0.03-1.07 s
     * per such hold, both teams, 18 holds over 12 recordings (yieldjet_proof.py). A body taking off leaves the teammate's lane: no wait. [C7]
     * jetoveryield: n. */
    if (w->time < c->yield_until && jet_over_yield_on() && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR)) {
        uint32_t jai = actor_ai(me->actor);
        if (jai && MEM8(jai + 0x240)) { c->yield_until = 0.0; c7_add("jetoveryield", "n", 1); }
    }
    if (w->time < c->yield_until) { body_stop(me->actor); return 0; }
    /* Final leg on foot: the planner's last segment is free; walk it with the body's own turn +
     * speed (MoveTo's local avoidance orbits a point beside a hull instead of reaching it). */
    /* The last 25 m on foot are walked along our own route with the body's own turn and speed.
     * MoveTo stops a pilot well short of a point beside a parked hull (a VTOL's wings and belly
     * reach past what it lets a body near) and never takes the last steps to the entry. */
    /* PilotRunIn (default on, RECOMP_BOT_PILOT_RUN_IN / [Bots] PilotRunIn): the direct walk moves a pilot at ~2 m/s (walk pace),
     * so 25 m of it took 10-13 s at the start of a match (laptop 11-01: "pilots walk around slowly"). MoveTo runs it in;
     * the direct walk only for the last 6 m beside the hull, where MoveTo stops short. */
    if (me->kind == MA2_KIND_PILOT && c->have_path && c->path.n > 0 &&
        hdist2(c->path.pt[c->path.n - 1], me->pos) < (c7_on("pilotrunin", "RECOMP_BOT_PILOT_RUN_IN", "PilotRunIn", 1) ? 6.f : 25.f)) {
        Ma2Vec3 fin = c->path.pt[c->path.n - 1];
        body_walk_direct(me->actor, me->pos, hulls_clear(w, me, me->pos, fin, ignore_veh) ? fin : carrot, 100);
    /* LowStick (default on, RECOMP_BOT_VTOL_LOW_STICK / [Bots] LowStick): "6 m up" was world height -- New River City's ground is ~0.
     * Bogged Down's bog lies at -16..-12, so a craft over it never got the stick: v29f2 (BD CTF, blue bot 7, t=165.5-171) a pick-up sank
     * on the awake drive into the bog, the rider clawed on and the craft could not climb (lift-climb, 0 m/s, vy -5) -- 12 s, the rider off
     * four times. Now 6 m over the ground under it counts too. [C7] lowstick: n_s. */
    } else if (air && env_int("RECOMP_BOT_VTOL_MANUAL", 1) && (me->pos.y > 6.f || vt_low_stick(w, me)) && w->time >= c->vt_moveto_until &&
               /* StickClimb (default on, RECOMP_BOT_VTOL_STICK_CLIMB / [Bots] StickClimb): the stick's vertical does not lift the
                * craft (full up, target 24 -> 134 m, held 13 m at 95 m/s into the hill: laptop + desktop ferry 10-31) -- only MoveTo
                * works the height. With climbing to do, MoveTo flies it (the branch below); the stick only at cruise height. */
               !(env_int("RECOMP_BOT_VTOL_CLIMB_MODE", 1) == 1 && c7_on("stickclimb", "RECOMP_BOT_VTOL_STICK_CLIMB", "StickClimb", 1) &&
                 vt_climb_wanted(c, me))) {
        c->vt_climb = 0;   /* (it stayed set while the stick flew: a level cruise read as a climb that gained nothing -- the bogus caps) */
        vt_rider_dodge(index, c, w, me, &dest);                  /* RiderDodge: round what the rider cannot clear */
        vtol_manual(index, c, w, me, dest); c->va_branch = 'S';
        if (goal.y > me->pos.y + 3.f && vt_climb_up_on() && vt_mod_climb_on()) {   /* VtolClimbUp: climb to it, easing on near it */
            c->vt_lift_actor = me->actor; c->vt_lift_demand = 30.f; c7_add("vtclimbup", "climb_s", w->dt); c->vs_flags |= 1;
            if (hdist2(goal, me->pos) < 30.f && c->axis[4] > 0.3f) c->axis[4] = 0.3f;
        }
        vt_arrive(index, c, w, me, goal, stop);           /* VtolArrive: brake (and with a rider sink) for an exact stop */
    } else if (air && (
               /* The awake drive also parks now and then with its point dead ahead and the height
                * right (never explained); under 2 m/s for 1.5 s with the point well away, let MoveTo
                * fly it for 4 s rather than wait out the 4 s no-headway check. */
               vt_park_window(c, w, me, dest) ||
               w->time < c->vt_moveto_until ||
                       (c->vt_climb = vt_climb_wanted(c, me)))) {
        /* The awake FlyerAI is fast but holds the height it has (it flew a whole leg at 23 m
         * whatever the cruise asked, and parked at the first taller building); only the game's
         * MoveTo works the altitude (ai+0x20C). So: MoveTo while there is climbing to do, the
         * fast drive once up at the cruise height. */
        body_moveto(me->actor, vt_route_look_on() ? vt_route_point(c, me, dest, 150.f) : dest); if (air) c->va_branch = w->time < c->vt_moveto_until ? 'W' : 'M';
    } else {
        if (air && c->vt_alt > 0.f) dest.y = c->vt_alt;
        body_steer(me, dest); if (air) c->va_branch = 'A';
    }
    /* A travel hop over built-up ground comes down on a roof as often as not, and the walk back off
     * it costs more than the hop saved: flag carriers spent half a minute on the blue and red
     * bases' roofs (runs tk1, tk2). Ground checked every 8 m along the hop: a structure (a surface
     * 3 m over the bare terrain) means walk. Checked at most once a second. */
    int hop_ok = 1;
    if ((me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && w->time >= c->jump_next && w->time >= c->hop_check_t) {
        float hx = dest.x - me->pos.x, hz = dest.z - me->pos.z, hl = hypotf(hx, hz), reach = me->kind == MA2_KIND_MECH ? 95.f : 60.f;
        c->hop_check_t = w->time + 1.0;
        for (float sd = 8.f; hl > 1.f && sd <= reach && hop_ok; sd += 8.f) {
            int ok1 = 0, ok2 = 0;
            float px = me->pos.x + hx / hl * sd, pz = me->pos.z + hz / hl * sd;
            float sf = surface_height(px, pz, &ok1), th = terrain_height(px, pz, &ok2);
            if (ok1 && ok2 && sf > th + 3.f) hop_ok = 0;
            /* JumpLand (default on, RECOMP_BOT_JUMP_LAND / [Bots] JumpLand): ground rising 4 m+ over the body on the hop line is a mound or a
             * ridge the jet comes down on: au3_20 bot 3 hopped a leg at y -12 (t=12.9, jet dir -31.5 deg) and landed 20 m on at y 5.1, where
             * MoveTo would not walk it down (GotoWalk). Walk instead. [C7] jumpland: refused. */
            else if ((ok1 || ok2) && (ok1 && (!ok2 || sf > th) ? sf : th) > me->pos.y + 4.f && c7_on("jumpland", "RECOMP_BOT_JUMP_LAND", "JumpLand", 1)) { hop_ok = 0; c7_add("jumpland", "refused", 1); }
        }
        c->hop_ok = hop_ok;
    } else hop_ok = c->hop_ok;
    /* BattleArmor walks like the rest; it jets over long open legs now and then (and whenever
     * walking makes no headway for a moment -- a ledge, a slope too steep to walk). */
    /* DeckJet (default on, RECOMP_BOT_DECK_JET / [Bots] DeckJet): the jet at a raised goal waited for 35 m, but a route that ends short under a
     * deck can end farther out: Snatch It Rush Hour's core deck (y 55 over the y 30 street) -- p19_85a b4 held 46 m off at 1255,30,1345 for 66 s
     * (116-182), b0 / b5 the same at 46-56 m (rc4_85a b7 / b2 / b5 too). A 'Mech jump covers 57-95 m and climbs 22-26 m (JETGAIN): from a
     * partial route's end, 60 m. [C7] deckjet: n. */
    float up_r = hold_end && c7_on("deckjet", "RECOMP_BOT_DECK_JET", "DeckJet", 1) ? 60.f : 35.f;
    if (me->kind == MA2_KIND_MECH && goal_rise(goal, me) > 3.f && hdist2(me->pos, goal) < up_r && w->time >= c->jump_next && up_allowed(c, w, me) &&
        !hot_upjet_hold(c, w, me) && body_jump_up(me->actor, me->pos, goal, 1.9f)) { c->jumps++; c->jump_next = w->time + 3.0; c->upjet_until = w->time + 4.0; c->upjets++; c->upjet_goal = goal; }
    if (me->kind == MA2_KIND_BATTLEARMOR && w->time >= c->jump_next) {
        /* It walks; long open legs are hopped (BaWalk; 0 = the old jets-first, a hop whenever
         * there is fuel). A goal up on a roof or a ledge close by is jetted straight at. */
        int up = goal_rise(goal, me) > 3.f && hdist2(me->pos, goal) < 30.f && up_allowed(c, w, me), bw = ba_walk_on();
        if (up ? body_jump_up(me->actor, me->pos, goal, 1.5f)
               : (hop_ok && !nopath && hdist2(me->pos, goal) > (bw ? 90.f : 50.f) && hdist2(me->pos, dest) > 10.f && !heat_jet_hold(w, me) &&
                  /* a hop no longer than the way left to go (some 24 m a second of burn): never
                   * over the vehicle it is walking to and off the deck beyond (BaWalk: 40 m short of it) */
                  body_jump_fuel_max(me->actor, me->pos, dest, 1.6f, maxf2(0.5f, (hdist2(me->pos, goal) - (bw ? 40.f : 15.f)) / 24.f)))) {
            c->jumps++; c->jump_next = w->time + (bw ? 4.0 : 2.5);  /* one burn, then a few steps: not a stutter of short hops */
            if (bw && !up) c7_add("bawalk", "travel_hops", 1);
            if (up) { c->upjet_until = w->time + 4.0; c->upjets++; c->upjet_goal = goal; body_steer(me, goal); c->dest = goal; }
        }
    }
    /* MechClimbJet (default on, RECOMP_BOT_MECH_CLIMB_JET / [Bots] MechClimbJet; parity pass 11-08): a jet 'Mech that makes no headway walking UP
     * toward its next point jets at it, as a player does when the 'Mech bogs down on a slope. p2_81a3 (Snatch It Ill Wind, team 2): b5 / b10 walked
     * into the y -18 basin under the core bank at 45 s and slid back down its 0.41-0.52 exit ramp (762,1414 -> 720,1414, 15 m up) for 70 s
     * (AIKICKs, replans every few s, heat 16-19 from hops that never went up), while team 1 had scored. Uphill only (the next point or the ground
     * 10 m on 2 m+ higher), 1.2 s with < 0.9 m moved and < 1 m risen, a burn aimed 3 m over the next point no farther than 30 m. [C7] mechclimbjet: jets. */
    /* ClimbProgress (same switch, 11-08 evening): also a body that keeps MOVING without getting nearer its next route point for 2.5 s (circling under
     * the ledge), and BattleArmor too: p4_84a1 b7 (BA, On the Rocks Snatch It) circled 25 s at 930-980,61,1360-1395 under the y 78 core plateau
     * (route up a 0.5 slope; BaUnstick never fired, it was never still). [C7] mechclimbjet: jets, jets_ba, progress. */
    if ((me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && !air && !hold_end && c->have_path && c->path.next < c->path.n &&
        c7_on("mechclimbjet", "RECOMP_BOT_MECH_CLIMB_JET", "MechClimbJet", 1) && (me->kind == MA2_KIND_BATTLEARMOR || ma2_bot_has_jets(me->actor))) {
        Ma2Vec3 npt = c->path.pt[c->path.next];
        float nd = hdist2(me->pos, npt);
        /* the clock restarts on progress (2 m nearer the point, 1 m higher) or a new point; "still" = not moved 0.9 m since it started */
        /* ClimbAhead (default on, RECOMP_BOT_CLIMB_AHEAD / [Bots] ClimbAhead; parity pass 11-10): (1) a replan that moves the next corner a few metres
         * is the same climb, not a new one -- the clock restarted on any 2 m move, and replans every second under the Ill Wind core bank kept it from
         * ever running out: P1_81b1 b8 (team 2) milled at 733-765,-17,1400-1453 from 42 to 68.8 s before its first climb jet. A new point now means
         * 12 m+ away. (2) a climb steeper than 0.35 rising 6 m+ within 30 m is jetted at once, as a player does, instead of walked into a stall
         * (the basin's exit ramp is 0.41-0.52; 'Mechs stop on 30 % of uphill steps there). [C7] mechclimbjet: ahead. */
        static int cah = -1; if (cah < 0) cah = c7_on("climbahead", "RECOMP_BOT_CLIMB_AHEAD", "ClimbAhead", 1);
        if (c->mcu_t <= 0.0 || w->time < c->mcu_t || me->pos.y > c->mcu_pos.y + 1.f || hdist2(npt, c->mcu_np) > (cah ? 144.f : 4.f) || nd < c->mcu_d - 2.f) {
            c->mcu_t = w->time; c->mcu_d = nd; c->mcu_np = npt; c->mcu_pos = me->pos;
        }
        int still = hdist2(me->pos, c->mcu_pos) <= 0.9f;
        double el = w->time - c->mcu_t;
        int ahead = 0;
        if (cah && me->kind == MA2_KIND_MECH && nd > 3.f && w->time >= c->jump_next && w->time >= c->yield_until) {
            float dl = nd < 30.f ? nd : 30.f; int ok = 0;
            float th = terrain_height(me->pos.x + (npt.x - me->pos.x) * dl / nd, me->pos.z + (npt.z - me->pos.z) * dl / nd, &ok);
            ahead = ok && th > me->pos.y + 6.f && th - me->pos.y > 0.35f * dl;
        }
        if (((still && el > 1.2) || el > 2.5 || ahead) && w->time >= c->jump_next && w->time >= c->yield_until && nd > 3.f) {
            Ma2Vec3 tp = npt; int okh = 0, ok10 = 0;
            float dl = nd, f10 = minf2(1.f, 10.f / maxf2(dl, 1.f));
            if (dl > 30.f) { tp.x = me->pos.x + (npt.x - me->pos.x) * 30.f / dl; tp.z = me->pos.z + (npt.z - me->pos.z) * 30.f / dl; }
            float th = terrain_height(tp.x, tp.z, &okh);
            float h10 = terrain_height(me->pos.x + (npt.x - me->pos.x) * f10, me->pos.z + (npt.z - me->pos.z) * f10, &ok10);
            if (okh) tp.y = th;
            if ((okh && th > me->pos.y + 2.f) || (ok10 && h10 > me->pos.y + 2.f)) {
                if (body_jump_up(me->actor, me->pos, tp, me->kind == MA2_KIND_BATTLEARMOR ? 1.2f : 1.5f)) {
                    c->jumps++; c->jump_next = w->time + 2.5; c->mcu_t = w->time; c->mcu_d = nd; c->mcu_pos = me->pos;
                    c7_add("mechclimbjet", me->kind == MA2_KIND_BATTLEARMOR ? "jets_ba" : "jets", 1); if (!still) c7_add("mechclimbjet", "progress", 1);
                    if (ahead && el <= 2.5 && !still) c7_add("mechclimbjet", "ahead", 1);
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CLIMBJET] t=%.1f bot=%d kind=%d %s uphill at %.0f,%.0f,%.0f: jets at %.0f,%.0f,%.0f\n", w->time, index, me->kind, still ? "stalled" : "no progress", me->pos.x, me->pos.y, me->pos.z, tp.x, tp.y, tp.z);
                }
            } else { c->mcu_t = w->time; c->mcu_d = nd; c->mcu_np = npt; }
        }
    }
    /* BaUnstick (default on, RECOMP_BOT_BA_UNSTICK / [Bots] BaUnstick): a BattleArmor walking that makes no headway for
     * 1.5 s -- a slope or lip it cannot walk, running on the spot -- hops toward its next point, a burn no longer than the
     * way left (no overshooting a flag). Before, it ran in place until the 6 s stall watchdog (laptop log 10-31: 13 s). */
    if (me->kind == MA2_KIND_BATTLEARMOR && !air && !hold_end && c7_on("baunstick", "RECOMP_BOT_BA_UNSTICK", "BaUnstick", 1)) {
        if (c->bau_t <= 0.0 || w->time < c->bau_t || hdist2(me->pos, c->bau_pos) > 0.7f || fabsf(me->pos.y - c->bau_pos.y) > 1.f) { c->bau_pos = me->pos; c->bau_t = w->time; }
        else if (w->time - c->bau_t > 1.5 && w->time >= c->jump_next && hdist2(me->pos, dest) > 2.f) {
            float left = minf2(hdist2(me->pos, goal), hdist2(me->pos, dest) + 6.f);
            if (body_jump_fuel_max(me->actor, me->pos, dest, 1.4f, maxf2(0.3f, left / 24.f))) {
                c->jumps++; c->jump_next = w->time + 1.5; c->bau_t = w->time; c7_add("baunstick", "hops", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-UNSTICK] t=%.1f bot=%d hops from %.0f,%.0f,%.0f toward %.0f,%.0f (%.0f m left)\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, dest.x, dest.z, left);
            }
        }
    }
    /* 'Mechs with jets jump along long, clear legs of the route (57-95 m per jump, BAVT_SPEC 1),
     * like a player covering ground; RECOMP_BOT_MECH_JUMP=0 turns it off. */
    /* CruisePace (default on, RECOMP_BOT_CRUISE_PACE / [Bots] CruisePace; parity pass 8): the cruise hop's 7 s pacing was put on jump_next, the
     * spacing EVERY jet waits for, so a climb that came up within 7 s of a cruise hop was walked into: H_81a1 (Ill Wind Snatch It) T2 lead b8
     * cruise-hopped at 23.2 s (jump_next 30.2), reached the 0.41-0.52 basin exit ramp at 28.0 s with a FULL tank (actor+0x49C 7.0 from 26.4 s),
     * ClimbAhead / MechClimbJet were held off and it stood 0-12 m/s at 757-764,1413 until 30.2 (2.2 s; floor 0). The 7 s now paces cruise hops
     * only (cruise_next); the shared spacing after a cruise hop is the usual 3 s. [C7] cruisepace: hops. */
    else if (me->kind == MA2_KIND_MECH && hop_ok && !nopath && hdist2(me->pos, goal) > 90.f && hdist2(me->pos, dest) > 25.f && w->time >= c->jump_next &&
             (w->time >= c->cruise_next || w->time < c->cruise_next - 8.0) &&
             env_int("RECOMP_BOT_MECH_JUMP", 1) && !heat_jet_hold(w, me) && body_jump_toward(me->actor, me->pos, dest)) {
        c->jumps++;
        if (c7_on("cruisepace", "RECOMP_BOT_CRUISE_PACE", "CruisePace", 1)) { c->jump_next = w->time + 3.0; c->cruise_next = w->time + 7.0; c7_add("cruisepace", "hops", 1); }
        else c->jump_next = w->time + 7.0;
    }
    /* Progress watchdog: replan when the body makes no headway. The spot just ahead becomes an
     * obstacle for this bot's plans (this match only), so the next route goes around it. A pilot
     * closing on a vehicle gives up sooner: the brain then takes another vehicle. */
    int boarding_leg = me->kind == MA2_KIND_PILOT && hdist2(me->pos, goal) < 15.f;
    /* Shuttling: a body that keeps moving yet stays in one small patch for 12 s (back and forth in
     * front of something it cannot pass) makes no headway either. The watchdog below only sees a
     * body standing still, so this counts as a stall too. */
    int shuttled = 0;
    if (w->time - c->area_t > 3.0 || w->time < c->area_t) c->narea = 0;             /* not travelling for a while */
    /* ShuttleReset (default on, RECOMP_BOT_SHUTTLE_RESET / [Bots] ShuttleReset): the 12 s patch is about one goal. A peek-hide / peek-shoot fight
     * kept a 'Mech in an 11 m patch on purpose; its new retreat goal 37 m off read as "shuttling" 0.8 s in, running at 10 m/s, and the stall's
     * backtrack held it 4 s (au3_00 bot 0, t=190.0-194.8). Samples are only kept while the goal is 25 m+ off (the test never fires nearer):
     * a short leg starts them over. (Not on every goal change: a retreat re-picks its point every few seconds.) [C7] shuttlereset: n. */
    if (hdist2(me->pos, goal) <= 25.f && c->narea && c7_on("shuttlereset", "RECOMP_BOT_SHUTTLE_RESET", "ShuttleReset", 1)) { c->narea = 0; c7_add("shuttlereset", "n", 1); }
    if (c->narea == 0 || w->time - c->area_t >= 1.0) {
        c->area[c->area_i] = me->pos; c->area_i = (c->area_i + 1) % 12; if (c->narea < 12) c->narea++; c->area_t = w->time;
        if (c->narea == 12 && hdist2(me->pos, goal) > 25.f && w->time >= c->back_until && w->time >= c->upjet_until && !air && !hold_end) {
            float mx = 0.f, mz = 0.f, rmax = 0.f;
            for (int i = 0; i < 12; i++) { mx += c->area[i].x / 12.f; mz += c->area[i].z / 12.f; }
            for (int i = 0; i < 12; i++) rmax = maxf2(rmax, hypotf(c->area[i].x - mx, c->area[i].z - mz));
            if (rmax < 11.f) {
                c->narea = 0; shuttled = 1;
                tank_orbit_pivot(c, w, me, dest);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CTRL] bot=%d kind=%d shuttling in a %.0f m patch at %.0f,%.0f for 12 s: a stall\n", index, me->kind, rmax, mx, mz);
            }
        }
    }
    /* FastStall (default on, RECOMP_BOT_FAST_STALL / [Bots] FastStall): the 6 s watchdog below ticks slowly when the body is plainly wedged (a tank on a rock:
     * sw65 799,865 sat 18 s through two ticks). A ground tank / 'Mech / BattleArmor still within 0.7 m for 3 s with the carrot 6 m+ off brings the
     * stall forward. [C7] faststall: n. */
    if (fast_stall_on() && !air && me->kind != MA2_KIND_PILOT && !hold_end && w->time >= c->yield_until && w->time >= c->back_until) {
        if (c->fq_t <= 0.0 || w->time < c->fq_t || hdist2(me->pos, c->fq_pos) > 0.7f || hdist2(me->pos, dest) < 6.f) { c->fq_pos = me->pos; c->fq_t = w->time; }
        else if (w->time - c->fq_t > 3.0) { c->fq_t = w->time; if (c->prog_t > 0.0 && w->time - c->prog_t < 6.0) { c->prog_t = w->time - 6.1; c7_add("faststall", "n", 1); } }
    }
    /* EjectGrace (default on, RECOMP_BOT_EJECT_GRACE / [Bots] EjectGrace): a pilot just out of a vehicle starts inside the hull it left for
     * 2.8-4.5 s; read as a stalled boarding leg (2.5 s) the brain dropped the 'Mech 12-14 m off it was swapping to (au9_33 LMS Proving
     * Ground: b4 lost 26 s, b6 stayed on foot 160 s). Its first 5 s on foot get the normal 6 s. [C7] ejectgrace: s. */
    int eg = boarding_leg && w->time - c->actor_since < 5.0 && c7_on("ejectgrace", "RECOMP_BOT_EJECT_GRACE", "EjectGrace", 1);
    if (eg) c7_add("ejectgrace", "s", w->dt);
    if (!shuttled && (hdist2(me->pos, c->prog_pos) > 3.f || c->prog_t <= 0.0)) { c->prog_pos = me->pos; c->prog_t = w->time; }
    else if (hold_end) c->prog_t = w->time;                                  /* push-x3: holding at the end of a partial route is not a stall */
    else if (shuttled || w->time - c->prog_t > (boarding_leg && !eg ? 2.5 : 6.0)) {
        int same = hdist2(me->pos, c->stall_pos) < 10.f;
        c->stalls = same ? c->stalls + 1 : 1; c->n_stalls++;
        c->stall_pos = me->pos; c->prog_t = w->time;
        /* TankFace (default on, RECOMP_BOT_TANK_FACE / [Bots] TankFace; parity pass 12): TankSlope lets a tank plan over cells up to slp 55, but the
         * height samples smooth a lip: On the Rocks' T2 plateau edge (1505-1530,1465-1550, sf 63.8 -> 52.6 in ~15 m, cells slp 43-53) stops the game's
         * tank dead at the top every time -- A11_84a3 b5 milled 100 s there (stall -> backtrack -> the same face again, three faces), D11_84a b5 the
         * same (7 stalls), no T2 tank got down in B11/D11. A tank stalled with its carrot over a drop (ground 2 m+ lower 4-24 m ahead, short of the carrot) marks the face:
         * its plans (every tank's) leave slope >= 20 cells within 35 m of it for 10 min (the lip cells are slp 21-32), as PilotFace does for pilots. tankface_proof.py: every T2
         * stall at the edge marks a face and the pile stays reachable on the tank grid with all of them banned. [C7] tankface: learned. */
        /* TankFaceReal (default on, RECOMP_BOT_TANK_FACE_REAL / [Bots] TankFaceReal; parity pass 13): only a hull that can still drive learns a face.
         * H12_84a: 10 of 13 faces came from two EMBEDDED hulls (TankUnembed had fired: T2 b6 at 70.6, T1 b4 at 87.0 by the pile) -- every 6 s
         * stall of a hull that cannot move re-learned a 'face' and banned slope >= 20 cells 35 m round the pile for every tank. An embedded hull
         * has its own mark + bail (EmbedMark, PocketStill / TrapBail). And the bail needs no net progress: the third face only bails when the
         * hull got under 20 m closer to its goal since its first face, 20 s+ back (a tank working its way down past several lips keeps it). */
        int tf_real = c7_on("tankfacereal", "RECOMP_BOT_TANK_FACE_REAL", "TankFaceReal", 1);
        int tf_embedded = tf_real && c->ue_actor == me->actor && c->ue_n > 0;
        if (tf_embedded && me->kind == MA2_KIND_TANK) c7_add("tankface", "embedded_skip", 1);
        if (me->kind == MA2_KIND_TANK && !air && !tf_embedded && c7_on("tankface", "RECOMP_BOT_TANK_FACE", "TankFace", 1)) {
            float tl = hdist2(me->pos, dest); int ok0 = 0; float g0 = terrain_height(me->pos.x, me->pos.z, &ok0);
            /* TankFaceReal, part 2: a lip stop is on TOP of the drop -- ground 4 m behind the hull (away from the carrot) not 1.5 m+ above it. P_84a (p23)
             * T1 b2 drove off its own pile's deck (y 79.4 -> 72.6), stood 5 s part-way down at 842,74.9,1196 and learned a 'face' there at 161.6 s,
             * then drove on at 163: a hull already on the slope is not stopped by a lip. [C7] tankface: on_slope_skip. */
            if (ok0 && tl > 8.f && tf_real) {
                int okb = 0; float gb = terrain_height(me->pos.x - (dest.x - me->pos.x) / tl * 4.f, me->pos.z - (dest.z - me->pos.z) / tl * 4.f, &okb);
                if (okb && gb > g0 + 1.5f) { ok0 = 0; c7_add("tankface", "on_slope_skip", 1); }
            }
            if (ok0 && tl > 8.f) {
                for (float d = 4.f; d <= 24.f && d <= tl; d += 2.f) {
                    Ma2Vec3 q = { me->pos.x + (dest.x - me->pos.x) / tl * d, me->pos.y, me->pos.z + (dest.z - me->pos.z) / tl * d };
                    int okq = 0; float gq = terrain_height(q.x, q.z, &okq);
                    if (okq && gq < g0 - 2.f) {
                        ma2_path_face_add(q.x, q.z, 35.f, w->time + 600.0, 2); c7_add("tankface", "learned", 1);
                        /* TankFaceBail: the third face one tank learns (G12_84a b5: seven faces round the On the Rocks T2 plateau, 37-141 s, every edge
                         * it tried stopped it) -- the way down is not drivable for it: trapped, so the bail (TrapBail) gets the pilot out to go on
                         * foot / take another ride. [C7] tankface: bails. */
                        static uint32_t tf_actor[MAX_BOTS]; static int tf_n[MAX_BOTS]; static double tf_t0[MAX_BOTS]; static float tf_gd0[MAX_BOTS];
                        if (index >= 0 && index < MAX_BOTS) {
                            float gd = hdist2(me->pos, goal);
                            if (tf_actor[index] != me->actor || w->time < tf_t0[index]) { tf_actor[index] = me->actor; tf_n[index] = 0; }
                            if (tf_n[index] == 0) { tf_t0[index] = w->time; tf_gd0[index] = gd; }
                            if (++tf_n[index] >= 3) {
                                int stuck_net = w->time - tf_t0[index] >= 20.0 && gd > tf_gd0[index] - 20.f;
                                if (!tf_real || stuck_net) { b->status.trapped = 1; c->n_trapped++; c7_add("tankface", "bails", 1); tf_n[index] = 0; }
                                else { c7_add("tankface", "bail_progress", 1); tf_n[index] = 1; tf_t0[index] = w->time; tf_gd0[index] = gd; }
                            }
                        }
                        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-TANKFACE] t=%.1f bot=%d stalled at %.0f,%.1f,%.0f, carrot %.0f,%.0f over a drop at %.0f,%.0f (%.1f m lower): tanks plan off it 35 m round\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, dest.x, dest.z, q.x, q.z, g0 - gq);
                        break;
                    }
                }
            }
        }
        /* DropTrap (default on, RECOMP_BOT_DROP_TRAP / [Bots] DropTrap): a drop is one way. Snatch It Ill Wind: team 2's route to the cores
         * stepped off the ledge at 950,14,1637 into the basin at 912,-2,1595, whose ways out the grid shows open but no 'Mech or tank
         * climbs (b6 bs81b 28.7-240 s, 10 shuttle stalls; bs81a b6 61 s, tank b8; team 2 lost 0-5 and 0-4). Two stalls within 60 m
         * of where a drop came down, inside 60 s of it: no one plans that drop for 10 min. [C7] droptrap: n. */
        /* PocketBail (default on, RECOMP_BOT_POCKET_BAIL / [Bots] PocketBail): a 'Mech or tank already down in a pocket it cannot walk out of
         * never reached "trapped" (4 stalls at one spot), so TrapBail never got it out: bs81b b6 (Snatch It Ill Wind) shuttled in the basin at
         * 908-916,1585-1606 from 41 to 240 s, 16 stalls, every one logged as "stall 1" (each exit it tried was walled by the same low
         * structure, h1 blocked / h3 clear). Four stalls within 25 m of each other inside 90 s: trapped (the bot climbs out and takes
         * another ride, as TrapBail does for a wedged hull). [C7] pocketbail: n. */
        if (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK) {
            static Ma2Vec3 pb_pos[16]; static double pb_t[16]; static int pb_n[16]; static uint32_t pb_actor[16];
            int pi_ = index & 15;
            if (pb_actor[pi_] != me->actor || w->time - pb_t[pi_] > 90.0 || w->time < pb_t[pi_] || hdist2(me->pos, pb_pos[pi_]) > 25.f) {
                pb_actor[pi_] = me->actor; pb_pos[pi_] = me->pos; pb_t[pi_] = w->time; pb_n[pi_] = 0;
            }
            /* PocketStill (default on, RECOMP_BOT_POCKET_STILL / [Bots] PocketStill): a hull that has not moved 1.5 m over three stalls is wedged, not
             * shuttling in a pocket: r21_84a Snatch Rush Hour tank b6 sat on the y 63 deck at 1548,1426 from 160 s, bailed at the 4th stall (175). */
            ++pb_n[pi_];
            int pb_still = pb_n[pi_] >= 3 && hdist2(me->pos, pb_pos[pi_]) < 1.5f && c7_on("pocketstill", "RECOMP_BOT_POCKET_STILL", "PocketStill", 1);
            if (pb_still) c7_add("pocketstill", "n", 1);
            if ((pb_n[pi_] >= 4 || pb_still) && hdist2(me->pos, goal) > 15.f && c7_on("pocketbail", "RECOMP_BOT_POCKET_BAIL", "PocketBail", 1)) {
                b->status.trapped = 1; c->n_trapped++; c7_add("pocketbail", "n", 1); pb_n[pi_] = 0; pb_t[pi_] = w->time;
                /* PocketMark (default on, RECOMP_BOT_POCKET_MARK / [Bots] PocketMark): Ill Wind has a dip by each base exit a tank drives into
                 * and cannot climb out of (rc4_63b b3/b4 at 331-335,11,994-999, 5 m under the base; iw9b b8 at 1167,3,1410, 12 m under), and the
                 * game refuses the eject there (no room to set the pilot down), so the next tank drove in too. Mark the spot (5 m) on the map. */
                if (me->kind == MA2_KIND_TANK && c7_on("pocketmark", "RECOMP_BOT_POCKET_MARK", "PocketMark", 1)) { side_prop_add(me->pos, 5.0f); turrets_reapply(); c7_add("pocketbail", "marked", 1); }
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-POCKETBAIL] t=%.1f bot=%d kind=%d 4 stalls within 25 m of %.0f,%.0f,%.0f: trapped\n", w->time, index, me->kind, pb_pos[pi_].x, pb_pos[pi_].y, pb_pos[pi_].z);
            }
        }
        if (c->dt_t > 0.0 && w->time - c->dt_t < 60.0 && w->time >= c->dt_t && hdist2(me->pos, c->dt_land) < 60.f) c->dt_n++;   /* (the stall count keys on 10 m: b6 stalled 8-12 m apart) */
        if (c->dt_t > 0.0 && c->dt_n >= 2 && c7_on("droptrap", "RECOMP_BOT_DROP_TRAP", "DropTrap", 1)) {
            ma2_path_forbid_drop(c->dt_top.x, c->dt_top.z, w->time + 600.0);
            c7_add("droptrap", "n", 1); c->dt_t = 0.0;
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DROP] t=%.1f bot=%d drop at %.0f,%.0f led into a pocket (stalled at %.0f,%.0f): no drops there for 10 min\n",
                                                     w->time, index, c->dt_top.x, c->dt_top.z, me->pos.x, me->pos.z);
        }
        {
            Ma2Vec3 ahead = me->pos; float l = hypotf(dest.x - me->pos.x, dest.z - me->pos.z);
            float aheadm = air ? 20.f : 4.f;                  /* a VTOL stopped by a barrier in the air: well ahead */
            if (l > 0.1f) { ahead.x += (dest.x - me->pos.x) / l * aheadm; ahead.z += (dest.z - me->pos.z) / l * aheadm; }
            int slot = c->nstuck < 8 ? c->nstuck++ : (int)(c->stalls % 8);
            c->stuck_pt[slot] = ahead; c->stuck_t[slot] = w->time;
            if (ma2_trav_ready() && !air) ma2_trav_learn(ahead, 3.0f, w->time, 30.0);   /* the ground map learns nothing from the air */
            /* Whatever stopped us is missing from the map (a fence or wall thinner than the 2 m
             * grid, a ledge): look again at 0.5 m around it; the map learns it for everyone and
             * every plan through here is redone. */
            if (ma2_trav_ready() && g_probe.phys && !air) {
                int swept = 0, deferred = 0;
                if (stall_batch_on() && stall_slice_on()) {
                    /* ft-bots StallSlice: the wall sweep (13 rays) now; the 8 m re-probe (~830 rays), the grid commit and the "unseen obstacle"
                     * test are queued with the wreck pump and spread over the next frames (a stall tick was 20-54 ms on NRC). */
                    int lvl_ok2 = 1;
                    if (navfix_on()) {
                        int oks = 0; float sa = surface_height(ahead.x, ahead.z, &oks);
                        if (oks && fabsf(sa - me->pos.y) >= 3.f) { lvl_ok2 = 0; c7_add("navfix", "learn_skipped_level", 1); }
                    }
                    wall_sweep(index, w, me, dest); swept = 1;
                    deferred = stall_queue(ahead, c->stalls, lvl_ok2, index);
                    c7_add("stallslice", deferred ? "deferred" : "queue_full", 1);
                }
                if (deferred) { /* the pump finishes it */ }
                else {
                if (stall_batch_on()) {   /* ew-b8 B8.17 (R1 E7/P1-2): reprobe + wall sweep + turret restamp, then ONE grid commit (was up to 3 full refreshes) */
                    unsigned e0 = ma2_trav_epoch();
                    double bt0 = c7_t0();
                    ma2_trav_reprobe_ex(ahead.x, ahead.z, 8.f, wreck_probe, NULL, 1);
                    if (!air && !swept) wall_sweep(index, w, me, dest);
                    turrets_restamp();
                    ma2_trav_commit_if_dirty();
                    c7_ms("stallbatch", "stall_grid", bt0);
                    c7_add("stallbatch", "stalls", 1);
                    if (ma2_trav_epoch() != e0) c7_add("stallbatch", "commits", ma2_trav_epoch() - e0);
                } else {
                    ma2_trav_reprobe(ahead.x, ahead.z, 8.f, wreck_probe, NULL);
                    if (!air && !swept) wall_sweep(index, w, me, dest);
                }
                /* Still open on the map, yet it stopped us (a rock face or a barrier no ray sees):
                 * mark the spot itself. A long wall fills in bump by bump, the way a player learns it. */
                int lvl_ok = 1, lvl_learned = 0;
                if (navfix_on()) {   /* push-x3 E9: only where this body stands on the grid's surface (under a bridge deck the cell is the deck: 2D learning carved holes in it) */
                    int oks = 0; float sa = surface_height(ahead.x, ahead.z, &oks);
                    if (oks && fabsf(sa - me->pos.y) >= 3.f) { lvl_ok = 0; c7_add("navfix", "learn_skipped_level", 1); }
                }
                /* PilotFace (default on, RECOMP_BOT_PILOT_FACE / [Bots] PilotFace): Check It No Man's Land, the mesa terminal 634,74,1227: FootFallback
                 * pilots were routed up the west face and stood at its foot 505-509,1237-1242 (rc10_73a b1 58 s, b2 50 s at 500,1239, rc6_73b b1); the
                 * grid reads a 30-43 deg slope there (slope 47-94), pilots walk 0.5-0.9 rise/run elsewhere, and no collider is there (FPROBE): a lip in
                 * the terrain's collision. The one-cell learning moved them 9 m along the face. A pilot stalled walking uphill onto steep ground (slope
                 * >= 40) marks the face 12 m round for pilots' plans (10 min): the route goes up the other face (east, 583,55,1177 -> 604,74,1192). */
                if (me->kind == MA2_KIND_PILOT && lvl_ok && c7_on("pilotface", "RECOMP_BOT_PILOT_FACE", "PilotFace", 1)) {
                    int oka = 0, okm = 0; float ha = terrain_height(ahead.x, ahead.z, &oka), hm = terrain_height(me->pos.x, me->pos.z, &okm);
                    int sa = ma2_trav_slope(ahead.x, ahead.z), sm = ma2_trav_slope(me->pos.x, me->pos.z);
                    if (oka && okm && ha > hm + 0.3f && (sa >= 40 || sm >= 40)) {
                        ma2_path_pilot_face_add(ahead.x, ahead.z, 30.f, w->time + 600.0); c7_add("pilotface", "learned", 1);
                        c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
                        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-NAV] t=%.1f bot=%d pilot stalled up a steep face at %.0f,%.0f (slope %d/%d): pilots plan off it 30 m round\n", w->time, index, ahead.x, ahead.z, sm, sa);
                    }
                }
                if (ma2_trav_clear(ahead.x, ahead.z, 0.0) > 1.5f && c->stalls >= 2 && lvl_ok) {
                    side_prop_add(ahead, 2.5f); lvl_learned = 1;
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-NAV] t=%.1f bot=%d learned an unseen obstacle at %.0f,%.0f\n", w->time, index, ahead.x, ahead.z);
                }
                /* LowWallLearn (default on, RECOMP_BOT_LOW_WALL_LEARN / [Bots] LowWallLearn): a wall lower than the grid's obstacle rays reads open,
                 * and the learning above needs two stalls within 10 m: bs81b / iw11b b6 (Snatch It Ill Wind) stalled 16 times in the basin at
                 * 908-965,1537-1606 against one low structure (BOT-BLOCK: the 1 m ray hits 00CF40D0, 3 m / 6 m / 10 m clear), each stall a
                 * "stall 1" 8-12 m from the last, so the way west stayed open on the map for every bot that came down. A 'Mech / tank stall with
                 * the 1 m ray blocked within 6 m and the 3 m ray clear, no body there: learn it at once. [C7] lowwall: n. */
                if (!lvl_learned && lvl_ok && !air && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK) && ma2_trav_clear(ahead.x, ahead.z, 0.0) > 1.5f &&
                    hdist2(dest, me->pos) > 1.f && c7_on("lowwall", "RECOMP_BOT_LOW_WALL_LEARN", "LowWallLearn", 1)) {
                    float lx = (dest.x - me->pos.x) / hdist2(dest, me->pos), lz = (dest.z - me->pos.z) / hdist2(dest, me->pos);
                    Ma2Vec3 f1 = me->pos, t1, f3, t3; uint32_t h1 = 0, h3 = 0; int body = 0;
                    f1.y += 1.f; t1 = f1; t1.x += lx * 6.f; t1.z += lz * 6.f; f3 = f1; f3.y = me->pos.y + 3.f; t3 = t1; t3.y = f3.y;
                    for (int q = 0; q < w->nplayers; q++) if (w->players[q].alive && w->players[q].actor != me->actor && hdist2(w->players[q].pos, ahead) < 8.f) body = 1;
                    /* WallLearn: not only walls under the grid's rays -- rc4_93a b0 (Base War Rush Hour) stalled at 1477,30,1487 against building
                     * 01009710 (cls C3E3D036) that every ray hit (1 / 3 / 6 / 10 m) while the grid showed it open (AiKicks 28 and 46 s; rc4_93b b0, bs93b b1
                     * on the same exit). A structure (not a body, not a parked hull) across the 1 m ray within 6 m is learned at once. */
                    for (int q = 0; q < w->nvehicles; q++) if (hdist2(w->vehicles[q].pos, ahead) < 8.f) body = 1;
                    int wall_any = c7_on("walllearn", "RECOMP_BOT_WALL_LEARN", "WallLearn", 1);
                    if (!body && !ai_los(me->actor, 0, f1, t1, &h1) && h1 && (wall_any || ai_los(me->actor, 0, f3, t3, &h3))) {
                        side_prop_add(ahead, 2.5f); lvl_learned = 1; c7_add("lowwall", "n", 1);
                        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-NAV] t=%.1f bot=%d learned a low wall at %.0f,%.0f (1 m ray hit %08X, 3 m clear)\n", w->time, index, ahead.x, ahead.z, h1);
                    }
                }
                if (!stall_batch_on() || lvl_learned) turrets_reapply();   /* B8.17: already restamped above unless a new obstacle was learned */
                }   /* !deferred */
            }
        }
        if (c->stalls >= 3 || boarding_leg) b->status.stalled = 1;
        if (pilot_escape(c, w, me, 9.f)) {                 /* pinned against a parked hull: back straight off it */
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-ESCAPE] bot=%d from %.0f,%.0f t=%.1f\n", index, me->pos.x, me->pos.z, w->time);
            return 0;
        }
        /* PocketEscape (default on, RECOMP_BOT_POCKET_ESCAPE / [Bots] PocketEscape): a pilot stalled 4+ times in one spot (sweep swA33: 8 stalls, 25 s at 712,193,409
         * with no route, the backtrack crumb the same pocket) has nothing else that moves it: it walks 12 m straight toward the next open side
         * of eight (the map's clearance at 6 m and 12 m out), turning on each stall, for 2.5 s, then plans again. [C7] pocketescape: n. */
        if (me->kind == MA2_KIND_PILOT && c->stalls >= 3 && !boarding_leg && edge_step_on() && on_raised(me, NULL)) {
            Ma2Vec3 ep2;
            if (edge_step_pt(me, goal, &ep2)) {
                c->esc_pt = ep2; c->esc_until = w->time + 3.5; c->escapes++; c7_add("edgestep", "n", 1);
                c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 3.5;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-EDGESTEP] t=%.1f bot=%d stall %d on the top at %.0f,%.0f,%.0f steps off toward %.0f,%.0f\n", w->time, index, c->stalls, me->pos.x, me->pos.y, me->pos.z, ep2.x, ep2.z);
                return 0;
            }
        }
        if (me->kind == MA2_KIND_PILOT && c->stalls >= 4 && !boarding_leg && pocket_escape_on() && ma2_trav_ready()) {
            for (int k = 0; k < 8; k++) {
                float a = (float)((c->stalls + k) % 8) * 0.785398f, sx = sinf(a), cz = cosf(a);
                if (ma2_trav_clear(me->pos.x + sx * 6.f, me->pos.z + cz * 6.f, 0.0) > 1.5f && ma2_trav_clear(me->pos.x + sx * 12.f, me->pos.z + cz * 12.f, 0.0) > 1.5f) {
                    c->esc_pt = me->pos; c->esc_pt.x += sx * 12.f; c->esc_pt.z += cz * 12.f;
                    c->esc_until = w->time + 2.5; c->escapes++; c7_add("pocketescape", "n", 1);
                    c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 2.5;
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-POCKET] t=%.1f bot=%d stall %d at %.0f,%.0f,%.0f: walking out toward %.0f,%.0f\n", w->time, index, c->stalls, me->pos.x, me->pos.y, me->pos.z, c->esc_pt.x, c->esc_pt.z);
                    return 0;
                }
            }
        }
        if (c->stalls >= 4 && me->kind != MA2_KIND_PILOT && hdist2(me->pos, goal) > 15.f) { b->status.trapped = 1; if (c->stalls == 4) c->n_trapped++; }   /* backing out keeps failing */
        c7_add("f35", me->kind == MA2_KIND_BATTLEARMOR ? "ba_stalls" : "other_stalls", 1);
        /* Back out the way we came: the breadcrumb from a few seconds ago is known to be reachable.
         * (F35: not a BA's first stall in ground the map shows open -- it braked, nothing holds it.) */
        if (me->kind == MA2_KIND_BATTLEARMOR && c->stalls == 1 && !air && ma2_trav_ready() && ba_stride_on() &&
            ma2_trav_clear(me->pos.x, me->pos.z, 0.0) > 1.5f && hdist2(dest, me->pos) > 0.1f &&
            ma2_trav_clear(me->pos.x + (dest.x - me->pos.x) / hdist2(dest, me->pos) * 4.f, me->pos.z + (dest.z - me->pos.z) / hdist2(dest, me->pos) * 4.f, 0.0) > 1.5f)
            c7_add("f35", "skipped_backtracks", 1);
        else if (c->ncrumb >= 2) {
            int back = c->ncrumb >= 3 ? 3 : c->ncrumb - 1;
            Ma2Vec3 p = c->crumb[(c->crumb_i - back + 8 * 4) % 8];
            if (hdist2(p, me->pos) > 4.f) {
                c->back_pt = p; c->back_until = w->time + 4.0; c->backtracks++;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-BACKTRACK] bot=%d kind=%d from %.0f,%.0f to %.0f,%.0f t=%.1f\n", index, me->kind, me->pos.x, me->pos.z, p.x, p.z, w->time);
            }
        }
        /* Jets out: over the lip on the first stall; stuck again at the same spot (wedged in a
         * gap, on a ledge with no way down), hop up and back toward the last place we walked. */
        if ((me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && w->time >= c->jump_next) {
            int jumped = 0;
            if (c->stalls == 1) jumped = (navfix_on() && nopath) ? 0 :
                     hdist2(me->pos, goal) < 40.f && c7_on("baunstick", "RECOMP_BOT_BA_UNSTICK", "BaUnstick", 1) ?   /* BaUnstick: near the goal, no further than it */
                     body_jump_fuel_max(me->actor, me->pos, dest, 1.6f, maxf2(0.3f, hdist2(me->pos, goal) / 24.f)) :
                     body_jump_toward(me->actor, me->pos, dest);   /* push-x3 E10: no jet at a goal with no route */
            else if (c->ncrumb >= 2) {
                Ma2Vec3 p = c->crumb[(c->crumb_i - (c->ncrumb >= 3 ? 3 : c->ncrumb - 1) + 8 * 4) % 8];
                if (hdist2(p, me->pos) > 3.f) { p.y = maxf2(p.y, me->pos.y) + 4.f; jumped = body_jump_up(me->actor, me->pos, p, 1.2f); }
            }
            if (!jumped && c->stalls >= 2) {
                /* No walked-in crumb to go back to (wedged where it spawned, on a roof against a
                 * vent): hop up and away from what holds us, to one side then the other. */
                float bx = me->pos.x - dest.x, bz = me->pos.z - dest.z, bl = hypotf(bx, bz) + 1e-3f;
                float a = atan2f(bx / bl, bz / bl) + ((c->stalls & 1) ? 1.0f : -1.0f);
                Ma2Vec3 p = { me->pos.x + sinf(a) * 8.f, me->pos.y + 4.f, me->pos.z + cosf(a) * 8.f };
                jumped = body_jump_up(me->actor, me->pos, p, 1.2f);
            }
            if (jumped) { c->jumps++; c->jump_next = w->time + 3.0; }
        }
        c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
        if (env_on("RECOMP_BOT_DIAG")) {
            static const float hs[4] = { 1.f, 3.f, 6.f, 10.f };
            char buf[256]; int o = 0;
            for (int k = 0; k < 4; k++) {
                Ma2Vec3 from = me->pos, to = dest; uint32_t hit = 0;
                from.y += hs[k]; to.y = from.y;
                float l = hdist2(from, to);
                if (l > 25.f) { to.x = from.x + (to.x - from.x) / l * 25.f; to.z = from.z + (to.z - from.z) / l * 25.f; }
                int clr = ai_los(me->actor, 0, from, to, &hit);
                o += snprintf(buf + o, sizeof buf - (size_t)o, " h%.0f=%d/%08X/%08X", hs[k], clr, hit, gva_ok(hit, 0x30) ? MEM32(hit + 0x28) : 0u);
            }
            fprintf(stderr, "[BOT-BLOCK] bot=%d pos=%.1f,%.1f,%.1f toward %.0f,%.0f:%s\n", index, me->pos.x, me->pos.y, me->pos.z, dest.x, dest.z, buf);
        }
        if (env_on("RECOMP_BOT_DIAG")) {
            uint32_t a = me->actor, comp = MEM32(a + 0x8C), ai = actor_ai(a);
            fprintf(stderr, "[BOT-CTRL] bot=%d no progress (stall %d) kind=%d pos=%.0f,%.0f dest=%.0f,%.0f goal=%.0f,%.0f c144=%08X cBC=%.1f a1C=%08X a1E8=%08X mode=%u nav=%u awake=%u avt=%08X\n", index, c->stalls, me->kind,
                    me->pos.x, me->pos.z, dest.x, dest.z, goal.x, goal.z, gva_ok(comp, 0x150) ? MEM32(comp + 0x144) : 0, gva_ok(comp, 0x150) ? MEMF(comp + 0xBC) : -1.f,
                    MEM32(a + 0x1C), MEM32(a + 0x1E8), ai ? MEM32(ai + 0x12C) : 99u, ai ? MEM32(ai + 0x2C) : 99u, ai ? MEM8(ai + 0x90) : 9u, MEM32(a));
        }
    }
    return 0;
}

/* Diagnostic: the boarding gates of sub_CA975 -> sub_CADEE, evaluated from here. */
static void board_probe(int index, uint32_t p)
{
    uint32_t a = player_actor(p);
    GuestRegs r; regs_save(&r);
    g_eax = p; PUSH32(g_esp, 0x000CADF7u); sub_000C8EE9(); uint32_t c8ee9 = g_eax & 0xFF; regs_restore(&r);
    regs_save(&r); g_esi = a; PUSH32(g_esp, 0x000CAE3Fu); sub_000E35C6(); uint32_t e35c6 = g_eax & 0xFF; regs_restore(&r);
    regs_save(&r); g_eax = MEM32(p + 0x438); PUSH32(g_esp, p); PUSH32(g_esp, 0x000CAEA3u); sub_000CAD4A(); uint32_t cad4a = g_eax & 0xFF; regs_restore(&r);
    regs_save(&r); g_edi = p; PUSH32(g_esp, 0x000CA984u); sub_000CADEE(); uint32_t cadee = g_eax & 0xFF; regs_restore(&r);
    fprintf(stderr, "[BOT-BOARDP] bot=%d c8ee9=%u e35c6=%u cad4a=%u cadee=%u p424=%08X p428=%08X p438=%08X a80=%08X a71=%u a84=%08X a1C=%08X a26C=%08X c144=%08X\n",
            index, c8ee9, e35c6, cad4a, cadee, MEM32(p + 0x424), MEM32(p + 0x428), MEM32(p + 0x438), a ? MEM32(a + 0x80) : 0, a ? MEM8(a + 0x71) : 0,
            a ? MEM32(a + 0x84) : 0, a ? MEM32(a + 0x1C) : 0, a ? MEM32(a + 0x26C) : 0, a && gva_ok(MEM32(a + 0x8C), 0x150) ? MEM32(MEM32(a + 0x8C) + 0x144) : 0);
}

/* ---- boarding ---- */
/* Boarding (the "basic bot AI" rule): one A* path from the bot to the vehicle's exact entry zone
 * (MACrateGenerator/MATriggerPoint circle, ma2_bot_entry.c), with the target's own hull and every
 * other parked vehicle as obstacles so the route comes round to the entrance side; walked at full
 * speed with MoveTo; enter (39) the moment the zone offers THAT vehicle. Vehicles are claimed
 * (core fills Ma2VehicleView.claimed_by), so the brain retargets at once if it is taken. */
static float path_length(const Ma2Path *p, Ma2Vec3 from)
{
    float l = 0.f; Ma2Vec3 a = from;
    for (int i = p->next; i < p->n; i++) { l += hdist2(a, p->pt[i]); a = p->pt[i]; }
    return l;
}
/* The solid part of a parked vehicle's hull around its centre (smaller than the planner's circle). */
static float hull_core_r(int k)
{
    switch (k) { case MA2_KIND_MECH: return 3.0f; case MA2_KIND_TANK: return 3.5f; case MA2_KIND_VTOL: return 3.0f; case MA2_KIND_BATTLEARMOR: return 1.2f; default: return 2.5f; }
}
/* Hulls bigger than their kind's: a pilot that got pressed against one records how far out it
 * reaches (this match), so the next approach keeps clear of it. */
static struct { uint32_t actor; float r[MA2_HULL_DIRS]; double t; } g_hull[32];     /* learned reach per ray direction */
static int hull_dir(const Ma2VehicleView *v, Ma2Vec3 q)
{
    float a = atan2f(q.x - v->pos.x, q.z - v->pos.z), step = 6.2831853f / (float)MA2_HULL_DIRS;
    return ((int)floorf(a / step + 0.5f) % MA2_HULL_DIRS + MA2_HULL_DIRS) % MA2_HULL_DIRS;
}
/* The hull's reach per direction as bodies meet it: the measured rays (at least 0.6 x its kind's
 * core: a ray can slip through), a short ray between two long ones filled in (the gap between a
 * 'Mech's legs -- not the side of a VTOL's nose, where only one neighbour is long), and whatever
 * pilots learned by bumping into it. `tall`: the body walking past is a 'Mech -- the outline up to
 * its hips (a VTOL's tail boom), not just a pilot's height. */
static int hull_tall(int kind) { return kind == MA2_KIND_MECH; }
static void hull_star(const Ma2VehicleView *v, int tall, float *out)
{
    float core = 0.6f * hull_core_r(v->kind);
    const float *ext = tall ? v->ext_hi : v->ext;
    for (int k = 0; k < MA2_HULL_DIRS; k++) {
        float e = maxf2(ext[k], core);
        float n1 = maxf2(ext[(k + 1) % MA2_HULL_DIRS], core), n2 = maxf2(ext[(k + MA2_HULL_DIRS - 1) % MA2_HULL_DIRS], core);
        if (n1 > e && n2 > e) e = 0.5f * (e + (n1 < n2 ? n1 : n2));
        out[k] = e;
    }
    for (int i = 0; i < 32; i++)
        if (g_hull[i].actor == v->actor)
            for (int k = 0; k < MA2_HULL_DIRS; k++) if (g_hull[i].r[k] > out[k]) out[k] = g_hull[i].r[k];
}
static float hull_r(const Ma2VehicleView *v, int tall)
{
    float r = v->ext_max > 0.5f ? v->ext_max : hull_core_r(v->kind);           /* measured, else by kind */
    if (tall && v->ext_max > 0.5f) for (int k = 0; k < MA2_HULL_DIRS; k++) if (v->ext_hi[k] > r) r = v->ext_hi[k];
    for (int i = 0; i < 32; i++) if (g_hull[i].actor == v->actor) for (int k = 0; k < MA2_HULL_DIRS; k++) if (g_hull[i].r[k] > r) r = g_hull[i].r[k];
    return r;
}
/* Hull reach toward point q (+0.6 m for the pilot), else `fallback`. Each direction is the furthest
 * reach across its sector, so anywhere in a sector it is at least that sector's (a straight line
 * from a long direction to a short one cut the corner off a VTOL's nose); rising toward a longer
 * neighbour it follows the line. */
static float hull_at(const Ma2VehicleView *v, int tall, Ma2Vec3 q, float fallback)
{
    if (v->ext_max <= 0.5f) return fallback;
    float st[MA2_HULL_DIRS];
    hull_star(v, tall, st);
    float a = atan2f(q.x - v->pos.x, q.z - v->pos.z);
    if (a < 0.f) a += 6.2831853f;
    float f = a / (6.2831853f / (float)MA2_HULL_DIRS), t;
    int k = (int)f;
    t = f - (float)k; k %= MA2_HULL_DIRS;
    float lin = st[k] * (1.f - t) + st[(k + 1) % MA2_HULL_DIRS] * t, own = t < 0.5f ? st[k] : st[(k + 1) % MA2_HULL_DIRS];
    return maxf2(lin, own) + 0.6f;
}
/* 1 if walking straight a->b brushes the vehicle's measured outline. */
static int seg_hits_hull(const Ma2VehicleView *v, int tall, Ma2Vec3 a, Ma2Vec3 b)
{
    float l = hdist2(a, b);
    int n = (int)(l / 0.5f) + 1;
    for (int i = 0; i <= n; i++) {
        Ma2Vec3 p = a; float t = (float)i / (float)n;
        p.x += (b.x - a.x) * t; p.z += (b.z - a.z) * t;
        if (hdist2(p, v->pos) < hull_at(v, tall, p, hull_core_r(v->kind) + 0.6f) - 0.2f) return 1;
    }
    return 0;
}
/* A pilot pressed against a hull at `from`: the hull reaches at least `r` on that side (and a
 * little less on the two sides next to it) -- not all round: a VTOL's tail is long, its nose not. */
static void hull_learn(const Ma2VehicleView *v, float r, double now, Ma2Vec3 from)
{
    int k = hull_dir(v, from);
    int slot = 0;
    for (int i = 0; i < 32; i++) {
        if (g_hull[i].actor == v->actor) { slot = i; break; }
        if (g_hull[i].t < g_hull[slot].t || now < g_hull[i].t) slot = i;
    }
    if (g_hull[slot].actor != v->actor) { g_hull[slot].actor = v->actor; for (int j = 0; j < MA2_HULL_DIRS; j++) g_hull[slot].r[j] = 0.f; }
    if (r > g_hull[slot].r[k]) g_hull[slot].r[k] = r;
    if (0.9f * r > g_hull[slot].r[(k + 1) % MA2_HULL_DIRS]) g_hull[slot].r[(k + 1) % MA2_HULL_DIRS] = 0.9f * r;
    if (0.9f * r > g_hull[slot].r[(k + MA2_HULL_DIRS - 1) % MA2_HULL_DIRS]) g_hull[slot].r[(k + MA2_HULL_DIRS - 1) % MA2_HULL_DIRS] = 0.9f * r;
    g_hull[slot].t = now;
}
/* BaCentre: where the pilot walks for a BattleArmor's centre. The hull is not round (legs, a gun arm): it is walked round, at its longest reach plus
 * 0.8 m, to the sector where the measured hull reaches least (v->ext[]), and in from there. */
static Ma2Vec3 ba_approach(const Ma2VehicleView *v, const Ma2PlayerView *me, Ma2Vec3 ep)
{
    if (v->ext_max < 0.5f) return ep;
    int kmin = -1; float emin = 1e9f;
    for (int k = 0; k < MA2_HULL_DIRS; k++) if (v->ext[k] > 0.1f && v->ext[k] < emin) { emin = v->ext[k]; kmin = k; }
    if (kmin < 0) return ep;
    float at = (float)kmin * (6.2831853f / (float)MA2_HULL_DIRS), am = atan2f(me->pos.x - v->pos.x, me->pos.z - v->pos.z), da = at - am;
    while (da > 3.1415927f) da -= 6.2831853f;
    while (da < -3.1415927f) da += 6.2831853f;
    if (fabsf(da) < 0.5f || hdist2(me->pos, v->pos) > v->ext_max + 2.0f) return ep;
    float an = am + (da > 0.6f ? 0.6f : da < -0.6f ? -0.6f : da), R = v->ext_max + 0.8f;
    Ma2Vec3 t = ep; t.x = v->pos.x + sinf(an) * R; t.z = v->pos.z + cosf(an) * R;
    return t;
}
/* RimSlide: where the game offered a vehicle of this build (its entry zone's radius and distance ahead of the hull centre) to a
 * pilot, in the hull's own frame (bearing from its heading, distance) -- the same model's other copy is entered there too, either
 * side (NRC CTF VTOLs: 5.1-5.8 m out at +27, -39, +45 degrees off the nose; the zone centre 2.95 m ahead is under the hull). */
static struct { float er, ed, rel, d; } g_offer[16]; static int g_noffer; static double g_offer_t;
static void offer_learn(const Ma2VehicleView *v, const Ma2PlayerView *me, double now)
{
    if (now < g_offer_t) g_noffer = 0;   /* a new match */
    g_offer_t = now;
    float ed = hdist2(v->entry, v->pos), rel = atan2f(me->pos.x - v->pos.x, me->pos.z - v->pos.z) - v->yaw, d = hdist2(me->pos, v->pos);
    while (rel > 3.14159265f) rel -= 6.2831853f;
    while (rel < -3.14159265f) rel += 6.2831853f;
    for (int i = 0; i < g_noffer; i++)
        if (fabsf(g_offer[i].er - v->entry_r) < 0.2f && fabsf(g_offer[i].ed - ed) < 0.3f && fabsf(fabsf(g_offer[i].rel) - fabsf(rel)) < 0.17f) return;   /* known (or its mirror) */
    if (g_noffer >= 16) return;
    g_offer[g_noffer].er = v->entry_r; g_offer[g_noffer].ed = ed; g_offer[g_noffer].rel = rel; g_offer[g_noffer].d = d; g_noffer++;
    c7_add("rimslide", "learned", 1);
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-RIMSLIDE] learned an entry spot: kind %d, zone r %.1f %.2f m ahead, pilot %.2f m out at %.0f deg off the nose\n", v->kind, v->entry_r, ed, d, rel * 57.3f);
}
/* the learned spot (or its mirror) nearest `me` on this vehicle; 0 = none known */
static int offer_spot(const Ma2VehicleView *v, const Ma2PlayerView *me, Ma2Vec3 *out)
{
    float ed = hdist2(v->entry, v->pos), bd = 1e9f; int got = 0;
    for (int i = 0; i < g_noffer; i++) {
        if (fabsf(g_offer[i].er - v->entry_r) >= 0.2f || fabsf(g_offer[i].ed - ed) >= 0.3f) continue;
        for (int m = -1; m <= 1; m += 2) {
            float a = v->yaw + g_offer[i].rel * (float)m; Ma2Vec3 q = v->pos;
            q.x += g_offer[i].d * sinf(a); q.z += g_offer[i].d * cosf(a);
            float dq = hdist2(q, me->pos);
            if (dq < bd) { bd = dq; *out = q; got = 1; }
        }
    }
    return got;
}
/* RimSlide: one step round the zone (or to a learned offer spot within reach of a step) */
static void rim_step(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2VehicleView *v, Ma2Vec3 ep, float zr, float dzone, uint32_t veh)
{
    float th = atan2f(me->pos.x - ep.x, me->pos.z - ep.z), rr = maxf2(zr - 1.2f, 1.0f), hr = hull_r(v, 0);
    if (!c->rsl_side) {
        Ma2Vec3 qa = ep, qb = ep;
        qa.x += rr * sinf(th + 0.6f); qa.z += rr * cosf(th + 0.6f); qb.x += rr * sinf(th - 0.6f); qb.z += rr * cosf(th - 0.6f);
        c->rsl_side = hdist2(qa, v->pos) - hull_at(v, 0, qa, hr) >= hdist2(qb, v->pos) - hull_at(v, 0, qb, hr) ? 1 : -1;
        Ma2Vec3 os;   /* a spot the game offered this model at: round the way that is nearer it */
        if (offer_spot(v, me, &os)) { c->rsl_side = hdist2(qa, os) <= hdist2(qb, os) ? 1 : -1; c7_add("rimslide", "side_learned", 1); }
    }
    float s = c->rsl_side > 0 ? 1.f : -1.f;
    c->rsl_pt = ep; c->rsl_pt.x += rr * sinf(th + s * 0.6f); c->rsl_pt.z += rr * cosf(th + s * 0.6f);
    { Ma2Vec3 os; if (offer_spot(v, me, &os) && hdist2(os, me->pos) < 3.5f) { c->rsl_pt = os; c7_add("rimslide", "to_learned", 1); } }   /* within a step: go to it */
    c->rsl_pos = me->pos; c->rsl_t = w->time; c->rsl_n++;
    c7_add("rimslide", "steps", 1);
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-RIMSLIDE] t=%.1f bot=%d step %d %s at %.1f,%.1f (%.2f m from the zone, reach %.2f) -> %.1f,%.1f veh=%08X\n",
                                           w->time, index, c->rsl_n, s > 0.f ? "+" : "-", me->pos.x, me->pos.z, dzone, zr, c->rsl_pt.x, c->rsl_pt.z, veh);
    body_walk_direct(me->actor, me->pos, c->rsl_pt, 100);
    c->mode = CM_BOARD;
}
static void ctrl_board(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me, uint32_t veh)
{
    BotCtrl *c = &b->ctrl;
    const Ma2VehicleView *v = NULL;
    for (int i = 0; i < w->nvehicles; i++) if (w->vehicles[i].actor == veh) v = &w->vehicles[i];
    if (c->board_veh != veh) {
        if (c->board_veh && env_on("RECOMP_BOT_DIAG"))
            fprintf(stderr, "[BOT-RETARGET] bot=%d from=%08X to=%08X after %.1f s\n", index, c->board_veh, veh, w->time - c->bm_t0);
        if (c->board_veh) c->bm_retargets++;
        c->board_veh = veh; c->bm_t0 = w->time; c->bm_tzone = 0.0; c->bm_len = -1.f; c->bm_kind = v ? v->kind : 0;
        c->zone_since = 0.0; c->board_taps = 0; c->have_path = 0; c->pq_stale = 1; c->next_plan = 0.0;
        c->orb_t = 0.0; c->orb_side = 0; c->orb_n = 0;
    }
    if (w->time < c->esc_until) { body_walk_direct(me->actor, me->pos, c->esc_pt, 100); c->dest = c->esc_pt; c->mode = CM_MOVE; return; }
    if (!v || v->occupied || !v->has_entry) { body_stop(me->actor); return; }    /* the brain picks another next step */
    uint32_t p = b->player;
    Ma2Vec3 ep = v->entry;
    /* The game's own word that we are in the zone: it offers THIS vehicle (+0x428 = its UID).
     * Trust it over our distance to the zone centre (a centre tucked against a hull or wall). */
    int offered = MEM32(p + 0x428) && MEM32(p + 0x428) == MEM32(veh + 0x2C) && hdist2(me->pos, ep) <= v->entry_r + 3.f;
    if (offered && me->kind == MA2_KIND_PILOT && v->kind != MA2_KIND_BATTLEARMOR) offer_learn(v, me, w->time);   /* RimSlide */
    /* The zone is a sphere: a VTOL's sits 2 m up by its door, so across the ground it reaches
     * less than its radius. Every distance below is against that reach at our feet. */
    float zdy = ep.y - me->pos.y, zr = v->entry_r;
    if (fabsf(zdy) < zr - 0.5f) zr = sqrtf(zr * zr - zdy * zdy); else zr = 0.5f;
    float dzone = hdist2(me->pos, ep);
    /* Where in the zone to stand: of the centre, 8 points halfway out and 16 near its rim, the
     * closest one with room for a pilot -- clear of walls and of the vehicle's own hull (the centre
     * often sits against the hull; walking straight at it wedges the pilot against the 'Mech). The
     * rim matters: a VTOL's zone is centred under its nose, 3 m from its centre, and only the rim
     * beside the cockpit clears the outline (it reaches the gun barrel's tip across the nose). */
    Ma2Vec3 aim = ep, deep = ep; float deepd = 1e9f;
    {
        /* Nothing clear: the zone's far side from the vehicle (the zone is where a pilot stands,
         * so its outer part is open ground even when our outline of the hull is too big). */
        float ux = ep.x - v->pos.x, uz = ep.z - v->pos.z, ul = hypotf(ux, uz);
        if (ul > 0.3f) { aim.x = ep.x + ux / ul * (zr - 0.4f); aim.z = ep.z + uz / ul * (zr - 0.4f); }
        float best = 1e9f, hull = hull_r(v, 0);
        for (int k = -1; k < 24; k++) {
            Ma2Vec3 q = ep;
            float rr = k < 8 ? zr * 0.55f : zr * 0.85f, a = k < 8 ? (float)k * 0.785f : (float)(k - 8) * 0.3927f;
            if (k >= 0) { q.x += rr * sinf(a); q.z += rr * cosf(a); }
            if (ma2_trav_ready() && ma2_trav_clear(q.x, q.z, w->time) < 1.2f) continue;
            if (hdist2(q, v->pos) < hull_at(v, 0, q, hull)) continue;
            if (c->wr_veh == veh) {   /* WedgeRetry: not where a try on this vehicle already stuck -- round to another side */
                int near_bad = 0;
                for (int i = 0; i < c->wr_n && i < 3; i++) if (hdist2(q, c->wr_pt[i]) < 2.5f || hdist2(me->pos, c->wr_pt[i]) < 1.0f && hdist2(q, c->wr_pt[i]) < 3.5f) near_bad = 1;
                if (near_bad) continue;
            }
            float dq = hdist2(q, me->pos);
            if (dq < best) { best = dq; aim = q; }
            { float de = hdist2(q, ep); if (de < deepd) { deepd = de; deep = q; } }   /* BoardDeep: the clear spot nearest the zone centre */
        }
    }
    /* At the edge of the zone with a clear line to that spot (the hull is not in between -- the
     * zone may lie on the far side of the vehicle): step in (no planner tolerance to stop us
     * short). Otherwise the planner walks round the hull to it. */
    Ma2Vec3 pre = aim;                         /* the line in, short of the spot itself (it may lie at the hull's edge) */
    { float lx = aim.x - me->pos.x, lz = aim.z - me->pos.z, ll = hypotf(lx, lz);
      if (ll > 1.2f) { pre.x -= lx / ll * 1.0f; pre.z -= lz / ll * 1.0f; } else pre = me->pos; }
    /* Or the walk there has brought us as close as it goes (its end is clear of our outline, the
     * spot may not be) and we are still outside the zone: the rest is the zone's own ground. It had
     * stood there, 1.5 m short of the rim, for 20 s (a VTOL's nose, Overfalls, 2026-10-19). */
    if (!offered && dzone > zr - 0.2f && dzone <= zr + 4.0f &&
        (!seg_hits_hull(v, 0, me->pos, pre) || (c->arrived && hdist2(me->pos, aim) <= 2.5f))) {
        if (c->near_since <= 0.0) { c->near_since = w->time; c->prog_pos = me->pos; c->prog_t = w->time; }
        /* RimSlide (default on, RECOMP_BOT_RIM_SLIDE / [Bots] RimSlide): pressed at the rim, slide round the zone instead of pushing
         * the same line. The measured hull is a sector maximum: New River City's CTF VTOL (zone r 4.1, 1.9 m up, centre 2.95 m from
         * the hull's) measures 6.6 m in the zone's sectors (the wing), so every spot in the zone was "inside the hull", the aim was the
         * fixed far-side point, and the enemy pilot pushed into the wing at 1329.8,734.6 (4.26 m out, the reach at the feet 3.63 m)
         * for 7 s, three WedgeRetry tries on the same line, then gave up (vl1 11-04 t=9.9-17.5). The game offered the same VTOL to a
         * pilot standing 5.84 m from its centre, under the wing on the zone's east side (player+0x428 at 1329.9,732.8, vl2 t=6.97).
         * So each step walks 34 degrees round the zone and in (to 1.2 m inside the reach), first toward the side with more measured
         * room, the other way once a step gets nowhere; up to 6 steps, then WedgeRetry as before. [C7] rimslide: steps / flips. */
        if (c7_on("rimslide", "RECOMP_BOT_RIM_SLIDE", "RimSlide", 1) && me->kind == MA2_KIND_PILOT) {
            if (c->rsl_veh != veh) { c->rsl_veh = veh; c->rsl_n = 0; c->rsl_side = 0; c->rsl_t = 0.0; }
            if (c->rsl_t > 0.0 && w->time >= c->rsl_t) {
                if (w->time - c->rsl_t < 1.8 && hdist2(me->pos, c->rsl_pt) > 0.4f) {   /* a step under way */
                    body_walk_direct(me->actor, me->pos, c->rsl_pt, 100);
                    c->mode = CM_BOARD;
                    return;
                }
                if (hdist2(me->pos, c->rsl_pos) < 0.4f && c->rsl_side && abs(c->rsl_side) < 2) { c->rsl_side = -2 * c->rsl_side; c7_add("rimslide", "flips", 1); }   /* got nowhere: the other way round, once */
                c->rsl_t = 0.0; c->near_since = w->time; c->prog_pos = me->pos; c->prog_t = w->time;
            }
            if (hdist2(me->pos, c->prog_pos) >= 0.4f) { c->prog_pos = me->pos; c->prog_t = w->time; }   /* headway rolls: a pilot that crept, then stuck, is stuck */
            if (c->rsl_n < 6 && w->time - c->near_since > 1.2 && (w->time - c->prog_t > 1.2 || w->time - c->near_since > 3.5)) {   /* (and before the 4 s give-up: vm1 bot 9 crept 0.5 m, then sat until it) */
                rim_step(index, c, w, me, v, ep, zr, dzone, veh);
                return;
            }
        }
        body_walk_direct(me->actor, me->pos, aim, 60);
        c->mode = CM_BOARD;
        if (w->time - c->near_since > 4.0 ||                                   /* the zone will not take us */
            (w->time - c->near_since > 1.2 && w->time - c->prog_t > 1.2 && hdist2(me->pos, c->prog_pos) < 0.4f)) {   /* pressed against something (RimSlide rolls prog_*: 1.2 s without headway) */
            if (env_on("RECOMP_BOT_DIAG")) {
                Ma2Vec3 from = me->pos, to = aim; uint32_t hit = 0;
                from.y += 1.0f; to.y = from.y;
                int clr = ai_los(me->actor, 0, from, to, &hit);
                fprintf(stderr, "[BOT-WEDGE] bot=%d pos=%.1f,%.1f,%.1f aim=%.1f,%.1f veh=%08X at %.1f,%.1f kind=%d zone=%.1f,%.1f r=%.1f ray=%d hit=%08X cls=%08X\n", index,
                        me->pos.x, me->pos.y, me->pos.z, aim.x, aim.z, veh, v->pos.x, v->pos.z, v->kind, ep.x, ep.z, v->entry_r, clr, hit, gva_ok(hit, 0x30) ? MEM32(hit + 0x28) : 0u);
            }
            /* WedgeRetry (default on, RECOMP_BOT_WEDGE_RETRY / [Bots] WedgeRetry): pressed against the hull 0.1 m outside the zone
             * the vehicle was given up at once and refused 90 s -- the enemy pilot sent to their VTOL at the start (CtfVtolPick)
             * stood 4.26 m from a 4.1 m zone, then took a tank, so no flag run (NRC CTF test vp1 11-04, t=9.9). A wedge now
             * learns the hull, steps back and comes in again from the next clear spot (up to 3 times on this vehicle); only the
             * 4 s "the zone will not take us" or a third wedge give it up. [C7] wedgeretry: retries, gave_up. */
            /* (also the 4 s timeout: the walk never reached a spot under a VTOL's wing -- 1.9 m from it, 4.2 m from the zone's
             * centre, vi2 11-04 t=6-10 -- the hull learnt there moves the next spot round) */
            int pressed = !(w->time - c->near_since > 4.0);
            if (c->wr_veh != veh) { c->wr_veh = veh; c->wr_n = 0; }
            int retry = c->wr_n < 3 && c7_on("wedgeretry", "RECOMP_BOT_WEDGE_RETRY", "WedgeRetry", 1);
            if (retry) { if (c->wr_n < 3) c->wr_pt[c->wr_n] = aim; c->wr_n++; c7_add("wedgeretry", pressed ? "retries_pressed" : "retries_timeout", 1); }   /* the spot it could not reach is skipped next time */
            else { b->status.enter_failed = veh; c7_add("wedgeretry", "gave_up", 1); }
            c->near_since = 0.0;
            hull_learn(v, hdist2(me->pos, v->pos) + 0.7f, w->time, me->pos);
            /* RimSlide: the step back reaches as far as this hull does -- 5 m missed a VTOL whose centre stood 7 m off, so retries
             * 1 and 2 never stepped back and pushed the same line again (vl1 11-04: esc_until stayed 0 until the give-up) */
            pilot_escape(c, w, me, retry ? (c7_on("rimslide", "RECOMP_BOT_RIM_SLIDE", "RimSlide", 1) ? maxf2(5.f, hull_r(v, 0) + 3.f) : 5.f) : 12.f);
            c->rsl_n = 0; c->rsl_side = 0; c->rsl_t = 0.0;   /* RimSlide: the next try slides afresh */
        }
        return;
    }
    c->near_since = 0.0;
    if (env_on("RECOMP_BOT_BOARD_DIAG") && w->time - c->bdiag_t >= 1.0) {
        c->bdiag_t = w->time;
        fprintf(stderr, "[BOT-BOARDDIAG] bot=%d t=%.1f kind=%d me=%.1f,%.1f veh=%.1f,%.1f entry=%.1f,%.1f r=%.1f aim=%.1f,%.1f dzone=%.1f offered=%d hulls_clear=%d seg_hits=%d hull_r=%.1f at_aim=%.1f path=%d/%d ey=%.1f my=%.1f d3=%.1f\n",
                index, w->time, v->kind, me->pos.x, me->pos.z, v->pos.x, v->pos.z, ep.x, ep.z, v->entry_r, aim.x, aim.z, dzone, offered,
                hulls_clear(w, me, me->pos, aim, 0), seg_hits_hull(v, 0, me->pos, aim), hull_r(v, 0), hull_at(v, 0, aim, -1.f), c->path.next, c->have_path ? c->path.n : -1,
                ep.y, me->pos.y, sqrtf(dzone * dzone + (ep.y - me->pos.y) * (ep.y - me->pos.y)));
        if (c->have_path) {
            fprintf(stderr, "[BOT-BOARDPATH] bot=%d", index);
            for (int i = c->path.next; i < c->path.n && i < c->path.next + 8; i++) fprintf(stderr, " %.1f,%.1f", c->path.pt[i].x, c->path.pt[i].z);
            fprintf(stderr, "%c", 10);
        }
    }
    if (offered || hdist2(me->pos, ep) <= zr - 0.2f) {
        /* Inside the zone. The zone's offer (sub_154678 -> sub_C86EC, promoted into +0x424..+0x444
         * by sub_C8576 earlier in this player tick) names the vehicle at +0x428 (UID) only on frames
         * with a fresh offer -- the frames on which enter can board (sub_CADEE). */
        if (c->bm_tzone <= 0.0) c->bm_tzone = w->time;
        /* BoardDeep (default on, RECOMP_BOT_BOARD_DEEP / [Bots] BoardDeep): the pilot stopped the moment it was 0.2 m inside the rim (dzone = r - 0.2) and
         * waited for the prompt: every boarding that failed without one in the sweep stood there (WP2-C: sw30 2.7 / 2.8 / 2.8 of r 3.0, sw33 4.8 / 4.8 /
         * 3.8 of r 5.0, player+0x428 = 0 for 3 s, then the hull refused by the roster for 90 s); every boarding that worked stood 1.0-1.7 m from
         * the centre. So it walks on to the clear spot nearest the centre (within max(1.3 m, half the reach) of it) before it waits; the 3 s wait
         * starts once it is there, or after 2.5 s of walking. [C7] boarddeep: walks / walk_s / settled / timeout. */
        /* BaCentre (default on, RECOMP_BOT_BA_CENTRE / [Bots] BaCentre): a BattleArmor's entry zone is centred on the BA itself and the prompt only came to
         * pilots within 0.7-0.8 m of that centre (WP2-C, c3e6039: successes sw14 0.8, sw63 0.7; failures 2.0-3.8 m out, where BoardDeep stopped, 0 of 5
         * LMS tries boarded). So for a BA the pilot walks on to the centre itself, through the hull clearance (the BA is 1.8 m across; the zone
         * is where a pilot stands), for up to 4 s, and then waits the usual 3 s for the entry. [C7] bacentre: walks / reached / timeout. */
        if (ba_centre_on() && v->kind == MA2_KIND_BATTLEARMOR && !offered && hdist2(me->pos, ep) > 0.8f) {
            if (c->bd_t == 0.0) { c->bd_t = w->time; c7_add("bacentre", "walks", 1); }
            if (c->bd_t > 0.0 && w->time - c->bd_t < 6.0) {
                c7_add("bacentre", "walk_s", w->dt);
                Ma2Vec3 bt = ba_approach(v, me, ep); if (bt.x != ep.x || bt.z != ep.z) c7_add("bacentre", "orbit_s", w->dt);
                body_walk_direct(me->actor, me->pos, bt, 100);   /* full pace: the hull pushes a slow pilot out */
                c->mode = CM_BOARD;
                return;
            }
        }
        if (c->bd_t > 0.0 && ba_centre_on() && v->kind == MA2_KIND_BATTLEARMOR) {
            c7_add("bacentre", hdist2(me->pos, ep) <= 0.8f || offered ? "reached" : "timeout", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-BACENTRE] t=%.1f bot=%d ended %.2f m from the centre, %.2f m from the BA (zone r %.1f), offered=%d, took %.1f s\n", w->time, index, hdist2(me->pos, ep), hdist2(me->pos, v->pos), v->entry_r, offered, w->time - c->bd_t);
            c->bd_t = -1.0;
        }
        int want_deep = board_deep_on() && v->kind != MA2_KIND_BATTLEARMOR && !offered && deepd < 1e8f && dzone > maxf2(1.3f, 0.5f * zr) && hdist2(me->pos, deep) > 0.6f;
        if (want_deep && c->bd_t == 0.0) { c->bd_t = w->time; c7_add("boarddeep", "walks", 1); }
        if (want_deep && c->bd_t > 0.0 && w->time - c->bd_t < 2.5) {
            c7_add("boarddeep", "walk_s", w->dt);
            body_walk_direct(me->actor, me->pos, deep, 60);
            c->mode = CM_BOARD;
            return;
        }
        if (c->bd_t > 0.0) { c7_add("boarddeep", want_deep ? "timeout" : "settled", 1); c->bd_t = -1.0; }
        if (c->zone_since <= 0.0) c->zone_since = w->time;
        /* RimSlide inside the zone: "inside" by our sphere is not where the game offers -- vn1 11-04 bot 9 stood 3.46 m from the NRC
         * VTOL's zone centre (reach 3.63) just in front of its nose (-19 deg, 6.1 m from the hull centre) for 3 s with no offer, was
         * refused the VTOL and took a tank: its team made no opening run. Every offer seen came from the sides (+-27-45 deg). So 1 s
         * without an offer is a step: to a learned spot, else round the zone (6 steps at most, then the usual 3 s wait). */
        if (c7_on("rimslide", "RECOMP_BOT_RIM_SLIDE", "RimSlide", 1) && me->kind == MA2_KIND_PILOT && v->kind != MA2_KIND_BATTLEARMOR && !offered) {
            if (c->rsl_veh != veh) { c->rsl_veh = veh; c->rsl_n = 0; c->rsl_side = 0; c->rsl_t = 0.0; }
            if (c->rsl_t > 0.0 && w->time >= c->rsl_t) {
                if (w->time - c->rsl_t < 1.8 && hdist2(me->pos, c->rsl_pt) > 0.4f) { body_walk_direct(me->actor, me->pos, c->rsl_pt, 100); c->mode = CM_BOARD; return; }
                c->rsl_t = 0.0; c->zone_since = w->time;   /* a fresh wait where the step ended */
            }
            if (c->zone_since > 0.0 && w->time - c->zone_since > 1.0 && c->rsl_n < 6) {
                c7_add("rimslide", "inside_steps", 1);
                rim_step(index, c, w, me, v, ep, zr, dzone, veh);
                c->zone_since = 0.0;
                return;
            }
        }
        if (ba_centre_on() && v->kind == MA2_KIND_BATTLEARMOR && !offered && hdist2(me->pos, ep) > 0.8f) body_walk_direct(me->actor, me->pos, ba_approach(v, me, ep), 100);   /* BaCentre: keep pressing in while it waits */
        else body_stop(me->actor);
        int offer = MEM32(p + 0x428) && MEM32(p + 0x428) == MEM32(veh + 0x2C);
        if (offer && !c->press[39] && c->val[39] == 0.f) {
            ctrl_tap(c, 39, 1);
            c->board_taps++;
            if (env_on("RECOMP_BOT_DIAG") && c->board_taps <= 3)
                fprintf(stderr, "[BOT-BOARD] bot=%d tap 39 veh=%08X kind=%d try=%d t=%.1f\n", index, veh, v->kind, c->board_taps, w->time);
        }
        if (w->time - c->zone_since > 3.0) {                 /* the zone never offered it to us */
            if (env_on("RECOMP_BOT_DIAG")) board_probe(index, p);
            b->status.enter_failed = veh; c->zone_since = 0.0;
        }
        c->mode = CM_BOARD;
        return;
    }
    c->zone_since = 0.0; c->bd_t = 0.0;
    /* F16 (bots batch 1): stood still within 15 m of the hull, outside its zone (editor maps: the
     * zone on the hull's far side, MoveTo halting against it 5-12 m short, then the stall check
     * dropping the ride): step round the hull toward the zone's side, a sixth of a turn at a time,
     * keeping the side first chosen; up to 6 steps per vehicle. Before the 2.5 s boarding stall. */
    if (ma2_bot_entry_fix_on() && me->kind == MA2_KIND_PILOT && hdist2(me->pos, v->pos) < 15.f && c->orb_n < 6) {
        if (c->orb_t <= 0.0 || hdist2(me->pos, c->orb_pos) > 1.0f) { c->orb_pos = me->pos; c->orb_t = w->time; }
        else if (w->time - c->orb_t > 1.5) {
            float am = atan2f(me->pos.x - v->pos.x, me->pos.z - v->pos.z), da = atan2f(aim.x - v->pos.x, aim.z - v->pos.z) - am;
            while (da > 3.1415927f) da -= 6.2831853f;
            while (da < -3.1415927f) da += 6.2831853f;
            if (!c->orb_side) c->orb_side = da >= 0.f ? 1 : -1;
            float a = am + (fabsf(da) < 1.05f ? da : 1.05f * (float)c->orb_side), rr = hull_r(v, 0) + 2.5f;
            c->esc_pt = v->pos; c->esc_pt.x += rr * sinf(a); c->esc_pt.z += rr * cosf(a);
            c->esc_until = w->time + 1.8; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 1.8;
            c->orb_t = 0.0; c->orb_n++; c->prog_t = w->time; c->prog_pos = me->pos;
            c7_add("entry", "orbits", 1);
            if (env_on("RECOMP_BOT_DIAG"))
                fprintf(stderr, "[BOT-ENTRY] bot=%d orbit %d veh=%08X kind=%d me=%.1f,%.1f hull=%.1f,%.1f r=%.1f zone=%.1f,%.1f dzone=%.1f side=%d -> %.1f,%.1f t=%.1f\n", index, c->orb_n, veh, v->kind,
                        me->pos.x, me->pos.z, v->pos.x, v->pos.z, hull_r(v, 0), ep.x, ep.z, dzone, c->orb_side, c->esc_pt.x, c->esc_pt.z, w->time);
            body_walk_direct(me->actor, me->pos, c->esc_pt, 100); c->dest = c->esc_pt; c->mode = CM_MOVE;
            return;
        }
    }
    /* Taking far longer than the walk should (twice the path at a pilot's pace, plus slack): this
     * one is not reachable from here after all -- the brain takes another. */
    if (c->bm_len > 0.f && w->time - c->bm_t0 > 12.0 + 2.0 * c->bm_len / 8.0) { b->status.enter_failed = veh; c->bm_t0 = w->time; }
    /* Walk to that spot; MoveTo itself halts a few metres before its point. */
    ctrl_travel(index, b, w, me, aim, 1.0f, 0, kind_stop_margin(me->kind));
    if (c->bm_len < 0.f && c->have_path) c->bm_len = path_length(&c->path, me->pos);
}

/* ---- engagement ---- */
static int ctrl_can_fight(int kind) { return kind == MA2_KIND_MECH || kind == MA2_KIND_TANK || kind == MA2_KIND_BATTLEARMOR || kind == MA2_KIND_PILOT || kind == MA2_KIND_VTOL; }

static void ctrl_engaged(BotCtrl *c, Bot *b, const Ma2World *w)
{
    if (!c->engaged) { c->engaged = 1; c->eng_since = w->time; c->engagements++; }
    c->mode = CM_ATTACK;
    c->eng_time += w->dt > 0.f && w->dt < 0.25f ? w->dt : 1.f / 60.f;
    b->status.engaged = 1; b->status.engaged_tgt = c->tgt;
}

/* Hand the body back from the AI attack state (it must not keep fighting on its own). */
static void ctrl_release_attack(BotCtrl *c, uint32_t ai)
{
    if (ai && (MEM8(ai + 0x90) || MEM32(ai + 0xB4))) { MEM8(ai + 0x90) = 0; MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0; }
    c->engaged = 0;
}

/* Just climbed in with a teammate still on foot under us: wait a moment before moving off. */
/* F79b (switch RECOMP_BOT_HIDE_EXACT / ini BotHideExact): a hide corner sits against a wall, inside the
 * cells the walk planner keeps a body's width clear of it, so the route ended 3-4 m short of it --
 * still in the target's shot line. Within 8 m of an exact point, not above us (BattleArmor hops: below
 * is fine) and with no structure in between, a 'Mech or BattleArmor steers at the point itself and stops inside the
 * order's radius. No headway for 1.5 s (the wall after all): back to the planner for 3 s.
 * ExactDirect (default on, RECOMP_BOT_EXACT_DIRECT / [Bots] ExactDirect): the steer went through the awake MechAI's goto,
 * which counts itself there ~7 m out -- a BattleArmor the player set down by their flag stood 7.2 m from it, 1.3 s at
 * 0 m/s steering at it, nobody within 100 m (a playtester's game 11-03, botrec t=38.2-39.5), and never touched it. Now the
 * last 8 m are walked on the body's own turn + forward speed (body_walk_direct, as a pilot's entry walk), half speed
 * inside 3 m. [C7] exactdirect: frames. */
static int exact_direct_on(void) { static int on = -1; if (on < 0) on = c7_on("exactdirect", "RECOMP_BOT_EXACT_DIRECT", "ExactDirect", 1); return on; }
static int ctrl_exact_walk(BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o)
{
    float d = hdist2(me->pos, o->point), reach = o->exact_r > 0.f ? o->exact_r : 8.f;
    int kind_ok_ = me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR || (o->exact_r > 0.f && me->kind == MA2_KIND_TANK) ||
                   /* PilotExact (default on, RECOMP_BOT_PILOT_EXACT / [Bots] PilotExact): HoldStill's exact walk never ran for a pilot -- d5_70 b8
                    * circled terminal K1 3 m short at 5.5 m/s for 36 s (26 hold/claim flips, 0 captures). An order with an exact reach walks a pilot in too. */
                   (o->exact_r > 0.f && me->kind == MA2_KIND_PILOT && c7_on("pilotexact", "RECOMP_BOT_PILOT_EXACT", "PilotExact", 1));
    if (!o->exact || !kind_ok_ || d > reach || w->time < c->ex_off_until) return 0;
    if (goal_rise(o->point, me) > (o->exact_r > 0.f ? 4.f : 2.5f) || !c7_on("f79b", "RECOMP_BOT_HIDE_EXACT", "BotHideExact", 1)) return 0;
    for (float s = 1.f; s < d - 0.5f; s += 1.f) {
        int ok = 0;
        float sf = surface_height(me->pos.x + (o->point.x - me->pos.x) * s / d, me->pos.z + (o->point.z - me->pos.z) * s / d, &ok);
        if (ok && sf > maxf2(me->pos.y, o->point.y) + 2.f) { c7_add("f79b", "walled", 1); return 0; }
    }
    if (w->time - c->ex_last > 0.3) { c->ex_pos = me->pos; c->ex_t = w->time; }
    c->ex_last = w->time;
    if (hdist2(me->pos, c->ex_pos) > 0.7f) { c->ex_pos = me->pos; c->ex_t = w->time; }
    else if (w->time - c->ex_t > 1.5 && (o->touch || d > maxf2(o->stop, 0.8f))) {
        c->ex_off_until = w->time + 3.0; c7_add("f79b", "stalls", 1);
        return 0;
    }
    if (o->touch) {   /* FlagTouch: no arrival -- onto the point, and over it (a step past, then back) until it is ours */
        Ma2Vec3 to = o->point;
        /* TouchJetCut (default on, RECOMP_BOT_TOUCH_JET_CUT / [Bots] TouchJetCut): a jet burn started by the order before (a retreat hop)
         * flew on under the touch walk: bs62b b0 (CTF Going Commando) went retreat -> grab 0.5 s into a 2 s hop and crossed blue's flag
         * 6.5 m up at 30 m/s (196.5-197.8), bs62a b7 the same over red's (53.1-54.9); 0 grabs either side in two runs. On a touch order
         * inside 30 m a running jump ends (the jet goes out; the body comes down by the flag). [C7] flagtouch: jet_cut. */
        if ((me->kind == MA2_KIND_BATTLEARMOR || me->kind == MA2_KIND_MECH) && d < 30.f && c7_on("touchjetcut", "RECOMP_BOT_TOUCH_JET_CUT", "TouchJetCut", 1)) {
            uint32_t jai = actor_ai(me->actor);
            if (jai && MEM8(jai + 0x240)) {
                MEM8(jai + 0x240) = 0;
                MEM32(me->actor + 0x268) &= ~0x8000u; MEMF(me->actor + 0x4E4) = 0.f; MEMF(me->actor + 0x4E8) = 0.f;
                c->press[3] = 0; c->jump_next = w->time + 2.0; c7_add("flagtouch", "jet_cut", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-TOUCHJET] t=%.1f jet cut %.0f m from the flag at %.0f,%.0f,%.0f\n", w->time, d, me->pos.x, me->pos.y, me->pos.z);
            }
        }
        if (d < 1.0f) {
            if (c->ft_over <= 0.0 || w->time - c->ft_over > 1.5 || w->time < c->ft_over) { c->ft_over = w->time; c7_add("flagtouch", "passes", 1); }
            float hx = me->vel.x, hz = me->vel.z, hl = hypotf(hx, hz);
            if (hl < 0.5f) { hx = 1.f; hz = 0.f; hl = 1.f; }
            to.x += hx / hl * 3.f; to.z += hz / hl * 3.f;
        }
        /* TankTouch (default on, RECOMP_BOT_TANK_TOUCH / [Bots] TankTouch): MoveTo orbits a point a tank cannot quite reach: ad1_64 (CTF NRC
         * Dragoon) b4's tank circled the standing flag at 11-18 m for 10 s (132-142) and never touched it. The hull turns on the spot when the
         * flag is 45 deg+ off its nose, else drives straight at it on its own turn + speed. [C7] tanktouch: frames. */
        if (me->kind == MA2_KIND_TANK && c7_on("tanktouch", "RECOMP_BOT_TANK_TOUCH", "TankTouch", 1)) {
            c7_add("tanktouch", "frames", 1);
            if (fabsf(wrap_pi(atan2f(to.x - me->pos.x, to.z - me->pos.z) - me->yaw)) > 0.785f) body_turn_in_place(me->actor, me->pos, to);
            else body_walk_direct(me->actor, me->pos, to, d < 3.f ? 50 : 100);
        }
        else if (me->kind == MA2_KIND_TANK) body_steer(me, to);
        else {
            /* TouchStride (default on, RECOMP_BOT_TOUCH_STRIDE / [Bots] TouchStride; playtest 11-05 tiny steps toward a power-up): 100 % of
             * sub_2AD5C0 walked a 'Mech at ~7.5 m/s against 16-19 under the navigator, over the whole 30 m PuReach run-in (a playtester's TD game
             * 11-05 b0 166-168.6: 7.4-7.9 m/s, 25 -> 8 m). A bigger percentage does not help (250 %: sz1_20 median 8.8 m/s -- the direct walk's
             * gait is the cap). Lined up (within 20 deg) and 8 m+ out, the navigator runs the 'Mech in at full speed instead (ctrl_exact_walk
             * returns 0); the direct walk only turns it in (PuReach's orbit) and takes the last 8 m. The touch counts as collected up to
             * 6 m + 0.5 s of speed off the spot (order_powerup). sz3_20: got 12 / gone_far 3. [C7] touchstride: frames. */
            /* TouchStall (default on, RECOMP_BOT_TOUCH_STALL / [Bots] TouchStall): the navigator's awake drive parks a 'Mech ~10 m short of its
             * point, so a lined-up 'Mech handed to it stood still: B_803 b2 (core grab, 3 free cores) stood 10.4-11.2 m from the core at 0 m/s
             * for 4 s (73.9-77.8, A348 each frame, lined up within 0.35 rad), under fire from two enemies. Once the handed-off 'Mech has stood
             * under 1.5 m/s for 0.5 s inside 16 m (not the start-of-match wait far out), the direct walk takes it the rest of the way to this point. [C7] touchstall: n. */
            static const BotCtrl *ts_c[16]; static Ma2Vec3 ts_pt[16]; static double ts_since[16], ts_last[16], ts_sl[16]; static int ts_latch[16];
            int tsi = 0; while (tsi < 15 && ts_c[tsi] && ts_c[tsi] != c) tsi++;
            if (ts_c[tsi] != c || hdist2(ts_pt[tsi], o->point) > 4.f || w->time < ts_last[tsi] || w->time - ts_last[tsi] > 5.0) {
                ts_c[tsi] = c; ts_pt[tsi] = o->point; ts_since[tsi] = 0.0; ts_latch[tsi] = 0;
            }
            if (d > 8.f && me->kind == MA2_KIND_MECH && fabsf(wrap_pi(atan2f(to.x - me->pos.x, to.z - me->pos.z) - me->yaw)) < 0.35f &&
                !ts_latch[tsi] && c7_on("touchstride", "RECOMP_BOT_TOUCH_STRIDE", "TouchStride", 1)) {
                if (w->time - ts_sl[tsi] > 0.25 || hypotf(me->vel.x, me->vel.z) >= 1.5f || d > 16.f) ts_since[tsi] = w->time;
                ts_sl[tsi] = ts_last[tsi] = w->time;
                if (w->time - ts_since[tsi] > 0.5 && c7_on("touchstall", "RECOMP_BOT_TOUCH_STALL", "TouchStall", 1)) {
                    ts_latch[tsi] = 1; c7_add("touchstall", "n", 1);
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-TOUCHSTALL] t=%.1f navigator parked %.1f m short at %.0f,%.0f; direct walk\n", w->time, d, me->pos.x, me->pos.z);
                } else { c7_add("touchstride", "frames", 1); return 0; }
            }
            ts_last[tsi] = w->time;
            /* BaTouchThrough (default on, RECOMP_BOT_BA_TOUCH_THROUGH / [Bots] BaTouchThrough): a BattleArmor's direct walk at the flag circled it
             * (d5_60 b3 35.4-44.0: 6 passes, never closer than 1.35 m, the over-step only starts inside 1 m). Inside 3 m it aims 2.5 m past the
             * flag on the line through it, so the body runs across the spot. [C7] flagtouch: ba_through. */
            if (me->kind == MA2_KIND_BATTLEARMOR && d < 3.f && d > 0.2f && c7_on("batouchthrough", "RECOMP_BOT_BA_TOUCH_THROUGH", "BaTouchThrough", 1)) {
                to.x = o->point.x + (o->point.x - me->pos.x) / d * 2.5f; to.z = o->point.z + (o->point.z - me->pos.z) / d * 2.5f;
                c7_add("flagtouch", "ba_through", 1);
            }
            body_walk_direct(me->actor, me->pos, to, d < 3.f ? 50 : 100);
        }
        c->dest = to; c->mode = CM_MOVE; c->arrived = 0; c7_add("flagtouch", "frames", 1);
        return 1;
    }
    if (d <= maxf2(o->stop, 0.8f)) { body_stop(me->actor); c->mode = CM_STOP; c->arrived = 1; c->ex_t = w->time; }
    else {
        if (exact_direct_on() && me->kind != MA2_KIND_TANK) { body_walk_direct(me->actor, me->pos, o->point, d < 3.f ? 50 : 100); c7_add("exactdirect", "frames", 1); }
        else body_steer(me, o->point);
        c->dest = o->point; c->mode = CM_MOVE; c->arrived = 0; c7_add("f79b", "exact_frames", 1);
    }
    return 1;
}
static int ctrl_underfoot(const BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    if (me->kind == MA2_KIND_PILOT || w->time - c->actor_since >= 6.0) return 0;
    int under = 0;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *q = &w->players[i];
        if (q != me && q->alive && q->kind == MA2_KIND_PILOT && q->nation == me->nation && hdist2(q->pos, me->pos) < 9.f) { under = 1; break; }
    }
    if (under && me->kind == MA2_KIND_VTOL && vt_lift_off_on()) {             /* VtolLiftOff: off the ground or with a rider on, nobody is under its feet */
        int okg = 0, rider = 0; float g = terrain_height(me->pos.x, me->pos.z, &okg);
        for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == me->actor) rider = 1;
        if (rider || (okg && me->pos.y - g > 4.f)) { c7_add("vtliftoff", "underfoot_skip_s", w->dt); return 0; }
    }
    return under;
}

/* ---- 'Mech / tank fight movement (ours; aim and fire are native, through ctrl_fire) ----
 * Inside the order's distance band the body keeps moving like a player holding a direction:
 * 'Mechs strafe across the line of fire and flip side every few seconds; tanks mostly sit on
 * their stand-off spot and snipe, shifting now and then. Too close: back away to the side (a
 * tank under a 'Mech's feet is dead). Every spot is checked against the traversability grid. */
static float ctrl_rand(int index, double t, int salt)
{
    uint32_t h = (uint32_t)(t * 1000.0) * 2654435761u ^ (uint32_t)(index * 40503 + salt * 97);
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return (float)(h & 0xFFFF) / 65536.f;
}
static int fight_spot_ok(const Ma2PlayerView *me, Ma2Vec3 p, double now)
{
    float r = plan_radius(me) + 1.f, top;
    if (on_raised(me, &top)) {                  /* up on a roof: the step and its midpoint stay on it */
        int ok1 = 0, ok2 = 0;
        Ma2Vec3 mid = { 0.5f * (me->pos.x + p.x), 0.f, 0.5f * (me->pos.z + p.z) };
        float s1 = surface_height(p.x, p.z, &ok1), s2 = surface_height(mid.x, mid.z, &ok2);
        return ok1 && ok2 && fabsf(s1 - top) < 1.5f && fabsf(s2 - top) < 1.5f;
    }
    if (!ma2_trav_ready()) return 1;
    if (me->kind == MA2_KIND_MECH) return ma2_trav_clear_heavy(p.x, p.z) >= r && ma2_trav_segment_heavy(me->pos, p, r * 0.7f, now);
    return ma2_trav_clear(p.x, p.z, now) >= r && ma2_trav_segment(me->pos, p, r * 0.7f, now);
}
/* A fight step: MoveTo halts a pilot 5-10 m short of its point, so a 7 m side-step never
 * started (pilots stood still trading pistol shots); a pilot walks it directly. */
static void fight_step(const Ma2PlayerView *me, Ma2Vec3 p)
{
    /* TankPivot in a fight: the strafe / firing-spot steps go through MoveTo too, and a stopped tank with the step 90-160 deg off its nose
     * sat there (au8_25 TD Killing Fields: 11 spells of 2-6 s; b1 lost 1073 hp to a BattleArmor 10-34 m off at 177-192). Stopped (no
     * navigator speed) with the step 45 deg+ off and 6 m+ away: the hull turns on the spot this tick. [C7] tankpivot: fight_s. */
    if (me->kind == MA2_KIND_TANK && hypotf(me->vel.x, me->vel.z) < 1.f && hdist2(me->pos, p) > 6.f && c7_on("tankpivot", "RECOMP_BOT_TANK_PIVOT", "TankPivot", 1)) {
        uint32_t comp = MEM32(me->actor + 0x8C);
        if (gva_ok(comp, 0x150) && MEMF(comp + 0x98) < 0.5f && fabsf(wrap_pi(atan2f(p.x - me->pos.x, p.z - me->pos.z) - me->yaw)) > 0.785f) {
            body_turn_in_place(me->actor, me->pos, p); c7_add("tankpivot", "fight_s", 1.f / 60.f);
            return;
        }
    }
    if (me->kind == MA2_KIND_PILOT) body_walk_direct(me->actor, me->pos, p, 100); else body_steer(me, p);
}
static void ctrl_fight(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o, const Ma2PlayerView *t, float d)
{
    BotCtrl *c = &b->ctrl;
    int tank = me->kind == MA2_KIND_TANK;
    float bmin = o->band_min > 1.f ? o->band_min : 50.f;
    float bmax = o->band_max > bmin + 20.f ? o->band_max : bmin + 100.f, mid = 0.5f * (bmin + bmax);
    float ux = (t->pos.x - me->pos.x) / (d + 1e-3f), uz = (t->pos.z - me->pos.z) / (d + 1e-3f);
    if (c->strafe_side == 0) c->strafe_side = (index & 1) ? 1 : -1;
    if (w->time >= c->strafe_flip || w->time + 30.0 < c->strafe_flip) {
        c->strafe_side = -c->strafe_side;
        c->strafe_flip = w->time + (tank ? 5.0 + 5.0 * ctrl_rand(index, w->time, 1) : 2.5 + 2.0 * ctrl_rand(index, w->time, 1));
        c->strafe_hold = tank && ctrl_rand(index, w->time, 2) < 0.6f;
    }
    float radial, lateral;
    if (d < bmin) { radial = -(bmin - d + 25.f); lateral = 18.f; }
    else { radial = (d - mid) * 0.5f; radial = radial < -20.f ? -20.f : radial > 20.f ? 20.f : radial; lateral = tank ? 22.f : me->kind == MA2_KIND_BATTLEARMOR ? 14.f : me->kind == MA2_KIND_PILOT ? 7.f : 30.f; }
    if (o->sp_straight) lateral = 0.f;                      /* surprise C: straight lines, no strafing */
    int blocked = w->team_mode ? ally_line_side(w, me, t->pos) : 0;
    if (tank && c->strafe_hold && d >= bmin && !blocked) {
        body_stop(me->actor); c->dest = me->pos; c->fs_t = w->time; return;
    }
    /* A teammate stands in the line of fire: step out to the side away from him, as a player
     * would, instead of waiting for him to move. */
    if (blocked && d >= bmin) { c->strafe_side = -blocked; c->strafe_flip = w->time + 2.0; lateral = maxf2(lateral, 24.f); }
    /* Strafing but going nowhere (a spot the grid calls free that the body cannot reach -- a
     * kerb, a wreck, a teammate's hull): after 3 s switch sides, after 6 s look all round. */
    if (hdist2(me->pos, c->fs_pos) > 2.f || c->fs_t <= 0.0 || w->time < c->fs_t) { c->fs_pos = me->pos; c->fs_t = w->time; }
    else if (w->time - c->fs_t > 6.0) { c->fs_t = w->time; c->box_until = 0.0; c->box_next = 0.0; goto ring; }
    else if (w->time - c->fs_t > 3.0 && w->time >= c->strafe_flip - 1.0) { c->strafe_side = -c->strafe_side; c->strafe_flip = w->time + 3.0; }
    for (int tries = 0; tries < 3; tries++) {
        float s = tries == 2 ? 0.f : (float)c->strafe_side;
        Ma2Vec3 p = me->pos;
        p.x += ux * radial + uz * lateral * s;
        p.z += uz * radial - ux * lateral * s;
        if (fight_spot_ok(me, p, w->time)) {
            fight_step(me, p); c->dest = p; c->strafes++;
            if (me->kind == MA2_KIND_BATTLEARMOR && w->time >= c->jump_next && !on_raised(me, NULL) &&
                body_jump_fuel(me->actor, me->pos, p, 1.4f)) {
                c->jumps++; c->jump_next = w->time + (ba_walk_on() ? 9.0 + 5.0 * ctrl_rand(index, w->time, 3) : 3.0 + 2.0 * ctrl_rand(index, w->time, 3));
                if (ba_walk_on()) c7_add("bawalk", "fight_hops", 1);
            }
            return;
        }
        if (tries == 0) { c->strafe_side = -c->strafe_side; c->strafe_flip = w->time + 2.0; }
    }
    /* Boxed in (an alley, a ledge, a teammate's hull): any free step of 10-20 m will do --
     * across the line first, then on the slant, then back or forward -- rather than standing in
     * the guns. Looked for twice a second; the step found is kept until reached. */
ring:
    if (w->time < c->box_until && hdist2(me->pos, c->box_pt) > 3.f) { fight_step(me, c->box_pt); c->dest = c->box_pt; return; }
    if (w->time >= c->box_next) {
        static const float ring[8] = { 1.5708f, -1.5708f, 0.7854f, -0.7854f, 2.3562f, -2.3562f, 3.1416f, 0.f };
        c->box_next = w->time + 0.5;
        for (int ri = 0; ri < 2; ri++)
            for (int k = 0; k < 8; k++) {
                if (k == 7 && d < bmin + 10.f) continue;                    /* not in toward the target when close */
                float a = ring[k] * (float)c->strafe_side, rr = ri ? 10.f : 20.f, ca = cosf(a), sa = sinf(a);
                Ma2Vec3 p = me->pos;
                p.x += (ux * ca + uz * sa) * rr; p.z += (uz * ca - ux * sa) * rr;
                if (!fight_spot_ok(me, p, w->time)) continue;
                c->box_pt = p; c->box_until = w->time + 3.0;
                fight_step(me, p); c->dest = p; c->strafes++;
                return;
            }
    }
    if (w->time - c->box_last > 0.5 || w->time < c->box_last) c->box_since = w->time;   /* BoxWedge (ctrl_do_engage) */
    c->box_last = w->time;
    body_stop(me->actor); c->dest = me->pos;                     /* boxed in: stand and shoot */
}

/* ENGAGE. 'Mechs and tanks: in clear sight and inside the band, fight on the move (ctrl_fight)
 * with native aim and fire; otherwise close in along the planner's path and shoot when a line
 * opens. BattleArmor hops to its stand-off range and fires; VTOLs hold a stand-off hover facing
 * the target. */
static void ctrl_do_engage(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o, uint32_t ai)
{
    BotCtrl *c = &b->ctrl;
    const Ma2PlayerView *t = NULL;
    if (!ctrl_target_alive(w, o->target, &t)) { ctrl_release_attack(c, ai); body_stop(me->actor); return; }
    c->tgt = t->actor;
    float d = hdist2(t->pos, me->pos);
    int los = ctrl_shot_los(c, w, me, t), recent = w->time - c->fl_seen < 1.5;
    if (me->kind == MA2_KIND_BATTLEARMOR) {
        float bmax = o->band_max > 1.f ? o->band_max : 60.f;
        ctrl_release_attack(c, ai);
        if (on_raised(me, NULL)) bmax = maxf2(bmax, me->reach > 10.f ? me->reach : 150.f);   /* up high: hold it while the guns reach */
        if ((los || recent) && d <= bmax + 10.f) ctrl_fight(index, b, w, me, o, t, d);   /* laser + mortar from its spot */
        else ctrl_travel(index, b, w, me, o->point, maxf2(o->stop, 8.f), 0, 0.f);
        ctrl_fire(c, w, me, t->actor);
        ctrl_engaged(c, b, w);
        return;
    }
    if (me->kind == MA2_KIND_VTOL) {
        /* A gunship never hangs still in front of its target: it holds a firing hover for a few
         * seconds (shorter when it is being hit), then slides to a new spot on the circle round the
         * target -- 50-75 degrees on, mostly the same way round, inside its band -- and fires again. */
        float vmin = o->band_min > 1.f ? o->band_min : 60.f, vmax = o->band_max > vmin + 20.f ? o->band_max : 170.f;
        int hurt = o->dodge != 0;                     /* the brain sees us under fire */
        int gok = 0; float gnd = terrain_height(me->pos.x, me->pos.z, &gok);
        int grounded = gok && me->pos.y - gnd < 8.f;   /* still on (or just off) the pad: take off first */
        ctrl_release_attack(c, ai);
        if (grounded && c->vt_phase != 2) c->vt_phase = 0;
        if (grounded || d > vmax + 30.f || (!los && !recent && c->vt_phase != 2)) { c->vt_phase = 0; ctrl_travel(index, b, w, me, t->pos, vmax * 0.8f, 0, 0.f); }
        else {
            /* F81 (switch RECOMP_BOT_VTOL_TIMED / ini BotVtolTimed): with a slow gun the pass ends just
             * after it fires (Elites at once, Recruits late) and the duck lasts its refire less the
             * flight back out (Elites to ~0.2 s, Recruits a second either way). No slow gun: as before. */
            int timed = me->slow_reload > 0.f && vtol_timed_on(), sk = c->skill & 3, pass_over;
            static const float lag0[4] = { 0.8f, 0.5f, 0.3f, 0.15f }, jit0[4] = { 1.0f, 0.6f, 0.35f, 0.2f }, lagD[4] = { 0.8f, 0.5f, 0.3f, 0.05f }, jitD[4] = { 1.0f, 0.6f, 0.35f, 0.05f };
            const float *lag = ma2_bots_dragoon() ? lagD : lag0, *jit = ma2_bots_dragoon() ? jitD : jit0;   /* Dragoon: tighter VTOL passes */
            /* AirDuel (default on, RECOMP_BOT_VTOL_AIR_DUEL / [Bots] VtolAirDuel; film review 11-08): against another aircraft the hover-and-fire pass
             * is a short burst (0.7-1.3 s), then a new spot -- p5_85a1 21-33 s, rc15r_20 20-33 s, A_70a 35-45 s: both VTOLs sat at ~1 m/s trading
             * fire for 10 s+ (one died each time). A ground target keeps the long pass. [C7] airduel: bursts. */
            int airduel = (t->kind == MA2_KIND_VTOL || t->kind == MA2_KIND_FLYER) && c7_on("airduel", "RECOMP_BOT_VTOL_AIR_DUEL", "VtolAirDuel", 1);
            if (c->vt_phase == 0) { c->vt_phase = 1; c->vt_until = w->time + (airduel ? 0.7 + 0.6 * ctrl_rand(index, w->time, 41) : 1.5 + 2.0 * ctrl_rand(index, w->time, 41)); }
            if (c->vt_phase == 1 && c->vt_prev != 1) {
                c->vt_pass_t = w->time;
                if (timed && c->vt_ready_at > 0.0) { c7_add("f81", "ready_gap_s_sum", w->time - c->vt_ready_at); c7_add("f81", "ready_gap_n", 1); c->vt_ready_at = 0.0; }
            }
            if (c->vt_phase == 3 && c->vt_prev != 3) c->vt_hide_t0 = w->time;
            c->vt_prev = c->vt_phase;
            if (timed) {
                int fired = c->last_fire_t >= c->vt_pass_t;
                pass_over = (fired && w->time >= c->last_fire_t + lag[sk]) || w->time >= c->vt_pass_t + maxf2(4.f, me->slow_reload + 1.f);
            } else pass_over = w->time >= c->vt_until;
            /* Peek and hide: after a firing pass (sooner when hit), duck out of its sight -- sideways
             * behind a building, back, or down behind the ground -- for a few seconds, then come out
             * somewhere else and fire again. The skill sets how often (o->vt_cover). */
            if (c->vt_phase == 1 && (pass_over || (hurt && w->time + 1.0 < c->vt_until))) {
                c->vt_passes++;
                if (timed) { c7_add("f81", "passes", 1); if (c->last_fire_t < c->vt_pass_t) c7_add("f81", "passes_no_fire", 1); }
            }
            if (c->vt_phase == 1 && (pass_over || (hurt && w->time + 1.0 < c->vt_until)) &&
                ctrl_rand(index, w->time, 61) < o->vt_cover) {
                float ux = (t->pos.x - me->pos.x) / (d + 1e-3f), uz = (t->pos.z - me->pos.z) / (d + 1e-3f);
                float best = 1e9f; Ma2Vec3 te = t->pos; te.y += kind_eye(t->kind) * 0.6f;
                int tried = 0;
                for (int k = 0; k < 31 && tried < 24; k++) {
                    Ma2Vec3 p = me->pos;
                    if (k < 30) {
                        /* bearings from abeam (+-90 deg) round through behind; never toward it */
                        static const float brg[10] = { 1.571f, -1.571f, 2.09f, -2.09f, 1.05f, -1.05f, 2.62f, -2.62f, 3.14f, 0.0f };
                        float rr = k < 10 ? 30.f : k < 20 ? 55.f : 80.f, a = brg[k % 10];
                        if (k % 10 == 9) continue;
                        float ca = cosf(a), sa = sinf(a);
                        float dx2 = ux * ca - uz * sa, dz2 = ux * sa + uz * ca;   /* the line to the target turned by a */
                        p.x += dx2 * rr; p.z += dz2 * rr;
                        if (rr + 25.f > d && (p.x - t->pos.x) * (p.x - t->pos.x) + (p.z - t->pos.z) * (p.z - t->pos.z) < 40.f * 40.f) continue;
                    }
                    if (ma2_bot_air_blocked(p.x, p.z)) continue;
                    if (vt_edge_keep_on() && vt_edge_dist(p.x, p.z) < 25.f) continue;   /* VtolEdgeKeep */
                    int okg = 0; float g = terrain_height(p.x, p.z, &okg), sfc;
                    int oks = 0; sfc = surface_height(p.x, p.z, &oks);
                    float top = oks ? maxf2(sfc, okg ? g : sfc) : (okg ? g : me->pos.y - 10.f);
                    if (oks && okg && sfc > g + 6.f) continue;                 /* over a roof: in plain sight above it */
                    p.y = top + 6.f;                                           /* low over that spot */
                    uint32_t hit = 0; tried++;
                    if (ai_los(me->actor, t->actor, p, te, &hit) == 1) continue;          /* still in its sight */
                    float cost = hdist2(p, me->pos) + (float)k * 0.1f;
                    if (cost < best) { best = cost; c->vt_hide = p; }
                }
                c->vt_hide_tries++;
                if (best < 1e8f) {
                    c->vt_phase = 3; c->vt_until = w->time + 6.0; c->vt_hides++;
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VTOLHIDE] bot=%d ducks to %.0f,%.0f,%.0f from %.0f,%.0f,%.0f t=%.1f\n", index,
                                                           c->vt_hide.x, c->vt_hide.y, c->vt_hide.z, me->pos.x, me->pos.y, me->pos.z, w->time);
                }
            }
            if (c->vt_phase == 3 && (hdist2(me->pos, c->vt_hide) < 10.f || w->time >= c->vt_until)) {
                c->vt_phase = 4; c->vt_until = w->time + 1.8 + 1.5 * ctrl_rand(index, w->time, 67);   /* hidden: reload, wait */
                if (timed) {   /* F81: out again when the gun is ready, less about the flight in (the way back out) */
                    double ready = c->last_fire_t + me->slow_reload, back = 0.7 * (w->time - c->vt_hide_t0);
                    double out = ready - back + jit[sk] * (2.0 * ctrl_rand(index, w->time, 71) - 1.0);
                    c->vt_until = out < w->time + 0.3 ? w->time + 0.3 : out > w->time + 6.0 ? w->time + 6.0 : out;
                    c->vt_ready_at = ready;
                    c7_add("f81", "hides", 1); c7_add("f81", "hide_s_sum", c->vt_until - c->vt_hide_t0);
                }
            }
            if (c->vt_phase == 4 && w->time >= c->vt_until) { c->vt_phase = 1; c->vt_until = w->time - 1.0; c->vt_side = -c->vt_side; }
            if (c->vt_phase == 1 && (pass_over || (hurt && w->time + 1.0 < c->vt_until))) {
                if (c->vt_side == 0.f || ctrl_rand(index, w->time, 43) < 0.25f) c->vt_side = ctrl_rand(index, w->time, 47) < 0.5f ? -1.f : 1.f;
                float ang = atan2f(me->pos.x - t->pos.x, me->pos.z - t->pos.z) + c->vt_side * (0.9f + 0.4f * ctrl_rand(index, w->time, 53));
                float r = d < vmin + 10.f ? vmin + 10.f : d > vmax - 10.f ? vmax - 10.f : d;
                c->vt_spot = t->pos; c->vt_spot.x += sinf(ang) * r; c->vt_spot.z += cosf(ang) * r;
                if (vt_edge_keep_on() && vt_edge_dist(c->vt_spot.x, c->vt_spot.z) < 25.f) {   /* VtolEdgeKeep: the other side, else halfway in */
                    float a2 = atan2f(me->pos.x - t->pos.x, me->pos.z - t->pos.z) - c->vt_side * (0.9f + 0.4f * ctrl_rand(index, w->time, 53));
                    Ma2Vec3 s2 = t->pos; s2.x += sinf(a2) * r; s2.z += cosf(a2) * r;
                    if (vt_edge_dist(s2.x, s2.z) >= 25.f) { c->vt_spot = s2; c->vt_side = -c->vt_side; }
                    else { c->vt_spot.x = (me->pos.x + t->pos.x) * 0.5f; c->vt_spot.z = (me->pos.z + t->pos.z) * 0.5f; }
                    c7_add("vtedge", "spot_moved", 1);
                }
                c->vt_phase = 2; c->vt_until = w->time + 6.0;
            }
            if (c->vt_phase == 2 && (hdist2(me->pos, c->vt_spot) < 15.f || w->time >= c->vt_until)) {
                c->vt_phase = 1; c->vt_until = w->time + (hurt ? 1.0 : airduel ? 0.7 + 0.6 * ctrl_rand(index, w->time, 59) : 1.5 + 2.0 * ctrl_rand(index, w->time, 59));
                if (airduel) c7_add("airduel", "bursts", 1);
            }
            if (c->vt_phase == 2) ctrl_travel(index, b, w, me, c->vt_spot, 8.f, 0, 0.f);
            if (c->vt_phase == 3) ctrl_travel(index, b, w, me, c->vt_hide, 6.f, 0, 0.f);
            if (c->vt_phase == 4) { body_stop(me->actor); vtol_prepare(c, me, c->vt_hide, w->time); }
        }
        if (c->vt_phase == 1) {
            uint32_t comp = MEM32(me->actor + 0x8C);
            recomp_func_t face = resolve(0x002AE360u);
            body_stop(me->actor);
            vtol_prepare(c, me, t->pos, w->time);
            if (face && gva_ok(comp, 0x150)) {     /* hold and face the target (sub_2AE360) */
                GuestRegs r; regs_save(&r);
                g_esp -= 32; uint32_t v = g_esp & ~15u;
                MEMF(v) = t->pos.x - me->pos.x; MEMF(v + 4) = 0.f; MEMF(v + 8) = t->pos.z - me->pos.z;
                g_esp = v - 16; g_esi = comp; g_ecx = v; g_eax = 0; g_edx = 1;
                PUSH32(g_esp, f2u(1.0f)); PUSH32(g_esp, 0x002AE4E0u); face();
                regs_restore(&r);
            }
        }
        ctrl_fire(c, w, me, t->actor);
        ctrl_engaged(c, b, w);
        return;
    }
    if (me->kind == MA2_KIND_PILOT) {
        /* On foot: close to pistol range, then keep moving across the line of fire (a pilot that
         * stands still is the easiest kill on the map). */
        ctrl_release_attack(c, ai);
        if (d > 25.f) ctrl_travel(index, b, w, me, t->pos, 20.f, 0, 0.f);
        else { Ma2Order po = *o; po.band_min = 8.f; po.band_max = 30.f; ctrl_fight(index, b, w, me, &po, t, d); }
        ctrl_fire(c, w, me, t->actor);
        ctrl_engaged(c, b, w);
        return;
    }
    /* 'Mech / tank */
    /* WedgeLive (default on, RECOMP_BOT_WEDGE_LIVE / [Bots] WedgeLive): ctrl_release_attack clears c->engaged, so the line below reset the
     * wedge clock every frame: "fight wedged" fired 0 times in 56 sweep matches, and engagements counted every frame (s17_205a b5: 140/s).
     * The flag is kept across the release (ENGAGE restored it from was_engaged for exactly this). [C7] wedgelive: wedged. */
    int eng_keep = c->engaged && c7_on("wedgelive", "RECOMP_BOT_WEDGE_LIVE", "WedgeLive", 1);
    ctrl_release_attack(c, ai);
    if (eng_keep) c->engaged = 1;
    float bmax = o->band_max > 1.f ? o->band_max : 200.f;
    if ((los || (c->engaged && recent)) && d <= bmax + 15.f) {
        if (!c->engaged) { c->eng_pos = me->pos; c->eng_wt = w->time; c->eng_thp = t->hp; c->eng_shots = c->shots; }
        ctrl_fight(index, b, w, me, o, t, d);
        ctrl_fire(c, w, me, t->actor);
        ctrl_engaged(c, b, w);
        /* Wedged: no headway and not one shot of ours for 6 s (a teammate in the line, a blocked
         * spot) -> the brain finds a firing spot. The target losing armour to others does not
         * count: it kept a 'Mech stood holding fire behind a teammate for 50 s. */
        /* A shot is headway only for a body that means to stand (a tank sniping from its spot): one
         * whose step goes nowhere is wedged however often it fires (a 'Mech stood 20 s on a ledge
         * firing now and then while its strafes went nowhere). */
        int standing = hdist2(c->dest, me->pos) < 4.f;
        /* BoxWedge (default on, RECOMP_BOT_BOX_WEDGE / [Bots] BoxWedge): a stand that is only "boxed in" (no ring step passed) is not one the
         * body means: s17_205a TD b5 dropped to 1010,28.6,1668 at 86 and stood boxed 43 s firing at 160-260 m (96 -> 45 %); every shot reset
         * the wedge clock. Boxed 6 s+: shots are no headway, so the brain finds a firing spot. [C7] boxwedge: wedged. */
        /* W-34 BoxNear (default on, RECOMP_BOT_BOX_NEAR / [Bots] BoxNear): with an enemy 'Mech or tank inside 120 m the boxed stand
         * ends after 3 s, not 6, and the wedge fires at once instead of 6 s later (t23_203 TD b3 stood boxed 11 s, 68-79 s, while
         * b8 closed from 76 to 96 m and its hp fell 1752 -> 1126; now 73.05 s). The brain's answer (a firing spot) is unchanged. */
        int boxed = standing && w->time - c->box_last < 0.5;
        double box_lim = 6.0;
        if (boxed && c7_on("boxnear", "RECOMP_BOT_BOX_NEAR", "BoxNear", 1))
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *q = &w->players[i];
                if (q->alive && q->player != me->player && !(w->team_mode && q->nation == me->nation) &&
                    (q->kind == MA2_KIND_MECH || q->kind == MA2_KIND_TANK) && hdist2(q->pos, me->pos) < 120.f) { box_lim = 3.0; break; }
            }
        int box_wedge = boxed && w->time - c->box_since > box_lim && c7_on("boxwedge", "RECOMP_BOT_BOX_WEDGE", "BoxWedge", 1);
        if (!box_wedge && (hdist2(me->pos, c->eng_pos) > 4.f || (c->shots != c->eng_shots && standing))) { c->eng_pos = me->pos; c->eng_wt = w->time; c->eng_thp = t->hp; c->eng_shots = c->shots; }
        else if (box_wedge || w->time - c->eng_wt > 6.0) {
            b->status.engage_wedged = 1; c->eng_wt = w->time;
            if (box_wedge) { c->box_since = w->time; c7_add("boxwedge", "wedged", 1); }
            c7_add("wedgelive", "wedged", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CTRL] bot=%d fight wedged on %08X\n", index, t->actor);
        }
        return;
    }
    if (!ctrl_underfoot(c, w, me)) {
        /* Close enough by the band but blind (a building the brain's own look missed): after 2 s
         * close in, after 3 s have the brain find a firing spot -- a tank sat 25 s behind a wall
         * 30 m from where it would have seen its target. */
        float st = maxf2(o->stop, 8.f);
        if (!los && !recent) {
            if (c->blind_t <= 0.0 || w->time < c->blind_t) c->blind_t = w->time;
            if (w->time - c->blind_t > 2.0) st = maxf2(st * 0.6f, 15.f);   /* a step in, keeping most of the stand-off */
            if (w->time - c->blind_t > 3.0) { b->status.engage_wedged = 1; c->blind_t = w->time - 2.0; }
        } else c->blind_t = 0.0;
        ctrl_travel(index, b, w, me, o->point, st, 0, 0.f);
    } else body_stop(me->actor);
    if (ctrl_fire(c, w, me, t->actor)) ctrl_engaged(c, b, w);
}

/* BaHackHop (default on, RECOMP_BOT_BA_HACK_HOP / [Bots] BaHackHop): Worker A (11-01) found bot BattleArmor "rarely actually clamp
 * on". Cause, measured: a BattleArmor's walk is 0 m/s -- it moves only by jet hops -- and F19 held every hop off inside 40 m of
 * the 'Mech (the travel hop there flew 52-60 m and landed far past it), so a hacking BattleArmor stood still inside 40 m and could
 * only clamp a 'Mech that walked up to it; since BaHackFlee the 'Mechs walk away at 18 m. Now inside 60 m it hops AT the 'Mech
 * as a player does: aimed where the 'Mech will be (its velocity x the flight time, a share by skill), steered while the jet burns
 * (the jump's own direction ai+0x244, at a turn rate by skill), the burn cut once the coast down (~24 m/s on, ~44 m/s2 down) would
 * carry it onto the 'Mech, and the claw tried every 0.15 s within 11 m (in the air within 8 m of its height). The next hop waits
 * for a reaction by skill and the burn its distance needs (0.4 s + 1 s per 38 m). Against the player the lead and the turn are
 * 0.6x, so moving and turning still beat it; a 'Mech running 17-22 m/s outran the hops (Deliverance). The 'Mech's flight
 * (BaHackFlee) is unchanged. Measured, 5v6 CTF on Deliverance / Bogged Down (3 matches off, 4 on): won hack rolls 9 -> 12,
 * clamps on 'Mechs 1 -> 7, NeuroHacks done 1 -> 6; every claw tried within 11 m took (7 of 7). [C7] bahackhop: hops, hops_vs_player, steer_s, cuts, tries_air, tries_ground,
 * refused_air, refused_ground, clamps, clamps_vs_player, miss_m_sum / miss_n (closest pass per hop). [BOT-HACKHOP] per hop. */
static int ba_hack_hop_on(void) { return c7_on("bahackhop", "RECOMP_BOT_BA_HACK_HOP", "BaHackHop", 1); }
static const float k_hh_lead[4] = { 0.35f, 0.6f, 0.85f, 1.0f }, k_hh_turn[4] = { 1.2f, 2.0f, 3.0f, 4.0f }, k_hh_react[4] = { 0.9f, 0.6f, 0.4f, 0.25f };
static Ma2Vec3 hh_intercept(const Ma2PlayerView *me, const Ma2PlayerView *t, float lead)
{
    Ma2Vec3 p = t->pos; float tau = hdist2(t->pos, me->pos) / 24.f + 0.25f;
    for (int i = 0; i < 3; i++) {
        if (tau > 2.5f) tau = 2.5f;
        p.x = t->pos.x + t->vel.x * lead * tau; p.z = t->pos.z + t->vel.z * lead * tau;
        tau = hdist2(p, me->pos) / 24.f + 0.25f;
    }
    return p;
}
static void ba_hack_hop(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2PlayerView *t, float d)
{
    uint32_t ai = actor_ai(me->actor);
    if (!ai) return;
    int sk = c->skill & 3, hum = t->is_human, jumping = MEM8(ai + 0x240) != 0;
    float lead = k_hh_lead[sk] * (hum ? 0.6f : 1.f), fuel = MEMF(me->actor + 0x49C);
    Ma2Vec3 p = hh_intercept(me, t, lead);
    if (c->hh_on && c->hh_tgt == t->actor) {
        if (d < c->hh_dmin) { c->hh_dmin = d; c->hh_dy = me->pos.y - t->pos.y; }
        if (jumping) {
            /* the burn steered: the jump's direction (unit, its y kept) turned toward the intercept at the skill's rate */
            float dx = MEMF(ai + 0x244), dz = MEMF(ai + 0x24C), hl = hypotf(dx, dz), wx = p.x - me->pos.x, wz = p.z - me->pos.z;
            if (hl > 1e-3f && hypotf(wx, wz) > 1.f) {
                float a0 = atan2f(dx, dz), da = wrap_pi(atan2f(wx, wz) - a0), mx = (sk == 3 && ma2_bots_dragoon() ? 6.f : k_hh_turn[sk]) * (hum ? 0.6f : 1.f) * w->dt;
                da = da > mx ? mx : da < -mx ? -mx : da;
                MEMF(ai + 0x244) = sinf(a0 + da) * hl; MEMF(ai + 0x24C) = cosf(a0 + da) * hl;
                c7_add("bahackhop", "steer_s", w->dt);
            }
            /* the burn cut once the coast down would carry it onto the 'Mech */
            int oks = 0; float sf = surface_height(me->pos.x, me->pos.z, &oks), h = oks ? maxf2(me->pos.y - sf, 0.f) : 0.f, vy = maxf2(me->vel.y, 0.f);
            float tc = vy / 44.f + sqrtf(2.f * (h + vy * vy / 88.f) / 44.f);
            Ma2Vec3 pl = t->pos; pl.x += t->vel.x * lead * tc; pl.z += t->vel.z * lead * tc;
            float hv = hypotf(me->vel.x, me->vel.z);                      /* its own speed now (24 m/s assumed cut 10-13 m short: the burn is still speeding it up) */
            if (w->time - c->hh_t0 > 0.3 && hdist2(pl, me->pos) < 0.9f * hv * tc + 2.f && MEMF(ai + 0x250) > (float)w->time + 0.05f) { MEMF(ai + 0x250) = (float)w->time; c7_add("bahackhop", "cuts", 1); }
            body_steer(me, p); c->dest = p;
            return;
        }
        if (w->time - c->hh_t0 > 0.3) {                                      /* down: this hop is over */
            c->hh_on = 0; c->hh_next = w->time + (sk == 3 && ma2_bots_dragoon() ? 0.1f : k_hh_react[sk]);
            c7_add("bahackhop", "miss_m_sum", c->hh_dmin); c7_add("bahackhop", "miss_n", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-HACKHOP] t=%.1f bot=%d down after %.1f s: from %.1f m, closest %.1f m (dy %+.1f), now %.1f m%s\n",
                                                   w->time, index, w->time - c->hh_t0, c->hh_d0, c->hh_dmin, c->hh_dy, d, hum ? " (player)" : "");
        }
    } else if (c->hh_on) c->hh_on = 0;
    /* The burn the way calls for (a 0.85 s burn covered ~11 m, 1.5 s ~65 m: the climb makes the coast): waited for on the ground --
     * short hops after a 'Mech running at 17-22 m/s each lost ground (Deliverance 11-01: 42 -> 39 -> 33 -> 30 -> 25 -> 19 m in 10 s). */
    float dreq = minf2(maxf2(0.4f + hdist2(p, me->pos) / 38.f, 0.6f), 1.85f);
    if (!jumping && !c->hh_on && w->time >= c->hh_next && w->time >= c->dip_until && d > 6.f && d < 60.f && fabsf(t->pos.y - me->pos.y) < 15.f &&
        fuel - 0.15f < dreq && fuel < 1.95f) c7_add("bahackhop", "fuel_wait_s", w->dt);
    else if (!jumping && !c->hh_on && w->time >= c->hh_next && w->time >= c->dip_until && d > 6.f && d < 60.f && fuel >= 0.9f && fabsf(t->pos.y - me->pos.y) < 15.f &&
             !(c->hacked_actor == t->actor && w->time - c->hacked_t < 30.0)) {   /* not at the 'Mech it has just hacked (empty now) */
        Ma2Vec3 dir = { p.x - me->pos.x, 0.f, p.z - me->pos.z }; float l = hypotf(dir.x, dir.z);
        if (l > 0.5f) {
            dir.x /= l; dir.z /= l;
            if (ai_startjump(ai, dir, minf2(fuel - 0.15f, 2.f), 1.f) == 1) {
                c->hh_on = 1; c->hh_t0 = w->time; c->hh_tgt = t->actor; c->hh_d0 = d; c->hh_dmin = d; c->hh_dy = me->pos.y - t->pos.y; c->jumps++;
                c7_add("bahackhop", "hops", 1); if (hum) c7_add("bahackhop", "hops_vs_player", 1);
                body_steer(me, p); c->dest = p;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-HACKHOP] t=%.1f bot=%d sk=%d hops at %08X%s %.1f m (dy %+.1f, its speed %.1f, fuel %.2f) toward %.0f,%.0f\n",
                                                       w->time, index, sk, t->actor, hum ? " (player)" : "", d, t->pos.y - me->pos.y, hypotf(t->vel.x, t->vel.z), fuel, p.x, p.z);
            }
        }
    }
}

/* BaLand (default on, RECOMP_BOT_BA_LAND / [Bots] BaLand): the root of "BattleArmor runs in place" (playtest 11-03 laptop log).
 * The body's component flag comp+0x145 bit 0 (comp+0x144 bit 8) is the walker's hold on it: set, the walk moves the body;
 * clear, the walk anim plays and the body stays put. A jet clears it, and for a bot BattleArmor -- jetted through the
 * MechAI's StartJump -- nothing ever set it again: after its first jet a bot walked 0 m/s, its run anim on the spot, and
 * got about only by more jets, overshooting and undershooting whatever it walked to (a flag it was set down beside).
 * Measured 11-03 on Bogged Down CTF (RECOMP_BOT_BA_ANIM_DIAG): with every jet held off bot BattleArmor walk 11-15 m/s
 * (a player 15) with the bit set; after a landing the bit stays clear and the body stands 6 s and more until the next
 * jet; nothing else in the body or component differs. With the bit given back once down, walking went from 15-42% to
 * 74% of ground time (14 m/s). (The game's own ends of a jump state for other bodies -- sub_119521, sub_17D401 -- give
 * the bit back when the land / idle anim plays; MoveTo re-picks the walk anim every step. The setter for a player's
 * BattleArmor is not located.) Now a bot BattleArmor down on the ground 0.25 s with its burn over (ai+0x240 clear) gets
 * the bit back. Players are never touched. [C7] baland: releases. [BOT-BALAND] with RECOMP_BOT_DIAG. */
static int ba_land_on(void) { static int on = -1; if (on < 0) on = c7_on("baland", "RECOMP_BOT_BA_LAND", "BaLand", 1); return on; }
static void ba_land_release(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    uint32_t a = me->actor, comp = gva_ok(a, 0x100) ? MEM32(a + 0x8C) : 0;
    if (me->kind != MA2_KIND_BATTLEARMOR || me->riding || me->clamped_to || me->qte_active || !ba_land_on() || !gva_ok(comp, 0x150) ||
        (MEM8(comp + 0x145) & 1u)) { c->bal_t = 0.0; return; }
    uint32_t ai = actor_ai(a);
    int gk = 0, sk = 0; float g = terrain_height(me->pos.x, me->pos.z, &gk), s = surface_height(me->pos.x, me->pos.z, &sk);
    float floor_y = maxf2(gk ? g : -1e9f, sk ? s : -1e9f);
    /* RoofLand (default on, RECOMP_BOT_BA_ROOF_LAND / [Bots] BaRoofLand): down = on the terrain / a known surface, OR standing still in
     * height (|vy| < 0.5 and within 0.1 m of where the clock started) -- a rooftop is neither terrain nor a known surface, so a BattleArmor
     * knocked off its VTOL onto a 42 m roof stood with the bit clear until AiKick hopped it 1.7 s later (playtest rec 2026-11-04 bot 7,
     * t=38.9-40.6: vy 0 and burn off from 39.64, no release). */
    int roof = c7_on("baroofland", "RECOMP_BOT_BA_ROOF_LAND", "BaRoofLand", 1);
    /* BaLandDrop (default on, RECOMP_BOT_BA_LAND_DROP / [Bots] BaLandDrop): a BattleArmor set down by a VTOL keeps ai+0x240 = 1 with its
     * walk anim playing (ai+0x360 = 1, not the jet anim 21) on the ground -- the bit was never given back and it walked on the spot beside
     * the flag: d5_60 b8 35.3-39.3 7.5 m from the T1 flag (the exact walk's stall then handed it to MoveTo, which orbited at 7-13 m), b3
     * the same 30.9-35.4. Down with no jet anim counts too. [C7] baland: drop_frames. */
    int burn = ai && MEM8(ai + 0x240) && !(MEM32(ai + 0x360) != 21u && c7_on("balanddrop", "RECOMP_BOT_BA_LAND_DROP", "BaLandDrop", 1));
    int still = ai && !burn && fabsf(me->vel.y) < 0.5f;
    if (still && MEM8(ai + 0x240)) c7_add("baland", "drop_frames", 1);
    int down = still && floor_y > -1e8f && me->pos.y - floor_y < 1.5f;
    if (!down && !(roof && still)) { c->bal_t = 0.0; return; }
    if (c->bal_t <= 0.0 || w->time < c->bal_t) { c->bal_t = w->time; c->bal_y = me->pos.y; return; }
    if (!down && fabsf(me->pos.y - c->bal_y) > 0.1f) { c->bal_t = w->time; c->bal_y = me->pos.y; return; }   /* still moving up or down */
    if (w->time - c->bal_t < 0.25) return;
    if (!down) c7_add("baroofland", "roof_releases", 1);
    MEM8(comp + 0x145) |= 1u;
    c->bal_t = 0.0; c->bal_n++; c7_add("baland", "releases", 1);
    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-BALAND] t=%.1f bot=%d walker given back on the ground at %.0f,%.0f,%.0f (release %d)\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, c->bal_n);
}

/* BaDipHop (default on, RECOMP_BOT_BA_DIP_HOP / [Bots] BaDipHop): Worker A (11-01) saw a bot BattleArmor stuck in a terrain dip.
 * Found in the 11-01 logs (Bogged Down): a BattleArmor walks at 0 m/s, so whatever holds it in place holds it for good -- (a) in a
 * hollow under high ground 20 m up, standing (a stop each step: a jump started meanwhile never flies, the jet carries a body only
 * under MoveTo), 11 s at 969,5,1247; (b) a planned drop leg begun mid-hop: its start height was taken in the air, so landing 8 m
 * lower read as "came down" and neither the give-up nor DropLedge's jet ran -- 49 s at 886,17,1185 "stepping off" until its goal
 * changed. Now a BattleArmor with somewhere to go that stays within 4 m for 3.5 s hops out: toward the lowest rim of the ground
 * round it (12 bearings, 4-20 m out) weighed against the way it is going (the route's next point), the burn aimed over that rim,
 * steered there under MoveTo until it lands; first it waits for fuel (1.2). A drop leg's start is taken on the ground (1-2
 * re-seats a match; a BattleArmor-heavy Bogged Down match: 2 stuck spells, 2 hops out, both freed). [C7]
 * badiphop: stuck_n, hops, fuel_wait_s, freed / still (8 m+ from the spot 5 s after the hop or not), drop_reseat. [BOT-DIPHOP]. */
static int ba_dip_hop_on(void) { return c7_on("badiphop", "RECOMP_BOT_BA_DIP_HOP", "BaDipHop", 1); }
static void ba_dip_hop(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o)
{
    if (c->hh_on && o->type != MA2_ORD_HACK) c->hh_on = 0;                  /* BaHackHop: the hack is over (a hop left open blocked this) */
    if (me->kind != MA2_KIND_BATTLEARMOR || me->riding || me->clamped_to || me->qte_active || c->bav_until > 0.0 || !ba_dip_hop_on()) { c->dip_t = 0.0; c->dip_until = 0.0; return; }
    uint32_t ai = actor_ai(me->actor);
    if (!ai) return;
    int jumping = MEM8(ai + 0x240) != 0;
    if (c->dip_until > 0.0) {                                                /* the hop out: MoveTo carries the jet, steered at its point */
        if (w->time < c->dip_until && (jumping || w->time - c->dip_t0 < 0.5)) { body_steer(me, c->dip_pt); c->dest = c->dip_pt; return; }
        c->dip_until = 0.0;
    }
    if (c->dip_chk_t > 0.0 && w->time >= c->dip_chk_t) { c7_add("badiphop", hdist2(me->pos, c->dip_from) > 8.f ? "freed" : "still", 1); c->dip_chk_t = 0.0; }
    Ma2Vec3 want = me->pos; float stop = maxf2(o->stop, 6.f);
    const Ma2PlayerView *t = NULL;
    if (o->type == MA2_ORD_TRAVEL || o->type == MA2_ORD_FIGHT_MOVE) want = o->point;
    else if ((o->type == MA2_ORD_HACK || o->type == MA2_ORD_ENGAGE) && ctrl_target_alive(w, o->target, &t)) { want = t->pos; if (o->type == MA2_ORD_ENGAGE) stop = maxf2(stop, o->band_max > 1.f ? o->band_max : 60.f); }
    int hold_end = c->have_path && c->path.n > 0 && c->path.end_gap > 12.f && hdist2(me->pos, c->path.pt[c->path.n - 1]) < kind_stop_margin(me->kind) + 3.f;   /* a partial route's end: held on purpose */
    if (hdist2(me->pos, want) < stop + 4.f || jumping || hold_end || w->time < c->upjet_until || c->co_phase || c->hh_on) { c->dip_t = 0.0; c->dip_spell = 0; return; }
    if (c->dip_t <= 0.0 || w->time < c->dip_t || hdist2(me->pos, c->dip_pos) > 4.f || fabsf(me->pos.y - c->dip_pos.y) > 3.f) { c->dip_pos = me->pos; c->dip_t = w->time; c->dip_spell = 0; return; }
    if (w->time - c->dip_t < 3.5) return;
    if (!c->dip_spell) { c->dip_spell = 1; c7_add("badiphop", "stuck_n", 1); }
    float fuel = MEMF(me->actor + 0x49C);
    if (fuel < 1.2f) { c7_add("badiphop", "fuel_wait_s", w->dt); return; }
    Ma2Vec3 nx = c->have_path && c->path.next < c->path.n ? c->path.pt[c->path.next] : want;
    float hx = nx.x - me->pos.x, hz = nx.z - me->pos.z, hl = hypotf(hx, hz) + 1e-3f, best = 1e9f, brise = 0.f, br = 12.f, ba = 0.f;
    for (int k = 0; k < 12; k++) {
        float a = (float)k * 0.5236f, sx = sinf(a), sz = cosf(a), rise = -1e9f, rr = 12.f;
        for (float r = 4.f; r <= 20.f; r += 4.f) {
            int ok = 0; float s = surface_height(me->pos.x + sx * r, me->pos.z + sz * r, &ok);
            if (ok && s - me->pos.y > rise) { rise = s - me->pos.y; rr = r; }
        }
        if (rise < -1e8f) continue;
        float score = maxf2(rise, 0.f) + 6.f * (1.f - (sx * hx + sz * hz) / hl);   /* a rim 6 m lower is worth a quarter turn */
        if (score < best) { best = score; brise = maxf2(rise, 0.f); br = rr; ba = a; }
    }
    if (best > 1e8f) return;
    Ma2Vec3 pt = { me->pos.x + sinf(ba) * (br + 4.f), me->pos.y + brise + 3.f, me->pos.z + cosf(ba) * (br + 4.f) };
    if (body_jump_up(me->actor, me->pos, pt, 1.2f)) {
        c->dip_until = w->time + 3.5; c->dip_t0 = w->time; c->dip_pt = pt; c->dip_from = me->pos; c->dip_chk_t = w->time + 5.0; c->dip_n++;
        c->dip_t = w->time; c->jumps++; c7_add("badiphop", "hops", 1);
        body_steer(me, pt); c->dest = pt;
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DIPHOP] t=%.1f bot=%d ord=%d no headway 3.5 s at %.0f,%.0f,%.0f: hops out over a %.1f m rim %.0f m off toward %.0f,%.0f (fuel %.2f, %d)\n",
                                               w->time, index, o->type, me->pos.x, me->pos.y, me->pos.z, brise, br, pt.x, pt.z, fuel, c->dip_n);
    }
}

/* VtolEdgeAll recovery: a craft that has drifted into the band anyway -- carried on past a kept-in point, or holding a firing hover
 * where it stopped (Deliverance 11-01: 18 m off the middle hill's wall, engage phases 0-1) -- is flown back out to 32 m (under 24 m), whatever
 * it was doing this step (not on a pad run, setting down or doing tether work). */
static void vt_edge_recover(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o)
{
    if (!vt_edge_all_on() || !ef_ready() || o->vt_heal || o->type == MA2_ORD_EJECT || (o->type == MA2_ORD_TRAVEL && o->cargo && o->cargo <= 2)) return;
    int okg = 0; float g = terrain_height(me->pos.x, me->pos.z, &okg);
    if (!okg || me->pos.y - g < 12.f) return;                               /* low (taking off from a pad by the edge): its own business -- MoveTo held one hovering 6 m up for 8 s */
    {   /* coming in fast: a craft let go into a firing hover at 58 m/s coasted 20 m over the line (wcar2) -- the stick, reversed against
         * its drift for the moment it would take (a reversed stick took 94 -> 42 m/s in 0.5 s), whatever the order was flying */
        float e = ef_dist(me->pos.x, me->pos.z), ep = ef_dist(me->pos.x + me->vel.x * 0.8f, me->pos.z + me->vel.z * 0.8f);
        uint32_t comp = MEM32(me->actor + 0x8C), node = MEM32(me->actor + 0x94);
        if (ep < 24.f && ep < e - 2.f && hypotf(me->vel.x, me->vel.z) > 12.f && gva_ok(comp, 0x150) && gva_ok(node, 0x100)) {
            float fx = MEMF(node + 0xE0), fz = MEMF(node + 0xE8), fl = hypotf(fx, fz);
            if (fl > 1e-3f) {
                fx /= fl; fz /= fl;
                float vf = me->vel.x * fx + me->vel.z * fz, vl = me->vel.x * fz - me->vel.z * fx;
                uint32_t ai = actor_ai(me->actor);
                vtol_stick_take(me->actor, comp); if (vt_mod_climb_on()) MEM32(comp + 0x144) |= 0x8000u; if (ai) MEM8(ai + 0x90) = 0;   /* the stick, not the AI, this step (as vtol_manual) */
                c->axis[4] = clampf2(-vf * 0.1f, -1.f, 1.f); if (c->axis[4] == 0.f) c->axis[4] = 1e-6f;
                c->axis[8] = clampf2(-vl * 0.1f, -1.f, 1.f); if (c->axis[8] == 0.f) c->axis[8] = 1e-6f;
                c->axis[9] = 1e-6f;
                c7_add("vtedgeall", "brake_s", w->dt); c->vs_flags |= 16;
                return;
            }
        }
    }
    if (ef_dist(me->pos.x, me->pos.z) >= 24.f) return;
    {   /* EdgeStickOut: the stick already flies it out of the band this step (forward, nose inward) -- leave it to the stick. A MoveTo here
         * fought the stick every step (AI and stick frames in turn: neither kept a velocity): a red ferry over NRC's base, which lies in the
         * band, hung 15-30 s at 0-2 m/s with the stick full forward (v26 vtf1 t=16-34, vtf2 t=13-43), then flew 62-68 m/s once out. */
        uint32_t node = MEM32(me->actor + 0x94);
        if (c->axis[4] > 0.3f && gva_ok(node, 0x100) && c7_on("edgestickout", "RECOMP_BOT_VTOL_EDGE_STICK_OUT", "EdgeStickOut", 1)) {
            float fx = MEMF(node + 0xE0), fz = MEMF(node + 0xE8), fl = hypotf(fx, fz);
            if (fl > 1e-3f && ef_dist(me->pos.x + fx / fl * 24.f, me->pos.z + fz / fl * 24.f) > ef_dist(me->pos.x, me->pos.z) + 2.f) {
                c7_add("edgestickout", "stick_out_s", w->dt);
                return;
            }
        }
    }
    /* EdgeSlide (default on, RECOMP_BOT_VTOL_EDGE_SLIDE / [Bots] EdgeSlide; playtest 11-12 slow to reach the objective
     * with a bot rider): NRC's red ferry, rider aboard, 45-75 m up by its own base, hung 40-60 s at 0-2 m/s 24.0 m off an
     * out-of-bounds area that lies on the straight line to their flag (v29b1 rc30 t=13-64, v29a2 t=16-58, v29a3 [BOT-EDGEALONG] "edge 24.0
     * m, toward the point 3.8 m"): its carrot led into the band, this recovery's MoveTo pushed it back out to 32 m every other step, and
     * the parked window's MoveTo (W) climbed it to 75 m. Now a craft on a travel leg 60 m+ long is not pushed straight out: the stick
     * flies it ALONG the edge (the tangent of the distance field, on the side of the point, 40 m on) and out to 32 m, so it slides round
     * the area toward the point. The fast inward brake above still acts first. [C7] edgeslide: slide_s. */
    if (o->type == MA2_ORD_TRAVEL && hdist2(me->pos, o->point) > 60.f && c7_on("edgeslide", "RECOMP_BOT_VTOL_EDGE_SLIDE", "EdgeSlide", 1)) {
        float eh = ef_dist(me->pos.x, me->pos.z), sg = maxf2(g_ef.mx, 2.f);
        float nx = ef_dist(me->pos.x + sg, me->pos.z) - ef_dist(me->pos.x - sg, me->pos.z), nz = ef_dist(me->pos.x, me->pos.z + sg) - ef_dist(me->pos.x, me->pos.z - sg), nl = hypotf(nx, nz);
        if (eh > 2.f && nl > 1e-3f && nx == nx && nz == nz) {
            nx /= nl; nz /= nl;
            float tx = nz, tz = -nx, gx = o->point.x - me->pos.x, gz = o->point.z - me->pos.z, gl = hypotf(gx, gz), dt = tx * gx + tz * gz;
            /* the side: kept 4 s once chosen (v29d5: flipping each second with the point straight behind the area, it closed to 11 m);
             * else toward the point, or -- the point square behind the area -- the side with more room 40 m on */
            static signed char side[16]; static double side_t[16];
            int si = index >= 0 && index < 16 ? index : 0, sd;
            if (side[si] && w->time - side_t[si] < 4.0 && w->time >= side_t[si]) sd = side[si];
            else if (fabsf(dt) > 0.3f * gl) sd = dt > 0.f ? 1 : -1;
            else sd = ef_dist(me->pos.x + tx * 40.f, me->pos.z + tz * 40.f) >= ef_dist(me->pos.x - tx * 40.f, me->pos.z - tz * 40.f) ? 1 : -1;
            if (side[si] != sd || w->time - side_t[si] >= 4.0 || w->time < side_t[si]) side_t[si] = w->time;
            side[si] = (signed char)sd; tx *= (float)sd; tz *= (float)sd;
            float along = eh < 16.f ? 20.f : 35.f, out = maxf2(36.f - eh, 0.f);   /* deep in the band: mostly out */
            Ma2Vec3 sp = { me->pos.x + tx * along + nx * out, me->pos.y, me->pos.z + tz * along + nz * out };
            if (env_on("RECOMP_BOT_DIAG") && w->time - c->bdiag_t > 1.0) { c->bdiag_t = w->time; fprintf(stderr, "[BOT-EDGESLIDE] t=%.1f bot=%d edge %.1f m: slides toward %.0f,%.0f (point %.0f,%.0f)\n", w->time, index, eh, sp.x, sp.z, o->point.x, o->point.z); }
            vtol_manual(index, c, w, me, sp); c->va_branch = 'S'; c->dest = sp;
            c7_add("edgeslide", "slide_s", w->dt); c->vs_flags |= 8;
            return;
        }
    }
    Ma2Vec3 q = me->pos;
    if (vt_keep_in(&q, 32.f) <= 0) return;
    q.y = me->pos.y;
    body_moveto(me->actor, q); c->dest = q;
    c7_add("vtedgeall", "recover_s", w->dt); c->vs_flags |= 8;
}
/* VtolEdgeAll measure (switch on or off): how near a flying bot VTOL comes to out of bounds, and how high it is there. */
static void vt_edge_measure(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o)
{
    if (!ef_ready()) return;
    int okg = 0; float g = terrain_height(me->pos.x, me->pos.z, &okg), agl = okg ? me->pos.y - g : 0.f;
    if (agl < 4.f) return;                                                    /* on the pad / set down */
    float e = ef_dist(me->pos.x, me->pos.z), dt = w->dt > 0.f && w->dt < 0.5f ? w->dt : 0.f;
    c7_add("vtedgeall", "air_s", dt);
    if (e < 25.f) {
        static const char *const k_eng[5] = { "in25_eng0", "in25_eng1", "in25_eng2", "in25_eng3", "in25_eng4" };
        c7_add("vtedgeall", "in25_s", dt); c7_max("vtedgeall", "worst_in_m", 25.f - e);
        if (agl >= 12.f) c7_add("vtedgeall", "in25_hi_s", dt);                /* up in the air, not by a pad */
        c7_add("vtedgeall", o->vt_heal ? "in25_pad" : o->type == MA2_ORD_ENGAGE ? k_eng[c->vt_phase >= 0 && c->vt_phase < 5 ? c->vt_phase : 0] : o->type == MA2_ORD_TRAVEL ? (o->cargo ? "in25_cargo" : "in25_travel") :
                                o->type == MA2_ORD_FIGHT_MOVE ? "in25_fight" : o->type == MA2_ORD_EJECT ? "in25_eject" : "in25_other", dt);   /* which steering it was under */
    }
    if (e < 15.f) c7_add("vtedgeall", "in15_s", dt);
    if (e < 40.f && agl > 30.f) c7_add("vtedgeall", "hi_near_s", dt);
    if (env_on("RECOMP_BOT_VTOL_TRACE") && (w->time - c->ve2_t >= 0.5 || w->time < c->ve2_t)) {
        c->ve2_t = w->time;
        fprintf(stderr, "[BOT-VEDGE2] t=%.1f bot=%d pos=%.0f,%.0f,%.0f agl=%.0f edge=%.1f type=%d ord=%d cargo=%d rider=%d dest=%.0f,%.0f phase=%d heal=%d sp=%.0f dest_edge=%.0f\n", w->time, index,
                me->pos.x, me->pos.y, me->pos.z, agl, e, ti_type_at(me->pos.x, me->pos.z), o->type, o->cargo, c->vt_rider, c->dest.x, c->dest.z, c->vt_phase, o->vt_heal,
                hypotf(me->vel.x, me->vel.z), ef_dist(c->dest.x, c->dest.z));
    }
}

/* HACK (BattleArmor): jet-hop onto the target 'Mech, fire on the way, clamp inside 11 m (claw path
 * sub_10239A "can clamp" then sub_1019EB attach); the minigame answers come from the brain. */
/* BaReclaw (default on, RECOMP_BOT_BA_RECLAW / [Bots] BaReclaw): the 30 s no-repeat after a clamp is for hacking a 'Mech;
 * on a friendly VTOL a BattleArmor knocked off claws straight back on (it waited 30 s under the VTOL: desktop 10-31). */
static int ba_reclaw_on(void) { return c7_on("bareclaw", "RECOMP_BOT_BA_RECLAW", "BaReclaw", 1); }
static void ctrl_do_hack(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o, uint32_t ai)
{
    BotCtrl *c = &b->ctrl;
    const Ma2PlayerView *t = NULL;
    ctrl_release_attack(c, ai);
    if (me->qte_active || me->clamped_to) { body_stop(me->actor); c->mode = CM_ATTACK; return; }   /* riding it: the hack runs */
    if (!ctrl_target_alive(w, o->target, &t)) { body_stop(me->actor); return; }
    c->tgt = t->actor;
    float d = hdist2(t->pos, me->pos);
    /* F19: walk the last 40 m to a 'Mech. The travel hop (a burn sized to the way left) flew it
     * over the 'Mech, 10-12 m up, to land 25-32 m beyond: no clamp tries at all. */
    int f19 = t->kind == MA2_KIND_MECH && c7_on("f19", "RECOMP_BOT_F19", "BaHackCommit", 1);
    int hh = f19 && ba_hack_hop_on();                                       /* BaHackHop: the hops inside 60 m are its own */
    if (f19 && d < (hh ? 60.f : 40.f) && c->jump_next < w->time + 0.5) c->jump_next = w->time + 0.5;
    ctrl_travel(index, b, w, me, t->pos, 2.f, 0, 0.f);
    ctrl_fire(c, w, me, t->actor);
    if (hh) ba_hack_hop(index, c, w, me, t, d);
    uint32_t ai2 = actor_ai(me->actor);
    /* The claw is tried on the ground, or (F19) in the air when level with the 'Mech (< 6 m apart
     * in height; BaHackHop: 8 m): sub_10239A itself decides whether it can clamp from there. */
    int grounded = ai2 && !MEM8(ai2 + 0x240), level = f19 && fabsf(t->pos.y - me->pos.y) < (hh ? 8.f : 6.f);
    if ((t->kind == MA2_KIND_MECH || (t->kind == MA2_KIND_VTOL && t->nation == me->nation)) && d < 11.f && w->time >= c->clamp_next && ai2 &&
        (t->kind == MA2_KIND_VTOL || grounded || level) &&
        !(c->hacked_actor == t->actor && w->time - c->hacked_t < (t->kind == MA2_KIND_VTOL && ba_reclaw_on() ? 2.0 : 30.0)) && env_int("RECOMP_BOT_BA_CLAMP", 1)) {
        c->clamp_next = w->time + (hh ? 0.15 : 1.0); c->clamp_tries++;
        if (f19) c7_add("f19", grounded ? "clamp_tries_ground" : "clamp_tries_air", 1);
        if (hh) c7_add("bahackhop", grounded ? "tries_ground" : "tries_air", 1);
        if (ba_clamp(me->actor) == 1) {
            c->clamps++; c->clamp_next = w->time + (t->kind == MA2_KIND_VTOL && ba_reclaw_on() ? 2.0 : 20.0); c->hacked_actor = t->actor; c->hacked_t = w->time;
            if (f19) c7_add("f19", "clamps", 1);
            if (hh) { c7_add("bahackhop", "clamps", 1); if (t->is_human) c7_add("bahackhop", "clamps_vs_player", 1); }
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CLAMP] bot=%d onto %08X d=%.1f dy=%+.1f%s%s\n", index, t->actor, d, me->pos.y - t->pos.y, grounded ? "" : " air", hh && c->hh_on ? " (hack hop)" : "");
        } else {
            if (f19) c7_add("f19", "clamp_refused", 1);
            if (hh) {
                c7_add("bahackhop", grounded ? "refused_ground" : "refused_air", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-HACKHOP] t=%.2f bot=%d claw refused d=%.1f dy=%+.1f %s\n", w->time, index, d, me->pos.y - t->pos.y, grounded ? "ground" : "air");
            }
        }
    } else if (f19 && d < 11.f) c7_add("f19", !ai2 || (!grounded && !level) ? "near_refuse_air" : w->time < c->clamp_next ? "near_refuse_cooldown" : "near_refuse_other", 1);
    ctrl_engaged(c, b, w);
}

static float trace_sfc(const Ma2PlayerView *me) { int ok = 0; float s = surface_height(me->pos.x, me->pos.z, &ok); return ok ? s - me->pos.y : -99.f; }
static float trace_up(const Ma2PlayerView *me) { int ok = 0; float g = terrain_height(me->pos.x, me->pos.z, &ok); return ok ? me->pos.y - g : -99.f; }
static void ctrl_trace(int index, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o)
{
    static int lim = -2;
    if (lim == -2) lim = env_int("RECOMP_BOT_TRACE", 0);
    if (lim <= 0 || w->time > (double)lim) return;
    float dt = (float)(w->time - c->tr_t), v = dt > 0.f && dt < 0.5f ? hdist2(me->pos, c->tr_pos) / dt : -1.f;
    uint32_t ai = actor_ai(me->actor), comp = MEM32(me->actor + 0x8C);
    fprintf(stderr, "[TRACE] t=%.2f b=%d k=%d pos=%.1f,%.1f v=%.1f drv=%c%d ord=%d dest=%.0f,%.0f dd=%.1f path=%d/%d yield=%d back=%d mode=%u cBC=%.1f jet=%.2f j=%u run=%u g958=%.0f cC0=%.2f bc=%.2f c144=%08X anim=%d/%d f26c=%08X up=%.1f roof=%d sfc=%.1f jf=%u jend=%.2f th4=%.2f th8=%.2f ai90=%u aiB4=%08X ai12C=%u\n",
            w->time, index, me->kind, me->pos.x, me->pos.z, v, g_drv_kind ? g_drv_kind : '-', g_drv_line, o->type, c->dest.x, c->dest.z,
            hdist2(c->dest, me->pos), c->path.next, c->have_path ? c->path.n : 0, w->time < c->yield_until, w->time < c->back_until,
            ai ? MEM32(ai + 0x12C) : 99u, gva_ok(comp, 0x150) ? MEMF(comp + 0xBC) : -1.f, MEMF(me->actor + 0x49C), ai ? (unsigned)MEM8(ai + 0x240) : 9u, MEM32(me->actor + 0x268) & 1u, MEMF(me->actor + 0x958), gva_ok(comp, 0x150) ? MEMF(comp + 0xC0) : -1.f, gva_ok(comp, 0x150) ? MEMF(comp + 0xBC) : -1.f, gva_ok(comp, 0x150) ? MEM32(comp + 0x144) : 0u, me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_PILOT ? (int)MEM32(me->actor + 0x86C) : -1, me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_PILOT ? (int)MEM32(me->actor + 0x868) : -1, MEM32(me->actor + 0x26C), trace_up(me), on_raised(me, NULL), trace_sfc(me), (MEM32(me->actor + 0x268) >> 15) & 1u, ai ? (float)((double)MEMF(ai + 0x250) - w->time) : -1.f, MEMF(me->actor + 0x4E4), MEMF(me->actor + 0x4E8), ai ? (unsigned)MEM8(ai + 0x90) : 9u, ai ? MEM32(ai + 0xB4) : 0u, ai ? MEM32(ai + 0x12C) : 99u);
    c->tr_pos = me->pos; c->tr_t = w->time;
}

/* RECOMP_BOT_CLIMB_TEST=<bot>: BattleArmor wall climb, the game's own way (tutorial: "To climb,
 * jumpjet to a wall and press A. Once attached, move the Left thumbstick up."). The claw is
 * behaviour command 19 (vt+0xC0 sub_172E23 reads record +0x22C into +0x716). Logs [BOT-CLIMB]. */
static int climb_test(int index, Bot *b, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    static int who = -2;
    if (who == -2) { const char *e = getenv("RECOMP_BOT_CLIMB_TEST"); who = e ? atoi(e) : -1; }
    if (index != who || me->kind != MA2_KIND_BATTLEARMOR || !g_probe.phys) return 0;
    uint32_t a = me->actor, ai = actor_ai(a);
    if (w->time - c->cl_log_t > 0.5 || w->time < c->cl_log_t) {
        c->cl_log_t = w->time;
        fprintf(stderr, "[BOT-CLIMB] t=%.1f bot=%d ph=%d pos=%.1f,%.1f,%.1f wall=%.0f,%.0f top=%.0f f268=%08X f26C=%08X jump=%u fuel=%.2f rideuid=%08X\n",
                w->time, index, c->cl_phase, me->pos.x, me->pos.y, me->pos.z, c->cl_wall.x, c->cl_wall.z, c->cl_top,
                MEM32(a + 0x268), MEM32(a + 0x26C), ai ? MEM8(ai + 0x240) : 9u, MEMF(a + 0x49C), MEM32(a + 0x1B4));
    }
    if (c->cl_phase == 0) {                                   /* find a tall wall within 30 m */
        if (w->time - c->cl_t < 1.0 && w->time >= c->cl_t) {   /* between looks: wander */
            Ma2Vec3 p = { me->pos.x + sinf((float)w->time * 0.2f) * 40.f, me->pos.y, me->pos.z + cosf((float)w->time * 0.2f) * 40.f };
            ctrl_travel(index, b, w, me, p, 4.f, 0, 0.f);
            return 1;
        }
        c->cl_t = w->time;
        float best = 1e9f; int hits = 0;
        for (int k = 0; k < 16; k++) {
            float ang = (float)k * 0.3927f, t = 0.f; uint32_t ob = 0;
            Ma2Vec3 o = { me->pos.x, me->pos.y + 2.f, me->pos.z }, d = { sinf(ang), 0.f, cosf(ang) };
            if (!ray_cast(&g_probe, o, d, 40.f, 1, 1, &t, &ob) || t < 2.f) continue;
            hits++;
            float t2 = 0.f; uint32_t ob2 = 0;
            Ma2Vec3 o2 = { o.x, me->pos.y + 9.f, o.z };
            if (!ray_cast(&g_probe, o2, d, 40.f, 1, 1, &t2, &ob2) || fabsf(t2 - t) > 1.5f) continue;   /* a vertical face, not a post */
            int ok = 0;
            float top = surface_height(o.x + d.x * (t + 1.5f), o.z + d.z * (t + 1.5f), &ok);
            if (!ok || top - me->pos.y < 6.f) top = me->pos.y + 12.f;
            if (t < best) { best = t; c->cl_wall.x = o.x + d.x * t; c->cl_wall.z = o.z + d.z * t; c->cl_wall.y = me->pos.y; c->cl_dir = d; c->cl_top = top; }
        }
        if (best < 1e8f) { c->cl_phase = 1; c->cl_t = w->time; }
        else if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CLIMB] no wall: %d ray hits\n", hits);
        return 1;
    }
    float dw = hypotf(c->cl_wall.x - me->pos.x, c->cl_wall.z - me->pos.z);
    if (c->cl_phase == 1) {                                   /* walk in close, jet up at the face */
        Ma2Vec3 foot = { c->cl_wall.x - c->cl_dir.x * 4.f, me->pos.y, c->cl_wall.z - c->cl_dir.z * 4.f };
        if (dw > 6.f && w->time - c->cl_t < 20.0) { ctrl_travel(index, b, w, me, foot, 2.f, 0, 0.f); return 1; }
        body_steer(me, c->cl_wall);
        /* a short burn into the face, not over it: the claw wants the wall beside it */
        uint32_t jai = actor_ai(a);
        Ma2Vec3 jd = { c->cl_dir.x * 0.6f, 0.8f, c->cl_dir.z * 0.6f };
        if (jai && !MEM8(jai + 0x240) && MEMF(a + 0x49C) >= 1.2f && ai_startjump(jai, jd, 0.9f, 1.0f) == 1) { c->cl_phase = 2; c->cl_t = w->time; c->jumps++; }
        else if (w->time - c->cl_t > 14.0) { c->cl_phase = 0; c->cl_t = w->time; }
        return 1;
    }
    if (c->cl_phase == 2) {                                   /* airborne at the wall: claw (sub_10239A tries the wall first, sub_1025F5) */
        if (w->time - c->cl_tap_t > 0.1 && dw < 5.f) {
            int r = ba_clamp(a); c->cl_tap_t = w->time;
            fprintf(stderr, "[BOT-CLIMB] claw r=%d at %.1f,%.1f,%.1f d_wall=%.1f f268=%08X cd7cc=%.2f a284=%08X\n", r, me->pos.x, me->pos.y, me->pos.z, dw, MEM32(a + 0x268), MEMF(a + 0x7CC), MEM32(a + 0x284));
            if (r == 1) { c->cl_phase = 3; c->cl_t = w->time; }
        }
        if (w->time - c->cl_t > 3.0) { c->cl_phase = 3; c->cl_t = w->time; }
        return 1;
    }
    if (c->cl_phase == 3) {                                   /* hold the stick up */
        body_stop(a);
        c->axis[4] = 1.f;
        /* Past the roof (or 40 m up a face with no top -- the map's edge wall took one to y=690): let go. */
        if (me->pos.y > c->cl_top + 2.f || me->pos.y > c->cl_wall.y + 40.f || w->time - c->cl_t > 8.0) {
            int ok = 0; float sfc = surface_height(me->pos.x + c->cl_dir.x * 3.f, me->pos.z + c->cl_dir.z * 3.f, &ok);
            ba_release(a); c->axis[4] = 0.f;
            fprintf(stderr, "[BOT-CLIMB] let go at y=%.1f (top %.0f, roof ahead %.1f ok=%d) after %.1fs\n", me->pos.y, c->cl_top, sfc, ok, w->time - c->cl_t);
            c->cl_phase = 4; c->cl_t = w->time;
            if (me->pos.y > c->cl_top - 1.f && ok && fabsf(sfc - c->cl_top) < 4.f) {   /* step over onto the roof */
                Ma2Vec3 over = { me->pos.x + c->cl_dir.x * 6.f, c->cl_top + 1.f, me->pos.z + c->cl_dir.z * 6.f };
                body_jump_up(a, me->pos, over, 0.5f);
            }
        }
        return 1;
    }
    c->axis[4] = 0.f;
    if (w->time - c->cl_t > 6.0) { c->cl_phase = 0; c->cl_t = w->time; }
    return 1;
}

/* (a) BattleArmor wall climb on the brain's order (bots batch 5, RECOMP_BOT_BA_CLIMB / [Bots] BaWallClimb, the brain's
 * switch): climb_test's recipe (research 4: the claw probe sub_1025F5 takes a building face within 8 m unless a no-climb
 * node, so ba_clamp's own answer decides). Face toward o->climb_pt: rays at +2 m and +9 m (a vertical face); walk to
 * 4 m, a short jet into it, claw every 0.1 s within 5 m; latched: stick up (fire at the target if it can); past the
 * top let go and step over. No latch in 3 s, no face, 9 s on the wall, or a fall: failed (status 3), the brain marks
 * the top. [C7] hg_climb faces latched topped knocked_off no_face no_latch timeouts shots_latched. */
static int climb_order(int index, Bot *b, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me, const Ma2Order *o)
{
    uint32_t a = me->actor;
    if (!o->climb || me->kind != MA2_KIND_BATTLEARMOR || !g_probe.phys) {
        if (c->co_phase) {                                    /* the brain called it off */
            if (c->co_phase == 3) ba_release(a);
            c->co_phase = 0; c->axis[4] = 0.f;
        }
        c->co_done = 0; b->status.climb_state = 0;
        return 0;
    }
    if (c->co_done && c->co_phase == 0) return 0;              /* ended (2/3): hold it until the brain drops the order */
    b->status.climb_state = 1;
    if (c->co_phase == 0) {                                   /* the face toward the roof */
        /* push-x2: 24 headings all round (the BattleArmor may stand right at the wall, or on its far side), a vertical face
         * (two rays 7 m apart agree) whose foot is within 24 m of the roof spot, the nearest one wins. */
        float best = 1e9f; int nh1 = 0, nh2 = 0, nfar = 0;
        for (int k = 0; k < 24; k++) {
            float ang = (float)k * (6.2831853f / 24.f), t = 0.f, t2 = 0.f; uint32_t ob = 0, ob2 = 0;
            Ma2Vec3 o1 = { me->pos.x, me->pos.y + 2.f, me->pos.z }, d = { sinf(ang), 0.f, cosf(ang) }, o2 = { me->pos.x, me->pos.y + 9.f, me->pos.z };
            if (!ray_cast(&g_probe, o1, d, 70.f, 1, 1, &t, &ob) || t < 0.3f) continue;
            nh1++;
            if (!ray_cast(&g_probe, o2, d, 70.f, 1, 1, &t2, &ob2) || fabsf(t2 - t) > 1.5f) continue;
            nh2++;
            if (hypotf(o1.x + d.x * t - o->climb_pt.x, o1.z + d.z * t - o->climb_pt.z) > 24.f) { nfar++; continue; }
            if (t < best) { best = t; c->co_wall.x = o1.x + d.x * t; c->co_wall.z = o1.z + d.z * t; c->co_wall.y = me->pos.y; c->co_dir = d; }
        }
        if (best > 1e8f) {
            c7_add("hg_climb", "no_face", 1); b->status.climb_state = 3; c->co_done = 1;
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CLIMB] t=%.1f bot=%d no face toward %.0f,%.0f from %.0f,%.0f,%.0f: hits %d vertical %d far %d\n", w->time, index, o->climb_pt.x, o->climb_pt.z, me->pos.x, me->pos.y, me->pos.z, nh1, nh2, nfar);
            return 0;
        }
        c->co_top = o->climb_pt.y; c->co_phase = 1; c->co_t = w->time; c->co_latched = 0; c->co_ymax = me->pos.y;
        c7_add("hg_climb", "faces", 1);

    }
    float dw = hypotf(c->co_wall.x - me->pos.x, c->co_wall.z - me->pos.z);
    if (me->pos.y > c->co_ymax) c->co_ymax = me->pos.y;
    if (c->co_phase == 1) {                                   /* walk in close, jet up at the face */
        Ma2Vec3 foot = { c->co_wall.x - c->co_dir.x * 4.f, me->pos.y, c->co_wall.z - c->co_dir.z * 4.f };
        if (dw > 6.f && w->time - c->co_t < 15.0) { ctrl_travel(index, b, w, me, foot, 2.f, 0, 0.f); return 1; }
        body_steer(me, c->co_wall);
        uint32_t jai = actor_ai(a);
        Ma2Vec3 jd = { c->co_dir.x * 0.6f, 0.8f, c->co_dir.z * 0.6f };
        if (jai && !MEM8(jai + 0x240) && MEMF(a + 0x49C) >= 1.2f && ai_startjump(jai, jd, 0.9f, 1.0f) == 1) { c->co_phase = 2; c->co_t = w->time; c->jumps++; }
        else if (w->time - c->co_t > 15.0) { c7_add("hg_climb", "timeouts", 1); c->co_phase = 0; b->status.climb_state = 3; c->co_done = 1; }
        return 1;
    }
    if (c->co_phase == 2) {                                   /* airborne at the wall: claw */
        if (w->time - c->co_tap_t > 0.1 && dw < 5.f) {
            c->co_tap_t = w->time;
            if (ba_clamp(a) == 1) { c->co_phase = 3; c->co_t = w->time; c->co_latched = 1; c7_add("hg_climb", "latched", 1); }
        }
        if (c->co_phase == 2 && w->time - c->co_t > 3.0) { c7_add("hg_climb", "no_latch", 1); c->co_phase = 0; b->status.climb_state = 3; c->co_done = 1; }
        return 1;
    }
    if (c->co_phase == 3) {                                   /* hold the stick up */
        body_stop(a);
        c->axis[4] = 1.f;
        if (o->fire && o->target && ctrl_fire(c, w, me, o->target)) c7_add("hg_climb", "shots_latched", 1);
        if (me->pos.y < c->co_ymax - 3.f) {                   /* knocked off (a hit >= 110) or slipped */
            c7_add("hg_climb", "knocked_off", 1); c->axis[4] = 0.f; c->co_phase = 0; b->status.climb_state = 3; c->co_done = 1;
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CLIMB] t=%.1f bot=%d fell off at y=%.1f (max %.1f)\n", w->time, index, me->pos.y, c->co_ymax);
            return 1;
        }
        if (me->pos.y > c->co_top + 2.f || w->time - c->co_t > 9.0) {
            int ok = 0; float sfc = surface_height(me->pos.x + c->co_dir.x * 3.f, me->pos.z + c->co_dir.z * 3.f, &ok);
            int over = me->pos.y > c->co_top - 1.f && ok && fabsf(sfc - c->co_top) < 4.f;
            ba_release(a); c->axis[4] = 0.f;
            if (over) {
                Ma2Vec3 p = { me->pos.x + c->co_dir.x * 6.f, c->co_top + 1.f, me->pos.z + c->co_dir.z * 6.f };
                body_jump_up(a, me->pos, p, 0.5f);
                c7_add("hg_climb", "topped", 1); b->status.climb_state = 2;
            } else { c7_add("hg_climb", "timeouts", 1); b->status.climb_state = 3; }
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CLIMB] t=%.1f bot=%d let go at y=%.1f top %.0f %s after %.1f s on the wall\n", w->time, index, me->pos.y, c->co_top, over ? "over" : "short", w->time - c->co_t);
            c->co_res = b->status.climb_state; c->co_phase = 4; c->co_t = w->time;
        }
        return 1;
    }
    if (w->time - c->co_t < 1.0) { b->status.climb_state = c->co_res; return 1; }   /* the step over (or a short let-go) */
    c->co_phase = 0; b->status.climb_state = c->co_res; c->co_done = 1;
    return 0;
}

/* RECOMP_BOT_JET_SHOW=1 (developer test for the renderer): the first bot whose mech or BattleArmor
 * carries jump jets walks to a spot 30 m in front of the human and jets straight up there every 5 s,
 * keeping another body's jump-jet effect in view. Its brain idles meanwhile. Logs [BOT-JETSHOW]. */
/* Developer: RECOMP_BOT_HULL_TEST=1 -- every bot in a ground body walks back and forth across the
 * nearest parked VTOL, to points 24 m out either side of it on a line turned 35 degrees each trip
 * (and per bot), so a wing, the nose or the tail is in the way of most trips. Odd-numbered bots
 * walk as pilots from the start. Each trip logs its time, the closest it came to the outline and
 * the backtracks/escapes it needed: [BOT-HULLTEST]. */
static int hull_test(int index, Bot *b, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    static int on = -1;
    static struct { uint32_t veh, actor; int trip, side; double t0; float minclr; unsigned esc0, bt0; } st[MAX_BOTS];
    if (on < 0) on = env_on("RECOMP_BOT_HULL_TEST");
    if (!on || index < 0 || index >= MAX_BOTS || me->kind == MA2_KIND_VTOL || me->kind == MA2_KIND_FLYER) return 0;
    if (me->kind == MA2_KIND_PILOT && !(index & 1)) return 0;            /* even ones board first */
    const Ma2VehicleView *v = NULL; float bd = 200.f;
    for (int i = 0; i < w->nvehicles; i++) {
        const Ma2VehicleView *q = &w->vehicles[i];
        if (q->occupied || q->kind != MA2_KIND_VTOL || q->ext_max <= 0.5f) continue;
        float d = hdist2(q->pos, me->pos);
        if (d < bd) { bd = d; v = q; }
    }
    if (!v) return 0;
    if (st[index].veh != v->actor || st[index].actor != me->actor) {
        st[index].veh = v->actor; st[index].actor = me->actor; st[index].trip = 0; st[index].side = 1;
        st[index].t0 = w->time; st[index].minclr = 1e9f; st[index].esc0 = c->escapes; st[index].bt0 = c->backtracks;
    }
    float a = (float)st[index].trip * 0.61f + (float)index * 1.1f;
    Ma2Vec3 goal = { v->pos.x + sinf(a) * 24.f * (float)st[index].side, me->pos.y, v->pos.z + cosf(a) * 24.f * (float)st[index].side };
    float clr = hdist2(me->pos, v->pos) - (hull_at(v, hull_tall(me->kind), me->pos, 0.f) - 0.6f);
    if (clr < st[index].minclr) st[index].minclr = clr;
    if (hdist2(me->pos, goal) < 5.f || w->time - st[index].t0 > 40.0) {
        fprintf(stderr, "[BOT-HULLTEST] bot=%d kind=%d veh=%08X trip=%d %s %.1f s closest=%.1f m backtracks=%u escapes=%u t=%.1f\n", index, me->kind, v->actor,
                st[index].trip, w->time - st[index].t0 > 40.0 ? "TIMEOUT" : "done", w->time - st[index].t0, st[index].minclr,
                c->backtracks - st[index].bt0, c->escapes - st[index].esc0, w->time);
        st[index].trip++; st[index].side = -st[index].side; st[index].t0 = w->time; st[index].minclr = 1e9f;
        st[index].esc0 = c->escapes; st[index].bt0 = c->backtracks;
        return 1;
    }
    ctrl_travel(index, b, w, me, goal, 3.f, 0, 0.f);
    return 1;
}
static int jet_show(int index, Bot *b, BotCtrl *c, const Ma2World *w, const Ma2PlayerView *me)
{
    static int on = -1, who = -1; static double next;
    if (on < 0) on = env_on("RECOMP_BOT_JET_SHOW");
    if (!on || (me->kind != MA2_KIND_MECH && me->kind != MA2_KIND_BATTLEARMOR)) return 0;
    const Ma2PlayerView *h = NULL;
    for (int i = 0; i < w->nplayers; i++) if (w->players[i].is_human && !w->players[i].is_bot && w->players[i].alive) { h = &w->players[i]; break; }
    if (!h) return 0;
    if (who < 0 && MEMF(me->actor + 0x49C) >= 2.f && me->nation == h->nation && hdist2(me->pos, h->pos) < 150.f) {
        who = index;                                   /* a teammate near the human, so it arrives soon */
        fprintf(stderr, "[BOT-JETSHOW] bot=%d kind=%d fuel=%.1f\n", index, me->kind, MEMF(me->actor + 0x49C));
    }
    if (index != who) return 0;
    if (w->time - c->cl_log_t > 2.0 || w->time < c->cl_log_t) {
        c->cl_log_t = w->time;
        uint32_t jai = actor_ai(me->actor);
        fprintf(stderr, "[BOT-JETSHOW] t=%.1f bot=%d at %.0f,%.1f,%.0f human %.0f,%.0f yaw %.2f fuel %.2f burning %d\n", w->time, index,
                me->pos.x, me->pos.y, me->pos.z, h->pos.x, h->pos.z, h->yaw, MEMF(me->actor + 0x49C), jai ? MEM8(jai + 0x240) : -1);
    }
    Ma2Vec3 spot = { h->pos.x + sinf(h->yaw) * 30.f, me->pos.y, h->pos.z + cosf(h->yaw) * 30.f };
    uint32_t jai = actor_ai(me->actor);
    int burning = jai && MEM8(jai + 0x240);
    if (!burning && hdist2(me->pos, spot) > 15.f) { ctrl_travel(index, b, w, me, spot, 4.f, 0, 0.f); return 1; }
    /* The jump runs inside a MoveTo (an asleep AI never lifts off): a step across the human's
     * view, renewed every frame through the burn, keeps one going. */
    static int sidesign = 1;
    if (!burning && hdist2(me->pos, spot) < 2.f) sidesign = -sidesign;
    Ma2Vec3 side = { spot.x + cosf(h->yaw) * 10.f * sidesign, me->pos.y, spot.z - sinf(h->yaw) * 10.f * sidesign };
    body_moveto(me->actor, side);
    if (burning) return 1;
    if (w->time >= next || w->time + 10.0 < next) {
        Ma2Vec3 up = { side.x, me->pos.y + 15.f, side.z };
        if (body_jump_up(me->actor, me->pos, up, 1.0f)) {
            next = w->time + 5.0;
            fprintf(stderr, "[BOT-JETSHOW] t=%.1f bot=%d kind=%d jets at %.0f,%.0f,%.0f\n", w->time, index, me->kind, me->pos.x, me->pos.y, me->pos.z);
        }
    }
    return 1;
}

/* Pilot bombs (playtest 2026-10-27: on foot the pilot's weapon is a bomb, left mouse = place, press
 * again = set it off; wiki pilot-bomb-*). The human path: fire (command 21, behaviour +0x268) -> behaviour
 * slot 50 sub_1B0682 (avatar +0x26C bit 17 set, bit 25 clear) -> sub_1041FB -> DropPackManager at avatar
 * +0x254 (state +0x80, pack +0x84, out +0x7C = a byte). The pack goes off when an enemy stays inside 15 m for 5 s:
 * 1000 damage over 25 m, teammates included. A bot pilot with no teammate within 30 m plants one
 * (a) in the path of an enemy closing on it from 20-70 m, or (b) beside a free ride an enemy pilot is
 * walking to (not the ride it is going for itself); it never presses while its own pack is out (that
 * press would set it off), then runs 35 m clear for 5 s. Switch RECOMP_BOT_PILOT_BOMB / [Bots] PilotBomb.
 * [C7] pbomb: tries_ambush / tries_trap / placed / gone / skip_team. */
static int pbomb_on(void) { return c7_on("pbomb", "RECOMP_BOT_PILOT_BOMB", "PilotBomb", 1); }
static const char *const k_bomb_why[6] = { "tries_none", "tries_ambush", "tries_trap", "tries_guard", "tries_checkpoint", "tries_core" };   /* +6 = the name */
/* Pilot bombs: a pack goes off 5 s after a unit that sets it off (FFA: anyone but its owner; teams:
 * another nation) comes inside 15 m -- 1000 damage over 25 m, owner and teammates included -- and a blast sets
 * off the packs around it. So a bomb with its fuse running (or within 25 m of one that has) is left AT ONCE to
 * past 25 m plus our hull, whatever the bot was doing; one we would set off ourselves is left before we reach
 * its 15 m. Out the way that is clear of every such bomb and of walls, held until we are 2 m past the edge or
 * it has gone off. Our own fresh pack is the planting code's (ctrl_pilot_bomb's run-off).
 * [C7] bombavoid: left_zone (escapes), escaped (out while the bomb was still there), went_off (it went off first;
 * we lived only if we were past its edge). */
static void bomb_escape_end(BotCtrl *c, const Ma2World *w)
{
    if (c->bav_until <= 0.0) return;
    int there = 0;
    for (int i = 0; i < w->nbombs; i++) there |= w->bombs[i].ent == c->bav_ent;
    c7_add("bombavoid", there ? "escaped" : "went_off", 1);
    c->bav_until = 0.0;
}
/* Death blasts (core blasts_update): a 'Mech that has just gone down blows up within seconds; everyone inside its
 * radius leaves at once, like an armed pack -- even a Recruit knows. Reaction by skill (Recruit 0.4 s .. Elite 0.1 s),
 * so the weakest are still caught now and then (the fuse is 2.2 s: 0.6 s was a quarter of it). [C7] deathblast: fled
 * (escapes started), mechs_down (core), hops (BattleArmor jet hops out), fire_held (frames a shot was held).
 * BlastDrive (RECOMP_BOT_BLAST_DRIVE, default 1): how a 'Mech or tank leaves. Traced 11-01 (two 5v6 matches): MoveTo
 * to the escape point moved 'Mechs at half their top speed (0.51 aligned with it, travel 0.93) -- a running 'Mech
 * turning round plays a ~1 s pivot (anim 6) at walk pace -- and a BattleArmor not at all (its walk is
 * animation-driven: it only moves on jet hops). 0 = MoveTo to the escape point (old), 1 = MoveTo to a point 80 m out
 * along the same line, 2 = the body's own turn at full forward speed (walk pace: worse), 3 = 1, but a 'Mech whose
 * way out is behind its nose backs straight out. RECOMP_TEST_FAKE_BLAST A/B, ~48 escapes each, blast 18 m ahead of
 * a running 'Mech: outward speed 4.2 / 5.4 / 3.1 / 6.6 m/s, mean metres gained by the blast 6.6 / 8.4 / 4.5 / 11.0.
 * 3 is a test mode only: in a real match (game-bR1) the guest crashed with a wild write in the 'Mech animation update
 * (MAMech slot 51 sub_FED88 -> the +0x868 track table) while four 'Mechs backed out (anim 37); the campaign AI never
 * drives a 'Mech backwards. 1 is the default.
 * BattleArmor hops (BlastHop, RECOMP_BOT_BLAST_HOP, default on): 0 -> 16 m/s outward. BlastHoldFire
 * (RECOMP_BOT_BLAST_HOLD_FIRE, default on): no shots while leaving a death blast -- aiming the torso back at
 * a target behind fights the turn away. */
static const float k_blast_react[4] = { 0.4f, 0.3f, 0.2f, 0.1f };
static float blast_react(const BotCtrl *c) { return (c->skill & 3) == 3 && ma2_bots_dragoon() ? 0.03f : k_blast_react[c->skill & 3]; }   /* Dragoon: at once */
static int blast_drive_mode(void) { static int m = -1; if (m < 0) { m = env_int("RECOMP_BOT_BLAST_DRIVE", 1); if (m < 0 || m > 3) m = 1; } return m; }
static int blast_hop_on(void) { return c7_on("blasthop", "RECOMP_BOT_BLAST_HOP", "BlastHop", 1); }
static int blast_hold_fire_on(void) { return c7_on("blastholdfire", "RECOMP_BOT_BLAST_HOLD_FIRE", "BlastHoldFire", 1); }
/* BaHackFlee (default on, RECOMP_BOT_BA_HACK_FLEE / [Bots] BaHackFlee): playtest 11-01, bots should flee enemy
 * BattleArmor at close range (a successful hack is an instant kill), about as they flee
 * a 'Mech that is about to blow up. A bot in a 'Mech (only a
 * 'Mech can be NeuroHacked) treats every enemy BattleArmor inside 18 m (the claw needs ~11 m and the jets close the rest in
 * a moment; within 15 m of our height, not on a VTOL, not clamped to anyone) like a death blast: after the same reaction by
 * skill it leaves by the same escape-point scoring until 26 m off (22 m past our hull). Not once one is clamped on (the
 * NeuroHack answer is the QTE code's). Only the walk changes: aim and fire carry on. [C7] bahackflee: fled, fled_sk0..3,
 * vs_human, flee_s. */
static int ba_hack_flee_on(void) { return c7_on("bahackflee", "RECOMP_BOT_BA_HACK_FLEE", "BaHackFlee", 1); }
static void ctrl_bomb_escape(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me)
{
    BotCtrl *c = &b->ctrl;
    int k = me->kind;
    Ma2Vec3 bap[6]; uint32_t bae[6]; int bah[6], nba = 0;
    if (k == MA2_KIND_MECH && !me->enemy_rider && ba_hack_flee_on()) {
        int close = 0;
        for (int i = 0; i < w->nplayers && nba < 6; i++) {
            const Ma2PlayerView *q = &w->players[i];
            if (q == me || !q->alive || q->actor == me->actor || q->kind != MA2_KIND_BATTLEARMOR || q->riding || q->clamped_to) continue;
            if (w->team_mode && q->nation == me->nation) continue;
            float d = hdist2(q->pos, me->pos);
            if (fabsf(q->pos.y - me->pos.y) > 15.f) continue;
            if (d < 18.f) close = 1;
            if (d < 18.f || (c->bav_until > 0.0 && d < 26.f)) { bap[nba] = q->pos; bae[nba] = q->actor; bah[nba] = q->is_human; nba++; }
        }
        if (close) { if (c->bah_t0 <= 0.0 || w->time < c->bah_t0) c->bah_t0 = w->time; }
        else if (!nba) c->bah_t0 = 0.0;
        if (nba && (c->bah_t0 <= 0.0 || w->time - c->bah_t0 < blast_react(c))) nba = 0;   /* the death blast's reaction by skill */
    } else c->bah_t0 = 0.0;
    if ((!w->nbombs && !w->nblasts && !nba) || k == MA2_KIND_VTOL || k == MA2_KIND_FLYER || k == MA2_KIND_TURRET || (w->time < c->bomb_flee_until && !w->nblasts)) { bomb_escape_end(c, w); return; }
    float hull = k == MA2_KIND_MECH || k == MA2_KIND_TANK ? 4.f : k == MA2_KIND_PILOT ? 1.f : 2.5f;
    float trig = 15.f + hull + 3.f, hold = c->bav_until > 0.0 ? 2.f : 0.f;
    /* the hot hazards: position, the radius to get past, identity, and whether it is a death blast */
    Ma2Vec3 hp[24]; float hr[24]; uint32_t he[24]; int hb[24], nh = 0;
    for (int i = 0; i < w->nbombs && i < 16; i++) {
        int armed = w->bombs[i].fuse > 0.0;
        for (int j = 0; j < w->nbombs && !armed; j++)                    /* a chain: inside a running one's blast */
            armed = j != i && w->bombs[j].fuse > 0.0 && hdist2(w->bombs[i].pos, w->bombs[j].pos) < 25.f;
        int sets_off = w->bombs[i].owner != b->player && !(w->team_mode && w->bombs[i].nation == me->nation);
        float d = hdist2(w->bombs[i].pos, me->pos), blast = 25.f + hull + 2.f;
        if (armed ? d < blast + hold : sets_off && d < trig + hold) { hp[nh] = w->bombs[i].pos; hr[nh] = blast; he[nh] = w->bombs[i].ent; hb[nh] = 0; nh++; }
    }
    for (int i = 0; i < w->nblasts && nh < 24; i++) {
        if (w->blasts[i].actor == me->actor || w->time - w->blasts[i].t0 < blast_react(c)) continue;
        float r = w->blasts[i].r + hull + 2.f;
        /* once leaving, keep going to 8 m past the edge: at 2 m the goal walked them back in and they fled again */
        if (hdist2(w->blasts[i].pos, me->pos) < r + (c->bav_until > 0.0 ? 8.f : 0.f)) { hp[nh] = w->blasts[i].pos; hr[nh] = r; he[nh] = w->blasts[i].actor; hb[nh] = 1; nh++; }
    }
    for (int i = 0; i < nba && nh < 24; i++) { hp[nh] = bap[i]; hr[nh] = 26.f; he[nh] = bae[i]; hb[nh] = 2; nh++; }   /* BaHackFlee */
    if (nba) c7_add("bahackflee", "flee_s", w->dt);
    if (!nh) { bomb_escape_end(c, w); return; }
    if (c->bav_until <= 0.0 || w->time - c->bav_in_since > 0.5) {      /* pick (again) where to go */
        float px = 0.f, pz = 0.f, reach = 0.f;
        for (int h = 0; h < nh; h++) {
            float dx = me->pos.x - hp[h].x, dz = me->pos.z - hp[h].z, l = hypotf(dx, dz);
            if (l < 0.5f) { dx = (float)((index & 1) * 2 - 1); dz = 0.3f; l = hypotf(dx, dz); }
            px += dx / l * (hr[h] + 2.f - l); pz += dz / l * (hr[h] + 2.f - l);
            if (hr[h] > reach) reach = hr[h];
        }
        float l = hypotf(px, pz);
        if (l < 1e-3f) { px = 1.f; pz = 0.f; l = 1.f; }
        px /= l; pz /= l;
        static const float rot[7] = { 0.f, 0.6f, -0.6f, 1.2f, -1.2f, 1.9f, -1.9f };
        float best = -1e9f; Ma2Vec3 bpt = me->pos;
        for (int r = 0; r < 7; r++) {
            float cs = cosf(rot[r]), sn = sinf(rot[r]), ux = px * cs - pz * sn, uz = px * sn + pz * cs;
            Ma2Vec3 q = me->pos; q.x += ux * (reach + 4.f); q.z += uz * (reach + 4.f);
            float m = 1e9f;                                              /* how far that point is past the nearest hot edge */
            for (int h = 0; h < nh; h++) m = minf2(m, hdist2(hp[h], q) - hr[h]);
            float clear = ma2_trav_clear(q.x, q.z, w->time), mid = ma2_trav_clear(me->pos.x + ux * 8.f, me->pos.z + uz * 8.f, w->time);
            float score = minf2(m, 2.f) - (clear < 2.f ? 30.f : 0.f) - (mid < 1.5f ? 30.f : 0.f) - (float)r * 0.5f;
            if (score > best) { best = score; bpt = q; }
        }
        if (c->bav_until <= 0.0) {
            int blast_only = 1, ba_only = 1, ba_h = 0; for (int h = 0; h < nh; h++) { blast_only &= hb[h] == 1; ba_only &= hb[h] == 2; }
            for (int h = 0; h < nba; h++) ba_h |= bah[h];
            if (nba) {
                static const char *const sk[4] = { "fled_sk0", "fled_sk1", "fled_sk2", "fled_sk3" };
                c7_add("bahackflee", "fled", 1); c7_add("bahackflee", sk[c->skill & 3], 1); if (ba_h) c7_add("bahackflee", "vs_human", 1);
            }
            if (!ba_only) c7_add(blast_only ? "deathblast" : "bombavoid", blast_only ? "fled" : "left_zone", 1);
            if (env_on("RECOMP_BOT_DIAG"))
                fprintf(stderr, "[BOT-BOMBAV] t=%.1f bot=%d kind=%d sk=%d %d hot%s, nearest %08X %.1f m: leaving to %.0f,%.0f\n", w->time, index, k, c->skill & 3, nh,
                        ba_only ? (ba_h ? " (enemy BattleArmor, human)" : " (enemy BattleArmor)") : blast_only ? " (death blast)" : "", he[0], hdist2(hp[0], me->pos), bpt.x, bpt.z);
        }
        { float nd = 1e9f; for (int h = 0; h < nh; h++) if (hdist2(hp[h], me->pos) < nd) { nd = hdist2(hp[h], me->pos); c->bav_ent = he[h]; } }
        if (c->bav_until <= 0.0) { c->ed_on = 0; c->ed_t = 0.0; }   /* EscapeDirect: a new escape starts on MoveTo */
        c->bav_pt = bpt; c->bav_in_since = w->time; c->bav_until = w->time + 60.0;
    }
    int blast_on = 0; for (int h = 0; h < nh; h++) blast_on |= hb[h] == 1;
    c->bav_blast = blast_on;
    if (blast_on && blast_hold_fire_on()) c->bav_nofire_until = w->time + 0.25;   /* ctrl_fire runs earlier in the tick */
    if (k == MA2_KIND_PILOT) body_walk_direct(me->actor, me->pos, c->bav_pt, 100);
    else {
        int dm = blast_on ? blast_drive_mode() : 0;
        Ma2Vec3 to = c->bav_pt; float ex = to.x - me->pos.x, ez = to.z - me->pos.z, el = hypotf(ex, ez);
        if (dm >= 1 && el > 0.5f) { to.x = me->pos.x + ex / el * 80.f; to.z = me->pos.z + ez / el * 80.f; }
        /* 3: the way out is behind the nose -- back straight out (forward speed -100%: sub_2AD5C0 scales the signed percent
         * and clamps to +-comp+0xC0), the nose held on the blast; a turn round first cost ~1 s of the 2.2 s (pivot anim 6) */
        uint32_t nd = MEM32(me->actor + 0x94); float fx = 0.f, fz = 0.f;
        if (dm == 3 && k == MA2_KIND_MECH && gva_ok(nd, 0x100)) { fx = MEMF(nd + 0xE0); fz = MEMF(nd + 0xE8); }
        if (dm == 3 && el > 0.5f && (fx * ex + fz * ez) / el < -0.3f) {
            Ma2Vec3 face = me->pos; face.x -= ex / el * 20.f; face.z -= ez / el * 20.f;
            body_walk_direct(me->actor, me->pos, face, -100); c7_add("deathblast", "backing", 1);
        } else if (dm >= 2 && k == MA2_KIND_MECH) body_walk_direct(me->actor, me->pos, to, 100);
        else if (k == MA2_KIND_MECH && blast_on && el > 0.5f && c7_on("escdirect", "RECOMP_BOT_ESC_DIRECT", "EscapeDirect", 1) &&
                 (c->ed_on || ((me->vel.x * ex + me->vel.z * ez) / el < 1.f && (c->ed_t > 0.0 ? w->time - c->ed_t > 0.5 : (c->ed_t = w->time, 0))))) {   /* inward OR stalled 0.5 s (r2_212 b2 stood 0 m/s at 45 m, died) */
            /* EscapeDirect (default on, RECOMP_BOT_ESC_DIRECT / [Bots] EscapeDirect): MoveTo follows a cached sub-goal (sub_185D6E, ai+0x1E4) until it
             * is reached, so a new ai+0x130 does not turn a running 'Mech: au5_20 bot 7 ran 16 m/s INTO the blast for its whole escape (87.3-88.6,
             * -13.4 m/s outward) and died; 13 % of escape frames over 4 matches moved inward. Still heading in after 0.25 s: the body's own turn +
             * speed for the rest of this escape. [C7] escdirect: n. */
            if (!c->ed_on) { c->ed_on = 1; c7_add("escdirect", "n", 1); }
            body_walk_direct(me->actor, me->pos, to, 100);
        }
        else if (k == MA2_KIND_TANK && blast_on && el > 0.5f && c7_on("tankblastswerve", "RECOMP_BOT_TANK_BLAST_SWERVE", "TankBlastSwerve", 1)) {
            /* TankBlastSwerve (default on; film review 11-08): an escape point behind a tank is a pivot on the spot -- p5_85a1 T2 tank b10 got
             * "leaving to 1173,1444" at 63.3 beside the dying b3, moved 3 m in 2 s and died in the blast (65.5, also 76 % off b9). As
             * TankBrakeSwerve: out along the way it already faces, 30 m on, 15 m away from the blast. [C7] tankblastswerve: s. */
            float sp = hypotf(me->vel.x, me->vel.z);
            float hx = sp > 1.f ? me->vel.x / sp : sinf(me->yaw), hz = sp > 1.f ? me->vel.z / sp : cosf(me->yaw);   /* the way it faces / moves */
            float ox = ex / el, oz = ez / el;                       /* away from the blast */
            if (hx * ox + hz * oz < -0.2f) {                        /* nose toward the blast: out along the tangent on its own side */
                float tx = -oz, tz = ox;
                if (tx * hx + tz * hz < 0.f) { tx = -tx; tz = -tz; }
                hx = tx; hz = tz;
            }
            Ma2Vec3 sw = me->pos; sw.x += hx * 30.f + ox * 15.f; sw.z += hz * 30.f + oz * 15.f;
            body_steer(me, sw); c7_add("tankblastswerve", "s", w->dt);
        }
        else { if ((me->vel.x * ex + me->vel.z * ez) / maxf2(el, 0.5f) >= 1.f) c->ed_t = 0.0; body_steer(me, to); }
        if (k == MA2_KIND_BATTLEARMOR && blast_hop_on() && body_jump_fuel(me->actor, me->pos, to, 1.05f)) { c->jumps++; if (blast_on) c7_add("deathblast", "hops", 1); }
    }
    c->dest = c->bav_pt; c->mode = CM_MOVE; c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 0.3;
}
static void ctrl_pilot_bomb(int index, Bot *b, const Ma2World *w, const Ma2PlayerView *me)
{
    BotCtrl *c = &b->ctrl;
    if (me->kind != MA2_KIND_PILOT || !pbomb_on()) { c->bomb_hold = 0; c->bomb_flee_until = 0.0; return; }
    uint32_t beh = MEM32(b->player + 0x2F0), av = gva_ok(beh, 0x10) ? MEM32(beh + 0xC) : 0;
    uint32_t mgr = gva_ok(av, 0x270) ? MEM32(av + 0x254) : 0;
    if (env_on("RECOMP_BOT_BOMB_GATE") && (int)(w->time / 5.0) != (int)((w->time - 1.0 / 60.0) / 5.0))
        fprintf(stderr, "[BOT-BOMB-GATE] t=%.1f bot=%d player=%08X beh=%08X av=%08X mgr=%08X ok=%d out=%u pack=%08X st=%u hold=%d next=%.1f since=%.1f here=%d d=%.1f%c",
                w->time, index, b->player, beh, av, mgr, gva_ok(mgr, 0xA8), gva_ok(mgr, 0xA8) ? (uint32_t)MEM8(mgr + 0x7C) : 0u, gva_ok(mgr, 0xA8) ? MEM32(mgr + 0x84) : 0u,
                gva_ok(mgr, 0xA8) ? MEM32(mgr + 0x80) : 0u, c->bomb_hold, c->bomb_next, w->time - c->actor_since, b->order.bomb_here,
                b->order.bomb_here ? hdist2(me->pos, b->order.bomb_pt) : -1.f, 10);
    if (!gva_ok(mgr, 0xA8)) return;
    uint32_t pack = MEM32(mgr + 0x84), out = MEM8(mgr + 0x7C);   /* a BYTE count (the game reads MEM8; bytes +0x7D.. are other data, nonzero for some players) */
    int diag = env_on("RECOMP_BOT_DIAG");
    if (c->bomb_pack && !out && w->time - c->bomb_t > 0.5) {          /* our pack is gone: went off (or was cleared) */
        c7_add("pbomb", "gone", 1);
        { void ma2_bot_bomb_note(uint32_t, uint32_t, int, Ma2Vec3, int); ma2_bot_bomb_note(b->player, pack ? pack : mgr, me->nation, c->bomb_pos, 0); }
        if (diag) fprintf(stderr, "[BOT-BOMB] t=%.1f bot=%d pack gone after %.1fs at %.0f,%.0f%s", w->time, index, w->time - c->bomb_t, c->bomb_pos.x, c->bomb_pos.z, "\n");
        c->bomb_pack = 0; c->bomb_next = w->time + 20.0;
        b->status.bomb_out = 0;
    }
    if (c->bomb_hold > 0) {                                            /* holding fire: the place request */
        c->press[21] = 2; c->bomb_hold--;
        if (out && !c->bomb_pack) {
            c->bomb_pack = out; c->bomb_t = w->time; c->bomb_pos = me->pos; c->bombs_placed++; c->bomb_hold = 0;
            c->bomb_life = c->actor_since + 1.0;                           /* one per life on foot (playtest: opportunistic only) */
            b->status.bomb_out = 1; b->status.bomb_at = me->pos;
            c7_add("pbomb", "placed", 1);
            { void ma2_bot_bomb_note(uint32_t, uint32_t, int, Ma2Vec3, int); ma2_bot_bomb_note(b->player, pack ? pack : mgr, me->nation, me->pos, 1); }
            if (c->bomb_why == 4) c7_add("pbomb", "placed_checkpoint", 1); else if (c->bomb_why == 5) c7_add("pbomb", "placed_core", 1);
            c->bomb_flee_until = c->bomb_why >= 3 ? 0.0 : w->time + 5.0;   /* no enemy on it yet (guard / checkpoint / core): carry on to the ride, no forced run-off */
            if (diag) fprintf(stderr, "[BOT-BOMB] t=%.1f bot=%d PLACED %s (held %.2fs) pack=%08X (mgr %08X state=%u out=%08X) at %.0f,%.0f,%.0f%s", w->time, index,
                              k_bomb_why[c->bomb_why] + 6, w->time - c->bomb_press_t, pack, mgr, MEM32(mgr + 0x80), out, me->pos.x, me->pos.y, me->pos.z, "\n");
        } else if (!c->bomb_hold) {
            c7_add("pbomb", "press_no_pack", 1); c->bomb_next = w->time + 8.0;
            if (diag) fprintf(stderr, "[BOT-BOMB] t=%.1f bot=%d press gave no pack: av=%08X f26c=%08X state=%u pack=%08X out=%08X%s", w->time, index, av,
                              MEM32(av + 0x26C), MEM32(mgr + 0x80), pack, out, "\n");
        }
    }
    /* Set it off by hand (fire again = sub_E6838, the human's second press): an enemy inside 20 m of our bomb,
     * no teammate inside its 25 m blast, and we are 30 m+ clear. The game's own trigger needs the enemy to
     * stay 5 s inside 15 m; a walker crossing it does not. [C7] pbomb detonate / skip_det_team. */
    if (out && c->bomb_pack && w->time - c->bomb_t > 1.0 && w->time >= c->bomb_det_next && hdist2(me->pos, c->bomb_pos) > 30.f &&
        c7_on("pbomb", "RECOMP_BOT_BOMB_DETONATE", "BombDetonate", 1)) {
        const Ma2PlayerView *foe = NULL; int team = 0;
        for (int i = 0; i < w->nplayers; i++) {
            const Ma2PlayerView *p = &w->players[i];
            if (p == me || !p->alive || !p->actor) continue;
            float d = hdist2(p->pos, c->bomb_pos);
            if (w->team_mode && p->nation == me->nation) { if (d < 25.f) team = 1; }
            else if (d < 20.f) foe = p;
        }
        if (foe && team) { c7_add("pbomb", "skip_det_team", 1); c->bomb_det_next = w->time + 0.5; }
        else if (foe) {
            ctrl_tap(c, 21, 2); c->bomb_det_next = w->time + 1.5;
            c7_add("pbomb", "detonate", 1);
            if (diag) fprintf(stderr, "[BOT-BOMB] t=%.1f bot=%d DETONATE at %.0f,%.0f foe=%08X kind=%d d=%.1f me_d=%.0f%s", w->time, index, c->bomb_pos.x, c->bomb_pos.z,
                              foe->actor, foe->kind, hdist2(foe->pos, c->bomb_pos), hdist2(me->pos, c->bomb_pos), "\n");
        }
    }
    if (w->time < c->bomb_flee_until) {                                 /* clear of our own blast */
        if (hdist2(me->pos, c->bomb_pos) < 32.f && c->bav_until <= 0.0) { body_walk_direct(me->actor, me->pos, c->bomb_flee_pt, 100); c->dest = c->bomb_flee_pt; c->mode = CM_MOVE; }
        return;
    }
    if (out || c->bomb_hold > 0 || w->time < c->bomb_next || w->time - c->actor_since < 3.0) return;     /* a press now would set ours off; not on the spawn */
    if (c->bomb_life == c->actor_since + 1.0) return;                 /* this life on foot has had its bomb */
    c->bomb_next = w->time + 0.5;
    const Ma2PlayerView *foe = NULL; float fd = 1e9f, td = 1e9f; int why = 0;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (p == me || !p->alive || !p->actor) continue;
        float d = hdist2(p->pos, me->pos);
        if (w->team_mode && p->nation == me->nation) {
            if (d < td) td = d;                                          /* teammates take the blast too */
            continue;
        }
        float sp = hypotf(p->vel.x, p->vel.z), dx = me->pos.x - p->pos.x, dz = me->pos.z - p->pos.z;
        if (d > 20.f && d < 70.f && sp > 2.f && (p->vel.x * dx + p->vel.z * dz) > 0.8f * sp * d && d < fd) { foe = p; fd = d; why = 1; }
    }
    if (!foe && b->order.bomb_here && hdist2(me->pos, b->order.bomb_pt) < 8.f) why = 2 + b->order.bomb_here;   /* 3 own flag / base, 4 seized checkpoint, 5 core spawn: plant and leave */
    if (!foe && !why)
        for (int j = 0; j < w->nvehicles && j < MA2_BOT_MAX_VEHICLES && !foe; j++) {
            const Ma2VehicleView *v = &w->vehicles[j];
            if (v->occupied || v->kind == MA2_KIND_PILOT || v->actor == c->board_veh || hdist2(v->pos, me->pos) > 12.f) continue;
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (p == me || !p->alive || p->kind != MA2_KIND_PILOT || (w->team_mode && p->nation == me->nation)) continue;
                float d = hdist2(p->pos, v->pos), sp = hypotf(p->vel.x, p->vel.z), dx = v->pos.x - p->pos.x, dz = v->pos.z - p->pos.z;
                if (d > 15.f && d < 90.f && sp > 2.f && (p->vel.x * dx + p->vel.z * dz) > 0.8f * sp * d) { foe = p; why = 2; break; }
            }
        }
    if (!foe && why < 3) return;
    /* the nearest teammate from the bomb (it goes where we stand): an ambush or trap is set off soon, so
     * 30 m; a guard bomb sits on the enemy's way in, 30 m out from the flag its guards stay around: 20 m */
    if (td < (why >= 3 ? 20.f : 30.f)) { c7_add("pbomb", why >= 3 ? "skip_team_guard" : "skip_team", 1); c->bomb_next = w->time + 2.0; return; }
    {   /* the skill decides whether the bot thinks of it: Recruit 1 in 4 .. Elite always */
        static const float p_sk[4] = { 0.25f, 0.5f, 0.8f, 1.0f };
        if (aim_rnd(c) >= p_sk[skill_k(c)]) { c->bomb_next = w->time + 6.0; c7_add("pbomb", "skill_pass", 1); return; }
    }
    c7_add("pbomb", k_bomb_why[why], 1);
    c->bomb_press_t = w->time;
    c->bomb_why = why; c->bomb_hold = 200; c->bomb_next = w->time + 20.0;   /* held until the pack is out (the place request needs the button down; release cancels it) */
    {   /* run-off point: 35 m from here, away from the enemy */
        float dx = foe ? me->pos.x - foe->pos.x : 1.f, dz = foe ? me->pos.z - foe->pos.z : 0.f, l = hypotf(dx, dz) + 1e-3f;
        c->bomb_flee_pt = me->pos; c->bomb_flee_pt.x += dx / l * 35.f; c->bomb_flee_pt.z += dz / l * 35.f;
    }
    if (diag) fprintf(stderr, "[BOT-BOMB] t=%.1f bot=%d press (%s) foe=%08X kind=%d d=%.0f av=%08X f26c=%08X b17=%u b25=%u mgr=%08X state=%u%s", w->time, index,
                      k_bomb_why[why] + 6, foe ? foe->actor : 0u, foe ? foe->kind : -1, foe ? hdist2(foe->pos, me->pos) : -1.f, av, MEM32(av + 0x26C), (MEM32(av + 0x26C) >> 17) & 1u,
                      (MEM32(av + 0x26C) >> 25) & 1u, mgr, MEM32(mgr + 0x80), "\n");
}
static void ctrl_tick(int index, Bot *b, const Ma2World *w, int self)
{
    BotCtrl *c = &b->ctrl;
    const Ma2PlayerView *me = &w->players[self];
    const Ma2Order *o = &b->order;
    g_drv_line = 0; g_drv_kind = 0;
    c->va_bprev = c->va_branch; c->va_branch = 0; c->vs_fprev = c->vs_flags; c->vs_flags = 0;   /* VTOL: what flew it last step (vt_slow_note, vt_arrive_measure) */
    /* SinkAhead (stale sink): a -30 sink is asserted afresh each step by whoever wants it. Left set, it outlived the rider's drop sink
     * when the brain turned to "lift-climb" (rider under 12 m): v29d4 (BD CTF, blue bot 8, t=93.4-94.2) the craft kept sinking at
     * -25..-32 m/s through 0.8 s of steps that flew no stick branch, 27 -> 4 m over the ground, its rider put down 61 m short. */
    if (c->vt_lift_demand < 0.f && o->type != MA2_ORD_EJECT && sink_ahead_on()) c->vt_lift_demand = 0.f;
    c->seen_tgt = o->seen ? o->target : 0;
    c->blind = o->seen && o->blind; c->blind_pt = o->blind_pt;
    c->sp_far = o->sp_far;
    /* kind NONE: no body to drive (e.g. a NeuroHack victim's plain-MACorporeal placeholder until he respawns, push W1b) */
    if (!me->alive || !me->actor || me->kind == MA2_KIND_NONE) { c->engaged = 0; c->have_path = 0; c->mode = CM_IDLE; if (c->pq_ticket) { ma2_pathq_cancel(index); c->pq_ticket = 0; } return; }
    b->status.route_fails = c->path_fails;   /* ObjRouteFails: the brain's ObjGiveUp reads it every step (no_path is set on plan steps only) */
    /* BombOutStatus (default on, RECOMP_BOT_BOMB_OUT_STATUS / [Bots] BombOutStatus): status is memset every step and bomb_out was set on the
     * placement frame only, so the brain's b->bomb_out read 0 -- every post-capture bomb stand ran its full 5 s (d5_70: 6 of 6, b9 34.3-39.5
     * with the bomb out from 34.8). Our pack out = bomb_out, every step. */
    if (c->bomb_pack && c7_on("bomboutstatus", "RECOMP_BOT_BOMB_OUT_STATUS", "BombOutStatus", 1)) { b->status.bomb_out = 1; b->status.bomb_at = c->bomb_pos; }
    {   /* How high this body's jets climb: the best gain over a burn begun with a near-full tank (a
         * hop along the ground climbs less, so the figure only grows toward the real one). Decides
         * which drops a tactical move may take: one it can jet back up. */
        uint32_t jai = (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) ? actor_ai(me->actor) : 0;
        if (c->jet_actor != me->actor) { c->jet_actor = me->actor; c->jet_gain = 0.f; c->jl_on = 0; }
        int burning = jai && MEM8(jai + 0x240);
        if (burning && !c->jl_on) { c->jl_on = 1; c->jl_y0 = c->jl_max = me->pos.y; c->jl_fuel0 = MEMF(me->actor + 0x49C); }
        else if (burning) { if (me->pos.y > c->jl_max) c->jl_max = me->pos.y; }
        else if (c->jl_on) {
            float gain = c->jl_max - c->jl_y0;
            c->jl_on = 0;
            if (c->jl_fuel0 >= 1.5f && gain > c->jet_gain && gain < 80.f) {
                c->jet_gain = gain;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-JETGAIN] t=%.1f bot=%d kind=%d jets climb %.1f m (fuel %.2f)\n", w->time, index, me->kind, gain, c->jl_fuel0);
            }
        }
    }
    if (me->actor != c->actor) {                      /* new body (boarded, ejected, respawned) */
        c->xu_hull = me->kind == MA2_KIND_PILOT && c->kind != MA2_KIND_PILOT ? c->actor : 0;   /* ExitUnwedge: the hull just left */
        c->actor = me->actor; c->kind = me->kind; c->have_path = 0; c->engaged = 0; c->tgt = 0;
        c->prog_t = 0.0; c->stalls = 0; c->path_fails = 0; c->actor_since = w->time; c->narea = 0; c->bav_until = 0.0;
        if (navfix_on()) { c->ncrumb = 0; c->crumb_i = 0; c->back_until = 0.0; c->nostart = 0; }   /* push-x3 E8: last life's crumbs are 900 m away */
        /* merge safety pass: a wall climb (x2) belongs to the old body. If the BattleArmor died or was boarded away mid-climb, the next
         * climb_order would call ba_release() on the NEW actor (a pilot or 'Mech: not a BA, the claw routine would run on the wrong object). */
        c->co_phase = 0; c->co_done = 0; c->co_res = 0; c->axis[4] = 0.f; b->status.climb_state = 0;
        if (me->kind != MA2_KIND_PILOT && c->board_t <= 0.0 && c->stat_init) c->board_t = w->time - c->first_t;
        if (me->kind != MA2_KIND_PILOT && c->board_veh == me->actor && env_on("RECOMP_BOT_DIAG"))
            fprintf(stderr, "[BOT-BOARDING] bot=%d kind=%d path=%.0fm ideal=%.1fs to_zone=%.1fs boarded=%.1fs retargets=%u t=%.1f\n", index, me->kind,
                    c->bm_len, c->bm_len > 0.f ? c->bm_len / 9.f : -1.f, c->bm_tzone > 0.0 ? c->bm_tzone - c->bm_t0 : -1.0, w->time - c->bm_t0, c->bm_retargets, w->time);
        if (me->kind != MA2_KIND_PILOT) { c->bm_retargets = 0; c->board_veh = 0; }
    }
    if (!c->stat_init) { c->stat_init = 1; c->first_t = w->time; c->first_pos = me->pos; c->min_sep = 1e9f; }
    if (climb_test(index, b, c, w, me)) { ctrl_send_frame(index, b->player); return; }   /* its stick input goes out too */
    if (!(c->bav_until > 0.0 && c->bav_blast && c->co_phase < 3) && climb_order(index, b, c, w, me, o)) { ctrl_send_frame(index, b->player); return; }   /* not while leaving a death blast (unless latched on) */
    if (jet_show(index, b, c, w, me)) { ctrl_send_frame(index, b->player); return; }
    if (hull_test(index, b, c, w, me)) { ctrl_send_frame(index, b->player); return; }
    c->vt_low_pref = o->vt_low;
    c->vt_alt_force = 0.f;
    if (o->type != MA2_ORD_EJECT) { if (c->eject_t0 > 0.0 && c->vt_lift_demand < 0.f) c->vt_lift_demand = 0.f; c->eject_t0 = 0.0; }   /* EjectSink: not after it */
    pads_note(w);
    {   /* test hook RECOMP_BOT_TEST_TELEPORT="bot,t,dy" (diag only, default off): the game's set-position sub_DD890 (eax = actor, ecx = Vec3*),
         * as the teleport action uses it, on this bot's body at t, raised dy; logs the position then and 0.5 / 2 s later */
        static double tt_at[32]; static int tt_stage[32];
        const char *tt = getenv("RECOMP_BOT_TEST_TELEPORT"); int tb = -1; float tt_t = 0.f, tt_dy = 0.f;
        static int tt_any_done;   /* bot -1: the first bot found in a tank at t */
        if (tt && sscanf(tt, "%d,%f,%f", &tb, &tt_t, &tt_dy) == 3 && index < 32 &&
            (tb == index || (tb < 0 && (tt_stage[index] > 0 || (!tt_any_done && me->kind == MA2_KIND_TANK && w->time >= tt_t))))) {
            if (tt_stage[index] == 0 && w->time >= tt_t) {
                recomp_func_t setpos = resolve(0x000DD890u);
                uint32_t pinfo = MEM32(me->actor + 0x90), comp = MEM32(me->actor + 0x8C);
                tt_any_done = 1;
                if (setpos) {
                    GuestRegs r; regs_save(&r);
                    g_esp -= 32; uint32_t v = g_esp & ~15u;
                    MEMF(v) = me->pos.x; MEMF(v + 4) = me->pos.y + tt_dy; MEMF(v + 8) = me->pos.z;
                    if (pinfo && gva_ok(comp, 0x48) && gva_ok(MEM32(comp + 0x40), 8)) {   /* the TankUnembed component path */
                        recomp_func_t sp2 = resolve(MEM32(MEM32(comp + 0x40) + 4));
                        if (sp2) { g_esp = v - 16; g_ecx = comp + 0x40; PUSH32(g_esp, v); PUSH32(g_esp, 0x000DD8B6u); sp2(); }
                    } else {
                        g_esp = v - 16; g_eax = me->actor; g_ecx = v;
                        PUSH32(g_esp, 0x000DD890u); setpos();
                    }
                    regs_restore(&r);
                }
                fprintf(stderr, "[TEST-TELEPORT] t=%.2f bot=%d kind=%d actor=%08X +90=%08X at %.2f,%.2f,%.2f -> y+%.1f\n", w->time, index, me->kind, me->actor, MEM32(me->actor + 0x90), me->pos.x, me->pos.y, me->pos.z, tt_dy);
                tt_stage[index] = 1; tt_at[index] = w->time;
            } else if ((tt_stage[index] == 1 && w->time >= tt_at[index] + 0.5) || (tt_stage[index] == 2 && w->time >= tt_at[index] + 2.0)) {
                fprintf(stderr, "[TEST-TELEPORT] t=%.2f bot=%d now %.2f,%.2f,%.2f vel %.2f,%.2f,%.2f\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, me->vel.x, me->vel.y, me->vel.z);
                tt_stage[index]++;
            }
        }
    }
    /* ExitUnwedge (default on, RECOMP_BOT_EXIT_UNWEDGE / [Bots] ExitUnwedge): a pilot comes out 2-2.7 m from the centre of the 'Mech it left, inside its
     * 6-6.5 m hull, and stood there: rc10_74b b9 476.4,1052.8 from 71.2 s, not 1 cm in 8 s, then boarded it again; rc10_73a b1, p31_72a b0,
     * rc11_72b b3/b5 (AiKick, FROZEN, BOT-CONTACT sep -5 to -6.5). EjectStep runs inside the travel leg in the first 0.5 s only, so it fired 0-2
     * times a match (0 in rc10_74b), and its one push went the way the legs block. Every tick of its first 4 s on foot, while within 4.5 m of that
     * hull: walk 7 m out, radially first, then +-60, +-120, 180 deg, 0.8 s each. [C7] exitunwedge: tries. */
    /* bal38 (p38_73b b3): the first 2.4 s are the game's own exit hold (no input moves the pilot), so tries made then were wasted, and a
     * try 120 deg off the way out walked it in under the 'Mech (0.9 m from its centre), where nothing moves it: tries start at 2.2 s, run
     * to 7 s, 1 s each, outward only (0, +-45, +-90 deg). */
    if (me->kind == MA2_KIND_PILOT && c->xu_hull && w->time - c->actor_since >= 2.2 && w->time - c->actor_since < 7.0 && c7_on("exitunwedge", "RECOMP_BOT_EXIT_UNWEDGE", "ExitUnwedge", 1)) {
        const Ma2VehicleView *hv = NULL;
        for (int i = 0; i < w->nvehicles; i++) if (w->vehicles[i].actor == c->xu_hull && !w->vehicles[i].occupied) hv = &w->vehicles[i];
        float d = hv ? hypotf(me->pos.x - hv->pos.x, me->pos.z - hv->pos.z) : 1e9f;
        if (c->xu_actor != me->actor) { c->xu_actor = me->actor; c->xu_try = 0; c->xu_until = 0.0; }
        if (hv && d < 4.5f && (w->time < c->xu_until || c->xu_try < 5)) {
            if (w->time >= c->xu_until) {
                static const float off[5] = { 0.f, 0.7854f, -0.7854f, 1.5708f, -1.5708f };
                float a = atan2f(me->pos.x - hv->pos.x, me->pos.z - hv->pos.z) + off[c->xu_try++];
                c->xu_pt = me->pos; c->xu_pt.x += sinf(a) * 7.f; c->xu_pt.z += cosf(a) * 7.f; c->xu_until = w->time + 1.0;
                c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time + 0.8;
                c7_add("exitunwedge", "tries", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-EXITUNWEDGE] t=%.1f bot=%d try %d at %.1f,%.1f, %.1f m from the hull it left: out toward %.0f,%.0f\n", w->time, index, c->xu_try, me->pos.x, me->pos.z, d, c->xu_pt.x, c->xu_pt.z);
            }
            body_walk_direct(me->actor, me->pos, c->xu_pt, 100); c->dest = c->xu_pt; c->mode = CM_MOVE;
            ctrl_send_frame(index, b->player); return;
        }
    }
    /* PilotFace, shuttle case: a pilot pushed back off a face walks to and fro along its foot -- never "stalled" (bal39 p39_73a b2: 502,1242
     * <-> 512,1236 at y 29-31 for 25 s toward a waypoint 511,1252 up the No Man's Land mesa's west face). Its waypoint 2 m+ above it, under
     * 1.5 m gained in 8 s while staying within 12 m: that face is learned for pilots (12 m round the waypoint) and the route replanned. */
    if (me->kind == MA2_KIND_PILOT && c->have_path && c7_on("pilotface", "RECOMP_BOT_PILOT_FACE", "PilotFace", 1)) {
        if (c->pf_t <= 0.0 || w->time < c->pf_t || hdist2(me->pos, c->pf_anchor) > 144.f || me->pos.y > c->pf_anchor.y + 1.5f) { c->pf_anchor = me->pos; c->pf_t = w->time; }
        else if (w->time - c->pf_t > 8.0) {
            int okd = 0; float gd = terrain_height(c->dest.x, c->dest.z, &okd);
            if (okd && gd > me->pos.y + 2.f && hdist2(me->pos, c->dest) < 625.f) {
                ma2_path_pilot_face_add(c->dest.x, c->dest.z, 30.f, w->time + 600.0); c7_add("pilotface", "shuttle", 1);
                c->have_path = 0; c->pq_stale = 1; c->next_plan = w->time;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-NAV] t=%.1f bot=%d pilot 8 s under a face at %.0f,%.0f (waypoint %.0f,%.0f is %.1f m up): pilots plan off it 30 m round\n", w->time, index, me->pos.x, me->pos.z, c->dest.x, c->dest.z, gd - me->pos.y);
            }
            c->pf_anchor = me->pos; c->pf_t = w->time;
        }
    }
    /* TankEmbed (default on, RECOMP_BOT_TANK_UNEMBED / [Bots] TankUnembed): a tank that landed hard nose-in is embedded for good: x/z velocity
     * exactly 0, position fixed to the cm, vertical speed swinging (-4..+75 m/s), pivot and eject refused (On the Rocks 1512-1528,1463-1506 and
     * 1023,58,1357, rc4_65b / rc10_65a / rc14_65a,b / rc14_84a; CTF Bogged Down 873.6,972.1 rc12_60b b9 after a -17.8 m/s drop at 9 m/s).
     * Lifting it does not work: the game's set-position sub_DD890 moved a pilot 4.9 m (RECOMP_BOT_TEST_TELEPORT bal42) but neither of its paths
     * (the PhysicsInfo copy, the body component +0x40 slot 1) moves a tank (bal46: a tank stayed to the cm; rc14 lifts on embedded tanks: no
     * change). Detected after 1.5 s with the vertical speed swinging in a third of the frames (never a tank at rest: rc13 lifted 8 resting
     * tanks before that test): the spot becomes a 6 m obstacle for the rest of the match (EmbedMark) so the next tank plans round it, and the
     * bail that follows marks the hull so nobody boards it again (StuckHull, brain). [C7] tankunembed: detected. */
    if (me->kind == MA2_KIND_TANK && c7_on("tankunembed", "RECOMP_BOT_TANK_UNEMBED", "TankUnembed", 1)) {
        if (c->ue_actor != me->actor) { c->ue_actor = me->actor; c->ue_n = 0; c->ue_t = 0.0; c->ue_last = -1e9; }
        int stuck = hypotf(me->vel.x, me->vel.z) < 0.02f && fabsf(me->vel.y) > 0.5f;
        if (!stuck && !(c->ue_t > 0.0 && hdist2(me->pos, c->ue_pos) < 0.0025f && hypotf(me->vel.x, me->vel.z) < 0.02f)) c->ue_t = 0.0;
        else if (c->ue_t <= 0.0 || hdist2(me->pos, c->ue_pos) > 0.0025f) { c->ue_t = w->time; c->ue_pos = me->pos; c->ue_frames = 0; c->ue_vy = 0; }
        else if ((++c->ue_frames, stuck && ++c->ue_vy, 0)) {}
        else if (w->time - c->ue_t > 1.5 && c->ue_n < 1 && stuck && c->ue_frames >= 10 && c->ue_vy * 3 >= c->ue_frames) {
            if (c7_on("embedmark", "RECOMP_BOT_EMBED_MARK", "EmbedMark", 1)) { side_prop_add(me->pos, 6.0f); turrets_reapply(); c7_add("embedmark", "marked", 1); }
            c->ue_n++; c->ue_last = w->time; c7_add("tankunembed", "detected", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-TANKUNEMBED] t=%.1f bot=%d tank embedded at %.2f,%.2f,%.2f (vel.y %.1f): spot marked for the other tanks\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, me->vel.y);
        }
    }
    /* TankCrest (default on, RECOMP_BOT_TANK_CREST / [Bots] TankCrest): tanks cruise at 18-20 m/s; off a convex break at that speed they fly, and
     * one that lands nose-in on a slope is embedded for good -- x/z velocity exactly 0, position fixed to the cm, vertical speed swinging -4..+75
     * m/s, pivot and eject refused: On the Rocks 1512-1514,1503-1506 (rc10_65a b5 t=167-240: 20.6 m/s at 1530,1492, flat y 63.8 then ~1.9 m
     * under the tangent within 11 m, vel.y -11 at 166.0; rc4_65b b6 the same spot, 8 bails). Nothing is there for the rays (FPROBE: no
     * collider); it is the landing. Ahead along the velocity, where the ground falls away from the current tangent by D at distance d, a
     * body at v drops 4.9 (d/v)^2: v above d sqrt(4.9/D) leaves the ground. Faster than that + 1 m/s: drive on at the safe speed (percent of
     * 20 m/s, heading kept) this tick. [C7] tankcrest: s. */
    if (me->kind == MA2_KIND_TANK && c7_on("tankcrest", "RECOMP_BOT_TANK_CREST", "TankCrest", 1)) {
        float sp = hypotf(me->vel.x, me->vel.z);
        if (sp > 8.f) {
            float dx = me->vel.x / sp, dz = me->vel.z / sp, vmax = 1e9f, dcrest = 0.f;
            int ok0 = 0, ok2 = 0;
            float g0 = terrain_height(me->pos.x, me->pos.z, &ok0), g2 = terrain_height(me->pos.x + dx * 2.f, me->pos.z + dz * 2.f, &ok2);
            if (ok0 && ok2) {
                float s0 = (g2 - g0) / 2.f;
                for (float d = 6.f; d <= 15.f; d += 3.f) {
                    int okd = 0; float gd = terrain_height(me->pos.x + dx * d, me->pos.z + dz * d, &okd);
                    float D = okd ? g0 + s0 * d - gd : 0.f;
                    if (D > 0.6f) { float v = d * sqrtf(4.9f / D); if (v < vmax) { vmax = v; dcrest = d; } }
                }
            }
            if (vmax < 1e8f && sp > vmax + 1.f) {
                /* rc14: sub_2AD5C0 takes a percent of the tank's own top speed, far above its 18-20 m/s cruise: "90 %" drove tanks over the
                 * On the Rocks plateau at 30-53 m/s (rc14_65a b5 49.5, rc14_84a b5 53.2) and off the crest -- 5 stuck tanks in 4 runs, rc10 1.
                 * No forward speed at all: the tank coasts and brakes, heading kept, until it is under the safe speed. */
                int pct = 0;
                Ma2Vec3 ahead = { me->pos.x + dx * 30.f, me->pos.y, me->pos.z + dz * 30.f };
                body_walk_direct(me->actor, me->pos, ahead, pct); c->mode = CM_MOVE;
                c7_add("tankcrest", "s", w->dt);
                if (env_on("RECOMP_BOT_DIAG") && (w->time - c->tc_log > 1.0 || w->time < c->tc_log)) {
                    c->tc_log = w->time;
                    fprintf(stderr, "[BOT-TANKCREST] t=%.1f bot=%d at %.0f,%.0f,%.0f %.1f m/s, crest %.0f m ahead safe at %.1f: %d%%\n", w->time, index, me->pos.x, me->pos.y, me->pos.z, sp, dcrest, vmax, pct);
                }
                ctrl_send_frame(index, b->player); return;
            }
        }
    }
    if (me->kind == MA2_KIND_VTOL && getenv("RECOMP_TEST_VTOL_BUTTONS") && w->time >= atof(getenv("RECOMP_TEST_VTOL_BUTTONS"))) {
        /* test: which command moves a hovering VTOL up or down -- each held 2 s, sticks centred */
        /* 6 s held, 4 s rest (-1); the hover bobs +-1.7 m every 2 s, so short holds drown in it */
        static int cmds[128] = { -1, 12, -1, 13, -1, 14, -1, 15, -1, 28, -1, 29, -1, 30, -1, 31, -1, 42, -1, 46, -1, 47, -1, 48, -1, 50, -1, 11, -1, 35, -1, 36 };
        static int n = 32; static float lys[128];
        { static int parsed; const char *l = getenv("RECOMP_TEST_VTOL_CMDS");   /* "a,b,c": each held 3 s, 2 s rest */
          if (!parsed && l) { parsed = 1; n = 0; for (const char *s = l; *s && n < 126; ) { lys[n] = 0.f; cmds[n++] = -1; lys[n] = 0.6f; cmds[n] = atoi(s);
              while (*s && *s != ',' && *s != ':') s++; if (*s == ':') { lys[n] = (float)atof(s + 1); while (*s && *s != ',') s++; } n++; if (*s) s++; } } }
        const int fwd = getenv("RECOMP_TEST_VTOL_CMDS") != NULL;
        double t0 = atof(getenv("RECOMP_TEST_VTOL_BUTTONS"));
        double tt = w->time - t0; int slot = 0;
        for (double acc = 0.0; slot < n; slot++) { double len = fwd ? (cmds[slot] < 0 ? 2.0 : 3.0) : cmds[slot] < 0 ? 4.0 : 6.0; if (tt < acc + len) break; acc += len; }
        if (slot != c->btn_slot) {
            if (c->btn_slot >= 0 && c->btn_slot < n)
                fprintf(stderr, "[TEST-BTN] bot=%d cmd=%d dy=%+.1f dxz=%.1f\n", index, cmds[c->btn_slot], me->pos.y - c->btn_y, hdist2(me->pos, c->btn_pos));
            c->btn_slot = slot; c->btn_y = me->pos.y; c->btn_pos = me->pos;
        }
        if (slot < n) {
            uint32_t comp = MEM32(me->actor + 0x8C), ai2 = actor_ai(me->actor);
            if (gva_ok(comp, 0x150)) MEM32(comp + 0x144) &= ~1u;
            if (ai2) MEM8(ai2 + 0x90) = 0;
            c->axis[4] = c->axis[5] = c->axis[8] = c->axis[9] = 1e-6f;
            if (fwd) { c->axis[4] = lys[slot] != 0.f ? lys[slot] : 1e-6f; if (!getenv("RECOMP_TEST_VTOL_NOYAW")) c->axis[9] = 0.35f; }   /* "a:ly" = action a held with the stick at ly */
            if (w->time - c->vt_probe_t > 0.5 || w->time < c->vt_probe_t) { c->vt_probe_t = w->time; int gk = 0; float gg = terrain_height(me->pos.x, me->pos.z, &gk);
                fprintf(stderr, "[TEST-BTNY] t=%.1f bot=%d cmd=%d ly=%.1f y=%.1f gnd=%.1f vy=%.1f sp=%.1f\n", w->time, index, cmds[slot], lys[slot], me->pos.y, gg, me->vel.y, hypotf(me->vel.x, me->vel.z)); }
            if (cmds[slot] == 100) ctrl_button(c, BTN_LS, 2); else if (cmds[slot] == 101) ctrl_button(c, BTN_RS, 2); else if (cmds[slot] >= 0 && cmds[slot] < 60) c->press[cmds[slot]] = 2;   /* 100/101: a whole stick click */
            ctrl_send_frame(index, b->player);
            return;
        }
    }
    if (c->hz_in && !(me->kind == MA2_KIND_VTOL && o->vt_heal == 1 && hdist2(me->pos, o->point) < 70.f)) vtpad_close(c, me);
    /* VtolPadKick (default on, RECOMP_BOT_VTOL_PAD_KICK / [Bots] VtolPadKick): a craft that comes to the pad run at cruise and is braked to a stop
     * (the reversed stick) hangs at 0-1 m/s with the stick full forward: its thrust ramps with its speed (0.5 -> 5 -> 23 m/s in 0.4 s once it
     * stirs). Sweep rerun swA63b 11-02: bot 5 hovered 27 s, 68 m from the pad at 11 m up (the same crawl as the older "full throttle, < 4 m/s"
     * spans). Under 2.5 m/s for 1.2 s with the pad 18 m+ off, the game's MoveTo flies it 3.5 s (the drive's own window) to get it moving again;
     * the stick takes over on the way in. [C7] vtpad: kicks. */
    int pad_kick = 0;
    if (me->kind == MA2_KIND_VTOL && o->vt_heal == 1 && vt_padkick_on()) {
        float ddk = hdist2(me->pos, o->point), spk = hypotf(me->vel.x, me->vel.z);
        if (ddk < 70.f && w->time < c->pk_until && ddk > 14.f) { pad_kick = 1; c->vt_moveto_until = c->pk_until; }
        else if (ddk < 70.f && spk < 2.5f && ddk > 18.f) {
            if (c->pk_t <= 0.0 || w->time < c->pk_t) c->pk_t = w->time;
            else if (w->time - c->pk_t > 1.2) {
                c->pk_until = w->time + 3.5; c->pk_t = 0.0; c->vt_moveto_until = c->pk_until; pad_kick = 1; c7_add("vtpad", "kicks", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-REPAIR] t=%.1f bot=%d hanging at %.1f m/s, %.0f m off the pad: MoveTo for 3.5 s\n", w->time, index, spk, ddk);
            }
        } else c->pk_t = 0.0;
    }
    if (me->kind == MA2_KIND_VTOL && o->vt_heal && !pad_kick && hdist2(me->pos, o->point) < (o->vt_heal == 1 ? 70.f : 35.f)) {
        const int fastpad = o->vt_heal == 1 && vt_fastpad_on();            /* VtolFastPad: descend while closing in, no careful line-up */
        c->va_branch = 'P';
        if (o->vt_heal == 1) {
            if (!c->hz_in) { c->hz_in = 1; c->hz_t0 = w->time; c->hz_a_t = 0.0; c->hz_hp0 = me->hp; c->hz_hp_full = 0; c->hz_full_t = 0.0; c->heal_tap_t = 0.0; }
            c->hz_last = w->time;
            if (!c->hz_hp_full && me->hp_max > 0.f && me->hp >= 0.97f * me->hp_max) { c->hz_hp_full = 1; c->hz_full_t = w->time; }
        }
        /* Last stretch of a repair run: down low (descend held, the stick down), then on the pad hold
         * still and press A for the repair prompt; at a health pack skim straight through it. */
        int okg = 0, oks = 0;
        float g = terrain_height(o->point.x, o->point.z, &okg), sf = surface_height(o->point.x, o->point.z, &oks);
        float top = oks ? maxf2(sf, okg ? g : sf) : (okg ? g : o->point.y);
        float dd = hdist2(me->pos, o->point);
        /* Over the spot, all the way down first (A would lift it again in this control scheme), then on
         * the pad the prompt (Space / A); onto a pack it just settles -- low over it is enough. */
        /* Heights from the terrain alone: a surface probe straight down from above the spot met the
         * VTOL's own hull once it hovered there, and the bot climbed to stay "above" itself. */
        float agl = okg ? me->pos.y - g : me->pos.y - top;
        {   /* ...and from what is really under it: a raised pad, a roof, a tank parked on the pad -- held
             * Y lowers it only down to the height a pilot gets out at, so it must never be judged
             * higher than it is (a ray straight down from just under the hull) */
            void ma2_bot_ray_below(Ma2Vec3, float, float *, uint32_t *, uint32_t *);
            Ma2Vec3 f = me->pos; f.y -= 2.5f; float t; uint32_t ow, cl;
            ma2_bot_ray_below(f, 60.f, &t, &ow, &cl);
            if (t >= 0.f && t + 2.5f < agl) agl = t + 2.5f;
        }
        /* Run in at its height, nose on the spot, braking as it closes -- it came in at cruise speed
         * and overshot the pad by 30 m, round and round (an Elite at 33 m never got down); inside
         * 12 m settle (latched until it drifts past 25 m): nose held still. Both are the forward and
         * sideways stick onto the spot with the drift damped out, gentler once settled. */
        if (dd < 12.f) c->settle = 1; else if (dd > 25.f) { if (c->settle) c7_add("vtpad", "overshoot_n", 1); c->settle = 0; }
        /* VtolFastPad: the glide down to the pad -- 6 m over it at the pad, 0.25 m more per metre out -- flown while the
         * stick closes in (the old run held its height until it was within 12 m, then sank). */
        float glide = 6.2f + maxf2(0.f, dd - 6.f) * 0.25f;
        if (o->vt_heal == 1 && env_on("RECOMP_BOT_PADTRACE") && w->time - c->hz_tr_t >= 0.25) {   /* test hook: the pad run, 4 lines a second */
            c->hz_tr_t = w->time;
            fprintf(stderr, "[PADTR] t=%.2f bot=%d fast=%d dd=%.1f agl=%.1f sp=%.1f vy=%.1f hp=%.0f settle=%d\n", w->time, index, fastpad, dd, agl, hypotf(me->vel.x, me->vel.z), me->vel.y, me->hp, c->settle);
        }
        {
            uint32_t node = MEM32(me->actor + 0x94);
            Ma2Vec3 ahead = me->pos; float fx = 0.f, fz = 1.f;
            if (gva_ok(node, 0x100)) { fx = MEMF(node + 0xE0); fz = MEMF(node + 0xE8); ahead.x += fx * 10.f; ahead.z += fz * 10.f; }
            if (!c->settle) vtol_stick(index, c, w, me, o->point, fastpad ? minf2(me->pos.y, me->pos.y - agl + glide) : me->pos.y, 0.f);
            else vtol_stick(index, c, w, me, ahead, me->pos.y - 20.f, 0.f);
            /* VtolPadLift (default on, RECOMP_BOT_VTOL_PAD_LIFT / [Bots] VtolPadLift): players want the repair run to heal. The sweep (WP2 11-02: sw25 sw63 sw70 sw73
             * sw81 sw85 sw93, vtpad runs 16 / aborted 16 / healed 0) shows why it never did: Y (the sink, comp+0x90 = -30) takes the craft down, vtol_manual's ModClimb
             * check (y + 0.6 vy under the route's surface need, 27 m at the pad) answers with the +30 climb demand, and this branch returns before the drive
             * step that clears it each step -- so the +30 stayed latched, the craft shot up to 60-100 m and hung there with the stick full forward. The pad
             * run sets its own height (the glide, Y), so the route's climb is off here and the demand is cleared every step. [C7] vtpad: lift_cleared. */
            if (vt_padlift_on() && c->vt_lift_demand != 0.f) { c->vt_lift_demand = 0.f; c->vt_mclimb = 0; c7_add("vtpad", "lift_cleared", 1); }
            float dx = o->point.x - me->pos.x, dz = o->point.z - me->pos.z;
            float fwd = dx * fx + dz * fz, lat = dx * fz - dz * fx;          /* metres ahead / to the right */
            float vf = me->vel.x * fx + me->vel.z * fz, vl = me->vel.x * fz - me->vel.z * fx;
            float kp = c->settle ? 0.06f : 0.025f, kd = c->settle ? 0.12f : 0.08f, lim = c->settle ? 0.4f : 0.7f;
            float flim = lim;
            if (fastpad) { kp = c->settle ? 0.06f : 0.045f; kd = 0.12f; flim = c->settle ? 0.4f : 1.0f; lim = c->settle ? 0.4f : 0.7f; }
            float fstick = fwd * kp - vf * kd;
            if (fastpad) {   /* the run in is a braking curve, not a spring: fast out there (<= 26 m/s), 3.6 m/s at 6 m, the stick holding that speed */
                float af = fabsf(fwd), tv = af < 6.f ? 0.6f * af : minf2(26.f, 3.6f + sqrtf(24.f * (af - 6.f)));
                fstick = ((fwd < 0.f ? -tv : tv) - vf) * 0.12f;
                if (vt_padkick_on() && fwd > 0.f && vf > 0.f && fstick < -0.4f) fstick = -0.4f;   /* VtolPadKick: a hard reverse stopped it dead (and a craft at rest hardly accelerates) */
                if (c->settle) flim = lim = 0.5f; else flim = lim = 1.0f;
            }
            c->axis[4] = clampf2(fstick, -lim, flim); if (c->axis[4] == 0.f) c->axis[4] = 1e-6f;
            c->axis[8] = clampf2(lat * kp - vl * kd, -lim, lim); if (c->axis[8] == 0.f) c->axis[8] = 1e-6f;
        }
        /* Setting down on the pad: the stick and the descend control only sink a VTOL to its hover
         * floor (9-14 m, and it stayed at 33 m over the pad); the exit control (Y) lowers it while it
         * is held -- a press of it dropped it 3-4 m -- and at the bottom (~2 m) gets the pilot out
         * (the eject runs: 33 m, exit, 30, 27 ... 2 m, out). So Y is held over the pad only down to
         * 6.5 m -- well clear of getting out -- and it sinks on into the pad's zone (the prompt
         * answered A at 5.6-6.0 m), where A repairs it. Onto a health pack or a missile upgrade the
         * same, down to 5 m -- it has to fly through it (hovering over one at 18 m on the stick alone
         * it never got it). */
        if (fastpad ? (agl > 6.5f && (agl > glide + 1.0f || dd < 10.f) && !c->hz_hp_full) : (c->settle && (o->vt_heal == 1 ? dd < 10.f && agl > 6.5f : dd < 6.f && agl > 5.f))) {
            ctrl_button(c, BTN_Y, 2);                                          /* lower (Q / Y) */
            if (c->pad_land_t <= 0.0 && env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-REPAIR] bot=%d setting down on the %s from %.1f m, d=%.1f t=%.1f\n", index, o->vt_heal == 1 ? "pad" : "pick-up", agl, dd, w->time);
            c->pad_land_t = w->time;
        } else if (!c->settle) c->pad_land_t = 0.0;
        if (o->vt_heal == 1 && c->settle && dd < 12.f && agl < 6.5f && w->time - c->heal_tap_t > 0.7) {
            c->heal_tap_t = w->time; ctrl_button(c, BTN_A, 3);                     /* the pad's prompt (Space / A) */
            if (c->hz_a_t <= 0.0) c->hz_a_t = w->time;
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-REPAIR] bot=%d A on the pad at %.1f m up, d=%.1f hp=%.0f t=%.1f\n", index, agl, dd, me->hp, w->time);
        }
        if (env_on("RECOMP_BOT_DIAG") && w->time - c->heal_log_t > 1.0) {
            c->heal_log_t = w->time;
            uint32_t bh = MEM32(b->player + 0x2F0);
            {   /* what it rests on: rays down from under the hull and from 6 m to each side */
                void ma2_bot_ray_below(Ma2Vec3, float, float *, uint32_t *, uint32_t *);
                static const float offs[3][2] = { { 0.f, 0.f }, { 6.f, 0.f }, { 0.f, 6.f } };
                char rb[160]; int ro = 0;
                for (int k = 0; k < 3; k++) {
                    Ma2Vec3 f = me->pos; f.x += offs[k][0]; f.z += offs[k][1]; f.y -= 2.5f;
                    float t; uint32_t ow, cl; ma2_bot_ray_below(f, 60.f, &t, &ow, &cl);
                    ro += snprintf(rb + ro, sizeof rb - (size_t)ro, " [%.1f %08X %08X]", t, ow, cl);
                }
                fprintf(stderr, "[BOT-REPAIR] bot=%d y=%.1f g=%.1f below:%s\n", index, me->pos.y, okg ? g : -999.f, rb);
            }
            fprintf(stderr, "[BOT-REPAIR] bot=%d %s d=%.1f up=%.1f hp=%.0f/%.0f scheme=%u e4=%u cf=%u ba=%u bb=%u v18=%.2f v3=%.2f v41=%.2f t=%.1f\n", index, o->vt_heal == 1 ? "pad" : "pack", dd,
                    agl, me->hp, me->hp_max, MEM32(0x004B296Cu), gva_ok(bh, 0x6F0) ? MEM8(bh + 0x6E4) : 99u, gva_ok(bh, 0x6F0) ? MEM8(bh + 0x6CF) : 99u, gva_ok(bh, 0x6F0) ? MEM8(bh + 0x6BA) : 99u, gva_ok(bh, 0x6F0) ? MEM8(bh + 0x6AB) : 99u, gva_ok(bh, 0x6F0) ? MEMF(bh + 0x214) : -1.f, gva_ok(bh, 0x6F0) ? MEMF(bh + 0x70) : -1.f, gva_ok(bh, 0x6F0) ? MEMF(bh + 0x460) : -1.f, w->time);
        }
        if (o->target && !c->settle) ctrl_fire(c, w, me, o->target);   /* settling: hands off (the AI fire took the flight back and held it up) */
        ctrl_send_frame(index, b->player);
        return;
    }
    if (me->kind == MA2_KIND_VTOL && getenv("RECOMP_TEST_VTOL_FLYTHRU") && w->time >= atof(getenv("RECOMP_TEST_VTOL_FLYTHRU"))) {
        int best = -1; float bd = 1e9f;
        for (int i = 0; i < w->npickups; i++) {
            if (w->pickups[i].kind != MA2_PICKUP_POWERUP) continue;
            float d = hdist2(w->pickups[i].pos, me->pos);
            if (w->pickups[i].entity == c->ft_pu) { best = i; break; }
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) {
            const Ma2PickupView *pu = &w->pickups[best];
            if (pu->entity != c->ft_pu) { c->ft_pu = pu->entity; c->ft_side = 1; c->ft_t = w->time;
                fprintf(stderr, "[TEST-FLYTHRU] bot=%d power-up %08X at %.0f,%.0f,%.0f armor=%d\n", index, pu->entity, pu->pos.x, pu->pos.y, pu->pos.z, pu->armor); }
            Ma2Vec3 end = pu->pos; end.x += 60.f * (float)c->ft_side;
            if (hdist2(me->pos, end) < 12.f) { c->ft_side = -c->ft_side; end = pu->pos; end.x += 60.f * (float)c->ft_side; }
            float dpu = hdist2(me->pos, pu->pos);
            c->vt_alt_force = pu->pos.y + (dpu < 40.f ? 1.5f : 8.f);
            if (env_on("RECOMP_BOT_DIAG") && dpu < 6.f && w->time - c->ft_t > 0.5) {
                c->ft_t = w->time;
                fprintf(stderr, "[TEST-FLYTHRU] bot=%d over it: d=%.1f dy=%.1f speed=%.1f cargo=%u\n", index, dpu, me->pos.y - pu->pos.y,
                        hypotf(me->vel.x, me->vel.z), MEM32(me->actor + 0x57C));
            }
            vtol_stick(index, c, w, me, end, pu->pos.y + 1.5f, dpu > 70.f ? 0.6f : 1.f);
            ctrl_send_frame(index, b->player);
            return;
        }
    }
    /* Alpha Strike: the player's trigger (action 18), held through the charge with the torso on the
     * target; the brain lets go once the charge is full and the shot is clear. The release frames
     * fire it, so the trigger step keeps running half a second after. */
    charge_trace(0, c, w, me);
    if (index >= 0 && index < 16 && s_chg_g[index] && c->trig_act == 18 && w->time >= s_chg_t[index] && w->time - s_chg_t[index] < 2.5)
        c->press[18] = 2;                                                    /* ChargeFire: a paused charge stays held on the player side too */
    if (!c->alpha_hold && c->trig_act == 18 && c->press[18] == 0 && c->val[18] == 0.f) c->trig_act = 0;   /* ChargeFire: the held trigger was let go and sent */
    if (me->kind == MA2_KIND_MECH && (o->alpha || c->alpha_hold)) {
        if (o->alpha && o->target) {
            if (!c->alpha_hold) c->alpha_charges++;
            c->alpha_hold = 1; c->press[18] = 2; c->trig_act = 18; c->alpha_rel_t = w->time;
            uint32_t ai = actor_ai(me->actor);
            recomp_func_t aim = resolve(0x0018AFDBu);
            if (ai && aim && gva_ok(o->target, 0x100)) { GuestRegs r; regs_save(&r); g_eax = ai; PUSH32(g_esp, o->target); PUSH32(g_esp, 0x001B44D9u); aim(); regs_restore(&r); }
        } else if (w->time - c->alpha_rel_t > 0.5) { c->alpha_hold = 0; c->trig_act = 0; }
    }
    if (me->kind == MA2_KIND_VTOL) { vt_edge_measure(index, c, w, me, o); vt_arrive_measure(index, c, w, me, o); }   /* VtolEdgeAll: near the edge, how high (switch on or off) */
    if (me->kind == MA2_KIND_VTOL && env_on("RECOMP_BOT_DIAG")) {
        int okg = 0; float g = terrain_height(me->pos.x, me->pos.z, &okg);
        if (okg && me->pos.y - g > 4.f) {
            int oks = 0; float sf = surface_height(me->pos.x, me->pos.z, &oks);
            float over = me->pos.y - (oks && sf > g ? sf : g);
            c->vt_agl_sum += over; c->vt_agl_n += 1.0; if (over < 14.f) c->vt_low_n += 1.0;
            if (!c->vt_rider) {   /* push-y VtolThreatAlt: height over the surface (roofs count) while threatened vs clear, by pilot skill */
                static const char *const nt[4] = { "sfc_t_thr_recruit", "sfc_t_thr_regular", "sfc_t_thr_veteran", "sfc_t_thr_elite" }, *const nc[4] = { "sfc_t_clr_recruit", "sfc_t_clr_regular", "sfc_t_clr_veteran", "sfc_t_clr_elite" };
                static const char *const ht[4] = { "sfc_h_thr_recruit", "sfc_h_thr_regular", "sfc_h_thr_veteran", "sfc_h_thr_elite" }, *const hc[4] = { "sfc_h_clr_recruit", "sfc_h_clr_regular", "sfc_h_clr_veteran", "sfc_h_clr_elite" };
                int k4 = o->vt_sk & 3;
                c7_add("vtthreat", o->vt_thr ? nt[k4] : nc[k4], w->dt); c7_add("vtthreat", o->vt_thr ? ht[k4] : hc[k4], (double)over * w->dt);
            }
            c->vt_alt_err += me->pos.y - c->vt_alt;
        }
        if (w->time - c->vt_stat_t >= 20.0) {
            c->vt_stat_t = w->time;
            if (c->vt_agl_n > 0.0)
                fprintf(stderr, "[BOT-VTOLSTAT] t=%.0f bot=%d over_sfc=%.1f low_frac=%.2f above_want=%.1f low_pref=%d dodges=%u hides=%d/%d passes=%d\n", w->time, index,
                        c->vt_agl_sum / c->vt_agl_n, c->vt_low_n / c->vt_agl_n, c->vt_alt_err / c->vt_agl_n, o->vt_low, c->dodges, c->vt_hides, c->vt_hide_tries, c->vt_passes);
        }
    }
    if (o->dodge_now && me->kind == MA2_KIND_VTOL && w->time >= c->vdodge_until + 0.5) {
        c->vdodge_until = w->time + 0.9; c->vdodge_dx = o->dodge_dx; c->vdodge_dz = o->dodge_dz; c->dodges++; c->dodge_t = w->time;
        if (vt_edge_all_on() && ef_ready()) {                                  /* VtolEdgeAll: not sideways into the wall */
            float ep = ef_dist(me->pos.x + c->vdodge_dx * 20.f, me->pos.z + c->vdodge_dz * 20.f), en = ef_dist(me->pos.x - c->vdodge_dx * 20.f, me->pos.z - c->vdodge_dz * 20.f);
            if (ep < 25.f && en > ep) { c->vdodge_dx = -c->vdodge_dx; c->vdodge_dz = -c->vdodge_dz; c7_add("vtedgeall", "dodge_flip", 1); }
        }
        c->vdodge_pos = me->pos; c->vdodge_logged = 0;
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DODGE] bot=%d VTOL strafe %.2f,%.2f t=%.1f\n", index, o->dodge_dx, o->dodge_dz, w->time);
    }
    if (me->kind == MA2_KIND_VTOL && w->time >= c->vdodge_until && c->vdodge_until > 0.0 && !c->vdodge_logged) {
        c->vdodge_logged = 1;
        if (env_on("RECOMP_BOT_DIAG"))
            fprintf(stderr, "[BOT-DODGE] bot=%d VTOL strafe done: %.1f m out of the line (%.1f m total)\n", index,
                    (me->pos.x - c->vdodge_pos.x) * c->vdodge_dx + (me->pos.z - c->vdodge_pos.z) * c->vdodge_dz, hdist2(me->pos, c->vdodge_pos));
    }
    if (me->kind == MA2_KIND_VTOL && w->time < c->vdodge_until && me->pos.y > 4.f) {
        /* strafe out of the missile's line on the stick (lateral thrust), keep the nose where it is */
        uint32_t comp = MEM32(me->actor + 0x8C), node = MEM32(me->actor + 0x94);
        if (gva_ok(comp, 0x150) && gva_ok(node, 0x100)) {
            float fx = MEMF(node + 0xE0), fz = MEMF(node + 0xE8), rxv = fz, rzv = -fx;   /* right of the nose */
            float lat = c->vdodge_dx * rxv + c->vdodge_dz * rzv;
            vtol_stick_take(me->actor, comp);
            uint32_t ai = actor_ai(me->actor); if (ai) MEM8(ai + 0x90) = 0;
            c->axis[8] = lat >= 0.f ? 1.f : -1.f; c->axis[4] = 1e-6f; c->axis[9] = 1e-6f;
            if (o->target) ctrl_fire(c, w, me, o->target);
            ctrl_send_frame(index, b->player);       /* the strafe is stick input: send it (a bare return dropped it) */
            return;
        }
    }
    /* A jet burn ends before the tank runs dry. StartJump's flight keeps the jet lit for its whole
     * duration; with the tank empty the jet fires and cuts out frame after frame on the trickle of
     * recharge, and its sound restarts every frame (a stutter, like a key tapped 60 times a
     * second). Ending the jump here lets the flight put the jet out once, cleanly. */
    if (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) {
        uint32_t ai = actor_ai(me->actor);
        if (ai && MEM8(ai + 0x240) && MEMF(me->actor + 0x49C) < 0.1f) MEM8(ai + 0x240) = 0;
        /* No jump running, jet still lit: the flight step that puts it out only runs under MoveTo,
         * not while the body stands (mode 7), so the jet burned on dry and stuttered. Put it out
         * here the way the game's own resets do (actor+0x268 bit15, thrust +0x4E4/+0x4E8). */
        if (ai && !MEM8(ai + 0x240) && (MEM32(me->actor + 0x268) & 0x8000u)) {
            MEM32(me->actor + 0x268) &= ~0x8000u; MEMF(me->actor + 0x4E4) = 0.f; MEMF(me->actor + 0x4E8) = 0.f;
        }
        /* While the jet burns, hold the jump command (action 3, record +0x6C of the behaviour) like a
         * player holding the button. The 'Mech's thrust update (sub_104DA1) runs twice a frame -- once
         * for the jet the AI is flying (held) and once for the player's input (sub_1B0409) -- and with
         * the button up the second call put the jet out again: the jet's start and stop events fired
         * every frame and its sound restarted each time (the "rapid tapping"). */
        if (ai && MEM8(ai + 0x240) && (MEM32(me->actor + 0x268) & 0x8000u) && MEMF(me->actor + 0x49C) >= 0.1f) c->press[3] = 2;
    }
    {
        float dd = hdist2(me->pos, c->first_pos);
        if (dd > c->max_dist) c->max_dist = dd;
        if (w->time - c->first_t <= 63.0 && dd > c->max_dist63) c->max_dist63 = dd;
    }
    uint32_t ai = actor_ai(me->actor);
    /* The target slot is ours: never leave a dead or foreign actor in it. */
    if (ai) {
        uint32_t s = MEM32(ai + 0xB4);
        if (s && (s != c->tgt || !c->engaged || !ctrl_target_alive(w, s, NULL))) { MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0; }
    }
    int drivable = me->kind == MA2_KIND_PILOT || me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK || me->kind == MA2_KIND_BATTLEARMOR ||
                   (me->kind == MA2_KIND_VTOL && ma2_brain_kind_allowed(MA2_KIND_VTOL, w->game_type));
    int was_engaged = c->engaged;
    c->engaged = 0;
    c->ff_skip = o->ff_skip; c->ff_rider = o->ff_rider;
    /* DropLatch (default on, RECOMP_BOT_DROP_LATCH / [Bots] DropLatch): a rider that decided to let go keeps letting go for 3 s whatever
     * its order turns into. The drop was decided in the ride order (IDLE) and the release only ran there; the flag-run grab goal took
     * over 0.4 s later (a TRAVEL to the flag), so the BattleArmor hung on 7 s while its VTOL flew on at up to 94 m/s and fell 85 m past
     * the flag (au3_64 bots 0/4, t=35.7-42.9). [C7] droplatch: releases. */
    {
        static double s_drop_until[16];
        if (index >= 0 && index < 16 && c7_on("droplatch", "RECOMP_BOT_DROP_LATCH", "DropLatch", 1)) {
            if (o->detach && me->riding) s_drop_until[index] = w->time + 3.0;
            else if (me->riding && me->kind == MA2_KIND_BATTLEARMOR && o->type != MA2_ORD_IDLE && w->time < s_drop_until[index] && w->time + 4.0 > s_drop_until[index] &&
                     (w->time >= c->release_next || w->time + 2.0 < c->release_next)) {
                ba_release(me->actor); c->release_next = w->time + 1.0; c7_add("droplatch", "releases", 1);
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-LIFT] bot=%d lets go of %08X at %.0f,%.0f,%.0f t=%.1f (the drop it decided, order now %d)\n", index, me->riding, me->pos.x, me->pos.y, me->pos.z, w->time, o->type);
            }
        }
    }
    /* BlastBrake (default on, RECOMP_BOT_BLAST_BRAKE / [Bots] BlastBrake): BlastWary stopped ORDERING a bot closer to a 'Mech about to blow up, but
     * the momentum it already had still carried it in. r3_213 (TD Witch's Cauldron) b1: a jet hop at ~16 m/s (jet on 145.8-148.4, forward demand 0)
     * flew it from 51 m to 17 m of b9's dying 'Mech under the 'keeps clear' band; the blast took 3130 of 3929. b5 walked 53 -> 37 m along a route
     * whose end was clear of it. The brain hands over the dying 'Mech (order keep_pt / keep_r); closing on it fast enough to be inside keep_r within
     * 1.5 s, a jet burn is turned to point away (its lift kept) and the body steers out to keep_r + 10 m, held 1 s after the closing stops; it
     * keeps shooting. No hop or dodge is started toward it meanwhile. [C7] blastbrake: jet_turned / walked_out / s / hops_skipped. */
    /* Churn (default on, RECOMP_BOT_CHURN / [Bots] Churn; playtest 11-05 a tank stuck on rocks in a low valley): b3's tank
     * drove 5-23 m/s for 40 s between 809,1002 and 820,1014, short of a route point at 791,1027 it could not climb to (a playtester's TD game 11-05,
     * 52-92 s). The 12 s shuttle test starts over whenever the order leaves TRAVEL (engage flickers did) and one stall's re-plan kept the
     * same route. Here: travel time only (an engage pauses the count; anything else resets it), a sample every 2 s; 10 samples all within
     * 20 m of their middle after 70 m+ driven, the order's point 50 m+ off -- the route point we were making for is marked blocked (8 m,
     * 60 s) and the route is planned again, counted as a stall. Ground bodies only. [C7] churn: n. */
    if (drivable && index >= 0 && index < 16 && !me->riding && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_TANK || me->kind == MA2_KIND_BATTLEARMOR) &&
        c7_on("churn", "RECOMP_BOT_CHURN", "Churn", 1)) {
        static Ma2Vec3 ch_p[16][10], ch_last[16]; static int ch_n[16], ch_i[16]; static double ch_t[16]; static float ch_len[16];
        int trav = (o->type == MA2_ORD_TRAVEL || o->type == MA2_ORD_FIGHT_MOVE) && hdist2(me->pos, o->point) > 50.f;
        if (w->time < ch_t[index]) { ch_n[index] = 0; ch_len[index] = 0.f; ch_t[index] = w->time; }   /* a new match: no samples from the last one */
        if (!trav && o->type != MA2_ORD_ENGAGE) { ch_n[index] = 0; ch_len[index] = 0.f; ch_t[index] = w->time; }
        else if (!trav) ch_last[index] = me->pos;                          /* engage: paused, no distance counted */
        else {
            if (ch_n[index] == 0 && ch_len[index] == 0.f) ch_last[index] = me->pos;
            ch_len[index] += hdist2(me->pos, ch_last[index]); ch_last[index] = me->pos;
            if (w->time - ch_t[index] >= 2.0 || w->time < ch_t[index]) {
                static float ch_cum[16][10];
                ch_t[index] = w->time; ch_p[index][ch_i[index]] = me->pos; ch_cum[index][ch_i[index]] = ch_len[index];
                ch_i[index] = (ch_i[index] + 1) % 10; if (ch_n[index] < 10) ch_n[index]++;
                if (ch_n[index] == 10 && ch_len[index] - ch_cum[index][ch_i[index]] > 70.f) {   /* (ch_i now indexes the oldest sample) */
                    float mx = 0.f, mz = 0.f, rmax = 0.f;
                    for (int i = 0; i < 10; i++) { mx += ch_p[index][i].x / 10.f; mz += ch_p[index][i].z / 10.f; }
                    for (int i = 0; i < 10; i++) rmax = maxf2(rmax, hypotf(ch_p[index][i].x - mx, ch_p[index][i].z - mz));
                    if (rmax < 20.f) {
                        Ma2Vec3 bp = c->dest;
                        if (hdist2(bp, me->pos) < 6.f || hdist2(bp, o->point) < 15.f) {   /* the carrot sits on us / is the goal itself: a point 12 m on toward it */
                            float l = hdist2(o->point, me->pos);
                            bp = me->pos; if (l > 0.1f) { bp.x += (o->point.x - me->pos.x) / l * 12.f; bp.z += (o->point.z - me->pos.z) / l * 12.f; }
                        }
                        if (ma2_trav_ready()) ma2_trav_learn(bp, 8.0f, w->time, 60.0);
                        c->have_path = 0; c->pq_stale = 1; c->next_plan = 0.0;
                        c->stalls = hdist2(me->pos, c->stall_pos) < 10.f ? c->stalls + 1 : 1; c->n_stalls++; c->stall_pos = me->pos;   /* as a stall (same-spot count) */
                        if (c->stalls >= 3) b->status.stalled = 1;
                        c7_add("churn", "n", 1);
                        tank_orbit_pivot(c, w, me, c->dest);
                        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CHURN] t=%.1f bot=%d kind=%d %.0f m driven in a %.0f m patch at %.0f,%.0f: blocked %.0f,%.0f, re-plan\n",
                                                               w->time, index, me->kind, ch_len[index] - ch_cum[index][ch_i[index]], rmax, mx, mz, bp.x, bp.z);
                        ch_n[index] = 0; ch_len[index] = 0.f;
                    }
                }
            }
        }
    }
    int braking = 0;
    if (drivable && o->keep_r > 0.f && index >= 0 && index < 16 && !me->riding &&
        (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR || me->kind == MA2_KIND_TANK) && c7_on("blastbrake", "RECOMP_BOT_BLAST_BRAKE", "BlastBrake", 1)) {
        static double s_bb_until[16]; static Ma2Vec3 s_bb_pt[16];
        float ux = me->pos.x - o->keep_pt.x, uz = me->pos.z - o->keep_pt.z, dk = hypotf(ux, uz);
        if (dk > 0.5f) { ux /= dk; uz /= dk; } else { ux = sinf(me->yaw + 3.14159f); uz = cosf(me->yaw + 3.14159f); dk = 0.5f; }
        float closing = -(me->vel.x * ux + me->vel.z * uz);
        if (w->time > s_bb_until[index] + 5.0 || w->time + 2.0 < s_bb_until[index]) s_bb_until[index] = 0.0;
        if (closing > 3.f && dk - 1.5f * closing < o->keep_r && dk < o->keep_r + 40.f) {
            int fresh = w->time >= s_bb_until[index];
            s_bb_until[index] = w->time + 1.0;
            s_bb_pt[index] = me->pos; s_bb_pt[index].x = o->keep_pt.x + ux * (o->keep_r + 10.f); s_bb_pt[index].z = o->keep_pt.z + uz * (o->keep_r + 10.f);
            /* TankBrakeSwerve (default on, RECOMP_BOT_TANK_BRAKE_SWERVE / [Bots] TankBrakeSwerve): straight back out is a reverse a tank cannot
             * make -- it stopped and its turn radius held it (comp+0xD4): d5_211 b1, full hp, stood 1.6 s of the 2.2 s fuse 42 m from the dying
             * b0 (keep 45 m) and died. A tank swerves out on the side it is already moving: 30 m along the tangent, 15 m outward. */
            if (me->kind == MA2_KIND_TANK && c7_on("tankbrakeswerve", "RECOMP_BOT_TANK_BRAKE_SWERVE", "TankBrakeSwerve", 1)) {
                float tx = -uz, tz = ux;
                if (tx * me->vel.x + tz * me->vel.z < 0.f) { tx = -tx; tz = -tz; }
                s_bb_pt[index].x = me->pos.x + tx * 30.f + ux * 15.f; s_bb_pt[index].z = me->pos.z + tz * 30.f + uz * 15.f;
                if (fresh) c7_add("blastbrake", "tank_swerves", 1);
            }
            if (ai && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && MEM8(ai + 0x240)) {
                float jx = MEMF(ai + 0x244), jz = MEMF(ai + 0x24C), hl = hypotf(jx, jz);
                if (hl > 1e-3f && jx * ux + jz * uz < 0.5f * hl) { MEMF(ai + 0x244) = ux * hl; MEMF(ai + 0x24C) = uz * hl; if (fresh) c7_add("blastbrake", "jet_turned", 1); }
            } else if (fresh) c7_add("blastbrake", "walked_out", 1);
            if (fresh && env_on("RECOMP_BOT_DIAG"))
                fprintf(stderr, "[BOT-BLASTBRAKE] t=%.1f bot=%d kind=%d %.0f m from a dying 'Mech, closing at %.1f m/s (keep %.0f m)%s: out to %.0f,%.0f\n", w->time, index, me->kind,
                        dk, closing, o->keep_r, ai && MEM8(ai + 0x240) ? ", jet turned" : "", s_bb_pt[index].x, s_bb_pt[index].z);
        }
        if (w->time < s_bb_until[index] && hdist2(me->pos, s_bb_pt[index]) > 4.f) {
            if (ai && was_engaged) ctrl_release_attack(c, ai);
            body_steer(me, s_bb_pt[index]); c->dest = s_bb_pt[index]; c->mode = CM_MOVE;
            if (o->target && ctrl_can_fight(me->kind)) ctrl_fire(c, w, me, o->target);
            c7_add("blastbrake", "s", w->dt);
            braking = 1;
        }
    }
    if (braking) { }
    else if (!drivable) {
        if (ai && was_engaged) ctrl_release_attack(c, ai);
        if (o->type == MA2_ORD_EJECT) ctrl_tap(c, 39, 3);   /* turrets & co: no native movement; only the exit */
    } else switch (o->type) {
    case MA2_ORD_ENGAGE:
        if (!ctrl_can_fight(me->kind) || me->qte_active) { ctrl_release_attack(c, ai); body_stop(me->actor); break; }
        c->engaged = was_engaged;
        ctrl_do_engage(index, b, w, me, o, ai);
        break;
    case MA2_ORD_FIGHT_MOVE: {
        ctrl_release_attack(c, ai);
        int arr;
        if (ctrl_exact_walk(c, w, me, o)) arr = hdist2(me->pos, o->point) <= maxf2(o->stop, 0.8f);
        else arr = ctrl_underfoot(c, w, me) ? (body_stop(me->actor), 0) : ctrl_travel(index, b, w, me, o->point, o->stop, 0, 0.f);
        if (arr) b->status.arrived = 1;
        if (o->fire && !o->hold_fire && ctrl_fire(c, w, me, o->target)) { c->tgt = o->target; ctrl_engaged(c, b, w); }
        break;
    }
    case MA2_ORD_HACK:
        if (me->kind != MA2_KIND_BATTLEARMOR) { body_stop(me->actor); break; }
        ctrl_do_hack(index, b, w, me, o, ai);
        break;
    case MA2_ORD_GET_VEHICLE:
        ctrl_release_attack(c, ai);
        if (me->kind == MA2_KIND_PILOT) {
            ctrl_board(index, b, w, me, o->vehicle);
            if (o->fire && o->target) ctrl_fire(c, w, me, o->target);
        } else body_stop(me->actor);
        break;
    case MA2_ORD_IDLE:
        if (me->kind == MA2_KIND_BATTLEARMOR && me->riding) {
            /* On a lift: the claw holds us; shoot what the brain names, jet off when told. */
            /* RideQuiet (default on, RECOMP_BOT_RIDE_QUIET / [Bots] RideQuiet): the MechAI was left awake in its goto (mode 6,
             * ai+0x90 = 1) from the last walk and kept driving the hanging body at its old point the whole ride (a playtester's game
             * 11-04 botrec t=13-31): the jets lit and cut every frame (playtest: the boost flickered every frame
             * while latched on). Aboard, the AI is put to sleep in mode 7 as body_stop does, claw untouched.
             * [C7] ridequiet: calms. */
            if (ai && (MEM8(ai + 0x90) || MEM32(ai + 0x12C) != 7u) && c7_on("ridequiet", "RECOMP_BOT_RIDE_QUIET", "RideQuiet", 1)) {
                MEM8(ai + 0x90) = 0; MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0;
                if (MEM32(ai + 0x12C) != 7u) ai_setmode(ai, 7);
                c7_add("ridequiet", "calms", 1);
            }
            if (o->target) ctrl_fire(c, w, me, o->target);
            if (o->detach && (w->time >= c->release_next || w->time + 2.0 < c->release_next)) {
                ba_release(me->actor); c->release_next = w->time + 1.0;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-LIFT] bot=%d lets go of %08X at %.0f,%.0f,%.0f t=%.1f\n", index, me->riding, me->pos.x, me->pos.y, me->pos.z, w->time);
            }
            break;
        }
        body_stop(me->actor);
        break;
    case MA2_ORD_TRAVEL: {
        ctrl_release_attack(c, ai);
        if (me->kind == MA2_KIND_VTOL && o->cargo) {
            /* Tether work: fly there, then sink to hover 9-12 m over the point and work the
             * hook -- A hooks what lies below, B lets it go over the teammate. */
            /* VtolEdgeAll: the hover point kept off the edge -- 25 m for a rider's pick-up / drop / wait, 10 m for tether work
             * (5-12 m up over what it hooks: no cliff top) */
            Ma2Vec3 cpt = o->point;
            if (vt_edge_all_on() && vt_keep_in(&cpt, o->cargo <= 2 ? 10.f : 25.f) > 0) c7_add("vtedgeall", "hover_in_s", w->dt);
            if (hdist2(me->pos, cpt) > 14.f) {
                int aboard = 0;
                for (int j = 0; j < w->nplayers && j < MA2_WORLD_MAX_PLAYERS; j++) if (w->players[j].alive && w->players[j].riding == me->actor) { aboard = 1; break; }
                g_vt_pickup = o->cargo == 3 && !aboard;                     /* PickupGlide */
                g_vkeep_cap = o->cargo <= 2 ? 10.f : 25.f; ctrl_travel(index, b, w, me, cpt, 4.f, 0, 0.f); g_vkeep_cap = 0.f; g_vt_pickup = 0;   /* the run in keeps no wider */
            }
            else {
                body_moveto(me->actor, cpt); c->va_branch = 'H';
                /* Down low over what we hook (the tether's collection area hangs under the hull;
                 * 9 m up the hook found nothing), never under 3 m off the ground. */
                /* (3: a BattleArmor claws on or off. It hangs 7-8 m under the skids: at 6 m a rider just clawed on was in the
                 * ground and knocked off within a second, 13 times running -- desktop ferry 10-31. 9 m: RiderHover.) */
                float hov = o->cargo == 1 ? 5.f : o->cargo == 2 || o->cargo == 4 ? 12.f : c7_on("riderhover", "RECOMP_BOT_VTOL_RIDER_HOVER", "RiderHover", 1) ? 9.f : 6.f;
                {
                    int gok = 0; float gnd = terrain_height(me->pos.x, me->pos.z, &gok);
                    if (gok && o->point.y + hov < gnd + 3.f) hov = gnd + 3.f - o->point.y;
                }
                /* HoverFix: MoveTo settles ~2 m short of its height -- 11 m over a BattleArmor asked to hover at 9 m, out of
                 * its claw's reach for 30 s (desktop 10-31) -- so it is asked that much lower */
                if (ai) MEMF(ai + 0x20C) = o->point.y + hov - (c7_on("hoverfix", "RECOMP_BOT_VTOL_HOVER_FIX", "HoverFix", 1) && me->pos.y > o->point.y + hov - 0.5f ? 2.5f : 0.f);
                c->dest = cpt; c->mode = CM_MOVE;
                if (env_on("RECOMP_BOT_DIAG") && w->time - c->bdiag_t > 1.0) {
                    c->bdiag_t = w->time;
                    fprintf(stderr, "[BOT-CARGODIAG] bot=%d cargo=%d d=%.1f y=%.1f py=%.1f want=%.1f next=%.1f ai20C=%.1f t=%.1f\n", index, o->cargo, hdist2(me->pos, o->point),
                            me->pos.y, o->point.y, o->point.y + hov, c->cargo_next, ai ? MEMF(ai + 0x20C) : -1.f, w->time);
                }
                if (o->cargo <= 2 && w->time >= c->cargo_next && fabsf(me->pos.y - (o->point.y + hov)) < 6.f) {
                    ctrl_tap(c, o->cargo == 1 ? 37 : 38, 2); c->cargo_next = w->time + 1.0;
                    if (env_on("RECOMP_BOT_DIAG")) {      /* what the hook sees: the collection area (+0x62C) and the entity under us (+0x60) */
                        uint32_t a = me->actor, u60 = MEM32(a + 0x60), u62c = MEM32(a + 0x62C);
                        fprintf(stderr, "[BOT-CARGOSEE] bot=%d +60=%08X +62C=%08X +630=%.2f +578=%d +57C=%d f260=%08X\n", index, u60, u62c,
                                MEMF(a + 0x630), (int)MEM32(a + 0x578), (int)MEM32(a + 0x57C), MEM32(a + 0x260));
                    }
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-CARGO] bot=%d %s at %.0f,%.0f,%.0f over %.0f,%.0f,%.0f t=%.1f\n", index, o->cargo == 1 ? "hook" : "drop",
                                                           me->pos.x, me->pos.y, me->pos.z, o->point.x, o->point.y, o->point.z, w->time);
                }
            }
            if (o->fire && !o->hold_fire && o->target && ctrl_fire(c, w, me, o->target)) { c->tgt = o->target; ctrl_engaged(c, b, w); }
            break;
        }
        int arr;
        if (o->exact_r > 0.f && ctrl_exact_walk(c, w, me, o)) arr = hdist2(me->pos, o->point) <= maxf2(o->stop, 0.8f);   /* flag grab: the last metres straight */
        else { g_spread_ctx = o->stop >= 1.5f && !o->fire && !o->structure && !o->cargo; arr = ctrl_underfoot(c, w, me) ? (body_stop(me->actor), 0) : ctrl_travel(index, b, w, me, o->point, o->stop, 0, 0.f); g_spread_ctx = 0; }
        if (arr) b->status.arrived = 1;
        int nf_fired = o->fire && !o->hold_fire && o->target && ctrl_fire(c, w, me, o->target);
        if (o->target && env_on("RECOMP_BOT_DIAG") && index >= 0 && index < 16) {   /* NoFireWhy */
            const Ma2PlayerView *nt = NULL;
            if (nf_fired || !ctrl_target_alive(w, o->target, &nt) || hdist2(nt->pos, me->pos) > me->reach) s_nf_t0[index] = w->time;
            else if (w->time - s_nf_t0[index] > 1.5 && w->time - s_nf_log[index] > 2.0) {
                s_nf_log[index] = w->time;
                fprintf(stderr, "[BOT-NOFIRE] t=%.1f bot=%d %.1f s unshot target %08X %.0f m (reach %.0f): %s\n", w->time, index, w->time - s_nf_t0[index], o->target,
                        hdist2(nt->pos, me->pos), me->reach, !o->fire ? "order fire=0" : o->hold_fire ? "hold_fire" : s_nf_why[index] ? s_nf_why[index] : "?");
            }
        }
        if (nf_fired) { c->tgt = o->target; ctrl_engaged(c, b, w); }
        else if (o->structure && hdist2(me->pos, o->structure_pos) < 260.f && !ctrl_fire_structure(c, w, me, o->structure, o->structure_pos)) {
            /* At the building with no clear shot (scenery, a hull or a teammate in between): after 3 s, close in.
             * Base War side step ([Bots] ObjSideStep): closing in straight wedged attackers against the
             * base for the rest of the match (F20 run: frozen 25 s at 69 m); instead walk to a firing spot 70 deg
             * round the building, the other side each time, re-picked every 8 s or when we stop moving. */
            if (c->obj_nolos_t <= 0.0 || w->time < c->obj_nolos_t) { c->obj_nolos_t = w->time; c->obj_side_t = 0.0; }
            else if (w->time - c->obj_nolos_t > 3.0 && c7_on("objside", "RECOMP_BOT_OBJ_SIDESTEP", "ObjSideStep", 1)) {
                float dx = me->pos.x - o->structure_pos.x, dz = me->pos.z - o->structure_pos.z, d = hypotf(dx, dz);
                int stale = c->obj_side_t <= 0.0 || w->time - c->obj_side_t > 8.0 ||
                            (w->time - c->obj_side_t > 4.0 && hdist2(me->pos, c->obj_side_from) < 3.f && hdist2(me->pos, c->obj_side_pt) > 8.f);
                if (stale && d > 1.f) {
                    float r = fminf(fmaxf(d, 35.f), 60.f), a = atan2f(dz, dx) + (c->obj_side ? 1.22f : -1.22f);
                    c->obj_side ^= 1;
                    c->obj_side_pt = o->structure_pos;
                    c->obj_side_pt.x += cosf(a) * r; c->obj_side_pt.z += sinf(a) * r;
                    c->obj_side_t = w->time; c->obj_side_from = me->pos;
                    c7_add("objside", "sidesteps", 1);
                }
                ctrl_travel(index, b, w, me, c->obj_side_pt, 6.f, 0, 0.f);
            }
            else if (arr && w->time - c->obj_nolos_t > 3.0) ctrl_travel(index, b, w, me, o->structure_pos, 20.f, 0, 0.f);
        }
        else if (o->structure && hdist2(me->pos, o->structure_pos) < 260.f) {
            c->obj_nolos_t = 0.0;
            c7_add("objside", "firing_s", w->dt > 0.f && w->dt < 0.5f ? w->dt : 0.f);
            obj_note_fire(o->structure, w->dt > 0.f && w->dt < 0.5f ? w->dt : 0.f);
            static int nlog;
            if (env_on("RECOMP_BOT_DIAG") && nlog++ < 20) fprintf(stderr, "[BOT-OBJFIRE] bot=%d at %08X d=%.0f t=%.1f\n", index, o->structure, hdist2(me->pos, o->structure_pos), w->time);
        }
        break;
    }
    case MA2_ORD_EJECT:
        ctrl_release_attack(c, ai);
        if (me->kind == MA2_KIND_VTOL) {
            /* A player holds exit (Y / Q): the VTOL sets itself down and the pilot gets out at the bottom
             * -- on the player's controls, not the flight AI's: handed to the AI (body_stop) the exit was
             * refused at any height. Holding still over the spot meanwhile. */
            int okg = 0, oks = 0;
            float g = terrain_height(me->pos.x, me->pos.z, &okg), sf = surface_height(me->pos.x, me->pos.z, &oks);
            float top = oks ? maxf2(sf, okg ? g : sf) : (okg ? g : me->pos.y);
            if (fabsf(me->pos.y - c->land_y) > 0.4f) { c->land_y = me->pos.y; c->land_t = w->time; }
            int low = me->pos.y - top < 3.0f || w->time - c->land_t > 1.5;
            uint32_t node = MEM32(me->actor + 0x94);
            Ma2Vec3 ahead = me->pos; float fx = 0.f, fz = 1.f;
            if (gva_ok(node, 0x100)) { fx = MEMF(node + 0xE0); fz = MEMF(node + 0xE8); ahead.x += fx * 10.f; ahead.z += fz * 10.f; }
            vtol_stick(index, c, w, me, ahead, me->pos.y - 20.f, 0.f);   /* stick full down */
            /* EjectSink (default on, RECOMP_BOT_EJECT_SINK / [Bots] EjectSink): under ModClimb the stick's vertical does not move the craft --
             * a player sinks with Ctrl (the -30 demand, comp+0x90) -- and exit held does not set it down from high up: a swap-eject VTOL
             * hung 48 m over the Bogged Down base for 6 s, y flat, then the "stuck high" retry climbed it to 71 m (au3_60 bot 10,
             * t=100.1-106.4). Sink with the demand to 3 m over what is under it (not while the retry lifts). [C7] ejectsink: sink_s. */
            if (vt_mod_climb_on() && c7_on("ejectsink", "RECOMP_BOT_EJECT_SINK", "EjectSink", 1)) {
                c->vt_lift_actor = me->actor;
                c->vt_lift_demand = w->time < c->exit_off_until ? 30.f : me->pos.y - top > 3.f ? -30.f : 0.f;
                if (c->vt_lift_demand < 0.f) { c7_add("ejectsink", "sink_s", w->dt); c->land_t = w->time; }   /* sinking is headway: not "stuck high" */
            }
            {   /* the drift braked out (coasting on, one set down on top of a 'Mech); and a descent that
                 * stops high -- resting on something -- slides forward off it */
                float vf = me->vel.x * fx + me->vel.z * fz, vl = me->vel.x * fz - me->vel.z * fx;
                c->axis[4] = clampf2(-vf * 0.12f, -0.5f, 0.5f); if (c->axis[4] == 0.f) c->axis[4] = 1e-6f;
                c->axis[8] = clampf2(-vl * 0.12f, -0.5f, 0.5f); if (c->axis[8] == 0.f) c->axis[8] = 1e-6f;
                /* Come to rest on something above the ground (it froze there, stick and all): let go of
                 * exit a moment, climb (letting go alone did not lift it) and move on, then set down again. */
                if (c->eject_t0 <= 0.0) c->eject_t0 = w->time;
                if ((w->time - c->land_t > 2.5 || w->time - c->eject_t0 > 8.0) && me->pos.y - top > 3.f && w->time > c->exit_off_until + 3.0) {
                    c->exit_off_until = w->time + 1.5; c->eject_t0 = w->time + 1.5;
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-VTOL] bot=%d stuck %.1f m up setting down: lifting off to try again\n", index, me->pos.y - top);
                }
                if (w->time < c->exit_off_until) { c->axis[4] = 0.7f; ctrl_button(c, BTN_LS, 2); }   /* up (LS click / Shift) and on */
            }
            if (w->time >= c->exit_off_until) ctrl_button(c, BTN_Y, 2);   /* exit held (Q / Y): it sets the VTOL down, then the pilot gets out */
            if (env_on("RECOMP_BOT_DIAG") && w->time - c->land_log_t > 2.0) {
                c->land_log_t = w->time;
                fprintf(stderr, "[BOT-VTOL] bot=%d getting out: %.1f m up%s\n", index, me->pos.y - top, low ? ", exit pressed" : "");
            }
            break;
        }
        body_stop(me->actor);
        if (me->kind != MA2_KIND_PILOT) ctrl_tap(c, 39, 3);
        break;
    default:
        ctrl_release_attack(c, ai);
        body_stop(me->actor);
        break;
    }
    if (me->kind == MA2_KIND_VTOL && drivable) vt_edge_recover(index, c, w, me, o);   /* VtolEdgeAll: back out of the band if it drifted in */
    if (c->last_fire_t > 0.0 && w->time - c->last_fire_t < 0.05) b->status.fired = 1;
    /* Jumpjets: a hop along the route (terrain, the brain's call) or a sidestep under fire. */
    if (o->jump && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && hdist2(c->dest, me->pos) > 3.f && !braking &&
        !(o->keep_r > 0.f && hdist2(c->dest, o->keep_pt) < o->keep_r + 10.f && hdist2(c->dest, o->keep_pt) < hdist2(me->pos, o->keep_pt) && (c7_add("blastbrake", "hops_skipped", 1), 1)) &&
        body_jump_toward(me->actor, me->pos, c->dest)) c->jumps++;
    /* not while leaving a death blast: 16 of 263 escapes took a dodge jump sideways (toward the blast as often as not) and a BattleArmor spent the fuel its escape hop needed */
    if (o->dodge && (me->kind == MA2_KIND_MECH || me->kind == MA2_KIND_BATTLEARMOR) && !(c->bav_until > 0.0 && c->bav_blast) && w->time - c->dodge_t > (o->dodge_now ? 1.0 : me->kind == MA2_KIND_BATTLEARMOR && ba_walk_on() ? 7.0 : 3.5) && (o->dodge_now || w->time >= c->jump_next)) {
        const Ma2PlayerView *t = NULL;
        if (o->dodge_now || ctrl_target_alive(w, o->target, &t)) {
            Ma2Vec3 side = me->pos;
            if (o->dodge_now) { side.x += o->dodge_dx * 20.f; side.z += o->dodge_dz * 20.f; }   /* out of the missile's line */
            else {
                float dx = t->pos.x - me->pos.x, dz = t->pos.z - me->pos.z, l = hypotf(dx, dz) + 1e-3f;
                side.x += dz / l * 25.f * (float)o->dodge; side.z -= dx / l * 25.f * (float)o->dodge;
            }
            /* DodgeBurn (default on, RECOMP_BOT_DODGE_BURN / [Bots] DodgeBurn): a missile dodge needs 3-5 m off its line, not the whole
             * tank -- the full 2 s burn threw a flag-run BattleArmor 40 m back and 20 m up onto a roof, where it stood till AiKick
             * (au3_64 bot 4, t=43.2-46.8; 0.7 s into the burn it was 8 m clear and still at ground height). */
            float dburn = o->dodge_now && c7_on("dodgeburn", "RECOMP_BOT_DODGE_BURN", "DodgeBurn", 1) ? 0.7f : 2.0f;
            int bb_in = braking || (o->keep_r > 0.f && hdist2(side, o->keep_pt) < o->keep_r + 10.f && hdist2(side, o->keep_pt) < hdist2(me->pos, o->keep_pt));   /* BlastBrake: not toward the dying 'Mech */
            if (bb_in) c7_add("blastbrake", "hops_skipped", 1);
            else if (ma2_trav_clear(side.x, side.z, w->time) > 4.f && body_jump_fuel_max(me->actor, me->pos, side, 1.9f, dburn)) {
                c->dodges++; c->dodge_t = w->time;
                if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-DODGE] bot=%d side=%d t=%.1f\n", index, o->dodge, w->time);
            }
        }
    }
    ctrl_bomb_escape(index, b, w, me);
    ctrl_pilot_bomb(index, b, w, me);
    ba_land_release(index, c, w, me);                 /* BaLand: a landed BattleArmor walks again */
    ba_dip_hop(index, c, w, me, o);                    /* BaDipHop: after anything that stood it still this step */
    if (o->defend) ctrl_tap(c, 12, 3);                 /* the defence (RS click) is command 12; 49..55 answer NeuroHacks */
    for (int k = 0; k < 7; k++) if (o->qte_press & (1u << k)) ctrl_tap(c, 49 + k, 3);
    ctrl_send_frame(index, b->player);
    ctrl_trace(index, c, w, me, o);
}
