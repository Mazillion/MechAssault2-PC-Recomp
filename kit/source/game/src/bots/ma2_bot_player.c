/* MechAssault 2 offline bots - BotPlayer creation.
 *
 * NOT a standalone translation unit: #included by ma2_bots_core.c.
 *
 * A bot is an ordinary non-human MAPlayer (vtable 0x4654B0) that this machine
 * simulates. At level spawn (sub_1DB150), right after the lobby iterator has
 * created the human players, one BotPlayer per configured bot is built with the
 * game's own per-player constructor sub_1DAE30(&record, session) -- the routine
 * the iterator calls for every player: CreatePlayer sub_1668D8 (isHuman = 0),
 * ids, the vehicle sub_166C0C (no MAInput: that is created for humans only),
 * SetNation (team / FFA slot via 0x4AE8B4), team colours, team stat slots.
 * The world then activates and places it with everyone else (sub_1665DE).
 *
 * Afterwards: the vehicle's network-proxy bits (set for every non-human at
 * loc_001DB0BC) are cleared -- this player is simulated here -- the player gets
 * bit 0 of +0x244 (the bit MAInput's activation sets: control behaviour and
 * has-control get created by the player tick), and it joins the is_local_ai
 * registry. Nothing else: no lobby proxy, no pad, no camera, no viewport.
 * Respawns, scoring and lists are the game's.
 */

static uint32_t bot_create(int i, uint32_t session)
{
    GuestRegs r; regs_save(&r);
    g_esp -= 0x80;
    uint32_t rec = (g_esp + 0x10) & ~15u;
    ma2_bot_lobby_record(i, session, rec);            /* name, controller id, vehicle, team, slot, isHuman=0 */
    MEM32(0x50C9B8u) = 0;                             /* sub_1DAE30 stores the new player here */
    g_esp = rec - 0x10;
    PUSH32(g_esp, session); PUSH32(g_esp, rec); PUSH32(g_esp, 0x001D3F2Eu);
    sub_001DAE30();                                   /* cdecl; ret */
    uint32_t p = MEM32(0x50C9B8u);
    regs_restore(&r);
    if (!gva_ok(p, 0x4B0)) return 0;
    if (!g_cls.corporeal) classes_refresh();
    uint32_t a = player_actor(p);
    if (a && (MEM32(a + 0x1C) & 0x30000u)) MEM32(a + 0x1C) &= ~0x30000u;   /* not a network proxy */
    MEM32(p + 0x244) |= 1u;                           /* input-controlled: behaviour + has-control */
    return p;
}

/* Hook at loc_001DB1E8 (sub_1DB150, right after the lobby iterator). */
void ma2_bots_level_spawn(uint32_t session)
{
    if (!g_bot_spawning) return;
    GuestRegs regs; regs_save(&regs);
    g_bot_spawning = 0;
    Roster r; compute_roster(session, &r);
    uint32_t seed = GetTickCount();
    int n = 0, skills[MAX_BOTS];
    roster_skills(&r, g_rt.skill, seed, skills);
    for (int i = 0; i < r.n && i < MAX_BOTS; i++) {
        Bot *b = &g_rt.bots[i];
        memset(b, 0, sizeof *b);
        b->port = MA2_BOT_PORT_BASE + (uint32_t)i; b->team = r.team[i]; b->slot = r.slot[i];
        b->skill = skills[i]; b->ctrl.skill = skills[i];
        uint32_t p = bot_create(i, session);
        if (env_on("RECOMP_BOT_DIAG"))
            fprintf(stderr, "[BOT-PLAYER] created bot=%d player=%08X port=%u team=%d slot=%d skill=%d nation=%d actor=%08X f244=%08X\n", i, p, b->port, b->team, b->slot, b->skill,
                    p ? actor_int(p, 0xA8, 0x000EB091u, -1) : -1, p ? player_actor(p) : 0, p ? MEM32(p + 0x244) : 0);
        if (!p) continue;
        b->player = p;
        b->created = MEMD(VA_GAME_TIME);
        g_cur_bot = i;   /* MixedDragoon: the brain picks its knobs by ma2_bots_dragoon() */
        b->brain = ma2_brain_create(i, b->skill, seed * 2654435761u + (uint32_t)i * 40503u);
        g_cur_bot = -1;
        if (g_dragoon_bot[i] && env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-PLAYER] bot=%d is a Dragoon ace (Mixed)\n", i);
        n = i + 1;
        g_rt.nbots = n;                               /* registered from here on */
    }
    if (n > 0) { extern void ma2_bot_sp_match_begin(void); ma2_bot_sp_match_begin(); c7_match_begin(MEMD(VA_GAME_TIME)); rec_match_begin(); }
    regs_restore(&regs);
}

/* Activation check: the world normally activates the new players with the others.
 * If one is still not in the player vector a few seconds in (it never was here, but
 * the original only creates players inside the iterator), use the mid-game join path:
 * sub_B2B70 activation + a spawn request through the respawn tick. */
static void bot_ensure_active(int index, Bot *b)
{
    uint32_t p = b->player, vb = MEM32(VA_PLAYERS_BEGIN), ve = MEM32(VA_PLAYERS_END);
    static double last_check[MAX_BOTS];
    double now = MEMD(VA_GAME_TIME);
    if (now < last_check[index]) last_check[index] = -1e9;       /* ew-b8 B8.5 (R5 E5): a new match's clock restarts below the last check */
    if (!p || now - last_check[index] < 3.0) return;
    last_check[index] = now;
    if (ve > vb && ve - vb <= 64) for (uint32_t q = vb; q < ve; q += 4) if (MEM32(q) == p) return;
    fprintf(stderr, "[BOT-PLAYER] bot=%d player=%08X not active -> join activation\n", index, p);
    GuestRegs r; regs_save(&r);
    PUSH32(g_esp, 0u); PUSH32(g_esp, 0x000B4080u); PUSH32(g_esp, 0u); PUSH32(g_esp, p); PUSH32(g_esp, 0x001DB58Bu);
    sub_000B2B70();
    regs_restore(&r);
    MEM32(p + 0x244) |= 0x40u; MEMF(p + 0x2D0) = (float)now;
}
