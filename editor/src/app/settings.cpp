#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>

#include "../core/gamedata.h"
#include "imgui.h"

namespace ed {

using namespace ma2;

fs::path Settings::local_dir() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = fs::path(p) / L"MechAssault2";
    CoTaskMemFree(p);
    return out;
}

static fs::path& ini_override();

fs::path Settings::default_scenarios() {
    // A run on its own settings file (automation, --settings) keeps its own scenario folder beside
    // that file, so test saves never land in the user's real Scenarios folder.
    if (!ini_override().empty()) return ini_override().parent_path() / L"Scenarios";
    // FOLDERID_Documents respects OneDrive redirection.
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = fs::path(p) / L"MechAssault 2" / L"Scenarios";
    CoTaskMemFree(p);
    return out;
}

fs::path Settings::scenarios_ini() {
    // The game reads local_dir()\Scenarios.ini; a run on its own settings file must not touch it.
    return ini_override().empty() ? local_dir() / L"Scenarios.ini" : ini_override().parent_path() / L"Scenarios.ini";
}

static fs::path& ini_override() {
    static fs::path p;
    return p;
}
void Settings::set_ini_override(const fs::path& p) { ini_override() = p; }
fs::path Settings::ini_path() { return ini_override().empty() ? local_dir() / L"ScenarioEditor.ini" : ini_override(); }
fs::path Settings::autosave_dir() {
    // Automation and other runs on their own settings file keep their own recovery slot too.
    return ini_override().empty() ? local_dir() / L"Autosave" : ini_override().parent_path() / L"Autosave";
}

fs::path Settings::read_scenario_folder() {
    Bytes b;
    if (!read_file(scenarios_ini(), b)) return {};
    std::string text(b.begin(), b.end());
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.erase(0, 3);
    IniFile ini;
    ini.parse(text, false);
    std::string f = ini.get("Scenarios", "Folder");
    return f.empty() ? fs::path() : fs::path(widen(f));
}

bool Settings::write_scenario_folder(const fs::path& folder, std::string* err) {
    std::string text = "[Scenarios]\r\n";
    if (!folder.empty()) text += "Folder=" + path_utf8(folder) + "\r\n";
    return write_file(scenarios_ini(), text.data(), text.size(), err);
}

fs::path Settings::scenarios() const {
    return scenario_folder.empty() ? default_scenarios() : scenario_folder;
}

// ----------------------------------------------------------------------------- hotkeys

static const struct { const char* label; const char* section; const char* ini; } kActs[A_COUNT] = {
    {"Pan forward", "Camera", "PanForward"},
    {"Pan back", "Camera", "PanBack"},
    {"Pan left", "Camera", "PanLeft"},
    {"Pan right", "Camera", "PanRight"},
    {"Top-down view on / off", "Camera", "TopDown"},
    {"Reset the view", "Camera", "ResetView"},
    {"Show / hide helpers", "Camera", "Helpers"},
    {"Zoom to the selection", "Camera", "ZoomSelection"},
    {"Stop the tool / clear the selection", "Editing", "Cancel"},
    {"Delete the selection", "Editing", "Delete"},
    {"Turn left", "Editing", "TurnLeft"},
    {"Turn right", "Editing", "TurnRight"},
    {"Put on the ground", "Editing", "Snap"},
    {"Undo", "Editing", "Undo"},
    {"Redo", "Editing", "Redo"},
    {"Copy", "Editing", "Copy"},
    {"Cut (copy, then delete)", "Editing", "Cut"},
    {"Paste under the mouse", "Editing", "Paste"},
    {"Duplicate (a copy next to it)", "Editing", "Duplicate"},
    {"Select all", "Editing", "SelectAll"},
    {"Place again (the last thing placed)", "Editing", "PlaceAgain"},
    {"Place more of the selected type", "Editing", "PickType"},
    {"New", "Files and windows", "New"},
    {"Open", "Files and windows", "Open"},
    {"Save", "Files and windows", "Save"},
    {"Save as", "Files and windows", "SaveAs"},
    {"Test in the game", "Files and windows", "Test"},
    {"Help", "Files and windows", "Help"},
    {"Settings", "Files and windows", "Preferences"},
    {"Smaller brush", "Editing", "BrushSmaller"},
    {"Bigger brush", "Editing", "BrushBigger"},
};

const char* mod_name(int m) { return m == KM_CTRL ? "Ctrl" : m == KM_ALT ? "Alt" : "Shift"; }
static int mod_of(const std::string& s, int def) {
    return iequals(s, "Ctrl") ? KM_CTRL : iequals(s, "Alt") ? KM_ALT : iequals(s, "Shift") ? KM_SHIFT : def;
}

const char* act_label(int a) { return a >= 0 && a < A_COUNT ? kActs[a].label : ""; }
const char* act_section(int a) { return a >= 0 && a < A_COUNT ? kActs[a].section : ""; }

std::string key_text(const KeyBind& k) {
    if (!k.key) return {};
    std::string t;
    if (k.mods & KM_CTRL) t += "Ctrl+";
    if (k.mods & KM_SHIFT) t += "Shift+";
    if (k.mods & KM_ALT) t += "Alt+";
    return t + ImGui::GetKeyName(ImGuiKey(k.key));
}

KeyBind parse_key(const std::string& text) {
    KeyBind k;
    std::string s = trim(text);
    for (;;) {
        if (istarts_with(s, "Ctrl+")) { k.mods |= KM_CTRL; s = s.substr(5); }
        else if (istarts_with(s, "Shift+")) { k.mods |= KM_SHIFT; s = s.substr(6); }
        else if (istarts_with(s, "Alt+")) { k.mods |= KM_ALT; s = s.substr(4); }
        else break;
    }
    if (s.empty()) return {};
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
        if (iequals(s, ImGui::GetKeyName(ImGuiKey(key)))) { k.key = key; return k; }
    return {};
}

void Settings::reset_keys() {
    for (auto& a : keys) a[0] = a[1] = KeyBind();
    auto set = [&](int a, ImGuiKey k1, int m1 = 0, ImGuiKey k2 = ImGuiKey_None, int m2 = 0) {
        keys[a][0] = {int(k1), m1};
        keys[a][1] = {int(k2), m2};
    };
    set(A_PAN_FWD, ImGuiKey_W, 0, ImGuiKey_UpArrow);
    set(A_PAN_BACK, ImGuiKey_S, 0, ImGuiKey_DownArrow);
    set(A_PAN_LEFT, ImGuiKey_A, 0, ImGuiKey_LeftArrow);
    set(A_PAN_RIGHT, ImGuiKey_D, 0, ImGuiKey_RightArrow);
    set(A_TOP_DOWN, ImGuiKey_T);
    set(A_RESET_VIEW, ImGuiKey_Home);
    set(A_HELPERS, ImGuiKey_H);
    set(A_ZOOM_SEL, ImGuiKey_F);
    set(A_CANCEL, ImGuiKey_Escape);
    set(A_DELETE, ImGuiKey_Delete);
    set(A_TURN_LEFT, ImGuiKey_Q);
    set(A_TURN_RIGHT, ImGuiKey_E);
    set(A_SNAP, ImGuiKey_G);
    set(A_UNDO, ImGuiKey_Z, KM_CTRL);
    set(A_REDO, ImGuiKey_Y, KM_CTRL, ImGuiKey_Z, KM_CTRL | KM_SHIFT);
    set(A_COPY, ImGuiKey_C, KM_CTRL);
    set(A_CUT, ImGuiKey_X, KM_CTRL);
    set(A_PASTE, ImGuiKey_V, KM_CTRL);
    set(A_DUPLICATE, ImGuiKey_D, KM_CTRL);
    set(A_SELECT_ALL, ImGuiKey_A, KM_CTRL);
    set(A_PLACE_AGAIN, ImGuiKey_Space);
    set(A_PICK_TYPE, ImGuiKey_I);
    set(A_NEW, ImGuiKey_N, KM_CTRL);
    set(A_OPEN, ImGuiKey_O, KM_CTRL);
    set(A_SAVE, ImGuiKey_S, KM_CTRL);
    set(A_SAVE_AS, ImGuiKey_S, KM_CTRL | KM_SHIFT);
    set(A_TEST, ImGuiKey_F5);
    set(A_HELP, ImGuiKey_F1);
    set(A_PREFS, ImGuiKey_Comma, KM_CTRL);
    set(A_BRUSH_SMALLER, ImGuiKey_LeftBracket);
    set(A_BRUSH_BIGGER, ImGuiKey_RightBracket);
    hold_free = KM_SHIFT;
    hold_through = KM_ALT;
    hold_quarter = KM_CTRL;
    hold_reverse = KM_CTRL;
}

void Settings::reset_camera() {
    right_drag_pan = true;
    edge_scroll = false;
    edge_speed = pan_speed = rotate_speed = zoom_speed = 1.f;
    invert_rotate = invert_pitch = invert_zoom = invert_drag_pan = false;
}

void Settings::reset_general() {
    autosave_minutes = 2;
    ui_scale = 1.f;
    helpers_on = true;
    legend_on = false;  // the Helpers legend floats over the map: View > Legend opens it
    scene_lights = true;
    fast_terrain_detail = true;
    objects_follow_terrain = true;
}

void Settings::reset_testing() { quick_test = true; }

static float ini_float(const IniFile& ini, const char* sec, const char* key, float def, float lo, float hi) {
    std::string v = ini.get(sec, key);
    if (v.empty()) return def;
    return std::clamp(float(atof(v.c_str())), lo, hi);
}

void Settings::load() {
    scenario_folder = read_scenario_folder();
    Bytes b;
    if (!read_file(ini_path(), b)) { first_run = true; return; }
    IniFile ini;
    ini.parse(std::string(b.begin(), b.end()), false);
    std::string i = ini.get("Editor", "Install");
    if (!i.empty()) install = fs::path(widen(i));
    author = ini.get("Editor", "Author");
    for (int k = 0; k < 8; k++) recent[k] = ini.get("Recent", "File" + std::to_string(k));
    auto flag = [&](const char* sec, const char* key, bool def) { return ini.get(sec, key, def ? "1" : "0") != "0"; };
    // Camera ([Editor] EdgeScroll / InvertRotate are the names older versions wrote).
    // Edge scrolling is OFF by default: a stored EdgeScroll=1 only counts when the user switched it on themselves (EdgeScrollOptIn=1, written by
    // this version); older profiles stored 1 while it was the default, which made the view run away when the mouse touched the window edge.
    edge_scroll = flag("Editor", "EdgeScroll", false) && flag("Camera", "EdgeScrollOptIn", false);
    invert_rotate = flag("Editor", "InvertRotate", false);
    right_drag_pan = flag("Camera", "RightDragPan", true);
    invert_pitch = flag("Camera", "InvertPitch", false);
    invert_zoom = flag("Camera", "InvertZoom", false);
    invert_drag_pan = flag("Camera", "InvertDragPan", false);
    edge_speed = ini_float(ini, "Camera", "EdgeSpeed", 1, 0.2f, 4);
    pan_speed = ini_float(ini, "Camera", "PanSpeed", 1, 0.2f, 4);
    rotate_speed = ini_float(ini, "Camera", "RotateSpeed", 1, 0.2f, 4);
    zoom_speed = ini_float(ini, "Camera", "ZoomSpeed", 1, 0.2f, 4);
    // General
    autosave_minutes = std::clamp(atoi(ini.get("General", "AutosaveMinutes", "2").c_str()), 0, 60);
    ui_scale = ini_float(ini, "General", "UIScale", 1, 0.75f, 2);
    helpers_on = flag("General", "Helpers", true);
    legend_on = flag("General", "Legend", false);
    scene_lights = flag("General", "SceneLights", true);
    fast_terrain_detail = flag("General", "FastTerrainDetail", true);
    objects_follow_terrain = flag("General", "ObjectsFollowTerrain", true);
    skip_update_below = std::clamp(atoi(ini.get("General", "SkipUpdateBelow", "0").c_str()), 0, 1000);
    // Testing
    quick_test = flag("Testing", "QuickTest", true);
    // Hotkeys: a missing entry keeps the default; "None" clears a slot.
    for (int a = 0; a < A_COUNT; a++)
        for (int k = 0; k < 2; k++) {
            std::string v = ini.get("Hotkeys", std::string(kActs[a].ini) + (k ? "2" : ""));
            if (v.empty()) continue;
            keys[a][k] = iequals(v, "None") ? KeyBind() : parse_key(v);
        }
    hold_free = mod_of(ini.get("Hotkeys", "HoldPlaceFreely"), KM_SHIFT);
    hold_through = mod_of(ini.get("Hotkeys", "HoldClickThrough"), KM_ALT);
    hold_quarter = mod_of(ini.get("Hotkeys", "HoldQuarterTurn"), KM_CTRL);
    hold_reverse = mod_of(ini.get("Hotkeys", "HoldReverseBrush"), KM_CTRL);
    if (hold_free == hold_through || hold_free == hold_quarter || hold_through == hold_quarter) {  // a hand-edited clash: the defaults
        hold_free = KM_SHIFT;
        hold_through = KM_ALT;
        hold_quarter = KM_CTRL;
    }
}

void Settings::save() const {
    auto b01 = [](bool v) { return std::string(v ? "1" : "0"); };
    auto f2s = [](float v) { char t[32]; snprintf(t, sizeof t, "%.2f", v); return std::string(t); };
    std::string t = "[Editor]\r\nInstall=" + path_utf8(install) + "\r\nEdgeScroll=" + b01(edge_scroll) +
                    "\r\nInvertRotate=" + b01(invert_rotate) + "\r\nAuthor=" + author + "\r\n";
    t += "[Camera]\r\nEdgeScrollOptIn=" + b01(edge_scroll) + "\r\nRightDragPan=" + b01(right_drag_pan) + "\r\nInvertPitch=" + b01(invert_pitch) + "\r\nInvertZoom=" + b01(invert_zoom) +
         "\r\nInvertDragPan=" + b01(invert_drag_pan) + "\r\nEdgeSpeed=" + f2s(edge_speed) + "\r\nPanSpeed=" + f2s(pan_speed) +
         "\r\nRotateSpeed=" + f2s(rotate_speed) + "\r\nZoomSpeed=" + f2s(zoom_speed) + "\r\n";
    t += "[General]\r\nAutosaveMinutes=" + std::to_string(autosave_minutes) + "\r\nUIScale=" + f2s(ui_scale) + "\r\nHelpers=" + b01(helpers_on) +
         "\r\nLegend=" + b01(legend_on) + "\r\nSceneLights=" + b01(scene_lights) + "\r\nFastTerrainDetail=" + b01(fast_terrain_detail) +
         "\r\nObjectsFollowTerrain=" + b01(objects_follow_terrain) + "\r\nSkipUpdateBelow=" + std::to_string(skip_update_below) + "\r\n";
    t += "[Testing]\r\nQuickTest=" + b01(quick_test) + "\r\n";
    t += "[Hotkeys]\r\n";
    for (int a = 0; a < A_COUNT; a++)
        for (int k = 0; k < 2; k++) {
            std::string v = key_text(keys[a][k]);
            t += std::string(kActs[a].ini) + (k ? "2" : "") + "=" + (v.empty() ? "None" : v) + "\r\n";
        }
    t += std::string("HoldPlaceFreely=") + mod_name(hold_free) + "\r\nHoldClickThrough=" + mod_name(hold_through) +
         "\r\nHoldQuarterTurn=" + mod_name(hold_quarter) + "\r\nHoldReverseBrush=" + mod_name(hold_reverse) + "\r\n";
    t += "[Recent]\r\n";
    for (int k = 0; k < 8; k++) if (!recent[k].empty()) t += "File" + std::to_string(k) + "=" + recent[k] + "\r\n";
    write_file(ini_path(), t.data(), t.size());
}

void Settings::add_recent(const fs::path& p) {
    std::string s = path_utf8(p);
    int at = 7;
    for (int k = 0; k < 8; k++) if (iequals(recent[k], s)) at = k;
    for (int k = at; k > 0; k--) recent[k] = recent[k - 1];
    recent[0] = s;
}

void Settings::remove_recent(const fs::path& p) {
    const std::string s = path_utf8(p);
    int to = 0;
    for (int k = 0; k < 8; k++)
        if (!recent[k].empty() && !iequals(recent[k], s)) recent[to++] = recent[k];
    for (int k = to; k < 8; k++) recent[k].clear();
}

}  // namespace ed
