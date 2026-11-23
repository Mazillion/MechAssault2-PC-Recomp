/* MechAssault 2 offline bots - New Game screen rows and settings (ui module).
 *
 * Implements from shared/ma2_bot_api.h:
 *   ma2_bots_config(), ma2_bots_config_set(), ma2_bots_newgame_update()
 * and one extra helper for the localisation hook:
 *   uint32_t ma2_bots_localized(uint32_t id, uint32_t original);
 *
 * Drop-in location: src/ma2_bots_ui.c (include path must reach src/ for
 * "recomp/gen/..." and "pc_settings.h", and the directory holding
 * ma2_bot_api.h). Guest calls follow src/pc_gui_bridge.c: registers are saved,
 * arguments are pushed on the guest stack, RECOMP_ABI_CALL runs the original
 * function, registers are restored. Everything here runs on the guest thread
 * (the hook sits at the entry of sub_226587).
 *
 * Verified guest ABIs (disassembly of game_files/ma2.xbe, see HANDOFF.md):
 *   sub_00208C45  add barrel item   ebx=barrel, eax=WIDE text ptr,
 *                 stack (value, secondary=-1, flag=1), callee pops 12,
 *                 returns eax = new item index. Same call the Time Limit /
 *                 Kill-Scores items use in sub_225A63 (after sub_C1990 turned
 *                 a narrow string into eax).
 *   sub_00208C9E  same, but eax = NARROW char* (it calls sub_C1990 itself).
 *   sub_00208BF9  select by value   eax=barrel, stack (value, notify),
 *                 callee pops 8, al = 1 if found. notify=0 is silent.
 *   sub_00208BD0  find index        edx=barrel, stack (value), pops 4, eax=index/-1.
 *   sub_0020B21F  eax=barrel -> eax = value of the selected item.
 *   sub_0001B7F0  visibility        eax=widget, cl=visible: flags(+0x74) bit1.
 *                 (trivial, so done inline below)
 *   sub_001CAA80  find by action    ecx=form, edx=action -> eax (ma2_gui_find).
 *   sub_001639DA  text id -> wide   edi=id -> eax (this is the text-manager
 *                 lookup that ends in ma2_pc_localized, so shipped ids AND our
 *                 0x7002xxxx ids resolve through the exact same path).
 *   sub_001FF864  set widget text   esi=widget, stack (wide ptr) (ma2_gui_text).
 */
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"
#include "pc_settings.h"
#include "ma2_bot_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ma2_bot_env.h"   /* getenv, cached (the bot code asks every tick) */
#include "ma2_bot_c7.h"    /* C7 switches + counters */
#include "ma2_split_slots.h" /* split screen: room for bots next to the humans */
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define esp g_esp

/* ---- settings accessors added to pc_native_settings.cpp (see HANDOFF.md) ---- */
int  ma2_pc_ini_get_int(const char *section, const char *key, int fallback);
void ma2_pc_ini_set_int(const char *section, const char *key, int value);

/* ---- Shipped text ids (Persistent.mgText, all six languages) ---- */
#define TEXT_SKILL_GREEN    0x3AC   /* Green   */
#define TEXT_SKILL_REGULAR  0x3AD   /* Regular */
#define TEXT_SKILL_VETERAN  0x3AE   /* Veteran */
#define TEXT_SKILL_ELITE    0x3AF   /* Elite   */
#define TEXT_OFF            0xBB8   /* Off     */
/* ---- Our ids (resolved by ma2_bots_localized) ---- */
#define TEXT_ROW_BASE       0x70020000u
#define TEXT_ROW_BOTS       (TEXT_ROW_BASE + 1)
#define TEXT_ROW_RED        (TEXT_ROW_BASE + 2)
#define TEXT_ROW_BLUE       (TEXT_ROW_BASE + 3)
#define TEXT_ROW_SKILL      (TEXT_ROW_BASE + 4)
#define TEXT_SKILL_MIXED    (TEXT_ROW_BASE + 5)   /* Mixed (our own word: the game has none) */
#define TEXT_SKILL_DRAGOON  (TEXT_ROW_BASE + 6)   /* Dragoon (experimental, past Elite) */

#define ACTION_RED 910
#define ACTION_BLUE 920
#define ACTION_SKILL 930
#define ROW_PITCH 29

/* ============================ configuration ============================ */
static Ma2BotConfig g_cfg;
static int g_cfg_loaded;
/* The editor's Test launch (scenario/ma2_scenario.c) plays with its own bot counts and skill:
 * 1 = those values are in g_cfg, held in memory only (nothing reaches pc_settings.ini);
 * 2 = the test match has started, the player's saved values come back at the next New Game. */
static int g_cfg_test;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int env_or(const char *name, int v) { const char *s = getenv(name); return s && *s ? atoi(s) : v; }

/* Total players cap (humans + bots). RECOMP_BOTS_MAX=12 raises it. */
static int total_cap(void) { return clampi(env_or("RECOMP_BOTS_MAX", 12), 2, MA2_BOT_MAX_PLAYERS); }   /* 6v6 */

/* A team holds at most half the players: MechAssault 2's sides are 6 strong, humans included. */
static int side_cap(void) { return total_cap() / 2; }

static void config_sanitize(Ma2BotConfig *c) {
    int max_each = total_cap() - 1;
    c->red = clampi(c->red, 0, side_cap());
    c->blue = clampi(c->blue, 0, side_cap());
    c->ffa = clampi(c->ffa, 0, max_each);
    c->skill = clampi(c->skill, MA2_SKILL_RECRUIT, MA2_SKILL_DRAGOON);
    c->autobalance = c->autobalance != 0;
}

const Ma2BotConfig *ma2_bots_config(void) {
    if (!g_cfg_loaded) {
        /* W-30 (D-1): a fresh install starts team games 4 v 4 (3 Red + 4 Blue bots, the player on the short
         * side) and free-for-all with 5 bots, skill Mixed. A row the player sets to Off is written as 0 and kept. */
        g_cfg.red = ma2_pc_ini_get_int("Bots", "Red", 3);
        g_cfg.blue = ma2_pc_ini_get_int("Bots", "Blue", 4);
        g_cfg.ffa = ma2_pc_ini_get_int("Bots", "FFA", 5);
        g_cfg.skill = ma2_pc_ini_get_int("Bots", "Skill", MA2_SKILL_MIXED);
        g_cfg.autobalance = ma2_pc_ini_get_int("Bots", "AutoBalance", 1);
        /* Environment overrides take priority (like every other pc_settings key). */
        g_cfg.red = env_or("RECOMP_BOTS_RED", g_cfg.red);
        g_cfg.blue = env_or("RECOMP_BOTS_BLUE", g_cfg.blue);
        g_cfg.ffa = env_or("RECOMP_BOTS_FFA", g_cfg.ffa);
        g_cfg.skill = env_or("RECOMP_BOTS_SKILL", g_cfg.skill);
        g_cfg.autobalance = env_or("RECOMP_BOTS_AUTOBALANCE", g_cfg.autobalance);
        config_sanitize(&g_cfg);
        g_cfg_loaded = 1;
    }
    return &g_cfg;
}

void ma2_bots_config_set(const Ma2BotConfig *c) {
    Ma2BotConfig n = *c;
    (void)ma2_bots_config();                 /* make sure the old value is loaded */
    config_sanitize(&n);
    if (!memcmp(&n, &g_cfg, sizeof n)) return;
    g_cfg = n;
    if (g_cfg_test) return;                  /* a Test launch's values are never saved */
    ma2_pc_ini_set_int("Bots", "Red", n.red);
    ma2_pc_ini_set_int("Bots", "Blue", n.blue);
    ma2_pc_ini_set_int("Bots", "FFA", n.ffa);
    ma2_pc_ini_set_int("Bots", "Skill", n.skill);
    ma2_pc_ini_set_int("Bots", "AutoBalance", n.autobalance);
}

/* Test launch values (negative = keep the player's own). */
void ma2_bots_test_config(int red, int blue, int ffa, int skill) {
    Ma2BotConfig n = *ma2_bots_config();
    if (red >= 0) n.red = red;
    if (blue >= 0) n.blue = blue;
    if (ffa >= 0) n.ffa = ffa;
    if (skill >= 0) n.skill = skill;
    config_sanitize(&n);
    g_cfg = n; g_cfg_test = 1;
}

/* ============================ localisation ============================ */
/* Xbox XC_LANGUAGE: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian.
 * The kernel layer currently always reports English; the game's text manager
 * is the authority, so this follows the same setting it reads. */
#ifndef MA2_BOTS_STANDALONE
extern uint32_t __stdcall xbox_ExQueryNonVolatileSetting(uint32_t index, uint32_t *type, void *value,
                                                         uint32_t length, uint32_t *result_length);
#endif
static int bots_language(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    int lang = MA2_LANG_ENGLISH;
    const char *forced = getenv("RECOMP_BOT_LANGUAGE");          /* 0..5, Ma2Language order (testing) */
    if (forced && *forced) {
        lang = clampi(atoi(forced), 0, MA2_LANG_COUNT - 1);
    } else {
#ifndef MA2_BOTS_STANDALONE
        uint32_t value = 1, type = 0, len = 0;
        if (xbox_ExQueryNonVolatileSetting(0x07 /*XC_LANGUAGE*/, &type, &value, sizeof value, &len) == 0) {
            static const int map[7] = { MA2_LANG_ENGLISH, MA2_LANG_ENGLISH, MA2_LANG_JAPANESE,
                                        MA2_LANG_GERMAN, MA2_LANG_FRENCH, MA2_LANG_SPANISH, MA2_LANG_ITALIAN };
            lang = value < 7 ? map[value] : MA2_LANG_ENGLISH;
        }
#endif
    }
    return cached = lang;
}

/* Called from ma2_pc_localized (pc_prompts.cpp) for ids 0x70020000..0x7002FFFF.
 * Returns a guest wide-string pointer, or `original` if the id is not ours. */
uint32_t ma2_bots_localized(uint32_t id, uint32_t original) {
    static const int text_for[7] = { -1, MA2_TEXT_BOTS, MA2_TEXT_RED_BOTS, MA2_TEXT_BLUE_BOTS, MA2_TEXT_BOT_SKILL, MA2_TEXT_SKILL_MIXED, MA2_TEXT_SKILL_DRAGOON };
    if (id < TEXT_ROW_BASE || id >= TEXT_ROW_BASE + 0x10000) return original;
    unsigned n = id - TEXT_ROW_BASE;
    if (n >= 1 && n <= 6) {
        const wchar_t *w = ma2_bot_text(text_for[n], bots_language());
        if (w) return ma2_pc_wide(w);
    }
    return original;
}

/* The New Game update writes its description (action 700) every frame (recomp_0058.c loc_0022691C); while the Bot Skill
 * row has focus that write shows the skill's line instead (set each frame by ma2_bots_newgame_update). */
static uint32_t g_desc_widget, g_desc_text;
uint32_t ma2_bots_desc_override(uint32_t widget) { return widget && widget == g_desc_widget ? g_desc_text : 0; }

/* ============================ guest calls ============================ */
typedef struct GuiRegisters {
    uint32_t a, c, d, b, s, i, sp, bp, seh;
    RecompXmm x[8]; double fp[8]; int top;
    uint16_t fp_control, fp_cc; int fp_cmp, df;
} GuiRegisters;
static GuiRegisters save_registers(void) {
    GuiRegisters r = { eax, ecx, edx, ebx, esi, edi, esp, g_ebp, g_seh_ebp,
        { g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7 }, {0}, g_fp_top };
    memcpy(r.fp, g_fp_stack, sizeof(r.fp)); r.fp_control = g_fp_control_word; r.fp_cc = g_fp_cc;
    r.fp_cmp = g_fp_cmp; r.df = g_df; return r;
}
static void restore_registers(GuiRegisters *r) {
    eax = r->a; ecx = r->c; edx = r->d; ebx = r->b; esi = r->s; edi = r->i; esp = r->sp;
    g_ebp = r->bp; g_seh_ebp = r->seh;
    g_xmm0 = r->x[0]; g_xmm1 = r->x[1]; g_xmm2 = r->x[2]; g_xmm3 = r->x[3];
    g_xmm4 = r->x[4]; g_xmm5 = r->x[5]; g_xmm6 = r->x[6]; g_xmm7 = r->x[7];
    memcpy(g_fp_stack, r->fp, sizeof(r->fp)); g_fp_top = r->top;
    g_fp_control_word = r->fp_control; g_fp_cc = r->fp_cc; g_fp_cmp = r->fp_cmp; g_df = r->df;
}

static int valid_widget(uint32_t w) { return w >= 0x10000 && w < 0x03FFF000; }

/* text id -> guest wide string, through the game's own text manager. */
static uint32_t guest_text(uint32_t id) {
    GuiRegisters r = save_registers();
    edi = id;
    PUSH32(esp, 0x002265B0u); RECOMP_ABI_CALL(0x001639DAu, sub_001639DA);
    uint32_t result = eax;
    restore_registers(&r);
    return result;
}
static void barrel_add(uint32_t barrel, uint32_t wide, int value) {
    GuiRegisters r = save_registers();
    ebx = barrel; eax = wide;
    PUSH32(esp, 1); PUSH32(esp, 0xFFFFFFFFu); PUSH32(esp, (uint32_t)value);
    PUSH32(esp, 0x002265C0u); RECOMP_ABI_CALL(0x00208C45u, sub_00208C45);
    restore_registers(&r);
}
static void barrel_select(uint32_t barrel, int value, int notify) {
    GuiRegisters r = save_registers();
    eax = barrel;
    PUSH32(esp, (uint32_t)notify); PUSH32(esp, (uint32_t)value);
    PUSH32(esp, 0x002265D0u); RECOMP_ABI_CALL(0x00208BF9u, sub_00208BF9);
    restore_registers(&r);
}
static int barrel_count(uint32_t b) { return (int)((MEM32(b + 0xB0) - MEM32(b + 0xAC)) / 0x1C); }
static int barrel_value(uint32_t b) {              /* value of the selected item, -1 if none */
    int32_t idx = SMEM32(b + 0x154), n = barrel_count(b);
    if (idx < 0 || idx >= n) return -1;
    return SMEM32(MEM32(b + 0xAC) + (uint32_t)idx * 0x1C + 0xC);
}
static void widget_show(uint32_t w, int visible) {   /* == sub_1B7F0 (eax=w, cl=visible) */
    uint32_t f = MEM32(w + 0x74);
    uint32_t n = visible ? (f | 2u) : (f & ~2u);
    if (n != f) MEM32(w + 0x74) = n;
}

/* ============================ screen state ============================ */
enum { ROW_RED = 0, ROW_BLUE = 1, ROW_SKILL = 2, ROWS = 3, KIDS = 6 };
typedef struct Ui {
    uint32_t menu, form;
    int absent;                         /* form has no bots rows (overlay not installed) */
    uint32_t barrel[ROWS], label[ROWS];
    uint32_t kid[ROWS][KIDS];           /* barrel, list, label, focus, left, right */
    int32_t  y[ROWS][KIDS], wy[ROWS][KIDS];
    uint32_t desc; int32_t desc_y, desc_wy;
    int frames, populated, team;        /* team: -1 unknown, 0 FFA, 1 team */
    int cur[ROWS];                      /* last synchronised selection values */
    int placed;                         /* the local players were put on a side for these counts */
    unsigned mask;                      /* the local-player mask the rows were last shown for (split screen) */
} Ui;
static Ui g_ui;

/* End of a Test launch's bot values. now: the player's own at once (the New Game screen re-selects
 * them); otherwise from the next visit to New Game, so rematches from the Briefing Room keep them. */
void ma2_bots_test_release(int now) {
    if (!g_cfg_test) return;
    if (now) { g_cfg_test = 0; g_cfg_loaded = 0; g_ui.populated = 0; }
    else { g_cfg_test = 2; g_ui.menu = 0; }   /* in the match: the next New Game binds (and populates) afresh */
}

static int row_valid(const Ui *u, int i) {
    static const uint32_t action[ROWS] = { ACTION_RED, ACTION_BLUE, ACTION_SKILL };
    return valid_widget(u->barrel[i]) && MEM32(u->barrel[i] + 0x70) == action[i];
}

static Ui *bind(uint32_t menu, uint32_t form) {
    Ui *u = &g_ui;
    if (u->menu == menu && u->form == form) {
        if (u->absent) return NULL;
        if (row_valid(u, 0) && row_valid(u, 1) && row_valid(u, 2)) return u;
    }
    memset(u, 0, sizeof *u);
    u->menu = menu; u->form = form; u->team = -1;
    u->barrel[ROW_RED] = ma2_gui_find(form, ACTION_RED);
    u->barrel[ROW_BLUE] = ma2_gui_find(form, ACTION_BLUE);
    u->barrel[ROW_SKILL] = ma2_gui_find(form, ACTION_SKILL);
    for (int i = 0; i < ROWS; i++) if (!valid_widget(u->barrel[i])) { u->absent = 1; return NULL; }
    u->label[ROW_RED] = ma2_gui_find(form, 911);
    u->label[ROW_BLUE] = ma2_gui_find(form, 921);
    u->label[ROW_SKILL] = ma2_gui_find(form, 931);
    u->desc = ma2_gui_find(form, 700);
    for (int i = 0; i < ROWS; i++) {
        int n = 0; u->kid[i][n++] = u->barrel[i];
        for (uint32_t c = MEM32(u->barrel[i] + 0x1C); valid_widget(c) && n < KIDS; c = MEM32(c + 0x18)) u->kid[i][n++] = c;
        for (int k = 0; k < n; k++) { u->y[i][k] = SMEM32(u->kid[i][k] + 0x50); u->wy[i][k] = SMEM32(u->kid[i][k] + 0x60); }
    }
    if (valid_widget(u->desc)) { u->desc_y = SMEM32(u->desc + 0x50); u->desc_wy = SMEM32(u->desc + 0x60); }
    return u;
}

static void place_row(Ui *u, int row, int32_t target_y) {
    int32_t delta = target_y - u->y[row][0];
    for (int k = 0; k < KIDS; k++) {
        uint32_t w = u->kid[row][k];
        if (!w || k == 1) continue;                    /* k == 1: item-list helper keeps its retail origin */
        SMEM32(w + 0x50) = u->y[row][k] + delta;       /* standard rectangle  */
        SMEM32(w + 0x60) = u->wy[row][k] + delta;      /* widescreen rectangle */
    }
}

/* Same team rule the screen applies (sub_16616C on the Game Type barrel's value;
 * value 1 is mapped to 0xA by sub_226587, see HANDOFF). */
static int g_ui_gametype = -1;
static int team_mode(uint32_t menu, uint32_t session) {
    int gt = -1;
    uint32_t b = MEM32(menu + 0x74);
    if (valid_widget(b)) gt = barrel_value(b);
    if (gt < 0) gt = (int)MEM32(session + 0x44);
    if (gt == 1) gt = 0xA;
    g_ui_gametype = gt;
    { static int last = -2; if (gt != last && getenv("RECOMP_BOT_DIAG")) { last = gt;
        fprintf(stderr, "[BOT-UI] gametype=%d\n", gt); } }
    return gt > 0 && (gt <= 3 || (gt > 0xA && gt <= 0x11));
}

static int humans(void) {
    unsigned m = MEM32(0x50C9AC) & 0xF; int n = 0;
    while (m) { n += m & 1; m >>= 1; }
    return n < 1 ? 1 : n;
}
/* The local players' sides in the lobby: session+0x3A8 + controller*4 (0 Red, 1 Blue) -- the word
 * the Briefing Room's team choice sets and the level spawns each player on. */
static void human_sides(uint32_t session, int *red, int *blue) {
    unsigned m = MEM32(0x50C9AC) & 0xF; *red = *blue = 0;
    for (int c = 0; c < 4; c++) if (m & (1u << c)) { if (MEM32(session + 0x3A8 + c * 4) == 1) (*blue)++; else (*red)++; }
}
/* Setting the bot counts puts each local player on the side they leave short (6 Red bots, 5 Blue:
 * you join Blue for 6v6; 1 Red, 4 Blue: you join Red, 2 against 4). Even: the side you are on.
 * The game sets every player's side again while it builds the lobby (Red), so the choice is also
 * held for the lobby's first frames (g_place, ma2_menu_policy_pulse) -- then a team picked in the
 * Briefing Room stands. */
#define LOBBY_MENU_VT 0x00482240u
static struct { int pending, frames; uint32_t side[4]; } g_place;
/* Split screen: the guest's team. Owner decision 2026-11-23: a joining guest keeps the ORIGINAL
 * game's own team assignment and may switch sides in the Briefing Room; no rule of ours places it.
 * THE ONE PLACE TO CHANGE THAT: set SPLIT_PLACE_GUEST to 1 and the short-side rule below places the
 * guest like player 1 (each human on the side the counts leave short). */
#define SPLIT_PLACE_GUEST 0
static int lowest_human(unsigned m) { for (int c = 0; c < 4; c++) if (m & (1u << c)) return c; return -1; }
static int placed_human(unsigned m, int c) { return SPLIT_PLACE_GUEST || c == lowest_human(m); }
static void place_humans(uint32_t session, int red_bots, int blue_bots) {
    unsigned m = MEM32(0x50C9AC) & 0xF;
    int red = red_bots, blue = blue_bots;
    for (int c = 0; c < 4; c++) {   /* a human the rule does not place still fills its side first */
        if (!(m & (1u << c)) || placed_human(m, c)) continue;
        if (MEM32(session + 0x3A8 + c * 4) == 1) blue++; else red++;
    }
    for (int c = 0; c < 4; c++) {
        if (!(m & (1u << c)) || !placed_human(m, c)) continue;
        uint32_t at = session + 0x3A8 + c * 4, side = MEM32(at) == 1 ? 1u : 0u;
        if (red < blue) side = 0; else if (blue < red) side = 1;
        if (MEM32(at) != side) {
            MEM32(at) = side;
            if (getenv("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-UI] player %d placed on %s (bots red=%d blue=%d)\n", c, side ? "Blue" : "Red", red_bots, blue_bots);
        }
        g_place.side[c] = side;
        if (side) blue++; else red++;
    }
    g_place.pending = 1; g_place.frames = 0;
}
static void place_hold(void) {
    uint32_t session = MEM32(0x4EBBFC);
    unsigned m = MEM32(0x50C9AC) & 0xF;
    if (!g_place.pending || !valid_widget(session) || MEM32(session + 0x4C) != 2) return;
    for (int c = 0; c < 4; c++) {
        if (!(m & (1u << c)) || !placed_human(m, c)) continue;
        uint32_t at = session + 0x3A8 + c * 4;
        if (MEM32(at) != g_place.side[c]) {
            MEM32(at) = g_place.side[c];
            if (getenv("RECOMP_BOT_DIAG")) fprintf(stderr, "[BOT-UI] lobby: player %d kept on %s\n", c, g_place.side[c] ? "Blue" : "Red");
        }
    }
    if (++g_place.frames >= 30) g_place.pending = 0;   /* half a second: then the Briefing Room's choice */
}

static void populate(Ui *u, const Ma2BotConfig *c, int team, int max_each) {
    for (int row = ROW_RED; row <= ROW_BLUE; row++) {
        uint32_t b = u->barrel[row];
        if (MEM32(b + 0xAC) != MEM32(b + 0xB0)) continue;       /* already filled */
        barrel_add(b, guest_text(TEXT_OFF), 0);
        for (int v = 1; v <= max_each; v++) {
            wchar_t digits[8]; swprintf(digits, 8, L"%d", v);
            barrel_add(b, ma2_pc_wide(digits), v);
        }
    }
    uint32_t s = u->barrel[ROW_SKILL];
    if (MEM32(s + 0xAC) == MEM32(s + 0xB0)) {
        static const int ids[4] = { TEXT_SKILL_GREEN, TEXT_SKILL_REGULAR, TEXT_SKILL_VETERAN, TEXT_SKILL_ELITE };
        for (int v = 0; v < 4; v++) barrel_add(s, guest_text((uint32_t)ids[v]), v);
        barrel_add(s, guest_text(TEXT_SKILL_DRAGOON), MA2_SKILL_DRAGOON);   /* past Elite, then Mixed (playtest 11-06: Dragoon before Mixed) */
        barrel_add(s, guest_text(TEXT_SKILL_MIXED), MA2_SKILL_MIXED);
    }
    u->cur[ROW_RED] = team ? c->red : c->ffa;
    u->cur[ROW_BLUE] = c->blue;
    u->cur[ROW_SKILL] = c->skill;
    barrel_select(u->barrel[ROW_RED], u->cur[ROW_RED], 0);
    barrel_select(u->barrel[ROW_BLUE], u->cur[ROW_BLUE], 0);
    barrel_select(u->barrel[ROW_SKILL], u->cur[ROW_SKILL], 0);
    u->populated = 1;
}

/* ============================ barrel arrows (all menus) ============================ */
/* Advance widths (A+B+C) of ImpactReg18.mgFont for codes 0x20..0xFF, from the
 * English text.mgf. A barrel value is centred on the barrel, so its width puts
 * the arrows: left = centre - w/2 - gap - arrow, right = centre + w/2 + gap. */
static const unsigned char g_adv[224] = {
    3, 5, 7, 11, 10, 12, 10, 3, 6, 6, 5, 10, 3, 5, 3, 7,
    10, 7, 9, 10, 9, 10, 10, 7, 10, 10, 4, 4, 10, 10, 10, 9,
    14, 9, 10, 10, 10, 7, 7, 10, 10, 5, 6, 10, 7, 13, 10, 10,
    9, 10, 10, 9, 8, 10, 9, 15, 9, 9, 7, 5, 7, 5, 9, 10,
    6, 9, 9, 9, 9, 9, 5, 9, 9, 5, 5, 9, 5, 13, 9, 9,
    9, 9, 6, 8, 5, 9, 7, 11, 7, 8, 6, 7, 5, 7, 9, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 5, 9, 10, 10, 9, 5, 9, 6, 14, 6, 7, 10, 5, 14, 10,
    6, 10, 6, 6, 6, 8, 10, 6, 6, 4, 6, 7, 11, 12, 12, 9,
    9, 9, 9, 9, 9, 9, 13, 10, 7, 7, 7, 7, 5, 5, 5, 5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 9, 10,
    9, 9, 9, 9, 9, 9, 14, 9, 9, 9, 9, 9, 5, 5, 5, 5,
    9, 9, 9, 9, 9, 9, 9, 10, 9, 9, 9, 9, 9, 8, 9, 8,
};
/* ---- generic barrel arrows (every GGF menu with barrels) ----
 * Barrel and arrow classes are identified by their vtables in the shipped
 * ma2.xbe (text class 0x4830F0 is not used). The engine places an arrow from
 * the width of the value it first showed, so after a change the arrows drift.
 * Each frame the arrows are re-placed next to the selected item and fixed
 * (policy 0). A left click on an arrow focuses the barrel and steps it, like
 * Left/Right. ws_mode selects the widescreen rectangle (+0x5C) or the 4:3 one. */
#define GUI_BARREL_VT 0x00485778u
#define GUI_ARROW_VT  0x00487E68u
#define ARROW_SIZE 16
#define ARROW_GAP 8
#define ARROW_SLOP 6
float pc_display_aspect(float original);   /* platform/pc_frontend.h */
/* widescreen layout: 480*aspect units across, the 4:3 panel centred (x 256..896 of 1152 at 2.4:1) */
#define LAYOUT_W (480.0f * pc_display_aspect(4.0f / 3.0f))
#define LAYOUT_H 480.0f
#define LAYOUT_X0 ((LAYOUT_W - 640.0f) * 0.5f)
#define DESC_BELOW_FOOTER 419     /* description below the footer when it has to move */

static int32_t item_text_width(uint32_t begin, uint32_t end) {
    if (!valid_widget(begin) || end < begin || end - begin > 2 * 256) return -1;
    int32_t width = 0;
    for (uint32_t p = begin; p + 2 <= end; p += 2) {
        uint16_t c = *(const uint16_t *)(g_xbox_mem_offset + p);
        width += (c >= 0x20 && c < 0x100) ? g_adv[c - 0x20] : 9;
    }
    return width;
}

/* Left and right arrows: the two arrow-class children of the barrel. The child
 * order differs between screens (Campaign lists the right arrow first), so the
 * left one is the arrow with the smaller x while the engine still has them at
 * their authored places. The pair is cached per barrel. */
typedef struct { uint32_t barrel, left, right; } ArrowPair;
static ArrowPair g_pairs[64];
static unsigned g_pair_next;

static void barrel_arrows(uint32_t barrel, uint32_t *left, uint32_t *right) {
    *left = *right = 0;
    /* A cached pair is used only while both arrows are still children of this
     * barrel: widget addresses are reused after a menu closes. */
    for (unsigned i = 0; i < sizeof g_pairs / sizeof *g_pairs; i++) {
        if (g_pairs[i].barrel != barrel) continue;
        int in_l = 0, in_r = 0;
        for (uint32_t c = MEM32(barrel + 0x1C); valid_widget(c); c = MEM32(c + 0x18)) {
            if (c == g_pairs[i].left) in_l = 1;
            if (c == g_pairs[i].right) in_r = 1;
        }
        if (in_l && in_r && MEM32(g_pairs[i].left) == GUI_ARROW_VT && MEM32(g_pairs[i].right) == GUI_ARROW_VT) {
            *left = g_pairs[i].left; *right = g_pairs[i].right; return;
        }
        g_pairs[i].barrel = 0;
    }
    uint32_t first = 0, second = 0;
    for (uint32_t c = MEM32(barrel + 0x1C); valid_widget(c); c = MEM32(c + 0x18)) {
        if (MEM32(c) != GUI_ARROW_VT) continue;
        if (!first) first = c; else if (!second) { second = c; break; }
    }
    if (!first || !second) return;
    if (SMEM32(second + 0x4C) < SMEM32(first + 0x4C)) { uint32_t t = first; first = second; second = t; }
    *left = first; *right = second;
    ArrowPair *slot = &g_pairs[g_pair_next++ % (sizeof g_pairs / sizeof *g_pairs)];
    slot->barrel = barrel; slot->left = first; slot->right = second;
}

typedef struct { int ws; float nx, ny; int dir; float row_x, row_y; int click; } ArrowCtx;

static void visit_barrels(uint32_t w, int depth, void (*fn)(uint32_t, uint32_t, uint32_t, ArrowCtx *), ArrowCtx *ctx, int *budget) {
    for (; valid_widget(w) && *budget > 0; w = MEM32(w + 0x18)) {
        (*budget)--;
        if (MEM32(w) == GUI_BARREL_VT) {
            uint32_t left, right;
            barrel_arrows(w, &left, &right);
            if (valid_widget(left) && valid_widget(right)) fn(w, left, right, ctx);
        }
        if (depth < 12) visit_barrels(MEM32(w + 0x1C), depth + 1, fn, ctx, budget);
    }
}

static void for_each_barrel(uint32_t object, void (*fn)(uint32_t, uint32_t, uint32_t, ArrowCtx *), ArrowCtx *ctx) {
    if (!valid_widget(object)) return;
    uint32_t begin = MEM32(object + 4), end = MEM32(object + 8);
    if (end < begin || end - begin > 1024) return;
    for (uint32_t e = begin; e < end; e += 4) {
        uint32_t form = MEM32(e);
        if (!valid_widget(form)) continue;
        int budget = 1500;
        visit_barrels(MEM32(form + 0x38), 0, fn, ctx, &budget);
    }
}

/* Custom Scenarios moves the scenario barrel (title, description, arrows) right by this many
 * 640x480 units to make room for the Editor button (pc_native_settings.cpp); 0 elsewhere. The
 * barrel's own x is kept per barrel so the shift is applied once, not once per frame, and is
 * taken back when the offset returns to 0: the Campaign mission picker can be the same widget
 * the Custom Scenarios one was, and it has no Editor button, so it stays centred. */
static int g_barrel_dx;
void ma2_barrel_offset(int dx) { g_barrel_dx = dx; }
static struct { uint32_t w; int32_t x[2]; int32_t dx; } g_shifted[16];
static unsigned g_shifted_next, g_shifted_live;   /* entries with dx != 0 */
/* Both rectangles move: in widescreen the arrows follow +0x5C but the title still draws from +0x4C. */
static void shift_widget(uint32_t w) {
    unsigned i;
    for (i = 0; i < 16 && g_shifted[i].w != w; i++) {}
    if (i == 16 && !g_barrel_dx) return;   /* never moved: nothing to take back */
    /* new widget, or a reused address holding another widget's x */
    if (i == 16 || SMEM32(w + 0x4C) != g_shifted[i].x[0] + g_shifted[i].dx) {
        if (i == 16) i = g_shifted_next++ % 16;
        if (g_shifted[i].dx) g_shifted_live--;
        g_shifted[i].w = w; g_shifted[i].x[0] = SMEM32(w + 0x4C); g_shifted[i].x[1] = SMEM32(w + 0x5C);
        g_shifted[i].dx = 0;
    }
    if (g_shifted[i].dx == g_barrel_dx) return;
    if (!g_shifted[i].dx) g_shifted_live++; else if (!g_barrel_dx) g_shifted_live--;
    g_shifted[i].dx = g_barrel_dx;
    SMEM32(w + 0x4C) = g_shifted[i].x[0] + g_barrel_dx;
    SMEM32(w + 0x5C) = g_shifted[i].x[1] + g_barrel_dx;
}
/* The barrel (title) and its text children (the description, id 1) move as one block; the arrows
 * are re-placed from the barrel every frame (place_one). */
static void shift_barrel(uint32_t b) {
    if (!g_barrel_dx && !g_shifted_live) return;
    shift_widget(b);
    int n = 0;
    for (uint32_t c = MEM32(b + 0x1C); valid_widget(c) && n < 8; c = MEM32(c + 0x18), n++)
        if (MEM32(c) != GUI_ARROW_VT) shift_widget(c);
}

/* A vertical barrel (Select Difficulty: the selected item in the middle row, its neighbours
 * above and below, one arrow above the list and one below) has its two arrows one above the
 * other. The PC arrows, row focus and Left/Right stepping are for the horizontal value rows
 * only; a vertical barrel is handled by ma2_vbarrel_pointer. */
static int barrel_vertical(uint32_t left, uint32_t right) {
    int32_t dy = SMEM32(left + 0x50) - SMEM32(right + 0x50);
    return dy > 24 || dy < -24;
}

static void place_one(uint32_t b, uint32_t left, uint32_t right, ArrowCtx *ctx) {
    if (barrel_vertical(left, right)) {
        /* Up/down arrows: centred on the list in both layouts, with the layout policy (+0x80)
         * cleared as for the value-row arrows, so the drawn chevron and its hover/click box
         * (ma2_vbarrel_pointer, from the same rectangle) coincide at every aspect. The authored
         * policy drew them right of the list in widescreen. */
        for (int k = 0; k < 2; k++) {
            uint32_t a = k ? right : left;
            for (uint32_t off = 0x4C; off <= 0x5C; off += 0x10) {
                int32_t x = SMEM32(b + off) + SMEM32(b + off + 8) / 2 - SMEM32(a + off + 8) / 2;
                SMEM32(a + off) = x;
            }
            MEM32(a + 0x80) = 0;
        }
        return;
    }
    int32_t sel = SMEM32(b + 0x154);
    shift_barrel(b);
    uint32_t count = (MEM32(b + 0xB0) - MEM32(b + 0xAC)) / 0x1C;
    if (getenv("RECOMP_MENU_TRACE")) {
        static uint32_t last_b; static int32_t last_sel = -2;
        if (b == last_b && sel != last_sel) fprintf(stderr, "[PC-BARREL] %08X selection %d -> %d (focus=%u, arrows %08X/%08X shown %u/%u)\n",
                                                    b, last_sel, sel, (MEM32(b + 0x74) >> 3) & 1, left, right, MEM32(left + 0x74) & 2, MEM32(right + 0x74) & 2);
        if (b != last_b || sel != last_sel) { last_b = b; last_sel = sel; }
    }
    if (sel < 0 || (uint32_t)sel >= count) return;
    /* The arrows stand clear of the WIDEST value this barrel can show, not the current one, so
     * they never move while the player steps through maps or modes: clicking the same spot
     * keeps stepping instead of landing on a longer name. */
    int32_t w = -1;
    for (uint32_t i = 0; i < count && i < 256; i++) {
        uint32_t item = MEM32(b + 0xAC) + i * 0x1C;
        int32_t iw = item_text_width(MEM32(item), MEM32(item + 4));
        if (iw > w) w = iw;
    }
    if (w < 0) return;
    uint32_t off = ctx->ws ? 0x5C : 0x4C;
    int32_t centre = SMEM32(b + off) + SMEM32(b + off + 8) / 2;
    if (getenv("RECOMP_MENU_TRACE")) {
        static uint32_t seen[32]; static int ns; int k;
        for (k = 0; k < ns && seen[k] != b; k++) {}
        if (k == ns && ns < 32) { seen[ns++] = b; fprintf(stderr, "[PC-BARREL] %08X x=%d y=%d w=%d widest=%d items=%u\n", b, SMEM32(b + off), SMEM32(b + off + 4), SMEM32(b + off + 8), w, count); }
    }
    int32_t lx = centre - w / 2 - ARROW_GAP - ARROW_SIZE;
    int32_t rx = centre + (w + 1) / 2 + ARROW_GAP;
    SMEM32(left + 0x4C) = lx;  SMEM32(left + 0x5C) = lx;  MEM32(left + 0x80) = 0;
    SMEM32(right + 0x4C) = rx; SMEM32(right + 0x5C) = rx; MEM32(right + 0x80) = 0;
}

static void click_one(uint32_t b, uint32_t left, uint32_t right, ArrowCtx *ctx) {
    if (ctx->click || !(MEM32(b + 0x74) & 2) || barrel_vertical(left, right)) return;
    float lx, ly = ctx->ny * LAYOUT_H;
    uint32_t off = ctx->ws ? 0x5C : 0x4C;
    lx = ctx->ws ? ctx->nx * LAYOUT_W - LAYOUT_X0 : ctx->nx * 640.0f;
    for (int side = 0; side < 2; side++) {
        uint32_t a = side ? right : left;
        float ax = (float)SMEM32(a + off), ay = (float)SMEM32(a + off + 4);
        if (getenv("RECOMP_MENU_TRACE")) fprintf(stderr, "[PC-ARROWS] click %.1f,%.1f vs %s arrow %08X at %.0f,%.0f (4:3 %d,%d ws %d,%d)\n", lx, ly, side ? "right" : "left", a, ax, ay,
                                                 SMEM32(a + 0x4C), SMEM32(a + 0x50), SMEM32(a + 0x5C), SMEM32(a + 0x60));
        if (lx >= ax - ARROW_SLOP && lx < ax + ARROW_SIZE + ARROW_SLOP &&
            ly >= ay - ARROW_SLOP && ly < ay + ARROW_SIZE + ARROW_SLOP) {
            float cx = (float)SMEM32(b + off) + (float)SMEM32(b + off + 8) / 2.0f;
            float cy = (float)SMEM32(b + off + 4) + 16.0f;
            ctx->row_x = ctx->ws ? (cx + LAYOUT_X0) / LAYOUT_W : cx / 640.0f;
            ctx->row_y = cy / LAYOUT_H;
            ctx->dir = side ? 1 : -1;
            ctx->click = 1;
            return;
        }
    }
}

/* A click inside a barrel's row (not on its arrows): the row takes focus only. Stepping is
 * the arrows' and Left/Right's job, confirming is Enter's or the select button's. */
static void inside_one(uint32_t b, uint32_t left, uint32_t right, ArrowCtx *ctx) {
    if (ctx->click || barrel_vertical(left, right)) return;
    uint32_t off = ctx->ws ? 0x5C : 0x4C;
    float lx = ctx->ws ? ctx->nx * LAYOUT_W - LAYOUT_X0 : ctx->nx * 640.0f, ly = ctx->ny * LAYOUT_H;
    float bx = (float)SMEM32(b + off), by = (float)SMEM32(b + off + 4), bw = (float)SMEM32(b + off + 8);
    if (lx >= bx - ARROW_SIZE - ARROW_GAP && lx < bx + bw + ARROW_SIZE + ARROW_GAP && ly >= by - 4.0f && ly < by + 36.0f) ctx->click = 1;
}
int ma2_barrel_row_hit(uint32_t object, int ws_mode, float nx, float ny) {
    ArrowCtx ctx = {0};
    ctx.ws = ws_mode; ctx.nx = nx; ctx.ny = ny;
    for_each_barrel(object, inside_one, &ctx);
    return ctx.click;
}

/* Per-frame: re-place the arrows of every barrel in this menu. */
void ma2_barrel_arrows_pulse(uint32_t object, int ws_mode) {
    ArrowCtx ctx = {0};
    ctx.ws = ws_mode;
    for_each_barrel(object, place_one, &ctx);
}

/* Left click on a barrel arrow of this menu: returns -1 (left), +1 (right) and the
 * normalised point of the barrel row (focus it first), otherwise 0. */
int ma2_barrel_arrow_click(uint32_t object, int ws_mode, float nx, float ny, float *row_x, float *row_y) {
    ArrowCtx ctx = {0};
    ctx.ws = ws_mode; ctx.nx = nx; ctx.ny = ny;
    for_each_barrel(object, click_one, &ctx);
    if (!ctx.click) return 0;
    *row_x = ctx.row_x; *row_y = ctx.row_y;
    return ctx.dir;
}

/* Vertical barrels under the pointer. The barrel draws the selected item on its middle row and
 * the items before and after it 29 units above and below (Select Difficulty: Green 211, Regular
 * 240, Veteran 269 for a barrel at y 200, h 80); its arrows are its two arrow children, shown
 * (+0x74 bit 1) only while there is more to scroll to. */
#define VBARREL_PITCH 29.0f
typedef struct { int ws; float nx, ny; int found, step; float box[4], centre[2]; } VBarrelCtx;
static void vbarrel_visit(uint32_t w, int depth, VBarrelCtx *ctx, int *budget) {
    for (; valid_widget(w) && *budget > 0 && !ctx->found; w = MEM32(w + 0x18)) {
        (*budget)--;
        if (!(MEM32(w + 0x74) & 2)) continue;   /* hidden: nor its children */
        if (MEM32(w) == GUI_BARREL_VT) {
            uint32_t a0, a1;
            barrel_arrows(w, &a0, &a1);
            if (valid_widget(a0) && valid_widget(a1) && barrel_vertical(a0, a1)) {
                uint32_t off = ctx->ws ? 0x5C : 0x4C;
                float lx = ctx->ws ? ctx->nx * LAYOUT_W - LAYOUT_X0 : ctx->nx * 640.0f, ly = ctx->ny * LAYOUT_H;
                float to_nx = ctx->ws ? 1.0f / LAYOUT_W : 1.0f / 640.0f, x0 = ctx->ws ? LAYOUT_X0 : 0.0f;
                float bx = (float)SMEM32(w + off), by = (float)SMEM32(w + off + 4);
                float bw = (float)SMEM32(w + off + 8), bh = (float)SMEM32(w + off + 12);
                ctx->centre[0] = (bx + bw * 0.5f + x0) * to_nx; ctx->centre[1] = (by + bh * 0.5f) / LAYOUT_H;
                /* the arrows: the upper one steps back (Up), the lower one forward (Down) */
                for (int k = 0; k < 2 && !ctx->found; k++) {
                    uint32_t a = k ? a1 : a0;
                    if (!(MEM32(a + 0x74) & 2)) continue;
                    float ax = (float)SMEM32(a + off), ay = (float)SMEM32(a + off + 4);
                    float aw = (float)SMEM32(a + off + 8), ah = (float)SMEM32(a + off + 12);
                    if (lx >= ax - ARROW_SLOP && lx < ax + aw + ARROW_SLOP && ly >= ay - ARROW_SLOP && ly < ay + ah + ARROW_SLOP) {
                        uint32_t other = k ? a0 : a1;
                        ctx->step = SMEM32(a + off + 4) < SMEM32(other + off + 4) ? -1 : 1;
                        ctx->box[0] = (ax + x0) * to_nx; ctx->box[1] = ay / LAYOUT_H;
                        ctx->box[2] = aw * to_nx; ctx->box[3] = ah / LAYOUT_H;
                        ctx->found = 2;
                    }
                }
                if (ctx->found) return;
                /* the rows: -1 above the selection, 0 the selection, +1 below; only rows with an item */
                int32_t sel = SMEM32(w + 0x154);
                uint32_t first = MEM32(w + 0xAC), end = MEM32(w + 0xB0);
                int32_t count = end > first && end - first < 0x1C * 256 ? (int32_t)((end - first) / 0x1C) : 0;
                float cy = by + bh * 0.5f;
                if (sel >= 0 && sel < count && lx >= bx && lx < bx + bw) {
                    for (int row = -1; row <= 1; row++) {
                        float ry = cy + row * VBARREL_PITCH;
                        if (sel + row < 0 || sel + row >= count) continue;
                        if (ly >= ry - VBARREL_PITCH * 0.5f && ly < ry + VBARREL_PITCH * 0.5f) {
                            ctx->step = row;
                            /* the glow: as wide as the selection's lines (80 % of the barrel), centred */
                            ctx->box[0] = (bx + bw * 0.1f + x0) * to_nx; ctx->box[1] = (ry - 11.0f) / LAYOUT_H;
                            ctx->box[2] = bw * 0.8f * to_nx; ctx->box[3] = 22.0f / LAYOUT_H;
                            ctx->found = 1;
                            return;
                        }
                    }
                }
            }
        }
        if (depth < 12) vbarrel_visit(MEM32(w + 0x1C), depth + 1, ctx, budget);
    }
}
/* Returns 1 for an item row (step -1 = the item above the selection, 0 = the selection,
 * +1 = the item below), 2 for a shown arrow (step -1 up, +1 down), 0 when the point is on no
 * vertical barrel of the menu's active forms. box: the row or arrow, normalised; centre: the
 * barrel's middle row, normalised (a point that focuses the barrel). */
int ma2_vbarrel_pointer(uint32_t object, int ws_mode, float nx, float ny, int *step, float box[4], float centre[2]) {
    VBarrelCtx ctx = {0};
    ctx.ws = ws_mode; ctx.nx = nx; ctx.ny = ny;
    if (!valid_widget(object)) return 0;
    uint32_t begin = MEM32(object + 4), end = MEM32(object + 8);
    if (end < begin || end - begin > 1024) return 0;
    for (uint32_t e = begin; e < end && !ctx.found; e += 4) {
        uint32_t form = MEM32(e);
        if (!valid_widget(form) || (MEM32(form + 0x4C) & 3) != 3) continue;
        int budget = 1500;
        vbarrel_visit(MEM32(form + 0x38), 0, &ctx, &budget);
    }
    if (!ctx.found) return 0;
    if (step) *step = ctx.step;
    if (box) memcpy(box, ctx.box, sizeof ctx.box);
    if (centre) memcpy(centre, ctx.centre, sizeof ctx.centre);
    return ctx.found;
}

/* ============================ footer legends ============================ */
/* The PC input legends (key label text 0x70010000..5 with its caption as a
 * child, see build_pc_assets.py patch_legends) under a normalised point, as one
 * normalised box. Same tree walk and rectangles as pc_gui_bridge.c widget_hit,
 * without focusing anything, so it can run every frame for the hover glow. */
static void widget_rect(uint32_t widget, float rect[4]) {
    GuiRegisters r = save_registers(); esp -= 16; uint32_t address = esp;
    PUSH32(esp, address); PUSH32(esp, widget);
    PUSH32(esp, 0x001FF971u); RECOMP_ABI_CALL(0x00201B61u, sub_00201B61);
    memcpy(rect, (void *)(g_xbox_mem_offset + address), 16); restore_registers(&r);
}
static int inside(const float r[4], float x, float y) { return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }
/* Advance widths (A+B+C) of ArialUnicodeMSBold14.mgFont for 0x20..0xFF, in
 * 480-line units: the font of every PC legend key and of nearly every retail
 * legend caption (build_pc_assets.py patch_legends). */
static const unsigned char g_legend_adv[224] = {
    5, 5, 6, 9, 9, 13, 10, 4, 6, 6, 6, 9, 5, 6, 5, 5,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 5, 5, 9, 9, 9, 9,
    15, 10, 10, 11, 11, 10, 10, 12, 11, 4, 8, 10, 9, 13, 11, 12,
    10, 12, 11, 10, 10, 11, 10, 14, 10, 10, 10, 5, 5, 5, 8, 8,
    6, 9, 9, 8, 9, 9, 5, 9, 9, 4, 4, 8, 4, 12, 9, 9,
    9, 9, 6, 8, 5, 9, 8, 11, 8, 8, 8, 6, 5, 6, 9, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    0, 6, 9, 9, 9, 9, 5, 9, 6, 11, 6, 9, 9, 6, 11, 8,
    7, 9, 6, 6, 6, 9, 9, 5, 6, 6, 6, 9, 13, 13, 13, 10,
    10, 10, 10, 10, 10, 10, 15, 11, 10, 10, 10, 10, 4, 4, 4, 4,
    11, 11, 12, 12, 12, 12, 12, 9, 12, 11, 11, 11, 11, 10, 10, 10,
    9, 9, 9, 9, 9, 9, 14, 8, 9, 9, 9, 9, 4, 4, 4, 4,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 8, 9, 8
};
/* Width of a text widget's string in 480-line units, -1 if none: the string
 * set at run time (+0xE0, sub_001FF864) or else its text id (+0xE8). */
static float string_width(uint32_t s) {
    if (!valid_widget(s)) return -1.0f;
    int width = 0, n = 0;
    for (; n < 96; n++) {
        uint16_t c = MEM16(s + 2u * (uint32_t)n);
        if (!c) break;
        width += (c >= 0x20 && c < 0x100) ? g_legend_adv[c - 0x20] : 9;
    }
    return n ? (float)width : -1.0f;
}
static float legend_width(uint32_t w) {
    float width = string_width(MEM32(w + 0xE0));
    if (width < 0 && MEM32(w + 0xE8)) width = string_width(guest_text(MEM32(w + 0xE8)));
    return width;
}
typedef struct { float x, y, unit_x; float *out; int budget; } FooterQuery;
/* Returns 0x80000000 | menu button (as pc_gui_bridge.c widget_hit) for the
 * legend under the point, else 0. */
static uint32_t footer_hit(uint32_t w, int depth, FooterQuery *q) {
    if (!valid_widget(w) || depth > 40 || --q->budget < 0 || !(MEM32(w + 0x74) & 2)) return 0;
    uint32_t text = MEM32(w + 0xE8);
    if (text >= 0x70010000u && text < 0x70010006u) {
        /* First the row the key label and its captions span (cheap reject), then the drawn
         * text below; the gap between key and caption is not a dead spot. */
        float r[4]; widget_rect(w, r);
        float bx0 = r[0], by0 = r[1], bx1 = r[0] + r[2], by1 = r[1] + r[3];
        for (uint32_t c = MEM32(w + 0x1C); valid_widget(c); c = MEM32(c + 0x18)) {
            float rc[4]; widget_rect(c, rc);
            if (rc[0] < bx0) bx0 = rc[0];
            if (rc[1] < by0) by0 = rc[1];
            if (rc[0] + rc[2] > bx1) bx1 = rc[0] + rc[2];
            if (rc[1] + rc[3] > by1) by1 = rc[1] + rc[3];
        }
        if (!(q->x >= bx0 && q->x < bx1 && q->y >= by0 && q->y < by1)) return 0;
        /* Box: the drawn text. The key is right-justified in its rectangle, the
         * captions left-justified; a rectangle stands in for an unknown string. */
        float kw = legend_width(w);
        float x0 = kw >= 0 ? r[0] + r[2] - kw * q->unit_x : r[0], x1 = r[0] + r[2];
        for (uint32_t c = MEM32(w + 0x1C); valid_widget(c); c = MEM32(c + 0x18)) {
            widget_rect(c, r);
            float cw = legend_width(c), cx1 = cw >= 0 ? r[0] + cw * q->unit_x : r[0] + r[2];
            if (r[0] < x0) x0 = r[0];
            if (cx1 > x1) x1 = cx1;
        }
        /* Caption rectangles run far past their words (Back's reaches into Start, Player Options'
         * into Esc), so the row alone would hand the point to whichever prompt comes first. The
         * prompt owns its drawn text, as the glow shows it, plus 3 units either side. */
        float margin = 3.0f * q->unit_x;
        if (q->x < x0 - margin || q->x >= x1 + margin) return 0;
        if (getenv("RECOMP_FOOTER_TRACE"))
            fprintf(stderr, "[PC-FOOTER] %08X key %u box %.3f,%.3f-%.3f,%.3f key width %.0f\n", w, text - 0x70010000u, x0, by0, x1, by1, kw);
        q->out[0] = x0; q->out[1] = by0; q->out[2] = x1 - x0; q->out[3] = by1 - by0;
        static const unsigned buttons[] = { 0, 1, 2, 3, 5, 4 };
        return 0x80000000u | buttons[text - 0x70010000u];
    }
    for (uint32_t c = MEM32(w + 0x1C); valid_widget(c); c = MEM32(c + 0x18)) {
        uint32_t hit = footer_hit(c, depth + 1, q);
        if (hit) return hit;
    }
    return 0;
}
/* owner: the menu object (its active forms), or 0 for the top modal dialog.
 * aspect: display aspect, for the text widths (fonts scale with the height).
 * Returns 0x80000000 | button and the text box (normalised) of the legend under
 * the point, else 0. */
uint32_t ma2_gui_footer_at(uint32_t owner, float x, float y, float aspect, float rect[4]) {
    FooterQuery q = { x, y, 1.0f / (480.0f * aspect), rect, 1500 };
    if (!owner) {
        uint32_t begin = MEM32(0x5240D0), end = MEM32(0x5240D4);
        if (end <= begin || end - begin > 4096) return 0;
        uint32_t form = MEM32(MEM32(end - 4) + 4);
        return valid_widget(form) ? footer_hit(MEM32(form + 0x38), 0, &q) : 0;
    }
    if (!valid_widget(owner)) return 0;
    uint32_t begin = MEM32(owner + 4), end = MEM32(owner + 8);
    if (end < begin || end - begin > 1024) return 0;
    for (uint32_t e = begin; e < end; e += 4) {
        uint32_t form = MEM32(e);
        if (!valid_widget(form) || (MEM32(form + 0x4C) & 3) != 3) continue;
        uint32_t hit = footer_hit(MEM32(form + 0x38), 0, &q);
        if (hit) return hit;
    }
    return 0;
}

/* The same legend test on one form's tree: the name keyboard (MAKeyboardDlg) keeps its form at
 * owner+0x23C, not in the modal record, so ma2_gui_footer_at(0, ...) cannot see its footer. */
uint32_t ma2_gui_footer_in_form(uint32_t form, float x, float y, float aspect, float rect[4]) {
    FooterQuery q = { x, y, 1.0f / (480.0f * aspect), rect, 1500 };
    return valid_widget(form) ? footer_hit(MEM32(form + 0x38), 0, &q) : 0;
}
static uint32_t legend_find(uint32_t w, int depth, uint32_t text, int *budget) {
    for (; valid_widget(w) && --*budget > 0; w = MEM32(w + 0x18)) {
        if (MEM32(w + 0xE8) == text && (MEM32(w + 0x74) & 2)) return w;
        if (depth < 12) { uint32_t f = legend_find(MEM32(w + 0x1C), depth + 1, text, budget); if (f) return f; }
    }
    return 0;
}
/* Caption of a form's PC legend (key text 0x70010000 + key): wide = a guest wide string to
 * show, or 0 for the caption's own text id again. Writes only when the wanted text changes. */
void ma2_gui_legend_caption(uint32_t form, unsigned key, uint32_t wide) {
    static uint32_t last_caption, last_wide;
    if (!valid_widget(form)) return;
    int budget = 1500;
    uint32_t w = legend_find(MEM32(form + 0x38), 0, 0x70010000u + key, &budget);
    uint32_t c = w ? MEM32(w + 0x1C) : 0;
    if (!valid_widget(c)) return;
    if (c != last_caption) { last_caption = c; last_wide = 0; }   /* a new form shows its own text */
    if (wide == last_wide) return;
    uint32_t id = MEM32(c + 0xE8);
    if (!wide && (!id || id >= 0x80000000u)) return;
    ma2_gui_text(c, wide ? wide : guest_text(id));
    last_wide = wide;
}

/* ============================ menu dump (RECOMP_MENU_DUMP) ============================ */
/* Every widget of a menu's forms (or of the top modal dialog, owner 0): address, class
 * vtable, flags (+0x74, bit 1 shown), mode (+0x48), id (+0x6C), action (+0x70), text id
 * (+0xE8) and its string, 4:3 rectangle. For finding which widget a screen item is. */
static void dump_string(uint32_t s, char *out, int max) {
    int n = 0;
    if (valid_widget(s))
        for (int i = 0; i < 60 && n < max - 1; i++) {
            uint16_t c = MEM16(s + 2u * (uint32_t)i);
            if (!c) break;
            out[n++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
        }
    out[n] = 0;
}
static void dump_widget(uint32_t w, int depth, int *budget) {
    for (; valid_widget(w) && --*budget > 0; w = MEM32(w + 0x18)) {
        char text[64];
        uint32_t s = MEM32(w + 0xE0);
        if (!valid_widget(s) && MEM32(w + 0xE8) && MEM32(w + 0xE8) < 0x80000000u) s = guest_text(MEM32(w + 0xE8));
        dump_string(s, text, sizeof text);
        fprintf(stderr, "[MENU-DUMP] %*s%08X vt=%08X flags=%08X mode=%u id=%u action=%u text=%08X \"%s\" r=%d,%d,%d,%d\n",
                depth * 2, "", w, MEM32(w), MEM32(w + 0x74), MEM32(w + 0x48), MEM32(w + 0x6C), MEM32(w + 0x70),
                MEM32(w + 0xE8), text, SMEM32(w + 0x4C), SMEM32(w + 0x50), SMEM32(w + 0x54), SMEM32(w + 0x58));
        if (depth < 14) dump_widget(MEM32(w + 0x1C), depth + 1, budget);
    }
}
void ma2_gui_dump(uint32_t owner, const char *why) {
    int budget = 2000;
    if (!owner) {
        uint32_t begin = MEM32(0x5240D0), end = MEM32(0x5240D4);
        if (end <= begin || end - begin > 4096) return;
        uint32_t form = MEM32(MEM32(end - 4) + 4);
        fprintf(stderr, "[MENU-DUMP] ---- %s: modal form %08X\n", why, form);
        if (valid_widget(form)) dump_widget(MEM32(form + 0x38), 0, &budget);
        return;
    }
    if (!valid_widget(owner)) return;
    uint32_t begin = MEM32(owner + 4), end = MEM32(owner + 8);
    if (end < begin || end - begin > 1024) return;
    for (uint32_t e = begin; e < end; e += 4) {
        uint32_t form = MEM32(e);
        if (!valid_widget(form)) continue;
        fprintf(stderr, "[MENU-DUMP] ---- %s: owner %08X form %08X state %u\n", why, owner, form, MEM32(form + 0x4C));
        dump_widget(MEM32(form + 0x38), 0, &budget);
    }
}

/* ============================ menu policy ============================ */
/* Features this PC recomp has no backend for are shown disabled with the game's own state 5: the
 * dimmed state the pause menu already uses for Multiplayer Options without Live. The game's list
 * and carousel navigation skip it natively, and pc_gui_bridge.c's pointer and the footer code skip
 * it too.
 *   - Xbox Live (Conquest, Xbox Live) and System Link: no network stack.
 *   - Add Guest: one controller on the emulated USB bus, so no splitscreen guest can join.
 *   - Game Demos: launches cdx.xbe, a separate Xbox program on the disc.
 * The pause menu's Xbox Live status box and its Sign In prompt are hidden. The policy runs every
 * menu frame (menus rebuild); sub_001CB320, the GUI library's enable call, leaves these widgets
 * alone (ma2_menu_policy_keeps_disabled). Two items are decided by the game every frame, so the
 * policy answers at the game's own check: the Multiplayer menu's network-link test for System Link
 * (recomp_0058.c, ma2_menu_policy_online) and the pause menu's free-controller test for Add Guest
 * (recomp_0052.c, ma2_menu_policy_splitscreen).
 *
 * The pause menu's action-600 slot is Add Guest (text 0x7D00/0x7D01) only in a networked session
 * (sub_001FCB14: byte 0x50C94D set); everywhere else, a bot match included, the game makes it Quit to
 * Main Hub (text 0xE) and enables it. So that entry matches on the text too: it had matched the slot
 * alone and greyed Quit to Main Hub, the only way out of a bot match (playtest 2026-10-19).
 *
 * A fork that adds a backend can leave items alone:
 *   - RECOMP_ONLINE_MENUS=1: Conquest, System Link, Xbox Live and the Live status;
 *   - RECOMP_SPLITSCREEN_MENUS=1: Add Guest (also needs RECOMP_USB_PADS=2..4, so that a second
 *     controller exists on the bus). Since 1.2.0 split screen is on by default: main.c sets both
 *     unless RECOMP_SPLITSCREEN=0 / [Debug] SplitScreen=0, and gives the guest's port its own host
 *     pad (xinput_device.c, split_route.h). The pause menu's Add Guest stays disabled either way:
 *     a guest joins only in the Multiplayer menus, never mid-match (owner rule, 1.2.0; offline the
 *     pause menu's slot is Quit to Main Hub anyway);
 *   - RECOMP_DEMO_MENUS=1: Game Demos. */
enum { FEATURE_ONLINE = 1, FEATURE_SPLITSCREEN = 2, FEATURE_DEMOS = 3, FEATURE_MIDMATCH_GUEST = 4 };
int ma2_scn_mission_count(void);   /* scenario/ma2_scenario.c: FEATURE_DEMOS = Custom Scenarios */
/* text_lo..text_hi: the item's text id (+0xE8) must be in that range (0, 0 = any text). */
typedef struct { uint32_t menu_vt, item_vt, action; int feature; const char *what; uint32_t text_lo, text_hi; } DisabledItem;
static const DisabledItem k_disabled[] = {
    { 0x00481B70u, 0x004874A8u,   7, FEATURE_ONLINE,      "main menu CONQUEST", 0, 0 },
    { 0x00481B70u, 0x004874A8u,   8, FEATURE_DEMOS,       "main menu GAME DEMOS", 0, 0 },
    { 0x0048C228u, 0x004874A8u,   2, FEATURE_ONLINE,      "multiplayer SYSTEM LINK", 0, 0 },
    { 0x0048C228u, 0x004874A8u,   3, FEATURE_ONLINE,      "multiplayer XBOX LIVE", 0, 0 },
    { 0x0048C228u, 0x004874A8u,   4, FEATURE_SPLITSCREEN, "multiplayer ADD GUEST", 0, 0 },
    { 0x004827B0u, 0x004830F0u, 600, FEATURE_MIDMATCH_GUEST, "pause Add Guest", 0x7D00u, 0x7D01u },
};
#define PAUSE_MENU_VT   0x004827B0u
#define CREDITS_MENU_VT 0x0048BD88u
static int feature_off(int feature) {
    static int online = -1, split = -1, demos = -1;
    if (online < 0) {
        online = !env_or("RECOMP_ONLINE_MENUS", 0); split = !env_or("RECOMP_SPLITSCREEN_MENUS", 0);
        demos = !env_or("RECOMP_DEMO_MENUS", 0);
    }
    /* The Game Demos slot is Custom Scenarios now (scenario/ma2_scenario.c) and stays enabled: with
     * no missions it offers the Scenario Editor instead of the picker (ma2_scn_custom_enter). */
    (void)demos;
    if (feature == FEATURE_MIDMATCH_GUEST) return 1;   /* never: guests join in the Multiplayer menus only */
    return feature == FEATURE_ONLINE ? online : feature == FEATURE_SPLITSCREEN ? split : 0;
}
int ma2_menu_policy_online(void) { return !feature_off(FEATURE_ONLINE); }
/* The pause menu's free-controller test (recomp_0052.c): Add Guest mid-match stays off (owner rule, 1.2.0). */
int ma2_menu_policy_splitscreen(void) { return !feature_off(FEATURE_MIDMATCH_GUEST); }
/* Widgets the current menu keeps disabled, rebuilt each menu frame (render thread never reads it). */
static uint32_t g_kept[16]; static unsigned g_kept_n;
int ma2_menu_policy_keeps_disabled(uint32_t widget) {
    for (unsigned i = 0; i < g_kept_n; i++) if (g_kept[i] == widget) return 1;
    return 0;
}
typedef struct { uint32_t vtable; int budget; uint32_t kept[16]; unsigned kept_n; } PolicyCtx;
static void policy_widget(uint32_t w, int depth, PolicyCtx *ctx) {
    for (; valid_widget(w) && --ctx->budget > 0; w = MEM32(w + 0x18)) {
        uint32_t vt = MEM32(w), action = MEM32(w + 0x70);
        for (size_t i = 0; i < sizeof k_disabled / sizeof *k_disabled; i++) {
            const DisabledItem *d = &k_disabled[i];
            if (d->menu_vt != ctx->vtable || d->item_vt != vt || d->action != action || !feature_off(d->feature)) continue;
            if (d->text_hi && (MEM32(w + 0xE8) < d->text_lo || MEM32(w + 0xE8) > d->text_hi)) continue;
            if (ctx->kept_n < 16) ctx->kept[ctx->kept_n++] = w;
            if (MEM32(w + 0x48) != 5) {
                if (getenv("RECOMP_MENU_TRACE")) fprintf(stderr, "[PC-POLICY] %s %08X disabled (was state %u)\n", d->what, w, MEM32(w + 0x48));
                MEM32(w + 0x48) = 5;
            }
        }
        if (ctx->vtable == PAUSE_MENU_VT && feature_off(FEATURE_ONLINE)) {
            /* The Xbox Live status box (a panel holding the "Xbox Live:" caption, text 0xAC8)
             * and the Sign In legend (key label 0x70010002, action 9). */
            uint32_t first = MEM32(w + 0x1C);
            if (vt == 0x00489568u && valid_widget(first) && MEM32(first + 0xE8) == 0xAC8u) widget_show(w, 0);
            if (MEM32(w + 0xE8) == 0x70010002u && action == 9) widget_show(w, 0);
        }
        if (ctx->vtable == CREDITS_MENU_VT && MEM32(w + 0xE8) == 0x70010000u) {
            /* The credits' Back prompt was converted from an A-button graphic, but the screen backs
             * out with B: show and click it as Esc (button 1) so it does what it says. */
            uint32_t caption = MEM32(w + 0x1C);
            if (valid_widget(caption) && MEM32(caption + 0xE8) == 0x10u) {
                MEM32(w + 0xE8) = 0x70010001u;
                ma2_gui_text(w, guest_text(0x70010001u));
            }
        }
        if (depth < 14) policy_widget(MEM32(w + 0x1C), depth + 1, ctx);
    }
}
void ma2_scn_menu_frame(uint32_t object, uint32_t vtable);   /* scenario/ma2_scenario.c */
static void split_frame(uint32_t object, uint32_t vtable);
void ma2_menu_policy_pulse(uint32_t object, uint32_t vtable) {
    if (!valid_widget(object)) return;
    ma2_scn_menu_frame(object, vtable);   /* custom scenarios: Test file selected in the mission picker */
    split_frame(object, vtable);          /* split screen: guest join (quick join, Add Guest dialog) */
    if (vtable == LOBBY_MENU_VT) place_hold();
    int relevant = vtable == PAUSE_MENU_VT || vtable == CREDITS_MENU_VT;
    for (size_t i = 0; i < sizeof k_disabled / sizeof *k_disabled; i++) relevant |= k_disabled[i].menu_vt == vtable;
    if (!relevant) { g_kept_n = 0; return; }
    uint32_t begin = MEM32(object + 4), end = MEM32(object + 8);
    if (end < begin || end - begin > 1024) return;
    PolicyCtx ctx = { vtable, 2000, {0}, 0 };
    for (uint32_t e = begin; e < end; e += 4) {
        uint32_t form = MEM32(e);
        if (valid_widget(form)) policy_widget(MEM32(form + 0x38), 0, &ctx);
    }
    memcpy(g_kept, ctx.kept, sizeof g_kept); g_kept_n = ctx.kept_n;
}

/* ============================ split screen: guest join ============================ */
/* Owner scope (1.2.0): split screen on by default; the guest joins only in the Multiplayer menus,
 * with the original Add Guest item, or by pressing Menu/START on a connected controller that is not
 * player 1's ("quick join"). A quick join goes through the very function the Add Guest dialog ends
 * in: sub_00224753 (eax = controller, args (menu, 1)) -- the dialog's completion in the Multiplayer
 * menu's update (sub_00224362, 0x2243C3) calls it with the controller that pressed START. It adds the
 * player (sub_001F4BCB: local mask 0x50C9AC), sets 0x50C9B0 and re-splits the views (sub_0016B6D7).
 * Offline it does not touch the menu argument (that is only written when the session object
 * 0x524C14 has flag 0x18 bit 2, a networked session); the join is refused then off the Multiplayer menu.
 * The input layer decides which pad is free and routes it (xinput_device.c, split_route.h). */
#define MP_MENU_VT 0x0048C228u          /* Multiplayer: LOCAL XBOX / SYSTEM LINK / XBOX LIVE / ADD GUEST */
#define NEWGAME_MENU_VT 0x0048C2F8u     /* New Game (Game Type, map, bots) */
int  xbox_SplitScreenOn(void);
void xbox_SplitJoinWindow(int dialog);
int  xbox_SplitTakeJoin(void);
void xbox_SplitSetGuest(int slot);
int  xbox_SplitGuestSlot(void);
static int split_join_screen(uint32_t vtable) {
    static int lobby = -1;   /* RECOMP_SPLIT_JOIN_LOBBY=1: the Briefing Room too (test; see RESULT-splitscreen.md) */
    if (lobby < 0) lobby = env_or("RECOMP_SPLIT_JOIN_LOBBY", 0) != 0;
    return vtable == MP_MENU_VT || vtable == NEWGAME_MENU_VT || (lobby && vtable == LOBBY_MENU_VT);
}
static int popcount4u(unsigned m) { int n = 0; for (m &= 0xF; m; m &= m - 1) n++; return n; }
/* sub_00224753 with add = 1: what the Add Guest dialog calls once a controller pressed START. */
static int guest_add(uint32_t controller, uint32_t menu) {
    GuiRegisters r = save_registers();
    eax = controller;
    PUSH32(esp, 1); PUSH32(esp, menu);
    PUSH32(esp, 0x002243D5u); RECOMP_ABI_CALL(0x00224753u, sub_00224753);
    int ok = (int)(eax & 0xFF);
    restore_registers(&r);
    return ok;
}
static void split_frame(uint32_t object, uint32_t vtable) {
    static int on = -1, gone;
    if (on < 0) on = xbox_SplitScreenOn() != 0;
    if (!on) return;
    unsigned mask = MEM32(0x50C9AC) & 0xF;
    int humans_in = popcount4u(mask);
    int dialog = vtable == MP_MENU_VT && valid_widget(MEM32(object + 0x8C)) && MEM32(object + 0x80) != 0;
    int screen = split_join_screen(vtable);
    if (screen && humans_in < 2) xbox_SplitJoinWindow(dialog);
    int slot = xbox_SplitTakeJoin();
    if (slot >= 0) {
        uint32_t session = MEM32(0x524C14);
        uint32_t flags = valid_widget(session) ? MEM32(session + 0x18) : 0;
        int p1 = lowest_human(mask), controller = p1 == 0 ? 1 : -1;   /* the guest's pad is on guest port 1 */
        const char *why = !screen ? "not in the Multiplayer menus" : humans_in != 1 ? "a guest is already in"
                        : xbox_SplitGuestSlot() >= 0 ? "a pad is joining" : controller < 0 ? "player 1 is not controller 1"
                        : (flags & 0x10) ? "the session refuses guests" : ((flags & 4) && vtable != MP_MENU_VT) ? "networked session" : NULL;
        if (why) fprintf(stderr, "[PC-SPLIT] quick join from host slot %d ignored: %s\n", slot, why);
        else {
            xbox_SplitSetGuest(slot);
            int ok = guest_add((uint32_t)controller, vtable == MP_MENU_VT ? object : 0);
            unsigned after = MEM32(0x50C9AC) & 0xF;
            fprintf(stderr, "[PC-SPLIT] quick join: host slot %d -> controller %d (%s; local mask %X -> %X, menu %08X, session flags %X)\n",
                    slot, controller + 1, ok && popcount4u(after) >= 2 ? "joined" : "refused by the game", mask, after, vtable, flags);
            if (popcount4u(after) < 2) xbox_SplitSetGuest(-1);
            mask = after; humans_in = popcount4u(after);
        }
    }
    /* The guest left (Remove Guest) or a dialog join was refused: player 1 gets the active pad back. */
    if (xbox_SplitGuestSlot() >= 0 && humans_in < 2 && !dialog) {
        if (++gone >= 3) { xbox_SplitSetGuest(-1); gone = 0; }
    } else gone = 0;
    {   static unsigned last = 0xFFFF;
        if (mask != last) { fprintf(stderr, "[PC-SPLIT] local players mask %X (%d)\n", mask, humans_in); last = mask; } }
}

/* ============================ per-frame hook ============================ */
/* F5 (split screen): the free-for-all row's help says how many bots fit ("Bot opponents, up to 11.").
 * With a guest in the room is 10: the same sentence with the number the room allows, in every
 * language (the first "11" in the text is the count). */
static const wchar_t *ffa_row_text(const wchar_t *w, int room) {
    static wchar_t buf[256];
    const wchar_t *at = w ? wcsstr(w, L"11") : NULL;
    if (!at || room == 11 || room < 1 || room > 99 || wcslen(w) + 2 >= sizeof buf / sizeof *buf) return w;
    size_t head = (size_t)(at - w);
    wmemcpy(buf, w, head);
    swprintf(buf + head, sizeof buf / sizeof *buf - head, L"%d%ls", room, at + 2);
    return buf;
}
int ma2_bots_newgame_update(uint32_t menu) {
    if (!valid_widget(menu)) return 0;
    uint32_t form = MEM32(menu + 0x9C);
    uint32_t session = MEM32(0x4EBBFC);
    if (!valid_widget(form) || !valid_widget(session)) return 0;
    Ui *u = bind(menu, form);
    if (!u) return 0;                                   /* original form: do nothing at all */
    if (g_cfg_test == 2 && !u->populated) { g_cfg_test = 0; g_cfg_loaded = 0; }   /* after a Test match */

    if (MEM32(session + 0x4C) != 2) {                   /* not Local Xbox: rows stay hidden */
        for (int i = 0; i < ROWS; i++) widget_show(u->barrel[i], 0);
        return 0;
    }

    const Ma2BotConfig *cfg = ma2_bots_config();
    Ma2BotConfig c = *cfg;
    int team = team_mode(menu, session);
    int grinder = g_ui_gametype == 6;
    /* The rows list every count a match can have (11 = total slots minus one player); what a row
     * shows is cut to the room there is next to the humans (split screen: a guest takes a bot's
     * place), without saving the cut: the bot is back when the guest leaves (ma2_split_slots.h). */
    if (!u->populated) populate(u, &c, team, total_cap() - 1);

    /* Mode change (FFA <-> team): relabel row 910 and swap its saved value. */
    if (u->team != team) {
        u->team = team;
        if (valid_widget(u->label[ROW_RED])) ma2_gui_text(u->label[ROW_RED], guest_text(team ? TEXT_ROW_RED : TEXT_ROW_BOTS));
        u->cur[ROW_RED] = -1;   /* shown afresh below; the barrel's value is not a pick */
    }

    /* Read what the player chose. */
    int r = barrel_value(u->barrel[ROW_RED]), b = barrel_value(u->barrel[ROW_BLUE]), s = barrel_value(u->barrel[ROW_SKILL]);
    if (u->cur[ROW_RED] < 0) r = -1;
    if (s < 0) s = u->cur[ROW_SKILL];
    int saved_r = team ? c.red : c.ffa, saved_b = c.blue;
    int room_r, room_b = side_cap();
    unsigned mask = MEM32(0x50C9AC) & 0xF;
    if (team) {
        /* Each row is that side's bots, as set (no longer split evenly round the players). A side
         * holds 6 players, humans included. Changing a count (or opening the screen, or a guest
         * joining) places player 1 on the side the counts leave short; a team picked in the
         * Briefing Room afterwards stands. The guest keeps the game's own side (SPLIT_PLACE_GUEST). */
        int cap = side_cap(), hr = 0, hb = 0;
        int want_r = split_bots_pick(saved_r, u->cur[ROW_RED], r, cap), want_b = split_bots_pick(saved_b, u->cur[ROW_BLUE], b, cap);
        if (!u->placed || want_r != saved_r || want_b != saved_b || mask != u->mask) {
            u->placed = 1;
            /* F48: the design rule (players on the short side) no longer hangs on the hidden
             * AutoBalance key; it has its own switch, [Bots] ShortSide / RECOMP_BOT_SHORT_SIDE. */
            if (c7_on("shortside", "RECOMP_BOT_SHORT_SIDE", "ShortSide", 1)) { place_humans(session, want_r, want_b); c7_add("shortside", "placed", 1); }
        }
        human_sides(session, &hr, &hb);
        room_r = split_side_room(cap, hr); room_b = split_side_room(cap, hb);
    } else {
        /* Free-for-all: total slots minus humans. Grinder is two-player co-op as the game has it:
         * no bot or one, guest or not; showing 1 is not saved, Off is. */
        room_r = grinder ? 1 : split_ffa_room(total_cap(), humans());
    }
    u->mask = mask;
    int new_r = split_bots_pick(saved_r, u->cur[ROW_RED], r, room_r);
    int new_b = team ? split_bots_pick(saved_b, u->cur[ROW_BLUE], b, room_b) : saved_b;
    int show_r = split_bots_shown(new_r, room_r), show_b = split_bots_shown(new_b, room_b);
    if (r != show_r) barrel_select(u->barrel[ROW_RED], show_r, 0);
    if (team && b != show_b) barrel_select(u->barrel[ROW_BLUE], show_b, 0);
    {   /* The Bot Skill row focused: its one-line description replaces the screen's (see ma2_bots_desc_override). */
        uint32_t sb = u->barrel[ROW_SKILL];
        /* focus: the game shows a barrel's step arrows only while it has focus (sub_00212113); +0x74 bit 3 stays set */
        uint32_t la = 0, ra = 0;
        if (valid_widget(sb)) barrel_arrows(sb, &la, &ra);
        int focus = (valid_widget(la) && (MEM32(la + 0x74) & 2)) || (valid_widget(ra) && (MEM32(ra + 0x74) & 2));
        const wchar_t *w = focus && s >= 0 && s <= MA2_SKILL_DRAGOON ? ma2_bot_text(s == MA2_SKILL_DRAGOON ? MA2_TEXT_DESC_DRAGOON : MA2_TEXT_DESC_RECRUIT + s, bots_language()) : NULL;
        if (!w) {   /* W-32: the count rows say what they count and what limits them */
            for (int row = ROW_RED; row <= ROW_BLUE && !w; row++) {
                uint32_t cb = u->barrel[row], ca = 0, cr = 0;
                if (!valid_widget(cb)) continue;
                barrel_arrows(cb, &ca, &cr);
                if (!((valid_widget(ca) && (MEM32(ca + 0x74) & 2)) || (valid_widget(cr) && (MEM32(cr + 0x74) & 2)))) continue;
                int text = team ? MA2_TEXT_DESC_TEAM_ROWS : grinder ? MA2_TEXT_DESC_GRINDER_ROW : MA2_TEXT_DESC_FFA_ROW;
                w = ma2_bot_text(text, bots_language());
                if (text == MA2_TEXT_DESC_FFA_ROW) w = ffa_row_text(w, room_r);   /* split screen: "up to 10" with a guest */
            }
        }
        static int had;
        if (had && !w && valid_widget(u->desc)) ma2_gui_text(u->desc, 0x476708u);   /* focus left the row: blank, as the bot rows leave it */
        if (getenv("RECOMP_MENU_TRACE") && had != (w != NULL)) fprintf(stderr, "[BOT-UI] skill description %s (desc %08X)\n", w ? "on" : "off", u->desc);
        had = w != NULL;
        g_desc_widget = w && valid_widget(u->desc) ? u->desc : 0;
        g_desc_text = g_desc_widget ? ma2_pc_wide(w) : 0;
    }
    if (new_r != saved_r || (team && new_b != saved_b) || s != u->cur[ROW_SKILL]) {
        if (team) c.red = new_r; else c.ffa = new_r;
        if (team) c.blue = new_b;
        c.skill = s;
        ma2_bots_config_set(&c);
    }
    if (show_r != u->cur[ROW_RED] || show_b != u->cur[ROW_BLUE]) {
        static int last = -1; int key = show_r | show_b << 8 | (int)mask << 16 | team << 24;
        if (key != last && (getenv("RECOMP_BOT_DIAG") || (mask & (mask - 1)))) {   /* always with a guest in (proof) */
            last = key;
            fprintf(stderr, "[BOT-UI] rows %s: shown %d/%d (saved %d/%d, room %d/%d, humans mask %X)\n", team ? "team" : grinder ? "grinder" : "ffa",
                    show_r, show_b, new_r, new_b, room_r, room_b, mask);
        }
    }
    u->cur[ROW_RED] = show_r; u->cur[ROW_BLUE] = show_b; u->cur[ROW_SKILL] = s;

    /* Layout: stack under the lowest visible original row (29 px pitch). */
    static const unsigned original_rows[] = { 0x88, 0x74, 0x70, 0x84, 0x80, 0x7C, 0x78 };  /* 800,100,200,300,400,600,500 */
    int32_t bottom = 0; int any = 0;
    for (unsigned i = 0; i < sizeof original_rows / sizeof *original_rows; i++) {
        uint32_t w = MEM32(menu + original_rows[i]);
        if (!valid_widget(w) || !(MEM32(w + 0x74) & 2)) continue;
        int32_t y = SMEM32(w + 0x50);
        if (!any || y > bottom) bottom = y;
        any = 1;
    }
    int32_t first = any ? bottom + ROW_PITCH : u->y[ROW_RED][0] - 0;   /* nothing visible: keep authored slot */
    int32_t at = first, last = first;
    u->frames++;
    int show = u->frames >= 3;                          /* let the original screen settle first */
    for (int row = 0; row < ROWS; row++) {
        int visible = show && !(row == ROW_BLUE && !team);
        widget_show(u->barrel[row], visible);
        {   /* The arrows: each arrow shows itself while the barrel it is linked to (arrow+0xE0) has
             * focus and can step its way (+0xE4; sub_212113, every frame). The bot rows' arrows were
             * cloned from the Time Limit row and stayed linked to that (hidden) barrel, so the
             * clickable triangles never showed: link them to their own row. */
            uint32_t la = 0, ra = 0;
            barrel_arrows(u->barrel[row], &la, &ra);
            uint32_t arr[2] = { la, ra };
            for (int k = 0; k < 2; k++) {
                uint32_t was = valid_widget(arr[k]) ? MEM32(arr[k] + 0xE0) : 0;
                if (!was || was == u->barrel[row] || !valid_widget(was) || MEM32(was) != GUI_BARREL_VT) continue;   /* only a link to another barrel */
                if (getenv("RECOMP_MENU_TRACE")) fprintf(stderr, "[PC-ARROWS] bot row %d arrow %08X was linked to %08X: now %08X\n", row, arr[k], MEM32(arr[k] + 0xE0), u->barrel[row]);
                MEM32(arr[k] + 0xE0) = u->barrel[row];
            }
        }
        if (!visible) continue;
        place_row(u, row, at);
        last = at; at += ROW_PITCH;
    }
    /* Keep the description below the last row when they would collide
     * (retail gap between the last row and the description is 33 px). */
    if (valid_widget(u->desc)) {
        int32_t target = last + 33;
        if (target > u->desc_y && target < DESC_BELOW_FOOTER) target = DESC_BELOW_FOOTER;
        int32_t shift = target > u->desc_y ? target - u->desc_y : 0;
        SMEM32(u->desc + 0x50) = u->desc_y + shift;
        SMEM32(u->desc + 0x60) = u->desc_wy + shift;
    }
    return 0;                                           /* the original handler continues */
}
