// Format library tests: run against the user's own game_files (read-only).
//   fmt_test.exe [--game <install folder>] [--quick]
// Exit code 0 when every check passes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>

#include "../src/core/brush.h"
#include "../src/core/crop.h"
#include "../src/core/ground_layers.h"
#include "../src/core/culture_bits.h"
#include "../src/core/cutscene.h"
#include "../src/core/gamedata.h"
#include "../src/core/json.h"
#include "../src/core/lossless.h"
#include "../src/core/mgf.h"
#include "../src/core/mgic.h"
#include "../src/core/mission_extras.h"
#include "../src/core/names.h"
#include "../src/core/particles.h"
#include "../src/core/pathfind.h"
#include "../src/core/placement.h"
#include "../src/core/scenario.h"
#include "../src/core/snap.h"
#include "../src/core/templates.h"
#include "../src/core/terrain.h"
#include "../src/core/triggers.h"
#include "../src/core/wdf.h"
#include "../src/core/weather.h"
#include "../src/core/lamp_rule.h"
#include "../src/app/version.h"
#include "../src/core/water_custom.h"
#include "../src/app/tool_hints.h"
#include "../src/app/text_util.h"
#include "../src/app/page_map.h"
#include "../src/app/palette_rows.h"
#include "../src/app/palette_rank.h"
#include "../src/app/input_map.h"
#include "../src/app/help_text.h"
#include "../src/app/select_rules.h"
#include "../src/app/snap_prefs.h"
#include "../src/app/undo_rules.h"
#include "../models/ma2models.h"


#include <windows.h>

using namespace ma2;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) g_pass++;                                  \
        else { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    } while (0)

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static const uint32_t K_TERRAIN = key_of("MATerrain"), K_TINFO = key_of("MATerrainInfo");

// Every editor tool has a status-bar line from the one template, ASCII only, and every
// tool other than Select names its stop key.
// The tool strip's pages: every tool belongs to exactly one page, every page lists the pickers it must show (the same
// list as tools\pickers.json, which tools\pickers_check.py reads), and the eight page keys are in the input table without a clash.
static void test_pages() {
    using namespace ed;
    bool seen[kPageCount] = {};
    for (int t = 0; t < int(Tool::Count); t++) {
        const Page p = page_of(Tool(t));
        CHECK(int(p) >= 0 && int(p) < kPageCount, "tool %d has no page", t);
        if (int(p) >= 0 && int(p) < kPageCount) seen[int(p)] = true;
    }
    for (int p = 0; p < kPageCount; p++) {
        CHECK(seen[p] || Page(p) == Page::RoadsWalls, "page %s has no tool", page_name(Page(p)));  // Roads & walls arms PlaceObject, like Place
        CHECK(page_pickers(Page(p))[0] != nullptr, "page %s lists no required picker", page_name(Page(p)));
        CHECK(strstr(page_strip_label(Page(p)), "##strip") != nullptr, "page %d strip label", p);
    }
    std::string json;
    for (const char* path : {"tools/pickers.json", "../tools/pickers.json"}) {
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) json.append(b, n);
        fclose(f);
        break;
    }
    if (!json.empty()) {
        // JSON escapes & as-is; "##" and "+" are plain text, so each item is a plain substring of the file
        for (int p = 0; p < kPageCount; p++)
            for (const char* const* it = page_pickers(Page(p)); *it; ++it) CHECK(json.find(std::string("\"") + *it + "\"") != std::string::npos, "pickers.json lacks '%s' (page %s)", *it, page_name(Page(p)));
    }
    int rows = 0;
    for (int a = 0; a < A_COUNT; a++)
        if (strcmp(kActTable[a].section, "Tools") == 0) {
            rows++;
            CHECK(kActTable[a].def[0].mods == KM_SHIFT && a - A_PAGE_SELECT == rows - 1, "page key row %d", rows);
        }
    CHECK(rows == 8, "eight page rows in the input table (%d)", rows);
}

// The palette browser's row packing: every entry once, in order, a heading on its own row, never more than `cols` cells a row.
static void test_pack_rows() {
    using namespace ed;
    for (int cols : {1, 3, 5, 9})
        for (int n : {0, 1, 4, 10, 37}) {
            std::vector<std::pair<int, int>> heads = {{6, 1}, {0, 0}, {20, 2}, {n, 3}};  // out of order; one before the first entry, one past the end
            auto rows = pack_rows(n, heads, cols);
            int next = 0, headings = 0;
            bool ok = true;
            for (auto& r : rows) {
                if (r.heading >= 0) { headings++; ok &= r.count == 0; continue; }
                ok &= r.first == next && r.count >= 1 && r.count <= cols;
                next = r.first + r.count;
            }
            CHECK(ok && next == n, "pack_rows cols %d n %d: every entry once, in order (%d of %d)", cols, n, next, n);
            int want = (n > 0 ? 1 : 0) + (n > 6 ? 1 : 0) + (n > 20 ? 1 : 0);  // the heading at index n has nothing under it
            CHECK(headings == want, "pack_rows cols %d n %d: %d headings, wanted %d", cols, n, headings, want);
        }
    // a heading ends the row before it: entries 0-3, heading, 4-6 with 5 columns = rows of 4 and 3
    auto rows = pack_rows(7, {{4, 0}}, 5);
    CHECK(rows.size() == 3 && rows[0].count == 4 && rows[1].heading == 0 && rows[2].first == 4 && rows[2].count == 3, "pack_rows: a heading ends the row before it");
    ed::ToolHintCtx c;
    c.scatter_empty = true;
    CHECK(ed::tool_hint_parts(ed::Tool::ScatterTrees, c).click.find("pick objects below first") != std::string::npos, "scatter hint with an empty brush set");
}

// The palette search ranking. Fixtures only, no game data.
static void test_palette_rank() {
    using namespace ed;
    auto T = [](const char* vis, const char* off, const char* ali) { RankText t; t.visible = vis; t.official = off; t.aliases = ali; return t; };
    const RankText tree = T("Dante city tree 01 Scenery", "Env_DAN_CityTree01\nenv_dan_citytree01", "plant vegetation foliage forest wood Trees");
    const RankText road = T("Long straight road piece", "longstraight.road\nlongstraight", "street avenue pavement asphalt");
    const RankText fence = T("Security fence Building", "Env_SecurityFence\nenv_securityfence", "wall barrier");
    const RankText turret = T("Auto cannon turret Turret", "Turret_AutoCannon\nturret_autocannon", "gun cannon emplacement defense defence stationary");
    const RankText mech = T("Atlas 'Mech", "Mech_Atlas\nmech_atlas", "battlemech robot walker");
    const RankText light = T("Streetlight 02 Scenery", "Env_Streetlight02\nenv_streetlight02", "street light lamp lighting");
    const RankText lot = T("Parking lot 01 Building", "Env_Parking_Lot_01\nenv_parking_lot_01", "pavement");
    const RankText camel = T("Concrete pad Building", "Env_ParkingLot_01\nenv_parkinglot_01", "");
    CHECK(palette_rank("tree", tree) == 0, "rank: tree finds 'Dante city tree 01' at tier 0 (%d)", palette_rank("tree", tree));
    CHECK(palette_rank("tree", road) == -1, "rank: tree rejects 'Long straight' whose aliases hold 'street avenue'");
    CHECK(palette_rank("tree", light) == -1, "rank: tree does not find 'Streetlight' (no word starts with it, 4 letters do not match inside)");
    CHECK(palette_rank("fence", fence) == 0, "rank: fence finds 'Security fence' at tier 0");
    CHECK(palette_rank("fence", turret) == -1, "rank: fence rejects a turret whose aliases hold 'defence'");
    CHECK(palette_rank("mech", mech) == 0, "rank: mech finds the 'Mech row through its kind label at tier 0");
    CHECK(palette_rank("light", light) == 1, "rank: light finds 'Streetlight 02' at tier 1 (%d)", palette_rank("light", light));
    CHECK(palette_rank("parkinglot", lot) == 4, "rank: parkinglot finds Env_Parking_Lot_01 only glued, tier 4 (%d)", palette_rank("parkinglot", lot));
    CHECK(palette_rank("parkinglot", camel) == 2, "rank: parkinglot finds Env_ParkingLot_01 as an official word, tier 2 (%d)", palette_rank("parkinglot", camel));
    CHECK(palette_rank("tree 01", tree) == 0, "rank: two words that both match (%d)", palette_rank("tree 01", tree));
    CHECK(palette_rank("tree zebra", tree) == -1, "rank: two words must both match");
    CHECK(palette_rank("wood", tree) == 3, "rank: an alias word is tier 3 (%d)", palette_rank("wood", tree));
    CHECK(palette_rank("street", road) == 3, "rank: street finds the road piece through its aliases at tier 3 (%d)", palette_rank("street", road));
    CHECK(palette_rank("wood tree", tree) == 3, "rank: the tier of a query is its worst word (%d)", palette_rank("wood tree", tree));
    CHECK(palette_rank("", tree) == 0, "rank: an empty query lists everything");
    CHECK(palette_rank("env_dan", tree) == 2, "rank: official name words, split on _ (%d)", palette_rank("env_dan", tree));
    CHECK(palette_rank("citytree", tree) == 2, "rank: the unsplit official word (%d)", palette_rank("citytree", tree));
    CHECK(palette_rank("tree", tree) < palette_rank("tree", T("Rock Scenery", "Env_Rock", "tree stump")), "rank: a name word beats an alias word");
}

static void test_tool_hints() {
    ed::ToolHintCtx c;
    c.thing = "Test Thing";
    c.brush_m = 40;
    for (int i = 0; i < int(ed::Tool::Count); i++) {
        for (int sel = 0; sel < 2; sel++) {
            c.selected = sel;
            std::string line = ed::tool_hint_line(ed::Tool(i), c);
            CHECK(!line.empty(), "tool %d has no status line", i);
            bool ascii = true;
            for (unsigned char ch : line) ascii &= ch >= 32 && ch < 127;
            CHECK(ascii, "tool %d status line is not plain ASCII: %s", i, line.c_str());
            CHECK(line.find(": ") != std::string::npos, "tool %d status line has no 'Name: click' form: %s", i, line.c_str());
            if (i != int(ed::Tool::Select)) CHECK(line.find("right-click") != std::string::npos, "tool %d status line names no way to stop: %s", i, line.c_str());
        }
    }
    c.selected = 1;
    c.no_place = true;
    c.what = "This trigger";
    std::string np = ed::tool_hint_line(ed::Tool::Select, c);
    CHECK(np.find("This trigger: no place on the map") == 0, "position-less selection line: %s", np.c_str());
    // The placing line names the grid and the angle step while they are on.
    c.grid_m = 8; c.angle_step = 15; c.heading_deg = 90;
    std::string pl = ed::tool_hint_line(ed::Tool::PlaceObject, c);
    CHECK(pl.find("Grid 8 m") != std::string::npos && pl.find("steps of 15") != std::string::npos, "placing line shows the grid and the angle step: %s", pl.c_str());
    c.grid_m = 0; c.angle_step = 0;
    pl = ed::tool_hint_line(ed::Tool::PlaceObject, c);
    CHECK(pl.find("Grid") == std::string::npos && pl.find("steps of") == std::string::npos, "placing line is plain with the grid and the steps off: %s", pl.c_str());
    // Playtest 10-26: the sculpt tools say whether things on the ground move with it.
    c.objects_follow = false;
    std::string off = ed::tool_hint_line(ed::Tool::Lower, c);
    c.objects_follow = true;
    std::string on = ed::tool_hint_line(ed::Tool::Lower, c);
    CHECK(off.find("objects follow the ground: OFF") != std::string::npos && on.find("objects follow the ground: ON") != std::string::npos,
          "sculpt line shows the 'Objects follow the terrain' state: %s / %s", on.c_str(), off.c_str());
}


// The [Snap] section of the settings file: write, read, equal; a file from an older version gives the defaults; bad values give the defaults.
static void test_snap_prefs() {
    using namespace ed;
    SnapPrefs d;
    CHECK(d.joints && !d.grid && d.grid_m == 5.f && d.angle && d.angle_deg == 15.f && d.reach_px == 16.f, "snap defaults: joints on, grid off at 5 m, angle on at 15, reach 16");
    SnapPrefs p;
    p.joints = false; p.grid = true; p.grid_m = 8; p.angle = false; p.angle_deg = 45; p.reach_px = 24;
    IniFile ini;
    ini.parse("[General]\r\nUIScale=1.00\r\n" + snap_prefs_text(p) + "[Testing]\r\nQuickTest=1\r\n", false);
    SnapPrefs q = snap_prefs_from_ini(ini);
    CHECK(q.joints == p.joints && q.grid == p.grid && q.grid_m == p.grid_m && q.angle == p.angle && q.angle_deg == p.angle_deg && q.reach_px == p.reach_px, "snap prefs round trip");
    SnapPrefs d2 = d;
    IniFile old;
    old.parse("[General]\r\nUIScale=1.00\r\n", false);
    q = snap_prefs_from_ini(old);
    CHECK(q.joints == d2.joints && q.grid == d2.grid && q.grid_m == d2.grid_m && q.angle == d2.angle && q.angle_deg == d2.angle_deg && q.reach_px == d2.reach_px, "a file without [Snap] gives the defaults");
    IniFile bad;
    bad.parse("[Snap]\r\nGridSize=7\r\nAngleStep=17\r\nReachPx=500\r\nGrid=1\r\n", false);
    q = snap_prefs_from_ini(bad);
    CHECK(q.grid_m == 5.f && q.angle_deg == 15.f && q.reach_px == 32.f && q.grid, "bad grid size / angle step give the defaults, the reach clamps to 8..32");
    IniFile bad2;
    bad2.parse("[Snap]\r\nGridSize=abc\r\nAngleStep=\r\nReachPx=2\r\n", false);
    q = snap_prefs_from_ini(bad2);
    CHECK(q.grid_m == 5.f && q.angle_deg == 15.f && q.reach_px == 8.f, "non-numbers give the defaults, a small reach clamps to 8");
    CHECK(snap_grid_step(5, 1) == 8 && snap_grid_step(8, 1) == 10 && snap_grid_step(20, 1) == 20 && snap_grid_step(1, -1) == 1 && snap_grid_step(5, -1) == 2, "grid size steps through 1 2 5 8 10 20 and stops at the ends");
}

// The one key table. Names and labels are unique, every Act has its row, the defaults do not clash, and the resolver
// sends each key to exactly one action in the context it is pressed in.
static void test_input_map() {
    using namespace ed;
    ed::KeyBind keys[A_COUNT][2];
    auto reset = [&]() {
        for (int a = 0; a < A_COUNT; a++) { keys[a][0] = kActTable[a].def[0]; keys[a][1] = kActTable[a].def[1]; }
    };
    reset();
    auto KB = [](int k, int m) { return ed::KeyBind{k, m}; };
    const Hold hold;  // Shift free, Alt through, Ctrl quarter, Ctrl reverse, Alt orbit
    const int G = C_GLOBAL | C_MAP | C_EDIT;
    std::set<std::string> inis, labels;
    for (int a = 0; a < A_COUNT; a++) {
        CHECK(kActTable[a].id == a, "act %d has its own row (row says %d)", a, int(kActTable[a].id));
        CHECK(inis.insert(kActTable[a].ini).second, "ini name %s is used twice", kActTable[a].ini);
        CHECK(labels.insert(kActTable[a].label).second, "label '%s' is used twice", kActTable[a].label);
        for (int k = 0; k < 2; k++) {
            const ed::KeyBind& kb = keys[a][k];
            if (!kb.key) continue;
            CHECK(clashes(keys, a, kb, k).empty(), "default key %s of '%s' clashes", key_label(kb).c_str(), kActTable[a].label);
            CHECK(parse_key(key_text(kb)) == kb, "key_text / parse_key round trip of '%s' (%s)", kActTable[a].label, key_text(kb).c_str());
            bool sec = false;
            for (const char* s : kSections) sec |= strcmp(s, kActTable[a].section) == 0;
            CHECK(sec, "section '%s' of '%s' is not in kSections", kActTable[a].section, kActTable[a].label);
        }
    }
    // One name per command; no two sections alike; every action sits in a section; the replaced names are gone.
    {
        std::set<std::string> secs;
        for (int s = 0; s < kSectionCount; s++) CHECK(secs.insert(kSections[s]).second, "section '%s' is listed twice", kSections[s]);
        for (int a = 0; a < A_COUNT; a++) {
            bool sec = false;
            for (const char* s : kSections) sec |= strcmp(s, kActTable[a].section) == 0;
            CHECK(sec, "section '%s' of '%s' is not in kSections", kActTable[a].section, kActTable[a].label);
            const std::string l = kActTable[a].label;
            CHECK(l.find("(layer order)") == std::string::npos && l.find("Snap to ground") == std::string::npos && l.find("Frame") == std::string::npos &&
                      l.find("(copy") == std::string::npos && l.find("under the mouse") == std::string::npos,
                  "label '%s' still carries an old name", kActTable[a].label);
        }
        CHECK(std::string(kActTable[A_ZOOM_SEL].label) == "Zoom to the selection" && std::string(kActTable[A_SNAP].label) == "Put on the ground" &&
                  std::string(kActTable[A_PREFS].label) == "Settings" && std::string(kActTable[A_NEW].label) == "New map" &&
                  std::string(kActTable[A_NEW_MISSION].label) == "New mission" && std::string(kActTable[A_BRING_FORWARD].label) == "Bring forward" &&
                  std::string(kActTable[A_SEND_BACK].label) == "Send back" && std::string(kActTable[A_PASTE].label) == "Paste" &&
                  std::string(kActTable[A_CUT].label) == "Cut" && std::string(kActTable[A_PLACE_AGAIN].label) == "Place again" &&
                  std::string(kActTable[A_DUPLICATE].label) == "Duplicate",
              "the fixed names of the key table");
        CHECK(!kActTable[A_NEW_MISSION].def[0].key && !kActTable[A_CHECK].def[0].key, "the unbound rows have no default key");
        CHECK(heading_display(-90.f) == 270.f && heading_display(360.f) == 0.f && heading_display(0.f) == 0.f && heading_display(630.f) == 270.f &&
                  heading_display(-0.001f) == 0.f && heading_display(359.f) == 359.f,
              "heading_display -90 -> 270, 360 -> 0, 630 -> 270, -0.001 -> 0");
        CHECK(n_things(1, "object") == "1 other object moves" && n_things(2, "object") == "2 other objects move" && n_things(5, "object") == "5 other objects move",
              "n_things(1) = '1 other object moves', (2) = '2 other objects move'");
        CHECK(fix_size_token("Building base 34x34 generic 03 concrete square", 38.f, 38.f) == "Building base 38x38 generic 03 concrete square" &&
                  fix_size_token("Building base 34x34 generic", 34.4f, 33.8f) == "Building base 34x34 generic" &&
                  fix_size_token("Building base 34x40 generic", 40.f, 34.f) == "Building base 34x40 generic" &&
                  fix_size_token("Dante office 07", 20.f, 20.f) == "Dante office 07" && fix_size_token("Bb_34x34_x", 38.f, 38.f) == "Bb_34x34_x",
              "a NxN size in a name follows the measured model when it differs by more than 1 m");
    }
    // Resolver: one action per key.
    const int K_RB = ik::K("RightBracket"), K_LB = ik::K("LeftBracket");
    CHECK(resolve(keys, K_RB, 0, G | C_BRUSH, hold) == A_BRUSH_BIGGER, "] in a brush tool -> Bigger brush");
    CHECK(resolve(keys, K_LB, 0, G | C_BRUSH, hold) == A_BRUSH_SMALLER, "[ in a brush tool -> Smaller brush");
    CHECK(resolve(keys, K_RB, 0, G | C_SELECTION, hold) == A_GRID_BIGGER, "] with a selection is Bigger grid");
    CHECK(resolve(keys, ik::E, KM_SHIFT, G | C_PLACING, hold) == A_TURN_RIGHT, "Shift+E while placing freely still turns");
    CHECK(resolve(keys, ik::E, 0, G | C_PLACING, hold) == A_TURN_RIGHT, "E turns the ghost");
    CHECK(resolve(keys, ik::E, KM_CTRL, G | C_PLACING, hold) == A_TURN_QUARTER_R, "Ctrl+E is the quarter turn");
    CHECK(resolve(keys, ik::Q, KM_CTRL, G | C_SELECTION, hold) == A_TURN_QUARTER_L, "Ctrl+Q is the quarter turn left");
    CHECK(resolve(keys, K_LB, KM_CTRL, G | C_BRUSH, hold) == A_BRUSH_SMALLER, "Ctrl+[ with the reverse key held still sizes the brush");
    CHECK(resolve(keys, K_RB, KM_SHIFT, G | C_BRUSH, hold) == A_BRUSH_STRONGER && resolve(keys, K_LB, KM_SHIFT, G | C_BRUSH, hold) == A_BRUSH_WEAKER, "Shift+] / Shift+[ in a brush tool -> Stronger / Weaker brush");
    CHECK(resolve(keys, K_RB, KM_CTRL, G | C_BRUSH, hold) == A_BRUSH_BIGGER, "Ctrl+] with the reverse key held still sizes the brush");
    CHECK(resolve(keys, K_LB, KM_CTRL, G | C_SELECTION, hold) == A_SEND_BACK, "Ctrl+[ with a selection is Send back");
    CHECK(resolve(keys, K_RB, KM_CTRL, G | C_SELECTION, hold) == A_BRING_FORWARD, "Ctrl+] with a selection is Bring forward");
    CHECK(resolve(keys, ik::Up, 0, G, hold) == A_PAN_FWD && kActTable[A_PAN_FWD].kind == Kind::Held, "Up arrow pans (held)");
    CHECK(resolve(keys, ik::Up, KM_SHIFT, G, hold) == A_PAN_FWD, "Shift+Up pans fast");
    CHECK(resolve(keys, ik::Up, 0, G | C_SELECTION, hold) == A_NUDGE_FWD, "Up arrow nudges with a selection");
    CHECK(resolve(keys, ik::Up, KM_SHIFT, G | C_SELECTION, hold) == A_NUDGE_FWD, "Shift+Up is the big nudge");
    CHECK(resolve(keys, ik::PgUp, 0, G | C_PLACING, hold) == A_RAISE, "PgUp lifts the ghost while placing");
    CHECK(resolve(keys, ik::Left, KM_ALT, G, hold) == A_VIEW_LEFT && resolve(keys, ik::Up, KM_ALT, G | C_SELECTION, hold) == A_VIEW_UP, "Alt+arrows turn and tilt the view");
    CHECK(resolve(keys, ik::Z, KM_CTRL, G, hold) == A_UNDO && resolve(keys, ik::Z, KM_CTRL | KM_SHIFT, G, hold) == A_REDO, "Ctrl+Z / Ctrl+Shift+Z");
    CHECK(resolve(keys, ik::S, KM_CTRL, G, hold) == A_SAVE && resolve(keys, ik::S, 0, G, hold) == A_PAN_BACK, "Ctrl+S saves, S pans");
    CHECK(resolve(keys, ik::S, KM_CTRL, C_GLOBAL, hold) == A_SAVE && resolve(keys, ik::Z, KM_CTRL, C_GLOBAL, hold) == A_NONE, "with no map only the Global keys work");
    CHECK(resolve(keys, ik::Slash, KM_SHIFT, C_GLOBAL, hold) == A_SHORTCUTS, "? opens the shortcuts");
    CHECK(resolve(keys, ik::Tab, KM_CTRL, C_GLOBAL, hold) == A_TAB_NEXT && resolve(keys, ik::Tab, KM_CTRL | KM_SHIFT, C_GLOBAL, hold) == A_TAB_PREV, "Ctrl+Tab");
    CHECK(resolve(keys, ik::N3, KM_CTRL, C_GLOBAL, hold) == A_TAB_3 && resolve(keys, ik::N3, 0, G | C_SELECTION, hold) == A_GIZMO_TILT, "Ctrl+3 is a tab, 3 is Tilt");
    CHECK(resolve(keys, ik::F, KM_CTRL, G, hold) == A_FIND && resolve(keys, ik::F, 0, G, hold) == A_ZOOM_SEL, "Ctrl+F finds, F frames");
    CHECK(resolve(keys, ik::Equal, KM_SHIFT, G, hold) == A_ZOOM_IN, "+ (Shift+=) zooms in");
    CHECK(resolve(keys, ik::Escape, 0, G | C_GLUE, hold) == A_CANCEL && resolve(keys, ik::Escape, 0, G | C_PREVIEW, hold) == A_CANCEL, "Esc is Cancel (Glue and Preview take it in the dispatcher)");
    // A rebound key moves the action and frees the old key.
    keys[A_BRUSH_BIGGER][0] = ed::parse_key("Period");
    CHECK(resolve(keys, K_RB, 0, G | C_BRUSH, hold) == A_NONE, "] rebound to . -> ] does nothing");
    CHECK(resolve(keys, ik::K("Period"), 0, G | C_BRUSH, hold) == A_BRUSH_BIGGER, ". is Bigger brush now");
    reset();
    // A key bound twice at the same specificity is a clash; a more specific context is only an override.
    keys[A_TOP_DOWN][0] = keys[A_HELPERS][0];
    CHECK(!clashes(keys, A_TOP_DOWN, keys[A_TOP_DOWN][0], 0).empty(), "T and H on one key clash");
    reset();
    // Up is Pan forward (Map) and Nudge forward (Selection): Selection beats Map, a note and not a clash.
    CHECK(clashes(keys, A_NUDGE_FWD, keys[A_NUDGE_FWD][0], 0).empty() && shadowed_by(keys, A_PAN_FWD, KB(ik::Up, 0)) == A_NUDGE_FWD, "a more specific context overrides without a clash");
    reset();
    // Key names.
    CHECK(key_label(KB(ik::K("LeftBracket"), 0)) == "[" && key_label(KB(ik::K("Delete"), 0)) == "Del" && key_label(KB(ik::K("PageUp"), 0)) == "PgUp", "friendly names ([, Del, PgUp)");
    CHECK(key_label(KB(ik::K("Escape"), 0)) == "Esc" && key_label(KB(ik::Up, 0)) == "Up arrow" && key_label(KB(ik::KpAdd, 0)) == "Num +" && key_label(KB(ik::Comma, KM_CTRL)) == "Ctrl+,", "friendly names (Esc, Up arrow, Num +, Ctrl+,)");
    CHECK(key_label(KB(ik::Slash, KM_SHIFT)) == "?" && key_label(KB(ik::S, KM_CTRL | KM_SHIFT)) == "Ctrl+Shift+S", "? and Ctrl+Shift+S");
    CHECK(key_text(KB(ik::K("LeftBracket"), 0)) == "LeftBracket" && key_text(KB(ik::K("Delete"), KM_CTRL)) == "Ctrl+Delete", "the ini form keeps ImGui's names");
    CHECK(parse_key("ctrl+shift+s") == KB(ik::S, KM_CTRL | KM_SHIFT) && parse_key("nonsense").key == 0 && parse_key("LeftCtrl").key == 0, "parse_key (case, unknown, modifier keys)");
    // The grid keys. [ / ] are the grid size while placing or with a selection, the brush size in a brush tool (the three
    // tool contexts never overlap); Ctrl+G is the grid on / off in any map; the defaults do not clash (checked for every row above).
    CHECK(resolve(keys, K_RB, 0, G | C_PLACING, hold) == A_GRID_BIGGER && resolve(keys, K_LB, 0, G | C_PLACING, hold) == A_GRID_SMALLER, "] / [ while placing -> Bigger / Smaller grid");
    CHECK(resolve(keys, K_RB, 0, G | C_BRUSH, hold) == A_BRUSH_BIGGER, "] in Brush is still the brush");
    CHECK(resolve(keys, K_RB, KM_CTRL, G | C_SELECTION, hold) == A_BRING_FORWARD, "Ctrl+] in Selection is still Bring forward");
    CHECK(resolve(keys, K_RB, KM_SHIFT, G | C_PLACING, hold) == A_GRID_BIGGER, "Shift+] while placing (place freely held) is Bigger grid");
    CHECK(resolve(keys, K_LB, KM_SHIFT, G | C_SELECTION, hold) == A_GRID_SMALLER, "Shift+[ with a selection is Smaller grid");
    CHECK(resolve(keys, ik::G, KM_CTRL, G, hold) == A_GRID_TOGGLE && resolve(keys, ik::G, KM_CTRL, G | C_PLACING, hold) == A_GRID_TOGGLE, "Ctrl+G is Grid on / off");
    CHECK(resolve(keys, ik::G, 0, G | C_SELECTION, hold) == A_SNAP, "G with a selection is still Put on the ground");
    CHECK(kActTable[A_SNAP_JOINTS].def[0].key == 0 && kActTable[A_ANGLE_SNAP].def[0].key == 0, "joints and angle toggles are unbound");
    // Brush step: 12 %, whole metres, at least 1 m.
    CHECK(brush_step(40, 1) == 45 && brush_step(45, 1) == 50 && brush_step(50, 1) == 56, "brush_step up 40, 45, 50, 56");
    CHECK(brush_step(56, -1) == 50 && brush_step(50, -1) == 45 && brush_step(45, -1) == 40, "brush_step back down to 40");
    CHECK(brush_step(39, 1) == 40 && brush_step(41, -1) == 40 && brush_step(44, 1) == 45 && brush_step(44, -1) == 40, "brush_step: any size joins the one ladder");
    CHECK(brush_step(4, 1) == 5 && brush_step(4, -1) == 3 && brush_step(300, -1) == 275, "brush_step moves at least 1 m");
}

// --mtb-sweep OUT.tsv (2026-11-09, pink placeholder): for every palette template, the templates
// its closure reaches only through .mtb references (TemplateCatalog::mtb_refs) and, per Base level,
// whether their files (model, .mtb) are mounted there (Base archive or common.mgf). Before the fix
// the editor shipped none of them, so an object listed here drew as the pink pole-and-cube
// placeholder on every Base marked in the 'fails_on' column.
static int run_mtb_sweep(GameData& gd, const fs::path& out_path) {
    TemplateCatalog cat;
    std::string err;
    if (!cat.load(gd, &err)) { printf("template catalog: %s\n", err.c_str()); return 2; }
    std::vector<std::pair<std::string, int>> bases;  // archive name, level_archives index
    {
        std::set<std::string> seen;
        for (auto& m : gd.maps()) {
            if (!m.resource_present || !seen.insert(m.resource).second) continue;
            int idx = -1;
            for (size_t a = 0; a < gd.level_archives().size(); a++) if (gd.level_archives()[a] == m.resource) idx = int(a);
            if (idx >= 0) bases.push_back({m.resource, idx});
        }
    }
    auto files_of = [&](uint32_t u) {
        std::vector<std::string> f;
        if (const TemplateInfo* t = cat.find(u)) {
            if (!t->model.empty()) f.push_back(t->model);
            if (!t->mtb.empty()) f.push_back(t->mtb);
        }
        return f;
    };
    auto in_common = [&](const std::string& p) { return gd.common().find(p) >= 0; };
    auto on_disc = [&](const std::string& p) { return in_common(p) || cat.archive_of(p) >= 0; };
    FILE* out = _wfopen(out_path.c_str(), L"w");
    if (!out) { printf("cannot write %s\n", path_utf8(out_path).c_str()); return 2; }
    fprintf(out, "uid\tname\tclass\tcategory\tmtb_only_templates\tunshipped_files_before_fix\tfails_on_bases_before_fix\tfails_on\n");
    size_t placeable = 0, with_refs = 0, affected = 0, unfixable = 0;
    std::map<std::string, size_t> per_base;
    std::set<uint32_t> culprits;
    for (auto& t : cat.all()) {
        if (!t.placeable) continue;
        placeable++;
        auto before = cat.closure({t.uid}, false);
        auto after = cat.closure({t.uid}, true);
        std::vector<uint32_t> added;
        for (uint32_t u : after) if (!before.count(u) && !files_of(u).empty()) added.push_back(u);
        std::set<std::string> code;  // files the game's code needs for a class in the closure (never shipped before)
        for (uint32_t u : after) for (auto& f : cat.code_required(u)) code.insert(f);
        if (added.empty() && code.empty()) continue;
        if (!added.empty()) with_refs++;
        std::sort(added.begin(), added.end());
        std::set<std::string> missing;  // files on a disc archive that the old export never shipped
        bool off_disc = false;
        for (uint32_t u : added)
            for (auto& f : files_of(u)) {
                if (!on_disc(f)) { off_disc = true; continue; }
                if (!in_common(f)) missing.insert(norm_path(f));
            }
        for (auto& f : code) {
            if (!on_disc(f)) { off_disc = true; continue; }
            if (!in_common(f)) missing.insert(norm_path(f));
        }
        std::vector<std::string> fails;
        for (auto& [name, idx] : bases) {
            bool bad = false;
            for (auto& f : missing) if (!cat.archive_has(idx, f)) { bad = true; break; }
            if (bad) { fails.push_back(name); per_base[name]++; }
        }
        if (off_disc) unfixable++;
        if (fails.empty()) continue;
        affected++;
        std::string tl, ml, fl;
        for (uint32_t u : added) { culprits.insert(u); const TemplateInfo* x = cat.find(u); tl += (tl.empty() ? "" : "; ") + std::to_string(u) + " " + (x ? x->name : std::string()); }
        for (auto& f : missing) ml += (ml.empty() ? "" : "; ") + f;
        for (auto& f : fails) fl += (fl.empty() ? "" : " ") + f;
        fprintf(out, "%u\t%s\t%s\t%s\t%s\t%s\t%zu\t%s\n", t.uid, t.name.c_str(), t.cls_name.c_str(), t.category.c_str(), tl.c_str(),
                ml.c_str(), fails.size(), fl.c_str());
    }
    fclose(out);
    // After the fix every one of those files ships from the archive that holds it, so what can still fail is a
    // needed file that is on no disc archive.
    printf("[mtb-sweep] %zu palette templates; %zu reach templates only through .mtb references; %zu drew as the placeholder on at least one of %zu Base levels before the fix (.mtb references + code-required building files); %zu still affected after the fix (need a file on no disc archive)\n",
           placeable, with_refs, affected, bases.size(), unfixable);
    printf("[mtb-sweep] %zu distinct .mtb-referenced templates involved:", culprits.size());
    for (uint32_t u : culprits) { const TemplateInfo* x = cat.find(u); printf(" %u(%s)", u, x ? x->name.c_str() : "?"); }
    printf("\n");
    for (auto& [b, n] : per_base) printf("[mtb-sweep]   %-22s %zu\n", b.c_str(), n);
    return 0;
}

// One ground. Terrain::height_at is the ground the game has (the saved 8-bit steps, each cell split on its
// (gx, gz)-(gx+1, gz+1) diagonal); the fine layer under it is the brushes' private working store.
static void test_ground() {
    using namespace ma2;
    Terrain t;
    t.min_h = -20.f;
    t.max_h = 100.f;
    t.scale = 8.f;
    t.resize_flat(33, 33, 0.f);
    Rng rng(0xC0FFEEull);
    for (int gz = 0; gz < t.h; gz++)
        for (int gx = 0; gx < t.w; gx++) t.ref(gx, gz) = rng.range(-20.f, 100.f);
    auto tri = [&](float x, float z) {  // the triangle formula over the stored steps, written out independently
        float fx = std::clamp(x / t.scale, 0.f, float(t.w - 1)), fz = std::clamp(z / t.scale, 0.f, float(t.h - 1));
        int x0 = std::min(int(fx), t.w - 2), z0 = std::min(int(fz), t.h - 2);
        float tx = fx - float(x0), tz = fz - float(z0);
        auto g = [&](int a, int b) { return t.stored(t.at(a, b)); };
        float a = g(x0, z0), b = g(x0 + 1, z0), c = g(x0, z0 + 1), d = g(x0 + 1, z0 + 1);
        return tx >= tz ? a + (b - a) * tx + (d - b) * tz : a + (c - a) * tz + (d - c) * tx;
    };
    std::vector<std::pair<float, float>> pts;
    for (int i = 0; i < 2000; i++) pts.push_back({rng.range(0.f, t.size_x()), rng.range(0.f, t.size_z())});
    float worst = 0;
    for (auto& p : pts) worst = std::max(worst, std::fabs(t.height_at(p.first, p.second) - tri(p.first, p.second)));
    CHECK(worst < 0.0001f, "T1.1 height_at equals the triangle formula over the stored steps (worst %.6f)", worst);
    int bad = 0;
    for (int gz = 0; gz < t.h; gz++)
        for (int gx = 0; gx < t.w; gx++)
            if (std::fabs(t.height_at(float(gx) * t.scale, float(gz) * t.scale) - t.step_at(gx, gz)) > 1e-4f) bad++;
    CHECK(bad == 0, "T1.2 height_at at every grid point is step_at (%d off)", bad);
    Terrain q = t;
    q.from_samples(q.samples());
    worst = 0;
    for (auto& p : pts) worst = std::max(worst, std::fabs(q.height_at(p.first, p.second) - t.height_at(p.first, p.second)));
    CHECK(worst < 0.0001f, "T1.3 height_at is unchanged by save and reload (worst %.6f)", worst);
    // Sub-step accumulation: the fine layer takes 0.1 m adds; the ground moves when the point crosses half a step.
    {
        Terrain s;
        s.min_h = -20.f;
        s.max_h = 100.f;
        s.resize_flat(4, 4, 0.f);
        s.ref(1, 1) = s.stored(10.f);
        const float base = s.step_at(1, 1);
        std::vector<float> seen;
        for (int i = 0; i < 5; i++) { s.ref(1, 1) += 0.1f; seen.push_back(s.step_at(1, 1)); }
        CHECK(std::fabs(s.step() - 0.4706f) < 0.0001f, "T1.4 step of a -20..100 map is %.4f", s.step());
        CHECK(seen[0] == base && seen[1] == base, "T1.4 two 0.1 m adds leave the ground on its step");
        CHECK(std::fabs(seen[2] - (base + s.step())) < 1e-4f, "T1.4 the third add moves it one step (%.4f vs %.4f)", seen[2], base + s.step());
    }
    {
        Terrain f;
        f.min_h = -20.f;
        f.max_h = 107.5f;
        CHECK(f.step() == 0.5f, "T1.5 a -20..107.5 map has 0.5 m steps (%.5f)", f.step());
        CHECK(f.stored(10.f) == 10.f && f.stored(20.f) == 20.f && f.stored(19.95f) == 20.f, "T1.5 round heights are steps");
    }
}

// Sheets and layers (core/placement.h). A 65 x 65 terrain, range -20..107.5, flat at 10.0; "road" = footprint
// x -13..13, z -60..0, faces 0 / 0; "plate" = 38 x 38 centred, faces 0.016 / 0.016.
static void test_sheets() {
    using namespace ma2;
    using namespace ma2::place;
    Terrain t;
    t.min_h = -20.f;
    t.max_h = 107.5f;
    t.scale = 8.f;
    t.resize_flat(65, 65, 10.f);
    auto road = [&](uint32_t uid, float x, float z, int layer, float yaw = 0.f) {
        Sheet s;
        s.uid = uid; s.pos = {x, 0, z}; s.yaw = yaw; s.fp = {-13, 13, -60, 0}; s.under = s.top = 0; s.layer = layer;
        return s;
    };
    auto plate = [&](uint32_t uid, float x, float z, int layer, float yaw = 0.f) {
        Sheet s;
        s.uid = uid; s.pos = {x, 0, z}; s.yaw = yaw; s.fp = {-19, 19, -19, 19}; s.under = s.top = 0.016f; s.layer = layer;
        return s;
    };
    const float L = kSheetLift, S = kSheetStep;
    // T2.1 lone sheets rest kSheetLift over the ground.
    {
        std::vector<Sheet> v{road(1, 200, 300, 0), plate(2, 100, 100, 0)};
        seat(t, v);
        CHECK(std::fabs(v[0].pos.y - (10.f + L)) < 1e-4f, "T2.1 a lone road seats at ground + kSheetLift (%.4f)", v[0].pos.y);
        CHECK(std::fabs(v[1].pos.y + v[1].under - (10.f + L)) < 1e-4f, "T2.1 a lone plate's underside is at the same height (%.4f)", v[1].pos.y + v[1].under);
    }
    // T2.2 a road over a plate and the other way round.
    {
        std::vector<Sheet> v{plate(1, 200, 200, 0), road(2, 200, 220, 1)};
        seat(t, v);
        CHECK(std::fabs(v[1].pos.y - (v[0].pos.y + v[0].top + S)) < 1e-4f, "T2.2 the later road is kSheetStep above the plate's top (%.4f vs %.4f)", v[1].pos.y, v[0].pos.y + v[0].top + S);
        v[0].layer = 1; v[1].layer = 0;
        seat(t, v);
        CHECK(std::fabs((v[0].pos.y + v[0].under) - (v[1].pos.y + S)) < 1e-4f, "T2.2 layers swapped: the plate's underside is kSheetStep above the road");
    }
    // T2.3 two overlapping plates always differ; equal layers: the higher uid is on top.
    {
        std::vector<Sheet> v{plate(1, 200, 200, 0), plate(2, 215, 205, 1)};
        seat(t, v);
        CHECK(std::fabs((v[1].pos.y + v[1].top) - (v[0].pos.y + v[0].top) - S) < 1e-4f, "T2.3 two plates, layers 0 and 1: tops differ by kSheetStep");
        v[0].layer = v[1].layer = 3;
        seat(t, v);
        CHECK(v[1].pos.y > v[0].pos.y + S - 1e-4f, "T2.3 equal layers: the higher uid is on top, and they differ");
    }
    // T2.4 plates joined edge to edge (0.1 m) share a height; overlapping by 0.5 m they stack.
    {
        std::vector<Sheet> v{plate(1, 200, 200, 0), plate(2, 237.9f, 200, 0)};
        seat(t, v);
        CHECK(std::fabs(v[0].pos.y - v[1].pos.y) < 1e-4f && std::fabs(v[0].pos.y + v[0].under - (10.f + L)) < 1e-4f, "T2.4 an overlap of 0.1 m: both at ground + lift");
        v[1].pos.x = 237.5f;
        seat(t, v);
        CHECK(std::fabs(v[1].pos.y - v[0].pos.y) > S - 1e-4f, "T2.4 an overlap of 0.5 m: stacked");
    }
    // T2.5 a run seats level at its highest member.
    {
        Terrain h = t;
        for (int gz = 0; gz < h.h; gz++)
            for (int gx = 18; gx < 30; gx++) h.ref(gx, gz) = 11.f;
        std::vector<Sheet> v{plate(1, 60, 100, 0), plate(2, 190, 100, 0)};
        v[0].run = v[1].run = 7;
        seat(h, v);
        CHECK(std::fabs((v[0].pos.y + v[0].top) - (v[1].pos.y + v[1].top)) < 1e-4f, "T2.5 a run: both tops equal");
        CHECK(std::fabs(v[0].pos.y + v[0].under - (11.f + L)) < 1e-4f, "T2.5 a run: at the higher member's height (%.4f)", v[0].pos.y + v[0].under);
    }
    // T2.6 fixed sheets do not move and still hold up higher layers; a fixed higher layer is ignored by a lower one.
    {
        std::vector<Sheet> v{plate(1, 200, 200, 0), road(2, 200, 220, 1)};
        v[0].pos.y = 12.f;
        v[0].fixed = true;
        seat(t, v);
        CHECK(v[0].pos.y == 12.f, "T2.6 a fixed plate does not move");
        CHECK(std::fabs(v[1].pos.y - (12.f + 0.016f + S)) < 1e-4f, "T2.6 a road over a fixed plate: plate top + kSheetStep (%.4f)", v[1].pos.y);
        v[0].layer = 5;
        seat(t, v);
        CHECK(std::fabs(v[1].pos.y - (10.f + L)) < 1e-4f, "T2.6 a fixed plate with a higher layer is ignored by the road (%.4f)", v[1].pos.y);
    }
    // T2.7 three stacked; the bottom one goes to the top.
    {
        std::vector<Sheet> v{plate(1, 200, 200, 0), plate(2, 205, 205, 1), plate(3, 210, 200, 2)};
        seat(t, v);
        v[0].layer = top_layer_under(v, v[0]) + 1;
        seat(t, v);
        const float a = v[0].pos.y, b = v[1].pos.y, c = v[2].pos.y;
        CHECK(b < c && c < a && std::fabs(c - b - S) < 1e-4f && std::fabs(a - c - S) < 1e-4f, "T2.7 order is now B, C, A, each kSheetStep apart (%.4f %.4f %.4f)", b, c, a);
    }
    // T2.8 on a ramp the road clears the ground everywhere and touches kSheetLift at the highest point.
    {
        Terrain r = t;
        for (int gz = 0; gz < r.h; gz++)
            for (int gx = 0; gx < r.w; gx++) r.ref(gx, gz) = 10.f + 0.5f * float(gx);
        std::vector<Sheet> v{road(1, 200, 300, 0, 0.7f)};
        seat(r, v);
        Rng rr(5);
        float lowest = 1e30f;
        int n = 0;
        for (int i = 0; i < 500; i++) {
            const float lx = rr.range(-13.f, 13.f), lz = rr.range(-60.f, 0.f);
            const float c = std::cos(0.7f), sn = std::sin(0.7f);
            const float x = 200 + c * lx + sn * lz, z = 300 - sn * lx + c * lz;
            if (!r.inside(x, z)) continue;
            lowest = std::min(lowest, v[0].pos.y - r.height_at(x, z));
            n++;
        }
        CHECK(n > 400 && lowest >= L - 0.0001f, "T2.8 on a ramp the road is at least kSheetLift over the ground at %d points (least %.4f)", n, lowest);
        CHECK(std::fabs(v[0].pos.y - ground_max(r, v[0]) - L) < 1e-4f, "T2.8 and exactly kSheetLift over its highest ground point");
    }
    // T2.9 overlap.
    {
        Sheet a = plate(1, 0, 0, 0);
        CHECK(overlap(a, plate(2, 30, 0, 0)), "T2.9 axis aligned, 8 m in: overlap");
        CHECK(!overlap(a, plate(2, 38, 0, 0), 0.f), "T2.9 touching edges do not overlap");
        CHECK(overlap(a, plate(2, 44.87f, 0, 0, 0.7853982f)), "T2.9 45 degrees, corner 1 m in: overlap");
        CHECK(!overlap(a, plate(2, 48.f, 0, 0, 0.7853982f)), "T2.9 45 degrees, corner 2 m short: apart");
        CHECK(!overlap(a, plate(2, 37.8f, 0, 0)), "T2.9 an overlap of 0.2 m is inside the margin");
        CHECK(overlap(a, plate(2, 37.7f, 0, 0)), "T2.9 an overlap of 0.3 m counts");
    }
    // T2.11 seating twice changes nothing the second time.
    {
        std::vector<Sheet> v{plate(1, 200, 200, 0), road(2, 200, 220, 1), plate(3, 215, 205, 2), road(4, 300, 300, 0, 0.4f)};
        seat(t, v);
        const std::vector<float> m = seat(t, v);
        float worst = 0;
        for (float d : m) worst = std::max(worst, std::fabs(d));
        CHECK(worst == 0.f, "T2.11 seat twice: the second pass moves nothing");
    }
    // T2.12 a tilted sheet's faces against the orient_instance formula (gizmo.cpp), copied here.
    {
        Sheet s = plate(1, 120, 130, 0, 0.7f);
        s.pos.y = 20.f;
        s.pitch = 0.2f;
        s.roll = -0.1f;
        const float ch = std::cos(s.yaw), sh = std::sin(s.yaw), cp = std::cos(s.pitch), sp = std::sin(s.pitch), cr = std::cos(s.roll), sr = std::sin(s.roll);
        float worst = 0;
        for (int k = 0; k < 4; k++) {
            const float lx = (k & 1) ? s.fp.x1 : s.fp.x0, lz = (k & 2) ? s.fp.z1 : s.fp.z0, ly = s.top;
            const float wx = (ch * cr + sh * sp * sr) * lx + (-ch * sr + sh * sp * cr) * ly + (sh * cp) * lz + s.pos.x;
            const float wy = (cp * sr) * lx + (cp * cr) * ly + (-sp) * lz + s.pos.y;
            const float wz = (-sh * cr + ch * sp * sr) * lx + (sh * sr + ch * sp * cr) * ly + (ch * cp) * lz + s.pos.z;
            worst = std::max(worst, std::fabs(face_y(s, s.top, wx, wz) - wy));
        }
        CHECK(worst < 0.001f, "T2.12 face_y at the four corners equals the orient_instance corners (worst %.5f)", worst);
    }
    // T2.13 contains for a footprint turned 30 degrees.
    {
        Sheet s = road(1, 100, 100, 0, 0.5236f);
        auto at = [&](float lx, float lz) { return std::make_pair(s.pos.x + std::cos(s.yaw) * lx + std::sin(s.yaw) * lz, s.pos.z - std::sin(s.yaw) * lx + std::cos(s.yaw) * lz); };
        bool ok = contains(s, at(0, -30).first, at(0, -30).second);
        for (int k = 0; k < 4; k++) {
            const float cx = (k & 1) ? s.fp.x1 : s.fp.x0, cz = (k & 2) ? s.fp.z1 : s.fp.z0;
            const float ix = cx + ((k & 1) ? -0.1f : 0.1f), iz = cz + ((k & 2) ? -0.1f : 0.1f);
            const float ox = cx + ((k & 1) ? 0.1f : -0.1f), oz = cz + ((k & 2) ? 0.1f : -0.1f);
            ok = ok && contains(s, at(ix, iz).first, at(ix, iz).second) && !contains(s, at(ox, oz).first, at(ox, oz).second);
        }
        CHECK(ok, "T2.13 contains: centre, corners pulled in 0.1 m (true), pushed out 0.1 m (false)");
    }
    // T2.14 a road over a tilted fixed plate rests kSheetStep over the plate's highest top point under the road.
    {
        Sheet p = plate(1, 200, 200, 0, 0.3f);
        p.pos.y = 14.f;
        p.pitch = 0.1f;
        p.fixed = true;
        Sheet r = road(2, 205, 215, 1);
        std::vector<Sheet> v{p, r};
        seat(t, v);
        float best = -1e30f;  // brute force over the road's footprint
        for (float lx = -13.f; lx <= 13.f; lx += 0.1f)
            for (float lz = -60.f; lz <= 0.f; lz += 0.1f) {
                const float x = r.pos.x + lx, z = r.pos.z + lz;
                if (contains(p, x, z)) best = std::max(best, face_y(p, p.top, x, z));
            }
        CHECK(best > -1e29f && v[1].pos.y >= best + S - 1e-4f && v[1].pos.y < best + S + 0.01f, "T2.14 over a tilted plate: %.4f vs highest top %.4f + step", v[1].pos.y, best);
    }
    // The layer property text.
    {
        int l = 0;
        bool f = false;
        CHECK(parse_layer("3", l, f) && l == 3 && !f && parse_layer("-2 fixed", l, f) && l == -2 && f && !parse_layer("fixed", l, f), "T2 layer text parses");
        CHECK(format_layer(4, true) == "4 fixed" && format_layer(0, false) == "0", "T2 layer text formats");
    }
}

// What stands on what (surface_at), tilt_for_up, and where a plate goes when a road is already there (drop_layer).
static void test_grid_under() {
    using namespace ma2::place;
    Sheet s;
    s.pos = {64.f, 0, 64.f};
    s.fp = {-19, 19, -19, 19};
    auto a = grid_under(s, 8.f, 0), b = grid_under(s, 8.f, 1);
    CHECK(a.size() == 25, "T4.1 plate 38 x 38 on 8 m cells covers 5 x 5 points, got %zu", a.size());
    CHECK(b.size() == 49, "T4.1 grown by one cell it covers 7 x 7 points, got %zu", b.size());
    s.yaw = 0.7853982f;
    s.pos = {70.f, 0, 61.f};
    const int grow = 1;
    auto c = grid_under(s, 8.f, grow);
    Sheet g = s;
    g.fp = {-19.f - 8.f, 19.f + 8.f, -19.f - 8.f, 19.f + 8.f};
    int missing = 0, outside = 0;
    for (auto& q : c) if (!contains(g, float(q.first) * 8.f, float(q.second) * 8.f)) outside++;
    for (int gz = -2; gz < 30; gz++)
        for (int gx = -2; gx < 30; gx++) {
            if (!contains(g, float(gx) * 8.f, float(gz) * 8.f)) continue;
            bool found = false;
            for (auto& q : c) found |= q.first == gx && q.second == gz;
            if (!found) missing++;
        }
    CHECK(outside == 0 && missing == 0 && !c.empty(), "T4.2 turned 45 deg: %d outside, %d missing, %zu points", outside, missing, c.size());
}

// Slopes. A 65 x 65 terrain, 8 m cells, range -20..107.5 (0.5 m steps), a planar ramp rising 0.5 m per cell along +z.
static void test_slopes() {
    using namespace ma2;
    using namespace ma2::place;
    Terrain t;
    t.min_h = -20.f;
    t.max_h = 107.5f;
    t.scale = 8.f;
    t.resize_flat(65, 65, 10.f);
    for (int gz = 0; gz < t.h; gz++)
        for (int gx = 0; gx < t.w; gx++) t.ref(gx, gz) = 10.f + 0.5f * float(gz);
    auto road = [&](float x, float z, float yaw) {
        Sheet s;
        s.uid = 1; s.kind = 1; s.pos = {x, 0, z}; s.yaw = yaw; s.fp = {-13, 13, -60, 0}; s.under = s.top = 0;
        return s;
    };
    const float ramp = std::atan(0.5f / 8.f);
    // T5.1 along the ramp. yaw 3.14159 makes the road run (local -z) toward +z, uphill.
    {
        Sheet s = road(300, 100, 3.14159265f);
        fit_tilt(t, s, false, 0);
        const float lo = min_clearance(t, s);
        float hi = 0;
        std::vector<Sheet> one{s};
        hi = -1e30f;
        // highest clearance over the footprint samples = float_gap + kSheetLift
        hi = float_gap(t, s) + kSheetLift;
        CHECK(lo >= kSheetLift - 1e-3f && hi <= kSheetLift + t.step() + 1e-3f, "T5.1 along the ramp the clearance is %.4f .. %.4f (want %.2f .. +one step)", lo, hi, kSheetLift);
        CHECK(std::fabs(std::fabs(s.pitch) - ramp) < 0.5f * 3.14159265f / 180.f && std::fabs(s.roll) < 0.2f * 3.14159265f / 180.f,
              "T5.1 pitch %.2f deg (ramp %.2f), roll %.3f deg", s.pitch * 57.2958f, ramp * 57.2958f, s.roll * 57.2958f);
    }
    // T5.2 across the ramp: the road runs along x, so the roll takes the slope.
    {
        Sheet s = road(300, 100, 1.57079633f);
        fit_tilt(t, s, false, 0);
        CHECK(std::fabs(std::fabs(s.roll) - ramp) < 0.5f * 3.14159265f / 180.f && std::fabs(s.pitch) < 0.5f * 3.14159265f / 180.f,
              "T5.2 across the ramp roll %.2f deg (ramp %.2f), pitch %.3f deg", s.roll * 57.2958f, ramp * 57.2958f, s.pitch * 57.2958f);
        CHECK(min_clearance(t, s) >= kSheetLift - 1e-3f, "T5.2 it clears the ground (%.4f)", min_clearance(t, s));
    }
    // T5.3 joined: the origin does not move; the smallest clearance is kSheetLift.
    {
        Sheet s = road(300, 100, 3.14159265f);
        s.pos.y = t.height_at(300, 100) + kSheetLift;
        const Vec3 o = s.pos;
        fit_tilt(t, s, true, 0);
        CHECK(std::fabs(s.pos.x - o.x) < 1e-4f && std::fabs(s.pos.y - o.y) < 1e-4f && std::fabs(s.pos.z - o.z) < 1e-4f, "T5.3 the joint stays where it is");
        CHECK(std::fabs(min_clearance(t, s) - kSheetLift) < 0.02f, "T5.3 joined: smallest clearance %.4f", min_clearance(t, s));
        CHECK(std::fabs(std::fabs(s.pitch) - ramp) < 1.f * 3.14159265f / 180.f, "T5.3 joined pitch %.2f deg", s.pitch * 57.2958f);
    }
    // T5.4 a 30 m cliff under the far end: the pitch stops at 25 degrees.
    {
        Terrain c = t;
        for (int gz = 0; gz < c.h; gz++)
            for (int gx = 0; gx < c.w; gx++) c.ref(gx, gz) = gz >= 14 ? 40.f : 10.f;
        Sheet s = road(300, 100, 3.14159265f);
        s.pos.y = c.height_at(300, 100) + kSheetLift;
        fit_tilt(c, s, true, 0);
        CHECK(std::fabs(std::fabs(s.pitch) - kMaxTilt) < 1e-3f, "T5.4 cliff: pitch clamped to %.2f deg", s.pitch * 57.2958f);
        Sheet f = road(300, 100, 3.14159265f);
        fit_tilt(c, f, false, 0);
        CHECK(std::fabs(f.pitch) <= kMaxTilt + 1e-4f && std::fabs(f.roll) <= kMaxTilt + 1e-4f, "T5.4 free piece also stays within 25 degrees");
    }
    // T5.5 float_gap of a level road along the ramp = the ramp's rise over its length, to one step.
    {
        Sheet s = road(300, 100, 3.14159265f);
        const float rise = 0.5f * 60.f / 8.f;
        CHECK(std::fabs(float_gap(t, s) - rise) <= t.step() + 1e-3f, "T5.5 float_gap %.3f, rise %.3f", float_gap(t, s), rise);
        Terrain flat = t;
        flat.resize_flat(65, 65, 10.f);
        CHECK(float_gap(flat, s) < 1e-4f, "T5.5 flat ground: no gap");
    }
    // Hang_gap is what hangs at the height the piece is at. A level piece resting on the highest ground = float_gap; resting on
    // the lowest ground (a base on its levelled plate, higher ground left alone beside it) = nothing hangs; raised above the highest = gap + rise.
    {
        Sheet s = road(300, 100, 3.14159265f);
        const float lo = ground_min(t, s), hi = ground_max(t, s);
        s.pos.y = hi + kSheetLift - s.under;
        CHECK(std::fabs(hang_gap(t, s) - float_gap(t, s)) < 1e-4f && hang_gap(t, s) > 3.f, "hang_gap on the highest ground = float_gap (%.3f / %.3f)", hang_gap(t, s), float_gap(t, s));
        s.pos.y = lo + kSheetLift - s.under;
        CHECK(hang_gap(t, s) < 1e-4f && float_gap(t, s) > 3.f, "resting on the lowest ground: nothing hangs (%.4f) though the ground is uneven (%.3f)", hang_gap(t, s), float_gap(t, s));
        s.pos.y = hi + 6.5f + kSheetLift - s.under;
        CHECK(std::fabs(hang_gap(t, s) - (hi - lo + 6.5f)) < 1e-3f, "6.5 m above the highest ground hangs 6.5 m more (%.3f)", hang_gap(t, s));
        s.pos.y = lo - 3.f - s.under;
        CHECK(hang_gap(t, s) < 1e-4f, "buried: no hang (%.4f)", hang_gap(t, s));
        Sheet tl = road(300, 100, 3.14159265f);
        fit_tilt(t, tl, false, 0);
        CHECK(std::fabs(hang_gap(t, tl) - float_gap(t, tl)) < 1e-5f, "a tilted piece: hang_gap = float_gap");
    }
}

// The basin a water remembers, and flat pieces at one height.
static void test_basins() {
    using namespace ma2;
    using namespace ma2::place;
    // T6.1 encode / decode round trip of a 26 x 16 rectangle of random bytes (and the footprint text after the two runs).
    {
        const int nx = 26, nz = 16;
        std::vector<uint8_t> b(size_t(nx) * nz), a(b.size());
        uint32_t seed = 12345;
        for (auto& v : b) { seed = seed * 1664525u + 1013904223u; v = uint8_t(seed >> 24); }
        for (auto& v : a) { seed = seed * 1664525u + 1013904223u; v = uint8_t(seed >> 24); }
        const std::string enc = basin_encode(7, 11, nx, nz, b, a, "300.500 410.250 80.000 60.000 0.0000 0 9.850");
        int gx = 0, gz = 0, cx = 0, cz = 0;
        std::vector<uint8_t> b2, a2;
        std::string ex;
        const bool ok = basin_decode(enc, gx, gz, cx, cz, b2, a2, &ex);
        CHECK(ok && gx == 7 && gz == 11 && cx == nx && cz == nz && b2 == b && a2 == a, "T6.1 basin_encode / basin_decode round trip (26 x 16)");
        CHECK(ex == "300.500 410.250 80.000 60.000 0.0000 0 9.850", "T6.1 the text after the two runs comes back");
        std::vector<uint8_t> b3, a3;
        CHECK(!basin_decode("1 2 3 4 zz", gx, gz, cx, cz, b3, a3) && !basin_decode("1 2 3 4 00 00", gx, gz, cx, cz, b3, a3) && !basin_decode("", gx, gz, cx, cz, b3, a3),
              "T6.1 a damaged string is refused");
    }
    // T6.3 same_height_pairs: overlapping level sheets with tops within 0.015 m; 0.030 apart, tilted, apart, or in one run do not count.
    {
        auto plate = [](uint32_t uid, float x, float y) {
            Sheet s;
            s.uid = uid; s.pos = {x, y, 0}; s.fp = {-19, 19, -19, 19}; s.under = s.top = 0.016f; s.kind = 2;
            return s;
        };
        std::vector<Sheet> v{plate(1, 0, 10.f), plate(2, 10, 10.010f)};
        CHECK(same_height_pairs(v).size() == 1, "T6.3 two overlapping level sheets 0.010 apart are a pair");
        v[1].pos.y = 10.030f;
        CHECK(same_height_pairs(v).empty(), "T6.3 0.030 apart is not");
        v[1].pos.y = 10.f;
        v[1].pitch = 0.1f;
        CHECK(same_height_pairs(v).empty(), "T6.3 a tilted sheet is not");
        v[1].pitch = 0;
        v[1].pos.x = 200;
        CHECK(same_height_pairs(v).empty(), "T6.3 sheets that do not overlap are not");
        v[1].pos.x = 10;
        v[0].run = v[1].run = 3;
        CHECK(same_height_pairs(v).empty(), "T6.3 plates joined in one run share a height on purpose");
    }
    // T6.4 float_gap is 0 on flat ground and above float_tol on a ramp (the T5 ramp: 0.5 m per 8 m).
    {
        Terrain flat, ramp;
        for (Terrain* t : {&flat, &ramp}) { t->min_h = -20.f; t->max_h = 107.5f; t->scale = 8.f; t->resize_flat(65, 65, 10.f); }
        for (int gz = 0; gz < ramp.h; gz++)
            for (int gx = 0; gx < ramp.w; gx++) ramp.ref(gx, gz) = 10.f + 0.5f * float(gz);
        Sheet s;
        s.uid = 1; s.kind = 1; s.pos = {300, 0, 100}; s.fp = {-13, 13, -60, 0};
        CHECK(float_gap(flat, s) < 1e-4f, "T6.4 float_gap is 0 on flat ground");
        s.yaw = 3.14159265f;
        CHECK(float_gap(ramp, s) > float_tol(ramp), "T6.4 float_gap on the ramp is %.2f, above float_tol %.2f", float_gap(ramp, s), float_tol(ramp));
    }
}

static void test_surface() {
    using namespace ma2;
    using namespace ma2::place;
    Terrain t;
    t.min_h = -20.f;
    t.max_h = 107.5f;
    t.scale = 8.f;
    t.resize_flat(65, 65, 10.f);
    auto road = [&](uint32_t uid, float x, float z, int layer) {
        Sheet s;
        s.uid = uid; s.pos = {x, 0, z}; s.fp = {-13, 13, -60, 0}; s.under = s.top = 0; s.layer = layer; s.kind = 1;
        return s;
    };
    auto plate = [&](uint32_t uid, float x, float z, int layer) {
        Sheet s;
        s.uid = uid; s.pos = {x, 0, z}; s.fp = {-19, 19, -19, 19}; s.under = s.top = 0.016f; s.layer = layer; s.kind = 2;
        return s;
    };
    const float S = kSheetStep;
    // T3.1 surface_at: bare ground, one plate, two stacked, below_y between them, just outside.
    {
        std::vector<Sheet> v{plate(1, 200, 200, 0), plate(2, 205, 200, 1)};
        seat(t, v);
        uint32_t on = 99;
        CHECK(std::fabs(surface_at(t, v, 400, 400, 1e30f, &on) - 10.f) < 1e-4f && on == 0, "T3.1 bare ground is the ground (on %u)", on);
        const float lo = v[0].pos.y + v[0].top, hi = v[1].pos.y + v[1].top;
        CHECK(std::fabs(surface_at(t, v, 183, 200, 1e30f, &on) - lo) < 1e-4f && on == 1, "T3.1 under one plate: its top (%.4f vs %.4f)", surface_at(t, v, 183, 200), lo);
        CHECK(std::fabs(surface_at(t, v, 202, 200, 1e30f, &on) - hi) < 1e-4f && on == 2, "T3.1 under two stacked plates: the higher top (%.4f vs %.4f)", surface_at(t, v, 202, 200), hi);
        CHECK(std::fabs(surface_at(t, v, 202, 200, (lo + hi) * 0.5f, &on) - lo) < 1e-4f && on == 1, "T3.1 below_y between them: the lower top");
        CHECK(std::fabs(surface_at(t, v, 202, 200, 10.0f, &on) - 10.f) < 1e-4f && on == 0, "T3.1 below_y under both: the ground");
        CHECK(std::fabs(surface_at(t, v, 200 - 19.3f, 200, 1e30f, &on) - 10.f) < 1e-4f && on == 0, "T3.1 0.3 m outside the footprint: the ground");
        SurfaceIndex idx(v);
        CHECK(std::fabs(idx.at(t, 202, 200, 1e30f, &on, lo + 0.001f) - hi) < 1e-4f && on == 2 && idx.covers(202, 200) && !idx.covers(400, 400), "T3.1 the index agrees, and min_y / covers work");
        CHECK(std::fabs(idx.at(t, 183, 200, 1e30f, &on, hi) - 10.f) < 1e-4f && on == 0, "T3.1 min_y above the only top: the ground");
    }
    // T3.2 under a tilted sheet: the plane of its top face.
    {
        Sheet p = plate(1, 200, 200, 0);
        p.pos.y = 14.f; p.pitch = 0.2f; p.roll = -0.1f; p.fixed = true; p.yaw = 0.4f;
        std::vector<Sheet> v{p};
        double worst = 0;
        int n = 0;
        for (float x = 188; x <= 212; x += 3.7f)
            for (float z = 188; z <= 212; z += 3.1f)
                if (contains(p, x, z)) {
                    n++;
                    worst = std::max(worst, double(std::fabs(surface_at(t, v, x, z, 1e30f) - std::max(10.f, face_y(p, p.top, x, z)))));
                }
        CHECK(n > 20 && worst < 1e-4, "T3.2 surface_at under a tilted sheet is face_y of its top (%d points, worst %.6f)", n, worst);
    }
    // T3.3 tilt_for_up inverts up_of.
    {
        Rng rng(4242);
        double worst = 0;
        for (int i = 0; i < 100; i++) {
            const float yaw = (rng.unit() - 0.5f) * 6.2f, pi = (rng.unit() - 0.5f) * 1.2f, ro = (rng.unit() - 0.5f) * 1.2f;
            float p2 = 0, r2 = 0;
            tilt_for_up(up_of(yaw, pi, ro), yaw, p2, r2);
            worst = std::max(worst, double(std::max(std::fabs(p2 - pi), std::fabs(r2 - ro))));
        }
        CHECK(worst < 1e-4, "T3.3 tilt_for_up(up_of(...)) returns pitch and roll (worst %.6f)", worst);
    }
    // T3.4 a plate put down over a road goes under it: roads are always above the plates near them, whatever the order.
    {
        std::vector<Sheet> v{road(1, 200, 220, 0)};
        Sheet b = plate(2, 200, 200, INT_MIN);
        const int l = drop_layer(v, b);
        CHECK(l == 0 && v[0].layer == 1, "T3.4 a plate over a lone road takes its layer and the road moves up one (plate %d, road %d)", l, v[0].layer);
        b.layer = l;
        v.push_back(b);
        seat(t, v);
        CHECK(v[1].pos.y + v[1].top + S <= v[0].pos.y + 1e-4f, "T3.4 and the road sits kSheetStep over the plate (road %.4f, plate top %.4f)", v[0].pos.y, v[1].pos.y + v[1].top);
        // a second plate over both: over the first plate, still under the road
        Sheet c = plate(3, 205, 205, INT_MIN);
        const int l2 = drop_layer(v, c);
        c.layer = l2;
        v.push_back(c);
        seat(t, v);
        CHECK(v[1].layer < v[2].layer && v[2].layer < v[0].layer, "T3.4 a second plate: over the first, under the road (layers %d %d, road %d)", v[1].layer, v[2].layer, v[0].layer);
        CHECK(v[2].pos.y + v[2].top > v[1].pos.y + v[1].top + S - 1e-4f && v[2].pos.y + v[2].top + S <= v[0].pos.y + 1e-4f, "T3.4 heights follow: plate, plate, road, each kSheetStep apart");
        // a road placed after plates goes on top of all of them
        Sheet r2 = road(4, 202, 215, INT_MIN);
        r2.layer = drop_layer(v, r2);
        CHECK(r2.layer > v[0].layer, "T3.4 a road placed later goes on top of the road and plates under it (%d)", r2.layer);
        // equal layers: plate under road whatever the uids
        std::vector<Sheet> e{road(1, 200, 220, 0), plate(2, 200, 200, 0)};
        seat(t, e);
        CHECK(e[0].pos.y > e[1].pos.y + e[1].top, "T3.4 equal layers: the road is over the plate even with the lower uid");
        // a plate that was ordered above a road by hand stays above it
        std::vector<Sheet> h{road(1, 200, 220, 0), plate(2, 200, 200, 5)};
        Sheet d = plate(3, 203, 203, INT_MIN);
        CHECK(drop_layer(h, d) == 6, "T3.4 a plate over a road that is under another plate goes on top (6)");
        // a dragged plate keeps a layer that is already under the road, and slips under a road it was over
        std::vector<Sheet> m{plate(1, 400, 400, 0), road(2, 200, 220, 3)};
        Sheet mv = plate(3, 200, 200, 7);
        const int ml = drop_layer(m, mv, nullptr);
        CHECK(ml == 3 && m[1].layer == 4, "T3.4 a plate dragged (layer 7) onto a road (layer 3) slips under it (%d, road %d)", ml, m[1].layer);
        Sheet mv2 = plate(3, 200, 200, 2);
        std::vector<Sheet> m2{road(2, 200, 220, 3)};
        CHECK(drop_layer(m2, mv2) == 2 && m2[0].layer == 3, "T3.4 a plate already under the road keeps its layer");
    }
}

// When an action joins the undo step on top.
// The scatter brush (footprint radius, spacing grid, coverage cells, lean, the lowest point of a turned box).
static void test_scatter() {
    using namespace ma2;
    // T5.1 footprint_radius: half the longer side across the ground, at least 0.5 m.
    {
        const float a0[3] = {-5.f, 0.f, -2.f}, a1[3] = {5.f, 8.f, 2.f};
        const float b0[3] = {-0.1f, 0.f, -0.1f}, b1[3] = {0.1f, 0.2f, 0.1f};
        const float c0[3] = {-1.f, 0.f, -3.f}, c1[3] = {1.f, 4.f, 3.f};
        CHECK(std::fabs(footprint_radius(a0, a1) - 5.f) < 1e-5f, "T5.1 a 10 x 4 m box has radius 5 (%.3f)", footprint_radius(a0, a1));
        CHECK(std::fabs(footprint_radius(b0, b1) - 0.5f) < 1e-5f, "T5.1 a 0.2 m box has radius 0.5 (%.3f)", footprint_radius(b0, b1));
        CHECK(std::fabs(footprint_radius(c0, c1) - 3.f) < 1e-5f, "T5.1 the longer side counts, along z too (%.3f)", footprint_radius(c0, c1));
    }
    // T5.2 SpacingGrid: radius-5 objects refused closer than k x (5 + 5).
    {
        SpacingGrid g;
        g.add(0.f, 0.f, 5.f);
        CHECK(!g.clear(9.9f, 0.f, 5.f, 1.f), "T5.2 9.9 m from a radius-5 neighbour is refused at k 1.0");
        CHECK(g.clear(10.1f, 0.f, 5.f, 1.f), "T5.2 10.1 m is accepted at k 1.0");
        CHECK(g.clear(5.1f, 0.f, 5.f, 0.5f), "T5.2 5.1 m is accepted at k 0.5");
        CHECK(!g.clear(4.9f, 0.f, 5.f, 0.5f), "T5.2 4.9 m is refused at k 0.5");
        CHECK(!g.clear(0.f, -9.f, 5.f, 1.f) && g.clear(0.f, 40.f, 5.f, 1.f), "T5.2 across cells: 9 m refused, 40 m free");
        SpacingGrid big;
        big.add(0.f, 0.f, 30.f);
        CHECK(!big.clear(30.f, 0.f, 1.f, 1.f) && big.clear(32.f, 0.f, 1.f, 1.f), "T5.2 a radius-30 neighbour keeps a radius-1 object 31 m off (30 refused, 32 free)");
        SpacingGrid e;
        CHECK(e.clear(0.f, 0.f, 5.f, 1.f), "T5.2 an empty grid refuses nothing");
    }
    // T5.3 Coverage: a ring twice gives its cells once; moved 10 m only the new crescent.
    {
        Coverage cv;
        std::vector<std::pair<float, float>> cells;
        const int n1 = cv.newly(100.f, 100.f, 20.f, &cells);
        CHECK(n1 > 0 && int(cells.size()) == n1, "T5.3 a 20 m ring covers some cells (%d, listed %d)", n1, int(cells.size()));
        const float want = 3.14159265f * 400.f / (kScatterCell * kScatterCell);
        CHECK(std::fabs(float(n1) - want) < want * 0.05f, "T5.3 about pi R^2 / 4 cells (%d of %.0f)", n1, want);
        CHECK(cv.newly(100.f, 100.f, 20.f) == 0, "T5.3 the same ring again covers nothing new");
        const int n2 = cv.newly(110.f, 100.f, 20.f);
        CHECK(n2 > 0 && n2 < n1 && n1 + n2 == int(cv.done.size()), "T5.3 moved 10 m: only the crescent (%d new of %d)", n2, n1);
        // Every cell of the crescent is outside the first ring and inside the second.
        Coverage c2;
        c2.newly(100.f, 100.f, 20.f);
        std::vector<std::pair<float, float>> cres;
        c2.newly(110.f, 100.f, 20.f, &cres);
        bool ok = !cres.empty();
        for (auto& c : cres) {
            const float d1 = std::hypot(c.first - 100.f, c.second - 100.f), d2 = std::hypot(c.first - 110.f, c.second - 100.f);
            ok = ok && d1 > 20.f && d2 <= 20.f;
        }
        CHECK(ok, "T5.3 the crescent's cells lie outside the first ring and inside the second");
        // The expected count of a click is density x area: 40 per hectare over a 25 m ring is about 7.85.
        Coverage c3;
        const float got = float(c3.newly(0.f, 0.f, 25.f)) * scatter_per_cell(40.f);
        CHECK(std::fabs(got - 7.85f) < 0.5f, "T5.3 40 per hectare over a 25 m ring earns about 7.85 (%.2f)", got);
    }
    // lean_up: within the limit of the base up vector, unit length; a flat base with no lean stays up.
    {
        const float up[3] = {0.f, 1.f, 0.f};
        float o[3];
        bool ok = true;
        for (int i = 0; i < 40; i++) {
            lean_up(up, 10.f, float(i) * 0.7f, float(i % 5) / 4.f, o);
            const float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            const float ang = std::acos(std::clamp(o[1], -1.f, 1.f)) * 57.29578f;
            ok = ok && std::fabs(len - 1.f) < 1e-4f && ang <= 10.01f;
        }
        CHECK(ok, "T5 lean_up stays within 10 degrees and unit length");
        lean_up(up, 10.f, 1.f, 0.f, o);
        CHECK(std::fabs(o[0]) < 1e-5f && std::fabs(o[1] - 1.f) < 1e-5f && std::fabs(o[2]) < 1e-5f, "T5 a zero fraction leaves the up vector alone");
        const float slope[3] = {0.5f, 0.8660254f, 0.f};   // a 30 degree base
        lean_up(slope, 0.f, 2.f, 1.f, o);
        CHECK(std::fabs(o[0] - 0.5f) < 1e-4f && std::fabs(o[1] - 0.8660254f) < 1e-4f, "T5 no lean limit leaves a tilted base alone");
    }
    // rotate_local: the base centre of a box, turned. Untilted it is straight below the origin's height by -bmin[1]; the up axis maps to up_of.
    {
        const Vec3 f = place::rotate_local(1.2f, 0.f, 0.f, 0.f, -0.5f, 0.f);
        CHECK(std::fabs(f.x) < 1e-4f && std::fabs(f.y + 0.5f) < 1e-4f && std::fabs(f.z) < 1e-4f, "T5 an untilted base centre sits at (0, -0.5, 0) (%.3f %.3f %.3f)", f.x, f.y, f.z);
        const Vec3 u = place::up_of(0.4f, 0.3f, -0.2f), r = place::rotate_local(0.4f, 0.3f, -0.2f, 0.f, 1.f, 0.f);
        CHECK(std::fabs(u.x - r.x) < 1e-5f && std::fabs(u.y - r.y) < 1e-5f && std::fabs(u.z - r.z) < 1e-5f, "T5 rotate_local of (0,1,0) is up_of");
        const Vec3 q = place::rotate_local(0.f, 0.3f, 0.f, 0.f, -1.f, 0.f);
        CHECK(q.y > -1.f && q.y < -0.9f, "T5 a pitched base centre rises toward the origin (%.3f)", q.y);
    }
    // The brush up vector -> pitch / roll -> up vector round trip (what Follow the slope relies on).
    {
        const Vec3 n{0.3f, 0.9f, -0.2f};
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        const Vec3 u{n.x / l, n.y / l, n.z / l};
        float p = 0, r = 0;
        place::tilt_for_up(u, 0.8f, p, r);
        const Vec3 b = place::up_of(0.8f, p, r);
        CHECK(std::fabs(b.x - u.x) < 1e-3f && std::fabs(b.y - u.y) < 1e-3f && std::fabs(b.z - u.z) < 1e-3f, "T5 tilt_for_up inverts up_of (%.3f %.3f %.3f)", b.x, b.y, b.z);
    }
}

static void test_undo_rules() {
    using namespace ed;
    const std::set<uint32_t> a{1, 2}, b{1, 3}, a2{2, 1};
    const uint64_t ha = undo_sel_hash(a), hb = undo_sel_hash(b);
    CHECK(ha == undo_sel_hash(a2) && ha != hb && undo_sel_hash({}) != ha, "selection hash is order free and tells selections apart");
    CHECK(undo_should_merge("rotate", ha, 10.0, "rotate", ha, 10.5, true), "same tag, selection, 0.5 s, nothing undone: merge");
    CHECK(undo_should_merge("rotate", ha, 10.0, "rotate", ha, 11.49, true), "1.49 s still merges");
    CHECK(!undo_should_merge("rotate", ha, 10.0, "rotate", ha, 11.5, true), "1.5 s starts a new step");
    CHECK(!undo_should_merge("rotate", ha, 10.0, "rotate", ha, 13.0, true), "3 s starts a new step");
    CHECK(!undo_should_merge("rotate", ha, 10.0, "nudge", ha, 10.2, true), "another tag does not merge");
    CHECK(!undo_should_merge("rotate", ha, 10.0, "rotate", hb, 10.2, true), "another selection does not merge");
    CHECK(!undo_should_merge("rotate", ha, 10.0, "rotate", ha, 10.2, false), "a pending redo does not merge");
    CHECK(!undo_should_merge("", ha, 10.0, "", ha, 10.2, true), "an empty tag never merges");
    CHECK(!undo_should_merge("rotate", ha, 10.0, "rotate", ha, 9.0, true), "a clock that went back does not merge");
    CHECK(kUndoSteps == 200 && kUndoBudget == (1ull << 30), "history is 200 steps and 1 GiB");
}

// What a click, a box and a list range do to the selection; copy names.
static void test_select_rules() {
    using namespace ed;
    std::set<uint32_t> s{1, 2, 3, 4};
    sel_box(s, {1, 2}, {false, true});
    CHECK(s == std::set<uint32_t>({3, 4}), "Ctrl+box takes the boxed objects out");
    sel_click(s, 5, {true, false});
    CHECK(s == std::set<uint32_t>({3, 4, 5}), "Shift+click adds");
    sel_click(s, 5, {true, false});
    CHECK(s.size() == 3, "Shift+click on a selected object keeps it");
    sel_click(s, 5, {false, true});
    CHECK(s == std::set<uint32_t>({3, 4}), "Ctrl+click toggles it off");
    sel_click(s, 5, {false, true});
    CHECK(s.count(5) == 1 && s.size() == 3, "Ctrl+click toggles it on");
    sel_click(s, 0, {true, false});
    CHECK(s.size() == 3, "Shift+click on empty ground keeps the selection");
    sel_click(s, 0, {});
    CHECK(s.empty(), "a plain click on empty ground clears");
    sel_click(s, 7, {});
    sel_click(s, 8, {});
    CHECK(s == std::set<uint32_t>({8}), "a plain click selects only that object");
    sel_box(s, {1, 2}, {true, false});
    CHECK(s == std::set<uint32_t>({1, 2, 8}), "Shift+box adds");
    sel_box(s, {9}, {});
    CHECK(s == std::set<uint32_t>({9}), "a plain box replaces");
    CHECK(same_type(5, 10, 5, 10) && !same_type(5, 10, 5, 11) && !same_type(5, 10, 6, 10) && same_type(5, 0, 5, 11), "same type: class and template (a helper without one: the class)");
    const std::vector<uint32_t> order{10, 20, 30, 40, 50};
    CHECK(list_range(order, 20, 40) == std::vector<uint32_t>({20, 30, 40}), "list range down");
    CHECK(list_range(order, 40, 20) == std::vector<uint32_t>({20, 30, 40}), "list range up");
    CHECK(list_range(order, 99, 30) == std::vector<uint32_t>({30}) && list_range(order, 20, 20) == std::vector<uint32_t>({20}), "no anchor: just the row");
    CHECK(ma2::copy_name("Cougar 7") == "Cougar 7 copy" && ma2::copy_name("Cougar 7 (copy)") == "Cougar 7 copy" && ma2::copy_name("Cougar 7 copy copy") == "Cougar 7 copy" &&
              ma2::copy_name("Cougar 7 (copy) (copy)") == "Cougar 7 copy" && ma2::copy_name("").empty(),
          "copy_name never stacks copy suffixes");
}


// The snapping solver. Orthographic to_px: metres x 10 unless said.
static ma2::snap::Mover snap_road(float len) {
    ma2::snap::Mover m;
    m.fam = 4;
    m.cx = 0; m.cz = -len / 2; m.hx = 13; m.hz = len / 2;
    m.straight = true; m.road_w = 13; m.road_l = len;
    ma2::snap::Socket a, b;
    a.x = 0; a.z = 0; a.ox = 0; a.oz = 1; a.index = 0;
    b.x = 0; b.z = -len; b.ox = 0; b.oz = -1; b.index = 1;
    m.own = {a, b};
    return m;
}
static ma2::snap::Mover snap_wall(float len, float ratio, int fam) {  // long axis along z, centred on its origin
    ma2::snap::Mover m;
    m.fam = fam;
    m.hx = 1; m.hz = len / 2; m.ratio = ratio;
    ma2::snap::Socket a, b;
    a.x = 0; a.z = len / 2; a.ox = 0; a.oz = 1; a.index = 0;
    b.x = 0; b.z = -len / 2; b.ox = 0; b.oz = -1; b.index = 1;
    m.own = {a, b};
    return m;
}
static ma2::snap::Mover snap_plate(float w, float d) {
    ma2::snap::Mover m;
    m.fam = 3; m.hx = w / 2; m.hz = d / 2; m.top = 0.1f;
    const float ex[4] = {w / 2, -w / 2, 0, 0}, ez[4] = {0, 0, d / 2, -d / 2}, ox[4] = {1, -1, 0, 0}, oz[4] = {0, 0, 1, -1};
    for (int i = 0; i < 4; i++) {
        ma2::snap::Socket e;
        e.x = ex[i]; e.z = ez[i]; e.ox = ox[i]; e.oz = oz[i]; e.index = i; e.len = i < 2 ? d / 2 : w / 2;
        m.own.push_back(e);
    }
    return m;
}
static ma2::snap::Socket snap_sock(int fam, float x, float z, float ox, float oz, uint32_t uid, int index) {
    ma2::snap::Socket s;
    s.fam = fam; s.x = x; s.z = z; s.ox = ox; s.oz = oz; s.uid = uid; s.index = index;
    s.axis = std::atan2(ox, oz);
    return s;
}
static void test_snap() {
    using namespace ma2::snap;
    const ToPx ortho = [](float x, float, float z, float& sx, float& sy) { sx = x * 10; sy = z * 10; return true; };
    std::vector<Cand> c;
    // T1 road: the free ghost's entry 1 m from an open end joins; 30 m and 88 m (the audit's pull) do not.
    {
        const Mover m = snap_road(60);
        std::vector<Socket> open{snap_sock(4, 500, 440, 0, -1, 1, 1)};
        c.clear();
        same_family(m, open, c);
        int w = choose(c, m, 500, 411, 0, kReachPx, Prev{}, ortho);   // entry at (500, 441)
        CHECK(w >= 0 && c[size_t(w)].kind == Kind::End && std::fabs(c[size_t(w)].ox - 500) < 0.01f && std::fabs(c[size_t(w)].oz - 440) < 0.01f &&
                  std::fabs(c[size_t(w)].yaw) < 1e-4f && std::fabs(c[size_t(w)].d_m - 1.f) < 0.01f,
              "T1 entry 1 m from the open end joins it (win %d)", w);
        CHECK(choose(c, m, 588, 410, 0, kReachPx, Prev{}, ortho) < 0, "T1 88 m away does not join");
        CHECK(choose(c, m, 530, 410, 0, kReachPx, Prev{}, ortho) < 0, "T1 30 m away does not join");
        w = choose(c, m, 500, 411, kPi, kReachPx, Prev{}, ortho);     // heading 180: its exit end is the one 1 m from the open end
        CHECK(w >= 0 && std::fabs(std::fabs(c[size_t(w)].yaw) - kPi) < 1e-3f && c[size_t(w)].own == 1, "T1 heading 180 takes the exit-join variant (win %d)", w);
    }
    // T2 two open ends: the nearer older one wins (no chain bonus).
    {
        const Mover m = snap_road(60);
        std::vector<Socket> open{snap_sock(4, 500, 434, 0, -1, 9, 1), snap_sock(4, 500, 440, 0, -1, 1, 1)};   // newest piece (uid 9) 6 m beyond the older one
        c.clear();
        same_family(m, open, c);
        int w = choose(c, m, 500, 410.5f, 0, kReachPx, Prev{}, ortho);   // entry at (500, 440.5): 0.5 m from uid 1, 6.5 m from uid 9
        CHECK(w >= 0 && c[size_t(w)].uid == 1, "T2 the older, nearer end wins");
    }
    // T3 hysteresis.
    {
        const Mover m = snap_road(60);
        std::vector<Socket> open{snap_sock(4, 500, 440, 0, -1, 1, 1)};
        c.clear();
        same_family(m, open, c);
        int w = choose(c, m, 500, 411.5f, 0, kReachPx, Prev{}, ortho);   // 1.5 m = 15 px
        CHECK(w >= 0, "T3 accepted at 15 px");
        if (w >= 0) {
            Prev pv;
            pv.on = true; pv.uid = c[size_t(w)].uid; pv.socket = c[size_t(w)].socket; pv.own = c[size_t(w)].own; pv.kind = c[size_t(w)].kind; pv.yaw = c[size_t(w)].yaw;
            CHECK(choose(c, m, 500, 412.2f, 0, kReachPx, pv, ortho) >= 0, "T3 kept at 22 px");
            CHECK(choose(c, m, 500, 412.5f, 0, kReachPx, pv, ortho) < 0, "T3 dropped at 25 px");
        }
        CHECK(choose(c, m, 500, 412.2f, 0, kReachPx, Prev{}, ortho) < 0, "T3 not taken at 22 px without prev");
    }
    // T4 walls: spacing, corners, mirrored, the heading pick; T8 the Q / E cycle.
    {
        Mover m = snap_wall(40.5f, 1.f, 1);
        std::vector<Socket> open{snap_sock(1, 500, 520.25f, 0, 1, 1, 0)};   // a neighbour at (500, 500) running along z; its +z end is open
        open[0].py = 12.5f;
        c.clear();
        same_family(m, open, c);
        bool straight = false, corner = false, mirror = false, corner_ok = false;
        for (const Cand& k : c) {
            if (std::fabs(k.yaw) < 1e-3f && std::fabs(k.oz - 540.5f) < 0.01f && std::fabs(k.ox - 500) < 0.01f && k.y == 12.5f) straight = true;
            if (std::fabs(std::fabs(k.yaw) - kPi) < 1e-3f) mirror = true;
            if (std::fabs(k.yaw - kPi / 2) < 1e-3f) {
                corner = true;
                // the piece is centred on its origin: 20.25 m along the way it turns, its end on the join
                if (std::fabs(k.ox - 520.25f) < 0.01f && std::fabs(k.oz - 520.25f) < 0.01f) corner_ok = true;
            }
        }
        CHECK(straight, "T4 straight join: centre spacing 40.5");
        CHECK(corner && corner_ok, "T4 a corner at +90 with no overlap");
        CHECK(mirror, "T4 the mirrored candidate (+180) exists");
        Mover lw = snap_wall(40.5f, 0.94f, 1);
        c.clear();
        same_family(lw, open, c);
        bool sp = false;
        for (const Cand& k : c)
            if (std::fabs(k.yaw) < 1e-3f && std::fabs((k.oz - 500) - (20.25f + 20.25f * 0.88f)) < 0.01f) sp = true;
        CHECK(sp, "T4 LargeWall ratio 0.94: reach half x 0.88, the overlap retail uses");
        // H = neighbour + 120 deg, free end on the join: the +90 corner wins, never the straight one.
        c.clear();
        same_family(m, open, c);
        const float H = 120.f * kPi / 180.f;
        const float wx = std::sin(H) * (-20.25f), wz = std::cos(H) * (-20.25f);   // R(H) applied to own end 1 (0, -20.25), relative to the centre
        const float fcx = 500 - wx, fcz = 520.25f - wz;
        int w = choose(c, m, fcx, fcz, H, kReachPx, Prev{}, ortho);
        CHECK(w >= 0 && std::fabs(c[size_t(w)].yaw - kPi / 2) < 1e-3f, "T4 +120 deg heading picks the +90 corner (win %d)", w);
        int w0 = -1;
        for (size_t i = 0; i < c.size(); i++) if (std::fabs(c[i].yaw) < 1e-3f) { w0 = int(i); break; }
        float y = 0, seq[4];
        for (int i = 0; i < 4; i++) { y = next_yaw(c, w0, y, 1); seq[i] = y; }
        CHECK(std::fabs(seq[0] - kPi / 2) < 1e-3f && std::fabs(std::fabs(seq[1]) - kPi) < 1e-3f && std::fabs(seq[2] + kPi / 2) < 1e-3f && std::fabs(seq[3]) < 1e-3f,
              "T8 next_yaw +1 cycles 90, 180, 270, 0 (%.2f %.2f %.2f %.2f)", seq[0], seq[1], seq[2], seq[3]);
        y = 0;
        for (int i = 0; i < 4; i++) { y = next_yaw(c, w0, y, -1); seq[i] = y; }
        CHECK(std::fabs(seq[0] + kPi / 2) < 1e-3f && std::fabs(std::fabs(seq[1]) - kPi) < 1e-3f && std::fabs(seq[2] - kPi / 2) < 1e-3f && std::fabs(seq[3]) < 1e-3f,
              "T8 next_yaw -1 cycles the other way (%.2f %.2f %.2f %.2f)", seq[0], seq[1], seq[2], seq[3]);
        CHECK(std::isnan(next_yaw(std::vector<Cand>(), 0, 0, 1)), "T8 no candidates: NaN (a plain turn)");
    }
    // T5 barricade: cap 2 m.
    {
        const Mover m = snap_wall(3.6f, 1.f, 1);
        CHECK(std::fabs(cap_m(m) - 2.f) < 1e-4f, "T5 barricade cap 2 m");
        std::vector<Socket> open{snap_sock(1, 600, 601.8f, 0, 1, 1, 0)};
        c.clear();
        same_family(m, open, c);
        const ToPx coarse = [](float x, float, float z, float& sx, float& sy) { sx = x * 4; sy = z * 4; return true; };
        // the heading-0 candidate joins by its own end 1 (local (0, -1.8)): the free centre sits 1.8 m past the free end
        CHECK(choose(c, m, 600, 601.8f + 1.8f + 1.5f, 0, kReachPx, Prev{}, ortho) >= 0, "T5 free end 1.5 m away joins");
        CHECK(choose(c, m, 600, 601.8f + 1.8f + 2.5f, 0, kReachPx, Prev{}, coarse) < 0, "T5 2.5 m does not join (cap), even at 10 px");
        CHECK(choose(c, m, 600, 601.8f + 1.8f + 80.f, 0, kReachPx, Prev{}, ortho) < 0, "T5 80 m does not join");
    }
    // T11 aiming at an open end: the footprint centre within reach of it joins there; far away, or with Shift (not asked), does not.
    {
        const Mover m = snap_road(60);
        std::vector<Socket> open{snap_sock(4, 500, 440, 0, -1, 1, 1)};
        c.clear();
        same_family(m, open, c);
        int w = choose(c, m, 500, 438.5f, 0, kReachPx, Prev{}, ortho);   // the road's middle 1.5 m before the end, its own ends far away
        CHECK(w >= 0 && c[size_t(w)].own == 0 && std::fabs(c[size_t(w)].oz - 440.f) < 0.01f && std::fabs(c[size_t(w)].yaw) < 1e-4f && std::fabs(c[size_t(w)].d_m - 1.5f) < 0.01f,
              "T11 road centred 1.5 m from an open end joins it (win %d)", w);
        CHECK(choose(c, m, 500, 435.f, 0, kReachPx, Prev{}, ortho) < 0, "T11 road centred 5 m (50 px) from the end does not join");
        Mover wl = snap_wall(40.5f, 1.f, 1);
        std::vector<Socket> wopen{snap_sock(1, 500, 520.25f, 0, 1, 1, 0)};
        c.clear();
        same_family(wl, wopen, c);
        w = choose(c, wl, 500, 521.f, 0, kReachPx, Prev{}, ortho);       // wall centred 0.75 m past the open end, overlapping its neighbour
        CHECK(w >= 0 && std::fabs(c[size_t(w)].oz - 540.5f) < 0.01f && std::fabs(c[size_t(w)].ox - 500.f) < 0.01f, "T11 wall centred on an open end joins end to end (win %d)", w);
        CHECK(choose(c, wl, 500, 534.f, 0, kReachPx, Prev{}, ortho) < 0, "T11 wall centred 14 m from the end does not join");
        // a wall turned 90 deg to the neighbour whose centre is on the end takes the corner variant, not the straight one
        w = choose(c, wl, 500, 521.f, kPi / 2, kReachPx, Prev{}, ortho);
        CHECK(w >= 0 && std::fabs(c[size_t(w)].yaw - kPi / 2) < 1e-3f, "T11 wall centred on the end, heading +90: the corner (win %d)", w);
    }
    // T6 bridge 236 m.
    {
        const Mover m = snap_wall(236.f, 1.f, 2);
        std::vector<Socket> open{snap_sock(2, 600, 420, 0, 1, 1, 0)};
        c.clear();
        same_family(m, open, c);
        {   // Aiming with the centre at the open end joins it (the piece's own end then meets it, centre 118 m past)
            int wc = choose(c, m, 600, 420, 0, kReachPx, Prev{}, ortho);
            CHECK(wc >= 0 && std::fabs(c[size_t(wc)].oz - (420.f + 118.f)) < 0.01f, "T6 free centre at the open end joins by aim (win %d)", wc);
            CHECK(choose(c, m, 600, 420.f - 14.f, 0, kReachPx, Prev{}, ortho) < 0, "T6 free centre 14 m before the open end: no join");
        }
        int w = choose(c, m, 600, 421.f + 118.f, 0, kReachPx, Prev{}, ortho);
        CHECK(w >= 0 && std::fabs(c[size_t(w)].oz - (420.f + 118.f)) < 0.01f, "T6 free end 1 m from the open end joins, centre 118 m past it (win %d)", w);
    }
    // T7 plates.
    {
        const Mover m = snap_plate(38, 38);
        Socket s = snap_sock(3, 519, 500, 1, 0, 1, 0);
        s.len = 19; s.half = 19; s.y = 10.1f;
        c.clear();
        same_family(m, {s}, c);
        bool mid = false;
        std::set<int> yaws;
        for (const Cand& k : c) {
            yaws.insert(int(std::lround(k.yaw * 2 / kPi)));
            if (k.kind == Kind::Edge && std::fabs(k.yaw) < 1e-3f && std::fabs(k.ox - 538) < 0.01f && std::fabs(k.oz - 500) < 0.01f && std::fabs(k.y - 10.0f) < 1e-3f) mid = true;
        }
        CHECK(mid, "T7 edge-to-edge: centre 38 m from the neighbour, same yaw");
        CHECK(yaws.size() == 4, "T7 four quarter turns (%zu)", yaws.size());
        const Mover pk = snap_plate(30, 30);
        c.clear();
        same_family(pk, {s}, c);
        std::vector<float> zs;
        for (const Cand& k : c) if (std::fabs(k.yaw) < 1e-3f) zs.push_back(k.oz);
        std::sort(zs.begin(), zs.end());
        CHECK(zs.size() == 3 && std::fabs(zs[0] - 496) < 0.01f && std::fabs(zs[1] - 500) < 0.01f && std::fabs(zs[2] - 504) < 0.01f,
              "T7 30 m lot on a 38 m base: middle and two corner-flush candidates 4 m apart (%zu)", zs.size());
    }
    // T9 off-screen targets and the heading tolerance.
    {
        const Mover m = snap_road(60);
        std::vector<Socket> open{snap_sock(4, 500, 440, 0, -1, 1, 1)};
        c.clear();
        same_family(m, open, c);
        const ToPx off = [](float, float, float, float&, float&) { return false; };
        CHECK(choose(c, m, 500, 411, 0, kReachPx, Prev{}, off) < 0, "T9 a target that is off screen is never chosen");
        const float d49 = 49.f * kPi / 180.f, d51 = 51.f * kPi / 180.f;
        auto at = [&](float H) {  // the free entry on the end, the free ghost turned to H
            const float wx = std::sin(H) * 30.f, wz = std::cos(H) * 30.f;   // R(H) applied to (0, 30): the entry sits that far from the centre
            return choose(c, m, 500 - wx, 440 - wz, H, kReachPx, Prev{}, ortho);
        };
        CHECK(at(d49) >= 0, "T9 49 degrees accepted");
        CHECK(at(d51) < 0, "T9 51 degrees rejected");
    }
    // T10 rounding.
    CHECK(size_step(42.3f, 5, 1) == 40.f && size_step(0.4f, 5, 1) == 1.f && size_step(42.3f, 0, 1) == 42.3f && size_step(3.f, 5, 1) == 5.f, "R5 size_step (%.2f %.2f)", size_step(42.3f, 5, 1), size_step(0.4f, 5, 1));
    // T3.1 edge_snap: the dragged side's middle at the origin, outward +z, running along x; a 1 m wide piece.
    {
        EdgeDrag e;
        e.ex = 0; e.ez = 0; e.nx = 0; e.nz = 1; e.tx = 1; e.tz = 0; e.half_t = 0.5f; e.uid = 7;
        auto pt = [](float x, float z, uint32_t uid) { EdgeTarget t; t.x = x; t.z = z; t.kind = 0; t.uid = uid; return t; };
        auto line = [](float z, float deg, uint32_t uid) { EdgeTarget t; t.x = 0; t.z = z; t.dx = std::cos(deg * kPi / 180.f); t.dz = std::sin(deg * kPi / 180.f); t.kind = 1; t.uid = uid; return t; };
        EdgeHit h;
        CHECK(edge_snap(e, {pt(0.2f, 0.8f, 1)}, 16, 8, nullptr, h) && std::fabs(h.s - 0.8f) < 1e-5f && h.uid == 1, "R5 T3.1 point 0.8 m ahead s=0.8 (%.3f)", h.s);
        CHECK(!edge_snap(e, {pt(0, 3.f, 1)}, 16, 8, nullptr, h), "R5 T3.1 3 m ahead: none");
        CHECK(!edge_snap(e, {pt(2.f, 0.5f, 1)}, 16, 8, nullptr, h), "R5 T3.1 lateral 2 m on a 1 m piece: none");
        CHECK(edge_snap(e, {pt(3.f, 0.5f, 1)}, 16, 8, nullptr, h) == false, "R5 T3.1 lateral 3 m: none");
        e.half_t = 4.f;
        CHECK(edge_snap(e, {pt(3.f, 0.5f, 1)}, 16, 8, nullptr, h), "R5 T3.1 lateral 3 m on an 8 m piece: accepted");
        e.half_t = 0.5f;
        CHECK(edge_snap(e, {line(-0.6f, 1.f, 2)}, 16, 8, nullptr, h) && std::fabs(h.s + 0.6f) < 1e-5f && h.kind == 1, "R5 T3.1 a line 1 deg off: accepted (%.3f)", h.s);
        CHECK(!edge_snap(e, {line(-0.6f, 5.f, 2)}, 16, 8, nullptr, h), "R5 T3.1 a line 5 deg off: refused");
        CHECK(edge_snap(e, {line(0.5f, 180.f, 2), pt(0, 1.5f, 3)}, 16, 8, nullptr, h) && h.uid == 2, "R5 T3.1 the nearer target wins, a reversed line counts");
        EdgeHit prev;
        prev.kind = 0; prev.uid = 1; prev.index = 0;
        CHECK(edge_snap(e, {pt(0, 2.8f, 1)}, 16, 8, &prev, h) && h.uid == 1, "R5 T3.1 hysteresis keeps a hit at 1.4 x reach");
        CHECK(!edge_snap(e, {pt(0, 3.2f, 1)}, 16, 8, &prev, h), "R5 T3.1 hysteresis drops it at 1.6 x");
        CHECK(!edge_snap(e, {pt(0, 2.8f, 1)}, 16, 8, nullptr, h), "R5 T3.1 no previous hit: 1.4 x reach is out");
    }
    CHECK(grid_round(-7.4f, 5) == -5.f && grid_round(7.5f, 5) == 10.f && grid_round(12.3f, 2) == 12.f, "T10 grid_round");
    const float st = 15.f * kPi / 180.f;
    CHECK(std::fabs(angle_round(0.0099f, st)) < 1e-6f && std::fabs(angle_round(3.10f, st) - kPi) < 1e-4f && std::fabs(angle_round(-3.14f, st) - kPi) < 1e-4f,
          "T10 angle_round (%.4f %.4f)", angle_round(3.10f, st), angle_round(-3.14f, st));
    // T19 an axis move rounds one coordinate and leaves the other alone (the arrow code rounds along the axis only).
    {
        const float x = 501.3f + 37.f * 0.1f, z = 612.37f;
        const float nx = grid_round(x, 5.f);
        CHECK(std::fabs(nx - 505.f) < 1e-3f && z == 612.37f, "T19 axis grid (%.3f, %.3f)", nx, z);
        CHECK(grid_round(grid_round(501.3f + 5.f, 5.f) + 5.f, 5.f) == 510.f && grid_round(501.3f + 5.f, 5.f) == 505.f, "T19 nudge lattice");
    }
    // T20 the translation-only filter keeps candidates within 1 degree of the anchor's heading.
    {
        std::vector<Cand> c(4);
        c[0].yaw = 0.5f; c[1].yaw = 0.5f + 0.01f; c[2].yaw = 0.5f + 0.1f; c[3].yaw = 0.5f + kPi;
        filter_translation(c, 0.5f);
        CHECK(c.size() == 2 && c[0].yaw == 0.5f && std::fabs(c[1].yaw - 0.51f) < 1e-6f, "T20 filter_translation keeps %zu", c.size());
        std::vector<Cand> w(2);
        w[0].yaw = 3.13f; w[1].yaw = -3.12f;   // across the wrap: 0.023 rad = 1.3 deg away: only the near one stays
        filter_translation(w, 3.14f);
        CHECK(w.size() == 1 && w[0].yaw == 3.13f, "T20 wrap (%zu)", w.size());
    }
}

// Cross-family snaps (building on a plate, road beside / ending at a plate edge, wall at a corner), plate runs, the heading pull.
static ma2::snap::Mover snap_building(float w, float d) {   // footprint centred on its origin
    ma2::snap::Mover m;
    m.fam = 5; m.hx = w / 2; m.hz = d / 2;
    auto add = [&](float x, float z, float ox, float oz, int i) { ma2::snap::Socket s; s.x = x; s.z = z; s.ox = ox; s.oz = oz; s.index = i; m.own.push_back(s); };
    add(0, 0, 0, 0, 0);
    add(m.hx, 0, 1, 0, 1); add(-m.hx, 0, -1, 0, 2); add(0, m.hz, 0, 1, 3); add(0, -m.hz, 0, -1, 4);
    add(m.hx, m.hz, 0, 0, 5); add(m.hx, -m.hz, 0, 0, 6); add(-m.hx, m.hz, 0, 0, 7); add(-m.hx, -m.hz, 0, 0, 8);
    return m;
}
static ma2::snap::Anchor snap_anchor(int kind, float cx, float cz, float yaw, float hx, float hz) {
    ma2::snap::Anchor a;
    a.uid = 7; a.kind = kind; a.cx = cx; a.cz = cz; a.yaw = yaw; a.hx = hx; a.hz = hz; a.top = 10.1f; a.py = 10.f;
    return a;
}
static void test_snap_cross() {
    using namespace ma2::snap;
    const ToPx ortho = [](float x, float, float z, float& sx, float& sy) { sx = x * 10; sy = z * 10; return true; };
    const float D = kPi / 180.f;
    auto near2 = [](float a, float b, float t = 0.01f) { return std::fabs(a - b) < t; };
    std::vector<Cand> c;
    // T11 Office 20 x 14 on a 38 x 38 plate turned 30 degrees.
    {
        const Mover m = snap_building(20, 14);
        const float Y = 30 * D;
        const Anchor a = snap_anchor(3, 500, 500, Y, 19, 19);
        on_plate(m, a, 500, 500, c);
        int q0 = 0;
        bool centre = false, ne = false;
        const float ux = std::cos(Y), uz = -std::sin(Y), vx = std::sin(Y), vz = std::cos(Y);
        for (const Cand& k : c) {
            if (std::fabs(wrap_pi(k.yaw - Y)) > 1e-3f) continue;
            q0++;
            if (k.kind == Kind::Centre && near2(k.ox, 500) && near2(k.oz, 500)) centre = true;
            if (k.kind == Kind::Inside && k.socket == 5) {
                // the office's own corner (+10, +7), turned by the plate's yaw, lands on the plate's (+19, +19) corner
                const float jx = k.ox + std::cos(k.yaw) * 10 + std::sin(k.yaw) * 7, jz = k.oz - std::sin(k.yaw) * 10 + std::cos(k.yaw) * 7;
                if (near2(jx, 500 + ux * 19 + vx * 19) && near2(jz, 500 + uz * 19 + vz * 19)) ne = true;
            }
        }
        CHECK(c.size() == 36 && q0 == 9, "T11 centre, 4 inside edges and 4 inside corners per quarter turn (%zu, %d)", c.size(), q0);
        CHECK(centre && ne, "T11 centred on the plate; the NE-corner candidate puts the footprint corner on the plate's corner");
        const float H = Y + 100 * D;
        const int w = choose(c, m, 500, 500, H, kReachPx, Prev{}, ortho);
        CHECK(w >= 0 && c[size_t(w)].kind == Kind::Centre && std::fabs(wrap_pi(c[size_t(w)].yaw - (Y + 90 * D))) < 1e-3f, "T11 H = plate yaw + 100 deg picks plate yaw + 90 (win %d)", w);
        c.clear();
        on_plate(m, a, 560, 500, c);
        CHECK(c.empty(), "T11 a free centre well outside the plate offers nothing");
    }
    // T12 Office wider than the plate: only x = 0 on that axis.
    {
        const Mover m = snap_building(20, 14);
        const Anchor a = snap_anchor(3, 500, 500, 0, 4, 19);   // 8 x 38 plate
        c.clear();
        on_plate(m, a, 500, 500, c);
        bool allx = !c.empty();
        for (const Cand& k : c) if (!near2(k.ox, 500)) allx = false;
        CHECK(allx, "T12 a piece wider than the plate stays centred on that axis (%zu candidates)", c.size());
    }
    // T13 Straight road beside a plate's east edge.
    {
        const Mover m = snap_road(60);
        const Anchor a = snap_anchor(3, 500, 500, 0, 19, 19);
        auto centre_of_c = [&](const Cand& k, float& x, float& z) { centre_of(m, k.ox, k.oz, k.yaw, x, z); };
        c.clear();
        road_edge(m, a, 522, 505, 0, c);
        bool out_ok = false, along = false;
        for (const Cand& k : c) {
            if (k.kind != Kind::Beside) continue;
            float x, z;
            centre_of_c(k, x, z);
            const bool par = near2(std::fabs(std::sin(k.yaw)), 0, 1e-3f);
            if (par && near2(x, 532) && near2(z, 505)) out_ok = true;
        }
        c.clear();
        road_edge(m, a, 510, 490, 0, c);
        bool in_ok = false;
        for (const Cand& k : c) {
            float x, z;
            centre_of_c(k, x, z);
            if (k.kind == Kind::Beside && near2(x, 506) && near2(z, 490)) in_ok = true;
            if (k.kind == Kind::Corner && near2(k.tx, 506) && near2(k.tz, 519)) along = true;   // the end flush with the NE corner (inside variant)
        }
        CHECK(out_ok, "T13 free centre outside: kerb on the edge line (centre x = edge + 13), parallel, sliding with the free centre");
        CHECK(in_ok && along, "T13 free centre inside: road inside, kerb on the line; a corner-flush candidate puts the road end on the corner");
        Mover turn = snap_road(60);
        turn.straight = false;
        c.clear();
        road_edge(turn, a, 522, 505, 0, c);
        bool beside = false;
        for (const Cand& k : c) if (k.kind == Kind::Beside || k.kind == Kind::Corner) beside = true;
        CHECK(!beside, "T13 a turn piece gets no Beside candidate");
    }
    // T14 Road ending at the south edge: end on the edge line, perpendicular and outside, sliding; the middle as an extra.
    {
        const Mover m = snap_road(60);
        const Anchor a = snap_anchor(3, 500, 500, 0, 19, 19);
        c.clear();
        road_edge(m, a, 505, 451, 0, c);   // the free entry (0, 0) sits 30 m from the centre: (505, 481)
        bool slide = false, mid = false;
        for (const Cand& k : c) {
            if (k.own != 0 || k.socket != 4 || std::fabs(k.yaw) > 1e-3f) continue;
            if (k.kind == Kind::EndOnEdge && near2(k.ox, 505) && near2(k.oz, 481)) slide = true;
            if (k.kind == Kind::Edge && near2(k.ox, 500) && near2(k.oz, 481)) mid = true;
        }
        CHECK(slide && mid, "T14 own end on the south edge line, road outside, perpendicular; the middle is an extra candidate");
    }
    // T15 Wall at a building corner and at a plate corner.
    {
        const Mover m = snap_wall(10, 1, 1);   // hx = 1: 2 m thick
        const Anchor b = snap_anchor(5, 500, 500, 0, 10, 7);   // 20 x 14 office, NE corner (510, 507)
        c.clear();
        wall_corner(m, b, c);
        bool face = false, line = false;
        for (const Cand& k : c) {
            if (k.socket != 5 || std::fabs(k.yaw) > 1e-3f) continue;
            if (near2(k.tx, 511) && near2(k.tz, 507) && near2(k.ox, 511) && near2(k.oz, 502)) face = true;   // inner face on x = 510, running along the face
            if (near2(k.tx, 510) && near2(k.tz, 507) && near2(k.ox, 510) && near2(k.oz, 512)) line = true;   // on the edge line, away from the corner
        }
        CHECK(face && line, "T15 wall end on the building's corner: inner face on the face (offset half thickness, outward), or on along the edge line");
        const Anchor p = snap_anchor(3, 500, 500, 0, 19, 19);
        c.clear();
        wall_corner(m, p, c);
        bool rim = false;
        for (const Cand& k : c) if (k.socket == 5 && std::fabs(k.yaw) < 1e-3f && near2(k.tx, 518) && near2(k.tz, 519) && near2(k.ox, 518)) rim = true;
        CHECK(rim, "T15 along a plate edge the wall sits inward (a fence on the rim)");
        CHECK(c.size() == 32, "T15 4 corners x 2 edges x 2 directions x 2 ends (%zu)", c.size());
    }
    // T16 Plates join into one run when their edges touch along 2 m or more, not at a corner point.
    {
        CHECK(near2(edge_overlap(519, 500, 1, 0, 19, 519, 504, -1, 0, 15), 30.f), "T16 a 30 m lot flush with the corner of a 38 m base touches along 30 m");
        CHECK(near2(edge_overlap(519, 500, 1, 0, 19, 519, 534, -1, 0, 15), 0.f), "T16 touching at a corner point only: not a run");
        CHECK(edge_overlap(519, 500, 1, 0, 19, 519.5f, 500, -1, 0, 19) == 0.f, "T16 edges 0.5 m apart are not on one line");
        CHECK(edge_overlap(519, 500, 1, 0, 19, 519, 500, 1, 0, 19) == 0.f, "T16 edges facing the same way do not touch");
    }
    // A joint may turn the ghost by up to 50 degrees. The joint is the ghost's own end, so a ghost 15 degrees off joins when ITS
    // END is at the open end and the candidate then straightens it; a ghost whose centre sits where an aligned one would does not (its end swung away).
    {
        const Mover m = snap_wall(40.5f, 1.f, 1);
        Socket s = snap_sock(1, 500, 520.25f, 0, 1, 1, 0);
        s.py = 12.5f;
        c.clear();
        same_family(m, {s}, c);
        const float H = 15 * D;
        int w = choose(c, m, 500 + 20.25f * std::sin(H), 520.25f + 20.25f * std::cos(H), H, kReachPx, Prev{}, ortho);
        CHECK(w >= 0 && std::fabs(c[size_t(w)].yaw) < 1e-3f, "D3 ghost 15 deg off with its end on the open end joins and straightens (win %d)", w);
        CHECK(choose(c, m, 500, 540.5f, H, kReachPx, Prev{}, ortho) < 0, "D3 ghost 15 deg off whose centre is where an aligned one would sit: its end is 5 m away, no join");
    }
}


// Clip_volumes on a synthetic XML (no game data).
static void test_clip_volumes() {
    auto box = [](float ox, float oy, float oz, const char* ax, float lx, float ly, float lz) {
        char b[600];
        snprintf(b, sizeof b, "<bv_obb origin=\"%.8f, %.8f, %.8f\" %s lengths=\"%.8f, %.8f, %.8f\"/>\n", ox, oy, oz, ax, lx, ly, lz);
        return std::string(b);
    };
    const char* straight = "xaxis=\"1.0, 0.0, 0.0\" yaxis=\"0.0, 1.0, 0.0\" zaxis=\"0.0, 0.0, 1.0\"";
    const char* turned = "xaxis=\"0.70710678, 0.0, 0.70710678\" yaxis=\"0.0, 1.0, 0.0\" zaxis=\"-0.70710678, 0.0, 0.70710678\"";
    const std::string head = "<xmlres>\n<node_generic name=\"root\" position=\"0, 0, 0\" rot_axis=\"-1, 0, 0\" rot_angle=\"0\" scale=\"1, 1, 1\">\n<bounding_volume>\n<bv_composite>\n";
    const std::string tail = "</bv_composite>\n</bounding_volume>\n</node_generic>\n</xmlres>\n";
    // A: 20 m long along z at z -10..10 (half inside the kept z 0..30); B: z 40..50 (fully outside); C: a box turned 45 deg around y at z -1 (its centre outside, a corner inside)
    const std::string src = head + box(0, 2, 0, straight, 3, 2, 10) + box(0, 2, 45, straight, 3, 2, 5) + box(0, 2, -1, turned, 3, 2, 3) + tail;
    const crop::Rect r{-10.f, 10.f, 0.f, 30.f};
    const std::string out = crop::clip_volumes(src, r);
    auto nbox = [](const std::string& x) { int n = 0; for (size_t p = x.find("<bv_obb"); p != std::string::npos; p = x.find("<bv_obb", p + 1)) n++; return n; };
    auto read = [](const std::string& x, int n, float* o, float* l) {
        size_t p = 0;
        for (int i = 0; i <= n; i++) { p = x.find("<bv_obb", p); if (i < n) p++; }
        const size_t a = x.find("origin=\"", p), b = x.find("lengths=\"", p);
        return a != std::string::npos && b != std::string::npos && sscanf(x.c_str() + a + 8, "%f, %f, %f", &o[0], &o[1], &o[2]) == 3 && sscanf(x.c_str() + b + 9, "%f, %f, %f", &l[0], &l[1], &l[2]) == 3;
    };
    float o[3], l[3];
    CHECK(nbox(out) == 3 && nbox(src) == 3, "T1.1 clip_volumes keeps the number of boxes (%d)", nbox(out));
    CHECK(read(out, 0, o, l) && std::fabs(o[2] - 5.f) < 1e-3f && std::fabs(l[2] - 5.f) < 1e-3f && std::fabs(o[1] - 2.f) < 1e-3f && std::fabs(l[0] - 3.f) < 1e-3f, "T1.1 the half-inside box shrinks exactly (z centre %.3f, half %.3f)", o[2], l[2]);
    CHECK(read(out, 1, o, l) && std::fabs(l[0] - 0.005f) < 1e-4f && std::fabs(l[1] - 0.005f) < 1e-4f && std::fabs(l[2] - 0.005f) < 1e-4f && o[2] >= 0.f && o[2] <= 30.f, "T1.1 the box fully outside becomes 5 mm inside the rectangle (z %.3f)", o[2]);
    CHECK(read(out, 2, o, l) && l[0] < 3.f && l[2] < 3.f && l[0] > 0.1f, "T1.1 the turned box keeps its inside part (half lengths %.3f, %.3f of 3)", l[0], l[2]);
    // Nothing else changed: with the three boxes replaced by the source's own text, the strings match.
    std::string a = out, b = src;
    for (int n = 2; n >= 0; n--) {
        for (std::string* s : {&a, &b}) {
            size_t p = 0;
            for (int i = 0; i <= n; i++) { p = s->find("<bv_obb", p); if (i < n) p++; }
            s->replace(p, s->find("/>", p) - p, "<bv_obb");
        }
    }
    CHECK(a == b, "T1.1 nothing but the box attributes changed");
    // A box inside the rectangle is left byte for byte.
    const std::string inner = head + box(0, 2, 15, straight, 3, 2, 5) + tail;
    CHECK(crop::clip_volumes(inner, r) == inner, "T1.1 a box inside the rectangle is untouched");
}

// The lossless picture codec and the hash.
static void test_lossless() {
    auto same = [](const Image& a, const Image& b) { return a.w == b.w && a.h == b.h && a.px == b.px; };
    auto make = [](int w, int h, int kind) {
        Image im;
        im.w = w;
        im.h = h;
        im.px.resize(size_t(w) * h);
        uint32_t st = 12345u + uint32_t(kind);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                st = st * 1664525u + 1013904223u;
                uint32_t v = kind == 0 ? st : kind == 1 ? (0xFF000000u | uint32_t(x * 255 / std::max(1, w - 1)) | uint32_t(y * 255 / std::max(1, h - 1)) << 8 | uint32_t((x + y) & 255) << 16) : 0xFF406040u;
                im.px[size_t(y) * w + x] = v;
            }
        return im;
    };
    struct Case { int w, h, kind; const char* what; };
    const Case cases[] = {{256, 256, 0, "random 256x256 RGBA"}, {256, 256, 1, "gradient"}, {256, 256, 2, "flat colour"}, {1, 1, 0, "1x1"}, {3, 5, 0, "3x5"}, {37, 11, 1, "odd gradient"}};
    for (const Case& c : cases) {
        Image im = make(c.w, c.h, c.kind), back;
        Bytes packed = pack_image(im);
        CHECK(unpack_image(packed, back) && same(im, back), "T1.1 pack/unpack %s", c.what);
        if (c.kind == 2) CHECK(packed.size() < 2048, "T1.1 a flat 256x256 packs under 2 KB (%zu bytes)", packed.size());
        if (c.kind == 1) CHECK(packed.size() < im.px.size() * 4, "T1.1 a gradient packs smaller than raw (%zu of %zu)", packed.size(), im.px.size() * 4);
        Bytes st = colormap_store(im, 0x1122334455667788ull);
        Image r2;
        CHECK(colormap_restore(st, 0x1122334455667788ull, r2) && same(im, r2), "T1.1 colormap store/restore %s", c.what);
        Image r3;
        CHECK(!colormap_restore(st, 0x1122334455667789ull, r3) && r3.px.empty(), "T1.1 another hash restores nothing (%s)", c.what);
        if (st.size() > 40) {
            Bytes bad = st;
            bad[bad.size() / 2] ^= 0x5A;
            Image r4;
            (void)colormap_restore(bad, 0x1122334455667788ull, r4);  // a damaged stream may fail or decode to another image, never crash
            CHECK(true, "T1.1 a damaged blob does not crash (%s)", c.what);
            Bytes cut(st.begin(), st.end() - 5);
            CHECK(!colormap_restore(cut, 0x1122334455667788ull, r4), "T1.1 a truncated blob is refused (%s)", c.what);
        }
    }
    const uint8_t a = 'a';
    CHECK(fnv1a64(nullptr, 0) == 0xcbf29ce484222325ull, "T1.2 FNV-1a 64 of the empty string");
    CHECK(fnv1a64(&a, 1) == 0xaf63dc4c8601ec8cull, "T1.2 FNV-1a 64 of \"a\"");
}


// F4 (T2.1-T2.5): sculpting by the stroke.
static void test_brush() {
    using namespace ma2;
    // T2.1 touching a point once or a hundred times gives the same height.
    {
        StrokeCap a, b;
        a.reset(1);
        b.reset(1);
        float ha = 0, hb = 0;
        ha = stroke_height(10.f, 1.f, 2.f, a.touch(0, 0.7f));
        for (int i = 0; i < 100; i++) hb = stroke_height(10.f, 1.f, 2.f, b.touch(0, 0.7f));
        CHECK(ha == hb && std::fabs(ha - 11.4f) < 1e-5f, "T2.1 stroke_height does not build up (%.3f / %.3f)", ha, hb);
        CHECK(b.touch(0, 0.2f) == 0.7f && b.touch(0, 0.9f) == 0.9f, "T2.1 the cap keeps the biggest weight");
    }
    // T2.2 a 160 m straight stroke fed with 4 and with 40 cursor samples: the same ground within 0.5 m.
    {
        const int N = 33;
        const float scale = 8.f, R = 20.f, amount = 2.f;
        auto run = [&](int samples) {
            std::vector<float> h0(size_t(N) * N, 10.f), h = h0;
            StrokeCap cap;
            cap.reset(h.size());
            auto dab = [&](float cx, float cz) {
                for (int gz = 0; gz < N; gz++)
                    for (int gx = 0; gx < N; gx++) {
                        const float w = sculpt_falloff(gx * scale - cx, gz * scale - cz, R, 0.f, false);
                        if (w <= 0) continue;
                        const size_t i = size_t(gz) * N + gx;
                        h[i] = stroke_height(h0[i], 1.f, amount, cap.touch(i, w));
                    }
            };
            const float x0 = 48.f, x1 = 208.f, z = 128.f;
            dab(x0, z);
            float px = x0;
            for (int s = 1; s <= samples; s++) {
                const float nx = x0 + (x1 - x0) * float(s) / float(samples);
                const int n = substeps(px, z, nx, z, R);
                for (int k = 1; k <= n; k++) dab(px + (nx - px) * float(k) / float(n), z);
                px = nx;
            }
            return h;
        };
        const std::vector<float> a = run(4), b = run(40);
        float worst = 0, top = 0;
        for (size_t i = 0; i < a.size(); i++) { worst = std::max(worst, std::fabs(a[i] - b[i])); top = std::max(top, a[i]); }
        CHECK(worst <= 0.5f, "T2.2 4 and 40 samples agree within 0.5 m (worst %.3f)", worst);
        CHECK(top <= 12.0001f && top > 11.9f, "T2.2 the bank never exceeds the amount (%.3f)", top);
        CHECK(substeps(0, 0, 0, 0, 20) == 1 && substeps(0, 0, 100, 0, 20) == 40 && substeps(0, 0, 4, 0, 2) == 8, "T2.2 substeps (%d %d %d)", substeps(0, 0, 0, 0, 20), substeps(0, 0, 100, 0, 20), substeps(0, 0, 4, 0, 2));
    }
    CHECK(min_brush_radius(8.f) == 12.f && min_brush_radius(4.f) == 6.f, "T2.3 smallest brush %.1f / %.1f", min_brush_radius(8.f), min_brush_radius(4.f));
    {
        float worst = 0;
        for (int i = 0; i < 50; i++) {
            const float d = float(i) * 0.8f, R = 40.f, f = 1.f - d / R;
            const float old = d >= R ? 0.f : f * f * (3 - 2 * f);
            worst = std::max(worst, std::fabs(brush_falloff(d, R, 0.f) - old));
            worst = std::max(worst, std::fabs(sculpt_falloff(d, 0.f, R, 0.f, false) - old));
        }
        CHECK(worst < 1e-6f, "T2.4 Edge soft is today's smoothstep (worst %.2g)", worst);
        CHECK(sculpt_falloff(30.f, 0.f, 40.f, 0.9f, false) > sculpt_falloff(30.f, 0.f, 40.f, 0.f, false), "T2.4 a harder edge lifts further out");
    }
    CHECK(cap_amount(2.f, true, 1.5f) == 5.f && cap_amount(2.f, false, 9.f) == 2.f, "T2.5 cap_amount");
    // The slope helpers the cursor readout uses (builds on them).
    {
        Terrain t;
        t.resize_flat(5, 5, 10.f);
        CHECK(cell_slope(t, 1, 1) == 0.f && slope_at(t, 12, 12) == 0.f, "T2.6 flat ground has no slope");
        for (int gz = 0; gz < 5; gz++)
            for (int gx = 0; gx < 5; gx++) t.ref(gx, gz) = t.stored(10.f + 4.f * float(gx));
        CHECK(std::fabs(cell_slope(t, 1, 1) - 0.5f) < 0.03f && std::fabs(slope_at(t, 12, 12) - 0.5f) < 0.03f, "T2.6 a planar 4 m per cell ramp is 0.5 (%.3f)", cell_slope(t, 1, 1));
    }
}

// Painting by the stroke (paint_value, the layer rub-out).
static void test_paint() {
    using namespace ma2;
    // T4.1 a full-strength, full-weight stroke reaches the target; half strength is half way; the same weight gives the same value (no build-up).
    CHECK(paint_value(0, 255.f, 1.f, 1.f) == 255, "T4.1 full strength reaches 255 (%d)", int(paint_value(0, 255.f, 1.f, 1.f)));
    CHECK(paint_value(255, 0.f, 1.f, 1.f) == 0, "T4.1 full strength reaches 0");
    {
        const int h = paint_value(0, 255.f, 0.5f, 1.f);
        CHECK(h >= 127 && h <= 128, "T4.1 half strength is half way (%d)", h);
        const int p = paint_value(200, 0.f, 1.f, 0.3f);
        CHECK(p >= 139 && p <= 141, "T4.1 weight 0.3 from 200 toward 0 gives about 140 (%d)", p);
        CHECK(paint_value(200, 0.f, 1.f, 0.3f) == p, "T4.1 the same weight twice gives the same value");
        CHECK(paint_value(77, 255.f, 0.7f, 0.f) == 77, "T4.1 no weight leaves the value alone");
        StrokeCap cap;
        cap.reset(1);
        int v1 = 0, v2 = 0;
        for (int k = 0; k < 100; k++) v1 = paint_value(10, 255.f, 0.5f, cap.touch(0, 0.6f));   // held for 100 frames
        v2 = paint_value(10, 255.f, 0.5f, 0.6f);
        CHECK(v1 == v2, "T4.1 holding still adds nothing (%d vs %d)", v1, v2);
    }
    // T4.2 rubbing a layer out gives its weight to the others in proportion; alone, to texture 1 (or 2); the sum stays 255.
    auto sum = [](const uint8_t* w) { int s = 0; for (int i = 0; i < kGroundMaxLayers; i++) s += w[i]; return s; };
    {
        uint8_t w[kGroundMaxLayers] = {128, 127, 0, 0, 0, 0, 0, 0};
        rub_out(w, 0, 1.f);
        CHECK(w[0] == 0 && w[1] == 255 && sum(w) == 255, "T4.2 rubbing layer 0 of (128,127,0) gives (0,255,0): %d %d %d", w[0], w[1], w[2]);
    }
    {
        uint8_t w[kGroundMaxLayers] = {64, 64, 127, 0, 0, 0, 0, 0};
        rub_out(w, 2, 1.f);
        CHECK(std::abs(w[0] - 128) <= 1 && std::abs(w[1] - 127) <= 1 && w[2] == 0 && sum(w) == 255, "T4.2 rubbing layer 2 of (64,64,127) gives (128,127,0): %d %d %d", w[0], w[1], w[2]);
    }
    {
        uint8_t w[kGroundMaxLayers] = {0, 255, 0, 0, 0, 0, 0, 0};
        rub_out(w, 1, 1.f);
        CHECK(w[0] == 255 && w[1] == 0 && sum(w) == 255, "T4.2 rubbing the only layer 1 gives all to texture 1 (%d %d)", w[0], w[1]);
        uint8_t v[kGroundMaxLayers] = {255, 0, 0, 0, 0, 0, 0, 0};
        rub_out(v, 0, 1.f);
        CHECK(v[0] == 0 && v[1] == 255 && sum(v) == 255, "T4.2 rubbing the only layer 0 gives all to texture 2 (%d %d)", v[0], v[1]);
        uint8_t h[kGroundMaxLayers] = {100, 100, 55, 0, 0, 0, 0, 0};
        rub_out(h, 2, 0.5f);
        CHECK(sum(h) == 255 && h[2] >= 27 && h[2] <= 28 && std::abs(h[0] - h[1]) <= 1, "T4.2 half rub keeps the sum and shares in proportion: %d %d %d", h[0], h[1], h[2]);
    }
    // The paint step on a texel: the chosen layer rises, the others fall in proportion, from the stroke's start weights.
    {
        const uint8_t base[kGroundMaxLayers] = {255, 0, 0, 0, 0, 0, 0, 0};
        uint8_t out[kGroundMaxLayers];
        paint_layer_texel(base, out, 3, 1.f);
        CHECK(out[3] == 255 && out[0] == 0, "T4 paint step at full weight puts the layer at 255");
        paint_layer_texel(base, out, 3, 0.5f);
        CHECK(out[3] >= 127 && out[3] <= 128 && out[0] >= 127 && out[0] <= 128, "T4 paint step at half weight mixes the two (%d, %d)", out[3], out[0]);
    }
}

// The Ramp tool's target surface and the steep-ground classes.
static void test_slopes2() {
    using namespace ma2;
    {
        Terrain t;
        t.resize_flat(33, 33, 10.f);
        float worst = 0;
        for (int cz = 0; cz < 32; cz++)
            for (int cx = 0; cx < 32; cx++) worst = std::max(worst, cell_slope(t, cx, cz));
        CHECK(worst == 0.f, "T3.1 flat ground: every cell slope 0 (%.3f)", worst);
        CHECK(kSteepBot < kSteepLight && kSteepBot > 0.f, "T3.1 the two steep limits are ordered");
        CHECK(std::fabs(kSteepBot - 0.55f) < 1e-6f && std::fabs(kSteepLight - 0.80f) < 1e-6f, "T3.1 the bot planner's limits are 55%% ('Mechs, tanks) / 80%% (Battle Armor, pilots) (%.2f / %.2f)", double(kSteepBot), double(kSteepLight));
        CHECK(steep_tier(0.54f) == 0 && steep_tier(0.56f) == 1 && steep_tier(0.79f) == 1 && steep_tier(0.81f) == 2 && steep_tier(1.7f) == 2 && steep_tier(8.f) == 2, "T3.1 the tiers split at 55 / 80%% only (no player-limit tier, no wall tier)");
        CHECK(std::string(steep_tag(0.3f)).empty() && std::string(steep_tag(0.7f)).find("bots") != std::string::npos && std::string(steep_tag(1.6f)).find("bots won't route") != std::string::npos && std::string(steep_tag(1.6f)).find("some 'Mechs may still climb it") != std::string::npos, "T3.1 the readout names the bots and makes no player claim");
        {   // No steep text claims what the player can or cannot climb (the reach rerun: the player's Atlas climbed 169% and 825%).
            bool ok = true;
            for (float s : {0.3f, 0.7f, 1.0f, 1.6f, 1.9f, 8.f}) {
                const std::string g = steep_tag(s);
                if (g.find("player") != std::string::npos || g.find("wall") != std::string::npos) ok = false;
            }
            CHECK(ok, "the cursor readout text names no player limit and no wall");
        }
    }
    {
        float out = 0;
        const float ax = 40, az = 128, bx = 200, bz = 128, ha = 10, hb = 50, W = 16, blend = 12;
        const float ts[3] = {0.25f, 0.5f, 0.75f}, want[3] = {20, 30, 40};
        for (int k = 0; k < 3; k++) {
            const float x = ax + (bx - ax) * ts[k];
            const bool on = ramp_target(ax, az, bx, bz, ha, hb, W, blend, x, az, 10.f, out);
            CHECK(on && std::fabs(out - want[k]) < 1e-3f, "T3.2 on the axis at t=%.2f: %.3f (want %.0f)", double(ts[k]), double(out), double(want[k]));
            const bool off = ramp_target(ax, az, bx, bz, ha, hb, W, blend, x, az + 4.f, 10.f, out);
            CHECK(off && std::fabs(out - want[k]) < 1e-3f, "T3.2 4 m off the axis at t=%.2f: %.3f", double(ts[k]), double(out));
        }
        CHECK(!ramp_target(ax, az, bx, bz, ha, hb, W, blend, 120.f, az + 30.f, 10.f, out) && out == 10.f, "T3.2 30 m off the axis keeps the old ground (%.3f)", double(out));
        CHECK(ramp_target(ax, az, bx, bz, ha, hb, W, blend, 120.f, az + 8.f + 6.f, 10.f, out) && out > 10.f && out < 30.f, "T3.2 the shoulder blends (%.3f)", double(out));
        CHECK(!ramp_target(ax, az, bx, bz, ha, hb, W, blend, ax - 12.f, az, 10.f, out), "T3.2 past the end by the blend: nothing");
        // T3.3 level across, for 20 values of t, at -6 / 0 / +6 m.
        float worst = 0;
        for (int i = 0; i < 20; i++) {
            const float x = ax + (bx - ax) * (float(i) + 0.5f) / 20.f;
            float a, b, c;
            ramp_target(ax, az, bx, bz, ha, hb, W, blend, x, az - 6.f, 0.f, a);
            ramp_target(ax, az, bx, bz, ha, hb, W, blend, x, az, 0.f, b);
            ramp_target(ax, az, bx, bz, ha, hb, W, blend, x, az + 6.f, 0.f, c);
            worst = std::max(worst, std::max(std::fabs(a - b), std::fabs(c - b)));
        }
        CHECK(worst < 1e-4f, "T3.3 the cross-section is level (worst %.2g)", double(worst));
        // A diagonal ramp is level across too.
        ramp_target(10, 10, 90, 90, 5, 25, 16, 12, 50.f - 5.f, 50.f + 5.f, 0.f, out);
        float out2;
        ramp_target(10, 10, 90, 90, 5, 25, 16, 12, 50.f, 50.f, 0.f, out2);
        CHECK(std::fabs(out - out2) < 1e-3f, "T3.3 a diagonal ramp is level across (%.3f / %.3f)", double(out), double(out2));
    }
}

// The Help text. One tokenised text feeds the Help window, Getting Started and docs\Scenario editor.txt.
static void test_help() {
    using namespace ed;
    const KeyText def = KeyText::defaults();
    const auto& raw = help_raw();
    CHECK(raw.size() >= 19, "W2 help has at least the 19 topics of the spec (%d)", int(raw.size()));
    // Every {token} resolves, for the defaults and for rebound keys.
    KeyText re = def;
    re.keys[A_SNAP][0] = {ik::K("B"), 0};
    re.keys[A_TOP_DOWN][0] = {ik::K("V"), 0};
    re.keys[A_BRUSH_BIGGER][0] = {ik::K("Period"), 0};
    re.hold.free = KM_ALT;
    re.hold.through = KM_SHIFT;
    re.hold.quarter = KM_CTRL;
    re.hold.reverse = KM_CTRL;
    re.hold.orbit = KM_SHIFT;
    std::vector<std::string> bad;
    int tokens = 0;
    for (const HelpTopic& h : raw) {
        help_expand(h.body, def, &bad);
        help_expand(h.body, re, &bad);
        for (const char* p = h.body; (p = strchr(p, '{')); p++) tokens++;
    }
    CHECK(bad.empty(), "W2 every {token} of the help text resolves (%d unresolved, first: %s)", int(bad.size()), bad.empty() ? "" : bad[0].c_str());
    CHECK(tokens > 80, "W2 the help text uses its tokens (%d)", tokens);
    // The raw text names no literal key: keys come from tokens. Mouse gestures with a fixed modifier are allowed.
    {
        const std::regex tok("\\{[^}]*\\}");
        const std::regex lit("\\b(Ctrl|Shift|Alt)\\+(?!click|wheel|double-click|drag|box)\\w|\\bF\\d+\\b|\\([A-Z]\\)|\\bEsc\\b|\\bDel\\b|PgUp|PgDn|\\[ / \\]|\\bEnter\\b");
        int hits = 0;
        std::string first;
        for (const HelpTopic& h : raw) {
            const std::string t = std::regex_replace(std::string(h.body), tok, "");
            std::smatch m;
            if (std::regex_search(t, m, lit)) { hits++; if (first.empty()) first = std::string(h.title) + ": " + m.str(); }
        }
        CHECK(hits == 0, "W2 no literal key names in the raw help text (%d topics, first: %s)", hits, first.c_str());
    }
    // Words the spec retired.
    {
        std::string all;
        for (const HelpPage& p : help_topics(def)) all += p.title + "\n" + p.body + "\n";
        for (const char* w : {"Units and Props", "Terrain tab", "Players tab", "top right", "retail", "Snap to ground", "Frame the selection"})
            CHECK(all.find(w) == std::string::npos, "W2 the help text does not say \"%s\"", w);
        // The steep-ground text claims nothing about what the player can climb (the reach rerun disproved the old limits), and has no red wall tier.
        for (const char* w : {"can't climb", "cannot climb", "climbs up to", "player can still", "player still can", "red, over", "a wall."})
            CHECK(all.find(w) == std::string::npos, "the help text does not say \"%s\"", w);
        CHECK(all.find("some 'Mechs may still climb it") != std::string::npos, "the orange tier says some 'Mechs may still climb it");
        CHECK(all.find("New map...") != std::string::npos && all.find("New mission...") != std::string::npos, "W2 the help text names New map... and New mission...");
        CHECK(all.find("arrow opens the test settings") != std::string::npos, "W2 Test is described in the toolbar with its arrow");
    }
    // Topic "The screen" names every tab and tool, in the strip's order.
    {
        std::string screen;
        for (const HelpPage& p : help_topics(def))
            if (p.title == "The screen") screen = p.body;
        size_t at = 0;
        bool ordered = true;
        for (const char* tab : kHelpTabs) {
            const size_t f = screen.find(std::string("- ") + tab + ":", at);
            if (f == std::string::npos) { ordered = false; break; }
            at = f;
        }
        CHECK(ordered, "W2 \"The screen\" lists the five tabs in the strip's order");
        for (int i = 0; i < kPageCount; i++)
            CHECK(screen.find(page_name(Page(i))) != std::string::npos, "W2 \"The screen\" names the tool %s", page_name(Page(i)));
    }
    // Rebinding changes the text: B, V, "." and the new hold keys show; the old keys do not stand for those actions.
    {
        std::string a, b;
        for (const HelpPage& p : help_topics(re)) a += p.title + "\n" + p.body + "\n";
        for (const HelpPage& p : help_topics(def)) b += p.title + "\n" + p.body + "\n";
        CHECK(a.find("B: Put on the ground") != std::string::npos && b.find("G: Put on the ground") != std::string::npos, "W2 a rebound Put on the ground shows its new key (B, was G)");
        CHECK(a.find("Top-down view: V") != std::string::npos && a.find("Top-down view: T") == std::string::npos, "W2 a rebound top-down key shows V, not T");
        CHECK(a.find("or . to") != std::string::npos || a.find(", or .") != std::string::npos || a.find("[ and .") != std::string::npos, "W2 a rebound brush key shows a period");
        CHECK(a.find("Hold Alt to place freely") != std::string::npos && b.find("Hold Shift to place freely") != std::string::npos, "W2 the Place freely hold key follows Settings (Alt vs Shift)");
        CHECK(a.find("or hold Shift and drag with the left button") != std::string::npos && b.find("or hold Alt and drag with the left button") != std::string::npos, "W2 the orbit hold key follows Settings");
        CHECK(a.find("(G)") == std::string::npos && a.find(" T ") == std::string::npos, "W2 no stale (G) or ' T ' after rebinding");
    }
    // The steep limits are read from core\brush.h.
    {
        std::string t8;
        for (const HelpPage& p : help_topics(def))
            if (p.title == "Shaping the ground") t8 = p.body;
        CHECK(t8.find("over 55%") != std::string::npos && t8.find("up to 80%") != std::string::npos && t8.find("orange, over 80%") != std::string::npos && t8.find("over 150%") == std::string::npos && t8.find("over 170%") == std::string::npos,
              "W2 the steep-ground tiers read 55 / 80%% from the constants (no 150 / 170%% tier)");
    }
    // Find in Help.
    {
        const std::vector<HelpPage> pages = help_topics(def);
        std::vector<std::string> hit;
        for (const HelpPage& p : pages)
            if (help_matches(p, "scatter")) hit.push_back(p.title);
        bool has_scatter = false, has_undo = false;
        for (auto& t : hit) { has_scatter |= t == "Scatter"; has_undo |= t == "Undo"; }
        CHECK(has_scatter && !has_undo && hit.size() >= 2 && hit.size() < pages.size() / 2, "W2 \"Find in Help\" scatter leaves the Scatter topic and those that mention it (%d of %d)", int(hit.size()), int(pages.size()));
        CHECK(help_matches(pages[0], "") && help_matches(pages[0], "NEW MAP") && !help_matches(pages[0], "zzzqq"), "W2 Find in Help ignores case and an empty box shows all");
    }
    // The guide: made from the topics, 76 columns, and equal to docs\Scenario editor.txt (regenerate = copy out\ over docs\).
    {
        const std::string guide = help_guide(def);
        size_t worst = 0, line = 0, start = 0;
        for (size_t i = 0; i <= guide.size(); i++)
            if (i == guide.size() || guide[i] == '\n') {
                size_t len = i - start;
                if (len && guide[i - 1] == '\r') len--;
                if (len > worst) worst = len, line = start;
                start = i + 1;
            }
        CHECK(worst <= 76, "W2 the guide fits 76 columns (longest %d at %d)", int(worst), int(line));
        CHECK(guide.find("KEYS (defaults)") != std::string::npos && guide.find("{{VERSION}}") != std::string::npos && guide.find("{{MA2_SUPPORT_URL}}") != std::string::npos, "W2 the guide keeps the kit's version and support placeholders and ends with the default keys");
        std::string rest = guide;
        for (const char* ph : {"{{VERSION}}", "{{MA2_SUPPORT_URL}}"}) { size_t f = rest.find(ph); if (f != std::string::npos) rest.erase(f, strlen(ph)); }
        CHECK(rest.find('{') == std::string::npos, "W2 no unresolved token in the guide");
        fs::path root = fs::current_path();
        if (root.filename() == "out") root = root.parent_path();
        std::error_code ec;
        fs::create_directories(root / "out", ec);
        if (FILE* f = fopen((root / "out" / "Scenario editor.txt").string().c_str(), "wb")) { fwrite(guide.data(), 1, guide.size(), f); fclose(f); }
        std::string on_disk;
        if (FILE* f = fopen((root / "docs" / "Scenario editor.txt").string().c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) on_disk.append(buf, n);
            fclose(f);
        }
        CHECK(on_disk == guide, "W2 docs\\Scenario editor.txt equals the guide made from the help text (copy out\\Scenario editor.txt over it); %d vs %d bytes", int(on_disk.size()), int(guide.size()));
    }
}

int wmain(int argc, wchar_t** argv) {
    fs::path install, samples, bisect, mvariant, mtb_sweep;
    std::string dump_model;  // --dump-model <substring>: print the MGMODEL XML of the first model whose path contains it (developer)
    std::string dump_nodes;  // --dump-nodes <substring>: node tree + .mtb state of every model whose path contains it (developer)
    bool quick = false, all_maps = false;
    setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps the lines printed before it
    test_tool_hints();
    test_pages();
    test_pack_rows();
    test_palette_rank();
    test_input_map();
    test_help();
    test_snap_prefs();
    test_lossless();
    test_brush();
    test_slopes2();
    test_paint();
    test_scatter();
    test_undo_rules();
    test_select_rules();
    test_snap();
    test_snap_cross();
    test_ground();
    test_sheets();
    test_surface();
    test_grid_under();
    test_slopes();
    test_basins();
    test_clip_volumes();
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc) install = argv[++i];
        else if (a == L"--samples" && i + 1 < argc) samples = argv[++i];
        else if (a == L"--quick") quick = true;
        else if (a == L"--all-maps") all_maps = true;
        else if (a == L"--bisect" && i + 1 < argc) bisect = argv[++i];
        else if (a == L"--mission-variant" && i + 1 < argc) mvariant = argv[++i];
        else if (a == L"--mtb-sweep" && i + 1 < argc) mtb_sweep = argv[++i];
        else if (a == L"--dump-model" && i + 1 < argc) dump_model = path_utf8(fs::path(argv[++i]));
        else if (a == L"--dump-nodes" && i + 1 < argc) dump_nodes = path_utf8(fs::path(argv[++i]));
    }
    if (install.empty()) {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        install = GameData::find_install(fs::path(buf).parent_path());
    }
    printf("install: %s\n", path_utf8(install).c_str());
    GameData gd;
    std::string err;
    if (!gd.open(install, &err)) { printf("cannot open game data: %s\n", err.c_str()); return 2; }
    printf("names: %zu keys; level archives: %zu; maps: %zu\n", gd.names().size(), gd.level_archives().size(), gd.maps().size());
    if (!mtb_sweep.empty()) return run_mtb_sweep(gd, mtb_sweep);
    if (!dump_model.empty()) {
        ma2m::ArchiveSet all;
        for (auto& e : fs::directory_iterator(gd.game_files()))
            if (e.path().extension() == L".mgf") all.Add(path_utf8(e.path()));
        for (auto& f : all.ListAll(dump_model)) {
            std::vector<uint8_t> b;
            if (!all.Read(f, b)) continue;
            printf("=== %s (%zu bytes)\n%.*s\n", f.c_str(), b.size(), int(std::min<size_t>(b.size(), 60000)), reinterpret_cast<const char*>(b.data()));
        }
        return 0;
    }
    if (!dump_nodes.empty()) {  // developer: node tree of every model whose path has this text, with the .mtb state
        ma2m::ArchiveSet all;
        all.Add(path_utf8(gd.game_files() / L"common.mgf"));
        for (auto& e : fs::directory_iterator(gd.game_files()))
            if (e.path().extension() == L".mgf" && !iequals(path_utf8(e.path().filename()), "common.mgf")) all.Add(path_utf8(e.path()));
        std::set<std::string> done;
        for (auto& f : all.ListAll(dump_nodes)) {
            if (lower(f).find(".mgmodel") == std::string::npos || f.find('{') != std::string::npos || !done.insert(lower(f)).second) continue;
            ma2m::Model m;
            ma2m::ModelLoadOptions mo;
            mo.include_invisible = true;
            std::string merr;
            if (!ma2m::LoadModel(all, f, m, mo, &merr)) { printf("=== %s: %s\n", f.c_str(), merr.c_str()); continue; }
            printf("=== %s\n", f.c_str());
            for (size_t i = 0; i < m.nodes.size(); i++) {
                const auto& n = m.nodes[i];
                printf("  node %zu %s %s parent %d on %d mtbhide %d lod %d light %d\n", i, n.name.c_str(), n.type.c_str(), n.parent, int(n.turned_on), int(n.hidden_by_mtb), n.lod, int(n.is_light));
            }
            for (const auto& sm : m.meshes) {
                const ma2m::Material* mt = sm.material >= 0 && sm.material < int(m.materials.size()) ? &m.materials[size_t(sm.material)] : nullptr;
                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};  // Model-space bounds (node world applied)
                for (size_t v = 0; v + 2 < sm.positions.size(); v += 3)
                    for (int k = 0; k < 3; k++) {
                        float w = sm.transform[k] * sm.positions[v] + sm.transform[4 + k] * sm.positions[v + 1] + sm.transform[8 + k] * sm.positions[v + 2] + sm.transform[12 + k];
                        lo[k] = std::min(lo[k], w); hi[k] = std::max(hi[k], w);
                    }
                printf("  mesh %s node %d visible %d alpha %d selfillum %.2f tex %s bounds %.3f %.3f %.3f .. %.3f %.3f %.3f tris %zu\n", sm.name.c_str(), sm.node_index, int(sm.visible), mt ? int(mt->alpha) : -1, mt ? std::max({mt->selfillum[0], mt->selfillum[1], mt->selfillum[2]}) : 0.f, mt ? mt->diffuse_texture.c_str() : "-", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], sm.indices.size() / 3);
            }
        }
        return 0;
    }

    // ---------------------------------------------------------------- the game's own text (text.mgf mgText)
    CHECK(gd.game_text(524787) == "To the Rescue", "game_text(524787) = Mission_05 title ('%s')", gd.game_text(524787).c_str());
    CHECK(gd.game_text(524370) == "Press A to open the gate.", "game_text(524370) = the M05 gate prompt ('%s')", gd.game_text(524370).c_str());
    CHECK(gd.game_text(0xF00001).empty(), "a scenario-owned id has no game text");

    // ---------------------------------------------------------------- P1 palette <-> layers (no game data)
    {
        Image cm;
        cm.w = 8; cm.h = 4;
        cm.px.resize(32);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++) cm.at(x, y) = 0x00808080u | (x < 4 ? 0xFF000000u : 0u);  // west half First, east half Second
        GroundLayers g;
        g.enabled = true;
        g.layers.resize(2);
        g.init_from_colormap(cm);
        CHECK(g.w == 32 && g.h == 16 && g.map_w == 8 && g.map_h == 4, "layers weights are 4x the colour map (%dx%d)", g.w, g.h);
        CHECK(g.texel(0, 0)[0] == 255 && g.texel(0, 0)[1] == 0 && g.texel(31, 0)[0] == 0 && g.texel(31, 0)[1] == 255, "start = the retail alpha mix");
        // The third texture: paint a disc in the middle of a 100 x 50 m map; the rest keeps the pair.
        g.layers.resize(3);
        for (int i = 0; i < 40; i++) g.paint(2, 50.f, 25.f, 10.f, 1.f, 100.f, 50.f);
        const uint8_t* mid = g.texel(16, 8);
        CHECK(mid[2] == 255 && mid[0] == 0 && mid[1] == 0, "brush centre = layer 2 (%d %d %d)", mid[0], mid[1], mid[2]);
        CHECK(g.texel(2, 2)[0] == 255 && g.texel(2, 2)[2] == 0, "painting elsewhere stays (west still layer 0)");
        // Layers persist: painting layer 1 over part of the disc edge keeps layer 2 where the brush did not reach.
        for (int i = 0; i < 40; i++) g.paint(1, 50.f + 12.f, 25.f, 6.f, 1.f, 100.f, 50.f);
        CHECK(g.texel(16, 8)[2] == 255, "layer 2 at the centre survives a later stroke");
        // Round trip of the three-layer palette through the file format.
        g.layers[2].image = resample_square(Image(), 8);
        g.layers[2].path = "terrain/x.tif"; g.layers[2].archive = "a.mgf";
        Bytes enc = encode_ground_layers(g);
        GroundLayers dec;
        std::string perr;
        CHECK(decode_ground_layers(enc, dec, &perr) && dec.count() == 3 && *dec.weights == *g.weights, "3-texture palette encode/decode %s", perr.c_str());
        CHECK(brush_falloff(0.f, 10.f, 0.f) == 1.f && brush_falloff(10.f, 10.f, 0.5f) == 0.f && brush_falloff(5.f, 10.f, 0.5f) == 1.f && brush_falloff(5.f, 10.f, 0.f) < 0.6f,
              "brush hardness widens the full-strength core");
        // Fill.
        GroundLayers f = g;
        f.fill(2);
        bool all2 = true;
        for (int y = 0; y < f.h; y++)
            for (int x = 0; x < f.w; x++) all2 = all2 && f.texel(x, y)[2] == 255 && f.texel(x, y)[0] == 0 && f.texel(x, y)[1] == 0;
        CHECK(all2 && g.texel(2, 2)[0] == 255, "fill whole map with layer 2 (copy on write left the original alone)");
        // Remove layer 2 (a fourth shifts down), then bake back to the retail alpha.
        g.layers.resize(4);
        for (int i = 0; i < 40; i++) g.paint(3, 10.f, 25.f, 6.f, 1.f, 100.f, 50.f);
        CHECK(g.texel(3, 8)[3] == 255, "layer 3 painted in the west");
        g.remove_layer(2);
        CHECK(g.count() == 3 && g.texel(3, 8)[2] == 255 && g.texel(16, 8)[2] == 0, "removing layer 2 moves layer 3 down and clears the old disc");
        g.remove_layer(2);
        CHECK(g.count() == 2, "back to the pair");
        Image back = cm;
        for (auto& p : back.px) p = (p & 0x00FFFFFFu) | 0x7F000000u;  // a stale alpha: only the texels the pair covers are rewritten
        g.bake_colormap_alpha(back);
        CHECK((back.at(0, 0) >> 24) == 255 && (back.at(7, 0) >> 24) == 0, "bake back to the colour-map alpha (west %u, east %u)", back.at(0, 0) >> 24, back.at(7, 0) >> 24);
        // A map filled with a removed layer has no pair weights anywhere: the alpha in the colour map stays.
        GroundLayers z = dec;
        z.fill(2);
        z.remove_layer(2);
        Image keep = cm;
        for (auto& p : keep.px) p = (p & 0x00FFFFFFu) | 0x7F000000u;
        z.bake_colormap_alpha(keep);
        bool same = true;
        for (auto& p : keep.px) same = same && (p >> 24) == 0x7F;
        CHECK(same, "bake keeps the alpha where only removed layers were");
    }

    // ---------------------------------------------------------------- MGF + WDF round trip
    double t0 = now_s();
    int worlds = 0, world_ok = 0;
    size_t world_bytes = 0, keys_total = 0, keys_named = 0;
    std::vector<std::pair<std::string, Bytes>> heightfields, infoblobs;
    for (auto& f : gd.common().files()) {
        if (!iends_with(f.path, ".wdf")) continue;
        Bytes b;
        bool rd = gd.common().read_file(f.path, b, &err);
        CHECK(rd, "read %s: %s", f.path.c_str(), err.c_str());
        if (!rd) continue;
        CHECK(crc_acc(b.data(), b.size()) == f.rec[1], "record crc %s", f.path.c_str());
        Wdf w;
        bool ok = w.parse(b, &err);
        CHECK(ok, "parse %s: %s", f.path.c_str(), err.c_str());
        worlds++;
        if (!ok) continue;
        Bytes s = w.serialize();
        bool same = s == b;
        CHECK(same, "round trip %s (%zu vs %zu bytes)", f.path.c_str(), s.size(), b.size());
        if (same) world_ok++;
        world_bytes += b.size();
        for (auto& o : w.objs) {
            keys_total++; if (gd.names().name(o.cls)) keys_named++;
            for (auto& p : o.props) { keys_total++; if (gd.names().name(p.key)) keys_named++; }
            if (o.cls == K_TERRAIN) if (auto* p = o.find("TerrainHeight Field")) heightfields.push_back({f.path, p->data});
            if (o.cls == K_TINFO) for (auto& p : o.props) if (p.type == PT_BLOB && p.data.size() > 0x40) infoblobs.push_back({f.path, p.data});
        }
    }
    printf("[wdf] common.mgf world files: %d/%d byte-identical (%.1f MB) in %.1fs; keys named %.2f%%\n", world_ok, worlds,
           world_bytes / 1048576.0, now_s() - t0, 100.0 * keys_named / std::max<size_t>(1, keys_total));

    int culled = 0, culled_ok = 0;
    if (MgfReader* tm = gd.templates_archive()) {
        for (auto& f : tm->files()) {
            if (!iends_with(f.path, ".wdf")) continue;
            Bytes b;
            if (!tm->read_file(f.path, b, &err)) { CHECK(false, "read %s", f.path.c_str()); continue; }
            Wdf w;
            culled++;
            if (w.parse(b, &err) && w.serialize() == b) culled_ok++;
            else CHECK(false, "culled round trip %s", f.path.c_str());
        }
    }
    printf("[wdf] Templates.mgf culled sets: %d/%d byte-identical\n", culled_ok, culled);

    // ---------------------------------------------------------------- MGIc in world files
    int hf_ok = 0;
    for (auto& [name, blob] : heightfields) {
        Mgic m;
        bool ok = m.parse(blob, &err) && m.serialize() == blob;
        CHECK(ok, "heightfield container round trip %s", name.c_str());
        Picture p;
        ok = ok && read_picture(m, p, &err);
        CHECK(ok, "heightfield picture %s: %s", name.c_str(), err.c_str());
        std::vector<uint8_t> s;
        ok = ok && picture_samples8(p, s);
        if (!ok) continue;
        Bytes rebuilt = build_heightfield(m, p.width, p.height, s);
        Picture q;
        std::vector<uint8_t> s2;
        bool same = read_picture(rebuilt, q, &err) && picture_samples8(q, s2) && s2 == s && rebuilt.size() == blob.size();
        CHECK(same, "heightfield re-encode %s", name.c_str());
        if (same) hf_ok++;
    }
    printf("[mgic] heightfields: %d/%zu container byte-identical and re-encoded losslessly (P8, 64-byte pitch)\n", hf_ok, heightfields.size());
    int ib_ok = 0;
    std::map<std::string, int> info_formats;
    for (auto& [name, blob] : infoblobs) {
        Mgic m;
        bool ok = m.parse(blob, &err) && m.serialize() == blob;
        CHECK(ok, "terrain-info blob round trip %s", name.c_str());
        Picture p;
        if (ok && read_picture(m, p, &err)) {
            info_formats[p.format_name() + " " + std::to_string(p.width) + "x" + std::to_string(p.height)]++;
            Picture q = p;
            Image img;
            if (decode_picture(p, 0, img, &err) && p.format() != PF_ARGB4444) {
                q.bits = encode_levels(p, img);
                CHECK(q.bits == p.bits, "terrain-info re-encode %s", name.c_str());
            }
            ib_ok++;
        }
    }
    printf("[mgic] terrain-info blobs: %d/%zu;", ib_ok, infoblobs.size());
    for (auto& [k, v] : info_formats) printf(" %s x%d;", k.c_str(), v);
    printf("\n");

    // ---------------------------------------------------------------- MGIc terrain textures in level archives
    std::map<std::string, int> tex_formats;
    int tex = 0, tex_ok = 0, tex_lossless = 0, tex_lossless_ok = 0;
    double worst_psnr = 1e9;
    std::string worst_name;
    t0 = now_s();
    for (auto& an : gd.level_archives()) {
        MgfReader* a = gd.archive(an);
        if (!a) { CHECK(false, "open %s", an.c_str()); continue; }
        for (auto& f : a->files()) {
            if (!istarts_with(f.path, "Terrain/") || !iends_with(f.path, ".tif")) continue;
            Bytes b;
            if (!a->read_file(f.path, b, &err)) { CHECK(false, "read %s/%s", an.c_str(), f.path.c_str()); continue; }
            Mgic m;
            tex++;
            bool ok = m.parse(b, &err) && m.serialize() == b;
            CHECK(ok, "texture container round trip %s/%s", an.c_str(), f.path.c_str());
            Picture p;
            if (!ok || !read_picture(m, p, &err)) { CHECK(false, "picture %s/%s: %s", an.c_str(), f.path.c_str(), err.c_str()); continue; }
            tex_formats[p.format_name()]++;
            Image img;
            if (!decode_picture(p, 0, img, &err)) { CHECK(false, "decode %s/%s: %s", an.c_str(), f.path.c_str(), err.c_str()); continue; }
            tex_ok++;
            Picture q = p;
            q.bits = encode_levels(p, img);
            CHECK(q.bits.size() == p.bits.size(), "encoded size %s/%s", an.c_str(), f.path.c_str());
            Image back;
            decode_picture(q, 0, back, &err);
            if (!p.dxt()) {
                tex_lossless++;
                bool same = std::equal(q.bits.begin(), q.bits.begin() + p.level_size(0), p.bits.begin());
                CHECK(same, "lossless re-encode of level 0 %s/%s", an.c_str(), f.path.c_str());
                if (same) tex_lossless_ok++;
            } else {
                // DXT: compare against the retail decode (the source is already block compressed).
                double se = 0;
                for (size_t i = 0; i < img.px.size(); i++)
                    for (int c = 0; c < 3; c++) {
                        double d = double((img.px[i] >> (8 * c)) & 255) - double((back.px[i] >> (8 * c)) & 255);
                        se += d * d;
                    }
                double mse = se / (img.px.size() * 3.0);
                double psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
                if (psnr < worst_psnr) { worst_psnr = psnr; worst_name = an + "/" + f.path; }
                CHECK(psnr > 30, "DXT re-encode quality %s/%s: %.1f dB", an.c_str(), f.path.c_str(), psnr);
            }
            // swizzle: decode the re-encoded picture and compare (all formats).
            if (!p.dxt()) CHECK(back.px == img.px, "decode(encode(x)) %s/%s", an.c_str(), f.path.c_str());
        }
        if (quick) break;
    }
    printf("[mgic] level-archive Terrain/*.tif: %d/%d decoded; lossless formats level 0 re-encoded identically %d/%d; worst DXT re-encode %.1f dB (%s); %.1fs\n",
           tex_ok, tex, tex_lossless_ok, tex_lossless, worst_psnr, worst_name.c_str(), now_s() - t0);
    for (auto& [k, v] : tex_formats) printf("        %-24s x%d\n", k.c_str(), v);

    // ---------------------------------------------------------------- MGF writer
    {
        MgfWriter w;
        Bytes a{1, 2, 3}, b(1000, 7), c;
        w.add("scenario.ini", a);
        w.add("Terrain\\Scn0001_ColorMap.tif", b);
        w.add("Terrain/Scn0001_Lightmap.tif", c);
        w.add("editor/project.json", a);
        w.add("Pathfinding/Scn0001_Mech.map", b);
        Bytes arc = w.build();
        fs::path tmp = fs::temp_directory_path() / L"ma2scn_fmt_test.mgf";
        write_file(tmp, arc);
        MgfReader r;
        bool ok = r.open(tmp, &err);
        CHECK(ok, "reopen written archive: %s", err.c_str());
        Bytes x;
        CHECK(ok && r.files().size() == 5, "written archive file count %zu", r.files().size());
        CHECK(ok && r.read_file("terrain/scn0001_colormap.tif", x) && x == b, "written archive content");
        CHECK(ok && r.read_file("SCENARIO.INI", x) && x == a, "written archive case-insensitive lookup");
        CHECK(ok && r.read_file("Terrain/Scn0001_Lightmap.tif", x) && x.empty(), "written archive empty file");
        for (auto& f : r.files()) {
            Bytes y;
            r.read_file(f.path, y);
            CHECK(crc_acc(y.data(), y.size()) == f.rec[1], "written crc %s", f.path.c_str());
        }
        r.close();
        fs::remove(tmp);
        printf("[mgf] writer: 5-file archive reopened, contents and CRCs match\n");
        // Folder-row hashes and file-record keys: key(lower("\dir\name")), checked on retail archives.
        auto check_keys = [&](MgfReader& a, const char* label) {
            size_t ok = 0, total = 0;
            auto full = [&](int row) {
                std::vector<std::string> parts;
                while (row > 0) { parts.push_back(a.row_names[size_t(row)]); row = a.rows[size_t(row)][1]; }
                std::string p;
                for (size_t k = parts.size(); k-- > 0;) p += "\\" + parts[k];
                return lower(p.empty() ? std::string("\\") : p);
            };
            for (size_t r = 0; r < a.rows.size(); r++) {
                total++;
                if (a.rows[r][5] < 0) { if (uint32_t(a.rows[r][0]) == key_of(full(int(r)))) ok++; }
                else if (a.files().size() && true) {
                    for (auto& f : a.files()) if (f.row == int(r)) { if (f.rec[2] == key_of(full(int(r)))) ok++; break; }
                }
            }
            CHECK(ok == total, "%s directory keys %zu/%zu", label, ok, total);
            printf("[mgf] %s: %zu/%zu folder hashes and file keys match key(lower path)\n", label, ok, total);
        };
        check_keys(gd.common(), "common.mgf");
        if (MgfReader* bd = gd.archive("boggeddown.mgf")) check_keys(*bd, "boggeddown.mgf");
        write_file(tmp, arc);
        if (r.open(tmp, &err)) {
            check_keys(r, "editor-written archive");
            CHECK(r.header[2] == 0x10 && r.header[1] == 0x5A5A0104u, "written header words");
            for (auto& f : r.files()) CHECK(f.rec[7] == 0x12F700u && f.rec[5] != 0, "written record fields %s", f.path.c_str());
            r.close();
        }
        fs::remove(tmp);
    }

    // ---------------------------------------------------------------- Templates catalog
    {
        t0 = now_s();
        TemplateCatalog cat;
        bool ok = cat.load(gd, &err);
        CHECK(ok, "template catalog: %s", err.c_str());
        if (ok) {
            printf("[templates] %zu templates, %zu culled sets, %zu reference keys, %zu with model files; %.1fs\n",
                   cat.count(), cat.culled_set_count(), cat.ref_keys().size(), cat.model_template_count(), now_s() - t0);
            // Rebuilding each retail culled file from its own UID set must give identical bytes.
            int rb = 0, rb_ok = 0;
            for (auto& level : cat.culled_levels()) {
                Bytes orig;
                if (!gd.templates_archive()->read_file("CulledTemplates/globalentities." + level + ".wdf", orig)) continue;
                rb++;
                Bytes built = cat.write_culled(cat.culled_set(level));
                if (built == orig) rb_ok++;
                else CHECK(false, "culled rebuild %s (%zu vs %zu)", level.c_str(), built.size(), orig.size());
            }
            printf("[templates] culled sets rebuilt from UID lists: %d/%d byte-identical\n", rb_ok, rb);
            // Closure check: Base culled set + closure(world) covers the retail set for every level.
            int cov = 0, cov_ok = 0;
            for (auto& m : gd.maps()) {
                std::string level = lower(m.name);
                if (!cat.has_culled(level)) continue;
                Wdf w;
                if (!gd.load_world(m, w)) continue;
                auto need = cat.closure_for_world(w, level, m.multiplayer);
                auto& want = cat.culled_set(level);
                size_t miss = 0;
                for (uint32_t u : want) if (!need.count(u)) miss++;
                cov++;
                if (!miss) cov_ok++;
                else CHECK(false, "closure misses %zu templates for %s", miss, m.name.c_str());
            }
            printf("[templates] closure(base + world) covers the retail culled set: %d/%d levels\n", cov_ok, cov);
            // Information: how close the pure reference closure (no base set) gets.
            size_t pure_miss = 0, pure_extra = 0, want_total = 0;
            for (auto& m : gd.maps()) {
                std::string level = lower(m.name);
                if (!cat.has_culled(level)) continue;
                Wdf w;
                if (!gd.load_world(m, w)) continue;
                auto seed = cat.seed_of(w);
                for (uint32_t u : cat.common_set(m.multiplayer)) seed.insert(u);
                auto pure = cat.closure(seed);
                auto& want = cat.culled_set(level);
                want_total += want.size();
                for (uint32_t u : want) if (!pure.count(u)) pure_miss++;
                for (uint32_t u : pure) if (!want.count(u)) pure_extra++;
            }
            printf("[templates] info: pure closure (world refs + common set, no base) misses %zu and adds %zu of %zu retail entries\n",
                   pure_miss, pure_extra, want_total);
            // Model archive index sample.
            const TemplateInfo* t = cat.find(36028);
            if (t) printf("[templates] sample 36028 '%s' category '%s' model '%s' in %zu archives\n", t->name.c_str(),
                          t->category.c_str(), t->model.c_str(), t->archives.size());
        }
    }

    // ---------------------------------------------------------------- FT-ED: DPI scale, crash-save timeout, file names
    {
        CHECK(dpi_to_scale(96) == 1.f && dpi_to_scale(144) == 1.5f && dpi_to_scale(192) == 2.f, "FT dpi_to_scale 96/144/192");
        CHECK(dpi_to_scale(0) == 1.f && dpi_to_scale(5) == 1.f && dpi_to_scale(100000) == 1.f, "FT dpi_to_scale nonsense gives 1");

        bool fin = false;
        CHECK(run_with_timeout([] { return true; }, 2000, &fin) && fin, "FT run_with_timeout fast true");
        CHECK(!run_with_timeout([] { return false; }, 2000, &fin) && fin, "FT run_with_timeout fast false");
        CHECK(!run_with_timeout([]() -> bool { throw 1; }, 2000, &fin) && fin, "FT run_with_timeout swallows a C++ exception");
        CHECK(!run_with_timeout([]() -> bool { *(volatile int*)nullptr = 1; return true; }, 2000, &fin) && fin,
              "FT run_with_timeout swallows an access violation");
        auto t0 = std::chrono::steady_clock::now();
        static volatile long hung_done = 0;
        bool r = run_with_timeout([] { Sleep(1500); hung_done = 1; return true; }, 150, &fin);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!r && !fin && ms < 1000, "FT run_with_timeout gives up on a hung job (%.0f ms)", ms);
        Sleep(1800);  // the abandoned thread finishes and frees itself without touching freed state
        CHECK(hung_done == 1, "FT abandoned job still ran to its end");
        // A job that locks a mutex the caller holds (the crash case) must not hang the caller.
        static std::mutex mu;
        mu.lock();
        r = run_with_timeout([] { std::lock_guard<std::mutex> g(mu); return true; }, 150, &fin);
        CHECK(!r && !fin, "FT run_with_timeout on a lock held by the caller");
        mu.unlock();
        Sleep(100);

        CHECK(safe_file_stem("My Map") == "My Map", "FT safe_file_stem plain");
        CHECK(safe_file_stem("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j", "FT safe_file_stem bad characters");
        CHECK(safe_file_stem("CON") == "_CON" && safe_file_stem("nul") == "_nul" && safe_file_stem("com1") == "_com1" &&
                  safe_file_stem("LPT9.txt") == "_LPT9.txt", "FT safe_file_stem reserved names");
        CHECK(safe_file_stem("Console") == "Console" && safe_file_stem("com10") == "com10", "FT safe_file_stem near-reserved names kept");
        CHECK(safe_file_stem("map. . ") == "map" && safe_file_stem("   ") == "Scenario" && safe_file_stem("") == "Scenario" &&
                  safe_file_stem("...") == "Scenario", "FT safe_file_stem trailing dots and spaces, empty");
        CHECK(safe_file_stem(std::string(300, 'x')).size() == 120, "FT safe_file_stem length cap");
        CHECK(safe_file_stem("Caf\xC3\xA9 Map") == "Caf\xC3\xA9 Map", "FT safe_file_stem keeps UTF-8");
    }

    // ---------------------------------------------------------------- Durable save, recovery names, load guards
    {
        // write_file: replaces the target, leaves no .part behind, and works on an empty file.
        fs::path d = fs::temp_directory_path() / L"ma2scn_b9_test";
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::path f = d / L"a.bin";
        Bytes one(5000, 1), two(7, 2), got;
        CHECK(write_file(f, one) && read_file(f, got) && got == one, "B9 write_file first write");
        CHECK(write_file(f, two) && read_file(f, got) && got == two, "B9 write_file replaces an existing file");
        CHECK(!fs::exists(fs::path(f) += L".part", ec), "B9 write_file leaves no .part");
        CHECK(write_file(f, Bytes()) && read_file(f, got) && got.empty(), "B9 write_file empty");
        fs::remove_all(d, ec);

        // Terrain guards: the largest retail heightfield stays under the load cap, and a bad scale is repaired (a scale of 0
        // made raycast loop forever).
        int max_w = 0, max_h = 0, n_t = 0, dup_uids = 0;
        for (auto& m : gd.maps()) {
            Wdf w;
            Terrain t;
            if (!gd.load_world(m, w) || !t.load_from_world(w, &err)) continue;
            n_t++;
            max_w = std::max(max_w, t.w);
            max_h = std::max(max_h, t.h);
            CHECK(t.scale > Terrain::kMinScale && t.scale < Terrain::kMaxScale, "B9 retail terrain scale %s: %g", m.name.c_str(), t.scale);
            std::set<uint32_t> uids;
            for (auto& o : w.objs) if (o.uid() && !uids.insert(o.uid()).second) dup_uids++;
        }
        printf("[ft] duplicate object UIDs across retail levels: %d\n", dup_uids);
        printf("[b9] %d retail terrains; largest %dx%d (load cap %d)\n", n_t, max_w, max_h, Terrain::kMaxSize);
        CHECK(n_t > 0 && max_w <= Terrain::kMaxSize && max_h <= Terrain::kMaxSize, "B9 retail terrain size under the cap");
        if (const MapInfo* m = gd.find_map("NewRiverCity")) {
            Wdf w;
            if (gd.load_world(*m, w)) {
                WObj* tobj = nullptr;
                for (auto& o : w.objs) if (o.cls == K_TERRAIN) { tobj = &o; break; }
                if (tobj) {
                    for (float bad : {0.f, -3.f, std::nanf(""), 1e30f}) {
                        tobj->set_f32("TerrainWorldScale", bad);
                        Terrain t;
                        CHECK(t.load_from_world(w, &err) && t.scale == 8.f, "B9 bad TerrainWorldScale %g falls back to 8", bad);
                        float step_end = 0;
                        Vec3 o{0, 500, 0}, dir{0.1f, -1, 0.1f};
                        CHECK(t.raycast(o, dir, step_end, 2000.f) || true, "B9 raycast returns with the repaired scale");
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- Terrain model + pathfinding
    {
        const MapInfo* m = gd.find_map("NewRiverCity");
        Wdf w;
        if (m && gd.load_world(*m, w)) {
            Terrain t;
            bool ok = t.load_from_world(w, &err);
            CHECK(ok, "terrain from NewRiverCity: %s", err.c_str());
            MgfReader* a = gd.archive(m->resource);
            Bytes mech;
            if (ok && a && a->read_file("PATHFINDING/NewRiverCity_Mech.map", mech)) {
                NavGraph g;
                g.parse(std::string_view(reinterpret_cast<const char*>(mech.data()), mech.size()));
                int on = 0, tot = 0;
                for (auto& n : g.nodes) {
                    if (n.type != 4) continue;
                    tot++;
                    if (std::fabs(t.height_at(n.pos.x, n.pos.z) + 2.0f - n.pos.y) < 0.05f) on++;
                }
                printf("[terrain] NewRiverCity: %dx%d, %.0f..%.0f m; retail grid nodes at terrain+2 m: %d/%d\n", t.w, t.h,
                       t.min_h, t.max_h, on, tot);
                CHECK(tot > 0 && on * 10 >= tot * 9, "height formula vs retail nav nodes (%d/%d)", on, tot);
                std::string text = g.serialize();
                NavGraph g2;
                g2.parse(text);
                CHECK(g2.nodes.size() == g.nodes.size(), "nav graph text round trip");
            }
        }
    }

    // ---------------------------------------------------------------- Scenario save / reload
    {
        t0 = now_s();
        TemplateCatalog cat;
        if (cat.load(gd, &err)) {
            ScenarioIO io(gd, cat);
            auto report = [&](const char* label, Scenario& s, const fs::path& out) {
                SaveReport rep;
                bool ok = io.save(s, out, &rep, &err);
                CHECK(ok, "save %s: %s", label, err.c_str());
                if (!ok) return;
                printf("[scenario] %s -> %s: %zu objects, %zu templates, %zu asset files (%.1f MB outside Base), %d nav nodes, %.2f MB file\n",
                       label, path_utf8(out.filename()).c_str(), rep.objects, rep.templates, rep.asset_files,
                       rep.asset_bytes / 1048576.0, rep.nav_nodes, rep.file_bytes / 1048576.0);
                for (auto& w : rep.warnings) printf("           warning: %s\n", w.c_str());
                Scenario back;
                ok = io.load(out, back, &err);
                CHECK(ok, "reload %s: %s", label, err.c_str());
                if (!ok) return;
                CHECK(back.id == s.id && back.name == s.name && back.types == s.types && back.base == s.base, "reload metadata %s", label);
                CHECK(back.terrain.samples() == s.terrain.samples(), "reload heights %s", label);
                CHECK(back.colormap_custom == s.colormap_custom && back.lightmap_custom == s.lightmap_custom, "reload textures %s", label);
                // Saving the reloaded scenario again gives the same archive.
                Bytes a, b;
                SaveReport r2;
                io.build(s, a, &r2, &err);
                io.build(back, b, &r2, &err);
                CHECK(a.size() == b.size(), "re-save stable size %s (%zu vs %zu)", label, a.size(), b.size());
                // F-V5R-1: an allocation failure while building the file is a message, not a dead editor; the map stays as it was.
                {
                    const bool dirty0 = s.dirty;
                    const size_t objs0 = s.world.objs.size();
                    Bytes fb;
                    std::string ferr;
                    ScenarioIO::test_fail_alloc = true;
                    const bool fok = io.build(s, fb, &r2, &ferr);
                    CHECK(!fok && fb.empty() && ferr.find("Not enough memory") != std::string::npos && !ScenarioIO::test_fail_alloc,
                          "F-V5R-1 build with an allocation failure returns false with a message (%s)", label);
                    fs::path ff = out;
                    ff += L".fail";
                    ferr.clear();
                    ScenarioIO::test_fail_alloc = true;
                    const bool sok = io.save(s, ff, &r2, &ferr);
                    std::error_code fec;
                    CHECK(!sok && !fs::exists(ff, fec) && ferr.find("Not enough memory") != std::string::npos,
                          "F-V5R-1 save with an allocation failure writes nothing and says why (%s)", label);
                    Bytes again;
                    CHECK(io.build(s, again, &r2, &ferr) && again.size() == a.size() && s.world.objs.size() == objs0 && s.dirty == dirty0,
                          "F-V5R-1 the map builds again after the failure, unchanged (%s)", label);
                }
                for (auto& item : io.validate(back))
                    if (item.level > 0) printf("           validate %s: %s\n", item.level == 2 ? "ERROR" : "warn", item.text.c_str());
            };
            fs::path dir = samples.empty() ? fs::temp_directory_path() / L"ma2scn_test" : samples;
            fs::create_directories(dir);
            // New maps have round 0.5 m steps; a sculpted map with two road pieces on it changes no
            // ground height and no road height through save and reload.
            {
                // Per-template graphics memory and the live sum (8 + Base + the distinct other-level templates).
                {
                    NewMapParams wq;
                    wq.multiplayer = true;
                    wq.name = "Gfx test";
                    wq.base = "witchcauldron.mgf";
                    Scenario sq;
                    std::string errq;
                    if (io.create(wq, sq, &errq)) {
                        const auto& culled = cat.culled_set(sq.base_level());
                        const auto& common = cat.common_set(true);
                        uint32_t base_tpl = 0;
                        std::vector<uint32_t> others;
                        for (auto& t : cat.all()) {
                            if (!t.placeable || t.cls_name != "MABuilding" || t.model.empty() || t.archives.empty()) continue;
                            if (culled.count(t.uid) && !base_tpl && !io.model_absent(sq, t.uid)) base_tpl = t.uid;
                            if (!culled.count(t.uid) && !common.count(t.uid) && others.size() < 60) others.push_back(t.uid);
                        }
                        CHECK(base_tpl && io.template_graphics_mb(sq, base_tpl) == 0.0, "a template of the Base costs 0 MB");
                        std::vector<std::pair<double, uint32_t>> cost;
                        auto tq0 = std::chrono::steady_clock::now();
                        for (uint32_t u : others) if (double m = io.template_graphics_mb(sq, u); m > 0) cost.push_back({m, u});
                        printf("info: template_graphics_mb first call %.1f ms average over %zu templates\n",
                               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tq0).count() / double(std::max<size_t>(others.size(), 1)), others.size());
                        std::sort(cost.begin(), cost.end());
                        CHECK(!cost.empty() && cost.back().first > 0.5, "an object from another level costs over 0.5 MB (%.2f)", cost.empty() ? 0.0 : cost.back().first);
                        CHECK(cost.size() >= 3, "three other-level buildings found (%zu)", cost.size());
                        if (cost.size() >= 3) {
                            const double base_mb = io.base_graphics_mb(sq);
                            const double e0 = io.graphics_estimate(sq).total_mb;
                            const uint32_t a = cost[cost.size() - 1].second, b = cost[cost.size() - 2].second, c = cost[cost.size() - 3].second;
                            io.place_template(sq, a, {200.f, 10.f, 200.f}, 0.f);
                            const double e1 = io.graphics_estimate(sq).total_mb;
                            io.place_template(sq, a, {260.f, 10.f, 200.f}, 0.f);
                            const double e2 = io.graphics_estimate(sq).total_mb;
                            CHECK(std::fabs(e2 - e1) < 1e-6 && e1 > e0, "two copies of one object count once (%.3f %.3f %.3f)", e0, e1, e2);
                            io.place_template(sq, b, {320.f, 10.f, 200.f}, 0.f);
                            io.place_template(sq, c, {380.f, 10.f, 200.f}, 0.f);
                            const double est = io.graphics_estimate(sq).total_mb;
                            const double live = 8.0 + base_mb + io.template_graphics_mb(sq, a) + io.template_graphics_mb(sq, b) + io.template_graphics_mb(sq, c);
                            CHECK(std::fabs(live - est) <= 0.15 * est, "live sum %.1f is within 15%% of graphics_estimate %.1f", live, est);
                        }
                    } else printf("info: could not create a Witch Cauldron map (%s)\n", errq.c_str());
                }
                // Plants from other levels. Bit allocation (pure), then a Witch Cauldron map with a plant of another level
                // saved and reopened.
                {
                    CHECK(alloc_culture_bits({}, 3) == 0, "no layers: shift 0");
                    CHECK(alloc_culture_bits({0x000F, 0x00F0}, 3) == 8, "two layers of 4 bits: next 3 bits at shift 8");
                    CHECK(alloc_culture_bits({0x0007, 0x01C0}, 3) == 3, "a gap of 3 bits between two layers is used");
                    CHECK(alloc_culture_bits({0x00FF, 0xFF00}, 1) == -1, "full map: -1");
                    CHECK(alloc_culture_bits({0x7FFF}, 1) == 15 && alloc_culture_bits({0x7FFF}, 2) == -1, "the last free bit, and no room for two");
                    NewMapParams wp;
                    wp.multiplayer = true;
                    wp.name = "Plant test";
                    wp.base = "witchcauldron.mgf";
                    Scenario sp;
                    std::string errp;
                    if (io.create(wp, sp, &errp)) {
                        const auto& culled = cat.culled_set(sp.base_level());
                        const auto& srcs = io.plant_sources();
                        printf("info: %zu plants over the retail levels\n", srcs.size());
                        CHECK(srcs.size() >= 10, "plant sources over the retail worlds (%zu)", srcs.size());
                        const ScenarioIO::PlantSource* pick = nullptr;
                        for (auto& ps : srcs) {
                            bool fresh = false;
                            for (uint32_t t : ps.templates) fresh = fresh || !culled.count(t);
                            bool here = false;
                            for (auto& o : sp.world.objs) here = here || (o.cls == key_of("MACultureType") && iequals(o.str("Name"), ps.name));
                            if (fresh && !here && ps.nbits <= 3) { pick = &ps; break; }
                        }
                        CHECK(pick != nullptr, "a plant of another level to add");
                        if (pick) {
                            auto count_types = [&](const Scenario& sc) {
                                int n = 0;
                                for (auto& o : sc.world.objs) n += o.cls == key_of("MACultureType") && iequals(o.str("Name"), pick->name);
                                return n;
                            };
                            const int ti = world::find_class(sp.world, key_of("MATerrainInfo"));
                            const uint32_t n0 = sp.world.objs[size_t(ti)].u32("culturenBitfields");
                            const size_t objs0 = sp.world.objs.size();
                            CHECK(io.add_plant(sp, *pick, true) == ScenarioIO::PlantResult::Added && sp.world.objs.size() == objs0, "dry run changes nothing");
                            bool fresh_layer = false;
                            CHECK(io.add_plant(sp, *pick, false, &fresh_layer) == ScenarioIO::PlantResult::Added, "plant added");
                            CHECK(fresh_layer && sp.world.objs[size_t(world::find_class(sp.world, key_of("MATerrainInfo")))].u32("culturenBitfields") == n0 + 1, "culturenBitfields + 1");
                            CHECK(count_types(sp) == 1 && sp.world.objs.size() == objs0 + pick->tree.objs.size(), "the type and its generators are on the map (%zu objects)", pick->tree.objs.size());
                            CHECK(io.add_plant(sp, *pick) == ScenarioIO::PlantResult::AlreadyHere, "the same plant twice: already here");
                            // The new type's uids are fresh and its generator hangs under it.
                            {
                                WTree wt;
                                wt.build(sp.world);
                                bool linked = false;
                                for (size_t i = 0; i < sp.world.objs.size(); i++)
                                    if (sp.world.objs[i].cls == key_of("MACultureType") && iequals(sp.world.objs[i].str("Name"), pick->name))
                                        for (int k : wt.kids[i]) linked = linked || (sp.world.objs[size_t(k)].cls == key_of("MACultureGenerator") && sp.world.objs[size_t(k)].u32("GenerationTemplate") == pick->templates.front());
                                CHECK(linked, "the generator sits under the type with its template");
                                std::set<uint32_t> uids;
                                bool dup = false;
                                for (auto& o : sp.world.objs) dup = dup || !uids.insert(o.uid()).second;
                                CHECK(!dup, "every uid is unique");
                            }
                            const fs::path pf = dir / L"plant_test.ma2map";
                            SaveReport prep;
                            const bool oks = io.save(sp, pf, &prep, &errp);
                            CHECK(oks, "save: %s", errp.c_str());
                            Scenario pb;
                            if (oks && io.load(pf, pb, &errp)) {
                                const int tb = world::find_class(pb.world, key_of("MATerrainInfo"));
                                CHECK(tb >= 0 && pb.world.objs[size_t(tb)].u32("culturenBitfields") == n0 + 1 && count_types(pb) == 1, "reopened: layer and type kept");
                                uint64_t bytes = 0;
                                auto assets = io.asset_closure(pb, {}, &bytes);
                                const TemplateInfo* ti = cat.find(pick->templates.front());
                                std::string stem = ti ? lower(ti->model) : std::string();
                                const size_t cut = stem.find_last_of("\\/");
                                stem = stem.substr(cut == std::string::npos ? 0 : cut + 1);
                                stem = stem.substr(0, stem.find('.'));
                                bool listed = false;
                                for (auto& [arc, path] : assets) listed = listed || (!stem.empty() && lower(path).find(stem) != std::string::npos);
                                CHECK(listed, "the plant's model (%s) is in the save's [Assets] (%zu files)", stem.c_str(), assets.size());
                            } else CHECK(false, "reload: %s", errp.c_str());
                            std::error_code ec;
                            fs::remove(pf, ec);
                        }
                    } else printf("info: could not create a Witch Cauldron map (%s)\n", errp.c_str());
                }
                CHECK(NewMapParams().max_h == 107.5f, "T1.5 NewMapParams default max height is 107.5 m");
                NewMapParams gp;
                gp.multiplayer = true;
                gp.name = "Ground test";
                Scenario sg;
                if (io.create(gp, sg, &err)) {
                    CHECK(sg.terrain.step() == 0.5f, "T1.5 a created map has 0.5 m steps (%.5f)", sg.terrain.step());
                    CHECK(sg.terrain.height_at(100.f, 100.f) == 10.f, "T1.5 flat ground reads 10.00 (%.4f)", sg.terrain.height_at(100.f, 100.f));
                    Rng gr(99);
                    for (int i = 0; i < 200; i++) {
                        const int gx = int(gr.next() % uint64_t(sg.terrain.w)), gz = int(gr.next() % uint64_t(sg.terrain.h));
                        sg.terrain.ref(gx, gz) += gr.range(-6.f, 6.f);
                    }
                    std::string road_def;
                    if (const Wdf* rw = io.retail_world("Mission_16"))
                        for (auto& o : rw->objs)
                            if (o.cls == key_of("MARoad") && iends_with(o.str("RoadDefinition"), "longStraight.road")) { road_def = o.str("RoadDefinition"); break; }
                    CHECK(!road_def.empty(), "T1.6 a retail long straight road exists");
                    std::vector<uint32_t> roads;
                    if (!road_def.empty()) {
                        roads.push_back(io.place_road(sg, road_def, {300.f, 0.f, 300.f}, 0.3f));
                        roads.push_back(io.place_road(sg, road_def, {520.f, 0.f, 610.f}, 1.1f));
                    }
                    CHECK(roads.size() == 2 && roads[0] && roads[1], "T1.6 two road pieces placed");
                    auto heights = [&](Scenario& sc) {
                        std::vector<float> v;
                        for (float z = 0; z <= sc.terrain.size_z(); z += 4.f)
                            for (float x = 0; x <= sc.terrain.size_x(); x += 4.f) v.push_back(sc.terrain.height_at(x, z));
                        for (uint32_t u : roads)
                            for (auto& o : sc.world.objs)
                                if (o.uid() == u) { Vec3 p; if (world::get_pos(o, p)) v.push_back(p.y); }
                        return v;
                    };
                    const std::vector<float> before = heights(sg);
                    fs::path gf = dir / L"ma2scn_ground_test.ma2map";
                    SaveReport grep_;
                    Scenario sgb;
                    const bool okg = io.save(sg, gf, &grep_, &err);
                    CHECK(okg, "T1.6 save: %s", err.c_str());
                    const std::vector<float> after_save = heights(sg);
                    if (okg && io.load(gf, sgb, &err)) {
                        const std::vector<float> after_load = heights(sgb);
                        float w1 = 0, w2 = 0;
                        for (size_t i = 0; i < before.size() && i < after_save.size(); i++) w1 = std::max(w1, std::fabs(before[i] - after_save[i]));
                        for (size_t i = 0; i < before.size() && i < after_load.size(); i++) w2 = std::max(w2, std::fabs(before[i] - after_load[i]));
                        CHECK(before.size() == after_load.size() && w1 < 0.0005f && w2 < 0.0005f, "T1.6 ground and road heights equal before / after save / after load (%zu values, worst %.5f / %.5f)", before.size(), w1, w2);
                    } else CHECK(false, "T1.6 reload: %s", err.c_str());
                    // T2.10 the layer property: kept in editor/project.json, never in the saved world, restored on load, heights untouched.
                    if (roads.size() == 2 && roads[0] && roads[1]) {
                        const std::string basin_text = place::basin_encode(3, 4, 2, 2, {1, 2, 3, 4}, {5, 6, 7, 8}, "10.000 20.000 80.000 60.000 0.0000 0 9.850");
                        for (auto& o : sg.world.objs) {
                            if (o.uid() == roads[0]) place::set_layer(o, 1, false);
                            if (o.uid() == roads[1]) place::set_layer(o, 2, true);
                            if (o.uid() == roads[0]) o.set_str(place::kBasinProp, basin_text);  // T6.2 (any object carries it the same way as a water)
                        }
                        Bytes arc2;
                        SaveReport r2;
                        const bool b2 = io.build(sg, arc2, &r2, &err);
                        CHECK(b2, "T2.10 build: %s", err.c_str());
                        fs::path lf = fs::temp_directory_path() / L"ma2scn_layers_test.ma2map";
                        if (b2 && write_file(lf, arc2)) {
                            MgfReader lar;
                            Wdf lw;
                            Bytes wb2, pb2;
                            const bool rd = lar.open(lf, &err) && lar.read_file("world.wdf", wb2) && lw.parse(wb2) && lar.read_file("editor/project.json", pb2);
                            CHECK(rd, "T2.10 read the saved layers world");
                            bool left = false;
                            for (auto& o : lw.objs) left |= o.find(place::kLayerProp) != nullptr;
                            const std::string pj2(pb2.begin(), pb2.end());
                            CHECK(!left, "T2.10 the saved world has no EditorLayer property");
                            CHECK(pj2.find("\"layers\"") != std::string::npos && pj2.find("2 fixed") != std::string::npos, "T2.10 editor/project.json holds the layers");
                            bool left_b = false;
                            for (auto& o : lw.objs) left_b |= o.find(place::kBasinProp) != nullptr;
                            CHECK(!left_b && pj2.find("\"basins\"") != std::string::npos, "T6.2 the saved world has no EditorBasin property, project.json has \"basins\"");
                            lar.close();
                            Scenario lb;
                            if (io.load(lf, lb, &err)) {
                                bool same = true;
                                int seen = 0;
                                for (auto& o : lb.world.objs) {
                                    int ly = 0;
                                    bool fx = false;
                                    if (o.uid() != roads[0] && o.uid() != roads[1]) continue;
                                    const bool has = place::layer_of(o, ly, fx);
                                    if (has && ((o.uid() == roads[0] && ly == 1 && !fx) || (o.uid() == roads[1] && ly == 2 && fx))) seen++;
                                    Vec3 p1, p2;
                                    for (auto& o2 : sg.world.objs)
                                        if (o2.uid() == o.uid() && world::get_pos(o, p1) && world::get_pos(o2, p2)) same = same && p1.y == p2.y;
                                }
                                CHECK(seen == 2 && same, "T2.10 after load both layers are back and every road's y is bit-identical");
                                bool basin_back = false;
                                for (auto& o : lb.world.objs)
                                    if (o.uid() == roads[0] && o.find(place::kBasinProp)) basin_back = o.str(place::kBasinProp) == basin_text;
                                CHECK(basin_back, "T6.2 load restores EditorBasin unchanged");
                            } else CHECK(false, "T2.10 reload: %s", err.c_str());
                        }
                        std::error_code lec;
                        fs::remove(lf, lec);
                    }
                    std::error_code ec;
                    fs::remove(gf, ec);
                }
            }
            // .bak of the last good version, the over-the-limit refusal, and a Base that is not a bare archive name.
            {
                NewMapParams bp;
                bp.start = NewMapParams::CopyRetail;
                bp.copy_map = "UnderCover";
                bp.multiplayer = true;
                bp.name = "B9 save test";
                Scenario sb;
                if (io.create(bp, sb, &err)) {
                    fs::path bf = fs::temp_directory_path() / L"ma2scn_b9_save.ma2map", bak = fs::path(bf) += L".bak";
                    std::error_code bec;
                    fs::remove(bf, bec);
                    fs::remove(bak, bec);
                    SaveReport brep;
                    bool first = io.save(sb, bf, &brep, &err);
                    CHECK(first && !brep.over_limit, "B9 first save: %s", err.c_str());
                    CHECK(!fs::exists(bak, bec), "B9 the first save has nothing to back up");
                    Bytes v1;
                    read_file(bf, v1);
                    sb.name = "B9 save test 2";
                    CHECK(io.save(sb, bf, &brep, &err), "B9 second save: %s", err.c_str());
                    Bytes b1;
                    CHECK(fs::exists(bak, bec) && read_file(bak, b1) && b1 == v1, "B9 .bak holds the previous version");
                    // Past 3,000 objects: refused, the file untouched; forced: written.
                    Scenario big = sb;
                    size_t n0 = big.world.objs.size();
                    for (size_t i = 0; i < n0 && big.world.objs.size() <= ScenarioIO::kMaxObjects + 5; i = (i + 1) % n0) {
                        WObj o = big.world.objs[i];
                        if (o.cls == K_TERRAIN || o.cls == K_TINFO) continue;
                        big.world.objs.push_back(o);
                    }
                    Bytes before;
                    read_file(bf, before);
                    SaveReport orep;
                    std::string oerr;
                    bool refused = !io.save(big, bf, &orep, &oerr);
                    Bytes after;
                    read_file(bf, after);
                    CHECK(refused && orep.over_limit && !oerr.empty(), "B9 a map over 3,000 objects is not saved (%zu objects)", big.world.objs.size());
                    CHECK(before == after, "B9 the refused save leaves the file as it was");
                    CHECK(io.save(big, bf, &orep, &oerr, true), "B9 forced save of an over-limit map: %s", oerr.c_str());
                    fs::remove(bf, bec);
                    fs::remove(bak, bec);
                    printf("[b9] save/.bak/limit checks ran (over-limit map: %zu objects, %s)\n", big.world.objs.size(), orep.limit_text.c_str());
                    // A Base naming a path is refused on load.
                    Bytes ab;
                    SaveReport arep;
                    if (io.build(sb, ab, &arep, &err)) {
                        std::string txt(ab.begin(), ab.end());
                        size_t at = txt.find("Base = ");
                        CHECK(at != std::string::npos, "B9 scenario.ini holds a Base line");
                    }
                    Scenario badbase = sb;
                    badbase.base = "../evil.mgf";
                    Bytes bb;
                    SaveReport brep2;
                    fs::path bbf = fs::temp_directory_path() / L"ma2scn_b9_badbase.ma2map";
                    // build() writes the Base as named; load() must refuse it.
                    if (io.build(badbase, bb, &brep2, &err) && write_file(bbf, bb)) {
                        Scenario lb;
                        std::string lerr;
                        CHECK(!io.load(bbf, lb, &lerr), "B9 load refuses a Base that names a path");
                    }
                    fs::remove(bbf, bec);
                }
            }
            // The save revision. scenario.ini carries SavedWith = kSaveRevision; a file without the line loads as revision 1.
            // rewrite_scn copies a saved .ma2map into a new archive with scenario.ini passed through `edit` (an "old editor" file).
            auto rewrite_scn = [&](const fs::path& src, const fs::path& dst, const std::function<std::string(const std::string&)>& edit) {
                MgfReader rd;
                std::string rerr;
                if (!rd.open(src, &rerr)) return false;
                MgfWriter wr;
                for (size_t i = 0; i < rd.files().size(); i++) {
                    Bytes data;
                    if (!rd.read_file(int(i), data, &rerr)) return false;
                    if (iequals(rd.files()[i].path, "scenario.ini")) {
                        std::string t = edit(std::string(data.begin(), data.end()));
                        data.assign(t.begin(), t.end());
                    }
                    wr.add(rd.files()[i].path, data, rd.files()[i].rec[5]);
                }
                return write_file(dst, wr.build());
            };
            {
                NewMapParams wp;
                wp.start = NewMapParams::CopyRetail;
                wp.copy_map = "UnderCover";
                wp.multiplayer = true;
                wp.name = "W13b stamp";
                Scenario sw;
                if (io.create(wp, sw, &err)) {
                    CHECK(sw.saved_with == kSaveRevision, "a new scenario carries the current revision");
                    fs::path wf = fs::temp_directory_path() / L"ma2scn_w13b.ma2map", wold = fs::temp_directory_path() / L"ma2scn_w13b_old.ma2map";
                    std::error_code wec;
                    SaveReport wrep;
                    CHECK(io.save(sw, wf, &wrep, &err), "save: %s", err.c_str());
                    {
                        MgfReader rd;
                        Bytes ini;
                        bool ok = rd.open(wf, &err) && rd.read_file("scenario.ini", ini, &err);
                        std::string t(ini.begin(), ini.end());
                        CHECK(ok && t.find("FormatVersion = 1\r\n") != std::string::npos, "FormatVersion stays 1");
                        CHECK(ok && t.find("SavedWith = " + std::to_string(kSaveRevision) + "\r\n") != std::string::npos, "scenario.ini says SavedWith = %d", kSaveRevision);
                        CHECK(ok && t.find("Editor = " MA2SE_VERSION " (") != std::string::npos, "scenario.ini names the editor version");
                    }
                    Scenario back;
                    CHECK(io.load(wf, back, &err) && back.saved_with == kSaveRevision, "a round trip keeps SavedWith");
                    ScenarioHead head;
                    CHECK(ScenarioIO::peek(wf, head) && head.saved_with == kSaveRevision && head.id == sw.id && head.name == sw.name && head.multiplayer, "peek reads the header");
                    const bool made = rewrite_scn(wf, wold, [](const std::string& t) {
                        std::string o;
                        size_t at = 0;
                        while (at < t.size()) {
                            size_t e = t.find("\r\n", at);
                            std::string line = t.substr(at, e == std::string::npos ? std::string::npos : e - at);
                            if (line.rfind("SavedWith", 0) != 0 && line.rfind("Editor =", 0) != 0) o += line + "\r\n";
                            if (e == std::string::npos) break;
                            at = e + 2;
                        }
                        return o;
                    });
                    CHECK(made, "rewrite an old-style file");
                    Scenario old;
                    CHECK(made && io.load(wold, old, &err) && old.saved_with == 1, "a file without SavedWith loads as revision 1");
                    ScenarioHead oh;
                    CHECK(made && ScenarioIO::peek(wold, oh) && oh.saved_with == 1, "peek of a file without SavedWith says 1");
                    if (made) {
                        CHECK(io.save(old, wold, &wrep, &err) && old.saved_with == kSaveRevision, "saving an old file updates it");
                        Scenario upd;
                        CHECK(io.load(wold, upd, &err) && upd.saved_with == kSaveRevision, "the updated file loads as the current revision");
                        fs::path bak = wold;
                        bak += L".bak";
                        CHECK(fs::exists(bak, wec), "the update keeps the old file as .bak");
                        fs::remove(bak, wec);
                    }
                    fs::remove(wf, wec);
                    fs::remove(wold, wec);
                }
            }
            // A file copied in Explorer keeps the Id; open_path sets Id 0 and the next save mints a new one: the Scn<Id> names follow.
            {
                NewMapParams cp;
                cp.start = NewMapParams::CopyRetail;
                cp.copy_map = "UnderCover";
                cp.multiplayer = true;
                cp.name = "W14b copy";
                Scenario sc;
                if (io.create(cp, sc, &err)) {
                    Bytes first;
                    SaveReport frep;
                    CHECK(io.build(sc, first, &frep, &err) && sc.id != 0, "first build mints an Id");
                    const std::string old_hex = sc.id_hex();
                    sc.id = 0;  // what open_path does for a copy
                    Bytes second;
                    CHECK(io.build(sc, second, &frep, &err) && sc.id != 0 && sc.id_hex() != old_hex, "Id 0 gets a new Id on save");
                    fs::path cf = fs::temp_directory_path() / L"ma2scn_w14b.ma2map";
                    CHECK(write_file(cf, second), "write");
                    MgfReader rd;
                    Bytes ini;
                    if (rd.open(cf, &err) && rd.read_file("scenario.ini", ini, &err)) {
                        const std::string t(ini.begin(), ini.end());
                        auto count = [&](const std::string& s) { size_t n = 0; for (size_t at = t.find(s); at != std::string::npos; at = t.find(s, at + 1)) n++; return n; };
                        CHECK(t.find("Id = " + sc.id_hex() + "\r\n") != std::string::npos, "scenario.ini holds the new Id");
                        size_t with_new = 0, with_old = 0;
                        for (auto& f : rd.files()) {
                            with_new += f.path.find("Scn" + sc.id_hex()) != std::string::npos;
                            with_old += f.path.find("Scn" + old_hex) != std::string::npos;
                        }
                        (void)count;
                        CHECK(with_new > 0, "the archive's Scn<Id> files use the new Id (%zu)", with_new);
                        CHECK(with_old == 0, "no file in the archive keeps the old Id (%zu)", with_old);
                    } else CHECK(false, "read scenario.ini: %s", err.c_str());
                    std::error_code cec;
                    fs::remove(cf, cec);
                }
            }
            // W-114: a flipped byte inside one entry of a saved .ma2map is reported by load (the entry is named; an optional one
            // falls back to its default), a damaged scenario.ini fails the load, an intact file reports nothing.
            {
                NewMapParams dp;
                dp.start = NewMapParams::CopyRetail;
                dp.copy_map = "UnderCover";
                dp.multiplayer = true;
                dp.name = "W114 damaged";
                Scenario sd;
                if (io.create(dp, sd, &err)) {
                    fs::path df = fs::temp_directory_path() / L"ma2scn_w114.ma2map";
                    std::error_code dec;
                    SaveReport drep;
                    CHECK(io.save(sd, df, &drep, &err), "W-114 save: %s", err.c_str());
                    std::vector<std::string> warn;
                    Scenario ok_back;
                    CHECK(io.load(df, ok_back, &err, &warn) && warn.empty(), "W-114 an intact file loads with no warning (%zu)", warn.size());
                    Bytes raw;
                    read_file(df, raw);
                    std::string target;
                    uint64_t at = 0;
                    {
                        MgfReader rd;
                        rd.open(df, &err);
                        for (auto& f : rd.files())
                            if (iequals(f.path, "editor/project.json") && f.size() > 64) { target = f.path; at = f.offset() + f.size() / 2; break; }
                    }
                    CHECK(!target.empty(), "W-114 the saved file has an editor/project.json entry");
                    if (!target.empty()) {
                        Bytes hurt = raw;
                        for (int i = 0; i < 16; i++) hurt[size_t(at) + size_t(i)] ^= 0xFF;
                        fs::path hf = fs::temp_directory_path() / L"ma2scn_w114_bad.ma2map";
                        CHECK(write_file(hf, hurt), "W-114 write the damaged copy");
                        Scenario bad;
                        std::vector<std::string> w2;
                        const bool loaded = io.load(hf, bad, &err, &w2);
                        CHECK(loaded, "W-114 a damaged optional entry still loads: %s", err.c_str());
                        CHECK(w2.size() == 1 && w2[0] == target, "W-114 the warning names the entry (%zu: %s)", w2.size(), w2.empty() ? "" : w2[0].c_str());
                        std::error_code hec;
                        fs::remove(hf, hec);
                    }
                    fs::remove(df, dec);
                }
            }
            // Scenario.ini says Ready = 0 while Check shows a Must fix item (the game then does not list the file), else Ready = 1.
            {
                NewMapParams rp;
                rp.start = NewMapParams::CopyRetail;
                rp.copy_map = "UnderCover";
                rp.multiplayer = true;
                rp.name = "W15b ready";
                Scenario sr;
                if (io.create(rp, sr, &err)) {
                    auto ready_line = [&](Scenario& s, const char* label) -> std::string {
                        fs::path rf = fs::temp_directory_path() / L"ma2scn_w15b.ma2map";
                        SaveReport rrep;
                        std::string line;
                        if (!io.save(s, rf, &rrep, &err)) { CHECK(false, "save %s: %s", label, err.c_str()); return line; }
                        MgfReader rd;
                        Bytes ini;
                        if (rd.open(rf, &err) && rd.read_file("scenario.ini", ini, &err)) {
                            const std::string t(ini.begin(), ini.end());
                            size_t at = t.find("Ready = ");
                            if (at != std::string::npos) line = t.substr(at, 9);
                        }
                        rd.close();
                        std::error_code rec;
                        fs::remove(rf, rec);
                        fs::remove(fs::path(rf) += L".bak", rec);
                        return line;
                    };
                    bool errors = false;
                    for (auto& v : io.validate(sr)) errors |= v.level == 2;
                    CHECK(!errors, "the copied level has no Must fix item");
                    CHECK(ready_line(sr, "ready") == "Ready = 1", "a scenario with no Must fix item is Ready = 1");
                    const size_t before = sr.world.objs.size();
                    sr.world.objs.erase(std::remove_if(sr.world.objs.begin(), sr.world.objs.end(), [](const WObj& o) { return o.cls == key_of("MALevelStart"); }), sr.world.objs.end());
                    errors = false;
                    for (auto& v : io.validate(sr)) errors |= v.level == 2;
                    CHECK(sr.world.objs.size() < before && errors, "without spawn points Check shows a Must fix item");
                    CHECK(ready_line(sr, "unfinished") == "Ready = 0", "a scenario with a Must fix item is Ready = 0");
                }
            }
            // The colour map is kept lossless; what the save adds for the game is undone on reopen.
            {
                const fs::path tmpa = fs::temp_directory_path() / L"ma2scn_f3_a.ma2map";
                auto entry = [&](const Bytes& arc, const std::string& name, Bytes& out) {
                    MgfReader rd;
                    std::string e2;
                    return write_file(tmpa, arc) && rd.open(tmpa, &e2) && rd.read_file(name, out);
                };
                auto roundtrip = [&](Scenario& sc, Scenario& back, Bytes* arc_out) {
                    Bytes arc;
                    SaveReport rp;
                    std::string e2;
                    if (!io.build(sc, arc, &rp, &e2, true)) return false;
                    if (arc_out) *arc_out = arc;
                    return write_file(tmpa, arc) && io.load(tmpa, back, &e2);
                };
                NewMapParams fp;
                fp.multiplayer = true;
                fp.name = "F3 saving";
                Scenario f3;
                CHECK(io.create(fp, f3, &err), "F3 new flat map: %s", err.c_str());
                if (!f3.colormap.px.empty()) {
                    // 2,000 random texels with random RGBA (alpha 0..255).
                    uint32_t st = 777;
                    for (int i = 0; i < 2000; i++) {
                        st = st * 1664525u + 1013904223u;
                        const size_t at = (st >> 8) % f3.colormap.px.size();
                        st = st * 1664525u + 1013904223u;
                        f3.colormap.px[at] = st;
                    }
                    f3.colormap_custom = true;
                    const std::string tif = "Terrain/Scn" + f3.id_hex() + "_ColorMap.tif";
                    Bytes arc1, tif1, arc2, tif2;
                    Scenario b1;
                    CHECK(roundtrip(f3, b1, &arc1) && entry(arc1, tif, tif1), "T1.3 build and load");
                    CHECK(b1.colormap_lossless && b1.colormap.w == f3.colormap.w && b1.colormap.px == f3.colormap.px, "T1.3 colormap.px bit-identical after save + load");
                    CHECK(io.build(b1, arc2, nullptr, &err) && entry(arc2, tif, tif2) && !tif1.empty() && tif1 == tif2, "T1.3 the TIF written again is byte-identical (%zu bytes)", tif1.size());
                    // Five generations.
                    Scenario cur = b1;
                    bool same_cm = true, same_tif = true;
                    for (int g = 0; g < 5; g++) {
                        Bytes ga, gt;
                        Scenario nx;
                        if (!roundtrip(cur, nx, &ga) || !entry(ga, tif, gt)) { same_cm = same_tif = false; break; }
                        same_cm = same_cm && nx.colormap.px == f3.colormap.px;
                        same_tif = same_tif && gt == tif1;
                        cur = std::move(nx);
                    }
                    CHECK(same_cm, "T1.4 five generations: colour map bit-identical every time");
                    CHECK(same_tif, "T1.4 five generations: TIF bytes identical every time");
                    // A TIF changed by another tool: the decoded TIF is used and the load does not fail.
                    {
                        MgfReader rd;
                        std::string e2;
                        MgfWriter wr;
                        bool built = write_file(tmpa, arc1) && rd.open(tmpa, &e2);
                        for (size_t i = 0; built && i < rd.files().size(); i++) {
                            Bytes data;
                            built = rd.read_file(int(i), data, &e2);
                            if (built && iequals(rd.files()[i].path, tif) && data.size() > 400) data[data.size() / 2] ^= 0x40;
                            if (built) wr.add(rd.files()[i].path, data, rd.files()[i].rec[5]);
                        }
                        const fs::path tmpb = fs::temp_directory_path() / L"ma2scn_f3_b.ma2map";
                        Scenario tb;
                        CHECK(built && write_file(tmpb, wr.build()) && io.load(tmpb, tb, &err), "T1.5 a changed TIF still loads: %s", err.c_str());
                        CHECK(built && !tb.colormap_lossless && !tb.colormap.px.empty() && tb.colormap.px != f3.colormap.px, "T1.5 a changed TIF: the decoded TIF is used, not the stored image");
                        std::error_code rec;
                        fs::remove(tmpb, rec);
                    }
                    // The game's colour map holds exactly the editor's texels (swizzled A8R8G8B8, the level's size and mips).
                    {
                        Picture gp;
                        Image g0;
                        const bool ok = read_picture(tif1, gp) && decode_picture(gp, 0, g0);
                        CHECK(ok && gp.dxt() == 0 && gp.format() == PF_ARGB8888 && gp.swizzled(), "the saved colour map is swizzled A8R8G8B8, not DXT (flags %08X)", gp.flags);
                        CHECK(ok && gp.width == f3.colormap_layout.width && gp.height == f3.colormap_layout.height && gp.mips == f3.colormap_layout.mips,
                              "the colour map keeps the level's size and mips (%ux%u, %u mips)", gp.width, gp.height, gp.mips);
                        CHECK(ok && g0.w == f3.colormap.w && g0.px == f3.colormap.px, "the game's colour-map texels equal the editor's (RGBA, 2,000 random texels included)");
                    }
                }
                // Cause: a DXT block of the dark tint gradient seen at the P8 spawn (live editor texels) comes back up to 8 levels
                // off per channel and flips olive (R < G) to brown (R > G): the bands the game showed while the editor drew the gradient.
                {
                    const uint8_t live[16][3] = {{24, 28, 16}, {25, 29, 16}, {27, 32, 19}, {29, 35, 20}, {24, 28, 16}, {25, 30, 17}, {28, 33, 19}, {32, 38, 22},
                                                 {24, 29, 16}, {27, 32, 19}, {32, 36, 23}, {35, 41, 27}, {25, 29, 16}, {28, 33, 19}, {33, 28, 16}, {31, 30, 18}};
                    uint32_t in[16], out[16];
                    for (int i = 0; i < 16; i++) in[i] = 0xFF000000u | uint32_t(live[i][2]) << 16 | uint32_t(live[i][1]) << 8 | live[i][0];
                    uint8_t blk[16];
                    dxt_encode_block(5, in, blk);
                    dxt_decode_block(5, blk, out);
                    int worst = 0, flips = 0;
                    for (int i = 0; i < 16; i++) {
                        for (int c = 0; c < 3; c++) worst = std::max(worst, std::abs(int((in[i] >> (8 * c)) & 255) - int((out[i] >> (8 * c)) & 255)));
                        flips += (int(in[i] & 255) > int((in[i] >> 8) & 255)) != (int(out[i] & 255) > int((out[i] >> 8) & 255));
                    }
                    CHECK(worst >= 4 && flips >= 1, "cause: DXT moves a dark tint gradient by %d levels and flips %d texels' red / green order", worst, flips);
                }
                // A multiplayer map with an Atlas pad and no MAPathDB: nothing the save adds shows after reopening.
                {
                    NewMapParams mp;
                    mp.multiplayer = true;
                    mp.name = "F3 rewrites";
                    Scenario sm;
                    CHECK(io.create(mp, sm, &err), "T1.7 new map: %s", err.c_str());
                    const uint32_t k_pathdb = key_of("MAPathDB");
                    bool had_db = false;
                    for (auto& o : sm.world.objs) had_db = had_db || o.cls == k_pathdb;
                    for (auto& pk : io.pad_kinds())
                        if (pk.label == "Atlas") { io.add_pad(sm, pk, {200.f, 0, 200.f}, 0.f); break; }
                    Bytes arcm;
                    Scenario bm;
                    CHECK(roundtrip(sm, bm, &arcm), "T1.7 save + load");
                    auto prop_text = [](const WObj& o, const char* k) { const WProp* p = o.find(k); return p ? std::string("=") + std::to_string(p->as_int()) + ":" + p->as_string() : std::string("-"); };
                    bool equal_props = bm.world.objs.size() == sm.world.objs.size();
                    int mismatches = 0;
                    for (size_t i = 0; equal_props && i < sm.world.objs.size(); i++) {
                        const WObj& a = sm.world.objs[i];
                        const WObj& b = bm.world.objs[i];
                        for (const char* k : {"Corporal3DObj", "ForceUseLowDetail", "ForceUseHighDetail", "LightMapName", "TerrainBaseMap"})
                            if (prop_text(a, k) != prop_text(b, k)) { mismatches++; if (mismatches < 4) printf("   T1.7 %s of uid %u: '%s' vs '%s'\n", k, a.uid(), prop_text(a, k).c_str(), prop_text(b, k).c_str()); }
                    }
                    CHECK(bm.world.objs.size() == sm.world.objs.size(), "T1.7 object count after reopen (%zu vs %zu; the PathDB was %s)", bm.world.objs.size(), sm.world.objs.size(), had_db ? "there" : "added by the save");
                    CHECK(equal_props && mismatches == 0, "T1.7 model paths and detail flags equal the original (%d differ)", mismatches);
                    Bytes wdf1;
                    Wdf saved;
                    CHECK(entry(arcm, "world.wdf", wdf1) && saved.parse(wdf1), "T1.7 the saved world parses");
                    int dbs = 0, lows = 0, flagged = 0;
                    for (auto& o : saved.objs) {
                        dbs += o.cls == k_pathdb;
                        lows += o.str("Corporal3DObj").find("_low") != std::string::npos;
                        flagged += o.find("ForceUseLowDetail") && o.i32("ForceUseLowDetail") == 1;
                    }
                    printf("[F3] T1.7 saved world.wdf: %d MAPathDB, %d _low model paths, %d ForceUseLowDetail=1 (PathDB in the editor's own world: %s)\n", dbs, lows, flagged, had_db ? "yes" : "no");
                    CHECK(dbs == 1, "T1.7 the saved world.wdf has one MAPathDB (%d)", dbs);
                    Bytes arcm2, wdf2;
                    CHECK(io.build(bm, arcm2, nullptr, &err) && entry(arcm2, "world.wdf", wdf2) && wdf1 == wdf2, "T1.7 build(reopened) world.wdf bytes equal the first build's (%zu vs %zu)", wdf1.size(), wdf2.size());
                    // A file from before the two keys: it loads with the PathDB and the respelled paths, as it always did.
                    {
                        MgfReader rd;
                        std::string e2;
                        MgfWriter wr;
                        bool built = write_file(tmpa, arcm) && rd.open(tmpa, &e2);
                        for (size_t i = 0; built && i < rd.files().size(); i++) {
                            Bytes data;
                            built = rd.read_file(int(i), data, &e2);
                            if (built && iequals(rd.files()[i].path, "editor/project.json")) {
                                Json pj, np = Json::object();
                                built = Json::parse(std::string(data.begin(), data.end()), pj);
                                for (auto& [k, v] : pj.o) if (k != "save_rewrites" && k != "save_added") np.set(k, v);
                                std::string t = np.dump(1);
                                data.assign(t.begin(), t.end());
                            }
                            if (built) wr.add(rd.files()[i].path, data, rd.files()[i].rec[5]);
                        }
                        const fs::path tmpc = fs::temp_directory_path() / L"ma2scn_f3_c.ma2map";
                        Scenario old;
                        CHECK(built && write_file(tmpc, wr.build()) && io.load(tmpc, old, &err), "T1.8 a file without the keys loads: %s", err.c_str());
                        int odbs = 0;
                        for (auto& o : old.world.objs) odbs += o.cls == k_pathdb;
                        CHECK(old.world.objs.size() == saved.objs.size() && odbs == dbs, "T1.8 it keeps what the save wrote (%zu objects, %d PathDB)", old.world.objs.size(), odbs);
                        std::error_code rec;
                        fs::remove(tmpc, rec);
                    }
                }
                std::error_code fec;
                fs::remove(tmpa, fec);
            }
            // Ground layers: 4 textures (the retail pair + 2 from other levels) on a copy of UnderCover,
            // quadrants plus soft circles; the file, the weights and the baked fallback alpha survive a reload.
            // MA2SE_P4_OUT=<file> also writes the scenario there (the in-game test).
            {
                NewMapParams gp;
                gp.start = NewMapParams::CopyRetail;
                gp.copy_map = "UnderCover";
                gp.multiplayer = true;
                gp.name = "Ground layers test";
                Scenario sg;
                bool gok = io.create(gp, sg, &err);
                CHECK(gok, "copy UnderCover for the ground layers test: %s", err.c_str());
                std::vector<std::pair<std::string, std::string>> extra;  // archive, path
                for (auto& an : gd.level_archives()) {
                    if (extra.size() >= 2 || iequals(an, sg.base)) continue;
                    MgfReader* a = gd.archive(an);
                    if (!a) continue;
                    for (auto& f : a->files()) {
                        std::string l = lower(f.path);
                        if (!istarts_with(l, "terrain/") || !iends_with(l, ".tif") || l.find("detail") == std::string::npos || l.find("micro") != std::string::npos) continue;
                        extra.push_back({an, f.path});
                        break;
                    }
                }
                CHECK(extra.size() == 2, "two ground textures from other levels (%zu)", extra.size());
                if (gok && extra.size() == 2 && !sg.colormap.px.empty()) {
                    GroundLayers& g = sg.ground;
                    g.enabled = true;
                    g.layers.resize(4);
                    int ti = -1;
                    for (size_t i = 0; i < sg.world.objs.size(); i++) if (sg.world.objs[i].cls == K_TERRAIN) ti = int(i);
                    if (ti >= 0) {
                        g.layers[0].repeats = sg.world.objs[size_t(ti)].f32("TerrainDetailMult", 64);
                        g.layers[1].repeats = sg.world.objs[size_t(ti)].f32("TerrainDetailMult1", 64);
                    }
                    for (int k = 0; k < 2; k++) {
                        g.layers[size_t(2 + k)].archive = extra[size_t(k)].first;
                        g.layers[size_t(2 + k)].path = extra[size_t(k)].second;
                        g.layers[size_t(2 + k)].repeats = 48;
                        bool lt = load_ground_texture(*gd.archive(extra[size_t(k)].first), extra[size_t(k)].second, g.layers[size_t(2 + k)].image, &err);
                        CHECK(lt, "decode ground texture %s: %s", extra[size_t(k)].second.c_str(), err.c_str());
                        printf("[layers] layer %d = %s|%s\n", 2 + k, extra[size_t(k)].first.c_str(), extra[size_t(k)].second.c_str());
                    }
                    g.init_from_colormap(sg.colormap);
                    uint8_t* d = g.mutable_weights();
                    for (int y = 0; y < g.h; y++)
                        for (int x = 0; x < g.w; x++) {
                            uint8_t* tx = d + (size_t(y) * g.w + x) * 8;
                            std::fill(tx, tx + 8, uint8_t(0));
                            // NW (the UnderCover spawn) 2, NE 3, SW 0, SE 1: the spawn view shows an extra layer.
                            int q = ((x >= g.w / 2) + 2 * (y >= g.h / 2) + 2) % 4;
                            float cx = (x + 0.5f) / g.w - 0.5f, cy = (y + 0.5f) / g.h - 0.5f;
                            float r = std::sqrt(cx * cx + cy * cy);
                            float ring = std::clamp((0.12f - r) / 0.03f, 0.f, 1.f);  // a soft disc of layer 3 in the middle
                            tx[q] = uint8_t(std::lround(255 * (1 - ring)));
                            tx[3] = uint8_t(std::min(255, tx[3] + int(std::lround(255 * ring))));
                        }
                    sg.colormap_custom = true;
                    Bytes enc = encode_ground_layers(g);
                    GroundLayers dec;
                    bool dok = decode_ground_layers(enc, dec, &err);
                    CHECK(dok && dec.count() == 4 && dec.w == g.w && *dec.weights == *g.weights && dec.layers[2].image.px == g.layers[2].image.px &&
                          dec.layers[3].repeats == 48.f && dec.layers[2].path == g.layers[2].path, "ground layers encode/decode");
                    printf("[layers] terrain_layers.bin %.2f MB (weights %dx%d RLE)\n", enc.size() / 1048576.0, g.w, g.h);
                    fs::path gf = fs::temp_directory_path() / L"ma2scn_layers_test.ma2map";
                    SaveReport grep;
                    bool saved = io.save(sg, gf, &grep, &err);
                    CHECK(saved, "save the ground layers test: %s", err.c_str());
                    Scenario gb;
                    if (saved && io.load(gf, gb, &err)) {
                        CHECK(gb.ground.enabled && gb.ground.count() == 4 && gb.ground.weights && *gb.ground.weights == *g.weights &&
                              gb.ground.layers[3].image.px == g.layers[3].image.px, "ground layers reload");
                        // Fallback alpha: SW = layer 0 (255), SE = layer 1 (0), the extra quadrants keep the pair's share (none: 255).
                        // It is in the game's TIF; the reloaded colour map is the editor's own (pre-bake).
                        Image tifimg;
                        {
                            MgfReader tr;
                            Bytes tb;
                            Mgic tm;
                            Picture tl;
                            const bool tok = tr.open(gf, &err) && tr.read_file("Terrain/Scn" + sg.id_hex() + "_ColorMap.tif", tb) && tm.parse(tb) && read_picture(tm, tl) && decode_picture(tl, 0, tifimg);
                            CHECK(tok, "decode the saved colour-map TIF");
                        }
                        uint32_t sw = tifimg.px.empty() ? 0 : tifimg.at(tifimg.w / 8, tifimg.h * 7 / 8) >> 24, se = tifimg.px.empty() ? 1 : tifimg.at(tifimg.w * 7 / 8, tifimg.h * 7 / 8) >> 24;
                        CHECK(sw == 255 && se == 0, "baked colour-map alpha (SW %u, SE %u)", sw, se);
                        CHECK(gb.colormap_lossless && gb.colormap.px == sg.colormap.px, "T1.6 the layers map's pre-bake colour map is exact after reload");
                        {
                            Bytes a1, a2, l1, l2;
                            const fs::path t1 = fs::temp_directory_path() / L"ma2scn_f3_l1.ma2map", t2 = fs::temp_directory_path() / L"ma2scn_f3_l2.ma2map";
                            SaveReport rr;
                            const bool b1 = io.build(sg, a1, &rr, &err) && io.build(gb, a2, &rr, &err);
                            bool le = false;
                            if (b1 && write_file(t1, a1) && write_file(t2, a2)) {
                                MgfReader x1, x2;
                                le = x1.open(t1, &err) && x2.open(t2, &err) && x1.read_file(kGroundLayersFile, l1) && x2.read_file(kGroundLayersFile, l2) && l1 == l2 && !l1.empty();
                            }
                            CHECK(le, "T1.6 the layers file's bytes are identical on re-save");
                            std::error_code ec1;
                            fs::remove(t1, ec1);
                            fs::remove(t2, ec1);
                        }
                        CHECK(gb.ground.map_w == gb.colormap.w && gb.ground.map_h == gb.colormap.h, "layers carry the colour map size (%dx%d)", gb.ground.map_w, gb.ground.map_h);
                        MgfReader ar;
                        Bytes ini;
                        CHECK(ar.open(gf, &err) && ar.read_file("scenario.ini", ini) &&
                              std::string(ini.begin(), ini.end()).find("[Terrain]\r\nLayers = terrain_layers.bin") != std::string::npos, "scenario.ini [Terrain] Layers");
                    } else CHECK(false, "reload the ground layers test: %s", err.c_str());
                    // Off again: no file, no [Terrain] section.
                    Scenario off = sg;
                    off.ground = GroundLayers();
                    Bytes arc;
                    SaveReport orep;
                    io.build(off, arc, &orep, &err);
                    fs::path of = fs::temp_directory_path() / L"ma2scn_layers_off.ma2map";
                    MgfReader orr;
                    Bytes ini;
                    CHECK(write_file(of, arc) && orr.open(of, &err) && !orr.read_file(kGroundLayersFile, ini) && orr.read_file("scenario.ini", ini) &&
                          std::string(ini.begin(), ini.end()).find("[Terrain]") == std::string::npos, "no layers -> no terrain_layers.bin / [Terrain]");
                    char outp[MAX_PATH];
                    if (GetEnvironmentVariableA("MA2SE_P4_OUT", outp, MAX_PATH)) {
                        CHECK(io.save(sg, fs::u8path(outp), &grep, &err), "MA2SE_P4_OUT save: %s", err.c_str());
                        printf("[layers] wrote %s (id %s)\n", outp, sg.id_hex().c_str());
                    }
                }
            }
            // MA2SE_P4_MAPS=<dir>: one layered copy (4 textures: quadrants + a disc, like the UnderCover test) of each campaign
            // level in MA2SE_P4_LEVELS (comma list, default Mission_01b,Mission_02,Mission_03,Mission_04,Mission_05), written as
            // <dir>\<level>_layers.ma2map: the in-game check that the P4 recognition key also holds on levels without a lightmap T3.
            {
                char dirp[MAX_PATH], lvls[512];
                if (GetEnvironmentVariableA("MA2SE_P4_MAPS", dirp, MAX_PATH)) {
                    std::string list = GetEnvironmentVariableA("MA2SE_P4_LEVELS", lvls, sizeof(lvls)) ? lvls : "Mission_01b,Mission_02,Mission_03,Mission_04,Mission_05";
                    std::vector<std::pair<std::string, std::string>> extra;  // archive, path
                    std::vector<std::string> names;
                    for (size_t b = 0; b < list.size();) {
                        size_t e = list.find(',', b);
                        if (e == std::string::npos) e = list.size();
                        if (e > b) names.push_back(list.substr(b, e - b));
                        b = e + 1;
                    }
                    for (auto& nm : names) {
                        NewMapParams mp;
                        mp.start = NewMapParams::CopyRetail;
                        mp.copy_map = nm;
                        mp.multiplayer = false;
                        mp.types.clear();
                        mp.name = nm + " layers";
                        Scenario sm;
                        bool mok = io.create(mp, sm, &err);
                        CHECK(mok, "copy %s for the layers map: %s", nm.c_str(), err.c_str());
                        if (!mok || sm.colormap.px.empty()) continue;
                        // MA2SE_P4_LAYERS=N (2..8, default 4): N-2 extra textures; N > 4 splits the map into N pie slices (no disc).
                        size_t nl = 4;
                        { char nb[16]; if (GetEnvironmentVariableA("MA2SE_P4_LAYERS", nb, sizeof nb)) nl = size_t(std::clamp(atoi(nb), 4, 8)); }
                        extra.clear();
                        for (auto& an : gd.level_archives()) {
                            if (extra.size() >= nl - 2 || iequals(an, sm.base)) continue;
                            MgfReader* a = gd.archive(an);
                            if (!a) continue;
                            for (auto& f : a->files()) {
                                std::string l = lower(f.path);
                                if (!istarts_with(l, "terrain/") || !iends_with(l, ".tif") || l.find("detail") == std::string::npos || l.find("micro") != std::string::npos) continue;
                                if (extra.size() >= nl - 2) break;
                                bool dup = false;
                                for (auto& e2 : extra) dup = dup || iequals(e2.second, f.path);
                                if (dup) continue;
                                extra.push_back({an, f.path});
                                if (extra.size() % 2 == 0 || nl == 4) break;  // two per archive at most keeps them varied
                            }
                        }
                        if (extra.size() != nl - 2) { CHECK(false, "%zu extra ground textures for %s (got %zu)", nl - 2, nm.c_str(), extra.size()); continue; }
                        GroundLayers& g = sm.ground;
                        g.enabled = true;
                        g.layers.resize(nl);
                        int ti = -1;
                        for (size_t i = 0; i < sm.world.objs.size(); i++) if (sm.world.objs[i].cls == K_TERRAIN) ti = int(i);
                        if (ti >= 0) {
                            g.layers[0].repeats = sm.world.objs[size_t(ti)].f32("TerrainDetailMult", 64);
                            g.layers[1].repeats = sm.world.objs[size_t(ti)].f32("TerrainDetailMult1", 64);
                        }
                        bool lok = true;
                        for (size_t k = 0; k + 2 < nl; k++) {
                            GroundLayer& L = g.layers[2 + k];
                            L.archive = extra[size_t(k)].first;
                            L.path = extra[size_t(k)].second;
                            L.repeats = 48;
                            lok = lok && load_ground_texture(*gd.archive(L.archive), L.path, L.image, &err);
                        }
                        CHECK(lok, "decode the extra ground textures for %s: %s", nm.c_str(), err.c_str());
                        g.init_from_colormap(sm.colormap);
                        uint8_t* d = g.mutable_weights();
                        // MA2SE_P4_CENTER=fx,fy (0..1 of the map): where the quadrants meet and the disc sits (default the middle).
                        float pcx = 0.5f, pcy = 0.5f;
                        { char cb[64]; if (GetEnvironmentVariableA("MA2SE_P4_CENTER", cb, sizeof cb)) sscanf(cb, "%f,%f", &pcx, &pcy); }
                        for (int y = 0; y < g.h; y++)
                            for (int x = 0; x < g.w; x++) {
                                uint8_t* tx = d + (size_t(y) * g.w + x) * 8;
                                std::fill(tx, tx + 8, uint8_t(0));
                                int q = (((x + 0.5f) / g.w >= pcx) + 2 * ((y + 0.5f) / g.h >= pcy) + 2) % 4;
                                float cx = (x + 0.5f) / g.w - pcx, cy = (y + 0.5f) / g.h - pcy;
                                float r = std::sqrt(cx * cx + cy * cy);
                                float ring = std::clamp((0.12f - r) / 0.03f, 0.f, 1.f);
                                if (nl > 4) {  // N pie slices around the centre
                                    float ang = std::atan2(cy, cx) + 3.14159265f;
                                    tx[std::min(size_t(ang / 6.2831853f * float(nl)), nl - 1)] = 255;
                                    continue;
                                }
                                tx[q] = uint8_t(std::lround(255 * (1 - ring)));
                                tx[3] = uint8_t(std::min(255, tx[3] + int(std::lround(255 * ring))));
                            }
                        sm.colormap_custom = true;
                        fs::create_directories(fs::u8path(dirp));
                        fs::path outf = fs::u8path(dirp) / fs::u8path(nm + (nl > 4 ? "_layers" + std::to_string(nl) : std::string("_layers")) + ".ma2map");
                        SaveReport mrep;
                        CHECK(io.save(sm, outf, &mrep, &err), "save %s layers map: %s", nm.c_str(), err.c_str());
                        printf("[layers] wrote %s (%s, colour map %dx%d, weights %dx%d)\n", outf.string().c_str(), nm.c_str(), sm.colormap.w, sm.colormap.h, g.w, g.h);
                    }
                }
            }
            // 0. Custom-size water: the Description carries the size, the archive a model of that size, and
            // a reload shows the template's own model again.
            {
                NewMapParams wp;
                wp.start = NewMapParams::CopyRetail;
                wp.copy_map = "UnderCover";
                wp.multiplayer = true;
                wp.name = "Water test";
                Scenario sw;
                bool wok = io.create(wp, sw, &err);
                CHECK(wok, "copy UnderCover for the water test: %s", err.c_str());
                uint32_t wt = 0;
                for (auto& ti : cat.all()) if (ti.cls_name == "MAWater" && ti.name == "Water_1000x1000 NEW") { wt = ti.uid; break; }
                CHECK(wt != 0, "water template found");
                if (wok && wt) {
                    CHECK(io.water_resizable(sw, wt), "Water_1000x1000 NEW can be resized");
                    uint32_t root = io.place_template(sw, wt, {300, 10, 600}, 0);
                    for (auto& o : sw.world.objs)
                        if (o.uid() == root) o.set_str("Description", ScenarioIO::water_description("Lake", 300, 120));
                    Bytes arc;
                    SaveReport wrep;
                    bool built = io.build(sw, arc, &wrep, &err);
                    CHECK(built, "build the water test: %s", err.c_str());
                    fs::path wf = fs::temp_directory_path() / L"ma2scn_water_test.ma2map";
                    if (built && write_file(wf, arc)) {
                        MgfReader ar;
                        std::string tm = cat.find(wt)->model;
                        for (char& c : tm) if (c == '\\') c = '/';
                        if (!tm.empty() && tm[0] == '/') tm.erase(0, 1);
                        size_t sl = tm.find_last_of('/');
                        std::string stem = tm.substr(sl + 1, tm.rfind('.') - sl - 1);
                        std::string mdl = tm.substr(0, sl + 1) + "MA2SE_" + stem + "_300x120.MGMODEL";
                        Bytes v;
                        bool have = ar.open(wf, &err) && ar.find(mdl) >= 0 && ar.read_file(mdl + "{water_1000x1000 verts}", v) && ar.find(mdl + "{water_1000x1000 indices}") >= 0;
                        if (!have) for (auto& f : ar.files()) if (f.path.find("MA2SE") != std::string::npos) printf("           archive has %s\n", f.path.c_str());
                        CHECK(have, "the archive holds the 300 x 120 water model");
                        float ww = 0, wl = 0;
                        CHECK(have && wcustom::extent(v, ww, wl) && std::fabs(ww - 300) < 0.01f && std::fabs(wl - 120) < 0.01f, "the saved water vertices are 300 x 120 (%g x %g)", ww, wl);
                        ar.close();
                        Scenario wb;
                        bool lok = io.load(wf, wb, &err);
                        CHECK(lok, "reload the water test: %s", err.c_str());
                        const TemplateInfo* wti = cat.find(wt);
                        int found = 0;
                        if (lok)
                            for (auto& o : wb.world.objs) {
                                float cw, cl;
                                if (o.uid() == root) {
                                    found++;
                                    CHECK(ScenarioIO::water_custom_size(o, cw, cl) && cw == 300 && cl == 120 && ScenarioIO::water_name(o) == "Lake", "reloaded water keeps its name and size (%s)", o.description().c_str());
                                    CHECK(wti && o.str("Corporal3DObj") == wti->model, "reloaded water draws the template's model again (%s)", o.str("Corporal3DObj").c_str());
                                }
                            }
                        CHECK(found == 1, "the reloaded scenario has the water");
                        fs::remove(wf);
                    }
                }
            }
            printf("[water] done\n");
            // 0b. Cropped road piece and foundation (crop.h, playtest 11-01): the saved world points at cut models in the
            // scenario's archive, those load with the model library and stay inside the kept rectangle, and a reload puts
            // the crop and the original paths back.
            {
                NewMapParams mp16;
                mp16.start = NewMapParams::CopyRetail;
                mp16.copy_map = "Mission_16";
                mp16.multiplayer = false;
                mp16.types.clear();
                mp16.name = "Crop test";
                Scenario sc;
                printf("[crop] copy Mission_16\n");
                bool cok = io.create(mp16, sc, &err);
                CHECK(cok, "copy Mission_16 for the crop test: %s", err.c_str());
                const uint32_t K_ROAD_T = key_of("MARoad");
                uint32_t road_uid = 0, base_uid = 0;
                std::string road_def, base_model;
                for (auto& o : sc.world.objs) {
                    if (!road_uid && o.cls == K_ROAD_T && iends_with(o.str("RoadDefinition"), "longStraight.road")) { road_uid = o.uid(); road_def = o.str("RoadDefinition"); }
                    if (!base_uid && o.str("Corporal3DObj").find("Bb_34x34_Generic01_patternbrick") != std::string::npos) { base_uid = o.uid(); base_model = o.str("Corporal3DObj"); }
                }
                CHECK(road_uid && base_uid, "Mission_16 has a long straight road piece (%u) and a pattern-brick building base (%u)", road_uid, base_uid);
                const crop::Rect rr{-13, 13, -40, -5}, br{-19, 6, -19, 11};  // road: 35 m of its 60; base: 25 x 30 of 38 x 38
                if (cok && road_uid && base_uid) {
                    for (auto& o : sc.world.objs) {
                        if (o.uid() == road_uid) crop::set(o, &rr);
                        if (o.uid() == base_uid) crop::set(o, &br);
                    }
                    Bytes arc;
                    SaveReport crep;
                    printf("[crop] build\n");
                    bool built = io.build(sc, arc, &crep, &err);
                    CHECK(built, "build the crop test: %s", err.c_str());
                    for (auto& wmsg : crep.warnings) if (wmsg.find("cropped") != std::string::npos) CHECK(false, "crop warning: %s", wmsg.c_str());
                    fs::path cf = fs::temp_directory_path() / L"ma2scn_crop_test.ma2map";
                    if (built && write_file(cf, arc)) {
                        MgfReader ar;
                        Wdf sw;
                        Bytes wb;
                        CHECK(ar.open(cf, &err) && ar.read_file("world.wdf", wb) && sw.parse(wb), "read the saved crop world");
                        std::string saved_road, saved_base;
                        bool prop_left = false;
                        for (auto& o : sw.objs) {
                            if (o.uid() == road_uid) saved_road = o.str("RoadDefinition");
                            if (o.uid() == base_uid) saved_base = o.str("Corporal3DObj");
                            prop_left |= o.find(crop::kProp) != nullptr;
                        }
                        CHECK(!prop_left, "the saved world has no EditorCrop property");
                        CHECK(saved_road.find("MA2SE_longStraight_c") != std::string::npos && saved_road[0] != '\\', "saved road points at the cut .road (%s)", saved_road.c_str());
                        CHECK(saved_base.find("MA2SE_Bb_34x34_Generic01_patternbrick_c") != std::string::npos && saved_base[0] == '\\', "saved base points at the cut model (%s)", saved_base.c_str());
                        ar.close();
                        printf("[crop] load the cut models\n");
                        // The cut files load like retail ones (scenario archive first, then Base and common, as the game mounts them).
                        {  // the archive set holds the file open until the block ends
                        ma2m::ArchiveSet set;
                        set.Add(path_utf8(cf));
                        for (const char* an : {"mission_16.mgf", "common.mgf", "mission_02.mgf"}) set.Add(path_utf8(gd.game_files() / widen(an)));
                        Bytes rb;
                        std::string road_txt;
                        if (set.Read(ma2m::NormalizePath(saved_road), rb)) road_txt.assign(rb.begin(), rb.end());
                        CHECK(!road_txt.empty() && road_txt.find("MA2SE_") != std::string::npos, "the cut .road names a cut model");
                        CHECK(road_txt.find("point1\t  = 13.000000, 0.000000, -5.000000") != std::string::npos || road_txt.find("-5.000000") != std::string::npos,
                              "the cut .road boundary starts at the cut");
                        std::string rmodel;
                        for (auto& line : split(road_txt, '\n'))
                            if (size_t eq = line.find('='); eq != std::string::npos && lower(trim(line.substr(0, eq))) == "model") rmodel = ma2m::ResolveRelative(ma2m::NormalizePath(saved_road), trim(line.substr(eq + 1)));
                        {   // The game's INI reader ends a line only at '\r', so every line of the cut .road, .node and .mesh ends "\r\n"
                            auto bare_lf = [](const std::string& t) {
                                for (size_t i = 0; i < t.size(); i++)
                                    if (t[i] == '\n' && (i == 0 || t[i - 1] != '\r')) return true;
                                return false;
                            };
                            Bytes nb, mb2;
                            std::string node_txt, mesh_txt, mesh_path;
                            if (!rmodel.empty() && set.Read(rmodel, nb)) node_txt.assign(nb.begin(), nb.end());
                            for (auto& line : split(node_txt, '\n'))
                                if (size_t eq = line.find('='); eq != std::string::npos && lower(trim(line.substr(0, eq))) == "mesh") {
                                    std::string v = trim(line.substr(eq + 1));
                                    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
                                    mesh_path = ma2m::ResolveRelative(rmodel, v);
                                }
                            if (!mesh_path.empty() && set.Read(mesh_path, mb2)) mesh_txt.assign(mb2.begin(), mb2.end());
                            CHECK(!road_txt.empty() && !node_txt.empty() && !mesh_txt.empty() && !bare_lf(road_txt) && !bare_lf(node_txt) && !bare_lf(mesh_txt),
                                  "the cut .road, .node and .mesh end every line with CR LF (road %d node %d mesh %d bytes)", int(road_txt.size()), int(node_txt.size()), int(mesh_txt.size()));
                            CHECK(road_txt.find("\r\n[attachments]\r\nnPoints") != std::string::npos, "the cut .road's [attachments] nPoints is a line of its own");
                        }
                        for (auto& [path, r] : std::vector<std::pair<std::string, crop::Rect>>{{rmodel, rr}, {ma2m::NormalizePath(saved_base), br}}) {
                            ma2m::Model m;
                            ma2m::ModelLoadOptions mo;
                            mo.apply_mtb = false;
                            std::string merr;
                            bool lm = ma2m::LoadModel(set, path, m, mo, &merr);
                            size_t tris = 0;
                            bool inside = true;
                            for (auto& sm : m.meshes) {
                                tris += sm.indices.size() / 3;
                                std::vector<float> pos, nrm;
                                ma2m::BakeSubMesh(sm, pos, nrm);
                                for (size_t i = 0; i + 2 < pos.size(); i += 3)
                                    inside = inside && pos[i] >= r.x0 - 0.01f && pos[i] <= r.x1 + 0.01f && pos[i + 2] >= r.z0 - 0.01f && pos[i + 2] <= r.z1 + 0.01f;
                            }
                            CHECK(lm && m.failed_meshes == 0 && tris >= 2, "cut model %s loads (%zu triangles) %s", path.c_str(), tris, merr.c_str());
                            CHECK(inside && m.has_bbox && std::fabs(m.bbox_min[0] - r.x0) < 0.05f && std::fabs(m.bbox_max[2] - r.z1) < 0.05f,
                                  "cut model %s fills the kept rectangle (x %.2f..%.2f z %.2f..%.2f)", path.c_str(), m.bbox_min[0], m.bbox_max[0], m.bbox_min[2], m.bbox_max[2]);
                        }
                        }
                        printf("[crop] reload\n");
                        Scenario cb;
                        bool lok = io.load(cf, cb, &err);
                        CHECK(lok, "reload the crop test: %s", err.c_str());
                        int back = 0;
                        for (auto& o : cb.world.objs) {
                            crop::Rect r;
                            if (o.uid() == road_uid) back += crop::of(o, r) && std::fabs(r.z0 - rr.z0) < 0.01f && o.str("RoadDefinition") == road_def;
                            if (o.uid() == base_uid) back += crop::of(o, r) && std::fabs(r.x1 - br.x1) < 0.01f && o.str("Corporal3DObj") == base_model;
                        }
                        CHECK(back == 2, "reload restores both crops and the original paths (%d of 2)", back);
                        std::error_code rec;
                        fs::remove(cf, rec);
                    }
                    // A legacy .NODE plate whose node scales its mesh (Env_ParkingLot_30x30: scale 0.5, mesh +-30 m): the cut is
                    // in model space, so the planes go through the node transform.
                    {
                        ma2m::ArchiveSet src;
                        for (const char* an : {"gocommando.mgf", "common.mgf"}) src.Add(path_utf8(gd.game_files() / widen(an)));
                        std::vector<crop::OutFile> files;
                        std::string cut, cerr;
                        const crop::Rect pr{-15, 5, -10, 15};
                        bool made = crop::make_model(src, "Models/Environment/BuildingBases/ParkingLots/Env_ParkingLot_30x30.NODE", pr, files, cut, &cerr);
                        CHECK(made && files.size() == 4, "cut the parking lot node (%zu files) %s", files.size(), cerr.c_str());
                        MgfWriter mw;
                        for (auto& f : files) mw.add(f.path, f.data);
                        fs::path pf = fs::temp_directory_path() / L"ma2scn_crop_node.mgf";
                        if (made && write_file(pf, mw.build())) {
                            {
                                ma2m::ArchiveSet set;
                                set.Add(path_utf8(pf));
                                for (const char* an : {"gocommando.mgf", "common.mgf"}) set.Add(path_utf8(gd.game_files() / widen(an)));
                                ma2m::Model m;
                                ma2m::ModelLoadOptions mo;
                                mo.apply_mtb = false;
                                bool lm = ma2m::LoadModel(set, cut, m, mo, &cerr);
                                CHECK(lm && m.failed_meshes == 0 && m.has_bbox && std::fabs(m.bbox_min[0] - pr.x0) < 0.05f && std::fabs(m.bbox_max[0] - pr.x1) < 0.05f &&
                                          std::fabs(m.bbox_min[2] - pr.z0) < 0.05f && std::fabs(m.bbox_max[2] - pr.z1) < 0.05f,
                                      "cut parking lot loads to the kept rectangle (x %.2f..%.2f z %.2f..%.2f) %s", m.bbox_min[0], m.bbox_max[0], m.bbox_min[2], m.bbox_max[2], cerr.c_str());
                            }
                            std::error_code rec;
                            fs::remove(pf, rec);
                        }
                    }
                }
            }
            // Cut models keep their collision as separate boxes (clip_volumes) and the cut test (cuttable) matches the save.
            {
                auto in_set = [&](ma2m::ArchiveSet& set, const char* model) {
                    set.Add(path_utf8(gd.game_files() / L"common.mgf"));
                    if (set.Exists(model)) return true;
                    for (auto& e : fs::directory_iterator(gd.game_files()))
                        if (e.path().extension() == L".mgf" && !iequals(path_utf8(e.path().filename()), "common.mgf")) set.Add(path_utf8(e.path()));
                    return set.Exists(model);
                };
                const char* kBridge = "Models/Roads/Bridges/Env_WideBridge02/Env_WideBridge02.MGMODEL";
                const char* kWall = "Models/Environment/Walls/WallMedium/Env_MedWall_Span/Env_MedWall_Span.MGMODEL";
                const char* kBase = "Models/Environment/BuildingBases/Bb_34x34_Generic01_patternbrick/Bb_34x34_Generic01_patternbrick.MGMODEL";
                ma2m::ArchiveSet set;
                CHECK(in_set(set, kBridge) && set.Exists(kWall) && set.Exists(kBase), "the bridge, wall span and base models are on the disc");
                // The z range of box n (model axes, as every retail box is aligned): origin +- the half length along the axis that points along z.
                auto box_z = [](const std::string& x, int n, float& lo, float& hi) {
                    size_t p = 0;
                    for (int i = 0; i <= n; i++) { p = x.find("<bv_obb", p); if (p == std::string::npos) return false; if (i < n) p++; }
                    const size_t e = x.find('>', p);
                    float o[3], len[3], ax[3][3];
                    auto g3 = [&](const char* name, float* v) { std::string k = std::string(" ") + name + "=\""; size_t a = x.find(k, p); return a != std::string::npos && a < e && sscanf(x.c_str() + a + k.size(), "%f, %f, %f", &v[0], &v[1], &v[2]) == 3; };
                    if (!g3("origin", o) || !g3("lengths", len) || !g3("xaxis", ax[0]) || !g3("yaxis", ax[1]) || !g3("zaxis", ax[2])) return false;
                    for (int k = 0; k < 3; k++)
                        if (std::fabs(ax[k][2]) > 0.9f) { lo = o[2] - len[k]; hi = o[2] + len[k]; return true; }
                    return false;
                };
                auto count_boxes = [](const std::string& x) { int n = 0; for (size_t p = x.find("<bv_obb"); p != std::string::npos; p = x.find("<bv_obb", p + 1)) n++; return n; };
                auto cut_xml = [&](const char* model, const crop::Rect& r, std::string& xml, std::string* cutpath, std::vector<crop::OutFile>* all) {
                    std::vector<crop::OutFile> files;
                    std::string cut, cerr;
                    if (!crop::make_model(set, model, r, files, cut, &cerr)) { printf("           make_model: %s\n", cerr.c_str()); return false; }
                    for (auto& f : files) if (f.path == cut) xml.assign(f.data.begin(), f.data.end());
                    if (cutpath) *cutpath = cut;
                    if (all) *all = std::move(files);
                    return !xml.empty();
                };
                {  // T1.2 the bridge: 10 boxes, each clipped on its own
                    std::string x1, x2;
                    crop::Rect r1{-13.f, 13.f, -60.f, 0.55f}, r2{-13.f, 13.f, -30.f, 0.55f};
                    const bool ok1 = cut_xml(kBridge, r1, x1, nullptr, nullptr), ok2 = cut_xml(kBridge, r2, x2, nullptr, nullptr);
                    float lo = 0, hi = 0, blo = 0, bhi = 0;
                    CHECK(ok1 && ok2 && count_boxes(x1) == 10 && count_boxes(x2) == 10, "T1.2 the cut bridge keeps its 10 collision boxes (%d, %d)", ok1 ? count_boxes(x1) : -1, ok2 ? count_boxes(x2) : -1);
                    CHECK(ok1 && box_z(x1, 1, lo, hi) && std::fabs(lo + 60.f) < 0.01f && std::fabs(hi - 0.55f) < 0.01f, "T1.2 the deck box reads z %.2f..%.2f (-60..0.55)", lo, hi);
                    CHECK(ok1 && box_z(x1, 0, blo, bhi) && std::fabs(blo + 51.38f) < 0.05f && std::fabs(bhi + 35.02f) < 0.05f, "T1.2 the support block (z -51.4..-35.0) is unchanged by the 60 m cut (%.2f..%.2f)", blo, bhi);
                    CHECK(ok2 && box_z(x2, 0, blo, bhi) && bhi - blo < 0.02f && blo >= -30.01f && bhi <= 0.56f, "T1.2 cut to z -30..0.55 the support block is 5 mm and inside (%.3f..%.3f)", blo, bhi);
                    CHECK(ok2 && box_z(x2, 1, lo, hi) && std::fabs(lo + 30.f) < 0.01f, "T1.2 cut to 30 m the deck box starts at z -30 (%.2f)", lo);
                    // The cut model reloads.
                    std::vector<crop::OutFile> files;
                    std::string cut, dummy;
                    if (cut_xml(kBridge, r1, dummy, &cut, &files)) {
                        MgfWriter mw;
                        for (auto& f : files) mw.add(f.path, f.data);
                        fs::path pf = fs::temp_directory_path() / L"ma2scn_r5_bridge.mgf";
                        if (write_file(pf, mw.build())) {
                            {
                                ma2m::ArchiveSet s2;
                                s2.Add(path_utf8(pf));
                                for (size_t i = 0; i < set.Count(); i++) s2.Add(set.ArchivePath(i));
                                ma2m::Model m;
                                ma2m::ModelLoadOptions mo;
                                mo.apply_mtb = false;
                                std::string lerr;
                                const bool lm = ma2m::LoadModel(s2, cut, m, mo, &lerr);
                                CHECK(lm && m.failed_meshes == 0 && m.has_bbox && m.bbox_min[2] >= -60.05f, "T1.2 the cut bridge reloads (z from %.2f) %s", m.bbox_min[2], lerr.c_str());
                            }
                            std::error_code rec;
                            fs::remove(pf, rec);
                        }
                    }
                }
                {  // T1.3 a wall span cut to half its long axis
                    ma2m::Model whole;
                    ma2m::ModelLoadOptions mo;
                    mo.apply_mtb = false;
                    std::string lerr;
                    if (ma2m::LoadModel(set, kWall, whole, mo, &lerr) && whole.has_bbox) {
                        const float mid = 0.5f * (whole.bbox_min[0] + whole.bbox_max[0]);
                        crop::Rect r{whole.bbox_min[0] - 0.05f, mid, whole.bbox_min[2] - 0.05f, whole.bbox_max[2] + 0.05f};
                        std::vector<crop::OutFile> files;
                        std::string cut, xml;
                        const bool ok = cut_xml(kWall, r, xml, &cut, &files);
                        CHECK(ok, "T1.3 make_model cuts the wall span to half");
                        if (ok) {
                            MgfWriter mw;
                            for (auto& f : files) mw.add(f.path, f.data);
                            fs::path pf = fs::temp_directory_path() / L"ma2scn_r5_wall.mgf";
                            if (write_file(pf, mw.build())) {
                                {
                                    ma2m::ArchiveSet s2;
                                    s2.Add(path_utf8(pf));
                                    for (size_t i = 0; i < set.Count(); i++) s2.Add(set.ArchivePath(i));
                                    ma2m::Model m;
                                    const bool lm = ma2m::LoadModel(s2, cut, m, mo, &lerr);
                                    CHECK(lm && m.has_bbox && std::fabs(m.bbox_max[0] - mid) < 0.05f && std::fabs(m.bbox_min[0] - whole.bbox_min[0]) < 0.05f,
                                          "T1.3 the cut wall reloads to the kept half (x %.2f..%.2f of %.2f..%.2f) %s", m.bbox_min[0], m.bbox_max[0], whole.bbox_min[0], whole.bbox_max[0], lerr.c_str());
                                }
                                std::error_code rec;
                                fs::remove(pf, rec);
                            }
                        }
                    } else CHECK(false, "T1.3 load the wall span: %s", lerr.c_str());
                }
                {  // T1.4 the cut test
                    std::string why;
                    CHECK(crop::cuttable(set, kBridge, &why) && crop::cuttable(set, kWall, &why) && crop::cuttable(set, kBase, &why), "T1.4 the bridge, the wall span and the base can be cut (%s)", why.c_str());
                    CHECK(!crop::cuttable(set, "Models/Roads/Bridges/Not_There.MGMODEL", &why) && why.find("not in the game's files") != std::string::npos, "T1.4 a missing model: %s", why.c_str());
                    const std::string shadow = "<xmlres>\n<node_generic name=\"a\">\n<shadow_mesh name=\"s\"/>\n</node_generic>\n</xmlres>\n";
                    MgfWriter mw;
                    mw.add("Models/Test/Shadowed.MGMODEL", Bytes(shadow.begin(), shadow.end()));
                    fs::path pf = fs::temp_directory_path() / L"ma2scn_r5_shadow.mgf";
                    if (write_file(pf, mw.build())) {
                        {
                            ma2m::ArchiveSet s2;
                            s2.Add(path_utf8(pf));
                            const bool c = crop::cuttable(s2, "Models/Test/Shadowed.MGMODEL", &why);
                            CHECK(!c && why.find("shadow volumes") != std::string::npos, "T1.4 a model with shadow volumes is refused (%s)", why.c_str());
                        }
                        std::error_code rec;
                        fs::remove(pf, rec);
                    }
                }
            }
            // 1. Bogged Down copy with one visible change: a raised plateau in the middle.
            NewMapParams cp;
            cp.start = NewMapParams::CopyRetail;
            cp.copy_map = "BoggedDown";
            cp.name = "Bogged Down - Plateau";
            Scenario s1;
            bool ok = io.create(cp, s1, &err);
            CHECK(ok, "copy BoggedDown: %s", err.c_str());
            if (ok) {
                s1.description = "Bogged Down with a 40 m plateau raised in the middle of the map (editor sample).";
                s1.author = "Scenario Editor sample";
                float cx = s1.terrain.size_x() * 0.5f, cz = s1.terrain.size_z() * 0.5f;
                for (int gz = 0; gz < s1.terrain.h; gz++)
                    for (int gx = 0; gx < s1.terrain.w; gx++) {
                        float dx = gx * s1.terrain.scale - cx, dz = gz * s1.terrain.scale - cz, d = std::sqrt(dx * dx + dz * dz);
                        if (d < 120.f) {
                            float f = std::clamp((120.f - d) / 40.f, 0.f, 1.f);
                            float& h = s1.terrain.ref(gx, gz);
                            h = std::max(h, h + (40.f - h) * f);
                        }
                    }
                io.bake_lightmap(s1);
                report("BoggedDown copy", s1, dir / L"BoggedDown_Plateau.ma2map");
            }
            // Sky picker: the catalog, the Base level's own sky, and a sky from another level's archive.
            {
                double ts = now_s();
                auto& skies = io.skies();
                printf("[sky] %zu skies in %.2fs:", skies.size(), now_s() - ts);
                for (auto& si : skies) printf(" %s(%zu models, %zu uses)", si.name.c_str(), si.models.size(), si.uses.size());
                printf("\n");
                const char* want[] = {"Env_SkyDusk02", "Env_Sky_CityNight", "Env_SkyNOR_01", "Env_SkyNOR_02", "Env_SkyHesperus_01",
                                      "Env_SkyHesperus_02", "Env_SkyAurora_01", "Env_Sky_StormFront_01", "Env_Sky_Stormy", "En_Sky_WarmSun",
                                      "Env_SkySpace", "Env_Planet"};
                int found = 0;
                for (const char* w : want) {
                    bool f = io.find_sky(w) != nullptr;
                    if (!f) for (auto& si : skies) f |= iequals(si.folder, w);
                    if (f) found++;
                    else printf("           missing sky %s\n", w);
                }
                CHECK(skies.size() >= 12 && found == 12, "sky catalog (%zu skies, %d of 12 named ones)", skies.size(), found);
                NewMapParams sp;
                sp.start = NewMapParams::CopyRetail;
                sp.copy_map = "BoggedDown";
                sp.name = "Sky Test";
                Scenario sk;
                if (io.create(sp, sk, &err)) {
                    std::string base_sky = io.base_sky_name(sk);
                    CHECK(base_sky == "Env_SkyDusk02" && io.sky_name(sk) == base_sky, "Bogged Down's own sky (%s)", base_sky.c_str());
                    int si = world::find_class(sk.world, world::K_SKY), ei = world::find_class(sk.world, world::K_ENV);
                    WObj before = sk.world.objs[size_t(si)];
                    uint32_t fog_before = sk.world.objs[size_t(ei)].u32("FogColor");
                    bool ok = io.set_sky(sk, "Env_SkyAurora_01", true, &err);
                    const WObj& now = sk.world.objs[size_t(si)];
                    printf("[sky] Sky_Test: %s | %s | scale %.0f,%.0f,%.0f offset %.0f | fog %08X -> %08X\n", now.str("Corporal3DObj").c_str(),
                           now.str("CorporalMTB").c_str(), now.vec3("SkyTerrainSkyScale").x, now.vec3("SkyTerrainSkyScale").y,
                           now.vec3("SkyTerrainSkyScale").z, now.vec3("SkyTerrainSkyOffset").y, fog_before,
                           sk.world.objs[size_t(ei)].u32("FogColor"));
                    CHECK(ok && iends_with(now.str("Corporal3DObj"), "Env_SkyAurora_01_MP.MGMODEL") && io.sky_name(sk) == "Env_SkyAurora_01",
                          "set_sky Env_SkyAurora_01 (MP model): %s", err.c_str());
                    // Back to the Base level's own sky restores its MASky2 exactly.
                    Scenario back = sk;
                    io.set_sky(back, base_sky, true, &err);
                    const WObj& rest = back.world.objs[size_t(si)];
                    bool same = rest.props.size() == before.props.size();
                    for (size_t k = 0; same && k < rest.props.size(); k++) same = rest.props[k].key == before.props[k].key && rest.props[k].data == before.props[k].data;
                    CHECK(same && back.world.objs[size_t(ei)].u32("FogColor") == fog_before, "restoring the Base sky and fog");
                    sk.description = "Bogged Down with New River City's aurora sky (Env_SkyAurora_01_MP) and its fog colour (editor sample).";
                    sk.author = "Scenario Editor sample";
                    fs::path out = dir / L"Sky_Test.ma2map";
                    report("sky test", sk, out);
                    MgfReader mr;
                    Bytes ib;
                    if (mr.open(out) && mr.read_file("scenario.ini", ib)) {
                        std::string ini = lower(std::string(ib.begin(), ib.end()));
                        int n = 0;
                        for (size_t q = ini.find("newrivercity.mgf"); q != std::string::npos; q = ini.find("newrivercity.mgf", q + 1)) n++;
                        CHECK(ini.find("env_skyaurora_01_mp.mgmodel") != std::string::npos, "sky model listed in [Assets]");
                        printf("[sky] Sky_Test [Assets]: %d entries from newrivercity.mgf\n", n);
                    }
            // Starry space on a multiplayer base. The first archive holding the sky (cinema_2_1) has only Env_SkySpace.NODE, which
            // [Assets] ships; the saved world must name that file (it named the MGMODEL nothing shipped and the game drew no sky at all).
            {
                NewMapParams sp;
                sp.start = NewMapParams::CopyRetail;
                sp.copy_map = "BoggedDown";
                sp.name = "Space Sky Test";
                Scenario ss;
                std::string serr;
                if (io.create(sp, ss, &serr) && io.set_sky(ss, "Env_SkySpace", false, &serr)) {
                    fs::path out = dir / L"Space_Sky_Test.ma2map";
                    SaveReport srep;
                    const bool saved = io.save(ss, out, &srep, &serr);
                    MgfReader mr;
                    Bytes wb, ib;
                    Wdf w;
                    std::string model, ini;
                    if (saved && mr.open(out) && mr.read_file("world.wdf", wb) && w.parse(wb) && mr.read_file("scenario.ini", ib)) {
                        int si = world::find_class(w, world::K_SKY);
                        if (si >= 0) model = w.objs[size_t(si)].str("Corporal3DObj");
                        ini = lower(std::string(ib.begin(), ib.end()));
                    }
                    CHECK(iends_with(model, "Env_SkySpace.NODE") && ini.find("models/environment/sky/env_skyspace/env_skyspace.node") != std::string::npos,
                          "Starry space saves the sky file [Assets] ships (%s)", model.c_str());
                    std::error_code ec;
                    fs::remove(out, ec);
                } else {
                    CHECK(false, "Starry space test map: %s", serr.c_str());
                }
            }
            // The dust storm's effect files (template 31516 names a .node and .mtb no archive holds; PC Release builds the effect
            // from these and the lighting stays normal).
            {
                std::vector<std::pair<std::string, Bytes>> dust;
                std::string derr;
                const bool ok = weather::dust_files(gd, dust, &derr);
                auto has = [&](const char* p) {
                    for (auto& [k, b] : dust) if (iequals(k, p)) return &b;
                    return static_cast<Bytes*>(nullptr);
                };
                const Bytes* mtb = has("SpecialEffects/Environment/Env_DustStorm_Wst/Env_DustStorm_Wst.mtb");
                const Bytes* node = has("SpecialEffects/Environment/Env_DustStorm_Wst/Env_DustStorm_Wst.node");
                const Bytes* grit = has("SpecialEffects/Environment/Env_Dust_01.mgparticle");
                std::string ms = mtb ? std::string(mtb->begin(), mtb->end()) : std::string(), gs = grit ? std::string(grit->begin(), grit->end()) : std::string();
                CHECK(ok && dust.size() == 6 && mtb && node && grit && has("SpecialEffects/Environment/Env_DustHaze_01.mgparticle"),
                      "dust storm files (%zu): %s", dust.size(), derr.c_str());
                CHECK(ms.find("Env_Dust_01.mgparticle") != std::string::npos && ms.find("Env_DustHaze_01.mgparticle") != std::string::npos &&
                          ms.find("Env_Snow") == std::string::npos && mtb->size() == 3056,
                      "the dust .mtb names the dust particles, same size as the snow .mtb (%zu)", mtb ? mtb->size() : size_t(0));
                CHECK(gs.find("176, 138, 92, ") != std::string::npos && gs.find("255, 255, 255, ") == std::string::npos, "the grit particle is brown");
            }
            // Lamp parts and light cones on the three models of the frames; the lightning gate and bursts.
            {
                ma2m::ArchiveSet set;
                set.Add(path_utf8(gd.game_files() / L"common.mgf"));
                for (auto& e : fs::directory_iterator(gd.game_files()))
                    if (e.path().extension() == L".mgf" && !iequals(path_utf8(e.path().filename()), "common.mgf")) set.Add(path_utf8(e.path()));
                auto load = [&](const char* path, ma2m::Model& m) {
                    ma2m::ModelLoadOptions mo;
                    mo.include_invisible = true;
                    std::string lerr;
                    return ma2m::LoadModel(set, path, m, mo, &lerr);
                };
                auto lamp_drawn = [](const ma2m::Model& m, int node) {  // the editor's use of lamp_part_drawn (model_cache.cpp)
                    std::vector<char> holds(m.nodes.size(), 0);
                    for (size_t i = 0; i < m.nodes.size(); i++)
                        if (m.nodes[i].is_light) for (int p = int(i); p >= 0; p = m.nodes[size_t(p)].parent) holds[size_t(p)] = 1;
                    std::vector<LampNode> path;
                    for (int n = node; n >= 0; n = m.nodes[size_t(n)].parent) path.push_back({m.nodes[size_t(n)].hidden_by_mtb, m.nodes[size_t(n)].turned_on, holds[size_t(n)] != 0});
                    return lamp_part_drawn(path);
                };
                auto mesh_named = [](const ma2m::Model& m, const char* name) -> const ma2m::SubMesh* {
                    for (auto& sm : m.meshes) if (iequals(sm.name, name)) return &sm;
                    return nullptr;
                };
                ma2m::Model canal, bridge, lamp;
                const bool lc = load("Models/Environment/Walls/Canal/Canal_Wall_ST/Canal_Wall_ST.MGMODEL", canal);
                const bool lb = load("Models/Roads/Bridges/Env_PedBridgeB/Env_PedBridgeB/Env_PedBridgeB.MGMODEL", bridge);
                const bool ll = load("Models/Environment/Parkway/Env_Highway_Light/Env_Highway_Light.MGMODEL", lamp);
                CHECK(lc && lb && ll, "Canal_Wall_ST, Env_PedBridgeB and Env_Highway_Light load");
                const ma2m::SubMesh* vcone = lc ? mesh_named(canal, "vcone mesh") : nullptr;
                const ma2m::SubMesh* bulb = ll ? mesh_named(lamp, "lightbulb mesh") : nullptr;
                const ma2m::SubMesh* hcone = ll ? mesh_named(lamp, "cone mesh") : nullptr;
                const ma2m::SubMesh* blight = lb ? mesh_named(bridge, "point_light01 mesh") : nullptr;
                CHECK(vcone && bulb && hcone && blight, "the lamp meshes exist");
                if (vcone && bulb && hcone && blight) {
                    // Canal wall cone: a plain visible additive mesh, no node_light, no .mtb rule: the lamp-part rule does not touch it.
                    CHECK(vcone->visible && !lamp_drawn(canal, vcone->node_index), "Canal_Wall_ST vcone is a plain visible mesh, not a lamp part");
                    const ma2m::Material& vm = canal.materials[size_t(vcone->material)];
                    CHECK(vm.alpha == ma2m::AlphaMode::Additive && !vm.two_sided && cull_back_faces(vm.two_sided, false),
                          "the canal cone is additive and single-sided, so it is back-face culled");
                    // Highway lamp: lightbulb and cone hidden by the .mtb under a node holding the spotlight: drawn as lamp parts (fallback).
                    CHECK(!bulb->visible && lamp_drawn(lamp, bulb->node_index) && lamp_drawn(lamp, hcone->node_index), "Env_Highway_Light bulb and cone are lamp parts (drawn)");
                    // Pedestrian bridge B: its 'lights' group is hidden by the .mtb and holds no node_light: never drawn (game: no glow).
                    CHECK(!blight->visible && !lamp_drawn(bridge, blight->node_index), "Env_PedBridgeB lights stay hidden");
                    // The two lamp columns of one canal section: from the G0 spot-A eye (494, 13.6, 343; section at 599.7, 6.07, 296.3,
                    // heading 0.08) the near bank's cones turn their back to the eye and the far bank's face it.
                    std::vector<float> pos, nrm;
                    ma2m::BakeSubMesh(*vcone, pos, nrm);
                    const float eye[3] = {494.f - 599.71f, 13.6f - 6.07f, 343.f - 296.29f};
                    int front[2] = {0, 0}, total[2] = {0, 0};
                    for (size_t t = 0; t + 2 < vcone->indices.size(); t += 3) {
                        const float* a = &pos[vcone->indices[t] * 3];
                        const float* b = &pos[vcone->indices[t + 1] * 3];
                        const float* c = &pos[vcone->indices[t + 2] * 3];
                        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                        const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                        const float cx = (a[0] + b[0] + c[0]) / 3;
                        const int bank = std::fabs(cx - eye[0]) < 70.f ? 0 : 1;  // 0 = the near bank (the bridge foot in G0), 1 = the far bank
                        // D3D11 default: front = clockwise on screen = this normal toward the eye (left-handed world)
                        const float d = n[0] * (eye[0] - a[0]) + n[1] * (eye[1] - a[1]) + n[2] * (eye[2] - a[2]);
                        total[bank]++;
                        if (d > 0) front[bank]++;
                    }
                    printf("[f56e2] canal cones from spot A: near bank %d / %d triangles face the eye, far bank %d / %d\n", front[0], total[0], front[1], total[1]);
                    CHECK(total[0] > 0 && total[1] > 0 && front[0] * 5 < total[0] && front[1] * 5 > total[1] * 4,
                          "near-bank cones are back faces from spot A (%d/%d front), far-bank cones front faces (%d/%d)", front[0], total[0], front[1], total[1]);
                }
                // The flash lights lit model draws only; bursts as the game's at 16 frames/s.
                CHECK(weather::flash_lights(weather::FlashDraw::LitModel) && !weather::flash_lights(weather::FlashDraw::Terrain) &&
                      !weather::flash_lights(weather::FlashDraw::Water) && !weather::flash_lights(weather::FlashDraw::Sky) && !weather::flash_lights(weather::FlashDraw::UnlitPart),
                      "the lightning flash lights lit models only (not ground, water, sky colour or glow parts)");
                bool runs_ok = true, apart = true, counts_ok = true;
                int max_seen = 0;
                for (uint32_t seed = 1; seed < 400; seed++) {
                    const double len = weather::burst_length(seed, 4);
                    int runs = 0, run = 0, sky = 0;
                    bool prev = false;
                    for (double t = 0; t < len + 0.2; t += 0.005) {
                        const weather::FlashNow f = weather::burst_flash(t, seed, 4);
                        if (f.model && f.sky) apart = false;
                        if (f.sky) sky++;
                        if (f.model) run++;
                        if (!f.model && prev) { runs++; if (run * 0.005 < 0.055 || run * 0.005 > 0.26) runs_ok = false; run = 0; }
                        prev = f.model;
                    }
                    if (runs < 1 || runs > 4 || sky == 0 || len > 2.5) counts_ok = false;
                    max_seen = std::max(max_seen, runs);
                }
                CHECK(runs_ok && apart && counts_ok && max_seen == 4, "bursts of 1-4 flashes of 0.06-0.25 s (1-4 frames at 16/s), one sky flash apart from them (max %d)", max_seen);
            }
            // Looks read the retail level (fog, lightning), the light keeps its name under a hand-picked sky, sky time.
            {
                auto look_named = [&](const char* label) -> const weather::Look* {
                    for (auto& l : weather::looks()) if (iequals(l.label, label)) return &l;
                    return nullptr;
                };
                auto env_of = [&](const char* map) -> const WObj* {
                    const Wdf* w = io.retail_world(map);
                    const int e = w ? world::find_class(*w, world::K_ENV) : -1;
                    return e >= 0 ? &w->objs[size_t(e)] : nullptr;
                };
                const weather::Look* warm = look_named("Warm sun");
                const weather::Look* stormy = look_named("Stormy night");
                CHECK(warm && stormy, "Warm sun and Stormy night looks exist");
                if (warm && stormy) {
                    // look_fog: the look's own distances = its retail level's FogStart / FogEnd (every look, not only Warm sun).
                    for (auto& l : weather::looks()) {
                        float fs = 0, fe = 0;
                        const WObj* e = env_of(l.world);
                        const bool ok = weather::look_fog(io, l, &fs, &fe);
                        CHECK(ok && e && std::fabs(fs - e->f32("FogStart")) < 0.5f && std::fabs(fe - e->f32("FogEnd")) < 0.5f,
                              "look_fog(%s) = %s's FogStart / FogEnd (%.0f-%.0f vs %.0f-%.0f)", l.label, l.world, fs, fe, e ? e->f32("FogStart") : -1.f, e ? e->f32("FogEnd") : -1.f);
                    }
                    Scenario t = sk;
                    std::string e1;
                    CHECK(weather::apply_look(io, t, *stormy, true, &e1), "apply_look(Stormy night): %s", e1.c_str());
                    const WObj* ill = env_of("IllWind");
                    const WObj& te = t.world.objs[size_t(world::find_class(t.world, world::K_ENV))];
                    CHECK(ill && ill->i32("LightFlash") != 0 && te.i32("LightFlash") != 0 && te.u32("FlashTimeMin") == ill->u32("FlashTimeMin") &&
                              te.u32("FlashTimeMax") == ill->u32("FlashTimeMax") && te.u32("MaxFlashes") == ill->u32("MaxFlashes"),
                          "apply_look(Stormy night) sets Ill Wind's lightning (min %u max %u, Ill Wind %u..%u)", te.u32("FlashTimeMin"), te.u32("FlashTimeMax"),
                          ill ? ill->u32("FlashTimeMin") : 0, ill ? ill->u32("FlashTimeMax") : 0);
                    // Rain and snow are never touched by a look.
                    CHECK(weather::current_look_light(io, t) == stormy && weather::current_look(io, t) == stormy, "Stormy night is the look after apply_look");
                    CHECK(weather::apply_look(io, t, *warm, false, &e1), "apply_look(Warm sun): %s", e1.c_str());
                    const WObj& te2 = t.world.objs[size_t(world::find_class(t.world, world::K_ENV))];
                    CHECK(te2.i32("LightFlash") == 0, "apply_look(Warm sun) turns lightning off");
                    // A hand-picked sky: the light keeps its look, the exact match is lost.
                    std::string e2;
                    CHECK(io.set_sky(t, "Env_SkySpace", false, &e2), "set_sky Env_SkySpace: %s", e2.c_str());
                    CHECK(weather::current_look_light(io, t) == warm && weather::current_look(io, t) == nullptr, "current_look_light after a sky change = the same look (Warm sun)");
                    // Sky time: a night look's sky and a dark retail world's sky are night skies; a day look's sky is not.
                    CHECK(io.sky_is_night("Env_SkySpace"), "sky_is_night(Env_SkySpace)");
                    CHECK(!io.sky_is_night(weather::look_sky(*warm)), "sky_is_night(Warm sun's sky %s) is false", weather::look_sky(*warm));
                    CHECK(io.sky_is_night(weather::look_sky(*look_named("City night"))), "sky_is_night(City night's sky)");
                    CHECK(!weather::light_is_night(io, t) && weather::light_is_night(io, [&] { Scenario n = t; weather::apply_look(io, n, *stormy, true, nullptr); return n; }()),
                          "light_is_night: Warm sun no, Stormy night yes");
                    // The base level's own fog is what "Default" brings on this map's own light.
                    float bs = 0, be = 0;
                    const WObj* bge = env_of("BoggedDown");
                    CHECK(weather::own_fog(io, sk, &bs, &be) && bge && std::fabs(bs - bge->f32("FogStart")) < 0.5f && std::fabs(be - bge->f32("FogEnd")) < 0.5f,
                          "own_fog(Bogged Down map) = the level's FogStart / FogEnd (%.0f-%.0f)", bs, be);
                    // The own-light return restores the level's lightning (Bogged Down flashes).
                    CHECK(weather::apply_own_light(io, t, &e2) && (t.world.objs[size_t(world::find_class(t.world, world::K_ENV))].i32("LightFlash") != 0) == (bge && bge->i32("LightFlash") != 0),
                          "apply_own_light brings the level's lightning back");
                }
            }
                }
            }
            // 2. Small map from scratch: 6 Red + 6 Blue spawns, Team Destruction.
            NewMapParams np;
            np.multiplayer = true;
            np.w = np.h = 129;
            np.base = "gocommando.mgf";  // a retail 129x129 level (a 129x129 map on a 257x257 Base failed in game)
            np.start = NewMapParams::Noise;
            np.flat_height = 10.f;
            np.seed = 20261020;
            np.types = {"TeamDestruction"};
            np.name = "Small Skirmish";
            Scenario s2;
            ok = io.create(np, s2, &err);
            CHECK(ok, "create small map: %s", err.c_str());
            if (ok) {
                {   // On foot: the free-for-all slots (StartVehiclesNotIt/LMS/Destruction/Grinder) start in Battle Armor as on retail maps.
                    uint32_t was = io.start_container_raw(s2);
                    io.set_start_container(s2, 123999);
                    std::map<uint32_t, int64_t> slot;
                    if (int ri = world::find_class(s2.world, world::K_ROOT); ri >= 0)
                        for (auto& p : s2.world.objs[size_t(ri)].props) if (p.data.size() == 4) slot[p.key] = p.as_int();
                    CHECK(slot[0xF2845C32u] == 124001 && slot[0x3E1C0123u] == 124001 && slot[0xE297B910u] == 124001 && slot[0xBA4F078Au] == 124001 &&
                              slot[0x749BC5D7u] == 123999 && slot[0xC39D0BEDu] == 123999 && io.start_container(s2) == 123999,
                          "on-foot MP start: Not It/LMS/Destruction/Grinder in Battle Armor, the team types on foot (NotIt %lld TD %lld)",
                          (long long)slot[0xF2845C32u], (long long)slot[0x749BC5D7u]);
                    io.set_start_container(s2, was);
                }
                s2.description = "Small 129x129 map made from scratch on GoCommando: 6 Red and 6 Blue spawns, Team Destruction (editor sample).";
                s2.author = "Scenario Editor sample";
                float W = s2.terrain.size_x(), H = s2.terrain.size_z();
                io.add_spawn_ring(s2, 1, {W * 0.22f, 0, H * 0.22f}, 28.f, 6);
                io.add_spawn_ring(s2, 2, {W * 0.78f, 0, H * 0.78f}, 28.f, 6);
                auto& pads = io.pad_kinds();
                printf("[scenario] %zu vehicle pad kinds in the retail MP maps\n", pads.size());
                float off = 0;
                int pads_placed = 0;
                for (auto& pk : pads)
                    if (pk.map == "GoCommando" && pads_placed < 2) {  // the Base level's own vehicles
                        io.add_pad(s2, pk, {W * 0.30f + off, 0, H * 0.15f}, 0.f);
                        io.add_pad(s2, pk, {W * 0.70f - off, 0, H * 0.85f}, 3.14159f);
                        printf("[scenario] pad kind '%s' (%s) placed twice\n", pk.label.c_str(), pk.map.c_str());
                        off += 25.f;
                        pads_placed++;
                    }
                {   // Check: a free-for-all type that starts on foot with fewer pads than players gets a warning; the on-foot default
                    // writes Battle Armor into Not It's slot, so it gets none.
                    auto types = s2.types;
                    uint32_t was = io.start_container_raw(s2);
                    s2.types = {"NotIt"};
                    io.set_start_container(s2, 123999);
                    auto warned = [&] {
                        for (auto& it : io.validate(s2)) if (it.text.rfind("NotIt: players start on foot", 0) == 0) return true;
                        return false;
                    };
                    bool w_ba = warned();
                    if (int ri = world::find_class(s2.world, world::K_ROOT); ri >= 0)
                        for (auto& p : s2.world.objs[size_t(ri)].props) if (p.key == 0xF2845C32u) p.set_int(123999);
                    bool w_foot = warned();
                    CHECK(!w_ba && w_foot, "Check: Not It on foot with %d pads warns (BA start %d, on foot %d)", pads_placed * 2, int(w_ba), int(w_foot));
                    s2.types = types;
                    io.set_start_container(s2, was);
                }
                // Type map of a generated map: one playable area inside an out-of-bounds edge and a border band.
                {
                    int Wt = s2.typemap_w, Ht = s2.typemap_h;
                    std::map<int, int> hist;
                    for (uint16_t v : s2.typemap) hist[v]++;
                    CHECK(Wt == 512 && Ht == 512 && hist.size() == 3 && hist.count(0) && hist.count(1) && hist.count(2),
                          "generated type map holds 0/1/2 only (%dx%d, %zu values)", Wt, Ht, hist.size());
                    CHECK(s2.typemap[0] == tmtype::OutOfBounds && s2.typemap[size_t(Ht / 2) * Wt + Wt / 2] == tmtype::Playable &&
                              s2.typemap[size_t(5) * Wt + Wt / 2] == tmtype::Border,
                          "generated type map edge 1, band 2, centre 0");
                    printf("[scenario] generated type map: %d playable, %d border, %d out-of-bounds cells\n", hist[0], hist[2], hist[1]);
                    int errors = 0;
                    for (auto& it : io.validate(s2))
                        if (it.level == 2) { errors++; printf("           validate: %s\n", it.text.c_str()); }
                    CHECK(errors == 0, "small map validates (%d errors)", errors);
                    // A spawn on out-of-bounds ground is an error; an out-of-bounds wall splitting the map is a warning.
                    std::vector<uint16_t> keep = s2.typemap;
                    Vec3 sp{};
                    for (auto& o : s2.world.objs) if (o.cls == world::K_LEVELSTART) { world::get_pos(o, sp); break; }
                    int tx = int(sp.x / W * Wt), tz = int(sp.z / H * Ht);
                    s2.typemap[size_t(tz) * Wt + tx] = tmtype::OutOfBounds;
                    int e1 = 0;
                    for (auto& it : io.validate(s2))
                        if (it.level == 2 && it.text.find("stands where players cannot go") != std::string::npos) e1++;
                    s2.typemap = keep;
                    for (int y = 0; y < Ht; y++)
                        for (int x = Wt / 2 - 2; x <= Wt / 2 + 2; x++) s2.typemap[size_t(y) * Wt + x] = tmtype::OutOfBounds;
                    bool split = false;
                    for (auto& it : io.validate(s2))
                        if (it.level == 1 && it.text.find("separate areas") != std::string::npos) split = true;
                    s2.typemap = keep;
                    CHECK(e1 == 1 && split, "ground validation: spawn on type 1 -> %d error(s), split map warned %d", e1, int(split));
                }
                // A few snow trees from another level's archive: exercises [Assets].
                int base_idx = -1;
                for (size_t i = 0; i < gd.level_archives().size(); i++) if (gd.level_archives()[i] == "gocommando.mgf") base_idx = int(i);
                int placed = 0;
                for (auto& t : cat.all()) {
                    if (!t.placeable || t.archives.empty() || lower(t.name).find("conifer02_snow_single") == std::string::npos) continue;
                    if (std::find(t.archives.begin(), t.archives.end(), base_idx) != t.archives.end()) continue;
                    for (int k = 0; k < 4; k++) {
                        float a = k * 1.5708f;
                        io.place_template(s2, t.uid, {W * 0.5f + std::cos(a) * 30, 0, H * 0.5f + std::sin(a) * 30}, a);
                    }
                    printf("[scenario] placed 4 x '%s' (template %u, archive %s)\n", t.name.c_str(), t.uid,
                           gd.level_archives()[size_t(t.archives.front())].c_str());
                    if (++placed == 1) break;
                }
                report("small map", s2, dir / L"Small_TeamDestruction.ma2map");
            }
            // 3. Mission: two enemy mechs, a patrol, an ambush, a message with imported audio, an
            //    objective and the win condition.
            std::string perr;
            bool protos = load_trigger_prototypes(gd, &perr);
            CHECK(protos, "trigger prototypes: %s", perr.c_str());
            printf("[triggers] prototypes loaded; %zu retail sound cues, %zu music cues\n", retail_sound_cues().size(), retail_music_cues().size());
            {   // The cue pickers group by mission and show the subtitle that plays with the cue
                size_t with_words = 0, no_mission = 0;
                bool sorted = true;
                const std::vector<CueInfo>* lists[] = {&retail_sound_cues(), &retail_music_cues(), &retail_voice_cues()};
                for (auto* l : lists) {
                    for (size_t i = 0; i < l->size(); i++) {
                        const CueInfo& c = (*l)[i];
                        if (!c.words.empty()) with_words++;
                        if (c.mission.empty()) no_mission++;
                        if (!c.words.empty() && c.label.find(c.words) == std::string::npos) sorted = false;
                        if (i && (*l)[i - 1].mission > c.mission) sorted = false;
                    }
                }
                CHECK(no_mission == 0 && sorted, "every retail cue names its mission, grouped, and a label carries its words");
                CHECK(with_words > 0, "some retail cues show the subtitle that plays with them (%zu)", with_words);
                size_t voice_words = 0;
                for (auto& c : retail_voice_cues()) if (!c.words.empty()) voice_words++;
                printf("cues with words: %zu (voice %zu of %zu)\n", with_words, voice_words, retail_voice_cues().size());
            }
            NewMapParams mp;
            mp.multiplayer = false;
            mp.w = mp.h = 129;
            mp.base = "mission_05.mgf";
            mp.start = NewMapParams::Noise;
            mp.flat_height = 15.f;
            mp.seed = 5;
            mp.name = "Ambush at the Ridge";
            auto fill_mission = [&](Scenario& s3) {
            s3.description = "Mission sample: two enemy mechs, a patrol, an ambush, a message with imported audio, an objective, win condition.";
            s3.author = "Scenario Editor sample";
            s3.briefing_title = "Ambush at the Ridge";
            s3.briefing = "Two enemy mechs guard the ridge.\nDestroy both of them.";
            s3.objectives = {"Destroy both enemy mechs"};
            float W = s3.terrain.size_x(), H = s3.terrain.size_z();
            uint32_t m1 = io.place_template(s3, 27265, {W * 0.70f, 0, H * 0.70f}, 3.1f);
            uint32_t m2 = io.place_template(s3, 27265, {W * 0.78f, 0, H * 0.62f}, 3.1f);
            for (auto& o : s3.world.objs) if (o.uid() == m1 || o.uid() == m2) o.set_i32("Nation", 5);
            CHECK(m1 && m2, "place enemy mechs");
            // A one-second 440 Hz tone as the imported clip.
            {
                AudioClip clip;
                clip.source = "tone440.wav";
                const int rate = 22050, n = rate;
                Bytes& b = clip.wav;
                auto u32 = [&](uint32_t v) { put(b, v); };
                auto u16 = [&](uint16_t v) { put(b, v); };
                put_bytes(b, "RIFF", 4); u32(36 + n * 2); put_bytes(b, "WAVEfmt ", 8); u32(16); u16(1); u16(1);
                u32(rate); u32(rate * 2); u16(2); u16(16); put_bytes(b, "data", 4); u32(n * 2);
                for (int i = 0; i < n; i++) u16(uint16_t(int16_t(std::sin(i * 6.2831853 * 440 / rate) * 8000 * std::min(1.0, (n - i) / 2000.0))));
                s3.audio[1] = std::move(clip);
            }
            EdTrigger intro;
            intro.name = "Intro";
            intro.conditions.push_back({"timer", {{"seconds", "2"}}});
            intro.effects.push_back({"message", {{"text", "Pilot, two enemy mechs hold the ridge. Destroy them."}, {"caption", "Commander"}, {"seconds", "6"}, {"pause", "0"}}});
            intro.effects.push_back({"audio", {{"clip", "1"}}});
            EdTrigger patrol;
            patrol.name = "Patrol";
            char pathbuf[160];
            snprintf(pathbuf, sizeof pathbuf, "%d,%d;%d,%d;%d,%d", int(W * 0.70f), int(H * 0.70f), int(W * 0.55f), int(H * 0.75f),
                     int(W * 0.60f), int(H * 0.55f));
            patrol.effects.push_back({"ai", {{"unit", std::to_string(m1)}, {"order", "patrol"}, {"path", pathbuf}}});
            char ptbuf[64];
            snprintf(ptbuf, sizeof ptbuf, "%d,%d", int(W * 0.78f), int(H * 0.62f));
            patrol.effects.push_back({"ai", {{"unit", std::to_string(m2)}, {"order", "defend"}, {"point", ptbuf}, {"radius", "80"}}});
            EdTrigger ambush;
            ambush.name = "Ambush";
            snprintf(ptbuf, sizeof ptbuf, "%d,%d", int(W * 0.62f), int(H * 0.62f));
            ambush.conditions.push_back({"area", {{"point", ptbuf}, {"radius", "120"}, {"inside", "1"}}});
            ambush.effects.push_back({"ai", {{"unit", std::to_string(m1)}, {"order", "attack"}}});
            ambush.effects.push_back({"ai", {{"unit", std::to_string(m2)}, {"order", "attack"}}});
            ambush.effects.push_back({"message", {{"text", "They've spotted you!"}, {"seconds", "4"}}});
            EdTrigger victory;
            victory.name = "Victory";
            victory.conditions.push_back({"destroyed", {{"units", std::to_string(m1) + "," + std::to_string(m2)}, {"all", "1"}, {"jacking", "1"}}});
            victory.effects.push_back({"objective", {{"objective", "0"}}});
            victory.effects.push_back({"message", {{"text", "Ridge secured. Well done, pilot."}, {"seconds", "5"}}});
            victory.effects.push_back({"checkpoint", {{"name", "ridge"}}});
            victory.effects.push_back({"trigger_on", {{"trigger", "End"}}});
            // Retail pattern ("TT - delay end mission"): a disabled timer that another trigger
            // enables; its countdown starts then.
            EdTrigger end;
            end.name = "End";
            end.enabled = false;
            end.conditions.push_back({"timer", {{"seconds", "8"}}});
            end.effects.push_back({"win", {}});
            // A logic trigger: both mechs damaged or the player reaches the ridge -> music change.
            EdTrigger tension;
            tension.name = "Tension";
            tension.any = true;
            tension.conditions.push_back({"trigger", {{"trigger", "Ambush"}}});
            tension.conditions.push_back({"damaged", {{"units", std::to_string(m1)}, {"percent", "50"}}});
            if (!retail_music_cues().empty())
                tension.effects.push_back({"music", {{"cue", std::to_string(retail_music_cues().front().id)}, {"intensity", "7000"}}});
            s3.triggers = {intro, patrol, ambush, victory, end, tension};
            };
            Scenario s3;
            ok = io.create(mp, s3, &err);
            CHECK(ok, "create mission: %s", err.c_str());
            if (ok) {
                fill_mission(s3);
                CHECK(convert_legacy_triggers(s3) == 6 && s3.triggers.empty(), "trigger list compiled into the world");
                fs::path out = dir / L"Mission_Ambush.ma2map";
                report("mission", s3, out);
                // Inspect the compiled world.
                MgfReader mr;
                Bytes wb;
                Wdf cw;
                if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                    std::map<std::string, int> counts;
                    for (auto& o : cw.objs) {
                        std::string n = gd.names().label(o.cls);
                        if (n.compare(0, 8, "MATrigge") == 0 || n.compare(0, 8, "MAAction") == 0 || n == "MAEvent" || n == "MAPatrol" ||
                            n == "MADefend" || n == "MAAttack" || n == "MAWaypoint" || n == "MAAIGroup" || n == "MoeEntityReference")
                            counts[n]++;
                    }
                    printf("[triggers] compiled:");
                    for (auto& [n, c] : counts) printf(" %s x%d", n.c_str(), c);
                    printf("\n");
                    CHECK(counts["MATriggerLogic"] == 1 && counts["MATriggerDestroyed"] == 2 && counts["MATriggerRadius"] == 1 &&
                              counts["MAEvent"] == 4 && counts["MAActionBroadcast"] == 4 && counts["MAActionGameCondition"] == 1,  // 3 own + the generated briefing
                          "compiled trigger structure");
                    Bytes ib;
                    mr.read_file("scenario.ini", ib);
                    std::string ini(ib.begin(), ib.end());
                    CHECK(ini.find("61000 = Pilot, two enemy") != std::string::npos && ini.find("60100 = Destroy both") != std::string::npos,
                          "inline text in [Strings]");
                    CHECK(mr.find("Audio/1.wav") >= 0, "audio clip stored");
                    // The briefing is one generated trigger + message in the copy, texts from 64000, nothing in the document.
                    {
                        int trig_n = 0, bc_n = 0, gen_n = 0;
                        uint32_t bc_text = 0;
                        for (auto& o : cw.objs) {
                            std::string d = o.description();
                            if (d.compare(0, 6, kGenPrefix) != 0) continue;
                            gen_n++;
                            std::string n = gd.names().label(o.cls);
                            if (d == "[gen] Briefing" && n.rfind("MATrigger", 0) == 0) trig_n++;
                            if (d == "[gen] Briefing text" && n == "MAActionBroadcast") { bc_n++; bc_text = o.u32("BodyTextID"); }
                        }
                        CHECK(trig_n == 1 && bc_n == 1, "E2 the copy has one [gen] Briefing trigger and message (%d, %d)", trig_n, bc_n);
                        CHECK(bc_text == kTextBase + kStrGenerated + 1 && ini.find("64001 = Ambush at the Ridge\\nTwo enemy mechs guard the ridge.\\nDestroy both of them.") != std::string::npos,
                              "E2 the message text is the title, a line break and the text (id %u)", bc_text);
                        CHECK(gen_n == 3, "E2 [gen] objects in the copy: group, trigger, message (%d)", gen_n);
                        size_t doc_gen = 0;
                        for (auto& o : s3.world.objs) doc_gen += o.description().compare(0, 6, kGenPrefix) == 0;
                        CHECK(doc_gen == 0 && s3.strings.lower_bound(kStrGenerated) == s3.strings.end(), "E2 the document itself has no generated object or text");
                        CHECK(ini.find("[Mission]\r\nBriefingTitle = Ambush at the Ridge") != std::string::npos, "E2 BriefingTitle / Briefing still in [Mission]");
                    }
                    // Reopen = the authored document: same objects (class and every property) and texts.
                    {
                        Scenario back;
                        std::string e2;
                        bool lo = io.load(out, back, &e2);
                        CHECK(lo, "E2 reload: %s", e2.c_str());
                        if (lo) {
                            size_t diff = 0, gone = 0;
                            for (auto& o : s3.world.objs) {
                                const WObj* b = object_by_uid(back.world, o.uid());
                                if (!b) { gone++; continue; }
                                if (b->cls != o.cls || b->props.size() != o.props.size()) { diff++; continue; }
                                for (size_t i = 0; i < o.props.size(); i++)
                                    if (o.props[i].key != b->props[i].key || o.props[i].data != b->props[i].data) { diff++; if (diff <= 8) printf("[E2] differs: %s '%s' key %08X\n", gd.names().label(o.cls).c_str(), o.description().c_str(), o.props[i].key); break; }
                            }
                            CHECK(back.world.objs.size() == s3.world.objs.size() && gone == 0, "E2 reopened: same object count (%zu vs %zu, %zu missing)",
                                  back.world.objs.size(), s3.world.objs.size(), gone);
                            printf("[E2] objects differing after reopen: %zu\n", diff);
                            CHECK(back.strings == s3.strings, "E2 reopened: same texts (%zu vs %zu)", back.strings.size(), s3.strings.size());
                            CHECK(back.briefing_title == s3.briefing_title && back.briefing == s3.briefing, "E2 reopened: same briefing");
                            size_t g = 0;
                            for (auto& o : back.world.objs) g += o.description().compare(0, 6, kGenPrefix) == 0;
                            CHECK(g == 0 && back.strings.lower_bound(kStrGenerated) == back.strings.end(), "E2 reopened: no [gen] object, no text >= 64000");
                        }
                    }
                }
            }
            // Description from the briefing; pages; an intro cutscene holds the briefing back.
            {
                Scenario sd = s3;
                sd.description.clear();
                fs::path outd = dir / L"Mission_BriefDesc.ma2map";
                SaveReport repd;
                CHECK(io.save(sd, outd, &repd, &err), "E2 save (empty description): %s", err.c_str());
                MgfReader rd;
                Bytes ibd;
                std::string inid;
                if (rd.open(outd) && rd.read_file("scenario.ini", ibd)) inid.assign(ibd.begin(), ibd.end());
                CHECK(inid.find("Description = Ambush at the Ridge: Two enemy mechs guard the ridge. Destroy both of them.\r\n") != std::string::npos,
                      "E2 an empty map description shows the briefing in the list");
                Scenario bd;
                CHECK(io.load(outd, bd, &err) && bd.description.empty(), "E2 the derived description loads back as empty");
                // A typed description is kept as typed.
                sd.description = "My own words.";
                outd = dir / L"Mission_BriefDesc2.ma2map";
                io.save(sd, outd, &repd, &err);
                Scenario bd2;
                CHECK(io.load(outd, bd2, &err) && bd2.description == "My own words.", "E2 a typed description is kept ('%s' / %s)", bd2.description.c_str(), err.c_str());
                // Without a briefing the old placeholder stays.
                sd.description.clear(); sd.briefing.clear(); sd.briefing_title.clear();
                outd = dir / L"Mission_BriefDesc3.ma2map";
                io.save(sd, outd, &repd, &err);
                Scenario bd3;
                CHECK(io.load(outd, bd3, &err) && bd3.description.empty(), "E2 no briefing: placeholder description loads back as empty");
                MgfReader rd3;
                Bytes ib3;
                std::string ini3;
                if (rd3.open(outd) && rd3.read_file("scenario.ini", ib3)) ini3.assign(ib3.begin(), ib3.end());
                CHECK(ini3.find("Description = A custom scenario by") != std::string::npos && ini3.find("64001 = ") == std::string::npos,  // 64000 is the intro message's "Commander: ..." body
                      "E2 no briefing: nothing generated");
                // Pages: short = one page; long = several, each at most six lines and 30 s.
                auto pg = briefing_pages("IRON GATE", "Hold the gate.\nDo not let anyone through.");
                CHECK(pg.size() == 1 && pg[0].text == "IRON GATE\nHold the gate.\nDo not let anyone through." && pg[0].seconds > 4 && pg[0].seconds < 30,
                      "E2 a short briefing is one page (%zu)", pg.size());
                std::string longt;
                for (int i = 1; i <= 90; i++) longt += "word" + std::to_string(i % 10) + (i % 7 ? " " : "\n");
                auto pl = briefing_pages("T", longt);
                bool pages_ok = pl.size() >= 4;
                for (auto& p : pl) {
                    int lines = 1;
                    for (char ch : p.text) lines += ch == '\n';
                    pages_ok = pages_ok && lines <= 6 && p.seconds <= 30.f && p.seconds >= 4.f;
                }
                CHECK(pages_ok, "E2 a long briefing is split into short pages (%zu)", pl.size());
                CHECK(briefing_description("", "").empty() && briefing_description("A", "").size() == 1 &&
                          briefing_description("T", std::string(400, 'x')).size() == 243 &&
                          briefing_description("T", std::string(400, 'x')).substr(240) == "...",
                      "E2 briefing description: cut at 240 characters");
                // An intro cutscene played at the start: the briefing waits for its last shot's "ends" trigger.
                {
                    Scenario si = s3;
                    uint32_t cin = io.add_cutscene(si, {si.terrain.size_x() * 0.5f, 0, si.terrain.size_z() * 0.5f}, "Intro cutscene", nullptr);
                    uint32_t tm = cin ? add_trigger_object(si, "MATriggerTimer", "Play the intro") : 0;
                    uint32_t ac = tm ? add_action_object(si, tm, "MAActionActivateCinema", "Play intro") : 0;
                    if (WObj* ao = ac ? object_by_uid(si.world, ac) : nullptr) ao->set_u32("cinemaID", cin);
                    uint32_t exit = 0;
                    for (auto& o : si.world.objs) if (o.cls == key_of("MAFreeCameraController")) exit = o.u32("triggerExit");
                    CHECK(cin && ac && exit, "E2 intro setup (cutscene %u, action %u, exit %u)", cin, ac, exit);
                    fs::path outi = dir / L"Mission_BriefIntro.ma2map";
                    CHECK(io.save(si, outi, &repd, &err), "E2 save (intro): %s", err.c_str());
                    MgfReader ri;
                    Bytes wbi;
                    Wdf wi;
                    if (ri.open(outi) && ri.read_file("world.wdf", wbi) && wi.parse(wbi)) {
                        const WObj* gl = nullptr;
                        for (auto& o : wi.objs) if (o.description() == "[gen] Briefing") gl = &o;
                        bool logic = gl && gd.names().label(gl->cls) == "MATriggerLogic", ref = false;
                        if (gl) for (uint32_t r : reference_children(wi, gl->uid())) ref |= object_by_uid(wi, r)->u32("ReferenceUID") == exit;
                        CHECK(logic && ref, "E2 with an intro cutscene the briefing is a logic trigger waiting for the last shot's end (%d, %d)", logic, ref);
                    }
                    Scenario bi;
                    size_t gi = 0;
                    if (io.load(outi, bi, &err)) for (auto& o : bi.world.objs) gi += o.description().compare(0, 6, kGenPrefix) == 0;
                    CHECK(gi == 0 && bi.world.objs.size() == si.world.objs.size(), "E2 intro mission reopens as authored (%zu vs %zu objects)", bi.world.objs.size(), si.world.objs.size());
                }
            }
            // A message with a speaker reads "Speaker: text" in the copy; reopen = the authored message; Won / Lost texts.
            {
                Scenario sk = s3;
                uint32_t bc = 0, orig_body = 0, speaker = 0;
                for (auto& o : sk.world.objs)
                    if (!bc && o.cls == key_of("MAActionBroadcast") && own_text(sk, o.u32("BodyTextID"))) bc = o.uid();
                if (WObj* b = bc ? object_by_uid(sk.world, bc) : nullptr) {
                    orig_body = b->u32("BodyTextID");
                    *own_text(sk, orig_body) = "Go.";
                    speaker = add_text(sk, "Command");
                    b->set_u32("CaptionTextID", speaker);
                }
                CHECK(bc && speaker, "E3 speaker setup (%u, %u)", bc, speaker);
                sk.win_text = "THE GATE IS OURS";
                sk.lose_text = "THE CONVOY GOT AWAY";
                CHECK(speaker_line("Command", "Go.") == "Command: Go." && speaker_line("  ", "Go.") == "Go." && speaker_line("", "Go.") == "Go.",
                      "E3 speaker_line composes 'Speaker: text'");
                fs::path outk = dir / L"Mission_Speaker.ma2map";
                SaveReport repk;
                CHECK(io.save(sk, outk, &repk, &err), "E3 save: %s", err.c_str());
                MgfReader rk;
                Bytes wbk, ibk;
                Wdf wk;
                std::string inik;
                if (rk.open(outk) && rk.read_file("world.wdf", wbk) && wk.parse(wbk) && rk.read_file("scenario.ini", ibk)) {
                    inik.assign(ibk.begin(), ibk.end());
                    const WObj* b = object_by_uid(wk, bc);
                    uint32_t gid = b ? b->u32("BodyTextID") : 0;
                    int gn = int(gid) - int(kTextBase);
                    CHECK(gn >= kStrGenerated && gn < kStrGeneratedEnd && gid != orig_body, "E3 the copy's message has a generated body id (%d)", gn);
                    CHECK(inik.find(std::to_string(gn) + " = Command: Go.\r\n") != std::string::npos, "E3 the generated text is 'Command: Go.'");
                    CHECK(inik.find("[Generated]\r\nBody" + std::to_string(gn) + " = " + std::to_string(int(orig_body - kTextBase))) != std::string::npos,
                          "E3 [Generated] remembers the original body id");
                    CHECK(b && b->u32("CaptionTextID") == speaker, "E3 the speaker stays in the copy's CaptionTextID");
                    CHECK(inik.find("WinText = THE GATE IS OURS\r\nLoseText = THE CONVOY GOT AWAY\r\n") != std::string::npos, "E3 WinText / LoseText reach scenario.ini [Mission]");
                }
                CHECK(speaker_line("", "x") == "x", "E3 plain");
                Scenario bk;
                CHECK(io.load(outk, bk, &err), "E3 reload: %s", err.c_str());
                const WObj* rb = object_by_uid(bk.world, bc);
                CHECK(rb && rb->u32("BodyTextID") == orig_body && rb->u32("CaptionTextID") == speaker, "E3 reopened: original body id and speaker");
                CHECK(bk.strings == sk.strings && bk.world.objs.size() == sk.world.objs.size(), "E3 reopened: same texts and object count");
                CHECK(bk.win_text == sk.win_text && bk.lose_text == sk.lose_text, "E3 reopened: same Won / Lost texts");
                // Reopen then save again: nothing doubles up.
                fs::path outk2 = dir / L"Mission_Speaker2.ma2map";
                io.save(bk, outk2, &repk, &err);
                MgfReader rk2;
                Bytes ibk2;
                std::string inik2;
                if (rk2.open(outk2) && rk2.read_file("scenario.ini", ibk2)) inik2.assign(ibk2.begin(), ibk2.end());
                auto swaps = [](const std::string& ini_text) {  // the fixture's intro message ("Commander") is swapped too
                    size_t pos = ini_text.find("[Generated]"), n = 0;
                    for (size_t p = pos; pos != std::string::npos && (p = ini_text.find("\r\nBody", p)) != std::string::npos; p += 6) n++;
                    return n;
                };
                CHECK(swaps(inik2) == 2, "E3 save after reopen: the same two swapped bodies, none doubled (%zu)", swaps(inik2));
                // No speaker: no swap, no [Generated] section for messages.
                if (WObj* b = object_by_uid(sk.world, bc)) b->set_u32("CaptionTextID", 0);
                fs::path outk3 = dir / L"Mission_Speaker3.ma2map";
                io.save(sk, outk3, &repk, &err);
                MgfReader rk3;
                Bytes ibk3;
                inik2.clear();
                if (rk3.open(outk3) && rk3.read_file("scenario.ini", ibk3)) inik2.assign(ibk3.begin(), ibk3.end());
                CHECK(swaps(inik2) == 1, "E3 no speaker: that message body is not swapped (%zu left)", swaps(inik2));
            }
            // "During this cutscene": the hold compiled into the copy; reopen = the authored document.
            {
                Scenario sh;
                CHECK(io.create(mp, sh, &err), "E4 create: %s", err.c_str());
                const float W = sh.terrain.size_x(), H = sh.terrain.size_z();
                auto unit = [&](float fx, float fz, int nation, int ai) {
                    uint32_t u = io.place_template(sh, 27265, {W * fx, 0, H * fz}, 3.1f);
                    if (WObj* o = object_by_uid(sh.world, u)) { o->set_i32("Nation", nation); o->set_i32("m_bAIEnabled", ai); }
                    return u;
                };
                const uint32_t u1 = unit(0.60f, 0.60f, 5, 1), u2 = unit(0.62f, 0.60f, 5, 1), u3 = unit(0.64f, 0.60f, 5, 0), u4 = unit(0.66f, 0.60f, 5, 0),
                               fr = unit(0.58f, 0.60f, 6, 1);
                uint32_t cin = io.add_cutscene(sh, {W * 0.3f, 0, H * 0.3f}, "Reveal", nullptr);
                uint32_t shot2 = cin ? io.add_cutscene_shot(sh, cin, nullptr) : 0;
                std::vector<uint32_t> ctrls;
                for (auto& o : sh.world.objs) if (o.cls == key_of("MAFreeCameraController")) ctrls.push_back(o.uid());
                CHECK(u1 && u2 && u3 && u4 && fr && cin && shot2 && ctrls.size() == 2, "E4 setup (%u %u %u %u %u, cutscene %u, %zu shots)", u1, u2, u3, u4, fr, cin, ctrls.size());
                if (ctrls.size() == 2) {
                    object_by_uid(sh.world, ctrls[0])->set_f32("CameraDuration", 6.f);
                    object_by_uid(sh.world, ctrls[1])->set_f32("CameraDuration", 5.f);
                    uint32_t tm = add_trigger_object(sh, "MATriggerTimer", "Play the reveal");
                    object_by_uid(sh.world, tm)->set_f32("TrgTimerDurationSeconds", 5.f);
                    object_by_uid(sh.world, add_action_object(sh, tm, "MAActionActivateCinema", "Play reveal"))->set_u32("cinemaID", cin);
                    uint32_t wk = add_trigger_object(sh, "MATriggerTimer", "Wake the third");
                    object_by_uid(sh.world, wk)->set_f32("TrgTimerDurationSeconds", 30.f);
                    uint32_t wa = add_action_object(sh, wk, "MAActionAI", "Wake third");
                    if (WObj* a = object_by_uid(sh.world, wa)) { a->set_u32("ActionEntity", u3); a->set_i32("EnableAI", 1); }
                    const uint32_t init1 = object_by_uid(sh.world, ctrls[0])->u32("triggerInit"), exit2 = object_by_uid(sh.world, ctrls[1])->u32("triggerExit");
                    CHECK(cutscene_hold(*object_by_uid(sh.world, cin)) == 1 && cutscene_skippable(*object_by_uid(sh.world, cin)), "E4 defaults: hold the enemies, skippable");
                    const std::vector<uint32_t> held = cutscene_hold_units(sh.world, 1);
                    CHECK(std::find(held.begin(), held.end(), fr) == held.end() && std::find(held.begin(), held.end(), u4) != held.end() && held.size() >= 4,
                          "E4 the hold set is the enemies, not the friendly (%zu)", held.size());
                    CHECK(cutscene_hold_notes(sh.world).empty(), "E4 a 30 s waker (cutscene 5-16 s) raises no note");
                    fs::path outh = dir / L"Mission_Hold.ma2map";
                    SaveReport reph;
                    CHECK(io.save(sh, outh, &reph, &err), "E4 save: %s", err.c_str());
                    MgfReader rh;
                    Bytes wbh, ibh;
                    Wdf wh;
                    std::string inih;
                    if (rh.open(outh) && rh.read_file("world.wdf", wbh) && wh.parse(wbh) && rh.read_file("scenario.ini", ibh)) {
                        inih.assign(ibh.begin(), ibh.end());
                        auto members = [&](uint32_t set) {
                            std::vector<uint32_t> m;
                            if (const WObj* so = object_by_uid(wh, set)) for (int k = 0; k < so->i32("MoeEntitySetCount"); k++) m.push_back(so->u32("MoeEntitySetMember" + std::to_string(k)));
                            std::sort(m.begin(), m.end());
                            return m;
                        };
                        // Under the trigger that plays it (not shot 1's start trigger, which the game does not fire again on a
                        // replay of a cutscene of several shots): one AI-off action on a set of the enemies, one switch-on of the safety timer.
                        uint32_t off_set = 0, safety = 0, off_n = 0, under_init = 0;
                        for (auto& o : wh.objs) {
                            if (o.cls != key_of("MAActionAI")) continue;
                            under_init += o.parent() == init1 && o.description().compare(0, 6, "[gen] ") == 0;
                            if (o.parent() == tm && o.description() == "[gen] Hold the fight") { off_set = o.u32("AISet"); off_n += o.i32("EnableAI") == 0; }
                            else if (o.parent() == tm && o.description() == "[gen] Start the safety timer") safety = o.u32("ActionEntity");
                        }
                        CHECK(under_init == 0, "nothing generated under shot 1's start trigger (%u)", under_init);
                        std::vector<uint32_t> hm = members(off_set);
                        CHECK(off_n == 1 && std::count(hm.begin(), hm.end(), u1) && std::count(hm.begin(), hm.end(), u2) && std::count(hm.begin(), hm.end(), u3) &&
                                  std::count(hm.begin(), hm.end(), u4) && !std::count(hm.begin(), hm.end(), fr),
                              "E4 one EnableAI 0 under the trigger that plays it on a set of the enemies, not the friendly (%u action(s), %zu members)", off_n, hm.size());
                        const WObj* st = object_by_uid(wh, safety);
                        CHECK(st && st->cls == key_of("MATriggerTimer") && std::fabs(st->f32("TrgTimerDurationSeconds") - 11.5f) < 0.01f && st->i32("TriggerEnabled") == 0,
                              "E4 the safety timer is asleep and runs the shot lengths + 0.5 s (%.2f)", st ? st->f32("TrgTimerDurationSeconds") : -1.f);
                        // Every play holds and releases. No once-only "over" Logic: the release hangs under the last
                        // shot's end trigger (the game fires it on every play) and under the safety timer, which repeats.
                        CHECK(st && st->i32("TriggerAutoReset") == 1, "the safety timer has Repeat on (it can run again on the next play)");
                        size_t once_over = 0;
                        for (auto& o : wh.objs)
                            once_over += o.cls == key_of("MATriggerLogic") && o.description() == "[gen] Reveal - over";
                        CHECK(once_over == 0, "no once-only 'over' Logic trigger (%zu)", once_over);
                        // Only a cutscene played at the very start (a timer of one second or less) gets the second hold shortly after the
                        // start; this one is played at 5 s, so it has none. The briefing runs from the end of the intro (none here: 0 s).
                        size_t again_n = 0;
                        for (auto& o : wh.objs) again_n += o.cls == key_of("MATriggerTimer") && o.description() == "[gen] Reveal - hold again";
                        CHECK(again_n == 0, "no 'hold again' timer for a cutscene played at 5 s (%zu)", again_n);
                        sh.briefing_title = "Operation Iron Gate";
                        sh.briefing = "Commander, a Kurita outpost guards the valley. Destroy its tanks before the relief column arrives.";
                        float bf = -1, bt = -1;
                        CHECK(briefing_span(sh, bf, bt) && bf == 0.f && bt > 4.f && intro_seconds(sh.world) == 0.f, "briefing_span without an intro (%.1f to %.1f)", double(bf), double(bt));
                        sh.briefing_title.clear();
                        sh.briefing.clear();
                        // Under the end trigger and under the safety timer: the awake two come back, the woken check is switched on,
                        // and the safety timer is switched off (the safety timer's own switch-off is its last action).
                        auto gen_ai_under = [&](uint32_t trig) {
                            std::vector<const WObj*> v;
                            for (auto& o : wh.objs)
                                if (o.cls == key_of("MAActionAI") && o.parent() == trig && o.description().compare(0, 6, "[gen] ") == 0) v.push_back(&o);
                            return v;
                        };
                        uint32_t woken_logic = 0;
                        std::vector<uint32_t> back_woken, restored_all;
                        for (auto& o : wh.objs)
                            if (o.cls == key_of("MATriggerLogic") && o.description().find("woken and over") != std::string::npos) woken_logic = o.uid();
                        std::vector<uint32_t> want_a{u1, u2};
                        std::sort(want_a.begin(), want_a.end());
                        for (uint32_t trig : {exit2, safety}) {
                            std::vector<const WObj*> acts = gen_ai_under(trig);
                            std::vector<uint32_t> back_awake;
                            bool check_on = false, safety_off = false;
                            for (const WObj* a : acts) {
                                if (a->u32("AISet") && a->i32("EnableAI") == 1) back_awake = members(a->u32("AISet"));
                                if (a->u32("ActionEntity") == woken_logic && a->i32("EnableAI") == 1) check_on = true;
                                if (a->u32("ActionEntity") == safety && a->i32("EnableAI") == 0) safety_off = true;
                            }
                            // The end trigger never switches the safety timer off mid-count (the game keeps its old start then).
                            CHECK(back_awake == want_a && check_on && safety_off == (trig == safety) && woken_logic,
                                  "under the %s: EnableAI 1 for the two awake enemies (%zu), the woken check on, safety timer off only by itself", trig == exit2 ? "last shot's end" : "safety timer", back_awake.size());
                            if (trig == safety)
                                CHECK(!acts.empty() && acts.back()->u32("ActionEntity") == safety && acts.back()->i32("EnableAI") == 0, "the safety timer switches itself off last");
                        }
                        // The woken check: asleep, Repeat on, any(its waker); wakes the woken unit, then switches itself off.
                        const WObj* wl = object_by_uid(wh, woken_logic);
                        std::vector<uint32_t> w_in;
                        for (uint32_t r : reference_children(wh, woken_logic)) w_in.push_back(object_by_uid(wh, r)->u32("ReferenceUID"));
                        std::vector<const WObj*> wacts = gen_ai_under(woken_logic);
                        if (!wacts.empty()) back_woken = members(wacts.front()->u32("AISet"));
                        CHECK(wl && wl->i32("TriggerEnabled") == 0 && wl->i32("TriggerAutoReset") == 1 && w_in == std::vector<uint32_t>{wk} && back_woken == std::vector<uint32_t>{u3} &&
                                  wacts.size() == 2 && wacts.back()->u32("ActionEntity") == woken_logic && wacts.back()->i32("EnableAI") == 0,
                              "the woken unit comes back through a re-armable check on its 30 s timer (%zu members, %zu inputs, %zu actions)", back_woken.size(), w_in.size(), wacts.size());
                        for (auto& o : wh.objs)
                            if (o.cls == key_of("MAActionAI") && o.i32("EnableAI") == 1 && o.description().compare(0, 6, "[gen] ") == 0 && o.u32("AISet"))
                                for (uint32_t m : members(o.u32("AISet"))) restored_all.push_back(m);
                        CHECK(std::find(restored_all.begin(), restored_all.end(), u4) == restored_all.end() && std::find(restored_all.begin(), restored_all.end(), fr) == restored_all.end(),
                              "E4 the never-woken unit and the friendly are never switched on");
                        CHECK(inih.find("[Cutscenes]") == std::string::npos, "E4 a skippable cutscene writes no NoSkip");
                    }
                    // Reopen = the authored document: same objects, the units' own properties untouched, no [gen] object.
                    Scenario bh;
                    CHECK(io.load(outh, bh, &err), "E4 reload: %s", err.c_str());
                    size_t gen = 0;
                    // Which of the units / shots / cutscene differ after a save + reopen. Some differences are build-time rewrites that
                    // exist without any hold (note), so the reference is a save with the hold switched off.
                    auto differ = [&](const Scenario& a, const Scenario& b) {
                        std::map<uint32_t, uint32_t> d;  // uid -> first differing key (0 = missing / property count)
                        for (auto& o : a.world.objs) {
                            const WObj* bo = object_by_uid(b.world, o.uid());
                            if (!bo) { d[o.uid()] = 0; continue; }
                            if (o.cls != key_of("MAMech") && o.cls != key_of("MAFreeCameraController") && o.cls != key_of("MACinema")) continue;
                            if (bo->props.size() != o.props.size()) { d[o.uid()] = 1; continue; }
                            for (size_t i = 0; i < o.props.size(); i++)
                                if (o.props[i].key != bo->props[i].key || o.props[i].data != bo->props[i].data) { d[o.uid()] = o.props[i].key; break; }
                        }
                        return d;
                    };
                    const std::map<uint32_t, uint32_t> hold_diff = differ(sh, bh);
                    for (auto& o : bh.world.objs) gen += o.description().compare(0, 6, kGenPrefix) == 0;
                    CHECK(bh.world.objs.size() == sh.world.objs.size() && gen == 0, "E4 reopened: same object count (%zu vs %zu), no [gen] (%zu)", bh.world.objs.size(), sh.world.objs.size(), gen);
                    {
                        int link_diff = 0;  // the units' set back links and the shots' triggers are exactly the authored ones
                        for (auto& o : sh.world.objs) {
                            const WObj* bo = object_by_uid(bh.world, o.uid());
                            if (!bo) continue;
                            if (o.cls == key_of("MAMech") && ((o.find("SetCount") != nullptr) != (bo->find("SetCount") != nullptr) || o.i32("SetCount") != bo->i32("SetCount") || (o.find("SetMember0") != nullptr) != (bo->find("SetMember0") != nullptr))) link_diff++;
                            if (o.cls == key_of("MAFreeCameraController") && (o.u32("triggerInit") != bo->u32("triggerInit") || o.u32("triggerExit") != bo->u32("triggerExit"))) link_diff++;
                        }
                        CHECK(link_diff == 0, "E4 reopened: set back links and shot triggers as authored (%d differ)", link_diff);
                    }
                    // Choices: Nothing leaves the copy alone; not skippable -> NoSkip + project.json; Everyone holds the friendly too.
                    {
                        Scenario sn = sh;
                        set_cutscene_hold(*object_by_uid(sn.world, cin), 0);
                        set_cutscene_skippable(*object_by_uid(sn.world, cin), false);
                        fs::path outn = dir / L"Mission_HoldNothing.ma2map";
                        CHECK(io.save(sn, outn, &reph, &err), "E4 save (nothing): %s", err.c_str());
                        MgfReader rn;
                        Bytes wbn, ibn;
                        Wdf wn;
                        if (rn.open(outn) && rn.read_file("world.wdf", wbn) && wn.parse(wbn) && rn.read_file("scenario.ini", ibn)) {
                            std::string ini_n(ibn.begin(), ibn.end());
                            size_t g = 0;
                            for (auto& o : wn.objs) g += o.description().compare(0, 6, kGenPrefix) == 0 && o.description() != "[gen] Briefing" && o.description() != "[gen] Briefing text" && o.description() != kGenGroup && o.cls != key_of("MAActionBroadcast");
                            CHECK(g == 0, "E4 hold = Nothing: nothing generated for the cutscene (%zu)", g);
                            CHECK(ini_n.find("[Cutscenes]\r\nNoSkip = " + std::to_string(cin) + "\r\n") != std::string::npos, "E4 not skippable: scenario.ini [Cutscenes] NoSkip = the cutscene's uid");
                            CHECK(!object_by_uid(wn, cin)->find(kHoldProp) && !object_by_uid(wn, cin)->find(kSkipProp), "E4 the choices stay out of the game's world");
                        }
                        Scenario bn;
                        CHECK(io.load(outn, bn, &err), "E4 reload (nothing): %s", err.c_str());
                        const WObj* bc2 = object_by_uid(bn.world, cin);
                        CHECK(bc2 && cutscene_hold(*bc2) == 0 && !cutscene_skippable(*bc2), "E4 reopened: Nothing and not skippable come back from project.json");
                        CHECK(hold_diff == differ(sn, bn), "E4 reopen with the hold changes nothing of the units / shots / cutscene beyond what any save does (%zu vs %zu)",
                              hold_diff.size(), differ(sn, bn).size());
                        Scenario se = sh;
                        set_cutscene_hold(*object_by_uid(se.world, cin), 2);
                        fs::path oute = dir / L"Mission_HoldAll.ma2map";
                        CHECK(io.save(se, oute, &reph, &err), "E4 save (everyone): %s", err.c_str());
                        MgfReader re;
                        Bytes wbe;
                        Wdf we;
                        if (re.open(oute) && re.read_file("world.wdf", wbe) && we.parse(wbe)) {
                            bool fr_in = false;
                            for (auto& o : we.objs)
                                if (o.cls == key_of("MoeEntitySet") && o.description() == "[gen] Hold Reveal - units")
                                    for (int k = 0; k < o.i32("MoeEntitySetCount"); k++) fr_in |= o.u32("MoeEntitySetMember" + std::to_string(k)) == fr;
                            CHECK(fr_in, "E4 hold = Everyone: the friendly is in the set");
                        }
                    }
                    // A waker that fires while the cutscene plays raises a Check note.
                    object_by_uid(sh.world, wk)->set_f32("TrgTimerDurationSeconds", 8.f);
                    auto notes = cutscene_hold_notes(sh.world);
                    CHECK(notes.size() == 1 && notes[0].second == u3 && notes[0].first.find("fight on camera") != std::string::npos,
                          "E4 a unit woken during the cutscene is a Check note (%zu)", notes.size());
                    // A cutscene with no shot: a note, nothing generated.
                    Scenario sz = sh;
                    for (uint32_t c : ctrls) wedit::remove_subtree(sz.world, c);
                    notes = cutscene_hold_notes(sz.world);
                    CHECK(notes.size() == 1 && notes[0].first.find("has no shot") != std::string::npos, "E4 a cutscene without a shot is a Check note (%zu)", notes.size());
                }
            }
            // A 3-shot intro played again by a 30 s timer gets its colour filter steps at alpha 0 in the copy, a
            // countdown starts when the intro is over; reopen gives both back as authored.
            {
                Scenario sf;
                CHECK(io.create(mp, sf, &err), "create: %s", err.c_str());
                const float W = sf.terrain.size_x(), H = sf.terrain.size_z();
                uint32_t cin = io.add_cutscene(sf, {W * 0.4f, 0, H * 0.4f}, "Intro", nullptr);
                uint32_t s2 = cin ? io.add_cutscene_shot(sf, cin, nullptr) : 0, s3 = cin ? io.add_cutscene_shot(sf, cin, nullptr) : 0;
                uint32_t play = add_trigger_object(sf, "MATriggerTimer", "Play Intro");
                object_by_uid(sf.world, play)->set_f32("TrgTimerDurationSeconds", 0.f);
                object_by_uid(sf.world, play)->set_f32("TrgTimerDuration", 0.f);
                object_by_uid(sf.world, add_action_object(sf, play, "MAActionActivateCinema", "Play"))->set_u32("cinemaID", cin);
                uint32_t clock = add_trigger_object(sf, "MATriggerTimer", "Countdown");
                object_by_uid(sf.world, clock)->set_f32("TrgTimerDurationSeconds", 120.f);
                object_by_uid(sf.world, clock)->set_i32("TrgTimerDisplayOnHUD", 1);
                std::map<uint32_t, std::pair<float, float>> fades;  // type-4 steps of the authored cutscene
                for (auto& o : sf.world.objs)
                    if (o.cls == key_of("MACameraProperty") && o.i32("type") == 4) fades[o.uid()] = {o.f32("startAlpha"), o.f32("endAlpha")};
                bool some_alpha = false;
                for (auto& [u, a] : fades) some_alpha |= a.first > 0 || a.second > 0;
                CHECK(cin && s2 && s3 && fades.size() >= 6 && some_alpha, "setup: 3 shots with %zu colour filter steps", fades.size());
                auto save_read = [&](Scenario& sc, const wchar_t* fname, Wdf& out, std::string& ini) {
                    fs::path p = dir / fname;
                    SaveReport rep;
                    MgfReader r;
                    Bytes wb, ib;
                    bool ok = io.save(sc, p, &rep, &err) && r.open(p) && r.read_file("world.wdf", wb) && out.parse(wb) && r.read_file("scenario.ini", ib);
                    ini.assign(ib.begin(), ib.end());
                    return ok ? p : fs::path();
                };
                auto alphas_zero = [&](const Wdf& w) {
                    size_t z = 0;
                    for (auto& [u, a] : fades)
                        if (const WObj* o = object_by_uid(w, u); o && o->f32("startAlpha") == 0 && o->f32("endAlpha") == 0) z++;
                    return z;
                };
                // Played once: the fades stay.
                Wdf w1;
                std::string ini1;
                save_read(sf, L"FX8_Once.ma2map", w1, ini1);
                size_t authored_zero = 0;
                for (auto& [u, a] : fades) authored_zero += a.first == 0 && a.second == 0;
                CHECK(alphas_zero(w1) == authored_zero && ini1.find("\r\nFade") == std::string::npos, "a cutscene played once keeps its fades (%zu at 0)", alphas_zero(w1));
                // The countdown: asleep in the copy, switched on by a once-only Logic on the last shot's end trigger.
                uint32_t exit3 = object_by_uid(sf.world, s3)->u32("triggerExit");
                const WObj* c1 = object_by_uid(w1, clock);
                uint32_t go = 0;
                for (auto& o : w1.objs)
                    if (o.cls == key_of("MAActionAI") && o.u32("ActionEntity") == clock && o.i32("EnableAI") == 1 && o.description().compare(0, 6, "[gen] ") == 0) go = o.parent();
                const WObj* gl = object_by_uid(w1, go);
                std::vector<uint32_t> gin;
                for (uint32_t r : reference_children(w1, go)) gin.push_back(object_by_uid(w1, r)->u32("ReferenceUID"));
                CHECK(c1 && c1->i32("TriggerEnabled") == 0 && gl && gl->cls == key_of("MATriggerLogic") && gl->i32("TriggerAutoReset") == 0 && gin == std::vector<uint32_t>{exit3},
                      "the countdown starts asleep and a once-only Logic on the intro's end switches it on (%zu inputs)", gin.size());
                // Played again by a 30 s timer: every colour filter step at alpha 0, one FadeN line each.
                uint32_t again = add_trigger_object(sf, "MATriggerTimer", "Play Intro again");
                object_by_uid(sf.world, again)->set_f32("TrgTimerDurationSeconds", 30.f);
                object_by_uid(sf.world, add_action_object(sf, again, "MAActionActivateCinema", "Play"))->set_u32("cinemaID", cin);
                Wdf w2;
                std::string ini2;
                fs::path p2 = save_read(sf, L"FX8_Twice.ma2map", w2, ini2);
                size_t lines = 0;
                for (size_t p = 0; (p = ini2.find("\r\nFade", p)) != std::string::npos; p += 6) lines++;
                CHECK(alphas_zero(w2) == fades.size() && lines == fades.size(), "a cutscene played twice: all %zu colour filters at alpha 0 in the copy (%zu lines)", alphas_zero(w2), lines);
                Scenario sb;
                CHECK(!p2.empty() && io.load(p2, sb, &err), "reload: %s", err.c_str());
                size_t same = 0;
                for (auto& [u, a] : fades)
                    if (const WObj* o = object_by_uid(sb.world, u); o && o->f32("startAlpha") == a.first && o->f32("endAlpha") == a.second) same++;
                const WObj* cb = object_by_uid(sb.world, clock);
                size_t gen = 0;
                for (auto& o : sb.world.objs) gen += o.description().compare(0, 6, kGenPrefix) == 0;
                CHECK(same == fades.size() && cb && cb->i32("TriggerEnabled") == 1 && gen == 0 && sb.world.objs.size() == sf.world.objs.size(),
                      "reopened: fades (%zu of %zu) and the countdown as authored, no [gen]", same, fades.size());
            }
            // The cutscene model: lengths and total, set_shot_length linked / held, Add shot (a still 5 s shot from a view, nothing
            // copied, end actions moved), a deleted eye path and ensure_shot_eye_path, the Check rows, save / reopen.
            {
                Scenario sh;
                CHECK(io.create(mp, sh, &err), "create: %s", err.c_str());
                const float W = sh.terrain.size_x(), H = sh.terrain.size_z();
                uint32_t cin = io.add_cutscene(sh, {W * 0.3f, 0, H * 0.3f}, "Flyby", nullptr);
                auto shots = [&]() { return cutscene_shots(sh.world, cin); };
                auto kids_of = [&](uint32_t trig) {
                    int n = 0;
                    for (auto& o : sh.world.objs) n += o.parent() == trig && is_trigger_action(gd.names(), o);
                    return n;
                };
                CHECK(cin && shots().size() == 1 && std::fabs(shots()[0].len - 8.f) < 0.01f && std::fabs(cutscene_total(shots()) - 8.f) < 0.01f && !shots()[0].still &&
                          shots()[0].eye.size() >= 2,
                      "a new cutscene: one moving 8 s shot (%zu shots, %.1f s)", shots().size(), cutscene_total(shots()));
                const uint32_t ctrl1 = shots()[0].ctrl, path1 = shots()[0].eye_path;
                // Linked: the path's own duration equals the length, so changing the length moves the path's too.
                CHECK(set_shot_length(sh, ctrl1, 6.f) && std::fabs(shots()[0].len - 6.f) < 0.01f && std::fabs(shots()[0].move_len - 6.f) < 0.01f, "length 6: the path's move_len follows (linked)");
                CHECK(set_shot_length(sh, ctrl1, 500.f) && std::fabs(shots()[0].len - 120.f) < 0.01f && set_shot_length(sh, ctrl1, 0.1f) && std::fabs(shots()[0].len - 0.5f) < 0.01f,
                      "length clamps to 0.5-120 s");
                // Held: a path with its own, shorter duration keeps it.
                object_by_uid(sh.world, path1)->set_f32("duration", 3.f);
                set_shot_length(sh, ctrl1, 10.f);
                CHECK(std::fabs(shots()[0].len - 10.f) < 0.01f && std::fabs(shots()[0].move_len - 3.f) < 0.01f, "a held path keeps its own duration (move_len %.1f)", shots()[0].move_len);
                {   // The hold reads as the gap, and "Move over the whole shot" closes it
                    CHECK(std::fabs(shot_hold(shots()[0]) - 7.f) < 0.01f || shots()[0].still, "hold = length - move (%.1f)", shot_hold(shots()[0]));
                    CHECK(set_shot_move_len(sh, ctrl1, shots()[0].len) && std::fabs(shots()[0].move_len - 10.f) < 0.01f && shot_hold(shots()[0]) == 0.f, "move over the whole shot");
                    CHECK(set_shot_move_len(sh, ctrl1, 99.f) && std::fabs(shots()[0].move_len - 10.f) < 0.01f && set_shot_move_len(sh, ctrl1, 0.f) && std::fabs(shots()[0].move_len - 0.5f) < 0.01f,
                          "the move length stays between 0.5 s and the shot length");
                    const Vec3 lk{W * 0.3f, 12.f, H * 0.3f};
                    CHECK(set_shot_look(sh, ctrl1, lk) && shots()[0].has_look && std::fabs(shots()[0].look.x - lk.x) < 0.01f && std::fabs(shots()[0].look.y - lk.y) < 0.01f,
                          "aim: the focus moved to the point");
                }
                {   // The game's horizontal Field of view across the band, framing writes eye / look, a typed length carries the path along
                    const float a = game_fov_to_editor(48.f, 1000.f, 500.f), g = editor_fov_to_game(a, 1000.f, 500.f);
                    CHECK(std::fabs(g - 48.f) < 0.01f && std::fabs(std::tan(a * 0.5f) - std::tan(24.f * 0.0174532925f) * 0.5f) < 1e-4f, "fov: horizontal across the band, round trip %.3f", double(g));
                    const Vec3 e{W * 0.5f, 14.f, H * 0.5f}, lk{W * 0.5f + 10.f, 12.f, H * 0.5f + 25.f};
                    CHECK(set_shot_view(sh, ctrl1, 1, e, lk, true) && shots()[0].eye.size() >= 2 && std::fabs(shots()[0].eye.front().y - 14.f) < 0.01f && std::fabs(shots()[0].look.z - lk.z) < 0.01f,
                          "start view: first eye point + look");
                    const float last_x = shots()[0].eye.back().x;
                    CHECK(set_shot_view(sh, ctrl1, 2, {W * 0.5f + 30.f, 20.f, H * 0.5f}, lk, false) && std::fabs(shots()[0].eye.back().y - 20.f) < 0.01f &&
                              std::fabs(shots()[0].eye.back().x - last_x) > 1.f && std::fabs(shots()[0].look.z - lk.z) < 0.01f && std::fabs(shots()[0].eye.front().y - 14.f) < 0.01f,
                          "end view: last eye point only, look kept");
                    set_shot_length(sh, ctrl1, 8.f);
                    object_by_uid(sh.world, path1)->set_f32("duration", 8.f);
                    object_by_uid(sh.world, ctrl1)->set_f32("CameraDuration", 12.f);  // typed in Properties (in place)
                    CHECK(carry_path_duration(sh, ctrl1, 8.f) && std::fabs(shots()[0].len - 12.f) < 0.01f && std::fabs(shots()[0].move_len - 12.f) < 0.01f, "a length typed in Properties carries the path's duration (linked)");
                    object_by_uid(sh.world, path1)->set_f32("duration", 3.f);
                    object_by_uid(sh.world, ctrl1)->set_f32("CameraDuration", 9.f);
                    CHECK(!carry_path_duration(sh, ctrl1, 12.f) && std::fabs(shots()[0].move_len - 3.f) < 0.01f, "a held path keeps its own duration when typed in Properties");
                }
                {   // The game starts a path at its waypoint group's owner (MAPath start 0x15705D) and looks at focus + Offset (0x1C9892):
                    // the reader shows both; framing a shot moves the group under "Paths" (owner marker gone) and zeroes the Offset.
                    Scenario sc;
                    CHECK(io.create(mp, sc, &err), "create: %s", err.c_str());
                    const uint32_t cc = io.add_cutscene(sc, {W * 0.4f, 0, H * 0.4f}, "Cam1", nullptr, false);
                    auto cs = [&]() { return cutscene_shots(sc.world, cc); };
                    auto near3 = [](Vec3 a, Vec3 b, float e) { return std::fabs(a.x - b.x) < e && std::fabs(a.y - b.y) < e && std::fabs(a.z - b.z) < e; };
                    auto group_parent = [&](uint32_t path) -> const WObj* {
                        const WObj* p = object_by_uid(sc.world, path);
                        const WObj* g = p ? object_by_uid(sc.world, p->u32("m_wptGroupUID")) : nullptr;
                        return g ? object_by_uid(sc.world, g->parent()) : nullptr;
                    };
                    auto s0 = cs();
                    const WObj* own = s0.size() == 1 ? group_parent(s0[0].eye_path) : nullptr;
                    Vec3 op{}, mk{};
                    const bool own_pos = own && own->cls == key_of("MADummyObj") && world::get_pos(*own, op);
                    const WObj* fm = s0.size() == 1 ? object_by_uid(sc.world, s0[0].focus) : nullptr;
                    CHECK(own_pos && s0[0].eye.size() == 4 && near3(s0[0].eye[0], op, 0.001f), "the template path reads as the game flies it: owner marker first, then 3 waypoints (%zu)",
                          s0.empty() ? size_t(0) : s0[0].eye.size());
                    CHECK(fm && world::get_pos(*fm, mk) && std::fabs(s0[0].look.y - mk.y - 10.f) < 0.001f, "the look is the focus marker + its Offset (0, 10, 0)");
                    const uint32_t owner_uid = own ? own->uid() : 0, c1 = s0.empty() ? 0 : s0[0].ctrl;
                    const Vec3 e{W * 0.45f, 18.f, H * 0.45f}, lk{W * 0.45f, 10.f, H * 0.45f + 44.f};
                    CHECK(set_shot_view(sc, c1, 0, e, lk, true, &io), "Frame it / Use this view writes");
                    auto s1 = cs();
                    const WObj* np = group_parent(s1[0].eye_path);
                    CHECK(np && np->cls == key_of("MAGroup") && np->description() == "Paths" && !object_by_uid(sc.world, owner_uid),
                          "the eye path's group now sits in the cutscene's Paths group, the flyby marker is gone");
                    CHECK(s1[0].still && s1[0].eye.size() == 2 && near3(s1[0].eye[0], e, 0.001f) && near3(s1[0].look, lk, 0.001f) && near3(object_by_uid(sc.world, s1[0].focus)->vec3("ObjPosition"), lk, 0.001f),
                          "still shot: the game's path is the two points at the eye, the look is the marker itself (Offset 0)");
                    const Vec3 st{W * 0.42f, 30.f, H * 0.42f}, en{W * 0.42f + 20.f, 25.f, H * 0.42f};
                    set_shot_view(sc, c1, 1, st, lk, false, &io);
                    set_shot_view(sc, c1, 2, en, lk, false, &io);
                    Wdf back;
                    CHECK(back.parse(sc.world.serialize()), "save round trip");
                    auto s2 = cutscene_shots(back, cc);
                    CHECK(s2.size() == 1 && s2[0].eye.size() == 2 && near3(s2[0].eye.front(), st, 0.001f) && near3(s2[0].eye.back(), en, 0.001f) && !s2[0].still,
                          "moving shot after save: start and end are the only path points");
                    // Draw a path on the map detaches too (a fresh template copy).
                    const uint32_t cc2 = io.add_cutscene(sc, {W * 0.6f, 0, H * 0.6f}, "Cam1b", nullptr);
                    const uint32_t p2 = cutscene_shots(sc.world, cc2)[0].eye_path;
                    CHECK(camera_path_point(sc, p2, W * 0.6f, H * 0.6f, true) && camera_path_point(sc, p2, W * 0.6f + 30.f, H * 0.6f, false) &&
                              cutscene_shots(sc.world, cc2)[0].eye.size() == 2 && group_parent(p2) && group_parent(p2)->cls == key_of("MAGroup"),
                          "a drawn flight path is only the drawn points");
                }
                set_shot_length(sh, ctrl1, 8.f);
                object_by_uid(sh.world, path1)->set_f32("duration", 8.f);
                // End and start actions on shot 1 (what a copy of the last shot used to duplicate).
                const uint32_t init1 = shots()[0].init, exit1 = shots()[0].exit;
                uint32_t a_in = add_action_object(sh, init1, "MAActionBroadcast", "Start text");
                uint32_t a_ex = add_action_object(sh, exit1, "MAActionBroadcast", "End text");
                CHECK(a_in && a_ex && kids_of(init1) == 1 && kids_of(exit1) == 1, "setup: one action under shot 1's start and end triggers");
                const Vec3 eyeA{W * 0.4f, 80.f, H * 0.4f}, lookA{W * 0.5f, 22.f, H * 0.5f};
                uint32_t c2 = io.add_cutscene_shot(sh, cin, &eyeA, &lookA, 5.f);
                const Vec3 eyeB{W * 0.6f, 55.f, H * 0.35f}, lookB{W * 0.55f, 20.f, H * 0.45f};
                uint32_t c3 = io.add_cutscene_shot(sh, cin, &eyeB, &lookB, 5.f);
                auto sv = shots();
                CHECK(c2 && c3 && sv.size() == 3 && std::fabs(sv[0].len - 8.f) < 0.01f && std::fabs(sv[1].len - 5.f) < 0.01f && std::fabs(sv[2].len - 5.f) < 0.01f &&
                          std::fabs(cutscene_total(sv) - 18.f) < 0.01f && std::fabs(sv[2].start - 13.f) < 0.01f,
                      "Add shot x2: lengths 8, 5, 5 (total %.1f)", cutscene_total(sv));
                if (sv.size() == 3) {
                    auto near3 = [](Vec3 a, Vec3 b, float e) { return std::fabs(a.x - b.x) < e && std::fabs(a.y - b.y) < e && std::fabs(a.z - b.z) < e; };
                    CHECK(sv[1].still && sv[1].eye.size() == 2 && near3(sv[1].eye[0], eyeA, 0.001f) && std::fabs(sv[1].eye[1].x - sv[1].eye[0].x - kStillGap) < 0.0005f &&
                              sv[1].eye[1].y == sv[1].eye[0].y && sv[1].eye[1].z == sv[1].eye[0].z && near3(sv[1].look, lookA, 0.001f),
                          "a new shot is still: two eye points 2 cm apart at the given eye, the focus at the given look");
                    CHECK(sv[2].still && near3(sv[2].eye[0], eyeB, 0.001f) && near3(sv[2].look, lookB, 0.001f) && std::fabs(sv[2].move_len - 5.f) < 0.01f &&
                              std::fabs(sv[1].move_len - 5.f) < 0.01f,
                          "shot 3 stands at its own eye / look; the path's move_len = its length");
                    const WObj* cam2 = object_by_uid(sh.world, sv[1].cam);
                    Vec3 cp{};
                    CHECK(cam2 && world::get_pos(*cam2, cp) && near3(cp, eyeA, 0.001f), "the shot's camera object stands at the eye too");
                    CHECK(sv[0].eye_path != sv[1].eye_path && sv[1].eye_path != sv[2].eye_path && sv[0].focus != sv[1].focus && sv[1].cam != sv[2].cam && sv[1].init != sv[2].init,
                          "every shot owns its path, focus, camera and triggers");
                    // Nothing it carries is copied; the cutscene's end actions are under the LAST shot now.
                    CHECK(kids_of(sv[1].init) == 0 && kids_of(sv[2].init) == 0 && kids_of(sv[1].exit) == 0, "the new shots' start / end triggers hold no actions");
                    CHECK(kids_of(init1) == 1 && kids_of(exit1) == 0 && kids_of(sv[2].exit) == 1, "shot 1 keeps its start action; the end action moved to the last shot");
                    // Delete the last shot: the end actions go back to the new last shot.
                    {
                        Scenario sd = sh;
                        io.remove_cutscene_shot(sd, sv[2].ctrl);
                        auto d2 = cutscene_shots(sd.world, cin);
                        int n_end = 0;
                        for (auto& o : sd.world.objs) n_end += d2.size() == 2 && o.parent() == d2[1].exit && is_trigger_action(gd.names(), o);
                        CHECK(d2.size() == 2 && n_end == 1, "deleting the last shot moves the end action to the new last (%zu shots, %d end action)", d2.size(), n_end);
                    }
                }
                std::set<uint32_t> ids;
                bool dup = false;
                for (auto& o : sh.world.objs) dup |= !ids.insert(o.uid()).second;
                CHECK(!dup, "no duplicate uid after two Add shots");
                // No eye given (the old call): the pieces move 25 m aside, the length is 5 s.
                {
                    Scenario sn = sh;
                    uint32_t c4 = io.add_cutscene_shot(sn, cin);
                    auto s4 = cutscene_shots(sn.world, cin);
                    CHECK(c4 && s4.size() == 4 && std::fabs(s4[3].len - 5.f) < 0.01f, "Add shot without a view still gives a 5 s shot");
                }
                // A deleted eye path: still, no crash, and ensure_shot_eye_path restores a two-point path.
                {
                    Scenario sp = sh;
                    auto s0 = cutscene_shots(sp.world, cin);
                    wedit::remove_subtree(sp.world, s0[0].eye_path);
                    auto s1 = cutscene_shots(sp.world, cin);
                    CHECK(s1.size() == 3 && s1[0].still && s1[0].eye_path == 0 && s1[0].eye.size() == 1, "a deleted eye path reads still at the camera's own spot");
                    bool flagged = false;
                    for (auto& v : io.validate(sp)) flagged |= v.text.find("Shot 1 of \"Flyby\" has no camera path or focus left") != std::string::npos;
                    CHECK(flagged, "Check names the shot whose path is gone");
                    uint32_t np = io.ensure_shot_eye_path(sp, s0[0].ctrl);
                    auto s2 = cutscene_shots(sp.world, cin);
                    CHECK(np && s2[0].eye_path == np && s2[0].eye.size() == 2 && !s2[0].still, "ensure_shot_eye_path restores a two-point path (%zu points)", s2[0].eye.size());
                    Vec3 eyeC{W * 0.2f, 70.f, H * 0.2f};
                    CHECK(set_shot_view(sp, s0[0].ctrl, 0, eyeC, lookA, true, &io) && cutscene_shots(sp.world, cin)[0].still && cutscene_shots(sp.world, cin)[0].eye.size() == 2,
                          "set_shot_view(still) on the restored path");
                    // Without a path and without ensure: the camera object still takes the view.
                    Scenario sq = sh;
                    wedit::remove_subtree(sq.world, s0[0].eye_path);
                    CHECK(set_shot_view(sq, s0[0].ctrl, 0, eyeC, lookA, true, nullptr), "set_shot_view with no path and no io moves the camera object");
                    {   // path object kept, its waypoints gone: the points are rebuilt, the path object stays
                        Scenario sg = sh;
                        uint32_t grp = object_by_uid(sg.world, s0[0].eye_path)->u32("m_wptGroupUID");
                        std::vector<uint32_t> wps;
                        for (auto& o : sg.world.objs) if (o.parent() == grp) wps.push_back(o.uid());
                        for (uint32_t u : wps) wedit::remove_subtree(sg.world, u);
                        uint32_t np2 = io.ensure_shot_eye_path(sg, s0[0].ctrl);
                        CHECK(np2 == s0[0].eye_path && cutscene_shots(sg.world, cin)[0].eye.size() == 2, "a path with its points gone gets two new ones, same path object");
                    }
                }
                // Moving start / end of a moving shot.
                {
                    Scenario sm = sh;
                    auto s0 = cutscene_shots(sm.world, cin);
                    const Vec3 st{W * 0.11f, 61.f, H * 0.12f}, en{W * 0.31f, 63.f, H * 0.32f};
                    set_shot_view(sm, s0[0].ctrl, 1, st, {}, false, &io);
                    set_shot_view(sm, s0[0].ctrl, 2, en, {}, false, &io);
                    auto s1 = cutscene_shots(sm.world, cin);
                    CHECK(s1[0].eye.size() == s0[0].eye.size() && std::fabs(s1[0].eye.front().x - st.x) < 0.001f && std::fabs(s1[0].eye.back().z - en.z) < 0.001f &&
                              std::fabs(s1[0].eye.back().y - en.y) < 0.001f && !s1[0].still,
                          "Frame start / end write the first / last eye point (heights absolute)");
                }
                // Check rows: a long cutscene, a long hold, a mission that ends while it plays.
                {
                    Scenario sc = sh;
                    auto s0 = cutscene_shots(sc.world, cin);
                    set_shot_length(sc, s0[0].ctrl, 30.f);
                    object_by_uid(sc.world, s0[0].eye_path)->set_f32("duration", 8.f);
                    set_shot_length(sc, s0[1].ctrl, 30.f);
                    set_shot_length(sc, s0[2].ctrl, 30.f);
                    uint32_t pl = add_trigger_object(sc, "MATriggerTimer", "Play it");
                    object_by_uid(sc.world, pl)->set_f32("TrgTimerDurationSeconds", 5.f);
                    object_by_uid(sc.world, add_action_object(sc, pl, "MAActionActivateCinema", "Play flyby"))->set_u32("cinemaID", cin);
                    uint32_t win = add_trigger_object(sc, "MATriggerTimer", "Win at 50 s");
                    object_by_uid(sc.world, win)->set_f32("TrgTimerDurationSeconds", 50.f);
                    if (WObj* a = object_by_uid(sc.world, add_action_object(sc, win, "MAActionGameCondition", "Win"))) a->set_i32("GameCondition", 2);
                    bool lng = false, hold = false, ends = false;
                    for (auto& v : io.validate(sc)) {
                        lng |= v.text.find("Cutscene \"Flyby\" is 90.0 s long") != std::string::npos;
                        hold |= v.text.find("(shot 1 holds still for 22.0 s)") != std::string::npos;
                        ends |= v.text.find("The mission ends (trigger \"Win at 50 s\" at 50.0 s) while cutscene \"Flyby\" (5.0-95.0 s) still plays") != std::string::npos;
                    }
                    CHECK(lng && hold && ends, "Check rows: long cutscene %d, hold %d, mission ends during the cutscene %d", lng, hold, ends);
                    int rows = 0;
                    for (auto& v : io.validate(sh)) rows += v.text.find("Cutscene \"Flyby\"") != std::string::npos || v.text.find("while cutscene") != std::string::npos;
                    CHECK(rows == 0, "a normal 18 s cutscene raises no cutscene row (%d)", rows);
                }
                // Save and reopen: the same shots.
                {
                    fs::path outp = dir / L"Mission_F2W1.ma2map";
                    SaveReport repp;
                    CHECK(io.save(sh, outp, &repp, &err), "save: %s", err.c_str());
                    Scenario lw;
                    std::string lerr;
                    if (io.load(outp, lw, &lerr)) {
                        auto a = cutscene_shots(sh.world, cin), b = cutscene_shots(lw.world, cin);
                        bool same = a.size() == b.size();
                        for (size_t i = 0; same && i < a.size(); i++)
                            same = std::fabs(a[i].len - b[i].len) < 1e-4f && a[i].eye.size() == b[i].eye.size() && a[i].still == b[i].still && std::fabs(a[i].start - b[i].start) < 1e-4f &&
                                   a[i].eye_path == b[i].eye_path && a[i].has_look == b[i].has_look && (!a[i].has_look || std::fabs(a[i].look.x - b[i].look.x) < 1e-3f);
                        CHECK(same, "save and reopen: the same shots (%zu / %zu)", a.size(), b.size());
                        int exits = 0;
                        for (auto& o : lw.world.objs) exits += b.size() == 3 && o.parent() == b[2].exit && is_trigger_action(gd.names(), o);
                        CHECK(exits == 1, "reopened: the end action is under the last shot (%d)", exits);
                    } else CHECK(false, "reopen: %s", lerr.c_str());
                }
                // The triggers a cutscene owns (shot start / end, cue timers) stay off the Triggers list; a user's timer stays on.
                {
                    auto sv = cutscene_shots(sh.world, cin);
                    uint32_t own_t = add_trigger_object(sh, "MATriggerTimer", "Message after 30 s");
                    uint32_t cue = add_trigger_object(sh, "MATriggerTimer", "Flyby - shot 2 + 1 s");
                    if (WObj* o = object_by_uid(sh.world, cue)) o->set_i32("TriggerEnabled", 0);
                    uint32_t plain_off = add_trigger_object(sh, "MATriggerTimer", "Asleep timer");
                    if (WObj* o = object_by_uid(sh.world, plain_off)) o->set_i32("TriggerEnabled", 0);
                    uint32_t start = sv.size() > 1 ? sv[1].init : 0;
                    uint32_t wake = start ? add_action_object(sh, start, "MAActionAI", "Start the shot 2 timer") : 0;
                    if (WObj* a = object_by_uid(sh.world, wake)) { a->set_u32("ActionEntity", cue); a->set_i32("EnableAI", 1); }
                    auto owned = cutscene_owned_triggers(sh.world);
                    CHECK(start && owned.count(start) && owned.count(sv[0].exit) && owned.count(cue), "owned: shot start / end triggers and a cue timer");
                    CHECK(!owned.count(own_t) && !owned.count(plain_off), "owned: a plain timer and a disabled timer nothing starts are the user's");
                }
            }
            // Events on the timeline: an action at 0 s under the shot's start trigger, later ones under a cue timer (disabled, Repeat,
            // switches itself off, started from the shot's start trigger), orders, move / remove, shots reordered and deleted, save / reopen,
            // the Check rows, and the "Play from its own view" fix for shots that hang under the template's flyby marker.
            {
                Scenario se;
                CHECK(io.create(mp, se, &err), "create: %s", err.c_str());
                const float W = se.terrain.size_x(), H = se.terrain.size_z();
                const uint32_t cin = io.add_cutscene(se, {W * 0.3f, 0, H * 0.3f}, "Evt", nullptr);
                const Vec3 eA{W * 0.4f, 80.f, H * 0.4f}, lA{W * 0.5f, 22.f, H * 0.5f};
                io.add_cutscene_shot(se, cin, &eA, &lA, 5.f);
                io.add_cutscene_shot(se, cin, &eA, &lA, 5.f);
                set_shot_length(se, cutscene_shots(se.world, cin)[0].ctrl, 5.f);  // 5, 5, 5: shot 2 starts at 5, shot 3 at 10
                auto shots = [&]() { return cutscene_shots(se.world, cin); };
                const NameTable& nm = gd.names();
                auto find_cls = [&](const char* cls, uint32_t parent) {
                    uint32_t r = 0;
                    for (auto& o : se.world.objs) if (o.cls == key_of(cls) && (!parent || o.parent() == parent)) r = o.uid();
                    return r;
                };
                const std::vector<ShotInfo> sv = shots();
                CHECK(sv.size() == 3 && std::fabs(sv[1].start - 5.f) < 0.01f, "setup: three 5 s shots");
                if (sv.size() == 3) {
                    // 0 s: straight under the start trigger.
                    const uint32_t a0 = cutscene_add_action(se, cin, 0.f, "MAActionBroadcast", "Hello");
                    CHECK(a0 && object_by_uid(se.world, a0)->parent() == sv[0].init, "an action at 0 s goes under shot 1's start trigger");
                    // 6 s: shot 2 + 1 s: a cue timer.
                    const uint32_t a1 = cutscene_add_action(se, cin, 6.f, "MAActionBroadcast", "Later");
                    const uint32_t timer = a1 ? object_by_uid(se.world, a1)->parent() : 0;
                    const WObj* to = object_by_uid(se.world, timer);
                    CHECK(to && to->cls == key_of("MATriggerTimer") && to->i32("TriggerEnabled") == 0 && to->i32("TriggerAutoReset") == 1 &&
                              std::fabs(to->f32("TrgTimerDurationSeconds") - 1.f) < 0.001f && to->description().find("Evt - shot 2 + 1 s") != std::string::npos,
                          "an action at 6 s (shot 2 + 1 s) hangs under a cue timer: disabled, Repeat on, 1.0 s, named after the shot");
                    const WObj* tg = to ? object_by_uid(se.world, to->parent()) : nullptr;
                    CHECK(tg && tg->cls == key_of("MAGroup") && tg->description() == "Cutscene timers", "the cue timer lives in the root group \"Cutscene timers\"");
                    int starts = 0;
                    for (auto& o : se.world.objs)
                        if (o.cls == key_of("MAActionAI") && o.parent() == sv[1].init && o.u32("ActionEntity") == timer && o.i32("EnableAI") == 1) starts++;
                    CHECK(starts == 1, "shot 2's start trigger holds one action that switches the timer on (%d)", starts);
                    uint32_t last = 0;
                    for (auto& o : se.world.objs) if (o.parent() == timer) last = o.uid();
                    const WObj* lo = object_by_uid(se.world, last);
                    CHECK(lo && lo->cls == key_of("MAActionAI") && lo->u32("ActionEntity") == timer && lo->i32("EnableAI") == 0 && last != a1,
                          "the timer's LAST action switches the timer itself off");
                    const uint32_t a2 = cutscene_add_action(se, cin, 6.f, "MAActionSound", "Sound");
                    CHECK(a2 && object_by_uid(se.world, a2)->parent() == timer, "a second event at 6 s shares the timer");
                    uint32_t last2 = 0;
                    for (auto& o : se.world.objs) if (o.parent() == timer) last2 = o.uid();
                    CHECK(last2 == last, "... and the switch-off stays the timer's last action");
                    auto evs = cutscene_events(se.world, nm, cin);
                    CHECK(evs.size() == 3 && evs[0].obj == a0 && std::fabs(evs[0].t) < 0.001f && evs[0].shot == 0 && evs[1].shot == 1 && std::fabs(evs[1].offset - 1.f) < 0.001f &&
                              std::fabs(evs[1].t - 6.f) < 0.001f,
                          "cutscene_events lists the three events in time order, no plumbing (%zu)", evs.size());
                    CHECK(cutscene_owned_triggers(se.world).count(timer), "the cue timer is a cutscene-owned trigger (off the Triggers list)");
                    // Move 6 s > 0 s: both leave the timer, which goes with its start action.
                    CHECK(cutscene_move_event(se, nm, cin, a1, 0.f) && cutscene_move_event(se, nm, cin, a2, 0.f), "move both events to 0 s");
                    CHECK(!object_by_uid(se.world, timer) && object_by_uid(se.world, a1)->parent() == sv[0].init, "the emptied cue timer is deleted");
                    int starts2 = 0;
                    for (auto& o : se.world.objs) starts2 += o.cls == key_of("MAActionAI") && o.parent() == sv[1].init && o.i32("EnableAI") == 1;
                    CHECK(starts2 == 0, "... and so is its start action (%d left)", starts2);
                    // Remove an event; the timer of the other one stays while it has content.
                    const uint32_t b1 = cutscene_add_action(se, cin, 12.f, "MAActionBroadcast", "Shot 3 + 2");
                    const uint32_t b2 = cutscene_add_action(se, cin, 12.f, "MAActionBroadcast", "Shot 3 + 2 again");
                    const uint32_t t3 = b1 ? object_by_uid(se.world, b1)->parent() : 0;
                    CHECK(t3 && cutscene_remove_event(se, nm, cin, b1) && object_by_uid(se.world, t3), "removing one of two events keeps the timer");
                    CHECK(cutscene_remove_event(se, nm, cin, b2) && !object_by_uid(se.world, t3), "removing the last event removes the timer");
                    // Reordering shots keeps events with their shot (they hang under the shot's own triggers).
                    const uint32_t c1 = cutscene_add_action(se, cin, 6.5f, "MAActionBroadcast", "Moves with shot 2");
                    const uint32_t c1t = c1 ? object_by_uid(se.world, c1)->parent() : 0;
                    {
                        Scenario sr = se;
                        wedit::move_sibling(sr.world, sv[1].ctrl, +1);  // shot 2 plays after shot 3
                        auto e2 = cutscene_events(sr.world, nm, cin);
                        const CutEvent* mine = nullptr;
                        for (auto& e : e2) if (e.obj == c1) mine = &e;
                        auto s2 = cutscene_shots(sr.world, cin);
                        CHECK(mine && s2.size() == 3 && s2[2].ctrl == sv[1].ctrl && mine->shot == 2 && std::fabs(mine->t - 11.5f) < 0.001f,
                              "reordering shots: the event moves with its shot (now %.1f s)", mine ? double(mine->t) : -1.0);
                    }
                    // Order events: a unit's AI order starts at the cue; a sleeping unit also gets its wake action.
                    auto unit = [&](float fx, float fz, int ai) {
                        uint32_t u = io.place_template(se, 27265, {W * fx, 0, H * fz}, 3.1f);
                        if (WObj* o = object_by_uid(se.world, u)) { o->set_i32("Nation", 6); o->set_i32("m_bAIEnabled", ai); }
                        return u;
                    };
                    const uint32_t un = unit(0.6f, 0.6f, 1), us = unit(0.62f, 0.6f, 0);
                    const uint32_t ev1 = cutscene_add_order(se, cin, 7.f, un, "MAPatrol", false);
                    const WObj* eo = object_by_uid(se.world, ev1);
                    const uint32_t holder = eo ? eo->u32("TriggerID") : 0;
                    const WObj* ho = object_by_uid(se.world, holder);
                    CHECK(ev1 && ho && ho->cls == key_of("MATriggerTimer") && std::fabs(ho->f32("TrgTimerDurationSeconds") - 2.f) < 0.001f, "an order at 7 s (shot 2 + 2 s): MAEvent TriggerID = a 2.0 s cue timer");
                    const uint32_t pg = order_path(se.world, ev1);
                    CHECK(pg && object_by_uid(se.world, pg)->i32("TraverseType") == 1, "\"A unit walks a path\": the new path walks once (TraverseType 1)");
                    order_add_point(se, ev1, W * 0.55f, H * 0.6f);
                    CHECK(object_by_uid(se.world, pg)->i32("TraverseType") == 1, "drawing points keeps walk-once");
                    const uint32_t ev2 = cutscene_add_order(se, cin, 7.f, us, "MAAttack");
                    int wakes = 0;
                    for (auto& o : se.world.objs) wakes += o.cls == key_of("MAActionAI") && o.parent() == holder && o.u32("ActionEntity") == us && o.i32("EnableAI") == 1;
                    CHECK(ev2 && wakes == 1, "a sleeping unit's order brings its wake action under the same cue timer (%d)", wakes);
                    uint32_t lastk = 0;
                    for (auto& o : se.world.objs) if (o.parent() == holder) lastk = o.uid();
                    CHECK(object_by_uid(se.world, lastk)->i32("EnableAI") == 0 && object_by_uid(se.world, lastk)->u32("ActionEntity") == holder, "the switch-off is still last after the orders");
                    auto e3 = cutscene_events(se.world, nm, cin);
                    int orders = 0, plumbing = 0;
                    for (auto& e : e3) { orders += e.order; plumbing += !e.order && (e.obj == lastk); }
                    CHECK(orders == 2 && plumbing == 0, "both orders are events; the wake and the switch-off are not (%d orders)", orders);
                    CHECK(cutscene_move_event(se, nm, cin, ev2, 0.f), "move the attack order to 0 s");
                    int wakes2 = 0, wakes3 = 0;
                    for (auto& o : se.world.objs) {
                        wakes2 += o.cls == key_of("MAActionAI") && o.parent() == holder && o.u32("ActionEntity") == us && o.i32("EnableAI") == 1;
                        wakes3 += o.cls == key_of("MAActionAI") && o.parent() == sv[0].init && o.u32("ActionEntity") == us && o.i32("EnableAI") == 1;
                    }
                    CHECK(object_by_uid(se.world, ev2)->u32("TriggerID") == sv[0].init && wakes2 == 0 && wakes3 == 1, "moving an order moves its wake action with it (%d, %d)", wakes2, wakes3);
                    // End actions: under the last shot's end trigger; they follow the last shot.
                    const uint32_t en = cutscene_add_end_action(se, cin, "MAActionBroadcast", "HANDBACK");
                    auto ends = cutscene_end_actions(se.world, nm, cin);
                    CHECK(en && ends.size() == 1 && ends[0].obj == en && std::fabs(ends[0].t - cutscene_total(shots())) < 0.001f && object_by_uid(se.world, en)->parent() == sv[2].exit,
                          "an end action sits under the last shot's end trigger and reads as the total");
                    {
                        Scenario sd = se;
                        io.remove_cutscene_shot(sd, sv[2].ctrl);
                        auto d2 = cutscene_shots(sd.world, cin);
                        auto de = cutscene_end_actions(sd.world, nm, cin);
                        CHECK(d2.size() == 2 && de.size() == 1 && object_by_uid(sd.world, en)->parent() == d2[1].exit, "deleting the last shot hands the end action to the new last shot");
                        Scenario sk = se;
                        const uint32_t tm_before = c1t;
                        io.remove_cutscene_shot(sk, sv[1].ctrl);  // shot 2 holds the 6.5 s event and the 7 s order
                        CHECK(!object_by_uid(sk.world, tm_before) && !object_by_uid(sk.world, ev1) && object_by_uid(sk.world, ev2),
                              "deleting a shot deletes its cue timers and the orders waiting for them (the 0 s order of shot 1 stays)");
                    }
                    // Save and reopen: the same events.
                    {
                        fs::path outp = dir / L"Mission_F2W4.ma2map";
                        SaveReport repp;
                        CHECK(io.save(se, outp, &repp, &err), "save: %s", err.c_str());
                        Scenario lw;
                        std::string lerr;
                        if (io.load(outp, lw, &lerr)) {
                            auto a = cutscene_events(se.world, nm, cin), b = cutscene_events(lw.world, nm, cin);
                            bool same = a.size() == b.size();
                            for (size_t i = 0; same && i < a.size(); i++) same = a[i].obj == b[i].obj && std::fabs(a[i].t - b[i].t) < 1e-4f && a[i].order == b[i].order && a[i].shot == b[i].shot;
                            CHECK(same, "save and reopen: the same events (%zu / %zu)", a.size(), b.size());
                            CHECK(cutscene_end_actions(lw.world, nm, cin).size() == 1, "reopened: the end action is still there");
                        } else CHECK(false, "reopen: %s", lerr.c_str());
                    }
                    // Check: an event after the cutscene's end.
                    {
                        Scenario sc = se;
                        const uint32_t late = cutscene_add_action(sc, cin, 17.f, "MAActionBroadcast", "Too late");
                        bool row = false;
                        for (auto& v : io.validate(sc)) row |= v.text.find("an event at 17.0 s") != std::string::npos && v.text.find("is after its end (15.0 s)") != std::string::npos;
                        CHECK(late && row, "Check: an event at 17.0 s is after the cutscene's end (15.0 s)");
                        bool quiet = true;
                        for (auto& v : io.validate(se)) quiet &= v.text.find("is after its end") == std::string::npos;
                        CHECK(quiet, "Check: no end row for events inside the cutscene");
                    }
                    (void)find_cls;
                }
                // The fix: an editor-made shot that still hangs under the template's flyby marker (a pre- file) gets a Check row; "Play
                // from its own view" detaches it and zeroes the look offset; a retail cinema is never listed.
                {
                    Scenario sx;
                    CHECK(io.create(mp, sx, &err), "flyby create: %s", err.c_str());
                    const uint32_t old_c = io.add_cutscene(sx, {sx.terrain.size_x() * 0.3f, 0, sx.terrain.size_z() * 0.3f}, "Old flyby", nullptr, false);
                    auto s0 = cutscene_shots(sx.world, old_c);
                    WTree tt;
                    tt.build(sx.world);
                    CHECK(s0.size() == 1 && shot_hangs_under_marker(sx.world, tt, s0[0]), "a fresh template shot hangs under the flyby marker (the state old files are in)");
                    bool row = false;
                    int fix = 0;
                    std::vector<uint32_t> fixu;
                    for (auto& v : io.validate(sx)) if (v.text.find("\"Old flyby\" fly") != std::string::npos) { row = true; fix = v.fix; fixu = v.uids; }
                    CHECK(row && fix == 4 && fixu.size() == 1 && fixu[0] == s0[0].ctrl, "Check lists it with the fix \"Play from its own view\" (fix %d)", fix);
                    CHECK(cutscene_play_from_own_view(sx, s0[0].ctrl), "Play from its own view changes the shot");
                    auto s1 = cutscene_shots(sx.world, old_c);
                    tt.build(sx.world);
                    bool still_row = false;
                    for (auto& v : io.validate(sx)) still_row |= v.text.find("\"Old flyby\" fly") != std::string::npos;
                    Vec3 mk{};
                    const WObj* fm = object_by_uid(sx.world, s1[0].focus);
                    CHECK(s1[0].eye.size() == s0[0].eye.size() - 1 && !shot_hangs_under_marker(sx.world, tt, s1[0]) && !still_row && fm && world::get_pos(*fm, mk) &&
                              std::fabs(s1[0].look.y - mk.y) < 0.001f && !cutscene_play_from_own_view(sx, s0[0].ctrl),
                          "after the fix the path is only its own points (%zu of %zu), the look is the marker, the row is gone, a second press changes nothing",
                          s1[0].eye.size(), s0[0].eye.size());
                    // a retail cinema (copy of Mission_05) is never listed
                    NewMapParams rp;
                    rp.copy_map = "Mission_05";
                    rp.start = NewMapParams::CopyRetail;
                    rp.name = "retail copy";
                    rp.multiplayer = false;
                    Scenario sr;
                    std::string rerr;
                    if (io.create(rp, sr, &rerr)) {
                        bool any = false;
                        for (auto& v : io.validate(sr)) any |= v.fix == 4;
                        CHECK(!any, "a retail cinema (Mission_05 copy) is never offered the fix");
                    }
                }
                {   // A fresh New intro (behind the start) is detached from the retail flyby marker at creation; a Neutral unit with orders is flagged.
                    Scenario sw;
                    CHECK(io.create(mp, sw, &err), "create: %s", err.c_str());
                    const float face = 0.5f;
                    const uint32_t ic = io.add_cutscene(sw, {sw.terrain.size_x() * 0.5f, 0, sw.terrain.size_z() * 0.5f}, "Fresh intro", &face);
                    auto is0 = cutscene_shots(sw.world, ic);
                    WTree tw;
                    tw.build(sw.world);
                    CHECK(is0.size() == 1 && !shot_hangs_under_marker(sw.world, tw, is0[0]), "a fresh intro does not hang under the flyby marker");
                    bool hang_row = false;
                    for (auto& v : io.validate(sw)) hang_row |= v.text.find("\"Fresh intro\" fly") != std::string::npos;
                    CHECK(!hang_row, "Check has no 'Play from its own view' row for a fresh intro");
                    CHECK(is0[0].eye.size() == 3, "the intro's own flight is its 3 waypoints (%zu)", is0[0].eye.size());
                    const uint32_t nu = io.place_template(sw, 27265, {sw.terrain.size_x() * 0.4f, 0, sw.terrain.size_z() * 0.4f}, 0.f);
                    if (WObj* o = object_by_uid(sw.world, nu)) { o->set_i32("Nation", 0); o->set_str("Description", "Neutral Hackman"); }
                    uint32_t tg = add_trigger_object(sw, "MATriggerTimer", "Orders: test");
                    const uint32_t nev = add_order(sw, tg, nu, "MAPatrol");
                    auto neutral_rows = [&]() {
                        int n = 0;
                        for (auto& v : io.validate(sw)) n += v.text.find("is Neutral, so it ignores its orders") != std::string::npos;
                        return n;
                    };
                    CHECK(nev && neutral_rows() == 1, "Check lists a Neutral unit that has orders (%d)", neutral_rows());
                    io.apply_unit_side(sw, nu, 5);
                    CHECK(neutral_rows() == 0, "on the enemy's side the row is gone");
                }
                {   // A side change on an EMPTY unit is flagged; crew_for_side_change gives it the retail
                    // "changes side later" form (Nation 0, crewed, asleep, no owner controller).
                    Scenario sc;
                    CHECK(io.create(mp, sc, &err), "TS1 create: %s", err.c_str());
                    const uint32_t cu = io.place_template(sc, 27265, {sc.terrain.size_x() * 0.4f, 0, sc.terrain.size_z() * 0.4f}, 0.f);
                    io.apply_unit_side(sc, cu, 0);
                    const WObj* co = object_by_uid(sc.world, cu);
                    CHECK(co && co->i32("StartAbandoned") == 1 && io.boardable(sc, cu), "TS1 a Neutral Cougar is empty and boardable");
                    const uint32_t tg = add_trigger_object(sc, "MATriggerTimer", "Side change");
                    WObj* na = object_by_uid(sc.world, add_action_object(sc, tg, "MAActionNation", "Turn enemy"));
                    CHECK(na != nullptr, "TS1 side-change action made");
                    if (na) { na->set_u32("ActionEntity", cu); na->set_i32("Nation", 5); }
                    auto empty_rows = [&]() {
                        int n = 0;
                        for (auto& v : io.validate(sc)) n += v.text.find("is empty (no pilot), so changing its side") != std::string::npos;
                        return n;
                    };
                    CHECK(empty_rows() == 1, "TS1 Check flags a side change on an empty unit (%d)", empty_rows());
                    CHECK(io.crew_for_side_change(sc, cu), "TS1 crew_for_side_change changes an empty unit");
                    co = object_by_uid(sc.world, cu);
                    CHECK(co && co->i32("StartAbandoned") == 0 && co->i32("AttackPilot") == 1 && co->i32("m_bAIEnabled") == 0 &&
                              co->i32("Nation") == 0 && !io.boardable(sc, cu),
                          "TS1 crewed form: Nation 0, StartAbandoned 0, AttackPilot 1, AI asleep, no owner controller");
                    CHECK(empty_rows() == 0, "TS1 the Check row is gone once it has a pilot");
                    CHECK(!io.crew_for_side_change(sc, cu), "TS1 a crewed unit is left alone");
                }
            }
            // 3. Older files: the trigger list in editor/project.json plus the hidden compiled
            //    group load as ordinary world objects (no "[ed] " names, no Editor Triggers group, no list).
            {
                Scenario sl;
                ok = io.create(mp, sl, &err);
                CHECK(ok, "create legacy mission: %s", err.c_str());
                if (ok) {
                    fill_mission(sl);  // the list stays, as an older editor saved it
                    fs::path out = dir / L"Mission_Legacy.ma2map";
                    SaveReport rep;
                    CHECK(io.save(sl, out, &rep, &err), "save legacy mission: %s", err.c_str());
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        int hidden = 0, groups = 0, trig = 0, broadcasts = 0, texts = 0;
                        for (auto& o : back.world.objs) {
                            std::string d = o.description();
                            hidden += d.compare(0, strlen(kEditorPrefix), kEditorPrefix) == 0;
                            groups += o.cls == key_of("MAGroup") && d == kEditorTriggerGroup;
                            trig += gd.names().label(o.cls).rfind("MATrigger", 0) == 0;
                            if (o.cls == key_of("MAActionBroadcast")) {
                                broadcasts++;
                                texts += own_text(back, o.u32("BodyTextID")) != nullptr;
                            }
                        }
                        CHECK(back.triggers.empty() && hidden == 0 && groups == 0, "legacy file: list gone, no [ed] objects, no Editor Triggers group (%d, %d)", hidden, groups);
                        CHECK(trig == 7 && broadcasts == 3 && texts == 3, "legacy file: %d triggers, %d messages with their own text (%d)", trig, broadcasts, texts);
                        report("legacy mission converted", back, dir / L"Mission_Legacy2.ma2map");
                    }
                }
            }
            // 3a. AI patrol paths and defend/teleport markers are real objects: the effect
            //     names them ("wpt" / "marker"), they stay visible under "AI Paths" in the editor and
            //     on save the waypoint group hangs under the unit's MAEvent like retail; old arguments convert.
            {
                Scenario s5;
                ok = io.create(mp, s5, &err);
                CHECK(ok, "create AI-path mission: %s", err.c_str());
                if (ok) {
                    uint32_t unit = 0;
                    for (auto& o : s5.world.objs) if (o.cls == key_of("MALevelStart")) { unit = o.uid(); break; }
                    float W = s5.terrain.size_x(), H = s5.terrain.size_z();
                    char pts[160], pt[40];
                    snprintf(pts, sizeof pts, "%d,%d;%d,%d;%d,%d", int(W * .4f), int(H * .4f), int(W * .45f), int(H * .4f), int(W * .45f), int(H * .45f));
                    snprintf(pt, sizeof pt, "%d,%d", int(W * .6f), int(H * .6f));
                    EdTrigger t1;
                    t1.name = "Orders";
                    t1.effects.push_back({"ai", {{"unit", std::to_string(unit)}, {"order", "patrol"}, {"path", pts}}});
                    t1.effects.push_back({"ai", {{"unit", std::to_string(unit)}, {"order", "defend"}, {"point", pt}, {"radius", "50"}}});
                    s5.triggers = {t1};
                    int conv = adopt_ai_objects(s5);
                    auto& ea = s5.triggers[0].effects;
                    uint32_t wpt = uint32_t(strtoul(ea[0].args["wpt"].c_str(), nullptr, 10));
                    uint32_t mk = uint32_t(strtoul(ea[1].args["marker"].c_str(), nullptr, 10));
                    CHECK(conv == 2 && wpt && mk && ea[0].args["path"].empty() && ea[1].args["point"].empty(), "old path/point converted to objects (%d)", conv);
                    CHECK(ai_path_length(s5, wpt) == 3, "path object has 3 waypoints");
                    convert_legacy_triggers(s5);
                    fs::path out = dir / L"Mission_AIPaths.ma2map";
                    report("ai paths", s5, out);
                    MgfReader mr;
                    Bytes wb;
                    Wdf cw;
                    if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                        WTree t;
                        t.build(cw);
                        int gi = t.find(wpt);
                        CHECK(gi >= 0 && cw.objs[size_t(gi)].cls == key_of("MAWaypointGroup"), "saved: the patrol path is a MAWaypointGroup");
                        if (gi >= 0) {
                            int p_ = t.parent_of[size_t(gi)];
                            CHECK(p_ >= 0 && cw.objs[size_t(p_)].cls == key_of("MAEvent"), "saved: the path hangs under the unit's MAEvent (retail)");
                            CHECK(t.kids[size_t(gi)].size() == 3, "saved: 3 waypoints");
                        }
                        int groups = 0, mks = 0;
                        for (auto& o : cw.objs) { groups += o.cls == key_of("MAWaypointGroup"); mks += o.uid() == mk; }
                        CHECK(groups == 1 && mks == 1, "no extra hidden copies (%d path groups, %d marker)", groups, mks);
                        int mi = t.find(mk);
                        int mp = mi >= 0 ? t.parent_of[size_t(mi)] : -1;
                        CHECK(mp >= 0 && cw.objs[size_t(mp)].description() == kAIPathGroup, "the marker stays in the visible AI Paths group");
                    }
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        CHECK(back.triggers.empty(), "reloaded orders: no trigger list");
                        WTree t2;
                        t2.build(back.world);
                        int ix = t2.find(wpt);
                        int pp = ix >= 0 ? t2.parent_of[size_t(ix)] : -1;
                        CHECK(pp >= 0 && back.world.objs[size_t(pp)].cls == key_of("MAEvent"), "reloaded path still hangs under its MAEvent");
                        int events = 0;
                        for (auto& o : back.world.objs) events += o.cls == key_of("MAEvent");
                        CHECK(events == 2, "reloaded: 2 orders (%d)", events);
                    }
                }
            }
            // 3b. Trigger boxes and circles are retail objects: a box condition names a
            //     MATriggerCuboidalVolume of the map ("vol"), saved as the trigger itself with the
            //     actions under it, with exactly the property layout of a retail volume; older files'
            //     box/circle arguments are converted at load into such objects.
            {
                Scenario s4;
                ok = io.create(mp, s4, &err);
                CHECK(ok, "create trigger-shape mission: %s", err.c_str());
                if (ok) {
                    float W = s4.terrain.size_x(), H = s4.terrain.size_z();
                    TriggerShape sh;
                    sh.x = W * 0.5f; sh.z = H * 0.5f; sh.base = 5; sh.w = 30; sh.l = 12; sh.h = 20; sh.heading = 30;
                    sh.name = "TB - gate";
                    uint32_t vol = add_trigger_shape(s4, sh);
                    CHECK(vol != 0, "add a trigger box");
                    EdTrigger gate;
                    gate.name = "Gate";
                    gate.conditions.push_back({"box", {{"vol", std::to_string(vol)}}});
                    gate.effects.push_back({"message", {{"text", "Gate box"}, {"seconds", "5"}}});
                    // An older file's trigger: shape in its own arguments.
                    EdTrigger old;
                    old.name = "Old box";
                    char pt[64];
                    snprintf(pt, sizeof pt, "%d,%d", int(W * 0.3f), int(H * 0.3f));
                    old.conditions.push_back({"box", {{"point", pt}, {"base", "8"}, {"height", "10"}, {"width", "40"}, {"length", "40"}, {"heading", "0"}}});
                    old.conditions.push_back({"area", {{"point", pt}, {"radius", "55"}, {"inside", "1"}}});
                    old.effects.push_back({"message", {{"text", "Old box"}, {"seconds", "5"}}});
                    s4.triggers = {gate, old};
                    int conv = adopt_trigger_shapes(s4);
                    const WObj* ob = condition_shape(s4, s4.triggers[1].conditions[0].args);
                    const WObj* oc = condition_shape(s4, s4.triggers[1].conditions[1].args);
                    CHECK(conv == 2 && ob && oc && s4.triggers[1].conditions[0].args.size() == 1, "old box/circle conditions converted (%d)", conv);
                    if (ob) {
                        Vec3 p;
                        world::get_pos(*ob, p);
                        float g = s4.terrain.height_at(W * 0.3f, H * 0.3f);
                        CHECK(std::fabs(p.y - (g + 8 + 5)) < 0.6f && ob->f32("VolumeHeight") == 10.f, "converted box keeps its height (centre %.1f, ground %.1f)", p.y, g);
                    }
                    CHECK(oc && oc->f32("EncounterRadius") == 55.f && oc->i32("InRadiusCheck") == 1, "converted circle keeps its radius");
                    // "outside" is 2 (the game tests 1 / 2); an old file's 0 opens as 2.
                    {
                        TriggerShape so;
                        so.box = false;
                        so.x = W * 0.4f; so.z = H * 0.4f; so.radius = 30; so.inside = false; so.name = "TB - outside";
                        uint32_t ov = add_trigger_shape(s4, so);
                        const WObj* oo = object_by_uid(s4.world, ov);
                        CHECK(oo && oo->i32("InRadiusCheck") == 2, "E1 an outside circle is written as 2 (%d)", oo ? oo->i32("InRadiusCheck") : -1);
                        if (WObj* ow = ov ? object_by_uid(s4.world, ov) : nullptr) ow->set_i32("InRadiusCheck", 0);
                        CHECK(fix_outside_circles(s4.world) == 1 && object_by_uid(s4.world, ov)->i32("InRadiusCheck") == 2, "E1 InRadiusCheck 0 from an old file becomes 2");
                        // The objectives tab's win trigger with no inputs is switched off (it would win at load), with an input on.
                        uint32_t wt = add_trigger_object(s4, "MATriggerLogic", kWinAllTrigger);
                        CHECK(wt && object_by_uid(s4.world, wt)->i32("TriggerEnabled") == 1 && win_all_idle(s4.world), "G5 a new win trigger starts on with no inputs");
                        CHECK(fix_win_all(s4.world) == 1 && object_by_uid(s4.world, wt)->i32("TriggerEnabled") == 0, "G5 no inputs: switched off");
                        add_reference(s4, wt, vol);
                        CHECK(!win_all_idle(s4.world) && fix_win_all(s4.world) == 1 && object_by_uid(s4.world, wt)->i32("TriggerEnabled") == 1, "G5 with an input: switched on");
                        wedit::remove_subtree(s4.world, wt);
                        wedit::remove_subtree(s4.world, ov);  // the later checks count the map's circles
                    }
                    // Property layout (keys and wire types, in order) of a retail volume / radius.
                    auto layout = [](const WObj& o) {
                        std::vector<std::pair<uint32_t, int>> v;
                        for (auto& p : o.props)
                            if (p.key != key_of("UID") && p.key != key_of("parent") && p.key != key_of("child") && p.key != key_of("sibling"))
                                v.push_back({p.key, int(p.type)});
                        return v;
                    };
                    std::set<std::vector<std::pair<uint32_t, int>>> retail_vol, retail_rad;
                    for (auto& m : gd.maps()) {
                        if (m.multiplayer) continue;
                        Wdf rw;
                        if (!gd.load_world(m, rw)) continue;
                        for (auto& o : rw.objs) {
                            if (o.cls == key_of("MATriggerCuboidalVolume")) retail_vol.insert(layout(o));
                            if (o.cls == key_of("MATriggerRadius")) retail_rad.insert(layout(o));
                        }
                    }
                    uint32_t oc_uid = oc ? oc->uid() : 0;  // compiling grows the object list: no pointers across it
                    convert_legacy_triggers(s4);
                    fs::path out = dir / L"Mission_TriggerBoxes.ma2map";
                    report("trigger boxes", s4, out);
                    MgfReader mr;
                    Bytes wb;
                    Wdf cw;
                    if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                        WTree t;
                        t.build(cw);
                        int vi = t.find(vol);
                        CHECK(vi >= 0 && cw.objs[size_t(vi)].cls == key_of("MATriggerCuboidalVolume"), "saved: the gate box is a MATriggerCuboidalVolume");
                        if (vi >= 0) {
                            const WObj& v = cw.objs[size_t(vi)];
                            CHECK(retail_vol.count(layout(v)) == 1, "saved box has a retail volume's exact property layout (%zu retail layouts)", retail_vol.size());
                            Vec3 p;
                            world::get_pos(v, p);
                            float g = s4.terrain.height_at(W * 0.5f, H * 0.5f);
                            CHECK(std::fabs(p.y - (g + 5 + 10)) < 0.6f && v.f32("VolumeWidth") == 30.f && v.f32("VolumeLength") == 12.f &&
                                      v.f32("VolumeHeading") == 30.f && v.i32("TriggerEnabled") == 1 && v.i32("PlayerEntitesOnly") == 1,
                                  "saved box fields");
                            int msgs = 0;
                            for (int k : t.kids[size_t(vi)]) msgs += cw.objs[size_t(k)].cls == key_of("MAActionBroadcast");
                            CHECK(msgs == 1, "the message action hangs under the box itself (%d)", msgs);
                            int p_ = t.parent_of[size_t(vi)];
                            CHECK(p_ >= 0 && cw.objs[size_t(p_)].description() == kTriggerShapeGroup, "the box stays in the visible Trigger Volumes group");
                        }
                        int vols = 0, rads = 0;
                        for (auto& o : cw.objs) { vols += o.cls == key_of("MATriggerCuboidalVolume"); rads += o.cls == key_of("MATriggerRadius"); }
                        CHECK(vols == 2 && rads == 1, "no extra compiled copies (%d boxes, %d circles)", vols, rads);
                        if (oc_uid) {
                            int ci = t.find(oc_uid);
                            CHECK(ci >= 0 && retail_rad.count(layout(cw.objs[size_t(ci)])) == 1, "saved circle has a retail radius trigger's exact property layout");
                        }
                    }
                    // Reload and save again: the shapes stay, the actions do not pile up.
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        CHECK(back.triggers.empty() && object_by_uid(back.world, vol) != nullptr, "reloaded gate box is still on the map");
                        Bytes b2;
                        SaveReport r2;
                        io.build(back, b2, &r2, &err);
                        fs::path out2 = dir / L"Mission_TriggerBoxes2.ma2map";
                        write_file(out2, b2.data(), b2.size());
                        MgfReader m2;
                        Wdf w2;
                        if (m2.open(out2) && m2.read_file("world.wdf", wb) && w2.parse(wb)) {
                            int vols = 0, msgs = 0;
                            for (auto& o : w2.objs) { vols += o.cls == key_of("MATriggerCuboidalVolume"); msgs += o.cls == key_of("MAActionBroadcast"); }
                            CHECK(vols == 2 && msgs == 2, "re-saved: %d boxes, %d messages (2 and 2)", vols, msgs);
                        }
                    }
                }
            }
            // 3d. Native trigger editing: triggers, actions and orders are made and changed as
            //     world objects; nothing is rebuilt at save.
            {
                Scenario sn;
                ok = io.create(mp, sn, &err);
                CHECK(ok, "create native-trigger mission: %s", err.c_str());
                if (ok) {
                    uint32_t unit = 0;
                    for (auto& o : sn.world.objs) if (o.cls == key_of("MALevelStart")) { unit = o.uid(); break; }
                    uint32_t trig = add_trigger_object(sn, "MATriggerTimer", "Start");
                    uint32_t a1 = add_action_object(sn, trig, "MAActionBroadcast", "Message");
                    uint32_t a2 = add_action_object(sn, trig, "MAActionGameCondition", "Win");
                    uint32_t a3 = add_action_object(sn, trig, "MAActionShow", "Show");
                    CHECK(trig && a1 && a2 && a3, "native trigger with 3 actions");
                    const WObj* m = object_by_uid(sn.world, a1);
                    std::string* tx = m ? own_text(sn, m->u32("BodyTextID")) : nullptr;
                    CHECK(tx != nullptr, "message action has its own text");
                    if (tx) *tx = "Hello pilot";
                    CHECK(wedit::move_sibling(sn.world, a3, -1), "move an action up");
                    {
                        WTree t;
                        t.build(sn.world);
                        int ti = t.find(trig);
                        std::vector<uint32_t> kids;
                        for (int k : t.kids[size_t(ti)]) kids.push_back(sn.world.objs[size_t(k)].uid());
                        CHECK(kids == std::vector<uint32_t>({a1, a3, a2}), "actions reordered");
                        CHECK(!wedit::move_sibling(sn.world, a1, -1), "first action cannot move up");
                    }
                    uint32_t ev = add_order(sn, trig, unit, "MAPatrol");
                    for (int i = 0; i < 3; i++) order_add_point(sn, ev, 100.f + 20 * i, 200.f);
                    CHECK(ev && order_unit(sn.world, ev) == unit && order_path(sn.world, ev) && ai_path_length(sn, order_path(sn.world, ev)) == 3, "patrol order with a 3-point path");
                    CHECK(set_order_kind(sn, ev, "MADefend") && gd.names().label(object_by_uid(sn.world, order_behaviour(sn.world, ev))->cls) == "MADefend", "order changed to defend");
                    uint32_t mk = set_marker_link(sn, order_behaviour(sn.world, ev), "DefendUID", 300.f, 300.f, true, "Defend point");
                    CHECK(mk && object_by_uid(sn.world, order_behaviour(sn.world, ev))->u32("DefendUID") == mk, "defend marker created");
                    CHECK(orders_of_trigger(sn.world, trig).size() == 1, "one order starts from the trigger");
                    uint32_t logic = add_trigger_object(sn, "MATriggerLogic", "Both");
                    add_reference(sn, logic, trig);
                    CHECK(reference_children(sn.world, logic).size() == 1, "logic input");
                    fs::path out = dir / L"Mission_NativeTriggers.ma2map";
                    report("native triggers", sn, out);
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        CHECK(back.triggers.empty() && object_by_uid(back.world, trig) && orders_of_trigger(back.world, trig).size() == 1, "native triggers reload unchanged");
                        const WObj* bm = object_by_uid(back.world, a1);
                        std::string* bt = bm ? own_text(back, bm->u32("BodyTextID")) : nullptr;
                        CHECK(bt && *bt == "Hello pilot", "message text reloads");
                    }
                    int warnings = 0;
                    for (auto& v : io.validate(sn)) warnings += v.level >= 2;
                    CHECK(warnings == 0, "native trigger mission validates without errors (%d)", warnings);
                }
            }
            // 3c. Add part / Duplicate: every retail class can be added from its prototype
            //     (same property layout as the retail instance it copies), saved and read back; a duplicated
            //     trigger subtree gets fresh UIDs with its internal links remapped.
            {
                Scenario s5;
                ok = io.create(mp, s5, &err);
                CHECK(ok, "create add-part mission: %s", err.c_str());
                if (ok) {
                    auto layout = [](const WObj& o) {
                        std::vector<std::pair<uint32_t, int>> v;
                        for (auto& p : o.props)
                            if (p.key != key_of("UID") && p.key != key_of("parent") && p.key != key_of("child") && p.key != key_of("sibling"))
                                v.push_back({p.key, int(p.type)});
                        return v;
                    };
                    const auto& kinds = io.part_kinds();
                    CHECK(kinds.size() > 80, "part kinds from the retail worlds (%zu)", kinds.size());
                    uint32_t nx = world::next_uid(s5.world);
                    uint32_t grp = world::ensure_group(s5.world, world::root_uid(s5.world), "Added parts", nx);
                    Vec3 at{s5.terrain.size_x() * 0.5f, 0, s5.terrain.size_z() * 0.5f};
                    int added = 0, bad_layout = 0;
                    std::set<uint32_t> uids;
                    for (auto& k : kinds) {
                        uint32_t u = io.add_part(s5, k.cls, grp, &at);
                        if (!u) continue;
                        added++;
                        CHECK(uids.insert(u).second, "unique uid for %s", k.name.c_str());
                        for (auto& o : s5.world.objs)
                            if (o.uid() == u) {
                                if (layout(o) != layout(k.proto)) { bad_layout++; printf("           layout differs: %s\n", k.name.c_str()); }
                                Vec3 p;
                                if (k.positioned) CHECK(world::get_pos(o, p) && std::fabs(p.x - at.x) < 0.01f, "%s placed at the view centre", k.name.c_str());
                                // The retail links are empty.
                                if (o.cls == key_of("MAActionShow")) CHECK(o.u32("ActionEntity") == 0 && o.u32("AISet") == 0, "MAActionShow links empty");
                            }
                    }
                    CHECK(added == int(kinds.size()) && bad_layout == 0, "every part kind added with its retail layout (%d of %zu, %d differ)", added, kinds.size(), bad_layout);
                    // Duplicate a trigger with its actions.
                    uint32_t trig = 0;
                    for (auto& o : s5.world.objs) if (o.cls == key_of("MATriggerTimer")) { trig = o.uid(); break; }
                    uint32_t act = io.add_part(s5, key_of("MAActionBroadcast"), trig, &at);
                    uint32_t act2 = io.add_part(s5, key_of("MAActionShow"), trig, &at);
                    CHECK(act && act2, "add actions under a timer");
                    size_t before = s5.world.objs.size();
                    uint32_t copy = io.duplicate_subtree(s5, trig, 0);
                    CHECK(copy && copy != trig && s5.world.objs.size() == before + 3, "duplicate a timer with its two actions (%zu -> %zu)", before, s5.world.objs.size());
                    WTree t5;
                    t5.build(s5.world);
                    int ci = t5.find(copy);
                    CHECK(ci >= 0 && t5.kids[size_t(ci)].size() == 2 && t5.parent_of[size_t(ci)] == t5.parent_of[size_t(t5.find(trig))], "the copy has its two actions and the original's parent");
                    std::set<uint32_t> all;
                    bool dup_uid = false;
                    for (auto& o : s5.world.objs) dup_uid |= !all.insert(o.uid()).second;
                    CHECK(!dup_uid, "no duplicate uid after duplicate");
                    fs::path out = dir / L"Mission_AddParts.ma2map";
                    report("add parts", s5, out);
                    MgfReader mr;
                    Bytes wb;
                    Wdf cw;
                    if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                        WTree t6;
                        t6.build(cw);
                        int reach = 0;
                        std::vector<int> sub;
                        if (t6.root >= 0) { t6.subtree(t6.root, sub); reach = int(sub.size()); }
                        CHECK(reach == int(cw.objs.size()), "saved world is one tree (%d of %zu objects reachable)", reach, cw.objs.size());
                        CHECK(t6.find(copy) >= 0, "the duplicate was saved");
                    }
                }
            }
            // 3d. Cutscenes: a real MACinema tree, shots add on, links stay inside.
            {
                Scenario s6;
                ok = io.create(mp, s6, &err);
                CHECK(ok, "create cutscene mission: %s", err.c_str());
                if (ok) {
                    Vec3 at{s6.terrain.size_x() * 0.4f, 0, s6.terrain.size_z() * 0.6f};
                    uint32_t cin = io.add_cutscene(s6, at, "Test cutscene");
                    CHECK(cin != 0, "add a cutscene");
                    auto count = [&](const char* cls) { int n = 0; for (auto& o : s6.world.objs) n += o.cls == key_of(cls); return n; };
                    if (cin) {
                        WTree t6;
                        t6.build(s6.world);
                        std::vector<int> sub;
                        t6.subtree(t6.find(cin), sub);
                        std::set<uint32_t> in;
                        for (int i : sub) in.insert(s6.world.objs[size_t(i)].uid());
                        int ctrl = 0, cam = 0, paths = 0, props = 0, actions = 0, dangling = 0;
                        for (int i : sub) {
                            const WObj& o = s6.world.objs[size_t(i)];
                            ctrl += o.cls == key_of("MAFreeCameraController");
                            cam += o.cls == key_of("MAFreeCamera");
                            paths += o.cls == key_of("MAPath");
                            props += o.cls == key_of("MACameraProperty");
                            actions += gd.names().label(o.cls).compare(0, 8, "MAAction") == 0;
                            for (const char* k : {"Camera", "triggerInit", "triggerExit", "eyeUID", "focusUID", "m_wptGroupUID", "WaypointID"})
                                if (o.find(k) && o.u32(k) && !in.count(o.u32(k))) { dangling++; printf("           dangling %s in %s\n", k, o.description().c_str()); }
                        }
                        CHECK(ctrl == 1 && cam == 1 && paths == 1 && props == 5 && actions == 0 && dangling == 0, "cutscene shape (ctrl %d cam %d path %d steps %d actions %d dangling %d)", ctrl, cam, paths, props, actions, dangling);
                        const WObj& co = s6.world.objs[size_t(t6.find(cin))];
                        Vec3 cp;
                        CHECK(co.description() == "Test cutscene" && world::get_pos(co, cp) && std::fabs(cp.x - at.x) < 0.01f, "cutscene named and placed");
                        uint32_t c2 = io.add_cutscene_shot(s6, cin);
                        CHECK(c2 != 0 && count("MAFreeCameraController") == 2 && count("MAPath") == 2 && count("MAFreeCamera") == 2, "second shot added (%d controllers, %d paths)", count("MAFreeCameraController"), count("MAPath"));
                        t6.build(s6.world);
                        int c2i = t6.find(c2);
                        if (c2i >= 0) {
                            const WObj& cc = s6.world.objs[size_t(c2i)];
                            int camera_i = t6.find(cc.u32("Camera"));
                            CHECK(camera_i >= 0 && cc.u32("Camera") != s6.world.objs[size_t(t6.find(cin))].uid(), "shot 2 uses its own camera");
                            std::vector<int> ks;
                            t6.subtree(c2i, ks);
                            bool own_path = false;
                            for (int i : ks) {
                                const WObj& pr = s6.world.objs[size_t(i)];
                                if (pr.cls == key_of("MACameraProperty") && pr.u32("eyeUID")) {
                                    int pi = t6.find(pr.u32("eyeUID"));
                                    own_path = pi >= 0 && t6.find(s6.world.objs[size_t(pi)].u32("m_wptGroupUID")) >= 0;
                                    // the first shot's path is another object
                                    for (int j : sub) own_path = own_path && s6.world.objs[size_t(j)].uid() != pr.u32("eyeUID") || own_path;
                                }
                            }
                            CHECK(own_path, "shot 2's eye path and waypoint group exist");
                        }
                        std::set<uint32_t> all;
                        bool dup = false;
                        for (auto& o : s6.world.objs) dup |= !all.insert(o.uid()).second;
                        CHECK(!dup, "no duplicate uid after cutscene + shot");
                        {
                            size_t before = s6.world.objs.size();
                            int gone = io.remove_cutscene_shot(s6, c2);
                            CHECK(gone > 5 && count("MAFreeCameraController") == 1 && count("MAPath") == 1 && count("MAFreeCamera") == 1, "second shot deleted (%d objects, %d controllers)", gone, count("MAFreeCameraController"));
                            std::set<uint32_t> ids;
                            bool dangling = false;
                            for (auto& o : s6.world.objs) ids.insert(o.uid());
                            for (auto& o : s6.world.objs)
                                for (const char* k : {"Camera", "triggerInit", "triggerExit", "eyeUID", "focusUID"}) dangling |= o.find(k) && o.u32(k) && !ids.count(o.u32(k));
                            CHECK(!dangling && s6.world.objs.size() == before - size_t(gone), "no dangling shot links after deleting a shot");
                            uint32_t c3 = io.add_cutscene_shot(s6, cin);
                            CHECK(c3 != 0 && count("MAFreeCameraController") == 2, "shot can be added again");
                        }
                        EdTrigger tt;
                        tt.name = "Play it";
                        tt.conditions.push_back({"timer", {{"seconds", "3"}}});
                        tt.effects.push_back({"cinema", {{"cinema", std::to_string(cin)}}});
                        s6.triggers = {tt};
                        convert_legacy_triggers(s6);
                        fs::path out = dir / L"Mission_Cutscene.ma2map";
                        report("cutscene", s6, out);
                        MgfReader mr;
                        Bytes wb;
                        Wdf cw;
                        if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                            WTree t7;
                            t7.build(cw);
                            std::vector<int> all7;
                            t7.subtree(t7.root, all7);
                            CHECK(all7.size() == cw.objs.size(), "saved cutscene world is one tree (%zu of %zu)", all7.size(), cw.objs.size());
                            int acts = 0;
                            for (auto& o : cw.objs) acts += o.cls == key_of("MAActionActivateCinema") && o.u32("cinemaID") == cin;
                            CHECK(acts == 1, "the trigger plays the new cutscene (%d)", acts);
                        }
                    }
                }
            }
            // Mode kits: a CTF kit from Bogged Down makes a fresh map pass CTF validation.
            {
                NewMapParams kp;
                kp.w = kp.h = 129;
                kp.types = {"CTF"};
                Scenario k;
                if (io.create(kp, k, &err)) {
                    io.add_spawn_ring(k, 1, {200, 0, 200}, 25, 6);
                    io.add_spawn_ring(k, 2, {800, 0, 800}, 25, 6);
                    bool before = false, after = false;
                    for (auto& v : io.validate(k)) before |= v.level == 2;
                    int n = io.add_mode_kit(k, "CTF", "BoggedDown", &err);
                    io.sync_spawn_groups(k);
                    for (auto& v : io.validate(k)) { after |= v.level == 2; if (v.level == 2) printf("           kit validate: %s\n", v.text.c_str()); }
                    CHECK(before && !after && n > 0, "CTF kit fixes validation (%d objects added)", n);
                    printf("[scenario] CTF kit from BoggedDown: %d objects; validation errors before %d, after %d\n", n, int(before), int(after));
                }
            }
            // Snatch It kit (bots batch 3): the data cores keep the retail pile's spacing on a smaller map (scaled to 5 m apart
            // the core models made one solid block 'Mechs could not reach).
            {
                NewMapParams sp;
                sp.multiplayer = true;
                sp.w = sp.h = 129;
                sp.types = {"SnatchIt"};
                Scenario k;
                auto smaps = io.maps_with_mode("SnatchIt");
                if (!smaps.empty() && io.create(sp, k, &err)) {
                    io.add_spawn_ring(k, 1, {200, 0, 200}, 25, 6);
                    io.add_spawn_ring(k, 2, {800, 0, 800}, 25, 6);
                    int n = io.add_mode_kit(k, "SnatchIt", smaps.front(), &err);
                    std::vector<Vec3> cores;
                    for (auto& o : k.world.objs) { Vec3 p; if (o.cls == key_of("MASnatchItDataCore") && world::get_pos(o, p)) cores.push_back(p); }
                    float mind = 1e9f;
                    for (size_t a = 0; a < cores.size(); a++)
                        for (size_t b = a + 1; b < cores.size(); b++) mind = std::min(mind, std::hypot(cores[a].x - cores[b].x, cores[a].z - cores[b].z));
                    CHECK(n > 0 && cores.size() >= 2 && mind >= 9.f, "Snatch It kit from %s: %zu cores, nearest two %.1f m apart (>= 9)", smaps.front().c_str(), cores.size(), mind);
                }
            }
            // Base War kit: the saved map's objective slots 0/1 name the kit's power plants (the clone renumbers
            // them; the retail UIDs left in MARoot ended the match at once), and the start-up sets list their members again.
            {
                NewMapParams bp;
                bp.multiplayer = true;
                bp.w = bp.h = 129;
                bp.types = {"BaseWar"};
                bp.base = "boggeddown.mgf";
                Scenario k;
                if (io.create(bp, k, &err)) {
                    io.add_spawn_ring(k, 1, {200, 0, 200}, 25, 6);
                    io.add_spawn_ring(k, 2, {800, 0, 800}, 25, 6);
                    int n = io.add_mode_kit(k, "BaseWar", "BoggedDown", &err);
                    io.sync_spawn_groups(k);
                    fs::path bf = fs::temp_directory_path() / L"ma2scn_basewar_test.ma2map";
                    SaveReport brep;
                    Scenario kb;
                    // A base level without Base War slot types (a from-scratch map): the save writes 9 / 10.
                    if (int zi = world::find_class(k.world, world::K_ROOT); zi >= 0) {
                        k.world.objs[size_t(zi)].set_i32("ObjectiveType0", 0);
                        k.world.objs[size_t(zi)].set_i32("ObjectiveType1", 0);
                    }
                    bool bok = n > 0 && io.save(k, bf, &brep, &err) && io.load(bf, kb, &err);
                    CHECK(bok, "Base War kit map saves and reloads (%d objects): %s", n, err.c_str());
                    if (bok) {
                        int ri = world::find_class(kb.world, world::K_ROOT);
                        for (int slot = 0; slot < 2 && ri >= 0; slot++) {
                            const WObj& r = kb.world.objs[size_t(ri)];
                            uint32_t t = r.u32("ob" + std::to_string(slot) + "obj0");
                            auto want = objective_targets(kb.world, slot);
                            const WObj* b = t ? object_by_uid(kb.world, t) : nullptr;
                            CHECK(b && want.size() == 1 && want[0] == t && r.i32("NumObjectiveObjects" + std::to_string(slot)) == 1,
                                  "Base War slot %d names the kit's power plant (ob%dobj0 %u, %s)", slot, slot, t, b ? b->description().c_str() : "missing");
                        }
                        if (ri >= 0)
                            CHECK(kb.world.objs[size_t(ri)].i32("ObjectiveType0") == 9 && kb.world.objs[size_t(ri)].i32("ObjectiveType1") == 10,
                                  "E1 Base War save writes slot types 9 / 10 (%d / %d)", kb.world.objs[size_t(ri)].i32("ObjectiveType0"), kb.world.objs[size_t(ri)].i32("ObjectiveType1"));
                        int filled = 0, sets = 0;
                        for (auto& o : kb.world.objs)
                            if (o.cls == key_of("MoeEntitySet")) { sets++; filled += o.i32("MoeEntitySetCount") > 0; }
                        CHECK(filled > 0, "Base War kit: the start-up entity sets list members again (%d of %d)", filled, sets);
                        printf("[scenario] Base War kit from BoggedDown: %d objects; %d of %d entity sets with members\n", n, filled, sets);
                    }
                    // Check per team: the kit puts a helipad by each team's VTOLs; with the repair areas gone,
                    // each team gets a 'no helipad' warning.
                    auto heli_warns = [&](const Scenario& sc) {
                        int c = 0;
                        for (auto& it : io.validate(sc)) c += it.text.find("no helipad") != std::string::npos;
                        return c;
                    };
                    int with = heli_warns(k);
                    for (auto& o : k.world.objs) if (o.cls == key_of("MACollectionArea")) o.cls = key_of("MAGroup");
                    int without = heli_warns(k);
                    CHECK(with == 0 && without == 2, "Check: a team with VTOLs and no helipad near its spawns is warned (kit %d, no pads %d)", with, without);
                }
            }
            // Grinder kit: every set link in the copied waves names an entity set (the kit's new sets took
            // the clone's own ids, so the wave 'Mechs' links were renumbered onto the group; DestroySet / RadiusSet,
            // which start the next wave, were never mapped).
            {
                NewMapParams gp;
                gp.multiplayer = true;
                gp.w = gp.h = 129;
                gp.types = {"Grinder"};
                Scenario k;
                if (io.create(gp, k, &err)) {
                    int n = io.add_mode_kit(k, "Grinder", "InYourFace", &err);
                    int links = 0, bad = 0, wave_sets = 0;
                    for (auto& o : k.world.objs)
                        for (const char* f : {"SetMember0", "SetMember1", "ActionSet", "AISet", "DestroySet", "RadiusSet"}) {
                            uint32_t v = o.u32(f);
                            if (!v) continue;
                            const WObj* t = object_by_uid(k.world, v);
                            links++;
                            if (!t || t->cls != key_of("MoeEntitySet")) { bad++; continue; }
                            wave_sets += std::string(f) == "DestroySet";
                        }
                    CHECK(n > 0 && links > 30 && bad == 0 && wave_sets == 4, "Grinder kit: set links name entity sets (%d objects, %d links, %d broken, %d wave triggers)",
                          n, links, bad, wave_sets);
                    printf("[scenario] Grinder kit from InYourFace: %d objects; %d set links, %d broken\n", n, links, bad);
                }
            }
            if (!bisect.empty()) {
                // bisect2 (maintainer 2026-10-20): the type map is now always the Base level's own
                // (an all-0x0001 map stopped rounds, run X5). These isolate the remaining 129x129
                // failure on a Bogged Down base and check the fix.
                fs::create_directories(bisect);
                auto centroids = [&](const char* map, Vec3& red, Vec3& blue) {
                    red = blue = {0, 0, 0};
                    int nr = 0, nb = 0;
                    if (const Wdf* bw = io.retail_world(map))
                        for (auto& o : bw->objs) {
                            Vec3 p;
                            if (o.cls != key_of("MALevelStart") || !world::get_pos(o, p)) continue;
                            if (o.i32("TeamIndex") == 1) { red.x += p.x; red.z += p.z; nr++; }
                            if (o.i32("TeamIndex") == 2) { blue.x += p.x; blue.z += p.z; nb++; }
                        }
                    if (nr) { red.x /= nr; red.z /= nr; }
                    if (nb) { blue.x /= nb; blue.z /= nb; }
                };
                struct Variant { const char* file; const char* what; const char* map; int size; bool gen; int content; bool drop_culture; };
                // content: 0 = the Base map's own TD kit (its MTGs with TeamDestruction=1), 1 = editor spawns
                //          (6 Red + 6 Blue in MTG "Team Spawns"), 2 = editor spawns + 4 vehicle pads + 4 snow trees
                const Variant vs[] = {
                    {"G1_GoCommando129_GenHeights_EditorSpawns", "GoCommando base, generated 129 heights, GoCommando's own type map, editor spawns", "GoCommando", 129, true, 1, false},
                    {"G2_GoCommando129_OwnHeights_EditorSpawns", "GoCommando base, GoCommando's own heights and textures, editor spawns at its retail spawn areas", "GoCommando", 129, false, 1, false},
                    {"G3_GoCommando129_OwnHeights_RetailTDKit", "GoCommando base, own heights and textures, GoCommando's TD kit", "GoCommando", 129, false, 0, false},
                    {"G4_GoCommando129_GenHeights_RetailTDKit", "GoCommando base, generated 129 heights, GoCommando's TD kit", "GoCommando", 129, true, 0, false},
                    {"G5_GoCommando129_GenHeights_Spawns_Pads_SnowTrees", "the Small_TeamDestruction sample rebuilt on GoCommando: generated heights, editor spawns, 4 pads, 4 snow trees ([Assets])", "GoCommando", 129, true, 2, false},
                    {"B9_BD257_GenHeights_BDTypeMap_RetailTDKit", "B3 with the fix: Bogged Down base, generated 257 heights, Bogged Down's own type map, BD TD kit", "BoggedDown", 257, true, 0, false},
                    {"B10_BD129_GenHeights_BDTypeMap_EditorSpawns_NoCulture", "B6 without the culture system (MACultureSystem removed): Bogged Down base, generated 129 heights, BD type map, editor spawns", "BoggedDown", 129, true, 1, true},
                };
                for (auto& v : vs) {
                    Scenario sc;
                    NewMapParams bp;
                    bp.multiplayer = true;
                    bp.types = {"TeamDestruction"};
                    bp.name = v.file;
                    bp.base = lower(std::string(v.map)) + ".mgf";
                    bp.start = v.gen ? NewMapParams::Noise : NewMapParams::Flat;
                    bp.keep_base_terrain = !v.gen;
                    bp.drop_culture = v.drop_culture;
                    bp.w = bp.h = v.size;
                    bp.seed = 20261020;
                    bool good = io.create(bp, sc, &err);
                    CHECK(good, "bisect %s: %s", v.file, err.c_str());
                    if (!good) continue;
                    float W = sc.terrain.size_x(), H = sc.terrain.size_z();
                    if (v.content == 0) {
                        int n = io.add_mode_kit(sc, "TeamDestruction", v.map, &err);
                        CHECK(n > 0, "kit %s: %s", v.file, err.c_str());
                    } else if (!v.gen) {
                        Vec3 red, blue;
                        centroids(v.map, red, blue);
                        io.add_spawn_ring(sc, 1, red, 25, 6);
                        io.add_spawn_ring(sc, 2, blue, 25, 6);
                    } else {
                        io.add_spawn_ring(sc, 1, {W * 0.22f, 0, H * 0.22f}, 28.f, 6);
                        io.add_spawn_ring(sc, 2, {W * 0.78f, 0, H * 0.78f}, 28.f, 6);
                    }
                    if (v.content == 2) {
                        float off = 0;
                        std::set<std::string> pads_used;
                        for (auto& pk : io.pad_kinds())
                            if ((pk.label == "Atlas" || pk.label == "Catapult") && pads_used.insert(pk.label).second) {
                                io.add_pad(sc, pk, {W * 0.30f + off, 0, H * 0.15f}, 0.f);
                                io.add_pad(sc, pk, {W * 0.70f - off, 0, H * 0.85f}, 3.14159f);
                                off += 25.f;
                            }
                        for (int k = 0; k < 4; k++) {
                            float a = k * 1.5708f;
                            io.place_template(sc, 123384, {W * 0.5f + std::cos(a) * 30, 0, H * 0.5f + std::sin(a) * 30}, a);
                        }
                    }
                    io.sync_spawn_groups(sc);
                    sc.description = std::string("Bisect2: ") + v.what + ".";
                    sc.author = "Scenario Editor bisect";
                    SaveReport rep;
                    fs::path out = bisect / (widen(v.file) + L".ma2map");
                    bool saved = io.save(sc, out, &rep, &err);
                    CHECK(saved, "save %s: %s", v.file, err.c_str());
                    int errors = 0;
                    for (auto& it : io.validate(sc)) if (it.level == 2) errors++;
                    printf("[bisect] %-52s %4zu objects %4zu templates %3zu assets %4d nav %d validation errors\n", v.file, rep.objects,
                           rep.templates, rep.asset_files, rep.nav_nodes, errors);
                }
            }
            if (!mvariant.empty()) {
                // M3 (maintainer runs bi_M1/bi_M2): generated terrain with the height-fog band moved
                // under the lowest ground.
                fs::create_directories(mvariant);
                NewMapParams mm;
                mm.multiplayer = false;
                mm.w = mm.h = 129;
                mm.base = "mission_05.mgf";
                mm.start = NewMapParams::Noise;
                mm.flat_height = 15.f;
                mm.seed = 5;
                mm.name = "M3_Mission05_GenTerrain_FogUnderGround";
                Scenario sc;
                if (io.create(mm, sc, &err)) {
                    fill_mission(sc);
                    sc.name = mm.name;
                    sc.description = "Bisect2: mission sample on generated terrain; height-fog band moved 2 m under the lowest ground, colour-map alpha = Base mean.";
                    int ei = world::find_class(sc.world, world::K_ENV);
                    float lowest = sc.terrain.lowest();
                    if (ei >= 0) {
                        const WObj& env = sc.world.objs[size_t(ei)];
                        printf("[bisect] M3 fog band %.2f..%.2f m, lowest ground %.2f m, FogStart/End %.0f/%.0f\n", env.f32("FogBottom"),
                               env.f32("FogTop"), lowest, env.f32("FogStart"), env.f32("FogEnd"));
                        CHECK(env.f32("FogTop") < lowest, "fog band under the ground");
                    }
                    SaveReport rep;
                    bool saved = io.save(sc, mvariant / L"M3_Mission05_GenTerrain_FogUnderGround.ma2map", &rep, &err);
                    CHECK(saved, "save M3: %s", err.c_str());
                }
            }
            if (all_maps) {
                // Every retail map: copy, build the .ma2map in memory, reload it.
                int okc = 0, total = 0;
                double tt = now_s();
                for (auto& m : gd.maps()) {
                    if (!m.resource_present) continue;
                    if (const char* only = getenv("FMT_MAP")) if (m.name != only) continue;
                    total++;
                    NewMapParams cp2;
                    cp2.start = NewMapParams::CopyRetail;
                    cp2.copy_map = m.name;
                    Scenario sc;
                    Bytes arc;
                    SaveReport rep;
                    bool good = io.create(cp2, sc, &err) && io.build(sc, arc, &rep, &err);
                    fs::path tmpf = fs::temp_directory_path() / L"ma2scn_allmaps.ma2map";
                    Scenario back;
                    good = good && write_file(tmpf, arc) && io.load(tmpf, back, &err) && back.world.objs.size() >= sc.world.objs.size();
                    fs::remove(tmpf);
                    CHECK(good, "copy+save+load %s: %s", m.name.c_str(), err.c_str());
                    // Retail copies keep their own type map, and every retail spawn, pad and objective stands on 0/8.
                    int ground_errors = 0;
                    if (good)
                        for (auto& it : io.validate(sc))
                            if (it.level == 2 && it.text.find("stands where players cannot go") != std::string::npos) ground_errors++;
                    CHECK(ground_errors == 0, "retail copy %s: %d spawns/pads/objectives off the playable area", m.name.c_str(), ground_errors);
                    CHECK(!good || rep.asset_files == 0, "retail copy %s needs no [Assets] (%zu files)", m.name.c_str(), rep.asset_files);
                    // Single player reads StartVehiclesGeneric: 11a and 13b start on foot (the other 17 slots say Mad Dog / Rommel).
                    if (m.name == "Mission_11a" || m.name == "Mission_13b") CHECK(!good || io.start_container(sc) == 123999, "%s: the player starts on foot (container %u)", m.name.c_str(), io.start_container(sc));
                    // A new objective on a retail copy gets a mission variable nothing else sets (Mission_01b: slot 16's
                    // index was set by 'TT - delay post barge dest', so the new objective finished at once).
                    if (good && !sc.multiplayer && sc.objectives.size() < 30) {
                        const int slot = int(sc.objectives.size());
                        sc.objectives.push_back("New objective");
                        const int v = claim_objective_var(sc, slot);
                        int clash = 0;
                        for (auto& o : sc.world.objs)
                            if (o.cls == key_of("MAActionMissionVariableSet") && o.i32("MissionVariableSet", -1) == v) clash++;
                        for (int j = 0; j < slot; j++) clash += objective_var(sc, j) == v;
                        CHECK(clash == 0 && v >= 0 && v < 30, "retail copy %s: new objective %d got variable %d, already used %d times", m.name.c_str(), slot + 1, v, clash);
                    }
                    if (good) okc++;
                    // Loss audit: every object of the retail world must come back from the saved file with the same
                    // class and the same properties (key, type, bytes). Differences are tallied by class.property.
                    if (good)
                        if (const Wdf* rw = io.retail_world(m.name)) {
                            std::unordered_map<uint32_t, const WObj*> got;
                            for (auto& o : back.world.objs) got[o.uid()] = &o;
                            int miss = 0, cls_diff = 0, prop_miss = 0, prop_diff = 0, prop_extra = 0;
                            std::map<std::string, int> tally;
                            for (auto& o : rw->objs) {
                                auto it = got.find(o.uid());
                                if (it == got.end()) { miss++; tally["missing:" + gd.names().label(o.cls)]++; continue; }
                                const WObj& b2 = *it->second;
                                if (b2.cls != o.cls) { cls_diff++; continue; }
                                for (auto& pr : o.props) {
                                    const WProp* q = b2.find(pr.key);
                                    if (!q) { prop_miss++; tally["lost:" + gd.names().label(o.cls) + "." + gd.names().label(pr.key)]++; }
                                    else if (q->type != pr.type || q->data != pr.data) { prop_diff++; tally["changed:" + gd.names().label(o.cls) + "." + gd.names().label(pr.key)]++; }
                                }
                                for (auto& pr : b2.props) if (!o.find(pr.key)) prop_extra++;
                            }
                            printf("           audit %-16s objects %zu->%zu  missing %d  class %d  props lost %d changed %d extra %d\n", m.name.c_str(),
                                   rw->objs.size(), back.world.objs.size(), miss, cls_diff, prop_miss, prop_diff, prop_extra);
                            std::vector<std::pair<int, std::string>> top;
                            for (auto& [k, n] : tally) top.push_back({n, k});
                            std::sort(top.rbegin(), top.rend());
                            for (size_t k = 0; k < top.size() && k < 6; k++) printf("               %6d %s\n", top[k].first, top[k].second.c_str());
                            CHECK(cls_diff == 0 && miss == 0 && prop_miss == 0, "audit %s: nothing lost in copy+save+load (%d missing objects, %d class changes, %d lost properties; value changes are the model-path / low-detail / terrain fix-ups)", m.name.c_str(), miss, cls_diff, prop_miss);
                        }
                    int edited_nav = 0;
                    if (good) {  // the same map with one terrain sample changed gets a generated graph
                        sc.terrain.ref(1, 1) += 1.f;
                        SaveReport r2;
                        Bytes a2;
                        if (io.build(sc, a2, &r2, &err)) edited_nav = r2.nav_nodes;
                        CHECK(edited_nav > 0 && edited_nav <= 2500, "generated nav size %s: %d", m.name.c_str(), edited_nav);
                    }
                    printf("           %-16s %5zu objects %4zu templates %3zu assets %5d nav (edited %4d)  %.2f MB\n", m.name.c_str(), rep.objects,
                           rep.templates, rep.asset_files, rep.nav_nodes, edited_nav, rep.file_bytes / 1048576.0);
                }
                printf("[scenario] all retail maps copied, saved and reloaded: %d/%d in %.1fs\n", okc, total, now_s() - tt);
            }
            printf("[scenario] %.1fs\n", now_s() - t0);
        }
    }

    // ---------------------------------------------------------------- custom-size water
    {
        const char* kM = "Models/Environment/Water/Water_Sw_1000x1000/Water_Sw_1000x1000.MGMODEL";
        MgfReader* a = gd.archive("mission_07.mgf");
        wcustom::Source src;
        bool have = a && a->read_file(std::string(kM) + "{water_1000x1000 verts}", src.verts) && a->read_file(std::string(kM) + "{water_1000x1000 indices}", src.idx);
        CHECK(have, "water model files read");
        if (have) {
            CHECK(wcustom::plain(src), "retail 1000x1000 water is a plain grid");
            {
                // The Description carries name, size and shape: rename keeps size and shape, an oval reads back as an oval.
                WObj wo;
                wo.set_str("Description", ScenarioIO::water_description("Mere", 140, 90, wcustom::Shape::Oval));
                float cw = 0, cl = 0;
                CHECK(ScenarioIO::water_custom_size(wo, cw, cl) && cw == 140 && cl == 90, "oval water description keeps its size");
                CHECK(ScenarioIO::water_custom_shape(wo) == wcustom::Shape::Oval && ScenarioIO::water_name(wo) == "Mere", "oval water description keeps shape and name");
                wo.set_str("Description", ScenarioIO::water_description("Pond", cw, cl, ScenarioIO::water_custom_shape(wo)));
                CHECK(ScenarioIO::water_custom_shape(wo) == wcustom::Shape::Oval && ScenarioIO::water_name(wo) == "Pond", "renaming a water keeps its oval shape");
            }
            for (auto sz : {std::pair<float, float>{1000, 1000}, {300, 120}, {8, 8}, {75, 640}}) {
                Bytes v, i;
                std::string werr;
                bool ok = wcustom::generate(src, sz.first, sz.second, wcustom::Shape::Rect, v, i, nullptr, &werr);
                CHECK(ok, "water %gx%g generate: %s", sz.first, sz.second, werr.c_str());
                if (!ok) continue;
                float w = 0, l = 0;
                CHECK(wcustom::extent(v, w, l) && std::fabs(w - sz.first) < 0.01f && std::fabs(l - sz.second) < 0.01f, "water %gx%g extent %g x %g", sz.first, sz.second, w, l);
                CHECK(rd<uint32_t>(v.data() + 8) == v.size() && rd<uint32_t>(i.data() + 8) == i.size(), "water %gx%g header sizes", sz.first, sz.second);
                // Decode the strip: every real triangle faces up (same winding as the retail tile).
                auto find_tag = [](const Bytes& b, const char* t, size_t from) { for (size_t k = from; k + 4 <= b.size(); k++) if (!memcmp(b.data() + k, t, 4)) return k; return size_t(-1); };
                size_t vd = find_tag(v, "data", 0x20), vi = find_tag(v, "info", 0x20);
                size_t nn = find_tag(i, "nidx", 0x20), id = nn == size_t(-1) ? nn : find_tag(i, "data", nn);
                bool good = vd != size_t(-1) && id != size_t(-1) && vi != size_t(-1);
                int bad = 0, tris = 0;
                if (good) {
                    uint32_t cnt = rd<uint32_t>(v.data() + vi + 12), n = rd<uint32_t>(i.data() + nn + 8);
                    for (uint32_t t = 0; t + 2 < n; t++) {
                        uint16_t q[3] = {rd<uint16_t>(i.data() + id + 8 + 2 * t), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 1)), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 2))};
                        if (q[0] >= cnt || q[1] >= cnt || q[2] >= cnt) { bad++; continue; }
                        if (q[0] == q[1] || q[1] == q[2] || q[0] == q[2]) continue;
                        float p[3][2];
                        for (int k = 0; k < 3; k++) { p[k][0] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20); p[k][1] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20 + 8); }
                        float y = (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]) - (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]);
                        if (t & 1) y = -y;
                        if (y <= 0) bad++;
                        tris++;
                    }
                }
                CHECK(good && bad == 0 && tris > 0, "water %gx%g strip: %d of %d triangles wrong or out of range", sz.first, sz.second, bad, tris);
            }
            // An oval: a triangle list of rings, up-facing, inside the box, with the box's extent.
            for (auto sz : {std::pair<float, float>{300, 120}, {60, 60}, {1000, 400}}) {
                Bytes v, i;
                bool list = false;
                std::string werr;
                bool ok = wcustom::generate(src, sz.first, sz.second, wcustom::Shape::Oval, v, i, &list, &werr);
                CHECK(ok && list, "oval %gx%g generate: %s", sz.first, sz.second, werr.c_str());
                if (!ok) continue;
                float w = 0, l = 0;
                CHECK(wcustom::extent(v, w, l) && std::fabs(w - sz.first) < 0.5f && std::fabs(l - sz.second) < 0.5f, "oval %gx%g extent %g x %g", sz.first, sz.second, w, l);
                auto find_tag = [](const Bytes& b, const char* t, size_t from) { for (size_t k = from; k + 4 <= b.size(); k++) if (!memcmp(b.data() + k, t, 4)) return k; return size_t(-1); };
                size_t vd = find_tag(v, "data", 0x20), vi = find_tag(v, "info", 0x20);
                size_t nn = find_tag(i, "nidx", 0x20), id = nn == size_t(-1) ? nn : find_tag(i, "data", nn);
                int bad = 0, tris = 0;
                if (vd != size_t(-1) && vi != size_t(-1) && id != size_t(-1)) {
                    uint32_t cnt = rd<uint32_t>(v.data() + vi + 12), n = rd<uint32_t>(i.data() + nn + 8);
                    bad += n % 3 != 0;
                    for (uint32_t t = 0; t + 2 < n; t += 3) {
                        uint16_t q[3] = {rd<uint16_t>(i.data() + id + 8 + 2 * t), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 1)), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 2))};
                        if (q[0] >= cnt || q[1] >= cnt || q[2] >= cnt) { bad++; continue; }
                        float p[3][2];
                        for (int k = 0; k < 3; k++) { p[k][0] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20); p[k][1] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20 + 8); }
                        float y = (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]) - (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]);
                        if (y <= 0) bad++;
                        tris++;
                    }
                } else bad = 1;
                CHECK(bad == 0 && tris > 0, "oval %gx%g list: %d of %d triangles wrong or out of range", sz.first, sz.second, bad, tris);
            }
        }
    }

    // ---------------------------------------------------------------- particle effects (View > Effects)
    {
        // Every retail .mgparticle parses; a looped sprite emitter simulates finite sprites.
        int files = 0, parsed = 0, looping = 0;
        for (auto& an : gd.level_archives()) {
            MgfReader* a = gd.archive(an);
            if (!a) continue;
            for (auto& f : a->files()) {
                if (!iends_with(f.path, ".mgparticle")) continue;
                Bytes b;
                if (!a->read_file(f.path, b, &err)) continue;
                files++;
                ParticleFx fx;
                if (parse_particle(std::string(b.begin(), b.end()), fx)) {
                    parsed++;
                    if (fx.loops()) {
                        looping++;
                        std::vector<PSpriteOut> out;
                        float o[3] = {0, 0, 0};
                        simulate_particle(fx, o, 12.3f, 7, 1.f, 0, 0, false, out);
                        bool finite = true;
                        for (auto& p : out) finite = finite && std::isfinite(p.pos[0] + p.pos[1] + p.pos[2] + p.w + p.h) && p.sprite;
                        CHECK(finite, "particle %s/%s: non-finite sprite", an.c_str(), f.path.c_str());
                    }
                } else CHECK(false, "particle %s/%s does not parse", an.c_str(), f.path.c_str());
            }
        }
        const char* xml =
            "<mgParticle version=\"3.0\" lod_start=\"100\" lod_range=\"50\"><emitter type=\"mgParticleEmitter\" name=\"e\" cycle_mode=\"MODE_LOOPED\" cycle_length=\"1.0\" emit_worldspace=\"true\">"
            "<source type=\"mgPointSource\"><pitch_start variance=\"0\"><key time=\"0\" value=\"0\"/></pitch_start></source>"
            "<system type=\"mgKineticSystem\" gravity=\"0\" drag=\"0\" offset=\"0, 1, 0\"><rate variance=\"0\"><key time=\"0\" value=\"10\"/></rate>"
            "<lifespan variance=\"0\"><key time=\"0\" value=\"2\"/></lifespan><initial_velocity variance=\"0\"><key time=\"0\" value=\"1\"/></initial_velocity>"
            "<prototype type=\"mgSpriteType\" name=\"s\"><size variance=\"0\"><key time=\"0\" value=\"2\"/></size><color variance=\"0\"><key time=\"0\" value=\"255, 128, 0, 200\"/></color>"
            "<material type=\"BASE\" blend_mode=\"additive\"><base_texture name=\"x.tif\"/></material></prototype></system></emitter></mgParticle>";
        ParticleFx fx;
        CHECK(parse_particle(xml, fx) && fx.loops() && fx.emitters[0].systems[0].sprite.blend == 1, "synthetic particle parse");
        std::vector<PSpriteOut> out;
        float o[3] = {10, 0, 5};
        simulate_particle(fx, o, 5.0f, 1, 1.f, 0, 0, false, out);
        // 10 per second for 2 s: about 20 alive, each rising 1 m/s from y = 1 + origin.
        bool rises = out.size() >= 15 && out.size() <= 21;
        for (auto& p : out) rises = rises && std::fabs(p.pos[0] - 10) < 1e-3f && p.pos[1] >= 1.f - 1e-3f && p.pos[1] <= 3.f + 1e-3f && std::fabs(p.rgba[3] - 200.f / 255.f) < 0.01f;
        CHECK(rises, "synthetic particle simulation (%zu sprites)", out.size());
        std::vector<PSpriteOut> again;
        simulate_particle(fx, o, 5.0f, 1, 1.f, 0, 0, false, again);
        CHECK(again.size() == out.size() && (out.empty() || again[0].pos[1] == out[0].pos[1]), "particle simulation is deterministic");
        std::vector<PSpriteOut> half;
        simulate_particle(fx, o, 5.0f, 1, 0.5f, 0, 0, false, half);
        CHECK(!half.empty() && std::fabs(half[0].rgba[3] - 100.f / 255.f) < 0.01f, "particle LOD fade scales the alpha");
        std::vector<PSpriteOut> gated;
        simulate_particle(fx, o, 5.0f, 1, 1.f, 0.2f, 1.8f, false, gated);
        CHECK(gated.size() < out.size(), "on/off gating thins an intermittent effect (%zu of %zu)", gated.size(), out.size());
        printf("particles: %d files, %d parsed, %d looping\n", files, parsed, looping);
    }

    // W-113: Name / Description limits count characters, not bytes.
    {
        std::string a(119, 'x');
        CHECK(clip_chars(a, kNameMax) == kNameMax && a.size() == size_t(kNameMax), "W-113 a 119-character name is cut to %d", kNameMax);
        std::string u;
        for (int i = 0; i < 70; i++) u += "\xC3\xA9";  // 70 x e-acute: 2 bytes each
        CHECK(clip_chars(u, kNameMax) == kNameMax && u.size() == size_t(kNameMax) * 2, "W-113 multi-byte characters are cut on a character boundary");
        std::string s = "short";
        CHECK(clip_chars(s, kNameMax) == 5 && s == "short", "W-113 a short name is untouched");
        std::string d(300, 'y');
        CHECK(clip_chars(d, kDescMax) == kDescMax && d.size() == size_t(kDescMax), "W-113 the description is cut to %d", kDescMax);
    }

    // Log privacy: user names and profile paths are redacted from logged text.
    {
        SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\Jane Doe\\AppData\\Local");
        SetEnvironmentVariableW(L"APPDATA", L"C:\\Users\\Jane Doe\\AppData\\Roaming");
        SetEnvironmentVariableW(L"USERPROFILE", L"C:\\Users\\Jane Doe");
        auto R = [](const char* s) { return redact_user_paths(std::string_view(s)); };
        CHECK(R("x C:\\Users\\Jane Doe\\AppData\\Local\\MechAssault2\\Logs\\a.txt") == "x %LOCALAPPDATA%\\MechAssault2\\Logs\\a.txt", "redact LOCALAPPDATA");
        CHECK(R("c:\\users\\jane doe\\appdata\\roaming") == "%APPDATA%", "redact APPDATA (case-insensitive, at the end)");
        CHECK(R("\"C:\\Users\\Jane Doe\\Documents\\m.mgf\" open") == "\"%USERPROFILE%\\Documents\\m.mgf\" open", "redact USERPROFILE");
        CHECK(R("D:\\Users\\Bob\\x and C:\\Users\\Public\\y") == "D:\\Users\\<user>\\x and C:\\Users\\Public\\y", "redact other user names, keep Public");
        CHECK(R("C:\\Users\\Jane Doe2\\q") == "C:\\Users\\<user>\\q", "no partial-name match of the profile");
        CHECK(R("no paths here, game\\maps\\a.mgf") == "no paths here, game\\maps\\a.mgf", "plain text untouched");
    }

    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
