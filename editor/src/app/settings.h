// Editor settings (%LOCALAPPDATA%\MechAssault2\ScenarioEditor.ini) and the shared scenario
// folder setting (%LOCALAPPDATA%\MechAssault2\Scenarios.ini, read by the game too).
#pragma once
#include <string>

#include "../core/util.h"

#include "input_map.h"
#include "snap_prefs.h"

namespace ed {

// What a flat piece (road piece or base) does where the ground slopes under it (app_slope.cpp).
enum class SlopeMode { Level, Tilt, Float };

// Rebindable shortcuts (File > Settings > Keys): the table, Act and KeyBind live in input_map.h. Each action has two key slots.
const char* mod_name(int m);             // "Shift", "Ctrl", "Alt" for one KeyMod bit
const char* act_label(int a);
inline std::string act_text(int a, const char* suffix = "") { return std::string(act_label(a)) + suffix; }  // the one name of a command (+ "..." or "##id")
const char* act_section(int a);         // "Camera", "Editing", "Files and windows"

struct Settings {
    fs::path install;              // empty = automatic (exe folder or its parent)
    fs::path scenario_folder;      // empty = default (Documents\MechAssault 2\Scenarios)
    std::string author;
    std::string recent[8];
    bool first_run = false;        // no settings file yet: show Getting Started

    // Camera
    bool right_drag_pan = true;    // right-drag pans; a right click without moving still cancels
    bool edge_scroll = false;
    float edge_speed = 1.f;
    float pan_speed = 1.f, rotate_speed = 1.f, zoom_speed = 1.f;
    bool invert_rotate = false;    // middle-drag left/right
    bool invert_pitch = false;     // middle-drag up/down
    bool invert_zoom = false;
    bool invert_drag_pan = false;  // right-drag moves the view instead of the ground
    void reset_camera();
    // General
    int autosave_minutes = 2;      // 0 = off
    float ui_scale = 1.f;
    bool ui_scale_auto = true;  // The text and button size follows the window size (quarter steps, 1 to 2) until the user picks one
    float ui_scale_run = 0.f;  // --ui-scale for this run only (never saved)
    bool pal_pictures = true;  // [Palette] View: the object browsers show pictures (true) or a list
    int pal_size = 1;          // [Palette] PicSize: picture size 0 S / 1 M / 2 L (56 / 76 / 104 px at 100%)
    float left_panel = 0.f;    // [Layout] LeftPanel: the left panel width at 100% UI scale (0 = the default, 370)
    int timeline = 0;          // [Layout] Timeline: the cutscene strip under the map, 1 = folded to its header
    int minimap = -1;          // [Layout] Minimap: the overview under Properties, -1 = on unless the window is short, 0 hidden, 1 shown
    bool helpers_on = true;        // helper layer at start
    bool legend_on = false;
    bool lift_dark = true;         // View > Brighten dark maps (editor view only)
    bool scene_lights = true;      // View > Scene lights (lamps light the scene; slower)
    bool fast_terrain_detail = true;  // fine ground detail switches off by 128 m blocks (the game's rule) instead of fading
    int skip_update_below = 0;     // 'No' to the start-up offer to update old scenarios: not asked again for revisions below this (kSaveRevision)
    float raise_amount = 2.f;      // [Brush] RaiseAmount: metres one Raise / Lower / Roughen stroke moves the ground (F4)
    bool build_up = false;         // [Brush] BuildUp: keep raising while the button is held
    float ramp_width = 16.f;       // [Brush] RampWidth: the Ramp tool's width in metres
    // [Brush] Strength*: how much one stroke changes the ground, remembered per tool: Texture, Colour tint, Smooth, Flatten, Plants.
    float tool_strength[5] = {1.f, 0.5f, 0.5f, 0.5f, 0.5f};
    bool show_steep = false;       // [Brush] ShowSteep: steep ground tinted amber / red on the map
    // [Reach] the "Can it get here?" overlay: on, the unit label, team (1 red, 2 blue), driver (0 player, 1 bots), compare all units.
    bool reach_show = false;
    std::string reach_unit = "Stiletto";
    int reach_team = 1, reach_driver = 0;
    bool reach_compare = true;
    // [Scatter] the scatter brush: objects per hectare, spacing as a multiple of the two footprint radii, random facing, the
    // most an object leans (degrees), tilt to the slope, keep off roads / bases / buildings, skip slopes over sc_slope percent.
    float sc_density = 40.f, sc_spacing = 1.f, sc_lean = 0.f, sc_slope = 60.f;
    bool sc_facing = true, sc_follow = false, sc_keep_off = true, sc_skip_steep = false;
    bool objects_follow_terrain = true;  // Terrain tab sculpt tools: things standing on the ground (bridges too) move with it
    SlopeMode slope_plate = SlopeMode::Level;  // what a flat piece does on sloping ground when it is placed, remembered per family
    SlopeMode slope_road = SlopeMode::Tilt;
    void reset_general();
    // Testing
    bool quick_test = true;        // Test skips the game's menus and movies and returns to the editor
    void reset_testing();
    // Hotkeys
    KeyBind keys[A_COUNT][2];
    // Hold keys (one KeyMod bit each, all different): placing ignores every snap while hold_free is down; a click with
    // hold_through down reaches the object behind (cycles the stack; places on top of the object under the cursor); hold_quarter
    // with Turn left / right turns 90 degrees (and steps a drag-aim by 15).
    int hold_free = KM_SHIFT, hold_through = KM_ALT, hold_quarter = KM_CTRL;
    // 'Reverse the brush': the Terrain shaping tools use their pair's other half while it is down (Flatten: a click
    // picks the height). It works only there, where the three above do nothing, so it may share a key with them.
    int hold_reverse = KM_CTRL;
    // Snapping: joints, grid, angle steps and the reach, kept per user in [Snap] (the snap bar in the viewport, Edit menu, Settings > General).
    SnapPrefs snap;
    // 'Turn the view with left-drag': Alt+left-drag orbits like middle-drag. A drag is never a click, so it may share a key.
    int hold_orbit = KM_ALT;
    Hold hold() const { return {hold_free, hold_through, hold_quarter, hold_reverse, hold_orbit}; }
    void reset_keys();

    Settings() { reset_camera(); reset_general(); reset_testing(); reset_keys(); }
    void load();
    void save() const;
    fs::path scenarios() const;    // effective folder

    static fs::path local_dir();   // %LOCALAPPDATA%\MechAssault2
    static fs::path default_scenarios();
    static fs::path scenarios_ini();
    // ScenarioEditor.ini (or the --settings file) and the recovery slot folder beside it.
    static fs::path ini_path();
    static fs::path autosave_dir();
    static void set_ini_override(const fs::path& p);
    // Writes Scenarios.ini ([Scenarios] Folder=...). An empty path removes the setting.
    static bool write_scenario_folder(const fs::path& folder, std::string* err);
    static fs::path read_scenario_folder();
    void add_recent(const fs::path& p);
    void remove_recent(const fs::path& p);
};

}  // namespace ed
