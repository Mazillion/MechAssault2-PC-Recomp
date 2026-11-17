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

const char* mod_name(int m) { return m == KM_CTRL ? "Ctrl" : m == KM_ALT ? "Alt" : "Shift"; }
static int mod_of(const std::string& s, int def) {
    return iequals(s, "Ctrl") ? KM_CTRL : iequals(s, "Alt") ? KM_ALT : iequals(s, "Shift") ? KM_SHIFT : def;
}

const char* act_label(int a) { return a >= 0 && a < A_COUNT ? kActTable[a].label : ""; }
const char* act_section(int a) { return a >= 0 && a < A_COUNT ? kActTable[a].section : ""; }

void Settings::reset_keys() {
    for (int a = 0; a < A_COUNT; a++) {
        keys[a][0] = kActTable[a].def[0];
        keys[a][1] = kActTable[a].def[1];
    }
    hold_free = KM_SHIFT;
    hold_through = KM_ALT;
    hold_quarter = KM_CTRL;
    hold_reverse = KM_CTRL;
    hold_orbit = KM_ALT;
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
    ui_scale_auto = true;
    helpers_on = true;
    legend_on = false;  // the Helpers legend floats over the map: View > Legend opens it
    scene_lights = true;
    lift_dark = true;
    fast_terrain_detail = true;
    objects_follow_terrain = true;
    raise_amount = 2.f;
    build_up = false;
    ramp_width = 16.f;
    tool_strength[0] = 1.f; tool_strength[1] = tool_strength[2] = tool_strength[3] = tool_strength[4] = 0.5f;
    show_steep = false;
    sc_density = 40.f; sc_spacing = 1.f; sc_lean = 0.f; sc_slope = 60.f;
    sc_facing = true; sc_follow = false; sc_keep_off = true; sc_skip_steep = false;
    slope_plate = SlopeMode::Level;
    slope_road = SlopeMode::Tilt;
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
    ui_scale_auto = flag("General", "UIScaleAuto", ui_scale <= 1.001f);   // an older file with a size the user chose keeps it
    left_panel = ini_float(ini, "Layout", "LeftPanel", 0, 0, 720);
    if (left_panel > 0) left_panel = std::clamp(left_panel, 300.f, 720.f);
    minimap = std::clamp(atoi(ini.get("Layout", "Minimap", "-1").c_str()), -1, 1);
    timeline = std::clamp(atoi(ini.get("Layout", "Timeline", "0").c_str()), 0, 1);
    pal_pictures = !iequals(ini.get("Palette", "View", "Pictures"), "List");
    pal_size = std::clamp(atoi(ini.get("Palette", "PicSize", "1").c_str()), 0, 2);
    helpers_on = flag("General", "Helpers", true);
    legend_on = flag("General", "Legend", false);
    scene_lights = flag("General", "SceneLights", true);
    lift_dark = flag("General", "BrightenDark", true);
    fast_terrain_detail = flag("General", "FastTerrainDetail", true);
    objects_follow_terrain = flag("General", "ObjectsFollowTerrain", true);
    slope_plate = SlopeMode(std::clamp(atoi(ini.get("General", "SlopePlate", "0").c_str()), 0, 2));
    slope_road = SlopeMode(std::clamp(atoi(ini.get("General", "SlopeRoad", "1").c_str()), 0, 2));
    skip_update_below = std::clamp(atoi(ini.get("General", "SkipUpdateBelow", "0").c_str()), 0, 1000);
    raise_amount = ini_float(ini, "Brush", "RaiseAmount", 2.f, 0.1f, 1000.f);
    build_up = flag("Brush", "BuildUp", false);
    ramp_width = ini_float(ini, "Brush", "RampWidth", 16.f, 4.f, 200.f);
    show_steep = flag("Brush", "ShowSteep", false);
    sc_density = ini_float(ini, "Scatter", "Density", 40.f, 1.f, 400.f);
    sc_spacing = ini_float(ini, "Scatter", "Spacing", 1.f, 0.3f, 3.f);
    sc_lean = ini_float(ini, "Scatter", "Lean", 0.f, 0.f, 20.f);
    sc_slope = ini_float(ini, "Scatter", "SlopeLimit", 60.f, 10.f, 150.f);
    sc_facing = flag("Scatter", "RandomFacing", true);
    sc_follow = flag("Scatter", "FollowSlope", false);
    sc_keep_off = flag("Scatter", "KeepOff", true);
    sc_skip_steep = flag("Scatter", "SkipSteep", false);
    tool_strength[0] = ini_float(ini, "Brush", "StrengthTexture", 1.f, 0.02f, 1.f);
    tool_strength[1] = ini_float(ini, "Brush", "StrengthTint", 0.5f, 0.02f, 1.f);
    tool_strength[2] = ini_float(ini, "Brush", "StrengthSmooth", 0.5f, 0.02f, 1.f);
    tool_strength[3] = ini_float(ini, "Brush", "StrengthFlatten", 0.5f, 0.02f, 1.f);
    tool_strength[4] = ini_float(ini, "Brush", "StrengthPlants", 0.5f, 0.02f, 1.f);
    snap = snap_prefs_from_ini(ini);  // a file from an older version has no [Snap]: the defaults
    // Testing
    quick_test = flag("Testing", "QuickTest", true);
    // Hotkeys: a missing entry keeps the default; "None" clears a slot.
    for (int a = 0; a < A_COUNT; a++)
        for (int k = 0; k < 2; k++) {
            std::string v = ini.get("Hotkeys", std::string(kActTable[a].ini) + (k ? "2" : ""));
            if (v.empty()) continue;
            keys[a][k] = iequals(v, "None") ? KeyBind() : parse_key(v);
        }
    hold_free = mod_of(ini.get("Hotkeys", "HoldPlaceFreely"), KM_SHIFT);
    hold_through = mod_of(ini.get("Hotkeys", "HoldClickThrough"), KM_ALT);
    hold_quarter = mod_of(ini.get("Hotkeys", "HoldQuarterTurn"), KM_CTRL);
    hold_reverse = mod_of(ini.get("Hotkeys", "HoldReverseBrush"), KM_CTRL);
    hold_orbit = mod_of(ini.get("Hotkeys", "HoldOrbit"), KM_ALT);
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
    t += "[General]\r\nAutosaveMinutes=" + std::to_string(autosave_minutes) + "\r\nUIScale=" + f2s(ui_scale) + "\r\nUIScaleAuto=" + b01(ui_scale_auto) + "\r\nHelpers=" + b01(helpers_on) +
         "\r\nLegend=" + b01(legend_on) + "\r\nSceneLights=" + b01(scene_lights) + "\r\nBrightenDark=" + b01(lift_dark) +"\r\nFastTerrainDetail=" + b01(fast_terrain_detail) +
         "\r\nObjectsFollowTerrain=" + b01(objects_follow_terrain) + "\r\nSlopePlate=" + std::to_string(int(slope_plate)) + "\r\nSlopeRoad=" + std::to_string(int(slope_road)) + "\r\nSkipUpdateBelow=" + std::to_string(skip_update_below) + "\r\n";
    t += "[Brush]\r\nRaiseAmount=" + f2s(raise_amount) + "\r\nBuildUp=" + b01(build_up) + "\r\nRampWidth=" + f2s(ramp_width) + "\r\nShowSteep=" + b01(show_steep) +
         "\r\nStrengthTexture=" + f2s(tool_strength[0]) + "\r\nStrengthTint=" + f2s(tool_strength[1]) + "\r\nStrengthSmooth=" + f2s(tool_strength[2]) +
         "\r\nStrengthFlatten=" + f2s(tool_strength[3]) + "\r\nStrengthPlants=" + f2s(tool_strength[4]) + "\r\n";
    t += "[Scatter]\r\nDensity=" + f2s(sc_density) + "\r\nSpacing=" + f2s(sc_spacing) + "\r\nRandomFacing=" + b01(sc_facing) + "\r\nLean=" + f2s(sc_lean) +
         "\r\nFollowSlope=" + b01(sc_follow) + "\r\nKeepOff=" + b01(sc_keep_off) + "\r\nSkipSteep=" + b01(sc_skip_steep) + "\r\nSlopeLimit=" + f2s(sc_slope) + "\r\n";
    t += snap_prefs_text(snap);
    t += "[Layout]\r\nLeftPanel=" + f2s(left_panel) + "\r\nMinimap=" + std::to_string(minimap) + "\r\nTimeline=" + std::to_string(timeline) + "\r\n";
    t += std::string("[Palette]\r\nView=") + (pal_pictures ? "Pictures" : "List") + "\r\nPicSize=" + std::to_string(pal_size) + "\r\n";
    t += "[Testing]\r\nQuickTest=" + b01(quick_test) + "\r\n";
    t += "[Hotkeys]\r\n";
    for (int a = 0; a < A_COUNT; a++)
        for (int k = 0; k < 2; k++) {
            std::string v = key_text(keys[a][k]);
            t += std::string(kActTable[a].ini) + (k ? "2" : "") + "=" + (v.empty() ? "None" : v) + "\r\n";
        }
    t += std::string("HoldPlaceFreely=") + mod_name(hold_free) + "\r\nHoldClickThrough=" + mod_name(hold_through) +
         "\r\nHoldQuarterTurn=" + mod_name(hold_quarter) + "\r\nHoldReverseBrush=" + mod_name(hold_reverse) +
         "\r\nHoldOrbit=" + mod_name(hold_orbit) + "\r\n";
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
