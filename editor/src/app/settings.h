// Editor settings (%LOCALAPPDATA%\MechAssault2\ScenarioEditor.ini) and the shared scenario
// folder setting (%LOCALAPPDATA%\MechAssault2\Scenarios.ini, read by the game too; SPEC.md).
#pragma once
#include <string>

#include "../core/util.h"

namespace ed {

// Rebindable shortcuts (View > Preferences > Hotkeys). Each action has two key slots.
enum Act {
    A_PAN_FWD, A_PAN_BACK, A_PAN_LEFT, A_PAN_RIGHT, A_TOP_DOWN, A_RESET_VIEW, A_HELPERS, A_ZOOM_SEL,
    A_CANCEL, A_DELETE, A_TURN_LEFT, A_TURN_RIGHT, A_SNAP,
    A_UNDO, A_REDO, A_COPY, A_CUT, A_PASTE, A_DUPLICATE, A_SELECT_ALL, A_PLACE_AGAIN, A_PICK_TYPE,
    A_NEW, A_OPEN, A_SAVE, A_SAVE_AS, A_TEST, A_HELP, A_PREFS,
    A_BRUSH_SMALLER, A_BRUSH_BIGGER,
    A_COUNT
};
enum KeyMod { KM_CTRL = 1, KM_SHIFT = 2, KM_ALT = 4 };
struct KeyBind {
    int key = 0;   // ImGuiKey, 0 = unset
    int mods = 0;  // KeyMod bits
    bool operator==(const KeyBind& o) const { return key == o.key && mods == o.mods; }
};
const char* mod_name(int m);             // "Shift", "Ctrl", "Alt" for one KeyMod bit
const char* act_label(int a);
const char* act_section(int a);         // "Camera", "Editing", "Files and windows"
std::string key_text(const KeyBind& k);  // "Ctrl+Shift+S", "" when unset
KeyBind parse_key(const std::string& s);

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
    bool helpers_on = true;        // helper layer at start
    bool legend_on = false;
    bool scene_lights = true;      // View > Scene lights (lamps light the scene; slower)
    bool fast_terrain_detail = true;  // fine ground detail switches off by 128 m blocks (the game's rule) instead of fading
    int skip_update_below = 0;     // 'No' to the start-up offer to update old scenarios: not asked again for revisions below this (kSaveRevision)
    bool objects_follow_terrain = true;  // Terrain tab sculpt tools: things standing on the ground (bridges too) move with it
    void reset_general();
    // Testing
    bool quick_test = true;        // Test skips the game's menus and movies and returns to the editor
    void reset_testing();
    // Hotkeys
    KeyBind keys[A_COUNT][2];
    // Hold keys (one KeyMod bit each, all different): placing ignores every snap while hold_free is down; a click with
    // hold_through down reaches the object behind (cycles the stack; places on the ground under a roof); hold_quarter
    // with Turn left / right turns 90 degrees (and steps a drag-aim by 15).
    int hold_free = KM_SHIFT, hold_through = KM_ALT, hold_quarter = KM_CTRL;
    // 'Reverse the brush': the Terrain shaping tools use their pair's other half while it is down (Flatten: a click
    // picks the height). It works only there, where the three above do nothing, so it may share a key with them.
    int hold_reverse = KM_CTRL;
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
