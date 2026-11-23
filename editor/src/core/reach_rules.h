// "Can it get here?" rules: the ONE table of movement numbers, the unit list, and the profile builder.
// Pure data and maths (no ImGui, no D3D): linked into fmt_test.
//
// Every number has a source string and an `est` flag (estimated, not measured). Rule: Bots = min(planner, physical), so the
// Bots verdict is never wider than the Player verdict. The `bot` column below already holds that effective value.
// When a measurement lands (reach\MEASURED.md), edit the value, the source string and the est flag in this file only.
// Terrain vs object faces (rules update 3): the Player's walkers cross a TERRAIN face (both cells bare ground) up to terrain_slope /
// tank_terrain_slope (rise / run, measured); every other edge (roof, wall top, collider-raised cell) keeps the step rows (est.). Bots: the
// planner does not split the two, so their terrain rows are 0 = off. The two terrain rows are the ONLY place those numbers live.
//
// Source abbreviations (root: kit/source/game/src/bots/):
//   NAV = ma2_bot_nav.c   PATH = ma2_bot_path.c   CTRL = ma2_bot_ctrl.c   CORE = ma2_bots_core.c   BRAIN = ma2_bot_brain.c
//   wiki tNN = docs/wiki (fact title in quotes)   V4 = the reach verification notes
//   PLANNER-RULES / UNITS-AND-STATS = the reach notes (the file:line citations live there)
// A source string that names an estimate contains the word "est." (T-R10 checks that the flag and the text agree).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ma2 {

enum class ReachDriver : uint8_t { Player, Bots };
enum class ReachGroup : uint8_t { JetMech, Mech, BattleArmor, Foot, Tank, Vtol, Turret };

inline const char* reach_group_name(ReachGroup g) {
    switch (g) {
        case ReachGroup::JetMech: return "'Mechs with jump jets";
        case ReachGroup::Mech: return "'Mechs without jets";
        case ReachGroup::BattleArmor: return "Battle Armor";
        case ReachGroup::Foot: return "On foot (pilot)";
        case ReachGroup::Tank: return "Tanks";
        case ReachGroup::Vtol: return "VTOL";
        case ReachGroup::Turret: return "Turret (doesn't move)";
    }
    return "?";
}

// ---- 1. The rules table -----------------------------------------------------------------------------------------------------
// sense: how the Bots column relates to the Player column (T-R10 checks it): Limit = bot <= player (a reach), Need = bot >= player
// (a clearance), Same = equal, Free = no relation (a constant of the method).
enum class RuleSense : uint8_t { Limit, Need, Same, Free };

struct ReachRule {
    const char* key;
    float player;
    const char* player_src;
    bool player_est;
    float bot;
    const char* bot_src;
    RuleSense sense;
};

enum RuleId : int {
    RL_CELL,            // grid cell, m
    RL_MECH_STEP,       // 'Mech walk step per 2 m of ground (m)
    RL_BA_STEP,
    RL_FOOT_STEP,
    RL_TANK_STEP,
    RL_TANK_SLOPE,      // extra per-edge slope cap (rise / run), 0 = none
    RL_TERRAIN_SLOPE,   // terrain faces (both cells bare ground): 'Mech / BA / pilot slope cap, rise / run, 0 = no terrain rule
    RL_TANK_TERRAIN_SLOPE,  // the same for the tank
    RL_RADIUS_MECH,     // clearance radius, m
    RL_RADIUS_BA,
    RL_RADIUS_FOOT,
    RL_DROP_MECH_NOJET, // one-way step-off, max fall m
    RL_DROP_MECH_JET,
    RL_DROP_BA,
    RL_DROP_TANK,
    RL_DROP_FOOT,
    RL_DROP_RUN,        // step-off run limit, m
    RL_JET_PRIOR_PER_S, // jet climb prior = min(cap, per_s x JumpjetDuration)
    RL_JET_PRIOR_CAP,
    RL_JET_MARGIN,      // player climb = prior - margin; bot g_up = prior - margin
    RL_JET_UPX_ADD,     // bot g_upx = max(g_up + add, floor)
    RL_JET_UPX_FLOOR,
    RL_BA_CLIMB,
    RL_UP_RUN,          // jet up-link run over a blocked band, m
    RL_UP_MIN_RISE,
    RL_UP_BAND,         // in-between tops tolerance, m
    RL_HOP_RUN,
    RL_HOP_MARGIN,      // hop band tops <= start + climb - margin
    RL_DIVE_RUN,
    RL_DIVE_FALL_MIN,
    RL_LAND_EXTRA,      // jet / drop landing clearance = radius + this
    RL_UNDER_RUN,
    RL_UNDER_DECK,      // deck height over bare terrain, m
    RL_UNDER_LAND,      // landing within this of the start height, m
    RL_NATURAL,         // surface - terrain below this = bare ground, m
    RL_OBJ_MIN,         // collider top above terrain by more than this becomes the surface, m
    RL_LATCH,           // 1 = the BA claw can climb object faces
    RL_VTOL_CAP_DEFAULT,
    RL_VTOL_CAP_OFFSET, // ceiling = terrain + MaxVTOLHeight - this
    RL_VTOL_CLEAR,
    RL_VTOL_RIDER_EXTRA,
    RL_VTOL_HULL,
    RL_VTOL_LAND_SLOPE,
    RL_VTOL_LAND_RADIUS,
    RL_WATER_REFUSE,    // 1 = landings of jumps / drops / hops / under-links in water are refused
    RL_SPAWN_SNAP,      // a spawn snaps to the nearest free cell within this, m
    RL_DROP_POCKET,     // PocketDrop: the Bots' second-pass drop limit for a body in a small region, m (a planner pass, not a player rule)
    RL_POCKET_CELLS,    // PocketDrop applies while the walk region holds fewer cells than this (the planner's 40000 expansions)
    RL_COUNT
};

inline constexpr ReachRule kReachRules[RL_COUNT] = {
    {"cell", 2.f, "NAV:135", false, 2.f, "NAV:135", RuleSense::Same},
    {"mech_step", 2.80f, "OBJECT faces only (roof, wall top, collider-raised cell), not measured: V4-verify.md s2 Atlas 148% climbs / 171% blocked (est.); terrain faces use terrain_slope", true,
     1.10f, "NAV:142,412: planner heavy grid, 1.1 m per 2 m cell, breakables ignored", RuleSense::Limit},
    {"ba_step", 1.6f, "OBJECT faces only, not measured: planner light grid as the floor (est.); terrain faces use terrain_slope", true, 1.6f, "NAV:136: light grid 0.80 x 2 m", RuleSense::Limit},
    {"foot_step", 1.32f, "OBJECT faces only, not measured: wiki t31 FootSlope 66 keeps the Killing Fields walk, 55 broke it (bracket, est.); terrain faces use terrain_slope", true,
     1.32f, "BRAIN:546-555: the planner's own FootPrecheck setting (slope_max 66% x 2 m), not a physical estimate", RuleSense::Limit},
    {"tank_step", 1.1f, "OBJECT faces only, not measured: wiki t22 TankSlope (est.); terrain faces use tank_terrain_slope", true, 1.1f, "CTRL:2337, PATH:150: light grid 1.6 AND slp <= 55 = 1.1 (physical est.)", RuleSense::Limit},
    {"tank_slope", 0.f, "no extra cap: the step rule above is the limit", false, 0.55f, "CTRL:2337, PATH:150: slp > 55 impassable", RuleSense::Free},
    {"terrain_slope", 8.25f,
     "TERRAIN faces (both cells bare ground): Atlas, Hackman, Mad Dog, BA and pilot climbed a 66 m single-cell face (66 m over 8 m = 825%), the highest one built; nothing above it measured, so beyond it is can't (MEASURED.md M2: runs\\w66_*.csv, s_atlas_*.csv)", false,
     0.f, "none: the planner does not split terrain from objects, its step rows apply (mech_step, ba_step, foot_step)", RuleSense::Limit},
    {"tank_terrain_slope", 6.25f,
     "TERRAIN faces: the Rommel tank climbed a +50 m single-cell face (625%) and was stopped at +66 m (825%): the lower bracket (MEASURED.md M2: runs\\j_rommel.csv, w66_rommel.csv)", false,
     0.f, "none: the planner does not split terrain from objects, tank_step applies", RuleSense::Limit},
    {"radius_mech", 2.6f, "CTRL:68-82", false, 2.6f, "CTRL:68-82", RuleSense::Need},
    {"radius_ba", 1.4f, "CTRL:68-82", false, 1.4f, "CTRL:68-82", RuleSense::Need},
    {"radius_foot", 0.8f, "CTRL:68-82", false, 0.8f, "CTRL:68-82", RuleSense::Need},
    {"drop_mech_nojet", 30.f, "terrain edges; object edges not tested: 6 to 30 m, all four bodies came down without a stop (MEASURED.md M3: runs\\m3_atlas.csv); 30 is a measured lower bound", false,
     60.f, "PATH:705-755, CTRL:2352: planner 60 on trips. Every drop link starts, crosses and lands on bare terrain (PATH:705-755), and the physical cap on a terrain face is the slope rule (the walkers go up and down it), so bots = the planner's 60 there; object edges: min(60, the player's 30)", RuleSense::Free},
    {"drop_mech_jet", 60.f, "CTRL:2352: falls do no damage, jets cushion", false, 60.f, "CTRL:2352", RuleSense::Limit},
    {"drop_ba", 60.f, "CTRL:2352; terrain edges to 30 m also measured (MEASURED.md M3: runs\\m3_ba.csv)", false, 60.f, "CTRL:2352", RuleSense::Limit},
    {"drop_tank", 30.f, "terrain edges; object edges not tested: 6 to 30 m, the Rommel came down without a stop (MEASURED.md M3: runs\\m3_rommel.csv); 30 is a measured lower bound", false,
     6.f, "CTRL:2349-2356: the planner's own limit", RuleSense::Limit},
    {"drop_foot", 30.f, "terrain edges; object edges not tested: 6 to 30 m, the pilot came down without a stop (MEASURED.md M3: runs\\m3_pilot.csv); 30 is a measured lower bound", false,
     10.f, "CTRL:2349; BRAIN:546: the planner's own limit", RuleSense::Limit},
    {"drop_run", 20.f, "PATH:235-236 (PATH_DROP_REACH 10 cells)", false, 20.f, "PATH:235-236", RuleSense::Same},
    {"jet_prior_per_s", 3.4f, "CTRL:2373-2378: bot prior per second of JumpjetDuration", false, 3.4f, "CTRL:2373-2378", RuleSense::Same},
    {"jet_prior_cap", 24.f, "CTRL:2373-2378; wiki t32 '24-25 m per burn'", false, 24.f, "CTRL:2373-2378", RuleSense::Same},
    {"jet_margin", 1.f, "measured apex gain minus a 1 m margin (MEASURED.md M1: runs\\m1_<chassis>.csv); the per-chassis value is in the unit list", false, 2.f, "PATH:263-274: g_up = prior - 2", RuleSense::Need},
    {"jet_upx_add", 4.f, "(planner constant, not a physical limit)", false, 4.f, "PATH:263-274: g_upx = max(g_up + 4, 20)", RuleSense::Same},
    {"jet_upx_floor", 20.f, "(planner constant, not a physical limit)", false, 20.f, "PATH:263-274", RuleSense::Same},
    {"ba_climb", 18.7f, "apex 19.71 m minus 1 m (MEASURED.md M1: runs\\m1_ba.csv); the in-match UpStretch median is 16.9, p90 18.5 (wiki t14)", false, 16.f, "g_up 16 (CTRL:2368-2378, UpStretch median 16.9); the planner stretches it to g_upx = max(g_up + 4, 20) = 20 (PATH:266-273, up-links PATH:792-835), which is the bots' reach over a terrain face (every up-link's band is bare terrain, PATH:797-821; the physical cap there is the slope rule); object faces: min(g_upx, the player's 18.7)", RuleSense::Limit},
    {"up_run", 28.f, "PATH:251-274 (PATH_HOP_LONG 14 cells)", false, 28.f, "PATH:251-274", RuleSense::Same},
    {"up_min_rise", 1.2f, "PATH:251-274 (PATH_UP_MIN)", false, 1.2f, "PATH:251-274", RuleSense::Same},
    {"up_band", 0.5f, "PATH:251-274: tops <= start + climb + 0.5, none above landing + 0.5", false, 0.5f, "PATH:251-274", RuleSense::Same},
    {"hop_run", 12.f, "PATH:28,881 (PATH_HOP_REACH 6 cells)", false, 12.f, "PATH:28,881", RuleSense::Same},
    {"hop_margin", 1.f, "PATH:881: band tops <= start + climb - 1", false, 1.f, "PATH:881", RuleSense::Same},
    {"dive_run", 40.f, "PATH:31,858-887 (PATH_DIVE_LONG 20 cells)", false, 40.f, "PATH:31,858-887", RuleSense::Same},
    {"dive_fall_min", 1.f, "PATH:858-887", false, 1.f, "PATH:858-887", RuleSense::Same},
    {"land_extra", 2.f, "PATH:251-274,705-755: landing clearance r + 2", false, 2.f, "PATH:251-274,705-755", RuleSense::Same},
    {"under_run", 32.f, "PATH:254-255,758-785 (PATH_UNDER_REACH 16 cells)", false, 32.f, "PATH:254-255,758-785", RuleSense::Same},
    {"under_deck", 8.f, "PATH:758-785: deck >= 8 m over bare terrain", false, 8.f, "PATH:758-785", RuleSense::Same},
    {"under_land", 1.5f, "PATH:758-785: landing within 1.5 m of the start height", false, 1.5f, "PATH:758-785", RuleSense::Same},
    {"natural", 0.5f, "PATH:215-231: bare ground = surface - terrain < 0.5", false, 0.5f, "PATH:215-231", RuleSense::Same},
    {"obj_min", 0.4f, "NAV:143,392-399: collider top > terrain + 0.4 becomes the surface", false, 0.4f, "NAV:143,392-399", RuleSense::Same},
    {"latch", 1.f, "wiki t12 BA claw latch surface rules: MACorporeal faces, never raw terrain; a building can veto it (est.)", true,
     0.f, "PLANNER-RULES 2.3: the planner has no latch link, bots do not plan it", RuleSense::Limit},
    {"vtol_cap_default", 55.f, "wiki t18 ctfvtol-maxvtolheight: default MaxVTOLHeight of VTOL - Player (per instance, not per map)", false,
     55.f, "CTRL:1873-1878", RuleSense::Same},
    {"vtol_cap_offset", 6.6f, "wiki t18 (measured 48.0-48.7 over NRC, 28.2-28.4 over the Bogged Down bog); terrain-relative, confirmed by MEASURED.md M4: 49.55 m over flat ground, +20.9 m over a 26 m mesa (runs\\v_ceil.csv)", false, 6.6f, "CTRL:1873-1878,2212-2215", RuleSense::Same},
    {"vtol_clear", 2.f, "none measured (est.)", true, 7.f, "CTRL:1956 (13 with a rider)", RuleSense::Need},
    {"vtol_rider_extra", 4.7f, "wiki t43 vtol-rider-dodge-towers: a BA hangs 4.7 m under the skids", false, 6.f, "CTRL:1956: 13 with a rider, 7 without", RuleSense::Need},
    {"vtol_hull", 4.5f, "hull half-width over the ground (est.)", true, 4.5f, "(est.) same as the player", RuleSense::Same},
    {"vtol_land_slope", 0.30f, "30% settles for 10 s, 50% slides 85 m in 7 s (MEASURED.md M4: runs\\v_land30.csv, v_land50.csv)", false, 0.30f, "same as the player: 30% settles (MEASURED.md M4)", RuleSense::Same},
    {"vtol_land_radius", 5.f, "no measurement (est. until M4)", true, 5.f, "(est. until M4)", RuleSense::Same},
    {"water_refuse", 0.f, "water is passable: Atlas, Rommel and pilot crossed 4.7 m and 1.2 m editor ponds at full speed (MEASURED.md M6: runs\\m6_*.csv)", false,
     1.f, "PATH:592,744,779,814,879: landings refused in water", RuleSense::Need},
    {"spawn_snap", 10.f, "FM:148-158 (floor_model start snap, 10 m)", false, 10.f, "FM:148-158", RuleSense::Same},
    {"drop_pocket", 0.f, "not a player rule: a planner pass (the player steps off whatever his body survives)", false,
     40.f, "PATH:27,658-663,905-925 PocketDrop: a dry search that ended 30 m+ short after < 40000 expansions restarts with a drop limit of 40", RuleSense::Free},
    {"pocket_cells", 40000.f, "(planner constant, not a player rule)", false, 40000.f, "PATH:911: expanded < 40000 (approximated by the cells of the walk region)", RuleSense::Free},
};
static_assert(sizeof(kReachRules) / sizeof(kReachRules[0]) == RL_COUNT, "one rule row per RuleId");

// True when a source string names an estimate: the text "est." not preceded by a letter.
constexpr bool src_has_est(const char* s) {
    for (size_t i = 0; s[i]; i++) {
        const bool prev_letter = i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z'));
        if (!prev_letter && s[i] == 'e' && s[i + 1] == 's' && s[i + 2] == 't' && s[i + 3] == '.') return true;
    }
    return false;
}
constexpr const ReachRule& reach_rule(RuleId id) { return kReachRules[id]; }
constexpr float rule_value(RuleId id, ReachDriver d) { return d == ReachDriver::Bots ? kReachRules[id].bot : kReachRules[id].player; }
constexpr bool rule_est(RuleId id, ReachDriver d) { return d == ReachDriver::Bots ? src_has_est(kReachRules[id].bot_src) : kReachRules[id].player_est; }

// The bot prior of a jet 'Mech's climb per full burn (CTRL:2373-2378).
constexpr float jet_prior(float duration) {
    const float v = kReachRules[RL_JET_PRIOR_PER_S].player * duration;
    return v < kReachRules[RL_JET_PRIOR_CAP].player ? v : kReachRules[RL_JET_PRIOR_CAP].player;
}

// ---- 2. The unit list ---------------------------------------------------------------------------------------------------------
struct ReachUnit {
    const char* label;
    ReachGroup group;
    int tpl;             // template uid (0 = not known)
    int start;           // start container uid for --start-in (0 = none: AI only, or reached through another container)
    const char* cls;     // MAMech / MATank / MAFlyer
    float jet_duration;  // catalog JumpjetDuration (s), 0 = no jump jets
    float climb;         // player jet climb (m): measured apex - 1; 0 = none
    const char* climb_src;
    bool climb_est;
    bool ai_only;        // not player-usable: listed as "AI only, same as a tank"
};

#define REACH_NOJET_SRC "no jump jets (catalog: every jet field is 0)"
// Jet 'Mech: climb = measured apex gain - 1 m (MEASURED.md M1), the source names the run. REACH_JE: not run, same jet stats as a measured chassis (est.).
// The jet ceiling is relative to the ground under the 'Mech, and holding W against a terrain wall rides it up (66 m tested). Terrain faces are
// walkable anyway (terrain_slope), so the jet climb only decides object faces and gaps. The Bots column keeps the planner prior (jet_prior, CTRL:2373).
#define REACH_JM(label, tpl, start, dur, climb, run) {label, ReachGroup::JetMech, tpl, start, "MAMech", dur, climb, "measured apex gain minus 1 m (M1: runs\\" run ".csv)", false, false}
#define REACH_JE(label, tpl, start, dur, climb, same) {label, ReachGroup::JetMech, tpl, start, "MAMech", dur, climb, "not run: same jet stats and mass as " same " (est.)", true, false}
#define REACH_NJ(label, tpl, start) {label, ReachGroup::Mech, tpl, start, "MAMech", 0.f, 0.f, REACH_NOJET_SRC, false, false}
#define REACH_BA(label, tpl, start) {label, ReachGroup::BattleArmor, tpl, start, "MAMech", 2.f, 18.7f, "apex 19.71 m minus 1 m (M1: runs\\m1_ba.csv); in-match UpStretch median 16.9 (wiki t14)", false, false}
#define REACH_FT(label, tpl, start) {label, ReachGroup::Foot, tpl, start, "MAMech", 0.f, 0.f, "no jump jets (pilot)", false, false}
#define REACH_TK(label, tpl, start, ai) {label, ReachGroup::Tank, tpl, start, "MATank", 0.f, 0.f, "tracked tank: no jets", false, ai}

inline constexpr ReachUnit kReachUnits[] = {
    // 'Mechs with jump jets (14), start container uids from UNITS-AND-STATS 1a
    REACH_JM("Kit Fox", 27908, 124033, 5.f, 26.3f, "m1_kitfox"),
    REACH_JM("Uller", 27269, 124047, 5.f, 26.3f, "m1_uller"),
    REACH_JM("Corvus", 32689, 124028, 4.25f, 26.3f, "m1_corvus"),
    REACH_JM("Puma", 27872, 124040, 5.f, 25.0f, "m1_puma"),
    REACH_JM("Stiletto", 124691, 124698, 7.f, 24.2f, "m1_stiletto"),
    REACH_JM("Raptor", 32911, 124042, 7.f, 24.3f, "m1_raptor"),
    REACH_JM("Cougar", 27265, 124029, 5.f, 24.3f, "m1_cougar2"),
    REACH_JM("Belial", 27852, 124021, 6.f, 24.8f, "m1_belial"),
    REACH_JE("Uziel", 27261, 124048, 6.f, 24.8f, "the Belial"),
    REACH_JM("Bowman", 27802, 124026, 8.f, 25.8f, "m1_bowman"),
    REACH_JM("Catapult", 27283, 124027, 8.f, 25.7f, "m1_catapult"),
    REACH_JM("Hellbringer", 32693, 124032, 5.f, 23.7f, "m1_hellbringer"),
    REACH_JE("Summoner", 27786, 124044, 5.f, 23.7f, "the Hellbringer and Thor"),
    REACH_JM("Thor", 27275, 124045, 5.f, 23.7f, "m1_thor"),
    // 'Mechs without jets (16)
    REACH_NJ("Hackman", 27928, 124031),
    REACH_NJ("Owens", 27273, 124038),
    REACH_NJ("Raven", 32683, 124043),
    REACH_NJ("Mad Dog", 27828, 124035),
    REACH_NJ("Vulture", 27257, 124049),
    REACH_NJ("Loki", 32687, 124034),
    REACH_NJ("MadCat", 27279, 124036),
    REACH_NJ("Timber Wolf", 27758, 124046),
    REACH_NJ("Star Adder", 124678, 124687),
    REACH_NJ("Wendigo", 124626, 124672),
    REACH_NJ("Atlas", 27285, 124022),
    REACH_NJ("Blood Asp", 123517, 124023),
    REACH_NJ("NovaCat", 123413, 124037),
    REACH_NJ("Prometheus", 27715, 124039),
    REACH_NJ("Ragnarok", 28206, 124041),
    REACH_NJ("Ymir", 31634, 124050),
    // Battle Armor (one stat set, three templates)
    REACH_BA("Battle Armor", 32956, 124001),
    REACH_BA("Battle Armor (near clip)", 125891, 125894),
    REACH_BA("Elemental", 36132, 125292),
    // On foot: the six pilot templates share one movement body
    REACH_FT("Pilot", 32986, 123999),
    REACH_FT("Natalia", 116817, 0),
    REACH_FT("Foster", 116816, 0),
    REACH_FT("Alera", 125501, 0),
    REACH_FT("Nemesis", 125493, 0),
    REACH_FT("Pilot, no bomb", 125549, 125553),
    // Tanks: the Rommel Mk2 (player) and the AI-only tracked tanks (no hover vehicle exists in MA2)
    REACH_TK("Rommel Mk2 tank", 35997, 124052, false),
    REACH_TK("Rommel Mk2 (Kurita, player)", 126157, 126165, false),
    REACH_TK("Rommel Mk2 (WOB)", 124185, 125370, false),
    REACH_TK("Manticore (AI only)", 40026, 0, true),
    REACH_TK("Partisan (AI only)", 116804, 0, true),
    REACH_TK("Gator (AI only)", 36072, 0, true),
    REACH_TK("APC Laser (AI only)", 125772, 0, true),
    REACH_TK("APC Missile (AI only)", 0, 0, true),
    REACH_TK("APC AutoCannon (AI only)", 125765, 0, true),
    REACH_TK("APC Laser Friendly (AI only)", 125986, 0, true),
    REACH_TK("APC for VTOL (AI only)", 125380, 0, true),
    // VTOL
    {"VTOL", ReachGroup::Vtol, 33119, 124051, "MAFlyer", 0.f, 0.f, "flies: ceiling = terrain + MaxVTOLHeight - 6.6 (wiki t18)", false, false},
    // Turrets: immobile
    {"Turret: Laser", ReachGroup::Turret, 116318, 124334, "MATank", 0.f, 0.f, "immobile", false, false},
    {"Turret: Autocannon", ReachGroup::Turret, 116471, 0, "MATank", 0.f, 0.f, "immobile", false, false},
    {"Turret: LRM", ReachGroup::Turret, 116472, 0, "MATank", 0.f, 0.f, "immobile", false, false},
    {"Turret: Gauss", ReachGroup::Turret, 116473, 0, "MATank", 0.f, 0.f, "immobile", false, false},
    {"Turret: Guided Missile", ReachGroup::Turret, 124226, 0, "MATank", 0.f, 0.f, "immobile", false, false},
};
inline constexpr int kReachUnitCount = int(sizeof(kReachUnits) / sizeof(kReachUnits[0]));
#undef REACH_NOJET_SRC
#undef REACH_JM
#undef REACH_JE
#undef REACH_NJ
#undef REACH_BA
#undef REACH_FT
#undef REACH_TK

inline int reach_find_unit(const char* label) {
    for (int i = 0; i < kReachUnitCount; i++)
        if (std::strcmp(kReachUnits[i].label, label) == 0) return i;
    return -1;
}

// ---- 3. The profile: what one (unit, driver) can do, built from the table ------------------------------------------------------
enum : uint32_t {  // which estimated rules a profile leans on (reach_explain appends "(est.)" when one of them decided)
    E_STEP = 1, E_DROP = 2, E_CLIMB = 4, E_LATCH = 8, E_VTOL = 16,
};

struct ReachProfile {
    const ReachUnit* unit = nullptr;
    ReachDriver driver = ReachDriver::Player;
    ReachGroup group = ReachGroup::Mech;
    float step = 2.8f;         // largest surface change per 2 m of ground a walker crosses
    float slope_cap = 0.f;     // extra per-edge cap, rise / run (0 = none)
    float terrain_slope = 0.f; // cap on a terrain face (both cells bare ground), rise / run; 0 = no terrain rule (the step rule applies to every edge)
    float radius = 2.6f;       // clearance radius
    float drop = 0.f;          // largest one-way fall (0 = no step-off)
    float drop_run = 20.f;
    float climb = 0.f;         // jet up-link rise limit over an OBJECT face (0 = no jets): the Bots' min(planner g_upx, measured apex - 1)
    float climb_terr = 0.f;    // the same over a TERRAIN face: the Bots' planner g_upx (the physical cap there is the slope rule); the player's = climb
    float drop_terr = 0.f;     // one-way fall limit over bare terrain: the Bots' planner value; the player's = drop
    float drop_pocket = 0.f;   // PocketDrop (Bots): the limit while the walk region is small, else 0
    int pocket_cells = 0;
    float hop_climb = 0.f;     // hop band tops <= start + this
    float up_run = 28.f, hop_run = 12.f, dive_run = 40.f, under_run = 32.f, up_min = 1.2f, band_tol = 0.5f, land_extra = 2.f;
    bool jets = false, latch = false, ride = false, flies = false, heavy = false, immobile = false, refuse_water = false;
    float vtol_clear = 2.f, vtol_hull = 4.5f, rider_extra = 4.7f, land_slope = 0.25f, land_radius = 5.f, ceil_offset = 6.6f;
    uint32_t est = 0;
};

inline ReachProfile reach_profile(const ReachUnit& u, ReachDriver d) {
    ReachProfile p;
    p.unit = &u;
    p.driver = d;
    p.group = u.group;
    const bool bots = d == ReachDriver::Bots;
    auto V = [&](RuleId id) { return rule_value(id, d); };
    auto E = [&](RuleId id, uint32_t bit) { return rule_est(id, d) ? bit : 0u; };
    p.refuse_water = V(RL_WATER_REFUSE) > 0.5f;
    p.drop_run = V(RL_DROP_RUN);
    p.up_run = V(RL_UP_RUN);
    p.hop_run = V(RL_HOP_RUN);
    p.dive_run = V(RL_DIVE_RUN);
    p.under_run = V(RL_UNDER_RUN);
    p.up_min = V(RL_UP_MIN_RISE);
    p.band_tol = V(RL_UP_BAND);
    p.land_extra = V(RL_LAND_EXTRA);
    p.ceil_offset = V(RL_VTOL_CAP_OFFSET);
    if (bots) {
        p.drop_pocket = V(RL_DROP_POCKET);
        p.pocket_cells = int(V(RL_POCKET_CELLS));
    }
    switch (u.group) {
        case ReachGroup::JetMech:
        case ReachGroup::Mech:
            p.heavy = true;
            p.step = V(RL_MECH_STEP);
            p.terrain_slope = V(RL_TERRAIN_SLOPE);
            p.radius = V(RL_RADIUS_MECH);
            p.est |= E(RL_MECH_STEP, E_STEP);
            if (u.group == ReachGroup::Mech) {
                p.drop_terr = V(RL_DROP_MECH_NOJET);  // Bots: the planner's 60 over terrain; object edges keep the player's measured 30
                p.drop = bots ? std::min(p.drop_terr, rule_value(RL_DROP_MECH_NOJET, ReachDriver::Player)) : p.drop_terr;
                p.est |= E(RL_DROP_MECH_NOJET, E_DROP);
                break;
            }
            p.drop = p.drop_terr = V(RL_DROP_MECH_JET);
            p.jets = true;
            p.est |= u.climb_est ? E_CLIMB : 0u;
            p.climb = p.climb_terr = u.climb;
            p.hop_climb = u.climb - V(RL_HOP_MARGIN);
            if (bots) {  // the planner's own numbers (PATH:263-274): g_up = prior - 2, rise <= min(g_upx, player climb)
                const float g_up = jet_prior(u.jet_duration) - V(RL_JET_MARGIN);
                const float g_upx = std::max(g_up + V(RL_JET_UPX_ADD), V(RL_JET_UPX_FLOOR));
                p.climb = std::min(g_upx, u.climb);
                p.climb_terr = g_upx;  // every planner up-link crosses bare terrain only (PATH:797-821): its own reach, not the measured apex
                p.hop_climb = std::min(g_up, u.climb) - V(RL_HOP_MARGIN);
            }
            break;
        case ReachGroup::BattleArmor:
            p.step = V(RL_BA_STEP);
            p.terrain_slope = V(RL_TERRAIN_SLOPE);
            p.radius = V(RL_RADIUS_BA);
            p.drop = p.drop_terr = V(RL_DROP_BA);
            p.jets = true;
            p.climb = p.climb_terr = std::min(V(RL_BA_CLIMB), u.climb);
            p.hop_climb = p.climb - V(RL_HOP_MARGIN);
            if (bots) {  // g_up = the row (16); the planner stretches it to g_upx = max(g_up + 4, 20) (PATH:266-273)
                const float g_up = V(RL_BA_CLIMB);
                const float g_upx = std::max(g_up + V(RL_JET_UPX_ADD), V(RL_JET_UPX_FLOOR));
                p.climb = std::min(g_upx, u.climb);
                p.climb_terr = g_upx;
                p.hop_climb = std::min(g_up, u.climb) - V(RL_HOP_MARGIN);
            }
            p.latch = V(RL_LATCH) > 0.5f;
            p.ride = true;
            p.est |= E(RL_BA_STEP, E_STEP) | E(RL_BA_CLIMB, E_CLIMB) | (p.latch ? E_LATCH : 0u);
            break;
        case ReachGroup::Foot:
            p.step = V(RL_FOOT_STEP);
            p.terrain_slope = V(RL_TERRAIN_SLOPE);
            p.radius = V(RL_RADIUS_FOOT);
            p.drop = p.drop_terr = V(RL_DROP_FOOT);
            p.est |= E(RL_FOOT_STEP, E_STEP) | E(RL_DROP_FOOT, E_DROP);
            break;
        case ReachGroup::Tank:
            p.step = V(RL_TANK_STEP);
            p.slope_cap = V(RL_TANK_SLOPE);
            p.terrain_slope = V(RL_TANK_TERRAIN_SLOPE);
            p.radius = V(RL_RADIUS_MECH);
            p.drop = p.drop_terr = V(RL_DROP_TANK);
            p.est |= E(RL_TANK_STEP, E_STEP) | E(RL_DROP_TANK, E_DROP);
            break;
        case ReachGroup::Vtol:
            p.flies = true;
            p.vtol_clear = V(RL_VTOL_CLEAR);
            p.rider_extra = V(RL_VTOL_RIDER_EXTRA);
            p.vtol_hull = V(RL_VTOL_HULL);
            p.land_slope = V(RL_VTOL_LAND_SLOPE);
            p.land_radius = V(RL_VTOL_LAND_RADIUS);
            p.est |= E_VTOL;
            break;
        case ReachGroup::Turret:
            p.immobile = true;
            p.radius = V(RL_RADIUS_MECH);
            break;
    }
    return p;
}
inline ReachProfile reach_profile(int unit_index, ReachDriver d) { return reach_profile(kReachUnits[unit_index], d); }

// The VTOL a BA rides: the same VTOL profile with the rider clearance, taken from the first VTOL row of the unit list.
inline ReachProfile reach_vtol_profile(ReachDriver d) {
    for (int i = 0; i < kReachUnitCount; i++)
        if (kReachUnits[i].group == ReachGroup::Vtol) return reach_profile(i, d);
    return ReachProfile();
}

}  // namespace ma2
