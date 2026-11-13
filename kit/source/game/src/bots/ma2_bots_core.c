#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
/* MechAssault 2 offline bots - core module (native design, NATIVE_BOTS_DESIGN.md).
 *
 * A bot is its own player: a non-human MAPlayer this machine simulates, created at
 * level start by ma2_bot_player.c and driven every simulation step by its
 * BotController (ma2_bot_ctrl.c) from the player's own tick. Strategy lives in
 * ma2_bot_brain*.c, path planning in ma2_bot_path.c. This file holds the world
 * snapshot, the roster (lobby listing for the Briefing Room and launch checks),
 * the is_local_ai registry behind the authority gates, the AI line of sight, the
 * threat-based target pick and diagnostics. No virtual pads, no bot cameras.
 *
 * Everything runs on the guest thread, inside original guest functions. Every
 * exported entry point saves and restores the complete guest register state,
 * so a hook never changes what the surrounding generated code sees.
 *
 * Guest ABIs used (all verified against game_files/ma2.xbe):
 *   sub_1760E1(ecx=phys, [esp+4]=float3* point, [esp+8]=byte apply,
 *     [esp+0xC]=byte* hit) ret 0xC, result in x87 ST0
 *                                 physics ground follow; called with apply=0,
 *                                 which skips every write to the component
 *   actor vt+0x6C / vt+0x74       thiscall, no args, int HP max / current (eax)
 *   actor vt+0xA8                 thiscall, no args, int nation (eax)
 *   entity vt+0x54                class descriptor (decoded from "mov eax,imm32;
 *                                 ret", guest call as fallback)
 * Terrain height is computed natively: an exact port of sub_2AAA30 (reached via
 * sub_F70A9), checked against a probe memory capture (matches actor+0x4C0.y).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <stdarg.h>
#include <io.h>
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"
#include "ma2_bot_c7.h"   /* c7_log_on (W-09b) */
#include "ma2_bot_api.h"
#include "ma2_bot_track.h"
#include "ma2_bots_core.h"
#include "platform/pc_frontend.h"   /* pc_binding_get: RECOMP_TEST_HIJACK answers the minigame with the bound keys */
#include "ma2_bot_env.h"   /* getenv, cached (the bot code asks every tick) */
int ma2_scn_level_custom(void);   /* scenario/ma2_scenario.c: the level being played is a scenario (F21) */
/* NavCache (ma2_bot_nav.c): the static obstacle grid on disk */
const char *ma2_trav_cache_params(void);
int   ma2_trav_cache_save(const wchar_t *path, const char *key, const void *extra, uint32_t extralen);
void *ma2_trav_cache_read(const wchar_t *path, const char *key, float max_x, float max_z, int *status);
void  ma2_trav_cache_free(void *image);
int   ma2_trav_cache_install(const void *image, void *extra, uint32_t extra_cap, uint32_t *extralen);
int   ma2_trav_cache_compare(const void *image, const void *extra, uint32_t extralen, long long *first_diff, long long *bytes, const char **where);
uint64_t ma2_trav_state_hash(void);
void  ma2_trav_set_quant(int on);

extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
extern uint32_t ma2_pc_wide(const wchar_t *text);          /* pc_prompts.cpp */

/* ------------------------------------------------------------------ */
/* Guest addresses (guide section 2)                                   */
/* ------------------------------------------------------------------ */
#define VA_SESSION          0x4EBBFCu   /* session singleton (sub_1CE1A0) */
#define VA_LOCAL_MASK       0x50C9ACu   /* local controller bitmask */
#define VA_PLAYERS_BEGIN    0x50E280u
#define VA_PLAYERS_END      0x50E284u
#define VA_GAME             0x50C958u   /* game object (sub_161B4B) */
#define VA_TERRAIN          0x4EAF64u
#define VA_STEP_TIME        0x4E2430u   /* simulation step stamp */
#define VA_STEP_DT          0x4E2428u   /* double */
#define VA_GAME_TIME        0x4E2440u   /* double */
#define VA_PAUSED           0x4EAED4u   /* byte: the pause menu is up (sub_C97D1) */

#define CLS_MAPLAYER_VA     0x4EAED0u
#define CLS_MACORPOREAL_VA  0x4EAF2Cu
#define CLS_MAMOVER_VA      0x4EAF40u
#define CLS_MAMECH_VA       0x4EAF78u
#define CLS_MATANK_VA       0x4EAF84u
#define CLS_MATURRET_VA     0x4EAF88u
#define CLS_MAFLYER_VA      0x4EAF98u
#define CLS_MASPIDER_VA     0x4EAF9Cu

#define MAX_BOTS            12

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */
static int gva_ok(uint32_t a, uint32_t size)
{
    return a >= 0x10000u && a < 0x04000000u && size < 0x04000000u && a + size <= 0x04000000u;
}
static int env_int(const char *name, int fallback)
{
    const char *s = getenv(name);
    return s && *s ? atoi(s) : fallback;
}
static int env_on(const char *name) { const char *s = getenv(name); return s && *s && *s != '0'; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int popcount4(uint32_t m) { int n = 0; for (int i = 0; i < 4; i++) n += (m >> i) & 1; return n; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : (v == v ? v : 0.f); }
#include "ma2_bot_c7.c"   /* C7 per-match diagnostics counters (bots batch 1) */

typedef struct GuestRegs {
    uint32_t a, c, d, b, s, i, sp, bp, seh;
    RecompXmm x[8]; double fp[8]; int top;
    uint16_t fp_control, fp_cc; int fp_cmp, df;
} GuestRegs;
static void regs_save(GuestRegs *r)
{
    r->a = g_eax; r->c = g_ecx; r->d = g_edx; r->b = g_ebx; r->s = g_esi; r->i = g_edi;
    r->sp = g_esp; r->bp = g_ebp; r->seh = g_seh_ebp;
    r->x[0] = g_xmm0; r->x[1] = g_xmm1; r->x[2] = g_xmm2; r->x[3] = g_xmm3;
    r->x[4] = g_xmm4; r->x[5] = g_xmm5; r->x[6] = g_xmm6; r->x[7] = g_xmm7;
    memcpy(r->fp, (const void *)g_fp_stack, sizeof(r->fp)); r->top = g_fp_top;
    r->fp_control = g_fp_control_word; r->fp_cc = g_fp_cc; r->fp_cmp = g_fp_cmp; r->df = g_df;
}
static void regs_restore(const GuestRegs *r)
{
    g_eax = r->a; g_ecx = r->c; g_edx = r->d; g_ebx = r->b; g_esi = r->s; g_edi = r->i;
    g_esp = r->sp; g_ebp = r->bp; g_seh_ebp = r->seh;
    g_xmm0 = r->x[0]; g_xmm1 = r->x[1]; g_xmm2 = r->x[2]; g_xmm3 = r->x[3];
    g_xmm4 = r->x[4]; g_xmm5 = r->x[5]; g_xmm6 = r->x[6]; g_xmm7 = r->x[7];
    memcpy((void *)g_fp_stack, r->fp, sizeof(r->fp)); g_fp_top = r->top;
    g_fp_control_word = r->fp_control; g_fp_cc = r->fp_cc; g_fp_cmp = r->fp_cmp; g_df = r->df;
}

static recomp_func_t resolve(uint32_t va)
{
    recomp_func_t f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup(va);
    return f;
}

/* thiscall, no stack arguments, int result in eax. */
static uint32_t vcall0(uint32_t obj, uint32_t slot, uint32_t ret_site, int *ok)
{
    if (ok) *ok = 0;
    if (!gva_ok(obj, 4)) return 0;
    uint32_t vt = MEM32(obj);
    if (!gva_ok(vt, slot + 4)) return 0;
    recomp_func_t f = resolve(MEM32(vt + slot));
    if (!f) return 0;
    GuestRegs r; regs_save(&r);
    g_ecx = obj;
    PUSH32(g_esp, ret_site);
    f();
    uint32_t v = g_eax;
    regs_restore(&r);
    if (ok) *ok = 1;
    return v;
}

/* Class ids are ~crc32(name) (checked on 11 classes in a memory capture). */
static uint32_t crc_table[256];
static uint32_t class_id_of(const char *name)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) c = crc_table[(c ^ *p) & 0xFF] ^ (c >> 8);
    return c;          /* zlib crc32 = ~c; the engine stores c */
}
static uint32_t class_id(uint32_t global_va, const char *name)
{
    uint32_t v = global_va ? MEM32(global_va) : 0;
    return v ? v : class_id_of(name);
}

/* vtable -> class descriptor cache (descriptors are static per class). */
static struct { uint32_t vt, desc; } g_desc_cache[1024];   /* ew-b8 B8.15: was 128 (a full cache misclassified every new vtable) */
static uint32_t class_desc(uint32_t e)
{
    uint32_t vt = MEM32(e);
    if (!gva_ok(vt, 0x58)) return 0;
    unsigned h = (vt >> 2) & 1023;
    for (unsigned k = 0; k < 1024; k++) {
        unsigned j = (h + k) & 1023;
        if (g_desc_cache[j].vt == vt) return g_desc_cache[j].desc;
        if (!g_desc_cache[j].vt) {
            uint32_t fn = MEM32(vt + 0x54), desc = 0;
            if (gva_ok(fn, 6) && MEM8(fn) == 0xB8 && MEM8(fn + 5) == 0xC3) desc = MEM32(fn + 1);
            else desc = vcall0(e, 0x54, 0x000B3C5Au, NULL);
            g_desc_cache[j].vt = vt; g_desc_cache[j].desc = desc;
            return desc;
        }
    }
    return 0;
}
/* Native sub_B3C50(ecx=entity, esi=classId). */
static int is_kind(uint32_t e, uint32_t id)
{
    if (!id || !gva_ok(e, 0x30)) return 0;
    if (MEM32(e + 0x28) == id) return 1;
    uint32_t d = class_desc(e);
    if (!gva_ok(d, 8)) return 0;
    uint32_t n = MEM32(d + 4);
    for (int guard = 0; n && guard < 32; guard++) {
        if (!gva_ok(n, 8)) return 0;
        if (MEM32(n) == id) return 1;
        n = MEM32(n + 4);
    }
    return 0;
}

/* Native sub_B4F70(ecx=world, esi=uid): uid hash table at world+0x34/+0x38. */
static uint32_t uid_lookup(uint32_t world, uint32_t uid)
{
    if (!uid || !gva_ok(world, 0x40)) return 0;
    uint32_t b = MEM32(world + 0x34), e = MEM32(world + 0x38);
    if (e <= b || e - b > 0x100000u || !gva_ok(b, e - b)) return 0;
    uint32_t n = (e - b) / 4;
    uint32_t node = MEM32(b + (uid % n) * 4);
    for (int guard = 0; node && guard < 4096; guard++) {
        if (!gva_ok(node, 12)) return 0;
        if (MEM32(node + 4) == uid) return MEM32(node + 8);
        node = MEM32(node);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Terrain (native sub_2AAA30) and the ground probe callback           */
/* ------------------------------------------------------------------ */
static float surface_height(float x, float z, int *ok);
static float terrain_height(float x, float z, int *ok)
{
    *ok = 0;
    uint32_t t = MEM32(VA_TERRAIN);
    if (!gva_ok(t, 0x840)) return 0.f;
    if (SMEM32(t) >= 0) return 0.f;                     /* "js" in sub_2AAA30 */
    float cell = MEMF(t + 0x814);
    if (!(cell > 0.f)) return 0.f;
    float inv = 1.0f / cell, fx = inv * x, fz = z * inv;
    int w = (int)MEM32(t + 4), d = (int)MEM32(t + 8);
    if (0.f > fx || fx >= (float)(w - 1) || 0.f > fz || fz >= (float)(d - 1)) return 0.f;
    uint32_t rows = MEM32(t + 0x834);
    int zi = (int)fz, xi = (int)fx;
    if (!gva_ok(rows + zi * 4, 8)) return 0.f;
    uint32_t r0 = MEM32(rows + zi * 4) + xi, r1 = MEM32(rows + zi * 4 + 4) + xi;
    if (!gva_ok(r0, 2) || !gva_ok(r1, 2)) return 0.f;
    float u = fx - (float)xi, v = fz - (float)zi;
#define PAL(i) MEMF(t + 0x14 + (uint32_t)(i) * 4)
    float c00, c10, c01, c11;
    if (u > v) {
        c00 = PAL(MEM8(r0)); c10 = PAL(MEM8(r0 + 1)); c11 = PAL(MEM8(r1 + 1));
        c01 = ((c11 + c00) * 0.5f - c10) * 2.0f + c10;
    } else {
        c00 = PAL(MEM8(r0)); c01 = PAL(MEM8(r1)); c11 = PAL(MEM8(r1 + 1));
        c10 = ((c11 + c00) * 0.5f - c01) * 2.0f + c01;
    }
#undef PAL
    float h0 = (c10 - c00) * u + c00, h1 = (c11 - c01) * u + c01;
    *ok = 1;
    return (h1 - h0) * v + h0;
}

typedef struct ProbeCtx { uint32_t actor, phys, vt; float offset; int objects; } ProbeCtx;
static ProbeCtx g_probe;

/* The probe actor is picked once per world build, but the game can free it before the frame is
 * out -- a pilot that finishes boarding a vehicle is deleted -- and the ray filter then ran
 * IsKindOf (sub_B3C50) on the freed object and crashed (twice: a bot pilot boarding a VTOL,
 * 2026-10-13/14). Still the same live object: same vtable, physics back-links intact. */
static int probe_live(const ProbeCtx *p)
{
    return p && p->actor && gva_ok(p->actor, 0x4D0) && MEM32(p->actor) == p->vt && gva_ok(p->phys, 0x170) &&
           MEM32(p->actor + 0x8C) == p->phys && MEM32(p->phys + 0x148) == p->actor;
}

/* sub_1760E1 with apply=0: max(sweep hit, terrain + actor+0xD8). */
static float object_probe(const ProbeCtx *p, float x, float y, float z)
{
    GuestRegs r; regs_save(&r);
    g_esp -= 32;
    uint32_t buf = g_esp;
    MEMF(buf) = x; MEMF(buf + 4) = y; MEMF(buf + 8) = z; MEM32(buf + 12) = 0;
    PUSH32(g_esp, buf + 12);           /* [esp+0xC] byte *hit (written only when apply) */
    PUSH32(g_esp, 0);                  /* [esp+8]   apply = 0: no component writes */
    PUSH32(g_esp, buf);                /* [esp+4]   const float3 *point */
    g_ecx = p->phys;
    PUSH32(g_esp, 0x001760E1u);        /* return address (virtual call; any) */
    sub_001760E1();                    /* ret 0xC */
    double v = g_fp_stack[g_fp_top & 7];
    regs_restore(&r);
    return (float)v;
}

static float ground_fn(void *ctx, float x, float y_from, float z, int *ok)
{
    const ProbeCtx *p = (const ProbeCtx *)ctx;
    int tok = 0;
    float h = terrain_height(x, z, &tok);
    if (ok) *ok = tok;
    if (p && p->objects && probe_live(p)) {
        float o = object_probe(p, x, y_from, z) - p->offset;
        if (o == o && o > h + 0.05f && o < y_from + 1.0f) { h = o; if (ok) *ok = 1; }
    }
    return h;
}

/* Ray cast through the game's collision space (the query sub_1760E1 uses for its
 * vertical ground sweep): sub_A10A0 builds a ray query {type 2, origin, dir, length,
 * ignore-filter}, sub_8DF50(space, query, result) traces it; result+0xC = hit distance,
 * result+4 = hit collider. flags2d/2e = query dynamic / static layers (both 1 in the
 * original call). Returns 1 on hit. */
static unsigned g_perf_rays, g_perf_los, g_perf_plans;   /* RECOMP_BOT_PERF counters */
static double g_perf_ray_ms, g_perf_los_ms, g_perf_plan_ms, g_perf_brain_ms, g_perf_ctrl_ms;
static double perf_now_ms(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }
/* `self`: the actor whose colliders the game's filter (vtable 0x477CA8, sub_176791) skips -- it
 * rejects a collider whose owner (+0x128) is self, or one self's vtable+0x148 call says belongs to
 * it; 0 = none. Must be a live actor. */
static int ray_cast_as(uint32_t self, Ma2Vec3 o, Ma2Vec3 d, float len, int flags2d, int flags2e,
                       float *t_out, uint32_t *obj_out);
static int ray_cast(const ProbeCtx *p, Ma2Vec3 o, Ma2Vec3 d, float len, int flags2d, int flags2e,
                    float *t_out, uint32_t *obj_out)
{
    return ray_cast_as(probe_live(p) ? p->actor : 0, o, d, len, flags2d, flags2e, t_out, obj_out);
}
static int ray_cast_as(uint32_t self, Ma2Vec3 o, Ma2Vec3 d, float len, int flags2d, int flags2e,
                       float *t_out, uint32_t *obj_out)
{
    g_perf_rays++; double perf_t0 = perf_now_ms();
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0x20)) return 0;
    uint32_t wl = MEM32(game + 0x18);
    if (!gva_ok(wl, 0x20)) return 0;
    uint32_t space = MEM32(wl + 0x18);
    if (!gva_ok(space, 0x20)) return 0;
    GuestRegs r; regs_save(&r);
    g_esp -= 0x200;
    uint32_t base = g_esp & ~15u, R = base, Q = base + 0x40, F = base + 0x80, S = base + 0xC0;
    for (uint32_t i = 0; i < 0x1A0; i += 4) MEM32(base + i) = 0;
    MEMF(R) = o.x; MEMF(R + 4) = o.y; MEMF(R + 8) = o.z;
    MEMF(R + 12) = d.x; MEMF(R + 16) = d.y; MEMF(R + 20) = d.z;
    MEM32(F) = 0x477CA8u; MEM32(F + 4) = self;   /* the filter's "self" (0: none) */
    g_esp = base - 16;
    g_eax = Q; g_edx = R; g_xmm0.f[0] = len;
    PUSH32(g_esp, F);
    PUSH32(g_esp, 0x000A10A0u);
    sub_000A10A0();
    MEM8(Q + 0x2D) = (uint8_t)flags2d; MEM8(Q + 0x2E) = (uint8_t)flags2e;
    g_esp = base - 16;
    PUSH32(g_esp, S); PUSH32(g_esp, Q); PUSH32(g_esp, space);
    PUSH32(g_esp, 0x0008DF50u);
    sub_0008DF50();
    int hit = (g_eax & 0xFF) != 0;
    if (hit) {
        if (t_out) *t_out = MEMF(S + 0xC);
        if (obj_out) *obj_out = MEM32(S + 4);
    }
    regs_restore(&r);
    g_perf_ray_ms += perf_now_ms() - perf_t0;
    return hit;
}

/* ------------------------------------------------------------------ */
/* Roster (lobby): config + game type + auto-balance (guide 3.3)       */
/* ------------------------------------------------------------------ */
typedef struct BotOverride { int has_team, team, has_vehicle, has_option, has_ready, ready; uint32_t vehicle, option; } BotOverride;
typedef struct Roster { int n, team_mode, game_type, humans_red, humans_blue; int team[MAX_BOTS], slot[MAX_BOTS]; } Roster;

static BotOverride g_ovr[MAX_BOTS];
static uint32_t g_ovr_key_session, g_ovr_key_type, g_ovr_key_n;
static int g_name_base = -1;

static int session_ok(uint32_t s) { return gva_ok(s, 0x730); }
static int g_bot_spawning;        /* between sub_1DB150 entry and the end of its lobby iterator */
static int g_bot_spawning;        /* between sub_1DB150 entry and the end of its lobby iterator */
static int team_mode_of(uint32_t gt)   /* sub_16616C */
{
    int t = (int)gt;
    return (t > 0 && t <= 3) || (t > 10 && t <= 0x11);
}
static int total_cap(void) { return clampi(env_int("RECOMP_BOTS_MAX", 12), 2, MA2_BOT_MAX_PLAYERS); }   /* 6v6 */

static void compute_roster(uint32_t session, Roster *r)
{
    memset(r, 0, sizeof(*r));
    if (!session_ok(session) || MEM32(session + 0x4C) != 2) return;
    const Ma2BotConfig *cfg = ma2_bots_config();
    if (!cfg) return;
    uint32_t mask = MEM32(VA_LOCAL_MASK) & 0xF;
    int humans = popcount4(mask), hr = 0, hb = 0;
    for (int c = 0; c < 4; c++)
        if (mask & (1u << c)) { uint32_t t = MEM32(session + 0x3A8 + c * 4); if (t == 1) hb++; else hr++; }   /* ew-b8 B8.6: the same rule as ma2_bots_ui.c human_sides */
    int room = total_cap() - humans;
    if (room > MA2_BOT_MAX_PLAYERS - humans) room = MA2_BOT_MAX_PLAYERS - humans;
    if (room > MAX_BOTS) room = MAX_BOTS;
    r->game_type = (int)MEM32(session + 0x44);
    r->team_mode = team_mode_of(MEM32(session + 0x44));
    r->humans_red = r->team_mode ? hr : humans; r->humans_blue = r->team_mode ? hb : 0;
    if (r->game_type == 6 && room > 1) room = 1;      /* Grinder: two-player co-op against the waves -- one bot */
    if (room <= 0) return;
    int br = 0, bb = 0;
    if (!r->team_mode) {
        br = clampi(cfg->ffa, 0, room);
    } else {
        /* Each side's bots as set, up to 6 players a side (MechAssault 2's team size) with its humans.
         * AutoBalance no longer splits the total evenly (1 Red + 4 Blue had become 3v3 with you):
         * it places the players on the short side when the counts are set (ma2_bots_ui.c). */
        int cap = total_cap() / 2;
        br = clampi(cfg->red, 0, cap - hr > 0 ? cap - hr : 0);
        bb = clampi(cfg->blue, 0, cap - hb > 0 ? cap - hb : 0);
        if (br + bb > room) bb = room - br > 0 ? room - br : 0;
    }
    r->n = br + bb;
    if (env_on("RECOMP_BOT_DIAG")) {   /* once per change: the humans' teams as read, the bots per side */
        static int last = -1; int key = hr | hb << 4 | br << 8 | bb << 12 | (int)mask << 16 | r->team_mode << 20;
        if (key != last) {
            last = key;
            if (r->team_mode)
                fprintf(stderr, "[BOT-ROSTER] humans mask=%X red=%d blue=%d (team words %u %u %u %u) bots red=%d blue=%d autobalance=%d cfg red=%d blue=%d\n",
                        mask, hr, hb, MEM32(session + 0x3A8), MEM32(session + 0x3AC), MEM32(session + 0x3B0), MEM32(session + 0x3B4),
                        br, bb, cfg->autobalance, cfg->red, cfg->blue);
            else   /* F49: free-for-all is one pool of bots (it read like 'red=11 blue=0' next to the team cfg) */
                fprintf(stderr, "[BOT-ROSTER] FFA humans mask=%X humans=%d bots ffa=%d cfg ffa=%d (team cfg red=%d blue=%d unused)\n",
                        mask, hr + hb, br + bb, cfg->ffa, cfg->red, cfg->blue);
        }
    }
    /* Slots (rec+0x20, the FFA nation index into 0x4AE8B4[12]): 4+i like the
     * guide; past slot 11 use controller ids no local human occupies. */
    int spare[4], nspare = 0;
    for (int c = 0; c < 4; c++) if (!(mask & (1u << c))) spare[nspare++] = c;
    for (int i = 0; i < r->n; i++) {
        r->team[i] = !r->team_mode ? 0 : (i < br ? 0 : 1);
        if (4 + i <= 11) r->slot[i] = 4 + i;
        else r->slot[i] = (i - 8) < nspare ? spare[i - 8] : 11;
    }
    /* Setter overrides (Briefing Room writes for controller ids >= 4). */
    if (g_ovr_key_session != session || g_ovr_key_type != (uint32_t)r->game_type || g_ovr_key_n != (uint32_t)r->n) {
        memset(g_ovr, 0, sizeof(g_ovr));
        g_ovr_key_session = session; g_ovr_key_type = (uint32_t)r->game_type; g_ovr_key_n = (uint32_t)r->n;
    }
    if (r->team_mode)
        for (int i = 0; i < r->n; i++)
            if (g_ovr[i].has_team && (g_ovr[i].team == 0 || g_ovr[i].team == 1)) r->team[i] = g_ovr[i].team;
}

/* Bot Skill "Mixed": the spread of a real lobby -- a few aces, a few green players, the rest in
 * between -- on both sides, the side facing the humans one notch up (below). A side of n players gets n/3 Elites and n/3 Recruits, the rest
 * Veterans then Regulars (6: E E V R Rc Rc; 4: one of each). Each local human counts as an Elite of
 * his side, so his teammates are the weaker part of the spread. FFA: the spread over everyone. The
 * bots of a side draw their places in a shuffled order, so who is the ace changes from match to match. */
static void mixed_skills(const Roster *r, uint32_t seed, int *out)
{
    for (int side = 0; side < (r->team_mode ? 2 : 1); side++) {
        int idx[MAX_BOTS], nb = 0;
        for (int i = 0; i < r->n && i < MAX_BOTS; i++) if (!r->team_mode || r->team[i] == side) idx[nb++] = i;
        int hum = side == 0 ? r->humans_red : r->humans_blue, n = nb + hum;
        if (nb <= 0) continue;
        int spread[16], ns = 0, e = n / 3, rc = n / 3, mid = n - e - rc;
        /* MixedDragoon (playtest 11-06 Mixed includes Dragoon): the top place of a side of three or more is a Dragoon ace */
        for (int k = 0; k < e && ns < 16; k++) spread[ns++] = k == 0 ? MA2_SKILL_DRAGOON : MA2_SKILL_ELITE;
        for (int k = 0; k < (mid + 1) / 2 && ns < 16; k++) spread[ns++] = MA2_SKILL_VETERAN;
        for (int k = 0; k < mid / 2 && ns < 16; k++) spread[ns++] = MA2_SKILL_REGULAR;
        for (int k = 0; k < rc && ns < 16; k++) spread[ns++] = MA2_SKILL_RECRUIT;
        /* The bots facing the humans are stronger than the spread: the weakest place goes up a
         * level (6: E E V R R Rc instead of E E V R Rc Rc; 4: E V R R; 1: Elite). Bots are less
         * of a challenge than a real lobby's players, and the human was the ace of a match that
         * ended in spawn camping (playtest 2026-10-20). FFA: every bot faces the human. */
        int opp_hum = r->team_mode ? (side == 0 ? r->humans_blue : r->humans_red) : hum;
        if (ns > 0 && (r->team_mode ? hum == 0 && opp_hum > 0 : hum > 0) && spread[ns - 1] < MA2_SKILL_ELITE) spread[ns - 1]++;
        int first = hum < ns ? hum : ns;                 /* the humans take the top places */
        uint32_t s = seed * 2654435761u + (uint32_t)side * 97u + 1u;
        for (int k = nb - 1; k > 0; k--) {               /* shuffle who gets which place */
            s = s * 1664525u + 1013904223u;
            int j = (int)((s >> 8) % (uint32_t)(k + 1)), t = idx[k]; idx[k] = idx[j]; idx[j] = t;
        }
        for (int k = 0; k < nb; k++) out[idx[k]] = first + k < ns ? spread[first + k] : MA2_SKILL_REGULAR;
    }
}
static int g_dragoon_bot[MAX_BOTS];   /* MixedDragoon: this bot plays with the Dragoon knobs (ma2_bots_dragoon while it ticks) */
static int g_cur_bot = -1;            /* the bot whose brain / controller is running now (-1: none) */
static void roster_skills(const Roster *r, int setting, uint32_t seed, int *out)
{
    int s = setting;
    memset(g_dragoon_bot, 0, sizeof g_dragoon_bot);
    for (int i = 0; i < MAX_BOTS; i++) out[i] = s >= MA2_SKILL_RECRUIT && s <= MA2_SKILL_ELITE ? s : MA2_SKILL_REGULAR;
    if (s == MA2_SKILL_MIXED) mixed_skills(r, seed, out);
    if (s == MA2_SKILL_DRAGOON) for (int i = 0; i < MAX_BOTS; i++) out[i] = MA2_SKILL_ELITE;   /* Elites, knobs past Elite (ma2_bots_dragoon) */
    for (int i = 0; i < MAX_BOTS; i++) if (out[i] == MA2_SKILL_DRAGOON) { out[i] = MA2_SKILL_ELITE; g_dragoon_bot[i] = 1; }   /* Mixed's Dragoon aces */
}

int ma2_bot_count_for(uint32_t session)
{
    Roster r; compute_roster(session, &r); return r.n;
}
int ma2_bot_count(void)
{
    return ma2_bot_count_for(MEM32(VA_SESSION));
}
/* Lobby listing (Briefing Room, launch checks, "Players x/y", scoreboard): bots appear
 * as roster records. Not while the level spawns: there BotPlayers are created by
 * ma2_bot_player.c instead of through the lobby. */
int ma2_bots_lobby_active(uint32_t session)
{
    if (g_bot_spawning) return 0;
    return session_ok(session) && MEM32(session + 0x4C) == 2 && ma2_bot_count_for(session) > 0;
}

/* Callsign for bot i: consecutive entries of the content list from a per-process
 * start, skipping any that matches a human's name (session+0x264 holds the local
 * profile name as a wide string) so a bot never shares a name with the player. */
static int wide_eq_ci(const wchar_t *a, uint32_t guest_wide)
{
    for (int k = 0; k < 32; k++) {
        wchar_t x = a[k], y = (wchar_t)MEM16(guest_wide + (uint32_t)k * 2);
        if (x >= L'A' && x <= L'Z') x += 32;
        if (y >= L'A' && y <= L'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}
static const wchar_t *bot_name(int i, uint32_t session)
{
    int count = ma2_bot_callsign_count();
    if (g_name_base < 0) g_name_base = count > 0 ? (int)(GetTickCount() / 1000u % (unsigned)count) : 0;
    int want = i, k = 0;
    for (int tries = 0; tries < count; tries++, k++) {
        const wchar_t *c = ma2_bot_callsign(g_name_base + k);
        if (!c || !*c) continue;
        if (session_ok(session) && wide_eq_ci(c, session + 0x264)) continue;
        if (want-- == 0) return c;
    }
    return L"Bot";
}

static uint32_t first_human_vehicle(uint32_t session)
{
    uint32_t mask = MEM32(VA_LOCAL_MASK) & 0xF;
    for (int c = 0; c < 4; c++) if (mask & (1u << c)) return MEM32(session + 0x388 + c * 4);
    return MEM32(session + 0x388);
}

/* Record layout = the original local-loop stores in sub_1D3E20 (see HANDOFF):
 * +00 name  +04/+0C ctrl  +08 0  +10 vehicle UID  +14 0  +18 team  +1C 0
 * +20 slot  +24 not-host  +28..+34 0  +38 option  +3C 1  +3D 0  +3E isHuman
 * +3F 1  +40 0  +41 1  +42 1  +43 ready  +44 1 (byte)  +48 peer 0 */
void ma2_bot_lobby_record(int i, uint32_t session, uint32_t rec)
{
    GuestRegs regs; regs_save(&regs);
    Roster r; compute_roster(session, &r);
    if (i >= 0 && i < r.n && gva_ok(rec, 0x4C)) {
        uint32_t port = MA2_BOT_PORT_BASE + (uint32_t)i;
        for (uint32_t o = 0; o < 0x4C; o += 4) MEM32(rec + o) = 0;
        MEM32(rec + 0x00) = ma2_pc_wide(bot_name(i, session));
        MEM32(rec + 0x04) = port;
        MEM32(rec + 0x0C) = port;
        MEM32(rec + 0x10) = g_ovr[i].has_vehicle ? g_ovr[i].vehicle : first_human_vehicle(session);
        MEM32(rec + 0x18) = (uint32_t)r.team[i];
        MEM32(rec + 0x20) = (uint32_t)r.slot[i];
        MEM32(rec + 0x24) = 1;                      /* not the host */
        MEM32(rec + 0x38) = g_ovr[i].has_option ? g_ovr[i].option : 0;
        MEM8(rec + 0x3C) = 1;                       /* original local loop: 1 (guide said 0) */
        MEM8(rec + 0x3D) = 0;
        MEM8(rec + 0x3E) = 0;                       /* non-human: System Link style */
        MEM8(rec + 0x3F) = 1;
        MEM8(rec + 0x40) = 0;
        MEM8(rec + 0x41) = 1;
        MEM8(rec + 0x42) = 1;
        MEM8(rec + 0x43) = (uint8_t)(g_ovr[i].has_ready ? g_ovr[i].ready : 1);
        MEM8(rec + 0x44) = 1;
        MEM32(rec + 0x48) = 0;                      /* no peer */
    }
    regs_restore(&regs);
}

int ma2_bot_session_set(int field, uint32_t ctrl, uint32_t value)
{
    if (ctrl < MA2_BOT_PORT_BASE) return 0;
    uint32_t i = ctrl - MA2_BOT_PORT_BASE;
    if (i >= MAX_BOTS) return 1;          /* never index the 4-entry session arrays */
    switch (field) {
    case MA2_BOT_FIELD_VEHICLE: g_ovr[i].has_vehicle = 1; g_ovr[i].vehicle = value; break;
    case MA2_BOT_FIELD_OPTION:  g_ovr[i].has_option = 1; g_ovr[i].option = value; break;
    case MA2_BOT_FIELD_TEAM:    g_ovr[i].has_team = 1; g_ovr[i].team = (int)value; break;
    case MA2_BOT_FIELD_READY:   g_ovr[i].has_ready = 1; g_ovr[i].ready = (int)(value & 0xFF); break;
    default: break;
    }
    return 1;
}

uint32_t ma2_bot_session_get(int field, uint32_t ctrl)
{
    uint32_t i = ctrl - MA2_BOT_PORT_BASE;
    if (ctrl < MA2_BOT_PORT_BASE || i >= MAX_BOTS) return 0;
    uint32_t session = MEM32(VA_SESSION);
    Roster r; compute_roster(session, &r);
    switch (field) {
    case MA2_BOT_FIELD_VEHICLE: return g_ovr[i].has_vehicle ? g_ovr[i].vehicle : (session_ok(session) ? first_human_vehicle(session) : 0);
    case MA2_BOT_FIELD_OPTION:  return g_ovr[i].has_option ? g_ovr[i].option : 0;
    case MA2_BOT_FIELD_TEAM:    return (int)i < r.n ? (uint32_t)r.team[i] : 0;
    case MA2_BOT_FIELD_READY:   return g_ovr[i].has_ready ? (uint32_t)g_ovr[i].ready : 1;
    default: return 0;
    }
}


/* ------------------------------------------------------------------ */
/* Level runtime                                                       */
/* ------------------------------------------------------------------ */
#include "ma2_bot_ctrl.h"   /* BotController state */

/* One BotPlayer: an ordinary non-human MAPlayer that this machine simulates. */
typedef struct Bot {
    uint32_t port, player;           /* player: the MAPlayer (the is_local_ai registry), 0 when gone */
    int team, slot, removed;
    uint32_t last_step; int ticked;
    int paused;                      /* standing down while the game is paused */
    Ma2Vec3 pause_pos; uint32_t pause_actor;
    int skill;                       /* this bot's own skill (enum Ma2Skill; Mixed hands out a spread) */
    Ma2Brain *brain;
    Ma2Order order;
    Ma2CtrlStatus status;
    BotCtrl ctrl;
    DWORD diag_tick, ai_diag;
    double created;
    /* Damage since the brain's last tick (Ma2Senses): received by source, dealt by victim. */
    int nin, nout; Ma2DmgEvent ev_in[MA2_SENSE_MAX_EVENTS], ev_out[MA2_SENSE_MAX_EVENTS];
    unsigned tgt_human, tgt_bot;
} Bot;

enum { NAV_PENDING = 0, NAV_DONE = 1 };
static void navcache_level_reset(void);   /* NavCache: forget this level's cache state */
static int g_nav_slicing; static uint32_t g_nav_slice_step = 0xFFFFFFFFu;   /* W2: sliced obstacle-grid build in progress / last step sliced */

static struct {
    int nbots, skill, game_type, team_mode, spawning;
    uint32_t session;
    Bot bots[MAX_BOTS];
    Ma2Nav *nav; int nav_state; char nav_source[96];
    Ma2World world; int world_valid; uint32_t world_step; double scan_time; int scan_valid;
    Ma2Vec3 prev_pos[MA2_WORLD_MAX_PLAYERS]; uint32_t prev_actor[MA2_WORLD_MAX_PLAYERS]; double prev_time[MA2_WORLD_MAX_PLAYERS];
    float prev_yaw[MA2_WORLD_MAX_PLAYERS];
} g_rt;
int ma2_bots_dragoon(void)
{
    if (g_rt.skill == MA2_SKILL_DRAGOON) return 1;
    return g_rt.skill == MA2_SKILL_MIXED && g_cur_bot >= 0 && g_cur_bot < MAX_BOTS && g_dragoon_bot[g_cur_bot];   /* Mixed: this bot is one of its aces */
}

/* Bot recorder (ma2_bot_rec.c, #included after ma2_bot_ctrl.c): g_rec_cur = the bot whose brain / controller runs now (c7_add attribution). */
static int g_rec_cur = -1;
static void rec_match_end(void);
static void rec_cmds(int index, const float *val);

static void skill_tune_level_reset(void);
static void hull_cache_level_reset(void);
static void lead_level_dump(void);
static int flag_is_mover(uint32_t v);
static uint32_t g_flagged[24];   /* ew-b8 B8.1: declared here so runtime_free_level can clear it */
/* ft-bots (NRC stall ticks): a stall's 8 m re-probe (~830 rays) is queued and run by the wreck pump a few ms a frame, the grid
 * committed in slices, and the "learn an unseen obstacle here" test run once the commit has landed. */
typedef struct { Ma2Vec3 ahead; int stalls, lvl_ok, bot; } StallJob;
static StallJob g_stq[8], g_stl[8], g_stcur;
static int g_nstq, g_nstl, g_stjob;
static void runtime_free_level(void)
{
    g_nstq = g_nstl = g_stjob = 0; ma2_trav_commit_cancel();
    memset(g_flagged, 0, sizeof g_flagged);       /* ew-b8 B8.1 (R5 E1): the actors die with the level; the next match must not body_drive them */
    ma2_pathq_reset();                            /* ew-b8 B8.7 (R5 E7): queued / active path jobs belong to the old grid */
    ma2_trav_reprobe_cancel();
    skill_tune_level_reset();
    hull_cache_level_reset();
    lead_level_dump();
    rec_match_end();
    c7_match_end();
    for (int i = 0; i < MAX_BOTS; i++) {
        if (g_rt.bots[i].brain) ma2_brain_free(g_rt.bots[i].brain);
        g_rt.bots[i].brain = NULL;
    }
    if (g_rt.nav) ma2_nav_free(g_rt.nav);
    g_rt.nav = NULL;
    g_rt.nav_state = NAV_PENDING; g_nav_slicing = 0; g_nav_slice_step = 0xFFFFFFFFu;
    navcache_level_reset();
    g_rt.world_valid = 0; g_rt.scan_valid = 0;
    memset(&g_probe, 0, sizeof(g_probe));
}

/* ---- is_local_ai registry ----
 * A BotPlayer is registered when ma2_bot_player.c creates it and removed when the
 * game deactivates it (sub_C6FF3). Every authority gate asks this, never a port
 * range or the human bit. */
static int bot_index_of(uint32_t player)
{
    if (g_rt.nbots <= 0 || !player) return -1;
    for (int i = 0; i < g_rt.nbots; i++) if (g_rt.bots[i].player == player) return i;
    return -1;
}
int ma2_bot_is_local_ai(uint32_t player) { return bot_index_of(player) >= 0; }

int ma2_bot_is_port(uint32_t port)
{
    for (int i = 0; i < g_rt.nbots; i++) if (g_rt.bots[i].player && g_rt.bots[i].port == port) return 1;
    return 0;
}

int ma2_bot_is_player(uint32_t player) { return ma2_bot_is_local_ai(player); }

int ma2_player_is_local(uint32_t player)
{
    if (!gva_ok(player, 0x4B0)) return 0;
    return (MEM32(player + 0x244) & 4) || ma2_bot_is_local_ai(player);
}

/* player+0x244 as the authority gates should see it: a local AI player counts as
 * local (bit 2) for simulation authority. */
uint32_t ma2_bot_local_flags(uint32_t player)
{
    uint32_t f = MEM32(player + 0x244);
    if (!(f & 4) && ma2_bot_is_local_ai(player)) f |= 4;
    return f;
}

/* Diagnostic: a NeuroHack completed (sub_C6A62 sets the victim's eject flag). */
/* HackEject: when a NeuroHack completes on a BOT, stamp it: the request bit sub_C6A62 sets lives one frame and the game's own handler
 * clears it without ejecting a bot (r4_85: 3 of 3 victims drove on), so the brain ejects on this stamp (build_player: hack_eject, 8 s). */
static double g_hack_ej_t[MAX_BOTS];
static int hack_ej_recent(uint32_t player)
{
    int i = bot_index_of(player); double t = MEMD(VA_GAME_TIME);
    return i >= 0 && g_hack_ej_t[i] > 0.0 && t >= g_hack_ej_t[i] && t - g_hack_ej_t[i] < 8.0;   /* 8 s: a 'Mech still running at 17 m/s needs ~2 s to stop, then ~1.5 s to get out (r5_34 b6: 4 s ran out) */
}
void ma2_bot_hack_eject(uint32_t player, uint32_t on)
{
    if (on) { int bi = bot_index_of(player); if (bi >= 0 && bi < MAX_BOTS) { g_hack_ej_t[bi] = MEMD(VA_GAME_TIME); c7_add("hackeject", "bot_victims", 1); } }
    if (on) c7_add("f19", "hacks_done", 1);   /* every NeuroHack that ejected its victim (bots or the human) */
    if (!on || !env_on("RECOMP_BOT_DIAG") || !gva_ok(player, 0x300)) return;
    fprintf(stderr, "[BOT-HACKED] t=%.2f victim=%08X port=%d bot=%d\n", MEMD(VA_GAME_TIME), player,
            (int)MEM32(player + 0x2AC), ma2_bot_is_player(player));
}

/* patch_round10.py: +0x1E8 for a player-parented vehicle. Humans: AI off
 * (original). Bots in a real vehicle: the campaign AI drives (bits 0|1).
 * The on-foot pilot body stays ours (boarding). RECOMP_BOT_GAME_AI=0 opts out. */
/* The campaign AI only ticks (sub_183345) for an entity whose body
 * ([entity+0x8C]+0x144) has bit 0 set -- "externally driven" -- and the
 * human control tick (sub_172E96) skips such entities. Set it the way the
 * game does (sub_2AC940, al=1) for a bot's real vehicle. */
static void body_drive(uint32_t comp, int on)
{
    GuestRegs r; regs_save(&r);
    g_ecx = comp; g_eax = on;
    PUSH32(g_esp, 0x000DE796u);
    sub_002AC940();
    regs_restore(&r);
}
static void ai_drive_body(uint32_t v)
{
    uint32_t comp = MEM32(v + 0x8C);
    if (!gva_ok(comp, 0x150) || (MEM8(comp + 0x144) & 1)) return;
    body_drive(comp, 1);
    for (int i = 0; i < 24; i++) if (g_flagged[i] == v) return;
    for (int i = 0; i < 24; i++) if (!g_flagged[i]) { g_flagged[i] = v; return; }
}
static void ai_release_body(uint32_t v)
{
    uint32_t comp = MEM32(v + 0x8C);
    if (gva_ok(comp, 0x150) && (MEM8(comp + 0x144) & 1)) body_drive(comp, 0);
}
/* Vehicles we put under AI control go back to the pad once no bot is in them
 * (a human boarding must be able to steer; an empty one must not roam). */
static int actor_owned_decl(uint32_t actor);
static void ai_drive_sweep(void)
{
    for (int i = 0; i < 24; i++) {
        uint32_t v = g_flagged[i];
        if (!v) continue;
        if (!gva_ok(v, 0x300) || !flag_is_mover(v)) { g_flagged[i] = 0; c7_add("safety", "flag_stale", 1); continue; }   /* ew-b8 B8.2 (R5 E2): freed or reused actor: never body_drive it */
        if (gva_ok(v, 0x300) && actor_owned_decl(v) && !((MEM32(v + 0x26C) >> 17) & 1)) continue;
        uint32_t comp = gva_ok(v, 0x300) ? MEM32(v + 0x8C) : 0;
        if (gva_ok(comp, 0x150) && (MEM8(comp + 0x144) & 1)) body_drive(comp, 0);
        g_flagged[i] = 0;
    }
}

static uint32_t ai_flag_bits(void) { return env_int("RECOMP_BOT_ATTACK_PILOT", 1) ? 0x83u : 3u; }
/* patch_round10.py (sub_E6C55, loc_000E6C8E): +0x1E8 for a player-parented body. Humans:
 * AI off (original). A local AI player's body -- the on-foot pilot included -- keeps the
 * campaign AI enabled: the BotController drives it through MoveTo / attack mode. */
uint32_t ma2_bot_vehicle_ai(uint32_t vehicle, uint32_t flags)
{
    uint32_t owner = MEM32(vehicle + 4);
    if (g_rt.nbots > 0 && gva_ok(owner, 0x300) && ma2_bot_is_local_ai(owner)) return flags | ai_flag_bits();
    return flags & ~1u;
}

/* patch_round9.py (NeuroHack cue sounds) and the body sweep: is this actor's parent a local AI player? */
int ma2_bot_actor_owned(uint32_t actor)
{
    if (g_rt.nbots <= 0 || !gva_ok(actor, 0x30)) return 0;
    uint32_t owner = MEM32(actor + 4);
    return gva_ok(owner, 0x300) && ma2_bot_is_local_ai(owner);
}

static int actor_owned_decl(uint32_t actor) { return ma2_bot_actor_owned(actor); }


/* sub_1DB150 entry (level spawn): the roster for this match. The bots themselves are
 * created after the lobby iterator (ma2_bots_level_spawn, ma2_bot_player.c); until then
 * the iterator lists humans only (g_bot_spawning). */
/* W2: RECOMP_HITCH timeline notes ([HITCH-N], once per site; zero cost when off). */
int nv2a_hitch_enabled(void); double nv2a_hitch_rel_s(void); void nv2a_hitch_mark(const char *what); void nv2a_hitch_note(const char *what, double ms);
#define HN(w) do { static int hn_done_; if (!hn_done_ && nv2a_hitch_enabled()) { hn_done_ = 1; nv2a_hitch_note(w, 0.0); } } while (0)
void ma2_bots_level_begin(uint32_t session)
{
    HN("bots_level_begin");
    GuestRegs regs; regs_save(&regs);
    runtime_free_level();
    Roster r; compute_roster(session, &r);
    for (int i = 0; i < MAX_BOTS; i++) memset(&g_rt.bots[i], 0, sizeof(Bot));
    g_rt.nbots = 0;
    g_rt.session = session;
    g_bot_spawning = r.n > 0;
    if (r.n > 0) {
        const Ma2BotConfig *cfg = ma2_bots_config();
        g_rt.skill = cfg ? clampi(cfg->skill, MA2_SKILL_RECRUIT, MA2_SKILL_DRAGOON) : MA2_SKILL_REGULAR;
        g_rt.game_type = r.game_type; g_rt.team_mode = r.team_mode;
        if (env_on("RECOMP_BOT_DIAG")) {
            fprintf(stderr, "[BOT-LEVEL] session=%08X bots=%d type=%d team=%d skill=%d\n",
                    session, r.n, r.game_type, r.team_mode, g_rt.skill);
            /* Accuracy scaling (sub_E6C55) is skipped when [0x524A60]+0x69 (network session) or byte 0x50C85C is non-zero. */
            uint32_t nsess = MEM32(0x00524A60u);
            fprintf(stderr, "[BOT-NETFLAG] [0x524A60]=%08X +0x69=%d byte[0x50C85C]=%d\n", nsess,
                    nsess ? (int)MEM8(nsess + 0x69) : -1, (int)MEM8(0x0050C85Cu));
        }
    }
    regs_restore(&regs);
}

void ma2_bot_player_removed(uint32_t player)
{
    if (g_rt.nbots <= 0 || !player) return;
    GuestRegs regs; regs_save(&regs);
    int idx = bot_index_of(player), live = 0;
    if (idx >= 0) {
        Bot *b = &g_rt.bots[idx];
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-PLAYER] removed bot=%d player=%08X\n", idx, player);
        b->player = 0; b->removed = 1;
        ma2_pathq_cancel(idx);
        ctrl_reset(&b->ctrl);
    }
    for (int i = 0; i < g_rt.nbots; i++) if (g_rt.bots[i].player) live++;
    if (idx >= 0 && live == 0) {          /* level end: free brains and nav */
        runtime_free_level();
        g_rt.nbots = 0;
    }
    regs_restore(&regs);
}

/* ------------------------------------------------------------------ */
/* Inflate (RFC 1950/1951) for reading PATHFINDING maps from .mgf      */
/* ------------------------------------------------------------------ */
typedef struct Inf { const uint8_t *in; size_t inlen, inpos; uint32_t bb; int bc; uint8_t *out; size_t outlen, outpos; int err; } Inf;
typedef struct Huff { short count[16]; short symbol[320]; } Huff;

static int inf_bits(Inf *s, int n)
{
    uint32_t v = s->bb;
    while (s->bc < n) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        v |= (uint32_t)s->in[s->inpos++] << s->bc;
        s->bc += 8;
    }
    s->bb = n < 32 ? v >> n : 0;
    s->bc -= n;
    return (int)(v & ((1u << n) - 1u));
}
static int huff_build(Huff *h, const short *len, int n)
{
    short offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int s = 0; s < n; s++) if (len[s]) h->symbol[offs[len[s]]++] = (short)s;
    return left;
}
static int huff_decode(Inf *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static const short k_lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short k_lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short k_dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short k_dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inf_codes(Inf *s, const Huff *lc, const Huff *dc)
{
    for (;;) {
        int sym = huff_decode(s, lc);
        if (s->err || sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = k_lbase[sym] + inf_bits(s, k_lext[sym]);
            int ds = huff_decode(s, dc);
            if (s->err || ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)(k_dbase[ds] + inf_bits(s, k_dext[ds]));
            if (s->err || dist > s->outpos || s->outpos + (size_t)len > s->outlen) return -1;
            while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
        }
    }
}
static int inf_block_stored(Inf *s)
{
    s->bb = 0; s->bc = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    unsigned nlen = s->in[s->inpos + 2] | (s->in[s->inpos + 3] << 8);
    s->inpos += 4;
    if (len != (~nlen & 0xFFFFu) || s->inpos + len > s->inlen || s->outpos + len > s->outlen) return -1;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len; s->outpos += len;
    return 0;
}
static int inf_block_fixed(Inf *s)
{
    static Huff lc, dc; static int built;
    if (!built) {
        short l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dc, l, 30);
        built = 1;
    }
    return inf_codes(s, &lc, &dc);
}
static int inf_block_dynamic(Inf *s)
{
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    Huff lc, dc;
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    int i = 0;
    for (; i < ncode; i++) lengths[order[i]] = (short)inf_bits(s, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (s->err || huff_build(&lc, lengths, 19) != 0) return -1;
    i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(s, &lc);
        if (s->err || sym < 0) return -1;
        if (sym < 16) { lengths[i++] = (short)sym; continue; }
        int len = 0, rep;
        if (sym == 16) { if (i == 0) return -1; len = lengths[i - 1]; rep = 3 + inf_bits(s, 2); }
        else if (sym == 17) rep = 3 + inf_bits(s, 3);
        else rep = 11 + inf_bits(s, 7);
        if (s->err || i + rep > nlen + ndist) return -1;
        while (rep--) lengths[i++] = (short)len;
    }
    if (lengths[256] == 0) return -1;
    int err = huff_build(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    err = huff_build(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -1;
    return inf_codes(s, &lc, &dc);
}
/* zlib stream -> out; returns bytes written or -1. */
static long zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    if (inlen < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return -1;
    Inf s; memset(&s, 0, sizeof(s));
    s.in = in; s.inlen = inlen; s.inpos = 2; s.out = out; s.outlen = outlen;
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.err) return -1;
        int rc = type == 0 ? inf_block_stored(&s) : type == 1 ? inf_block_fixed(&s) : type == 2 ? inf_block_dynamic(&s) : -1;
        if (rc != 0) return -1;
    } while (!last);
    return (long)s.outpos;
}

/* ------------------------------------------------------------------ */
/* MGF archive reader (format as bringup/inspect_mgf.py)               */
/* ------------------------------------------------------------------ */
typedef struct Mgf {
    FILE *f; int compressed; uint32_t size, block_size, nchunks;
    uint32_t *chunks;                  /* start, length, crc */
    int cached_block; uint8_t *block; uint32_t block_len;
} Mgf;

static void mgf_close(Mgf *m)
{
    if (m->f) fclose(m->f);
    free(m->chunks); free(m->block);
    memset(m, 0, sizeof(*m));
}
static int mgf_open(Mgf *m, const char *path)
{
    memset(m, 0, sizeof(*m));
    m->cached_block = -1;
    m->f = fopen(path, "rb");
    if (!m->f) return 0;
    uint8_t h[32];
    if (fread(h, 1, 32, m->f) != 32) { mgf_close(m); return 0; }
    uint32_t w[8]; memcpy(w, h, 32);
    if (!memcmp(h, "mgf ", 4)) {
        fseek(m->f, 0, SEEK_END); m->size = (uint32_t)ftell(m->f);
        return 1;
    }
    if (w[1] != 0xC5735A0Fu || !w[2] || w[2] > 100000 || !w[5] || w[5] > (16u << 20)) { mgf_close(m); return 0; }
    m->compressed = 1; m->nchunks = w[2]; m->size = w[3]; m->block_size = w[5];
    m->chunks = (uint32_t *)malloc((size_t)m->nchunks * 12);
    m->block = (uint8_t *)malloc(m->block_size);
    if (!m->chunks || !m->block || fseek(m->f, 32, SEEK_SET) ||
        fread(m->chunks, 12, m->nchunks, m->f) != m->nchunks) { mgf_close(m); return 0; }
    return 1;
}
static int mgf_read(Mgf *m, uint32_t addr, uint32_t size, uint8_t *dst)
{
    if ((uint64_t)addr + size > m->size) return 0;
    if (!m->compressed) {
        return !fseek(m->f, (long)addr, SEEK_SET) && fread(dst, 1, size, m->f) == size;
    }
    while (size) {
        uint32_t number = addr / m->block_size, offset = addr % m->block_size;
        if (number >= m->nchunks) return 0;
        if (m->cached_block != (int)number) {
            uint32_t start = m->chunks[number * 3], length = m->chunks[number * 3 + 1];
            if (!length || length > (64u << 20)) return 0;
            uint8_t *packed = (uint8_t *)malloc(length);
            if (!packed) return 0;
            int okr = !fseek(m->f, (long)start, SEEK_SET) && fread(packed, 1, length, m->f) == length;
            long got = okr ? zlib_inflate(packed, length, m->block, m->block_size) : -1;
            free(packed);
            if (got <= 0) { m->cached_block = -1; return 0; }
            m->cached_block = (int)number; m->block_len = (uint32_t)got;
        }
        if (offset >= m->block_len) return 0;
        uint32_t take = m->block_len - offset;
        if (take > size) take = size;
        memcpy(dst, m->block + offset, take);
        dst += take; addr += take; size -= take;
    }
    return 1;
}
/* Finds <dir>/<anything><suffix> (case-insensitive) and returns a malloc'd copy. */
static char *mgf_find_file(Mgf *m, const char *dir, const char *suffix, uint32_t *out_len)
{
    uint32_t h[11];
    if (!mgf_read(m, 0, 44, (uint8_t *)h) || h[0] != 0x2066676Du) return NULL;
    uint32_t file_count = h[3], file_off = h[5], dir_count = h[6], dir_off = h[8], str_len = h[9], str_off = h[10];
    if (dir_count > 200000 || file_count > 200000 || str_len > (32u << 20)) return NULL;
    int32_t *rows = (int32_t *)malloc((size_t)dir_count * 24);
    char *strings = (char *)malloc((size_t)str_len + 1);
    char *result = NULL;
    if (rows && strings && mgf_read(m, dir_off, dir_count * 24, (uint8_t *)rows) &&
        mgf_read(m, str_off, str_len, (uint8_t *)strings)) {
        strings[str_len] = 0;
        size_t sl = strlen(suffix);
        for (uint32_t r = 0; r < dir_count && !result; r++) {
            int32_t *row = rows + r * 6;
            if (row[5] < 0 || (uint32_t)row[5] >= file_count || (uint32_t)row[4] >= str_len) continue;
            const char *name = strings + row[4];
            size_t nl = strlen(name);
            if (nl < sl || _stricmp(name + nl - sl, suffix)) continue;
            int32_t parent = row[1];
            if (parent < 0 || (uint32_t)parent >= dir_count || (uint32_t)rows[parent * 6 + 4] >= str_len) continue;
            if (_stricmp(strings + rows[parent * 6 + 4], dir)) continue;
            uint32_t rec[8];
            if (!mgf_read(m, file_off + (uint32_t)row[5] * 32, 32, (uint8_t *)rec)) continue;
            uint32_t length = rec[3], offset = rec[6];
            if (!length || length > (8u << 20)) continue;
            result = (char *)malloc(length + 1);
            if (result && mgf_read(m, offset, length, (uint8_t *)result)) { result[length] = 0; *out_len = length; }
            else { free(result); result = NULL; }
        }
    }
    free(rows); free(strings);
    return result;
}

/* ------------------------------------------------------------------ */
/* Navigation: shipped graph (matched by terrain), cache, or generate  */
/* ------------------------------------------------------------------ */
static const char *const k_nav_archives[] = { "inyourface", "newrivercity", "rushhour", "witchcauldron" };

/* Fraction of graph nodes lying on this level's terrain. The shipped graphs need not use the
 * game's height datum exactly, so node height is fitted against terrain height (y = a*h + b,
 * least squares; a plain median offset when the ground is flat) and a node matches when it is
 * within 0.75 m of the fit. Returns -1 when it cannot tell (fewer than half the sampled nodes,
 * or under 30, have terrain under them): the caller keeps the shipped graph then.
 * RECOMP_BOT_F21_DUMP=1 prints the first 12 samples. */
static struct { int tested, got; float a, b; } g_nav_fit;
static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static float nav_terrain_score(const Ma2Nav *nav)
{
    int n = ma2_nav_node_count(nav), dump = env_on("RECOMP_BOT_F21_DUMP");
    memset(&g_nav_fit, 0, sizeof g_nav_fit); g_nav_fit.a = 1.f;
    if (n <= 0) return -1.f;
    float ys[300], hs[300], dy[300]; int step = n > 256 ? n / 256 : 1, tested = 0, got = 0, good = 0;
    for (int i = 0; i < n && got < 300; i += step) {
        Ma2Vec3 p = ma2_nav_node_pos(nav, i);
        int ok;
        float h = terrain_height(p.x, p.z, &ok);
        tested++;
        if (dump && tested <= 12) fprintf(stderr, "[BOT-NAV] f21 sample %d at %.0f,%.0f node y=%.2f terrain=%.2f ok=%d\n", i, p.x, p.z, p.y, h, ok);
        if (ok) { ys[got] = p.y; hs[got] = h; dy[got] = p.y - h; got++; }
    }
    g_nav_fit.tested = tested; g_nav_fit.got = got;
    if (got < 30 || 2 * got < tested) return -1.f;
    double mh = 0, my = 0, shh = 0, shy = 0;
    for (int i = 0; i < got; i++) { mh += hs[i]; my += ys[i]; }
    mh /= got; my /= got;
    for (int i = 0; i < got; i++) { shh += (hs[i] - mh) * (hs[i] - mh); shy += (hs[i] - mh) * (ys[i] - my); }
    if (shh / got > 1.0) { g_nav_fit.a = (float)(shy / shh); g_nav_fit.b = (float)(my - g_nav_fit.a * mh); }
    else { qsort(dy, (size_t)got, sizeof dy[0], cmp_float); g_nav_fit.a = 1.f; g_nav_fit.b = dy[got / 2]; }
    if (g_nav_fit.a < 0.8f || g_nav_fit.a > 1.25f) return 0.f;                     /* no real relation to this ground */
    for (int i = 0; i < got; i++) if (fabsf(ys[i] - (g_nav_fit.a * hs[i] + g_nav_fit.b)) < 0.75f) good++;
    return (float)good / (float)got;
}

static Ma2Nav *nav_from_archive(const char *name)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "game_files\\%s.mgf", name);
    Mgf m;
    if (!mgf_open(&m, path)) return NULL;
    uint32_t len = 0;
    char *text = mgf_find_file(&m, "pathfinding", "_mech.map", &len);
    mgf_close(&m);
    if (!text) return NULL;
    Ma2Nav *nav = ma2_nav_load_text(text, len);
    free(text);
    return nav;
}

static uint32_t terrain_fingerprint(void)
{
    uint32_t t = MEM32(VA_TERRAIN), hsh = 2166136261u;
#define FNV(b) (hsh = (hsh ^ (uint8_t)(b)) * 16777619u)
    uint32_t w = MEM32(t + 4), d = MEM32(t + 8), rows = MEM32(t + 0x834);
    for (uint32_t o = 4; o < 0x414; o++) FNV(MEM8(t + o));
    FNV(MEM8(t + 0x814)); FNV(MEM8(t + 0x815)); FNV(MEM8(t + 0x816)); FNV(MEM8(t + 0x817));
    if (w <= 4096 && d <= 4096 && gva_ok(rows, d * 4))
        for (uint32_t z = 0; z < d; z++) {
            uint32_t row = MEM32(rows + z * 4);
            if (!gva_ok(row, w)) break;
            for (uint32_t x = 0; x < w; x++) FNV(MEM8(row + x));
        }
#undef FNV
    return hsh;
}

static int nav_cache_path(uint32_t fp, wchar_t *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH - 64) return 0;
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\MechAssault2", base); CreateDirectoryW(dir, NULL);
    swprintf(dir, MAX_PATH, L"%ls\\MechAssault2\\BotNav", base); CreateDirectoryW(dir, NULL);
    swprintf(out, cap, L"%ls\\gen_%08X.map", dir, fp);
    return 1;
}

static Ma2Nav *nav_load_file_w(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    Ma2Nav *nav = NULL;
    if (len > 0 && len < (64L << 20)) {
        char *text = (char *)malloc((size_t)len + 1);
        if (text && fread(text, 1, (size_t)len, f) == (size_t)len) { text[len] = 0; nav = ma2_nav_load_text(text, (size_t)len); }
        free(text);
    }
    fclose(f);
    return nav;
}

/* Stuck memory: every place a bot got wedged (an edge the brain penalised) is
 * appended to "<bots profile>/Bot Nav/<map>.stuck" and replayed into the graph
 * when that map loads again, so later matches route around known bad spots.
 * RECOMP_BOT_STUCK_MEMORY=1 enables it (off by default). */
extern void (*g_ma2_nav_penalize_hook)(Ma2Vec3 a, Ma2Vec3 b, float extra);
static wchar_t g_stuck_path[MAX_PATH];
static int g_stuck_lines;
static void stuck_memory_record(Ma2Vec3 a, Ma2Vec3 b, float extra)
{
    if (!g_stuck_path[0] || g_stuck_lines >= 4000) return;
    FILE *f = _wfopen(g_stuck_path, L"a");
    if (!f) return;
    fprintf(f, "%.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", a.x, a.y, a.z, b.x, b.y, b.z, extra);
    fclose(f);
    g_stuck_lines++;
}
static void roofs_free(void);
static void stuck_memory_load(Ma2Nav *nav, const char *map)
{
    ma2_bot_air_reset(); ma2_bot_brain_reset(); ma2_path_drops_reset(); { void ma2_path_pilot_face_reset(void); ma2_path_pilot_face_reset(); } ma2_track_reset(); roofs_free();
    g_ma2_nav_penalize_hook = NULL;
    g_stuck_path[0] = 0; g_stuck_lines = 0;
    /* Off by default: remembered penalties fed back on themselves across sessions (a
     * bot stuck near a penalised edge adds more), until spawn routes were unusable.
     * Penalties from stuck events still apply within the current match. */
    if (!nav || !map || !env_int("RECOMP_BOT_STUCK_MEMORY", 0)) return;
    /* Directory: next to the settings ini the bots profile uses. */
    wchar_t dir[MAX_PATH] = L"";
    const char *ini = getenv("RECOMP_PC_SETTINGS_INI");
    if (ini && *ini) {
        MultiByteToWideChar(CP_ACP, 0, ini, -1, dir, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (!slash) slash = wcsrchr(dir, L'/');
        if (slash) *slash = 0; else dir[0] = 0;
    }
    if (!dir[0]) {
        const char *la = getenv("LOCALAPPDATA");
        if (!la) return;
        MultiByteToWideChar(CP_ACP, 0, la, -1, dir, MAX_PATH);
        wcscat_s(dir, MAX_PATH, L"\\MechAssault2");
    }
    wcscat_s(dir, MAX_PATH, L"\\Bot Nav");
    CreateDirectoryW(dir, NULL);
    wchar_t wmap[64]; MultiByteToWideChar(CP_ACP, 0, map, -1, wmap, 64);
    _snwprintf_s(g_stuck_path, MAX_PATH, _TRUNCATE, L"%ls\\%ls.stuck", dir, wmap);
    int applied = 0;
    FILE *f = _wfopen(g_stuck_path, L"r");
    if (f) {
        Ma2Vec3 a, b; float e;
        while (fscanf(f, "%f %f %f %f %f %f %f", &a.x, &a.y, &a.z, &b.x, &b.y, &b.z, &e) == 7) {
            ma2_nav_penalize(nav, a, b, e * 0.5f);      /* remembered, slightly softer than fresh */
            applied++; g_stuck_lines++;
        }
        fclose(f);
    }
    g_ma2_nav_penalize_hook = stuck_memory_record;
    if (env_on("RECOMP_BOT_DIAG")) { char shown[MAX_PATH * 2]; fprintf(stderr, "[BOT-NAV] stuck memory %s: %d remembered\n", xbox_log_pathw(g_stuck_path, shown, sizeof shown), applied); }
}

static struct { int hits, water, dyn, soft, under, nc; uint32_t cls[12]; int cnt[12]; } g_obst_stat;
static float obstacle_top(void *ctx, float x, float y_from, float z, int *ok);
static float g_obst_top_y = -1e9f;               /* TallSolid: the grid probe's first ray starts here (see obstacle_top) */
static struct { int solid, open; } g_tall_stat;
static void trav_fine_props(void);
static struct { int tried, on, hit, write_pending; long long t0, t_grid; void *image; const char *via; char map[48]; char key[640]; wchar_t path[MAX_PATH]; } g_nc;   /* NavCache state of this level */
static long long nc_qpc(void);
static int navcache_try(float max_x, float max_z);
static void navcache_after_build(int okb);
static void navcache_ready_log(void);
static Ma2Vec3 read_vec3(uint32_t a);
static float terrain_only_fn(void *ctx, float x, float y_from, float z, int *ok)
{
    (void)ctx; (void)y_from;
    return terrain_height(x, z, ok);
}

static void ensure_nav(void)
{
    if (g_rt.nav_state != NAV_PENDING) return;
    int ok;
    uint32_t t = MEM32(VA_TERRAIN);
    HN("ensure_nav_first_call");
    if (!gva_ok(t, 0x840) || SMEM32(t) >= 0) return;   /* terrain not ready: retry next step */
    HN("ensure_nav_terrain_ready");
    (void)terrain_height(0, 0, &ok);
    if (!g_nc.t0) g_nc.t0 = nc_qpc();
    /* W2 (start hitching): the obstacle grid is ~570,000 physics rays = 1.1-1.2 s on the game thread, a frozen
     * first second of the match. With NavSlice (default on; [Bots] NavSlice=0 / RECOMP_BOT_NAV_SLICE=0 restores)
     * it is built a few ms per game step instead (RECOMP_BOT_NAV_SLICE_MS, default 6); the grid is the same cell
     * for cell, the bots simply start ~1-2 s later (they are held while it builds). */
    int okb_pre = -1;
    if (!g_nc.tried && !g_nav_slicing && env_int("RECOMP_BOT_TRAV", 1)) {   /* NavCache: the static grid from disk, ready this frame */
        float cellc = MEMF(t + 0x814);
        if (navcache_try(((int)MEM32(t + 4) - 1) * cellc - 0.01f, ((int)MEM32(t + 8) - 1) * cellc - 0.01f)) okb_pre = 1;
    }
    if (okb_pre < 0 && env_int("RECOMP_BOT_TRAV", 1) && c7_on("navslice", "RECOMP_BOT_NAV_SLICE", "NavSlice", 1)) {
        if (!g_nav_slicing) {
            float cellt = MEMF(t + 0x814), topt = -1e9f;
            for (int i = 0; i < 256; i++) { float v = MEMF(t + 0x14 + i * 4); if (v > topt) topt = v; }
            memset(&g_obst_stat, 0, sizeof g_obst_stat); memset(&g_tall_stat, 0, sizeof g_tall_stat); g_obst_top_y = topt + 160.f;
            if (ma2_trav_build_begin(((int)MEM32(t + 4) - 1) * cellt - 0.01f, ((int)MEM32(t + 8) - 1) * cellt - 0.01f, topt + 10.f,
                                     terrain_only_fn, NULL, g_probe.phys ? obstacle_top : NULL, &g_probe)) g_nav_slicing = 1;
            else okb_pre = 0;
        }
        if (g_nav_slicing) {
            uint32_t st = MEM32(VA_STEP_TIME);
            if (st == g_nav_slice_step) return;                 /* one slice per game step */
            g_nav_slice_step = st;
            if (g_nav_slicing == 1) {
                if (!ma2_trav_build_step((double)env_int("RECOMP_BOT_NAV_SLICE_MS", 6))) { c7_add("navslice", "slices", 1); return; }
                g_nav_slicing = 2; trav_fine_props_run(1, -1.0); return;       /* props pass: the next steps */
            }
            if (!trav_fine_props_run(0, (double)env_int("RECOMP_BOT_NAV_SLICE_MS", 6))) { c7_add("navslice", "slices", 1); return; }
            g_nav_slicing = 0; okb_pre = 1;
        }
    }
    HN("nav_build_complete");
    g_rt.nav_state = NAV_DONE;
    if (env_int("RECOMP_BOT_TRAV", 1)) {
        float cellt = MEMF(t + 0x814), topt = -1e9f;
        for (int i = 0; i < 256; i++) { float v = MEMF(t + 0x14 + i * 4); if (v > topt) topt = v; }
        LARGE_INTEGER qf, qa, qb; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qa);
        if (okb_pre < 0) { memset(&g_obst_stat, 0, sizeof g_obst_stat); memset(&g_tall_stat, 0, sizeof g_tall_stat); g_obst_top_y = topt + 160.f; }
        int okb = okb_pre >= 0 ? okb_pre : ma2_trav_build(((int)MEM32(t + 4) - 1) * cellt - 0.01f, ((int)MEM32(t + 8) - 1) * cellt - 0.01f, topt + 10.f,
                                 terrain_only_fn, NULL, g_probe.phys ? obstacle_top : NULL, &g_probe);
        HN("nav_trav_built"); if (okb && okb_pre < 0) trav_fine_props();
        HN("nav_fine_props_done");
        if (okb_pre != 1 || g_tall_stat.solid || g_tall_stat.open) { c7_add("tallsolid", "cells", g_tall_stat.solid); c7_add("tallsolid", "open", g_tall_stat.open); }
        if (env_on("RECOMP_BOT_DIAG") && (g_tall_stat.solid || g_tall_stat.open)) fprintf(stderr, "[BOT-NAV] tallsolid: %d cells inside a solid over ground+40 (roof = surface), %d open below an overhead\n", g_tall_stat.solid, g_tall_stat.open);
        g_nc.t_grid = nc_qpc(); navcache_after_build(okb);
        QueryPerformanceCounter(&qb);
        if (env_on("RECOMP_BOT_DIAG")) {
            int gw, gh, no, ns; ma2_trav_stats(&gw, &gh, &no, &ns);
            fprintf(stderr, "[BOT-NAV] obstacle grid %s %dx%d obstacles=%d steep=%d in %.0f ms (rays: %d hits, %d breakable, %d water, %d under water, %d vehicles)\n", okb ? "ok" : "FAILED", gw, gh, no, ns,
                    (double)(qb.QuadPart - qa.QuadPart) * 1000.0 / qf.QuadPart, g_obst_stat.hits, g_obst_stat.soft, g_obst_stat.water, g_obst_stat.under, g_obst_stat.dyn);
            for (int i = 0; i < g_obst_stat.nc; i++) fprintf(stderr, "[BOT-NAV]   hit class %08X x%d\n", g_obst_stat.cls[i], g_obst_stat.cnt[i]);
        }
        if (okb && getenv("RECOMP_BOT_PATHTEST")) {   /* "x0,z0,x1,z1,radius,crush,drop[|...]": plan and print */
            char buf[512]; snprintf(buf, sizeof buf, "%s", getenv("RECOMP_BOT_PATHTEST"));
            for (char *q = buf, *nx; q && *q; q = nx) {
                nx = strchr(q, '|'); if (nx) *nx++ = 0;
                float ax, az, bx, bz, rad, dropm; int crush;
                float dk = 1.0f, upm = 0.0f; int dw = 0;
                if (sscanf(q, "%f,%f,%f,%f,%f,%d,%f,%f,%d,%f", &ax, &az, &bx, &bz, &rad, &crush, &dropm, &dk, &dw, &upm) < 7) continue;   /* W5: optional ,drop_cost_k,water_ok ; push-x3: ,up (jet-up metres) */
                static Ma2Path tp; Ma2Vec3 a = { ax, 0.f, az }, b2 = { bx, 0.f, bz };
                ma2_path_set_danger(NULL, 0); ma2_path_set_crush(crush); ma2_path_set_drop(dropm); ma2_path_set_drop_pref(dk, dw); ma2_path_set_up(upm);
                if (getenv("RECOMP_BOT_PATHTEST_SLOPE")) ma2_path_set_slope_max(atoi(getenv("RECOMP_BOT_PATHTEST_SLOPE")));   /* TankSlope check */
                ma2_path_set_reach_slot(11);
                Ma2PathObstacle pdz[4]; int npdz = 0;   /* RECOMP_BOT_PATHTEST_DYN="x,z,r[|x,z,r...]": parked hulls (the recorded path.dyn of the failing plan) for every query */
                if (getenv("RECOMP_BOT_PATHTEST_DYN")) {
                    char db[128]; snprintf(db, sizeof db, "%s", getenv("RECOMP_BOT_PATHTEST_DYN"));
                    for (char *dq = db, *dn; dq && *dq && npdz < 4; dq = dn) {
                        dn = strchr(dq, '|'); if (dn) *dn++ = 0;
                        float dx, dz2, dr; if (sscanf(dq, "%f,%f,%f", &dx, &dz2, &dr) == 3) { memset(&pdz[npdz], 0, sizeof pdz[0]); pdz[npdz].x = dx; pdz[npdz].z = dz2; pdz[npdz].r = dr; npdz++; }
                    }
                }
                int okp = ma2_path_plan(&tp, a, b2, rad, npdz ? pdz : NULL, npdz, 0.0); int expd = ma2_path_last_expanded();
                ma2_path_set_reach_slot(-1);
                int rch = ma2_path_reach_has(11, a, b2, 0.0);   /* ReachSet: is the goal inside what that search reached (-1 = no finished search without the goal) */
                if (getenv("RECOMP_BOT_PATHTEST_SLICE")) {   /* W7: a queued, sliced plan must give the same path as the synchronous one (plain, then with a danger circle on the way) */
                    for (int var = 0; var < 2; var++) {
                        static Ma2Path sp, np; int steps = 0, rr = 0, got = 0;
                        Ma2PathDanger dzt = { (ax + bx) * 0.5f, (az + bz) * 0.5f, 60.f, 8.f };
                        if (var) ma2_path_set_danger(&dzt, 1); else ma2_path_set_danger(NULL, 0);
                        int ok_sync = ma2_path_plan(&sp, a, b2, rad, NULL, 0, 0.0);
                        int tk = ma2_pathq_submit(-3, a, b2, rad, NULL, 0, 0.0, NULL);
                        while (tk > 0 && steps < 100000) {
                            ma2_pathq_pump(0.5); steps++;
                            if ((got = ma2_pathq_take(-3, tk, &np, &rr)) != 0) break;
                        }
                        int same = got == 1 && ok_sync == rr && sp.n == np.n && sp.ok == np.ok && memcmp(sp.pt, np.pt, sizeof(Ma2Vec3) * (size_t)sp.n) == 0 &&
                                   memcmp(sp.drop, np.drop, (size_t)sp.n) == 0 && sp.end_gap == np.end_gap && sp.ndyn == np.ndyn;
                        fprintf(stderr, "[PATHTEST-SLICE] %.0f,%.0f -> %.0f,%.0f var=%s same=%d n=%d/%d ok=%d/%d steps=%d\n", ax, az, bx, bz, var ? "danger" : "plain", same, sp.n, np.n, sp.ok, np.ok, steps);
                    }
                    ma2_path_set_danger(NULL, 0);
                }
                ma2_path_set_drop_pref(1.0f, 0);
                fprintf(stderr, "[PATHTEST] %.0f,%.0f -> %.0f,%.0f r=%.1f crush=%d drop=%.0f: ok=%d n=%d gap=%.0f exp=%d reach=%d:", ax, az, bx, bz, rad, crush, dropm, okp && tp.ok, tp.n, tp.end_gap, expd, rch);
                { Ma2TravView tv; int hv = ma2_trav_view_heavy(&tv); float plen = 0.f;
                  for (int i = 0; i < tp.n; i++) {
                      float yy = -999.f; if (hv && tv.sf) { int cx = (int)floorf(tp.pt[i].x / tv.cell + 0.5f), cz = (int)floorf(tp.pt[i].z / tv.cell + 0.5f);   /* push-x3 E12: the planner's own rounding */ if (cx >= 0 && cz >= 0 && cx < tv.w && cz < tv.h) yy = tv.sf[cz * tv.w + cx]; }
                      if (i) plen += hypotf(tp.pt[i].x - tp.pt[i - 1].x, tp.pt[i].z - tp.pt[i - 1].z);
                      fprintf(stderr, " %s%.0f,%.0f/y%.0f", tp.drop[i] ? "v" : "", tp.pt[i].x, tp.pt[i].z, yy); }
                  fprintf(stderr, " len=%.0f", plen); }
                fprintf(stderr, "\n");
            }
            ma2_path_set_drop(0.f); ma2_path_set_crush(0); ma2_path_set_up(0.f);
        }
        if (okb && getenv("RECOMP_BOT_NAV_REGION")) {   /* "x0,z0,x1,z1": the grid there as text */
            float r0 = 0, r1 = 0, r2 = 0, r3 = 0;
            if (sscanf(getenv("RECOMP_BOT_NAV_REGION"), "%f,%f,%f,%f", &r0, &r1, &r2, &r3) == 4) ma2_trav_dump_region(r0, r1, r2, r3);
        }
        if (okb && env_on("RECOMP_BOT_TRAV_DUMP")) {
            char nm[64]; snprintf(nm, sizeof nm, "trav_%08X.bin", terrain_fingerprint());
            ma2_trav_dump(nm);
        }
    }
    HN("nav_graph_start");
    int diag = env_on("RECOMP_BOT_DIAG") || env_on("RECOMP_BOT_DIAG_NAV");
    Ma2Nav *nav = NULL;
    const char *file = getenv("RECOMP_BOT_NAV_FILE");
    if (file && *file) {
        wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, file, -1, w, MAX_PATH);
        nav = nav_load_file_w(w);
        if (nav) snprintf(g_rt.nav_source, sizeof(g_rt.nav_source), "file:%s", file);
    }
    /* The loading level is identified by its cache file (ma2_bot_mapid.c).
     * Maps that ship a 'Mech graph load it directly; the others go straight to
     * generation. The terrain-match scan below only runs if the map is unknown. */
    const char *map_archive = ma2_bot_map_archive();
    char map_base[32] = {0};
    if (map_archive) {
        size_t n = strcspn(map_archive, ".");
        if (n >= sizeof(map_base)) n = sizeof(map_base) - 1;
        memcpy(map_base, map_archive, n);
        if (diag) fprintf(stderr, "[BOT-NAV] map=%s shipped=%s\n", map_archive,
                          ma2_bot_map_nav() ? ma2_bot_map_nav() : "none");
    }
    if (!nav && map_archive && ma2_bot_map_nav()) {
        nav = nav_from_archive(map_base);
        if (nav) snprintf(g_rt.nav_source, sizeof(g_rt.nav_source), "shipped:%s", map_base);
        /* F21 (batch 2): a scenario built on In Your Face, New River City, Rush Hour or Witch Cauldron
         * plays on that base's archive but may have its own terrain; the retail graph then sends
         * roams and flanks to ground that is not there. On a scenario the shipped graph has to sit
         * on this terrain (9 nodes in 10 on the height fit, nav_terrain_score), else the graph is generated;
         * a copy whose terrain is the retail one (known fingerprint), or a score that cannot tell, keeps it.
         * RECOMP_BOT_F21=0 / [Bots] CustomNavCheck=0 = always the shipped graph. */
        int scn = ma2_scn_level_custom();
        if (nav && diag && !scn) {                                                   /* retail: the same score, diag only (validates the check) */
            float sc = nav_terrain_score(nav);
            fprintf(stderr, "[BOT-NAV] retail %s: shipped graph terrain-match=%.2f (fit y=%.3f*h%+.2f, %d/%d samples on terrain, terrain %08X)\n",
                    map_base, sc, g_nav_fit.a, g_nav_fit.b, g_nav_fit.got, g_nav_fit.tested, terrain_fingerprint());
        }
        if (nav && scn && c7_on("f21", "RECOMP_BOT_F21", "CustomNavCheck", 1)) {
            /* terrain identical to a retail level (fingerprint as seen on retail runs): keep, no score */
            static const uint32_t k_retail_fp[] = { 0x216EF193u /* newrivercity */ };
            uint32_t fp = terrain_fingerprint();
            int known = 0;
            for (size_t k = 0; k < sizeof k_retail_fp / sizeof k_retail_fp[0]; k++) if (fp == k_retail_fp[k]) known = 1;
            float raw = nav_terrain_score(nav), sc = known || raw < 0.f ? 1.f : raw;   /* < 0 = cannot tell: keep the shipped graph */
            fprintf(stderr, "[BOT-NAV] scenario on %s: shipped graph terrain-match=%.2f raw=%.2f (%s; fit y=%.3f*h%+.2f, %d/%d samples on terrain, terrain %08X) -> %s\n",
                    map_base, sc, raw, known ? "retail terrain" : g_nav_fit.got ? "scored" : "no samples", g_nav_fit.a, g_nav_fit.b, g_nav_fit.got, g_nav_fit.tested, fp,
                    sc >= 0.9f ? "kept" : "generated instead");
            c7_add("f21", sc >= 0.9f ? "kept" : "regenerated", 1);
            if (sc < 0.9f) { ma2_nav_free(nav); nav = NULL; g_rt.nav_source[0] = 0; }
        }
    }
    if (!nav && !map_archive) {
        const char *forced = getenv("RECOMP_BOT_NAV_MAP");
        float best_score = 0.f, second = 0.f; Ma2Nav *best = NULL; const char *best_name = NULL;
        for (size_t k = 0; k < sizeof(k_nav_archives) / sizeof(k_nav_archives[0]); k++) {
            if (forced && *forced && _stricmp(forced, k_nav_archives[k])) continue;
            Ma2Nav *cand = nav_from_archive(k_nav_archives[k]);
            if (!cand) continue;
            float sc = forced && *forced ? 1.f : nav_terrain_score(cand);
            if (diag) fprintf(stderr, "[BOT-NAV] candidate %s nodes=%d terrain-match=%.2f\n",
                              k_nav_archives[k], ma2_nav_node_count(cand), sc);
            if (sc > best_score) { second = best_score; if (best) ma2_nav_free(best); best = cand; best_score = sc; best_name = k_nav_archives[k]; }
            else { if (sc > second) second = sc; ma2_nav_free(cand); }
        }
        if (best && best_score >= 0.5f && best_score - second >= 0.2f) {
            nav = best;
            snprintf(g_rt.nav_source, sizeof(g_rt.nav_source), "shipped:%s(%.2f)", best_name, best_score);
        } else if (best) ma2_nav_free(best);
    }
    if (!nav) {
        uint32_t fp = terrain_fingerprint();
        wchar_t path[MAX_PATH];
        int have_path = ma2_trav_ready() ? 0 : nav_cache_path(fp, path, MAX_PATH);
        if (have_path) nav = nav_load_file_w(path);
        if (nav) snprintf(g_rt.nav_source, sizeof(g_rt.nav_source), "cache:gen_%08X", fp);
        else {
            float cell = MEMF(t + 0x814), top = -1e9f;
            for (int i = 0; i < 256; i++) { float v = MEMF(t + 0x14 + i * 4); if (v > top) top = v; }
            Ma2NavGenParams gp;
            gp.min_x = 0.f; gp.min_z = 0.f;
            gp.max_x = (float)((int)MEM32(t + 4) - 1) * cell - 0.01f;
            gp.max_z = (float)((int)MEM32(t + 8) - 1) * cell - 0.01f;
            gp.cell = 20.f;
            gp.max_step = (float)env_int("RECOMP_BOT_NAV_STEP", 7);
            gp.probe_top = top + 10.f;
            nav = ma2_nav_generate(&gp, ground_fn, &g_probe);
            if (nav && have_path) ma2_nav_save_text(nav, path);
            if (nav && env_on("RECOMP_BOT_TRAV_DUMP")) { wchar_t dp[64]; swprintf(dp, 64, L"navdump_%08X.map", fp); ma2_nav_save_text(nav, dp); }
            snprintf(g_rt.nav_source, sizeof(g_rt.nav_source), "generated:gen_%08X", fp);
        }
    }
    g_rt.nav = nav;
    if (diag) fprintf(stderr, "[BOT-NAV] source=%s nodes=%d\n", nav ? g_rt.nav_source : "none",
                      nav ? ma2_nav_node_count(nav) : 0);
    stuck_memory_load(nav, map_base[0] ? map_base : NULL);
    {   /* RECOMP_BOT_PATHTEST="x1,z1,x2,z2": log the route the graph gives between two points */
        const char *pt = getenv("RECOMP_BOT_PATHTEST");
        float a1, a2, b1, b2;
        if (nav && pt && sscanf(pt, "%f,%f,%f,%f", &a1, &a2, &b1, &b2) == 4) {
            Ma2Vec3 from = { a1, 0.f, a2 }, to = { b1, 0.f, b2 }, wp[32];
            int n = ma2_nav_path(nav, from, to, wp, 32), i;
            float len = 0.f; Ma2Vec3 prev = from;
            for (i = 0; i < n; i++) { len += hypotf(wp[i].x - prev.x, wp[i].z - prev.z); prev = wp[i]; }
            fprintf(stderr, "[BOT-NAV] pathtest (%.0f,%.0f)->(%.0f,%.0f): %d waypoints, length %.0f m, straight %.0f m, connected=%d\n", a1, a2, b1, b2, n, len,
                    hypotf(b1 - a1, b2 - a2), ma2_nav_connected(nav, from, to));
            for (i = 0; i < n; i++) fprintf(stderr, "[BOT-NAV]   wp %.0f,%.0f\n", wp[i].x, wp[i].z);
        }
    }
    {   /* RECOMP_BOT_HPROBE="x0,z0,x1,z1,step[|...]" (diagnostic): terrain / surface height and grid clearance (light, heavy) along a line */
        extern float ma2_trav_clear_heavy(float x, float z);
        extern int ma2_trav_slope(float x, float z);
        const char *hp = getenv("RECOMP_BOT_HPROBE");
        if (hp) {
            char hb[512]; snprintf(hb, sizeof hb, "%s", hp);
            for (char *q = hb, *nx; q && *q; q = nx) {
                nx = strchr(q, '|'); if (nx) *nx++ = 0;
                float x0, z0, x1, z1, st; if (sscanf(q, "%f,%f,%f,%f,%f", &x0, &z0, &x1, &z1, &st) != 5 || st < 0.25f) continue;
                float L = hypotf(x1 - x0, z1 - z0);
                for (float d = 0.f; d <= L + 0.01f; d += st) {
                    float x = x0 + (x1 - x0) * d / L, z = z0 + (z1 - z0) * d / L; int ok1 = 0, ok2 = 0;
                    float th = terrain_height(x, z, &ok1), sh = surface_height(x, z, &ok2);
                    fprintf(stderr, "[HPROBE] %.1f,%.1f terrain %.2f%s surface %.2f%s clear %.1f heavy %.1f slope %d\n", x, z, th, ok1 ? "" : "?", sh, ok2 ? "" : "?", ma2_trav_clear(x, z, 0.0), ma2_trav_clear_heavy(x, z), ma2_trav_slope(x, z));
                }
            }
        }
    }
    HN("nav_graph_done");
    navcache_ready_log();
}

/* ------------------------------------------------------------------ */
/* World snapshot (guide 2.3/2.4/3.6)                                  */
/* ------------------------------------------------------------------ */
typedef struct Classes {
    uint32_t player, corporeal, mover, mech, tank, turret, flyer, spider, projectile, water;
    uint32_t powerup, flag, checkpoint, datacore, collection;
    uint32_t bdestroyed, cdestroyed;
} Classes;
static Classes g_cls;
static int flag_is_mover(uint32_t v) { return is_kind(v, g_cls.mover); }

static void classes_refresh(void)
{
    g_cls.player = class_id(CLS_MAPLAYER_VA, "MAPlayer");
    g_cls.corporeal = class_id(CLS_MACORPOREAL_VA, "MACorporeal");
    g_cls.water = class_id(0, "MAWater");
    g_cls.mover = class_id(CLS_MAMOVER_VA, "MAMover");
    g_cls.mech = class_id(CLS_MAMECH_VA, "MAMech");
    g_cls.tank = class_id(CLS_MATANK_VA, "MATank");
    g_cls.turret = class_id(CLS_MATURRET_VA, "MATurret");
    g_cls.flyer = class_id(CLS_MAFLYER_VA, "MAFlyer");
    g_cls.spider = class_id(CLS_MASPIDER_VA, "MASpider");
    g_cls.projectile = class_id_of("MAProjectile");
    g_cls.powerup = class_id_of("MAPowerUp");
    g_cls.flag = class_id_of("MAFlagCTF");
    g_cls.checkpoint = class_id_of("MACheckpoint");
    g_cls.datacore = class_id_of("MASnatchItDataCore");
    g_cls.collection = class_id_of("MACollectionArea");
    g_cls.bdestroyed = class_id_of("MABuildingDestroyed");
    g_cls.cdestroyed = class_id_of("MACorporealDestroyed");
}

/* MACorporeal flags (entity+0x80) that make scenery break when a 'Mech walks into it. */
#define CORP_BREAKABLE ((1u << 6) | (1u << 20) | (1u << 27) | (1u << 31))

/* Thin static scenery -- pylons, posts, lamp poles -- slips between the 2 m obstacle rays. Every
 * static MACorporeal gets a second, 0.5 m pass of the same downward ray over +-4 m around it; the
 * cells its own collider covers above 2.5 m become obstacles (breakable ones for all but 'Mechs). */
/* Props found only side-on (no downward hit): re-stamped after any local re-probe, which uses
 * downward rays and would clear them again. */
static struct { Ma2Vec3 p; float r; } g_side[512];
static int g_nside;
static void side_prop_add(Ma2Vec3 p, float r) { if (g_nside < 512) { g_side[g_nside].p = p; g_side[g_nside].r = r; g_nside++; } }
/* Something destroyed here: what was learned about its walls no longer holds. */
static void side_prop_forget(Ma2Vec3 p, float r)
{
    for (int i = 0; i < g_nside; i++)
        if (hypotf(g_side[i].p.x - p.x, g_side[i].p.z - p.z) < r) { g_side[i] = g_side[--g_nside]; i--; }
}
/* W2: resumable (NavSlice): trav_fine_props_run(1, 0) starts, (0, ms) continues a slice; 1 = finished. */
static struct { uint32_t stack[512]; int sp, visited, nprops, nhit, ncells, nside; long rays; } g_fp;
static int trav_fine_props_run(int begin, double budget_ms)
{
    LARGE_INTEGER fq0, fq1, ffq;
    uint32_t *stack = g_fp.stack;
    if (begin) {
        memset(&g_fp, 0, sizeof g_fp);
        if (!g_probe.phys || !ma2_trav_ready()) return 1;
        uint32_t game = MEM32(VA_GAME);
        if (!gva_ok(game, 0xA8)) return 1;
        uint32_t r1 = MEM32(game + 0xA4);
        if (!gva_ok(r1, 8)) return 1;
        uint32_t root = MEM32(r1 + 4);
        if (!gva_ok(root, 0x30)) return 1;
        memset(&g_fp, 0, sizeof g_fp);
        g_nside = 0;
        stack[g_fp.sp++] = root;
        if (budget_ms < 0.0) return 0;      /* W2b: negative budget = set up only (the old (1, 0.0) call ran the WHOLE pass unbudgeted: the 420 ms frame) */
    }
    QueryPerformanceFrequency(&ffq); QueryPerformanceCounter(&fq0);
    #define sp g_fp.sp
    #define visited g_fp.visited
    #define nprops g_fp.nprops
    #define nhit g_fp.nhit
    #define ncells g_fp.ncells
    #define nside g_fp.nside
    #define rays g_fp.rays
    while (sp > 0 && visited < 8192 && rays < 1200000) {
        if (budget_ms > 0.0) {
            QueryPerformanceCounter(&fq1);
            if ((double)(fq1.QuadPart - fq0.QuadPart) * 1000.0 / (double)ffq.QuadPart >= budget_ms) return 0;
        }
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58) || !is_kind(e, g_cls.corporeal) || (is_kind(e, g_cls.mover) && !is_kind(e, g_cls.turret)) || !gva_ok(e, 0x4D0)) continue;   /* turrets: solid too */
        uint32_t node = MEM32(e + 0x94);
        if (!gva_ok(node, 0x100)) continue;
        Ma2Vec3 c = read_vec3(node + 0xF0);
        if (!(c.x == c.x) || fabsf(c.x) + fabsf(c.z) < 1.f) continue;
        int brk = (MEM32(e + 0x80) & CORP_BREAKABLE) != 0, any = 0;
        nprops++;
        for (int iz = -8; iz <= 8; iz++)
            for (int ix = -8; ix <= 8; ix++) {
                int ok = 0;
                float x = c.x + 0.5f * (float)ix, z = c.z + 0.5f * (float)iz, g = terrain_height(x, z, &ok);
                if (!ok) continue;
                float t = 0.f; uint32_t ob = 0;
                Ma2Vec3 o = { x, g + 40.f, z }, dn = { 0.f, -1.f, 0.f };
                rays++;
                if (!ray_cast(&g_probe, o, dn, 44.f, 1, 1, &t, &ob)) continue;
                uint32_t owner = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0;
                if (owner != e || 40.f - t < 2.5f) continue;
                ma2_trav_block(x, z, brk); any = 1; ncells++;
            }
        if (!any) {
            /* Nothing from above -- some props (team pylons) only answer side-on rays: 8 rays at
             * chest height from 10 m out; if they meet it, its measured outline is solid. */
            int okg = 0;
            float g = terrain_height(c.x, c.z, &okg), mx = 0.f;
            for (int kd = 0; okg && kd < 8; kd++) {
                float a = (float)kd * 0.7853982f, dx = sinf(a), dz = cosf(a), t = 0.f;
                uint32_t ob = 0;
                Ma2Vec3 o = { c.x + dx * 10.f, g + 1.5f, c.z + dz * 10.f }, d = { -dx, 0.f, -dz };
                rays++;
                if (ray_cast(&g_probe, o, d, 10.f, 1, 1, &t, &ob) && gva_ok(ob, 0x130) && MEM32(ob + 0x128) == e && 10.f - t > mx) mx = 10.f - t;
            }
            if (mx > 0.3f) { ncells += ma2_trav_block_disc(c.x, c.z, mx + 0.6f); side_prop_add(c, mx + 0.6f); nside++; any = 1; }
        }
        if (any) nhit++;
    }
    ma2_trav_refresh();
    if (env_on("RECOMP_BOT_DIAG"))
        fprintf(stderr, "[BOT-NAV] fine prop pass: %d props, %d with a collider above 2.5 m (%d seen only side-on), %d samples blocked, %ld rays\n", nprops, nhit, nside, ncells, rays);
    #undef sp
    #undef visited
    #undef nprops
    #undef nhit
    #undef ncells
    #undef nside
    #undef rays
    return 1;
}
static void trav_fine_props(void) { trav_fine_props_run(1, 0.0); trav_fine_props_run(0, 0.0); }

/* ---- NavCache (1.0 release; USER-approved): the static obstacle grid on disk ----
 * Bots wait 1.1-1.6 s at the start of every match while the obstacle grid is built (the sliced build above). Its STATIC result --
 * terrain, steep cells, collider tops, props, the fine prop pass -- is the same every time for one map and mode, so it is kept in
 * %LOCALAPPDATA%\MechAssault2\NavCache\<map>_m<mode>.navc and loaded in the first frame. Live parts (turrets, vehicles, wrecks,
 * destroyed buildings) are applied live exactly as before: turret_seen waits for the build, so no turret disc is baked in.
 * Key (ma2_bot_nav.c checks it byte for byte): map archive + size, game mode, grid extent, terrain and prop fingerprints,
 * NAV_CACHE_VERSION, the nav.c grid constants and the build stamp. Any mismatch or read error = the normal build, and the file is
 * rewritten. Writes are atomic (temp file + rename). Stock maps only (a scenario is edited too often to be worth a file).
 * Switch: NavCache (RECOMP_BOT_NAV_CACHE / [Bots] NavCache), DEFAULT ON (1.0.1). Byte-identical to a fresh build on 8 maps; on Going
 * Commando two FRESH builds already differ by 1-3 cells of 258,064 (edges of swaying MechTopples trees, blk only), so a cached grid is
 * as good as any fresh one there (1.0 NAVCACHE-RESULT.md). At most NAVC_KEEP files are kept (newest by write time, ~8-17 MB each).
 * RECOMP_BOT_NAV_CACHE_VERIFY=1: on a hit the grid is
 * still built normally and compared with the file, array by array (derived maps re-derived from the cached ones):
 *   [BOT-NAV-CACHE] verify map=<m> mode=<g> identical=1/0 bytes=<n> first_diff=<offset>
 * [C7] navcache hit miss write reject (+ write_fail verify_ok verify_diff). */
#define NAV_CACHE_VERSION 2   /* bump whenever obstacle_top, trav_fine_props_run or the ma2_trav_* grid code / layout changes */
static long long nc_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static double nc_ms(long long a, long long b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)(b - a) * 1000.0 / (double)f.QuadPart; }
static int nc_log(void) { return env_on("RECOMP_BOT_DIAG") || env_on("RECOMP_BOT_NAV_CACHE_VERIFY"); }
static void navcache_level_reset(void)
{
    if (g_nc.image) ma2_trav_cache_free(g_nc.image);
    memset(&g_nc, 0, sizeof g_nc); ma2_trav_set_quant(0);
}
/* Fingerprint of the static props (same walk and filter as the fine prop pass): class, flags and position of every solid non-mover. */
static uint64_t navcache_prop_fp(int *count)
{
    uint64_t h = 1469598103934665603ull; int visited = 0, sp = 0, n = 0;
    uint32_t stack[512];
    *count = 0;
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return 0;
    uint32_t r1 = MEM32(game + 0xA4);
    if (!gva_ok(r1, 8)) return 0;
    uint32_t root = MEM32(r1 + 4);
    if (!gva_ok(root, 0x30)) return 0;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192) {
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58) || !is_kind(e, g_cls.corporeal) || (is_kind(e, g_cls.mover) && !is_kind(e, g_cls.turret)) || !gva_ok(e, 0x4D0)) continue;
        uint32_t node = MEM32(e + 0x94);
        if (!gva_ok(node, 0x100)) continue;
        Ma2Vec3 c = read_vec3(node + 0xF0);
        uint32_t w[5] = { MEM32(e + 0x28), MEM32(e + 0x80), 0, 0, 0 };
        memcpy(&w[2], &c.x, 4); memcpy(&w[3], &c.y, 4); memcpy(&w[4], &c.z, 4);
        for (int i = 0; i < 5; i++) { h = (h ^ w[i]) * 0x100000001B3ull; h ^= h >> 29; }
        n++;
    }
    *count = n;
    return h;
}
/* Verify aid (RECOMP_BOT_NAV_CACHE_VERIFY): the solid props within 14 m of a cell that differs between a cached and a fresh grid. */
void navcache_cell_probe(float x, float z)
{
    int visited = 0, sp = 0, shown = 0;
    uint32_t stack[512];
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4);
    if (!gva_ok(r1, 8)) return;
    uint32_t root = MEM32(r1 + 4);
    if (!gva_ok(root, 0x30)) return;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192 && shown < 6) {
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58) || !is_kind(e, g_cls.corporeal) || !gva_ok(e, 0x4D0)) continue;
        uint32_t node = MEM32(e + 0x94);
        if (!gva_ok(node, 0x100)) continue;
        Ma2Vec3 c = read_vec3(node + 0xF0);
        float d = hypotf(c.x - x, c.z - z);
        if (d > 14.f) continue;
        shown++;
        fprintf(stderr, "[BOT-NAV-CACHE]     near e=%08X cls=%08X vt=%08X flags=%08X mover=%d turret=%d at %.1f,%.1f,%.1f d=%.1f\n", e, MEM32(e + 0x28), MEM32(e), MEM32(e + 0x80),
                is_kind(e, g_cls.mover), is_kind(e, g_cls.turret), c.x, c.y, c.z, d);
    }
}

/* Keeps the cache folder bounded: all map x mode files would be ~1 GB; drop the oldest beyond NAVC_KEEP after each write. */
#define NAVC_KEEP 24
static void navcache_prune(void)
{
    wchar_t dir[MAX_PATH], pat[MAX_PATH], fp[MAX_PATH];
    struct { FILETIME t; wchar_t n[MAX_PATH]; } old = { { 0 }, { 0 } };
    size_t L = wcslen(g_nc.path);
    while (L && g_nc.path[L - 1] != L'\\') L--;
    if (!L || L >= MAX_PATH - 8) return;
    wmemcpy(dir, g_nc.path, L); dir[L] = 0;
    swprintf(pat, MAX_PATH, L"%ls*.navc", dir);
    for (int guard = 0; guard < 64; guard++) {
        WIN32_FIND_DATAW fd; int n = 0;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!n++ || CompareFileTime(&fd.ftLastWriteTime, &old.t) < 0) { old.t = fd.ftLastWriteTime; wcsncpy(old.n, fd.cFileName, MAX_PATH - 1); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (n <= NAVC_KEEP) return;
        swprintf(fp, MAX_PATH, L"%ls%ls", dir, old.n);
        if (!DeleteFileW(fp)) return;
        c7_add("navcache", "prune", 1);
    }
}

/* 1 = the grid was installed from the cache (ready now); 0 = build it (g_nc.write_pending says whether to write it afterwards). */
static int navcache_try(float max_x, float max_z)
{
    long long t_a = nc_qpc();
    g_nc.tried = 1;
    if (!c7_on("navcache", "RECOMP_BOT_NAV_CACHE", "NavCache", 1)) return 0;
    const char *arch = ma2_bot_map_archive();
    if (!arch || !*arch || !g_probe.phys || ma2_scn_level_custom()) return 0;
    wchar_t base[MAX_PATH], dir[MAX_PATH], wm[48];
    DWORD bn = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!bn || bn >= MAX_PATH - 96) return 0;
    size_t k = strcspn(arch, ".");
    if (k >= sizeof g_nc.map) k = sizeof g_nc.map - 1;
    memcpy(g_nc.map, arch, k); g_nc.map[k] = 0;
    for (size_t i = 0; i <= k; i++) wm[i] = (wchar_t)(unsigned char)g_nc.map[i];
    swprintf(dir, MAX_PATH, L"%ls\\MechAssault2", base); CreateDirectoryW(dir, NULL);
    swprintf(dir, MAX_PATH, L"%ls\\MechAssault2\\NavCache", base); CreateDirectoryW(dir, NULL);
    swprintf(g_nc.path, MAX_PATH, L"%ls\\%ls_m%d.navc", dir, wm, g_rt.game_type);
    long long asz = 0; char ap[MAX_PATH]; WIN32_FILE_ATTRIBUTE_DATA fad;
    snprintf(ap, sizeof ap, "game_files\\%s", arch);
    if (GetFileAttributesExA(ap, GetFileExInfoStandard, &fad)) asz = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    int npf = 0; uint64_t pfp = navcache_prop_fp(&npf);
    snprintf(g_nc.key, sizeof g_nc.key, "NAVC|cv%d|%s|%s %s|map=%s|msz=%lld|mode=%d|mx=%.2f|mz=%.2f|tfp=%08X|pfp=%016llX|np=%d|uw=%d",
             NAV_CACHE_VERSION, ma2_trav_cache_params(), __DATE__, __TIME__, arch, asz, g_rt.game_type, max_x, max_z,
             terrain_fingerprint(), (unsigned long long)pfp, npf, env_int("RECOMP_BOT_UNDERWATER", 1));
    g_nc.on = 1; ma2_trav_set_quant(1);   /* surfaces to 1/256 m so a rebuilt grid can equal a cached one */
    int st = 0; void *img = ma2_trav_cache_read(g_nc.path, g_nc.key, max_x, max_z, &st);
    if (!img) {
        c7_add("navcache", st ? "reject" : "miss", 1);
        g_nc.write_pending = 1; g_nc.via = st ? "reject" : "miss";
        if (nc_log()) fprintf(stderr, "[BOT-NAV-CACHE] %s map=%s mode=%d (%.1f ms to look); building the grid\n[BOT-NAV-CACHE]   key %s\n", g_nc.via, g_nc.map, g_rt.game_type, nc_ms(t_a, nc_qpc()), g_nc.key);
        return 0;
    }
    c7_add("navcache", "hit", 1);
    if (env_on("RECOMP_BOT_NAV_CACHE_VERIFY")) {
        g_nc.image = img; g_nc.via = "hit+verify";
        fprintf(stderr, "[BOT-NAV-CACHE] hit map=%s mode=%d: verify mode, building the grid anyway\n", g_nc.map, g_rt.game_type);
        return 0;
    }
    uint32_t el = 0;
    int ok = ma2_trav_cache_install(img, g_side, (uint32_t)sizeof g_side, &el) && el % (uint32_t)sizeof g_side[0] == 0;
    ma2_trav_cache_free(img);
    if (!ok) {
        c7_add("navcache", "reject", 1);
        g_nc.write_pending = 1; g_nc.via = "reject";
        if (nc_log()) fprintf(stderr, "[BOT-NAV-CACHE] reject map=%s mode=%d (install failed); building the grid\n", g_nc.map, g_rt.game_type);
        return 0;
    }
    g_nside = (int)(el / (uint32_t)sizeof g_side[0]);
    g_nc.hit = 1; g_nc.via = "hit";
    if (nc_log()) {
        int gw, gh, no, ns; ma2_trav_stats(&gw, &gh, &no, &ns);
        fprintf(stderr, "[BOT-NAV-CACHE] hit map=%s mode=%d grid %dx%d obstacles=%d steep=%d side_props=%d installed in %.1f ms state=%016llX\n", g_nc.map, g_rt.game_type,
                gw, gh, no, ns, g_nside, nc_ms(t_a, nc_qpc()), (unsigned long long)ma2_trav_state_hash());
    }
    return 1;
}
/* After a normal build (okb = it worked): verify against the cache image, or write the cache. */
static void navcache_after_build(int okb)
{
    if (!g_nc.on || g_nc.hit) return;
    if (!okb) { if (g_nc.image) { ma2_trav_cache_free(g_nc.image); g_nc.image = NULL; } return; }
    if (g_nc.image) {
        long long fd = 0, by = 0; const char *wh = "";
        int same = ma2_trav_cache_compare(g_nc.image, g_side, (uint32_t)(g_nside * (int)sizeof g_side[0]), &fd, &by, &wh);
        fprintf(stderr, "[BOT-NAV-CACHE] verify map=%s mode=%d identical=%d bytes=%lld first_diff=%lld\n", g_nc.map, g_rt.game_type, same, by, fd);
        if (!same) fprintf(stderr, "[BOT-NAV-CACHE] verify DIFFERENT in %s\n", wh);
        else if (nc_log()) fprintf(stderr, "[BOT-NAV-CACHE] verify state=%016llX\n", (unsigned long long)ma2_trav_state_hash());
        c7_add("navcache", same ? "verify_ok" : "verify_diff", 1);
        ma2_trav_cache_free(g_nc.image); g_nc.image = NULL;
        return;
    }
    if (g_nc.write_pending) {
        long long t_a = nc_qpc();
        int ok = ma2_trav_cache_save(g_nc.path, g_nc.key, g_side, (uint32_t)(g_nside * (int)sizeof g_side[0]));
        g_nc.write_pending = 0;
        c7_add("navcache", ok ? "write" : "write_fail", 1);
        if (ok) navcache_prune();
        if (nc_log()) fprintf(stderr, "[BOT-NAV-CACHE] %s map=%s mode=%d side_props=%d in %.1f ms state=%016llX\n", ok ? "write" : "write FAILED", g_nc.map, g_rt.game_type, g_nside,
                              nc_ms(t_a, nc_qpc()), (unsigned long long)ma2_trav_state_hash());
    }
}
/* The bots' ready time: wall ms from the first frame the terrain was there to the end of ensure_nav (grid and graph). */
static void navcache_ready_log(void)
{
    if (!nc_log() || !g_nc.t0) return;
    long long t_e = nc_qpc();
    fprintf(stderr, "[BOT-NAV-CACHE] ready via=%s map=%s mode=%d total=%.0f ms (grid %.0f ms, graph %.0f ms)\n", g_nc.via ? g_nc.via : "off", g_nc.map, g_rt.game_type,
            nc_ms(g_nc.t0, t_e), nc_ms(g_nc.t0, g_nc.t_grid ? g_nc.t_grid : t_e), g_nc.t_grid ? nc_ms(g_nc.t_grid, t_e) : 0.0);
}

/* Top height of the highest solid static collider under (x, z): vertical ray through the
 * game's collision space (trees, rocks, buildings; water volumes and vehicles ignored). */
/* TallSolid (default on, RECOMP_BOT_TALL_SOLID / [Bots] TallSolid): the probe below started at ground+40, and a collider's faces are
 * one-sided -- a ray that starts INSIDE a building sees none of it. So every building over 40 m was open ground in the grid: Rush Hour's
 * 42-50 m block at 1118-1138,1322-1344 (FPROBE D_85b1: down from +100 m hits its roof at +42/+50, up from +0.5 m hits nothing) read as a
 * corridor between two 7-9 m walls, and team 2's 'Mechs planned into it and stood 25-30 s at its face (Z_85a/b). Now the first ray comes
 * from far above every height (g_obst_top_y = terrain top + 160 m). When it meets something above ground+40, the 44 m ray gives the
 * surface below, and a ray UP from that surface tells the two apart: it meets an underside (a deck, an overhang: open below, the low
 * surface stays) or nothing (it is inside a solid: the roof is the surface). Movers / projectiles / water above: the old ray. [C7] tallsolid
 * cells (solid) open (open below). Same ray count where nothing stands above ground+40 (one longer ray instead of the 44 m one). */
static float obstacle_top(void *ctx, float x, float y_from, float z, int *ok)
{
    const ProbeCtx *p = (const ProbeCtx *)ctx;
    float t = 0.f; uint32_t ob = 0;
    Ma2Vec3 o = { x, y_from, z }, dn = { 0.f, -1.f, 0.f };
    static int tall_on = -1;
    if (ok) *ok = 0;
    if (tall_on < 0) tall_on = c7_on("tallsolid", "RECOMP_BOT_TALL_SOLID", "TallSolid", 1);
    if (tall_on && g_obst_top_y > y_from + 1.f) {
        float span = g_obst_top_y - y_from, th = 0.f; uint32_t oh = 0;
        Ma2Vec3 oT = { x, g_obst_top_y, z };
        if (!ray_cast(p, oT, dn, span + 44.f, 1, 1, &th, &oh)) return 0.f;   /* nothing down to ground-4: the 44 m ray meets nothing either */
        if (th >= span) { t = th - span; ob = oh; }                         /* the first thing is below ground+40: the 44 m ray's own hit */
        else {
            uint32_t owh = gva_ok(oh, 0x130) ? MEM32(oh + 0x128) : 0, clh = gva_ok(owh, 0x30) ? MEM32(owh + 0x28) : 0;
            float tl = 0.f; uint32_t ol = 0;
            int low = ray_cast(p, o, dn, 44.f, 1, 1, &tl, &ol);
            int dyn = (owh && ((is_kind(owh, g_cls.mover) && !is_kind(owh, g_cls.turret)) || is_kind(owh, g_cls.projectile))) || (clh && clh == g_cls.water);
            float H = g_obst_top_y - th, s = low ? y_from - tl : y_from - 40.f, tu = 0.f; uint32_t ou = 0;
            Ma2Vec3 u0 = { x, s + 0.5f, z }, up = { 0.f, 1.f, 0.f };
            if (!dyn && H - s > 1.5f && !ray_cast(p, u0, up, H - s - 0.7f, 1, 1, &tu, &ou)) { t = y_from - H; ob = oh; g_tall_stat.solid++; }
            else { if (!dyn) g_tall_stat.open++; if (!low) return 0.f; t = tl; ob = ol; }
        }
    }
    else if (!ray_cast(p, o, dn, 44.f, 1, 1, &t, &ob)) return 0.f;   /* from ground+40 down to ground-4 */
    uint32_t owner = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0;
    uint32_t cls = gva_ok(owner, 0x30) ? MEM32(owner + 0x28) : 0;
    /* Water first: what stands under its surface is still in a pilot's way (ed4: a 6-8 m
     * structure beside the VTOL bay at 174-195,272-297 under the editor's water plane read as
     * open bed; pilots stalled at it 25 s). Probe on from just below the surface. RECOMP_BOT_UNDERWATER=0 = old. */
    static int under_on = -1;
    if (under_on < 0) under_on = env_int("RECOMP_BOT_UNDERWATER", 1);
    if (cls && cls == g_cls.water && under_on) {
        float t2 = 0.f; uint32_t ob2 = 0;
        Ma2Vec3 o2 = { x, y_from - t - 0.05f, z };
        if (44.f - t > 0.2f && ray_cast(p, o2, dn, 44.f - t - 0.05f, 1, 1, &t2, &ob2)) {
            uint32_t ow2 = gva_ok(ob2, 0x130) ? MEM32(ob2 + 0x128) : 0;
            uint32_t cl2 = gva_ok(ow2, 0x30) ? MEM32(ow2 + 0x28) : 0;
            if (cl2 != g_cls.water) { g_obst_stat.under++; ob = ob2; owner = ow2; cls = cl2; t += 0.05f + t2; }
        }
    }
    int i;
    for (i = 0; i < g_obst_stat.nc; i++) if (g_obst_stat.cls[i] == cls) break;
    if (i == g_obst_stat.nc && i < 12) { g_obst_stat.cls[i] = cls; g_obst_stat.cnt[i] = 0; g_obst_stat.nc++; }
    if (i < 12) g_obst_stat.cnt[i]++;
    if (cls && cls == g_cls.water) { g_obst_stat.water++; if (ok) *ok = 3; return 0.f; }   /* 3 = water here (the grid keeps the bed below) */
    /* Vehicles and units move (planned round as they stand); turrets stay put -- solid scenery. */
    if (owner && ((is_kind(owner, g_cls.mover) && !is_kind(owner, g_cls.turret)) || is_kind(owner, g_cls.projectile))) { g_obst_stat.dyn++; return 0.f; }
    g_obst_stat.hits++;
    if (ok) *ok = 1;
    /* Scenery a 'Mech breaks by walking into it: MACorporeal flags at +0x80 (written by the
     * property serializer sub_D8561, this = entity+0x20): MechStepDestroy bit 6, MechTopples
     * bit 20, TreeGroup bit 27, SnowyTreeGroup bit 31. Posts, pylons, rocks have none of them. */
    if (owner && gva_ok(owner, 0x88) && is_kind(owner, g_cls.corporeal) && (MEM32(owner + 0x80) & CORP_BREAKABLE)) {
        g_obst_stat.soft++;
        if (ok) *ok = 2;
    }
    return y_from - t;
}


/* Fixed gun emplacements. Some MATank instances are authored with no drive: the physics root's
 * speed and acceleration limits ([actor+0x8C] +0xBC/+0xC0, also +0x98/+0xC4/+0xD0) are 0. Bogged
 * Down has one by each base (348,1686 and 861,909: 850 HP, a ~2 m hull; the real tanks there have
 * 2000 HP and a 4-7 m hull); Ill Wind, On The Rocks and Go Commando have them too. A player in one
 * turns and fires it but never moves it (a human held forward in one: it stayed put, log
 * 2026-10-14). They are turrets, not rides: kind TURRET -- no bot picks one (vehicle_pref 0),
 * ctrl_tick never drives one, and a bot that finds itself in one gets out. (tank_repair used to
 * write tank drive limits into them when a bot got in, and the bot drove the gun across the map.) */
static int tank_fixed(uint32_t a)
{
    uint32_t comp = MEM32(a + 0x8C);
    return gva_ok(comp, 0x200) && MEMF(comp + 0xBC) == 0.0f && MEMF(comp + 0xC0) == 0.0f;
}
/* RECOMP_BOT_DIAG: each fixed gun once a match. */
static void fixed_gun_note(uint32_t e, Ma2Vec3 p, int hp, double now)
{
    static uint32_t seen[16]; static int n; static double last;
    if (now < last) n = 0;                                /* a new match: the clock went back */
    last = now;
    if (!env_on("RECOMP_BOT_DIAG")) return;
    for (int i = 0; i < n; i++) if (seen[i] == e) return;
    if (n < 16) seen[n++] = e;
    fprintf(stderr, "[BOT-FIXEDGUN] t=%.1f %08X at %.0f,%.0f hp=%d: an MATank with no drive (a gun emplacement) -> kind TURRET, not a ride\n",
            now, e, p.x, p.z, hp);
}

/* Control-behaviour factory order (sub_C9084). */
/* push W1 (the 'crash ~5 s after a hijack', 2026-10-27): the bot controller writes the AI-drive flags
 * at body+0x1E8 (|= 3) and asks the body for its AI (vt+0x200). Both only hold for a MAMech (the
 * on-foot pilot is one) or a MAMover. Right after a NeuroHack throws a bot out of its 'Mech, the
 * player's controlled actor is briefly a corporeal that is neither (actor_kind's MACorporeal fallback
 * calls it a PILOT); its +0x1E8 is an embedded object's vtable pointer, and |= 3 shifted it by 3
 * bytes: every virtual call through it then read a function address <<8 (0x01ECA000 = 0x0001ECA0,
 * 0x0817E000 = 0x000817E0), giving the [ICALL] wild/null calls, R6025 pure virtual, divide faults
 * and the hang of log 2026-10-27. RECOMP_BOT_BODY_GUARD=0 restores the old (unguarded) writes. */
int ma2_bot_body_ok(uint32_t a)
{
    static int on = -1, said;
    if (on < 0) on = env_int("RECOMP_BOT_BODY_GUARD", 1);
    if (!on) return 1;
    if (!gva_ok(a, 0x4D0)) return 0;
    uint32_t c = MEM32(a + 0x28);
    if (c == g_cls.mech || is_kind(a, g_cls.mover)) return 1;
    c7_add("bodyguard", "skips", 1);   /* [C7] bodyguard skips= */
    if (said < 8) { said++; fprintf(stderr, "[BOT-BODY] t=%.1f actor %08X class %08X vt %08X is no MAMech/MAMover: no AI flags, no drive\n", g_rt.world.time, a, c, MEM32(a)); }
    return 0;
}
static int actor_kind(uint32_t a)
{
    if (!gva_ok(a, 0x4D0)) return MA2_KIND_NONE;
    /* MAMech property bits, 64-bit at +0x268 (read by sub_107175):
     * IsPersonMech = bit 49 (the on-foot pilot is a MAMech), IsBattleArmor = bit 18. */
    if (MEM32(a + 0x28) == g_cls.mech) {
        if ((MEM32(a + 0x26C) >> 17) & 1) return MA2_KIND_PILOT;
        return (MEM32(a + 0x268) >> 18) & 1 ? MA2_KIND_BATTLEARMOR : MA2_KIND_MECH;
    }
    if (is_kind(a, g_cls.flyer)) return (MEM32(a + 0x260) >> 2) & 1 ? MA2_KIND_VTOL : MA2_KIND_FLYER;
    if (is_kind(a, g_cls.tank)) return tank_fixed(a) ? MA2_KIND_TURRET : MA2_KIND_TANK;   /* no drive: a gun emplacement */
    if (is_kind(a, g_cls.turret)) return MA2_KIND_TURRET;
    if (is_kind(a, g_cls.mover)) return MA2_KIND_MOVER;
    if (is_kind(a, g_cls.corporeal)) {
        /* push W1b: a NeuroHack victim's player holds a PLAIN MACorporeal (class CA77C1A6, vt 00466BA0,
         * a 0x1A8-byte object) at the 'Mech for ~10 s until he respawns at a spawn point as a fresh
         * pilot MAMech (w1b runs: [BOT-KIDS] no MAMech/MAMover child, deaths +1, then kind=1 ~900 m away).
         * It is no pilot: nothing to drive (+0x1E8 lies past its end), nothing to shoot or hack. NONE makes
         * the brain wait (goal none), other bots ignore it, and no AI-flag write reaches it.
         * RECOMP_BOT_PLAIN_CORP=0 restores the old 'PILOT' fallback. [C7] bodyguard plain= counts reads. */
        static int plain = -1;
        if (plain < 0) plain = env_int("RECOMP_BOT_PLAIN_CORP", 1);
        if (plain && MEM32(a + 0x28) == g_cls.corporeal) { c7_add("bodyguard", "plain", 1); return MA2_KIND_NONE; }
        return MA2_KIND_PILOT;
    }
    return MA2_KIND_NONE;
}

static Ma2Vec3 read_vec3(uint32_t a) { Ma2Vec3 v = { MEMF(a), MEMF(a + 4), MEMF(a + 8) }; return v; }

/* World position: the physics position (+0x4C0) for walkers and ground
 * vehicles; flyers keep theirs only in the scene node (+0x94 -> +0xF0). */
static Ma2Vec3 actor_pos(uint32_t a)
{
    /* The scene node's world position is right for every kind; +0x4C0 is the
     * physics position for walkers but holds something else for tanks. */
    uint32_t node = MEM32(a + 0x94);
    if (gva_ok(node, 0x100)) {
        Ma2Vec3 n = read_vec3(node + 0xF0);
        if (n.x == n.x && n.z == n.z && fabsf(n.x) + fabsf(n.y) + fabsf(n.z) >= 1.f) return n;
    }
    return read_vec3(a + 0x4C0);
}

/* Heading from the scene node world matrix (actor+0x94 -> node, rows at
 * +0xC0 right/+0xD0 up/+0xE0 forward/+0xF0 position, found in a memory
 * capture). Validated per call: node+0xF0 must equal actor+0x4C0. */
static int actor_yaw(uint32_t a, Ma2Vec3 pos, float *yaw)
{
    uint32_t node = MEM32(a + 0x94);
    if (!gva_ok(node, 0x100)) return 0;
    Ma2Vec3 np = read_vec3(node + 0xF0);
    float dx = np.x - pos.x, dy = np.y - pos.y, dz = np.z - pos.z;
    if (!(dx * dx + dy * dy + dz * dz < 4.f)) return 0;
    float fx = MEMF(node + 0xE0), fz = MEMF(node + 0xE8);
    if (!(fx * fx + fz * fz > 1e-4f)) return 0;
    *yaw = atan2f(fx, fz);
    return 1;
}

static int actor_int(uint32_t a, uint32_t slot, uint32_t site, int fallback)
{
    int ok; uint32_t v = vcall0(a, slot, site, &ok);
    return ok ? (int)v : fallback;
}


static int player_stat(uint32_t pl, int id);   /* ma2_bot_objective.c */
/* sub_C8E6A: first child (+0x0C, siblings +0x08) that is-a MACorporeal. */
static uint32_t player_actor(uint32_t p)
{
    if (!g_cls.corporeal) return 0;
    uint32_t e = MEM32(p + 0xC);
    for (int n = 0; n < 64 && gva_ok(e, 0x30); n++, e = MEM32(e + 8))
        if (gva_ok(MEM32(e), 0x58) && is_kind(e, g_cls.corporeal)) {
            /* RECOMP_BOT_KIDS_DIAG=1 (test only): [BOT-KIDS] the player's children (and the first one's)
             * when its actor is a plain MACorporeal (the NeuroHack-victim placeholder, see actor_kind). */
            static int diag = -1, dumps;
            if (diag < 0) diag = env_int("RECOMP_BOT_KIDS_DIAG", 0);
            if (diag && dumps < 6 && MEM32(e + 0x28) == g_cls.corporeal) {
                dumps++;
                fprintf(stderr, "[BOT-KIDS] t=%.1f player %08X actor %08X (plain MACorporeal) deaths=%d; children:", g_rt.world.time, p, e, player_stat(p, 5));
                for (uint32_t list = MEM32(p + 0xC), k = 0; k < 2; k++, list = MEM32(e + 0xC)) {
                    if (k) fprintf(stderr, " | actor's children:");
                    uint32_t c = list;
                    for (int m = 0; m < 12 && gva_ok(c, 0x30); m++, c = MEM32(c + 8))
                        fprintf(stderr, " %08X/cls%08X/vt%08X", c, MEM32(c + 0x28), MEM32(c));
                }
                fputc('\n', stderr);
            }
            return e;
        }
    return 0;
}

/* First child of the given class id (exact), or 0. */
static uint32_t player_child(uint32_t p, uint32_t cls)
{
    uint32_t e = MEM32(p + 0xC);
    for (int n = 0; n < 64 && gva_ok(e, 0x30); n++, e = MEM32(e + 8))
        if (MEM32(e + 0x28) == cls) return e;
    return 0;
}

/* Diagnostic: furthest gate reached by each 'Mech control tick (sub_172E96). */
static uint32_t gate_obj[24];
static int gate_stage[24];
void ma2_bot_gate_mark(uint32_t obj, int stage)
{
    for (int i = 0; i < 24; i++) {
        if (gate_obj[i] == obj || !gate_obj[i]) {
            gate_obj[i] = obj;
            if (stage > gate_stage[i] || stage == 1) gate_stage[i] = stage;
            return;
        }
    }
}

/* Diagnostic (RECOMP_BOT_WATCH): hardware write watch on the first player's
 * camera controller (+0x2F4), then on that controller's +0x14 link. Logs the
 * host backtrace (exe RVAs) of each writer. */
#define WATCH_ARM_CODE 0xE0B07001u
static uint32_t watch_addr[4];
static int watch_hits;
static void watch_set(CONTEXT *c, int slot, uint32_t gva)
{
    watch_addr[slot] = gva;
    uintptr_t h = (uintptr_t)XBOX_PTR(gva);
    if (slot == 0) c->Dr0 = h; else if (slot == 1) c->Dr1 = h; else if (slot == 2) c->Dr2 = h; else c->Dr3 = h;
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    c->Dr7 |= (1ull << (slot * 2)) | (1ull << (16 + slot * 4)) | (3ull << (18 + slot * 4));   /* write, 4 bytes */
}
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    if (e->ExceptionRecord->ExceptionCode == WATCH_ARM_CODE) {
        watch_set(c, 0, (uint32_t)e->ExceptionRecord->ExceptionInformation[0]);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    int slot = (c->Dr6 & 1) ? 0 : (c->Dr6 & 2) ? 1 : (c->Dr6 & 4) ? 2 : 3;
    uint32_t a = watch_addr[slot], v = MEM32(a);
    {   /* only changes: a slot rewritten with the same value every frame would use up the log */
        static uint32_t last_v[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }, last_a[4];
        if (last_a[slot] == a && last_v[slot] == v) { c->Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION; }
        last_a[slot] = a; last_v[slot] = v;
    }
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    fprintf(stderr, "[BOT-WATCH] slot=%d addr=%08X val=%08X rip=+%llX bt:", slot, a, v,
            (unsigned long long)(c->Rip - base));
    void *bt[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, bt, NULL);
    for (USHORT i = 0; i < n; i++) {
        uintptr_t p = (uintptr_t)bt[i];
        if (p > base && p < base + 0x10000000u) fprintf(stderr, " +%llX", (unsigned long long)(p - base));
    }
    fprintf(stderr, " guest:");
    for (uint32_t i = 0, shown = 0, sp = g_esp; i < 128 && shown < 10; i++) {
        uint32_t w = gva_ok(sp + i * 4, 4) ? MEM32(sp + i * 4) : 0;
        if (w >= 0x11000u && w < 0x330000u) { fprintf(stderr, " %08X", w); shown++; }
    }
    fprintf(stderr, "\n");
    c->Dr6 = 0;
    if (++watch_hits > 300) c->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void watch_arm(uint32_t gva);
void ma2_bot_watch(uint32_t gva) { watch_arm(gva); }   /* diag hook for other units */
static void watch_arm(uint32_t gva)
{
    static int installed;
    if (!installed) { AddVectoredExceptionHandler(1, watch_veh); installed = 1; }
    ULONG_PTR arg = gva;
    RaiseException(WATCH_ARM_CODE, 0, 1, &arg);
}

/* Diagnostic (RECOMP_BOT_STALL): when the game stops for 3 s, sample the
 * game thread's native RIP and the image addresses on its stack (exe RVAs,
 * resolve with the .map). "Stops" = no bot tick and no menu/HUD pulse
 * (ma2_pc_menu_pulse, every frame in play and in menus): the Game Over
 * screen and the menus after it have no bot ticks, and the game clock and
 * step stand still there too (F51: it fired at every match end), so once the
 * bots are gone only a pulse missing for RECOMP_BOT_STALL_MENU_MS (15 s; a
 * load may hold it) counts. Each freeze gets 3 samples; the budget re-arms
 * when the game moves again, up to 6 freezes a process. */
static HANDLE g_game_thread;
int xbox_cs_waiters_dump(char *out, int cap);   /* kernel_rtl.c */
static volatile DWORD g_beat;
/* The game thread's guest registers (thread-local, so their addresses are taken on that thread):
 * a sample of a frozen game thread prints the guest registers and the guest return addresses on
 * the guest stack -- the native rip alone only names the innermost lifted function. */
static volatile uint32_t *g_stall_regs[8];   /* eax ecx edx ebx esp ebp esi edi */
static int stall_guest(char *out, int cap)
{
    int o = 0;
    if (!g_stall_regs[4] || cap < 512) return 0;
    uint32_t esp = *g_stall_regs[4];
    o += snprintf(out + o, (size_t)(cap - o), "[BOT-STALL]   guest eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X ret:",
                  *g_stall_regs[0], *g_stall_regs[1], *g_stall_regs[2], *g_stall_regs[3], esp, *g_stall_regs[5], *g_stall_regs[6], *g_stall_regs[7]);
    for (uint32_t k = 0, shown = 0; esp && k < 1024 && shown < 24 && o < cap - 32; k++) {
        uint32_t a = esp + k * 4;
        if (a < 0x10000u || a >= 0x4000000u - 4) break;
        uint32_t v = MEM32(a);
        if (v >= g_xbox_code_lo && v < g_xbox_code_hi) { o += snprintf(out + o, (size_t)(cap - o), " [+%X]%06X", k * 4, v); shown++; }
    }
    o += snprintf(out + o, (size_t)(cap - o), "\n");
    return o;
}
DWORD ma2_pc_menu_pulse_ms(void);
static void stall_say(const char *fmt, ...)   /* no CRT stream lock: a frozen game may hold it */
{
    char line[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    /* the CRT's stderr handle: in a windowed exe freopen(stderr) does not change GetStdHandle (W-07) */
    DWORD wr; if (n > 0) WriteFile((HANDLE)_get_osfhandle(_fileno(stderr)), line, (DWORD)(n < (int)sizeof line ? n : (int)sizeof line - 1), &wr, NULL);
}
static DWORD WINAPI stall_monitor(LPVOID unused)
{
    /* Every thread of the process is sampled -- the game thread and whoever it waits for (the
     * audio worker, a DPC) -- with nothing that can take a lock while threads are suspended:
     * the text goes into a buffer and out with WriteFile once all are running again (fprintf
     * with the game thread suspended inside the CRT deadlocked this monitor itself). */
    (void)unused;
    int samples = 0, freezes = 0;
    DWORD diag_t = 0; int diag_n = 0;
    DWORD fired = 0, menu_ms = (DWORD)clampi(env_int("RECOMP_BOT_STALL_MENU_MS", 15000), 3000, 600000);
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    static char out[65536];
    for (;;) {
        Sleep(700);
        DWORD beat = g_beat, now = GetTickCount();
        if (!beat) continue;
        DWORD gmove = ma2_pc_menu_pulse_ms();                        /* the game drew a menu or the HUD */
        if (diag_n < 40 && now - beat >= 3000 && now - diag_t >= 5000) {   /* what moves once the bots stop */
            diag_n++; diag_t = now;
            stall_say("[BOT-STALL] idle bots %lu ms, menu pulse %lu ms ago\n", (unsigned long)(now - beat),
                    gmove ? (unsigned long)(now - gmove) : 0ul);
        }
        DWORD last = (LONG)(gmove - beat) > 0 ? gmove : beat;      /* last sign of life */
        if (fired && (LONG)(last - fired) > 0) {                   /* it moved again: re-arm */
            stall_say("[BOT-STALL] resumed after %lu ms\n", (unsigned long)(last - fired));
            samples = 0; fired = 0;
        }
        DWORD limit = (LONG)(gmove - beat) > 1000 ? menu_ms : 3000; /* bots gone, menus still drawn: Game Over/load */
        if (now - last < limit || samples >= 3 || (!samples && freezes >= 6)) continue;
        int o = 0;
        if (!samples) {
            fired = now; freezes++;
            o += snprintf(out, sizeof out, "[BOT-STALL] freeze %d: no bot tick for %lu ms, no menu pulse for %lu ms (limit %lu)\n", freezes,
                          (unsigned long)(now - beat), (unsigned long)(now - gmove), (unsigned long)limit);
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te; te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok && o < (int)sizeof out - 1024; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &c)) {
                    o += snprintf(out + o, sizeof out - (size_t)o, "[BOT-STALL] %d tid=%lu%s rip=+%llX stack:", samples, (unsigned long)te.th32ThreadID,
                                  th && te.th32ThreadID == GetThreadId(g_game_thread) ? " (game)" : "", (unsigned long long)(c.Rip - base));
                    const uintptr_t *sp = (const uintptr_t *)c.Rsp;
                    MEMORY_BASIC_INFORMATION mbi;
                    size_t avail = VirtualQuery(sp, &mbi, sizeof mbi) ? (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)sp) / sizeof(uintptr_t) : 0;
                    for (size_t k = 0, shown = 0; k < avail && k < 4096 && shown < 32 && o < (int)sizeof out - 64; k++) {
                        uintptr_t v = sp[k];
                        if (v > base + 0x1000 && v < base + 0x8000000u) { o += snprintf(out + o, sizeof out - (size_t)o, " +%llX", (unsigned long long)(v - base)); shown++; }
                    }
                    o += snprintf(out + o, sizeof out - (size_t)o, "\n");
                    if (g_game_thread && te.th32ThreadID == GetThreadId(g_game_thread)) o += stall_guest(out + o, (int)sizeof out - o);
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        o += xbox_cs_waiters_dump(out + o, (int)sizeof out - o);   /* who waits on which guest lock, and who holds it */
        DWORD wr; WriteFile(GetStdHandle(STD_ERROR_HANDLE), out, (DWORD)o, &wr, NULL);
        samples++;
        Sleep(2000);
    }
    return 0;
}
/* RECOMP_BOT_STALL also samples every other thread when one faults (access violation, integer
 * divide by zero) -- the audio worker's crashes need to know what the game thread was doing. */
static LONG CALLBACK crash_sampler(PEXCEPTION_POINTERS ep)
{
    static volatile LONG once;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    /* the runtime handles its own write-watch faults on the game thread: only another thread's fault counts */
    if (code == EXCEPTION_ACCESS_VIOLATION && g_game_thread && GetCurrentThreadId() == GetThreadId(g_game_thread)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&once) > 3) return EXCEPTION_CONTINUE_SEARCH;
    static char out[32768];
    int o = 0;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    o += snprintf(out + o, sizeof out - (size_t)o, "[BOT-FAULT] code=%08lX tid=%lu rip=+%llX\n", (unsigned long)code, (unsigned long)self,
                  (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - base));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok && o < (int)sizeof out - 1024; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &c)) {
                    o += snprintf(out + o, sizeof out - (size_t)o, "[BOT-FAULT] other tid=%lu rip=+%llX stack:", (unsigned long)te.th32ThreadID, (unsigned long long)(c.Rip - base));
                    const uintptr_t *sp = (const uintptr_t *)c.Rsp;
                    MEMORY_BASIC_INFORMATION mbi;
                    size_t avail = VirtualQuery(sp, &mbi, sizeof mbi) ? (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)sp) / sizeof(uintptr_t) : 0;
                    for (size_t k = 0, shown = 0; k < avail && k < 4096 && shown < 24 && o < (int)sizeof out - 64; k++) {
                        uintptr_t v = sp[k];
                        if (v > base + 0x1000 && v < base + 0x8000000u) { o += snprintf(out + o, sizeof out - (size_t)o, " +%llX", (unsigned long long)(v - base)); shown++; }
                    }
                    o += snprintf(out + o, sizeof out - (size_t)o, "\n");
                    if (g_game_thread && te.th32ThreadID == GetThreadId(g_game_thread)) o += stall_guest(out + o, (int)sizeof out - o);
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
    }
    DWORD wr; WriteFile(GetStdHandle(STD_ERROR_HANDLE), out, (DWORD)o, &wr, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* Diagnostic (RECOMP_PROFILE="start:seconds"): a sampling profiler. From `start` seconds after the
 * first bot tick, for `seconds`, every thread of the process is suspended about once a millisecond
 * and its RIP counted; then every RIP seen 3+ times is printed per thread ([PROF] lines: exe RVAs
 * as +RVA, other modules as name+offset) with each thread's CPU time over the window. Resolve the
 * RVAs with the .map and add them up per function. */
typedef struct { uintptr_t rip; uint32_t n; } ProfSlot;
#define PROF_SLOTS 65536u
static DWORD WINAPI profiler(LPVOID unused)
{
    (void)unused;
    double t0 = 30.0, dur = 20.0;
    sscanf(getenv("RECOMP_PROFILE"), "%lf:%lf", &t0, &dur);
    Sleep((DWORD)(t0 * 1000.0));
    struct { DWORD tid; HANDLE h; uint32_t total; ProfSlot *tab; FILETIME k0, u0; } th[96];
    int nth = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && nth < 96; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        th[nth].tid = te.th32ThreadID; th[nth].h = h; th[nth].total = 0;
        th[nth].tab = (ProfSlot *)calloc(PROF_SLOTS, sizeof(ProfSlot));
        FILETIME c, x; GetThreadTimes(h, &c, &x, &th[nth].k0, &th[nth].u0);
        if (th[nth].tab) nth++; else CloseHandle(h);
    }
    CloseHandle(snap);
    uintptr_t img_lo = (uintptr_t)GetModuleHandleA(NULL), img_hi = img_lo;
    { const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)img_lo; const IMAGE_NT_HEADERS *nh = (const IMAGE_NT_HEADERS *)(img_lo + (uintptr_t)dh->e_lfanew); img_hi = img_lo + nh->OptionalHeader.SizeOfImage; }
    timeBeginPeriod(1);
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)(dur * 1000.0)) {
        for (int i = 0; i < nth; i++) {
            if (SuspendThread(th[i].h) == (DWORD)-1) continue;
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th[i].h, &c)) {
                uintptr_t r = (uintptr_t)c.Rip;
                if (r < img_lo || r >= img_hi) {   /* in a system DLL: charge the exe function that called in */
                    const uintptr_t *sp = (const uintptr_t *)c.Rsp;
                    MEMORY_BASIC_INFORMATION mb; uintptr_t lim = 0;
                    if (VirtualQuery(sp, &mb, sizeof mb) && mb.State == MEM_COMMIT) lim = (uintptr_t)mb.BaseAddress + mb.RegionSize;
                    for (int q = 0; q < 512 && (uintptr_t)(sp + q + 1) <= lim; q++) {   /* never past the committed stack */
                        uintptr_t v = sp[q];
                        if (v > img_lo + 0x1000 && v < img_hi) { r = v | ((uintptr_t)1 << 63); break; }
                    }
                }
                uint32_t hsh = (uint32_t)((r >> 2) * 2654435761u) & (PROF_SLOTS - 1u);
                for (uint32_t k = 0; k < 64; k++, hsh = (hsh + 1u) & (PROF_SLOTS - 1u)) {
                    if (th[i].tab[hsh].rip == r) { th[i].tab[hsh].n++; break; }
                    if (!th[i].tab[hsh].rip) { th[i].tab[hsh].rip = r; th[i].tab[hsh].n = 1; break; }
                }
                th[i].total++;
            }
            ResumeThread(th[i].h);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    double wall = (GetTickCount() - start) / 1000.0;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < nth; i++) {
        FILETIME c, x, k1, u1; GetThreadTimes(th[i].h, &c, &x, &k1, &u1);
        ULARGE_INTEGER a, b2, ka, kb;
        a.LowPart = th[i].u0.dwLowDateTime; a.HighPart = th[i].u0.dwHighDateTime; b2.LowPart = u1.dwLowDateTime; b2.HighPart = u1.dwHighDateTime;
        ka.LowPart = th[i].k0.dwLowDateTime; ka.HighPart = th[i].k0.dwHighDateTime; kb.LowPart = k1.dwLowDateTime; kb.HighPart = k1.dwHighDateTime;
        double cpu = ((double)(b2.QuadPart - a.QuadPart) + (double)(kb.QuadPart - ka.QuadPart)) / 1e4;   /* ms */
        fprintf(stderr, "[PROF] thread tid=%lu%s samples=%u cpu=%.0fms of %.0fms (%.0f%%)\n", (unsigned long)th[i].tid,
                g_game_thread && th[i].tid == GetThreadId(g_game_thread) ? " (game)" : "", th[i].total, cpu, wall * 1000.0, cpu / (wall * 10.0));
        if (cpu / (wall * 10.0) < 3.0) { free(th[i].tab); CloseHandle(th[i].h); continue; }
        for (uint32_t k = 0; k < PROF_SLOTS; k++) {
            ProfSlot *p = &th[i].tab[k];
            if (!p->rip || p->n < 3) continue;
            if (p->rip >> 63) { fprintf(stderr, "[PROF] %lu ext+%llX %u\n", (unsigned long)th[i].tid, (unsigned long long)((p->rip & ~((uintptr_t)1 << 63)) - base), p->n); continue; }
            HMODULE m = NULL; char name[64] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p->rip, &m) && m) {
                char full[MAX_PATH]; if (GetModuleFileNameA(m, full, sizeof full)) { const char *bs = strrchr(full, '\\'); snprintf(name, sizeof name, "%s", bs ? bs + 1 : full); }
            }
            if ((uintptr_t)m == base) fprintf(stderr, "[PROF] %lu +%llX %u\n", (unsigned long)th[i].tid, (unsigned long long)(p->rip - base), p->n);
            else fprintf(stderr, "[PROF] %lu %s+%llX %u\n", (unsigned long)th[i].tid, name, (unsigned long long)(p->rip - (uintptr_t)m), p->n);
        }
        free(th[i].tab); CloseHandle(th[i].h);
    }
    fprintf(stderr, "[PROF] done\n");
    return 0;
}
static void stall_heartbeat(void)
{
    static int started;
    if (!started) {
        started = 1;
        if (getenv("RECOMP_PROFILE")) {
            if (!g_game_thread) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread,
                                                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
            CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
        }
        if (!env_on("RECOMP_BOT_STALL")) return;
        g_stall_regs[0] = &g_eax; g_stall_regs[1] = &g_ecx; g_stall_regs[2] = &g_edx; g_stall_regs[3] = &g_ebx;
        g_stall_regs[4] = &g_esp; g_stall_regs[5] = &g_ebp; g_stall_regs[6] = &g_esi; g_stall_regs[7] = &g_edi;
        if (!g_game_thread) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
        CloseHandle(CreateThread(NULL, 0, stall_monitor, NULL, 0, NULL));
        AddVectoredExceptionHandler(0, crash_sampler);   /* last: the runtime's own trap pages (AC97, write watch) are handled first */
    }
    g_beat = GetTickCount();
}

/* Diagnostic: guest return addresses on the stack at a patched site. */
void ma2_bot_stack(int tag, uint32_t obj)
{
    static int n;
    for (uint32_t i = 0, sp = g_esp; i < 8; i++)
        if (gva_ok(sp + i * 4, 4) && MEM32(sp + i * 4) == 0x000B7635u) return;   /* level loader */
    if (++n > 40) return;
    fprintf(stderr, "[BOT-STACK] tag=%d obj=%08X:", tag, obj);
    for (uint32_t i = 0, shown = 0, sp = g_esp; i < 160 && shown < 14; i++) {
        uint32_t w = gva_ok(sp + i * 4, 4) ? MEM32(sp + i * 4) : 0;
        if (w >= 0x11000u && w < 0x330000u) { fprintf(stderr, " %08X", w); shown++; }
    }
    fprintf(stderr, "\n");
}

/* Diagnostic: first hit of each (site, object) pair. */
void ma2_bot_trace(int id, uint32_t a, uint32_t b)
{
    if (!c7_log_on()) return;   /* W-09b */
    static uint32_t seen[128][2];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i][0] == (uint32_t)id && seen[i][1] == a) return;
    if (n < 128) { seen[n][0] = (uint32_t)id; seen[n][1] = a; n++; }
    fprintf(stderr, "[BOT-TRACE] site=%d a=%08X b=%08X avt=%08X a4=%08X a8=%08X b4=%08X b28=%08X b2C=%08X\n", id, a, b,
            gva_ok(a, 0x10) ? MEM32(a) : 0, gva_ok(a, 0x10) ? MEM32(a + 4) : 0, gva_ok(a, 0x10) ? MEM32(a + 8) : 0,
            gva_ok(b, 0x30) ? MEM32(b + 4) : 0, gva_ok(b, 0x30) ? MEM32(b + 0x28) : 0, gva_ok(b, 0x30) ? MEM32(b + 0x2C) : 0);
}

static void watch_arm(uint32_t gva);
/* RECOMP_BOT_DIAG: activation state of every player, once per second. */
static void player_diag(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    for (int i = 0; i < 24 && gate_obj[i]; i++) {
        uint32_t o = gate_obj[i], c8 = MEM32(o + 8), cC = MEM32(o + 0xC);
        fprintf(stderr, "[BOT-GATE] obj=%08X vt=%08X f150=%08X stage=%d e4=%u cam=%08X cam78=%08X owner=%08X own8C=%08X\n",
                o, MEM32(o), gva_ok(MEM32(o), 0x154) ? MEM32(MEM32(o) + 0x150) : 0, gate_stage[i], MEM8(o + 0x6E4), c8, gva_ok(c8, 0x80) ? MEM32(c8 + 0x78) : 0,
                cC, gva_ok(cC, 0x90) ? MEM32(cC + 0x8C) : 0);
    }
    uint32_t b = MEM32(0x50E280), e = MEM32(0x50E284);
    if (e < b || e - b > 12 * 4) return;
    uint32_t transition = MEM32(0x4EBA8C), input_cls = MEM32(0x4EAFA4);
    for (uint32_t it = b; it < e; it += 4) {
        uint32_t p = MEM32(it);
        if (!gva_ok(p, 0x4B0)) continue;
        uint32_t a = player_actor(p), th = player_child(p, transition), in = player_child(p, input_cls);
        {
            static int armed;
            if (!armed && env_on("RECOMP_BOT_WATCH") && (MEM32(p + 0x244) & 4) && !ma2_bot_is_player(p)) {
                armed = 1;
                watch_arm(p + 0x45C);             /* human's enter candidate: vehicle */
            }
        }
        fprintf(stderr, "[BOT-PLAYER] p=%08X idx=%u port=%d flags=%08X ent1C=%08X beh=%08X cam=%08X actor=%08X actvt=%08X act1C=%08X trans=%08X/%u input=%08X slot=%d in78=%d in7C=%d a184=%d bC=%08X b8=%08X\n",
                p, MEM32(p + 0x88), (int)MEM32(p + 0x2AC), MEM32(p + 0x244), MEM32(p + 0x1C),
                MEM32(p + 0x2F0), MEM32(p + 0x2F4), a, a ? MEM32(a) : 0, a ? MEM32(a + 0x1C) : 0,
                th, th ? MEM32(th + 0x84) : 0, in, in ? (int)MEM32(in + 0x80) : -1,
                in ? (int)MEM32(in + 0x78) : -1, in ? (int)MEM8(in + 0x7C) : -1,
                a ? (int)MEM32(a + 0x184) : -99, gva_ok(MEM32(p + 0x2F0), 0x10) ? MEM32(MEM32(p + 0x2F0) + 0xC) : 0,
                gva_ok(MEM32(p + 0x2F0), 0x10) ? MEM32(MEM32(p + 0x2F0) + 8) : 0);
        if (a) fprintf(stderr, "[BOT-ACTOR] idx=%u actor=%08X kind=%d f268=%08X f26C=%08X p424=%08X p428=%08X p42C=%08X p438=%08X p43C=%08X c448=%08X c45C=%08X pos=%.0f,%.0f\n",
                       MEM32(p + 0x88), a, actor_kind(a), MEM32(a + 0x268), MEM32(a + 0x26C),
                       MEM32(p + 0x424), MEM32(p + 0x428), MEM32(p + 0x42C), MEM32(p + 0x438), MEM32(p + 0x43C), MEM32(p + 0x448), MEM32(p + 0x45C),
                       MEMF(a + 0x4C0), MEMF(a + 0x4C8));
    }
    const Ma2World *w = &g_rt.world;
    {
        static int dumped, vlist;
        uint32_t pilot = 0, mech = 0;
        if (vlist < 2 && w->nvehicles > 0) {       /* where are the vehicles (human test setups)? */
            vlist++;
            for (int i = 0; i < w->nvehicles; i++)
                fprintf(stderr, "[BOT-VLIST] %08X kind=%d pos=%.0f,%.0f,%.0f nation=%d occ=%d entry=%d\n", w->vehicles[i].actor, w->vehicles[i].kind,
                        w->vehicles[i].pos.x, w->vehicles[i].pos.y, w->vehicles[i].pos.z, w->vehicles[i].nation, w->vehicles[i].occupied, w->vehicles[i].has_entry);
        }
        for (int i = 0; i < w->nvehicles; i++) {
            if (w->vehicles[i].occupied && !pilot) pilot = w->vehicles[i].actor;
            if (!w->vehicles[i].occupied && !mech && w->vehicles[i].pos.x != 0.f && w->vehicles[i].kind == MA2_KIND_MECH)
                mech = w->vehicles[i].actor;
        }
        if (!dumped && pilot && mech) {
            dumped = 1;
            fprintf(stderr, "[BOT-DIFF] pilot=%08X mech=%08X\n", pilot, mech);
            for (uint32_t o = 0; o < 0x900; o += 4) {
                uint32_t pp = MEM32(pilot + o), mp = MEM32(mech + o);
                if (gva_ok(pp, 0x260) && gva_ok(mp, 0x260) && MEM32(pp) == MEM32(mp) && MEM32(pp) >= 0x440000u && MEM32(pp) < 0x480000u)
                    fprintf(stderr, "[BOT-DESC] +%03X vt=%08X p248=%08X p24C=%08X m248=%08X m24C=%08X\n", o, MEM32(pp),
                            MEM32(pp + 0x248), MEM32(pp + 0x24C), MEM32(mp + 0x248), MEM32(mp + 0x24C));
            }
        }
    }
    {
        static int zdump;
        if (zdump++ < 3)
            for (int i = 0; i < w->nvehicles; i++) {
                uint32_t v = w->vehicles[i].actor;
                if (w->vehicles[i].occupied) continue;
                for (uint32_t c = MEM32(v + 0xC), n = 0; n < 32 && gva_ok(c, 0x100); n++, c = MEM32(c + 8)) {
                    uint32_t node = MEM32(c + 0x94);
                    Ma2Vec3 p = gva_ok(node, 0x100) ? read_vec3(node + 0xF0) : (Ma2Vec3){0, 0, 0};
                    fprintf(stderr, "[BOT-ZONE] veh=%08X kind=%d vpos=%.1f,%.1f,%.1f child=%08X vt=%08X cls=%08X zcls=%08X pos=%.1f,%.1f,%.1f\n",
                            v, w->vehicles[i].kind, w->vehicles[i].pos.x, w->vehicles[i].pos.y, w->vehicles[i].pos.z,
                            c, MEM32(c), MEM32(c + 0x28), MEM32(0x4EBAE8), p.x, p.y, p.z);
                }
            }
    }
    for (int i = 0; i < w->nvehicles; i++)
        fprintf(stderr, "[BOT-VEH] %d actor=%08X kind=%d occ=%d nation=%d entry=%d r=%.1f f84=%08X hp=%.0f pos=%.0f,%.0f,%.0f\n", i,
                w->vehicles[i].actor, w->vehicles[i].kind, w->vehicles[i].occupied, w->vehicles[i].nation,
                w->vehicles[i].has_entry, w->vehicles[i].entry_r, MEM32(w->vehicles[i].actor + 0x84), w->vehicles[i].hp,
                w->vehicles[i].pos.x, w->vehicles[i].pos.y, w->vehicles[i].pos.z);
}

/* Fire-group reach of a body: weapons are the arsenal's (actor+0xA4) child entities of class
 * [0x4EAF4C]; a weapon's range is its definition's +0x128 (MEM32(weapon+0xC)). */
static uint32_t weapon_of(uint32_t a, int group);   /* ma2_bot_ctrl.c */
static void arsenal_reach(uint32_t a, float *rmax, float *rmin)
{
    uint32_t ars = MEM32(a + 0xA4), wcls = MEM32(0x004EAF4Cu);
    float hi = 0.f, lo = 1e9f;
    if (gva_ok(ars, 0x90))
        for (uint32_t wp = MEM32(ars + 0xC), n = 0; gva_ok(wp, 0xA0) && n < 64; wp = MEM32(wp + 8), n++) {
            if (MEM32(wp + 0x28) != wcls) continue;
            uint32_t def = MEM32(wp + 0xC);
            float r = gva_ok(def, 0x130) ? MEMF(def + 0x128) : 0.f;
            if (!(r > 10.f && r < 2000.f)) continue;
            if (r > hi) hi = r;
            if (r < lo) lo = r;
        }
    *rmax = hi; *rmin = hi > 0.f ? lo : 0.f;
}

#include "ma2_bot_defense.c"   /* RS-click defence state, weapon damage classes, RECOMP_DEFENSE_PROBE */

static int player_stat(uint32_t pl, int id);   /* ma2_bot_objective.c */
static void build_player(Ma2PlayerView *v, uint32_t p, int idx, double now)
{
    memset(v, 0, sizeof(*v));
    v->player = p;
    v->is_human = (MEM32(p + 0x244) & 4) != 0;
    v->is_bot = ma2_bot_is_player(p);
    /* The scoreboard everyone can see: kills (stat 2 -- 2, 3 and 4 rise on each kill) and deaths (5). */
    v->kills = player_stat(p, 2); v->deaths = player_stat(p, 5);
    if (v->is_human && getenv("RECOMP_TEST_HUMAN_KILLS")) v->kills += atoi(getenv("RECOMP_TEST_HUMAN_KILLS"));   /* test hook: a runaway leader */
    v->enter_target = MEM32(p + 0x438);
    /* Controlled actor as the game itself finds it (sub_C8E6A): the first
     * child of the player entity that is a MACorporeal. The control
     * behaviour (+0x2F0) only exists once the player has been activated. */
    uint32_t a = player_actor(p);
    if (!a) {
        uint32_t beh = MEM32(p + 0x2F0);
        a = gva_ok(beh, 0x10) ? MEM32(beh + 0xC) : 0;
    }
    if (!gva_ok(a, 0x4D0) || !gva_ok(MEM32(a), 4)) a = 0;
    v->actor = a;
    v->nation = actor_int(a ? a : p, 0xA8, 0x000EB091u, -1);
    if (!a) { g_rt.prev_actor[idx] = 0; return; }
    v->kind = actor_kind(a);
    v->pos = actor_pos(a);
    int hp = actor_int(a, 0x74, 0x000C9476u, 0), hpmax = actor_int(a, 0x6C, 0x000C8C2Au, 0);
    if (v->kind == MA2_KIND_MECH || v->kind == MA2_KIND_BATTLEARMOR) {
        /* MAMech health (sub_C8BDC): ints at [actor+0x260]+8 (current) / +4 (max);
         * vfunc +0x74 only reports a class constant for 'Mechs. */
        uint32_t hpb = MEM32(a + 0x260);
        if (gva_ok(hpb, 0x10) && (int)MEM32(hpb + 4) > 0) { hp = (int)MEM32(hpb + 8); hpmax = (int)MEM32(hpb + 4); }
    }
    v->hp = (float)hp; v->hp_max = (float)(hpmax > 0 ? hpmax : 0);
    v->alive = hp > 0;
    if (g_rt.prev_actor[idx] == a && now > g_rt.prev_time[idx]) {
        float dt = (float)(now - g_rt.prev_time[idx]);
        if (dt > 1e-4f && dt < 1.f) {
            v->vel.x = (v->pos.x - g_rt.prev_pos[idx].x) / dt;
            v->vel.y = (v->pos.y - g_rt.prev_pos[idx].y) / dt;
            v->vel.z = (v->pos.z - g_rt.prev_pos[idx].z) / dt;
        }
    }
    float yaw = g_rt.prev_actor[idx] == a ? g_rt.prev_yaw[idx] : 0.f;
    if (!actor_yaw(a, v->pos, &yaw) && v->vel.x * v->vel.x + v->vel.z * v->vel.z > 1.f)
        yaw = atan2f(v->vel.x, v->vel.z);
    v->yaw = yaw;
    g_rt.prev_actor[idx] = a; g_rt.prev_pos[idx] = v->pos; g_rt.prev_time[idx] = now; g_rt.prev_yaw[idx] = yaw;

    uint32_t world = MEM32(a + 0x60);
    /* Riders (12-byte entries at +0x1A8..+0x1AC, uid at +8; sub_EB12B). */
    uint32_t rb = MEM32(a + 0x1A8), re = MEM32(a + 0x1AC);
    if (re > rb && re - rb <= 12 * 16 && gva_ok(rb, re - rb))
        for (uint32_t e = rb; e + 12 <= re; e += 12) {
            uint32_t r = uid_lookup(world, MEM32(e + 8));
            if (r && r != a && gva_ok(r, 0x4D0)) {
                int rn = actor_int(r, 0xA8, 0x000EB091u, -2);
                if (rn != v->nation) { v->enemy_rider = r; break; }
            }
        }
    /* Clamp target: IsAttached +0x1A0 / RideMoverUID +0x194 (guide), else the
     * ride uid sub_EB475 reads at +0x1B4. */
    if (v->kind == MA2_KIND_BATTLEARMOR) {
        uint32_t t = 0;
        if (MEM8(a + 0x1A0)) t = uid_lookup(world, MEM32(a + 0x194));
        if (!t) { uint32_t u = MEM32(a + 0x1B4); t = u ? uid_lookup(world, u) : 0; }
        /* Only an enemy counts: standing on a parked friendly 'Mech also sets the ride uid. */
        if (t && t != a && is_kind(t, g_cls.mover) && actor_int(t, 0xA8, 0x000EB091u, -2) != v->nation) v->clamped_to = t;
        /* A friendly VTOL: a lift. */
        else if (t && t != a && is_kind(t, g_cls.flyer) && ((MEM32(t + 0x260) >> 2) & 1)) v->riding = t;
    }
    /* VTOL tether: what it carries (sub_115C13 hooks while +0x57C < +0x578). */
    if (v->kind == MA2_KIND_VTOL && is_kind(a, g_cls.flyer)) {
        int n = (int)MEM32(a + 0x57C), cap = (int)MEM32(a + 0x578);
        v->cargo = n > 0 && n <= cap && cap <= 16 ? n : 0;
    }
    /* Own-actor NeuroHack QTE (actor+0x274), guide 2.5. */
    if (v->kind == MA2_KIND_MECH || v->kind == MA2_KIND_BATTLEARMOR) {
        uint32_t q = MEM32(a + 0x274);
        int bit39 = (MEM32(a + 0x26C) >> (0x39 - 32)) & 1;
        if (gva_ok(q, 0xC8)) {
            /* The QTE object exists only while a hack runs (sub_101E9D requires
             * +0x274 == 0 to start one); bits 0x39 (minigame) / 0x3C (clamped). */
            int bit3c = (MEM32(a + 0x26C) >> (0x3C - 32)) & 1;
            v->qte_active = 1; (void)bit39; (void)bit3c;   /* inside gva_ok(q): object present */
            v->qte_required = MEM8(q) & 0x7F;
            v->qte_pressed = MEM8(q + 0x20) & 0x7F;
            v->qte_window = MEMF(q + 4);
            float target = MEMF(q + 0xA8);
            v->qte_progress = target > 0.f ? clampf(MEMF(q + 0x18) / target, 0.f, 1.f) : 0.f;
        }
        v->hack_eject = hack_ej_recent(p);   /* HackEject: a NeuroHack on this bot completed < 8 s ago (see ma2_bot_hack_eject) */
        v->qte_opp_progress = -1.f;
        if (v->qte_active) {
            /* The opposing side's bar (BattleArmor <-> 'Mech): first to fill wins the race. */
            uint32_t oa = v->kind == MA2_KIND_BATTLEARMOR ? v->clamped_to : v->enemy_rider;
            uint32_t oq = oa && gva_ok(oa, 0x280) ? MEM32(oa + 0x274) : 0;
            if (gva_ok(oq, 0xC8) && MEMF(oq + 0xA8) > 0.f) v->qte_opp_progress = clampf(MEMF(oq + 0x18) / MEMF(oq + 0xA8), 0.f, 1.f);
        }
    }
    v->defense_active = def_is_active(a);   /* sub_E694C: actor+0x220 defence, flag 2 (DEFENSE_SPEC.md) */
    {   /* What its running defence stops -- the shield bubble, absorber glow and AMS are visible to all. */
        DefView dv;
        v->defense_blocks = 0; v->defense_type = MA2_DEFT_NONE; v->defense_ready = 0;
        if (def_read(a, now, &dv)) {
            static const int map[] = { MA2_DEFT_NONE, MA2_DEFT_NONE, MA2_DEFT_SHIELD, MA2_DEFT_CAMO, MA2_DEFT_JAM, MA2_DEFT_ABSORB, MA2_DEFT_AMS };
            v->defense_type = dv.cls >= 0 && dv.cls <= DEFC_AMS ? map[dv.cls] : MA2_DEFT_NONE;
            v->defense_ready = v->defense_type != MA2_DEFT_NONE && !dv.active && !dv.recharging;
        }
        if (v->defense_type && dv.active) {
            if (dv.cls == DEFC_SHIELD) v->defense_blocks = MA2_DEF_DIRECT | MA2_DEF_SPLASH;
            else if (dv.cls == DEFC_ABSORB && dv.abs_on) v->defense_blocks = (dv.abs_dir ? MA2_DEF_DIRECT : 0) | (dv.abs_spl ? MA2_DEF_SPLASH : 0);
            else if (dv.cls == DEFC_AMS) v->defense_blocks = MA2_DEF_SPLASH;
        }
        v->cloak = 0; v->cloak_shimmer = 0.f;
        if (v->defense_type == MA2_DEFT_CAMO && dv.active && c7_on("cloak", "RECOMP_BOT_CLOAK", "BotCloak", 1)) {
            DefKind dk;   /* F5: the game's own cloak rating reaches the brain */
            def_kind(a, now, &dk);
            v->cloak = dk.cloak_strict ? 2 : dk.cloaked ? 1 : 0;
            v->cloak_shimmer = dk.shimmer;
        }
    }
    arsenal_reach(a, &v->reach, &v->reach_short);
    v->slow_reload = 0.f;
    for (int g = 0; g < 3; g++) {           /* F79: the slow group's refire (the fire groups in use now) */
        uint32_t wg = weapon_of(a, g); WpnProps wp;
        if (wg && wpn_props(wg, &wp) && wp.fire_ms >= 900.f && wp.fire_ms <= 10000.f && wp.fire_ms / 1000.f > v->slow_reload)
            v->slow_reload = wp.fire_ms / 1000.f;
    }
    v->alpha_ready = 0; v->alpha_charge = 0; v->heat = -1.f; v->wpn_room = 0; v->wpn_lvl = -1.f;
    {   uint32_t ars = gva_ok(a, 0xA8) ? MEM32(a + 0xA4) : 0;
        int got = 0, span = 0, lv[9] = { 0 };
        if (gva_ok(ars, 0xA0))
            for (int c = 0; c < 3; c++) {
                int top = arsenal_max_level(ars, c), lo = arsenal_min_level(ars, c), cur = (int)MEM32(ars + 0x7C + 4u * (uint32_t)c);
                if (top > 0 && cur < top) v->wpn_room |= 1 << c;   /* PuClass: per class */
                if (lo >= 0 && top > lo) { span += top - lo; got += cur < lo ? 0 : cur > top ? top - lo : cur - lo; }
                lv[3 * c] = cur; lv[3 * c + 1] = lo; lv[3 * c + 2] = top;
            }
        if (gva_ok(ars, 0xA0)) v->wpn_lvl = span ? (float)got / (float)span : 0.f;
        if (gva_ok(ars, 0xA0) && idx >= 0 && idx < 16 && env_on("RECOMP_BOT_DIAG")) {   /* F43: levels as read, on change */
            static int last[16];
            int key = got * 64 + span + 1 + 4096 * (v->kind & 15);
            if (last[idx] != key) {
                last[idx] = key;
                fprintf(stderr, "[F43-ARS] t=%.1f player=%08X kind=%d cur/lo/top c0=%d/%d/%d c1=%d/%d/%d c2=%d/%d/%d lvl=%.2f\n", now, p, v->kind,
                        lv[0], lv[1], lv[2], lv[3], lv[4], lv[5], lv[6], lv[7], lv[8], v->wpn_lvl);
            }
        }
    }
    if (v->kind == MA2_KIND_MECH) {
        v->heat = mech_heat_pct(a);
        v->alpha_ready = alpha_ready(a, NULL);
        {   uint32_t ars = gva_ok(a, 0xA8) ? MEM32(a + 0xA4) : 0;   /* F42: how far from Alpha */
            if (gva_ok(ars, 0xA0) && def_group_wpn(a, 7)) {
                v->alpha_cap = 1;
                for (int c = 0; c < 3; c++) {
                    int top = arsenal_max_level(ars, c), cur = (int)MEM32(ars + 0x7C + 4u * (uint32_t)c);
                    if (top > cur) v->alpha_short += top - cur;
                }
            }
        }
        uint32_t g7 = v->alpha_ready || MEM32(a + 0xF8) == 7u ? def_group_wpn(a, 7) : 0, L = g7 && gva_ok(g7, 0x10) ? MEM32(g7 + 0xC) : 0;
        /* launcher +0x78 bit 0x20: charging (sub_D1BFF), +0x88 the charge in percent */
        if (gva_ok(L, 0x1C0) && MEM32(a + 0xF8) == 7u && (MEM32(L + 0x78) & 0x20u)) {
            int r = (int)MEM32(L + 0x88);
            v->alpha_charge = r < 1 ? 1 : r > 100 ? 100 : r;
        }
    }
}

#include "ma2_bot_entry.c"   /* exact entry zones (MACrateGenerator / MATriggerPoint) */
#include "ma2_bot_track.c"   /* observed-motion tracks + intent (C1/C2) */

int ma2_pc_ini_get_int(const char *section, const char *key, int fallback);
int ma2_bot_yield_human_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_BOT_YIELD_HUMAN");
        on = e && *e ? *e != '0' : ma2_pc_ini_get_int("Bots", "YieldToHuman", 1) != 0;   /* batch 1: on */
    }
    return on;
}

float ma2_bot_ground_y(float x, float z, int *ok) { return terrain_height(x, z, ok); }

int ma2_bot_entry_fix_on(void)
{
    return c7_on("entry", "RECOMP_BOT_ENTRY_FIX", "EntryFix", 1);   /* batch 1: on */
}

/* Bots batch 1 switches (api.h): env RECOMP_BOT_<NAME> over ini [Bots] <Key>, default on. */
int ma2_bot_switch_on(int sw)
{
    static const char *const env[MA2_SW_COUNT] = { "RECOMP_BOT_VEH_ASSIGN", "RECOMP_BOT_SWAP_COOL", "RECOMP_BOT_VEH_COMMIT",
                                                   "RECOMP_BOT_GRINDER_SWAP", "RECOMP_BOT_FOOT_RIDE" };
    static const char *const key[MA2_SW_COUNT] = { "VehAssign", "SwapCool", "VehCommit", "GrinderSwap", "FootRide" };
    static const char *const piece[MA2_SW_COUNT] = { "c3-assign", "f1-swapcool", "f24-commit", "f12-grinder", "f14-footride" };
    if (sw < 0 || sw >= MA2_SW_COUNT) return 0;
    return c7_on(piece[sw], env[sw], key[sw], 1);   /* logs "[C7] <piece> switch=on|off" once a match */
}
int g_c7_c3[MA2_C3_COUNT];
void ma2_c3_count(int c)
{
    static const char *const name[MA2_C3_COUNT] = { "assigned", "reassigned", "human_wins", "swaps_claimed", "swap_cooldown_blocks",
                                                    "elite_keeps", "switches", "commit_holds", "grinder_swaps", "foot_getveh" };
    if (c < 0 || c >= MA2_C3_COUNT) return;
    g_c7_c3[c]++;
    c7_add("c3", name[c], 1.0);
}

/* C3 team vehicle assignment: every bot going for a free vehicle (a ride on foot, or a swap) is a
 * contender for it; the one that gets there first -- straight distance over its body's speed, plus
 * 1.5 s if it is not yet closing on it -- holds the claim, and the others (its teammates; FFA:
 * everyone) re-pick before they set off (brain: vehicle_usable). The holder keeps it unless a
 * contender is a second sooner. A human teammate heading for it beats them all (human_mech_intent). */
static void vehicle_assign(Ma2World *w)
{
    static const float spd[9] = { 5.f, 5.f, 22.f, 16.f, 18.f, 30.f, 30.f, 15.f, 5.f };   /* by Ma2ActorKind, m/s */
    static uint32_t pv_actor[64], pv_holder[64]; static int pv_human[64], pv_n;   /* last holder per vehicle */
    for (int i = 0; i < w->nvehicles; i++) {
        Ma2VehicleView *v = &w->vehicles[i];
        int sl = 0, best = -1, bnat = -1, prev_in = 0, human = 0;
        uint32_t prev;
        float bt = 1e9f;
        if (!v->actor || v->occupied) continue;
        while (sl < pv_n && pv_actor[sl] != v->actor) sl++;
        if (sl == pv_n) {   /* a new one: a free row, else the first row not holding anything */
            if (pv_n < 64) pv_n++;
            else for (sl = 0; sl < 63 && pv_holder[sl]; sl++) {}
            pv_actor[sl] = v->actor; pv_holder[sl] = 0; pv_human[sl] = 0;
        }
        prev = pv_holder[sl];
        for (int k = 0; k < g_rt.nbots; k++) {
            const Bot *bk = &g_rt.bots[k];
            uint32_t want = bk->order.want_vehicle ? bk->order.want_vehicle : bk->order.type == MA2_ORD_GET_VEHICLE ? bk->order.vehicle : 0;
            const Ma2PlayerView *pj = NULL;
            if (!bk->player || want != v->actor) continue;
            for (int j = 0; j < w->nplayers; j++) if (w->players[j].player == bk->player) pj = &w->players[j];
            if (!pj || !pj->alive) continue;
            if (v->human_claim && v->human_claim_nation == pj->nation) { human = 1; continue; }
            Ma2Vec3 at = v->has_entry ? v->entry : v->pos;
            float dx = at.x - pj->pos.x, dz = at.z - pj->pos.z, d = sqrtf(dx * dx + dz * dz);
            float t = d / spd[pj->kind >= 0 && pj->kind < 9 ? pj->kind : 0];
            if (d > 1.f && (pj->vel.x * dx + pj->vel.z * dz) / d < 1.f) t += 1.5f;
            if (bk->player == prev) { t -= 1.0f; prev_in = 1; }
            if (t < bt) { bt = t; best = k; bnat = pj->nation; }
        }
        if (human && !pv_human[sl]) ma2_c3_count(MA2_C3_HUMAN_WINS);
        if (best >= 0) {
            const Bot *wb = &g_rt.bots[best];
            v->claimed_by = wb->player; v->claim_nation = bnat;
            if (wb->player != prev) {
                ma2_c3_count(prev_in ? MA2_C3_REASSIGNED : MA2_C3_ASSIGNED);
                if (wb->order.type != MA2_ORD_GET_VEHICLE) ma2_c3_count(MA2_C3_SWAPS_CLAIMED);
                if (env_on("RECOMP_BOT_DIAG"))
                    fprintf(stderr, "[BOT-C3] t=%.2f vehicle %08X -> bot %08X (%.1f s)%s\n", w->time, v->actor, wb->player, bt,
                            prev_in ? " over the holder" : "");
            }
        }
        pv_actor[sl] = v->actor; pv_holder[sl] = best >= 0 ? g_rt.bots[best].player : 0; pv_human[sl] = human;
    }
}

int ma2_bot_avoid_hybrid_on(void)
{
    return c7_on("avoid", "RECOMP_BOT_AVOID_HYBRID", "AvoidHybrid", 1);   /* batch 1: on */
}

/* C4: tracks (C1) of the units the crowd steering predicts from what is seen of them -- humans,
 * and any unit within 60 m of a bot of another side (well inside sight and hearing range, and
 * the steering only looks 45 m out). One shared track per unit: every bot near it sees the same
 * positions. Teammate bots are steered round by their own steering point instead (ctrl). */
static void avoid_track_feed(Ma2World *w)
{
    if (!ma2_bot_avoid_hybrid_on()) return;
    for (int j = 0; j < w->nplayers; j++) {
        const Ma2PlayerView *q = &w->players[j];
        if (!q->alive || !q->actor) continue;
        int seen = q->is_human;
        for (int k = 0; k < w->nplayers && !seen; k++) {
            const Ma2PlayerView *b = &w->players[k];
            if (b->is_bot && b->alive && b->nation != q->nation && hypotf(b->pos.x - q->pos.x, b->pos.z - q->pos.z) < 60.f) seen = 1;
        }
        if (seen) ma2_track_observe(MA2_TRACK_AVOID, q->actor, q->pos, w->time);
    }
}

/* Human teammates heading for a free 'Mech (C1 tracks fed from what their team sees of them --
 * a teammate is always seen -- and C2 intent): the 'Mech is marked for that human, and teammate
 * bots leave it (brain: vehicle_usable). Any distance; the human only has to be going for it. */
int g_c7_yield_intents, g_c7_yields;   /* [C7] yield counters (brain adds the yields) */
static void human_mech_intent(Ma2World *w)
{
    {   /* per-match counter line every 30 s of match time; a new match (clock back) zeroes it */
        static double next_c7;
        if (w->time < next_c7 - 60.0) { next_c7 = 0.0; g_c7_yield_intents = g_c7_yields = 0; memset(g_c7_c3, 0, sizeof g_c7_c3); }
        if (w->time >= next_c7) {
            if (next_c7 > 0.0 && c7_log_on()) fprintf(stderr, "[C7] yield intents=%d yields=%d on=%d t=%.0f\n", g_c7_yield_intents, g_c7_yields, ma2_bot_yield_human_on(), w->time);
            if (next_c7 > 0.0) {
                const int *c = g_c7_c3;
                if (c7_log_on()) fprintf(stderr, "[C7] c3 assigned=%d reassigned=%d human_wins=%d swaps_claimed=%d swap_cooldown_blocks=%d elite_keeps=%d "
                        "switches=%d commit_holds=%d grinder_swaps=%d foot_getveh=%d on=%d%d%d%d%d t=%.0f\n",
                        c[MA2_C3_ASSIGNED], c[MA2_C3_REASSIGNED], c[MA2_C3_HUMAN_WINS], c[MA2_C3_SWAPS_CLAIMED], c[MA2_C3_SWAP_COOL_BLOCKS],
                        c[MA2_C3_ELITE_KEEPS], c[MA2_C3_SWITCHES], c[MA2_C3_COMMIT_HOLDS], c[MA2_C3_GRINDER_SWAPS], c[MA2_C3_FOOT_GETVEH],
                        ma2_bot_switch_on(MA2_SW_VEH_ASSIGN), ma2_bot_switch_on(MA2_SW_SWAP_COOL), ma2_bot_switch_on(MA2_SW_VEH_COMMIT),
                        ma2_bot_switch_on(MA2_SW_GRINDER_SWAP), ma2_bot_switch_on(MA2_SW_FOOT_RIDE), w->time);
            }
            next_c7 = w->time + 30.0;
        }
    }
    for (int i = 0; i < w->nvehicles; i++) { w->vehicles[i].human_claim = 0; w->vehicles[i].human_claim_nation = -1; }
    if (!ma2_bot_yield_human_on()) return;
    for (int j = 0; j < w->nplayers; j++) {
        const Ma2PlayerView *h = &w->players[j];
        if (!h->is_human || !h->alive || !h->actor || h->kind != MA2_KIND_PILOT) continue;
        ma2_track_observe(h->nation, h->actor, h->pos, w->time);
        for (int i = 0; i < w->nvehicles; i++) {
            Ma2VehicleView *v = &w->vehicles[i];
            if (v->kind != MA2_KIND_MECH || v->occupied || !v->actor) continue;
            if (v->nation > 0 && v->nation != 6 && v->nation != h->nation) continue;   /* not one the human could enter */
            Ma2Vec3 at = v->has_entry ? v->entry : v->pos;
            int on = ma2_intent_update(h->nation, h->actor, v->actor, at, w->time);
            if (on && !v->human_claim) { v->human_claim = h->player; v->human_claim_nation = h->nation; }
            {   /* C7 counter: intents that turned on this match */
                static uint32_t prev_v[16], prev_h[16]; static int prev_on[16];
                int sl = (int)((v->actor >> 4) & 15);
                if (on && !(prev_v[sl] == v->actor && prev_h[sl] == h->actor && prev_on[sl])) g_c7_yield_intents++;
                prev_v[sl] = v->actor; prev_h[sl] = h->actor; prev_on[sl] = on;
            }
            if (env_on("RECOMP_BOT_DIAG")) {
                static uint32_t last_h[16], last_v[16]; static int last_on[16];
                int slot = (int)((v->actor >> 4) & 15);
                if (last_v[slot] != v->actor || last_h[slot] != h->actor || last_on[slot] != on) {
                    last_v[slot] = v->actor; last_h[slot] = h->actor; last_on[slot] = on;
                    fprintf(stderr, "[BOT-INTENT] t=%.2f human %08X %s 'Mech %08X d=%.0f\n", w->time, h->player,
                            on ? "heading for" : "no longer heading for", v->actor, hypotf(at.x - h->pos.x, at.z - h->pos.z));
                }
            }
        }
    }
}

#include "ma2_bot_objective.c" /* game-mode objectives (flags, cores, checkpoints) */

/* Entity tree walk: child +0x0C, sibling +0x08 (guide 2.3). */
/* ---- The map changes: destroyed buildings and scenery ----
 * A building or prop that is destroyed is replaced by its destroyed version (MABuildingDestroyed /
 * MACorporealDestroyed). Each one that appears after the grid was built re-probes the grid around
 * it -- the same downward rays, 0.5 m apart -- so plans route through the gap (or round new rubble). */
static struct { uint32_t e; } g_wreck[512];
static int g_nwreck;
static struct { Ma2Vec3 p; float r; } g_wreck_q[16];
static void turrets_reapply(void);
static void turrets_restamp(void);
static int g_wreck_nq;
static double g_wreck_t = -1.0;
static void wreck_seen(uint32_t e, double now)
{
    if (now < g_wreck_t) { g_nwreck = 0; g_wreck_nq = 0; g_nstq = g_nstl = g_stjob = 0; ma2_trav_reprobe_cancel(); ma2_trav_commit_cancel(); }   /* a new match */
    g_wreck_t = now;
    for (int i = 0; i < g_nwreck; i++) if (g_wreck[i].e == e) return;
    if (g_nwreck < 512) g_wreck[g_nwreck++].e = e;
    if (!ma2_trav_ready() || g_wreck_nq >= 16) return;                     /* there from the start: in the grid already */
    uint32_t node = MEM32(e + 0x94);
    if (!gva_ok(node, 0x100)) return;
    g_wreck_q[g_wreck_nq].p = read_vec3(node + 0xF0);
    g_wreck_q[g_wreck_nq].r = MEM32(e + 0x28) == g_cls.bdestroyed ? 22.f : 6.f;
    g_wreck_nq++;
}
/* What stands at a grid cell now, from fresh rays 0.5 m apart: the highest top (the ground if
 * nothing); *brk = 1 when everything there is breakable scenery. */
static float wreck_probe(void *ctx, float x, float z, float ground, int *brk)
{
    int hard = 0, any = 0;
    float best = ground;
    (void)ctx;
    for (int iz = 0; iz < 4; iz++)
        for (int ix = 0; ix < 4; ix++) {
            int ok = 0;
            float sx = x - 0.75f + 0.5f * (float)ix, sz = z - 0.75f + 0.5f * (float)iz, g = terrain_height(sx, sz, &ok), t = 0.f;
            uint32_t ob = 0;
            if (!ok) continue;
            Ma2Vec3 o = { sx, g + 40.f, sz }, dn = { 0.f, -1.f, 0.f };
            if (!ray_cast(&g_probe, o, dn, 44.f, 1, 1, &t, &ob)) continue;
            uint32_t owner = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0;
            if (owner && ((is_kind(owner, g_cls.mover) && !is_kind(owner, g_cls.turret)) || is_kind(owner, g_cls.projectile) || MEM32(owner + 0x28) == g_cls.water)) continue;
            float top = g + 40.f - t;
            if (top <= g + 0.4f) continue;
            any = 1;
            if (!(owner && is_kind(owner, g_cls.corporeal) && (MEM32(owner + 0x80) & CORP_BREAKABLE))) hard = 1;
            if (top > best) best = top;
        }
    *brk = any && !hard;
    return best;
}
static void side_prop_forget(Ma2Vec3 p, float r);
static void wreck_queue(Ma2Vec3 p, float r)
{
    side_prop_forget(p, r + 2.f);
    if (g_wreck_nq >= 16) return;
    g_wreck_q[g_wreck_nq].p = p; g_wreck_q[g_wreck_nq].r = r; g_wreck_nq++;
}
/* ew-b8 (R1 P1-1): a destroyed building's re-probe (about 380 cells x 16 rays = 6000 rays, 70-180 ms in one step) is done a few ms per
 * step (RECOMP_BOT_WRECK_MS, default 3) and the grid committed ONCE when the queued jobs are done (or 1 s after the first change).
 * The cells probed and the grid reached are the same as the synchronous call. [Bots] WreckSlice=0 / RECOMP_BOT_WRECK_SLICE=0 restores the old call. */
static unsigned g_wreck_cells, g_wreck_steps;
static double g_wreck_job_t, g_wreck_pend_t = -1.0, g_wreck_job_ms;
static Ma2Vec3 g_wreck_cur; static float g_wreck_cur_r;
static int commit_slice_on(void)
{
    static int on = -1;
    if (on < 0) on = c7_on("commitslice", "RECOMP_BOT_COMMIT_SLICE", "CommitSlice", 1) && c7_on("wreckslice", "RECOMP_BOT_WRECK_SLICE", "WreckSlice", 1);
    return on;
}
static int stall_slice_on(void)
{
    static int on = -1;
    if (on < 0) on = c7_on("stallslice", "RECOMP_BOT_STALL_SLICE", "StallSlice", 1) && commit_slice_on();
    return on;
}
/* A stall at `ahead`: queue its re-probe. 0 = the queue is full (the caller does it at once, as before). */
static int stall_queue(Ma2Vec3 ahead, int stalls, int lvl_ok, int bot)
{
    for (int i = 0; i < g_nstq; i++)
        if (hypotf(g_stq[i].ahead.x - ahead.x, g_stq[i].ahead.z - ahead.z) < 4.f) {
            if (stalls > g_stq[i].stalls) g_stq[i].stalls = stalls;
            g_stq[i].lvl_ok &= lvl_ok; c7_add("stallslice", "merged", 1);
            return 1;
        }
    if (g_nstq >= 8) return 0;
    g_stq[g_nstq].ahead = ahead; g_stq[g_nstq].stalls = stalls; g_stq[g_nstq].lvl_ok = lvl_ok; g_stq[g_nstq].bot = bot; g_nstq++;
    return 1;
}
static void wreck_update_sliced(double now)
{
    static double budget = -1.0, cbudget = -1.0;
    if (budget < 0.0) { budget = (double)env_int("RECOMP_BOT_WRECK_MS", 3); if (budget < 1.0) budget = 1.0; }
    if (cbudget < 0.0) { cbudget = (double)env_int("RECOMP_BOT_COMMIT_MS", 3); if (cbudget < 1.0) cbudget = 1.0; }
    if (now < g_wreck_pend_t) g_wreck_pend_t = -1.0;                         /* a new match */
    int csl = commit_slice_on();
    if (!ma2_trav_reprobe_busy() && !(csl && ma2_trav_commit_busy())) {
        if (g_wreck_nq) {
            g_wreck_nq--;
            g_wreck_cur = g_wreck_q[g_wreck_nq].p; g_wreck_cur_r = g_wreck_q[g_wreck_nq].r;
            if (ma2_trav_reprobe_begin(g_wreck_cur.x, g_wreck_cur.z, g_wreck_cur_r, wreck_probe, NULL)) {
                g_wreck_cells = 0; g_wreck_steps = 0; g_wreck_job_t = now; g_wreck_job_ms = 0.0; g_stjob = 0;
            }
        } else if (g_nstq) {
            g_stcur = g_stq[0];
            for (int i = 1; i < g_nstq; i++) g_stq[i - 1] = g_stq[i];
            g_nstq--;
            g_wreck_cur = g_stcur.ahead; g_wreck_cur_r = 8.f;
            if (ma2_trav_reprobe_begin(g_wreck_cur.x, g_wreck_cur.z, g_wreck_cur_r, wreck_probe, NULL)) {
                g_wreck_cells = 0; g_wreck_steps = 0; g_wreck_job_t = now; g_wreck_job_ms = 0.0; g_stjob = 1;
                c7_add("stallslice", "jobs", 1);
            }
        }
    }
    if (ma2_trav_reprobe_busy()) {
        double t0 = c7_t0();
        int done = ma2_trav_reprobe_step(budget, &g_wreck_cells);
        c7_ms("wreckslice", "step", t0);
        if (g_stjob) c7_ms("stallslice", "step", t0);
        g_wreck_steps++;
        c7_add("wreckslice", "steps", 1);
        if (done) {
            turrets_restamp();                                               /* the surface steps rewrote the blocked cells under turret / learned discs */
            if (g_stjob) {
                c7_max("stallslice", "job_steps_max", g_wreck_steps);
                if (g_nstl < 8) g_stl[g_nstl++] = g_stcur;                   /* its learn test waits for the commit */
                g_stjob = 0;
            } else {
                c7_add("wreckslice", "jobs", 1); c7_add("wreckslice", "cells", g_wreck_cells); c7_max("wreckslice", "job_steps_max", g_wreck_steps);
            }
            if (g_wreck_pend_t < 0.0) g_wreck_pend_t = now;
            if (env_on("RECOMP_BOT_DIAG"))
                fprintf(stderr, "[BOT-NAV] t=%.1f destroyed at %.0f,%.0f: re-probed %.0f m around it (%u cells in %u steps, %.1f s)\n", now, g_wreck_cur.x, g_wreck_cur.z, g_wreck_cur_r, g_wreck_cells, g_wreck_steps, now - g_wreck_job_t);
        }
    }
    if (csl && ma2_trav_commit_busy()) {                                    /* ft-bots CommitSlice: a few ms of the grid refresh per step */
        double t0 = c7_t0();
        unsigned e0 = ma2_trav_epoch();
        int done = ma2_trav_commit_step(cbudget);
        c7_ms("commitslice", "step", t0);
        c7_add("commitslice", "steps", 1);
        if (done) {
            unsigned r = 0, f = 0;
            ma2_trav_commit_stats(&r, &f);
            c7_add("commitslice", "commits", 1); c7_max("commitslice", "restarts", r); c7_max("commitslice", "fallbacks", f);
            g_wreck_pend_t = -1.0;
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-NAV] t=%.1f sliced grid commit (epoch %u -> %u)\n", now, e0, ma2_trav_epoch());
            int learned = 0;
            for (int i = 0; i < g_nstl; i++) {                               /* stalls whose spot is still open on the fresh map: learn it */
                const StallJob *sj = &g_stl[i];
                if (ma2_trav_clear(sj->ahead.x, sj->ahead.z, 0.0) > 1.5f && sj->stalls >= 2 && sj->lvl_ok) {
                    side_prop_add(sj->ahead, 2.5f); learned++;
                    if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-NAV] t=%.1f bot=%d learned an unseen obstacle at %.0f,%.0f\n", now, sj->bot, sj->ahead.x, sj->ahead.z);
                }
            }
            if (g_nstl) { c7_add("stallslice", "learn_tests", g_nstl); c7_add("stallslice", "learned", learned); }
            g_nstl = 0;
            if (learned) turrets_restamp();                                  /* stamped now, committed by the next slice */
        }
    }
    if (!ma2_trav_reprobe_busy() && !(csl && ma2_trav_commit_busy()) && ma2_trav_dirty_pending() &&
        (!g_wreck_nq && !g_nstq ? 1 : (g_wreck_pend_t >= 0.0 && now - g_wreck_pend_t >= 1.0))) {
        double t0 = c7_t0();
        unsigned e0 = ma2_trav_epoch();
        if (csl) {
            if (ma2_trav_commit_begin()) {
                c7_add("commitslice", "jobs", 1);
                int done = ma2_trav_commit_step(cbudget);                   /* first slice at once */
                c7_ms("commitslice", "step", t0);
                c7_add("commitslice", "steps", 1);
                if (done) { c7_add("commitslice", "commits", 1); g_wreck_pend_t = -1.0; }
            }
        } else {
            ma2_trav_commit_if_dirty();
            c7_ms("wreckslice", "commit", t0);
            c7_add("wreckslice", "commits", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-NAV] t=%.1f wreck grid commit (epoch %u -> %u)\n", now, e0, ma2_trav_epoch());
            g_wreck_pend_t = -1.0;
        }
    }
}
static void wreck_update(double now)
{
    static int sliced = -1;
    if (sliced < 0) sliced = c7_on("wreckslice", "RECOMP_BOT_WRECK_SLICE", "WreckSlice", 1);
    if (sliced) { if (g_probe.phys) wreck_update_sliced(now); return; }
    if (!g_wreck_nq || !g_probe.phys) return;
    g_wreck_nq--;
    Ma2Vec3 p = g_wreck_q[g_wreck_nq].p; float r = g_wreck_q[g_wreck_nq].r;
    unsigned e0 = ma2_trav_epoch();
    ma2_trav_reprobe(p.x, p.z, r, wreck_probe, NULL);
    turrets_reapply();
    if (env_on("RECOMP_BOT_DIAG"))
        fprintf(stderr, "[BOT-NAV] t=%.1f destroyed at %.0f,%.0f: re-probed %.0f m around it%s\n", now, p.x, p.z, r, ma2_trav_epoch() != e0 ? ", grid changed" : "");
}

/* Hull outline of a parked vehicle (Ma2VehicleView.ext): its reach from pos in MA2_HULL_DIRS world
 * directions (k * 22.5 degrees from +z toward +x), measured with horizontal rays from 14 m out
 * toward its centre -- where each meets the vehicle's own collider. Cached per vehicle until it moves.
 *
 * Each direction is the furthest of five rays spread across its sector: a VTOL's nose and wing
 * tips fell between single rays (the two either side of a nose read 6.5 and 2.9 m; a pilot then
 * wedged on that nose 6.8 m out). The heights start at the vehicle's own base, not the terrain
 * under it (a pad), and run to a pilot's head; ext_hi adds a 'Mech's hips (a VTOL's tail boom
 * reaches 1.5 m further up there). A ray that meets something else first -- a 'Mech standing beside
 * it at the start, a crate -- goes on from there with that thing skipped: a spawn-line 'Mech hid a
 * VTOL's wing for the whole match, the outline on that side was the bare fuselage, and bots walked
 * into the wing (2026-10-19). Turrets keep their original three rays over the ground: their disc is
 * stamped into the map. */
#define HULL_SUB 5
#define HULL_SLOTS 64
static struct { uint32_t actor, used; Ma2Vec3 at; float ext[MA2_HULL_DIRS], hi[MA2_HULL_DIRS], max; } g_ext[HULL_SLOTS];
static uint32_t g_ext_clock;
/* F32: the table used to live across matches and fall back to slot (actor>>4)%48 when full, so
 * once 48 hulls had been seen (late in a match, or any rematch) two live hulls on one slot evicted
 * each other and were re-measured every 12 ms window (~+4 ms a frame). Now: 64 slots, cleared per
 * match, LRU eviction (a dead vehicle's slot is the oldest). RECOMP_BOT_HULL_CACHE=0 / ini [Bots]
 * HullCacheFix=0 = no clearing and the hash fallback. [C7] F32 counters. */
static void hull_cache_level_reset(void)
{
    if (c7_on("F32", "RECOMP_BOT_HULL_CACHE", "HullCacheFix", 1)) { memset(g_ext, 0, sizeof g_ext); g_ext_clock = 0; }
}
/* Distance along o + d*s (s <= len) at which the ray meets v's own collider, -1 if it does not;
 * *blocked set when other things stood in the way and it never got through them. */
static float hull_ray(const Ma2VehicleView *v, Ma2Vec3 o, Ma2Vec3 d, float len, int *blocked)
{
    uint32_t self = probe_live(&g_probe) ? g_probe.actor : 0;
    float s0 = 0.f;
    for (int tries = 0; tries < 5 && s0 < len; tries++) {
        float t = 0.f; uint32_t ob = 0;
        Ma2Vec3 q = { o.x + d.x * s0, o.y, o.z + d.z * s0 };
        if (!ray_cast_as(self, q, d, len - s0, 1, 1, &t, &ob)) return -1.f;
        uint32_t own = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0;
        if (own == v->actor) return s0 + t;
        if (own && own != self && gva_ok(own, 0x90)) self = own;     /* a unit, a crate: skip it from here on */
        s0 += t + 0.25f;
    }
    *blocked = 1;
    return -1.f;
}
static void vehicle_extent(Ma2VehicleView *v)
{
    int slot = -1, free_slot = -1, used = 0, f32 = c7_on("F32", "RECOMP_BOT_HULL_CACHE", "HullCacheFix", 1);
    for (int i = 0; i < HULL_SLOTS; i++) {
        if (g_ext[i].actor) used++;
        if (g_ext[i].actor == v->actor) { slot = i; break; }
        if (free_slot < 0 && !g_ext[i].actor) free_slot = i;
    }
    if (slot >= 0) g_ext[slot].used = ++g_ext_clock;
    if (slot >= 0 && hypotf(g_ext[slot].at.x - v->pos.x, g_ext[slot].at.z - v->pos.z) < 1.f) {
        memcpy(v->ext, g_ext[slot].ext, sizeof v->ext); memcpy(v->ext_hi, g_ext[slot].hi, sizeof v->ext_hi);
        v->ext_max = g_ext[slot].max; return;
    }
    if (!g_probe.phys) return;
    if (slot < 0 && free_slot >= 0) slot = free_slot;
    if (slot < 0 && !f32) slot = (int)((v->actor >> 4) % HULL_SLOTS);
    c7_max("F32", "slots_used", used);
    /* Some 4 ms of hull rays per frame: a match start lists ~20 parked bodies at ~5 ms each. The
     * rest are measured over the next frames, sized by their kind meanwhile (ext_max 0). Turrets
     * are cheap and measured once, so always at once. */
    static double win_t0, win_ms;
    if (v->kind != MA2_KIND_TURRET) {
        double now0 = perf_now_ms();
        if (now0 - win_t0 > 12.0 || now0 < win_t0) { win_t0 = now0; win_ms = 0.0; }
        if (win_ms > 4.0) return;
    }
    int ok = 0, nblk = 0;
    float g = terrain_height(v->pos.x, v->pos.z, &ok), mx = 0.f;
    if (!ok) g = v->pos.y;
    double perf_t0 = perf_now_ms();
    const float step = 6.2831853f / (float)MA2_HULL_DIRS;
    if (v->kind == MA2_KIND_TURRET) {
        static const float hts[3] = { 0.6f, 1.3f, 2.2f };
        for (int k = 0; k < MA2_HULL_DIRS; k++) {
            float a = (float)k * step, dx = sinf(a), dz = cosf(a), e = 0.f;
            for (int hi = 0; hi < 3; hi++) {
                float t = 0.f;
                uint32_t ob = 0;
                Ma2Vec3 o = { v->pos.x + dx * 14.f, g + hts[hi], v->pos.z + dz * 14.f }, d = { -dx, 0.f, -dz };
                if (ray_cast(&g_probe, o, d, 14.f, 1, 1, &t, &ob) && gva_ok(ob, 0x130) && MEM32(ob + 0x128) == v->actor && 14.f - t > e) e = 14.f - t;
            }
            v->ext[k] = v->ext_hi[k] = e; if (e > mx) mx = e;
        }
    } else {
        /* 0.3-2.6 m: feet to a pilot's head (a VTOL's wings and belly, a tank's skirts, a 'Mech's
         * feet); 3.5 and 5 m: a 'Mech's hips. */
        static const float hts[6] = { 0.3f, 1.0f, 1.8f, 2.6f, 3.5f, 5.0f };
        enum { NJ = MA2_HULL_DIRS * HULL_SUB };
        float lo[NJ], hi[NJ]; unsigned char unk[NJ];
        for (int j = 0; j < NJ; j++) {
            float a = ((float)(j / HULL_SUB) + 0.2f * (float)(j % HULL_SUB - HULL_SUB / 2)) * step, dx = sinf(a), dz = cosf(a);
            Ma2Vec3 d = { -dx, 0.f, -dz };
            int blk = 0;
            lo[j] = hi[j] = 0.f;
            for (int h = 0; h < 6; h++) {
                Ma2Vec3 o = { v->pos.x + dx * 14.f, v->pos.y + hts[h], v->pos.z + dz * 14.f };
                float s = hull_ray(v, o, d, 14.f, &blk), e = s < 0.f ? 0.f : 14.f - s;
                float *dst = h < 4 ? &lo[j] : &hi[j];
                if (e > *dst) *dst = e;
            }
            unk[j] = (unsigned char)blk; nblk += blk;
        }
        /* Rays that never got through: as far as the rays beside them. */
        for (int j = 0; j < NJ; j++) {
            if (!unk[j]) continue;
            int a = (j + NJ - 1) % NJ, b = (j + 1) % NJ;
            float nl = lo[a] > lo[b] ? lo[a] : lo[b], nh = hi[a] > hi[b] ? hi[a] : hi[b];
            if (nl > lo[j]) lo[j] = nl;
            if (nh > hi[j]) hi[j] = nh;
        }
        for (int k = 0; k < MA2_HULL_DIRS; k++) {
            float e = 0.f, eh = 0.f;
            for (int s = 0; s < HULL_SUB; s++) {
                int j = k * HULL_SUB + s;
                if (lo[j] > e) e = lo[j];
                if (hi[j] > eh) eh = hi[j];
            }
            v->ext[k] = e; v->ext_hi[k] = eh > e ? eh : e;
            if (e > mx) mx = e;
        }
    }
    v->ext_max = mx;
    double perf_ms = perf_now_ms() - perf_t0;
    win_ms += perf_ms;
    if (env_on("RECOMP_BOT_HULL_PROFILE")) {   /* developer: the hull's reach per height above its origin */
        fprintf(stderr, "[BOT-HULLPROF] %08X kind=%d pos=%.1f,%.1f,%.1f ground=%.1f\n", v->actor, v->kind, v->pos.x, v->pos.y, v->pos.z, g);
        for (int hi = -2; hi <= 16; hi++) {
            char b[200]; int o = 0; float h = 0.5f * (float)hi;
            for (int k = 0; k < MA2_HULL_DIRS; k++) {
                float a = (float)k * step, dx = sinf(a), dz = cosf(a), t = 0.f, e = 0.f;
                uint32_t ob = 0;
                Ma2Vec3 o3 = { v->pos.x + dx * 14.f, v->pos.y + h, v->pos.z + dz * 14.f }, d = { -dx, 0.f, -dz };
                if (ray_cast(&g_probe, o3, d, 14.f, 1, 1, &t, &ob)) e = gva_ok(ob, 0x130) && MEM32(ob + 0x128) == v->actor ? 14.f - t : -(14.f - t);
                o += snprintf(b + o, sizeof b - (size_t)o, " %5.1f", e);
            }
            fprintf(stderr, "[BOT-HULLPROF]   h=%+4.1f%s\n", h, b);
        }
    }
    if (env_on("RECOMP_BOT_DIAG"))
    {
        char b[200], bh[200]; int o = 0, oh = 0;
        for (int k = 0; k < MA2_HULL_DIRS; k++) {
            o += snprintf(b + o, sizeof b - (size_t)o, " %.1f", v->ext[k]);
            oh += snprintf(bh + oh, sizeof bh - (size_t)oh, " %.1f", v->ext_hi[k]);
        }
        fprintf(stderr, "[BOT-HULL] %08X kind=%d at %.0f,%.0f ext=%s max=%.1f hi=%s blocked=%d %.1f ms\n", v->actor, v->kind, v->pos.x, v->pos.z, b, mx, bh, nblk, perf_ms);
    }
    c7_add("F32", "measures", 1);
    if (slot < 0) {                       /* full: the least recently looked-up hull (a dead one first) */
        slot = 0;
        for (int i = 1; i < HULL_SLOTS; i++) if (g_ext[i].used < g_ext[slot].used) slot = i;
        c7_add("F32", "evictions", 1);
    }
    g_ext[slot].actor = v->actor; g_ext[slot].at = v->pos; g_ext[slot].max = mx; g_ext[slot].used = ++g_ext_clock;
    memcpy(g_ext[slot].ext, v->ext, sizeof v->ext); memcpy(g_ext[slot].hi, v->ext_hi, sizeof v->ext_hi);
}

/* ---- Turrets: solid, but placed after the map was built (and movers are not in the static
 * collision the map's rays see). Each one is measured with horizontal rays like a parked vehicle
 * and stamped into the map; destroyed, its spot is looked at again. ---- */
static struct { uint32_t e; Ma2Vec3 at; float r; int alive; } g_tur[32];
static int g_ntur;
static double g_tur_t = -1.0;
static int g_tur_dirty;   /* W2b: turret discs stamped, grid refresh pending (flushed after the scan) */
static int turrets_stamp(void)
{
    int n = 0;
    for (int i = 0; i < g_nside; i++) n += ma2_trav_block_disc(g_side[i].p.x, g_side[i].p.z, g_side[i].r);
    for (int i = 0; i < g_ntur; i++) if (g_tur[i].alive) n += ma2_trav_block_disc(g_tur[i].at.x, g_tur[i].at.z, g_tur[i].r);
    return n;
}
static void turrets_restamp(void) { turrets_stamp(); }   /* ew-b8: stamp only; the caller commits (ma2_trav_commit_if_dirty) */
static void turrets_reapply(void)
{
    if (turrets_stamp()) ma2_trav_commit();
}
static void wreck_queue(Ma2Vec3 p, float r);
static void turret_seen_impl(uint32_t e, double now);
static void turret_seen(uint32_t e, double now)   /* W2b: RECOMP_HITCH times it ([HITCH-W] turret_seen) */
{
    static int on = -1; static LARGE_INTEGER f; LARGE_INTEGER a, b;
    if (on < 0) { on = env_int("RECOMP_HITCH", 0) > 0; QueryPerformanceFrequency(&f); }
    if (!on) { turret_seen_impl(e, now); return; }
    QueryPerformanceCounter(&a); turret_seen_impl(e, now); QueryPerformanceCounter(&b);
    { double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart; if (ms >= 3.0) fprintf(stderr, "[HITCH-W] t=%+.3fs turret_seen=%.1fms e=%08X\n", nv2a_hitch_rel_s(), ms, e); }
}
static void turret_seen_impl(uint32_t e, double now)
{
    int i, alive;
    if (now < g_tur_t) g_ntur = 0;                               /* a new match */
    g_tur_t = now;
    if (!ma2_trav_ready() || (g_nav_slicing && g_nc.on)) return;      /* NavCache: a turret disc is a live part, stamped once the static grid is complete */
    Ma2Vec3 p = actor_pos(e);
    if (fabsf(p.x) + fabsf(p.z) < 1.f || !(p.x == p.x)) return;  /* not placed yet */
    alive = actor_int(e, 0x74, 0x000C9476u, 1) > 0;
    for (i = 0; i < g_ntur; i++) if (g_tur[i].e == e) break;
    if (i < g_ntur && hypotf(g_tur[i].at.x - p.x, g_tur[i].at.z - p.z) < 1.f && g_tur[i].alive == alive) return;
    if (i == g_ntur) { if (g_ntur >= 32) return; g_ntur++; }
    if (g_tur[i].e == e && g_tur[i].alive && !alive) wreck_queue(g_tur[i].at, g_tur[i].r + 2.f);   /* destroyed */
    Ma2VehicleView vv; memset(&vv, 0, sizeof vv);
    vv.actor = e; vv.kind = MA2_KIND_TURRET; vv.pos = p;
    vehicle_extent(&vv);
    g_tur[i].e = e; g_tur[i].at = p; g_tur[i].alive = alive;
    g_tur[i].r = vv.ext_max > 0.5f ? vv.ext_max + 0.5f : 3.5f;
    if (alive && ma2_trav_block_disc(p.x, p.z, g_tur[i].r)) {   /* W2b TurretBatch (default on; [Bots] TurretBatch=0 / RECOMP_BOT_TURRET_BATCH=0 restores): one grid refresh per scan, not one (about 9 ms, full-grid chamfer) per turret: UC has 6 = a 58 ms frame at match start */
        if (c7_on("turretbatch", "RECOMP_BOT_TURRET_BATCH", "TurretBatch", 1)) g_tur_dirty = 1; else ma2_trav_commit();
    }
    if (env_on("RECOMP_BOT_DIAG"))
        fprintf(stderr, "[BOT-NAV] t=%.1f turret %08X at %.0f,%.0f r=%.1f %s\n", now, e, p.x, p.z, g_tur[i].r, alive ? "blocked on the map" : "destroyed");
}

static float surface_height(float x, float z, int *ok);
/* F67: the view held 32 pickups, objectives included, and kill drops pile up (59 seen in 7 min on
 * UnderCover): past 32 the later ones in scan order -- a flag or core among them -- were left out.
 * Now 96, and when it is full an objective takes the last power-up's place.
 * RECOMP_BOT_PICKUP_CAP / [Bots] PickupCap (off = the old 32).
 * [C7] F67 live_max (most pickups in one view), objective_swaps, dropped (a power-up left out). */
static int f67_on(void) { return c7_on("F67", "RECOMP_BOT_PICKUP_CAP", "PickupCap", 1); }
static int pickup_cap(void) { return f67_on() ? MA2_BOT_MAX_PICKUPS : 32; }
static void pickup_overflow(Ma2World *w, const Ma2PickupView *pv)
{
    static int logged;
    if (pv->kind != MA2_PICKUP_POWERUP && f67_on())
        for (int i = w->npickups - 1; i >= 0; i--)
            if (w->pickups[i].kind == MA2_PICKUP_POWERUP) { w->pickups[i] = *pv; c7_add("F67", "objective_swaps", 1); return; }
    if (f67_on()) c7_add("F67", "dropped", 1);
    if (!logged++) fprintf(stderr, "[PICKUP] view full (%d): %08X kind=%d left out\n", w->npickups, pv->entity, pv->kind);
}

/* ---- Tall roofs (push-x2): a raster scan of the static surface, flood-filled into buildings ----
 * Found on UC: the city's tall buildings (25x25 m, tops 25-40 m over the ground) are NOT MABuilding entities (those are
 * 16 small base buildings), so the roofs come from the map itself: once per map, in slices after the nav grid is ready,
 * the surface height is sampled on a ~5 m grid; cells at least 8 m over the terrain are flood-filled into blobs; each
 * blob of at least 6 cells whose flat top stands 10 m over the ground becomes one roof (a stand point near the middle
 * of its top). The brain asks for the roofs near a BattleArmor through Ma2Senses.roofs. */
#define RF_MAX 192
typedef struct RoofRec { Ma2Vec3 p; float h; int cells; } RoofRec;
static RoofRec g_roof[RF_MAX]; static int g_nroof;
static struct RoofScan {
    int state, i, j, nx, nz, ntall; float x0, z0, st;
    unsigned char *tall; float *sh, *gh; int *stack;
} g_rs;
static void roofs_free(void)
{
    free(g_rs.tall); free(g_rs.sh); free(g_rs.gh); free(g_rs.stack);
    memset(&g_rs, 0, sizeof g_rs); g_nroof = 0;
}
static void roofs_finish(void)
{
    const int nx = g_rs.nx, nz = g_rs.nz; const float st = g_rs.st;
    int nb = 0;
    for (int j = 0; j < nz; j++) for (int i = 0; i < nx; i++) {
        if (g_rs.tall[j * nx + i] != 1) continue;
        int sp = 0, cnt = 0, nt = 0;
        float top = -1e9f; double sx = 0, sz = 0;
        g_rs.stack[sp++] = j * nx + i; g_rs.tall[j * nx + i] = 2;
        int first = sp;
        (void)first;
        /* pass 1: flood the blob (cells chained within 6 m of height), keep its cells on the stack array's tail */
        int *cells = g_rs.stack + (size_t)nx * nz / 2;   /* second half of the array holds the blob's cells */
        int ncell = 0;
        while (sp) {
            int c = g_rs.stack[--sp], ci = c % nx, cj = c / nx; cnt++;
            if (ncell < nx * nz / 2) cells[ncell++] = c;
            if (g_rs.sh[c] > top) top = g_rs.sh[c];
            for (int dj = -1; dj <= 1; dj++) for (int di = -1; di <= 1; di++) {
                int ni = ci + di, nj = cj + dj; if (ni < 0 || nj < 0 || ni >= nx || nj >= nz) continue;
                if (g_rs.tall[nj * nx + ni] == 1 && fabsf(g_rs.sh[nj * nx + ni] - g_rs.sh[c]) < 6.f && sp < nx * nz / 2 - 16) { g_rs.tall[nj * nx + ni] = 2; g_rs.stack[sp++] = nj * nx + ni; }
            }
        }
        if (cnt < 6 || g_nroof >= RF_MAX) continue;
        for (int q = 0; q < ncell; q++) { int c = cells[q]; if (g_rs.sh[c] >= top - 1.5f) { sx += c % nx; sz += c / nx; nt++; } }
        if (!nt) continue;
        float cx = (float)(sx / nt), cz = (float)(sz / nt), bd = 1e9f; int bc = -1;
        for (int q = 0; q < ncell; q++) {
            int c = cells[q], ci = c % nx, cj = c / nx;
            if (g_rs.sh[c] < top - 1.5f) continue;
            int interior = 1;
            for (int d = 0; d < 4; d++) {
                int ni = ci + (d == 0) - (d == 1), nj = cj + (d == 2) - (d == 3);
                if (ni < 0 || nj < 0 || ni >= nx || nj >= nz || fabsf(g_rs.sh[nj * nx + ni] - g_rs.sh[c]) > 1.0f) interior = 0;
            }
            float d2 = hypotf((float)ci - cx, (float)cj - cz) + (interior ? 0.f : 100.f);
            if (d2 < bd) { bd = d2; bc = c; }
        }
        if (bc < 0 || bd >= 100.f) continue;                       /* no flat patch with a neighbour ring: not a roof */
        float h = g_rs.sh[bc] - g_rs.gh[bc];
        if (h < 10.f) continue;
        RoofRec *r = &g_roof[g_nroof++];
        r->p.x = g_rs.x0 + st * (float)(bc % nx); r->p.z = g_rs.z0 + st * (float)(bc / nx); r->p.y = g_rs.sh[bc]; r->h = h; r->cells = cnt;
        nb++;
    }
    if (c7_log_on()) fprintf(stderr, "[BOT-ROOFS] grid %dx%d step %.0f tall cells %d -> %d roofs (>= 10 m)\n", nx, nz, st, g_rs.ntall, g_nroof);
    if (env_on("RECOMP_BOT_DIAG"))
        for (int i = 0; i < g_nroof; i++) fprintf(stderr, "[BOT-ROOFS] %d at %.0f,%.0f y=%.1f h=%.1f cells=%d\n", i, g_roof[i].p.x, g_roof[i].p.z, g_roof[i].p.y, g_roof[i].h, g_roof[i].cells);
    (void)nb;
    free(g_rs.tall); free(g_rs.sh); free(g_rs.gh); free(g_rs.stack);
    g_rs.tall = NULL; g_rs.sh = NULL; g_rs.gh = NULL; g_rs.stack = NULL; g_rs.state = 2;
}
/* One slice per game step (about 400 rays), started once the nav grid is ready. */
static void roofs_step(void)
{
    if (g_rs.state == 2 || !g_probe.phys || !ma2_trav_ready() || !env_int("RECOMP_BOT_ROOFS", 1)) return;
    uint32_t t = MEM32(VA_TERRAIN);
    if (!gva_ok(t, 0x840) || SMEM32(t) >= 0) return;
    if (g_rs.state == 0) {
        float cellt = MEMF(t + 0x814), mx = ((int)MEM32(t + 4) - 1) * cellt, mz = ((int)MEM32(t + 8) - 1) * cellt;
        float st = 5.f;
        /* merge safety pass: a garbage terrain header (NaN / inf / huge cell size) must neither spin this loop forever nor size the grid from UB */
        if (!(cellt > 0.f && cellt < 1000.f && mx > 0.f && mz > 0.f && mx < 1e5f && mz < 1e5f)) { g_rs.state = 2; return; }
        while (st < 200.f && (mx / st + 1.f) * (mz / st + 1.f) > 300000.f) st += 1.f;
        if ((mx / st + 1.f) * (mz / st + 1.f) > 300000.f) { g_rs.state = 2; return; }
        g_rs.x0 = 0.f; g_rs.z0 = 0.f; g_rs.st = st; g_rs.nx = (int)(mx / st) + 1; g_rs.nz = (int)(mz / st) + 1;
        size_t n = (size_t)g_rs.nx * (size_t)g_rs.nz;
        g_rs.tall = (unsigned char *)calloc(n, 1); g_rs.sh = (float *)calloc(n, sizeof(float)); g_rs.gh = (float *)calloc(n, sizeof(float)); g_rs.stack = (int *)malloc(n * sizeof(int));
        if (!g_rs.tall || !g_rs.sh || !g_rs.gh || !g_rs.stack || g_rs.nx < 4 || g_rs.nz < 4) { roofs_free(); g_rs.state = 2; return; }
        g_rs.state = 1; g_rs.i = g_rs.j = 0; g_rs.ntall = 0;
    }
    for (int n = 0; n < 400 && g_rs.state == 1; n++) {
        int ok = 0, okg = 0, c = g_rs.j * g_rs.nx + g_rs.i;
        float x = g_rs.x0 + g_rs.st * (float)g_rs.i, z = g_rs.z0 + g_rs.st * (float)g_rs.j;
        float g = terrain_height(x, z, &okg), s = okg ? surface_height(x, z, &ok) : 0.f;
        g_rs.sh[c] = s; g_rs.gh[c] = g;
        if (ok && okg && s >= g + 8.f) { g_rs.tall[c] = 1; g_rs.ntall++; }
        if (++g_rs.i >= g_rs.nx) { g_rs.i = 0; if (++g_rs.j >= g_rs.nz) { roofs_finish(); return; } }
    }
}
/* Roofs within maxd (horizontal) of `from`, nearest first, at most max; each re-checked (a destroyed building leaves a lower surface). */
static int sn_roofs(void *ctx, Ma2Vec3 from, float maxd, Ma2Vec3 *out, int max)
{
    int idx[RF_MAX], n = 0; float dd[RF_MAX];
    (void)ctx;
    for (int i = 0; i < g_nroof; i++) {
        float d = hypotf(g_roof[i].p.x - from.x, g_roof[i].p.z - from.z);
        if (d > maxd) continue;
        int a = n++; while (a > 0 && dd[a - 1] > d) { dd[a] = dd[a - 1]; idx[a] = idx[a - 1]; a--; } dd[a] = d; idx[a] = i;
    }
    int m = 0;
    for (int a = 0; a < n && m < max && a < 8; a++) {
        int ok = 0; float s = surface_height(g_roof[idx[a]].p.x, g_roof[idx[a]].p.z, &ok);
        if (!ok || fabsf(s - g_roof[idx[a]].p.y) > 1.5f) continue;
        out[m++] = g_roof[idx[a]].p;
    }
    return m;
}
static void scan_entities(Ma2World *w)
{
    w->nvehicles = 0; w->npickups = 0; w->nproj = 0;
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4);
    if (!gva_ok(r1, 8)) return;
    uint32_t root = MEM32(r1 + 4);
    if (!gva_ok(root, 0x30)) return;
    uint32_t stack[512]; int sp = 0, visited = 0;
    stack[sp++] = root;
    Ma2VehicleView occupied[MA2_BOT_MAX_VEHICLES]; int nocc = 0;
    while (sp > 0 && visited < 8192) {
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58)) continue;
        {   /* base helipads: a building with a collection area (landing zone) under it */
            static uint32_t building_cls;
            if (!building_cls) building_cls = class_id_of("MABuilding");
            if (MEM32(e + 0x28) == building_cls)
                for (uint32_t ch = MEM32(e + 0xC), k = 0; k < 16 && gva_ok(ch, 0x30); k++, ch = MEM32(ch + 8))
                    if (MEM32(ch + 0x28) == g_cls.collection) {
                        uint32_t nd = MEM32(e + 0x94);
                        if (gva_ok(nd, 0x100)) { void ma2_bot_pad_add(Ma2Vec3); ma2_bot_pad_add(read_vec3(nd + 0xF0)); }
                        if (env_on("RECOMP_BOT_DIAG")) {        /* once per area: floats that look like a position near the building */
                            static uint32_t seen[8]; int k2; for (k2 = 0; k2 < 8 && seen[k2] && seen[k2] != ch; k2++) {}
                            if (k2 < 8 && !seen[k2] && gva_ok(nd, 0x100)) {
                                seen[k2] = ch; Ma2Vec3 bp = read_vec3(nd + 0xF0); int okg2 = 0, oks2 = 0;
                                float tg = terrain_height(bp.x, bp.z, &okg2), ts = surface_height(bp.x, bp.z, &oks2);
                                char ln[1500]; int q = snprintf(ln, sizeof ln, "[BOT-PAD] area %08X of %08X ground=%.1f surface=%.1f:", ch, e, tg, ts);
                                for (uint32_t off = 0x30; off < 0x400 && q < 1450; off += 4) {
                                    float f = MEMF(ch + off);
                                    if (fabsf(f - bp.x) < 80.f || fabsf(f - bp.z) < 80.f || (fabsf(f) > 0.5f && fabsf(f) < 60.f && off < 0x200))
                                        q += snprintf(ln + q, sizeof ln - (size_t)q, " %X=%.1f", off, f);
                                }
                                fprintf(stderr, "%s\n", ln);
                            }
                        }
                        break;
                    }
        }
        {   /* test: RECOMP_TEST_DUMP_NEAR="x,z,r,t" lists every entity within r of (x,z) once, at time t */
            static double dump_at; const char *dn = getenv("RECOMP_TEST_DUMP_NEAR");
            float dx_ = 0, dz_ = 0, dr_ = 0, dt_ = 0;
            if (dn && sscanf(dn, "%f,%f,%f,%f", &dx_, &dz_, &dr_, &dt_) == 4 && w->time >= dt_ && (dump_at == 0.0 || dump_at == w->time)) {
                dump_at = w->time;
                uint32_t nd = MEM32(e + 0x94);
                if (gva_ok(nd, 0x100)) {
                    Ma2Vec3 q = read_vec3(nd + 0xF0);
                    int near_ = (q.x - dx_) * (q.x - dx_) + (q.z - dz_) * (q.z - dz_) < dr_ * dr_;
                    if (near_ || MEM32(e + 0x28) == 0xC3E3D036u)   /* near the spot, or any MABuilding */
                        fprintf(stderr, "[ENT] %08X cls=%08X vt=%08X pos=%.1f,%.1f,%.1f w2C=%08X w30=%08X w34=%08X w38=%08X w3C=%08X w40=%08X%s\n", e, MEM32(e + 0x28), MEM32(e), q.x, q.y, q.z,
                                MEM32(e + 0x2C), MEM32(e + 0x30), MEM32(e + 0x34), MEM32(e + 0x38), MEM32(e + 0x3C), MEM32(e + 0x40), near_ ? " NEAR" : "");
                    if (near_)
                        for (uint32_t ch = MEM32(e + 0xC), k = 0; k < 16 && gva_ok(ch, 0x30); k++, ch = MEM32(ch + 8))
                            fprintf(stderr, "[ENT]   child %08X cls=%08X vt=%08X\n", ch, MEM32(ch + 0x28), MEM32(ch));
                }
            }
        }
        if (is_kind(e, g_cls.mover)) {
            if (is_kind(e, g_cls.turret)) { turret_seen(e, w->time); continue; }
            if (is_kind(e, g_cls.projectile)) {
                /* where it is and how fast it closes, from where it was last frame */
                static uint32_t pe[128]; static Ma2Vec3 pp[128]; static double pt[128]; static int pn;   /* ew-b8 B8.16: 64 -> 128 */
                Ma2Vec3 pos = actor_pos(e), vel = { 0, 0, 0 };
                int k; for (k = 0; k < pn && pe[k] != e; k++) {}
                if (k < pn) {
                    double dtp = w->time - pt[k];
                    if (dtp > 0.005 && dtp < 0.5) { vel.x = (pos.x - pp[k].x) / (float)dtp; vel.y = (pos.y - pp[k].y) / (float)dtp; vel.z = (pos.z - pp[k].z) / (float)dtp; }
                } else { k = pn < 128 ? pn++ : (int)(w->frame % 128u); pe[k] = e; }
                pp[k] = pos; pt[k] = w->time;
                if (w->nproj < 32 && (vel.x != 0.f || vel.z != 0.f || vel.y != 0.f)) {
                    w->proj[w->nproj].ent = e; w->proj[w->nproj].pos = pos; w->proj[w->nproj].vel = vel; w->nproj++;
                }
                continue;
            }
            if (is_kind(e, g_cls.spider)) continue;
            if (!((MEM32(e + 0x1C) >> 11) & 1)) continue;          /* not controllable (sub_C9084) */
            int hp = actor_int(e, 0x74, 0x000C9476u, 0);
            if (hp <= 0) continue;
            Ma2VehicleView vv; memset(&vv, 0, sizeof(vv));
            vv.actor = e; vv.kind = actor_kind(e); vv.pos = actor_pos(e);
            vv.hp = (float)hp; vv.hp_max = (float)actor_int(e, 0x6C, 0x000C8C2Au, 0);
            float yaw = 0.f; actor_yaw(e, vv.pos, &yaw); vv.yaw = yaw;
            uint32_t parent = MEM32(e + 4);
            vv.occupied = gva_ok(parent, 0x30) && MEM32(parent + 0x28) == g_cls.player;
            /* Pool vehicles not yet placed sit at the origin; pilots are not boardable. */
            if (!vv.occupied && (fabsf(vv.pos.x) + fabsf(vv.pos.y) + fabsf(vv.pos.z) < 1.f ||
                                 !(vv.pos.x == vv.pos.x) || !(vv.pos.z == vv.pos.z) || vv.kind == MA2_KIND_PILOT)) continue;
            vv.nation = actor_int(e, 0xA8, 0x000EB091u, -1);
            if (vv.kind == MA2_KIND_TURRET) fixed_gun_note(e, vv.pos, hp, w->time);
            if (!vv.occupied) {
                Ma2EntryZone z;
                if (ma2_vehicle_entry_zones(e, &z, 1) > 0) { vv.has_entry = 1; vv.entry = z.center; vv.entry_r = z.radius; }
                vehicle_extent(&vv);
            }
            if (!vv.occupied) { if (w->nvehicles < MA2_BOT_MAX_VEHICLES) w->vehicles[w->nvehicles++] = vv; }
            else if (nocc < MA2_BOT_MAX_VEHICLES) occupied[nocc++] = vv;
            continue;
        }
        if (MEM32(e + 0x28) == g_cls.bdestroyed || MEM32(e + 0x28) == g_cls.cdestroyed) { wreck_seen(e, w->time); continue; }
        int pk = 0;
        if (is_kind(e, g_cls.powerup)) pk = MA2_PICKUP_POWERUP;   /* crates and what destroyed units drop */
        else if (is_kind(e, g_cls.flag)) pk = MA2_PICKUP_FLAG;
        else if (is_kind(e, g_cls.checkpoint)) pk = MA2_PICKUP_CHECKPOINT;
        else if (is_kind(e, g_cls.datacore)) pk = MA2_PICKUP_DATACORE;
        else if (is_kind(e, g_cls.collection)) pk = MA2_PICKUP_COLLECTION_AREA;
        if (pk && (w->npickups < pickup_cap() || pk != MA2_PICKUP_POWERUP)) {
            uint32_t node = MEM32(e + 0x94);
            Ma2PickupView pv; memset(&pv, 0, sizeof(pv));
            pv.entity = e; pv.kind = pk; pv.nation = -1;      /* TODO: owner team of flags/bases */
            pv.wclass = -1;
            if (pk == MA2_PICKUP_POWERUP && gva_ok(e, 0x1B8)) {
                /* PowerupArmor: the save routine sub_131206 writes [this+0x190]; RECOMP_BOT_PU_OFF
                 * picks the base (0 = entity, 0x20 = its save interface) until confirmed. */
                int off = env_int("RECOMP_BOT_PU_OFF", 0x20);
                int v = (int)MEM32(e + 0x190 + (uint32_t)off);
                pv.armor = v > 0 && v < 100000 ? v : 0;
                /* PuClass (11-08 pudump, CTF Deliverance): a weapon upgrade sets exactly one of +0x1B4/+0x1B8/+0x1BC (PowerupWeaponFireClass0/1/2);
                 * player 00F85DA0 took the +1B4 one then the +1B8 one and its arsenal class 0 then class 1 rose (+0x7C+4c). */
                if (!pv.armor && gva_ok(e, 0x1C0)) for (int c = 0; c < 3; c++) if (MEM32(e + 0x1B4 + 4u * (uint32_t)c) == 1) { pv.wclass = c; break; }
                if (env_on("RECOMP_BOT_DIAG")) {
                    static uint32_t seen[64]; static int ns;
                    int k; for (k = 0; k < ns && seen[k] != e; k++) {}
                    if (k == ns && ns < 64) {
                        seen[ns++] = e;
                        fprintf(stderr, "[PICKUP] %08X at %.0f,%.0f +190=%d +1B0=%d\n", e, read_vec3(MEM32(e + 0x94) + 0xF0).x, read_vec3(MEM32(e + 0x94) + 0xF0).z, (int)MEM32(e + 0x190), (int)MEM32(e + 0x1B0));
                        if (env_on("RECOMP_BOT_PU_DUMP") && gva_ok(e, 0x1E0)) {   /* find PowerupWeaponFireClass0/1/2 */
                            fprintf(stderr, "[PICKUP-DUMP] %08X", e);
                            for (uint32_t o = 0x180; o < 0x1E0; o += 4) fprintf(stderr, " %03X=%08X", o, MEM32(e + o));
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }
            if (gva_ok(node, 0x100)) pv.pos = read_vec3(node + 0xF0);
            else if (is_kind(e, g_cls.corporeal)) pv.pos = read_vec3(e + 0x4C0);
            else continue;
            /* A power-up that was taken waits out its respawn parked at the map's edge (seen at
             * 1,1 and 300,0): a VTOL flew at the corner until the play-area wall stopped it. Nothing
             * real lies within 15 m of the edge. */
            if (pk == MA2_PICKUP_POWERUP) {
                int gw = 0, gh = 0, no = 0, ns = 0;
                ma2_trav_stats(&gw, &gh, &no, &ns);
                float mx = gw > 0 ? (float)gw * 2.f : 1e9f, mz = gh > 0 ? (float)gh * 2.f : 1e9f;
                if (pv.pos.x < 15.f || pv.pos.z < 15.f || pv.pos.x > mx - 15.f || pv.pos.z > mz - 15.f) {
                    static int nlog;
                    if (env_on("RECOMP_BOT_DIAG") && nlog++ < 12) fprintf(stderr, "[PICKUP] %08X parked at %.0f,%.0f: ignored\n", e, pv.pos.x, pv.pos.z);
                    continue;
                }
            }
            if (w->npickups < pickup_cap()) w->pickups[w->npickups++] = pv;
            else pickup_overflow(w, &pv);
        }
    }
    if (f67_on()) c7_max("F67", "live_max", (uint32_t)w->npickups);
    for (int i = 0; i < nocc && w->nvehicles < MA2_BOT_MAX_VEHICLES; i++) w->vehicles[w->nvehicles++] = occupied[i];
}

/* RECOMP_BOT_DIAG_PROPS=1: once per match, the static scenery near the players' start and what the
 * traversability grid makes of it (clear = metres to a blocked cell, heavy = as a 'Mech sees it). */
/* RECOMP_BOT_DIAG_NEAR="x,z,r;x,z,r": once, every entity within r of each point (any class), with
 * what the map's downward and side rays make of it. */
static int ai_los(uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to, uint32_t *hit);
/* RECOMP_BOT_DIAG_SURF="x0,z0,x1,z1,step": once, the walkable surface heights over that box (rows of z). */
static float surface_height(float x, float z, int *ok);
static void diag_surf(const Ma2World *w)
{
    static int done; static double last = -1.0;
    const char *spec = getenv("RECOMP_BOT_DIAG_SURF");
    float x0, z0, x1, z1, st;
    if (!spec || !g_probe.phys) return;
    if (w->time < last) done = 0;
    last = w->time;
    if (done || w->time < 3.0 || sscanf(spec, "%f,%f,%f,%f,%f", &x0, &z0, &x1, &z1, &st) != 5 || st < 0.5f) return;
    done = 1;
    for (float z = z0; z <= z1; z += st) {
        char row[1024]; int o = 0;
        for (float x = x0; x <= x1 && o < 1000; x += st) { int ok = 0; float h = surface_height(x, z, &ok); o += snprintf(row + o, sizeof row - (size_t)o, " %5.1f", ok ? h : -99.f); }
        fprintf(stderr, "[SURF] z=%.0f%s\n", z, row);
    }
}
static void diag_near(const Ma2World *w)
{
    static int done; static double last = -1.0;
    const char *spec = getenv("RECOMP_BOT_DIAG_NEAR");
    if (!spec || !*spec || !ma2_trav_ready()) return;
    if (w->time < last) done = 0;
    last = w->time;
    if (done || w->time < 3.0) return;
    done = 1;
    float pts[8][3]; int np = 0;
    const char *s = spec;
    while (np < 8 && sscanf(s, "%f,%f,%f", &pts[np][0], &pts[np][1], &pts[np][2]) == 3) { np++; s = strchr(s, ';'); if (!s) break; s++; }
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4), root = gva_ok(r1, 8) ? MEM32(r1 + 4) : 0;
    if (!gva_ok(root, 0x30)) return;
    uint32_t stack[512]; int sp = 0, visited = 0;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192) {
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58)) continue;
        uint32_t node = MEM32(e + 0x94);
        if (!gva_ok(node, 0x100)) continue;
        Ma2Vec3 p = read_vec3(node + 0xF0);
        for (int i = 0; i < np; i++) {
            if (hypotf(p.x - pts[i][0], p.z - pts[i][1]) > pts[i][2]) continue;
            int okg = 0, down = 0, side = 0;
            float g = terrain_height(p.x, p.z, &okg), t = 0.f;
            uint32_t ob = 0;
            Ma2Vec3 o = { p.x, g + 40.f, p.z }, dn = { 0.f, -1.f, 0.f };
            if (ray_cast(&g_probe, o, dn, 44.f, 1, 1, &t, &ob)) down = gva_ok(ob, 0x130) && MEM32(ob + 0x128) == e ? 2 : 1;
            for (int kd = 0; kd < 8; kd++) {
                float a = (float)kd * 0.7853982f; Ma2Vec3 so = { p.x + sinf(a) * 10.f, g + 1.5f, p.z + cosf(a) * 10.f }, sd = { -sinf(a), 0.f, -cosf(a) };
                if (ray_cast(&g_probe, so, sd, 10.f, 1, 1, &t, &ob) && gva_ok(ob, 0x130) && MEM32(ob + 0x128) == e) side++;
            }
            uint32_t los_hit = 0;
            Ma2Vec3 la = { p.x + 12.f, g + 2.f, p.z }, lb = { p.x - 12.f, g + 2.f, p.z };
            int lc = ai_los(0, 0, la, lb, &los_hit);
            fprintf(stderr, "[NEAR] pt%d %08X cls=%08X at %.1f,%.1f,%.1f d=%.1f mover=%d turret=%d corp=%d down=%d side=%d/8 los=%d hit=%08X clear=%.1f\n", i, e, MEM32(e + 0x28),
                    p.x, p.y, p.z, hypotf(p.x - pts[i][0], p.z - pts[i][1]), is_kind(e, g_cls.mover), is_kind(e, g_cls.turret), is_kind(e, g_cls.corporeal),
                    down, side, lc, los_hit, ma2_trav_clear(p.x, p.z, 0.0));
        }
    }
}
static void diag_props(const Ma2World *w)
{
    static double last = -1.0; static int done;
    if (!env_on("RECOMP_BOT_DIAG_PROPS") || !ma2_trav_ready() || w->nplayers < 1) return;
    if (w->time < last) done = 0;
    last = w->time;
    if (done || w->time < 2.0) return;
    done = 1;
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4);
    if (!gva_ok(r1, 8)) return;
    uint32_t root = MEM32(r1 + 4);
    if (!gva_ok(root, 0x30)) return;
    uint32_t stack[512]; int sp = 0, visited = 0, n = 0;
    stack[sp++] = root;
    while (sp > 0 && visited < 8192) {
        uint32_t e = stack[--sp];
        visited++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 512) stack[sp++] = sib;
        if (gva_ok(child, 0x30) && sp < 512) stack[sp++] = child;
        if (!gva_ok(MEM32(e), 0x58) || !is_kind(e, g_cls.corporeal) || (is_kind(e, g_cls.mover) && !is_kind(e, g_cls.turret)) || !gva_ok(e, 0x4D0)) continue;
        uint32_t node = MEM32(e + 0x94);
        if (!gva_ok(node, 0x100)) continue;
        Ma2Vec3 p = read_vec3(node + 0xF0);
        float best = 1e9f;
        for (int i = 0; i < w->nplayers; i++) { float d = hypotf(p.x - w->players[i].pos.x, p.z - w->players[i].pos.z); if (d < best) best = d; }
        if (best > 120.f || n >= 200) continue;
        n++;
        fprintf(stderr, "[PROP] %scls=%08X at %.1f,%.1f,%.1f near=%.0f f80=%08X f84=%08X clear=%.1f heavy=%.1f\n", is_kind(e, g_cls.turret) ? "TURRET " : "", MEM32(e + 0x28), p.x, p.y, p.z, best,
                MEM32(e + 0x80), MEM32(e + 0x84), ma2_trav_clear(p.x, p.z, 0.0), ma2_trav_clear_heavy(p.x, p.z));
    }
    fprintf(stderr, "[PROP] %d listed\n", n);
}

/* ---- Lanes ----
 * Arenas are laid out with a main route between the bases and flanking routes. Once a match (team
 * modes) the planner finds them: the route between the two teams' spawn areas, then the best
 * route that keeps clear of it (a wide corridor around the found route is made expensive, away
 * from the bases where every route converges), then again. A route whose apex stays close to the
 * main one is not a lane. The brain sends some bots of each team down each lane. */
static struct { double t; int built, n, na; Ma2Lane lane[3]; Ma2Vec3 a, b; int nchokes; Ma2Choke chokes[MA2_MAX_CHOKES]; int back; Ma2Lane lane_b[3]; } g_lanes;
/* Batch 3: a flank found from lane_a's spawn may only go that way (NRC +1: its east end is a ledge
 * over the east spawn; from below, the planner's way up is through the centre). Each flank is planned
 * again from lane_b's spawn with the other lanes' corridors made expensive; where that finds no
 * route on the same side well off the main one, the lane_b team has no lane there (side 9).
 * RECOMP_BOT_LANE_BACK=0 / [Bots] LaneBack=0 = one set of lanes for both teams. */
static int laneback_on(void) { return c7_on("laneback", "RECOMP_BOT_LANE_BACK", "LaneBack", 1); }
static void lane_store(Ma2Lane *ln, Ma2Vec3 a, const Ma2Path *p)
{
    int n = p->n + 1, step = (n + MA2_LANE_PTS - 1) / MA2_LANE_PTS, k = 0;
    if (step < 1) step = 1;
    ln->pt[k++] = a;
    ln->drop_lo = ln->drop_hi = -1;
    for (int i = step - 1; i < p->n && k < MA2_LANE_PTS; i += step) {
        for (int d = i - step + 1 < 0 ? 0 : i - step + 1; d <= i; d++)   /* a drop leg in this stretch: the lane is one-way past here */
            if (p->drop[d]) { if (ln->drop_lo < 0) ln->drop_lo = k; ln->drop_hi = k; }
        ln->pt[k++] = p->pt[i];
    }
    if (k < MA2_LANE_PTS && p->n > 0 && (k < 2 || hypotf(ln->pt[k - 1].x - p->pt[p->n - 1].x, ln->pt[k - 1].z - p->pt[p->n - 1].z) > 1.f)) ln->pt[k++] = p->pt[p->n - 1];
    ln->n = k;
}
static float lane_dist(const Ma2Lane *ln, Ma2Vec3 q)          /* distance from q to the lane's polyline */
{
    float best = 1e9f;
    for (int i = 0; i + 1 < ln->n; i++) {
        float ax = ln->pt[i].x, az = ln->pt[i].z, bx = ln->pt[i + 1].x - ax, bz = ln->pt[i + 1].z - az;
        float l2 = bx * bx + bz * bz, t = l2 > 1e-3f ? ((q.x - ax) * bx + (q.z - az) * bz) / l2 : 0.f;
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        float dx = q.x - (ax + bx * t), dz = q.z - (az + bz * t), d = sqrtf(dx * dx + dz * dz);
        if (d < best) best = d;
    }
    return best;
}
static int lane_corridor(Ma2PathDanger *dz, int room, const Ma2Lane *ln, Ma2Vec3 a, Ma2Vec3 b, float r, float len)
{
    int n = 0;
    for (int i = 0; i + 1 < ln->n && n < room; i++) {
        float sx = ln->pt[i + 1].x - ln->pt[i].x, sz = ln->pt[i + 1].z - ln->pt[i].z, sl = sqrtf(sx * sx + sz * sz);
        for (float s = 0.f; s < sl && n < room; s += 0.6f * r) {
            float x = ln->pt[i].x + sx * s / (sl + 1e-3f), z = ln->pt[i].z + sz * s / (sl + 1e-3f);
            if (hypotf(x - a.x, z - a.z) < 0.2f * len || hypotf(x - b.x, z - b.z) < 0.2f * len) continue;   /* the bases */
            dz[n].x = x; dz[n].z = z; dz[n].r = r; dz[n].cost = 10.f; n++;
        }
    }
    return n;
}
/* C5 chokes: walk each lane in 4 m steps and measure the ground across it, wall to wall, as
 * the planner sees it (props, steep ground -- valley walls -- and map edges). A stretch narrower than
 * CHOKE_W for CHOKE_LEN or more, away from the two spawn areas, is a choke. */
#define CHOKE_W   46.f   /* NRC (probe run t3a-c5d): valley exits 40-50 m wall to wall, open route 60-80 m */
#define CHOKE_LEN 30.f
static int chokes_on(void) { return c7_on("chokes", "RECOMP_BOT_CHOKES", "Chokes", 1); }
static float choke_width(float x, float z, float nx, float nz)
{
    float wd = 0.f;
    for (int s = -1; s <= 1; s += 2) {
        float d = 2.f;
        while (d < 40.f && ma2_trav_clear(x + nx * s * d, z + nz * s * d, 0.0) > 0.5f) d += 2.f;
        wd += d;
    }
    return wd;
}
static void chokes_scan(const Ma2Path *pa, int j, int side, Ma2Vec3 a, Ma2Vec3 b)
{
    {
        int in = 0; float run = 0.f, wmin = 99.f; Ma2Vec3 s0 = { 0, 0, 0 }, s1 = { 0, 0, 0 };
        Ma2Vec3 pt0 = a;
        for (int i = 0; i < pa->n; i++) {
            Ma2Vec3 q0 = i ? pa->pt[i - 1] : pt0, q1 = pa->pt[i];
            float sx = q1.x - q0.x, sz = q1.z - q0.z, sl = sqrtf(sx * sx + sz * sz);
            if (sl < 0.5f) continue;
            for (float t = 0.f; t < sl; t += 4.f) {
                Ma2Vec3 p = { q0.x + sx * t / sl, q0.y, q0.z + sz * t / sl };
                int base = hypotf(p.x - a.x, p.z - a.z) < 50.f || hypotf(p.x - b.x, p.z - b.z) < 50.f;   /* the spawn areas (NRC: the valley exits start ~150 m out) */
                /* a point the route only grazes (its centre not clear: a drop leg, a corner) says nothing */
                float wd = base || ma2_trav_clear(p.x, p.z, 0.0) < 1.5f ? 99.f : choke_width(p.x, p.z, sz / sl, -sx / sl);
                int last = i + 1 == pa->n && t + 4.f >= sl;
                if (env_on("RECOMP_BOT_CHOKE_PROFILE") && ((int)(t / 4.f)) % 5 == 0)   /* tuning aid: the width along the lane */
                    fprintf(stderr, "[BOT-CHOKEW] lane %+d at %.0f,%.0f centre %.1f width %.0f%s\n", side, p.x, p.z, ma2_trav_clear(p.x, p.z, 0.0),
                            base ? 99.f : choke_width(p.x, p.z, sz / sl, -sx / sl), base ? " (spawn)" : "");
                if (wd < (in ? CHOKE_W * 1.3f : CHOKE_W)) {
                    if (!in) { in = 1; run = 0.f; wmin = 99.f; s0 = p; }
                    else run += 4.f;
                    s1 = p; if (wd < wmin) wmin = wd;
                    if (!last) continue;
                }
                if (in && run >= CHOKE_LEN && g_lanes.nchokes < MA2_MAX_CHOKES) {
                    Ma2Vec3 m = { (s0.x + s1.x) * 0.5f, 0, (s0.z + s1.z) * 0.5f };
                    int dup = 0;
                    for (int k = 0; k < g_lanes.nchokes; k++) {      /* two lanes through one gap: one choke */
                        const Ma2Choke *o = &g_lanes.chokes[k];
                        if (hypotf(m.x - (o->a.x + o->b.x) * 0.5f, m.z - (o->a.z + o->b.z) * 0.5f) < 0.5f * hypotf(o->b.x - o->a.x, o->b.z - o->a.z) + 20.f) dup = 1;
                    }
                    if (!dup) {
                        Ma2Choke *c = &g_lanes.chokes[g_lanes.nchokes++];
                        c->a = s0; c->b = s1; c->width = wmin; c->lane = j;
                        if (env_on("RECOMP_BOT_DIAG"))
                            fprintf(stderr, "[BOT-CHOKE] %d: lane %+d from %.0f,%.0f to %.0f,%.0f (%.0f m long, %.0f m wide)\n", g_lanes.nchokes - 1, side,
                                    s0.x, s0.z, s1.x, s1.z, hypotf(s1.x - s0.x, s1.z - s0.z), wmin);
                    }
                }
                in = 0;
            }
        }
    }
}

/* W7 PathSlice (default on; [Bots] PathSlice=0 / RECOMP_BOT_PATH_SLICE=0 restores): A* plans are searched
 * RECOMP_BOT_PATH_SLICE_MS (default 6) ms per game step from a queue (ma2_pathq_*, ma2_bot_path.c) instead of 40-230 ms
 * in one frame. Used by the lane build (here) and the bots' route plans (ma2_bot_ctrl.c). */
static int pathslice_on(void) { return c7_on("pathslice", "RECOMP_BOT_PATH_SLICE", "PathSlice", 1); }
/* W7 PathSlice: the lane build (up to five A* plans) as a state machine. With the switch off every plan finishes at
 * once and the whole build runs in the call that started it, exactly as before; on, each plan is a queued search
 * (ma2_pathq_*) and the machine moves on when its result is in. */
enum { LJ_MAIN = 1, LJ_FLANK_REQ, LJ_FLANK, LJ_BACK_INIT, LJ_BACK_REQ, LJ_BACK, LJ_END };
static struct { int active, st, sliced, ticket, ret, nd, k, j, na, nb; Ma2Vec3 a, b; float len, r; Ma2Path p; Ma2PathParams pp;
                Ma2PathDanger dz[64]; LARGE_INTEGER qf, q0; } g_lj;
/* Starts a lane plan. 1 = its result is in g_lj.p / g_lj.ret already, 0 = queued (g_lj.ticket). */
static int lane_plan_req(Ma2Vec3 from, Ma2Vec3 to, const Ma2PathDanger *dz, int nd, double now)
{
    if (g_lj.sliced) {
        Ma2PathParams pp = g_lj.pp;
        pp.ndanger = 0;
        for (int i = 0; dz && i < nd && pp.ndanger < 64; i++) if (dz[i].r > 1.0f && dz[i].cost > 0.0f) pp.danger[pp.ndanger++] = dz[i];   /* as ma2_path_set_danger */
        int tk = ma2_pathq_submit(-2, from, to, 2.6f, NULL, 0, now, &pp);
        if (tk > 0) { g_lj.ticket = tk; return 0; }
        c7_add("pathslice", "queue_full", 1);
        ma2_path_set_params(&pp);
    } else ma2_path_set_danger(dz, nd);
    g_lj.ret = ma2_path_plan(&g_lj.p, from, to, 2.6f, NULL, 0, now);
    return 1;
}
static void lanes_build_run(Ma2World *w)
{
    Ma2Vec3 a = g_lj.a, b = g_lj.b; float r = g_lj.r, len = g_lj.len;
    for (;;) {
        if (g_lj.ticket) {
            int rr = ma2_pathq_take(-2, g_lj.ticket, &g_lj.p, &g_lj.ret);
            if (rr == 0) return;
            g_lj.ticket = 0;
            if (rr < 0) { memset(&g_lj.p, 0, sizeof g_lj.p); g_lj.ret = 0; }
        }
        Ma2Path *p = &g_lj.p;
        switch (g_lj.st) {
        case LJ_MAIN:
            if (!(g_lj.ret && p->ok)) { g_lj.st = LJ_END; break; }
            g_lanes.a = a; g_lanes.b = b; g_lanes.na = g_lj.na;
            lane_store(&g_lanes.lane[0], a, p); g_lanes.lane[0].side = 0; g_lanes.n = 1;
            g_lanes.nchokes = 0;
            if (chokes_on()) chokes_scan(p, 0, 0, a, b);
            g_lj.nd = lane_corridor(g_lj.dz, 64, &g_lanes.lane[0], a, b, r, len);
            g_lj.k = 0; g_lj.st = LJ_FLANK_REQ;
            break;
        case LJ_FLANK_REQ:
            if (!(g_lj.k < 2 && g_lj.nd > 0)) { g_lj.st = LJ_BACK_INIT; break; }
            g_lj.st = LJ_FLANK;
            if (!lane_plan_req(a, b, g_lj.dz, g_lj.nd, w->time)) return;
            break;
        case LJ_FLANK: {
            Ma2Lane ln; memset(&ln, 0, sizeof ln);
            if (!g_lj.ret || !p->ok) { g_lj.st = LJ_BACK_INIT; break; }
            lane_store(&ln, a, p);
            for (int i = 0; i < ln.n; i++) {
                float d = lane_dist(&g_lanes.lane[0], ln.pt[i]);
                if (d > ln.apex_off) { ln.apex_off = d; ln.apex = ln.pt[i]; }
            }
            if (ln.apex_off < 0.6f * r) { g_lj.st = LJ_BACK_INIT; break; }                 /* not a lane of its own */
            float cr = (b.x - a.x) * (ln.apex.z - a.z) - (b.z - a.z) * (ln.apex.x - a.x);
            ln.side = cr < 0.f ? 1 : -1;                                                    /* the brain's flank sign */
            int dup = 0;
            for (int j = 1; j < g_lanes.n; j++) if (g_lanes.lane[j].side == ln.side) dup = 1;
            if (!dup) { if (chokes_on()) chokes_scan(p, g_lanes.n, ln.side, a, b); g_lanes.lane[g_lanes.n++] = ln; }
            g_lj.nd += lane_corridor(g_lj.dz + g_lj.nd, 64 - g_lj.nd, &ln, a, b, r, len);
            g_lj.k++; g_lj.st = LJ_FLANK_REQ;
            break; }
        case LJ_BACK_INIT:
            if (laneback_on() && g_lanes.n > 1) {
                g_lanes.back = 1;
                g_lanes.lane_b[0] = g_lanes.lane[0];
                g_lj.j = 1; g_lj.st = LJ_BACK_REQ;
            } else g_lj.st = LJ_END;
            break;
        case LJ_BACK_REQ: {
            int j = g_lj.j, m = 0;
            if (j >= g_lanes.n) { g_lj.st = LJ_END; break; }
            Ma2Lane *lb = &g_lanes.lane_b[j];
            memset(lb, 0, sizeof *lb); lb->side = 9; lb->drop_lo = lb->drop_hi = -1;
            for (int k = 0; k < g_lanes.n; k++) if (k != j) m += lane_corridor(g_lj.dz + m, 64 - m, &g_lanes.lane[k], a, b, r, len);
            g_lj.st = LJ_BACK;
            if (!lane_plan_req(b, a, g_lj.dz, m, w->time)) return;
            break; }
        case LJ_BACK: {
            int j = g_lj.j; Ma2Lane *lb = &g_lanes.lane_b[j], ln;
            if (g_lj.ret && p->ok) {
                memset(&ln, 0, sizeof ln);
                lane_store(&ln, b, p);
                for (int i = 0; i < ln.n / 2; i++) { Ma2Vec3 t = ln.pt[i]; ln.pt[i] = ln.pt[ln.n - 1 - i]; ln.pt[ln.n - 1 - i] = t; }   /* pt[0] at lane_a's end */
                ln.drop_lo = ln.drop_hi = -1;                                               /* walked the way it was planned */
                for (int i = 0; i < ln.n; i++) {
                    float d = lane_dist(&g_lanes.lane[0], ln.pt[i]);
                    if (d > ln.apex_off) { ln.apex_off = d; ln.apex = ln.pt[i]; }
                }
                float cr = (b.x - a.x) * (ln.apex.z - a.z) - (b.z - a.z) * (ln.apex.x - a.x);
                ln.side = cr < 0.f ? 1 : -1;
                if (ln.apex_off >= 0.6f * r && ln.side == g_lanes.lane[j].side) *lb = ln;
            }
            c7_add("lanes", lb->side == 9 ? "back_none" : "back_found", 1);
            g_lj.j++; g_lj.st = LJ_BACK_REQ;
            break; }
        case LJ_END: {
            LARGE_INTEGER q1;
            if (!g_lj.sliced) ma2_path_set_danger(NULL, 0);
            if (g_lanes.n > 0 && chokes_on()) c7_add("chokes", "found", g_lanes.nchokes);
            QueryPerformanceCounter(&q1);
            if (g_lj.sliced) c7_max("pathslice", "lanes_wall_ms", (double)(q1.QuadPart - g_lj.q0.QuadPart) * 1000.0 / g_lj.qf.QuadPart);
            if (env_on("RECOMP_BOT_DIAG")) {
                int nb = g_lj.nb;
                fprintf(stderr, "[BOT-LANES] %d lane(s) from %.0f,%.0f to %.0f,%.0f (corridor %.0f m) in %.0f ms\n", g_lanes.n, a.x, a.z, b.x, b.z, r,
                        (double)(q1.QuadPart - g_lj.q0.QuadPart) * 1000.0 / g_lj.qf.QuadPart);
                for (int j = 0; j < g_lanes.n; j++) {
                    const Ma2Lane *ln = &g_lanes.lane[j];
                    float run = 0.f;
                    for (int i = 0; i + 1 < ln->n; i++) run += hypotf(ln->pt[i + 1].x - ln->pt[i].x, ln->pt[i + 1].z - ln->pt[i].z);
                    fprintf(stderr, "[BOT-LANES]   side %+d: %d points, apex %.0f,%.0f %.0f m off the main route, %.0f m long, drops at points %d..%d\n", ln->side, ln->n,
                            ln->apex.x, ln->apex.z, ln->apex_off, run, ln->drop_lo, ln->drop_hi);
                    for (int i = 0; i < ln->n; i++) fprintf(stderr, "[BOT-LANES]     side %+d pt %d %.0f,%.0f\n", ln->side, i, ln->pt[i].x, ln->pt[i].z);
                }
                for (int j = 1; g_lanes.back && j < g_lanes.n; j++) {
                    const Ma2Lane *ln = &g_lanes.lane_b[j];
                    if (ln->side == 9) { fprintf(stderr, "[BOT-LANES]   back side %+d: none (nation %d keeps to the others)\n", g_lanes.lane[j].side, nb); continue; }
                    fprintf(stderr, "[BOT-LANES]   back side %+d: %d points, apex %.0f,%.0f %.0f m off the main route\n", ln->side, ln->n, ln->apex.x, ln->apex.z, ln->apex_off);
                    for (int i = 0; i < ln->n; i++) fprintf(stderr, "[BOT-LANES]     back side %+d pt %d %.0f,%.0f\n", ln->side, i, ln->pt[i].x, ln->pt[i].z);
                }
            }
            g_lj.active = 0;
            return; }
        default: g_lj.active = 0; return;
        }
    }
}

static void lanes_update(Ma2World *w)
{
    if (env_on("RECOMP_BOT_DIAG")) {   /* once a match, 3 s in: who is on which side, as the game has it */
        static double logged = -1.0;
        if (w->time < logged) logged = -1.0;
        if (logged < 0.0 && w->time >= 3.0) {
            int per[16] = {0}; char hum[64] = "", sides[160] = ""; int o = 0, so = 0;
            logged = w->time;
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (p->nation >= 0 && p->nation < 16) per[p->nation]++;
                if (p->is_human && !p->is_bot) o += snprintf(hum + o, sizeof hum - (size_t)o, " %d", p->nation);
            }
            /* F22: every nation that has players (the two teams are not always nations 0 and 1) */
            for (int k = 0; k < 16; k++) if (per[k]) so += snprintf(sides + so, sizeof sides - (size_t)so, " nation%d=%d", k, per[k]);
            fprintf(stderr, "[BOT-TEAMS] t=%.1f players=%d%s human nation(s):%s\n", w->time, w->nplayers, sides, hum);
        }
    }
    if (w->time < g_lanes.t) { memset(&g_lanes, 0, sizeof g_lanes); ma2_pathq_cancel(-2); memset(&g_lj, 0, sizeof g_lj); }    /* a new match */
    g_lanes.t = w->time;
    if (!g_lanes.built && w->team_mode && w->time >= 1.0 && ma2_trav_ready() && w->nplayers >= 2) {
        int na = -1, nb = -1, ca = 0, cb = 0;
        Ma2Vec3 a = { 0, 0, 0 }, b = { 0, 0, 0 };
        g_lanes.built = 1;
        for (int i = 0; i < w->nplayers; i++) {
            const Ma2PlayerView *p = &w->players[i];
            if (!p->alive) continue;
            if (na < 0 || p->nation == na) { na = p->nation; a.x += p->pos.x; a.y += p->pos.y; a.z += p->pos.z; ca++; }
            else if (nb < 0 || p->nation == nb) { nb = p->nation; b.x += p->pos.x; b.y += p->pos.y; b.z += p->pos.z; cb++; }
        }
        if (ca && cb) {
            a.x /= ca; a.y /= ca; a.z /= ca; b.x /= cb; b.y /= cb; b.z /= cb;
            float len = hypotf(b.x - a.x, b.z - a.z), r = clampf(0.18f * len, 120.f, 250.f);
            memset(&g_lj, 0, sizeof g_lj);
            g_lj.a = a; g_lj.b = b; g_lj.na = na; g_lj.nb = nb; g_lj.len = len; g_lj.r = r;
            g_lj.sliced = pathslice_on();
            QueryPerformanceFrequency(&g_lj.qf); QueryPerformanceCounter(&g_lj.q0);
            if (g_lj.sliced) { ma2_path_get_params(&g_lj.pp); g_lj.pp.crush = 0; g_lj.pp.up = 0.f; }   /* W7: the same set_crush(0) / set_up(0) as below, on the snapshot */
            else { ma2_path_set_crush(0); ma2_path_set_danger(NULL, 0); ma2_path_set_up(0.f); }
            {   /* push-x3 E7 LanePin (RECOMP_BOT_LANE_PIN / LanePin, default ON): the lane plans took whatever drop parameters the last bot plan had left
                 * (0, 6, 10 or 60, or a previous match's), so whether a flank had one-way drops and whether SnapWide could rescue a centroid inside a
                 * building changed from launch to launch. Pinned to the 'Mech's: 60 m drops, normal cost, no marsh landings. */
                static int pin = -1;
                if (pin < 0) pin = c7_on("lanepin", "RECOMP_BOT_LANE_PIN", "LanePin", 1);
                if (pin) {
                    if (g_lj.sliced) { g_lj.pp.drop = 60.f; g_lj.pp.dropk = 1.f; g_lj.pp.drop_water = 0; }
                    else { ma2_path_set_drop(60.f); ma2_path_set_drop_pref(1.f, 0); }
                }
            }
            g_lj.active = 1; g_lj.st = LJ_MAIN;
            if (len > 300.f) lane_plan_req(a, b, NULL, 0, w->time);
            else g_lj.st = LJ_END;
        }
    }
    if (g_lj.active) lanes_build_run(w);
    if (g_lj.active) {   /* W7: lanes appear all at once, when the whole build is done (as before, just later) */
        w->nlanes = 0; w->lane_nation_a = -1; w->lanes_back = 0; w->nchokes = 0;
        return;
    }
    w->nlanes = g_lanes.n;
    memcpy(w->lanes, g_lanes.lane, sizeof g_lanes.lane);
    w->lane_a = g_lanes.a; w->lane_b = g_lanes.b;
    w->lane_nation_a = g_lanes.n > 0 ? g_lanes.na : -1;
    w->lanes_back = g_lanes.back;
    memcpy(w->lanes_b, g_lanes.lane_b, sizeof g_lanes.lane_b);
    w->nchokes = g_lanes.nchokes;
    memcpy(w->chokes, g_lanes.chokes, sizeof g_lanes.chokes);
}

/* Co-op modes (Grinder: no teams, every player on one nation) are fought
 * against the game's own AI waves -- vehicles with no MAPlayer parent. Those
 * that have moved since first seen (driven, not parked) and belong to another
 * nation join the world as combatants, so bots hunt, aim and hack them like
 * players. Identity is the actor (player = actor); nothing dereferences it. */
static struct { uint32_t actor; Ma2Vec3 first; int moved; } g_ai[MA2_BOT_MAX_VEHICLES];
static int g_nai;
static void add_ai_combatants(Ma2World *w)
{
    static double last_time;
    if (w->time < last_time) g_nai = 0;                  /* new match */
    last_time = w->time;
    if (w->team_mode || w->nplayers < 2) return;
    int nation = w->players[0].nation;
    for (int i = 1; i < w->nplayers; i++) if (w->players[i].nation != nation) return;   /* real FFA */
    for (int i = 0; i < w->nvehicles && w->nplayers < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2VehicleView *v = &w->vehicles[i];
        if (v->occupied || v->hp <= 0.f || v->kind == MA2_KIND_PILOT) continue;
        if (v->nation == nation || v->nation == 0 || v->nation == 6) continue;
        int k = 0;
        while (k < g_nai && g_ai[k].actor != v->actor) k++;
        if (k == g_nai) {
            if (g_nai >= MA2_BOT_MAX_VEHICLES) continue;
            g_ai[g_nai].actor = v->actor; g_ai[g_nai].first = v->pos; g_ai[g_nai].moved = 0; g_nai++;
        }
        if (!g_ai[k].moved) {
            float dx = v->pos.x - g_ai[k].first.x, dz = v->pos.z - g_ai[k].first.z;
            if (dx * dx + dz * dz < 16.f) continue;
            g_ai[k].moved = 1;
        }
        Ma2PlayerView *p = &w->players[w->nplayers++];
        memset(p, 0, sizeof(*p));
        p->player = v->actor; p->actor = v->actor; p->kind = v->kind; p->nation = v->nation;
        p->alive = 1; p->pos = v->pos; p->yaw = v->yaw; p->hp = v->hp; p->hp_max = v->hp_max;
    }
}

/* F28 (bots batch 2): bots see no further than the level draws. The far plane is the camera's
 * CameraFarPlane (template default 475 m, retail 150..4000): the camera builder sub_44BE0 scales
 * camera+0x80 by the PC DrawDistance setting only while it builds the projection and restores the
 * authored value after, so reading it between frames gives the authored distance (the DrawDistance
 * setting does not change bot behaviour). The first human's camera is his camera controller's
 * camera slot ([player+0x2F4]+0x14, a MACameraThird/First); the render camera handed to sub_44BE0
 * sits at +0x390 in it (smoke 1, NRC: Third 022E6870 -> render camera 022E6C00, near 1, far 450 =
 * the level's fog end 450). Checked every 2 s; a camera without a sane near/far pair keeps the last
 * good value (the 475 m default until one is seen).
 * RECOMP_BOT_F28=0 / ini [Bots] BotSightCap=0 = no cap. [C7] f28 far_m src_cam src_default. */
static float view_range_now(const Ma2World *w)
{
    static double last = -1.0; static float vfar = 475.0f;
    if (w->time < last) vfar = 475.0f;   /* a new match (another level) */
    if (w->time - last < 2.0 && w->time >= last) return vfar;
    last = w->time;
    const char *src = "src_default";
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->is_human || !gva_ok(p->player, 0x300)) continue;
        uint32_t ctl = MEM32(p->player + 0x2F4), cam = gva_ok(ctl, 0x20) ? MEM32(ctl + 0x14) : 0, rc = cam + 0x390u;
        if (gva_ok(cam, 0x400)) {
            float n = MEMF(rc + 0x7C), fr = MEMF(rc + 0x80);
            if (n > 0.01f && n < 50.0f && fr > 100.0f && fr < 10000.0f && fr > n) {
                if (env_on("RECOMP_BOT_DIAG") && fr != vfar)
                    fprintf(stderr, "[BOT-SIGHTCAP] t=%.1f camera %08X render camera %08X near %.2f far %.1f\n", w->time, cam, rc, n, fr);
                vfar = fr; src = "src_cam";
            }
        }
        break;
    }
    c7_add("f28", src, 1); c7_max("f28", "far_m", vfar);
    return vfar;
}

/* F39 (bots batch 2): the bots' radar is the game's own. Retail's radar range is HUD property
 * RadarDist on 'HUD: Local Object Radar' (vtable 0x484140): +0x18C, ctor default 30.0 (0x2038FF),
 * overwritten by the HUD data (sub_2057F5 store 0x205870); a blip is drawn only when the 3-D distance
 * is below it (sub_204133). Nothing points at the object from a known global, so it is found once per
 * match by scanning guest memory for the vtable (at most 6 scans 5 s apart, ~64 MB of reads each),
 * then re-checked every step. Not found / off: RECOMP_BOT_RADAR or the old 300 m.
 * RECOMP_BOT_F39=0 / ini [Bots] BotGameRadar=0 = the old 300 m. [C7] f39 radar_m found scans cands. */
#define VT_HUD_RADAR 0x484140u
static float radar_game_range(double now)
{
    static uint32_t obj; static double next, last = -1.0; static int scans;
    if (now < last) { obj = 0; next = 0.0; scans = 0; }   /* a new match */
    last = now;
    if (obj && MEM32(obj) == VT_HUD_RADAR) {
        float r = MEMF(obj + 0x18C);
        if (r >= 1.0f && r <= 5000.0f) return r;
    }
    obj = 0;
    if (now < next || scans >= 6) return 0.0f;
    next = now + 5.0; scans++;
    c7_add("f39", "scans", 1);
    {
        const uint32_t lo = 0x10000u, hi = 0x04000000u - 0x200u;
        const uint32_t *base = (const uint32_t *)(uintptr_t)(g_xbox_mem_offset + lo);
        uint32_t n = (hi - lo) / 4u, i, cands = 0;
        for (i = 0; i < n; i++) {
            if (base[i] != VT_HUD_RADAR) continue;
            uint32_t a = lo + i * 4u;
            float r = MEMF(a + 0x18C);
            uint32_t ent = MEM32(a + 0x194);
            cands++;
            if (!(r >= 1.0f && r <= 5000.0f) || !gva_ok(ent, 4)) continue;
            if (!obj) obj = a;
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-RADAR] t=%.1f radar object %08X RadarDist=%.1f entity=%08X\n", now, a, r, ent);
        }
        c7_max("f39", "cands", cands);
        if (!obj) { if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-RADAR] t=%.1f scan %d: no radar object (%u vtable hits)\n", now, scans, cands); return 0.0f; }
        c7_add("f39", "found", 1);
        return MEMF(obj + 0x18C);
    }
}

/* Pilot bombs on the ground (batch 3; wiki pilot-bomb-*): a player's DropPackManager (avatar+0x254) counts its
 * packs out in the byte +0x7C (1 once placed; +0x7D.. are other data) and +0x84 is the pack. A
 * bomb is that count turning non-zero (watched per player from its first sight) and lies where its pilot stood
 * then (placed packs have no scene node). It is gone when the count drops back to 0; once its owner has boarded
 * or died and the manager is out of reach, it is kept 60 s. */
int ma2_bot_bomb_avoid_on(void) { return c7_on("bombavoid", "RECOMP_BOT_BOMB_AVOID", "BombAvoid", 1); }
/* A bot's own planting (ctrl_pilot_bomb) reports its pack placed / gone: the watch above does not reach the
 * bots' managers in a match's first seconds, when most of them plant. */
void ma2_bot_bomb_note(uint32_t player, uint32_t ent, int nation, Ma2Vec3 pos, int placed)
{
    Ma2World *w = &g_rt.world;
    if (!ma2_bot_bomb_avoid_on()) return;
    for (int k = 0; k < w->nbombs; k++) if (w->bombs[k].ent == ent) { w->bombs[k] = w->bombs[--w->nbombs]; k--; }
    if (!placed || w->nbombs >= 16) return;
    int k = w->nbombs++;
    w->bombs[k].ent = ent; w->bombs[k].owner = player; w->bombs[k].nation = nation; w->bombs[k].pos = pos; w->bombs[k].since = w->time; w->bombs[k].fuse = 0.0;
    w->bomb_epoch++;
    c7_add("bombavoid", "seen", 1);
}
/* Death blasts: every 'Mech (any side, human or bot) whose health has just reached 0. Measured 11-01 over 269 test
 * matches ([DEATH] pairs): units near a 'Mech that goes down die 2.2 s after its health reaches 0 (609 of the deaths
 * within 60 m fall at +2.2/+2.3 s; 120-400 m apart the timing is flat), out to 45-50 m (deaths thin out past 45 m).
 * So the hazard is 45 m and lasts the 2.2 s fuse plus 0.4 s; it was 35 m for 6 s, which stopped bots fleeing at
 * 41 m and kept them running for 4 s after the blast had gone off. A chain is a new blast of its own. */
#define DEATH_BLAST_S 2.6
#define DEATH_BLAST_R 45.f
static void blasts_update(Ma2World *w)
{
    static double last_t;
    static struct { uint32_t actor; float hp; } last[MA2_WORLD_MAX_PLAYERS]; static int nlast;
    if (w->time < last_t) { w->nblasts = 0; nlast = 0; }          /* a new match */
    last_t = w->time;
    /* BlastLinger (default on, RECOMP_BOT_BLAST_LINGER / [Bots] BlastLinger): "down" is hp <= 0.5 %, but a big 'Mech can sit there:
     * au16_61 (CTF Deliverance) b0, a 6000-hp 'Mech, lingered at 11 hp for 3.4 s, so the 2.6 s window closed before it blew; BlastWary
     * let go and two teammates (1883 / 2977 hp) walked into the blast at 141.1. While that 'Mech is still a 'Mech with hp > 0 its window
     * is held open (t0 = now); back over 2 % (repaired) it is no blast. [C7] deathblast: lingered_s. */
    if (c7_on("blastlinger", "RECOMP_BOT_BLAST_LINGER", "BlastLinger", 1))
        for (int k = 0; k < w->nblasts; k++)
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (p->actor != w->blasts[k].actor || p->kind != MA2_KIND_MECH) continue;
                float f = p->hp_max > 0.f ? p->hp / p->hp_max : p->hp;
                if (f > 0.02f) { w->blasts[k].t0 = -1e9; c7_add("deathblast", "recovered", 1); }
                else if (p->hp > 0.f) { w->blasts[k].t0 = w->time; w->blasts[k].pos = p->pos; c7_add("deathblast", "lingered_s", 1.f / 60.f); }
                break;
            }
    /* EjectBlast (default on, RECOMP_BOT_EJECT_BLAST / [Bots] EjectBlast): a 'Mech that goes down while its pilot is
     * ejecting blows 4.5-5.0 s after 0 hp, not 2.2 s (film 11-08: A_90a 48.3 -> 53.4, c13_70a 65.1 -> 70.1, A_74b, A_85b,
     * c13_72a x2); the 2.6 s window had closed, BlastWary let go and the defenders walked back in (A_90a b9 dead, b10 -50 %).
     * The sign is the pilot: an alive pilot within 8 m of the hull in the first 2 s. That hull's window runs 5.8 s.
     * [C7] ejectblast: late. */
    static uint32_t late_act[16]; static int nlate;
    if (w->time < 1.0) nlate = 0;
    int eb_on = c7_on("ejectblast", "RECOMP_BOT_EJECT_BLAST", "EjectBlast", 1);
    for (int k = 0; k < w->nblasts && eb_on; k++) {
        int is_late = 0;
        for (int j = 0; j < nlate; j++) is_late |= late_act[j] == w->blasts[k].actor;
        if (is_late || w->time - w->blasts[k].t0 > 2.0) continue;
        for (int i = 0; i < w->nplayers; i++) {
            const Ma2PlayerView *p = &w->players[i];
            if (p->kind != MA2_KIND_PILOT || !p->alive || hypotf(p->pos.x - w->blasts[k].pos.x, p->pos.z - w->blasts[k].pos.z) > 8.f) continue;
            late_act[nlate++ & 15] = w->blasts[k].actor; if (nlate > 16) nlate = 16;
            c7_add("ejectblast", "late", 1);
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-BLAST] t=%.1f mech %08X: its pilot is out beside it, blast held to 5.8 s\n", w->time, w->blasts[k].actor);
            break;
        }
    }
    for (int k = 0; k < w->nblasts; k++) {
        double life = DEATH_BLAST_S;
        for (int j = 0; j < nlate && eb_on; j++) if (late_act[j] == w->blasts[k].actor) life = 5.8;
        if (w->time - w->blasts[k].t0 > life) { w->blasts[k] = w->blasts[--w->nblasts]; k--; }
    }
    if (!c7_on("deathblast", "RECOMP_BOT_DEATH_BLAST", "DeathBlastFlee", 1)) { w->nblasts = 0; return; }
    int n = 0; static struct { uint32_t actor; float hp; } now[MA2_WORLD_MAX_PLAYERS];
    for (int i = 0; i < w->nplayers && n < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->actor || p->kind != MA2_KIND_MECH) continue;
        float hp = p->hp_max > 0.f ? p->hp / p->hp_max : p->hp;
        now[n].actor = p->actor; now[n].hp = hp; n++;
        if (hp > 0.005f) continue;
        float was = -1.f;
        for (int j = 0; j < nlast; j++) if (last[j].actor == p->actor) was = last[j].hp;
        if (was <= 0.005f) continue;                                   /* not just now (or not seen alive) */
        int dup = 0;
        for (int k = 0; k < w->nblasts; k++) dup |= w->blasts[k].actor == p->actor;
        if (dup || w->nblasts >= 8) continue;
        int k = w->nblasts++;
        w->blasts[k].actor = p->actor; w->blasts[k].pos = p->pos; w->blasts[k].t0 = w->time; w->blasts[k].r = DEATH_BLAST_R;
        c7_add("deathblast", "mechs_down", 1);
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-BLAST] t=%.1f mech %08X down at %.0f,%.0f nation=%d: %.0f m blast for %.1f s\n",
                                               w->time, p->actor, p->pos.x, p->pos.z, p->nation, (double)DEATH_BLAST_R, DEATH_BLAST_S);
    }
    for (int i = 0; i < n; i++) { last[i].actor = now[i].actor; last[i].hp = now[i].hp; }
    nlast = n;
    /* Test hook RECOMP_TEST_FAKE_BLAST=<s>: every <s> seconds a blast (nothing goes off) 18 m ahead of the next bot in a
     * 'Mech or BattleArmor, along its motion -- the hard case, it has to turn round -- so one match gives dozens of escapes
     * to measure (blastmon.py / fleespeed.py). */
    {   static double fb_next; static int fb_i, fb_n; const char *fbs = getenv("RECOMP_TEST_FAKE_BLAST"); double per = fbs ? atof(fbs) : 0.0;
        if (per > 0.0 && w->time < fb_next - per - 1.0) fb_next = 0.0;
        if (per > 0.0 && w->time >= fb_next && w->time > 20.0 && w->nblasts < 8) {
            fb_next = w->time + per;
            for (int tries = 0; tries < w->nplayers; tries++) {
                const Ma2PlayerView *p = &w->players[fb_i++ % (w->nplayers ? w->nplayers : 1)];
                if (!p->is_bot || !p->alive || p->riding || (p->kind != MA2_KIND_MECH && p->kind != MA2_KIND_BATTLEARMOR)) continue;
                float vl = hypotf(p->vel.x, p->vel.z), ux = vl > 1.f ? p->vel.x / vl : 1.f, uz = vl > 1.f ? p->vel.z / vl : 0.f;
                int k = w->nblasts++;
                w->blasts[k].actor = 0x7E570000u + (uint32_t)(fb_n++ & 0xFFFF); w->blasts[k].pos = p->pos;
                w->blasts[k].pos.x += ux * 18.f; w->blasts[k].pos.z += uz * 18.f; w->blasts[k].t0 = w->time; w->blasts[k].r = DEATH_BLAST_R;
                fprintf(stderr, "[BOT-BLAST] t=%.1f mech %08X down at %.0f,%.0f nation=%d: %.0f m blast for %.1f s (test, 18 m ahead of %08X)\n",
                        w->time, w->blasts[k].actor, w->blasts[k].pos.x, w->blasts[k].pos.z, 0, (double)DEATH_BLAST_R, DEATH_BLAST_S, p->actor);
                break;
            }
        }
    }
}

static void bombs_update(Ma2World *w)
{
    static double last_t;
    static struct { uint32_t player, out; } seen[MA2_WORLD_MAX_PLAYERS + MAX_BOTS]; static int nseen;
    if (w->time < last_t) { w->nbombs = 0; nseen = 0; }           /* a new match */
    last_t = w->time;
    if (!ma2_bot_bomb_avoid_on()) { w->nbombs = 0; return; }
    int diag = env_on("RECOMP_BOT_DIAG");
    uint32_t reach[MA2_WORLD_MAX_PLAYERS + MAX_BOTS], out_of[MA2_WORLD_MAX_PLAYERS + MAX_BOTS]; int nr = 0;
    /* Every player in the view, and our own bots by their player (a bot pilot's manager is not reached
     * through the view in its first seconds, when most plant). */
    Ma2PlayerView extra[MAX_BOTS]; int nx = 0;
    for (int k = 0; k < g_rt.nbots && nx < MAX_BOTS; k++) {
        uint32_t pl = g_rt.bots[k].player; int in = 0;
        if (!pl) continue;
        for (int i = 0; i < w->nplayers && !in; i++) in = w->players[i].player == pl;
        if (in) continue;
        memset(&extra[nx], 0, sizeof extra[nx]); extra[nx].player = pl; extra[nx].is_bot = 1;
        uint32_t a = player_actor(pl);
        extra[nx].pos = a ? actor_pos(a) : extra[nx].pos; extra[nx].nation = a ? actor_int(a, 0xA8, 0x000EB091u, -1) : -1;
        nx++;
    }
    for (int i = 0; i < w->nplayers + nx; i++) {
        const Ma2PlayerView *p = i < w->nplayers ? &w->players[i] : &extra[i - w->nplayers];
        uint32_t beh = gva_ok(p->player, 0x300) ? MEM32(p->player + 0x2F0) : 0, av = gva_ok(beh, 0x10) ? MEM32(beh + 0xC) : 0;
        uint32_t mgr = gva_ok(av, 0x270) ? MEM32(av + 0x254) : 0;
        if (!gva_ok(mgr, 0xA8) || !gva_ok(MEM32(mgr), 0x20)) continue;
        uint32_t out = MEM8(mgr + 0x7C) ? (MEM32(mgr + 0x84) ? MEM32(mgr + 0x84) : mgr) : 0;   /* +0x7C = packs out (a BYTE: +0x7D.. are other data, nonzero for some players); the pack is +0x84 */
        if (diag) {   /* every change of a manager's +0x7C */
            static uint32_t dp[MA2_WORLD_MAX_PLAYERS], dv[MA2_WORLD_MAX_PLAYERS]; static int dn;
            int q; for (q = 0; q < dn && dp[q] != p->player; q++) {}
            if (q == dn && dn < MA2_WORLD_MAX_PLAYERS) { dp[dn++] = p->player; dv[q] = ~0u; }
            if (q < dn && dv[q] != MEM32(mgr + 0x7C)) { dv[q] = MEM32(mgr + 0x7C); fprintf(stderr, "[BOMB-MGR] t=%.1f player=%08X bot=%d mgr=%08X 7C=%08X 80=%u 84=%08X\n", w->time, p->player, p->is_bot, mgr, dv[q], MEM32(mgr + 0x80), MEM32(mgr + 0x84)); }
        }
        reach[nr] = p->player; out_of[nr++] = out;
        int s; for (s = 0; s < nseen && seen[s].player != p->player; s++) {}
        if (s == nseen) { if (nseen < MA2_WORLD_MAX_PLAYERS + MAX_BOTS) { seen[nseen].player = p->player; seen[nseen].out = out; nseen++; } continue; }
        uint32_t was = seen[s].out; seen[s].out = out;
        if (!out || out == was) continue;
        for (int k = 0; k < w->nbombs; k++) if (w->bombs[k].ent == out) { w->bombs[k] = w->bombs[--w->nbombs]; k--; }   /* the pack moved on */
        if (w->nbombs >= 16) continue;
        int k = w->nbombs++;
        w->bombs[k].ent = out; w->bombs[k].owner = p->player; w->bombs[k].nation = p->nation;
        w->bombs[k].pos = p->pos; w->bombs[k].since = w->time; w->bombs[k].fuse = 0.0; w->bomb_epoch++;
        c7_add("bombavoid", "seen", 1);
        if (diag) fprintf(stderr, "[BOMB] t=%.1f new %08X owner=%08X nation=%d at %.0f,%.0f,%.0f\n", w->time, out, p->player, p->nation, p->pos.x, p->pos.y, p->pos.z);
    }
    for (int k = 0; k < w->nbombs; k++) {   /* the fuse: a unit that sets it off (FFA: anyone but its owner; teams: another nation) inside 15 m */
        if (w->bombs[k].fuse > 0.0) continue;
        for (int i = 0; i < w->nplayers; i++) {
            const Ma2PlayerView *p = &w->players[i];
            if (!p->alive || !p->actor || p->player == w->bombs[k].owner || (w->team_mode && p->nation == w->bombs[k].nation)) continue;
            float d = hypotf(p->pos.x - w->bombs[k].pos.x, p->pos.z - w->bombs[k].pos.z);
            if (d < 15.f + (p->kind == MA2_KIND_MECH || p->kind == MA2_KIND_TANK ? 3.f : 0.f)) {
                w->bombs[k].fuse = w->time;
                if (diag) fprintf(stderr, "[BOMB] t=%.1f fuse %08X (%.1fs old) by %08X kind=%d at %.1f m\n", w->time, w->bombs[k].ent, w->time - w->bombs[k].since, p->player, p->kind, d);
                break;
            }
        }
    }
    for (int k = 0; k < w->nbombs; k++) {
        int owned = 0, gone = 0;
        for (int r = 0; r < nr; r++) if (reach[r] == w->bombs[k].owner) { owned = 1; gone = out_of[r] != w->bombs[k].ent; }   /* went off (or was cleared) */
        if (!owned) gone = w->time - w->bombs[k].since > 60.0;
        if (!gone) continue;
        if (diag) fprintf(stderr, "[BOMB] t=%.1f gone %08X owner=%08X after %.1fs%s\n", w->time, w->bombs[k].ent, w->bombs[k].owner, w->time - w->bombs[k].since, owned ? "" : " (owner out of reach)");
        w->bombs[k] = w->bombs[--w->nbombs]; k--;
    }
}

/* W2b: RECOMP_HITCH per-stage timing of world_update ([HITCH-W], stages >= 5 ms) */
#define HW_DECL static int hw_on = -1; static LARGE_INTEGER hw_f, hw_t;
#define HW_START do { if (hw_on < 0) { hw_on = env_int("RECOMP_HITCH", 0) > 0; QueryPerformanceFrequency(&hw_f); } if (hw_on) QueryPerformanceCounter(&hw_t); } while (0)
#define HW_MARK(name) do { if (hw_on > 0) { LARGE_INTEGER hw_n; QueryPerformanceCounter(&hw_n); double hw_ms = (double)(hw_n.QuadPart - hw_t.QuadPart) * 1000.0 / (double)hw_f.QuadPart; if (hw_ms >= 5.0) fprintf(stderr, "[HITCH-W] t=%+.3fs %s=%.1fms\n", nv2a_hitch_rel_s(), name, hw_ms); hw_t = hw_n; } } while (0)
static void world_update(void)
{
    uint32_t step = MEM32(VA_STEP_TIME);
    HW_DECL
    if (g_rt.world_valid && g_rt.world_step == step) return;
    Ma2World *w = &g_rt.world;
    HW_START; classes_refresh(); HW_MARK("classes_refresh");
    w->time = MEMD(VA_GAME_TIME);
    w->dt = (float)MEMD(VA_STEP_DT);
    w->frame = step;
    w->game_type = g_rt.game_type;
    w->team_mode = g_rt.team_mode;
    w->radar_range = (float)env_int("RECOMP_BOT_RADAR", 300);
    if (!getenv("RECOMP_BOT_RADAR") && c7_on("f39", "RECOMP_BOT_F39", "BotGameRadar", 1)) {   /* F39: the game's RadarDist */
        float r = radar_game_range(w->time);
        if (r > 0.0f) { w->radar_range = r; c7_max("f39", "radar_m", r); }
    }
    uint32_t b = MEM32(VA_PLAYERS_BEGIN), e = MEM32(VA_PLAYERS_END);
    w->nplayers = 0;
    if (e > b && e - b <= 4 * 16 && gva_ok(b, e - b)) {
        for (uint32_t p = b; p < e && w->nplayers < MA2_BOT_MAX_PLAYERS; p += 4) {
            uint32_t pl = MEM32(p);
            if (!gva_ok(pl, 0x4B0) || MEM32(pl + 0x28) != g_cls.player) continue;
            build_player(&w->players[w->nplayers], pl, w->nplayers, w->time);
            w->nplayers++;
        }
    }
    if (env_on("RECOMP_BOT_SCOREDUMP")) {
        static double last;
        if (w->time - last >= 10.0 || w->time < last) {
            last = w->time;
            for (uint32_t p = b; p < e && e > b && e - b <= 64; p += 4) {
                uint32_t pl = MEM32(p);
                if (!gva_ok(pl, 0x4B0) || MEM32(pl + 0x28) != g_cls.player) continue;
                fprintf(stderr, "[SCORE] t=%.0f pl=%08X port=%d:", w->time, pl, (int)MEM32(pl + 0x2AC));
                for (uint32_t o = 0x2B0; o < 0x400; o += 4) { uint32_t v = MEM32(pl + o); if (v && v < 100000) fprintf(stderr, " %X=%u", o, v); }
                fprintf(stderr, "\n");
            }
        }
    }
    if (!g_rt.scan_valid || w->time - g_rt.scan_time >= 0.1 || w->time < g_rt.scan_time) {
        scan_entities(w);
        roofs_step();
        if (g_tur_dirty) {
            g_tur_dirty = 0;
            if (commit_slice_on()) c7_add("turretbatch", "sliced", 1);      /* ft-bots: the cells are marked dirty; the wreck pump commits them in slices */
            else { ma2_trav_commit(); c7_add("turretbatch", "commits", 1); }
        }
        g_rt.scan_time = w->time; g_rt.scan_valid = 1;
    }
    HW_MARK("players+scan");
    diag_props(w);
    diag_near(w);
    diag_surf(w);
    if (ma2_pathq_pending()) {   /* W7 PathSlice: queued A* plans, a few ms of search this step */
        double pt0 = c7_t0();
        /* PlanCatchUp ([Bots] PlanCatchUp / RECOMP_BOT_PLAN_CATCHUP, default on): a slow CPU (a power-limited laptop at 1.8 GHz)
         * does far less search per ms, and the queue backs up -- plans waited 1.3 s there, bots stood waiting for a route.
         * While the oldest unfinished plan has waited 250 ms or more, the step gets twice the slice; fast machines never reach it. */
        double slice = (double)env_int("RECOMP_BOT_PATH_SLICE_MS", 6);
        if (c7_on("plancatchup", "RECOMP_BOT_PLAN_CATCHUP", "PlanCatchUp", 1) && ma2_pathq_oldest_ms() >= 250.0) {
            slice *= 2.0; c7_add("plancatchup", "steps", 1);
        }
        g_perf_plan_ms += ma2_pathq_pump(slice);
        c7_ms("pathslice", "pump", pt0);
        { unsigned pj, pr, ps; double pm, px; ma2_pathq_stats(&pj, &pr, &ps, &pm, &px);
          c7_max("pathslice", "jobs", pj); c7_max("pathslice", "restarts", pr); c7_max("pathslice", "job_ms_max", px); }
        HW_MARK("pathq");
    }
    lanes_update(w); HW_MARK("lanes");
    wreck_update(w->time); HW_MARK("wreck");
    /* Vehicle claims: a bot walking to board a vehicle reserves it for its team (FFA: for itself). */
    for (int i = 0; i < w->nvehicles; i++) { w->vehicles[i].claimed_by = 0; w->vehicles[i].claim_nation = -1; }
    HW_MARK("diag"); human_mech_intent(w); HW_MARK("human_mech_intent");   /* first: a human teammate's claim beats the bots' (C3) */
    if (ma2_bot_switch_on(MA2_SW_VEH_ASSIGN)) vehicle_assign(w);
    else for (int k = 0; k < g_rt.nbots; k++) {
        const Bot *bk = &g_rt.bots[k];
        if (!bk->player || bk->order.type != MA2_ORD_GET_VEHICLE || !bk->order.vehicle) continue;
        for (int i = 0; i < w->nvehicles; i++)
            if (w->vehicles[i].actor == bk->order.vehicle && !w->vehicles[i].claimed_by) {
                w->vehicles[i].claimed_by = bk->player;
                for (int j = 0; j < w->nplayers; j++) if (w->players[j].player == bk->player) w->vehicles[i].claim_nation = w->players[j].nation;
            }
    }
    HW_MARK("vehicle_assign");
    avoid_track_feed(w); HW_MARK("avoid_feed");
    add_ai_combatants(w); HW_MARK("add_ai");
    objective_update(w); HW_MARK("objective");
    spc_gate_diag(w);
    w->view_range = c7_on("f28", "RECOMP_BOT_F28", "BotSightCap", 1) ? view_range_now(w) : 0.0f;
    /* Probe actor for sub_1760E1: any bot actor that is an MAMover with its
     * physics component back-linked (phys+0x148 == actor). */
    g_probe.objects = !env_on("RECOMP_BOT_PROBE_TERRAIN_ONLY");
    g_probe.actor = g_probe.phys = g_probe.vt = 0; g_probe.offset = 0.f;
    /* a 'Mech or tank first: a pilot on foot may be gone (boarded) before the frame is out */
    for (int pass = 0; pass < 2 && !g_probe.actor; pass++)
        for (int i = 0; i < w->nplayers; i++) {
            uint32_t a = w->players[i].actor;
            if (!w->players[i].is_bot || !a || !is_kind(a, g_cls.mover)) continue;
            if (pass == 0 && w->players[i].kind != MA2_KIND_MECH && w->players[i].kind != MA2_KIND_TANK) continue;
            uint32_t phys = MEM32(a + 0x8C);
            if (!gva_ok(phys, 0x170) || MEM32(phys + 0x148) != a || !gva_ok(MEM32(phys), 4)) continue;
            g_probe.actor = a; g_probe.phys = phys; g_probe.vt = MEM32(a); g_probe.offset = MEMF(a + 0xD8);
            break;
        }
    HW_MARK("probe");
    g_rt.world_step = step; g_rt.world_valid = 1;
}


/* RECOMP_BOT_RAYTEST: validate ray_cast against the vertical ground probe and print horizontal hits. */
static void ray_selftest(void)
{
    static double next_t = 0;
    const Ma2World *w = &g_rt.world;
    if (!env_on("RECOMP_BOT_RAYTEST") || w->time < next_t || !g_probe.phys) return;
    next_t = w->time + 8.0;
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    int n = 0, agree = 0, shown = 0;
    for (int i = 0; i < w->nplayers && shown < 2; i++) {
        const Ma2PlayerView *pv = &w->players[i];
        if (!pv->actor) continue;
        shown++;
        /* vertical comparison at 6 points around the player */
        for (int k = 0; k < 6; k++) {
            Ma2Vec3 o = { pv->pos.x + 15.f * cosf(k * 1.047f), pv->pos.y + 60.f, pv->pos.z + 15.f * sinf(k * 1.047f) };
            float t = 0; uint32_t obj = 0; int ok = 0;
            Ma2Vec3 dn = { 0, -1, 0 };
            int hit = ray_cast(&g_probe, o, dn, 200.f, 1, 1, &t, &obj);
            float gy = ground_fn(&g_probe, o.x, o.y, o.z, &ok);
            fprintf(stderr, "[RAYTEST] vert pl=%d k=%d hit=%d t=%.2f y_ray=%.2f ground_fn=%.2f obj=%08X cls=%08X\n", i, k, hit, t, o.y - t, gy, obj, obj ? MEM32(obj + 0x28) : 0);
            n++; if (hit && fabsf((o.y - t) - gy) < 1.0f) agree++;
        }
        for (int k = 0; k < 8; k++) {
            float ang = k * 0.7854f;
            Ma2Vec3 o = { pv->pos.x, pv->pos.y + 3.f, pv->pos.z }, d = { sinf(ang), 0, cosf(ang) };
            float t = 0; uint32_t obj = 0;
            int h11 = ray_cast(&g_probe, o, d, 80.f, 1, 1, &t, &obj);
            float t2 = 0; uint32_t o2 = 0;
            int h01 = ray_cast(&g_probe, o, d, 80.f, 0, 1, &t2, &o2);
            float t3 = 0; uint32_t o3 = 0;
            int h10 = ray_cast(&g_probe, o, d, 80.f, 1, 0, &t3, &o3);
            fprintf(stderr, "[RAYTEST] horiz pl=%d ang=%d pos=%.0f,%.0f,%.0f h11=%d t=%.1f cls=%08X | h01=%d t=%.1f cls=%08X | h10=%d t=%.1f cls=%08X\n", i, k,
                    pv->pos.x, pv->pos.y, pv->pos.z, h11, t, obj ? MEM32(obj + 0x28) : 0, h01, t2, o2 ? MEM32(o2 + 0x28) : 0, h10, t3, o3 ? MEM32(o3 + 0x28) : 0);
        }
    }
    {   static int edumped = 0;
        if (!edumped && w->time > 5.0) {
            edumped = 1;
            FILE *df = fopen("entities.txt", "w");
            uint32_t game = MEM32(VA_GAME);
            uint32_t r1 = gva_ok(game, 0xA8) ? MEM32(game + 0xA4) : 0;
            uint32_t root = gva_ok(r1, 8) ? MEM32(r1 + 4) : 0;
            if (df && gva_ok(root, 0x30)) {
                uint32_t stack[1024]; int sp = 0, visited = 0;
                stack[sp++] = root;
                while (sp > 0 && visited < 20000) {
                    uint32_t e = stack[--sp]; visited++;
                    uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
                    if (gva_ok(sib, 0x30) && sp < 1000) stack[sp++] = sib;
                    if (gva_ok(child, 0x30) && sp < 1000) stack[sp++] = child;
                    uint32_t node = MEM32(e + 0x94);
                    Ma2Vec3 pp = {0, 0, 0};
                    if (gva_ok(node, 0x100)) pp = read_vec3(node + 0xF0);
                    if ((MEM32(e + 0x28) == 0xCA77C1A6u || MEM32(e + 0x28) == 0xC3E3D036u) && (pp.x != 0.f || pp.z != 0.f)) {
                        static int nt = 0;
                        if (nt++ < 40) {
                            float t1 = 0, t2 = 0; uint32_t o1 = 0, o2 = 0;
                            Ma2Vec3 up = { pp.x, pp.y + 40.f, pp.z }, dn = { 0, -1, 0 };
                            Ma2Vec3 sd = { pp.x - 12.f, pp.y + 2.f, pp.z }, ex = { 1, 0, 0 };
                            int h1 = ray_cast(&g_probe, up, dn, 80.f, 1, 1, &t1, &o1);
                            int h2 = ray_cast(&g_probe, sd, ex, 24.f, 1, 1, &t2, &o2);
                            fprintf(stderr, "[RAYTEST] ent %08X cls=%08X pos=%.1f,%.1f,%.1f vert hit=%d t=%.1f obj=%08X/%08X horiz hit=%d t=%.1f obj=%08X/%08X phys=%08X\n", e, MEM32(e + 0x28), pp.x, pp.y, pp.z, h1, t1, o1, o1 ? MEM32(o1 + 0x28) : 0, h2, t2, o2, o2 ? MEM32(o2 + 0x28) : 0, MEM32(e + 0x8C));
                        }
                    }
                    fprintf(df, "%08X cls=%08X vt=%08X parent=%08X node=%08X pos=%.1f,%.1f,%.1f phys=%08X\n", e, MEM32(e + 0x28), MEM32(e), MEM32(e + 4), node, pp.x, pp.y, pp.z, MEM32(e + 0x8C));
                }
            }
            if (df) fclose(df);
        }
    }
    {   static int dumped = 0;
        if (!dumped) {
            dumped = 1;
            uint32_t t = MEM32(VA_TERRAIN);
            int tw = (int)MEM32(t + 4), td = (int)MEM32(t + 8); float cell = MEMF(t + 0x814);
            int nx = (int)((tw - 1) * cell / 4.f), nz = (int)((td - 1) * cell / 4.f);
            char name[128]; snprintf(name, sizeof name, "terrain_%08X.bin", terrain_fingerprint());
            FILE *df = fopen(name, "wb");
            if (df) {
                int hdr[2] = { nx, nz }; fwrite(hdr, 4, 2, df);
                for (int zi = 0; zi < nz; zi++)
                    for (int xi = 0; xi < nx; xi++) {
                        int ok; float h = ground_fn(&g_probe, xi * 4.f, 600.f, zi * 4.f, &ok);
                        float th = terrain_height(xi * 4.f, zi * 4.f, &ok); if (!ok) th = -999.f;
                        float v[2] = { ok ? h : -999.f, th }; fwrite(v, 4, 2, df);
                    }
                fclose(df);
            }
            fprintf(stderr, "[RAYTEST] terrain dump %s %dx%d\n", name, nx, nz);
        }
    }
    for (int i = 0; i < w->nplayers && i < 3; i++)
        for (int j = 0; j < w->nplayers && j < 4; j++) {
            if (i == j || !w->players[i].actor || !w->players[j].actor) continue;
            Ma2Vec3 o = w->players[i].pos, tg = w->players[j].pos, d;
            o.y += 3.f; tg.y += 3.f;
            d.x = tg.x - o.x; d.y = tg.y - o.y; d.z = tg.z - o.z;
            float L = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            if (L < 1.f) continue;
            d.x /= L; d.y /= L; d.z /= L;
            for (int fl = 0; fl < 4; fl++) {
                float t = 0; uint32_t ob = 0;
                int h = ray_cast(&g_probe, o, d, L + 20.f, fl & 1, (fl >> 1) & 1, &t, &ob);
                fprintf(stderr, "[RAYTEST] pair %d->%d flags=%d/%d dist=%.1f hit=%d t=%.1f obj=%08X cls=%08X actors=%08X/%08X\n", i, j, fl & 1, (fl >> 1) & 1, L, h, t, ob, ob ? MEM32(ob + 0x28) : 0, w->players[i].actor, w->players[j].actor);
            }
        }
    {   /* scatter test: ray vs object sweep over a 300 m square around the first player */
        static const float cen[6][2] = {{468,1393},{700,700},{300,300},{1000,1000},{500,1000},{800,1500}};
        for (int ci = 0; ci < 6; ci++) {
        int nobj = 0, nray = 0, both = 0, shown2 = 0;
        for (int k = 0; k < 600; k++) {
            float rx = cen[ci][0] + (((k * 7919) % 601) - 300) * 0.5f, rz = cen[ci][1] + (((k * 104729) % 601) - 300) * 0.5f;
            int tok = 0; float th = terrain_height(rx, rz, &tok);
            int ok = 0; float gy = ground_fn(&g_probe, rx, th + 80.f, rz, &ok);
            int obj_hit = tok && gy > th + 0.5f;
            Ma2Vec3 o = { rx, th + 80.f, rz }, dn = { 0, -1, 0 };
            float t = 0; uint32_t ob = 0;
            int rh = ray_cast(&g_probe, o, dn, 200.f, 1, 1, &t, &ob);
            nobj += obj_hit; nray += rh; both += (obj_hit && rh);
            if ((obj_hit || rh) && shown2++ < 12)
                fprintf(stderr, "[RAYTEST] scatter (%.0f,%.0f) terrain=%.2f ground_fn=%.2f ray_hit=%d ray_y=%.2f cls=%08X\n", rx, rz, th, gy, rh, o.y - t, ob ? MEM32(ob + 0x28) : 0);
        }
        fprintf(stderr, "[RAYTEST] scatter c%d objects(sweep)=%d rayhits=%d both=%d\n", ci, nobj, nray, both);
        }
    }
    QueryPerformanceCounter(&a);
    { Ma2Vec3 o = { 500, 100, 500 }, d = { 0.6f, 0, 0.8f }; for (int k = 0; k < 2000; k++) { float t; uint32_t ob; o.x += 0.01f; ray_cast(&g_probe, o, d, 150.f, 1, 1, &t, &ob); } }
    QueryPerformanceCounter(&b);
    fprintf(stderr, "[RAYTEST] vertical agree %d/%d; horizontal ray cost %.2f us\n", agree, n, (double)(b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / 2000.0);
    QueryPerformanceCounter(&a);
    { for (int k = 0; k < 2000; k++) { int ok; ground_fn(&g_probe, 500.f + k * 0.01f, 150.f, 500.f, &ok); } }
    QueryPerformanceCounter(&b);
    fprintf(stderr, "[RAYTEST] ground_fn cost %.2f us\n", (double)(b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / 2000.0);
}

/* ------------------------------------------------------------------ */
/* Bot tick                                                            */
/* ------------------------------------------------------------------ */
static const char *kind_name(int k)
{
    static const char *const n[] = { "NONE", "PILOT", "MECH", "BATTLEARMOR", "TANK", "VTOL", "FLYER", "MOVER", "TURRET" };
    return k >= 0 && k <= MA2_KIND_TURRET ? n[k] : "NONE";
}

static float hdist2(Ma2Vec3 a, Ma2Vec3 b) { return hypotf(a.x - b.x, a.z - b.z); }
static float maxf2(float a, float b) { return a > b ? a : b; }
static float minf2(float a, float b) { return a < b ? a : b; }
static float kind_eye(int k)
{
    switch (k) { case MA2_KIND_MECH: return 7.0f; case MA2_KIND_TANK: return 3.0f; case MA2_KIND_BATTLEARMOR: return 2.5f;
                 case MA2_KIND_PILOT: return 1.7f; case MA2_KIND_TURRET: return 3.0f; default: return 2.5f; }
}

/* ---- The AI's own line of sight: sub_17AD5C ----
 * cdecl(self, target, 3, Entity **hit) with edi = &from, ebx = &to; al = 1 when
 * clear. A ray through the game collision space (terrain, buildings, props,
 * vehicles) ignoring `self` and `target` (LOCOMOTION_SPEC.md section 4). */
static int ai_los_impl(uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to, uint32_t *hit);
static int ai_los(uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to, uint32_t *hit)
{
    g_perf_los++; double t0 = perf_now_ms();
    int r = ai_los_impl(self, target, from, to, hit);
    g_perf_los_ms += perf_now_ms() - t0;
    return r;
}
static int ai_los_impl(uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to, uint32_t *hit)
{
    recomp_func_t f = resolve(0x0017AD5Cu);
    if (!f) return -1;
    GuestRegs r; regs_save(&r);
    g_esp -= 64; uint32_t v = g_esp & ~15u;
    MEMF(v) = from.x; MEMF(v + 4) = from.y; MEMF(v + 8) = from.z;
    MEMF(v + 16) = to.x; MEMF(v + 20) = to.y; MEMF(v + 24) = to.z; MEM32(v + 32) = 0;
    g_esp = v - 16; g_edi = v; g_ebx = v + 16;
    PUSH32(g_esp, v + 32); PUSH32(g_esp, 3u); PUSH32(g_esp, target); PUSH32(g_esp, self);
    PUSH32(g_esp, 0x00185068u);
    f();
    int clear = (int)(g_eax & 0xFF);
    if (hit) *hit = MEM32(v + 32);
    regs_restore(&r);
    return clear;
}

static void bot_diag(int index, Bot *b, const Ma2World *w, int self)
{
    DWORD now = GetTickCount();
    if (now - b->diag_tick < 1000) return;
    b->diag_tick = now;
    const Ma2PlayerView *v = &w->players[self];
    const BotCtrl *c = &b->ctrl;
    char brain[256] = "";
    if (b->brain) ma2_brain_describe(b->brain, brain, sizeof(brain));
    static const char *const mn[] = { "idle", "move", "stop", "attack", "board" };
    static const char *const on[] = { "idle", "getveh", "travel", "eject", "engage", "fight", "hack" };
    fprintf(stderr, "[BOT] t=%.2f bot=%d port=%u team=%d veh=%s pos=%.2f,%.2f,%.2f hp=%.2f qte=%02X:%.2f clamp=%d ctl=%s ord=%s dest=%.0f,%.0f path=%d/%d eng=%d tgt=%08X los=%d %s heat=%.0f\n",
            w->time, index, b->port, b->team, kind_name(v->kind), v->pos.x, v->pos.y, v->pos.z,
            v->hp_max > 0.f ? v->hp / v->hp_max : 0.f, v->qte_required, v->qte_progress,
            v->clamped_to || v->enemy_rider ? 1 : 0, mn[c->mode % 5], on[b->order.type % 7], c->dest.x, c->dest.z,
            c->path.next, c->have_path ? c->path.n : 0, c->engaged, c->tgt, c->fl_clear, brain, v->heat);
    if (v->kind == MA2_KIND_BATTLEARMOR && v->clamped_to) {
        uint32_t a = v->actor, t = v->clamped_to;
        fprintf(stderr, "[BOT-LATCH] bot=%d actor=%08X q274=%08X tgt=%08X tnation=%d mynation=%d\n",
                index, a, MEM32(a + 0x274), t, actor_int(t, 0xA8, 0x000EB091u, -2), v->nation);
    }
}

/* Bot Skill through the vehicle's own campaign-AI tuning (MAMover values designers
 * set per enemy): +0x1F8 AccuracyRadius (random miss offset), +0x218 FireDelay.
 * Scaled once per vehicle from its authored values. RECOMP_BOT_SKILL_TUNE=0 leaves them. */
typedef struct { uint32_t actor, used; float radius, delay, set_radius, set_delay; int k; } MoverTune;
static MoverTune g_tune[48];
static uint32_t g_tune_clock;
/* F2: the table used to live across matches and fall back to a hash slot when full, so a
 * re-entered actor re-read its already-scaled values as authored and compounded (radius 2e38,
 * FireDelay so long a bot stopped firing). Now: cleared per match, LRU eviction that puts the
 * evicted hull's authored values back first, and a known actor never re-reads a field it set.
 * RECOMP_BOT_F2_TUNE=0 / ini [Bots] SkillTuneFix=0 = the old behaviour. [C7] F2 counters. */
static void skill_tune_level_reset(void)
{
    if (c7_on("F2", "RECOMP_BOT_F2_TUNE", "SkillTuneFix", 1)) { memset(g_tune, 0, sizeof g_tune); g_tune_clock = 0; }
}
static void skill_tune_mover(uint32_t a, int skill)
{
    static const float rad[4] = { 2.00f, 1.35f, 1.00f, 0.70f };
    static const float del[4] = { 1.60f, 1.20f, 1.00f, 0.80f };
    if (!env_int("RECOMP_BOT_SKILL_TUNE", 1) || !gva_ok(a, 0x220)) return;
    int f2 = c7_on("F2", "RECOMP_BOT_F2_TUNE", "SkillTuneFix", 1);
    int k = skill < 0 ? 0 : skill > 3 ? 3 : skill, slot = -1, ours = 0, found = 0, used = 0;
    for (int i = 0; i < 48; i++) {
        if (g_tune[i].actor) used++;
        if (g_tune[i].actor == a) {
            ours = MEMF(a + 0x1F8) == g_tune[i].set_radius && MEMF(a + 0x218) == g_tune[i].set_delay;
            g_tune[i].used = ++g_tune_clock;
            if (ours && g_tune[i].k == k) return;
            slot = i; found = 1; break;
        }
        if (slot < 0 && !g_tune[i].actor) slot = i;
    }
    c7_max("F2", "tune_slots_used", used);
    if (slot < 0 && !f2) slot = (int)(a >> 4) % 48;
    if (slot < 0) {                       /* full: evict the least recently used, restoring its hull */
        slot = 0;
        for (int i = 1; i < 48; i++) if (g_tune[i].used < g_tune[slot].used) slot = i;
        MoverTune *e = &g_tune[slot];
        if (gva_ok(e->actor, 0x220) && MEMF(e->actor + 0x1F8) == e->set_radius && MEMF(e->actor + 0x218) == e->set_delay) {
            MEMF(e->actor + 0x1F8) = e->radius; MEMF(e->actor + 0x218) = e->delay;
        }
        memset(e, 0, sizeof *e);
        c7_add("F2", "evictions", 1);
    }
    MoverTune *t = &g_tune[slot];
    if (!f2) {
        if (!ours) { t->radius = MEMF(a + 0x1F8); t->delay = MEMF(a + 0x218); }
    } else if (!found) {
        t->radius = MEMF(a + 0x1F8); t->delay = MEMF(a + 0x218);
    } else {                              /* known actor: re-read only a field the game itself rewrote */
        if (MEMF(a + 0x1F8) != t->set_radius) { t->radius = MEMF(a + 0x1F8); c7_add("F2", "rereads", 1); }
        if (MEMF(a + 0x218) != t->set_delay) { t->delay = MEMF(a + 0x218); c7_add("F2", "rereads", 1); }
    }
    t->actor = a; t->k = k; t->used = ++g_tune_clock;
    t->set_radius = t->radius * rad[k]; t->set_delay = t->delay * del[k];
    MEMF(a + 0x1F8) = t->set_radius; MEMF(a + 0x218) = t->set_delay;
    c7_add("F2", "retunes", 1);
    if (env_on("RECOMP_BOT_DIAG"))
        fprintf(stderr, "[BOT-SKILL] actor=%08X skill=%d radius=%.2f->%.2f delay=%.2f->%.2f engage=%.0f\n",
                a, k, t->radius, t->set_radius, t->delay, t->set_delay, MEMF(a + 0x200));
}

/* ---- Who is attacking whom, who is firing (Ma2PlayerView.attacking / .firing) ----
 * What another player could tell: a bot's controller target while it fights or shoots; the
 * game AI's target slot; the human's enemy in his sights (within ~12 degrees of his heading,
 * 320 m) and his trigger (weapon commands 44..47 held on his control behaviour). */
static int human_trigger(uint32_t player)
{
    uint32_t beh = MEM32(player + 0x2F0);
    if (!gva_ok(beh, 0x6F0)) return 0;
    for (uint32_t id = 44; id <= 47; id++) if (MEMF(beh + 0x18 + id * 0x1C + 4) != 0.f) return 1;
    return 0;
}
static void combat_state_update(Ma2World *w)
{
    for (int i = 0; i < w->nplayers; i++) {
        Ma2PlayerView *p = &w->players[i];
        p->attacking = 0; p->firing = 0;
        if (!p->alive || !p->actor) continue;
        if (p->is_bot) {
            int k = bot_index_of(p->player);
            if (k >= 0) {
                const BotCtrl *c = &g_rt.bots[k].ctrl;
                int shooting = c->last_fire_t > 0.0 && w->time >= c->last_fire_t && w->time - c->last_fire_t < 1.0;
                if ((c->engaged || shooting) && c->tgt) p->attacking = c->tgt;
                p->firing = shooting;
            }
        } else if (p->player == p->actor) {               /* the game's AI (Grinder waves) */
            int ok = 0; uint32_t ai = vcall0(p->actor, 0x200, 0x000E6CA1u, &ok);
            if (ok && gva_ok(ai, 0xC0) && MEM8(ai + 0x90) && MEM32(ai + 0xB4)) { p->attacking = MEM32(ai + 0xB4); p->firing = 1; }
        } else {                                          /* a human */
            float best = 1e9f;
            for (int j = 0; j < w->nplayers; j++) {
                const Ma2PlayerView *e = &w->players[j];
                if (j == i || !e->alive || !e->actor || e->nation == p->nation) continue;
                float dx = e->pos.x - p->pos.x, dz = e->pos.z - p->pos.z, d = hypotf(dx, dz);
                if (d > 320.f || d < 0.5f) continue;
                float cs = (sinf(p->yaw) * dx + cosf(p->yaw) * dz) / d;
                if (cs > 0.978f && d < best) { best = d; p->attacking = e->actor; }
            }
            p->firing = human_trigger(p->player);
        }
    }
}

/* ---- Damage events for the bots' senses ----
 * Every health drop of a player: dealt -> the bots shooting at it (their controller target,
 * engaged or fired within 1.5 s); received by a bot -> the enemies attacking it, else the
 * nearest enemy that is firing (what a player under fire can tell). */
static void ev_push(int *n, Ma2DmgEvent *ev, uint32_t actor, float amount)
{
    for (int i = 0; i < *n; i++) if (ev[i].actor == actor) { ev[i].amount += amount; return; }
    if (*n < MA2_SENSE_MAX_EVENTS) { ev[*n].actor = actor; ev[*n].amount = amount; (*n)++; }
}
/* F73 (damage credit was guessed: every health drop of a victim was split among all bots targeting
 * it that were engaged or fired in the last 1.5 s, the human's or another enemy's damage included,
 * which inflated out_v, the "finish" commitment and the [BOT-SUM] dmg metric). Now only bots that
 * fired at it within 0.5 s with a clear line share it, and a non-bot enemy attacking and firing at
 * the victim takes its own share. RECOMP_BOT_F73=0 / ini [Bots] BotDamageCredit=0 = off. [C7] F73. */
static int f73_on(void) { return c7_on("F73", "RECOMP_BOT_F73", "BotDamageCredit", 1); }
static void damage_track(void)
{
    static uint32_t pa[MA2_WORLD_MAX_PLAYERS]; static float ph[MA2_WORLD_MAX_PLAYERS]; static int pal[MA2_WORLD_MAX_PLAYERS];
    static double last = -1.0;
    const Ma2World *w = &g_rt.world;
    if (w->time == last) return;
    if (w->time < last) memset(pa, 0, sizeof pa);
    last = w->time;
    for (int i = 0; i < w->nplayers && i < MA2_WORLD_MAX_PLAYERS; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (pal[i] && pa[i] == p->actor && p->hp < ph[i] - 0.01f) {
            float dmg = ph[i] - p->hp;
            int who[MAX_BOTS], nd = 0, others = 0, f73 = f73_on();
            for (int k = 0; k < g_rt.nbots; k++) {
                const BotCtrl *c = &g_rt.bots[k].ctrl;
                if (!g_rt.bots[k].player || c->tgt != p->actor) continue;
                if (f73) {
                    /* fired at it within 0.5 s with the shot's line to it clear (ctrl_shot_los cache) */
                    if (c->last_fire_t > 0.0 && w->time >= c->last_fire_t && w->time - c->last_fire_t < 0.5 && c->fl_tgt == p->actor && c->fl_clear) who[nd++] = k;
                } else if (c->engaged || (c->last_fire_t > 0.0 && w->time - c->last_fire_t < 1.5)) who[nd++] = k;
            }
            if (f73) {
                /* a non-bot enemy of the victim attacking and firing at it takes its own share */
                for (int j = 0; j < w->nplayers; j++) {
                    const Ma2PlayerView *e = &w->players[j];
                    if (j == i || e->is_bot || !e->alive || !e->actor || e->nation == p->nation) continue;
                    if (e->firing && e->attacking == p->actor) others++;
                }
                c7_add("F73", nd ? (others ? "split_with_others" : "bots_only") : (others ? "others_only" : "nobody"), 1);
                if (nd > 1) c7_add("F73", "multi_bot", 1);
                c7_add("F73", "dmg_total", dmg);
            }
            for (int n = 0; n < nd; n++) {
                Bot *bk = &g_rt.bots[who[n]];
                float share = dmg / (float)(nd + others);
                ev_push(&bk->nout, bk->ev_out, p->actor, share);
                bk->ctrl.dmg += share; if (p->is_human) bk->ctrl.dmg_human += share;
                if (f73) c7_add("F73", "dmg_credited", share);
                if (env_on("RECOMP_BOT_DIAG"))
                    fprintf(stderr, "[BOT-DMG] t=%.1f bot=%d victim=%d%s kind=%s dmg=%.0f hp=%.0f/%.0f split=%d+%d\n", w->time, who[n], i, p->is_human ? "H" : (p->is_bot ? "B" : "A"),
                            kind_name(p->kind), share, p->hp, p->hp_max, nd, others);
            }
            int vb = p->is_bot ? bot_index_of(p->player) : -1;
            if (vb >= 0) {
                int src[MA2_WORLD_MAX_PLAYERS], ns = 0, closest = -1; float nd2 = 1e9f;
                for (int j = 0; j < w->nplayers; j++) {
                    const Ma2PlayerView *e = &w->players[j];
                    if (j == i || !e->alive || !e->actor || e->nation == p->nation) continue;
                    if (e->attacking == p->actor) src[ns++] = j;
                    float d = hypotf(e->pos.x - p->pos.x, e->pos.z - p->pos.z);
                    if (e->firing && d < 320.f && d < nd2) { nd2 = d; closest = j; }
                }
                if (!ns && closest >= 0) src[ns++] = closest;
                for (int n = 0; n < ns; n++) ev_push(&g_rt.bots[vb].nin, g_rt.bots[vb].ev_in, w->players[src[n]].actor, dmg / (float)ns);
            }
        }
        pa[i] = p->actor; ph[i] = p->hp; pal[i] = p->alive;
    }
}

/* ---- Senses (Ma2Senses callbacks for the brain) ---- */
static int sn_los(void *ctx, uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to) { (void)ctx; return ai_los(self, target, from, to, NULL); }
/* Push-s3 (Level): a MABuilding as the bots see it: position (scene node), hit points cur / max (the getters objective_update uses). */
static int bld_view(uint32_t e, Ma2BldView *o)
{
    static uint32_t cls;
    if (!cls) cls = class_id_of("MABuilding");
    if (!e || !gva_ok(e, 0x240) || MEM32(e + 0x28) != cls) return 0;
    uint32_t node = MEM32(e + 0x94);
    o->entity = e; o->cls = cls;
    o->pos = gva_ok(node, 0x100) ? read_vec3(node + 0xF0) : read_vec3(e + 0x4C0);
    o->hp = (float)actor_int(e, 0x74, 0x000C9476u, 0); o->hp_max = (float)actor_int(e, 0x6C, 0x000C8C2Au, 0);
    o->nation = actor_int(e, 0xA8, 0x000EB091u, -1);
    return 1;
}
static int sn_bld_get(void *ctx, uint32_t ent, Ma2BldView *out) { (void)ctx; return bld_view(ent, out); }
static int sn_bld_ray(void *ctx, uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to, Ma2BldView *out)
{
    uint32_t hit = 0; (void)ctx;
    if (ai_los(self, target, from, to, &hit) == 1) return 0;
    if (bld_view(hit, out)) return 1;
    out->entity = hit; out->cls = gva_ok(hit, 0x30) ? MEM32(hit + 0x28) : 0;
    return 0;
}
static float sn_clear(void *ctx, float x, float z) { (void)ctx; return ma2_trav_ready() ? ma2_trav_clear(x, z, g_rt.world.time) : 99.f; }
static float sn_ground(void *ctx, float x, float z, int *ok) { (void)ctx; return terrain_height(x, z, ok); }
/* Top of the highest static collider at (x, z) -- roof, ledge, rock -- or the terrain. */
static float surface_height(float x, float z, int *ok)
{
    int okg = 0;
    float g = terrain_height(x, z, &okg), t = 0.f;
    uint32_t ob = 0;
    if (ok) *ok = okg;
    if (!okg || !g_probe.phys) return g;
    Ma2Vec3 o = { x, g + 60.f, z }, dn = { 0.f, -1.f, 0.f };
    if (!ray_cast(&g_probe, o, dn, 64.f, 1, 1, &t, &ob)) return g;
    uint32_t owner = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0;
    if (owner && (is_kind(owner, g_cls.mover) || is_kind(owner, g_cls.projectile) || (g_cls.water && MEM32(owner + 0x28) == g_cls.water))) return g;
    return g + 60.f - t > g ? g + 60.f - t : g;
}
static float sn_surface(void *ctx, float x, float z, int *ok) { (void)ctx; return surface_height(x, z, ok); }
/* diag: what lies straight below a point -- distance and the hit collider's owner and class */
void ma2_bot_ray_below(Ma2Vec3 from, float len, float *t_out, uint32_t *owner_out, uint32_t *cls_out)
{
    float t = -1.f; uint32_t ob = 0, owner = 0;
    Ma2Vec3 dn = { 0.f, -1.f, 0.f };
    *t_out = -1.f; *owner_out = 0; *cls_out = 0;
    if (!g_probe.phys || !ray_cast(&g_probe, from, dn, len, 1, 1, &t, &ob)) return;
    owner = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0;
    *t_out = t; *owner_out = owner; *cls_out = gva_ok(owner, 0x30) ? MEM32(owner + 0x28) : 0;
}

/* patch_ai4.py (sub_1837B3): the AI selector of a local AI body returns the
 * controller's engaged target, or nobody. */
int ma2_bot_ai_select(uint32_t ai, uint32_t *out)
{
    if (g_rt.nbots <= 0 || !gva_ok(ai, 0x30)) return 0;
    uint32_t v = MEM32(ai + 0x20);
    if (!gva_ok(v, 0x300) || !ma2_bot_actor_owned(v)) return 0;
    int index = bot_index_of(MEM32(v + 4));
    if (index < 0) return 0;
    const BotCtrl *c = &g_rt.bots[index].ctrl;
    uint32_t t = 0;
    if (c->engaged && c->tgt && c->actor == v)
        for (int i = 0; i < g_rt.world.nplayers; i++) if (g_rt.world.players[i].actor == c->tgt && g_rt.world.players[i].alive) t = c->tgt;
    *out = t;
    return 1;
}

#include "ma2_bot_ctrl.c"     /* BotController */
#include "ma2_bot_rec.c"      /* bot recorder: everything, every frame (.botrec) */
#include "ma2_bot_player.c"   /* BotPlayer creation */

/* RECOMP_BOT_DIAG: human health every 4 s plus every drop ([HUMAN-HIT]) with the nearest enemy and its kind. */
static void human_hp_watch(void)
{
    static double last_log, prev_hp = -1.0; static uint32_t prev_actor; static int deaths;
    const Ma2World *w = &g_rt.world;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *h = &w->players[i];
        if (!h->is_human || h->is_bot) continue;
        double now = w->time;
        if (h->actor == prev_actor && prev_hp > 0.0 && h->hp < prev_hp - 0.5) {
            int bi = -1; float bd = 1e9f;
            for (int j = 0; j < w->nplayers; j++) {
                const Ma2PlayerView *e = &w->players[j];
                if (j == i || !e->actor || !e->alive || (w->team_mode && e->nation == h->nation)) continue;
                float d = hypotf(e->pos.x - h->pos.x, e->pos.z - h->pos.z);
                if (d < bd) bd = d, bi = j;
            }
            fprintf(stderr, "[HUMAN-HIT] t=%.1f hp=%.0f->%.0f/%.0f kind=%d nearest_enemy_kind=%d d=%.0f\n", now, prev_hp, h->hp, h->hp_max, h->kind, bi >= 0 ? w->players[bi].kind : -1, bd);
        }
        if (prev_hp > 0.0 && h->hp <= 0.0) { deaths++; fprintf(stderr, "[HUMAN-DEATH] t=%.1f n=%d kind=%d\n", now, deaths, h->kind); }
        prev_hp = h->hp; prev_actor = h->actor;
        if (now - last_log >= 4.0) { last_log = now; fprintf(stderr, "[HUMAN-HP] t=%.1f hp=%.0f/%.0f kind=%d pos=%.0f,%.0f deaths=%d\n", now, h->hp, h->hp_max, h->kind, h->pos.x, h->pos.z, deaths); }
        break;
    }
}

/* RECOMP_BOT_DIAG: deaths ([DEATH]), game stats changes ([STAT]) and damage dealt by
 * engaged bots ([BOT-DMG]: a target's health drop while a bot's attack state has it). */
static void death_watch(void)
{
    static struct { uint32_t player, actor; int alive, kind, nation, sinit; Ma2Vec3 pos; double dtime; float hp; int stat[0x1C]; } st[MA2_WORLD_MAX_PLAYERS];
    static double last_frame_time = -1.0;
    const Ma2World *w = &g_rt.world;
    if (w->time == last_frame_time) return;
    last_frame_time = w->time;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (st[i].player != p->player) { memset(&st[i], 0, sizeof(st[i])); st[i].player = p->player; }
        if (st[i].alive && st[i].actor == p->actor && p->hp < st[i].hp - 0.01f) {   /* the killing hit too */
            float dmg = st[i].hp - p->hp;
            if (env_on("RECOMP_BOT_DMGSCAN")) {   /* RE probe: which actor/player fields name the shooter? */
                static int nscan;
                if (nscan++ < 60) {
                    fprintf(stderr, "[DMG-SCAN] t=%.2f victim=%d actor=%08X dmg=%.0f:", w->time, i, p->actor, dmg);
                    for (uint32_t o = 0; o < 0x900; o += 4) {
                        uint32_t v = MEM32(p->actor + o);
                        if (!v) continue;
                        for (int j = 0; j < w->nplayers; j++) {
                            const Ma2PlayerView *q = &w->players[j];
                            if (j == i || !q->actor) continue;
                            if (v == q->actor) fprintf(stderr, " +%03X=A%d", o, j);
                            else if (v == q->player && q->player != q->actor) fprintf(stderr, " +%03X=P%d", o, j);
                            else if (gva_ok(q->actor, 0x30) && v == MEM32(q->actor + 0x2C)) fprintf(stderr, " +%03X=AU%d", o, j);
                            else if (q->player != q->actor && gva_ok(q->player, 0x30) && v == MEM32(q->player + 0x2C)) fprintf(stderr, " +%03X=PU%d", o, j);
                        }
                    }
                    fprintf(stderr, " | engaged-by:");
                    for (int k = 0; k < g_rt.nbots; k++) if (g_rt.bots[k].ctrl.engaged && g_rt.bots[k].ctrl.tgt == p->actor)
                        for (int j = 0; j < w->nplayers; j++) if (w->players[j].player == g_rt.bots[k].player) fprintf(stderr, " %d", j);
                    fprintf(stderr, "\n");
                }
            }
            (void)dmg;
        }
        if (st[i].alive && !p->alive) {
            st[i].dtime = w->time;
            fprintf(stderr, "[DEATH] t=%.1f who=%d%s team=%d kind=%s pos=%.0f,%.0f,%.0f friends:", w->time, i, p->is_human ? "H" : "B", st[i].nation, kind_name(st[i].kind), st[i].pos.x, st[i].pos.y, st[i].pos.z);
            for (int j = 0; j < w->nplayers; j++) if (j != i && w->players[j].alive) {
                const Ma2PlayerView *q = &w->players[j];
                float d = hypotf(q->pos.x - st[i].pos.x, q->pos.z - st[i].pos.z);
                if (d < 80.f) fprintf(stderr, " [%d%s %s t%d d%.0f]", j, q->is_human ? "H" : "B", kind_name(q->kind), q->nation, d);
            }
            fprintf(stderr, "\n");
        }
        if (p->player && p->player != p->actor && gva_ok(p->player, 0x4B0)) {
            for (int id = 0; id < 0x1C; id++) {
                int v = player_stat(p->player, id);
                if (st[i].sinit && v != st[i].stat[id] && id != 16 && id != 7 && id != 25)
                    fprintf(stderr, "[STAT] t=%.1f who=%d%s team=%d kind=%s id=%d %d->%d\n", w->time, i, p->is_human ? "H" : "B", p->nation, kind_name(p->kind), id, st[i].stat[id], v);
                st[i].stat[id] = v;
            }
            st[i].sinit = 1;
        }
        if (p->alive) { st[i].pos = p->pos; st[i].kind = p->kind; st[i].nation = p->nation; }
        st[i].alive = p->alive; st[i].actor = p->actor; st[i].hp = p->hp;
    }
}

/* RECOMP_BOT_DIAG: proof there are no bot cameras. Every 5 s: camera entities
 * (MACamera family, class MEM32(0x4EAFC0) or MACameraThird vtable 0x46D348) under each
 * player, the player's camera link +0x90, and the views the scene drew ([BOT-VIEWS]). */
static unsigned g_views_drawn, g_views_bot, g_views_frames, g_views_max, g_views_this; static uint32_t g_views_step;
/* Always on, logged only on change: which camera the local players see through. A player once
 * found the view off them for good (flying passes over the map, through respawns) with the HUD
 * and controls still theirs; this names the camera class and target when that happens. */
static const char *cam_vt_name(uint32_t vt)
{
    switch (vt) {
    case 0x0046D348u: return "Third"; case 0x0046D208u: return "First"; case 0x0046D4E0u: return "Spiral";
    case 0x0046D630u: return "Free"; case 0x0046D738u: return "Pan"; case 0x0046D110u: return "FreeCamera";
    case 0x00475DF8u: return "Lock"; case 0x0046CF08u: return "Camera"; default: return "?";
    }
}
static int g_cam_logs;
static void cam_watch_view(uint32_t view)
{
    if (!c7_log_on()) return;   /* W-09b: logging only (g_cam_logs is its only state) */
    /* the scene draws up to a couple of views a frame: log a view only when it is new (not seen
     * in the last few seconds) */
    static uint32_t seen[6]; static DWORD seen_t[6];
    DWORD now = GetTickCount(); int old = 0;
    for (int i = 0; i < 6; i++) {
        if (seen[i] == view) { seen_t[i] = now; return; }
        if (seen_t[i] < seen_t[old]) old = i;
    }
    for (int i = 0; i < 6; i++) if (seen[i] && now - seen_t[i] > 4000) { seen[i] = 0; seen_t[i] = 0; old = i; }
    seen[old] = view; seen_t[old] = now;
    if (g_cam_logs > 400) return;
    uint32_t c = view > 0x200u ? view - 0x200u : 0;
    uint32_t vt = gva_ok(c, 0x80) ? MEM32(c) : 0;
    g_cam_logs++;
    fprintf(stderr, "[CAM-WATCH] t=%.1f view=%08X vvt=%08X cam=%08X %s owner=%08X target=%08X\n", g_rt.world.time, view, gva_ok(view, 4) ? MEM32(view) : 0,
            c, cam_vt_name(vt), gva_ok(c, 0x80) ? MEM32(c + 4) : 0, gva_ok(c, 0x100) ? MEM32(c + 0xE0) : 0);
}
/* Every 2 s: the human's cameras (MACamera children of the player) and every field that points at a
 * game object (vtable in the image's data), with that object's class id -- logged on change. A
 * camera whose target went stale would show a projectile or a stranger there. */
static void cam_watch_fields(void)
{
    static double last; static char prev[4][1200];
    const Ma2World *w = &g_rt.world;
    if (w->time - last < 2.0 && w->time >= last) return;
    last = w->time;
    uint32_t cam_cls = MEM32(0x4EAFC0u);
    for (int i = 0, h = 0; i < w->nplayers && h < 4; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->is_human || !gva_ok(p->player, 0x300)) continue;
        char line[1200]; int o = 0; line[0] = 0;
        for (uint32_t e = MEM32(p->player + 0xC), g = 0; g < 64 && gva_ok(e, 0x30) && o < 1000; g++, e = MEM32(e + 8)) {
            uint32_t vt = MEM32(e);
            if (!(vt == 0x0046D348u || vt == 0x0046D208u || vt == 0x0046D4E0u || vt == 0x0046D630u || vt == 0x0046D738u || vt == 0x00475DF8u || (cam_cls && MEM32(e + 0x28) == cam_cls))) continue;
            o += snprintf(line + o, sizeof line - (size_t)o, " cam %08X %s:", e, cam_vt_name(vt));
            for (uint32_t off = 4; off < 0x220 && o < 1100; off += 4) {
                uint32_t v = MEM32(e + off);
                if (v == e || !gva_ok(v, 0x30)) continue;
                uint32_t ovt = MEM32(v);
                if (ovt < 0x00450000u || ovt > 0x004A0000u) continue;
                o += snprintf(line + o, sizeof line - (size_t)o, " +%X=%08X/%08X", off, v, MEM32(v + 0x28));
            }
        }
        if (strcmp(line, prev[h]) && g_cam_logs <= 400) {
            g_cam_logs++;
            fprintf(stderr, "[CAM-WATCH] t=%.1f human %d actor=%08X kind=%d%s\n", w->time, h, p->actor, p->kind, line);
            strcpy(prev[h], line);
        }
        h++;
    }
}
static void cam_watch_players(void)
{
    if (!c7_log_on()) return;   /* W-09b */
    cam_watch_fields();
    static uint32_t last_link[4], last_vt[4];
    const Ma2World *w = &g_rt.world;
    for (int i = 0, h = 0; i < w->nplayers && h < 4; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->is_human || !gva_ok(p->player, 0x300)) continue;
        uint32_t link = MEM32(p->player + 0x90), ctl = MEM32(p->player + 0x2F4);
        uint32_t vt = gva_ok(link, 0x10) ? MEM32(link) : 0;
        {   /* RECOMP_BOT_CAMWATCH (developer only; it found the gun-camera hijack): a hardware write
             * watch on this human's camera controller's camera slot (ctl+0x14), re-armed whenever the
             * controller changes (it is per body): the writer that drops the player's camera is logged
             * with its stack ([BOT-WATCH]). Not under RECOMP_BOT_DIAG: the watch outlives a dead
             * player's controller, keeps firing on whatever reuses that memory, and its handler once
             * overflowed a deep game-thread stack (run fr1, C00000FD in watch_veh). */
            static uint32_t armed_ctl;
            if (h == 0 && env_on("RECOMP_BOT_CAMWATCH") && gva_ok(ctl, 0x20) && ctl != armed_ctl) {
                armed_ctl = ctl; watch_hits = 0; watch_arm(ctl + 0x14);
                fprintf(stderr, "[CAM-WATCH] t=%.1f watching ctl %08X +0x14 (camera %08X)\n", w->time, ctl, MEM32(ctl + 0x14));
            }
        }
        if ((link != last_link[h] || vt != last_vt[h]) && g_cam_logs <= 400) {
            g_cam_logs++;
            fprintf(stderr, "[CAM-WATCH] t=%.1f human %d player=%08X camera link=%08X %s (vt %08X) ctl=%08X ctl14=%08X actor=%08X kind=%d\n", w->time, h, p->player, link,
                    cam_vt_name(vt), vt, ctl, gva_ok(ctl, 0x20) ? MEM32(ctl + 0x14) : 0, p->actor, p->kind);
            last_link[h] = link; last_vt[h] = vt;
        }
        h++;
    }
}
/* The VTOL camera hijack (2026-10-14, Bogged Down): the human's view left him and flew over the
 * map from a new angle every few seconds. sub_16B2F1 puts a camera in a screen's camera slot and
 * turns the slot's old camera off; the slot comes from the player (sub_16B7B8(player+0x88)), and a
 * bot has no screen of its own. A bot in a fixed gun that started an attack had a MACameraFirst
 * built for it (the vehicle-camera block in sub_C9084/sub_C9113, caller 0xC92DA) and put in the
 * slot -- the human's camera went off (ctl+0x14 = 0, player+0x90 = 0) and the screen showed the
 * bot's gun camera, a new one each time a gun bot engaged. Now that block skips bots
 * (recomp_0021.c) and no camera a bot owns goes in a slot here (returns 1: the call is skipped).
 * RECOMP_BOT_DIAG logs every call ([CAM-SET]). */
static uint32_t g_last_human_cam;   /* the last non-bot camera put in a screen slot (RECOMP_CAM_POSE fallback) */
int ma2_bot_cam_set(uint32_t slot, uint32_t table, uint32_t cam, uint32_t ret)
{
    static int n;
    uint32_t owner = gva_ok(cam, 0x30) ? MEM32(cam + 4) : 0;
    int bot = owner && ma2_bot_is_local_ai(owner);
    if (!bot && gva_ok(cam, 0x190)) g_last_human_cam = cam;
    if (env_on("RECOMP_BOT_DIAG") && n < 300) {
        n++;
        fprintf(stderr, "[CAM-SET] t=%.1f slot=%u table=%08X cam=%08X cls=%08X owner=%08X%s caller=%08X\n", g_rt.world.time, slot, table, cam,
                gva_ok(cam, 0x30) ? MEM32(cam + 0x28) : 0, owner, bot ? " (BOT: refused)" : "", ret);
    }
    return bot;
}
/* Editor fidelity: RECOMP_ENT_DUMP="file[,secs]" appends the whole world tree (every entity: depth,
 * address, parent, class id, flags, node matrix, raw words +0x2C..+0x90) once per level, when the game
 * clock passes secs (default 5), and again whenever "file.now" exists (deleted after the dump).
 * Runs on the game thread from the view-draw hook, so it works in campaign missions too. */
static void ent_dump_tree(const char *path, double now, const char *why)
{
    uint32_t game = MEM32(VA_GAME);
    if (!gva_ok(game, 0xA8)) return;
    uint32_t r1 = MEM32(game + 0xA4);
    if (!gva_ok(r1, 8)) return;
    uint32_t root = MEM32(r1 + 4);
    if (!gva_ok(root, 0x30)) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "DUMP t=%.2f why=%s root=%08X\n", now, why, root);
    static uint32_t st[8192]; static uint8_t sd[8192];
    int sp = 0, n = 0;
    st[sp] = root; sd[sp++] = 0;
    while (sp > 0 && n < 200000) {
        uint32_t e = st[--sp]; int d = sd[sp];
        n++;
        uint32_t sib = MEM32(e + 8), child = MEM32(e + 0xC);
        if (gva_ok(sib, 0x30) && sp < 8192) { st[sp] = sib; sd[sp++] = (uint8_t)d; }
        if (gva_ok(child, 0x30) && sp < 8192) { st[sp] = child; sd[sp++] = (uint8_t)(d < 255 ? d + 1 : d); }
        fprintf(f, "E %d %08X p=%08X cls=%08X vt=%08X f1C=%08X", d, e, MEM32(e + 4), MEM32(e + 0x28), MEM32(e), MEM32(e + 0x1C));
        uint32_t nd = MEM32(e + 0x94);
        if (gva_ok(nd, 0x100) && gva_ok(MEM32(e), 0x58))
            fprintf(f, " pos=%.3f,%.3f,%.3f m=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f",
                    MEMF(nd + 0xF0), MEMF(nd + 0xF4), MEMF(nd + 0xF8),
                    MEMF(nd + 0xC0), MEMF(nd + 0xC4), MEMF(nd + 0xC8), MEMF(nd + 0xD0), MEMF(nd + 0xD4), MEMF(nd + 0xD8),
                    MEMF(nd + 0xE0), MEMF(nd + 0xE4), MEMF(nd + 0xE8));
        fprintf(f, " w:");
        for (uint32_t o = 0x2C; o <= 0x90; o += 4) fprintf(f, " %X", MEM32(e + o));
        fprintf(f, "\n");
    }
    fprintf(f, "END n=%d\n", n);
    fclose(f);
    { char shown[MAX_PATH * 2]; fprintf(stderr, "[ENT-DUMP] t=%.2f %s: %d entities -> %s\n", now, why, n, xbox_log_path(path, shown, sizeof shown)); }
}
static void ent_dump_tick(void)
{
    static int init; static char path[512]; static double at = 5.0, next_poll;
    static uint32_t dumped_root;
    if (!init) {
        init = 1;
        const char *s = getenv("RECOMP_ENT_DUMP");
        if (s && *s) {
            strncpy(path, s, sizeof path - 1);
            char *c = strrchr(path, ',');
            if (c && c > path + 2) { at = atof(c + 1); *c = 0; }
        }
    }
    if (!path[0]) return;
    double now = MEMD(VA_GAME_TIME);
    uint32_t game = MEM32(VA_GAME), root = 0;
    if (gva_ok(game, 0xA8) && gva_ok(MEM32(game + 0xA4), 8)) root = MEM32(MEM32(game + 0xA4) + 4);
    if (root && root != dumped_root && now >= at) { dumped_root = root; ent_dump_tree(path, now, "level"); }
    if (now < next_poll && now >= next_poll - 2.0) return;
    next_poll = now + 1.0;
    char flag[530]; snprintf(flag, sizeof flag, "%s.now", path);
    if (GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(flag); ent_dump_tree(path, now, "flag"); }
}
/* Editor fidelity: RECOMP_CAM_POSE=1 logs the human camera's world position and matrix about once a
 * second ([CAM-POSE]), so editor views can be framed exactly like the game's. */
static void cam_pose_log(uint32_t view)
{
    static int init, on; static double last; static int logs;
    if (!init) { init = 1; const char *s = getenv("RECOMP_CAM_POSE"); on = s && *s && *s != '0'; }
    if (!on || logs > 4000 || view <= 0x200u) return;
    double now = MEMD(VA_GAME_TIME);
    if (now - last < 1.0 && now >= last) return;
    uint32_t c = view - 0x200u;
    uint32_t vt = gva_ok(c, 0x100) ? MEM32(c) : 0;
    if (vt != 0x0046D348u && vt != 0x0046D208u) {             /* MACameraThird / MACameraFirst */
        c = g_last_human_cam;                                 /* some levels draw the human's view from another struct */
        if (!c || !gva_ok(c, 0x190)) return;
        vt = MEM32(c);
    }
    if (ma2_bot_is_local_ai(MEM32(c + 4))) return;
    last = now; logs++;
    /* eye at +0x7C, horizontal FOV (deg) at +0x90, look-at point at +0x180 (checked on Mission 05) */
    fprintf(stderr, "[CAM-POSE] t=%.2f %s eye=%.3f,%.3f,%.3f target=%.3f,%.3f,%.3f hfov=%.2f\n", now,
            vt == 0x0046D348u ? "third" : vt == 0x0046D208u ? "first" : cam_vt_name(vt),
            MEMF(c + 0x7C), MEMF(c + 0x80), MEMF(c + 0x84), MEMF(c + 0x180), MEMF(c + 0x184), MEMF(c + 0x188), MEMF(c + 0x90));
}
void ma2_bot_view_drawn(uint32_t view)
{
    HN("view_drawn_first");
    ent_dump_tick();
    cam_pose_log(view);
    cam_watch_view(view);
    uint32_t step = MEM32(VA_STEP_TIME);
    if (step != g_views_step) { g_views_step = step; g_views_frames++; g_views_this = 0; }
    g_views_drawn++; g_views_this++;
    if (g_views_this > g_views_max) g_views_max = g_views_this;
    if (view > 0x200u) {                              /* a MACameraThird's view sits at camera+0x200 */
        uint32_t c = view - 0x200u;
        if (gva_ok(c, 0x80) && MEM32(c) == 0x0046D348u && MEM32(c + 0x78) == view && ma2_bot_is_local_ai(MEM32(c + 4))) g_views_bot++;
    }
}
static void camera_census(void)
{
    static double last;
    const Ma2World *w = &g_rt.world;
    if (w->time - last < 5.0 && w->time >= last) return;
    last = w->time;
    uint32_t cam_cls = MEM32(0x4EAFC0u);
    int hc = 0, bc = 0, blink = 0;
    for (int i = 0; i < w->nplayers; i++) {
        const Ma2PlayerView *p = &w->players[i];
        if (!p->player || p->player == p->actor || !gva_ok(p->player, 0x4B0)) continue;
        int n = 0;
        for (uint32_t e = MEM32(p->player + 0xC), g = 0; g < 64 && gva_ok(e, 0x30); g++, e = MEM32(e + 8))
            if (MEM32(e) == 0x0046D348u || (cam_cls && is_kind(e, cam_cls))) n++;
        if (p->is_bot) { bc += n; if (MEM32(p->player + 0x90)) blink++; } else if (p->is_human) hc += n;
    }
    fprintf(stderr, "[BOT-CAMS] t=%.0f human_cams=%d bot_cams=%d bot_cam_links=%d bots=%d | [BOT-VIEWS] drawn=%u frames=%u max_per_step=%u bot_views=%u\n",
            w->time, hc, bc, blink, g_rt.nbots, g_views_drawn, g_views_frames, g_views_max, g_views_bot);
}

static void bot_summary(void)
{
    static double last;
    const Ma2World *w = &g_rt.world;
    if (w->time - last < 10.0 && w->time >= last) return;
    last = w->time;
    for (int i = 0; i < g_rt.nbots; i++) {
        const Bot *b = &g_rt.bots[i]; const BotCtrl *c = &b->ctrl;
        if (!b->player) continue;
        char brain[320] = "";
        if (b->brain) ma2_brain_describe(b->brain, brain, sizeof brain);
        fprintf(stderr, "[BOT-SUM] t=%.0f bot=%d board=%.1f dist63=%.0f dist=%.0f eng_s=%.1f engagements=%u los_clear=%u los_block=%u dmg=%.0f dmg_human=%.0f frames=%u tap39=%u tap12=%u jumps=%u dodges=%u shots=%u ff_holds=%u clamp=%u/%u kind=%d minsep=%.1f contacts=%u avoids=%u replan_blocked=%u stalls=%u backtracks=%u escapes=%u trapped=%u dropfail=%u | %s\n",
                w->time, i, c->board_t, c->max_dist63, c->max_dist, c->eng_time, c->engagements, c->los_clear, c->los_block, c->dmg, c->dmg_human, c->frames,
                c->taps[39], c->taps[12], c->jumps, c->dodges, c->shots, c->ff_holds, c->clamps, c->clamp_tries, c->kind, c->min_sep, c->contacts, c->avoids, c->replans_blocked,
                c->n_stalls, c->backtracks, c->escapes, c->n_trapped, c->n_dropfail, brain);
    }
}

/* No tank_repair: the "parked tanks with zeroed drive parameters" it filled in were the fixed gun
 * emplacements (tank_fixed), and a bot in one drove the gun across the map. */

/* The human-autopilot test harness drove the human's pad; gone with the pads. */
int ma2_bot_test_human_pad(float *v) { (void)v; return 0; }

/* sub_17ED0 (patched in recomp_0000.c): the campaign pauses the whole world while a menu that asks
 * for it is up (the clock step sub_1DE70 then advances nothing); multiplayer never does, because on
 * System Link the other consoles play on. A local match with bots has no one else: it pauses like
 * the campaign. RECOMP_BOT_WORLD_PAUSE=0 keeps the multiplayer rule. */
int ma2_bot_world_pause_ok(void)
{
    static int on = -1;
    if (on < 0) on = env_int("RECOMP_BOT_WORLD_PAUSE", 1);
    return on && g_rt.nbots > 0;
}

/* Paused. The multiplayer pause stops only the local players: sub_C97D1 resets their controls and
 * sub_C7F8F hands their input to the behaviour's paused handler, while the world runs on as it must
 * for System Link opponents -- and the bots fought on against a player sitting in the menu. They
 * stand down instead: no decisions, nothing held or fired, the body stopped where it is, a VTOL
 * hovering on a centred stick; on resume they pick up where they were. */
static void bot_hold_paused(int index, Bot *b)
{
    BotCtrl *c = &b->ctrl;
    uint32_t a = b->player ? player_actor(b->player) : 0;
    if (!b->paused) {
        b->paused = 1; b->pause_actor = a; if (a) b->pause_pos = actor_pos(a);
        if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-PAUSE] bot=%d stands down (paused) t=%.1f world=%u\n", index, g_rt.world.time, MEM8(0x004D382Au));
    }
    memset(c->press, 0, sizeof c->press);
    for (int i = 0; i < 61; i++) c->axis[i] = 0.f;
    c->alpha_hold = 0; c->trig_act = 0; c->engaged = 0;
    if (!a) return;
    uint32_t ai = actor_ai(a);
    if (ai) { MEM8(ai + 0x90) = 0; MEM32(ai + 0xB4) = 0; MEM32(ai + 0xB8) = 0; }
    if (actor_kind(a) == MA2_KIND_VTOL) {
        uint32_t comp = MEM32(a + 0x8C);
        if (gva_ok(comp, 0x150)) MEM32(comp + 0x144) &= ~1u;      /* the player's stick, centred: it hovers */
        c->axis[4] = c->axis[5] = c->axis[8] = c->axis[9] = 1e-6f;
    } else body_stop(a);
    ctrl_send_frame(index, b->player);
}

/* ------------------------------------------------------------------ */
/* RECOMP_TEST_HUMAN_FERRY: the human's own keys, posted to the game's window (as the MCP live file does). */
extern void *xbox_FramebufferWindowHandle(void);
extern volatile DWORD g_heap_free_log_until;   /* kernel/xbox_memory_layout.c, test-only */
static uint32_t g_test_hijack_bot;   /* RECOMP_TEST_HIJACK: the enemy bot player called to the human */
static Ma2Vec3 g_test_hijack_to;
static unsigned char g_tkey[256];
static void test_key(int vk, int down)
{
    HWND h = (HWND)xbox_FramebufferWindowHandle();
    if (!h || g_tkey[vk & 255] == (unsigned char)down) return;
    g_tkey[vk & 255] = (unsigned char)down;
    PostMessageA(h, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, down ? 1 : (LPARAM)3 << 30);
}
static void test_tap(int vk)
{
    HWND h = (HWND)xbox_FramebufferWindowHandle();
    if (h) { PostMessageA(h, WM_KEYDOWN, (WPARAM)vk, 1); PostMessageA(h, WM_KEYUP, (WPARAM)vk, (LPARAM)3 << 30); }
}
static void test_steer_off(void) { test_key('W', 0); test_key('J', 0); test_key('L', 0); test_key('D', 0); test_key('A', 0); }
/* Walk the human toward `to`: walk with W, turn with J/L until the bearing is within 12 deg. Which key turns which way is learnt: if the error keeps growing while turning,
 * the sense is flipped. */
static void test_steer(const Ma2PlayerView *p, Ma2Vec3 to, float stop, double now)
{
    static int sense = 1; static double t0; static float e0 = -1.f;
    float dx = to.x - p->pos.x, dz = to.z - p->pos.z, d = hypotf(dx, dz);
    if (d < stop) { test_steer_off(); return; }
    float err = atan2f(dx, dz) * 57.29578f - p->yaw * 57.29578f;
    while (err > 180.f) err -= 360.f;
    while (err < -180.f) err += 360.f;
    if (now - t0 > 1.5) {
        if (e0 >= 0.f && fabsf(err) > 30.f && fabsf(err) > e0 + 10.f) {
            sense = -sense;
            fprintf(stderr, "[TEST-HFERRY] t=%.1f turn sense flipped (error %.0f -> %.0f deg)\n", now, e0, fabsf(err));
        }
        t0 = now; e0 = fabsf(err);
    }
    float turn = (float)sense * err;
    /* W always: on foot the right stick turns the camera, and the body only turns toward it while walking. */
    test_key('L', turn > 12.f); test_key('J', turn < -12.f);
    test_key('W', 1);
    static double slog;
    if (now - slog > 1.0) { slog = now; fprintf(stderr, "[TEST-HFERRY] t=%.1f steer err=%.0f sense=%d keys W%s%s %.1f m\n", now, err, sense, turn > 12.f ? " L" : "", turn < -12.f ? " J" : "", d); }
}

/* Per-step tick: hook at loc_000C8130 in the player tick sub_C80DB    */
/* (after sub_C94FF re-bound the control behaviour), for local AI      */
/* players only.                                                       */
/* ------------------------------------------------------------------ */
void ma2_bot_player_tick(uint32_t player)
{
    if (g_rt.nbots <= 0 || !player) return;          /* zero footprint */
    int index = bot_index_of(player);
    if (index < 0) return;
    g_cur_bot = index;   /* MixedDragoon: ma2_bots_dragoon() answers for this bot until done */
    HN("player_tick_first");
    GuestRegs regs; regs_save(&regs);
    Bot *b = &g_rt.bots[index];
    uint32_t step = MEM32(VA_STEP_TIME);
    static LARGE_INTEGER qf, q_last; static double bot_ms, world_ms; static int perf_on = -1, ticks;
    static int hitch_on = -1; static LARGE_INTEGER hq_f; LARGE_INTEGER hq0, hq1; double hs_world = 0, hs_nav = 0, hs_act = 0; double hp0_ray = g_perf_ray_ms, hp0_los = g_perf_los_ms, hp0_plan = g_perf_plan_ms, hp0_br = g_perf_brain_ms, hp0_ct = g_perf_ctrl_ms; unsigned hp0_plans = g_perf_plans, hp0_rays = g_perf_rays;   /* W2: RECOMP_HITCH per-tick split */
    if (hitch_on < 0) { hitch_on = nv2a_hitch_enabled(); QueryPerformanceFrequency(&hq_f); }
    if (hitch_on) QueryPerformanceCounter(&hq0);
    LARGE_INTEGER q0; if (perf_on < 0) { perf_on = env_on("RECOMP_BOT_PERF") || getenv("RECOMP_PROFILE") != NULL || hitch_on > 0; QueryPerformanceFrequency(&qf); }
    if (perf_on) QueryPerformanceCounter(&q0);
    if (b->ticked && b->last_step == step) goto done;
    b->ticked = 1; b->last_step = step;
    { static uint32_t c7_step = 0xFFFFFFFFu; if (step != c7_step) { c7_step = step; c7_frame(MEMD(VA_GAME_TIME)); rec_step(MEMD(VA_GAME_TIME), step); } }
    if (hitch_on) { static double hlast_t = 1e30; double gt = MEMD(VA_GAME_TIME); if (gt < hlast_t - 0.5) nv2a_hitch_mark("match-start"); hlast_t = gt; }   /* C7: one frame per game step */
    if (index == 0) {
        /* test hook RECOMP_BOT_TEST_JUMP="at:add": advance the game clock once (match-end flow tests) */
        static int jumped; const char *j = getenv("RECOMP_BOT_TEST_JUMP");
        double at = 0, add = 0;
        if (!jumped && j && sscanf(j, "%lf:%lf", &at, &add) == 2 && MEMD(VA_GAME_TIME) >= at) {
            jumped = 1; MEMD(VA_GAME_TIME) = MEMD(VA_GAME_TIME) + add;
            fprintf(stderr, "[TEST-JUMP] game time +%.0f\n", add);
        }
    }
    if (index == 0) {
        /* test hook RECOMP_BOT_TEST_FREEZE="at:ms": hold the game thread once (freeze watchdog tests) */
        static int froze; const char *fz = getenv("RECOMP_BOT_TEST_FREEZE");
        double fat = 0; int fms = 0;
        if (!froze && fz && sscanf(fz, "%lf:%d", &fat, &fms) == 2 && MEMD(VA_GAME_TIME) >= fat) {
            froze = 1; fprintf(stderr, "[TEST-FREEZE] holding the game thread %d ms\n", fms); fflush(stderr);
            Sleep((DWORD)fms);
        }
    }
    stall_heartbeat();
    { LARGE_INTEGER w0, w1; if (perf_on || hitch_on) QueryPerformanceCounter(&w0);
      world_update();
      if (hitch_on) { QueryPerformanceCounter(&w1); hs_world = (double)(w1.QuadPart - w0.QuadPart) * 1000.0 / (double)hq_f.QuadPart; }
      if (perf_on) { QueryPerformanceCounter(&w1); world_ms += (double)(w1.QuadPart - w0.QuadPart) * 1000.0 / (double)qf.QuadPart; } }
    if (MEM8(VA_PAUSED)) { bot_hold_paused(index, b); goto done; }
    if (b->paused) {
        b->paused = 0;
        if (env_on("RECOMP_BOT_DIAG")) {
            uint32_t a = b->player ? player_actor(b->player) : 0; Ma2Vec3 p = a ? actor_pos(a) : b->pause_pos;
            fprintf(stderr, "[BOT-PAUSE] bot=%d resumes t=%.1f, moved %.1f m (%.1f up) while paused%s\n", index, g_rt.world.time,
                    hypotf(p.x - b->pause_pos.x, p.z - b->pause_pos.z), p.y - b->pause_pos.y, a != b->pause_actor ? " (another body)" : "");
        }
    }
    if (env_int("RECOMP_BOT_DIAG", 0) >= 2) player_diag();
    { LARGE_INTEGER n0, n1; if (hitch_on) QueryPerformanceCounter(&n0);
      ensure_nav();
      if (hitch_on) { QueryPerformanceCounter(&n1); hs_nav = (double)(n1.QuadPart - n0.QuadPart) * 1000.0 / (double)hq_f.QuadPart; } }
    if (g_nav_slicing) goto done;   /* W2: bots wait for the sliced obstacle grid */
    ray_selftest();
    ai_drive_sweep();
    { LARGE_INTEGER a0, a1; if (hitch_on) QueryPerformanceCounter(&a0);
      for (int i = 0; i < g_rt.nbots; i++) bot_ensure_active(i, &g_rt.bots[i]);
      if (hitch_on) { QueryPerformanceCounter(&a1); hs_act = (double)(a1.QuadPart - a0.QuadPart) * 1000.0 / (double)hq_f.QuadPart; } }
    {   /* once per step: who attacks / fires (players' view), then the damage events */
        static uint32_t combat_step = 0xFFFFFFFFu;
        if (combat_step != g_rt.world_step) { combat_step = g_rt.world_step; combat_state_update(&g_rt.world); }
    }
    damage_track();
    {   /* Test hooks for the VTOL questions:
         * RECOMP_TEST_VTOL_LOWHP="frac:t": from time t every bot VTOL drops to frac of its health (once each)
         *   -- with RECOMP_BOT_FORCE_FERRY it then hovers down onto a power-up: does it heal?
         * RECOMP_TEST_VTOL_ABANDON="t": from time t the first bot in a VTOL drops to 30 %, bails out, and nobody
         *   may board that hull; it is then watched every 5 s ([TEST-HULL]) -- does it stay, go, come back fresh?
         * Both log every bot VTOL's health change ([TEST-VHP]). */
        const char *lv = getenv("RECOMP_TEST_VTOL_LOWHP"), *ab = getenv("RECOMP_TEST_VTOL_ABANDON");
        static uint32_t lowed[32]; static int nlowed; static uint32_t hull, hull_bot_player; static double hull_t, hull_log; static int hull_state;
        static uint32_t vhp_a[MAX_BOTS]; static float vhp_v[MAX_BOTS];
        const Ma2World *w = &g_rt.world;
        float fr = 0.f, at = 1e9f;
        if (lv) sscanf(lv, "%f:%f", &fr, &at);
        for (int k = 0; k < g_rt.nbots && (lv || ab); k++) {
            uint32_t a = g_rt.bots[k].player ? player_actor(g_rt.bots[k].player) : 0;
            if (!a || actor_kind(a) != MA2_KIND_VTOL) continue;
            const Ma2PlayerView *pv = NULL;
            for (int i = 0; i < w->nplayers; i++) if (w->players[i].actor == a) pv = &w->players[i];
            if (!pv) continue;
            float hp = pv->hp;   /* a VTOL keeps its health behind its vfunc +0x74, not the 'Mech block: read only */
            if (vhp_a[k] != a || fabsf(hp - vhp_v[k]) > 0.5f) {
                if (vhp_a[k] == a) fprintf(stderr, "[TEST-VHP] t=%.1f bot=%d vtol=%08X hp %.0f -> %.0f of %.0f cargo=%u at %.0f,%.0f,%.0f\n", w->time, k, a, vhp_v[k], hp, pv->hp_max, MEM32(a + 0x57C), pv->pos.x, pv->pos.y, pv->pos.z);
                vhp_a[k] = a; vhp_v[k] = hp;
            }
            (void)fr; (void)at; (void)lowed; (void)nlowed;
            if (ab && !hull && w->time >= atof(ab)) {      /* the first bot in a VTOL from then on */
                hull = a; hull_bot_player = g_rt.bots[k].player; hull_t = w->time; hull_state = 1;
                fprintf(stderr, "[TEST-HULL] t=%.1f bot=%d vtol=%08X hp=%.0f/%.0f bails out\n", w->time, k, a, pv->hp, pv->hp_max);
            }
            if (hull == a && hull_state == 1) ma2_brain_test_bail(g_rt.bots[k].brain, 1.0);
        }
        if (hull && w->time - hull_log >= (hull_state == 1 ? 1.0 : 5.0)) {
            hull_log = w->time;
            int found = 0;
            for (int i = 0; i < w->nvehicles; i++) if (w->vehicles[i].actor == hull) {
                found = 1;
                static int seen_occ;
                if (w->vehicles[i].occupied) seen_occ = 1;
                if (!w->vehicles[i].occupied && hull_state == 1 && seen_occ) { hull_state = 2; hull_t = w->time; }
                ma2_brain_test_refuse(hull, w->time, 600.0);
                fprintf(stderr, "[TEST-HULL] t=%.1f vtol=%08X %s %.0fs hp=%.0f/%.0f at %.0f,%.0f,%.0f f1C=%08X\n", w->time, hull,
                        w->vehicles[i].occupied ? "occupied" : "empty for", w->time - hull_t, w->vehicles[i].hp, w->vehicles[i].hp_max,
                        w->vehicles[i].pos.x, w->vehicles[i].pos.y, w->vehicles[i].pos.z, MEM32(hull + 0x1C));
            }
            if (!found) fprintf(stderr, "[TEST-HULL] t=%.1f vtol=%08X is gone (%.0fs after it was left); valid=%d cls=%08X\n", w->time, hull, w->time - hull_t,
                                gva_ok(hull, 0x100), gva_ok(hull, 0x100) ? MEM32(hull + 0x28) : 0);
            for (int i = 0; i < w->nvehicles; i++)                 /* every VTOL on the map: a fresh one at the base? */
                if (w->vehicles[i].kind == MA2_KIND_VTOL)
                    fprintf(stderr, "[TEST-HULL]   vtol %08X %s hp=%.0f/%.0f at %.0f,%.0f,%.0f\n", w->vehicles[i].actor, w->vehicles[i].occupied ? "occ" : "free",
                            w->vehicles[i].hp, w->vehicles[i].hp_max, w->vehicles[i].pos.x, w->vehicles[i].pos.y, w->vehicles[i].pos.z);
        }
    }
    if (env_on("RECOMP_HUMAN_ACT")) {
        /* diag: which behaviour actions the local human's input lights up (keyboard key -> action number) */
        static char last[512];
        const Ma2World *w = &g_rt.world;
        for (int i = 0; i < w->nplayers; i++) {
            if (!w->players[i].is_human) continue;
            uint32_t beh = MEM32(w->players[i].player + 0x2F0);
            if (!gva_ok(beh, 0x6F0)) continue;
            char buf[512]; int o = 0; buf[0] = 0;
            for (int a = 0; a < 60 && o < 480; a++) {
                float v = MEMF(beh + 0x18 + (uint32_t)a * 0x1C + 4);
                if (v != 0.f && a != 4 && a != 5 && a != 8 && a != 9) o += snprintf(buf + o, sizeof buf - (size_t)o, " %d(%.2f)", a, v);
            }
            if (strcmp(buf, last)) {
                strcpy(last, buf);
                fprintf(stderr, "[HUMAN-ACT] t=%.2f kind=%d y=%.1f actions:%s\n", w->time, w->players[i].kind, w->players[i].pos.y, buf[0] ? buf : " none");
            }
            { static double et; if (w->time - et > 2.0 || w->time < et) { et = w->time; const Ma2PlayerView *hp = &w->players[i];
                for (int k = 0; k < w->nvehicles; k++) { const Ma2VehicleView *v = &w->vehicles[k]; if (v->kind != MA2_KIND_VTOL || !v->has_entry) continue;
                    fprintf(stderr, "[HUMAN-VTOL] t=%.1f me=%.1f,%.1f vtol=%.1f,%.1f entry=%.1f,%.1f r=%.1f d=%.1f occ=%d\n", w->time, hp->pos.x, hp->pos.z, v->pos.x, v->pos.z, v->entry.x, v->entry.z, v->entry_r, hypotf(v->entry.x - hp->pos.x, v->entry.z - hp->pos.z), v->occupied); } } }
            break;
        }
    }
    {   /* test hook RECOMP_BOT_TEST_LOWHP="k:frac:t": bot k's 'Mech drops to frac of its health at time t (once) */
        static int done; const char *lh = getenv("RECOMP_BOT_TEST_LOWHP"); int k = -1; float fr = 0.f, at = 0.f;
        if (!done && lh && sscanf(lh, "%d:%f:%f", &k, &fr, &at) == 3 && g_rt.world.time >= at && k >= 0 && k < g_rt.nbots && g_rt.bots[k].player) {
            uint32_t a = player_actor(g_rt.bots[k].player), hpb = a ? MEM32(a + 0x260) : 0;
            if (a && actor_kind(a) == MA2_KIND_MECH && gva_ok(hpb, 0x10)) {
                done = 1; MEM32(hpb + 8) = (uint32_t)((float)MEM32(hpb + 4) * fr);
                fprintf(stderr, "[TEST-LOWHP] t=%.1f bot=%d actor=%08X hp=%u/%u\n", g_rt.world.time, k, a, MEM32(hpb + 8), MEM32(hpb + 4));
            }
        }
    }
    if (index == 0) { bombs_update(&g_rt.world); blasts_update(&g_rt.world); }           /* pilot bombs: watched here, beside the bots' own planting (world_update skips the first seconds) */
    if (index == 0 && g_probe.phys) {   /* RECOMP_BOT_FPROBE="x0,z0,x1,z1,step@t" (diagnostic): unfiltered rays on a fine lattice at game time t */
        static int fp_done; const char *fp = getenv("RECOMP_BOT_FPROBE"); float x0, z0, x1, z1, st, at;
        if (!fp_done && fp && sscanf(fp, "%f,%f,%f,%f,%f@%f", &x0, &z0, &x1, &z1, &st, &at) == 6 && st >= 0.1f && g_rt.world.time >= at) {
            fp_done = 1;
            for (float z = z0; z <= z1 + 0.01f; z += st)
                for (float x = x0; x <= x1 + 0.01f; x += st) {
                    int okg = 0; float g = terrain_height(x, z, &okg), t = 0.f; uint32_t ob = 0;
                    if (!okg) continue;
                    Ma2Vec3 o = { x, g + 30.f, z }, dn = { 0.f, -1.f, 0.f };
                    if (ray_cast(&g_probe, o, dn, 34.f, 1, 1, &t, &ob) && g + 30.f - t > g + 0.15f) {
                        uint32_t owner = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0, cls = gva_ok(owner, 0x30) ? MEM32(owner + 0x28) : 0;
                        fprintf(stderr, "[FPROBE] t=%.1f %.2f,%.2f down hit %.2f over terrain %.2f (+%.2f) owner=%08X cls=%08X mover=%d\n", g_rt.world.time, x, z, g + 30.f - t, g, g + 30.f - t - g, owner, cls, owner ? is_kind(owner, g_cls.mover) : -1);
                    }
                    {   /* overhead: down from 100 m up (what tops the point) and up from 0.5 m (the first underside) */
                        float th2 = 0.f, tu = 0.f; uint32_t oh = 0, ou = 0;
                        Ma2Vec3 oh0 = { x, g + 100.f, z }, ou0 = { x, g + 0.5f, z }, up = { 0.f, 1.f, 0.f };
                        int hh2 = ray_cast(&g_probe, oh0, dn, 104.f, 1, 1, &th2, &oh), hu = ray_cast(&g_probe, ou0, up, 100.f, 1, 1, &tu, &ou);
                        uint32_t owh = hh2 && gva_ok(oh, 0x130) ? MEM32(oh + 0x128) : 0, owu = hu && gva_ok(ou, 0x130) ? MEM32(ou + 0x128) : 0;
                        fprintf(stderr, "[FPROBE] t=%.1f %.2f,%.2f top %.2f (+%.2f) owner=%08X cls=%08X | up %s %.2f (+%.2f) owner=%08X cls=%08X\n", g_rt.world.time, x, z,
                                hh2 ? g + 100.f - th2 : -1.f, hh2 ? 100.f - th2 : -1.f, owh, gva_ok(owh, 0x30) ? MEM32(owh + 0x28) : 0,
                                hu ? "hit" : "none", hu ? g + 0.5f + tu : -1.f, hu ? 0.5f + tu : -1.f, owu, gva_ok(owu, 0x30) ? MEM32(owu + 0x28) : 0);
                    }
                    for (int h = 0; h < 3; h++) {   /* sideways at 0.4 / 0.9 / 1.6 m: 2 m east and 2 m north across the point */
                        static const float hh[3] = { 0.4f, 0.9f, 1.6f };
                        for (int ax = 0; ax < 2; ax++) {
                            Ma2Vec3 so = { x - (ax ? 0.f : 1.f), g + hh[h], z - (ax ? 1.f : 0.f) }, sd = { ax ? 0.f : 1.f, 0.f, ax ? 1.f : 0.f };
                            float ts = 0.f; uint32_t os = 0;
                            if (ray_cast(&g_probe, so, sd, 2.f, 1, 1, &ts, &os)) {
                                uint32_t ow = gva_ok(os, 0x130) ? MEM32(os + 0x128) : 0, cl = gva_ok(ow, 0x30) ? MEM32(ow + 0x28) : 0;
                                int okh = 0; float gh = terrain_height(so.x + sd.x * ts, so.z + sd.z * ts, &okh);
                                fprintf(stderr, "[FPROBE] t=%.1f %.2f,%.2f side%c h%.1f hit at %.2f (ground there %.2f, ray y %.2f) owner=%08X cls=%08X\n", g_rt.world.time, x, z, ax ? 'N' : 'E', hh[h], ts, gh, so.y, ow, cl);
                            }
                        }
                    }
                }
            fprintf(stderr, "[FPROBE] done\n");
        }
    }
    if (index == 0 && g_probe.phys && env_on("RECOMP_BOT_STALLPROBE")) {
        /* RECOMP_BOT_STALLPROBE=1 (diagnostic): a bot 'Mech that has made < 1.5 m/s for 3 s gets, every 2 s, rays from its own body
         * (its colliders skipped): 8 headings x heights 1/4/7/10/13 m, 12 m long; up from +0.5 m; down from +100 m. Hits only. */
        static double slow_since[64], last_log[64];
        const Ma2World *w = &g_rt.world;
        for (int i = 0; i < w->nplayers && i < 64; i++) {
            const Ma2PlayerView *p = &w->players[i];
            if (!p->is_bot || !p->alive || p->kind != MA2_KIND_MECH || !p->actor) { slow_since[i] = 0.0; continue; }
            if (hypotf(p->vel.x, p->vel.z) > 1.5f || w->time < slow_since[i]) { slow_since[i] = w->time; continue; }
            if (w->time - slow_since[i] < 3.0 || w->time - last_log[i] < 2.0) continue;
            last_log[i] = w->time;
            char buf[1600]; int o = 0;
            for (int d = 0; d < 8; d++) {
                float a = (float)d * 0.785398f, dx = sinf(a), dz = cosf(a);
                for (int hk = 0; hk < 5; hk++) {
                    float t = 0.f; uint32_t ob = 0; Ma2Vec3 so = { p->pos.x, p->pos.y + 1.f + 3.f * (float)hk, p->pos.z }, sd = { dx, 0.f, dz };
                    if (ray_cast_as(p->actor, so, sd, 12.f, 1, 1, &t, &ob) && o < (int)sizeof buf - 40) {
                        uint32_t ow = gva_ok(ob, 0x130) ? MEM32(ob + 0x128) : 0;
                        o += snprintf(buf + o, sizeof buf - (size_t)o, " d%d/h%d:%.1f:%X", d * 45, 1 + 3 * hk, t, gva_ok(ow, 0x30) ? MEM32(ow + 0x28) : 0);
                    }
                }
            }
            float tu = 0.f, td = 0.f; uint32_t ou = 0, od = 0;
            Ma2Vec3 u0 = { p->pos.x, p->pos.y + 0.5f, p->pos.z }, up = { 0.f, 1.f, 0.f }, d0 = { p->pos.x, p->pos.y + 100.f, p->pos.z }, dn = { 0.f, -1.f, 0.f };
            int hu = ray_cast_as(p->actor, u0, up, 100.f, 1, 1, &tu, &ou), hd = ray_cast_as(p->actor, d0, dn, 104.f, 1, 1, &td, &od);
            fprintf(stderr, "[STALLPROBE] t=%.1f p%d n%d at %.1f,%.1f,%.1f yaw %.2f slow %.1fs | up %.1f | top %.1f |%s\n", w->time, i, p->nation,
                    p->pos.x, p->pos.y, p->pos.z, p->yaw, w->time - slow_since[i], hu ? 0.5f + tu : -1.f, hd ? 100.f - td : -1.f, buf);
        }
    }
    if (index == 0 && getenv("RECOMP_BOT_TEST_KEEPALIVE")) {
        /* test hook RECOMP_BOT_TEST_KEEPALIVE=1 (diag only, default off): every player's unit (humans and bots)
         * (not the game's own AI units) is topped back up to full health each tick, so co-op modes never reach GAME OVER (Grinder wave proofs).
         * 'Mech/BA health = [actor+0x260]+8 (max +4); other movers = actor+0x184 (max = vfunc +0x6C). */
        static double logged; static int refills;
        const Ma2World *w = &g_rt.world;
        for (int i = 0; i < w->nplayers; i++) {
            const Ma2PlayerView *p = &w->players[i];
            uint32_t a = p->actor;
            if (!a || !p->alive || !gva_ok(a, 0x190) || p->player == p->actor) continue;   /* not the game's AI (Grinder waves) */
            if (p->kind == MA2_KIND_MECH || p->kind == MA2_KIND_BATTLEARMOR) {
                uint32_t hpb = MEM32(a + 0x260);
                if (gva_ok(hpb, 0x10) && (int)MEM32(hpb + 4) > 0 && MEM32(hpb + 8) < MEM32(hpb + 4)) { MEM32(hpb + 8) = MEM32(hpb + 4); refills++; }
            } else if (p->hp_max > 0.f && (int)MEM32(a + 0x184) > 0 && (int)MEM32(a + 0x184) < (int)p->hp_max) {
                MEM32(a + 0x184) = (uint32_t)(int)p->hp_max; refills++;
            }
        }
        if (w->time - logged >= 10.0) { logged = w->time; fprintf(stderr, "[TEST-KEEPALIVE] t=%.1f refills=%d\n", w->time, refills); }
    }
    if (index == 0) cam_watch_players();
    {   /* Test hooks for the VTOL camera hijack: RECOMP_TEST_HUMAN_BOARD="t" -- from time t the human's
         * pilot is walked by its own AI into the nearest friendly free VTOL's entry zone (the harness
         * presses Q there); RECOMP_TEST_VTOL_FRAGILE="t" -- from t every bot VTOL keeps at most 5 health
         * (actor+0x184), so the next hit destroys it the normal way. */
        const char *hb = getenv("RECOMP_TEST_HUMAN_BOARD"), *vf = getenv("RECOMP_TEST_VTOL_FRAGILE");
        const Ma2World *w = &g_rt.world;
        if (index == 0 && hb && w->time >= atof(hb))
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (!p->is_human || p->kind != MA2_KIND_PILOT || !p->actor) continue;
                int best = -1; float bd = 1e9f;
                static int dumped;
                if (!dumped) { dumped = 1;
                    for (int k = 0; k < w->nvehicles; k++) { const Ma2VehicleView *v = &w->vehicles[k];
                        fprintf(stderr, "[TEST-HBOARD] veh %08X kind=%d occ=%d entry=%d nation=%d (human nation %d) at %.0f,%.0f\n", v->actor, v->kind, v->occupied, v->has_entry, v->nation, p->nation, v->pos.x, v->pos.z); } }
                /* RECOMP_TEST_HUMAN_BOARD_KIND=2: a 'Mech instead of a VTOL -- the one a teammate bot has
                 * already set off for and is nearer to (the human starts behind it), else the nearest;
                 * kept until it is taken (the 'Mech yield test, BOTS-AI-PLAN.md APPROVED). */
                int want = env_int("RECOMP_TEST_HUMAN_BOARD_KIND", MA2_KIND_VTOL);
                static uint32_t keep;
                int kept = -1, ahead = -1; float ad = 1e9f;
                for (int k = 0; k < w->nvehicles; k++) {
                    const Ma2VehicleView *v = &w->vehicles[k];
                    if (v->kind != want || v->occupied || !v->has_entry || (w->team_mode && v->nation > 0 && v->nation != p->nation)) continue;   /* 0: nobody's */
                    float d = hypotf(v->entry.x - p->pos.x, v->entry.z - p->pos.z);
                    if (v->actor == keep) kept = k;
                    if (want == MA2_KIND_MECH && v->claimed_by && v->claim_nation == p->nation) {
                        for (int q = 0; q < w->nplayers; q++) {
                            const Ma2PlayerView *bq = &w->players[q];
                            if (bq->player != v->claimed_by) continue;
                            float bd2 = hypotf(v->entry.x - bq->pos.x, v->entry.z - bq->pos.z);
                            if (bd2 < d && d < ad) { ad = d; ahead = k; }
                        }
                    }
                    if (d < bd) { bd = d; best = k; }
                }
                if (want == MA2_KIND_MECH) {
                    if (kept >= 0) best = kept;
                    else if (ahead >= 0) best = ahead;
                    if (best >= 0 && w->vehicles[best].actor != keep) {
                        keep = w->vehicles[best].actor;
                        fprintf(stderr, "[TEST-HBOARD] t=%.1f target 'Mech %08X (%s), human %.0f m\n", w->time, keep,
                                best == ahead ? "a teammate bot is nearer and going for it" : "nearest", hypotf(w->vehicles[best].entry.x - p->pos.x, w->vehicles[best].entry.z - p->pos.z));
                    }
                    if (best >= 0) bd = hypotf(w->vehicles[best].entry.x - p->pos.x, w->vehicles[best].entry.z - p->pos.z);
                }
                if (best >= 0) {
                    static double logged;
                    /* The human's pilot has no AI (MoveTo / direct walk do not move it): for a 'Mech the
                     * harness steers the human with real input; the hook only names the target and logs
                     * the bearing to it (deg, from +z toward +x) against the human's facing. */
                    if (want != MA2_KIND_MECH) body_moveto(p->actor, w->vehicles[best].entry);
                    else {
                        static double blog;
                        const Ma2VehicleView *tv = &w->vehicles[best];
                        if (w->time - blog > 0.5) {
                            blog = w->time;
                            fprintf(stderr, "[TEST-HBOARD] t=%.1f human at %.0f,%.0f yaw=%.0f bearing=%.0f to 'Mech %08X, %.1f m (zone %.1f)\n", w->time, p->pos.x, p->pos.z,
                                    p->yaw * 57.29578f, atan2f(tv->entry.x - p->pos.x, tv->entry.z - p->pos.z) * 57.29578f, tv->actor, bd, tv->entry_r);
                        }
                    }
                    if (w->time - logged > 2.0) { logged = w->time; fprintf(stderr, "[TEST-HBOARD] t=%.1f human pilot to VTOL %08X entry, %.1f m\n", w->time, w->vehicles[best].actor, bd); }
                }
            }
        /* RECOMP_TEST_HUMAN_FERRY="t" (CTF, the human in a BattleArmor): proves the VTOL player ferry
         * (VtolWaitPlayer). From t: a lift is posted for the human so a bot VTOL comes down beside him;
         * within 8 m of it the claw is closed for him (ba_clamp, the bots' own lift path); flown to their
         * flag he lets go there, is walked onto the flag, clamps on again when the VTOL comes down beside
         * the carrier, and lets go over our stand. The human has no AI walk (body_moveto does not move
         * him): he is steered with his own keys posted to the game window -- W walks, J/L (right stick)
         * turn, Q boards -- first into the nearest free friendly BattleArmor if he is on foot. */
        /* RECOMP_TEST_HUMAN_VTOL_CLIMB="t" (test): from t the human walks into the nearest free friendly VTOL (keys), then
         * flies a key script -- W, W+Shift, Shift, W+Ctrl, S+Shift -- logging his height ([TEST-HCLIMB]); with
         * RECOMP_HUMAN_ACT the actions each key lights up. How a player climbs a VTOL (playtest 11-01: Shift held with W). */
        /* RECOMP_BOT_BA_ANIM_DIAG=1 (diag): every BattleArmor's walk-animation state twice a second -- the walker
         * (sub_FF68A) scales its stride by the current anim's speed range (sub_18B6F4: table at +0x868+0x54, 0x2C a row,
         * min +8 / max +0xC) and zeroes it for anims 0x12-0x14. [BA-ANIM] */
        if (index == 0 && env_on("RECOMP_BOT_BA_ANIM_DIAG")) {
            static double al; static uint32_t dumped[16];
            if (w->time - al >= 0.5 || w->time < al) { al = w->time;
                for (int i = 0; i < w->nplayers; i++) {
                    const Ma2PlayerView *p = &w->players[i]; uint32_t a = p->actor;
                    if (!p->alive || p->kind != MA2_KIND_BATTLEARMOR || !gva_ok(a, 0x960)) continue;
                    uint32_t ac = MEM32(a + 0x98), wk = a + 0x868, list = MEM32(wk + 0x1C), tab = MEM32(wk + 0x54); int n = (int)MEM32(wk + 0x10), idx = 0;
                    uint32_t cur = gva_ok(ac, 0x180) ? MEM32(ac + 0x17C) : 0;
                    if (cur && gva_ok(list, 4)) for (int k = 0; k < n && k < 64; k++) if (MEM32(list + 4 * k) == cur) { idx = k; break; }
                    float mn = -1.f, mx = -1.f; if (idx < n && gva_ok(tab, 0x2C * (idx + 1))) { mn = MEMF(tab + idx * 0x2C + 8); mx = MEMF(tab + idx * 0x2C + 0xC); }
                    uint32_t ai = actor_ai(a), comp = MEM32(a + 0x8C);
                    fprintf(stderr, "[BA-ANIM] t=%.1f %s p%d a=%08X idx=%d/%d cur=%08X range=%.2f..%.2f w958=%.3f cBC=%.3f sp=%.1f vy=%.1f mode=%d jump=%d\n", w->time, p->is_human ? "human" : "bot", i, a, idx, n, cur, mn, mx,
                            MEMF(a + 0x958), gva_ok(comp, 0xC0) ? MEMF(comp + 0xBC) : -1.f, hypotf(p->vel.x, p->vel.z), p->vel.y, ai ? (int)MEM32(ai + 0x12C) : -1, ai ? MEM8(ai + 0x240) : -1);
                    if (env_on("RECOMP_BOT_BA_ANIM_DUMP") && gva_ok(comp, 0x400)) {   /* every live float of the body and its component */
                        fprintf(stderr, "[BA-DUMP] t=%.1f %s p%d f260=%08X f264=%08X f268=%08X f26C=%08X f1E8=%08X c144=%08X c7C=%08X ai240=%08X", w->time, p->is_human ? "human" : "bot", i,
                                MEM32(a + 0x260), MEM32(a + 0x264), MEM32(a + 0x268), MEM32(a + 0x26C), MEM32(a + 0x1E8), MEM32(comp + 0x144), MEM32(comp + 0x7C), ai ? MEM32(ai + 0x240) : 0u);
                        for (int r = 0; r < 2; r++) { uint32_t b = r ? comp : a, lo = r ? 0 : 0x100, hi = r ? 0x400 : 0xA00;
                            for (uint32_t off = lo; off < hi; off += 4) { uint32_t u = MEM32(b + off); float f = MEMF(b + off);
                                if (u && (u & 0x7F800000u) && (u & 0x7F800000u) != 0x7F800000u && fabsf(f) > 1e-4f && fabsf(f) < 1e5f) fprintf(stderr, " %c%X=%.3f", r ? 'c' : 'a', off, f); } }
                        fprintf(stderr, "\n");
                    }
                    int seen = 0; for (int k = 0; k < 16; k++) if (dumped[k] == a) seen = 1;
                    if (!seen && n > 0 && gva_ok(tab, 0x2C * n)) {
                        for (int k = 0; k < 16; k++) if (!dumped[k]) { dumped[k] = a; break; }
                        fprintf(stderr, "[BA-ANIM] a=%08X table n=%d:", a, n);
                        for (int k = 0; k < n && k < 40; k++) fprintf(stderr, " %d:%08X=%.2f..%.2f", k, gva_ok(list, 4 * (k + 1)) ? MEM32(list + 4 * k) : 0u, MEMF(tab + k * 0x2C + 8), MEMF(tab + k * 0x2C + 0xC));
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
        const char *hc = getenv("RECOMP_TEST_HUMAN_VTOL_CLIMB");
        if (index == 0 && hc && w->time >= atof(hc))
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (!p->is_human || !p->alive || !p->actor) continue;
                static double qn, t_in, ylog; static int phase = -1;
                if (p->kind == MA2_KIND_PILOT && !getenv("RECOMP_TEST_HCLIMB_FOOT")) {
                    int best = -1; float bd = 1e9f;
                    for (int k = 0; k < w->nvehicles; k++) { const Ma2VehicleView *v = &w->vehicles[k];
                        if (v->kind != MA2_KIND_VTOL || v->occupied || !v->has_entry || (w->team_mode && v->nation > 0 && v->nation != p->nation)) continue;
                        float d = hypotf(v->entry.x - p->pos.x, v->entry.z - p->pos.z); if (d < bd) { bd = d; best = k; } }
                    if (best < 0) { test_steer_off(); break; }
                    { const Ma2VehicleView *v = &w->vehicles[best]; static int wp; static uint32_t wv;
                      float nx = v->entry.x - v->pos.x, nz = v->entry.z - v->pos.z, nl = hypotf(nx, nz) + 1e-3f; nx /= nl; nz /= nl;
                      float sx = -nz, sz = nx;   /* the side the bots board from (they stand 1.5 m that way of the entry) */
                      /* a bot pilot's own way in (desktop 11-01): up the boarding side from behind, then under the side of the nose */
                      static const float rel[4][2] = { { 6.4f, -8.7f }, { 6.5f, 1.0f }, { 1.7f, 2.8f }, { 1.7f, 2.8f } };
                      Ma2Vec3 pts[4]; for (int q = 0; q < 4; q++) { pts[q].x = v->pos.x + sx * rel[q][0] + nx * rel[q][1]; pts[q].y = 0.f; pts[q].z = v->pos.z + sz * rel[q][0] + nz * rel[q][1]; }
                      if (wv != v->actor) { wv = v->actor; wp = 0; }
                      if (wp < 3 && hypotf(pts[wp].x - p->pos.x, pts[wp].z - p->pos.z) < 1.5f) wp++;
                      test_steer(p, pts[wp], wp == 3 ? 0.5f : 1.0f, w->time);
                      static Ma2Vec3 lp; static double lt, side_until; static int side_n;   /* stuck on the hull: a side step */
                      if (hypotf(p->pos.x - lp.x, p->pos.z - lp.z) > 0.7f) { lp = p->pos; lt = w->time; }
                      else if (w->time - lt > 1.5) { side_until = w->time + 0.5; lt = w->time; side_n++; }
                      test_key((side_n & 1) ? 'A' : 'D', w->time < side_until); }
                    if ((p->enter_target == w->vehicles[best].actor || bd < w->vehicles[best].entry_r + 1.5f) && w->time >= qn) { qn = w->time + 1.0; test_tap('Q'); }
                    t_in = 0.0; break;
                }
                if (p->kind != MA2_KIND_VTOL && !getenv("RECOMP_TEST_HCLIMB_FOOT")) break;
                if (t_in <= 0.0) { t_in = w->time; test_steer_off(); fprintf(stderr, "[TEST-HCLIMB] t=%.1f in the VTOL at y=%.1f\n", w->time, p->pos.y); }
                static const struct { double at; int w, s, shift, ctrl; const char *n; } sc_brake[] = {   /* RECOMP_TEST_HCLIMB_SCRIPT=brake: full speed, then hold reverse (11-04 VTOL stick) */
                    { 0, 0, 0, 0, 0, "idle" }, { 3, 1, 0, 0, 0, "W" }, { 9, 0, 1, 0, 0, "S" }, { 17, 0, 0, 0, 0, "idle" }, { 19, 1, 0, 0, 0, "W" }, { 22, 0, 1, 0, 0, "S" }, { 30, 0, 0, 0, 0, "idle" }, { 1e9, 0, 0, 0, 0, "end" } };
                static const struct { double at; int w, s, shift, ctrl; const char *n; } sc_climb[] = {
                    { 0, 0, 0, 0, 0, "idle" }, { 3, 1, 0, 0, 0, "W" }, { 7, 1, 0, 1, 0, "W+Shift" }, { 11, 0, 0, 0, 0, "idle" },
                    { 14, 0, 0, 1, 0, "Shift" }, { 18, 0, 0, 0, 0, "idle" }, { 21, 1, 0, 0, 1, "W+Ctrl" }, { 25, 0, 0, 0, 0, "idle" },
                    { 28, 0, 1, 1, 0, "S+Shift" }, { 32, 1, 0, 1, 0, "W+Shift" }, { 38, 0, 0, 0, 0, "idle" }, { 1e9, 0, 0, 0, 0, "end" } };
                const char *scn = getenv("RECOMP_TEST_HCLIMB_SCRIPT"); const int brake = scn && !strcmp(scn, "brake");
                #define sc (brake ? sc_brake : sc_climb)
                double tt = w->time - t_in; int ph = 0;
                while (sc[ph + 1].at <= tt) ph++;
                if (ph != phase) { phase = ph; fprintf(stderr, "[TEST-HCLIMB] t=%.1f phase %s y=%.1f\n", w->time, sc[ph].n, p->pos.y); }
                test_key('W', sc[ph].w); test_key('S', sc[ph].s); test_key(VK_SHIFT, sc[ph].shift); test_key(VK_CONTROL, sc[ph].ctrl);
                if (w->time - ylog >= (brake ? 0.1 : 0.25) || w->time < ylog) { ylog = w->time; int gk = 0; float gg = terrain_height(p->pos.x, p->pos.z, &gk);
                    fprintf(stderr, "[TEST-HCLIMB] t=%.2f %s y=%.1f gnd=%.1f vy=%.1f sp=%.2f\n", w->time, sc[ph].n, p->pos.y, gg, p->vel.y, hypotf(p->vel.x, p->vel.z)); }
                if (getenv("RECOMP_TEST_HCLIMB_DUMP")) { static double dl; if (w->time - dl >= 0.5 || w->time < dl) { dl = w->time;   /* the craft's state: which field the climb input moves */
                    uint32_t a = p->actor, cmp = gva_ok(a, 0x100) ? MEM32(a + 0x8C) : 0, beh = MEM32(p->player + 0x2F0);
                    uint32_t base[3] = { cmp, a, beh }; static const uint32_t lo[3] = { 0, 0x100, 0 }, hi[3] = { 0x400, 0xA00, 0x700 }; static const char nm[3] = { 'c', 'a', 'b' };
                    fprintf(stderr, "[HCLIMB-DUMP] t=%.1f %s y=%.2f a260=%08X a264=%08X c144=%08X c7C=%08X", w->time, sc[ph].n, p->pos.y, MEM32(a + 0x260), MEM32(a + 0x264), gva_ok(cmp, 0x150) ? MEM32(cmp + 0x144) : 0u, gva_ok(cmp, 0x150) ? MEM32(cmp + 0x7C) : 0u);
                    for (int r = 0; r < 3; r++) { if (!gva_ok(base[r], hi[r])) continue; for (uint32_t off = lo[r]; off < hi[r]; off += 4) { uint32_t u = MEM32(base[r] + off); float f = MEMF(base[r] + off);
                        if (u && (u & 0x7F800000u) && (u & 0x7F800000u) != 0x7F800000u && fabsf(f) > 1e-4f && fabsf(f) < 1e5f) fprintf(stderr, " %c%X=%.3f", nm[r], off, f); } }
                    fprintf(stderr, "\n"); } }
                break;
            }
        #undef sc
        const char *hf = getenv("RECOMP_TEST_HUMAN_FERRY");
        if (index == 0 && hf && w->time >= atof(hf))
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (!p->is_human || !p->alive || !p->actor) continue;
                const Ma2ObjectiveView *mine = NULL, *theirs = NULL;
                for (int o = 0; o < w->nobjectives; o++) {
                    const Ma2ObjectiveView *ov = &w->objectives[o];
                    if (ov->kind != MA2_OBJ_FLAG) continue;
                    if (ov->nation == p->nation) mine = ov; else theirs = ov;
                }
                static double flog, ctry, step_t, hurt_for = -1.0; static int step = -1; static Ma2Vec3 board_at;
                int carrying = theirs && theirs->carrier == p->player;
                int st = p->kind != MA2_KIND_BATTLEARMOR ? 0 : p->riding ? (carrying ? 4 : 2) : carrying ? 3 : (theirs && hypotf(theirs->pos.x - p->pos.x, theirs->pos.z - p->pos.z) < 60.f ? 5 : 1);
                if (!mine || !theirs) st = 9;
                else if (st == 3 && hypotf(mine->home.x - p->pos.x, mine->home.z - p->pos.z) < 60.f) st = 6;
                static const char *const sname[10] = { "not a BattleArmor", "on foot: lift posted", "riding to their flag", "carrying: wait for the VTOL", "riding home",
                                                       "on foot at their flag: walk onto it", "carrying at our stand: walk in", "", "", "no flags" };
                if (st == 2 && step == 1 && getenv("RECOMP_TEST_HFERRY_REACH")) ma2_brain_test_lift(p->actor, p->nation, p->pos, p->pos, w->time, 1);   /* on by the game's own claw: his lift request is done */
                if (st != step) { fprintf(stderr, "[TEST-HFERRY] t=%.1f step %d -> %d (%s) kind=%d at %.0f,%.0f\n", w->time, step, st, sname[st], p->kind, p->pos.x, p->pos.z); step = st; board_at = p->pos; step_t = w->time; }
                /* RECOMP_TEST_FERRY_HURT="frac[:delay]" (test hook, env only; PlayerRunHurt proof): `delay` s (default 4) after he
                 * clawed on, the VTOL he hangs from is set to `frac` of its health (actor+0x184, as RECOMP_TEST_VTOL_HURT), once a ride. */
                const char *fh = getenv("RECOMP_TEST_FERRY_HURT");
                if (fh && st == 2 && p->riding && w->time - step_t > 0.0 && hurt_for != step_t) {
                    float fr = 0.3f, dl = 4.f; sscanf(fh, "%f:%f", &fr, &dl);
                    for (int k = 0; k < w->nplayers && w->time - step_t >= dl; k++) {
                        const Ma2PlayerView *rv = &w->players[k];
                        if (rv->actor != p->riding || !gva_ok(rv->actor, 0x190) || rv->hp_max <= 0.f) continue;
                        MEM32(rv->actor + 0x184) = (uint32_t)(int)(fr * rv->hp_max); hurt_for = step_t;
                        fprintf(stderr, "[TEST-FHURT] t=%.1f VTOL %08X carrying the player: hp %.0f -> %d of %.0f at %.0f,%.0f, %.0f m from their flag\n", w->time, rv->actor,
                                rv->hp, (int)(fr * rv->hp_max), rv->hp_max, rv->pos.x, rv->pos.z, theirs ? hypotf(theirs->pos.x - rv->pos.x, theirs->pos.z - rv->pos.z) : -1.f);
                    }
                }
                if (st == 9) { test_steer_off(); break; }
                /* RECOMP_TEST_HBA_WALK=1 (diag, with HUMAN_FERRY): once in a BattleArmor he just walks forward on W (no lift) */
                if (getenv("RECOMP_TEST_HBA_WALK") && st != 0) { test_key(VK_SPACE, 0); test_key('W', !p->riding); break; }
                if (st == 0) {   /* on foot: into the nearest free friendly (or nobody's) BattleArmor */
                    static double qnext, blog;
                    int best = -1; float bd = 1e9f;
                    for (int k = 0; k < w->nvehicles; k++) {
                        const Ma2VehicleView *v = &w->vehicles[k];
                        if (v->kind != MA2_KIND_BATTLEARMOR || v->occupied || !v->has_entry || (w->team_mode && v->nation > 0 && v->nation != p->nation)) continue;
                        float d = hypotf(v->entry.x - p->pos.x, v->entry.z - p->pos.z);
                        if (d < bd) { bd = d; best = k; }
                    }
                    if (p->kind != MA2_KIND_PILOT || best < 0) { test_steer_off(); break; }
                    const Ma2VehicleView *v = &w->vehicles[best];
                    test_steer(p, v->entry, 0.6f * v->entry_r, w->time);
                    if ((p->enter_target == v->actor || bd < 0.8f * v->entry_r) && w->time >= qnext) { qnext = w->time + 1.5; test_tap('Q'); }
                    if (w->time - blog > 1.0) { blog = w->time; fprintf(stderr, "[TEST-HFERRY] t=%.1f pilot to BattleArmor %08X %.1f m (zone %.1f) yaw=%.0f bearing=%.0f prompt=%08X\n", w->time, v->actor, bd, v->entry_r,
                                                                      p->yaw * 57.29578f, atan2f(v->entry.x - p->pos.x, v->entry.z - p->pos.z) * 57.29578f, p->enter_target); }
                    break;
                }
                /* The nearest friendly bot VTOL. */
                const Ma2PlayerView *vt = NULL; float vd = 1e9f;
                for (int k = 0; k < w->nplayers; k++) {
                    const Ma2PlayerView *q = &w->players[k];
                    if (!q->is_bot || !q->alive || q->kind != MA2_KIND_VTOL || q->nation != p->nation) continue;
                    float d = hypotf(q->pos.x - p->pos.x, q->pos.z - p->pos.z);
                    if (d < vd) { vd = d; vt = q; }
                }
                float dy = vt ? vt->pos.y - p->pos.y : 0.f;
                Ma2Vec3 walk = p->pos; int do_walk = 0;
                if (st != 1 && st != 3) test_key(VK_SPACE, 0);   /* RECOMP_TEST_HFERRY_REACH: no jet held on */
                if (st == 1 || st == 3) {
                    /* RECOMP_TEST_HFERRY_NOPOST=1: no lift request -- he walks / jets to a VTOL holding for him (with _REACH), as a
                     * player does, so the brain's own requests (LiftHuman's ride home) are what the VTOL sees (CaptureReset proof) */
                    int nopost = getenv("RECOMP_TEST_HFERRY_NOPOST") != NULL;
                    if (st == 1 && !nopost) ma2_brain_test_lift(p->actor, p->nation, p->pos, theirs->pos, w->time, 0);
                    /* RECOMP_TEST_HFERRY_REACH=1: a VTOL holding for him 12 m off (VtolMuster's player hover) is walked to and
                     * jetted up to, as a player does (VtolTwoRiders proof); the claw is tried every 0.3 s then */
                    static double jt = -9.0; int reach = getenv("RECOMP_TEST_HFERRY_REACH") != NULL;
                    if (reach && vt && vd >= 3.f && vd < 40.f) {
                        walk = vt->pos; do_walk = 1;
                        if (vd < 15.f && (w->time - jt > 2.5 || w->time < jt)) { jt = w->time; fprintf(stderr, "[TEST-HFERRY] t=%.1f jets toward VTOL %08X %.1f m dy=%.1f\n", w->time, vt->actor, vd, dy); }
                    }
                    if (reach) test_key(VK_SPACE, w->time >= jt && w->time - jt < 0.9);
                    /* The claw is tried within 13 m / 16 m up and the game's clamp test decides (it was 8 m: on Deliverance the
                     * VTOL came down 10 m off and 8 m up -- inside the claw's reach -- and hung there 45 s untried, wbdel1). */
                    if (vt && vd < 13.f && dy > -2.f && dy < 16.f && w->time >= ctry) {
                        ctry = w->time + (reach ? 0.3 : 1.0);
                        int r = ba_clamp(p->actor);
                        fprintf(stderr, "[TEST-HFERRY] t=%.1f clamp onto VTOL %08X d=%.1f dy=%.1f -> %d\n", w->time, vt->actor, vd, dy, r);
                        if (r == 1 && !nopost) ma2_brain_test_lift(p->actor, p->nation, p->pos, p->pos, w->time, 1);
                    }
                } else if (st == 2 || st == 4) {
                    const Ma2Vec3 at = st == 2 ? theirs->pos : mine->home;
                    float d = hypotf(at.x - p->pos.x, at.z - p->pos.z), h = p->pos.y - at.y;
                    /* Set down short (PlayerRunHurt): the VTOL hangs still and low with him 3 s -- he lets go there, as a player would */
                    static double low_t; int gk = 0; float gy = terrain_height(p->pos.x, p->pos.z, &gk);
                    int low = st == 2 && vt && gk && p->pos.y - gy < 9.f && hypotf(vt->vel.x, vt->vel.z) < 4.f && d > 60.f &&
                              hypotf(p->pos.x - board_at.x, p->pos.z - board_at.z) > 80.f;   /* (not the pickup itself) */
                    if (!low) low_t = w->time; else if (w->time - low_t > 3.0 && w->time >= ctry) {
                        ctry = w->time + 1.0;
                        fprintf(stderr, "[TEST-HFERRY] t=%.1f let go where the VTOL set him down: %.0f m from their flag, %.1f m up\n", w->time, d, p->pos.y - gy);
                        ba_release(p->actor);
                    }
                    /* Over their flag he lets go as a player would, within 30 m and up to 30 m up (no fall hurts): on Deliverance the
                     * VTOL crossed their base at ~35 m over the ground, never under 8 m, and circled until it was shot down (wd_f1). */
                    if (((st == 2 && d < 30.f && h < 30.f) || (st == 4 && d < 40.f && h < 8.f)) && w->time >= ctry) {
                        ctry = w->time + 1.0;
                        fprintf(stderr, "[TEST-HFERRY] t=%.1f let go %.1f m from %s, %.1f m up\n", w->time, d, st == 2 ? "their flag" : "our stand", h);
                        ba_release(p->actor);
                    }
                } else if (st == 5) { walk = theirs->pos; do_walk = 1; }
                else if (st == 6) { walk = mine->home; do_walk = 1; }
                if (do_walk) test_steer(p, walk, 1.5f, w->time); else test_steer_off();
                if (w->time - flog > 1.0) {
                    flog = w->time;
                    fprintf(stderr, "[TEST-HFERRY] t=%.1f step %d human %.0f,%.0f,%.0f yaw=%.0f vtol %08X %.0f m dy=%.0f flag %s %.0f m\n", w->time, st,
                            p->pos.x, p->pos.y, p->pos.z, p->yaw * 57.29578f, vt ? vt->actor : 0u, vd, dy,
                            theirs->state == MA2_OBJS_CARRIED ? "carried" : theirs->state == MA2_OBJS_HOME ? "home" : "dropped",
                            hypotf(theirs->pos.x - p->pos.x, theirs->pos.z - p->pos.z));
                    if (do_walk) fprintf(stderr, "[TEST-HFERRY] t=%.1f walk bearing=%.0f %.1f m\n", w->time, atan2f(walk.x - p->pos.x, walk.z - p->pos.z) * 57.29578f,
                                         hypotf(walk.x - p->pos.x, walk.z - p->pos.z));
                }
                break;
            }
        /* RECOMP_TEST_HIJACK="t" (push W1, the 'crash ~5 s after a hijack'): from t the human NeuroHacks an
         * enemy bot 'Mech the real way. On foot he is steered into the nearest free friendly BattleArmor
         * (W/J/L/Q, as HFERRY); in it he waits while the nearest enemy bot 'Mech is made to walk up to him
         * (its order is replaced, hold fire); within 9 m the claw closes (ba_clamp, the human tick's own
         * path) and his hack bar is filled each tick while the 'Mech's bar is held at 0, so the game's own
         * minigame completes the hijack. No health is written (see below). Afterwards
         * [TEST-HIJACK] alive lines every 5 s say whether the game is still running. Off by default. */
        const char *hj = getenv("RECOMP_TEST_HIJACK");
        if (index == 0 && hj && w->time >= atof(hj))
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (!p->is_human || !p->actor) continue;
                static double hlog, qnext, ctry, done_t, alive_t; static int step = -1, hijacks;
                static uint32_t ba_actor, won, vic_pl; static double vic_t0; static Ma2Vec3 vic_p0;
                {   /* push W1b: the bot thrown out by the won round -- [TEST-EJECT] every 2 s for 40 s
                     * (kind, body class, distance moved from where it landed, order) = does it act as a pilot? */
                    static double vlog;
                    for (int k = 0; vic_pl && k < w->nplayers; k++) {
                        const Ma2PlayerView *q = &w->players[k];
                        if (q->player != vic_pl) continue;
                        if (w->time - vic_t0 > 40.0) { vic_pl = 0; break; }
                        if (vic_p0.x == 0.f && vic_p0.z == 0.f && q->kind == MA2_KIND_PILOT) vic_p0 = q->pos;
                        if (w->time - vlog >= 2.0) {
                            vlog = w->time; int bo = -1;
                            for (int j = 0; j < g_rt.nbots; j++) if (g_rt.bots[j].player == vic_pl) bo = g_rt.bots[j].order.type;
                            fprintf(stderr, "[TEST-EJECT] t=%.1f +%.0f s victim %08X deaths=%d alive=%d kind=%d actor=%08X cls=%08X at %.0f,%.0f moved=%.1f m order=%d\n",
                                    w->time, w->time - vic_t0, vic_pl, q->deaths, q->alive, q->kind, q->actor, q->actor && gva_ok(q->actor, 0x30) ? MEM32(q->actor + 0x28) : 0,
                                    q->pos.x, q->pos.z, (vic_p0.x != 0.f || vic_p0.z != 0.f) ? hypotf(q->pos.x - vic_p0.x, q->pos.z - vic_p0.z) : 0.f, bo);
                        }
                    }
                }
                if (done_t > 0.0) {   /* hijacked: report that the game keeps running */
                    if (w->time - alive_t >= 5.0) { alive_t = w->time; fprintf(stderr, "[TEST-HIJACK] t=%.1f alive %.0f s after the hijack, kind=%d actor=%08X hp=%.0f/%.0f\n", w->time, w->time - done_t, p->kind, p->actor, p->hp, p->hp_max); }
                    break;
                }
                if (!p->alive) { test_steer_off(); break; }
                int st = p->kind == MA2_KIND_PILOT ? 0 : p->kind == MA2_KIND_BATTLEARMOR ? (p->qte_active || p->clamped_to ? 2 : 1) : 3;
                if (st == 3 && ba_actor) {   /* was a BattleArmor, now drives a 'Mech: the hijack happened */
                    done_t = alive_t = w->time; hijacks++; g_test_hijack_bot = 0; test_steer_off();
                    fprintf(stderr, "[TEST-HIJACK] t=%.1f HIJACKED: human now kind=%d actor=%08X (BattleArmor was %08X)\n", w->time, p->kind, p->actor, ba_actor);
                    break;
                }
                if (st != step) {
                    fprintf(stderr, "[TEST-HIJACK] t=%.1f step %d -> %d kind=%d actor=%08X at %.0f,%.0f\n", w->time, step, st, p->kind, p->actor, p->pos.x, p->pos.z);
                    if (st == 2 && env_on("RECOMP_TEST_HIJACK_FREES")) g_heap_free_log_until = GetTickCount() + 8000u;   /* [HEAP-FREE] for 8 s */
                    step = st;
                }
                if (st == 3) { test_steer_off(); break; }   /* started in a vehicle that is not a BattleArmor: nothing to do */
                if (st == 0) {   /* on foot: into the nearest free friendly (or nobody's) BattleArmor */
                    int hv0 = 0;
                    for (int k = 0; won && k < w->nvehicles; k++) {   /* W1b: a won round left the 'Mech empty: walk in on foot (Q in a BattleArmor only climbs out) */
                        const Ma2VehicleView *v = &w->vehicles[k];
                        if (v->actor != won || v->occupied || !v->has_entry) continue;
                        float bd = hypotf(v->entry.x - p->pos.x, v->entry.z - p->pos.z);
                        if (bd > 80.f) break;
                        {   /* blocked by the hull (no progress for 1.5 s): strafe right for 1.5 s while walking */
                            static double prog_t, strafe_until; static float prog_d = 1e9f;
                            if (bd < prog_d - 0.5f) { prog_d = bd; prog_t = w->time; }
                            else if (w->time - prog_t > 1.5) { strafe_until = w->time + 1.5; prog_t = w->time; prog_d = bd; }
                            test_key('D', w->time < strafe_until);
                        }
                        test_steer(p, v->entry, 0.6f * v->entry_r, w->time);
                        if ((p->enter_target == v->actor || bd < 0.8f * v->entry_r || bd < 6.f) && w->time >= qnext) { qnext = w->time + 1.5; test_tap('Q'); }
                        if (w->time - hlog > 1.0) { hlog = w->time; fprintf(stderr, "[TEST-HIJACK] t=%.1f pilot boards the hacked 'Mech %08X %.1f m\n", w->time, v->actor, bd); }
                        hv0 = 1; break;
                    }
                    if (hv0) break;
                    int best = -1; float bd = 1e9f;
                    for (int k = 0; k < w->nvehicles; k++) {
                        const Ma2VehicleView *v = &w->vehicles[k];
                        if (v->kind != MA2_KIND_BATTLEARMOR || v->occupied || !v->has_entry || (w->team_mode && v->nation > 0 && v->nation != p->nation)) continue;
                        float d = hypotf(v->entry.x - p->pos.x, v->entry.z - p->pos.z);
                        if (d < bd) { bd = d; best = k; }
                    }
                    if (best < 0) { test_steer_off(); if (w->time - hlog > 5.0) { hlog = w->time; fprintf(stderr, "[TEST-HIJACK] t=%.1f no free BattleArmor\n", w->time); } break; }
                    const Ma2VehicleView *v = &w->vehicles[best];
                    test_steer(p, v->entry, 0.6f * v->entry_r, w->time);
                    if ((p->enter_target == v->actor || bd < 0.8f * v->entry_r) && w->time >= qnext) { qnext = w->time + 1.5; test_tap('Q'); }
                    if (w->time - hlog > 2.0) { hlog = w->time; fprintf(stderr, "[TEST-HIJACK] t=%.1f pilot to BattleArmor %08X %.1f m\n", w->time, v->actor, bd); }
                    break;
                }
                ba_actor = p->actor;
                {   /* a won round throws the 'Mech's pilot out: the empty 'Mech is boarded like any vehicle (Q) */
                    if (st == 2 && p->clamped_to) {   /* the victim = the player whose actor is the clamped 'Mech (not the next bot called) */
                        won = p->clamped_to;
                        for (int k = 0; k < w->nplayers; k++)
                            if (w->players[k].actor == won && !w->players[k].is_human) { vic_pl = w->players[k].player; vic_t0 = w->time; vic_p0 = (Ma2Vec3){0, 0, 0}; }
                    }
                    int hv = -1;
                    for (int k = 0; won && st == 1 && k < w->nvehicles; k++)
                        if (w->vehicles[k].actor == won && !w->vehicles[k].occupied && w->vehicles[k].has_entry) hv = k;
                    if (hv >= 0) {
                        const Ma2VehicleView *v = &w->vehicles[hv];
                        float bd = hypotf(v->entry.x - p->pos.x, v->entry.z - p->pos.z);
                        test_steer(p, v->entry, 0.6f * v->entry_r, w->time);
                        if ((p->enter_target == v->actor || bd < 40.f) && w->time >= qnext) { qnext = w->time + 1.0; test_tap('Q'); }   /* W1b: Q climbs out of the BattleArmor; the pilot walks in */
                        if (w->time - hlog > 1.0) { hlog = w->time; fprintf(stderr, "[TEST-HIJACK] t=%.1f boards the hacked 'Mech %08X %.1f m\n", w->time, v->actor, bd); }
                        break;
                    }
                }
                /* No health writes: restoring a unit's HP after its death handler ran leaves a zombie whose parts
                 * are already queued for deferred deletion (hj1-4,7 [BOT-FAULT] in the B5D60/B4EF0 tree walk). */
                const Ma2PlayerView *t = NULL; float td = 1e9f;
                for (int k = 0; k < w->nplayers; k++) {
                    const Ma2PlayerView *q = &w->players[k];
                    if (!q->is_bot || !q->alive || q->kind != MA2_KIND_MECH || !q->actor || (w->team_mode && q->nation == p->nation)) continue;
                    float d = hypotf(q->pos.x - p->pos.x, q->pos.z - p->pos.z);
                    if (q->player == g_test_hijack_bot) d -= 1e6f;   /* keep the one already called */
                    if (d < td) { td = d; t = q; }
                }
                if (!t) { test_steer_off(); break; }
                if (g_test_hijack_bot != t->player) { g_test_hijack_bot = t->player; fprintf(stderr, "[TEST-HIJACK] t=%.1f calls enemy 'Mech %08X (player %08X)\n", w->time, t->actor, t->player); }
                g_test_hijack_to = p->pos;
                float d = hypotf(t->pos.x - p->pos.x, t->pos.z - p->pos.z);
                {   /* both walk to meet */
                    if (st == 1 && d > 7.f) test_steer(p, t->pos, 6.f, w->time); else test_steer_off();
                }
                if (st == 1 && d < 9.f && w->time >= ctry) {
                    ctry = w->time + 1.0;
                    int r = ba_clamp(p->actor);
                    fprintf(stderr, "[TEST-HIJACK] t=%.1f clamp onto %08X d=%.1f -> %d\n", w->time, t->actor, d, r);
                }
                if (st == 2) ctry = w->time + 2.0;   /* no re-clamp straight after a hack round (a player would not) */
                if (st == 2 && p->qte_active) {
                    /* The minigame, answered like a player: each cue's key (the game's own prompt map,
                     * ma2_pc_qte_caption: 4 A, 1 X, 2 Y, 0x40 B, 0x10 LT, 0x20 RT) tapped 0.25 s after it
                     * shows. The called 'Mech's bot does not answer its side (its order is replaced). */
                    static uint8_t last_req; static double cue_t, tap_t;
                    uint8_t req = (uint8_t)(p->qte_required & 0x7F), miss = (uint8_t)(req & ~p->qte_pressed & 0x7F);
                    if (req != last_req) { last_req = req; cue_t = w->time; }
                    {   /* test-only: the race is decided for the human (his bar +0x18 set to its target +0xA8, the
                         * clamped 'Mech's held at 0) so the game's own completion path runs every time */
                        uint32_t hq = MEM32(p->actor + 0x274), oa = p->clamped_to ? p->clamped_to : t->actor;
                        uint32_t tq = gva_ok(oa, 0x280) ? MEM32(oa + 0x274) : 0;
                        if (gva_ok(tq, 0xC8)) MEMF(tq + 0x18) = 0.f;
                        static double q0; static uint32_t q0_hq;   /* like a player: the bar fills after ~3 s of the round */
                        if (hq != q0_hq) { q0_hq = hq; q0 = w->time; }
                        if (gva_ok(hq, 0xC8) && MEMF(hq + 0xA8) > 0.f && w->time - q0 >= 3.0) MEMF(hq + 0x18) = MEMF(hq + 0xA8);
                    }
                    if (miss && w->time - cue_t >= 0.12 && w->time - tap_t >= 0.1) {
                        static const struct { uint8_t bit; int act, def; } km[] = {
                            { 4, PC_A, VK_SPACE }, { 1, PC_X, 'R' }, { 2, PC_Y, 'Q' }, { 0x40, PC_B, 'F' },
                            { 0x10, PC_LEFT_TRIGGER, 'C' }, { 0x20, PC_RIGHT_TRIGGER, 'V' } };
                        for (int k = 0; k < 6; k++) if (miss & km[k].bit) { test_tap(pc_binding_get((enum PcBinding)km[k].act, km[k].def)); tap_t = w->time; break; }
                    }
                }
                if (w->time - hlog > 1.0) { hlog = w->time; fprintf(stderr, "[TEST-HIJACK] t=%.1f step %d 'Mech %08X %.1f m qte=%d prog=%.2f opp=%.2f\n", w->time, st, t->actor, d, p->qte_active, p->qte_progress, p->qte_opp_progress); }
                break;
            }
        if (index == 0 && vf && w->time >= atof(vf))
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (!p->is_bot || p->kind != MA2_KIND_VTOL || !gva_ok(p->actor, 0x190)) continue;
                if ((int)MEM32(p->actor + 0x184) > 5) MEM32(p->actor + 0x184) = 5;
            }
        /* RECOMP_TEST_VTOL_HURT="first:period:frac" (W6 push, env-only test hook, default off): from `first` s, and then
         * every `period` s per bot VTOL, its health is set to frac of its maximum (actor+0x184) -- it goes for the helipad
         * (heal_at is 0.20-0.45), so the pad run can be timed ([C7] vtpad) without waiting for a fight to wound it. */
        const char *hh = getenv("RECOMP_TEST_VTOL_HURT");
        if (index == 0 && hh) {
            static double hfirst = -1, hper = 70, hfrac = 0.15; static uint32_t hact[8]; static double hlast[8];
            if (hfirst < 0) { float a = 60, b = 70, c2 = 0.15f; sscanf(hh, "%f:%f:%f", &a, &b, &c2); hfirst = a; hper = b; hfrac = c2; }
            for (int i = 0; i < w->nplayers && w->time >= hfirst; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (!p->is_bot || p->kind != MA2_KIND_VTOL || !gva_ok(p->actor, 0x190) || p->hp_max <= 0.f) continue;
                int slot = -1;
                for (int k = 0; k < 8; k++) if (hact[k] == p->actor) slot = k;
                if (slot < 0) for (int k = 0; k < 8 && slot < 0; k++) if (!hact[k]) { slot = k; hact[k] = p->actor; hlast[k] = -1e9; }
                if (slot < 0 || w->time - hlast[slot] < hper || (int)MEM32(p->actor + 0x184) <= (int)(hfrac * p->hp_max) + 1) continue;
                MEM32(p->actor + 0x184) = (uint32_t)(int)(hfrac * p->hp_max); hlast[slot] = w->time;
                fprintf(stderr, "[TEST-VHURT] t=%.1f vtol=%08X hp %.0f -> %d of %.0f at %.0f,%.0f,%.0f\n", w->time, p->actor, p->hp, (int)(hfrac * p->hp_max), p->hp_max, p->pos.x, p->pos.y, p->pos.z);
            }
        }
        if (getenv("RECOMP_TEST_TURRET_DRIVE"))   /* the old tank_repair on the gun this bot sits in: it drives it */
            for (int i = 0; i < w->nplayers; i++) {
                const Ma2PlayerView *p = &w->players[i];
                if (p->player != player || p->kind != MA2_KIND_TURRET || !is_kind(p->actor, g_cls.tank)) continue;
                uint32_t comp = MEM32(p->actor + 0x8C);
                if (!gva_ok(comp, 0x200) || MEMF(comp + 0xBC) != 0.0f || MEMF(comp + 0xC0) != 0.0f) continue;
                MEMF(comp + 0xBC) = MEMF(comp + 0xC0) = MEMF(comp + 0xC4) = 20.0f; MEMF(comp + 0x98) = 16.0f; MEMF(comp + 0xD0) = 40.0f;
                fprintf(stderr, "[TEST-TURRET-DRIVE] t=%.1f bot=%d gun %08X made drivable\n", w->time, index, p->actor);
            }
    }
    if (env_on("RECOMP_BOT_DIAG")) { human_hp_watch(); death_watch(); camera_census(); bot_summary(); }
    defense_probe();   /* RECOMP_DEFENSE_PROBE only; once per step */
    if (!b->brain) b->brain = ma2_brain_create(index, b->skill, GetTickCount() + (uint32_t)index * 7919u);
    {
        const Ma2World *w = &g_rt.world;
        int self = -1;
        for (int i = 0; i < w->nplayers; i++) if (w->players[i].player == player) { self = i; break; }
        if (self < 0 || !b->brain) goto done;
        {   /* EXPERIMENT (push W1): RECOMP_TEST_HACK_HOLD=<s> -- a bot thrown out of its 'Mech by a NeuroHack
             * (kind MECH with a hack running -> PILOT) is left entirely alone for <s> s: no brain, no ctrl. */
            static double qte_seen[MAX_BOTS], hold_until[MAX_BOTS]; static int was_kind[MAX_BOTS];
            static float hold = -1.f;
            if (hold < 0.f) { const char *e = getenv("RECOMP_TEST_HACK_HOLD"); hold = e && *e ? (float)atof(e) : 0.f; }
            const Ma2PlayerView *mv = &w->players[self];
            if (mv->qte_active || mv->enemy_rider) qte_seen[index] = w->time;
            if (hold > 0.f && was_kind[index] == MA2_KIND_MECH && mv->kind == MA2_KIND_PILOT && w->time - qte_seen[index] < 2.0) {
                hold_until[index] = w->time + hold;
                fprintf(stderr, "[TEST-HACK-HOLD] t=%.1f bot=%d hacked out of its 'Mech: left alone %.0f s\n", w->time, index, hold);
            }
            was_kind[index] = mv->kind;
            if (w->time < hold_until[index]) goto done;
        }
        uint32_t a = w->players[self].actor;
        if (a && (MEM8(a + 0x1E) & 3)) {               /* simulated here, never a network proxy */
            if (env_on("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-AUTH] bot=%d actor=%08X ent1C=%08X -> local\n", index, a, MEM32(a + 0x1C));
            MEM8(a + 0x1E) &= (uint8_t)~3u;
        }
        {
            Ma2Senses sn; memset(&sn, 0, sizeof sn);
            sn.bld_ray = sn_bld_ray; sn.bld_get = sn_bld_get; sn.los = sn_los; sn.clearance = sn_clear; sn.ground = sn_ground; sn.surface = sn_surface; sn.roofs = sn_roofs;
            sn.nin = b->nin; memcpy(sn.in, b->ev_in, sizeof sn.in);
            sn.nout = b->nout; memcpy(sn.out, b->ev_out, sizeof sn.out);
            b->nin = b->nout = 0;
            g_rec_cur = index;
            { double bt0 = perf_on ? perf_now_ms() : 0.0, ct0 = c7_t0();
              ma2_brain_tick(b->brain, w, self, g_rt.nav, &b->status, &sn, &b->order);
              c7_ms("bots", "brain", ct0);   /* all bots' brain time per frame */
              if (perf_on) g_perf_brain_ms += perf_now_ms() - bt0; }
            if (def_probe_on() && defense_test_tap(index, w->players[self].kind, w->time)) ctrl_tap(&b->ctrl, env_int("RECOMP_DEFENSE_TEST_CMD", 12), 3);
            {   /* test hook RECOMP_TEST_CLOAK="bot<i>:<t0>" or "all:<t0>" (F5 proof): from t0 bot i (all: every bot in a camo body) keeps its camo up -- it taps
                 * its real defence (command 12) whenever the system is idle and charged; the game's own
                 * visibility and reveal rules run as always. Pair with a camo body (RECOMP_BOT_FAV_KINDS). */
                static int ci = -2; static float ct0; static double cnext[MAX_BOTS];
                if (ci == -2) { const char *e = getenv("RECOMP_TEST_CLOAK"); ci = -1; if (e && sscanf(e, "all:%f", &ct0) == 1) ci = -3; else if (e && sscanf(e, "bot%d:%f", &ci, &ct0) != 2) ci = -1; }
                if ((ci == index || (ci == -3 && w->players[self].defense_type == MA2_DEFT_CAMO)) && w->time >= ct0 && w->time >= cnext[index] && w->players[self].actor && def_own_ready(w->players[self].actor, w->time)) {
                    ctrl_tap(&b->ctrl, 12, 3); cnext[index] = w->time + 1.0;
                    fprintf(stderr, "[TEST-CLOAK] t=%.1f bot=%d raises its defence (type %d)\n", w->time, index, w->players[self].defense_type);
                }
                /* RECOMP_TEST_CLOAK_CLOSE=<m> (F5 rolls): the batch-1 cases never rolled -- the camo bodies are
                 * tanks, which fight 160 m+ out, and the spotting ranges are 12-55 m (x1.6 shimmering). With it,
                 * a test bot whose camo is up walks to <m> m from the nearest enemy and holds fire (firing
                 * would reveal it), so the enemies' per-skill rolls actually happen. */
                static float cclose = -1.f;
                if (cclose < 0.f) { const char *e = getenv("RECOMP_TEST_CLOAK_CLOSE"); cclose = e && *e ? (float)atof(e) : 0.f; }
                if (cclose > 0.f && (ci == index || (ci == -3 && w->players[self].defense_type == MA2_DEFT_CAMO)) && w->time >= ct0 && w->players[self].cloak) {
                    const Ma2PlayerView *me = &w->players[self], *best = NULL; float bd = 1e9f;
                    for (int j = 0; j < w->nplayers; j++) {
                        const Ma2PlayerView *q = &w->players[j];
                        if (j == self || !q->alive || !q->actor || (w->team_mode && q->nation == me->nation)) continue;
                        float d = hypotf(q->pos.x - me->pos.x, q->pos.z - me->pos.z);
                        if (d < bd) { bd = d; best = q; }
                    }
                    if (best) {
                        Ma2Order *o = &b->order;
                        o->type = MA2_ORD_TRAVEL; o->point = best->pos; o->stop = cclose; o->target = 0; o->fire = 0; o->engage = 0;
                        o->hold_fire = 1; o->standoff = 0; o->ndanger = 0; o->dodge = 0; o->dodge_now = 0; o->jump = 0;
                        c7_add("cloak", "test_close", 1);
                    }
                }
            }
        }
        {   /* test hook RECOMP_BOT_TEST_PARK="k:t": from time t, bot k (in a vehicle) drives to a point 35 m
             * ahead on another travelling bot's route and stops there for 40 s (a unit parked in the way). */
            static int pk = -2; static float pt0; static double until; static Ma2Vec3 spot; static int victim = -1;
            if (pk == -2) { const char *e = getenv("RECOMP_BOT_TEST_PARK"); pk = -1; if (e && sscanf(e, "%d:%f", &pk, &pt0) != 2) pk = -1; }
            if (pk == index && w->players[self].kind != MA2_KIND_PILOT && w->time >= pt0) {
                if (victim < 0) {
                    for (int j = 0; j < g_rt.nbots && victim < 0; j++) {
                        const BotCtrl *cj = &g_rt.bots[j].ctrl;
                        if (j == index || !g_rt.bots[j].player || cj->mode != CM_MOVE || !cj->have_path) continue;
                        float left = 35.f; Ma2Vec3 a = w->players[self].pos; int found = 0;
                        for (int jj = 0; jj < w->nplayers; jj++) if (w->players[jj].player == g_rt.bots[j].player) a = w->players[jj].pos;
                        for (int q = cj->path.next; q < cj->path.n; q++) {
                            float sl = hypotf(cj->path.pt[q].x - a.x, cj->path.pt[q].z - a.z);
                            if (sl >= left) { spot = a; spot.x += (cj->path.pt[q].x - a.x) * left / sl; spot.z += (cj->path.pt[q].z - a.z) * left / sl; found = 1; break; }
                            left -= sl; a = cj->path.pt[q];
                        }
                        if (found) { victim = j; until = w->time + 60.0; fprintf(stderr, "[TEST-PARK] t=%.1f bot=%d parks at %.0f,%.0f on bot %d's route\n", w->time, index, spot.x, spot.z, j); }
                    }
                }
                if (victim >= 0 && w->time < until) {
                    memset(&b->order, 0, sizeof b->order);
                    b->order.type = MA2_ORD_TRAVEL; b->order.point = spot; b->order.stop = 3.f;
                }
            }
        }
        if (g_test_hijack_bot && g_test_hijack_bot == player && w->players[self].kind == MA2_KIND_MECH) {
            /* RECOMP_TEST_HIJACK: the called 'Mech walks up to the human's BattleArmor and stands, holding fire */
            memset(&b->order, 0, sizeof b->order);
            b->order.type = MA2_ORD_TRAVEL; b->order.point = g_test_hijack_to; b->order.stop = 4.f; b->order.hold_fire = 1;
        }
        memset(&b->status, 0, sizeof(b->status));
        { double ct0 = perf_on ? perf_now_ms() : 0.0;
          ctrl_tick(index, b, w, self);
          if (perf_on) g_perf_ctrl_ms += perf_now_ms() - ct0; }
        if (env_on("RECOMP_BOT_DIAG")) bot_diag(index, b, w, self);
        { double rt0 = c7_t0(); rec_bot_step(index, b, w, self); c7_ms("rec", "rec", rt0); }
        g_rec_cur = -1;
        if (env_on("RECOMP_BOT_DIAG") && GetTickCount() - b->ai_diag >= 1000) {
            b->ai_diag = GetTickCount();
            const Ma2PlayerView *me = &w->players[self];
            uint32_t ai = a ? actor_ai(a) : 0;
            const Ma2PlayerView *t = NULL;
            if (b->ctrl.tgt) ctrl_target_alive(w, b->ctrl.tgt, &t);
            if (t && b->ctrl.engaged) { if (t->is_human) b->tgt_human++; else b->tgt_bot++; }
            fprintf(stderr, "[BOT-AI] bot=%d kind=%d mode=%u awake=%u target=%08X tkind=%d human=%d tdist=%.0f hp=%.0f/%.0f eng=%d los=%d ord=%d thuman=%u tbot=%u\n",
                    index, me->kind, ai ? MEM32(ai + 0x12C) : 99u, ai ? MEM8(ai + 0x90) : 0u, ai ? MEM32(ai + 0xB4) : 0u, t ? t->kind : -1, t ? t->is_human : 0,
                    t ? hdist2(t->pos, me->pos) : -1.f, me->hp, me->hp_max, b->ctrl.engaged, b->ctrl.fl_clear, b->order.type, b->tgt_human, b->tgt_bot);
        }
    }
done:
    g_rec_cur = -1; g_cur_bot = -1;
    if (hitch_on) {   /* one bot's share of this step's tick; the first bot after a new step carries the world view and nav */
        QueryPerformanceCounter(&hq1);
        double tot = (double)(hq1.QuadPart - hq0.QuadPart) * 1000.0 / (double)hq_f.QuadPart;
        if (tot >= 3.0) fprintf(stderr, "[HITCH-BOT] t=%+.3fs bot=%d tick=%.1fms world=%.1f nav=%.1f ensure_active=%.1f plan=%.1f(%u) rays=%.1f(%u) los=%.1f brain=%.1f ctrl=%.1f\n", nv2a_hitch_rel_s(), index, tot, hs_world, hs_nav, hs_act,
                g_perf_plan_ms - hp0_plan, g_perf_plans - hp0_plans, g_perf_ray_ms - hp0_ray, g_perf_rays - hp0_rays, g_perf_los_ms - hp0_los, g_perf_brain_ms - hp0_br, g_perf_ctrl_ms - hp0_ct);
    }
    if (perf_on) {
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
        bot_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart; ticks++;
        if (!q_last.QuadPart) q_last = q1;
        double span = (double)(q1.QuadPart - q_last.QuadPart) / (double)qf.QuadPart;
        if (span >= 5.0) {
            fprintf(stderr, "[BOT-PERF] t=%.0f bots %.2f ms per second of play (%.1f%% of a core), world view %.2f ms/s, %d player ticks/s, rays %d/s (%.1f ms), los %d/s (%.1f ms), plans %d/s (%.1f ms), brain %.1f ms, ctrl %.1f ms\n",
                    g_rt.world.time, bot_ms / span, bot_ms / span / 10.0, world_ms / span, (int)(ticks / span), (int)(g_perf_rays / span), g_perf_ray_ms / span, (int)(g_perf_los / span), g_perf_los_ms / span, (int)(g_perf_plans / span), g_perf_plan_ms / span, g_perf_brain_ms / span, g_perf_ctrl_ms / span);
            g_perf_ray_ms = g_perf_los_ms = g_perf_plan_ms = g_perf_brain_ms = g_perf_ctrl_ms = 0.0;
            bot_ms = world_ms = 0.0; ticks = 0; q_last = q1; g_perf_rays = g_perf_los = g_perf_plans = 0;
        }
    }
    regs_restore(&regs);
}
