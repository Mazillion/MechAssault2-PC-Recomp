/* ma2_bot_defense.c -- RS-click defensive systems and weapon damage classes (DEFENSE_SPEC.md).
 * Included from ma2_bots_core.c (after kind_name / bot_index_of / class helpers).
 *
 * Defence object  = MAMover actor+0x220 (created from DefenseType actor+0x21C by sub_E6ED1).
 *   base (vt 0x478D98): +4 flags (1 recharging, 2 active, 4 down-cue played, 8/0x10/0x20/0x40/0x80 rider rules)
 *                       +8 DefenseDuration  +0xC DefenseRechargeTime  +0x10 end time (game time 0x4E2440):
 *                       active -> activation+Duration, recharging -> deactivation+RechargeTime
 *   absorber (type 6, vt 0x478B90): +0x94 absorbing, +0x8C shield HP, +0x24 MaxHPShield, +0x30 PreventsFire,
 *                       +0x34 PercentHeatNotAbsorbed, +0x38 AbsorbsSplashDamage, +0x39 AbsorbsDirectDamage,
 *                       +0x48[5] %sDamageAbsorbed (Standard/Energy/Ballistic/Explosive/Flame)
 * Weapon barrel   = first child of a MAWpnGrp (arsenal actor+0xA4 child): +0x78 flags, +0x128 range,
 *                   +0x148 BarrelDamage (direct), +0x14C BarrelSplashDamage, +0x158 splash radius;
 *                   projectile template = MAWpnGrp+0x7C (wpn_template): +0x370 DamageType, +0x260 bit17 AMS.
 * Defence command is id 12 (behaviour vt+0xA4 sub_175952); 49..55 (incl. 52) are NeuroHack answers.
 * RECOMP_DEFENSE_PROBE=1 logs [DEF] (1 Hz), [DEF-EDGE], [DEF-WPN], [DEF-VS], [DEF-HIT], [DEF-ABS],
 * [DEF-CMD]/[DEF-MSET]/[DEF-SET]/[DEF-ACT] (activation path).
 * RECOMP_DEFENSE_TEST=<s>: every bot in a 'Mech or tank taps command RECOMP_DEFENSE_TEST_CMD (default 12) every <s> s. */

static const char *kind_name(int k);

enum { DEFC_NONE = 0, DEFC_BASE, DEFC_SHIELD, DEFC_CAMO, DEFC_TYPE4, DEFC_ABSORB, DEFC_AMS };
static const char *const k_defc_name[] = { "none", "base", "shield", "camo", "type4", "absorber", "ams" };
static const char *const k_dmg_name[] = { "Standard", "Energy", "Ballistic", "Explosive", "Flame" };

static int def_class_of_vt(uint32_t vt)
{
    switch (vt) {
    case 0x00478D98u: return DEFC_BASE;
    case 0x00478B0Cu: return DEFC_SHIELD;   /* type 1, spawns an MAEffectShield bubble (ShieldUID +0x28) */
    case 0x00478A50u: return DEFC_CAMO;     /* type 5 (2 is remapped to 5 at load) */
    case 0x00478CA0u: return DEFC_TYPE4;    /* type 4, Target Jamming (no lock / lead against it) */
    case 0x00478B90u: return DEFC_ABSORB;   /* type 6 */
    case 0x00478CF0u: return DEFC_AMS;      /* type 7, Active AMS effect (ActiveAMSEffectTemplateUID +0x24) */
    }
    return DEFC_NONE;
}

typedef struct DefView {
    int dtype, cls, active, recharging, flags;
    uint32_t obj;
    float dur, rech, left, meter;
    int abs_on, abs_hp, abs_max, abs_dir, abs_spl, abs_pf;
    float abs_heat, abs_frac[5];
} DefView;

/* Reads the defence state of a MAMover actor; 0 when the actor has none. */
static int def_read(uint32_t a, double now, DefView *d)
{
    memset(d, 0, sizeof *d);
    if (!a || !gva_ok(a, 0x230) || !is_kind(a, g_cls.mover)) return 0;
    d->dtype = (int)MEM32(a + 0x21C);
    uint32_t o = MEM32(a + 0x220);
    if (!gva_ok(o, 0xB0) || !(d->cls = def_class_of_vt(MEM32(o)))) return 0;
    d->obj = o;
    d->flags = MEM8(o + 4);
    d->active = (d->flags & 2) != 0;
    d->recharging = (d->flags & 1) != 0;
    d->dur = MEMF(o + 8); d->rech = MEMF(o + 0xC);
    d->left = (float)((double)MEMF(o + 0x10) - now);
    /* HUD meter, port of sub_1897AA (via sub_E9E95): active -> fraction of active time left,
     * recharging -> fraction recharged, idle -> 1. */
    if (d->left > 0.f) {
        if (d->active) d->meter = d->dur != 0.f ? d->left / d->dur : 1.f;
        else if (d->recharging) d->meter = d->rech != 0.f ? 1.f - d->left / d->rech : 1.f;
        else d->meter = 1.f;
    } else d->meter = d->active ? 0.f : 1.f;
    if (d->cls == DEFC_ABSORB) {
        d->abs_on = MEM8(o + 0x94); d->abs_hp = (int)MEM32(o + 0x8C); d->abs_max = (int)MEM32(o + 0x24);
        d->abs_pf = MEM8(o + 0x30); d->abs_spl = MEM8(o + 0x38); d->abs_dir = MEM8(o + 0x39);
        d->abs_heat = MEMF(o + 0x34);
        for (int t = 0; t < 5; t++) d->abs_frac[t] = MEMF(o + 0x48 + 4u * (uint32_t)t);
    }
    return 1;
}

/* The game's own IsDefenseActive (sub_E694C): defence object present and flag 2 set. */
static int def_is_active(uint32_t a)
{
    if (!a || !gva_ok(a, 0x230) || !is_kind(a, g_cls.mover)) return 0;
    uint32_t o = MEM32(a + 0x220);
    return gva_ok(o, 8) && def_class_of_vt(MEM32(o)) && (MEM8(o + 4) & 2);
}

/* ---- weapons ---- */
enum { WD_NONE = 0, WD_DIRECT = 1, WD_SPLASH = 2, WD_BOTH = 3 };
static const char *const k_wd_name[] = { "none", "DIRECT", "SPLASH", "BOTH" };

static int g_def_probe = -1;
static int def_probe_on(void) { if (g_def_probe < 0) g_def_probe = env_int("RECOMP_DEFENSE_PROBE", 0); return g_def_probe > 0; }

static void def_who(uint32_t actor, char *buf, size_t n)
{
    const Ma2World *w = &g_rt.world;
    for (int i = 0; i < w->nplayers; i++)
        if (w->players[i].actor == actor && actor) {
            int bi = w->players[i].is_bot ? bot_index_of(w->players[i].player) : -1;
            if (w->players[i].is_human && !w->players[i].is_bot) snprintf(buf, n, "H");
            else snprintf(buf, n, "B%d", bi);
            return;
        }
    snprintf(buf, n, "-%08X", actor);
}
static uint32_t def_actor_by_uid(uint32_t uid)
{
    const Ma2World *w = &g_rt.world;
    for (int i = 0; i < w->nplayers; i++)
        if (w->players[i].actor && gva_ok(w->players[i].actor, 0x30) && MEM32(w->players[i].actor + 0x2C) == uid) return w->players[i].actor;
    return 0;
}

/* Projectile template of a fire group (MAWpnGrp w): uid MEM32(w+0x7C) in the template world
 * MEM32(world+0x24) (sub_B5B40 clones it at fire time, sub_D2035 then copies the barrel's damage in).
 * Template +0x370 DamageType (0..4) of the direct hit, +0x260 bit 17 ShotDownByActiveAMS. */
static uint32_t wpn_template(uint32_t w)
{
    uint32_t world = gva_ok(w, 0xA0) ? MEM32(w + 0x60) : 0;
    uint32_t tw = gva_ok(world, 0x30) ? MEM32(world + 0x24) : 0;
    uint32_t t = tw ? uid_lookup(tw, MEM32(w + 0x7C)) : 0;
    return gva_ok(t, 0x390) && is_kind(t, g_cls.projectile) ? t : 0;
}
typedef struct WpnClass { int direct, splash, dtype, ams; float radius; } WpnClass;   /* dtype -1 = unknown */
static int wpn_class(uint32_t w, WpnClass *c)
{
    memset(c, 0, sizeof *c); c->dtype = -1;
    uint32_t b = gva_ok(w, 0xA0) ? MEM32(w + 0xC) : 0, t = wpn_template(w);
    if (!gva_ok(b, 0x200)) return 0;
    c->direct = (int)MEM32(b + 0x148);
    c->radius = MEMF(b + 0x158);
    c->splash = c->radius > 0.f ? (int)MEM32(b + 0x14C) : 0;
    if (t) { c->dtype = (int)MEM32(t + 0x370); c->ams = (MEM32(t + 0x260) >> 17) & 1; }
    return 1;
}
/* Share (0..1) of a fire group's damage that the target's defence stops right now.
 * Absorber: exact (sub_1872A0). AMS: AMS-flagged projectiles crossing the AMS effect are destroyed
 * (sub_197E12) -> 1 for those. Shield bubble: projectiles hit the MAEffectShield (static, unverified) -> 1. */
static float def_weapon_stopped(uint32_t target, uint32_t w, double now)
{
    DefView d; WpnClass c;
    if (!def_read(target, now, &d) || !wpn_class(w, &c)) return 0.f;
    switch (d.cls) {
    case DEFC_ABSORB: {
        if (!d.abs_on || (d.abs_max > 0 && d.abs_hp <= 0)) return 0.f;
        float tot = (float)(c.direct + c.splash), stop = 0.f;
        if (d.abs_dir && c.dtype >= 0 && c.dtype < 5) stop += d.abs_frac[c.dtype] * (float)c.direct;
        if (d.abs_spl) stop += d.abs_frac[3] * (float)c.splash;          /* splash is always Explosive (sub_182160) */
        return tot > 0.f ? stop / tot : 0.f;
    }
    case DEFC_AMS: return d.active && c.ams ? 1.f : 0.f;
    case DEFC_SHIELD: return d.active ? 1.f : 0.f;
    default: return 0.f;
    }
}

static void def_dump_weapons(uint32_t a, const char *who, int kind)
{
    uint32_t ars = MEM32(a + 0xA4), wcls = MEM32(0x004EAF4Cu);
    if (!gva_ok(ars, 0x90)) return;
    for (uint32_t w = MEM32(ars + 0xC), n = 0; gva_ok(w, 0xA0) && n < 64; w = MEM32(w + 8), n++) {
        if (MEM32(w + 0x28) != wcls) continue;
        int g = (int)MEM32(w + 0x80);
        int sel = g >= 0 && g < 8 && MEM32(w + 0x98) == MEM32(ars + 0x7C + 4u * (uint32_t)g);
        uint32_t brl = MEM32(w + 0xC);
        if (!gva_ok(brl, 0x200)) continue;
        WpnClass c; wpn_class(w, &c);
        uint32_t fl = MEM32(brl + 0x78);
        fprintf(stderr, "[DEF-WPN] who=%s kind=%s g=%d sel=%d grp=%08X barrel=%08X tmpl=%08X range=%.0f direct=%d dtype=%d(%s) splash=%d r=%.1f ams=%u fl=%08X beam=%u flame=%u charge=%u class=%s\n",
                who, kind_name(kind), g, sel, w, brl, MEM32(w + 0x7C), MEMF(brl + 0x128), c.direct, c.dtype, c.dtype >= 0 && c.dtype < 5 ? k_dmg_name[c.dtype] : "?",
                c.splash, c.radius, c.ams, fl, (fl >> 16) & 1, (fl >> 18) & 1, (fl >> 3) & 1, k_wd_name[(c.direct > 0 ? WD_DIRECT : 0) | (c.splash > 0 ? WD_SPLASH : 0)]);
    }
}
/* Selected MAWpnGrp of fire group g (same rule as ma2_bot_ctrl.c weapon_of). */
static uint32_t def_group_wpn(uint32_t a, int g)
{
    uint32_t ars = MEM32(a + 0xA4), wcls = MEM32(0x004EAF4Cu), any = 0;
    if (!gva_ok(ars, 0x90)) return 0;
    for (uint32_t w = MEM32(ars + 0xC), n = 0; gva_ok(w, 0xA0) && n < 64; w = MEM32(w + 8), n++) {
        if (MEM32(w + 0x28) != wcls || (int)MEM32(w + 0x80) != g) continue;
        if (!any) any = w;
        if (MEM32(w + 0x98) == MEM32(ars + 0x7C + 4u * (uint32_t)g)) return w;
    }
    return any;
}
/* Fire group of `owner` whose barrel produced projectile values (direct, splash). */
static int def_group_of_shot(uint32_t owner, int direct, int splash)
{
    uint32_t ars = owner ? MEM32(owner + 0xA4) : 0, wcls = MEM32(0x004EAF4Cu);
    if (!gva_ok(ars, 0x90)) return -1;
    for (uint32_t w = MEM32(ars + 0xC), n = 0; gva_ok(w, 0xA0) && n < 64; w = MEM32(w + 8), n++) {
        if (MEM32(w + 0x28) != wcls) continue;
        for (uint32_t brl = MEM32(w + 0xC), nb = 0; gva_ok(brl, 0x200) && nb < 8; brl = MEM32(brl + 8), nb++)
            if ((int)MEM32(brl + 0x148) == direct && (int)MEM32(brl + 0x14C) == splash) return (int)MEM32(w + 0x80);
    }
    return -1;
}

/* ---- damage hooks (generated code, patch_defense_probe.py) ---- */
static struct { double t; int lines, dropped; } g_def_rate;
static int def_rate_ok(void)
{
    double now = MEMD(VA_GAME_TIME);
    if (now - g_def_rate.t >= 1.0 || now < g_def_rate.t) {
        if (g_def_rate.dropped) fprintf(stderr, "[DEF-HIT] (%d lines dropped)\n", g_def_rate.dropped);
        g_def_rate.t = now; g_def_rate.lines = 0; g_def_rate.dropped = 0;
    }
    if (g_def_rate.lines >= 40) { g_def_rate.dropped++; return 0; }
    g_def_rate.lines++; return 1;
}
static int def_is_player_actor(uint32_t a)
{
    const Ma2World *w = &g_rt.world;
    for (int i = 0; i < w->nplayers; i++) if (w->players[i].actor == a && a) return 1;
    return 0;
}

/* sub_00100A52 entry (MAMech vt+0x1D0 TakeDamage(ecx=mech, [esp+4]=damage info)).
 * info +0x0C source (entity+0x24), +0xB0 float damage, +0xB4 flags (1 splash, 8 fixed amount),
 * +0xC0 damage type, +0xC4 heat. */
void ma2_def_hit_hook(uint32_t mech, uint32_t info)
{
    if (!def_probe_on() || !gva_ok(info, 0xD0) || !def_is_player_actor(mech)) return;
    uint32_t s24 = MEM32(info + 0xC), src = s24 ? s24 - 0x24 : 0;
    static uint32_t effect_cls;
    if (!effect_cls) effect_cls = class_id(0x4EAF34u, "MAEffect");
    /* skip zero-damage contacts (bodies, buildings); keep projectiles, effects and fixed amounts */
    if (!(gva_ok(src, 0x390) && is_kind(src, g_cls.projectile)) && !(gva_ok(src, 0x280) && is_kind(src, effect_cls)) && MEMF(info + 0xB0) == 0.f) return;
    if (!def_rate_ok()) return;
    double now = MEMD(VA_GAME_TIME);
    char vw[16], ow[16] = "?"; def_who(mech, vw, sizeof vw);
    DefView d; def_read(mech, now, &d);
    uint32_t hpb = MEM32(mech + 0x260);
    int hp = gva_ok(hpb, 0x10) ? (int)MEM32(hpb + 8) : -1;
    unsigned dt = MEM32(info + 0xC0);
    char srcbuf[200] = "";
    if (gva_ok(src, 0x390) && is_kind(src, g_cls.projectile)) {
        uint32_t owner = def_actor_by_uid(MEM32(src + 0x2B8));
        if (owner) def_who(owner, ow, sizeof ow);
        int direct = (int)MEM32(src + 0x2A4), splash = (int)MEM32(src + 0x274);
        snprintf(srcbuf, sizeof srcbuf, "src=proj owner=%s g=%d pdirect=%d psplash=%d pr=%.1f pdt=%u pfl=%08X amsdown=%u",
                 ow, def_group_of_shot(owner, direct, splash), direct, splash, MEMF(src + 0x270), MEM32(src + 0x370), MEM32(src + 0x260),
                 (MEM32(src + 0x260) >> 17) & 1);
    } else if (gva_ok(src, 0x280)) {
        uint32_t owner = def_actor_by_uid(MEM32(src + 0x24C));
        if (owner) def_who(owner, ow, sizeof ow);
        snprintf(srcbuf, sizeof srcbuf, "src=cls%08X vt=%08X owner?=%s e240=%d e1B0=%d e274=%u", MEM32(src + 0x28), MEM32(src), ow,
                 (int)MEM32(src + 0x240), (int)MEM32(src + 0x1B0), MEM32(src + 0x274));
    }
    fprintf(stderr, "[DEF-HIT] t=%.2f victim=%s hp=%d def=%s act=%d abs_on=%d dmg=%.0f fl=%02X splash=%u type=%u(%s) heat=%.1f %s\n",
            now, vw, hp, k_defc_name[d.cls], d.active, d.abs_on, MEMF(info + 0xB0), MEM8(info + 0xB4), MEM8(info + 0xB4) & 1u,
            dt, dt < 5 ? k_dmg_name[dt] : "?", MEMF(info + 0xC4), srcbuf);
}

/* sub_000E9A10 (MAMover vt+0x1CC Absorb(ecx=mover, dmg, type, splash) -> damage passed). */
static struct { uint32_t self; int dmg, type, splash; } g_def_abs;
void ma2_def_absorb_pre(uint32_t self, uint32_t dmg, uint32_t type, uint32_t splash)
{
    g_def_abs.self = self; g_def_abs.dmg = (int)dmg; g_def_abs.type = (int)type; g_def_abs.splash = (int)(splash & 0xFF);
}
void ma2_def_absorb_post(uint32_t result)
{
    if (!def_probe_on() || g_def_abs.dmg <= 0 || !def_is_player_actor(g_def_abs.self)) return;   /* skip 0-damage contacts */
    DefView d; double now = MEMD(VA_GAME_TIME);
    if (!def_read(g_def_abs.self, now, &d) || d.cls != DEFC_ABSORB) return;
    if (!def_rate_ok()) return;
    char vw[16]; def_who(g_def_abs.self, vw, sizeof vw);
    fprintf(stderr, "[DEF-ABS] t=%.2f victim=%s dmg=%d type=%d(%s) splash=%d -> passed=%d abs_on=%d act=%d shieldhp=%d/%d dir=%d spl=%d frac=%.2f\n",
            now, vw, g_def_abs.dmg, g_def_abs.type, g_def_abs.type >= 0 && g_def_abs.type < 5 ? k_dmg_name[g_def_abs.type] : "?",
            g_def_abs.splash, (int)result, d.abs_on, d.active, d.abs_hp, d.abs_max, d.abs_dir, d.abs_spl,
            g_def_abs.type >= 0 && g_def_abs.type < 5 ? d.abs_frac[g_def_abs.type] : -1.f);
}

/* Command 12 path (NOT 52: ids 49..55 are the NeuroHack answers): behaviour vt+0xA4 (sub_175952, ecx=behaviour) -> actor vt+0x204(on=1, 0)
 * (MAMech sub_10665B -> MAMover sub_E9CB6) -> defence vt+0x18 SetActive (base sub_189462). */
void ma2_def_cmd_hook(uint32_t beh)
{
    if (!def_probe_on() || !gva_ok(beh, 0x6F0)) return;
    uint32_t a = MEM32(beh + 0xC);
    if (!def_is_player_actor(a) || !(MEMF(beh + 0x168 + 4) != 0.f && MEMF(beh + 0x168 + 0x10) == 0.f)) return;   /* rising edge only */
    char who[16]; def_who(a, who, sizeof who);
    fprintf(stderr, "[DEF-CMD] t=%.2f who=%s beh=%08X e4=%u b4=%u rec12=%.1f/%.1f\n", MEMD(VA_GAME_TIME), who, beh,
            MEM8(beh + 0x6E4), MEM8(beh + 0x6B4), MEMF(beh + 0x168 + 4), MEMF(beh + 0x168 + 0x10));
}
/* caller (return address): 0x175990 = behaviour command 12; 0x1B4A55 (also 0x1B99A8/0x1C115C/0x1C6872/
 * 0x1C7DD4) = campaign AI; 0x1896B6/0x189764 = defence Update (duration over / rider rules);
 * 0x10669F = MAMech vt+0x204 forwarding to MAMover sub_E9CB6 */
static void def_set_log(const char *tag, uint32_t a, uint32_t on, uint32_t arg2, uint32_t ret)
{
    if (!def_probe_on() || !def_is_player_actor(a)) return;
    char who[16]; def_who(a, who, sizeof who);
    uint32_t o = MEM32(a + 0x220);
    fprintf(stderr, "[%s] t=%.2f who=%s on=%u arg2=%u caller=%08X fl=%02X e1C.17=%u m268.31=%u\n", tag, MEMD(VA_GAME_TIME), who, on & 0xFF, arg2 & 0xFF, ret,
            gva_ok(o, 8) ? MEM8(o + 4) : 0xFFu, (MEM32(a + 0x1C) >> 17) & 1, (MEM32(a + 0x26C) >> (0x31 - 32)) & 1);
}
void ma2_def_mechset_hook(uint32_t mech, uint32_t on, uint32_t arg2, uint32_t ret) { def_set_log("DEF-MSET", mech, on, arg2, ret); }
void ma2_def_set_hook(uint32_t mover, uint32_t on, uint32_t arg2, uint32_t ret) { def_set_log("DEF-SET", mover, on, arg2, ret); }
void ma2_def_act_hook(uint32_t def, uint32_t owner, uint32_t on)
{
    if (!def_probe_on() || !def_is_player_actor(owner) || !gva_ok(def, 8)) return;
    char who[16]; def_who(owner, who, sizeof who);
    fprintf(stderr, "[DEF-ACT] t=%.2f who=%s cls=%s on=%u fl=%02X\n", MEMD(VA_GAME_TIME), who, k_defc_name[def_class_of_vt(MEM32(def))], on & 0xFF, MEM8(def + 4));
}

/* ==================================================================================
 * Bot decision helpers (DEFENSE_SPEC.md "Bot decision table")
 * ================================================================================== */

/* Weapon type = barrel+0x17C (BarrelClassName), 0-based, in the order of the HUD icon table built by
 * sub_F3016 (acannon_3.tif ... lasercannon_3.tif). Checked against 54 weapon templates (CAT-GRP, run c1). */
static const char *const k_wtype_name[18] = {
    "autocannon", "crossbow", "gaussrifle", "javelin", "laser", "machinegun", "ppc", "pulselaser", "warhammer",
    "lavagun", "flamer", "flamethrower", "mortar", "magrail", "antimatterppc", "alphastrike", "povmissile", "lasercannon" };
#define WTYPE_NAME(t) ((uint32_t)(t) < 18u ? k_wtype_name[(uint32_t)(t)] : "?")

/* What the target's defensive system is, in bot terms. */
enum Ma2DefKind {
    DK_NONE = 0,
    DK_SHIELD,          /* type 1: MAEffectShield bubble while up                                   [S]  */
    DK_WPN4,            /* type 3: no object; the "defence" fires arsenal fire class 4              [S]  */
    DK_JAMMER,          /* type 4: Target Jamming, no lock/lead/homing against it while up          [S]  */
    DK_CLOAK,           /* type 5: Null Signature / camouflage                                      [V]  */
    DK_ABSORB_ENERGY,   /* type 6, Energy (and Flame) fractions only: Reflective Armor              [V]  */
    DK_ABSORB_BALLISTIC,/* type 6, Ballistic fraction only: Reactive Armor                          [H]  */
    DK_ABSORB_ALL,      /* type 6, every type >= 0.5 and splash too: Enhanced Armor Shielding        [H]  */
    DK_ABSORB_OTHER,    /* type 6, any other mix                                                         */
    DK_AMS              /* type 7: Active AMS                                                       [V]  */
};
static const char *const k_dk_name[] = { "none", "shield", "wpn4", "jammer", "cloak", "absorb-energy",
                                         "absorb-ballistic", "absorb-all", "absorb-other", "ams" };

typedef struct DefKind {
    int kind;           /* enum Ma2DefKind                                                      */
    int up;             /* active now (flag 2); absorber: also absorbing (+0x94)                 */
    int recharging;     /* flag 1                                                               */
    float left;         /* s until it drops (up) or until it can be raised again (recharging)    */
    int cloaked;        /* camo: sub_E9E51(strict=0): AI only sees it within 50 m (65 m)          */
    int cloak_strict;   /* camo: sub_E9E51(strict=1): the campaign AI ignores it at any range     */
    float visibility;   /* camo +0x24: 0 hidden .. 1 visible                                     */
    float shimmer;      /* camo +0x28: render alpha (speed / heat shimmer), 0..1                 */
    int no_clamp;       /* a BattleArmor cannot clamp it now (sub_EB837: flags 0x40/0x20 while up) */
} DefKind;

static int def_absorb_kind(const DefView *d)
{
    int e = d->abs_frac[1] > 0.f || d->abs_frac[4] > 0.f, b = d->abs_frac[2] > 0.f, all = 1;
    for (int t = 0; t < 5; t++) if (d->abs_frac[t] < 0.5f) all = 0;
    if (all && d->abs_spl) return DK_ABSORB_ALL;
    if (e && !b && d->abs_frac[0] <= 0.f && d->abs_frac[3] <= 0.f) return DK_ABSORB_ENERGY;
    if (b && !e && d->abs_frac[0] <= 0.f && d->abs_frac[3] <= 0.f) return DK_ABSORB_BALLISTIC;
    return DK_ABSORB_OTHER;
}

static int def_kind(uint32_t target, double now, DefKind *k)
{
    DefView d;
    memset(k, 0, sizeof *k);
    if (!def_read(target, now, &d)) {
        if (target && gva_ok(target, 0x230) && is_kind(target, g_cls.mover) && MEM32(target + 0x21C) == 3) k->kind = DK_WPN4;
        return k->kind;
    }
    switch (d.cls) {
    case DEFC_SHIELD: k->kind = DK_SHIELD; break;
    case DEFC_TYPE4:  k->kind = DK_JAMMER; break;
    case DEFC_CAMO:   k->kind = DK_CLOAK; break;
    case DEFC_ABSORB: k->kind = def_absorb_kind(&d); break;
    case DEFC_AMS:    k->kind = DK_AMS; break;
    default:          k->kind = DK_NONE; break;
    }
    k->up = d.active && (d.cls != DEFC_ABSORB || d.abs_on);
    k->recharging = d.recharging;
    k->left = d.left > 0.f ? d.left : 0.f;
    if (d.cls == DEFC_CAMO) {
        float thr = MEMF(0x50C968u);
        k->visibility = MEMF(d.obj + 0x24);
        k->shimmer = clampf(MEMF(d.obj + 0x28), 0.f, 1.f);
        k->cloaked = d.active && k->visibility <= thr && MEMF(d.obj + 0x54) <= 0.f;
        k->cloak_strict = k->cloaked && MEMF(d.obj + 0x50) <= 0.f;
    }
    /* sub_EB837 -> slots 9/10: base 0x1822CC/DE (flag && active), camo 0x1822F4/31D (also fully cloaked) */
    k->no_clamp = d.active && (d.flags & 0x60) != 0 && (d.cls != DEFC_CAMO || k->cloaked);
    return k->kind;
}

/* Static properties of a fire group (MAWpnGrp w) for weapon choice. */
typedef struct WpnProps {
    int fire_class, level, wtype;            /* w+0x80, w+0x98 (WpnUpgradeLevel), barrel+0x17C              */
    int direct, splash, dtype, ams;          /* see wpn_class                                               */
    float radius, range;                     /* barrel+0x158, +0x128 (the AI's engage range)                */
    int seeker, swarmer, remote, lobbed, beam, flame, charge, fused, sniper, knockdown;
    float heat_shot, heat_sec, fire_ms;      /* barrel+0x168 BarrelHeats2, +0xE8 HeatPerSecond, +0x180 ms     */
    float ttl, seek_dist, seek_turn;         /* projectile TimeLimit +0x26C, SeekDist +0x298, SeekTurn +0x2E0  */
    float kd_general, kd_leg, heat_target;   /* projectile +0x384/+0x388 KnockDownMult, +0x364 HeatToApply    */
    float charge_rate, charge_min, charge_hold, fizzle_heat;   /* barrel +0x80, +0x98, +0xA0, +0x170        */
    int fizzle;                              /* barrel+0x78 bit 26 FizzleOnOverCharge                        */
    float fuse_min, fuse_max;                /* barrel +0xD0/+0xD4 (BarrelFusedAmmo)                         */
    int burst, ammo, arcs, nearby_dmg;       /* barrel +0x188 BurstShots, +0x164 CurAmmo; proj bit 14 Arcs, +0x374 */
} WpnProps;

static int wpn_props(uint32_t w, WpnProps *p)
{
    WpnClass c;
    memset(p, 0, sizeof *p); p->dtype = -1;
    if (!wpn_class(w, &c)) return 0;
    uint32_t b = MEM32(w + 0xC), t = wpn_template(w), bf = MEM32(b + 0x78);
    p->fire_class = (int)MEM32(w + 0x80); p->level = (int)MEM32(w + 0x98);
    p->wtype = (int)MEM32(b + 0x17C);
    p->direct = c.direct; p->splash = c.splash; p->dtype = c.dtype; p->ams = c.ams; p->radius = c.radius;
    p->range = MEMF(b + 0x128);
    p->lobbed = (bf >> 1) & 1; p->fused = (bf >> 2) & 1; p->charge = (bf >> 3) & 1; p->beam = (bf >> 16) & 1;
    p->flame = (bf >> 18) & 1; p->sniper = (bf >> 24) & 1; p->fizzle = (bf >> 26) & 1;
    p->heat_shot = MEMF(b + 0x168); p->heat_sec = MEMF(b + 0xE8); p->fire_ms = (float)(int)MEM32(b + 0x180);
    p->charge_rate = (float)(int)MEM32(b + 0x80); p->charge_min = (float)(int)MEM32(b + 0x98);
    p->charge_hold = MEMF(b + 0xA0); p->fizzle_heat = MEMF(b + 0x170);
    p->fuse_min = MEMF(b + 0xD0); p->fuse_max = MEMF(b + 0xD4);
    p->burst = (int)MEM32(b + 0x188); p->ammo = (int)MEM32(b + 0x164);
    if (t) {
        uint32_t pf = MEM32(t + 0x260);
        p->remote = (pf >> 1) & 1; p->seeker = (pf >> 3) & 1; p->swarmer = (pf >> 4) & 1; p->knockdown = (pf >> 9) & 1;
        p->ttl = MEMF(t + 0x26C); p->seek_dist = MEMF(t + 0x298); p->seek_turn = MEMF(t + 0x2E0);
        p->kd_general = MEMF(t + 0x384); p->kd_leg = MEMF(t + 0x388); p->heat_target = MEMF(t + 0x364);
        p->arcs = (pf >> 14) & 1; p->nearby_dmg = (int)MEM32(t + 0x374);
    }
    return 1;
}

/* Worth (0..1) of firing fire group w at target from dist metres right now:
 * 0 out of BarrelRange; times the share the target's defence lets through (def_weapon_stopped);
 * homing weapons lose their lock and lead against a raised jammer (sub_18482E/17B222) -> 0.6 [H weight];
 * a strictly cloaked target cannot be engaged by the AI fire at all -> 0 (bots: close to < 50 m or switch). */
static float wpn_value_vs(uint32_t target, uint32_t w, float dist, double now)
{
    WpnProps p; DefKind k;
    if (!wpn_props(w, &p) || (p.range > 10.f && dist > p.range)) return 0.f;
    float v = 1.f - def_weapon_stopped(target, w, now);
    def_kind(target, now, &k);
    if (k.kind == DK_JAMMER && k.up && p.seeker) v *= 0.6f;
    if (k.kind == DK_CLOAK && k.cloak_strict) v = 0.f;
    return v;
}
/* Own defensive system can be raised now (command 12 will not be refused): present, idle, not recharging. */
static int def_own_ready(uint32_t me, double now)
{
    DefView d;
    return def_read(me, now, &d) && !d.active && !d.recharging;
}

/* Alpha Strike (fire class 7): ready when classes 0..2 are each at the highest WpnUpgradeLevel the
 * arsenal holds for them (sub_12AE8E; sub_12B71A = max level of a class) and a class-7 MAWpnGrp
 * exists at the arsenal's class-7 level (sub_12B749). The game then auto-selects class 7 (sub_DF238:
 * actor vt+0x108(7), cue "Mech.AlphaEnabled"). Fire it like any group: ai_fire(ai, class-7 wpngrp).
 * Returns 1 when ready; *grp = the class-7 MAWpnGrp. */
static int arsenal_max_level(uint32_t ars, int cls)
{
    int m = 0;
    uint32_t wcls = MEM32(0x004EAF4Cu);
    for (uint32_t w = MEM32(ars + 0xC), n = 0; gva_ok(w, 0xA0) && n < 64; w = MEM32(w + 8), n++)
        if (MEM32(w + 0x28) == wcls && (int)MEM32(w + 0x80) == cls && (int)MEM32(w + 0x98) > m) m = (int)MEM32(w + 0x98);
    return m;
}
static int arsenal_min_level(uint32_t ars, int cls)
{
    int m = -1;
    uint32_t wcls = MEM32(0x004EAF4Cu);
    for (uint32_t w = MEM32(ars + 0xC), n = 0; gva_ok(w, 0xA0) && n < 64; w = MEM32(w + 8), n++)
        if (MEM32(w + 0x28) == wcls && (int)MEM32(w + 0x80) == cls && (m < 0 || (int)MEM32(w + 0x98) < m)) m = (int)MEM32(w + 0x98);
    return m;
}
static int alpha_ready(uint32_t actor, uint32_t *grp)
{
    if (grp) *grp = 0;
    uint32_t ars = gva_ok(actor, 0xA8) ? MEM32(actor + 0xA4) : 0;
    if (!gva_ok(ars, 0xA0)) return 0;
    uint32_t a7 = def_group_wpn(actor, 7);
    if (!a7 || MEM32(a7 + 0x98) != MEM32(ars + 0x7C + 4u * 7u)) return 0;
    if (grp) *grp = a7;
    for (int c = 0; c < 3; c++)
        if ((int)MEM32(ars + 0x7C + 4u * (uint32_t)c) != arsenal_max_level(ars, c)) return 0;
    return 1;
}
/* 'Mech heat (MAMech): +0x85C current, +0x850 HeatMax, +0x858 HeatCoolRate, +0x864 percent (int);
 * at >= 100 % sub_12BBEB reports "overheated" to the fire path (sub_12B234). */
static float mech_heat_pct(uint32_t a) { return gva_ok(a, 0x870) && MEM32(a + 0x28) == g_cls.mech ? (float)(int)MEM32(a + 0x864) : -1.f; }

/* ==================================================================================
 * Probe-only RE dumps: template catalog, projectile speeds, alpha-strike test
 * ================================================================================== */
static void def_cat_name(uint32_t t, char *out, size_t n)
{
    out[0] = 0;
    static const uint32_t offs[] = { 0xB84, 0xB94, 0xB64, 0xB74 };   /* HUDFriendIconImage / HUDEnemyIconImage (std::string) */
    for (int i = 0; i < 4; i++) {
        uint32_t s = gva_ok(t, offs[i] + 8) ? MEM32(t + offs[i]) : 0, e = s ? MEM32(t + offs[i] + 4) : 0;
        if (!gva_ok(s, 4) || e <= s || e - s > 120) continue;
        size_t k = 0; int ok = 1;
        for (uint32_t p = s; p < e && k + 1 < n; p++) { uint8_t ch = MEM8(p); if (ch < 32 || ch > 126) { ok = 0; break; } out[k++] = (char)ch; }
        out[k] = 0;
        if (ok && k > 3) return;
        out[0] = 0;
    }
}
static void def_cat_barrel(const char *tag, uint32_t b, uint32_t t)
{
    if (!gva_ok(b, 0x200)) return;
    uint32_t bf = MEM32(b + 0x78), wt = MEM32(b + 0x17C);
    fprintf(stderr, "%s barrel wtype=%u(%s) range=%.0f dmg=%d splash=%d r=%.1f fl=%08X lob=%u fused=%u charge=%u beam=%u flame=%u sniper=%u fizzle=%u heat=%.2f heat_s=%.2f fire_ms=%d burst=%d/%d ammo=%d/%d chg_rate=%d chg_min=%d chg_hold=%.2f fizzle_heat=%.2f fuse=%.2f..%.2f lock=%.2f nproj=%d",
            tag, wt, WTYPE_NAME(wt), MEMF(b + 0x128), (int)MEM32(b + 0x148), (int)MEM32(b + 0x14C), MEMF(b + 0x158), bf,
            (bf >> 1) & 1, (bf >> 2) & 1, (bf >> 3) & 1, (bf >> 16) & 1, (bf >> 18) & 1, (bf >> 24) & 1, (bf >> 26) & 1,
            MEMF(b + 0x168), MEMF(b + 0xE8), (int)MEM32(b + 0x180), (int)MEM32(b + 0x188), (int)MEM32(b + 0x18C), (int)MEM32(b + 0x160), (int)MEM32(b + 0x164),
            (int)MEM32(b + 0x80), (int)MEM32(b + 0x98), MEMF(b + 0xA0), MEMF(b + 0x170), MEMF(b + 0xD0), MEMF(b + 0xD4), MEMF(b + 0x13C), (int)MEM32(b + 0x1F8));
    if (gva_ok(t, 0x390)) {
        uint32_t pf = MEM32(t + 0x260);
        fprintf(stderr, " | proj dt=%u(%s) pfl=%08X seek=%u swarm=%u remote=%u large=%u kd=%u arcs=%u ams=%u ttl=%.2f minspd=%.1f seekdist=%.1f seekturn=%.2f startH=%.1f startV=%.1f sspeed=%.1f sdist=%.1f kdG=%.2f kdL=%.2f heat_tgt=%.2f slow=%.2f/%.2f nearby=%d",
                MEM32(t + 0x370), MEM32(t + 0x370) < 5 ? k_dmg_name[MEM32(t + 0x370)] : "?", pf, (pf >> 3) & 1, (pf >> 4) & 1, (pf >> 1) & 1, (pf >> 7) & 1,
                (pf >> 9) & 1, (pf >> 14) & 1, (pf >> 17) & 1, MEMF(t + 0x26C), MEMF(t + 0x268), MEMF(t + 0x298), MEMF(t + 0x2E0), MEMF(t + 0x310), MEMF(t + 0x31C),
                MEMF(t + 0x32C), MEMF(t + 0x328), MEMF(t + 0x384), MEMF(t + 0x388), MEMF(t + 0x364), MEMF(t + 0x358), MEMF(t + 0x35C), (int)MEM32(t + 0x374));
    }
    fputc('\n', stderr);
}
/* RECOMP_DEFENSE_CATALOG=1: once per match, every vehicle template in the template world
 * (MEM32(world+0x24), sub_B5B40) with its defence object, plus every MAWpnGrp template. */
static void def_catalog(void)
{
    static double done_at = -1.0;
    const Ma2World *w = &g_rt.world;
    if (!env_int("RECOMP_DEFENSE_CATALOG", 0) || w->time < 3.0 || (done_at >= 0.0 && w->time >= done_at)) return;
    uint32_t a0 = 0;
    for (int i = 0; i < w->nplayers && !a0; i++) a0 = w->players[i].actor;
    uint32_t world = a0 ? MEM32(a0 + 0x60) : 0, tw = gva_ok(world, 0x30) ? MEM32(world + 0x24) : 0;
    if (!gva_ok(tw, 0x40)) return;
    done_at = w->time;
    uint32_t b = MEM32(tw + 0x34), e = MEM32(tw + 0x38);
    if (e <= b || e - b > 0x100000u || !gva_ok(b, e - b)) return;
    uint32_t wcls = MEM32(0x004EAF4Cu);
    int nveh = 0, ngrp = 0, nall = 0;
    for (uint32_t bk = b; bk < e; bk += 4)
        for (uint32_t node = MEM32(bk), guard = 0; node && gva_ok(node, 12) && guard < 4096; node = MEM32(node), guard++) {
            uint32_t uid = MEM32(node + 4), t = MEM32(node + 8);
            if (!gva_ok(t, 0x30) || !gva_ok(MEM32(t), 0x58)) continue;
            nall++;
            if (MEM32(t + 0x28) == wcls) {
                ngrp++;
                uint32_t bt = uid_lookup(tw, MEM32(t + 0x78)), pt = uid_lookup(tw, MEM32(t + 0x7C));
                uint32_t par = MEM32(t + 4), gpar = gva_ok(par, 0x30) ? MEM32(par + 4) : 0;
                char tag[200];
                snprintf(tag, sizeof tag, "[CAT-GRP] uid=%08X class=%d level=%d parent=%08X/%08X(%08X) btmpl=%08X ptmpl=%08X", uid, (int)MEM32(t + 0x80),
                         (int)MEM32(t + 0x98), gva_ok(par, 0x30) ? MEM32(par + 0x2C) : 0, gva_ok(gpar, 0x30) ? MEM32(gpar + 0x2C) : 0,
                         gva_ok(gpar, 0x30) ? MEM32(gpar + 0x28) : 0, MEM32(t + 0x78), MEM32(t + 0x7C));
                def_cat_barrel(tag, gva_ok(bt, 0x200) ? bt : MEM32(t + 0xC), pt);
                continue;
            }
            if (!is_kind(t, g_cls.mover) || is_kind(t, g_cls.projectile) || !gva_ok(t, 0x4D0)) continue;
            nveh++;
            char name[128]; def_cat_name(t, name, sizeof name);
            int kind = actor_kind(t);
            uint32_t o = MEM32(t + 0x220);
            char dbuf[400] = "";
            int cls = gva_ok(o, 0xB0) ? def_class_of_vt(MEM32(o)) : 0;
            if (cls) {
                int n = snprintf(dbuf, sizeof dbuf, " def=%s fl=%02X dur=%.1f rech=%.1f", k_defc_name[cls], MEM8(o + 4), MEMF(o + 8), MEMF(o + 0xC));
                if (cls == DEFC_ABSORB)
                    snprintf(dbuf + n, sizeof dbuf - n, " absorb dir=%u spl=%u pf=%u heat=%.2f maxhp=%d rec=%.2f/%.2f frac=%.2f/%.2f/%.2f/%.2f/%.2f dmgfac=%.2f/%.2f/%.2f/%.2f/%.2f passon=%u%u%u%u%u",
                             MEM8(o + 0x39), MEM8(o + 0x38), MEM8(o + 0x30), MEMF(o + 0x34), (int)MEM32(o + 0x24), MEMF(o + 0x28), MEMF(o + 0x2C),
                             MEMF(o + 0x48), MEMF(o + 0x4C), MEMF(o + 0x50), MEMF(o + 0x54), MEMF(o + 0x58), MEMF(o + 0x5C), MEMF(o + 0x60), MEMF(o + 0x64),
                             MEMF(o + 0x68), MEMF(o + 0x6C), MEM8(o + 0x84), MEM8(o + 0x85), MEM8(o + 0x86), MEM8(o + 0x87), MEM8(o + 0x88));
                else if (cls == DEFC_SHIELD) snprintf(dbuf + n, sizeof dbuf - n, " shield tmpl=%08X", MEM32(o + 0x28));
                else if (cls == DEFC_AMS) snprintf(dbuf + n, sizeof dbuf - n, " ams tmpl=%08X", MEM32(o + 0x24));
                else if (cls == DEFC_CAMO) snprintf(dbuf + n, sizeof dbuf - n, " camo mode=%d", gva_ok(MEM32(o + 0x3C), 4) ? (int)MEM32(MEM32(o + 0x3C)) : -1);
            }
            uint32_t lo = uid_lookup(tw, MEM32(t + 0xA0));      /* WeaponLoadout (MACorporeal ser +0x80) */
            fprintf(stderr, "[CAT-VEH] uid=%08X cls=%08X kind=%s name=%s dtype=%d heatmax=%.0f cool=%.1f loadout=%08X%s\n", uid, MEM32(t + 0x28), kind_name(kind),
                    name[0] ? name : "?", (int)MEM32(t + 0x21C), kind == MA2_KIND_MECH || kind == MA2_KIND_BATTLEARMOR ? MEMF(t + 0x850) : -1.f,
                    kind == MA2_KIND_MECH || kind == MA2_KIND_BATTLEARMOR ? MEMF(t + 0x858) : -1.f, MEM32(t + 0xA0), dbuf);
            /* MAWeaponLoadout children: MAWpnGrpRef [0x4EB018] +0x64 WeaponGroupRef, +0x78 WeaponFireClass, +0x7C WeaponUpgradeLevel */
            uint32_t refcls = MEM32(0x004EB018u);
            for (uint32_t c = gva_ok(lo, 0x30) ? MEM32(lo + 0xC) : 0, nn = 0; gva_ok(c, 0x80) && nn < 64; c = MEM32(c + 8), nn++) {
                if (MEM32(c + 0x28) != refcls) continue;
                uint32_t g = uid_lookup(tw, MEM32(c + 0x64));
                if (!gva_ok(g, 0xA0)) g = uid_lookup(tw, MEM32(c + 0x44));
                if (!gva_ok(g, 0xA0)) { fprintf(stderr, "[CAT-WPN] veh=%08X ref=%08X unresolved %08X/%08X\n", uid, c, MEM32(c + 0x64), MEM32(c + 0x44)); continue; }
                char tag[160];
                snprintf(tag, sizeof tag, "[CAT-WPN] veh=%08X class=%d level=%d grp=%08X", uid, (int)MEM32(c + 0x78), (int)MEM32(c + 0x7C), MEM32(c + 0x64));
                def_cat_barrel(tag, uid_lookup(tw, MEM32(g + 0x78)), uid_lookup(tw, MEM32(g + 0x7C)));
            }
        }
    fprintf(stderr, "[CAT] templates=%d vehicles=%d wpngrps=%d\n", nall, nveh, ngrp);
}

/* RECOMP_DEFENSE_CATALOG=1: speed of each projectile template in flight (two samples one step apart). */
static void def_proj_speeds(void)
{
    static struct { uint32_t uid, tmpl; Ma2Vec3 p; double t; } seen[64];
    static uint32_t logged[64]; static int nlogged;
    const Ma2World *w = &g_rt.world;
    if (!env_int("RECOMP_DEFENSE_CATALOG", 0) || nlogged >= 64) return;
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4), root = gva_ok(r1, 8) ? MEM32(r1 + 4) : 0;
    if (!gva_ok(root, 0x30)) return;
    uint32_t stack[512]; int sp = 0, visited = 0;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192) {
        uint32_t ent = stack[--sp]; visited++;
        uint32_t sib = MEM32(ent + 8), child = MEM32(ent + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(ent), 0x58) || !gva_ok(ent, 0x390) || MEM32(ent + 0x28) != g_cls.projectile) continue;
        uint32_t uid = MEM32(ent + 0x2C), tmpl = MEM32(ent + 0x30);
        int done = 0;
        for (int i = 0; i < nlogged; i++) if (logged[i] == tmpl) done = 1;
        if (done) continue;
        Ma2Vec3 p = actor_pos(ent);
        int slot = (int)(uid % 64u);
        if (seen[slot].uid == uid && w->time > seen[slot].t && w->time - seen[slot].t < 0.2) {
            float dt = (float)(w->time - seen[slot].t), dx = p.x - seen[slot].p.x, dy = p.y - seen[slot].p.y, dz = p.z - seen[slot].p.z;
            float v = sqrtf(dx * dx + dy * dy + dz * dz) / dt;
            char ow[16] = "?"; uint32_t owner = def_actor_by_uid(MEM32(ent + 0x2B8)); if (owner) def_who(owner, ow, sizeof ow);
            fprintf(stderr, "[DEF-PSPD] tmpl=%08X owner=%s dt=%u speed=%.1f ttl=%.2f -> max_flight=%.0f m direct=%d splash=%d r=%.1f pfl=%08X\n", tmpl, ow,
                    MEM32(ent + 0x370), v, MEMF(ent + 0x26C), v * MEMF(ent + 0x26C), (int)MEM32(ent + 0x2A4), (int)MEM32(ent + 0x274), MEMF(ent + 0x270), MEM32(ent + 0x260));
            logged[nlogged++] = tmpl;
            continue;
        }
        seen[slot].uid = uid; seen[slot].tmpl = tmpl; seen[slot].p = p; seen[slot].t = w->time;
    }
}

/* RECOMP_DEFENSE_TEST_ALPHA="k:t": from time t, bot k's arsenal classes 0..2 are raised to their max level
 * (direct write of arsenal+0x7C+4c, as sub_12AE4D would after power-ups) and the class-7 group is fired at
 * its target through the AI fire (ai_fire -> sub_18482E) every step for 20 s. Logs [DEF-ALPHA]. */
static uint32_t actor_ai(uint32_t a);
static int ai_fire(uint32_t ai, uint32_t weapon, uint32_t tgt, Ma2Vec3 from);
static void def_alpha_topup(uint32_t me)
{
    recomp_func_t up = resolve(0x000DF238u);
    uint32_t ars = gva_ok(me, 0xA8) ? MEM32(me + 0xA4) : 0;
    if (!up || !gva_ok(ars, 0xA0)) return;
    for (int c = 0; c < 3; c++)
        if ((int)MEM32(ars + 0x7C + 4u * (uint32_t)c) < arsenal_max_level(ars, c)) {
            GuestRegs r; regs_save(&r);
            PUSH32(g_esp, 0xFFFFFFFFu); PUSH32(g_esp, 1u); PUSH32(g_esp, (uint32_t)c); PUSH32(g_esp, me);
            PUSH32(g_esp, 0x00132085u); up();
            regs_restore(&r);
            break;
        }
}
static void def_alpha_test(void)
{
    static int k = -2; static float t0; static double until, last_log; static int raised, shots;
    if (k == -2) { const char *e = getenv("RECOMP_DEFENSE_TEST_ALPHA"); k = -1; if (e && sscanf(e, "%d:%f", &k, &t0) != 2) k = -1; }
    const Ma2World *w = &g_rt.world;
    if (k != -1 && getenv("RECOMP_DEFENSE_TEST_ALPHA_ALL") && w->time >= t0) {   /* every bot 'Mech that can */
        static double next_all;
        if (w->time >= next_all) {
            next_all = w->time + 0.5;
            for (int j = 0; j < g_rt.nbots; j++)
                for (int i = 0; i < w->nplayers; i++)
                    if (w->players[i].player == g_rt.bots[j].player && w->players[i].kind == MA2_KIND_MECH && w->players[i].alive &&
                        def_group_wpn(w->players[i].actor, 7) && mech_heat_pct(w->players[i].actor) < 50.f)
                        def_alpha_topup(w->players[i].actor);
        }
    }
    if (k == 99 && w->time >= t0)                        /* k=99: the first bot in a 'Mech that has a class-7 group */
        for (int j = 0; j < g_rt.nbots && k == 99; j++)
            for (int i = 0; i < w->nplayers; i++)
                if (w->players[i].player == g_rt.bots[j].player && w->players[i].kind == MA2_KIND_MECH && def_group_wpn(w->players[i].actor, 7)) { k = j; break; }
    if (k < 0 || k >= g_rt.nbots || w->time < t0 || (until > 0.0 && w->time > until)) return;
    const Bot *bk = &g_rt.bots[k];
    uint32_t me = 0; Ma2Vec3 pos = { 0, 0, 0 };
    for (int i = 0; i < w->nplayers; i++) if (w->players[i].player == bk->player) { me = w->players[i].actor; pos = w->players[i].pos; }
    if (!me || actor_kind(me) != MA2_KIND_MECH) return;
    uint32_t ars = MEM32(me + 0xA4), g7 = def_group_wpn(me, 7);
    if (!gva_ok(ars, 0xA0) || !g7) {
        if (w->time - last_log >= 5.0) { last_log = w->time; fprintf(stderr, "[DEF-ALPHA] t=%.1f bot=B%d has no class-7 group\n", w->time, k); }
        return;
    }
    if (!raised) {
        raised = 1; until = w->time + 25.0;
        fprintf(stderr, "[DEF-ALPHA] t=%.1f bot=B%d before levels=%d/%d/%d l7=%d max=%d/%d/%d g7lvl=%d ready=%d heat=%.0f%%\n", w->time, k,
                (int)MEM32(ars + 0x7C), (int)MEM32(ars + 0x80), (int)MEM32(ars + 0x84), (int)MEM32(ars + 0x98),
                arsenal_max_level(ars, 0), arsenal_max_level(ars, 1), arsenal_max_level(ars, 2), (int)MEM32(g7 + 0x98), alpha_ready(me, NULL), mech_heat_pct(me));
    }
    /* Raise the levels the way a weapon power-up does (sub_13200F -> sub_DF238(actor, class, notify, -1)
     * ret 0x10): that also selects class 7 (vt+0x108(7), cue Mech.AlphaEnabled) once all three are maxed.
     * A hit drops each reloaded class a level again (sub_E9A5F -> ... -> sub_12AD89): keep topping up. */
    {
        static double next_up;
        recomp_func_t up = resolve(0x000DF238u);
        if (up && w->time >= next_up) {
            next_up = w->time + 0.5;
            for (int c = 0; c < 3; c++)
                if ((int)MEM32(ars + 0x7C + 4u * (uint32_t)c) < arsenal_max_level(ars, c)) {
                    GuestRegs r; regs_save(&r);
                    PUSH32(g_esp, 0xFFFFFFFFu); PUSH32(g_esp, 1u); PUSH32(g_esp, (uint32_t)c); PUSH32(g_esp, me);
                    PUSH32(g_esp, 0x00132085u); up();
                    regs_restore(&r);
                    break;
                }
        }
    }
    uint32_t grp = 0; int ready = alpha_ready(me, &grp);
    uint32_t tgt = bk->ctrl.tgt, ai = actor_ai(me);
    { extern uint32_t g_ma2_trace_launcher; g_ma2_trace_launcher = gva_ok(g7, 0x10) ? MEM32(g7 + 0xC) : 0; }
    static int input = -1, held; static float heat_prev;
    if (input < 0) input = getenv("RECOMP_DEFENSE_TEST_ALPHA_INPUT") ? atoi(getenv("RECOMP_DEFENSE_TEST_ALPHA_INPUT")) : 0;
    if (input && ready && tgt && ai) {
        /* The player's way: hold the trigger (action `input`) through the charge, torso on the target,
         * and run the player's fire step on it (ctrl_player_trigger); release after 6 s. */
        recomp_func_t aim = resolve(0x0018AFDBu);
        if (aim) { GuestRegs r; regs_save(&r); g_eax = ai; PUSH32(g_esp, tgt); PUSH32(g_esp, 0x001B44D9u); aim(); regs_restore(&r); }
        BotCtrl *cc = (BotCtrl *)&bk->ctrl;
        int phase = (int)fmod(w->time - t0, 8.0);
        if (phase < 6) { cc->press[input] = 2; held++; }
        cc->trig_act = input;
        float hp = mech_heat_pct(me);
        if (hp > heat_prev + 30.f) { shots++; fprintf(stderr, "[DEF-ALPHA] t=%.1f bot=B%d heat jump %.0f%% -> %.0f%% (held %d steps)\n", w->time, k, heat_prev, hp, held); }
        heat_prev = hp;
    } else if (ready && grp && tgt && ai && !getenv("RECOMP_DEFENSE_TEST_ALPHA_BRAIN")) {
        Ma2Vec3 from = pos; from.y += 8.f;
        int r = ai_fire(ai, grp, tgt, from);
        if (r == 1) shots++;
    }
    if (w->time - last_log >= 1.0) {
        last_log = w->time;
        uint32_t b = MEM32(grp ? grp + 0xC : g7 + 0xC);
        { uint32_t L = MEM32(g7 + 0xC); if (gva_ok(L, 0x1C0)) fprintf(stderr, "[DEF-ALPHA] launcher %08X fl=%08X r88=%d min=%.2f chg=%.2f b1bf=%u rc=%d\n", L, MEM32(L + 0x78), (int)MEM32(L + 0x88), MEMF(L + 0xA0), MEMF(L + 0xA4), MEM8(L + 0x1BF), shots); }
        fprintf(stderr, "[DEF-ALPHA] t=%.1f bot=B%d ready=%d tgt=%08X levels=%d/%d/%d l7=%d sel=%d heat=%.0f%% fired=%d barrel_fl=%08X\n", w->time, k, ready, tgt,
                (int)MEM32(ars + 0x7C), (int)MEM32(ars + 0x80), (int)MEM32(ars + 0x84), (int)MEM32(ars + 0x98), (int)MEM32(me + 0xF8), mech_heat_pct(me), shots,
                gva_ok(b, 0x80) ? MEM32(b + 0x78) : 0);
    }
}

/* Active AMS effect (obj+0x28 = uid of the spawned MAEffectChain): its bounding radius and offset. */
static void def_ams_live(uint32_t owner, uint32_t o, char *buf, size_t n)
{
    buf[0] = 0;
    uint32_t world = MEM32(owner + 0x60), eff = MEM32(o + 0x28) ? uid_lookup(world, MEM32(o + 0x28)) : 0;
    if (!gva_ok(eff, 0x100)) return;
    uint32_t node = MEM32(eff + 0x94), bs = gva_ok(node, 0xC0) ? MEM32(node + 0xB0) : 0;
    Ma2Vec3 pe = actor_pos(eff), po = actor_pos(owner);
    snprintf(buf, n, " eff=%08X cls=%08X bound_r=%.1f off=%.1f", eff, MEM32(eff + 0x28), gva_ok(bs, 0x14) ? MEMF(bs + 0x10) : -1.f,
             sqrtf((pe.x - po.x) * (pe.x - po.x) + (pe.y - po.y) * (pe.y - po.y) + (pe.z - po.z) * (pe.z - po.z)));
}

/* ---- 1 Hz state log + edges + one weapon dump per actor ---- */
static void defense_probe(void)
{
    static double last; static uint32_t step = 0xFFFFFFFFu;
    static struct { uint32_t actor; int active, recharging, dumped; } st[MA2_WORLD_MAX_PLAYERS];
    if (!def_probe_on() || step == g_rt.world_step) return;
    step = g_rt.world_step;
    def_catalog(); def_proj_speeds(); def_alpha_test();
    const Ma2World *w = &g_rt.world;
    double now = w->time;
    int tick = now - last >= 1.0 || now < last;
    if (tick) last = now;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->actor) continue;
        char who[16]; def_who(p->actor, who, sizeof who);
        if (st[i].actor != p->actor) { st[i].actor = p->actor; st[i].active = st[i].recharging = -1; st[i].dumped = 0; }
        DefView d; int has = def_read(p->actor, now, &d);
        if (!st[i].dumped && p->kind != MA2_KIND_PILOT) { st[i].dumped = 1; def_dump_weapons(p->actor, who, p->kind); }
        if (has && st[i].active >= 0 && (d.active != st[i].active || d.recharging != st[i].recharging))
            fprintf(stderr, "[DEF-EDGE] t=%.2f who=%s cls=%s act %d->%d rech %d->%d left=%.2f fl=%02X abs_on=%d\n",
                    now, who, k_defc_name[d.cls], st[i].active, d.active, st[i].recharging, d.recharging, d.left, d.flags, d.abs_on);
        if (has) { st[i].active = d.active; st[i].recharging = d.recharging; }
        if (!tick || p->kind == MA2_KIND_PILOT) continue;
        if (!has) {
            fprintf(stderr, "[DEF] t=%.1f who=%s kind=%s dtype=%d obj=%08X (no defence object)\n", now, who, kind_name(p->kind),
                    gva_ok(p->actor, 0x230) && is_kind(p->actor, g_cls.mover) ? (int)MEM32(p->actor + 0x21C) : -1,
                    gva_ok(p->actor, 0x230) ? MEM32(p->actor + 0x220) : 0);
            continue;
        }
        char extra[160] = "";
        if (d.cls == DEFC_ABSORB)
            snprintf(extra, sizeof extra, " | abs on=%d hp=%d/%d dir=%d spl=%d pf=%d heat=%.2f frac=%.2f/%.2f/%.2f/%.2f/%.2f",
                     d.abs_on, d.abs_hp, d.abs_max, d.abs_dir, d.abs_spl, d.abs_pf, d.abs_heat,
                     d.abs_frac[0], d.abs_frac[1], d.abs_frac[2], d.abs_frac[3], d.abs_frac[4]);
        else if (d.cls == DEFC_CAMO)
            snprintf(extra, sizeof extra, " | camo f24=%.2f f50=%.2f f54=%.2f g50C968=%.2f cloaked=%d", MEMF(d.obj + 0x24), MEMF(d.obj + 0x50), MEMF(d.obj + 0x54),
                     MEMF(0x50C968u), d.active && MEMF(0x50C968u) >= MEMF(d.obj + 0x24) && MEMF(d.obj + 0x54) <= 0.f);   /* sub_E9E51 */
        else if (d.cls == DEFC_SHIELD)
            snprintf(extra, sizeof extra, " | shield tmpl=%08X radius=%.1f", MEM32(d.obj + 0x28), MEMF(d.obj + 0x24));
        else if (d.cls == DEFC_AMS)
            { char lv[96]; def_ams_live(p->actor, d.obj, lv, sizeof lv);
              snprintf(extra, sizeof extra, " | ams tmpl=%08X node=%08X%s", MEM32(d.obj + 0x24), MEM32(d.obj + 0x2C), lv); }
        fprintf(stderr, "[DEF] t=%.1f who=%s kind=%s hp=%.0f dtype=%d cls=%s fl=%02X act=%d rech=%d left=%.2f dur=%.1f rech_t=%.1f meter=%.2f%s\n",
                now, who, kind_name(p->kind), p->hp, d.dtype, k_defc_name[d.cls], d.flags, d.active, d.recharging, d.left, d.dur, d.rech, d.meter, extra);
    }
    /* What each bot's fire groups would waste on its current target's running defence. */
    for (int k = 0; tick && k < g_rt.nbots; k++) {
        const Bot *bk = &g_rt.bots[k];
        uint32_t me = 0, tgt = bk->ctrl.tgt;
        for (int i = 0; i < w->nplayers; i++) if (w->players[i].player == bk->player) me = w->players[i].actor;
        DefView d;
        if (!me || !tgt || !def_read(tgt, now, &d) || !(d.active || d.abs_on)) continue;
        char tw[16]; def_who(tgt, tw, sizeof tw);
        char buf[200]; int n = 0;
        for (int g = 0; g < 3 && n < (int)sizeof buf - 60; g++) {
            uint32_t wg = def_group_wpn(me, g); WpnClass c;
            if (!wg || !wpn_class(wg, &c)) continue;
            WpnProps wp; wpn_props(wg, &wp);
            n += snprintf(buf + n, sizeof buf - n, " g%d=%s/%s/%s/ams%d:stop%.2f", g, WTYPE_NAME(wp.wtype), c.dtype >= 0 && c.dtype < 5 ? k_dmg_name[c.dtype] : "?",
                          k_wd_name[(c.direct > 0 ? WD_DIRECT : 0) | (c.splash > 0 ? WD_SPLASH : 0)], c.ams, def_weapon_stopped(tgt, wg, now));
        }
        DefKind dk; def_kind(tgt, now, &dk);
        fprintf(stderr, "[DEF-VS] t=%.1f bot=B%d tgt=%s def=%s kind=%s up=%d cloaked=%d/%d noclamp=%d left=%.1f%s\n", now, k, tw, k_defc_name[d.cls],
                k_dk_name[dk.kind], dk.up, dk.cloaked, dk.cloak_strict, dk.no_clamp, d.left, n ? buf : " (no groups)");
    }
}

/* RECOMP_DEFENSE_TEST=<s>: probe-only test hook, a bot in a 'Mech or tank taps the defence command every s seconds. */
static int defense_test_tap(int index, int kind, double now)
{
    static double next[MAX_BOTS];
    static int period = -1;
    if (period < 0) period = env_int("RECOMP_DEFENSE_TEST", 0);
    if (period <= 0 || index < 0 || index >= MAX_BOTS || (kind != MA2_KIND_MECH && kind != MA2_KIND_TANK)) return 0;
    if (now < next[index]) return 0;
    next[index] = now + (double)period + 0.7 * index;
    return 1;
}
