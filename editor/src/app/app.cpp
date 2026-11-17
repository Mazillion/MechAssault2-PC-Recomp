#include "ui_tips.h"
#include "app.h"
#include "theme.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_set>
#include <cstdio>

#include "../core/crop.h"
#include "../core/cutscene.h"
#include "../core/triggers.h"
#include "../core/weather.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "ui_util.h"
#include "automation.h"
#include "version.h"

namespace ed {

static int g_check_rows = -1;  // list length the Check window was last sized for (-1 = closed); it refits when it opens or the list changes length

using namespace ma2;
using namespace ma2::world;

static int ask_box(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags);
extern float g_dbg_w;
extern float g_sky_fog_shift;
extern int g_sky_fog_force;
extern float g_sky_heading_add;
extern int g_bloom_dbg;  // render.cpp
static bool g_cam_hold = false;  // --cam-eye: keep the traced eye height (no easing to the ground)

static const uint32_t K_BUILDING = key_of("MABuilding"), K_CORP = key_of("MACorporeal"), K_MECH = key_of("MAMech"),
                      K_TANK = key_of("MATank"), K_FLYER = key_of("MAFlyer"), K_VEHICLE = key_of("MAVehicle"),
                      K_TURRET = key_of("MATurret"), K_POWERUP = key_of("MAPowerUp"), K_FLAG = key_of("MAFlagCTF"),
                      K_TRIG_RADIUS = key_of("MATriggerRadius"), K_TRIG_NRADIUS = key_of("MATriggerNationRadius"), K_TRIG_VOLUME = key_of("MATriggerCuboidalVolume"),
                      K_WAYPOINT = key_of("MAWaypoint"), K_WPGROUP = key_of("MAWaypointGroup"), K_GROUPSET = key_of("MoeGroupSet"),
                      K_ROAD = key_of("MARoad"), K_PERSON = key_of("MAPerson"), K_CRATE = key_of("MACrateGenerator"),
                      K_SNATCH_BASE = key_of("MASnatchItBase"), K_SNATCH_CORE = key_of("MASnatchItDataCore"),
                      K_EFFECT = key_of("MAEffect"), K_AIGROUP = key_of("MAAIGroup"), K_FREECAM = key_of("MAFreeCamera"),
                      K_SPAWNER = key_of("MASpawner");

// Camera path colour (waypoints under a MAFreeCamera): teal, apart from the purple AI paths.
static constexpr uint32_t kCamPathColor = 0xFFC8DC3Cu;

// The waypoint groups cinema paths fly through (MAPath m_wptGroupUID, in a MACinema's 'Paths').
static std::set<uint32_t> cinema_path_groups(const Wdf& w) {
    static const uint32_t K_MAPATH = key_of("MAPath");
    std::set<uint32_t> g;
    for (auto& o : w.objs) if (o.cls == K_MAPATH) if (uint32_t u = o.u32("m_wptGroupUID")) g.insert(u);
    return g;
}

// A waypoint group owned by a MAFreeCamera, or flown by a cinema MAPath, is a cutscene camera path, not an AI path.
static bool camera_path_group(const Wdf& w, const WTree& t, size_t group, const std::set<uint32_t>& paths) {
    int p = t.parent_of[group];
    return (p >= 0 && w.objs[size_t(p)].cls == K_FREECAM) || paths.count(w.objs[group].uid()) > 0;
}

static bool is_container(uint32_t cls) {
    return cls == K_GROUP || cls == K_MTG || cls == K_TERRAIN || cls == K_ROOT || cls == K_GROUPSET || cls == K_WPGROUP ||
           cls == K_AIGROUP;
}

// ---------------------------------------------------------------------------------------------
// Lifecycle

bool App::init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, float dpi, std::string* err) {
    hwnd_ = hwnd;
    dpi_ = base_dpi_ = dpi;
    if (!r_.init(dev, ctx, err)) return false;
    models_.init(dev, ctx);
    settings_.load();
    apply_ui_scale();
    show_helpers_ = settings_.helpers_on;
    show_legend_ = settings_.legend_on;
    r_.params.scene_lights = settings_.scene_lights;
    r_.params.lift_dark = settings_.lift_dark;
    r_.params.fast_terrain_detail = settings_.fast_terrain_detail;
    // The MA2 dictionary: the snapshot embedded as resource 201; MA2SE_CATALOG=<catalog.json>
    // loads another one (developers refreshing the dictionary).
    {
        std::string text;
        if (const wchar_t* f = _wgetenv(L"MA2SE_CATALOG")) {
            Bytes b;
            if (read_file(f, b)) text.assign(b.begin(), b.end());
        }
        if (text.empty())
            if (HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10)))  // RT_RCDATA
                if (HGLOBAL g = LoadResource(nullptr, rs)) text.assign(static_cast<const char*>(LockResource(g)), SizeofResource(nullptr, rs));
        dict_.load(text);
        if (HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(202), MAKEINTRESOURCEW(10)))
            if (HGLOBAL g = LoadResource(nullptr, rs)) {
                std::string tsv(static_cast<const char*>(LockResource(g)), SizeofResource(nullptr, rs));
                for (auto& l : split(tsv, '\n')) {
                    if (l.empty() || l[0] == '#') continue;
                    size_t tab = l.find('\t');
                    if (tab != std::string::npos) palette_kind_[uint32_t(strtoul(l.c_str(), nullptr, 10))] = trim(l.substr(tab + 1));
                }
            }
    }
    std::string gerr;
    if (!load_game(&gerr)) {
        game_error_ = gerr;
        open_prefs_ = true;  // Settings > General holds the game folder
    }
    // No dialog at start: the start page in the view offers New map / New mission / Open / Look
    // (and welcomes a first-time user; Getting Started stays under Help).
    return true;
}

bool App::load_game(std::string* err) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path exe_dir = fs::path(buf).parent_path();
    fs::path install = settings_.install.empty() ? GameData::find_install(exe_dir) : settings_.install;
    io_.reset();
    cat_.reset();
    foundation_cache_.clear();
    water_types_.clear();  // point into the old catalog
    water_names_.clear();
    if (!gd_.open(install, err)) {
        // One short line; Settings is where this is shown, so no pointer back to Settings here.
        std::error_code fec;
        const bool there = fs::exists(install / L"game_files" / L"common.mgf", fec);
        if (err) *err = there ? "Could not read the game data in " + path_utf8(install) + ": " + *err
                              : "No game found in " + path_utf8(install) + ". Use Choose... to pick the folder with MechAssault 2.exe and game_files.";
        return false;
    }
    // The PC build scales fog by the player's draw distance (pc_draw_distance.c ma2_draw_distance_fog). The game reads
    // the player's own %LOCALAPPDATA%\MechAssault2\pc_settings.ini (pc_storage.c ma2_pc_settings_path), the install copy
    // only until that exists.
    fs::path pcini = Settings::local_dir() / L"pc_settings.ini";
    if (std::error_code ec; !fs::exists(pcini, ec)) pcini = install / "pc_settings.ini";
    int dd = int(GetPrivateProfileIntW(L"Display", L"DrawDistance", 50, pcini.wstring().c_str()));
    fog_scale_ = std::pow(2.f, float(std::clamp(dd, 0, 100) - 50) / 50.f);
    cat_ = std::make_unique<TemplateCatalog>();
    if (!cat_->load(gd_, err)) return false;
    io_ = std::make_unique<ScenarioIO>(gd_, *cat_);
    load_trigger_prototypes(gd_, nullptr);
    game_error_.clear();
    return true;
}

void App::shutdown() {
    // A normal exit: the player already saved or chose to discard (confirm_close).
    clear_autosave();
    settings_.save();
    r_.shutdown();
    if (test_proc_) CloseHandle(test_proc_);
    test_proc_ = nullptr;
    delete base_style_;
    base_style_ = nullptr;
    delete raw_style_;
    raw_style_ = nullptr;
}

void App::set_document(std::unique_ptr<Scenario> s) {
    doc_ = std::move(s);
    check_stale_ = true;  // an open Check window lists the new map's items, not the old one's
    // The views index the old map's objects until the next render rebuilds them; the panels drawn later in this frame
    // (Everything in this map) read doc_->world.objs[v.index], out of range when the new map has fewer objects.
    views_.clear();
    view_type_ = -1;
    show_new_ = false;
    show_getting_started_ = false;  // the welcome box would cover the map just opened
    ns_on_ = false;  // ns_start() turns the Next steps card on for a map made with New
    status_.clear();  // the last map's message ('Looking at X (read-only)...') does not describe the new one
    status_time_ = 0;
    models_.set_archives(gd_.game_files(), doc_->base, gd_.level_archives());
    selection_.clear();
    undo_.clear();
    redo_.clear();
    cur_state_id_ = 0;  // a freshly opened document is the saved state
    saved_state_id_ = 0;
    next_state_id_ = 1;
    merge_state_ = -1;
    snap_hgt_.reset();
    snap_cm_.reset();
    snap_lm_.reset();
    snap_tm_.reset();
    snap_audio_.reset();
    validation_.clear();
    last_report_ = SaveReport();
    gfx_reset();  // the graphics figures belong to the old map's Base
    lightmap_stale_ = false;
    stale_sun_only_ = false;
    preview_on_ = false;
    trig_uid_ = 0;
    pick_ = PickTarget();
    tool_ = Tool::Select;
    // The old map's message ('Looking at <level> (read-only)') stayed on an editable copy made
    // within its 6 s; each caller sets its own message after this.
    status_.clear();
    status_time_ = 0;
    document_changed(true, true);
    // The object list is rebuilt now, not later in the frame: until then views_ holds the old map's object
    // indices and models (freed when the level archives change), and the input, gizmo and panel code that
    // reads it before the frame's rebuild crashed on a second retail level (C0000005, Mission_14 -> Mission_03).
    rebuild_objects();
    adopt_sheets();  // flat pieces of a map made before layers existed keep their heights, in the order they are drawn
    reset_view();
}

// The view a document opens with: the whole map at an angle, or the first player start up close.
void App::reset_view() {
    if (!doc_) return;
    cam_.tx = doc_->terrain.size_x() * 0.5f;
    cam_.tz = doc_->terrain.size_z() * 0.35f;
    cam_.dist = std::max(doc_->terrain.size_x(), doc_->terrain.size_z()) * 0.75f;
    cam_.yaw = 0;
    cam_.pitch = 0.95f;
    // Missions open on the player start; multiplayer maps on the whole map (their level-start objects
    // can be copied spawns in a far corner, 'reopen').
    for (auto& o : doc_->world.objs)
        if (!doc_->multiplayer && o.cls == K_LEVELSTART) {
            Vec3 p;
            if (get_pos(o, p)) {
                // Review (open-map framing): a start near the map edge (Mission 05) put half the view off the map.
                // Aim up to 170 m inside the edge; the start stays in view (the view is ~190 m to each side at 350 m).
                const float m = std::min(170.f, 0.25f * std::min(doc_->terrain.size_x(), doc_->terrain.size_z()));
                cam_.tx = std::clamp(p.x, m, doc_->terrain.size_x() - m);
                cam_.tz = std::clamp(p.z, m, doc_->terrain.size_z() - m);
                cam_.ty = doc_->terrain.height_at(p.x, p.z);
                cam_.dist = 350.f;
                // Lower than the whole-map angle: at 54 deg down, 350 m away, a new 'Random hills' mission read as flat
                // ground; 40 deg shows the hills and some horizon around the start.
                cam_.pitch = 0.7f;
                return;
            }
            break;
        }
    // The whole map: aim at its middle and back off until all four corners are in the view (the fixed 0.75 x size
    // cut off the near edge and left the far half small).
    const float sx = doc_->terrain.size_x(), sz = doc_->terrain.size_z();
    cam_.tx = sx * 0.5f;
    cam_.tz = sz * 0.5f;
    cam_.ty = doc_->terrain.height_at(cam_.tx, cam_.tz);
    if (vp_w > 50 && vp_h > 50) {
        const float pad = 0.04f;
        for (cam_.dist = 0.4f * std::max(sx, sz); cam_.dist < 4000.f; cam_.dist *= 1.06f) {
            cam_.update(vp_w / vp_h);
            bool in = true;
            for (int k = 0; k < 4 && in; k++) {
                const float x = (k & 1) ? sx : 0, z = (k & 2) ? sz : 0;
                float px, py;
                in = cam_.project({x, doc_->terrain.height_at(x, z), z}, vp_w, vp_h, px, py) && px >= pad * vp_w &&
                     px <= (1 - pad) * vp_w && py >= pad * vp_h && py <= (1 - pad) * vp_h;
            }
            if (in) break;
        }
    }
}

void App::document_changed(bool terrain, bool textures) {
    if (!doc_) return;
    if (terrain) r_.set_terrain(doc_->terrain);
    if (textures) {
        if (!doc_->colormap.px.empty()) r_.set_colormap(doc_->colormap);
        r_.set_ground_layers(&doc_->ground);
        if (!doc_->lightmap.px.empty()) r_.set_lightmap(doc_->lightmap);
        r_.params.lighting = !doc_->lightmap.px.empty();
    }
    objects_dirty_ = minimap_dirty_ = overlay_dirty_ = true;
}

void App::world_changed() {
    if (!doc_) return;
    doc_->dirty = true;
    objects_dirty_ = minimap_dirty_ = check_stale_ = true;
    // Drop selections that no longer exist.
    std::set<uint32_t> keep;
    for (uint32_t u : selection_)
        for (auto& o : doc_->world.objs) if (o.uid() == u) { keep.insert(u); break; }
    selection_.swap(keep);
}

void App::message(const std::string& title, const std::string& text) {
    msg_title_ = title;
    msg_text_ = text;
    show_msg_ = true;
}

bool App::confirm_close() {
    if (!doc_ || !doc_->dirty || doc_->read_only) return true;
    std::wstring q = L"Save the changes to \"" + widen(doc_->name) + L"\" first?\n\nYes saves them, No throws them away, Cancel keeps editing.";
    int r = ask_box(hwnd_, q.c_str(), L"MA2 Scenario Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) {
        cmd_save(false);
        return !doc_->dirty;
    }
    clear_autosave();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Frame end, device loss, autosave and crash recovery

void App::end_frame() {
    r_.end_frame();
    models_.end_frame();
}

void App::device_lost() {
    r_.shutdown();
    models_.clear();
    models_.end_frame();
}

bool App::device_restored(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string* err) {
    if (!r_.init(dev, ctx, err)) return false;
    models_.reset_device(dev, ctx);
    if (doc_) {
        models_.set_archives(gd_.game_files(), doc_->base, gd_.level_archives());
        document_changed(true, true);
        overlay_dirty_ = true;
    }
    status_ = "The graphics device was reset; the view was rebuilt.";
    status_time_ = 6;
    return true;
}

static fs::path autosave_dir() { return Settings::autosave_dir(); }

// One recovery slot per editor process: Recovery-<pid>.ma2map (+ .txt). Two editors at once (one from the game's Custom
// Scenarios, one from the desktop) used to share one slot: the second offered the first's live copy, and either one's
// clean save or exit deleted the other's crash copy. A named mutex held for the life of the process marks a slot as
// owned; at start only slots whose owner is gone are offered. The old shared Recovery.ma2map is still offered once.
static std::wstring own_slot() { return L"Recovery-" + std::to_wstring(GetCurrentProcessId()); }
static std::wstring slot_mutex_name(const std::wstring& stem) { return L"Local\\MA2SE-" + stem; }
static void claim_own_slot() {
    static HANDLE h = CreateMutexW(nullptr, FALSE, slot_mutex_name(own_slot()).c_str());  // lives until the process ends
    (void)h;
}
static bool slot_owner_alive(const std::wstring& stem) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, slot_mutex_name(stem).c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// Writes the open scenario to the recovery slot. The slot only exists while there are unsaved
// changes; a clean save, a discard or a normal exit removes it.
bool App::autosave(bool force) {
    autosave_timer_ = 0;
    if (!doc_ || !io_ || doc_->read_only || (!doc_->dirty && !force)) return false;
    claim_own_slot();
    std::error_code ec;
    fs::create_directories(autosave_dir(), ec);
    Scenario copy = *doc_;
    Bytes b;
    std::string err;
    if (!io_->build(copy, b, nullptr, &err)) return false;
    if (!doc_->id) doc_->id = copy.id;
    if (!write_file(autosave_dir() / (own_slot() + L".ma2map"), b, &err)) return false;
    autosaved_at_ = (long long)time(nullptr);
    std::string note = path_utf8(doc_->file) + "\n" + doc_->name + "\n";
    write_file(autosave_dir() / (own_slot() + L".txt"), note.data(), note.size(), &err);
    return true;
}

void App::clear_autosave() {
    std::error_code ec;
    fs::remove(autosave_dir() / (own_slot() + L".ma2map"), ec);
    fs::remove(autosave_dir() / (own_slot() + L".txt"), ec);
}

bool App::emergency_save() { return autosave(true); }

bool App::has_unsaved() const { return doc_ && doc_->dirty && !doc_->read_only; }

// The monitor's DPI changed (the window was dragged to another screen, or the user changed the scaling): the next frame
// rebuilds the style and font scale from the unscaled theme. Safe from a window message, also from a nested modal loop.
void App::set_base_dpi(float scale) {
    if (!(scale > 0.25f && scale < 16.f) || scale == base_dpi_) return;
    base_dpi_ = scale;
    ui_scale_dirty_ = true;
}

void App::offer_recovery() {
    if (!io_) return;
    claim_own_slot();
    // The newest slot (by write time) that no running editor owns.
    fs::path rec_stem;
    std::error_code ec;
    fs::file_time_type best{};
    for (fs::directory_iterator it(autosave_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (!iequals(path_utf8(p.extension()), ".ma2map")) continue;
        std::wstring stem = p.stem().native();
        if (stem != L"Recovery" && stem.rfind(L"Recovery-", 0) != 0) continue;
        if (stem == own_slot() || slot_owner_alive(stem)) continue;
        std::error_code e2;
        auto t = fs::last_write_time(p, e2);
        if (rec_stem.empty() || t > best) { rec_stem = p.parent_path() / stem; best = t; }
    }
    if (rec_stem.empty()) return;
    fs::path rec = rec_stem; rec += L".ma2map";
    fs::path rec_txt = rec_stem; rec_txt += L".txt";
    auto drop_slot = [&] { std::error_code e3; fs::remove(rec, e3); fs::remove(rec_txt, e3); };
    // "No" used to delete the only copy for good; now it moves into Discarded\ (the newest 5 stay), so a mis-click is not final.
    auto put_slot_aside = [&](const fs::path& map, const fs::path& txt) {
        std::error_code e4;
        fs::path dir = autosave_dir() / L"Discarded";
        fs::create_directories(dir, e4);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t stamp[64];
        swprintf(stamp, 64, L"Discarded-%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fs::rename(map, dir / (std::wstring(stamp) + L".ma2map"), e4);
        if (e4) fs::remove(map, e4);  // could not move it: same as before
        fs::rename(txt, dir / (std::wstring(stamp) + L".txt"), e4);
        std::vector<fs::path> kept;
        for (fs::directory_iterator it(dir, e4), end; !e4 && it != end; it.increment(e4))
            if (iequals(path_utf8(it->path().extension()), ".ma2map")) kept.push_back(it->path());
        std::sort(kept.begin(), kept.end());  // the stamp sorts by time
        for (size_t i = 0; i + 5 < kept.size(); i++) {
            fs::remove(kept[i], e4);
            fs::path t = kept[i];
            t.replace_extension(L".txt");
            fs::remove(t, e4);
        }
    };
    Bytes note;
    read_file(rec_txt, note);
    auto lines = split(std::string(note.begin(), note.end()), '\n');
    std::string orig = lines.size() > 0 ? trim(lines[0]) : "", name = lines.size() > 1 ? trim(lines[1]) : "";
    std::wstring q = L"The editor did not close normally last time. Restore your unsaved work";
    q += name.empty() ? L"?" : L" on \"" + widen(name) + L"\"?";
    q += L"\n\nYes restores it (save it to keep it). No puts it aside (a copy stays in the Recovery folder\\Discarded for a while).";
    if (ask_box(hwnd_, q.c_str(), L"MA2 Scenario Editor", MB_YESNO | MB_ICONQUESTION) != IDYES) { put_slot_aside(rec, rec_txt); return; }
    auto s = std::make_unique<Scenario>();
    std::string err;
    if (!io_->load(rec, *s, &err)) { message("Recovery failed", "The recovery copy could not be read: " + err); return; }
    s->file = orig.empty() ? fs::path() : fs::path(widen(orig));
    set_document(std::move(s));
    doc_->dirty = true;
    if (autosave(true)) drop_slot();  // moved into this editor's own slot first, so a crash right now still has a copy
    show_getting_started_ = false;
    tab_ = Tab::Map;
    status_ = "Restored unsaved work. Save it (" + kt(A_SAVE) + ") to keep it.";
    status_time_ = 10;
}

void App::confirm(const std::string& text, std::function<void()> action) {
    confirm_text_ = text;
    confirm_action_ = std::move(action);
    open_confirm_ = true;
}

std::string App::answer_confirm(bool yes) {
    if (!confirm_action_ && !open_confirm_) return {};
    confirm_answer_ = yes ? 1 : 2;
    return confirm_text_;
}

void App::ui_confirm() {
    if (open_confirm_) { ImGui::OpenPopup("Please confirm"); open_confirm_ = false; }
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(460 * dpi_, 0), ImVec2(460 * dpi_, vp->WorkSize.y - 24 * dpi_));
    }
    if (theme::begin_modal("Please confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", confirm_text_.c_str());
        ImGui::Spacing();
        int answer = std::exchange(confirm_answer_, 0);
        if (ImGui::Button("Yes", ImVec2(120 * dpi_, 0)) || answer == 1) {
            ImGui::CloseCurrentPopup();
            auto a = std::move(confirm_action_);
            confirm_action_ = nullptr;
            if (a) a();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120 * dpi_, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || answer == 2) {
            confirm_action_ = nullptr;
            ImGui::CloseCurrentPopup();
        }
        theme::end_modal();
    }
}

// ---------------------------------------------------------------------------------------------
// File commands

// Scenario open/save dialog. It always starts in the scenario folder: the classic dialog reopened
// the folder last used anywhere in the editor (a WAV import), so missions were saved where the
// game never looks.
// Message boxes go through here: with --automation they are answered by the automation client
// (dialogs command) instead of popping up.
static int ask_box(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    AutomationDialogs& d = automation_dialogs();
    if (!d.active) return MessageBoxW(owner, text, title, flags);
    d.last_question = narrow(text);
    UINT kind = flags & 0xF;
    if (d.answer == IDCANCEL && kind == MB_YESNO) return IDNO;
    if (kind == MB_OK) return IDOK;
    return d.answer;
}

// File and folder pickers: with --automation the next path comes from the client (or cancel).
static bool automation_pick(std::wstring& out) {
    AutomationDialogs& d = automation_dialogs();
    if (!d.active) return false;
    out = d.next_file;
    d.next_file.clear();
    return true;
}

static std::wstring file_dialog(HWND owner, bool save, const fs::path& folder, const std::wstring& name) {
    if (std::wstring f; automation_pick(f)) return f;
    std::wstring out;
    IFileDialog* d = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    // A separate remembered state for scenario files.
    static const GUID kScenarioDialog = {0x6b1f3a52, 0x8d4e, 0x4c1a, {0x9e, 0x2b, 0x51, 0x7a, 0x33, 0x0c, 0xd2, 0x91}};
    d->SetClientGuid(kScenarioDialog);
    COMDLG_FILTERSPEC spec[] = {{L"MechAssault 2 scenario (*.ma2map)", L"*.ma2map"}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, spec);
    d->SetDefaultExtension(L"ma2map");
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    IShellItem* si = nullptr;
    if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
        d->SetFolder(si);
        si->Release();
    }
    if (!name.empty()) d->SetFileName(name.c_str());
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* r = nullptr;
        if (SUCCEEDED(d->GetResult(&r))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
            r->Release();
        }
    }
    d->Release();
    return out;
}

std::wstring pick_folder(HWND owner, const fs::path& start) {
    if (std::wstring f; automation_pick(f)) return f;
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IShellItem* si = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
        d->SetFolder(si);
        si->Release();
    }
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* r = nullptr;
        if (SUCCEEDED(d->GetResult(&r))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
            r->Release();
        }
    }
    d->Release();
    return out;
}

std::wstring pick_wav(HWND owner) {
    if (std::wstring f; automation_pick(f)) return f;
    wchar_t buf[MAX_PATH * 4] = {};
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = owner;
    of.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH * 4;
    of.Flags = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&of) ? std::wstring(buf) : std::wstring();
}

void App::cmd_new() {
    if (!io_) { message("No game data", game_error_ + "\nSet the game folder in File > Settings > General."); return; }
    if (!confirm_close()) return;
    // A name the player did not type: the first "New Mission" / "New Mission 2" / ... that no saved scenario has yet.
    if (is_default_name(new_params_.name)) new_params_.name = default_new_name(new_params_.multiplayer);
    show_new_ = true;
}

void App::cmd_new_kind(bool multiplayer) {
    new_params_.multiplayer = multiplayer;
    cmd_new();
}

bool App::is_default_name(const std::string& n) {
    for (const char* base : {"New Map", "New Mission", "New Scenario"}) {
        const size_t len = strlen(base);
        if (n.compare(0, len, base) != 0) continue;
        if (n.size() == len) return true;
        if (n[len] != ' ' || n.size() == len + 1) continue;
        bool digits = true;
        for (size_t i = len + 1; i < n.size(); i++) digits &= n[i] >= '0' && n[i] <= '9';
        if (digits) return true;
    }
    return false;
}

const ScenarioHead* App::head_of(const fs::path& f) {
    std::error_code ec;
    const auto t = fs::last_write_time(f, ec);
    if (ec) return nullptr;
    auto it = head_cache_.find(f);
    if (it == head_cache_.end() || it->second.time != t) {
        HeadCache c;
        c.time = t;
        c.ok = ScenarioIO::peek(f, c.head);
        it = head_cache_.insert_or_assign(f, c).first;
    }
    return it->second.ok ? &it->second.head : nullptr;
}

std::string App::name_clash(const std::string& name, const fs::path& self, bool multiplayer) {
    if (name.empty()) return {};
    std::error_code ec;
    for (fs::directory_iterator it(settings_.scenarios(), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& f = it->path();
        std::wstring ext = f.extension().native();
        for (auto& c : ext) c = wchar_t(towlower(c));
        if (ext != L".ma2map") continue;
        std::error_code e2;
        if (!self.empty() && fs::equivalent(f, self, e2)) continue;
        const ScenarioHead* h = head_of(f);
        if (h && h->multiplayer == multiplayer && iequals(h->name, name)) return path_utf8(f.filename());
    }
    return {};
}

static std::string fx5_stem(const std::string& path) {
    size_t s = path.find_last_of("/\\"), d = path.rfind('.');
    std::string n = path.substr(s == std::string::npos ? 0 : s + 1);
    d = n.rfind('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

std::vector<ValidationItem> App::check_now() {
    std::vector<ValidationItem> v = io_->validate(*doc_);
    if (!doc_->read_only) {
        placement_checks(v);  // heights: floating, same height, under / deep in / over the ground (app_checks.cpp)
        if (!doc_->old_cut_roads.empty()) {   // The file on disk holds cut road files an older editor wrote; the next save rewrites them
            ValidationItem it{1, "", doc_->old_cut_roads[0]};
            it.uids = doc_->old_cut_roads;
            const size_t n = it.uids.size();
            it.text = (n == 1 ? std::string("1 cut road piece was") : std::to_string(n) + " cut road pieces were") +
                      " saved by an older editor in a form the game cannot read: the game can run out of memory while loading this map. "
                      "Save the map (Ctrl+S) before you play it; saving writes the road pieces again (Test saves it for you).";
            v.push_back(it);
        }
        const std::string other = name_clash(doc_->name, doc_->file, doc_->multiplayer);
        if (!other.empty())
            v.push_back({1, "Another scenario, " + other + ", has the same name (\"" + doc_->name + "\"). The game lists them as \"" + doc_->name +
                                " (<file name>)\". Rename one of them on the Map tab.", 0});
    }
    // "No problems found" is only for a clean list: never above warnings.
    if (v.size() > 1) v.erase(std::remove_if(v.begin(), v.end(), [](const ValidationItem& i) { return i.level == 0; }), v.end());
    {   // The graphics figure is always listed (an info row), and the live figure takes this exact one
        const ScenarioIO::GraphicsEstimate g = io_->graphics_estimate(*doc_);
        gfx_calibrate(g.total_mb);
        char n[16];
        snprintf(n, sizeof n, "%.0f", g.total_mb);
        std::string t = "Graphics memory: about " + std::string(n) + " of the game's 64 MB (the game's own maps use about 25).";
        if (!g.biggest.empty()) {
            t += " Biggest from other levels: ";
            auto friendly = [&](const std::string& owner) {   // The palette's name for a model file name ("BldCom_Office07_WS_CS" -> Dante office 07)
                std::string o = lower(owner);
                if (o.size() > 3 && o.compare(o.size() - 3, 3, "_cs") == 0) o.resize(o.size() - 3);
                if (cat_)
                    for (auto& tp : cat_->all()) {
                        if (!tp.placeable || tp.model.empty()) continue;
                        if (lower(fx5_stem(tp.model)) == o) return palette_plain_name(tp);
                    }
                return owner;
            };
            std::vector<std::pair<std::string, double>> merged;   // a model and its collision model are one name
            for (auto& b : g.biggest) {
                const std::string nm = friendly(b.first);
                auto it = std::find_if(merged.begin(), merged.end(), [&](const auto& m) { return m.first == nm; });
                if (it != merged.end()) it->second += b.second; else merged.push_back({nm, b.second});
            }
            std::stable_sort(merged.begin(), merged.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            for (size_t i = 0; i < merged.size() && i < 3; i++) {
                char mb[16];
                snprintf(mb, sizeof mb, "%.1f", merged[i].second);
                t += (i ? ", " : "") + merged[i].first + " (" + mb + " MB)";
            }
            t += ".";
        }
        v.push_back({0, t, 0});
    }
    return v;
}

std::string App::default_new_name(bool multiplayer) {
    const std::string base = multiplayer ? "New Map" : "New Mission";
    std::error_code ec;
    for (int n = 1; n < 1000; n++) {
        const std::string name = n == 1 ? base : base + " " + std::to_string(n);
        if (name_clash(name, {}, multiplayer).empty() && !fs::exists(settings_.scenarios() / widen(safe_file_stem(name) + ".ma2map"), ec)) return name;
    }
    return base;
}

void App::offer_update_old_scenarios(bool from_menu) {
    if (!io_) return;
    if (!from_menu && settings_.skip_update_below >= kSaveRevision) return;
    std::vector<fs::path> old;
    std::error_code ec;
    for (fs::directory_iterator it(settings_.scenarios(), ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring ext = it->path().extension().native();
        for (auto& c : ext) c = wchar_t(towlower(c));
        if (ext != L".ma2map") continue;
        const ScenarioHead* h = head_of(it->path());
        if (h && h->saved_with < kSaveRevision) old.push_back(it->path());
    }
    if (old.empty()) {
        if (from_menu) { status_ = "All the scenarios in your scenario folder are up to date."; status_time_ = 6; }
        return;
    }
    const size_t n = old.size();
    std::wstring q = std::to_wstring(n) + (n == 1 ? L" scenario in your scenario folder was" : L" scenarios in your scenario folder were") +
                     L" saved with an older editor. Some buildings in " + (n == 1 ? L"it" : L"them") +
                     L" can show as pink markers in the game.\n\nUpdate " + (n == 1 ? L"it" : L"them") +
                     L" now? (The old version of each file is kept as .bak.)";
    if (ask_box(hwnd_, q.c_str(), L"MA2 Scenario Editor", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        if (!from_menu) { settings_.skip_update_below = kSaveRevision; settings_.save(); }
        return;
    }
    int done = 0;
    std::string failed;
    for (auto& f : old) {
        bool ok = false;
        std::error_code e2;
        const bool open_now = doc_ && !doc_->file.empty() && fs::equivalent(doc_->file, f, e2);
        if (!open_now) {
            Scenario s;
            std::string err;
            SaveReport rep;
            std::vector<std::string> damaged;
            ok = io_->load(f, s, &err, &damaged) && damaged.empty() && io_->save(s, f, &rep, &err);  // a damaged file is left as it is
        }
        if (ok) done++;
        else failed += (failed.empty() ? "" : ", ") + path_utf8(f.filename());
    }
    status_ = "Updated " + std::to_string(done) + (done == 1 ? " scenario." : " scenarios.");
    if (!failed.empty()) status_ += " " + std::to_string(n - size_t(done)) + " could not be updated: " + failed + ".";
    status_time_ = 12;
}

void App::cmd_open() {
    if (!io_) { message("No game data", game_error_ + "\nSet the game folder in File > Settings > General."); return; }
    if (!confirm_close()) return;
    std::error_code ec;
    fs::create_directories(settings_.scenarios(), ec);
    std::wstring f = file_dialog(hwnd_, false, settings_.scenarios(), L"");
    if (!f.empty()) open_path(f);
}

void App::start_from_game(const fs::path& file) {
    from_game = true;
    from_game_opened = file;
    // Save into the folder the game that opened us lists (tests run the game on RECOMP_SCENARIO_DIR, which the editor inherits;
    // otherwise both use Scenarios.ini). Not written to the settings file.
    if (const wchar_t* d = _wgetenv(L"RECOMP_SCENARIO_DIR"); d && *d) settings_.scenario_folder = d;
    if (!file.empty()) open_path(file);
    status_ = "Opened from the game - File > Back to the game returns";
    status_time_ = 10;
}

fs::path App::doc_path() const {
    std::error_code ec;
    return doc_ && !doc_->file.empty() ? fs::absolute(doc_->file, ec) : fs::path();
}

void App::live_game_event(int state, const std::wstring& note) {
    if (state == 1) { status_ = "Starting the test in the game..."; status_time_ = 10; }
    else if (state == 2 || state == 0) { status_.clear(); status_time_ = 0; }
    else if (state == 3) message("Test", note.empty() ? std::string("The test could not start.") : narrow(note));
}

void App::game_ended() {
    from_game = false;
    from_game_live = false;
    message("MechAssault 2 closed", "The game has closed. The Scenario Editor carries on on its own: Test starts the game again, "
                                    "File > Exit closes the editor.");
}

void App::write_saved_list(const fs::path& list) const {
    std::error_code ec;
    std::string t = "opened=" + (from_game_opened.empty() ? std::string() : path_utf8(fs::absolute(from_game_opened, ec))) + "\r\n";
    for (auto& f : saved_files_) t += "saved=" + path_utf8(f) + "\r\n";
    if (exit_code_ == 2) t += "test=" + path_utf8(fs::absolute(test_file_, ec)) + "\r\n";  //the file the game plays
    // Written whole or not at all (the game reads it as soon as this process ends): a .tmp file, then replaced in one step.
    fs::create_directories(list.parent_path(), ec);
    fs::path tmp = list;
    tmp += L".tmp";
    if (write_file(tmp, t.data(), t.size())) MoveFileExW(tmp.c_str(), list.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void App::open_path(const fs::path& p) {
    if (!io_) return;
    auto s = std::make_unique<Scenario>();
    std::string err;
    std::vector<std::string> damaged;
    show_new_ = false;  // a dialog left open would hide the error below
    show_getting_started_ = false;
    if (!io_->load(p, *s, &err, &damaged)) {
        std::error_code gone;
        if (!fs::exists(p, gone)) { settings_.remove_recent(p); settings_.save(); }  // a deleted file does not stay in File > Open recent
        message("Cannot open scenario", "This file could not be opened as a scenario:\n" + path_utf8(p) + "\n\n(" + err +
                                            ")\n\nIt may be damaged, or made by a newer version of the editor.");
        return;
    }
    settings_.add_recent(p);
    const bool old_file = s->saved_with < kSaveRevision;
    // A copy made in Explorer keeps the Id of the original, and the game lists only one file per Id: this one gets its own
    // (Id 0: the next save mints one; its Scn<Id> asset names follow).
    std::string copy_of;
    if (s->id) {
        std::error_code ec;
        for (fs::directory_iterator it(settings_.scenarios(), ec), end; !ec && it != end && copy_of.empty(); it.increment(ec)) {
            std::wstring ext = it->path().extension().native();
            for (auto& c : ext) c = wchar_t(towlower(c));
            std::error_code e2;
            if (ext != L".ma2map" || fs::equivalent(it->path(), p, e2)) continue;
            const ScenarioHead* h = head_of(it->path());
            if (h && h->id == s->id) copy_of = path_utf8(it->path().filename());
        }
        if (!copy_of.empty()) s->id = 0;
    }
    set_document(std::move(s));
    status_ = "Opened " + path_utf8(p.filename());
    status_time_ = 4;
    if (old_file) {
        status_ += ". It was saved with an older editor: save it once to update it for the game.";
        status_time_ = 8;
    }
    if (!copy_of.empty()) {
        doc_->dirty = true;
        status_ = "This file is a copy of " + copy_of + ": saving it gives it its own entry in the game.";
        status_time_ = 10;
    }
    if (!damaged.empty()) {
        std::string names;
        for (auto& d : damaged) names += (names.empty() ? "" : ", ") + d;
        message("Damaged file", "This file is damaged: " + names + " could not be read and " + (damaged.size() == 1 ? "was" : "were") +
                                    " replaced by defaults.\n\nDo not save over it unless you accept the loss; the previous version may be in " +
                                    path_utf8(p.filename()) + ".bak.");
        status_ = "Opened " + path_utf8(p.filename()) + ". It is damaged (" + names + "): see the message.";
        status_time_ = 12;
    }
}

void App::script(const std::wstring& cmd, const std::wstring& arg) {
    std::string a = narrow(arg);
    if (cmd == L"--retail" && io_) {
        if (const MapInfo* m = gd_.find_map(a)) { show_new_ = false; open_retail(*m); }
    } else if (cmd == L"--new-copy" && io_) {  // developer checks: an editable copy of a real level (name[,mp])
        NewMapParams p;
        size_t comma = a.find(',');
        p.copy_map = a.substr(0, comma);
        p.multiplayer = comma != std::string::npos && a.substr(comma + 1) == "mp";
        p.start = NewMapParams::CopyRetail;
        p.name = p.copy_map + " copy";
        if (!p.multiplayer) p.types.clear();
        auto s = std::make_unique<Scenario>();
        std::string err;
        if (io_->create(p, *s, &err)) {
            show_new_ = false;
            set_document(std::move(s));
            ns_start();
            doc_->dirty = false;  // untouched, as the New dialog's Create
            status_ = "Made " + doc_->name + ", an editable copy of " + pretty_name(p.copy_map) + ".";
            status_time_ = 6;
        }
        else ma2::log_stderr("new-copy failed: " + err + "\n");
    } else if (cmd == L"--list-maps") {  // developer: FILE gets every retail map: name, multiplayer, archive present
        std::string rep;
        for (auto& m : gd_.maps()) rep += m.name + (m.multiplayer ? " mp " : " sp ") + (m.resource_present ? "present" : "absent") + "\n";
        write_file(fs::path(arg), rep.data(), rep.size());
    } else if (cmd == L"--brush") {  // developer checks: brush radius[,strength]
        float r = brush_radius_, k = brush_strength_;
        sscanf(a.c_str(), "%f,%f", &r, &k);
        brush_radius_ = std::clamp(r, 4.f, 300.f);
        brush_strength_ = std::clamp(k, 0.02f, 1.f);
        if (strength_slot(tool_) >= 0) settings_.tool_strength[strength_slot(tool_)] = brush_strength_;
    } else if (cmd == L"--full-upload") {  // developer: 1 = brush strokes upload the whole colour map / layer weights every frame (the old way), 0 = the touched rectangle
        paint_full_upload_ = atoi(a.c_str()) != 0;
    } else if (cmd == L"--list-ground-textures" && doc_) {  // developer: FILE gets every ground texture the picker offers ("archive|path")
        ensure_detail_textures();
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            for (auto& [p, ar] : detail_textures_) fprintf(f, "%s|%s\n", ar.c_str(), p.c_str());
            fclose(f);
        }
    } else if (cmd == L"--paint-demo" && doc_ && !doc_->colormap.px.empty()) {  // developer: "word,word,...[;seed]" adds the first picker texture matching each word
        // (extra layers 2..) and paints bold regions: the map is split among all the layers by nearest seed point with a soft edge.
        size_t sc = a.find(';');
        std::string words = a.substr(0, sc);
        unsigned seed = sc == std::string::npos ? 1u : unsigned(atoi(a.c_str() + sc + 1));
        ensure_detail_textures();
        size_t pos = 0;
        while (pos <= words.size()) {
            size_t e = words.find(',', pos);
            std::string w = lower(words.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
            pos = e == std::string::npos ? words.size() + 1 : e + 1;
            if (w.empty()) continue;
            bool hit = false;
            for (auto& [p, ar] : detail_textures_)
                if (lower(p).find(w) != std::string::npos && lower(p).find("micro") == std::string::npos) { hit = add_ground_texture(p, ar); break; }
            fprintf(stderr, "paint-demo: %s -> %s\n", w.c_str(), hit ? "added" : "no match");
        }
        GroundLayers& g = doc_->ground;
        if (g.enabled && g.count() >= 3) {
            std::vector<std::pair<float, float>> pts;  // seed points (0..1) per layer, deterministic
            uint32_t st = seed * 2654435761u + 12345u;
            auto rnd = [&]() { st = st * 1664525u + 1013904223u; return float(st >> 8) / 16777216.f; };
            for (int k = 0; k < g.count(); k++) pts.push_back({0.1f + 0.8f * rnd(), 0.1f + 0.8f * rnd()});
            float asp = g.h ? float(g.w) / float(g.h) : 1.f;
            uint8_t* d = g.mutable_weights();
            for (int y = 0; y < g.h; y++)
                for (int x = 0; x < g.w; x++) {
                    float u = (x + 0.5f) / g.w, v = (y + 0.5f) / g.h;
                    float dist[kGroundMaxLayers];
                    float best = 1e9f;
                    for (int k = 0; k < g.count(); k++) {
                        dist[k] = std::hypot((u - pts[size_t(k)].first) * asp, v - pts[size_t(k)].second);
                        best = std::min(best, dist[k]);
                    }
                    uint8_t* tx = d + (size_t(y) * g.w + x) * 8;
                    std::fill(tx, tx + 8, uint8_t(0));
                    float sum = 0;
                    float wgt[kGroundMaxLayers];
                    for (int k = 0; k < g.count(); k++) { wgt[k] = std::clamp(1.f - (dist[k] - best) / 0.06f, 0.f, 1.f); sum += wgt[k]; }
                    for (int k = 0; k < g.count(); k++) tx[k] = uint8_t(std::lround(255.f * wgt[k] / sum));
                }
            r_.set_ground_weights(g);
            doc_->dirty = true;
            minimap_dirty_ = true;
        }
    } else if (cmd == L"--place-tool" && doc_ && cat_) {  // developer proof: pick a palette entry (name substring) and switch to the Place tool; "road:<name>" arms a road piece
        std::string nm = lower(a);
        if (nm.rfind("road:", 0) == 0) {
            const std::string rn = nm.substr(5);
            for (auto& r : road_pieces())
                if (r.search.find(rn) != std::string::npos || lower(r.friendly).find(rn) != std::string::npos) {
                    place_road_ = r.path;
                    tool_ = Tool::PlaceObject;
                    fprintf(stderr, "place-tool: road %s\n", r.path.c_str());
                    break;
                }
        } else
        for (auto& t : cat_->all())
            if (t.placeable && !t.model.empty() && lower(t.name).find(nm) != std::string::npos) {
                place_template_ = t.uid;
                place_road_.clear();
                tool_ = Tool::PlaceObject;
                fprintf(stderr, "place-tool: %s (%u)\n", t.name.c_str(), t.uid);
                break;
            }
    } else if (cmd == L"--crop-census" && doc_ && io_ && cat_) {  // developer: every wall / bridge / plate template and road piece model through the save's cut test -> a tab-separated file
        FILE* f = fopen(a.c_str(), "wb");
        if (f) {
            std::map<int, std::pair<int, int>> count;  // family -> ok, refused
            std::map<std::string, int> reasons;
            auto row = [&](int fam, const std::string& name, const std::string& model, bool ok, const std::string& why) {
                fprintf(f, "%d\t%s\t%s\t%d\t%s\n", fam, name.c_str(), model.c_str(), ok ? 1 : 0, ok ? "" : why.c_str());
                (ok ? count[fam].first : count[fam].second)++;
                if (!ok) reasons[why]++;
            };
            std::set<std::string> seen;
            for (auto& t : cat_->all()) {
                if (!t.placeable || t.model.empty() || (t.cls_name != "MACorporeal" && t.cls_name != "MABuilding")) continue;
                const int fam = snap_family(t);
                if (fam < 1 || fam > 3) continue;
                if (!seen.insert(lower(t.name) + "|" + lower(t.model)).second) continue;
                std::string why;
                const bool ok = io_->model_cuttable(*doc_, t.model, &why);
                row(fam, t.name, t.model, ok, why);
            }
            for (auto& rp : road_pieces()) {
                if (rp.model.empty()) continue;
                std::string why;
                const bool ok = io_->model_cuttable(*doc_, rp.model, &why);
                row(4, rp.path, rp.model, ok, why);
            }
            for (auto& [fam, c] : count) fprintf(f, "# family %d: %d cuttable, %d refused\n", fam, c.first, c.second);
            for (auto& [w, n] : reasons) fprintf(f, "# refused %d x: %s\n", n, w.c_str());
            fclose(f);
        }
    } else if (cmd == L"--road-tool" && doc_) {  // developer proof: arm a road piece (substring of its search words) and the Place tool, like picking it in the palette
        const std::string nm = lower(a);
        for (auto& r : road_pieces())
            if (r.search.find(nm) != std::string::npos) {
                place_road_ = r.path;
                tool_ = Tool::PlaceObject;
                fprintf(stderr, "road-tool: %s\n", r.path.c_str());
                break;
            }
    } else if (cmd == L"--place-named" && doc_ && io_ && cat_) {  // developer proof: "palette name=metres ahead of the start" placed like a click (model bottom on the ground)
        std::string nm = lower(a.substr(0, a.find('=')));
        float d = 15;
        if (size_t eq = a.find('='); eq != std::string::npos) d = float(atof(a.c_str() + eq + 1));
        for (auto& t : cat_->all()) {
            if (!t.placeable || t.model.empty() || lower(t.name).find(nm) == std::string::npos) continue;
            {   // not the cinema-only geometry sets: the palette does not offer them and the game draws the failed-build pillar for them
                const std::string tn = lower(t.name), tm = lower(t.model);
                if (tn.find("cinemas only") != std::string::npos || tn.find("(geo)") != std::string::npos || tn.find("set only") != std::string::npos ||
                    tm.find("geo_only") != std::string::npos) continue;
            }
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    Vec3 p;
                    get_pos(st, p);
                    float h = st.f32("StartOrientation");
                    Vec3 at{p.x + std::sin(h) * d, 0, p.z + std::cos(h) * d};
                    at.y = doc_->terrain.height_at(at.x, at.z);
                    uint32_t u = io_->place_template(*doc_, t.uid, at, h + 3.14159f);
                    if (u) { lift_to_foot(u); world_changed(); dev_placed_.push_back(u); }
                    fprintf(stderr, "place-named: %s uid %u at %.1f %.1f %.1f\n", t.name.c_str(), u, at.x, at.y, at.z);
                    break;
                }
            break;
        }
    } else if (cmd == L"--tab") {
        tab_ = Tab(std::clamp(atoi(a.c_str()), 0, 9));
    } else if (cmd == L"--ui-scale") {  // developer / automation: this run only, never saved
        settings_.ui_scale_run = std::clamp(float(atof(a.c_str())), 0.5f, 3.f);
        ui_scale_dirty_ = true;
    } else if (cmd == L"--place-foreign" && doc_ && io_ && cat_) {  // developer stress: N distinct objects from other levels
        int n = atoi(a.c_str());
        const auto& base = cat_->culled_set(doc_->base_level());
        Vec3 c{doc_->terrain.size_x() * 0.5f, 0, doc_->terrain.size_z() * 0.5f};
        int k = 0;
        for (auto& t : cat_->all()) {
            if (k >= n) break;
            if (!t.placeable || t.model.empty() || base.count(t.uid) || t.cls_name == "MASky2" || io_->model_absent(*doc_, t.uid)) continue;
            Vec3 at{c.x + float(k % 20) * 25.f - 250.f, 0, c.z + float(k / 20) * 25.f - 250.f};
            at.y = doc_->terrain.height_at(at.x, at.z);
            if (io_->place_template(*doc_, t.uid, at, 0)) k++;
        }
        fprintf(stderr, "placed %d foreign objects\n", k);
    } else if (cmd == L"--set-boardable" && doc_ && io_) {  // developer checks: uid=0|1 ("Player can climb in")
        unsigned u = 0;
        int on = 1;
        sscanf(a.c_str(), "%u=%d", &u, &on);
        io_->set_boardable(*doc_, u, on != 0);
    } else if (cmd == L"--check" && doc_ && io_) {
        validation_ = check_now();
        show_validation_ = true;
    } else if (cmd == L"--prefs") {
        open_prefs_ = true;
    } else if (cmd == L"--dump-help") {  // developer check: the Help text with the current bindings, one topic after another, to FILE
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "wb")) {
            for (const HelpPage& p : help_topics(help_keys())) fprintf(f, "== %s\n%s\n\n", p.title.c_str(), p.body.c_str());
            fclose(f);
        }
    } else if (cmd == L"--topdown") {
        cam_.pitch = 1.5533f;
        cam_.yaw = 0;
        if (doc_) { cam_.tx = doc_->terrain.size_x() * 0.5f; cam_.tz = doc_->terrain.size_z() * 0.5f; cam_.dist = std::max(doc_->terrain.size_x(), doc_->terrain.size_z()) * 0.95f; }
    } else if (cmd == L"--cam") {  // x,z,dist,yawdeg,pitchdeg
        float x, z, d, y, p;
        if (sscanf(a.c_str(), "%f,%f,%f,%f,%f", &x, &z, &d, &y, &p) == 5) { cam_.tx = x; cam_.tz = z; cam_.dist = d; cam_.yaw = y / 57.2958f; cam_.pitch = p / 57.2958f; }
    } else if (cmd == L"--sky-fog-shift") {  // developer pairs: metres added to the sky-fog height (fit the game's dome fog band)
        g_sky_fog_shift = float(atof(a.c_str()));
    } else if (cmd == L"--sky-heading-add") {  // developer pairs: radians added to the sky dome heading (is the game turning the dome by CorporalHeading?)
        g_sky_heading_add = float(atof(a.c_str()));
    } else if (cmd == L"--sky-fog") {  // developer pairs: 0 forces the sky fog off, 1 on, -1 the level's own
        g_sky_fog_force = atoi(a.c_str());
    } else if (cmd == L"--stage") {  // developer: shader stage dump N (MA2SE_WATER_DEBUG numbering), -1 = normal
        g_dbg_w = 1.f + float(atoi(a.c_str()));
    } else if (cmd == L"--cam-eye") {  // developer pairs: the game camera from a frame trace (eye x,y,z, forward x,y,z[, vertical fov deg]); holds the height
        float e[3], f[3], fv = 0;
        if (sscanf(a.c_str(), "%f,%f,%f,%f,%f,%f,%f", &e[0], &e[1], &e[2], &f[0], &f[1], &f[2], &fv) >= 6) {
            float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            for (float& v : f) v /= fl;
            cam_.dist = 15;
            cam_.tx = e[0] + f[0] * 15; cam_.ty = e[1] + f[1] * 15; cam_.tz = e[2] + f[2] * 15;
            cam_.yaw = std::atan2(f[0], f[2]);
            cam_.pitch = -std::asin(f[1]);
            if (fv > 0) cam_.fov = fv / 57.2958f;
            g_cam_hold = true;
        }
    } else if (cmd == L"--cam-start" && doc_) {  // developer checks: behind the player start like the game camera (dist,pitchdeg[,x,z])
        float d = 30, p = 14, x = -1, z = -1;
        sscanf(a.c_str(), "%f,%f,%f,%f", &d, &p, &x, &z);
        for (auto& st : doc_->world.objs)
            if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                Vec3 s;
                get_pos(st, s);
                float h = st.f32("StartOrientation");
                if (x >= 0 && z >= 0) { h = std::atan2(doc_->terrain.size_x() * 0.5f - x, doc_->terrain.size_z() * 0.5f - z); s = {x, 0, z}; }
                cam_.tx = s.x + std::sin(h) * d * 0.5f;
                cam_.tz = s.z + std::cos(h) * d * 0.5f;
                cam_.dist = d;
                cam_.yaw = h;
                cam_.pitch = p / 57.2958f;
                break;
            }
    } else if (cmd == L"--select") {
        selection_ = {uint32_t(strtoul(a.c_str(), nullptr, 10))};
    } else if (cmd == L"--select-class" && doc_) {
        // The first object of a class ("MAMech"), for checks that need a selection.
        uint32_t k = key_of(a);
        for (auto& o : doc_->world.objs) if (o.cls == k) { selection_ = {o.uid()}; break; }
    } else if (cmd == L"--select-class-all" && doc_) {  // every object of a class (checks)
        uint32_t k = key_of(a);
        selection_.clear();
        for (auto& o : doc_->world.objs) if (o.cls == k) selection_.insert(o.uid());
    } else if (cmd == L"--select-placed") {  // checks: the units placed with --place
        selection_.clear();
        for (uint32_t u : dev_placed_) selection_.insert(u);
    } else if (cmd == L"--overlay") {
        show_typemap_ = true;
        overlay_dirty_ = true;
    } else if (cmd == L"--dump-palette" && doc_) {  // developer check: palette names / groups / cut verdicts
        dump_palette(path_utf8(fs::path(arg)));
    } else if (cmd == L"--dump-search" && doc_) {  // developer: "word|FILE" -> palette entries that word finds, with their groups
        size_t bar = a.find('|');
        if (bar != std::string::npos) dump_search(a.substr(0, bar), path_utf8(fs::path(a.substr(bar + 1))));
    } else if (cmd == L"--dump-sweep") {  // developer: what the retail data holds that the palette could offer
        dump_sweep(path_utf8(fs::path(arg)));
    } else if (cmd == L"--dump-start" && doc_) {  // developer checks: uid, start state (objects_at_start), class, position of every placed object
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            WTree t;
            t.build(doc_->world);
            std::vector<char> st = objects_at_start(doc_->world, t);
            for (size_t i = 0; i < doc_->world.objs.size(); i++) {
                const WObj& o = doc_->world.objs[i];
                Vec3 p;
                if (get_pos(o, p)) fprintf(f, "%u %d %08X %.1f %.1f %.1f\n", o.uid(), int(st[i]), o.cls, p.x, p.y, p.z);
            }
            fclose(f);
        }
    } else if (cmd == L"--show-later") {  // developer checks: View > Objects the mission shows later on
        show_later_ = true;
        objects_dirty_ = true;
    } else if (cmd == L"--no-helpers") {  // developer shots: hide helpers (View > Helpers)
        show_helpers_ = false;
        objects_dirty_ = true;
    } else if (cmd == L"--wave-up") {  // developer checks: water normal channel order (0 blue, 1 green, 2 r/g swapped)
        r_.params.wave_up = _wtoi(arg.c_str());
    } else if (cmd == L"--dump-boxes") {  // developer checks: which classes still draw as placeholder boxes
        dump_boxes_ = path_utf8(fs::path(arg));
    } else if (cmd == L"--dump-objects" && doc_) {  // developer checks: class, model, position, ShadowReceiverType of every placed object
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            for (auto& o : doc_->world.objs) {
                Vec3 p;
                if (!get_pos(o, p)) continue;
                std::string m = o.str("Corporal3DObj");
                if (m.empty()) continue;
                fprintf(f, "%08X uid %u %.1f %.1f %.1f hd %.1f pt %.1f rl %.1f recv %d road %d model %s\n", o.cls, o.uid(), p.x, p.y, p.z, o.f32("CorporalHeading"), o.f32("CorporalPitch"), o.f32("CorporalRoll"), o.i32("ShadowReceiverType"), o.i32("IsRoadOrTarmac"), m.c_str());
            }
            fclose(f);
        }
    } else if (cmd == L"--dump-effect-templates" && cat_) {  // developer check: every MAEffect template (uid, category, name, effect file)
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            for (auto& t : cat_->all()) {
                if (t.cls_name != "MAEffect") continue;
                const WObj* o = cat_->object(t.uid);
                fprintf(f, "%u\t%s\t%s\t%s\t%d\n", t.uid, t.category.c_str(), t.name.c_str(), o ? o->str("EffectFile").c_str() : "", t.placeable ? 1 : 0);
            }
            fclose(f);
        }
    } else if (cmd == L"--dump-effects") {  // developer checks: the .mtb particle effects of the placed objects
        dump_effects_ = path_utf8(fs::path(arg));
    } else if (cmd == L"--bloom") {  // developer shots: View > Bloom on (1, default) or off (0); 2 / 3 show the bloom / its mask alone
        r_.params.bloom = arg != L"0";
        g_bloom_dbg = arg == L"2" ? 1 : arg == L"3" ? 2 : 0;  // 2: bloom alone, 3: its mask
    } else if (cmd == L"--fade-blockers") {  // developer shots: View > Walls that block the camera on (1, default) or off (0)
        fade_blockers_ = arg != L"0";
    } else if (cmd == L"--grid") {  // developer shots: View > Grid on (1, default) or off (0): the white net-like lines over the terrain on the sweep shots
        r_.params.grid = arg != L"0";
    } else if (cmd == L"--effects") {  // developer shots: View > Effects on (1, default) or off (0)
        show_effects_ = arg != L"0";
    } else if (cmd == L"--dump-file" && doc_) {  // developer: archive path,out file (a game file read from the open level's archives)
        size_t comma = a.find(',');
        std::vector<uint8_t> b;
        if (comma != std::string::npos && models_.archives() && models_.archives()->Read(a.substr(0, comma), b)) {
            FILE* f = fopen(a.substr(comma + 1).c_str(), "wb");
            if (f) { fwrite(b.data(), 1, b.size(), f); fclose(f); }
            status_ = f ? "Dumped " + std::to_string(b.size()) + " bytes to " + a.substr(comma + 1) : "dump-file: cannot open " + a.substr(comma + 1);
        } else status_ = "dump-file: not found or no archives";
        status_time_ = 60;
    } else if (cmd == L"--dump-texture" && doc_) {  // developer: archive path,out file: the decoded level 0 as int32 w,h + RGBA bytes (the game trace dump format)
        size_t comma = a.find(',');
        std::vector<uint8_t> b;
        ma2::Mgic mg;
        ma2::Picture pic;
        ma2::Image img;
        if (comma != std::string::npos && models_.archives() && models_.archives()->Read(a.substr(0, comma), b) && mg.parse(b) && ma2::read_picture(mg, pic) && ma2::decode_picture(pic, 0, img)) {
            if (FILE* f = fopen(a.substr(comma + 1).c_str(), "wb")) {
                int32_t wh[2] = {img.w, img.h};
                fwrite(wh, 4, 2, f);
                fwrite(img.px.data(), 4, img.px.size(), f);  // R in the low byte = RGBA byte order
                fclose(f);
                status_ = "Decoded " + std::to_string(img.w) + "x" + std::to_string(img.h);
            }
        } else status_ = "dump-texture: cannot read or decode";
        status_time_ = 60;
    } else if (cmd == L"--dump-pickable" && doc_) {  // developer audit: can each object be picked in the viewport? one line per object: uid class pos status description
        const bool sl = show_later_, sh = show_helpers_;
        bool sg[HG_COUNT];
        for (int g = 0; g < HG_COUNT; g++) { sg[g] = show_helper_group_[g]; show_helper_group_[g] = true; }
        show_later_ = true; show_helpers_ = true;
        rebuild_objects();
        std::set<int> all_on, own_all;
        for (auto& v : views_) { all_on.insert(v.root); own_all.insert(v.index); }
        show_later_ = sl; show_helpers_ = sh;
        for (int g = 0; g < HG_COUNT; g++) show_helper_group_[g] = sg[g];
        rebuild_objects();
        std::set<int> now_roots, own_now;
        for (auto& v : views_) { now_roots.insert(v.root); own_now.insert(v.index); }
        WTree t;
        t.build(doc_->world);
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            const auto& W = doc_->world.objs;
            for (size_t i = 0; i < W.size(); i++) {
                const WObj& o = W[i];
                Vec3 p{0, 0, 0};
                bool hp = get_pos(o, p);
                const char* st;
                if (own_now.count(int(i))) st = "PICK";                  // drawn with its own box/model and picked by it
                else if (own_all.count(int(i))) st = "HIDDEN_BY_VIEW";   // View > Objects the mission shows later / helper groups
                else if (!hp) st = "NOPOS";                              // container or position-less (reachable through the outline or Objectives & Triggers tab)
                else if (o.cls == K_SKY) st = "SKY";
                else st = "HIDDEN_OTHER";                                // compiled editor-trigger group or game-type filter
                int par = t.parent_of[i];
                float ext[6] = {0, 0, 0, 0, 0, 0};
                for (auto& v : views_) if (v.index == int(i)) { for (int k = 0; k < 3; k++) { ext[k] = v.lo[k]; ext[3 + k] = v.hi[k]; } break; }
                fprintf(f, "%s %u %08X %.1f %.1f %.1f parent %d box %.1f,%.1f,%.1f..%.1f,%.1f,%.1f model %s desc %s\n", st, o.uid(), o.cls, p.x, p.y, p.z, par >= 0 ? int(W[size_t(par)].uid()) : -1, ext[0], ext[1], ext[2], ext[3], ext[4], ext[5], o.str("Corporal3DObj").c_str(), o.description().c_str());
            }
            fclose(f);
        }
    } else if (cmd == L"--dump-lights") {  // developer checks: write the Scene lights list (implies --scene-lights)
        r_.params.scene_lights = true;
        dump_lights_ = path_utf8(fs::path(arg));
    } else if (cmd == L"--fog") {  // developer shots: View > Fog on
        r_.params.fog = true;
    } else if (cmd == L"--lightning-hold") {  // developer pairs: hold the lightning flash on (1 models, 2 sky, 3 both, 0 off) while lightning is on
        lightning_hold_ = _wtoi(arg.c_str());
    } else if (cmd == L"--weather") {  // developer shots: View > Weather on (1, default) or off (0)
        show_weather_ = arg != L"0";
    } else if (cmd == L"--culture") {  // developer shots: View > Ground scatter on (1, default) or off (0)
        show_culture_ = arg != L"0";
    } else if (cmd == L"--scene-lights") {  // developer shots: View > Scene lights on (not saved)
        r_.params.scene_lights = true;
    } else if (cmd == L"--no-dialog") {
        show_new_ = false;
    } else if (cmd == L"--models-all" && doc_) {
        rebuild_objects();
        models_.pump(1e9);
        objects_dirty_ = true;
    } else if (cmd == L"--trigger") {  // developer shots: open the Objectives & Triggers tab on the trigger with this UID
        trig_uid_ = uint32_t(strtoul(a.c_str(), nullptr, 10));
        tab_ = Tab::Triggers;
    } else if (cmd == L"--drop-trigger" && doc_) {  // developer bisects: remove a trigger by name
        uint32_t victim = 0;
        for (auto& o : doc_->world.objs) if (o.description() == a && gd_.names().label(o.cls).rfind("MATrigger", 0) == 0) { victim = o.uid(); break; }
        if (victim) { wedit::remove_subtree(doc_->world, victim); world_changed(); }
    } else if (cmd == L"--unit-side" && doc_) {  // developer bisects: uid=nation
        unsigned u = 0;
        int n = 0;
        if (sscanf(a.c_str(), "%u=%d", &u, &n) == 2)
            for (auto& o : doc_->world.objs) if (o.uid() == u) set_unit_side(o, n);
    } else if (cmd == L"--place-before-start" && doc_) {  // developer checks: uid=metres, in front of the player start
        unsigned u = 0;
        float d = 8;
        if (sscanf(a.c_str(), "%u=%f", &u, &d) >= 1)
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    Vec3 p;
                    get_pos(st, p);
                    float h = st.f32("StartOrientation");  // radians; forward = (sin h, cos h)
                    Vec3 at{p.x + std::sin(h) * d, 0, p.z + std::cos(h) * d};
                    at.y = doc_->terrain.height_at(at.x, at.z);
                    ScenarioIO::move_subtree_to(*doc_, u, at, h, true, nullptr);
                    break;
                }
    } else if ((cmd == L"--place" || cmd == L"--place-side") && doc_ && io_) {  // developer checks: template=metres[@side][/sideways]
        unsigned t = 0;
        float d = 10, x = 0;
        int side = -1;
        sscanf(a.c_str(), "%u=%f", &t, &d);
        if (size_t at = a.find('@'); at != std::string::npos) side = atoi(a.c_str() + at + 1);
        if (size_t sl = a.find('/'); sl != std::string::npos) x = float(atof(a.c_str() + sl + 1));
        for (auto& st : doc_->world.objs)
            if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                Vec3 p;
                get_pos(st, p);
                float h = st.f32("StartOrientation");
                Vec3 at{p.x + std::sin(h) * d + std::cos(h) * x, 0, p.z + std::cos(h) * d - std::sin(h) * x};
                at.y = doc_->terrain.height_at(at.x, at.z);
                uint32_t u = io_->place_template(*doc_, t, at, h + 3.14159f);
                if (u) dev_placed_.push_back(u);
                if (u && side >= 0) io_->apply_unit_side(*doc_, u, side);
                if (size_t hash = a.find('#'); u && hash != std::string::npos)  // #model: instance model with OverrideTemplate=1
                    for (auto& o : doc_->world.objs)
                        if (o.uid() == u) {
                            o.set_str("Corporal3DObj", a.substr(hash + 1));
                            if (WProp* ov = o.find("OverrideTemplate")) ov->set_int(1);
                            else o.set_i32("OverrideTemplate", 1);
                        }
                break;
            }
    } else if (cmd == L"--add-reinforce" && doc_) {  // developer checks: dist,radius,side[,hide]: a circle in front of the start
        float d = 60, r = 30;                             // brings in the units placed with --place
        int side = 5, hide = 1;
        sscanf(a.c_str(), "%f,%f,%d,%d", &d, &r, &side, &hide);
        for (auto& st : doc_->world.objs)
            if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                Vec3 p;
                get_pos(st, p);
                float h = st.f32("StartOrientation");
                EdTrigger t;
                t.name = "Reinforcements";
                char pt[64];
                snprintf(pt, sizeof pt, "%.1f,%.1f", p.x + std::sin(h) * d, p.z + std::cos(h) * d);
                t.conditions.push_back({"area", {{"point", pt}, {"radius", std::to_string(r)}, {"inside", "1"}}});
                std::string units;
                for (uint32_t u : dev_placed_) units += (units.empty() ? "" : ",") + std::to_string(u);
                t.effects.push_back({"reinforce", {{"units", units}, {"nation", std::to_string(side)}, {"hide", std::to_string(hide)}}});
                doc_->triggers.push_back(std::move(t));
                convert_legacy_triggers(*doc_);
                world_changed();
                doc_->dirty = true;
                break;
            }
    } else if (cmd == L"--add-patrol" && doc_) {  // developer proof: "d,x;d,x;..." the last --place'd unit patrols the points d m ahead / x m sideways of the start (starts with the mission)
        if (!dev_placed_.empty())
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    Vec3 p;
                    get_pos(st, p);
                    float h = st.f32("StartOrientation");
                    uint32_t unit = dev_placed_.back();
                    uint32_t trig = add_trigger_object(*doc_, "MATriggerTimer", "Patrol proof");
                    uint32_t ev = trig ? add_order(*doc_, trig, unit, "MAPatrol") : 0;
                    if (ev) add_action_object(*doc_, trig, "MAActionBroadcast", "Patrol proof message");  // on screen when the trigger fires
                    int n = 0;
                    bool once = a.compare(0, 5, "once;") == 0;  // "once;d,x;..." = walk the route once (TraverseType 1)
                    for (auto& pt : split(once ? a.substr(5) : a, ';')) {
                        float d = 0, x = 0;
                        if (ev && sscanf(pt.c_str(), "%f,%f", &d, &x) == 2) {
                            order_add_point(*doc_, ev, p.x + std::sin(h) * d + std::cos(h) * x, p.z + std::cos(h) * d - std::sin(h) * x);
                            n++;
                        }
                    }
                    if (once && ev)
                        if (WObj* go = object_by_uid(doc_->world, order_path(doc_->world, ev))) go->set_i32("TraverseType", 1);
                    fprintf(stderr, "add-patrol: unit %u trigger %u order %u points %d\n", unit, trig, ev, n);
                    world_changed();
                    doc_->dirty = true;
                    break;
                }
    } else if (cmd == L"--add-order" && doc_) {  // developer proof: "attack" (the player) | "attack:prev" (the unit --place'd before it) | "defend:d,x" | "follow" for the last --place'd unit
        if (!dev_placed_.empty())
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    Vec3 p;
                    get_pos(st, p);
                    float h = st.f32("StartOrientation");
                    std::string kind = a.substr(0, a.find(':')), arg = a.find(':') == std::string::npos ? std::string() : a.substr(a.find(':') + 1);
                    const char* cls = kind == "defend" ? "MADefend" : kind == "follow" ? "MAFollow" : "MAAttack";
                    uint32_t unit = dev_placed_.back();
                    uint32_t trig = add_trigger_object(*doc_, "MATriggerTimer", "Order proof");
                    uint32_t ev = trig ? add_order(*doc_, trig, unit, cls) : 0;
                    if (ev) add_action_object(*doc_, trig, "MAActionBroadcast", "Order proof message");  // on screen when the trigger fires
                    uint32_t beh = ev ? order_behaviour(doc_->world, ev) : 0;
                    float d = 0, x = 0, r = 0;
                    if (beh && kind == "defend" && sscanf(arg.c_str(), "%f,%f,%f", &d, &x, &r) >= 2) {  // "defend:d,x[,radius]"; a trailing w = the chooser's walk-and-guard
                        const float wx = p.x + std::sin(h) * d + std::cos(h) * x, wz = p.z + std::cos(h) * d - std::sin(h) * x;
                        if (r > 0)
                            for (auto& o : doc_->world.objs) if (o.uid() == beh) o.set_f32("Radius", r);
                        if (arg.find('w') != std::string::npos) {
                            make_defend_walk(beh, wx, wz, "Order proof - defend point");
                            uint32_t circ = 0;
                            for (auto& o : doc_->world.objs)
                                if (o.description().find("reaches its defend point") != std::string::npos) circ = o.uid();
                            if (circ) add_action_object(*doc_, circ, "MAActionBroadcast", "Arrived proof message");  // on screen when the unit gets there
                        } else {
                            set_marker_link(*doc_, beh, "DefendUID", wx, wz, true, "Order proof - defend point");
                        }
                    }
                    for (auto& o : doc_->world.objs)
                        if (beh && kind == "attack" && o.uid() == beh) {
                            if (arg.compare(0, 4, "prev") == 0 && dev_placed_.size() > 1) {
                                o.set_u32("AttackUID", dev_placed_[dev_placed_.size() - 2]);
                                o.set_u32("AttackPlayer", 1);
                            }
                            if (size_t at = arg.find('@'); at != std::string::npos) o.set_f32("AttackTimeout", float(atof(arg.c_str() + at + 1)));  // "...@s": give up after s seconds
                            fprintf(stderr, "add-order: AttackTimeout %.1f\n", o.f32("AttackTimeout"));
                        }
                    fprintf(stderr, "add-order: %s unit %u trigger %u order %u\n", cls, unit, trig, ev);
                    world_changed();
                    doc_->dirty = true;
                    break;
                }
    } else if (cmd == L"--add-swap" && doc_) {  // developer proof: "secs,configUid": after secs the player becomes that player configuration (a timer + a Put the player in a vehicle action + a message)
        float sec = 10;
        unsigned cfg = 0;
        sscanf(a.c_str(), "%f,%u", &sec, &cfg);
        uint32_t trig = add_trigger_object(*doc_, "MATriggerTimer", "Swap proof");
        if (WObj* t = trig ? object_by_uid(doc_->world, trig) : nullptr) {
            t->set_f32("TrgTimerDurationSeconds", sec);
            t->set_f32("TrgTimerDuration", sec / 60.f);
        }
        uint32_t ac = trig && io_ ? io_->add_part(*doc_, key_of("MAActionChangePlayerVehicle"), trig, nullptr) : 0;
        if (WObj* o = ac ? object_by_uid(doc_->world, ac) : nullptr) {
            o->set_u32("ConfigTemplateUID", cfg);
            o->set_i32("PlayerNum", 0);
            o->set_i32("UseHeading", 1);
            o->set_i32("SetAsDefaultConfig", 0);
        }
        if (trig) add_action_object(*doc_, trig, "MAActionBroadcast", "Swap proof message");
        world_changed();
        doc_->dirty = true;
    } else if (cmd == L"--add-box-msg" && doc_) {  // developer checks: name|dist|base|height|width|length|text: a message when the player is in a box at dist m ahead of the start
        auto f = split(a, '|');
        if (f.size() >= 7)
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    Vec3 p;
                    get_pos(st, p);
                    float h = st.f32("StartOrientation"), d = float(atof(f[1].c_str()));
                    EdTrigger t;
                    t.name = f[0];
                    char pt[64];
                    snprintf(pt, sizeof pt, "%.1f,%.1f", p.x + std::sin(h) * d, p.z + std::cos(h) * d);
                    if (f[1].find(':') != std::string::npos) snprintf(pt, sizeof pt, "%s", f[1].replace(f[1].find(':'), 1, ",").c_str());  // "x:z" = absolute
                    if (f[0].rfind("D:", 0) == 0) {  // name "D:Display@uid[@percent]" = that unit is destroyed (or damaged by percent)
                        auto g = split(f[0].substr(2), '@');
                        t.name = g[0];
                        if (g.size() > 2) t.conditions.push_back({"damaged", {{"units", g[1]}, {"percent", g[2]}}});
                        else if (g.size() > 1) t.conditions.push_back({"destroyed", {{"units", g[1]}, {"all", "1"}, {"jacking", "1"}}});
                    } else if (f[0].rfind("C:", 0) == 0) {  // name "C:Display@Target" = fires after the trigger called Target has fired (a chain)
                        size_t at = f[0].find('@');
                        t.name = f[0].substr(2, at == std::string::npos ? std::string::npos : at - 2);
                        t.conditions.push_back({"trigger", {{"trigger", at == std::string::npos ? std::string() : f[0].substr(at + 1)}}});
                    } else if (f[0].rfind("T:", 0) == 0)  // name "T:..." = a timer condition (seconds = width)
                        t.conditions.push_back({"timer", {{"seconds", f[4]}}});
                    else if (f[0].rfind("R:", 0) == 0)  // name "R:..." = a circle (area) condition of radius = width instead of a box
                        t.conditions.push_back({"area", {{"point", pt}, {"radius", f[4]}, {"inside", "1"}}});
                    else
                        t.conditions.push_back({"box", {{"point", pt}, {"base", f[2]}, {"height", f[3]}, {"width", f[4]}, {"length", f[5]}, {"heading", "0"}}});
                    if (f[6].rfind("cin:", 0) == 0) {  // "cin:UID" = play that cutscene (MACinema uid)
                        t.effects.push_back({"cinema", {{"cinema", f[6].substr(4)}}});
                    } else if (f[6].rfind("tp:", 0) == 0) {  // "tp:x:z[:heading]" = move the player there (retail MP triggers use teleports)
                        auto g = split(f[6].substr(3), ':');
                        if (g.size() >= 2) t.effects.push_back({"teleport", {{"point", g[0] + "," + g[1]}, {"heading", g.size() > 2 ? g[2] : "0"}}});
                    } else {
                        t.effects.push_back({"message", {{"text", f[6]}, {"seconds", "30"}}});
                    }
                    doc_->triggers.push_back(std::move(t));
                    convert_legacy_triggers(*doc_);  // the box becomes a MATriggerCuboidalVolume on the map
                    doc_->dirty = true;
                    world_changed();
                    break;
                }
    } else if (cmd == L"--add-shot" && doc_) {  // developer probes: uid of a MACinema; appends a shot (copy of the last), like the Add shot button
        add_cutscene_shot(uint32_t(strtoul(a.c_str(), nullptr, 10)));
    } else if (cmd == L"--set-prop" && doc_) {  // developer probes: uid;Key;int sets one integer property of an object (e.g. a trigger's TypeOfCheck)
        auto f = split(a, ';');
        if (f.size() == 3)
            for (auto& o : doc_->world.objs)
                if (o.uid() == uint32_t(strtoul(f[0].c_str(), nullptr, 10))) {
                    o.set_i32(f[1].c_str(), atoi(f[2].c_str()));
                    doc_->dirty = true;
                    world_changed();
                    break;
                }
    } else if (cmd == L"--board-audit" && io_ && cat_) {  // developer check: which unit templates have an owner controller
        std::string out;
        for (auto& t : cat_->all())
            if (t.placeable && (t.cls_name == "MAMech" || t.cls_name == "MATank" || t.cls_name == "MAVehicle" || t.cls_name == "MAFlyer"))
                out += std::to_string(t.uid) + "\t" + t.cls_name + "\t" + t.name + "\t" + (io_->can_board(t.uid) ? "board" : "no") + "\n";
        write_file(fs::path(arg), out.data(), out.size());
    } else if (cmd == L"--place-at-spawns" && doc_ && io_) {  // developer checks: template=metres@side in front of every spawn
        unsigned t = 0;
        float d = 10;
        int side = 0;
        sscanf(a.c_str(), "%u=%f", &t, &d);
        if (size_t at = a.find('@'); at != std::string::npos) side = atoi(a.c_str() + at + 1);
        std::vector<std::pair<Vec3, float>> spots;
        for (auto& st : doc_->world.objs)
            if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                Vec3 p;
                get_pos(st, p);
                spots.push_back({p, st.f32("StartOrientation")});
            }
        for (auto& [p, h] : spots) {
            Vec3 at{p.x + std::sin(h) * d, 0, p.z + std::cos(h) * d};
            at.y = doc_->terrain.height_at(at.x, at.z);
            if (uint32_t u = io_->place_template(*doc_, t, at, h + 3.14159f)) io_->apply_unit_side(*doc_, u, side);
        }
    } else if (cmd == L"--place-all-units" && doc_ && io_ && cat_) {  // developer sweep: every placeable unit in rings around the start
        int side = a.empty() ? 0 : atoi(a.c_str());
        models_.set_archives(gd_.game_files(), doc_->base, gd_.level_archives());
        std::vector<const TemplateInfo*> units;
        for (auto& t : cat_->all())
            if (t.placeable && !t.model.empty() && (t.cls_name == "MAMech" || t.cls_name == "MATank" || t.cls_name == "MAVehicle") &&
                !istarts_with(t.name, "Set") && !ma2m::ResolveModelPath(*models_.archives(), t.model).empty())
                units.push_back(&t);
        Vec3 c{};
        for (auto& st : doc_->world.objs) if (st.uid() == player_start_uid(doc_->world)) { get_pos(st, c); break; }
        std::string list;
        size_t i = 0;
        for (float r = 70; i < units.size(); r += 35) {
            int n = std::max(8, int(6.2832f * r / 16.f));
            for (int k = 0; k < n && i < units.size(); k++, i++) {
                float ang = 6.2832f * (k + 0.5f * (int(r) % 2)) / n;
                Vec3 at{c.x + std::sin(ang) * r, 0, c.z + std::cos(ang) * r};
                at.y = doc_->terrain.height_at(at.x, at.z);
                uint32_t u = io_->place_template(*doc_, units[i]->uid, at, ang + 3.14159f);
                if (u) io_->apply_unit_side(*doc_, u, side);
                char line[256];
                snprintf(line, sizeof line, "%u\t%s\t%s\tr=%.0f\tdeg=%.0f\n", units[i]->uid, units[i]->cls_name.c_str(), units[i]->name.c_str(), r, ang * 57.2958f);
                list += line;
            }
        }
        fprintf(stderr, "%s", list.c_str());
        write_file(fs::path(L"place-all-units.txt"), list.data(), list.size());
    } else if (cmd == L"--start-in" && doc_ && io_) {  // developer checks: player start container uid (or a choice label)
        uint32_t c = uint32_t(strtoul(a.c_str(), nullptr, 10));
        for (auto& ch : io_->start_choices()) if (iequals(ch.label, a)) c = ch.container;
        if (c) io_->set_start_container(*doc_, c);
    } else if (cmd == L"--delete" && doc_) {  // developer bisects: uid,uid,... (each with its subtree)
        for (auto& t : split(a, ',')) wedit::remove_subtree(doc_->world, uint32_t(strtoul(t.c_str(), nullptr, 10)));
        world_changed();  // the drawn objects, picking and roof hits drop them at once (a stale deck caught placements)
    } else if (cmd == L"--move" && doc_) {  // developer: uid=x,z moves an object (keeps its height above the ground)
        size_t eq = a.find('=');
        auto xz = eq == std::string::npos ? std::vector<std::string>() : split(a.substr(eq + 1), ',');
        if (xz.size() == 2) {
            uint32_t uid = uint32_t(strtoul(a.substr(0, eq).c_str(), nullptr, 10));
            for (auto& o : doc_->world.objs) {
                Vec3 p;
                if (o.uid() != uid || !get_pos(o, p)) continue;
                float nx = float(atof(xz[0].c_str())), nz = float(atof(xz[1].c_str()));
                const float above = above_surface(o);
                set_pos(o, {nx, p.y, nz});
                put_on_surface(uid, above);
                objects_dirty_ = true;
                break;
            }
        }
    } else if (cmd == L"--lift" && doc_) {  // developer: uid=metres raises an object (a landed drop ship hovering, for the thruster check)
        size_t eq = a.find('=');
        if (eq != std::string::npos) {
            uint32_t uid = uint32_t(strtoul(a.substr(0, eq).c_str(), nullptr, 10));
            for (auto& o : doc_->world.objs) {
                Vec3 p;
                if (o.uid() != uid || !get_pos(o, p)) continue;
                set_pos(o, {p.x, p.y + float(atof(a.substr(eq + 1).c_str())), p.z});
                objects_dirty_ = true;
                break;
            }
        }
    } else if (cmd == L"--dump-fog" && doc_) {  // developer pairs: the fog / sky fog properties of every object that has them
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            static const char* names[] = {"FogOn", "FogMode", "FogStart", "FogEnd", "FogTop", "FogBottom", "FogColor", "SkyFogOn", "SkyFogColor",
                                          "SkyFogStart", "SkyFogEnd", "SkyFogTop", "SkyFogBottom", "SkyFogChangeDuration", "SkyTerrainSkyScale", "SkyTerrainSkyOffset", "CorporalPos", "TerrainDetailBlend", "TerrainDetailModulate", "TerrainDetailMult", "TerrainDetailMult1", "LightAmbientColor", "DirectionalLightOn", "DirectionalLightColor", "DirectionalLightDirection", "IsNightTime"};
            for (auto& o : doc_->world.objs) {
                std::string line;
                for (const char* n : names)
                    if (const WProp* p = o.find(n)) line += std::string(" ") + n + "=" + p->display();
                if (!line.empty()) fprintf(f, "%08X uid %u%s\n", o.cls, o.uid(), line.c_str());
            }
            fclose(f);
        }
    } else if (cmd == L"--dump-maps" && doc_) {  // developer: writes FILE.color.rgba / FILE.light.rgba (int32 w,h then w*h RGBA bytes)
        std::string s = path_utf8(fs::path(arg));
        for (int k = 0; k < 2; k++) {
            const Image& im = k ? doc_->lightmap : doc_->colormap;
            if (im.px.empty()) continue;
            if (FILE* f = fopen((s + (k ? ".light.rgba" : ".color.rgba")).c_str(), "wb")) {
                fwrite(&im.w, 4, 1, f); fwrite(&im.h, 4, 1, f);
                fwrite(im.px.data(), 4, im.px.size(), f);
                fclose(f);
            }
        }
    } else if (cmd == L"--dump-terrain-at" && doc_) {  // developer pairs: "x,z;file" height, normal, N.L, colour map, light map at a world point
        float x = 0, z = 0;
        char fn[512] = {};
        std::string s = path_utf8(fs::path(arg));
        if (sscanf(s.c_str(), "%f,%f;%511[^\n]", &x, &z, fn) == 3)
            if (FILE* f = fopen(fn, "a")) {
                const auto& t = doc_->terrain;
                float e = t.scale, hx = t.height_at(x + e, z) - t.height_at(x - e, z), hz = t.height_at(x, z + e) - t.height_at(x, z - e);
                float n[3] = {-hx, 2 * e, -hz}, nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (float& v : n) v /= nl;
                const auto& d = r_.params.sun_dir;
                float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                float ndl = dl > 0 ? -(n[0] * d.x + n[1] * d.y + n[2] * d.z) / dl : 0;
                auto texel = [&](const ma2::Image& im) -> uint32_t {
                    if (im.px.empty()) return 0;
                    int u = std::clamp(int(x / t.size_x() * im.w), 0, im.w - 1), v = std::clamp(int((1 - z / t.size_z()) * im.h), 0, im.h - 1);
                    return im.at(u, v);
                };
                fprintf(f, "at %.1f %.1f h %.2f fine=%.3f n %.3f %.3f %.3f N.L %.3f colormap %08X lightmap %08X\n", x, z, t.height_at(x, z), t.fine_at(x, z), n[0], n[1], n[2], ndl,
                        texel(doc_->colormap), texel(doc_->lightmap));
                fclose(f);
            }
    } else if (cmd == L"--dump-starts" && doc_) {  // developer probes: every level start (uid, position, heading, team)
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    Vec3 p{};
                    get_pos(st, p);
                    fprintf(f, "uid %u %.1f %.1f %.1f hd %.1f team %d\n", st.uid(), p.x, p.y, p.z, st.f32("StartOrientation") * 57.2958f, st.i32("TeamIndex"));
                }
            fclose(f);
        }
    } else if (cmd == L"--dump-near" && doc_) {  // developer probes: x,z,r;file lists every positioned object (any class) within r metres
        size_t sc = a.find(';');
        float x = 0, z = 0, r = 8;
        sscanf(a.c_str(), "%f,%f,%f", &x, &z, &r);
        if (sc != std::string::npos)
            if (FILE* f = fopen(a.substr(sc + 1).c_str(), "w")) {
                const SheetSet nss = sheets(false);
                const place::SurfaceIndex nidx(nss.s);
                for (auto& o : doc_->world.objs) {
                    Vec3 p;
                    if (!get_pos(o, p) || std::hypot(p.x - x, p.z - z) > r) continue;
                    const float foot = p.y - stand_foot(o), sf = nidx.at(doc_->terrain, p.x, p.z, foot + place::kStandTol);
                    int ly = 0;
                    bool fx = false;
                    const bool has_ly = place::layer_of(o, ly, fx);
                    fprintf(f, "%08X uid %u %.1f %.1f %.1f hd %.1f model %s desc %s ground %.3f gap %.3f layer %d%s\n", o.cls, o.uid(), p.x, p.y, p.z, o.f32("CorporalHeading"),
                            o.str("Corporal3DObj").c_str(), o.str("Description").c_str(), doc_->terrain.height_at(p.x, p.z), foot - sf, has_ly ? ly : -1, has_ly && fx ? " fixed" : "");
                }
                fclose(f);
            }
    } else if (cmd == L"--dump-culture" && doc_) {  // developer probes: FILE = the culture types of the open level (template, model, density, share of the map, instances now)
        if (culture_.doc != doc_.get() || culture_.objs != doc_->world.objs.size()) load_culture();  // keeps the instances of the last frame
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            fprintf(f, "map %dx%d types %zu\n", culture_.w, culture_.h, culture_.types.size());
            {
                static const uint32_t K_CT = key_of("MACultureType"), K_CG = key_of("MACultureGenerator"), K_CS = key_of("MACultureSystem");
                int tix = find_class(doc_->world, K_TINFO);
                if (tix >= 0) {
                    fprintf(f, "terrain-info uid %u\n", doc_->world.objs[size_t(tix)].uid());
                    for (const WProp& pr : doc_->world.objs[size_t(tix)].props) {
                        std::string d = pr.display();
                        fprintf(f, "info %s t%u %s\n", gd_.names().label(pr.key).c_str(), pr.type, d.substr(0, 120).c_str());
                    }
                }
                for (auto& o : doc_->world.objs)
                    if (o.cls == K_CT || o.cls == K_CG || o.cls == K_CS) {
                        fprintf(f, "obj %s uid %u", gd_.names().label(o.cls).c_str(), o.uid());
                        for (const WProp& pr : o.props) fprintf(f, " | %s=%s", gd_.names().label(pr.key).c_str(), pr.display().c_str());
                        fprintf(f, "\n");
                    }
            }
            for (const CultureType& c : culture_.types) {
                size_t on = 0;
                for (uint16_t v : culture_.map) on += ((v >> c.shift) & ((1 << c.bits) - 1)) != 0;
                const TemplateInfo* ti = cat_ ? cat_->find(c.templ) : nullptr;
                {  // texels with a level near the camera (fade radius) and the model state
                    const Terrain& tr = doc_->terrain;
                    int near_on = 0, near_all = 0, hist[16] = {};
                    for (int z = 0; z < culture_.h; z++)
                        for (int x = 0; x < culture_.w; x++) {
                            float wx = (x + 0.5f) * tr.size_x() / culture_.w - cam_.eye.x, wz = (z + 0.5f) * tr.size_z() / culture_.h - cam_.eye.z;
                            if (wx * wx + wz * wz > c.stop_fade * c.stop_fade) continue;
                            int lv = (culture_.map[size_t(z) * culture_.w + x] >> c.shift) & ((1 << c.bits) - 1);
                            near_all++; near_on += lv != 0; hist[std::min(lv, 15)]++;
                        }
                    GpuModel* gm0 = ti && !ti->model.empty() ? models_.get(ti->model, ti->mtb) : nullptr;
                    fprintf(f, "  %s: eye %.0f,%.0f texels within fade %d, with level %d, levels", c.name.c_str(), cam_.eye.x, cam_.eye.z, near_all, near_on);
                    for (int l = 0; l < (1 << c.bits) && l < 16; l++) fprintf(f, " %d:%d", l, hist[l]);
                    fprintf(f, " model state %d bounds %.2f,%.2f,%.2f..%.2f,%.2f,%.2f\n", gm0 ? int(gm0->state) : -1, gm0 ? gm0->bmin[0] : 0.f, gm0 ? gm0->bmin[1] : 0.f, gm0 ? gm0->bmin[2] : 0.f, gm0 ? gm0->bmax[0] : 0.f, gm0 ? gm0->bmax[1] : 0.f, gm0 ? gm0->bmax[2] : 0.f);
                }
                size_t in_view = 0;  // instances inside the camera frustum (clip-space test) - comparable with the game's per-frame draw count
                for (const BoxInstance& bi : c.inst) {
                    XMVECTOR cp = XMVector4Transform(XMVectorSet(bi.r0.w, bi.r1.w, bi.r2.w, 1.f), XMLoadFloat4x4(&cam_.viewproj));
                    float w = XMVectorGetW(cp);
                    if (w > 0 && std::fabs(XMVectorGetX(cp)) <= w && std::fabs(XMVectorGetY(cp)) <= w) in_view++;
                }
                if (const GpuModel* g1 = ti && !ti->model.empty() ? models_.get(ti->model, ti->mtb) : nullptr) {  // submesh index counts (the game trace prints n per draw)
                    fprintf(f, "  %s: %zu meshes, counts", c.name.c_str(), g1->meshes.size());
                    for (const GpuMesh& m : g1->meshes) fprintf(f, " %u", m.count);
                    fprintf(f, "\n");
                }
                fprintf(f, "  %s: instances in view %zu of %zu\n", c.name.c_str(), in_view, c.inst.size());
                fprintf(f, "%s shift %d bits %d templ %u '%s' model %s density %.0f grid %.0f,%.0f scale %.2f slope %.0f..%.0f fade %.0f rot %d align %d snap %d map %.1f%% inst %zu\n", c.name.c_str(), c.shift, c.bits, c.templ, ti ? ti->name.c_str() : "?", ti ? ti->model.c_str() : "?", c.density, c.grid[0], c.grid[1], c.scale_range, c.min_slope, c.max_slope, c.stop_fade, c.random_rot, c.align, c.snap, culture_.map.empty() ? 0.0 : 100.0 * double(on) / double(culture_.map.size()), c.inst.size());
            }
            fclose(f);
        }
    } else if (cmd == L"--find-refs" && doc_) {  // developer probes: "uid;file" every object property whose 4-byte value is that uid (who targets / lists the object), plus the object's own parent chain
        size_t sc = a.find(';');
        unsigned uid = unsigned(atoi(a.c_str()));
        if (sc != std::string::npos)
            if (FILE* f = fopen(a.substr(sc + 1).c_str(), "w")) {
                WTree t;
                t.build(doc_->world);
                for (size_t i = 0; i < doc_->world.objs.size(); i++) {
                    const WObj& o = doc_->world.objs[i];
                    for (const WProp& pr : o.props) {
                        uint32_t v;
                        if (pr.data.size() != 4 || pr.type == 7) continue;
                        std::memcpy(&v, pr.data.data(), 4);
                        std::string lb = gd_.names().label(pr.key);
                        if (v == uid && lb != "UID" && lb != "TemplateID")
                            fprintf(f, "uid %u %s '%s' parent %d .%s=%u\n", o.uid(), gd_.names().label(o.cls).c_str(), o.str("Description").c_str(), t.parent_of[i] >= 0 ? int(doc_->world.objs[size_t(t.parent_of[i])].uid()) : -1, lb.c_str(), v);
                    }
                }
                fclose(f);
            }
    } else if (cmd == L"--dump-props" && doc_) {  // developer probes: "uid;file" every property (name or key, type, value) of that object
        size_t sc = a.find(';');
        unsigned uid = unsigned(atoi(a.c_str()));
        if (sc != std::string::npos)
            if (FILE* f = fopen(a.substr(sc + 1).c_str(), "w")) {
                for (auto& o : doc_->world.objs) {
                    if (o.uid() != uid) continue;
                    fprintf(f, "class %s\n", gd_.names().label(o.cls).c_str());
                    for (const WProp& pr : o.props) fprintf(f, "%s t%u %s\n", gd_.names().label(pr.key).c_str(), pr.type, pr.display().c_str());
                }
                if (cat_)  // a palette template that is not placed in the level: its properties come from the catalog
                    if (const WObj* o = cat_->object(uid)) {
                        fprintf(f, "template class %s\n", gd_.names().label(o->cls).c_str());
                        for (const WProp& pr : o->props) fprintf(f, "%s t%u %s\n", gd_.names().label(pr.key).c_str(), pr.type, pr.display().c_str());
                    }
                fclose(f);
            }
    } else if (cmd == L"--dump-class" && doc_) {  // developer probes: "Class;file" every object of that class with all its properties and world position
        size_t sc = a.find(';');
        if (sc != std::string::npos)
            if (FILE* f = fopen(a.substr(sc + 1).c_str(), "w")) {
                uint32_t want = key_of(a.substr(0, sc));
                for (auto& o : doc_->world.objs) {
                    if (o.cls != want) continue;
                    Vec3 p;
                    bool hp = get_pos(o, p);
                    fprintf(f, "uid %u '%s' pos %.1f %.1f %.1f\n", o.uid(), o.str("Description").c_str(), hp ? p.x : 0.f, hp ? p.y : 0.f, hp ? p.z : 0.f);
                    for (const WProp& pr : o.props) fprintf(f, "  %s t%u %s\n", gd_.names().label(pr.key).c_str(), pr.type, pr.display().c_str());
                }
                fclose(f);
            }
    } else if (cmd == L"--dump-colors" && doc_) {  // developer probes: FILE: every object with colour-set properties (colour shift / team colour census)
        if (FILE* f = fopen(path_utf8(fs::path(arg)).c_str(), "w")) {
            for (auto& o : doc_->world.objs) {
                if (!o.find("UseCustomColors") && !o.find("SupportsColorShift")) continue;
                fprintf(f, "uid %u %s model %s csup %d usecust %d ovrdef %d ovren %d custdiff %08X spdiff %08X custemis %08X spemis %08X rgb %.2f,%.2f,%.2f team %d\n", o.uid(), gd_.names().label(o.cls).c_str(),
                        o.str("Corporal3DObj").c_str(), o.i32("SupportsColorShift"), o.i32("UseCustomColors"), o.i32("OverrideDefaultColors"), o.i32("OverrideEnable"),
                        o.u32("CustomSinglePlayerDiffuseColor"), o.u32("SinglePlayerDiffuseColor"), o.u32("CustomSinglePlayerEmissiveColor"), o.u32("SinglePlayerEmissiveColor"),
                        o.f32("ColorRed"), o.f32("ColorGreen"), o.f32("ColorBlue"), o.i32("Nation"));
            }
            fclose(f);
        }
    } else if (cmd == L"--set-start" && doc_) {  // developer probes: x,z,headingDeg puts the level start there on the ground
        float x = 0, z = 0, hd = 0;
        if (sscanf(a.c_str(), "%f,%f,%f", &x, &z, &hd) == 3)
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    set_pos(st, {x, doc_->terrain.height_at(x, z), z});
                    set_heading(st, hd / 57.2958f);
                    objects_dirty_ = true;
                    break;
                }
    } else if (cmd == L"--place-water" && doc_ && io_ && cat_) {  // developer probes: "name part,metres ahead of the start,W,L" (W,L optional)
        std::vector<std::string> f = split(a, ',');
        float d = f.size() > 1 ? float(atof(f[1].c_str())) : 40.f, ww = f.size() > 3 ? float(atof(f[2].c_str())) : 0, wl = f.size() > 3 ? float(atof(f[3].c_str())) : 0;
        for (auto& ti : cat_->all()) {
            if (ti.cls_name != "MAWater" || lower(ti.name).find(lower(f[0])) == std::string::npos) continue;
            for (auto& st : doc_->world.objs)
                if (st.uid() == player_start_uid(doc_->world)) {  // the "Player Spawner" the game uses
                    Vec3 p;
                    get_pos(st, p);
                    float h = st.f32("StartOrientation");
                    Vec3 at{p.x + std::sin(h) * d, 0, p.z + std::cos(h) * d};
                    at.y = doc_->terrain.height_at(at.x, at.z) + 1.f;
                    uint32_t u = io_->place_template(*doc_, ti.uid, at, 0);
                    for (auto& o : doc_->world.objs)
                        if (u && o.uid() == u) {
                            set_pos(o, at);
                            if (ww > 0) o.set_str("Description", ScenarioIO::water_description(ScenarioIO::water_name(o), ww, wl));
                        }
                    world_changed();
                    break;
                }
            break;
        }
    } else if (cmd == L"--place-water-spawns" && doc_ && io_ && cat_) {  // developer probes: "name part,W,L,lift[,ahead[,oval|rect[,headingDeg]]]" puts one water on every spawn point (ahead = metres in front of it)
        std::vector<std::string> f = split(a, ',');
        if (f.size() >= 4)
            for (auto& ti : cat_->all()) {
                if (ti.cls_name != "MAWater" || lower(ti.name).find(lower(f[0])) == std::string::npos) continue;
                std::vector<Vec3> at;
                float ahead = f.size() > 4 ? float(atof(f[4].c_str())) : 0.f;
                for (auto& st : doc_->world.objs) {
                    Vec3 p;
                    if (st.cls == K_LEVELSTART && get_pos(st, p)) {
                        float h = st.f32("StartOrientation");
                        at.push_back({p.x + std::sin(h) * ahead, 0, p.z + std::cos(h) * ahead});
                    }
                }
                for (auto& p : at) {
                    uint32_t u = io_->place_template(*doc_, ti.uid, p, f.size() > 6 ? float(atof(f[6].c_str())) / 57.2958f : 0.f);
                    for (auto& o : doc_->world.objs)
                        if (u && o.uid() == u) {
                            set_pos(o, {p.x, doc_->terrain.height_at(p.x, p.z) + float(atof(f[3].c_str())), p.z});
                            if (atof(f[1].c_str()) > 0) o.set_str("Description", ScenarioIO::water_description(ScenarioIO::water_name(o), float(atof(f[1].c_str())), float(atof(f[2].c_str())), f.size() > 5 && f[5] == "oval" ? wcustom::Shape::Oval : wcustom::Shape::Rect));
                        }
                }
                world_changed();
                break;
            }
    } else if (cmd == L"--water-size" && doc_) {  // developer probes: W,L[,oval] sets every water object's size (and shape)
        float ww = 0, wl = 0;
        wcustom::Shape sh = a.size() > 5 && a.compare(a.size() - 5, 5, ",oval") == 0 ? wcustom::Shape::Oval : wcustom::Shape::Rect;
        if (sscanf(a.c_str(), "%f,%f", &ww, &wl) == 2)
            for (auto& o : doc_->world.objs)
                if (o.cls == K_WATER) {
                    const TemplateInfo* ti = cat_->find(o.template_id());
                    o.set_str("Description", ScenarioIO::water_description(ScenarioIO::water_name(o), ww, wl, sh));
                    objects_dirty_ = true;
                }
    } else if (cmd == L"--make-pond" && doc_) {  // developer probes: x,z,radius,depth runs the pond tool there
        float x = 0, z = 0, r = 25, d = 3;
        if (sscanf(a.c_str(), "%f,%f,%f,%f", &x, &z, &r, &d) >= 2) {
            pond_radius_ = r;
            pond_depth_ = d;
            make_pond({x, doc_->terrain.height_at(x, z), z});
        }
    } else if (cmd == L"--place-road" && doc_ && io_) {  // developer proof: "name substring=metres ahead of the start[/sideways]" places one road piece in front of EVERY level start (all spawns)
        std::string nm = a.substr(0, a.find_first_of("=@"));
        float d = 10, x = 0;
        if (size_t eq = a.find('='); eq != std::string::npos) sscanf(a.c_str() + eq + 1, "%f", &d);
        if (size_t sl = a.find('/'); sl != std::string::npos) x = float(atof(a.c_str() + sl + 1));
        const RoadPiece* rp = nullptr;
        for (auto& r : road_pieces())
            if (r.search.find(nm) != std::string::npos) { rp = &r; break; }
        if (!rp) fprintf(stderr, "place-road: no piece matches '%s'\n", nm.c_str());
        if (rp && a.find("@snap") != std::string::npos && !dev_placed_.empty()) {  // "name@snap": the piece snaps onto the free end of the last piece placed here, like a click near it
            Vec3 lp{};
            float ly = 0;
            for (auto& o : doc_->world.objs)
                if (o.uid() == dev_placed_.back()) {
                    get_pos(o, lp);
                    if (const WProp* r = o.find("RoadRotation")) if (r->data.size() >= 4) memcpy(&ly, r->data.data(), 4);
                }
            // The solver with no camera (pixels = metres x 8, so only the cap acts): the free ghost's entry on the last piece's open end.
            place_road_ = rp->path;
            const ma2::snap::Mover mv = mover_for_place();
            place_road_.clear();
            if (objects_dirty_) rebuild_objects();
            std::vector<ma2::snap::Socket> all;
            std::vector<char> opn;
            snap_sockets(lp.x, lp.z, 300.f, 4, {}, all, opn);
            const ma2::snap::Socket* tgt = nullptr;
            for (size_t i = 0; i < all.size(); i++)
                if (opn[i] && all[i].uid == dev_placed_.back() && (!tgt || all[i].index != 0)) tgt = &all[i];
            bool ok = false;
            if (tgt && !mv.own.empty()) {
                const float hc = std::cos(ly), hs = std::sin(ly), lx = mv.cx - mv.own[0].x, lz = mv.cz - mv.own[0].z;
                Vec3 cen{tgt->x + hc * lx + hs * lz, 0, tgt->z - hs * lx + hc * lz};
                float yaw = ly;
                const ma2::snap::ToPx px8 = [](float x, float, float z, float& sx, float& sy) { sx = x * 8.f; sy = z * 8.f; return true; };
                snap_px_override_ = &px8;
                ok = solve_snap(mv, cen, yaw);
                snap_px_override_ = nullptr;
            }
            if (!ok) fprintf(stderr, "place-road: nothing to snap to near %.1f %.1f\n", lp.x, lp.z);
            else {
                const Vec3 sp{snap_.c.ox, doc_->terrain.height_at(snap_.c.ox, snap_.c.oz), snap_.c.oz};
                if (uint32_t u = io_->place_road(*doc_, rp->path, sp, snap_.c.yaw)) {
                    fprintf(stderr, "place-road: %s snapped uid %u at %.1f %.1f heading %.1f\n", rp->path.c_str(), u, sp.x, sp.z, snap_.c.yaw * 57.2958f);
                    dev_placed_.push_back(u);
                }
            }
            snap_clear();
        } else
        if (size_t ab = a.find('@'); rp && ab != std::string::npos) {  // "name@x,z,headingDeg[,y]": absolute coordinates instead of a level start
            float ax = 0, az = 0, hd = 0, ay = -1e9f;
            sscanf(a.c_str() + ab + 1, "%f,%f,%f,%f", &ax, &az, &hd, &ay);
            Vec3 at{ax, ay > -1e8f ? ay : doc_->terrain.height_at(ax, az) + 0.3f, az};
            uint32_t u = io_->place_road(*doc_, rp->path, at, hd * 3.14159f / 180.f);
            fprintf(stderr, "place-road: %s uid %u at %.1f %.1f\n", rp->path.c_str(), u, ax, az);
            if (u) dev_placed_.push_back(u);
            if (u && ay > -1e8f)  // place_road snaps to the editor's terrain; an explicit height wins
                for (auto& o : doc_->world.objs)
                    if (o.uid() == u) set_pos(o, {ax, ay, az});
        } else
        for (auto& st : doc_->world.objs)
            if (rp && st.cls == K_LEVELSTART) {
                Vec3 p;
                get_pos(st, p);
                float h = st.f32("StartOrientation");
                Vec3 at{p.x + std::sin(h) * d + std::cos(h) * x, 0, p.z + std::cos(h) * d - std::sin(h) * x};
                at.y = doc_->terrain.height_at(at.x, at.z) + 0.3f;
                uint32_t u = io_->place_road(*doc_, rp->path, at, h);
                fprintf(stderr, "place-road: %s uid %u\n", rp->path.c_str(), u);
                if (u) dev_placed_.push_back(u);
            }
        objects_dirty_ = true;
    } else if (cmd == L"--set-sky" && doc_ && io_) {  // developer pairs: sky name (Env_SkySpace...), like picking it under Advanced > Sky
        std::string err;
        if (!io_->set_sky(*doc_, a, false, &err)) ma2::log_stderr("set-sky failed: " + err + "\n");
        objects_dirty_ = true;
    } else if (cmd == L"--sky-mtb" && doc_) {  // developer probes: the MASky2's CorporalMTB ("-" removes the property)
        int si = find_class(doc_->world, K_SKY);
        if (si >= 0) {
            WObj& sky = doc_->world.objs[size_t(si)];
            if (a == "-") sky.erase(key_of("CorporalMTB"));
            else sky.set_str("CorporalMTB", a);
            doc_->dirty = true;
        }
    } else if (cmd == L"--set-look" && doc_ && io_) {  // developer pairs: a look's label ("Warm sun"), like picking it in the Look list
        std::string err;
        for (auto& l : weather::looks())
            if (iequals(l.label, a) && !weather::apply_look(*io_, *doc_, l, l.night, &err)) ma2::log_stderr("set-look failed: " + err + "\n");
        objects_dirty_ = true;
    } else if (cmd == L"--set-weather" && doc_) {  // developer pairs: none | light | heavy | snow | dust
        int ei = find_class(doc_->world, K_ENV);
        if (ei >= 0) {
            WObj& env = doc_->world.objs[size_t(ei)];
            if (a == "light" || a == "heavy") weather::set_rain_level(env, a == "light" ? weather::RainLevel::Light : weather::RainLevel::Heavy);
            else weather::set_precip(env, a == "snow" ? weather::Precip::Snow : a == "dust" ? weather::Precip::Dust : weather::Precip::None);
            doc_->dirty = true;
        }
    } else if (cmd == L"--save-to" && doc_ && io_) {  // developer bisects: save the (scripted) document elsewhere
        std::string err;
        if (!io_->save(*doc_, fs::path(arg), nullptr, &err)) ma2::log_stderr("save failed: " + err + "\n");
    } else if (cmd == L"--place-sweep" && doc_ && io_ && cat_) {  // developer check: every palette entry placed on flat ground rests on its model's lowest point
        models_.set_archives(gd_.game_files(), doc_->base, gd_.level_archives());
        std::string rep;
        int n = 0, bad = 0, nomodel = 0;
        float gx = doc_->terrain.size_x() * 0.5f, gz = doc_->terrain.size_z() * 0.5f;
        for (auto& t : cat_->all()) {
            if (!t.placeable || t.model.empty() || t.cls_name == "MAWater") continue;
            Vec3 at{gx, doc_->terrain.height_at(gx, gz), gz};
            uint32_t u = io_->place_template(*doc_, t.uid, at, 0.f);
            if (!u) continue;
            lift_to_foot(u);
            for (auto& o : doc_->world.objs) {
                if (o.uid() != u) continue;
                Vec3 p;
                if (!get_pos(o, p)) break;
                GpuModel* gm = models_.get(o.str("Corporal3DObj"), o.str("CorporalMTB"));
                if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
                if (!gm || gm->state != GpuModel::Ready) { nomodel++; break; }
                n++;
                float bottom = p.y + gm->bmin[1] - at.y;  // lowest point above the ground
                bool ok = std::fabs(bottom) < 0.05f;
                if (!ok) bad++;
                char line[512];
                snprintf(line, sizeof line, "%s %s bottom_above_ground %.3f bmin_y %.3f ", ok ? "ok" : "BAD", t.cls_name.c_str(), bottom, gm->bmin[1]);
                rep += std::string(line) + t.name + "\n";
                break;
            }
        }
        char head[200];
        snprintf(head, sizeof head, "placed %d, off-ground %d, model not ready %d\n", n, bad, nomodel);
        std::string all = head + rep;
        write_file(fs::path(arg), all.data(), all.size());
    } else if (cmd == L"--model-sweep" && doc_ && cat_) {  // developer check: every palette entry's model
        // Same filters as the Units palette (placeable, a model or water), all levels. Each model is
        // decoded with the library the view renders with: ok (meshes to draw), empty (decodes, nothing
        // visible at full health, e.g. light-only), failed (on the disc but does not decode), absent
        // (no archive has it in any folder or variant).
        models_.set_archives(gd_.game_files(), doc_->base, gd_.level_archives());
        std::string rep;
        int ok = 0, empty = 0, failed = 0, absent = 0, none = 0;
        for (auto& t : cat_->all()) {
            if (!t.placeable) continue;
            if (t.model.empty()) { if (t.cls_name != "MAWater" && t.cls_name != "MACorporeal") none++; continue; }
            std::string status, extra;
            if (ma2m::ResolveModelPath(*models_.archives(), t.model).empty()) {
                status = "absent";
                absent++;
                if (const TemplateInfo* alt = cat_->find(io_->similar_template(*doc_, t.uid))) extra = "\tsame object: " + alt->name;
            }
            else {
                ma2m::ModelLoadOptions opt;
                opt.mtb_path = t.mtb;
                ma2m::Model m;
                std::string e;
                if (!ma2m::LoadModel(*models_.archives(), t.model, m, opt, &e)) { status = "failed " + e; failed++; }
                else {
                    size_t vis = 0;
                    for (auto& sm : m.meshes) if (sm.visible && !sm.shadow && !sm.indices.empty()) vis++;
                    if (vis) { status = "ok"; ok++; } else { status = "empty"; empty++; }
                }
            }
            rep += status + "\t" + t.cls_name + "\t" + t.category + "\t" + t.name + "\t" + t.model + extra + "\n";
        }
        char head[256];
        snprintf(head, sizeof head, "ok %d, empty %d, failed %d, absent %d, no model property %d\n", ok, empty, failed, absent, none);
        std::string all = head + rep;
        write_file(fs::path(arg), all.data(), all.size());
    } else if (cmd == L"--confirm-test") {  // developer check: the confirmation popup renders
        confirm("Test confirmation", nullptr);
    } else if (cmd == L"--no-models") {
        show_models_ = false;
        objects_dirty_ = true;
    }
}

// File > New, set to "A copy of a real level" for the level being viewed, the name pre-filled.
void App::make_editable_copy() {
    if (!doc_ || !doc_->read_only || !io_) return;
    std::string level = doc_->retail_name.empty() ? doc_->name : doc_->retail_name;
    new_params_.multiplayer = doc_->multiplayer;
    new_params_.name = pretty_name(level) + " (my copy)";
    new_start_ = 2;
    int k = 0;
    for (auto& m : gd_.maps()) {
        if (!m.resource_present || m.multiplayer != doc_->multiplayer) continue;
        if (iequals(m.name, level)) { new_copy_ = k; break; }
        k++;
    }
    show_new_ = true;
}

void App::open_retail(const MapInfo& m) {
    auto s = std::make_unique<Scenario>();
    std::string err;
    if (!io_->open_retail(m, *s, &err)) { message("Cannot open map", err); return; }
    set_document(std::move(s));
    status_ = "Looking at " + pretty_name(m.name) + " (read-only). To change it, press \"Make an editable copy\" on the left.";
    status_time_ = 6;
}

void App::cmd_save(bool save_as) {
    save_failed_ = false;
    if (!doc_ || !io_) return;
    if (doc_->read_only) { message("Read-only", "Real levels cannot be changed. Press \"Make an editable copy\" on the Map tab to work on your own copy."); return; }
    fs::path target = doc_->file;
    bool auto_path = false;
    if (save_as || target.empty()) {
        std::error_code ec;
        fs::create_directories(settings_.scenarios(), ec);
        std::string base = doc_->name.empty() ? "Scenario" : doc_->name;
        base = safe_file_stem(base);  // also reserved names (CON, NUL, COM1) and trailing dots / spaces
        std::wstring f = file_dialog(hwnd_, true, settings_.scenarios(), widen(base + ".ma2map"));
        // Under --automation with no path queued (dialogs next_file), a UI-only run still saves: a new
        // name in the scenario folder, as Test does, and the status bar says where.
        if (f.empty() && automation_dialogs().active) {
            fs::path p = settings_.scenarios() / widen(base + ".ma2map");
            for (int n = 2; fs::exists(p, ec) && n < 1000; n++) p = settings_.scenarios() / widen(base + " (" + std::to_string(n) + ").ma2map");
            f = p.native();
            auto_path = true;
        }
        if (f.empty()) { status_ = "Not saved: no file name was chosen (File > Save as... tries again)."; status_time_ = 6; return; }
        target = f;
        // The game only lists scenarios in the scenario folder (not in its subfolders).
        if (!fs::equivalent(target.parent_path(), settings_.scenarios(), ec)) {
            std::wstring q = L"The game only lists scenarios saved in the scenario folder:\n" + settings_.scenarios().native() +
                             L"\n\nSave it there instead? (No saves it where you chose; the game will not show it.)";
            int r = ask_box(hwnd_, q.c_str(), L"MA2 Scenario Editor", MB_YESNOCANCEL | MB_ICONWARNING);
            if (r == IDCANCEL) { status_ = "Not saved."; status_time_ = 5; return; }
            if (r == IDYES) {
                target = settings_.scenarios() / target.filename();
                if (fs::exists(target, ec) &&
                    ask_box(hwnd_, (L"Replace the existing " + target.filename().native() + L"?").c_str(), L"MA2 Scenario Editor",
                                MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    status_ = "Not saved: the existing file was kept.";
                    status_time_ = 5;
                    return;
                }
            }
        }
        // The game lists the scenario by its Name. A map that still has a placeholder name ("New Map", empty) takes the name
        // the player just typed for the file, and the status says so. A name the user typed is never replaced, not even when
        // another saved scenario has it: the clash note below says how the game lists the two.
        if (doc_->name.empty() || is_default_name(doc_->name)) {
            const std::string stem = path_utf8(target.stem());
            if (stem != doc_->name) placeholder_named_ = stem;  // the status line only speaks when the name changed
            doc_->name = stem;
        }
    }
    if (doc_->author.empty()) doc_->author = settings_.author;
    // Save as to another file makes a second scenario: it needs its own Id, or the game lists only one of the two
    // ('duplicate Id ..., skipped'; 4C T5). Id 0 = build() mints a new one. A failed save puts the old Id back.
    const uint32_t old_id = doc_->id;
    {
        std::error_code ec;
        if (!doc_->file.empty() && !fs::equivalent(doc_->file, target, ec)) doc_->id = 0;
    }
    std::string err;
    SaveReport rep;
    // The hill shadows follow the ground and the sun by themselves at save, so there is no button to press and no second save.
    bool shadows_refreshed = false;
    if (lightmap_stale_ && !doc_->read_only) {
        io_->bake_lightmap(*doc_);
        r_.set_lightmap(doc_->lightmap);
        lightmap_stale_ = false;
        stale_sun_only_ = false;
        minimap_dirty_ = true;
        shadows_refreshed = true;
    }
    bool saved = io_->save(*doc_, target, &rep, &err);
    if (!saved && rep.over_limit) {
        // The game has no guard of its own: a map over its limits crashes the player's game at load. Saving it anyway is
        // possible (to cut it down later), but never by accident, and the game only lists it for the player who ignores this.
        std::wstring q = L"This scenario is over what the game can load:\n\n" + widen(rep.limit_text) +
                         L"\n\nThe game crashes while loading a map like this, for you and for anyone you share it with. "
                         L"Delete objects or textures first (Check scenario lists the biggest).\n\nSave it anyway?";
        if (ask_box(hwnd_, q.c_str(), L"MA2 Scenario Editor", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            err.clear();
            saved = io_->save(*doc_, target, &rep, &err, true);
        } else {
            doc_->id = old_id;
            placeholder_named_.clear();
            status_ = "Not saved: the scenario is over the game's limits (" + rep.limit_text + ")";
            status_time_ = 10;
            return;
        }
    }
    if (!saved) {
        doc_->id = old_id;
        placeholder_named_.clear();
        save_failed_ = true;
        const bool memory = err.find("map is still open and unchanged") != std::string::npos;  // F-V5R-1: the build failed, not the disk
        message("Save failed", "The scenario could not be saved to\n" + path_utf8(target) + "\n\n" + err +
                                   (memory ? std::string() : std::string("\n\nCheck that the folder exists and is not read-only, or use File > Save as... to pick another place.")));
        return;
    }
    clear_autosave();
    note_saved();
    const double gfx_prev_save = last_report_.graphics_mb;
    last_report_ = rep;
    gfx_calibrate(rep.graphics_mb);  // the exact figure replaces the live one
    {
        std::error_code ec;
        bool seen = false;
        for (auto& f : saved_files_) seen |= fs::equivalent(f, target, ec);
        if (!seen) saved_files_.push_back(fs::absolute(target, ec));
    }
    settings_.add_recent(target);
    settings_.save();
    char b[256];
    snprintf(b, sizeof b, "Saved %s (%.1f MB)", path_utf8(auto_path ? target : target.filename()).c_str(), rep.file_bytes / 1048576.0);
    status_ = b;
    status_time_ = 6;
    if (gfx_prev_save > 0 && std::fabs(rep.graphics_mb - gfx_prev_save) >= 0.1) {  // The save line carries the graphics figure when it moved since the last save
        snprintf(b, sizeof b, ". Graphics memory: about %.1f of 64 MB (%+.1f since the last save).", rep.graphics_mb, rep.graphics_mb - gfx_prev_save);
        status_ += b;
    }
    if (!placeholder_named_.empty()) {
        status_ += ". Its name in the game is now \"" + placeholder_named_ + "\" (it had no name of its own); change it on the Map tab.";
        status_time_ = 10;
        placeholder_named_.clear();
    }
    // The saved light map is the last one made: after a sun or ground change without Update shadows, the game shows the
    // old shadows (and the reopened map has no way to tell).
    if (lightmap_stale_) {   // only a real level (read-only, never re-baked) can still be stale here
        status_ += ". The shadows still follow the old sun or ground: press Update shadows (Environment tab), then save again.";
        status_time_ = 12;
    } else if (shadows_refreshed) status_ += ". The hill shadows were updated for the ground and the sun.";
    if (const std::string other = name_clash(doc_->name, target, doc_->multiplayer); !other.empty()) {
        status_ += ". \"" + doc_->name + "\" is also the name of " + other + "; the game lists them as \"" + doc_->name +
                   " (<file name>)\". Rename it on the Map tab.";
        status_time_ = 12;
    }
    // The Must-fix items of Check, too (no spawn points, a trigger that fires at load, ...): a saved map that cannot work used to
    // look fine until someone played it. Never blocks the save.
    std::vector<std::string> must_fix = rep.warnings;
    if (io_) {
        std::vector<ValidationItem> vi = io_->validate(*doc_);
        for (auto& v : vi) {
            if (v.level != 2) continue;
            bool dup = false;
            for (auto& x : must_fix) dup |= x == v.text;
            if (!dup) must_fix.push_back(v.text);
        }
    }
    if (!must_fix.empty()) {
        std::string w = "The scenario was saved, but the game will not list it until you fix these (Test tells you when it is ready):\n\n";
        for (auto& x : must_fix) w += "- " + x + "\n";
        message("Saved with warnings", w);
    }
}

void App::cmd_test() {
    test_launch_skipped_ = false;  // automation: set again below when this Test got as far as the command line
    test_command_.clear();
    test_dir_.clear();
    if (!doc_) { message("Test", "Open or create a scenario first."); return; }
    if (doc_->read_only) { message("Test", "You are looking at a real level, which cannot be changed. Press \"Make an editable copy\" on the Map tab, then test your copy."); return; }
    validation_ = check_now();
    bool errors = false;
    for (auto& v : validation_) errors |= v.level == 2;
    if (errors) {
        show_validation_ = true;
        // No popup: it would cover the Check window, which already lists what to fix.
        status_ = "Not ready to test: fix the red items in the Check window.";
        status_time_ = 10;
        return;
    }
    // First Test of a new scenario: save it straight into the scenario folder (the only place the game looks)
    // instead of asking for a file name; File > Save as... renames or moves it later.
    bool auto_named = false;
    std::string name_before;
    if (doc_->file.empty()) {
        std::error_code ec;
        fs::create_directories(settings_.scenarios(), ec);
        std::string base = doc_->name.empty() ? "Scenario" : doc_->name;
        base = safe_file_stem(base);  // also reserved names (CON, NUL, COM1) and trailing dots / spaces
        fs::path f = settings_.scenarios() / widen(base + ".ma2map");
        for (int n = 2; fs::exists(f, ec) && n < 1000; n++) f = settings_.scenarios() / widen(base + " (" + std::to_string(n) + ").ma2map");
        doc_->file = f;
        auto_named = true;
        // The file got a number because "<name>.ma2map" exists: the game would list two scenarios with one Name, so the
        // Name takes the file's ("New Mission (2)") as Save does.
        name_before = doc_->name;
        if (doc_->name.empty() || is_default_name(doc_->name)) {
            const std::string stem = path_utf8(f.stem());
            if (stem != doc_->name) placeholder_named_ = stem;
            doc_->name = stem;
        }
    }
    cmd_save(false);
    placeholder_named_.clear();
    if (save_failed_) {  // nothing was written: do not run the file that is on disk
        if (auto_named) { doc_->file.clear(); doc_->name = name_before; }
        return;
    }
    if (auto_named && doc_->dirty) { doc_->file.clear(); doc_->name = name_before; }  // the save failed: keep it untitled
    if (doc_->dirty || doc_->file.empty()) return;
    if (from_game && from_game_live) {
        // Opened from the running game (live): it plays the file now; the editor stays as it is, out of sight, until the
        // test is left.
        live_test_ = LiveTest{fs::absolute(doc_->file), {}, -1, -1, -1};
        if (doc_->multiplayer && !doc_->types.empty()) {
            live_test_.mode = doc_->types[size_t(std::clamp(test_mode_, 0, int(doc_->types.size()) - 1))];
            live_test_.red = test_red_; live_test_.blue = test_blue_; live_test_.skill = test_skill_;
        }
        live_test_pending_ = true;
        ns_tested_ = true;
        status_ = "Saved. Starting the test in the game...";
        status_time_ = 10;
        return;
    }
    if (from_game) {
        // Opened from the game: no second game. Saved; the game plays it when the editor closes with exit code 2.
        exit_code_ = 2;
        test_file_ = doc_->file;
        status_ = "Saved. Back to the game to play it...";
        quit_ = true;
        return;
    }
    fs::path exe = gd_.install() / L"MechAssault 2.exe";
    std::error_code ec;
    if (!fs::exists(exe, ec)) { message("Test", "MechAssault 2.exe was not found in " + path_utf8(gd_.install()) + ".\nSet the game folder in File > Settings > General."); return; }
    std::wstring cmd = L"\"" + exe.native() + L"\" --scenario \"" + doc_->file.native() + L"\"";
    std::string mode, mode_label;
    if (doc_->multiplayer && !doc_->types.empty()) {
        mode = doc_->types[size_t(std::clamp(test_mode_, 0, int(doc_->types.size()) - 1))];
        const GameTypeDef* g = find_game_type(mode);
        mode_label = g ? g->label : mode;
        cmd += L" --mode " + widen(mode) + L" --bots " + std::to_wstring(test_red_) + L"v" + std::to_wstring(test_blue_) +
               L" --skill " + std::to_wstring(test_skill_);
    }
    bool quick = settings_.quick_test;
    if (quick) cmd += L" --quick-test";
    else cmd += L" --skip-intro";  // no startup movies either way; the game stays open after the test
    if (automation) {
        // Automation: Check, save and the command line above ran as usual, but a test run never opens a game (not even a hidden
        // one). The command line and working folder are kept for `state` (test_command / test_dir / test_launch_skipped).
        test_command_ = cmd;
        test_dir_ = gd_.install().native();
        test_launch_skipped_ = true;
        ns_tested_ = true;
        status_ = "Test: the game was not started (automation run). The command line is in the state reply (test_command).";
        status_time_ = 10;
        return;
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring dir = gd_.install().native();
    // A hidden (automation) editor starts the game hidden and silent too, like the MCP game tools:
    // nothing may appear on the user's screen. The child inherits these two variables.
    if (background) {
        SetEnvironmentVariableW(L"RECOMP_FB_HIDDEN", L"1");
        SetEnvironmentVariableW(L"RECOMP_AUDIO_SILENT", L"1");
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    // Test runs the game on its own scratch save folder: the player's saves and settings are never touched, and the game's
    // single-instance lock (which has no effect when RECOMP_SAVE_DIR is set) does not turn Test away while the game is open.
    fs::path test_saves = Settings::local_dir() / L"TestSaves";
    fs::create_directories(test_saves, ec);
    SetEnvironmentVariableW(L"RECOMP_SAVE_DIR", test_saves.c_str());
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi);
    SetEnvironmentVariableW(L"RECOMP_SAVE_DIR", nullptr);
    if (background) {
        SetEnvironmentVariableW(L"RECOMP_FB_HIDDEN", nullptr);
        SetEnvironmentVariableW(L"RECOMP_AUDIO_SILENT", nullptr);
    }
    if (!started) {
        message("Test", "Could not start MechAssault 2.exe.");
        return;
    }
    CloseHandle(pi.hThread);
    if (test_proc_) CloseHandle(test_proc_);
    test_proc_ = pi.hProcess;  // poll_test_process: back to the editor when the game exits
    test_quick_ = quick;
    test_game_up_ = false;
    ns_tested_ = true;
    test_started_ = ImGui::GetTime();
    status_ = auto_named ? "Saved as " + path_utf8(doc_->file.filename()) + " in your scenario folder (File > Save as... renames it). Testing in the game..." : "Testing in the game...";
    status_time_ = 5;
    std::string back = quick ? " When you leave the match or close the game, the editor comes back." : "";
    if (doc_->multiplayer) {
        const GameTypeDef* g = find_game_type(mode);
        std::string bots;
        if (iequals(mode, "Grinder")) bots = "1 bot";
        else if (g && !g->team) bots = std::to_string(test_red_ + test_blue_) + " bots";
        else bots = std::to_string(test_red_) + " Red and " + std::to_string(test_blue_) + " Blue bots";
        message("Game started", "The game starts straight into a " + mode_label + " match on \"" + doc_->name + "\" with " + bots + " (" +
                                    skill_name(test_skill_) + "). These bot settings are for this test only; your own New Game settings "
                                    "are not changed." + back);
    } else {
        // Custom Scenarios starts the mission itself: the campaign's intro (SPIntro) does not play (wiki t16).
        message("Game started", "The game starts straight into \"" + doc_->name + "\" through Custom Scenarios." +
                                    (quick ? back : std::string(" The game's start-up movies may play first (press Enter to skip them); the campaign intro does not.")));
    }
}

// ---------------------------------------------------------------------------------------------
// Undo

void App::capture_meta(Snapshot& s) const {
    s.name = doc_->name;
    s.description = doc_->description;
    s.author = doc_->author;
    s.briefing_title = doc_->briefing_title;
    s.briefing = doc_->briefing;
    s.win_text = doc_->win_text;
    s.lose_text = doc_->lose_text;
    s.types = doc_->types;
    s.objectives = doc_->objectives;
    s.strings = doc_->strings;
}

void App::apply_meta(const Snapshot& s) {
    doc_->name = s.name;
    doc_->description = s.description;
    doc_->author = s.author;
    doc_->briefing_title = s.briefing_title;
    doc_->briefing = s.briefing;
    doc_->win_text = s.win_text;
    doc_->lose_text = s.lose_text;
    doc_->types = s.types;
    doc_->objectives = s.objectives;
    doc_->strings = s.strings;
    if (s.audio) doc_->audio = *s.audio;
    pick_ = PickTarget();
}

template <class T>
static bool same_vec(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
static bool same_image(const Image& a, const Image& b) { return a.w == b.w && a.h == b.h && same_vec(a.px, b.px); }
static bool same_audio(const std::map<int, AudioClip>& a, const std::map<int, AudioClip>& b) {
    if (a.size() != b.size()) return false;
    for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j)
        if (i->first != j->first || i->second.source != j->second.source || !same_vec(i->second.wav, j->second.wav)) return false;
    return true;
}
// A rough size of the world's heap use (objects, properties and their payloads), computed once per snapshot.
static size_t world_bytes_of(const Wdf& w) {
    size_t b = w.objs.capacity() * sizeof(WObj);
    for (const WObj& o : w.objs) {
        b += o.props.capacity() * sizeof(WProp);
        for (const WProp& p : o.props) b += p.data.capacity() + 16;
    }
    return b;
}

Snapshot App::snapshot(const std::string& label) const {
    Snapshot s;
    s.world = doc_->world;
    s.selection = selection_;
    // The big buffers: reuse the last snapshot's when the bytes are equal (memcmp), so a step that did not touch the ground
    // or the colours shares them with its neighbours.
    if (!snap_hgt_ || !same_vec(*snap_hgt_, doc_->terrain.hgt)) snap_hgt_ = std::make_shared<const std::vector<float>>(doc_->terrain.hgt);
    s.hgt = snap_hgt_;
    s.min_h = doc_->terrain.min_h;
    s.max_h = doc_->terrain.max_h;
    if (!snap_cm_ || !same_image(*snap_cm_, doc_->colormap)) snap_cm_ = std::make_shared<const Image>(doc_->colormap);
    s.colormap = snap_cm_;
    s.colormap_custom = doc_->colormap_custom;
    if (!snap_lm_ || !same_image(*snap_lm_, doc_->lightmap)) snap_lm_ = std::make_shared<const Image>(doc_->lightmap);
    s.lightmap = snap_lm_;
    s.lightmap_custom = doc_->lightmap_custom;
    s.lightmap_stale = lightmap_stale_;
    s.stale_sun_only = stale_sun_only_;
    s.lightmap_sun = lightmap_sun_;
    s.ground = doc_->ground;
    if (!snap_tm_ || !same_vec(*snap_tm_, doc_->typemap)) snap_tm_ = std::make_shared<const std::vector<uint16_t>>(doc_->typemap);
    s.typemap = snap_tm_;
    s.shaped = ns_shaped_;
    capture_meta(s);
    if (!snap_audio_ || !same_audio(*snap_audio_, doc_->audio)) snap_audio_ = std::make_shared<const std::map<int, AudioClip>>(doc_->audio);
    s.audio = snap_audio_;
    s.label = label;
    s.state_id = cur_state_id_;
    s.small_bytes = world_bytes_of(s.world) + s.name.size() + s.description.size() + s.author.size() + s.briefing_title.size() + s.briefing.size() +
                    sizeof(Snapshot);
    for (const auto& t : s.types) s.small_bytes += t.size() + 32;
    for (const auto& t : s.objectives) s.small_bytes += t.size() + 32;
    for (const auto& kv : s.strings) s.small_bytes += kv.second.size() + 48;
    return s;
}

// Snapshot memory: every shared buffer counted once however many steps hold it.
uint64_t App::undo_bytes(uint64_t* parts) const {
    std::unordered_set<const void*> seen;
    uint64_t b = 0, pw = 0, ph = 0, pc = 0, pt = 0, pg = 0, pl = 0, pa = 0;
    auto add = [&](const Snapshot& s) {
        pw += s.small_bytes;
        if (s.hgt && seen.insert(s.hgt.get()).second) ph += uint64_t(s.hgt->capacity()) * sizeof(float);
        if (s.colormap && seen.insert(s.colormap.get()).second) pc += uint64_t(s.colormap->px.capacity()) * 4;
        if (s.lightmap && seen.insert(s.lightmap.get()).second) pc += uint64_t(s.lightmap->px.capacity()) * 4;
        if (s.typemap && seen.insert(s.typemap.get()).second) pt += uint64_t(s.typemap->capacity()) * 2;
        if (s.ground.weights && seen.insert(s.ground.weights.get()).second) pg += uint64_t(s.ground.weights->capacity());
        for (const auto& L : s.ground.layers) pl += uint64_t(L.image.px.capacity()) * 4 + L.path.size() + L.archive.size();
        if (s.audio && seen.insert(s.audio.get()).second)
            for (const auto& kv : *s.audio) pa += uint64_t(kv.second.wav.capacity()) + kv.second.source.size() + 64;
    };
    for (const Snapshot& s : undo_) add(s);
    for (const Snapshot& s : redo_) add(s);
    b = pw + ph + pc + pt + pg + pl + pa;
    if (parts) { parts[0] = pw; parts[1] = ph; parts[2] = pc; parts[3] = pt; parts[4] = pg; parts[5] = pl; parts[6] = pa; }
    return b;
}

// The history stays inside 200 steps and 1 GiB (the oldest go first); the first drop says so, then at most every 30 s.
void App::trim_undo() {
    std::string first;
    int dropped = 0;
    while (!undo_.empty() && (undo_.size() > kUndoSteps || (undo_.size() > 1 && undo_bytes() > kUndoBudget))) {
        if (!dropped) first = undo_.front().label;
        undo_.pop_front();
        dropped++;
    }
    const double now = ImGui::GetTime();
    if (dropped && now - undo_note_time_ >= kUndoDropNoteEvery) {
        undo_note_ = "Undo history is full: the oldest step (" + first + ") was dropped.";
        undo_note_time_ = now;
    }
}

// The document now differs from the saved file by something no undo step covers: the clean mark can never come back.
void App::mark_untracked_change() { saved_state_id_ = -1; }

// Any change that set the dirty flag without pushing a step shows as "dirty while still on the saved state".
void App::check_untracked_change() {
    if (doc_ && doc_->dirty && cur_state_id_ == saved_state_id_) mark_untracked_change();
}

void App::note_saved() {
    check_untracked_change();
    saved_state_id_ = cur_state_id_;
}

// One undo step for a widget that edits scenario data: the state before the widget became active
// (meta_pre_, refreshed every frame while nothing is being edited) plus the unchanged world.
void App::push_undo_once(const std::string& label) {
    if (!editable()) return;
    // One step per edit: while the same widget stays active (typing, dragging) nothing more is pushed.
    check_stale_ = true;
    // (A number typed into a field applies once more on Enter, the frame the widget goes inactive: still the same edit.)
    if (last_undo_id_ && (ImGui::IsAnyItemActive() || ImGui::GetCurrentContext()->ActiveIdPreviousFrame != 0)) { doc_->dirty = true; return; }
    last_undo_id_ = 1;
    check_untracked_change();
    Snapshot s = snapshot(label);
    s.name = meta_pre_.name;
    s.description = meta_pre_.description;
    s.author = meta_pre_.author;
    s.briefing_title = meta_pre_.briefing_title;
    s.briefing = meta_pre_.briefing;
    s.win_text = meta_pre_.win_text;
    s.lose_text = meta_pre_.lose_text;
    s.types = meta_pre_.types;
    s.objectives = meta_pre_.objectives;
    s.strings = meta_pre_.strings;
    undo_.push_back(std::move(s));
    redo_.clear();
    cur_state_id_ = next_state_id_++;
    trim_undo();
    doc_->dirty = true;
}

void App::restore(const Snapshot& s) {
    apply_meta(s);
    // Undoing a sun change after 'Update shadows' kept hill shadows baked for the undone sun.
    auto sun_of = [](const Wdf& w) { const int i = find_class(w, K_ENV); return i < 0 ? Vec3{} : w.objs[size_t(i)].vec3("DirectionalLightDirection"); };
    const Vec3 sun0 = sun_of(doc_->world), sun1 = sun_of(s.world);
    const bool sun_change = sun0.x != sun1.x || sun0.y != sun1.y || sun0.z != sun1.z;
    doc_->world = s.world;
    bool hchange = doc_->terrain.hgt != *s.hgt;
    doc_->terrain.hgt = *s.hgt;
    doc_->terrain.min_h = s.min_h;
    doc_->terrain.max_h = s.max_h;
    bool cchange = doc_->colormap.px != s.colormap->px || doc_->ground.weights != s.ground.weights || doc_->ground.count() != s.ground.count() ||
                   doc_->ground.enabled != s.ground.enabled;
    for (int k = 0; !cchange && k < doc_->ground.count(); k++)  // a changed texture or pattern size of an extra layer
        cchange = doc_->ground.layers[size_t(k)].path != s.ground.layers[size_t(k)].path || doc_->ground.layers[size_t(k)].repeats != s.ground.layers[size_t(k)].repeats;
    doc_->colormap = *s.colormap;
    doc_->ground = s.ground;
    doc_->colormap_custom = s.colormap_custom;
    doc_->typemap = *s.typemap;
    snap_hgt_ = s.hgt;  // the document equals these buffers now: the next snapshot shares them
    snap_cm_ = s.colormap;
    snap_tm_ = s.typemap;
    snap_audio_ = s.audio;
    doc_->typemap_dirty = true;
    ns_shaped_ = s.shaped;
    doc_->dirty = true;
    document_changed(hchange, cchange);
    if (s.lightmap && !s.lightmap->px.empty()) {
        // The light map of that state, exactly (never a fresh bake: the same ground gave a different picture).
        doc_->lightmap = *s.lightmap;
        doc_->lightmap_custom = s.lightmap_custom;
        snap_lm_ = s.lightmap;
        r_.set_lightmap(doc_->lightmap);
        if (hchange || sun_change) r_.params.lighting = true;
        lightmap_stale_ = s.lightmap_stale;
        stale_sun_only_ = s.stale_sun_only;
        lightmap_sun_ = s.lightmap_sun;
    } else if (hchange || sun_change) {
        // A snapshot without a light map: re-light the restored shape (or sun) so shadows match it.
        io_->bake_lightmap(*doc_);
        r_.set_lightmap(doc_->lightmap);
        r_.params.lighting = true;
        lightmap_stale_ = false;
        stale_sun_only_ = false;
    }
    world_changed();
}

void App::push_undo(const std::string& label) {
    if (!editable()) return;
    check_stale_ = true;
    check_untracked_change();
    undo_.push_back(snapshot(label));
    redo_.clear();
    cur_state_id_ = next_state_id_++;
    trim_undo();
}

void App::push_undo_merge(const std::string& label, const std::string& tag) {
    if (!editable()) return;
    const double t = ImGui::GetTime();
    const uint64_t sh = undo_sel_hash(selection_);
    // Only while nothing else happened since the merging push (any other step, undo or save moves the state id).
    if (!undo_.empty() && cur_state_id_ == merge_state_ && cur_state_id_ != saved_state_id_ &&
        undo_should_merge(merge_tag_, merge_sel_, merge_time_, tag, sh, t, redo_.empty())) {
        merge_time_ = t;
        check_stale_ = true;
        return;
    }
    push_undo(label);
    merge_tag_ = tag;
    merge_sel_ = sh;
    merge_time_ = t;
    merge_state_ = cur_state_id_;
}

Snapshot App::drop_last_undo() {
    Snapshot s = std::move(undo_.back());
    undo_.pop_back();
    cur_state_id_ = s.state_id;
    merge_state_ = -1;
    return s;
}

void App::undo() {
    if (!editable() || undo_.empty()) return;
    if (stroke_active_ || dragging_ || gizmo_drag_ || place_drag_ || water_drag_) {  // mid-gesture: the key press is dropped
        status_ = "Finish the drag first.";
        status_time_ = 3;
        return;
    }
    check_untracked_change();
    redo_.push_back(snapshot(undo_.back().label));
    restore(undo_.back());
    culture_.doc = nullptr;  // the culture map lives in the world: read it again
    selection_ = undo_.back().selection;
    world_changed();
    cur_state_id_ = undo_.back().state_id;
    doc_->dirty = cur_state_id_ != saved_state_id_;
    status_ = "Undo: " + undo_.back().label;
    status_time_ = 3;
    undo_.pop_back();
}

void App::redo() {
    if (!editable() || redo_.empty()) return;
    if (stroke_active_ || dragging_ || gizmo_drag_ || place_drag_ || water_drag_) {
        status_ = "Finish the drag first.";
        status_time_ = 3;
        return;
    }
    check_untracked_change();
    undo_.push_back(snapshot(redo_.back().label));
    restore(redo_.back());
    culture_.doc = nullptr;
    selection_ = redo_.back().selection;
    world_changed();
    cur_state_id_ = redo_.back().state_id;
    doc_->dirty = cur_state_id_ != saved_state_id_;
    status_ = "Redo: " + redo_.back().label;
    status_time_ = 3;
    redo_.pop_back();
    trim_undo();
}

// ---------------------------------------------------------------------------------------------
// Object views

static bool parse_extent(const std::string& name, float& wx, float& wz) {
    // "Water_Sw_1000x1000 NEW" -> 1000 x 1000
    for (size_t i = 0; i + 3 < name.size(); i++) {
        if (!isdigit((unsigned char)name[i]) || (i && isdigit((unsigned char)name[i - 1]))) continue;
        char* e;
        long a = strtol(name.c_str() + i, &e, 10);
        if ((*e == 'x' || *e == 'X') && isdigit((unsigned char)e[1])) {
            long b = strtol(e + 1, nullptr, 10);
            if (a >= 10 && b >= 10) { wx = float(a); wz = float(b); return true; }
        }
    }
    return false;
}

// Legend over the map: what each helper colour means, with a switch per group.
void App::ui_legend() {
    if (!doc_ || !show_helpers_ || !show_legend_) return;
    struct Row { int group; const char* cls; const char* label; const char* tip; uint32_t color = 0; };
    static const Row rows[] = {
        {HG_TRIGGERS, "MATriggerRadius", "Trigger circle", "A trigger fires when a unit enters this circle (Objectives & Triggers tab)."},
        {HG_TRIGGERS, "MATriggerCuboidalVolume", "Trigger box", "A trigger fires when a unit enters this box; real levels use thin ones as tripwires."},
        {HG_PATHS, "MAWaypoint", "AI path point", "Points AI units patrol along; lines join the points of one path."},
        {HG_MARKERS, "MADummyObj", "Marker", "A named spot scripts use (drop points, targets, camera aims)."},
        {HG_LIGHTS, "MAPointLight", "Light", "A point light; its circle is its reach."},
        {HG_CAMERAS, "MAFreeCamera", "Camera", "A cutscene or fly-by camera."},
        {HG_CAMERAS, nullptr, "Camera path", "The points a cutscene camera flies through, in order; lines join them. A short yellow tick on each point shows where the camera looks from there.", kCamPathColor},
        {HG_OTHER, nullptr, "Other logic", "Game logic with a position (objective areas, menus, checkpoints...)."},
    };
    ImGui::SetNextWindowPos(ImVec2(vp_x + 10 * dpi_, vp_y + vp_h - 10 * dpi_), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.75f);
    // Starts folded to its title bar in the corner, so it does not cover the ground being worked on; a click unfolds it.
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once);
    if (ImGui::Begin("Helpers##legend", &show_legend_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                       ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextDisabled("Not shown in the game (H hides all)");
        for (auto& r : rows) {
            ImGui::PushID(r.label);
            bool on = show_helper_group_[r.group];
            if (ImGui::Checkbox("##on", &on)) { show_helper_group_[r.group] = on; objects_dirty_ = true; }
            ImGui::SameLine();
            uint32_t c = r.color ? r.color : 0xFFAAAAAAu;
            if (r.cls) if (const DictClass* dc = dict_.cls(std::string(r.cls))) c = dc->color;
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h - 4, p.y + h - 2), c | 0xFF000000u);
            ImGui::Dummy(ImVec2(h, h));
            ImGui::SameLine();
            ImGui::TextUnformatted(r.label);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.tip);
            ImGui::PopID();
        }
    }
    ImGui::End();
}

// Which helper group a class belongs to; -1 for objects the game draws (models, water, roads) and
// the mode pieces players see (spawns, pads, flags, Snatch It, crates, effects). Classes the
// dictionary draws as "model"/"plane" are game objects; any other positioned class is a helper.
// Side only means something for things that fight or can be fought: not for the player start, helpers, roads,
// effects or water. Plain scenery keeps the control only when the level already gave it a side.
bool App::has_side(const ma2::WObj& o) const {
    static const uint32_t K_ROAD_ = key_of("MARoad"), K_ROADGROUP_ = key_of("MARoadGroup"), K_EFFECT_ = key_of("MAEffect"),
                          K_SCENERY_ = key_of("MACorporeal");
    if (!o.find("Nation") || o.cls == K_LEVELSTART || o.cls == K_WATER || helper_group(o.cls) >= 0) return false;
    if (o.cls == K_ROAD_ || o.cls == K_ROADGROUP_ || o.cls == K_EFFECT_) return false;
    return o.cls != K_SCENERY_ || o.i32("Nation") != 0;
}

int App::helper_group(uint32_t cls) const {
    static const std::unordered_map<uint32_t, int> fixed = {
        {key_of("MATriggerRadius"), HG_TRIGGERS}, {key_of("MATriggerCuboidalVolume"), HG_TRIGGERS},
        {key_of("MATriggerNationRadius"), HG_TRIGGERS}, {key_of("MATriggerPoint"), HG_TRIGGERS},
        {key_of("MATriggerMenuActivate"), HG_TRIGGERS}, {key_of("MACheckpoint"), HG_TRIGGERS},
        {key_of("MACollectionArea"), HG_TRIGGERS}, {key_of("MAWaypoint"), HG_PATHS}, {key_of("MAPath"), HG_PATHS},
        {key_of("MADummyObj"), HG_MARKERS}, {key_of("MarkerInfo"), HG_MARKERS}, {key_of("MAObjectiveType"), HG_MARKERS},
        {key_of("MAPointLight"), HG_LIGHTS}, {key_of("MAFreeCamera"), HG_CAMERAS}, {key_of("MACinema"), HG_CAMERAS},
        {K_LEVELSTART, -1}, {K_WATER, -1}, {K_ROAD, -1}, {K_SNATCH_BASE, -1}, {K_SNATCH_CORE, -1}, {K_FLAG, -1},
        {K_CRATE, -1}, {K_EFFECT, -1}};
    auto it = fixed.find(cls);
    if (it != fixed.end()) return it->second;
    if (const DictClass* dc = dict_.cls(cls)) return dc->draw_kind == "model" || dc->draw_kind == "plane" ? -1 : HG_OTHER;
    return -1;
}

// MARoad: one prefab piece. Its RoadDefinition .road file names the model (relative to the .road
// file's folder) and the piece's bounds (boundary point1/point2: x -W..+W, z 0..-L). RoadRotation's
// first float is the yaw in radians; the piece starts at RoadPosition and runs along local -z.
void App::road_view(const WObj& o, ObjView& v) {
    struct RoadDef { std::string model; float w = 13, l = 30; bool ok = false; };
    static std::unordered_map<std::string, RoadDef> defs;
    static const void* for_archives = nullptr;
    if (for_archives != models_.archives()) { defs.clear(); for_archives = models_.archives(); }
    std::string def = norm_path(o.str("RoadDefinition"));
    auto it = defs.find(def);
    if (it == defs.end()) {
        RoadDef d;
        std::vector<uint8_t> b;
        if (models_.archives() && models_.archives()->Read(def, b)) {
            std::string folder = def.substr(0, def.rfind('/') + 1);
            float p1[3] = {13, 0, 0}, p2[3] = {-13, 0, -30};
            int seen = 0;
            for (auto& line : split(std::string(b.begin(), b.end()), '\n')) {
                std::string l = trim(line.substr(0, line.find(';')));
                size_t eq = l.find('=');
                if (eq == std::string::npos) continue;
                std::string k = lower(trim(l.substr(0, eq))), val = trim(l.substr(eq + 1));
                if (k == "model") d.model = folder + norm_path(val);
                else if (k == "point1" && sscanf(val.c_str(), "%f, %f, %f", &p1[0], &p1[1], &p1[2]) == 3) seen |= 1;
                else if (k == "point2" && sscanf(val.c_str(), "%f, %f, %f", &p2[0], &p2[1], &p2[2]) == 3) seen |= 2;
            }
            if (seen == 3) { d.w = std::max(1.f, std::fabs(p1[0] - p2[0]) * 0.5f); d.l = std::max(1.f, std::fabs(p1[2] - p2[2])); }
            d.ok = true;
        }
        it = defs.emplace(def, d).first;
    }
    const RoadDef& d = it->second;
    v.kind = 5;
    const WProp* rot = o.find("RoadRotation");
    float yaw = 0;
    if (rot && rot->data.size() >= 4) memcpy(&yaw, rot->data.data(), 4);
    v.heading = yaw;
    v.road_w = d.w;
    v.road_l = d.l;
    v.size = {2 * d.w, 0.3f, d.l};
    v.color = 0xFF505050u;
    if (show_models_ && !d.model.empty()) v.model = models_.get(d.model, "", crop_key(o), true);  // cut to its crop (app_crop.cpp); a cut still loading shows the last one
}

void App::rebuild_objects() {
    objects_dirty_ = false;
    gfx_scan_ = true;  // the live graphics figure recounts the templates on the map
    views_.clear();
    placed_n_ = 0;
    if (!doc_) return;
    const Wdf& w = doc_->world;
    WTree t;
    t.build(w);
    const std::set<uint32_t> cam_groups = cinema_path_groups(w);
    // Objects under the compiled editor-trigger group are not shown.
    std::vector<char> hidden(w.objs.size(), 0);
    for (size_t i = 0; i < w.objs.size(); i++)
        if (w.objs[i].cls == K_GROUP && w.objs[i].description() == kEditorTriggerGroup) {
            std::vector<int> sub;
            t.subtree(int(i), sub);
            for (int k : sub) hidden[size_t(k)] = 1;
        }
    // Status bar "N placed": whole placed things (not parts), no helpers, whatever the View filters hide.
    for (size_t i = 0; i < w.objs.size(); i++) {
        const WObj& o = w.objs[i];
        if (hidden[i] || o.cls == K_SKY || !position(o) || helper_group(o.cls) >= 0) continue;
        bool part = false;
        for (int p = t.parent_of[i]; p >= 0 && !is_container(w.objs[size_t(p)].cls); p = t.parent_of[size_t(p)])
            if (position(w.objs[size_t(p)])) { part = true; break; }
        if (!part) placed_n_++;
    }
    // View > Show objects for game type: leave out what the game detaches in that mode.
    view_type_hidden_ = 0;
    if (view_type_ >= 0 && view_type_ < int(game_types().size()) && doc_->multiplayer) {
        std::vector<char> in = objects_in_game_type(w, t, game_types()[size_t(view_type_)]);
        for (size_t i = 0; i < w.objs.size(); i++)
            if (!in[i] && !hidden[i] && position(w.objs[i])) { hidden[i] = 1; view_type_hidden_++; }
    }
    later_hidden_ = 0;
    start_state_.clear();
    std::vector<char> start(w.objs.size(), START_SHOWN);
    // A cutscene preview shows what the game shows when the cutscene plays: a reinforcement that arrives later
    // is not there yet (whatever View > Show later objects says). A unit whose arrival delay timer has run out by the time
    // the cutscene starts (the earliest timer that plays it) is there.
    const bool cut_preview = cpv_.on && cpv_.cin && !cpv_.still;
    float cut_start = -1;
    if (cut_preview) {
        static const uint32_t K_ACT_CIN = key_of("MAActionActivateCinema"), K_TIMER_ = key_of("MATriggerTimer");
        for (auto& a : w.objs)
            if (a.cls == K_ACT_CIN && a.u32("cinemaID") == cpv_.cin)
                if (const WObj* tr = object_by_uid(w, a.parent()); tr && tr->cls == K_TIMER_) {
                    float at = tr->f32("TrgTimerDurationSeconds");
                    if (cut_start < 0 || at < cut_start) cut_start = at;
                }
    }
    if (!show_later_ || cut_preview) {
        start = objects_at_start(w, t);
        if (cut_preview)
            for (size_t i = 0; i < w.objs.size(); i++)
                if (start[i] == START_CUTSCENE) start[i] = START_SHOWN;  // only used in a cutscene: the preview shows it
        if (cut_preview && cut_start >= 0)
            for (size_t i = 0; i < w.objs.size(); i++)
                if (start[i] == START_SPAWNED)
                    if (uint32_t at = unit_arrival_trigger(w.objs[i].uid()))
                        if (const WObj* tr = object_by_uid(w, at); tr && tr->cls == key_of("MATriggerTimer") && tr->f32("TrgTimerDurationSeconds") <= cut_start)
                            start[i] = START_SHOWN;
        for (size_t i = 0; i < w.objs.size(); i++)
            if (start[i] != START_SHOWN && start[i] != START_DESTROYED && !hidden[i] && position(w.objs[i])) {
                hidden[i] = 1;
                later_hidden_++;
                start_state_[w.objs[i].uid()] = start[i];
            }
    }
    for (size_t i = 0; i < w.objs.size(); i++) {
        const WObj& o = w.objs[i];
        if (hidden[i] || o.cls == K_SKY) continue;
        ObjView v;
        if (!get_pos(o, v.pos)) continue;
        v.index = int(i);
        v.uid = o.uid();
        v.heading = heading(o);
        if (o.cls != K_WATER) get_tilt(o, v.pitch, v.roll);  // a road piece tilts by its RoadRotation (playtest 11-04)
        int r = int(i), p = t.parent_of[i];
        while (p >= 0 && !is_container(w.objs[size_t(p)].cls)) {
            if (position(w.objs[size_t(p)])) r = p;
            p = t.parent_of[size_t(p)];
        }
        v.root = r;
        // Shape and colour by class.
        uint32_t c = o.cls;
        const TemplateInfo* ti = cat_ ? cat_->find(o.template_id()) : nullptr;
        std::string cat = ti ? ti->category : "";
        auto rgb = [](int r_, int g, int b) { return 0xFF000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | uint32_t(r_); };
        if (c == K_BUILDING) { v.size = {22, 16, 22}; v.color = rgb(150, 140, 125); }
        else if (c == K_MECH) { v.size = {6, 11, 6}; v.color = rgb(235, 140, 40); }
        else if (c == K_TANK || c == K_VEHICLE) { v.size = {6, 4, 9}; v.color = rgb(150, 160, 70); }
        else if (c == K_FLYER) { v.size = {10, 3, 10}; v.color = rgb(70, 200, 220); }
        else if (c == K_TURRET) { v.size = {4, 5, 4}; v.color = rgb(220, 70, 60); }
        else if (c == K_POWERUP || c == K_CRATE) { v.size = {2, 2, 2}; v.color = rgb(250, 230, 60); }
        else if (c == K_FLAG) { v.size = {1.5f, 9, 1.5f}; v.color = o.description().find("Blue") != std::string::npos ? rgb(60, 110, 255) : rgb(255, 60, 60); }
        else if (c == K_SNATCH_BASE || c == K_SNATCH_CORE) { v.size = {4, 4, 4}; v.color = rgb(220, 60, 220); }
        else if (c == K_PERSON) { v.size = {1, 2, 1}; v.color = rgb(240, 200, 170); }
        else if (c == K_EFFECT) { v.size = {2, 2, 2}; v.color = rgb(255, 120, 0); }
        else if (c == K_LEVELSTART) {
            v.kind = 1;
            v.size = {3, 3, 3};
            int team = o.i32("TeamIndex");
            v.color = team == 1 ? rgb(255, 50, 50) : team == 2 ? rgb(60, 120, 255) : rgb(240, 240, 240);
        } else if (c == K_WATER) {
            v.kind = 2;
            float wx = 500, wz = 500;
            if (!ScenarioIO::water_custom_size(o, wx, wz)) {  // until the model's bounding box is known
                const TemplateInfo* wt = cat_ ? cat_->find(o.template_id()) : nullptr;
                parse_extent(wt ? wt->name : o.description(), wx, wz);
            }
            v.size = {wx, 0.5f, wz};
            // Deep colours per water model (dictionary MAWater), translucent.
            std::string m = lower(o.str("Corporal3DObj"));
            v.color = m.find("hesperus") != std::string::npos ? rgb(58, 73, 57) : m.find("water_sw_500") != std::string::npos ? rgb(41, 55, 18)
                    : m.find("waterfall") != std::string::npos ? rgb(18, 32, 43) : rgb(100, 122, 101);
            v.color = (v.color & 0x00FFFFFFu) | 0xA0000000u;
        } else if (c == K_TRIG_RADIUS || c == K_TRIG_NRADIUS) {
            v.kind = 3;
            v.radius = c == K_TRIG_NRADIUS ? o.f32("m_fRadius", 20.f) : o.f32("EncounterRadius", 20.f);
            v.size = {2, 2, 2};
            v.color = rgb(255, 210, 0);
        } else if (c == K_TRIG_VOLUME) {
            v.kind = 3;
            v.size = {o.f32("VolumeWidth", 20), o.f32("VolumeHeight", 10), o.f32("VolumeLength", 20)};
            v.color = rgb(255, 170, 0);
            v.pos.y -= v.size.y * 0.5f;  // TriggerPoint is the box centre (retail tripwires then reach the ground)
        } else if (c == K_WAYPOINT) { v.kind = 4; v.size = {1.5f, 1.5f, 1.5f}; v.color = rgb(190, 90, 255); }
        else if (c == K_CORP) {
            std::string d = lower(o.description());
            if (cat.find("Trees") != std::string::npos || d.find("tree") != std::string::npos || d.find("mangrove") != std::string::npos) {
                v.size = {4, 10, 4}; v.color = rgb(40, 150, 50);
            } else if (d == "null" || d.find("null ") == 0 || d.find("pad") != std::string::npos) {
                v.size = {8, 0.6f, 8}; v.color = rgb(90, 90, 100);
            } else if (cat.find("Rocks") != std::string::npos || d.find("rock") != std::string::npos) {
                v.size = {8, 5, 8}; v.color = rgb(120, 115, 105);
            } else if (d.find("light") != std::string::npos) {
                v.size = {1.5f, 8, 1.5f}; v.color = d.find("blue") != std::string::npos ? rgb(80, 130, 255) : d.find("red") != std::string::npos ? rgb(255, 80, 80) : rgb(255, 255, 200);
            } else {
                v.size = {5, 4, 5}; v.color = rgb(190, 160, 110);
            }
        } else { v.size = {3, 3, 3}; v.color = rgb(170, 170, 170); }
        // Vehicle pads (MACorporeal > MASpawner > vehicle) and power-ups are a few pixels at map zoom:
        // they get a ring and a label like the spawn markers.
        auto plain = [&](const WObj& x) {
            const TemplateInfo* xt = cat_ ? cat_->find(x.template_id()) : nullptr;
            if (!xt) return strip_copy_counter(x.description());
            std::string n = strip_copy_counter(palette_name(*xt)), off = " (" + xt->name + ")";
            if (n.size() > off.size() && n.compare(n.size() - off.size(), off.size(), off) == 0) n.resize(n.size() - off.size());
            return n;
        };
        if (c == K_POWERUP || c == K_CRATE) v.tag = plain(o);
        else if (c == K_CORP && (doc_->multiplayer || o.template_id() != 32093))  // a mission's NULL holder is a reinforcement, not a pad (pad_vehicle)
            for (int k : t.kids[i])
                if (w.objs[size_t(k)].cls == K_SPAWNER && !t.kids[size_t(k)].empty()) {
                    const WObj& u = w.objs[size_t(t.kids[size_t(k)].front())];
                    if (u.cls == K_MECH || u.cls == K_TANK || u.cls == K_VEHICLE || u.cls == K_FLYER) { v.tag = "Pad: " + plain(u); break; }
                }
        // Helpers (the game never draws them) take their colour from the dictionary and can be
        // hidden by group (View > Helpers, legend).
        v.helper = helper_group(c);
        bool cam_path = c == K_WAYPOINT && t.parent_of[i] >= 0 && camera_path_group(w, t, size_t(t.parent_of[i]), cam_groups);
        if (cam_path) v.helper = HG_CAMERAS;
        if (v.helper >= 0) {
            if (!show_helpers_ || !show_helper_group_[v.helper]) continue;
            if (const DictClass* dc = dict_.cls(c)) v.color = dc->color;
            if (cam_path) v.color = kCamPathColor;
            if (v.helper == HG_OTHER || v.helper == HG_MARKERS || v.helper == HG_LIGHTS || v.helper == HG_CAMERAS) {
                if (v.kind == 0) v.kind = 4;  // a marker that grows with distance
                v.size = {1.5f, 1.5f, 1.5f};
                if (v.helper == HG_LIGHTS) { v.kind = 3; v.radius = o.f32("Radius", o.f32("LightRadius", 0)); v.size = {1, 1, 1}; }
            }
        }
        // Start points are not a helper group (players need them), but the game never draws them:
        // hiding helpers (H) hides them too, so the view matches the game.
        // Crate generators (power-up respawn points) likewise: the game shows their crate only later.
        if ((c == K_LEVELSTART || c == K_CRATE) && !show_helpers_) continue;
        if (c == K_ROAD) road_view(o, v);
        v.lo[0] = -v.size.x * 0.5f; v.lo[1] = 0; v.lo[2] = -v.size.z * 0.5f;
        v.hi[0] = v.size.x * 0.5f; v.hi[1] = v.size.y; v.hi[2] = v.size.z * 0.5f;
        if (show_models_ && (v.kind == 0 || v.kind == 2)) {
            std::string mp = o.str("Corporal3DObj"), mtb = o.str("CorporalMTB");
            if (start[i] == START_DESTROYED && cat_)  // wrecked at the start: the game shows its destroyed version
                if (const TemplateInfo* dt = cat_->find(o.u32("DestroyedBuilding")); dt && !dt->model.empty()) { mp = dt->model; mtb = dt->mtb; }
            if (!mp.empty()) {
                GpuModel* gm = models_.get(mp, mtb, crop_key(o), true);  // a cropped foundation draws cut (app_crop.cpp); a cut still loading shows the last one
                v.fx_model = mp;
                v.fx_mtb = mtb;
                if (v.kind == 0 || v.kind == 2) v.model = gm;  // water: the plane below plus the model's other layers (oily sheets)
                if (gm && gm->state == GpuModel::Ready) {
                    if (v.kind == 2) {
                        // Water has no size property: the plane is its model's bounding box, centred on
                        // CorporalPos (dictionary MAWater; the template-name size was wrong for some).
                        float cw, cl;
                        if (ScenarioIO::water_custom_size(o, cw, cl)) v.size = {cw, 0.5f, cl};  // resized: the saved model has this size
                        else v.size = {gm->bmax[0] - gm->bmin[0], 0.5f, gm->bmax[2] - gm->bmin[2]};
                    } else {
                        for (int k = 0; k < 3; k++) { v.lo[k] = gm->bmin[k]; v.hi[k] = gm->bmax[k]; }
                        v.size = {v.hi[0] - v.lo[0], v.hi[1] - v.lo[1], v.hi[2] - v.lo[2]};
                    }
                }
            }
        }
        if (v.kind == 5 && show_models_ && v.model && v.model->state == GpuModel::Ready)
            for (int k = 0; k < 3; k++) { v.lo[k] = v.model->bmin[k]; v.hi[k] = v.model->bmax[k]; }
        else if (v.kind == 5) {  // the grey strip: from the start point along local -z
            v.lo[0] = -v.road_w; v.lo[1] = 0; v.lo[2] = -v.road_l;
            v.hi[0] = v.road_w; v.hi[1] = v.size.y; v.hi[2] = 0;
        }
        views_.push_back(v);
    }
}

static XMFLOAT4 color4(uint32_t c, float boost = 1.f) {
    return {std::min(1.f, float(c & 255) / 255.f * boost), std::min(1.f, float((c >> 8) & 255) / 255.f * boost),
            std::min(1.f, float((c >> 16) & 255) / 255.f * boost), float(c >> 24) / 255.f};
}

static void add_line(std::vector<LineVertex>& L, XMFLOAT3 a, XMFLOAT3 b, uint32_t c) {
    L.push_back({a, c});
    L.push_back({b, c});
}

// A selected cropped road piece or foundation (app_crop.cpp): the whole piece's outline, dim, around the kept part.
void App::crop_draw_full(const ObjView& v) {
    if (!doc_ || v.index < 0) return;
    const WObj& o = doc_->world.objs[size_t(v.index)];
    crop::Rect r;
    if (!crop::of(o, r)) return;
    GpuModel* full = nullptr;
    if (o.cls == K_ROAD) {
        std::string def = o.str("RoadDefinition");
        for (char& ch : def) if (ch == '\\') ch = '/';
        if (const RoadPiece* rp = find_road_piece(def); rp && !rp->model.empty()) full = models_.get(rp->model, "");
    } else {
        full = models_.get(o.str("Corporal3DObj"), o.str("CorporalMTB"));
    }
    if (!full || full->state != GpuModel::Ready) return;
    const float ch = std::cos(v.heading), sh = std::sin(v.heading), y = v.pos.y + full->bmin[1] + 0.3f;
    auto at = [&](float lx, float lz) { return XMFLOAT3{v.pos.x + ch * lx + sh * lz, y, v.pos.z - sh * lx + ch * lz}; };
    const float x0 = full->bmin[0], x1 = full->bmax[0], z0 = full->bmin[2], z1 = full->bmax[2];
    const uint32_t c = 0xA0B0B0B0u;
    add_line(r_.lines, at(x0, z0), at(x1, z0), c);
    add_line(r_.lines, at(x1, z0), at(x1, z1), c);
    add_line(r_.lines, at(x1, z1), at(x0, z1), c);
    add_line(r_.lines, at(x0, z1), at(x0, z0), c);
}

// Yellow wire box around a selected object's model bounds (turned and tilted with it).
static void add_selection_box(std::vector<LineVertex>& L, const ObjView& v, uint32_t lc, bool brackets = false) {
    float ch = std::cos(v.heading), sh = std::sin(v.heading);
    // Brackets hug the drawn mesh, not the file's bbox (M01b Manticore: the bbox reaches well past the model on +x).
    const bool vb = brackets && v.model && v.model->has_vbox;
    const float* lo = vb ? v.model->vmin : v.lo;
    const float* hi = vb ? v.model->vmax : v.hi;
    float y0 = v.pos.y + lo[1], y1 = v.pos.y + hi[1] + 0.5f;
    XMFLOAT3 c[8];
    for (int k = 0; k < 8; k++) {
        float lx = (k & 1) ? hi[0] + 0.5f : lo[0] - 0.5f, lz = (k & 2) ? hi[2] + 0.5f : lo[2] - 0.5f;
        c[k] = {v.pos.x + ch * lx + sh * lz, (k & 4) ? y1 : y0, v.pos.z - sh * lx + ch * lz};
        if (v.pitch != 0 || v.roll != 0) {  // tilted: the box turns with the model
            BoxInstance mt;
            orient_instance(mt, v.heading, v.pitch, v.roll, v.pos);
            float ly = (k & 4) ? hi[1] + 0.5f : lo[1];
            c[k] = {mt.r0.x * lx + mt.r0.y * ly + mt.r0.z * lz + v.pos.x, mt.r1.x * lx + mt.r1.y * ly + mt.r1.z * lz + v.pos.y,
                    mt.r2.x * lx + mt.r2.y * ly + mt.r2.z * lz + v.pos.z};
        }
    }
    const int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    if (!brackets) {
        for (auto& ed_ : e) add_line(L, c[ed_[0]], c[ed_[1]], lc);
        return;
    }
    // A unit or building with its model: corner brackets (RTS style) so the model shows through. The full wire box hid a
    // 'Mech seen from the default camera: two selected Lokis read as "yellow wireframe boxes, the model did not load"
    //.
    auto lerp = [](const XMFLOAT3& a, const XMFLOAT3& b, float t) { return XMFLOAT3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
    for (auto& ed_ : e) {
        const XMFLOAT3 &a = c[ed_[0]], &b = c[ed_[1]];
        add_line(L, a, lerp(a, b, 0.25f), lc);
        add_line(L, b, lerp(b, a, 0.25f), lc);
    }
}


// Side colours (the same as the Units tab "Placing for" picker): missions 5 enemy red, 6 player's
// side blue; multiplayer 1 Red team, 2 Blue team; neutral / other grey. Line colours are ABGR.
static uint32_t side_color(int nation, bool mp) {
    bool red = mp ? nation == 1 : nation == 5, blue = mp ? nation == 2 : nation == 6;
    return red ? 0xFF5A5AFFu : blue ? 0xFFFFA06Eu : 0xFFC8C8C8u;
}

static bool side_unit(uint32_t cls) {
    return cls == K_MECH || cls == K_TANK || cls == K_VEHICLE || cls == K_FLYER || cls == K_TURRET || cls == K_PERSON;
}

// The shot glyphs: while a cutscene is picked, each shot draws a small frustum (4 m) at its start and its end eye point,
// pointing at what it looks at, labelled "2a" / "2b" ("2" for a still shot). The labels are drawn by shot_glyph_labels.
void App::shot_glyph_lines() {
    shot_glyphs_.clear();
    if (!doc_ || tab_ != Tab::Cameras || !cine_uid_ || cpv_.on || !show_helpers_ || !show_helper_group_[HG_CAMERAS]) return;
    const std::vector<ShotInfo> shots = cutscene_shots(doc_->world, cine_uid_);
    for (size_t k = 0; k < shots.size(); k++) {
        const ShotInfo& sh = shots[k];
        if (sh.eye.empty() || !sh.has_look) continue;
        const bool sel = int(k) == tl_.shot;
        for (int end = 0; end < (sh.still ? 1 : 2); end++) {
            const Vec3 e = end ? sh.eye.back() : sh.eye.front();
            float fx = sh.look.x - e.x, fy = sh.look.y - e.y, fz = sh.look.z - e.z;
            const float d = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (d < 0.05f) continue;
            fx /= d; fy /= d; fz /= d;
            float rx = fz, rz = -fx;  // to the right of the view direction, level
            float rl = std::sqrt(rx * rx + rz * rz);
            if (rl < 1e-3f) { rx = 1; rz = 0; rl = 1; }
            rx /= rl; rz /= rl;
            const float ux = fy * rz, uy = fz * rx - fx * rz, uz = -fy * rx;  // up = forward x right
            const float hw = 4.f * std::tan(std::clamp(sh.fov, 5.f, 120.f) * 0.5f * 0.0174532925f), hh = hw / kGameBandAspect;
            const XMFLOAT3 c{e.x + fx * 4.f, e.y + fy * 4.f, e.z + fz * 4.f};
            XMFLOAT3 q[4];
            for (int i = 0; i < 4; i++) {
                const float sx = (i & 1) ? hw : -hw, sy = (i & 2) ? hh : -hh;
                q[i] = {c.x + rx * sx + ux * sy, c.y + uy * sy, c.z + rz * sx + uz * sy};
            }
            const uint32_t col = sel ? kCamPathColor : (kCamPathColor & 0x00FFFFFFu) | 0xB0000000u;
            const XMFLOAT3 eye{e.x, e.y, e.z};
            for (int i = 0; i < 4; i++) add_line(r_.lines, eye, q[i], col);
            add_line(r_.lines, q[0], q[1], col);
            add_line(r_.lines, q[1], q[3], col);
            add_line(r_.lines, q[3], q[2], col);
            add_line(r_.lines, q[2], q[0], col);
            float sx, sy;
            if (cam_.project(eye, vp_w, vp_h, sx, sy)) {
                ShotGlyph g;
                g.label = std::to_string(k + 1) + (sh.still ? "" : end ? "b" : "a");
                g.sx = sx;
                g.sy = sy;
                g.uid = sh.ctrl;
                shot_glyphs_.push_back(std::move(g));
            }
        }
    }
}

void App::build_frame_geometry() {
    r_.boxes.clear();
    r_.lines.clear();
    r_.flames.clear();
    r_.quads.clear();
    r_.models.clear();
    r_.sky = {};
    r_.env_cube = nullptr;
    r_.detail = {};
    if (!doc_) return;
    // Terrain detail layers named by MATerrain (from the Base archive, which the game mounts).
    if (int ti = find_class(doc_->world, K_TERRAIN); ti >= 0) {
        const WObj& t = doc_->world.objs[size_t(ti)];
        static const char* maps[4] = {"TerrainDetailMap", "TerrainDetailMap 1", "TerrainMicroDetailMap0", "TerrainMicroDetailMap1"};
        static const char* reps[4] = {"TerrainDetailMult", "TerrainDetailMult1", "TerrainMicroDetailTiling0", "TerrainMicroDetailTiling1"};
        for (int k = 0; k < 4; k++) {
            std::string mp = t.str(maps[k]);
            r_.detail.tex[k] = mp.empty() ? nullptr : models_.texture(mp);
            r_.detail.rep[k] = t.f32(reps[k], k < 2 ? 64.f : 240.f);
        }
        r_.detail.blend = t.f32("TerrainDetailBlend", 0.3f);
        r_.detail.modulate = t.find("TerrainDetailModulate") ? t.i32("TerrainDetailModulate") != 0 : true;
    }
    // MAEnvironment lighting and fog (View > Game lighting / Fog).
    if (int ei = find_class(doc_->world, K_ENV); ei >= 0) {
        const WObj& e = doc_->world.objs[size_t(ei)];
        auto rgbf = [](uint32_t c) { return XMFLOAT3{float((c >> 16) & 255) / 255, float((c >> 8) & 255) / 255, float(c & 255) / 255}; };
        r_.params.ambient = rgbf(e.u32("LightAmbientColor", 0xFF202028u));
        r_.params.night_time = e.i32("IsNightTime", 1) != 0;
        r_.params.sun = e.i32("DirectionalLightOn", 1) ? rgbf(e.u32("DirectionalLightColor", 0xFF595959u)) : XMFLOAT3{0, 0, 0};
        Vec3 d = e.find("DirectionalLightDirection") ? e.vec3("DirectionalLightDirection") : Vec3{1, -1, 1};  // game default (1,-1,1), batch 04
        r_.params.sun_dir = {d.x, d.y, d.z};
        r_.params.fog_color = rgbf(e.u32("FogColor", 0xFF808080u));
        r_.params.fog_start = e.f32("FogStart", 0) * fog_scale_;
        r_.params.fog_end = e.f32("FogEnd", 0) * fog_scale_;
        // The game's fog setter (sub_0003AEB0, research batches 04/14): mode 1 = linear over FogStart/FogEnd, modes 2-3 = a fixed
        // 0..1 range (Start/End ignored, source of the factor still open: kept as distance fog here), any other mode = fog off.
        if (int fm = e.i32("FogMode", 1); fm < 1 || fm > 3) r_.params.fog_end = r_.params.fog_start;
        r_.params.fog_height = e.i32("FogMode", 1) == 2;  // VS dump (Deliverance): mode 2 adds a height term to the distance fog
        r_.params.fog_top = e.f32("FogTop", 0);
        r_.params.fog_bottom = e.f32("FogBottom", 0);
        std::string env = e.str("EnvironmentMapName");
        env_map_name_ = env;
        if (!env.empty()) {
            r_.env_cube = models_.cube(env);
            if (!r_.env_cube) r_.env_cube = models_.cube("EnvMaps/" + norm_path(env).substr(norm_path(env).find_last_of('/') + 1));
        }
    }
    if (show_sky_ && show_models_) {
        int si = find_class(doc_->world, K_SKY);
        if (si >= 0) {
            const WObj& o = doc_->world.objs[size_t(si)];
            std::string mp = o.str("Corporal3DObj");
            GpuModel* g = mp.empty() ? nullptr : models_.get(mp, o.str("CorporalMTB"));
            if (g && g->state == GpuModel::Ready) {
                Vec3 sc = o.find("SkyTerrainSkyScale") ? o.vec3("SkyTerrainSkyScale") : Vec3{400, 400, 400};
                Vec3 off = o.vec3("SkyTerrainSkyOffset");
                r_.sky.model = g;
                r_.sky.scale = {sc.x, sc.y, sc.z};
                r_.sky.offset = {off.x, off.y, off.z};
                // The game does NOT turn the dome by the MASky2's CorporalHeading. NOR_02 trace (heading -41, PC Release, p10 spot A): the
                // dome's model matrix is 10000 x the MGMODEL node world exactly (env_skynor_01 turned 1.04 rad by its own node, nothing more), and at the
                // traced camera the editor matches the game only without the heading (sky region 127 vs game 125; with the heading 174, the 1.35x).
                r_.sky.heading = 0;
                // Ill Wind's sky has no SkyFogOn / SkyFogColor and the game still fogs it in the world's FogColor.
                // WC trace (Env_Sky_Stormy): the game frame draws only the 3 alpha-blended groups (sky, clouds, mountains); the two additive duplicate domes sky_main01/02 (colour 1.9) never reach the screen, and drawing them made the sky ~5x too bright.
                r_.sky.hide_additive = mp.find("Env_Sky_Stormy") != std::string::npos;
                r_.sky.fog_on = o.i32("SkyFogOn", 1) != 0 && o.find("SkyFogTop") && o.find("SkyFogBottom");
                {
                    int ei2 = find_class(doc_->world, K_ENV);
                    uint32_t c = o.find("SkyFogColor") ? o.u32("SkyFogColor", 0xFF808080u) : ei2 >= 0 ? doc_->world.objs[size_t(ei2)].u32("FogColor", 0xFF808080u) : 0xFF808080u;
                    r_.sky.fog_color = {float((c >> 16) & 255) / 255, float((c >> 8) & 255) / 255, float(c & 255) / 255};
                }
                r_.sky.fog_top = o.f32("SkyFogTop", 0);
                r_.sky.fog_bottom = o.f32("SkyFogBottom", 0);
            }
        }
    }
    r_.lights.clear();
    if (r_.params.scene_lights) {
        // MAPointLight objects: LightColor (ARGB) and LightRadius, a point light with a ground pool.
        static const uint32_t K_POINTLIGHT = key_of("MAPointLight");
        for (const WObj& o : doc_->world.objs) {
            if (o.cls != K_POINTLIGHT) continue;
            Vec3 p;
            if (!get_pos(o, p)) continue;
            if (!o.i32("LightOn", 1)) continue;  // LightOn 0 (M01b has one): the game starts it off (MAActionLights turns it on)
            Renderer::SceneLight l;
            uint32_t c = o.u32("LightColor", 0xFFFFFFFFu);
            l.pos = {p.x, p.y, p.z};
            l.color = {float((c >> 16) & 255) / 255, float((c >> 8) & 255) / 255, float(c & 255) / 255};
            l.outer = o.f32("LightRadius", 10);  // lights the objects around it
            // The ground pool is a separate thing (wiki pointlight-ground-splat, verified in game): only LightMap=1 adds a disc to the
            // ground light map, of radius LightMapRadius (retail M03: LightRadius 60, LightMapRadius 20) in LightMapColor at alpha
            // 0x60 (the node lights' pools use alpha 1/3 and the same x2 modulate, so the colour is scaled by 0x60/255 / (1/3)).
            // Retail lights without LightMap (3 of 21 on M03, 5 of 5 on M01b) leave the ground alone.
            if (o.i32("LightMap", 0)) {
                uint32_t pc = o.u32("LightMapColor", c);
                float k = (96.f / 255.f) * 3.f;
                l.min_r = l.max_r = o.f32("LightMapRadius", 20);
                l.pool_min = l.pool_max = {float((pc >> 16) & 255) / 255 * k, float((pc >> 8) & 255) / 255 * k, float(pc & 255) / 255 * k};
            }
            r_.lights.push_back(l);
        }
    }
    std::map<std::tuple<const GpuModel*, bool, uint32_t>, size_t> batch;
    std::set<int> sel_roots;
    std::string box_list;
    struct DumpBoxes {
        const std::string& path;
        const std::string& text;
        ~DumpBoxes() {
            if (path.empty()) return;
            if (FILE* f = fopen(path.c_str(), "w")) { fputs(text.c_str(), f); fclose(f); }
        }
    } dump_boxes{dump_boxes_, box_list};
    struct DumpLights {
        const std::string& path;
        const std::vector<Renderer::SceneLight>& lights;
        ~DumpLights() {
            if (path.empty()) return;
            FILE* f = fopen(path.c_str(), "w");
            if (!f) return;
            for (const auto& l : lights)
                fprintf(f, "%.1f %.1f %.1f col %.2f %.2f %.2f r %.1f-%.1f spot %d dir %.2f %.2f %.2f cos %.2f-%.2f pool h %.1f-%.1f r %.1f-%.1f\n", l.pos.x, l.pos.y, l.pos.z,
                        l.color.x, l.color.y, l.color.z, l.inner, l.outer, l.spot ? 1 : 0, l.dir.x, l.dir.y, l.dir.z, l.cos_in, l.cos_out, l.min_h, l.max_h, l.min_r, l.max_r);
            fclose(f);
        }
    } dump_lights{dump_lights_, r_.lights};
    for (auto& v : views_) if (selection_.count(doc_->world.objs[size_t(v.root)].uid())) sel_roots.insert(v.root);
    helpers_faded_ = 0;
    for (auto& v : views_) {
        bool sel = sel_roots.count(v.root) != 0;
        bool hov = hover_uid_ && doc_->world.objs[size_t(v.root)].uid() == hover_uid_;
        float ch = std::cos(v.heading), sh = std::sin(v.heading);
        bool drawn_model = false;
        if ((fade_blockers_ || g_cam_hold) && blocks_camera(v)) continue;  // g_cam_hold = a game camera pose replayed with --cam-eye; free orbit / pan never hides walls (View > Walls that block the camera is off by default)
        // Side ring under every unit ('Mech, vehicle, turret, soldier) in its side colour (an editor overlay: H hides it).
        if (show_helpers_ && v.index == v.root && v.index >= 0) {
            const WObj& so = doc_->world.objs[size_t(v.index)];
            if (side_unit(so.cls) && so.find("Nation")) {
                uint32_t c = side_color(so.i32("Nation"), doc_->multiplayer);
                float r = std::max(2.f, 0.6f * std::max(v.size.x, v.size.z)), mk = marker_scale();
                r = std::max(r, 2.5f * mk);  // stays readable when zoomed out (like the U7 markers)
                const int N = 32;
                // On a roof or bridge deck the ring lies flat at the unit's feet, not on the ground below.
                float foot = v.pos.y + std::min(0.f, v.lo[1]);
                bool raised = foot - doc_->terrain.height_at(v.pos.x, v.pos.z) > 1.f;
                for (int rep_ = 0; rep_ < 2; rep_++)
                    for (int k = 0; k < N; k++) {
                        float rr = r + rep_ * 0.25f * mk, a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
                        XMFLOAT3 p0{v.pos.x + std::cos(a0) * rr, 0, v.pos.z + std::sin(a0) * rr}, p1{v.pos.x + std::cos(a1) * rr, 0, v.pos.z + std::sin(a1) * rr};
                        p0.y = (raised ? foot : doc_->terrain.height_at(p0.x, p0.z)) + 0.4f;
                        p1.y = (raised ? foot : doc_->terrain.height_at(p1.x, p1.z)) + 0.4f;
                        add_line(r_.lines, p0, p1, c);
                    }
            }
        }
        // Pad / power-up ring (label in gizmo_draw): yellow power-ups, teal pads; grows with distance like the spawn markers.
        if (show_helpers_ && !v.tag.empty() && v.index == v.root) {
            bool pad = v.tag.rfind("Pad: ", 0) == 0;
            float mk = marker_scale(), r = std::max(pad ? 6.f : 2.5f, (pad ? 3.f : 2.f) * mk);
            uint32_t c = sel ? 0xFF30FFFFu : pad ? 0xFFDCDC3Cu : 0xFF3CE6FAu;
            const int N = 32;
            for (int rep_ = 0; rep_ < 2; rep_++)
                for (int k = 0; k < N; k++) {
                    float rr = r + rep_ * 0.25f * mk, a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
                    XMFLOAT3 p0{v.pos.x + std::cos(a0) * rr, 0, v.pos.z + std::sin(a0) * rr}, p1{v.pos.x + std::cos(a1) * rr, 0, v.pos.z + std::sin(a1) * rr};
                    p0.y = std::max(v.pos.y, doc_->terrain.height_at(p0.x, p0.z)) + 0.4f;
                    p1.y = std::max(v.pos.y, doc_->terrain.height_at(p1.x, p1.z)) + 0.4f;
                    add_line(r_.lines, p0, p1, c);
                }
        }
        if (v.model && v.model->state == GpuModel::Ready) {
            BoxInstance mi;
            orient_instance(mi, v.heading, v.pitch, v.roll, v.pos);
            mi.color = sel ? XMFLOAT4{1.5f, 1.4f, 0.8f, 1} : hov ? XMFLOAT4{1.25f, 1.25f, 1.25f, 1} : XMFLOAT4{1, 1, 1, 1};
            // ShadowReceiverType 1 (every MARoad, the ground slabs): the game pre-passes the ground light map onto it.
            bool recv = v.kind == 5 || (v.index >= 0 && doc_->world.objs[size_t(v.index)].i32("ShadowReceiverType") == 1);
            // Entity colour set (M11b player turret Tur_Player_01, game olive 49682B, not the template red CC0000 nor the material's grey 0.35):
            // a tank / turret with SupportsColorShift + UseCustomColors paints its COMPLEX colorshift1_diffuse tint with
            // CustomSinglePlayerDiffuseColor. Only that verified case: mechs / vehicles with UseCustomColors keep the material constant
            // (their custom colour is the dictionary default CC0000 or a placeholder, game colour unverified).
            uint32_t tint = 0;
            if (v.index >= 0) {
                const WObj& eo = doc_->world.objs[size_t(v.index)];
                if (eo.i32("SupportsColorShift") && eo.i32("UseCustomColors") && eo.find("CustomSinglePlayerDiffuseColor") && eo.u32("CustomSinglePlayerDiffuseColor") != 0xFFCC0000u) {
                    const std::string cn = gd_.names().label(eo.cls);
                    if (cn == "MATank" || cn == "MATurret") tint = eo.u32("CustomSinglePlayerDiffuseColor") | 0xFF000000u;
                }
            }
            auto it = batch.find({v.model, recv, tint});
            if (it == batch.end()) { it = batch.emplace(std::make_tuple(v.model, recv, tint), r_.models.size()).first; r_.models.push_back({v.model, {}, recv, tint}); }
            r_.models[it->second].inst.push_back(mi);
            drawn_model = true;
            // Lights inside the model (lamps): node_light + <lightmap>, turned by the heading.
            if (r_.params.scene_lights)
                for (const auto& ml : v.model->lights) {
                    const auto& L = ml.l;
                    Renderer::SceneLight l;
                    l.pos = {ch * ml.pos[0] + sh * ml.pos[2] + v.pos.x, ml.pos[1] + v.pos.y, -sh * ml.pos[0] + ch * ml.pos[2] + v.pos.z};
                    l.dir = {ch * ml.dir[0] + sh * ml.dir[2], ml.dir[1], -sh * ml.dir[0] + ch * ml.dir[2]};
                    l.color = {L.color[0], L.color[1], L.color[2]};
                    l.inner = L.inner_radius;
                    l.outer = L.outer_radius;
                    l.spot = L.spot;
                    l.cos_in = std::cos(L.inner_angle * 0.0174533f);
                    l.cos_out = std::cos(std::max(L.outer_angle, L.inner_angle + 0.5f) * 0.0174533f);
                    if (L.has_lightmap) {
                        l.min_h = L.min_height; l.max_h = L.max_height; l.min_r = L.min_radius; l.max_r = L.max_radius;
                        // The game's pool colour is min/max_color, not light_color (M02 trace C0 557F7A69 = min_color 127,122,105 of a
                        // 255,245,211 lamp). Env_Highway_Light's "32640" saturates to white (M01b pavement trace, game pre-pass 4C4F5C).
                        l.pool_min = {L.min_color[0], L.min_color[1], L.min_color[2]};
                        l.pool_max = {L.max_color[0], L.max_color[1], L.max_color[2]};
                    }
                    r_.lights.push_back(l);
                }
        }
        if (v.kind == 2) {
            {
                Renderer::Quad q{{v.pos.x, v.pos.y, v.pos.z}, v.size.x * 0.5f, v.size.z * 0.5f, v.color};
                q.heading = v.heading;
                q.oval = ScenarioIO::water_custom_shape(doc_->world.objs[size_t(v.root)]) == wcustom::Shape::Oval;
                q.selected = false;  // selection shows as the outline, centre diamond and gizmo, so the water keeps its game look
                // The game's look: the water model's material_water (deep colour, wave and sky
                // reflection textures); the dictionary colour when the model is not found.
                const WaterMaterial& wm = models_.water_material(doc_->world.objs[size_t(v.root)].str("Corporal3DObj"));
                if (wm.ok) {
                    q.color = (wm.deep & 0x00FFFFFFu) | 0xEB000000u;
                    q.shallow = wm.shallow;
                    q.wave = wm.wave;
                    q.refl = wm.refl ? wm.refl : r_.env_cube;  // no reflectiontexture: the level's env cube
                    water_dbg_[doc_->world.objs[size_t(v.root)].str("Corporal3DObj")] =
                        (wm.refl ? wm.refl_path : std::string("(env cube)")) + " wave=" + wm.wave_path;
                    q.fresnel_min = wm.fresnel_min;
                    q.fresnel_max = wm.fresnel_max;
                }
                r_.quads.push_back(q);
            }
            if (show_helpers_ || sel) {
                // Outline of the tile so water can be seen, hovered and picked at any zoom (H hides it unless this water is selected).
                bool wh = hov || sel;
                uint32_t oc = sel ? 0xFF30FFFFu : hov ? 0xFFFFFFA0u : 0x70E0B070u;
                float hx = v.size.x * 0.5f, hz = v.size.z * 0.5f;
                auto at = [&](float lx, float lz) { return XMFLOAT3{v.pos.x + ch * lx + sh * lz, v.pos.y + 0.2f, v.pos.z - sh * lx + ch * lz}; };
                if (ScenarioIO::water_custom_shape(doc_->world.objs[size_t(v.root)]) == wcustom::Shape::Oval) {
                    const int N = 48;
                    for (int rep_ = 0; rep_ < (wh ? 2 : 1); rep_++)
                        for (int k = 0; k < N; k++) {
                            float a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
                            add_line(r_.lines, at(std::cos(a0) * hx, std::sin(a0) * hz), at(std::cos(a1) * hx, std::sin(a1) * hz), oc);
                        }
                } else {
                    XMFLOAT3 c4[4];
                    for (int k = 0; k < 4; k++) c4[k] = at((k & 1) ? hx : -hx, (k & 2) ? hz : -hz);
                    const int e4[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
                    for (int rep_ = 0; rep_ < (wh ? 2 : 1); rep_++)
                        for (auto& e_ : e4) add_line(r_.lines, c4[e_[0]], c4[e_[1]], oc);
                }
            }
            continue;
        }
        if (v.kind == 5) {  // road piece without its model: a grey strip from the start point along local -z
            if (!drawn_model) {
                float cx = v.pos.x - sh * v.road_l * 0.5f, cz = v.pos.z - ch * v.road_l * 0.5f;
                BoxInstance b;
                b.r0 = {ch * v.size.x, 0, sh * v.size.z, cx};
                b.r1 = {0, v.size.y, 0, v.pos.y};
                b.r2 = {-sh * v.size.x, 0, ch * v.size.z, cz};
                b.color = color4(sel ? 0xFF60A0A0u : v.color, hov ? 1.3f : 1.f);
                r_.boxes.push_back(b);
            }
            if (sel && v.index == v.root) { add_selection_box(r_.lines, v, 0xFF30FFFFu); crop_draw_full(v); }  // The outline is on the piece itself
            continue;
        }
        // Spawn and path markers grow with the camera distance so they stay visible and clickable.
        float mk = (v.kind == 1 || v.kind == 4) ? marker_scale() : 1.f;
        BoxInstance b;
        b.r0 = {ch * v.size.x * mk, 0, sh * v.size.z * mk, v.pos.x};
        b.r1 = {0, v.size.y * mk, 0, v.pos.y};
        b.r2 = {-sh * v.size.x * mk, 0, ch * v.size.z * mk, v.pos.z};
        b.color = color4(v.color, sel ? 1.6f : hov ? 1.3f : 1.f);
        if (v.kind == 3 && v.radius <= 0) b.color.w = 0.35f;
        if (v.kind == 1 && !sel) {  // A start-point slab fades to 35 % while the camera is within 20 m of it (the player's view with helpers on)
            const ma2::Vec3 ce = cam_eye_now();
            if ((ce.x - v.pos.x) * (ce.x - v.pos.x) + (ce.y - v.pos.y) * (ce.y - v.pos.y) + (ce.z - v.pos.z) * (ce.z - v.pos.z) < 20.f * 20.f) {
                b.color.w = 0.35f;
                helpers_faded_++;
            }
        }
        // Effects draw as flame cards; their box only while selected or hovered.
        bool effect = v.index >= 0 && doc_->world.objs[size_t(v.index)].cls == K_EFFECT;
        // With View > Effects on, the object's own particle effects replace the card (once its model has loaded).
        bool has_fx = show_effects_ && !v.fx_model.empty() && models_.effects(v.fx_model, v.fx_mtb) && !models_.effects(v.fx_model, v.fx_mtb)->empty();
        if (effect && !drawn_model && !has_fx) r_.flames.push_back({v.pos.x, v.pos.y, v.pos.z});
        // Objects whose model is \tmpdev\NULL\NULL.NODE are invisible in the game: box only
        // while selected or hovered, like effects.
        bool null_model = v.index >= 0 && lower(doc_->world.objs[size_t(v.index)].str("Corporal3DObj")).find("null.node") != std::string::npos;
        if (!drawn_model && (v.kind != 3 || v.radius <= 0) && (!(effect || null_model) || sel || hov)) r_.boxes.push_back(b);
        if (!dump_boxes_.empty() && !drawn_model && v.helper < 0 && v.index >= 0) {
            const WObj& o = doc_->world.objs[size_t(v.index)];
            const DictClass* dc = dict_.cls(o.cls);
            char line[256];
            snprintf(line, sizeof line, "%08X %s kind %d at %.1f %.1f %.1f model '%s' %s\n", o.cls, dc ? dc->name.c_str() : "?", v.kind,
                     v.pos.x, v.pos.y, v.pos.z, o.str("Corporal3DObj").c_str(), v.model ? "(model not ready)" : "");
            box_list += line;
        }
        uint32_t lc = sel ? 0xFF30FFFFu : 0xC0FFFFFFu;
        if (v.kind == 1) {  // spawn arrow
            float L = 8 * mk, A = 3 * mk, B = 2 * mk;
            XMFLOAT3 a{v.pos.x, v.pos.y + 0.5f, v.pos.z}, f{v.pos.x + sh * L, v.pos.y + 0.5f, v.pos.z + ch * L};
            add_line(r_.lines, a, f, v.color);
            add_line(r_.lines, f, {f.x - sh * A + ch * B, f.y, f.z - ch * A - sh * B}, v.color);
            add_line(r_.lines, f, {f.x - sh * A - ch * B, f.y, f.z - ch * A + sh * B}, v.color);
        }
        if (v.kind == 3 && v.radius > 0) {
            const int N = 48;
            for (int k = 0; k < N; k++) {
                float a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
                XMFLOAT3 p0{v.pos.x + std::cos(a0) * v.radius, 0, v.pos.z + std::sin(a0) * v.radius};
                XMFLOAT3 p1{v.pos.x + std::cos(a1) * v.radius, 0, v.pos.z + std::sin(a1) * v.radius};
                p0.y = doc_->terrain.height_at(p0.x, p0.z) + 1;
                p1.y = doc_->terrain.height_at(p1.x, p1.z) + 1;
                add_line(r_.lines, p0, p1, sel ? lc : v.color);
            }
        }
        if (v.kind == 3 && v.radius <= 0 && v.helper == HG_TRIGGERS) {  // trigger volume: wire box over the fill
            XMFLOAT3 c[8];
            for (int k = 0; k < 8; k++) {
                float lx = (k & 1) ? v.size.x * 0.5f : -v.size.x * 0.5f, lz = (k & 2) ? v.size.z * 0.5f : -v.size.z * 0.5f;
                c[k] = {v.pos.x + ch * lx + sh * lz, v.pos.y + ((k & 4) ? v.size.y : 0.f), v.pos.z - sh * lx + ch * lz};
            }
            const int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (auto& ed_ : e) add_line(r_.lines, c[ed_[0]], c[ed_[1]], sel ? lc : (v.color | 0xFF000000u));
        }
        if (sel && v.index == v.root) { add_selection_box(r_.lines, v, lc, v.model && v.model->state == GpuModel::Ready); crop_draw_full(v); }
    }
    if (show_culture_ && show_models_) add_culture();
    add_effects();
    // Waypoint paths: consecutive waypoints inside a waypoint group.
    {
        const Wdf& w = doc_->world;
        WTree t;
        t.build(w);
        const std::set<uint32_t> cam_groups = cinema_path_groups(w);
        for (size_t i = 0; i < w.objs.size(); i++) {
            if (w.objs[i].cls != K_WPGROUP || !show_helpers_) continue;
            bool cam = camera_path_group(w, t, i, cam_groups);
            if (!show_helper_group_[cam ? HG_CAMERAS : HG_PATHS]) continue;
            Vec3 prev;
            bool have = cam && path_owner_point(w, t, int(i), prev);  // The game's camera path starts at the group's owner
            for (int k : t.kids[i]) {
                Vec3 p;
                if (!get_pos(w.objs[size_t(k)], p)) continue;
                if (have) add_line(r_.lines, {prev.x, prev.y + 1, prev.z}, {p.x, p.y + 1, p.z}, cam ? kCamPathColor : 0xFFFF60C0u);
                prev = p;
                have = true;
            }
        }
    }
    // Camera look direction: each shot (MAFreeCameraController) has an eye-point step (type 2, eyeUID) and a
    // focus-point step (type 3, focusUID), each an MAPath (its waypoint group) or an object. Every eye point gets a
    // tick towards the focus point it looks at (paths matched point for point by their share of the way).
    if (show_helpers_ && show_helper_group_[HG_CAMERAS]) {
        static const uint32_t K_CAMCTRL = key_of("MAFreeCameraController"), K_CAMPROP = key_of("MACameraProperty"), K_MAPATH = key_of("MAPath");
        const Wdf& w = doc_->world;
        WTree t;
        t.build(w);
        auto points = [&](uint32_t uid) {
            std::vector<Vec3> v;
            int k = uid ? t.find(uid) : -1;
            if (k < 0) return v;
            if (w.objs[size_t(k)].cls == K_MAPATH) k = t.find(w.objs[size_t(k)].u32("m_wptGroupUID"));
            if (k < 0) return v;
            if (w.objs[size_t(k)].cls == K_WPGROUP) {
                if (Vec3 o; path_owner_point(w, t, k, o)) v.push_back(o);
                for (int c : t.kids[size_t(k)]) { Vec3 p; if (get_pos(w.objs[size_t(c)], p)) v.push_back(p); }
            } else if (Vec3 p; get_pos(w.objs[size_t(k)], p)) {
                v.push_back(p);
            }
            return v;
        };
        for (size_t i = 0; i < w.objs.size(); i++) {
            if (w.objs[i].cls != K_CAMCTRL) continue;
            std::vector<Vec3> eye, focus;
            for (int k : t.kids[i]) {
                const WObj& st = w.objs[size_t(k)];
                if (st.cls != K_CAMPROP) continue;
                if (st.i32("type") == 2 && eye.empty()) eye = points(st.u32("eyeUID"));
                if (st.i32("type") == 3 && focus.empty()) focus = points(st.u32("focusUID"));
            }
            if (eye.empty() || focus.empty()) continue;
            for (size_t e = 0; e < eye.size(); e++) {
                size_t f = focus.size() == 1 ? 0 : size_t(std::lround(double(e) * double(focus.size() - 1) / double(std::max<size_t>(1, eye.size() - 1))));
                Vec3 a = eye[e], b = focus[std::min(f, focus.size() - 1)];
                float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < 0.01f) continue;
                float L = std::min(d, 15.f) / d;
                add_line(r_.lines, {a.x, a.y + 1, a.z}, {a.x + dx * L, a.y + 1 + dy * L, a.z + dz * L}, 0xFF80F0FFu);
            }
        }
    }
    shot_glyph_lines();
    if (tab_ == Tab::Triggers || (show_helpers_ && show_helper_group_[HG_TRIGGERS])) draw_trigger_overlay();
    if (tab_ == Tab::Environment) draw_sun_arrow();
    // Placing a road piece: every open road end gets a ring and an arrow pointing the way the road would continue, so it
    // is clear before the click where the piece can join; the end the ghost is joined to is yellow.
    if (preview_on_ && doc_)
        for (const RoadOpenEnd& e : road_open_ends_) {
            float y = doc_->terrain.height_at(e.x, e.z) + 0.6f, r = e.joined ? 6.f : 4.f;
            uint32_t c = e.joined ? 0xFF30E0FFu : 0xFFFFD060u;
            const int N = 24;
            for (int k = 0; k < N; k++) {
                float a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
                add_line(r_.lines, {e.x + std::cos(a0) * r, y, e.z + std::sin(a0) * r}, {e.x + std::cos(a1) * r, y, e.z + std::sin(a1) * r}, c);
            }
            float L = std::hypot(e.vx, e.vz);
            if (L < 1e-4f) continue;
            float dx = e.vx / L, dz = e.vz / L;
            XMFLOAT3 tip{e.x + dx * r * 2.2f, y, e.z + dz * r * 2.2f};
            add_line(r_.lines, {e.x + dx * r, y, e.z + dz * r}, tip, c);
            add_line(r_.lines, tip, {tip.x - dx * 3 - dz * 2.5f, y, tip.z - dz * 3 + dx * 2.5f}, c);
            add_line(r_.lines, tip, {tip.x - dx * 3 + dz * 2.5f, y, tip.z - dz * 3 - dx * 2.5f}, c);
        }
    // Placement preview (the object about to be placed), tinted green, or red where it cannot go.
    if (preview_on_) {
        float ch = std::cos(ghost_yaw()), sh = std::sin(ghost_yaw());
        const Vec3& p = preview_pos_;
        bool drawn = false;
        if (tool_ == Tool::PlaceObject && show_models_) {
            const TemplateInfo* t = cat_ && place_road_.empty() ? cat_->find(place_template_) : nullptr;
            GpuModel* gm = t && !t->model.empty() ? models_.get(t->model, t->mtb) : nullptr;
            if (!place_road_.empty())
                if (const RoadPiece* rp = find_road_piece(place_road_)) gm = rp->model.empty() ? nullptr : models_.get(rp->model, "");
            if (gm && gm->state == GpuModel::Ready) {
                BoxInstance mi;
                // A road piece rests at the height the click will give it; on flat ground the ghost sat exactly in the terrain and was hidden, so it floats a hair above.
                float py = p.y;
                const Vec3 po = place_origin_offset();
                // The click puts the model's lowest point on the ground under its origin (lift_to_foot); the ghost sits there too, else
                // models whose origin is high above their base (Wide bridge 01, ~35 m) showed only their top poking out of the ground.
                if (place_road_.empty() && doc_) py = (place_on_top_ ? p.y : surface_at(p.x, p.z)) - gm->bmin[1];  // the surface under the cursor spot (the middle of its base)
                // A foundation: just above the highest ground under it (app_crop.cpp plate_rest_y), as the click will place it.
                const bool plate = place_road_.empty() && t && is_foundation(*t) && !place_on_top_ && gm->bmax[1] - gm->bmin[1] <= 3.f;
                // Roads and thin plates are sheets (app_sheets.cpp): the click's height comes from the same functions.
                bool sheet_y = false;
                if (!place_road_.empty() || plate) {
                    float sy = 0;
                    if (sheet_ghost_y({p.x + po.x, p.y, p.z + po.z}, sy)) { py = sy; sheet_y = true; }
                }
                if (plate && !sheet_y) py = plate_rest_y(gm, p.x + po.x, p.z + po.z, ghost_yaw());
                const bool slope_ghost = slope_plan_.active && sheet_y && (slope_plan_.mode == SlopeMode::Tilt || slope_plan_.mode == SlopeMode::Level ||
                                                                   (slope_plan_.mode == SlopeMode::Float && !slope_plan_.plate && slope_plan_.joined && road_joint_ok_));
                if (slope_ghost) py = slope_plan_.y;  // the height the plan will give (Level: on the levelled ground)
                if (place_road_.empty() && piece_snapped_) py = sheet_y ? py : plate ? std::max(py, snap_origin_y_) : snap_origin_y_;  // a joined wall / bridge piece: at its neighbour's height
                else if (float by; place_road_.empty() && bridge_foot_height(p, by)) py = by;  // a bridge: pillar bases on the spot under the cursor
                preview_y_ = py;
                py += place_lift_;  // PgUp / PgDn: the piece lands this far above the click
                mi.r0 = {ch, 0, sh, p.x + po.x};
                mi.r1 = {0, 1, 0, py};
                mi.r2 = {-sh, 0, ch, p.z + po.z};
                if (slope_ghost && slope_plan_.mode == SlopeMode::Tilt) orient_instance(mi, ghost_yaw(), slope_plan_.pitch, slope_plan_.roll, slope_plan_.pos);  // tilted onto the slope
                mi.color = !preview_valid_ ? XMFLOAT4{1.9f, 0.35f, 0.35f, 1} : snap_.on ? XMFLOAT4{0.55f, 1.45f, 1.6f, 1} : XMFLOAT4{0.7f, 1.5f, 0.7f, 1};
                r_.models.push_back({gm, {mi}});
                drawn = true;
            }
            // A fixed-size water: its flat model mostly hides in sloped ground, so its outline at the surface height
            // the click gives it shows how much of the map it will cover (as Place water's rectangle does).
            if (t && t->cls_name == "MAWater") {
                float fw = 0, fl = 0;
                place_footprint(fw, fl);
                if (fw > 0 && fl > 0) {
                    float hw = fw * 0.5f, hl = fl * 0.5f, y = (water_new_dig_ ? p.y : p.y + 1.f) + 0.2f;
                    uint32_t oc = preview_valid_ ? 0xFFFFC060u : 0xFF4040FFu;
                    auto at = [&](float lx, float lz) { return XMFLOAT3{p.x + ch * lx + sh * lz, y, p.z - sh * lx + ch * lz}; };
                    XMFLOAT3 c4[4] = {at(-hw, -hl), at(hw, -hl), at(hw, hl), at(-hw, hl)};
                    for (int k = 0; k < 4; k++) add_line(r_.lines, c4[k], c4[(k + 1) % 4], oc);
                    if (water_new_dig_ && preview_valid_ && editable()) {  // its basin
                        BasinFoot bf{p.x, p.z, fw, fl, ghost_yaw(), p.y - kShoreDrop, false};
                        ground_preview_ = basin_preview_edit(bf);
                        ground_preview_frame_ = ImGui::GetFrameCount();
                        ground_preview_text_ = ground_edit_note(ground_preview_);
                    }
                }
            }
        }
        if (tool_ == Tool::PlaceWater) {
            // The water about to be placed: the rectangle at its real size, at the ground under the mouse (1 m above it without a basin).
            const bool wdrag = water_drag_ && water_drag_moved_;   // dragging out its size
            const float gw = wdrag ? water_drag_w_ : water_new_w_, gl = wdrag ? water_drag_l_ : water_new_l_;
            float hw = gw * 0.5f, hl = gl * 0.5f, y = water_new_dig_ ? p.y : p.y + 1.f;
            uint32_t wtpl = water_place_tpl();
            if (!wtpl || !water_can_resize(wtpl)) hw = hl = 8.f;
            // The basin the click will dig, outlined on the ground with the buildings and flat pieces it leaves alone.
            if (water_new_dig_ && wtpl && water_can_resize(wtpl) && preview_valid_ && editable()) {
                BasinFoot bf{p.x, p.z, gw, gl, ghost_yaw(), p.y - kShoreDrop, water_new_oval_};
                ground_preview_ = basin_preview_edit(bf);
                ground_preview_frame_ = ImGui::GetFrameCount();
                ground_preview_text_ = ground_edit_note(ground_preview_);
                if (!ground_preview_.kept.empty()) ground_preview_text_ += " The basin stops short of " + fit_summary_names(ground_preview_) + ".";
            }
            // With a basin the surface is the clicked ground itself; lift the ghost a little so the ground does not hide it.
            Renderer::Quad q{{p.x, water_new_dig_ ? y + 0.3f : y, p.z}, hw, hl, preview_valid_ ? 0x90FFA040u : 0x904040FFu};
            q.heading = ghost_yaw();  // Q / E turn it like a unit
            q.oval = water_new_oval_;
            // The ghost wears the real water of the chosen type (deep colour, waves, sky reflection), so the preview is what you will get.
            if (const TemplateInfo* wti = wtpl ? cat_->find(wtpl) : nullptr) {
                const WaterMaterial& wm = models_.water_material(wti->model);
                if (wm.ok && preview_valid_) {
                    q.color = (wm.deep & 0x00FFFFFFu) | 0xC0000000u;
                    q.shallow = wm.shallow;
                    q.wave = wm.wave;
                    q.refl = wm.refl ? wm.refl : r_.env_cube;
                    q.fresnel_min = wm.fresnel_min;
                    q.fresnel_max = wm.fresnel_max;
                }
            }
            r_.quads.push_back(q);
            uint32_t oc = preview_valid_ ? 0xFFFFC060u : 0xFF4040FFu;
            auto at = [&](float lx, float lz) { return XMFLOAT3{p.x + ch * lx + sh * lz, y + 0.2f, p.z - sh * lx + ch * lz}; };
            if (water_new_oval_) {
                const int N = 48;
                for (int k = 0; k < N; k++) {
                    float a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
                    add_line(r_.lines, at(std::cos(a0) * hw, std::sin(a0) * hl), at(std::cos(a1) * hw, std::sin(a1) * hl), oc);
                }
            } else {
                XMFLOAT3 c4[4] = {at(-hw, -hl), at(hw, -hl), at(hw, hl), at(-hw, hl)};
                for (int k = 0; k < 4; k++) add_line(r_.lines, c4[k], c4[(k + 1) % 4], oc);
            }
            drawn = true;
        }
        float mk = marker_scale();
        XMFLOAT3 sz = tool_ == Tool::PlacePad ? XMFLOAT3{8, 0.6f, 8} : tool_ == Tool::PlaceObject ? XMFLOAT3{4, 4, 4} : XMFLOAT3{3 * mk, 3 * mk, 3 * mk};
        if (!drawn) {
            BoxInstance b;
            b.r0 = {ch * sz.x, 0, sh * sz.z, p.x};
            b.r1 = {0, sz.y, 0, p.y};
            b.r2 = {-sh * sz.x, 0, ch * sz.z, p.z};
            b.color = preview_valid_ ? XMFLOAT4{0.3f, 1.f, 0.3f, 0.7f} : XMFLOAT4{1.f, 0.2f, 0.2f, 0.8f};
            r_.boxes.push_back(b);
        }
        if (tool_ != Tool::PlaceWater) {
            uint32_t c = preview_valid_ ? 0xFF40FF40u : 0xFF3030FFu;
            // The ring and arrow wear the side the click will give: the "Placing for" side for units, the team for spawn points.
            if (preview_valid_ && tool_ == Tool::PlaceObject && place_road_.empty() && cat_)
                if (const TemplateInfo* pt = cat_->find(place_template_); pt && side_unit(pt->cls)) {
                    bool mp = doc_->multiplayer, team = pt->cls == K_TURRET || pt->cls == K_PERSON;
                    if (!mp || team) c = side_color(mp ? place_team_ : place_side_, mp);
                }
            if (preview_valid_ && tool_ == Tool::PlaceSpawn) c = spawn_team_ == 1 ? 0xFF5A5AFFu : spawn_team_ == 2 ? 0xFFFFA06Eu : 0xFFF0F0F0u;
            // Facing arrow and a ground ring so the spot reads at any zoom. For an object the ring goes round its footprint
            // (the click is the footprint centre; the model origin sits place_origin_offset() away) and the arrow reaches past
            // it, so a big building's facing still shows.
            float R = 6.f * mk;
            const Vec3 pc = p;
            if (tool_ == Tool::PlaceObject && place_road_.empty())
                if (float fw, fl; place_footprint(fw, fl)) R = std::max(2.f * mk, 0.5f * std::sqrt(fw * fw + fl * fl) + 1.f * mk);  // round the object, not a 12 m ring round a 4 m barricade
            float L = std::max(10 * mk, R + 4 * mk), A = 3 * mk, B = 2 * mk;
            // A road piece runs along its local -z, so its arrow points the way it runs: opposite to the object facing.
            float asn = tool_ == Tool::PlaceObject && !place_road_.empty() ? -1.f : 1.f, ash = sh * asn, ach = ch * asn;
            XMFLOAT3 a{pc.x, p.y + 0.6f, pc.z}, f{pc.x + ash * L, p.y + 0.6f, pc.z + ach * L};
            add_line(r_.lines, a, f, c);
            add_line(r_.lines, f, {f.x - ash * A + ach * B, f.y, f.z - ach * A - ash * B}, c);
            add_line(r_.lines, f, {f.x - ash * A - ach * B, f.y, f.z - ach * A + ash * B}, c);
            const int N = 32;
            for (int k = 0; k < N; k++) {
                float a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N, r = R;
                XMFLOAT3 p0{pc.x + std::cos(a0) * r, 0, pc.z + std::sin(a0) * r}, p1{pc.x + std::cos(a1) * r, 0, pc.z + std::sin(a1) * r};
                p0.y = (place_on_top_ ? p.y : doc_->terrain.height_at(p0.x, p0.z)) + 0.5f;
                p1.y = (place_on_top_ ? p.y : doc_->terrain.height_at(p1.x, p1.z)) + 0.5f;
                add_line(r_.lines, p0, p1, c);
            }
        }
    }
    // Map border (an editor overlay: the white net-like line over terrain on the sweep shots; View > Helpers off hides it).
    float X = doc_->terrain.size_x(), Z = doc_->terrain.size_z();
    const int N = show_helpers_ ? 64 : 0;
    for (int k = 0; k < N; k++) {
        auto edge = [&](float ax, float az, float bx, float bz) {
            add_line(r_.lines, {ax, doc_->terrain.height_at(ax, az) + 0.5f, az}, {bx, doc_->terrain.height_at(bx, bz) + 0.5f, bz}, 0x80FFFFFFu);
        };
        float a = X * k / N, b = X * (k + 1) / N, c = Z * k / N, d = Z * (k + 1) / N;
        edge(a, 0, b, 0); edge(a, Z, b, Z); edge(0, c, 0, d); edge(X, c, X, d);
    }
}

// Overlay colours (ABGR): playable clear, water/special blue, border orange, out of bounds red,
// other retail values purple.
uint32_t type_overlay_color(uint16_t v) {
    switch (v) {
        case tmtype::Playable: return 0x00000000u;
        case tmtype::Special: return 0x90FF9020u;
        case tmtype::DeepWater: return 0xA0B04010u;  // darker blue
        case tmtype::Border: return 0xA000A5FFu;
        case tmtype::OutOfBounds: return 0xA02020E0u;
        default: return 0x90FF40C0u;
    }
}

void App::rebuild_overlay() {
    overlay_dirty_ = false;
    overlay_plants_ = tool_ == Tool::PaintCulture ? plant_pick_ : -1;
    if (doc_ && overlay_plants_ >= 0 && overlay_plants_ < int(culture_.types.size()) && !culture_.map.empty()) {
        // Plants: green as thick as the chosen type's level, so painted ground reads at any zoom.
        const CultureType& ct = culture_.types[size_t(overlay_plants_)];
        const int top = std::max(1, (1 << ct.bits) - 1);
        Image img;
        img.w = culture_.w;
        img.h = culture_.h;
        img.px.resize(size_t(img.w) * img.h);
        for (int y = 0; y < img.h; y++)
            for (int x = 0; x < img.w; x++) {
                int level = (culture_.map[size_t(y) * img.w + x] >> ct.shift) & top;
                uint32_t a = uint32_t(level ? 50 + 90 * level / top : 0);
                img.px[size_t(img.h - 1 - y) * img.w + x] = (a << 24) | 0x0040C060u;
            }
        r_.set_overlay(img);
        r_.params.overlay = true;
        return;
    }
    const bool tm_on = doc_ && !doc_->typemap.empty() && show_typemap_;
    if (!doc_ || (!tm_on && !show_steep_)) { r_.clear_overlay(); r_.params.overlay = false; return; }
    if (show_steep_) {
        // Steep ground: one texel per height cell, yellow over kSteepBot (bots' 'Mechs will not route), orange over kSteepLight (no bot body routes); over the Boundaries colours when both are on.
        const Terrain& t = doc_->terrain;
        Image si;
        si.w = std::max(1, t.w - 1);
        si.h = std::max(1, t.h - 1);
        si.px.assign(size_t(si.w) * size_t(si.h), 0u);
        for (int cz = 0; cz < si.h; cz++)
            for (int cx = 0; cx < si.w; cx++) {
                const float s = cell_slope(t, cx, cz);
                uint32_t c = 0;
                if (s > kSteepLight) c = (120u << 24) | 0x000080FFu;
                else if (s > kSteepBot) c = (85u << 24) | 0x003CE6FFu;
                if (tm_on) {
                    const int tx = std::min(doc_->typemap_w - 1, (cx * 2 + 1) * doc_->typemap_w / (si.w * 2));
                    const int tz = std::min(doc_->typemap_h - 1, (cz * 2 + 1) * doc_->typemap_h / (si.h * 2));
                    const uint32_t b = type_overlay_color(doc_->typemap[size_t(tz) * doc_->typemap_w + tx]);
                    const float ab = float(b >> 24) / 255.f, as = float(c >> 24) / 255.f, ao = as + ab * (1.f - as);
                    if (ao > 0.f) {
                        uint32_t o = uint32_t(ao * 255.f + 0.5f) << 24;
                        for (int k = 0; k < 3; k++) {
                            const float cs = float((c >> (8 * k)) & 255), cb = float((b >> (8 * k)) & 255);
                            o |= uint32_t(std::clamp((cs * as + cb * ab * (1.f - as)) / ao, 0.f, 255.f)) << (8 * k);
                        }
                        c = o;
                    }
                }
                si.px[size_t(si.h - 1 - cz) * size_t(si.w) + size_t(cx)] = c;
            }
        r_.set_overlay(si);
        r_.params.overlay = true;
        return;
    }
    Image img;
    img.w = doc_->typemap_w;
    img.h = doc_->typemap_h;
    img.px.resize(size_t(img.w) * img.h);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            uint16_t v = doc_->typemap[size_t(y) * img.w + x];
            uint32_t c = type_overlay_color(v);
            // Stored row 0 = z 0; the texture's row 0 is the far edge (z max).
            img.px[size_t(img.h - 1 - y) * img.w + x] = c;
        }
    r_.set_overlay(img);
    r_.params.overlay = true;
}

void App::rebuild_minimap() {
    minimap_dirty_ = false;
    if (!doc_) return;
    const int N = 256;
    Image img;
    img.w = img.h = N;
    img.px.resize(size_t(N) * N);
    const Terrain& t = doc_->terrain;
    // The colour tint alone shows ground painted with another texture (Sand ...) as neutral grey. The game shows ground = texture x tint
    // (tint / neutral), so the overview does the same with each texture's average colour (blended by the layer weights, or by the colour map's alpha).
    const GroundLayers& gl = doc_->ground;
    const bool layered = gl.enabled && gl.weights && gl.w > 0 && gl.h > 0 && gl.count() >= 2;
    const int nslots = layered ? std::min(gl.count(), kGroundMaxLayers) : 2;
    float avg[kGroundMaxLayers][3] = {};
    bool have_avg = true;
    for (int k = 0; k < nslots; k++) have_avg = have_avg && ground_texture_average(k, avg[k]);
    const float neutral = std::clamp(1.f / (4.f * std::max(0.05f, r_.detail.blend)), 0.f, 1.f) * 255.f;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            uint32_t c = 0xFF507050u;
            if (!doc_->colormap.px.empty()) c = doc_->colormap.at(x * doc_->colormap.w / N, y * doc_->colormap.h / N);
            if (have_avg && neutral > 1.f) {
                float w[kGroundMaxLayers] = {}, sum = 0;
                if (layered) {
                    const uint8_t* wt = gl.texel(std::min(gl.w - 1, x * gl.w / N), std::min(gl.h - 1, y * gl.h / N));
                    for (int k = 0; k < nslots; k++) { w[k] = float(wt[k]); sum += w[k]; }
                }
                if (sum <= 0.f) { const float a = float(c >> 24) / 255.f; w[0] = a; w[1] = 1.f - a; sum = 1.f; for (int k = 2; k < nslots; k++) w[k] = 0.f; }
                uint32_t o = c & 0xFF000000u;
                for (int ch = 0; ch < 3; ch++) {
                    float tex = 0;
                    for (int k = 0; k < nslots; k++) tex += w[k] * avg[k][ch];
                    tex /= sum;
                    const float tint = float((c >> (8 * ch)) & 255);
                    float v = tex * tint / neutral;
                    if (r_.params.lift_dark) v = std::max(v, 0.75f * tint);   // View > Brighten dark maps: the overview is not murkier than the tint it used to show
                    o |= uint32_t(std::clamp(v, 0.f, 255.f)) << (8 * ch);
                }
                c = o;
            }
            float wx = (x + 0.5f) / N * t.size_x(), wz = (1 - (y + 0.5f) / N) * t.size_z();
            Vec3 n = t.normal_at(wx, wz);
            float l = 0.55f + 0.6f * std::max(0.f, n.x * 0.5f + n.y * 0.7f - n.z * 0.4f);
            uint32_t o = 0xFF000000u;
            for (int k = 0; k < 3; k++) o |= uint32_t(std::clamp(int(float((c >> (8 * k)) & 255) * l), 0, 255)) << (8 * k);
            img.px[size_t(y) * N + x] = o;
        }
    r_.set_minimap(img);
}

// ---------------------------------------------------------------------------------------------
// Frame

void App::frame(float dt, int fb_w, int fb_h) {
    ImGuiIO& io = ImGui::GetIO();
    // MA2SE_ID_AUDIT: remember every item ID conflict the mouse found (window and position).
    if (io.ConfigDebugHighlightIdConflicts && GImGui->DebugDrawIdConflictsId) {
        ImGuiWindow* w = GImGui->HoveredWindow;
        char key[160], t[256];
        snprintf(key, sizeof key, "%s id %08X", w ? w->Name : "?", GImGui->DebugDrawIdConflictsId);
        snprintf(t, sizeof t, "%s (tab %d, mouse %.0f,%.0f)", key, int(tab_), io.MousePos.x, io.MousePos.y);
        bool seen = false;
        for (auto& c : id_conflicts_) seen |= c.compare(0, strlen(key), key) == 0;
        if (!seen && id_conflicts_.size() < 200) id_conflicts_.push_back(t);
    }
    {   // The window title carries the map name and the unsaved mark, so they are readable even when the top strip has no room.
        const std::wstring t = doc_ ? widen(doc_->name) + (doc_->dirty || doc_->file.empty() ? L" *" : L"") + L" - MA2 Scenario Editor" : L"MA2 Scenario Editor";
        if (hwnd_ && t != window_title_) { window_title_ = t; SetWindowTextW(hwnd_, t.c_str()); }
    }
    // First run on a big window (a 3840 x 1600 ultrawide at 100% scaling): start with a larger UI instead of tiny text in narrow
    // strips. Once, only when there was no settings file; Settings > General > UI scale changes it later.
    // Now whenever the window changes size, until the user picks a size (View > Text and button size, Settings).
    {
        auto_fb_w_ = float(fb_w);
        auto_fb_h_ = float(fb_h);
        if (settings_.ui_scale_auto && settings_.ui_scale_run <= 0 && std::fabs(auto_ui_scale() - auto_scale_cur_) > 0.01f) ui_scale_dirty_ = true;
        auto_scale_done_ = true;
    }
    float s = dpi_;
    float menu_h = ImGui::GetFrameHeight();
    float tab_h = ImGui::GetFrameHeight() + 10 * s;
    // A window narrower than 1800 UI pixels gets a two-line status bar: the hint (what a click does) wraps instead of being cut
    // with "...". Fixed by width, not by the text, so the map view never jumps when a message appears.
    status_two_ = float(fb_w) / s < 1800.f;
    // Two lines = both text lines, the space between them and the bar's own padding (it was 2 px short, cutting the second line)
    float status_h = status_two_ ? ImGui::GetTextLineHeight() * 2.f + 7 * s : ImGui::GetFrameHeight() + 4 * s;   // the two lines are 2 s apart (ui_status)
    // The tool strip is at the far left whenever a map is open; the left panel's width is the user's (a splitter drags it).
    const float strip_w = doc_ ? 76 * s : 0.f;
    // A wide window gets a wider left panel by default (the palette then shows more columns): 370 up to 1920 UI pixels, +0.3 per pixel after.
    const float left_default = 370.f + std::clamp((float(fb_w) / s - 1920.f) * 0.3f, 0.f, 260.f);
    float left_w = (settings_.left_panel > 0 ? std::clamp(settings_.left_panel, 300.f, 720.f) : left_default) * s, right_w = 330 * s;
    // A big UI size on a small window must not squeeze the map away: the side panels give up to
    // 30% of their width so the map keeps at least 45% of the window (the strip counts, it does not shrink).
    if (strip_w + left_w + right_w > 0.55f * float(fb_w)) {
        float f = std::max(0.7f, (0.55f * float(fb_w) - strip_w) / (left_w + right_w));
        left_w *= f;
        right_w *= f;
    }
    // Undo baseline for scenario-data widgets: their state before any widget starts editing.
    if (doc_ && !ImGui::IsAnyItemActive()) {
        capture_meta(meta_pre_);
        last_undo_id_ = 0;
    }
    // Autosave (Preferences > General, default every two minutes) while there are unsaved changes (not in the middle of a stroke).
    if (doc_ && doc_->dirty && !doc_->read_only && settings_.autosave_minutes > 0) {
        autosave_timer_ += dt;
        // A widget in use (a long briefing typed into one text box) delays the autosave, but not beyond twice the interval.
        const float due = 60.f * float(settings_.autosave_minutes);
        if (autosave_timer_ > due && (!ImGui::IsAnyItemActive() || autosave_timer_ > 2.f * due) && !stroke_active_ && !dragging_ && !gizmo_drag_ && !place_drag_ && !water_drag_) autosave(false);
    }
    ui_menu();
    ImGui::SetNextWindowPos(ImVec2(0, menu_h));
    ImGui::SetNextWindowSize(ImVec2(float(fb_w), tab_h));
    ui_tabs();
    float top = menu_h + tab_h;
    const float panel_h = float(fb_h) - top - status_h;
    normalise_tab();
    panels_.strip[0] = 0; panels_.strip[1] = top; panels_.strip[2] = strip_w; panels_.strip[3] = panel_h;
    panels_.left[0] = strip_w; panels_.left[1] = top; panels_.left[2] = left_w; panels_.left[3] = panel_h;
    panels_.right[0] = float(fb_w) - right_w; panels_.right[1] = top; panels_.right[2] = right_w; panels_.right[3] = panel_h;
    ImGui::g_tooltip_zones = doc_ ? ImGui::TooltipZones{strip_w + left_w, float(fb_w) - right_w} : ImGui::TooltipZones{};
    if (doc_) ui_strip(0, top, strip_w, panel_h);
    ImGui::SetNextWindowPos(ImVec2(strip_w, top));
    ImGui::SetNextWindowSize(ImVec2(left_w, panel_h));
    {
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        ui_left_panel();
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        const float ms = float(double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(fq.QuadPart));
        frame_ms_ = frame_ms_ <= 0 ? io.DeltaTime * 1000.f : frame_ms_ * 0.9f + io.DeltaTime * 100.f;
        if (stroke_active_) {   // the frames of the stroke in progress (kept after it ends: the numbers of the last stroke)
            if (!perf_in_stroke_) { perf_in_stroke_ = true; perf_stroke_n_ = 0; perf_stroke_frame_ = perf_stroke_brush_ = 0; }
            perf_stroke_n_++;
            perf_stroke_frame_ += io.DeltaTime * 1000.f;
            perf_stroke_brush_ += brush_frame_ms_;
        } else perf_in_stroke_ = false;
        frame_ring_[ring_n_ % 30] = io.DeltaTime * 1000.f;
        brush_ring_[ring_n_ % 30] = brush_frame_ms_;
        brush_frame_ms_ = 0;
        ring_n_++;
        left_panel_ms_ = left_panel_ms_ <= 0 ? ms : left_panel_ms_ * 0.9f + ms * 0.1f;  // What the left panel costs a frame (smoothed)
    }
    ImGui::SetNextWindowPos(ImVec2(float(fb_w) - right_w, top));
    ImGui::SetNextWindowSize(ImVec2(right_w, panel_h));
    ui_right_panel();
    if (doc_) ui_splitter(strip_w + left_w, top, panel_h);
    ImGui::SetNextWindowPos(ImVec2(0, float(fb_h) - status_h));
    ImGui::SetNextWindowSize(ImVec2(float(fb_w), status_h));
    ui_status();
    ui_new_dialog();
    if (show_retail_) ui_retail_dialog();
    if (show_validation_) ui_validation();
    else g_check_rows = -1;
    if (show_help_) ui_help();
    if (show_keys_) ui_keys();
    if (show_about_) ui_about();
    if (show_getting_started_) ui_getting_started();
    ui_confirm();
    ui_neutral_orders();
    ui_preferences();
    ui_message_box();
    poll_test_process();

    vp_x = strip_w + left_w;
    vp_y = top;
    vp_w = std::max(1.f, float(fb_w) - strip_w - left_w - right_w);
    vp_h = std::max(1.f, float(fb_h) - top - status_h);
    // The cutscene strip takes the bottom of the map view (the map, picking and the gizmo all see the smaller vp_h).
    tl_h_ = timeline_height();
    if (tl_h_ > 0) {
        vp_h = std::max(1.f, vp_h - tl_h_);
        timeline_draw();
    } else {
        tl_.rect[2] = tl_.rect[3] = 0;
    }
    // A palette cell dragged onto the map. Its drag source sets pal_drag_live_ every frame it runs; the first frame without
    // it, with the mouse up, is the drop. While it is dragged (and on the drop frame) the map counts as hovered when no window is
    // under the mouse, so the ghost follows the mouse and the release places one there.
    pal_drop_ = false;
    if (pal_drag_live_ && pal_drag_frame_ != ImGui::GetFrameCount()) {
        pal_drop_ = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
        pal_drag_live_ = false;
    }
    const bool over_nothing = ImGui::GetCurrentContext()->HoveredWindow == nullptr;
    vp_hover_ = (!io.WantCaptureMouse || ((pal_drag_live_ || pal_drop_) && over_nothing)) && io.MousePos.x >= vp_x && io.MousePos.y >= vp_y &&
                io.MousePos.x < vp_x + vp_w && io.MousePos.y < vp_y + vp_h;
    dispatch_keys(dt);
    if (doc_) {
        handle_viewport_input(dt);
        if (models_.pump(12.0)) objects_dirty_ = true;
        if (objects_dirty_) rebuild_objects();
        gfx_live_update();
        if (water_fixed_pending_) finish_fixed_water();
        track_water_ground();
        refit_moved_basins();
        if (minimap_dirty_) rebuild_minimap();
        // The Plants tool shows where its plant grows (they draw only near the camera); other tools the type map.
        const int plants_shown = tool_ == Tool::PaintCulture ? plant_pick_ : -1;
        if (plants_shown != overlay_plants_) overlay_dirty_ = true;
        // The Boundaries overlay that came on with the tool goes with it (it hid the ground under Texture / Plants).
        if (typemap_auto_ && tool_ != Tool::PaintType) { show_typemap_ = typemap_auto_ = false; overlay_dirty_ = true; }
        // Steep ground: on while Ramp is armed (and off again with it), else as the View menu / Sculpt page box says.
        if (tool_ != Tool::Ramp) ramp_stage_ = 0;
        if (tool_ == Tool::Ramp && !show_steep_) { show_steep_ = steep_auto_ = true; overlay_dirty_ = true; }
        else if (steep_auto_ && tool_ != Tool::Ramp) { show_steep_ = steep_auto_ = false; overlay_dirty_ = true; }
        else if (!steep_auto_ && show_steep_ != settings_.show_steep) { show_steep_ = settings_.show_steep; overlay_dirty_ = true; }
        if (show_steep_ && stroke_active_ && sculpt_tool(stroke_tool_)) {   // during a stroke: at most four times a second
            steep_acc_ += dt;
            if (steep_acc_ >= 0.25f) { steep_acc_ = 0; overlay_dirty_ = true; }
        } else steep_acc_ = 0;
        if (overlay_dirty_) rebuild_overlay();
        if (cpv_.on) cutscene_preview_step(dt);
        else if (frame_.on) frame_step();
        const bool hold = g_cam_hold || cpv_.on || frame_.on;  // framing a shot keeps the target height too
        if (!hold) cam_.ty += (doc_->terrain.height_at(cam_.tx, cam_.tz) - cam_.ty) * std::min(1.f, dt * 6);
        cam_.update(vp_w / vp_h);
        // Keep the eye above the ground (2 m): tilt up rather than look out from inside a hill.
        for (int k = 0; k < 40 && !hold && cam_.pitch < 1.55f &&
                        cam_.eye.y < doc_->terrain.height_at(cam_.eye.x, cam_.eye.z) + 2.f; k++) {
            cam_.pitch = std::min(1.55f, cam_.pitch + 0.02f);
            cam_.update(vp_w / vp_h);
        }
        build_frame_geometry();
        shot_glyph_labels();
        ui_viewport_hint();
        if (!cpv_.on) {
            gizmo_draw();
            trig_gizmo_draw();
            size_gizmo_draw();
        }
        ui_object_menu();
    } else {
        if (!show_getting_started_ && !show_new_ && !show_retail_) ui_start_page();  // the welcome box / dialogs say the same
    }
    // A greyed control on a real level says how to change it (one shell rule): the line is added under
    // the control's own tip (a second BeginTooltip in a frame appends to the same tooltip), or shown alone.
    if (doc_ && doc_->read_only && GImGui->HoveredIdIsDisabled && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        ImGuiWindow* tw = ImGui::FindWindowByName("##Tooltip_00");
        if (tw && tw->Active) {
            if (ImGui::BeginTooltip()) {
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
                ImGui::TextUnformatted(kReadOnlyReason);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        } else {
            ImGui::SetTooltip("%s", kReadOnlyReason);
        }
    }
    // The "Click each unit ... when done" line goes away with the pick (Esc, right-click, another tool).
    {
        // A walk / patrol route and a camera path (placing points with the waypoint tool) clear their line the same way.
        const bool armed = (pick_.kind == PickTarget::ObjTargets && tool_ == Tool::PickPoint) ||
                           ((pick_.kind == PickTarget::OrderPath || pick_.kind == PickTarget::CamPath) && tool_ == Tool::PlaceWaypoint);
        if (pick_obj_was_ && !armed && status_.find("when done") != std::string::npos) status_time_ = 0;
        pick_obj_was_ = armed;
    }
    if (status_time_ > 0) status_time_ -= dt;
    if (status_time_ <= 0 && !sheet_note_.empty()) { status_ = sheet_note_; status_time_ = 10; sheet_note_.clear(); }
    check_untracked_change();  // a change that set dirty with no undo step: the saved mark can no longer come back
    if (!undo_note_.empty()) {  // "Undo history is full...": says so once, ahead of whatever the action itself reported
        status_ = status_time_ > 0 && !status_.empty() ? undo_note_ + " " + status_ : undo_note_;
        status_time_ = 6;
        undo_note_.clear();
    }
    // A new UI scale takes effect from the next frame (style and fonts are set before it starts).
    if (ui_scale_dirty_) apply_ui_scale();
}

void App::render3d() {
    if (!doc_) return;
    D3D11_VIEWPORT vp{vp_x, vp_y, vp_w, vp_h, 0, 1};
    r_.params.time = float(ImGui::GetTime());
    r_.render(cam_, vp);
}

// ---------------------------------------------------------------------------------------------
// Input

bool App::cursor_ground(Vec3& out) {
    ImGuiIO& io = ImGui::GetIO();
    XMFLOAT3 o, d;
    cam_.ray(io.MousePos.x - vp_x, io.MousePos.y - vp_y, vp_w, vp_h, o, d);
    float t;
    if (!doc_->terrain.raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, t)) return false;
    out = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    return true;
}

// Placing: the top of an object under the cursor (a roof, a bridge deck, a platform) when the
// ray comes down onto it before it reaches the ground, so props can stand on buildings. The ray is tested against the
// model's opaque triangles (a waterfall's see-through sheets or a glass canopy hold nothing up; a box is not enough:
// the Over Falls lake rock's box reaches 24 m over the water and floated bridges there); only a face that looks up
// counts (a wall is no floor).
bool App::cursor_object_top(Vec3& out) {
    ImGuiIO& io = ImGui::GetIO();
    XMFLOAT3 o, d;
    cam_.ray(io.MousePos.x - vp_x, io.MousePos.y - vp_y, vp_w, vp_h, o, d);
    float tg = 1e30f;
    doc_->terrain.raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, tg);
    if (d.y >= -1e-4f) return false;
    float best = tg;
    bool hit = false;
    // Only solid scenery holds things up: an effect's particle box, a pickup or a unit is not a roof.
    static const uint32_t K_POWERUP_ = key_of("MAPowerUp");
    for (auto& v : views_) {
        if (v.kind != 0 || v.helper >= 0 || !v.model || v.pitch != 0 || v.roll != 0) continue;
        if (v.index < 0 || size_t(v.index) >= doc_->world.objs.size()) continue;
        if (uint32_t c = doc_->world.objs[v.index].cls; c == K_EFFECT || c == K_POWERUP_ || side_unit(c)) continue;
        const GpuModel& m = *v.model;
        if (m.state != GpuModel::Ready || !m.has_sbox) continue;
        if (v.pos.y + m.smax[1] - doc_->terrain.height_at(v.pos.x, v.pos.z) < 1.f) continue;  // flat props and ground decals are the ground
        // Ray into the model's frame (heading only).
        float ch = std::cos(v.heading), sh = std::sin(v.heading);
        float ox = o.x - v.pos.x, oz = o.z - v.pos.z;
        float lo_[3] = {ch * ox - sh * oz, o.y - v.pos.y, sh * ox + ch * oz};
        float ld[3] = {ch * d.x - sh * d.z, d.y, sh * d.x + ch * d.z};
        float t0 = 0, t1 = best;
        bool in = true;
        for (int k = 0; k < 3 && in; k++) {
            if (std::fabs(ld[k]) < 1e-6f) { in = lo_[k] >= m.smin[k] && lo_[k] <= m.smax[k]; continue; }
            float a = (m.smin[k] - lo_[k]) / ld[k], b = (m.smax[k] - lo_[k]) / ld[k];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            in = t0 <= t1;
        }
        if (!in) continue;
        // Nearest upward-looking triangle (Moller-Trumbore; either winding).
        const float* tr = m.solid_tris.data();
        for (size_t i = 0; i + 9 <= m.solid_tris.size(); i += 9, tr += 9) {
            float e1[3] = {tr[3] - tr[0], tr[4] - tr[1], tr[5] - tr[2]}, e2[3] = {tr[6] - tr[0], tr[7] - tr[1], tr[8] - tr[2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl < 1e-8f || std::fabs(n[1]) < 0.5f * nl) continue;  // steeper than 60 degrees: a wall
            float p[3] = {ld[1] * e2[2] - ld[2] * e2[1], ld[2] * e2[0] - ld[0] * e2[2], ld[0] * e2[1] - ld[1] * e2[0]};
            float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
            if (std::fabs(det) < 1e-10f) continue;
            float inv = 1.f / det;
            float s_[3] = {lo_[0] - tr[0], lo_[1] - tr[1], lo_[2] - tr[2]};
            float u = (s_[0] * p[0] + s_[1] * p[1] + s_[2] * p[2]) * inv;
            if (u < 0 || u > 1) continue;
            float q[3] = {s_[1] * e1[2] - s_[2] * e1[1], s_[2] * e1[0] - s_[0] * e1[2], s_[0] * e1[1] - s_[1] * e1[0]};
            float w = (ld[0] * q[0] + ld[1] * q[1] + ld[2] * q[2]) * inv;
            if (w < 0 || u + w > 1) continue;
            float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
            if (t <= 0 || t >= best) continue;
            best = t;
            place_on_top_of_ = v.index;
            out = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
            hit = true;
        }
    }
    return hit;
}

int App::pick_object(float mx, float my, std::vector<int>* all) {
    if (objects_dirty_) rebuild_objects();  // views_ index the world: a delete / cut / undo this frame shrank it
    XMFLOAT3 o, d;
    cam_.ray(mx - vp_x, my - vp_y, vp_w, vp_h, o, d);
    float best = 1e30f;
    int best_root = -1;
    std::vector<std::pair<float, int>> hits;  // (distance, root) of every box the ray crosses, for Alt+click cycling
    std::vector<int> firsts;                  // rings / water diamonds: drawn over everything, so they come first
    if (int wm = pick_water(mx, my, 0, true); wm >= 0) { if (!all) return wm; firsts.push_back(wm); }  // a water tile's centre diamond
    // A helper label drawn on the map (Pad: Raptor, a power-up's name) is on top of everything: a click on it picks what it names, not the
    // building behind it. gizmo_draw stores the rectangles it drew last frame.
    if (show_helpers_)
        for (auto& l : label_hits_)
            if (mx >= l.x0 && mx <= l.x1 && my >= l.y0 && my <= l.y1 && l.root >= 0 && size_t(l.root) < doc_->world.objs.size() && doc_->world.objs[size_t(l.root)].uid() == l.uid) {
                if (!all) return l.root;
                firsts.push_back(l.root);
                break;
            }
    // Trigger circles are picked on their ring too, not only on the centre box: where the ray meets the
    // ground, within about 8 pixels of the ring. The ring is drawn over everything, so it wins over
    // buildings in front (the closest ring line when several are near).
    float tg = 0;
    if (doc_->terrain.raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, tg)) {
        float gx = o.x + d.x * tg, gz = o.z + d.z * tg;
        float tol = std::max(1.5f, tg * 8.f / std::max(1.f, vp_h)), near_d = tol;
        int ring = -1;
        for (auto& v : views_)
            if (v.kind == 3 && v.radius > 0) {
                float e = std::fabs(std::hypot(gx - v.pos.x, gz - v.pos.z) - v.radius);
                if (e < near_d) { near_d = e; ring = v.root; }
            }
        if (ring >= 0) { if (!all) return ring; firsts.push_back(ring); }
        // The pad / power-up ring (drawn over everything, app.cpp build_frame_geometry) and the spawn point's facing arrow are helpers on top too.
        if (show_helpers_) {
            const float mk = marker_scale(), ptol = std::max(1.5f, tg * 8.f / std::max(1.f, vp_h));
            int hit = -1;
            float hd = 1e30f;
            for (auto& v : views_) {
                if (v.index != v.root) continue;
                float e = 1e30f;
                if (!v.tag.empty()) {
                    const bool pad = v.tag.rfind("Pad: ", 0) == 0;
                    const float rr = std::max(pad ? 6.f : 2.5f, (pad ? 3.f : 2.f) * mk) + 0.125f * mk;
                    e = std::fabs(std::hypot(gx - v.pos.x, gz - v.pos.z) - rr) - 0.125f * mk;
                } else if (v.kind == 1) {  // the arrow: from the marker along the heading, 8 m (times the marker scale)
                    const float ax = std::sin(v.heading), az = std::cos(v.heading), L = 8.f * mk;
                    const float t = std::clamp(((gx - v.pos.x) * ax + (gz - v.pos.z) * az) / L, 0.f, 1.f) * L;
                    e = std::hypot(gx - (v.pos.x + ax * t), gz - (v.pos.z + az * t));
                }
                if (e < ptol && e < hd) { hd = e; hit = v.root; }
            }
            if (hit >= 0) { if (!all) return hit; firsts.push_back(hit); }
        }
    }
    for (auto& v : views_) {
        if (v.kind == 2) continue;  // water: the diamond above, or its visible surface below
        float ch = std::cos(v.heading), sh = std::sin(v.heading);
        // Ray into the box's local frame.
        float ox = o.x - v.pos.x, oy = o.y - v.pos.y, oz = o.z - v.pos.z;
        float lox = ch * ox - sh * oz, loz = sh * ox + ch * oz;
        float ldx = ch * d.x - sh * d.z, ldz = sh * d.x + ch * d.z;
        float loy = oy, ldy = d.y;
        if (v.pitch != 0 || v.roll != 0) {  // tilted: into the model's frame with the transposed matrix
            BoxInstance mt;
            orient_instance(mt, v.heading, v.pitch, v.roll, {0, 0, 0});
            lox = mt.r0.x * ox + mt.r1.x * oy + mt.r2.x * oz; loy = mt.r0.y * ox + mt.r1.y * oy + mt.r2.y * oz; loz = mt.r0.z * ox + mt.r1.z * oy + mt.r2.z * oz;
            ldx = mt.r0.x * d.x + mt.r1.x * d.y + mt.r2.x * d.z; ldy = mt.r0.y * d.x + mt.r1.y * d.y + mt.r2.y * d.z; ldz = mt.r0.z * d.x + mt.r1.z * d.y + mt.r2.z * d.z;
        }
        float x0 = std::min(v.lo[0], -1.5f), x1 = std::max(v.hi[0], 1.5f), z0 = std::min(v.lo[2], -1.5f), z1 = std::max(v.hi[2], 1.5f);
        float y0 = std::min(v.lo[1], -0.5f), y1 = std::max(v.hi[1], 2.f);
        if (v.kind == 1 || v.kind == 4) {
            float mk = marker_scale();
            x0 *= mk; x1 *= mk; z0 *= mk; z1 *= mk; y1 *= mk;
        }
        if (v.kind == 3 && v.radius > 0) { x1 = z1 = std::max(3.f, v.radius * 0.15f); x0 = z0 = -x1; }
        float t0 = 0, t1 = 1e30f;
        auto slab = [&](float o_, float d_, float lo, float hi) {
            if (std::fabs(d_) < 1e-6f) return o_ >= lo && o_ <= hi;
            float a = (lo - o_) / d_, b = (hi - o_) / d_;
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            return t0 <= t1;
        };
        if (!slab(lox, ldx, x0, x1) || !slab(loy, ldy, y0, y1) || !slab(loz, ldz, z0, z1)) continue;
        // Rank by where the ray passes the middle of the box, not where it enters: a huge box (a canopy, a cliff, a
        // building the camera sits inside) is entered first but its middle is far, so the small object in front of or
        // inside it wins the click. Two separate boxes on the ray keep their front-to-back order.
        float tm = 0.5f * (t0 + t1);
        // The camera itself is inside this box (zoomed into a canopy or a hangar): everything else under the cursor
        // comes first; the box is picked only when nothing else is there (Alt+click still reaches it).
        bool inside = lox >= x0 && lox <= x1 && loy >= y0 && loy <= y1 && loz >= z0 && loz <= z1;
        if (inside) tm += 1e6f;
        // A shot's own camera object sits on its still shot's eye point, but the game flies the eye path:
        // the eye waypoint wins over a camera within a few metres of it on the ray (Alt+click still reaches the camera).
        if (doc_->world.objs[size_t(v.root)].cls == K_FREECAM) tm += 4.f;
        if (all) hits.push_back({tm, v.root});
        if (tm < best) { best = tm; best_root = v.root; }
    }
    if (all) {
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        all->clear();
        auto add = [&](int r) { if (std::find(all->begin(), all->end(), r) == all->end()) all->push_back(r); };
        for (int r : firsts) add(r);
        for (auto& h : hits) add(h.second);
        if (int ws = pick_water(mx, my, best, false); ws >= 0) add(ws);
        return all->empty() ? -1 : all->front();
    }
    if (int ws = pick_water(mx, my, best, false); ws >= 0) return ws;  // the water surface, when nothing nearer is hit
    return best_root;
}

std::vector<int> App::selected_roots() const {
    std::vector<int> out;
    if (!doc_) return out;
    for (size_t i = 0; i < doc_->world.objs.size(); i++) if (selection_.count(doc_->world.objs[i].uid())) out.push_back(int(i));
    return out;
}

// (AoE grammar): Space picks up the last placing tool again after Esc; I arms the Place tool with the
// selected object's own type, so "one more of these" never needs a trip back to the palette.
void App::place_again() {
    if (last_place_was_paste_ && !paste_src_.empty() && tool_ != Tool::PasteGhost) {  // the last thing put down was a paste: its ghost again
        arm_paste(paste_src_, paste_dup_);
        return;
    }
    if (last_place_tool_ == Tool::Select || tool_ == last_place_tool_) return;
    if (last_place_tool_ == Tool::PlaceObject && !place_template_ && place_road_.empty()) return;
    tool_ = last_place_tool_;
    place_drag_ = false;
}

void App::pick_selected_type() {
    std::vector<int> roots = selected_roots();
    const TemplateInfo* t = nullptr;
    if (roots.size() == 1 && cat_) t = cat_->find(doc_->world.objs[size_t(roots[0])].u32("TemplateID"));
    if (!t || !t->placeable || t->model.empty() || t->cls_name == "MASky2") {
        status_ = roots.size() == 1 ? "This object is not in the palette: pick something in the Place tool to place it."
                                    : "Select one placed object first; I then places more of the same.";
        status_time_ = 5;
        return;
    }
    if (t->cls_name == "MAWater" && water_can_resize(t->uid)) {
        water_new_tpl_ = t->uid;
        tool_ = Tool::PlaceWater;
    } else {
        place_template_ = t->uid;
        tool_ = Tool::PlaceObject;
    }
    place_road_.clear();
    place_drag_ = false;
    last_place_tool_ = tool_;
    if (doc_->world.objs[size_t(roots[0])].find("CorporalHeading")) place_heading_ = world::heading(doc_->world.objs[size_t(roots[0])]);  // faces the way the picked one does
}

void App::handle_viewport_input(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (tool_ == Tool::PlaceObject || tool_ == Tool::PlaceSpawn || tool_ == Tool::PlacePad || tool_ == Tool::PlaceWater) {
        last_place_tool_ = tool_;
        last_place_was_paste_ = false;
    }
    // Keys are read once a frame by dispatch_keys (app_input.cpp); the edge scroll below only needs to know they are free.
    bool keys = !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && capture_act_ < 0;
    float fx = std::sin(cam_.yaw), fz = std::cos(cam_.yaw), rx = std::cos(cam_.yaw), rz = -std::sin(cam_.yaw);
    // Edge scrolling while the window is focused. The zones are the client area's edges whatever
    // is drawn there (menu bar, status bar, panels), plus a band just outside the window, so a
    // maximized window scrolls with the mouse on the title bar or the taskbar. Deeper = faster.
    // Paused while a widget is dragged or a dialog is open.
    if (settings_.edge_scroll && (GetForegroundWindow() == hwnd_ || automation_dialogs().active) && keys && !ImGui::IsAnyItemActive() && !box_selecting_ && !dragging_ && !gizmo_drag_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        ImVec2 mp = io.MousePos;
        if (!ImGui::IsMousePosValid(&mp)) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            mp = ImVec2(float(pt.x), float(pt.y));
        }
        float zone = 14.f * dpi_;
        float W = float(rc.right), H = float(rc.bottom);
        // How far past each client edge still counts: 64 px, or up to the monitor's edge when the
        // window sits close to it (a maximized window: title bar above, taskbar below).
        float out_l = 64.f * dpi_, out_r = out_l, out_t = out_l, out_b = out_l;
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            POINT tl{0, 0}, br{rc.right, rc.bottom};
            ClientToScreen(hwnd_, &tl);
            ClientToScreen(hwnd_, &br);
            auto band = [&](float gap, float& out) { if (gap >= 0 && gap <= 160.f * dpi_) out = std::max(out, gap + 2.f); };
            band(float(tl.x - mi.rcMonitor.left), out_l);
            band(float(mi.rcMonitor.right - br.x), out_r);
            band(float(tl.y - mi.rcMonitor.top), out_t);
            band(float(mi.rcMonitor.bottom - br.y), out_b);
        }
        if (mp.x > -out_l && mp.y > -out_t && mp.x < W + out_r && mp.y < H + out_b) {
            // depth 0 at the inner edge of the zone .. 1 at the window edge and beyond.
            auto depth = [&](float d) { return d >= zone ? 0.f : std::clamp(0.35f + 0.65f * (zone - d) / zone, 0.f, 1.f); };
            float l = depth(mp.x), r = depth(W - 1 - mp.x), t = depth(mp.y), b = depth(H - 1 - mp.y);
            float speed = cam_.dist * 1.0f * dt * settings_.edge_speed;
            cam_.tx += (rx * (r - l) + fx * (t - b)) * speed;
            cam_.tz += (rz * (r - l) + fz * (t - b)) * speed;
            edge_scrolling_ = l + r + t + b > 0;
        } else {
            edge_scrolling_ = false;
        }
    } else {
        edge_scrolling_ = false;
    }
    cam_.tx = std::clamp(cam_.tx, -200.f, doc_->terrain.size_x() + 200.f);
    cam_.tz = std::clamp(cam_.tz, -200.f, doc_->terrain.size_z() + 200.f);

    Vec3 g;
    bool have_ground = vp_hover_ && cursor_ground(g);
    // A prop being placed stands on the ground or the paving under it. A roof, deck, rock or tree under the cursor holds it only
    // while the Click through key (Alt) is down (it used to be the other way round, and trees and lamps counted as roofs).
    if (!place_drag_) place_on_top_ = false;  // a drag-aim keeps the spot it started on
    if (have_ground && tool_ == Tool::PlaceObject && place_road_.empty() && editable() && holding(settings_.hold_through) &&
        !holding(settings_.hold_free) && !place_drag_) {
        Vec3 top;
        if (cursor_object_top(top)) { g = top; place_on_top_ = true; }
    }
    // One solver for every joint (core\snap.cpp, app_snap.cpp): a road piece, wall, bridge piece or foundation joins the open end, edge
    // or corner of a placed piece when its own end comes within reach of it, on the screen and in metres. The place-freely key (Shift) turns
    // it off. A drag-aim keeps the joint it started on.
    if (move_snap_.on && !dragging_ && !gizmo_drag_) end_move_snap();   // the move drag ended
    if (!place_drag_) {
        bool joined = false;
        if (have_ground && tool_ == Tool::PlaceObject && editable() && !holding(settings_.hold_free)) {
            const ma2::snap::Mover mv = mover_for_place();
            if (mv.fam) {
                Vec3 c = g;
                float y = place_heading_;
                if (solve_snap(mv, c, y)) { g = c; place_on_top_ = false; joined = true; }
            }
        }
        if (!joined && !move_snap_.on && tool_ != Tool::PasteGhost) snap_clear();   // (a move drag and the paste ghost keep their own joint between frames)
    }
    // The grid (off until the snap bar turns it on) rounds the footprint centre under the cursor to the lattice, for every placing
    // tool. A joint wins over it; the place-freely key (Shift) turns it off while held. The lines are drawn round the cursor while it can act.
    if (!move_snap_.on) {   // (a move drag keeps its label and grid lines on frames where the mouse stands still)
        snap_.grid_label.clear();
        r_.params.snap_grid = {0, 0, 0, 0};
    }
    if (grid_active() && have_ground && editable() &&
        (tool_ == Tool::PlaceObject || tool_ == Tool::PlaceSpawn || tool_ == Tool::PlacePad || tool_ == Tool::PlaceStart || tool_ == Tool::PlaceWater)) {
        const float cell = settings_.snap.grid_m;
        r_.params.snap_grid = {g.x, g.z, cell, 12.f};
        if (!snap_.on) {
            g.x = ma2::snap::grid_round(g.x, cell);
            g.z = ma2::snap::grid_round(g.z, cell);
            if (!place_on_top_) g.y = surface_at(g.x, g.z);  // on a roof: keep its height
            char b[32];
            std::snprintf(b, sizeof b, "Grid %g m", double(cell));
            snap_.grid_label = b;
        }
    }
    // Brush preview.
    bool brush_tool = tool_ >= Tool::Raise && tool_ <= Tool::PaintCulture;  // includes PaintTexture
    if (strength_slot(tool_) >= 0 && strength_slot(tool_) == strength_slot(brush_seen_tool_) && tool_ == brush_seen_tool_) {
        // The strength is remembered per tool: brush_strength_ mirrors this tool's slot.
        float& slot = settings_.tool_strength[strength_slot(tool_)];
        if (slot != brush_strength_) { slot = brush_strength_; strength_dirty_ = true; }
    }
    if (sc_run_ && tool_ != Tool::ScatterTrees) scatter_end();   // the brush was put down mid-stroke: the stroke ends here
    if (tool_ != brush_seen_tool_) {  // The smallest brush depends on what the tool edits (ground points, colour pixels, map cells)
        if (strength_slot(brush_seen_tool_) >= 0 && strength_dirty_) { settings_.save(); strength_dirty_ = false; }
        if (strength_slot(tool_) >= 0) brush_strength_ = settings_.tool_strength[strength_slot(tool_)];
        if ((brush_tool || tool_ == Tool::ScatterTrees) && doc_) brush_size_clamped(tool_, true);
        brush_seen_tool_ = tool_;
    }
    if (have_ground && (brush_tool || tool_ == Tool::ScatterTrees || tool_ == Tool::MakePond)) {
        r_.params.brush = {g.x, g.y, g.z};
        r_.params.brush_radius = tool_ == Tool::MakePond ? pond_radius_ : brush_radius_;
        if (brush_square_ && sculpt_tool(tool_)) r_.params.brush_radius = -brush_radius_;
        r_.params.brush_contour = sculpt_tool(tool_) ? 2.f : 0.f;
        // The ring shows which half of a pair the next (or current) stroke uses.
        const Tool bt = stroke_active_ ? stroke_tool_ : sculpt_now();
        r_.params.brush_tint = sculpt_partner(bt) == bt ? 0.f : bt == Tool::Raise || bt == Tool::Plateau || bt == Tool::Smooth ? 1.f : 2.f;
        if (vp_hover_ && !orbiting_ && !(tool_ == Tool::MakePond)) draw_brush_rings(g);
    } else {
        r_.params.brush_radius = 0;
        if (tool_ == Tool::Ramp && have_ground && vp_hover_ && !orbiting_ && editable()) draw_ramp_preview(g);
    }
    // Placement preview: what the click will place, red where it cannot go.
    preview_on_ = false;
    preview_y_ = std::numeric_limits<float>::quiet_NaN();
    water_over_new_ = false;
    paste_on_ = false;
    if (tool_ == Tool::PasteGhost && have_ground && editable()) { paste_on_ = true; paste_at_ = g; paste_solve(paste_at_); }   // Joins / grid like placing
    bool placing = tool_ == Tool::PlaceObject || tool_ == Tool::PlaceSpawn || tool_ == Tool::PlacePad || tool_ == Tool::PlaceStart || tool_ == Tool::PlaceWater;
    if (placing && have_ground && editable()) {
        preview_on_ = true;
        preview_pos_ = g;
        preview_valid_ = true;
        preview_reason_.clear();
        if (!doc_->terrain.inside(g.x, g.z)) {
            preview_valid_ = false;
            preview_reason_ = "This spot is outside the map.";
        } else if (doc_->multiplayer && (tool_ == Tool::PlaceSpawn || tool_ == Tool::PlacePad)) {
            int v = typemap_at(*doc_, g.x, g.z);
            if (v >= 0 && !tmtype::playable(uint16_t(v))) {
                preview_valid_ = false;
                preview_reason_ = "Players cannot reach this ground (it is marked Border or Out of bounds). Paint it Playable on the "
                                  "Sculpt and Paint tools, or place it further in.";
            }
        } else if (tool_ == Tool::PlaceWater && over_new_water(g)) {
            // Still over the water just placed: no ghost and no second water on top of it (the tool stays armed).
            preview_on_ = false;
            water_over_new_ = true;
        }
    }
    // Sloping ground under a flat piece being placed: what the click will do (app_slope.cpp). Level shows the ground it will change.
    slope_plan_ = SlopePlan{};
    if (preview_on_ && preview_valid_ && tool_ == Tool::PlaceObject) {
        const Vec3 spo = place_origin_offset();
        slope_plan_ = plan_slope({preview_pos_.x + spo.x, preview_pos_.y, preview_pos_.z + spo.z});
        if (slope_plan_.active && slope_plan_.mode == SlopeMode::Level) { ground_preview_ = slope_plan_.edit; ground_preview_frame_ = ImGui::GetFrameCount(); }
    }
    hover_uid_ = 0;
    if (!vp_hover_) { if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { if (sc_run_) scatter_end(); stroke_active_ = false; dragging_ = false; box_selecting_ = false; sel_press_ = false; gizmo_drag_ = 0; orb_press_ = orbiting_ = false; } }
    // Right button: a click without moving stops the tool or clears the selection; a drag of a few
    // pixels pans the map (Preferences > Camera) and does not cancel anything.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { rpress_vp_ = vp_hover_; rdragged_ = false; }
    if (rpress_vp_ && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        if (settings_.right_drag_pan && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.f * dpi_)) {
            rdragged_ = true;
            // The ground follows the mouse: metres per pixel at the camera's distance.
            float k = cam_.dist * 0.0016f * settings_.pan_speed * (settings_.invert_drag_pan ? -1.f : 1.f);
            float dx = io.MouseDelta.x * k, dy = io.MouseDelta.y * k;
            cam_.tx += -rx * dx + fx * dy;
            cam_.tz += -rz * dx + fz * dy;
        }
    }
    if (rpress_vp_ && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        rpress_vp_ = false;
        if (!rdragged_ && vp_hover_) {
            if (tool_ == Tool::Ramp && ramp_stage_ == 1) ramp_cancel(true);   // Drops the ramp's start, the tool stays armed
            else if (tool_ != Tool::Select) tool_ = Tool::Select;
            else {
                // On an object: its menu (an unselected one is selected first, a selected one keeps the whole selection). On empty ground: clear.
                const int h = editable() && !glue_mode_ ? pick_object(io.MousePos.x, io.MousePos.y) : -1;
                if (h >= 0) {
                    const uint32_t u = doc_->world.objs[size_t(h)].uid();
                    if (!selection_.count(u)) selection_ = {u};
                    open_objmenu_ = true;
                    objmenu_uid_ = u;
                } else {
                    selection_.clear();
                }
            }
        }
    }
    if (!vp_hover_ && !dragging_ && !box_selecting_ && !sel_press_ && !stroke_active_ && !gizmo_drag_ && !place_drag_ && !water_drag_ && !orb_press_ && !size_drag_.id) return;
    // Zoom and rotate. In a brush tool Ctrl+wheel is the brush size and Shift+wheel its strength (some mice send Shift+wheel sideways).
    bool wheel_used = false;
    if (vp_hover_ && (active_ctx() & C_BRUSH) && (io.KeyCtrl || io.KeyShift)) {
        const float wheel = io.MouseWheel != 0 ? io.MouseWheel : io.KeyShift && !io.KeyCtrl ? io.MouseWheelH : 0.f;
        const bool strength_ok = tool_ != Tool::PaintType && tool_ != Tool::ScatterTrees && tool_ != Tool::Ramp;
        if (wheel != 0 && (io.KeyCtrl || strength_ok)) {
            if (wheel_acc_ * wheel < 0) wheel_acc_ = 0;  // turned the wheel the other way: start counting again
            wheel_acc_ += wheel;
            while (wheel_acc_ >= 1.f || wheel_acc_ <= -1.f) {
                const int dir = wheel_acc_ > 0 ? 1 : -1;
                if (io.KeyCtrl) brush_size_step(dir); else brush_strength_step(dir);
                wheel_acc_ -= float(dir);
            }
            wheel_used = true;
        }
    }
    if (!wheel_used && io.MouseWheel != 0 && vp_hover_) {
        float w = io.MouseWheel * settings_.zoom_speed * (settings_.invert_zoom ? -1.f : 1.f);
        const float d0 = cam_.dist;
        cam_.dist = std::clamp(cam_.dist * std::pow(0.87f, w), 15.f, 5000.f);
        // Zoom toward the ground under the cursor: scale the camera about that point so it stays
        // under the mouse, like a map in an RTS. Off the map the view zooms about its centre as before.
        Vec3 c;
        if (cursor_ground(c) && d0 > 0) {
            const float k = cam_.dist / d0;
            cam_.tx = c.x + (cam_.tx - c.x) * k;
            cam_.ty = c.y + (cam_.ty - c.y) * k;
            cam_.tz = c.z + (cam_.tz - c.z) * k;
        }
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) turn_view(io.MouseDelta.x, io.MouseDelta.y);
    if (tool_ == Tool::Select && vp_hover_) {
        int h = pick_object(io.MousePos.x, io.MousePos.y);
        if (h >= 0) hover_uid_ = doc_->world.objs[size_t(h)].uid();
    }
    bool lclick = (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || (pal_drop_ && (tool_ == Tool::PlaceObject || tool_ == Tool::PlaceWater))) && vp_hover_;
    bool ldown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool lrelease = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    // Turn the view with left-drag: with the orbit key (Alt) down at the press, the tools do not see the press yet. Past 6 px it
    // turns the view like a middle-drag and the tools never see it; released before that, the tools get an ordinary click on release.
    orb_click_ = false;
    if (lclick && holding(settings_.hold_orbit)) {
        orb_press_ = true;
        orbiting_ = false;
        orb_through_ = holding(settings_.hold_through);
        orb_x_ = io.MousePos.x;
        orb_y_ = io.MousePos.y;
        lclick = false;
    }
    if (orb_press_) {
        if (ldown && !orbiting_) {
            const float dx = io.MousePos.x - orb_x_, dy = io.MousePos.y - orb_y_, th = 6.f * dpi_;
            if (dx * dx + dy * dy >= th * th) { orbiting_ = true; turn_view(dx, dy); }  // the whole way since the press, so no pixel is lost
        } else if (ldown && orbiting_) {
            turn_view(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (!ldown || lrelease) {
            orb_press_ = false;
            if (!orbiting_) { lclick = ldown = orb_click_ = true; lrelease = false; }  // a click: the tools see press now, release next frame
            else ldown = lrelease = false;
            orbiting_ = false;
        } else {
            ldown = lrelease = false;
        }
    }
    const bool through_held = holding(settings_.hold_through) || (orb_click_ && orb_through_);
    // Ramp: click 1 = start, click 2 = end (applied there).
    if (tool_ == Tool::Ramp && editable()) {
        if (lclick && have_ground) ramp_click(g);
        return;
    }
    // Brushes
    if (brush_tool && editable()) {
        if (tool_ != Tool::PaintColor) tint_pick_ = false;
        // Colour tint eyedropper: the button's next click, or Alt+click, picks the ground colour there.
        if (tool_ == Tool::PaintColor && lclick && have_ground && (tint_pick_ || through_held) && !doc_->colormap.px.empty()) {
            const Image& img = doc_->colormap;
            int px = std::clamp(int(g.x / doc_->terrain.size_x() * img.w), 0, img.w - 1);
            int py = std::clamp(int((1 - g.z / doc_->terrain.size_z()) * img.h), 0, img.h - 1);
            uint32_t c = img.at(px, py);
            paint_color_[0] = float(c & 255) / 255; paint_color_[1] = float((c >> 8) & 255) / 255; paint_color_[2] = float((c >> 16) & 255) / 255;
            tint_pick_ = false;
            return;
        }
        // Flatten: Alt+click (or the reverse key + click) only picks the height: no stroke, no undo step.
        if (tool_ == Tool::Flatten && lclick && have_ground && (through_held || holding(settings_.hold_reverse))) {
            flatten_height_ = doc_->terrain.stored(g.y);
            char m[96];
            snprintf(m, sizeof m, "Flatten height set to %.2f m (picked from the ground).", flatten_height_);
            status_ = m;
            status_time_ = 5;
            return;
        }
        if (lclick && have_ground) {
            // Undo/redo names the brush ("Undo: Paint texture"), the tool's status-bar name without its size.
            stroke_tool_ = sculpt_now();
            std::string what = tool_hint_parts(stroke_tool_, ToolHintCtx{}).name;
            push_undo(what.substr(0, what.find(" (")));
            ns_shaped_ = true;
            stroke_active_ = true;
            stroke_held_ = 0;
            // The ground can only be a saved step: a typed 19.95 on a 0.5 m map levels to 20.00, and the boxes say so.
            flatten_height_ = doc_->terrain.stored(flatten_height_);
            plateau_height_ = doc_->terrain.stored(plateau_height_);
            // Review: the plateau height defaulted to 30 m, below the ground on most maps (Mission 05 ~60 m), so a
            // first Plateau stroke did nothing. When it is not above the ground here, build 10 m up from here
            // (Plateau down: not below it, cut 10 m down).
            plateau_auto_ = (stroke_tool_ == Tool::Plateau && plateau_height_ <= g.y + 0.5f) || (stroke_tool_ == Tool::PlateauDown && plateau_height_ >= g.y - 0.5f);
            if (plateau_auto_) plateau_height_ = doc_->terrain.stored(stroke_tool_ == Tool::Plateau ? std::min(g.y + 10.f, doc_->terrain.max_h) : std::max(g.y - 10.f, doc_->terrain.min_h));
            // Shift+click: a straight stroke from where the last stroke of this tool ended to the click, one undo step; the stroke then goes on
            // from the click while the button is held. No previous stroke: an ordinary click.
            const bool line = holding(settings_.hold_free) && last_stroke_ok_[int(tool_)];
            const Vec3 from = line ? last_stroke_end_[int(tool_)] : g;
            // The reverse key on Texture / Boundaries / Plants: the stroke paints the other way, whatever the key does after.
            stroke_rev_ = (tool_ == Tool::PaintTexture || tool_ == Tool::PaintType || tool_ == Tool::PaintCulture) && holding(settings_.hold_reverse);
            brush_t0_ = std::chrono::steady_clock::now();
            apply_brush(from, dt, true);
            if (line) {
                constexpr float kLineSpeed = 40.f;  // m/s the line is drawn at, for the tools that work by time
                brush_path(from, g, std::max(dt, std::sqrt((g.x - from.x) * (g.x - from.x) + (g.z - from.z) * (g.z - from.z)) / kLineSpeed));
            }
            stroke_last_ = g;
            flush_brush();
            brush_frame_ms_ += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - brush_t0_).count();
        } else if (stroke_active_ && ldown && have_ground) {
            stroke_held_ += dt;
            brush_t0_ = std::chrono::steady_clock::now();
            brush_path(stroke_last_, g, dt);
            stroke_last_ = g;
            flush_brush();
            brush_frame_ms_ += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - brush_t0_).count();
        }
        if (stroke_active_ && (lrelease || !ldown)) {
            stroke_active_ = false;
            last_stroke_end_[int(tool_)] = stroke_last_;
            last_stroke_ok_[int(tool_)] = true;
            if (tool_ == Tool::PaintCulture) store_culture();
            if (sculpt_tool(stroke_tool_)) {
                // Review: only Raise / Lower said what a stroke did, so after Flatten or Smooth the status still read
                // the last Lower stroke's 'Lowered the ground by ...'. Every shaping brush says its own result.
                static const char* const did[] = {"Raised", "Lowered", "Flattened", "Smoothed", "Roughened", "Levelled", "Cut down"};
                const Tool st = stroke_tool_;
                const int k = int(st) - int(Tool::Raise);
                const bool up = st == Tool::Raise;
                char m[360];
                // The whole stroke's change (the brush's own number was one frame's step: a 1 s drag that dug 85 m said
                // "Lowered by up to 4.5 m").
                // Measured on the drawn ground (the saved steps); `fine` is the brush's own sub-step remainder, which nothing shows yet.
                float fine = 0;
                if (!undo_.empty() && undo_.back().hgt->size() == doc_->terrain.hgt.size())
                    for (size_t j = 0; j < doc_->terrain.hgt.size(); j++) {
                        const float cur = doc_->terrain.hgt[j], old = (*undo_.back().hgt)[j];
                        stroke_change_ = std::max(stroke_change_, std::fabs(doc_->terrain.stored(cur) - doc_->terrain.stored(old)));
                        fine = std::max(fine, std::fabs(cur - old));
                    }
                const char* limit_note = stroke_clamped_ ? " Some of it stopped at the map's height limit (Advanced > Height range)." : "";
                if (stroke_clamped_ && stroke_change_ < 0.05f && (st == Tool::Raise || st == Tool::Lower))
                    snprintf(m, sizeof m, "The ground here is already at this map's %s height (%.0f m), so it cannot go %s. Change the limit in Advanced > Height range.",
                             up ? "highest" : "lowest", up ? doc_->terrain.max_h : doc_->terrain.min_h, up ? "higher" : "lower");
                else if (stroke_change_ < 0.05f && fine > 1e-4f && (st == Tool::Raise || st == Tool::Lower))
                    snprintf(m, sizeof m, "Nothing changed yet: the ground here moves in steps of %.2f m. Raise the Height per stroke.", doc_->terrain.step());
                else if (stroke_change_ < 0.05f)
                    snprintf(m, sizeof m, "Nothing changed here: the ground is already %s.", st == Tool::Smooth ? "smooth" : st == Tool::PlateauDown ? "at or below that height" : "at that height");
                else if (st == Tool::Raise)
                    snprintf(m, sizeof m, "Raised the ground by up to %.2f m (highest point now %.2f m). Stroke again to raise more; %s undoes it.%s", stroke_change_, stroke_top_, kt(A_UNDO).c_str(), limit_note);
                else if (st == Tool::Lower)
                    snprintf(m, sizeof m, "Lowered the ground by up to %.2f m (lowest point now %.2f m). Stroke again to lower more; %s undoes it.%s", stroke_change_, stroke_bot_, kt(A_UNDO).c_str(), limit_note);
                else if (st == Tool::Roughen)
                    snprintf(m, sizeof m, "Roughened the ground by up to %.2f m. Stroke again for more bumps; %s undoes it.", stroke_change_, kt(A_UNDO).c_str());
                else
                    snprintf(m, sizeof m, "%s the ground: the biggest change at one spot is %.1f m%s. Hold the button and keep dragging for more; %s undoes it.",
                             k >= 0 && k < 7 ? did[k] : "Shaped", stroke_change_,
                             stroke_clamped_ ? " (some of it stopped at the map's height limit: Advanced > Height range)" : "", kt(A_UNDO).c_str());
                status_ = m;
                if (plateau_auto_) {
                    snprintf(m, sizeof m, " Plateau height set to %.1f m (10 m %s where you started); change it on the left.", plateau_height_, st == Tool::Plateau ? "above" : "below");
                    status_ += m;
                }
                status_time_ = 5;
            }
            if (tool_ != Tool::PaintColor && tool_ != Tool::PaintTexture && tool_ != Tool::PaintType && tool_ != Tool::PaintCulture) {
                io_->bake_lightmap(*doc_);
                r_.set_lightmap(doc_->lightmap);
                r_.params.lighting = true;
            }
            if (tool_ == Tool::PaintColor || tool_ == Tool::PaintTexture) r_.set_colormap(doc_->colormap);
            minimap_dirty_ = true;
            objects_dirty_ = true;
            if (show_steep_) overlay_dirty_ = true;
        }
        return;
    }
    if (tool_ == Tool::ScatterTrees && editable()) {
        scatter_frame(g, lclick, ldown, lrelease, have_ground);   // F4 (app_scatter.cpp); an empty brush set ignores clicks
        return;
    }
    // The paste ghost (Ctrl+V / Ctrl+D): one click puts the copies down where the mouse is, then the Select tool is back with them selected.
    if (tool_ == Tool::PasteGhost) {
        if (lclick && have_ground && editable()) {
            paste_clips(paste_src_, paste_on_ ? paste_at_ : g, paste_on_ ? paste_turn_ : paste_heading_, paste_dup_, paste_dup_ || !clipboard_cut_);
            tool_ = Tool::Select;
        }
        return;
    }
    // Place water by dragging: the press is one corner of the rectangle, the cursor the opposite one, in the frame of the Q / E turn;
    // release puts the water, centred in that rectangle, at that size. A click without a drag places the boxes' default size. Fixed types and the
    // other tools keep click-to-place.
    if (tool_ != Tool::PlaceWater) water_drag_ = false;
    if (tool_ == Tool::PlaceWater && editable() && water_can_resize(water_place_tpl())) {
        if (lclick && have_ground && !water_over_new_ && !(preview_on_ && !preview_valid_)) {
            water_drag_ = true;
            water_drag_moved_ = false;
            water_drag_press_ = g;
            water_drag_centre_ = g;
            water_drag_w_ = water_new_w_;
            water_drag_l_ = water_new_l_;
            water_drag_mx_ = ImGui::GetIO().MousePos.x;
            water_drag_my_ = ImGui::GetIO().MousePos.y;
            return;
        }
        if (water_drag_) {
            // The rectangle follows the mouse, on the release frame too (the last mouse position counts).
            ImGuiIO& aio = ImGui::GetIO();
            const float mdx = aio.MousePos.x - water_drag_mx_, mdy = aio.MousePos.y - water_drag_my_;
            if (!water_drag_moved_ && mdx * mdx + mdy * mdy > 36.f * dpi_ * dpi_) water_drag_moved_ = true;   // 6 px: a click is not a drag
            if (water_drag_moved_) {
                XMFLOAT3 ro, rd;
                cam_.ray(aio.MousePos.x - vp_x, aio.MousePos.y - vp_y, vp_w, vp_h, ro, rd);
                if (std::fabs(rd.y) > 1e-4f && (water_drag_press_.y - ro.y) / rd.y > 0) {   // the flat plane through the press, as aiming a piece
                    const float t = (water_drag_press_.y - ro.y) / rd.y;
                    const float cx = ro.x + rd.x * t, cz = ro.z + rd.z * t;
                    const float yaw = ghost_yaw(), ch = std::cos(yaw), sh = std::sin(yaw);
                    const float dx = cx - water_drag_press_.x, dz = cz - water_drag_press_.z;
                    float u = ch * dx - sh * dz, v = sh * dx + ch * dz;   // along the water's own width / length axes
                    auto fit = [&](float a) {
                        float m = std::fabs(a);
                        if (grid_active() && !holding(settings_.hold_free)) m = snap::size_step(m, settings_.snap.grid_m, wcustom::kMinSide);
                        return std::clamp(std::round(m), wcustom::kMinSide, wcustom::kMaxSide);
                    };
                    water_drag_w_ = fit(u);
                    water_drag_l_ = fit(v);
                    u = u < 0 ? -water_drag_w_ : water_drag_w_;
                    v = v < 0 ? -water_drag_l_ : water_drag_l_;
                    water_drag_centre_ = {water_drag_press_.x + ch * u * 0.5f + sh * v * 0.5f, water_drag_press_.y, water_drag_press_.z - sh * u * 0.5f + ch * v * 0.5f};
                }
            }
            if (ldown && !lrelease) {
                if (water_drag_moved_) {
                    char b[80];
                    snprintf(b, sizeof b, "%.0f x %.0f m", water_drag_w_, water_drag_l_);
                    readout_label(b);
                    status_ = std::string("Water ") + b + ": release to place it, right-click to cancel.";
                    status_time_ = 0.5f;
                }
                preview_on_ = true;
                preview_valid_ = true;
                preview_pos_ = water_drag_moved_ ? water_drag_centre_ : water_drag_press_;
                return;
            }
            water_drag_ = false;
            if (water_drag_moved_) place_water(water_drag_centre_, water_drag_w_, water_drag_l_);
            else place_water(water_drag_press_);
            water_drag_moved_ = false;
            return;
        }
    }
    // Placing: press puts the object down, dragging aims it (the arrow follows the mouse), release places it facing
    // that way; a plain click keeps the heading chosen with Q / E. Roads, spawn points, pads and the player start use the same code.
    if ((tool_ == Tool::PlaceObject || tool_ == Tool::PlaceSpawn || tool_ == Tool::PlacePad || tool_ == Tool::PlaceStart) && editable()) {
        if (lclick && have_ground && !(preview_on_ && !preview_valid_)) {
            place_drag_ = true;
            place_drag_pos_ = g;
            // A piece joined to its neighbour (road, wall, bridge, foundation: playtest 11-04 foundations rotated after a click)
            // keeps the join's facing; a drag never re-aims it, else the snapped spot got a heading from mouse jitter.
            place_drag_joined_ = snap_.on;
            place_drag_mx_ = ImGui::GetIO().MousePos.x;
            place_drag_my_ = ImGui::GetIO().MousePos.y;
            return;
        }
        if (place_drag_) {
            if (ldown && !lrelease) {
                // Aim on the flat plane through the press point, not the ground under the cursor: on slopes and in
                // perspective the ground point slides, so a sideways drag gave an odd facing.
                ImGuiIO& aio = ImGui::GetIO();
                XMFLOAT3 ro, rd;
                cam_.ray(aio.MousePos.x - vp_x, aio.MousePos.y - vp_y, vp_w, vp_h, ro, rd);
                if (std::fabs(rd.y) > 1e-4f && (place_drag_pos_.y - ro.y) / rd.y > 0) {
                    const float t = (place_drag_pos_.y - ro.y) / rd.y;
                    g = {ro.x + rd.x * t, place_drag_pos_.y, ro.z + rd.z * t};
                    have_ground = true;
                }
                // A plain click is not a drag: zoomed out, a few pixels of hand jitter are more than 2 m and turned the object
                // away from the heading its ghost showed (playtest 11-04). Aim only once the mouse moved 8 px from the press.
                const float mdx = aio.MousePos.x - place_drag_mx_, mdy = aio.MousePos.y - place_drag_my_;
                const bool moved = mdx * mdx + mdy * mdy > 64.f * dpi_ * dpi_;
                if (have_ground && !place_drag_joined_ && moved) {  // a joined piece keeps the facing of its join (Q / E picks the end / corner)
                    float dx = g.x - place_drag_pos_.x, dz = g.z - place_drag_pos_.z;
                    if (dx * dx + dz * dz > 4.f) place_heading_ = std::atan2(dx, dz) + (tool_ == Tool::PlaceObject && !place_road_.empty() ? 3.14159265f : 0.f);  // forward is (sin h, cos h); a road runs the other way
                    if (settings_.snap.angle && !holding(settings_.hold_free)) place_heading_ = angle_snapped(place_heading_);  // Always in steps while they are on
                    else if (holding(settings_.hold_quarter) && !holding(settings_.hold_free)) place_heading_ = std::round(place_heading_ / 0.2618f) * 0.2618f;  // quarter-turn key: 15-degree steps like Q / E
                }
                preview_pos_ = place_drag_pos_;
                return;
            }
            place_drag_ = false;
            g = place_drag_pos_;
            have_ground = true;
            lclick = true;
        }
    }
    if (lclick && have_ground && editable() && preview_on_ && !preview_valid_) {
        status_ = "Cannot place here: " + preview_reason_;
        status_time_ = 5;
        return;
    }
    if (lclick && have_ground && editable()) {
        switch (tool_) {
            case Tool::PlaceObject:
                {
                    const TemplateInfo* pt = cat_ && place_road_.empty() ? cat_->find(place_template_) : nullptr;
                    push_undo(!place_road_.empty() ? "place road piece" : pt && pt->cls_name == "MAWater" ? "place water" : "place object");
                }
                {
                    if (snap_.on) place_heading_ = ghost_heading_;  // the next piece continues the way this one was joined
                    const Vec3 po = place_origin_offset();
                    Vec3 gp = {g.x + po.x, g.y, g.z + po.z};
                    const size_t before = doc_->world.objs.size();
                    place_object(gp);
                    if (place_lift_ > 0 && doc_->world.objs.size() > before && !selection_.empty()) {  // lifted by PgUp: up by that much
                        mark_selection_fixed();
                        move_selection({0, place_lift_, 0}, false);
                    }
                }
                return;
            case Tool::MakePond:
                make_pond(g);
                return;
            case Tool::PlaceWater:
                if (water_over_new_) {
                    status_ = "That is the water you just placed. Move off it and click to place another, or right-click to stop.";
                    status_time_ = 5;
                } else
                    place_water(g);
                return;
            case Tool::PlaceSpawn: {
                push_undo("place spawn");
                g.y = surface_at(g.x, g.z) + place_lift_;  // on the paving under the click, else the ground (PgUp lifts it)
                uint32_t u = io_->add_spawn(*doc_, spawn_team_, g, place_heading_);
                selection_ = {u};
                world_changed();
                return;
            }
            case Tool::PlacePad: {
                auto& kinds = io_->pad_kinds();
                if (pad_kind_ >= 0 && pad_kind_ < int(kinds.size())) {
                    push_undo("place vehicle pad");
                    g.y = surface_at(g.x, g.z) + place_lift_;
                    uint32_t u = io_->add_pad(*doc_, kinds[size_t(pad_kind_)], g, place_heading_);
                    selection_ = {u};
                    world_changed();
                }
                return;
            }
            case Tool::PlaceStart: {
                push_undo("move player start");
                g.y = surface_at(g.x, g.z);
                bool moved = false;
                if (WObj* o = object_by_uid(doc_->world, player_start_uid(doc_->world))) {  // the 'Player Spawner' the game uses
                    set_pos(*o, {g.x, g.y + 2, g.z});
                    world::set_heading(*o, place_heading_);
                    moved = true;
                    selection_ = {o->uid()};
                }
                if (!moved) selection_ = {io_->add_spawn(*doc_, 0, g, place_heading_)};
                world_changed();
                tool_ = Tool::Select;
                return;
            }
            case Tool::PickPoint: {
                if (pick_.kind == PickTarget::ObjTargets) { pick_objective_target(io.MousePos.x, io.MousePos.y); return; }
                if (pick_.kind == PickTarget::LinkUnit) { pick_link_unit(io.MousePos.x, io.MousePos.y); return; }
                if (pick_.kind == PickTarget::CutUnit) { pick_cut_unit(io.MousePos.x, io.MousePos.y); return; }
                if (pick_.kind == PickTarget::OrderUnit) { pick_order_unit(io.MousePos.x, io.MousePos.y); return; }
                const Tab pick_back = pick_.back;
                if (pick_.kind == PickTarget::MoveShape) {
                    if (WObj* o = object_by_uid(doc_->world, pick_.uid)) {
                        push_undo("move trigger");
                        Vec3 p;
                        get_pos(*o, p);
                        // Keep its height above the ground (a floating box stays floating).
                        set_pos(*o, {g.x, p.y + doc_->terrain.height_at(g.x, g.z) - doc_->terrain.height_at(p.x, p.z), g.z});
                        if (WObj* f = pick_.follow ? object_by_uid(doc_->world, pick_.follow) : nullptr) set_pos(*f, {g.x, g.y, g.z});
                        selection_ = {pick_.uid};
                        world_changed();
                        status_ = "'" + o->description() + "' is here now. Drag its arrows to move it, or its edge to resize it.";  // not the stale "Click the map..."
                        status_time_ = 6;
                    }
                } else if (pick_.kind == PickTarget::LinkMarker) {
                    push_undo("pick a point");
                    uint32_t m = pick_.chain ? make_defend_walk(pick_.uid, g.x, g.z, pick_.name) : set_marker_link(*doc_, pick_.uid, pick_.key.c_str(), g.x, g.z, pick_.null_marker, pick_.name);
                    if (m) selection_ = {m};
                    world_changed();
                }
                pick_ = PickTarget();
                tool_ = Tool::Select;
                tab_ = pick_back;
                return;
            }
            case Tool::PlaceWaypoint:
                if (pick_.kind == PickTarget::OrderPath) {
                    push_undo("path point");
                    order_add_point(*doc_, pick_.uid, g.x, g.z);
                    world_changed();
                } else if (pick_.kind == PickTarget::CamPath) {
                    push_undo("flight path point");
                    if (camera_path_point(*doc_, pick_.uid, g.x, g.z, pick_.restart)) world_changed();
                    pick_.restart = false;
                }
                return;
            default: break;
        }
    }
    if (tool_ != Tool::Select) return;
    // Click through (Alt) + click always cycles the object stack under the cursor, even over a selected object's handles (below).
    const bool through = through_held;
    const bool alt_cycle = lclick && through;
    // The orange size handles of a selected road piece, base, wall or bridge (app_size_gizmo.cpp): before the double-click rule below, so a
    // double-click on a handle makes that side whole again instead of selecting every object of the type.
    if (!alt_cycle && editable() && size_gizmo_update(lclick, ldown)) return;
    // A double-click right after a click that selected an object: every object of that type (before the gizmo, which now sits under the cursor).
    if (lclick && !through && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && last_click_uid_ && !glue_mode_) {
        const int dh = pick_object(io.MousePos.x, io.MousePos.y);
        if (dh >= 0 && doc_->world.objs[size_t(dh)].uid() == last_click_uid_) {
            select_same_type(last_click_uid_, io.KeyCtrl);
            last_click_uid_ = 0;
            return;
        }
    }
    if (glue_pick(lclick)) return;  // the Glue tool (app_glue.cpp)
    if (!alt_cycle && trig_gizmo_update(lclick, ldown)) return;
    if (!alt_cycle && gizmo_update(lclick, ldown)) return;
    // Selection, moving and box selection. The press only records what is under it. A click (released before 6 px) selects: plain
    // = that object alone (empty ground clears), Shift adds, Ctrl toggles. Past 6 px a press on a SELECTED object moves the selection;
    // a press on anything else (empty ground or an unselected object) draws a box: plain replaces the selection, Shift adds, Ctrl removes.
    if (lclick) {
        sel_press_ = dragging_ = box_selecting_ = false;
        std::vector<int> stack;
        int h = pick_object(io.MousePos.x, io.MousePos.y, through ? &stack : nullptr);
        if (through && stack.size() > 1) {
            // Click through cycles through everything under the cursor (draw.io style), past the current selection: the next one after
            // the selected object in the stack, wrapping round to the nearest.
            size_t at = stack.size();
            for (size_t i = 0; i < stack.size(); i++)
                if (selection_.size() == 1 && selection_.count(doc_->world.objs[size_t(stack[i])].uid())) at = i;
            h = stack[at >= stack.size() ? 0 : (at + 1) % stack.size()];
            selection_ = {doc_->world.objs[size_t(h)].uid()};
            drag_start_x_ = io.MousePos.x; drag_start_y_ = io.MousePos.y;
            return;
        }
        drag_start_x_ = io.MousePos.x;
        drag_start_y_ = io.MousePos.y;
        const uint32_t u = h >= 0 ? doc_->world.objs[size_t(h)].uid() : 0;
        last_click_uid_ = 0;
        sel_press_ = true;
        sel_press_uid_ = u;
        sel_press_on_selected_ = u && selection_.count(u);
        sel_press_shift_ = io.KeyShift;
        sel_press_ctrl_ = io.KeyCtrl;
        drag_moved_ = false;
        if (have_ground) drag_last_ = g;
    }
    if (sel_press_ && ldown && !dragging_ && !box_selecting_) {
        const float dx = io.MousePos.x - drag_start_x_, dy = io.MousePos.y - drag_start_y_, th = 6.f * dpi_;
        if (dx * dx + dy * dy >= th * th) {
            if (sel_press_on_selected_ && editable()) dragging_ = true;
            else box_selecting_ = true;
        }
    }
    if (dragging_ && ldown && have_ground) {
        Vec3 d{g.x - drag_last_.x, 0, g.z - drag_last_.z};
        if (std::fabs(d.x) + std::fabs(d.z) > 0.01f) {
            if (!drag_moved_) {
                push_undo("move");
                drag_moved_ = true;
                body_move_ = {0, 0, 0};
                begin_move_snap(sel_press_uid_, drag_last_.x, drag_last_.z);   // The pressed object is the anchor
            }
            if (move_snap_.on) move_snap_step(g.x, g.z);
            else move_selection(d, true);
            drag_last_ = g;
            body_move_.x += d.x; body_move_.z += d.z;
            char rb[80];
            snprintf(rb, sizeof rb, "Move %.1f, %.1f m", body_move_.x, body_move_.z);
            set_readout(rb);
        }
    }
    if (box_selecting_) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRect(ImVec2(drag_start_x_, drag_start_y_), io.MousePos, IM_COL32(255, 220, 60, 255));
        dl->AddRectFilled(ImVec2(drag_start_x_, drag_start_y_), io.MousePos, IM_COL32(255, 220, 60, 30));
    }
    if (lrelease || !ldown) {
        if (sel_press_) {
            const SelMods mods{sel_press_shift_ || io.KeyShift, sel_press_ctrl_ || io.KeyCtrl};
            if (box_selecting_) {
                float x0 = std::min(drag_start_x_, io.MousePos.x), x1 = std::max(drag_start_x_, io.MousePos.x);
                float y0 = std::min(drag_start_y_, io.MousePos.y), y1 = std::max(drag_start_y_, io.MousePos.y);
                std::vector<uint32_t> inside;
                for (auto& v : views_) {
                    if (v.index != v.root) continue;
                    float sx, sy;
                    if (cam_.project({v.pos.x, v.pos.y + 1, v.pos.z}, vp_w, vp_h, sx, sy) && sx + vp_x >= x0 && sx + vp_x <= x1 &&
                        sy + vp_y >= y0 && sy + vp_y <= y1)
                        inside.push_back(v.uid);
                }
                sel_box(selection_, inside, mods);
            } else if (!dragging_) {
                sel_click(selection_, sel_press_uid_, mods);  // released before 6 px: a click
                last_click_uid_ = sel_press_uid_;               // a second click on it at once is a double-click
                if (!sel_press_uid_ && page_ == Page::Water && tab_ == Tab::Build && editable()) {  // The page's own click does not place anything until it is armed
                    status_ = "Nothing is placed by a click yet: press \"Place water on the map\" in the left panel, then click the map.";
                    status_time_ = 6;
                }
                // A still shot's eye is two points 2 cm apart: a plain click on one picks both, so a lift or move keeps
                // the shot still (one point alone would turn it into a 0.5 m camera move).
                if (!mods.shift && !mods.ctrl && selection_.size() == 1 && sel_press_uid_)
                    if (const WObj* wp = object_by_uid(doc_->world, sel_press_uid_); wp && wp->cls == K_WAYPOINT)
                        if (cinema_path_groups(doc_->world).count(wp->parent())) {
                            Vec3 p0, q;
                            std::vector<uint32_t> twins;
                            bool still = get_pos(*wp, p0);
                            for (auto& o : doc_->world.objs)
                                if (still && o.cls == K_WAYPOINT && o.parent() == wp->parent() && o.uid() != wp->uid()) {
                                    if (!get_pos(o, q) || std::hypot(q.x - p0.x, q.y - p0.y, q.z - p0.z) >= 0.1f) still = false;
                                    else twins.push_back(o.uid());
                                }
                            if (still) selection_.insert(twins.begin(), twins.end());
                        }
            }
        }
        if (dragging_ && drag_moved_) { objects_dirty_ = minimap_dirty_ = true; }
        sel_press_ = box_selecting_ = dragging_ = false;
    }
}

// Double-click (or the Select-same key / menu item): every object of the clicked one's type. On screen by default, the whole map when asked.
void App::select_same_type(uint32_t uid, bool whole_map) {
    const WObj* o = doc_ ? object_by_uid(doc_->world, uid) : nullptr;
    if (!o) return;
    const uint32_t cls = o->cls, tpl = o->template_id();
    std::set<uint32_t> now;
    for (auto& v : views_) {
        if (v.index != v.root || v.index < 0) continue;
        const WObj& w = doc_->world.objs[size_t(v.index)];
        if (!same_type(w.cls, w.template_id(), cls, tpl)) continue;
        if (!whole_map) {
            float sx, sy;
            if (!cam_.project({v.pos.x, v.pos.y + 1, v.pos.z}, vp_w, vp_h, sx, sy) || sx < 0 || sy < 0 || sx > vp_w || sy > vp_h) continue;
        }
        now.insert(v.uid);
    }
    if (now.empty()) now.insert(uid);
    selection_ = std::move(now);
    const TemplateInfo* ti = cat_ ? cat_->find(tpl) : nullptr;
    std::string what = ti ? palette_plain_name(*ti) : !o->description().empty() && o->description() != "NULL" ? o->description() : gd_.names().label(cls);
    status_ = "Selected " + std::to_string(selection_.size()) + " " + what + (whole_map ? " (the whole map)." : " (all on screen). Ctrl+double-click: the whole map.");
    status_time_ = 5;
}

// ---------------------------------------------------------------------------------------------
// Layout panels

void App::ui_menu() {
    // Esc closes the open menu, list or popup (one level), like every other program; without ImGui's
    // keyboard navigation it did nothing ("Add objective..."). Modal windows handle Esc
    // themselves; a text field being typed in keeps it (Esc ends the typing first).
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        // Also plain pop-up lists (Add objective..., drop-downs), but not a question box: it answers Esc itself.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && g.OpenPopupStack.Size > 0) {
            ImGuiWindow* top = g.OpenPopupStack.back().Window;
            if (top && !(top->Flags & ImGuiWindowFlags_Modal)) {
                ImGui::ClosePopupToLevel(g.OpenPopupStack.Size - 1, true);
                esc_closed_popup_ = true;  // this Esc is used up: it does not also leave the tool or clear the selection
                // ...nor close a window under it that polls Esc itself (Check, Add part, Preferences).
                ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##esc_closed_popup"), ImGuiInputFlags_LockThisFrame);
            }
        }
    }
    if (!ImGui::BeginMainMenuBar()) return;
    // Menu shortcut text = the first key bound to the action (Preferences > Hotkeys).
    std::string ktext[A_COUNT];
    for (int a = 0; a < A_COUNT; a++) ktext[a] = kt(a);
    auto K = [&](int a) { return ktext[a].empty() ? nullptr : ktext[a].c_str(); };
    if (ImGui::BeginMenu("File")) {
        // A greyed item says why (no game folder, no map open, a real level).
        auto why = [&](const char* text) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
        };
        const char* no_game = "Needs the game folder. It was not found: set it in File > Settings > General > Game folder (type or paste the path).";
        const char* no_map = !doc_ ? "Make or open a map first (File > New map...)." : doc_->read_only ? "This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab first." : nullptr;
        if (ImGui::MenuItem(act_text(A_NEW, "...").c_str(), K(A_NEW))) cmd_new_kind(true);
        if (ImGui::MenuItem(act_text(A_NEW_MISSION, "...").c_str())) cmd_new_kind(false);
        if (ImGui::MenuItem(act_text(A_OPEN, "...").c_str(), K(A_OPEN))) cmd_open();
        if (ImGui::MenuItem("Look at a real level...", nullptr, false, io_ != nullptr)) show_retail_ = true;
        if (!io_) why(no_game);
        if (ImGui::BeginMenu("Open recent")) {
            bool any = false;
            std::string pick;
            for (auto& r : settings_.recent) {
                if (r.empty()) continue;
                any = true;
                // The file name, with the full path as its tooltip.
                const std::string label = path_utf8(fs::path(widen(r)).filename()) + "##" + r;
                if (ImGui::MenuItem(label.c_str())) pick = r;
                ImGui::SetItemTooltip("%s", r.c_str());
            }
            if (!any) ImGui::MenuItem("(nothing yet)", nullptr, false, false);
            ImGui::EndMenu();
            if (!pick.empty() && confirm_close()) open_path(fs::path(widen(pick)));
        }
        ImGui::Separator();
        if (ImGui::MenuItem(act_text(A_SAVE).c_str(), K(A_SAVE), false, editable())) cmd_save(false);
        if (no_map && !editable()) why(no_map);
        if (ImGui::MenuItem(act_text(A_SAVE_AS, "...").c_str(), K(A_SAVE_AS), false, editable())) cmd_save(true);
        if (no_map && !editable()) why(no_map);
        if (ImGui::MenuItem(act_text(A_PREFS, "...").c_str(), K(A_PREFS))) open_prefs_ = true;
        if (ImGui::MenuItem("Open scenario folder")) {
            std::error_code ec;
            fs::create_directories(settings_.scenarios(), ec);
            if (background) message("Open folder", "Scenario folder: " + path_utf8(settings_.scenarios()));  // hidden editor: no Explorer window
            else ShellExecuteW(hwnd_, L"open", settings_.scenarios().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::MenuItem("Update old scenarios...", nullptr, false, io_ != nullptr)) offer_update_old_scenarios(true);
        if (!io_) why(no_game);
        ImGui::Separator();
        if (ImGui::MenuItem(act_text(A_CHECK, "...").c_str(), nullptr, false, doc_ != nullptr)) { validation_ = check_now(); show_validation_ = true; }
        if (!doc_) why("Make or open a map first (File > New map...).");
        if (ImGui::MenuItem(act_text(A_TEST).c_str(), K(A_TEST), false, editable())) cmd_test();
        if (no_map && !editable()) why(no_map);
        ImGui::Separator();
        if (ImGui::MenuItem(from_game ? "Back to the game" : "Exit")) quit_ = true;
        if (from_game) ImGui::SetItemTooltip("Closes the editor and returns to Custom Scenarios in the game (it asks about unsaved changes first).");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        // The step's name shows (Undo sun, Redo delete); "##" keeps the item findable as "Undo" / "Redo".
        const std::string undo_name = editable() && !undo_.empty() ? "Undo " + undo_.back().label : "Undo";
        const std::string redo_name = editable() && !redo_.empty() ? "Redo " + redo_.back().label : "Redo";
        if (ImGui::MenuItem((undo_name + "##undo").c_str(), K(A_UNDO), false, editable() && !undo_.empty())) undo();
        if (ImGui::MenuItem((redo_name + "##redo").c_str(), K(A_REDO), false, editable() && !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem(act_text(A_CUT).c_str(), K(A_CUT), false, editable() && !selection_.empty())) cut_selection();
        if (ImGui::MenuItem(act_text(A_COPY).c_str(), K(A_COPY), false, !selection_.empty())) copy_selection();
        if (ImGui::MenuItem("Paste at view centre", nullptr, false, editable() && !clipboard_.empty())) {
            paste({cam_.tx, 0, cam_.tz});
        }
        if (ImGui::MenuItem(act_text(A_DUPLICATE).c_str(), K(A_DUPLICATE), false, editable() && !selection_.empty())) duplicate_selected();
        if (ImGui::MenuItem(act_text(A_DELETE).c_str(), K(A_DELETE), false, editable() && !selection_.empty())) delete_selection();
        if (ImGui::MenuItem(act_text(A_SNAP).c_str(), K(A_SNAP), false, editable() && !selection_.empty())) snap_selection_to_ground();
        if (ImGui::MenuItem(act_text(A_BRING_FORWARD).c_str(), K(A_BRING_FORWARD), false, editable() && !selection_.empty())) layer_forward();
        ImGui::SetItemTooltip("Put the selected road piece or base on top of the flat pieces it overlaps.");
        if (ImGui::MenuItem(act_text(A_SEND_BACK).c_str(), K(A_SEND_BACK), false, editable() && !selection_.empty())) layer_back();
        ImGui::SetItemTooltip("Put the selected road piece or base under the flat pieces it overlaps.");
        if (ImGui::MenuItem("Put flat pieces in order", nullptr, false, editable())) reseat_command();
        ImGui::SetItemTooltip("Stack the selected road pieces and bases (or all of them) by the order they were placed in, so none lie at the same height. Tilted and glued pieces are left alone.");
        ImGui::Separator();
        if (ImGui::MenuItem(act_text(A_PLACE_AGAIN).c_str(), K(A_PLACE_AGAIN), false, editable() && last_place_tool_ != Tool::Select && tool_ != last_place_tool_)) place_again();
        if (ImGui::MenuItem(act_text(A_PICK_TYPE).c_str(), K(A_PICK_TYPE), false, editable() && selection_.size() == 1)) pick_selected_type();
        ImGui::SeparatorText("Snapping");
        snap_controls_ui(false);
        ImGui::Separator();
        ImGui::Separator();
        if (ImGui::MenuItem(act_text(A_SELECT_ALL).c_str(), K(A_SELECT_ALL), false, doc_ != nullptr)) select_all();
        if (ImGui::MenuItem("Deselect all", K(A_CANCEL), false, !selection_.empty())) selection_.clear();
        if (ImGui::MenuItem(act_text(A_ZOOM_SEL).c_str(), K(A_ZOOM_SEL), false, !selection_.empty())) zoom_to_selection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        // Three groups: what to show, how close to the game it looks, and the camera. Item names stay as the hints quote them ("View > Effects").
        ImGui::SeparatorText("Show");
        ImGui::MenuItem("Map grid (64 m lines)", nullptr, &r_.params.grid);
        if (ImGui::BeginMenu("Show objects for game type", doc_ && doc_->multiplayer)) {
            if (ImGui::MenuItem("All game types", nullptr, view_type_ < 0)) { view_type_ = -1; objects_dirty_ = true; }
            ImGui::Separator();
            for (size_t gi = 0; gi < game_types().size(); gi++) {
                const GameTypeDef& g = game_types()[gi];
                bool ticked = doc_ && std::find(doc_->types.begin(), doc_->types.end(), g.ini) != doc_->types.end();
                std::string label = std::string(g.label) + (ticked ? "" : "  (not ticked on the Map tab)");
                if (ImGui::MenuItem(label.c_str(), nullptr, view_type_ == int(gi))) { view_type_ = int(gi); objects_dirty_ = true; }
            }
            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Some objects only exist in one game type (for example the Snatch It base or the Capture the Flag flags). Pick a type to see just what the game shows in it. This only changes the view, not the saved map.");
        if (ImGui::BeginMenu("Helpers (not shown in the game)")) {
            if (ImGui::MenuItem(act_text(A_HELPERS).c_str(), K(A_HELPERS), &show_helpers_)) objects_dirty_ = true;
            ImGui::Separator();
            static const char* names[HG_COUNT] = {"Trigger areas", "AI paths (waypoints)", "Markers", "Lights", "Cameras and camera paths", "Other logic"};
            for (int g = 0; g < HG_COUNT; g++)
                if (ImGui::MenuItem(names[g], nullptr, &show_helper_group_[g], show_helpers_)) objects_dirty_ = true;
            ImGui::Separator();
            ImGui::MenuItem("Legend", nullptr, &show_legend_);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Objects the mission shows later", nullptr, &show_later_)) objects_dirty_ = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Some objects are not there when you start playing: hidden until a trigger shows them, sent in later by a spawner, removed at the start, or only used in a cutscene. Off: the view matches the game when play starts (%d left out). On: show them too.", later_hidden_);
        ImGui::MenuItem("Level tree", nullptr, &show_level_tree_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every object of the level in its groups, as the game reads it: triggers and their actions, cutscenes, sets, everything. Click one to open it in Properties, also when it has no place on the map.");
        if (ImGui::MenuItem("Where players may go (overlay)", nullptr, &show_typemap_)) overlay_dirty_ = true, typemap_auto_ = false;
        if (bool st = show_steep_; ImGui::MenuItem("Steep ground (overlay)", nullptr, &st)) { settings_.show_steep = st; show_steep_ = st; steep_auto_ = false; settings_.save(); overlay_dirty_ = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tints steep ground by who can get up it: yellow over %.0f%% (bots' 'Mechs won't walk up it; jump-jet 'Mechs may jet up a short face), orange over %.0f%% (very steep: bots won't route here; some 'Mechs may still climb it).", double(kSteepBot) * 100.0, double(kSteepLight) * 100.0);
        ImGui::SeparatorText("Look like the game");
        if (ImGui::MenuItem("3D models (off = simple boxes)", nullptr, &show_models_)) objects_dirty_ = true;
        ImGui::MenuItem("Sky", nullptr, &show_sky_);
        ImGui::MenuItem("Shadows and lighting", nullptr, &r_.params.lighting);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The sun's shadows and shading on the ground and models (the map's light map). Game lighting below adds the map's own sun and ambient colours.");
        ImGui::MenuItem("Ground textures", nullptr, &r_.params.detail);
        ImGui::MenuItem("Game lighting", nullptr, &r_.params.env_light);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Light the view with the map's own ambient and sun colours (Environment tab), so night maps look dark like in the game.");
        if (ImGui::MenuItem("Brighten dark maps", nullptr, &r_.params.lift_dark)) { settings_.lift_dark = r_.params.lift_dark; settings_.save(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Night looks are very dark. On: the editor view lifts a very dark map so you can see what you place. Only this view changes; the map and the game look as before. Turn it off to see the night as the game draws it.");
        ImGui::MenuItem("Fog", nullptr, &r_.params.fog);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the map's fog the way the game does (scaled by the game's Draw distance setting). Night maps fade to black a few hundred metres away, as in the game.");
        ImGui::MenuItem("Weather", nullptr, &show_weather_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the map's rain, snow, dust storm and lightning (Environment tab) over the view.");
        ImGui::MenuItem("Effects", nullptr, &show_effects_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the fires, smoke and sparks the game starts on objects when a level loads (the oil fires of a crash site, the flames on a wrecked tower's floors), as the game's own particle effects. Off: an orange marker shows where each placed effect is.");
        ImGui::MenuItem("Bloom (glow)", nullptr, &r_.params.bloom);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make bright lights, lit windows and glowing panels glow the way the game does (its bloom pass).");
        ImGui::MenuItem("Ground scatter", nullptr, &show_culture_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the grass, bushes, rocks and tree clusters the game scatters over the ground, near the camera (they fade out a few hundred metres away, as in the game).");
        if (ImGui::MenuItem("Scene lights", nullptr, &r_.params.scene_lights)) {
            settings_.scene_lights = r_.params.scene_lights;
            settings_.save();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Let street lamps and other lights light the ground and nearby models the way the game does, to check the look of a scene. Off by default because it costs a little speed.");
        ImGui::MenuItem("Walls that block the camera", nullptr, &fade_blockers_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide the walls and gates the game fades out when they stand between the camera and the player (an arena wall around the fight): objects ticked \"Blocks camera when faded\" disappear while the line from the camera to its target passes through them. Off by default; a replayed game camera pose (the pair scripts' --cam-eye) always does it.");
        ImGui::SeparatorText("Window");
        if (ImGui::BeginMenu("Text and button size")) {  // A big window can be given a bigger UI from here (also File > Settings > General)
            const float cur = dpi_ / std::max(0.5f, base_dpi_);
            static const float steps[] = {0.75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f};
            for (float sc : steps) {
                char lb[32];
                snprintf(lb, sizeof lb, "%.2fx", double(sc));
                if (ImGui::MenuItem(lb, nullptr, !settings_.ui_scale_auto && std::fabs(cur - sc) < 0.01f)) { settings_.ui_scale = sc; settings_.ui_scale_auto = false; settings_.ui_scale_run = 0; ui_scale_dirty_ = true; settings_.save(); }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fit to this window", nullptr, settings_.ui_scale_auto)) {   // Stays on, follows the window size
                settings_.ui_scale_auto = true;
                settings_.ui_scale_run = 0;
                ui_scale_dirty_ = true;
                settings_.save();
            }
            ImGui::EndMenu();
        }
        ImGui::SeparatorText("Camera");
        if (ImGui::MenuItem(act_text(A_TOP_DOWN).c_str(), K(A_TOP_DOWN))) cam_.pitch = cam_.pitch < 1.5f ? 1.5533f : 0.95f;
        if (ImGui::MenuItem(act_text(A_ZOOM_SEL).c_str(), K(A_ZOOM_SEL), false, doc_ != nullptr && !selection_.empty())) zoom_to_selection();
        if (ImGui::MenuItem(act_text(A_RESET_VIEW).c_str(), K(A_RESET_VIEW), false, doc_ != nullptr)) reset_view();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem(act_text(A_HELP, "...").c_str(), K(A_HELP))) { show_help_ = true; help_ctx_ = true; }
        if (ImGui::MenuItem(act_text(A_SHORTCUTS, "...").c_str(), K(A_SHORTCUTS))) show_keys_ = true;
        if (ImGui::MenuItem("Getting Started")) show_getting_started_ = true;
        if (ImGui::MenuItem("User guide")) open_user_guide();
        ImGui::Separator();
        if (ImGui::MenuItem("About...")) show_about_ = true;
        ImGui::Separator();
        ImGui::TextDisabled("Camera: %s or right-drag pan, wheel zoom, middle-drag or %s+drag turn, %s top-down.", pan_text().c_str(), mod_name(settings_.hold_orbit), kt(A_TOP_DOWN).c_str());
        ImGui::TextDisabled("%s turn, %s puts it on the ground, %s delete, %s undo. Change keys: File > Settings > Keys.", turn_keys_text().c_str(),
                            kt(A_SNAP).c_str(), kt(A_DELETE).c_str(), kt(A_UNDO).c_str());
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::ui_tabs() {
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##tabs", nullptr, f);
    // Task order (section 3): how a scenario is made. Players lives inside Units and Props, Messages inside Objectives & Triggers.
    struct TabDef { Tab t; const char* name; const char* tip; };
    static const TabDef defs[] = {
        {Tab::Map, "Map", "Name, description, size and game types of this map."},
        {Tab::Build, "Build", "Everything you put on the map or shape on it: objects, scatter, roads and walls, ground, paint, water and starts. The strip on the left picks the tool."},
        {Tab::Triggers, "Objectives & Triggers", "The briefing, the objectives, and what happens when (triggers, cutscenes)."},
        {Tab::Cameras, "Cameras & Cutscenes", "Cutscenes and the cameras they fly: make, aim and play them."},
        {Tab::Environment, "Environment", "Time of day, sky, light, fog and weather."},
    };
    const Tab shown = tab_ == Tab::Players || tab_ == Tab::Units || tab_ == Tab::Terrain ? Tab::Build : tab_ == Tab::Messages ? Tab::Triggers : tab_;
    for (int i = 0; i < int(sizeof defs / sizeof defs[0]); i++) {
        if (i) ImGui::SameLine();
        bool on = shown == defs[i].t;
        if (on) theme::push_toggle_on();
        // Tab labels in the game's condensed heading face (style-guide section 3); the tooltip stays in the body face.
        // With no map open only Map (new / open) leads anywhere; the task tabs wait for a map.
        const bool needs_map = !doc_ && defs[i].t != Tab::Map;
        ImGui::BeginDisabled(needs_map);
        if (theme::tab_font()) ImGui::PushFont(theme::tab_font(), 0.f);
        // Narrow window: tabs only as wide as their words, so the map name and Helpers fit in the strip.
        const bool tight = float(ImGui::GetMainViewport()->Size.x) / dpi_ < 1500.f;
        float w = std::max((tight ? 52.f : 96.f) * dpi_, ImGui::CalcTextSize(defs[i].name).x + (tight ? 20.f : 24.f) * dpi_);
        const bool clicked = ImGui::Button(defs[i].name, ImVec2(w, 0));
        if (theme::tab_font()) ImGui::PopFont();
        ImGui::EndDisabled();
        if (clicked) {
            if (defs[i].t == Tab::Build) goto_tab(Tab::Build);  // the page it was on
            else {
                tab_ = defs[i].t;
                if (tool_ != Tool::Select && tool_ != Tool::PickPoint) tool_ = Tool::Select;
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", needs_map ? "Make or open a map first (the start page in the middle, or File > New map or mission)." : defs[i].tip);
        if (on) theme::pop_toggle_on();
    }
    ImGui::SameLine(0, 30 * dpi_);
    ImGui::BeginDisabled(!editable());
    // Greyed buttons still say why (a newcomer otherwise has no idea what to do).
    const char* why_off = !doc_ ? "Make or open a scenario first (the start page in the middle, or File > New map...)." : doc_->read_only ? "This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab first." : nullptr;
    // Undo / Redo name the step they act on (section 2: the toolbar strip).
    ImGui::BeginDisabled(undo_.empty());
    if (ImGui::Button("Undo")) undo();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", undo_.empty() ? "Nothing to undo yet." : ("Undo " + undo_.back().label + " (" + kt(A_UNDO) + ")").c_str());
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2 * dpi_);
    ImGui::BeginDisabled(redo_.empty());
    if (ImGui::Button("Redo")) redo();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", redo_.empty() ? "Nothing to redo." : ("Redo " + redo_.back().label + " (" + kt(A_REDO) + ")").c_str());
    ImGui::EndDisabled();
    ImGui::SameLine(0, 14 * dpi_);
    if (ImGui::Button("Save")) cmd_save(false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why_off ? why_off : ("Save the scenario (" + kt(A_SAVE) + ").").c_str());
    ImGui::SameLine();
    // With no map the Test button is plain like Save, not the bright call to action.
    if (editable() ? theme::primary_button("Test", ImVec2(72 * dpi_, 0)) : ImGui::Button("Test", ImVec2(72 * dpi_, 0))) cmd_test();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why_off ? why_off : ("Saves, then starts the game straight into this scenario (" + kt(A_TEST) + ").").c_str());
    if (doc_ && doc_->multiplayer) {
        // The test match settings sit next to Test (they were at the bottom of the Units and Props tab).
        ImGui::SameLine(0, 1 * dpi_);
        if (ImGui::ArrowButton("##testset", ImGuiDir_Down)) ImGui::OpenPopup("##testsettings");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Test settings: game type, bots and their skill.");
        ImGui::SetNextWindowSize(ImVec2(380 * dpi_, 0));
        if (ImGui::BeginPopup("##testsettings")) { test_settings_ui(); ImGui::EndPopup(); }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (doc_) {
        // The Helpers button and the game-type box sit at the right end of the strip; the name is clipped
        // before them (full name in its tooltip), the "not saved" note after it.
        const float combo_w = doc_->multiplayer ? (ImGui::GetMainViewport()->Size.x / dpi_ < 1500.f ? 172.f : 200.f) * dpi_ : 0.f;
        const float help_w = ImGui::CalcTextSize("Helpers").x + ImGui::GetStyle().FramePadding.x * 2;
        const float helpers_x = ImGui::GetWindowContentRegionMax().x - help_w - combo_w - ImGui::GetStyle().ItemSpacing.x;
        const char* note = doc_->read_only ? "real level, read-only" : (doc_->dirty || doc_->file.empty()) ? "not saved" : nullptr;
        const float note_w = note ? ImGui::CalcTextSize(note).x + ImGui::GetStyle().ItemSpacing.x : 0.f;
        const float name_full = ImGui::CalcTextSize(doc_->name.c_str()).x;
        // The name is clipped before Helpers (full name in its tooltip); with less than ~36 px of room it is left out (the
        // window title and the Map tab carry it) instead of being drawn under the Helpers button.
        const float room = helpers_x - ImGui::GetCursorPosX() - 8 * dpi_ - note_w;
        if (room >= 36 * dpi_) {
            const float name_w = std::min(name_full, room);
            ImGui::AlignTextToFramePadding();
            {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(pos, ImVec2(pos.x + name_w, pos.y + ImGui::GetFrameHeight()), true);
                dl->AddText(ImVec2(pos.x, pos.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(theme::tok().text_bright), doc_->name.c_str());
                dl->PopClipRect();
                ImGui::Dummy(ImVec2(name_w, ImGui::GetFrameHeight()));
                if (name_w < name_full && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", doc_->name.c_str());
            }
            ImGui::SameLine();
            if (doc_->read_only) ImGui::TextColored(theme::tok().warn, "real level, read-only");
            else if (note) ImGui::TextColored(theme::tok().text_hint, "not saved");
        }
        // At the right end of the strip, always: it does not slide when "not saved" comes and goes, nor sit mid-bar on a very
        // wide window (this replaces's 300 px cap).
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20 * dpi_, helpers_x));
        const bool help_on = show_helpers_;
        if (theme::toggle_button("Helpers", help_on)) { show_helpers_ = !show_helpers_; objects_dirty_ = true; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", (std::string(help_on ? "Hide" : "Show") + " the helpers the game never draws: trigger areas, AI paths, markers, lights, cameras (" +
                                     kt(A_HELPERS) + "). Pick groups in View > Helpers.").c_str());
        if (doc_->multiplayer) {
            ImGui::SameLine();
            view_type_combo(combo_w);
        }
    }
    ImGui::End();
}

void App::ui_left_panel() {
    ImGuiWindowFlags f = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    // The Build pages never scroll themselves: their browser or list does.
    if (doc_ && tab_ == Tab::Build) f |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##left", nullptr, f);
    // Each tab keeps its own scroll position: opening Map after scrolling down the Terrain tab used
    // to leave Map scrolled past its name and description.
    static float tab_scroll[16] = {};
    static Tab scroll_tab = Tab::Map;
    const bool tab_switched = tab_ != scroll_tab;
    if (tab_switched) {
        ImGui::SetScrollY(tab_scroll[int(tab_)]);
        scroll_tab = tab_;
    }
    if (!io_) {
        // The details are in Settings (it opens at start): said once, not in both places.
        ImGui::TextWrapped("The game folder is not set up yet. Choose the folder with MechAssault 2.exe in Settings.");
        if (ImGui::Button("Settings...")) open_prefs_ = true;
    } else if (!doc_ && show_getting_started_) {
        // The same choices are in the Getting Started window in front: no second set with other words here.
        ImGui::TextWrapped("Pick how to start in the Getting Started window.");
    } else if (!doc_) {
        // The start page in the middle of the window holds New / Open / Look; this panel fills once a map is open.
        ImGui::TextDisabled("No map open.");
        if (show_new_) ImGui::TextWrapped("Fill in the New map or mission window to start.");  // the dialog is already open
        else ImGui::TextWrapped("Pick how to start in the middle of the window (or the File menu).");
    } else {
        if (doc_ && doc_->read_only) {
            // Retail levels open read-only; say so on every tab and offer the way out.
            ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
            ImGui::TextWrapped("You are looking at a real level from the game. It can't be changed here, so nothing on these tabs "
                               "can be edited. Make your own copy to change it, add units, triggers and orders.");
            ImGui::PopStyleColor();
            if (theme::primary_button("Make an editable copy", ImVec2(-1, 0))) make_editable_copy();
            ImGui::Separator();
            ImGui::Spacing();
        }
        switch (tab_) {
            case Tab::Map: tab_map(); break;
            case Tab::Terrain: case Tab::Players: case Tab::Units: case Tab::Build: ui_build_tab(); break;  // (the first three are normalised to Build each frame)
            case Tab::Triggers:
            case Tab::Messages:
            {
                // Objectives & Triggers: the briefing and objectives the player reads, then what happens when.
                // A real level's briefing is long and read-only, so there it starts folded (its own open state)
                // and the Triggers list is in view; "Jump to Triggers" gets there from anywhere above it.
                static bool jump_to_triggers = false;
                if (ImGui::SmallButton("Jump to Triggers")) jump_to_triggers = true;
                // Arriving from another tab to open a trigger (unit orders, "Open the trigger"): its editor is in
                // the Triggers list, below the briefing, so go there (it opened out of view).
                static uint32_t trig_seen = 0;
                if (tab_switched && trig_uid_ && trig_uid_ != trig_seen) jump_to_triggers = true;
                trig_seen = trig_uid_;
                ImGui::SetItemTooltip("Scrolls down to the list of triggers (what happens when).");
                const bool ro_level = doc_->read_only;
                const char* brief = ro_level ? "Briefing, objectives and messages###brief_ro" : "Briefing, objectives and messages###brief";
                if (theme::section(brief, !ro_level && (tab_ == Tab::Messages || !doc_->multiplayer) ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    ImGui::Indent(4 * dpi_);
                    tab_messages();
                    ImGui::Unindent(4 * dpi_);
                }
                if (jump_to_triggers) { ImGui::SetNextItemOpen(true); ImGui::SetScrollHereY(0.0f); jump_to_triggers = false; }
                if (theme::section("Triggers", ImGuiTreeNodeFlags_DefaultOpen)) tab_triggers();
                break;
            }
            case Tab::Cameras: tab_cameras(); break;
            case Tab::Environment: tab_environment(); break;
            case Tab::Options: tab_ = Tab::Map; break;  // old profiles / scripts: the Options tab is now File > Settings
        }
    }
    if (!tab_switched) tab_scroll[int(tab_)] = ImGui::GetScrollY();
    panels_.left_scroll_max = ImGui::GetScrollMaxY();
    ImGui::End();
}

void App::ui_right_panel() {
    ImGuiWindowFlags f = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##right", nullptr, f);
    // The overview map sits under the Properties list and never over it: the list's child ends where the overview starts. On a short
    // window (under 800 px (700 left Properties 380 px at 1584x861) of panel) the overview starts hidden so the Size fields and the buttons under them are in reach without
    // scrolling; a small button under the list shows / hides it, and the choice is kept (Settings file, [Layout] Minimap).
    const float avail_y = ImGui::GetContentRegionAvail().y;
    const bool mm_open = settings_.minimap < 0 ? avail_y >= 800.f * dpi_ : settings_.minimap == 1;
    minimap_open_ = mm_open;
    const float btn_h = ImGui::GetFrameHeight();
    // The overview shrinks on a short window (a quarter of the panel, 120..220 px) so the Properties list keeps its room.
    float mm = mm_open ? std::min(std::min(ImGui::GetContentRegionAvail().x, 220.f * dpi_), std::max(120.f * dpi_, 0.25f * avail_y)) : 0.f;
    ImGui::BeginChild("##inspector", ImVec2(0, avail_y - mm - btn_h - 8 * dpi_), ImGuiChildFlags_None);
    if (focus_props_) { ImGui::SetScrollY(0); focus_props_ = false; }  // the object menu's Properties: back to the top of the list
    transform_tools_ui();
    ui_inspector();
    ImGui::EndChild();
    if (ImGui::SmallButton(mm_open ? "Hide the overview map##mmtoggle" : "Show the overview map##mmtoggle")) {
        settings_.minimap = mm_open ? 0 : 1;
        settings_.save();
    }
    ImGui::SetItemTooltip("The small map under Properties. Hidden, Properties gets the room.");
    if (mm_open) ui_minimap();
    ImGui::End();
    ui_legend();
    snap_bar();
    level_tree_window();
}

// Status bar: left = the tool and what a click does (or the last message, green, while it shows);
// right = cursor position, selection, object count, map kind, last autosave.
// The game's key-hint footer look (style-guide section 5): in each " - " part after the first, the leading key
// names ("Ctrl+Z", "Q / E", "[ / ]", "Esc") are drawn in the hint blue and the action words in the body colour.
static bool is_key_word(const std::string& w) {
    if (w.empty() || w.size() > 9) return false;
    if (w == "/" || w == "[" || w == "]" || w == "?") return true;
    if (!std::isupper((unsigned char)w[0])) return false;
    for (char ch : w) if (!std::isalnum((unsigned char)ch) && ch != '+') return false;
    return w.size() <= 2 || w.find('+') != std::string::npos || w == "Esc" || w == "Del" || w == "Space" || w == "Shift" || w == "Ctrl" ||
           w == "Alt" || w == "Tab" || w == "Enter" || w == "Home" || w == "End" || w == "Delete" || w == "Backspace" || w == "Insert" ||
           w == "PgUp" || w == "PgDn" || w == "Up" || w == "Down" || w == "Left" || w == "Right" || w == "Num";
}

static void key_hint_text(const std::string& line, bool lead_sep = true) {
    const ImVec4 body = ImGui::GetStyleColorVec4(ImGuiCol_Text), keyc = theme::tok().text_hint, dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    size_t at = 0;
    for (int part = 0; at <= line.size(); part++) {
        size_t end = line.find(" - ", at);
        std::string seg = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (part) { ImGui::SameLine(0, 0); ImGui::TextColored(dim, " - "); ImGui::SameLine(0, 0); }
        size_t k = 0;  // end of the leading key words
        if (part || !lead_sep) {  // a continuation line starts with key words too, but has no separator before them
            for (size_t i = 0;;) {
                size_t sp = seg.find(' ', i);
                std::string w = seg.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                const bool colon = w.size() > 1 && w.back() == ':';  // "Q / E: another way" - the last key name ends in a colon
                if (colon) w.pop_back();
                if (!is_key_word(w)) break;
                k = sp == std::string::npos ? seg.size() : sp;
                if (colon || sp == std::string::npos) break;
                i = sp + 1;
            }
        }
        if (k) {
            ImGui::TextColored(keyc, "%s", seg.substr(0, k).c_str());
            if (k < seg.size()) { ImGui::SameLine(0, 0); ImGui::TextColored(body, "%s", seg.substr(k).c_str()); }
        } else {
            ImGui::TextColored(body, "%s", seg.c_str());
        }
        if (end == std::string::npos) break;
        at = end + 3;
    }
}

void App::ui_status() {
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * dpi_, 2 * dpi_));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2 * dpi_));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));   // The bar is shorter than a window's usual minimum, which pushed it below the window edge
    ImGui::Begin("##status", nullptr, f);
    if (doc_) {
        char right[256];
        Vec3 g;
        int k = (vp_hover_ && cursor_ground(g)) ? std::snprintf(right, sizeof right, "x %.0f  z %.0f  ground %.2f m", g.x, g.z, g.y)
                                                : std::snprintf(right, sizeof right, "x -  z -");
        if (vp_hover_ && k > 0 && editable()) {  // a flat piece under the cursor: its top as well
            const double now = ImGui::GetTime();
            if (now - paving_time_ > 0.1 || std::fabs(paving_x_ - g.x) + std::fabs(paving_z_ - g.z) > 0.5f) {  // a few times a second is plenty
                paving_time_ = now;
                paving_x_ = g.x;
                paving_z_ = g.z;
                uint32_t on = 0;
                const float sf = surface_at(g.x, g.z, 1e30f, &on);
                paving_ = on ? sf : std::numeric_limits<float>::quiet_NaN();
            }
            if (std::isfinite(paving_)) k += std::snprintf(right + k, sizeof right - k, "  paving %.2f m", paving_);
        }
        size_t n = doc_->world.objs.size();
        // "N placed" counts what the user put down; the warnings past 2,300 / 3,000 use the full count (the game's limit).
        // The biggest real level (Mission_03) has 2,098, so a copy of it plus a few units is not yet "a lot".
        std::string count = n > 3000 ? std::to_string(n) + " objects (too many: the limit is 3,000)"
                          : n > 2300 ? std::to_string(n) + " objects (a lot: the biggest real level has about 2,100)"
                                     : std::to_string(placed_count()) + " placed";
        k += std::snprintf(right + k, sizeof right - k, "   |   %zu selected   |   %s   |   %s %.0f x %.0f m", selection_.size(), count.c_str(),
                           doc_->multiplayer ? "Multiplayer map" : "Mission", doc_->terrain.size_x(), doc_->terrain.size_z());
        if (autosaved_at_ && doc_->dirty) {
            time_t t = time_t(autosaved_at_);
            tm lt{};
            localtime_s(&lt, &t);
            std::snprintf(right + k, sizeof right - k, "   |   Autosaved %02d:%02d", lt.tm_hour, lt.tm_min);
        }
        float rw = ImGui::CalcTextSize(right).x, avail = ImGui::GetWindowWidth() - rw - 28 * dpi_;
        bool msg = status_time_ > 0;
        std::string left = msg ? status_ : tool_hint_text();
        const ImVec4 msg_col = !status_amber_.empty() && status_ == status_amber_ ? theme::tok().warn : theme::tok().ok;
        // Two-line bar (narrow window): the text breaks into a second line (at a " - " when that fills most of the first line,
        // else at a space); only what does not fit in two lines is cut, and the whole line is in the tooltip.
        std::string shown = left, second;
        auto cut_to = [&](std::string t, float w) {
            while (t.size() > 4 && ImGui::CalcTextSize(t.c_str()).x > w) t.resize(t.size() - 4), t += "...";
            return t;
        };
        if (status_two_ && ImGui::CalcTextSize(left.c_str()).x > avail) {
            size_t brk = std::string::npos, skip = 1;
            for (size_t i = left.find(" - "); i != std::string::npos; i = left.find(" - ", i + 1)) {
                if (ImGui::CalcTextSize(left.substr(0, i).c_str()).x > avail) break;
                brk = i;
            }
            if (brk != std::string::npos && ImGui::CalcTextSize(left.substr(0, brk).c_str()).x > 0.6f * avail) skip = 3;
            else {
                brk = std::string::npos;
                for (size_t i = left.find(' '); i != std::string::npos; i = left.find(' ', i + 1)) {
                    if (ImGui::CalcTextSize(left.substr(0, i).c_str()).x > avail) break;
                    brk = i;
                }
            }
            if (brk == std::string::npos) brk = std::min<size_t>(left.size(), 40);
            shown = left.substr(0, brk);
            second = left.substr(std::min(left.size(), brk + skip));
            second = cut_to(second, ImGui::GetWindowWidth() - 24 * dpi_);  // the second line has no cursor / count text beside it: the whole width
        } else if (!status_two_) {
            shown = cut_to(left, avail);
        }
        if (msg) ImGui::TextColored(msg_col, "%s", shown.c_str());
        else if (shown == left) key_hint_text(shown);
        else if (status_two_) key_hint_text(shown);
        else ImGui::TextUnformatted(shown.c_str());
        if (!status_two_ && shown != left) ImGui::SetItemTooltip("%s", left.c_str());
        ImGui::SameLine(std::max(ImGui::GetWindowWidth() - rw - 10 * dpi_, ImGui::GetCursorPosX()));
        ImVec4 col = n > 3000 ? theme::tok().error : n > 2300 ? theme::tok().warn : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::TextColored(col, "%s", right);
        ImGui::SetItemTooltip("'placed' = things you placed on the map. Helpers (trigger areas, paths, markers) and the hidden objects\n"
                              "every map has (terrain, sky...) are not counted. The game's limit counts everything: %zu of 3,000.", n);
        if (!second.empty()) {  // the second line of a two-line bar
            if (msg) ImGui::TextColored(msg_col, "%s", second.c_str());
            else key_hint_text(second, false);
            if (second.size() > 3 && second.compare(second.size() - 3, 3, "...") == 0) ImGui::SetItemTooltip("%s", left.c_str());
        }
    } else if (status_time_ > 0) {
        ImGui::TextColored(theme::tok().ok, "%s", status_.c_str());
    } else if (io_) {
        ImGui::TextDisabled("No map open. Pick how to start: New map, New mission, Open a saved one or Look at a real level.");
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// No map open: the start page in the middle of the view (S11/M17). The same four words as
// the File menu and Getting Started, one line each, and the files opened last.
void App::ui_start_page() {
    if (!io_) return;  // the left panel says what is wrong with the game folder
    ImGui::SetNextWindowPos(ImVec2(vp_x + vp_w * 0.5f, vp_y + vp_h * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(560 * dpi_, vp_w - 40 * dpi_), 0));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Start", nullptr, f)) {
        theme::bracket_window();
        theme::heading("Start");
        if (settings_.first_run)
            ImGui::TextWrapped("Welcome to the MechAssault 2 Scenario Editor. Build multiplayer maps to play against bots, or "
                               "missions with briefings, objectives and enemies. Hover over any button to see what it does. "
                               "If the text is too small, File > Settings > General > UI scale makes it bigger.");
        else ImGui::TextDisabled("No map open. Pick how to start.");
        ImGui::Spacing();
        const float bw = 200 * dpi_, bh = 34 * dpi_;
        auto card = [&](const char* label, const char* line) {
            bool hit = ImGui::Button(label, ImVec2(bw, bh));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(line);
            ImGui::PopTextWrapPos();
            return hit;
        };
        if (card("New map...", "A multiplayer map to play against bots. Red and Blue spawn points are placed for you.")) cmd_new_kind(true);
        if (card("New mission...", "A single-player mission: a player start, enemies, a briefing and objectives.")) cmd_new_kind(false);
        if (card("Open a saved one...", "A scenario you made before (.ma2map, from your scenario folder).")) cmd_open();
        if (card("Look at a real level...", "A level from the game, read-only, to see how it is made. Make an editable copy to change it.")) show_retail_ = true;
        bool any = false;
        for (auto& r : settings_.recent) any |= !r.empty();
        if (any) {
            ImGui::SeparatorText("Opened lately");
            int shown = 0;
            for (auto& r : settings_.recent) {
                if (r.empty() || shown >= 5) continue;
                fs::path p(widen(r));
                ImGui::PushID(shown++);
                if (ImGui::Selectable(path_utf8(p.stem()).c_str())) open_path(p);
                ImGui::SetItemTooltip("%s", r.c_str());
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Help > Getting Started explains the first steps.");
    }
    ImGui::End();
}

std::string App::tool_hint_text() {
    ToolHintCtx c;
    c.read_only = doc_ && doc_->read_only;
    c.selected = int(selection_.size());
    if (doc_ && !selection_.empty()) {
        // A selection where nothing has a place of its own (timers, 'units destroyed' triggers, sets).
        int placed = 0, trig = 0;
        for (const auto& o : doc_->world.objs) {
            if (!selection_.count(o.uid())) continue;
            Vec3 p;
            placed += get_pos(o, p);
            trig += gd_.names().label(o.cls).rfind("MATrigger", 0) == 0;
        }
        c.no_place = !placed;
        size_t n = selection_.size();
        c.what = n == 1 ? (trig ? "This trigger" : "This object") : "These " + std::to_string(n) + (trig == int(n) ? " triggers" : " objects");
    }
    if (tool_ == Tool::Select) {
        // Fresh each time (the flags the map input sets are stale until the mouse is over the map): the same piece says the same
        // thing on every tab.
        SizeLayout slo;
        const bool sok = doc_ && selection_.size() == 1 && size_layout(slo);
        c.sizable = sok;
        c.size_water = sok && slo.si.kind == SizeKind::Water;
        c.size_small = doc_ && selection_.size() == 1 && !sok && slo.reason == "too small on screen";
        c.size_hint = size_hint_;
        {  // Also with something selected (placing just stopped): the lit page says what it does
            const bool on_build = tab_ == Tab::Build || tab_ == Tab::Terrain || tab_ == Tab::Players || tab_ == Tab::Units;   // Other tabs do not show a page's line
            if (!on_build) {}
            else if (page_ == Page::Water) c.page_pick = "press Place water on the map in the panel, then click the map";   // 
            else if (page_ == Page::Scatter) c.page_pick = "pick an object in the list to arm the scatter brush";
            else if (page_ == Page::Place) c.page_pick = "pick an object in the list, then click the map to place it";
            else if (page_ == Page::RoadsWalls) c.page_pick = "pick a piece in the list, then click the map to place it";
            else if (page_ == Page::Paint) c.page_pick = "pick a texture or a plant in the panel to start painting";
            else if (page_ == Page::Sculpt) c.page_pick = "pick a shaping tool in the panel (Raise, Ramp ...)";
            else if (page_ == Page::Starts) c.page_pick = doc_ && !doc_->multiplayer ? "press Move player start in the panel, then click the map where the player starts"
                                                                                   : "press Place spawn point (or Add 6 on a ring) in the panel, then click the map";   // F-V5R-3
            if (!c.page_pick.empty()) c.page_name = page_name(page_);   // The line names the page, not Select
        }
    }
    c.water_w = water_new_w_;
    c.water_l = water_new_l_;
    c.water_dragging = water_drag_ && water_drag_moved_;
    c.paste_count = int(paste_src_.size());
    c.paste_dup = paste_dup_;
    c.brush_m = brush_radius_;
    c.raise_amount = settings_.raise_amount;
    c.build_up = settings_.build_up;
    c.ramp_stage = ramp_stage_;
    c.ramp_width = settings_.ramp_width;
    c.heading_deg = heading_display(std::round(ghost_yaw() * 57.2958f));  // round first: -0.3 deg read '360'
    c.road = !place_road_.empty();
    c.road_snapped = road_snapped_;
    c.snapped = snap_.on && (tool_ == Tool::PlaceObject || tool_ == Tool::PasteGhost || (tool_ == Tool::Select && move_snap_.on));
    c.moving = move_snap_.on;
    c.snap_what = snap_.label;
    c.move_shape = pick_.kind == PickTarget::MoveShape;
    c.obj_targets = pick_.kind == PickTarget::ObjTargets;
    c.link_unit = pick_.kind == PickTarget::LinkUnit || pick_.kind == PickTarget::CutUnit || pick_.kind == PickTarget::OrderUnit;
    c.cam_path = pick_.kind == PickTarget::CamPath;
    c.tint_pick = tint_pick_;
    c.scatter_empty = scatter_set_.empty();
    c.scatter_count = tool_ == Tool::ScatterTrees ? scatter_so_far() : -1;
    c.scatter_erase = sc_run_ && sc_run_->erase;
    {   // The reverse key on the paint tools flips the line while it is held, or for the whole stroke once it has begun.
        const bool rev_tool = tool_ == Tool::PaintTexture || tool_ == Tool::PaintType || tool_ == Tool::PaintCulture;
        c.paint_reversed = rev_tool && (stroke_active_ ? stroke_rev_ : holding(settings_.hold_reverse));
        c.paint_layers = doc_ && doc_->ground.enabled && doc_->ground.count() >= 3;
    }
    c.plant_erase = plant_erase_ != (tool_ == Tool::PaintCulture && c.paint_reversed);
    c.objects_follow = settings_.objects_follow_terrain;
    if (tool_ == Tool::PlaceObject && slope_plan_.active) {
        c.slope_mode = int(slope_plan_.mode);
        c.slope_fell_back = slope_plan_.fell_back;
        c.slope_gap = slope_plan_.gap;
        c.slope_up = slope_plan_.up;
        c.slope_down = slope_plan_.down;
        c.slope_deg = std::max(std::fabs(slope_plan_.pitch), std::fabs(slope_plan_.roll)) * 57.2958f;
        c.slope_left = slope_plan_.left;
        c.slope_fits = slope_plan_.fits_ground;
        c.slope_joint = slope_plan_.joint_kept;
        c.slope_buried = slope_plan_.buried;
        c.slope_key = kt(A_SLOPE_MODE);
    }
    auto key = [&](int a, const char* def) { std::string t = kt(a); return t.empty() ? std::string(def) : t; };
    c.stop = key(A_CANCEL, "-");
    c.turn_l = key(A_TURN_LEFT, "-");
    c.turn_r = key(A_TURN_RIGHT, "-");
    c.del = key(A_DELETE, "-");
    c.frame = key(A_ZOOM_SEL, "-");
    c.snap = key(A_SNAP, "-");
    c.select_all = key(A_SELECT_ALL, "-");
    c.shortcuts = key(A_SHORTCUTS, "-");
    c.undo = key(A_UNDO, "-");
    c.brush = key(A_BRUSH_SMALLER, "-") + " / " + key(A_BRUSH_BIGGER, "-");
    c.brush_weak = key(A_BRUSH_WEAKER, "-") + " / " + key(A_BRUSH_STRONGER, "-");
    c.hold_free = mod_name(settings_.hold_free);
    c.grid_m = grid_active() ? settings_.snap.grid_m : 0.f;
    c.angle_step = settings_.snap.angle && !holding(settings_.hold_free) ? settings_.snap.angle_deg : 0.f;
    c.grid_keys = key(A_GRID_SMALLER, "-") + " / " + key(A_GRID_BIGGER, "-");
    c.hold_through = mod_name(settings_.hold_through);
    c.hold_quarter = mod_name(settings_.hold_quarter);
    c.hold_reverse = mod_name(settings_.hold_reverse);
    bool mp = doc_ && doc_->multiplayer;
    if (tool_ == Tool::PlaceObject && cat_) {
        if (const TemplateInfo* t = place_road_.empty() ? cat_->find(place_template_) : nullptr) {
            c.thing = palette_plain_name(*t);  // the game name stays on the palette row tooltip
            // The same rule as the 'Placing for' row: on a multiplayer map only turrets and soldiers take a team.
            static const uint32_t K_TURRET_ = key_of("MATurret"), K_PERSON_ = key_of("MAPerson");
            if (mp && (t->cls == K_TURRET_ || t->cls == K_PERSON_)) c.side = place_team_ == 1 ? "Red team" : place_team_ == 2 ? "Blue team" : "(no team)";
            else if (mp && side_unit(t->cls)) c.side = "(no team: it stands empty)";
            else if (!mp && side_unit(t->cls)) c.side = place_side_ == 5 ? "Enemy" : place_side_ == 6 ? "the player's side" : "neutral";
        } else if (const RoadPiece* rp = place_road_.empty() ? nullptr : find_road_piece(place_road_)) {
            c.thing = "road piece " + rp->friendly;
        }
    }
    if (tool_ == Tool::PlaceSpawn) {
        c.thing = "spawn point";
        c.side = spawn_team_ == 1 ? "Red team" : spawn_team_ == 2 ? "Blue team" : "free-for-all games";
    }
    if (tool_ == Tool::PlacePad && io_) {  // pads have no team: whoever reaches one climbs in
        auto& pads = io_->pad_kinds();
        c.thing = pad_kind_ >= 0 && pad_kind_ < int(pads.size()) ? "vehicle pad (" + pads[size_t(pad_kind_)].label + ")" : "vehicle pad";
        c.side = "any team";
    }
    c.on_top = place_on_top_ && tool_ == Tool::PlaceObject;
    if (c.on_top && doc_ && place_on_top_of_ >= 0 && size_t(place_on_top_of_) < doc_->world.objs.size())
        c.on_top_of = plain_object_name(doc_->world.objs[size_t(place_on_top_of_)].description());
    if (tool_ == Tool::PlaceWater && cat_)
        if (const TemplateInfo* wt = cat_->find(water_place_tpl())) c.thing = palette_name(*wt);
    // A shaping tool's line follows the reverse key: 'Lower (40 m, release Ctrl for Raise)'.
    if (sculpt_tool(tool_)) {
        const Tool now = stroke_active_ ? stroke_tool_ : sculpt_now();
        c.reversed = now != tool_ || (tool_ == Tool::Flatten && holding(settings_.hold_reverse));
        return tool_hint_line(now, c);
    }
    return tool_hint_line(tool_, c);
}

Tool App::sculpt_now() const {
    if (!sculpt_tool(tool_) || !holding(settings_.hold_reverse)) return tool_;
    return sculpt_partner(tool_);
}

// Over the map, top left: the status bar's tool line, shown only where a newcomer needs it (a read-only
// level, and the first session for Select / placing / sculpting) plus why the game preview is off.
void App::ui_viewport_hint() {
    if (!doc_) return;
    ui_weather();
    // No hint banner over the view: the status bar carries the tool hint.
    // Background list: over the 3D view, under every window and menu.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float pad = 6 * dpi_;
    ImVec2 at(vp_x + 10 * dpi_, vp_y + 8 * dpi_);
    const ImVec2 ts(0, -10 * dpi_);
    if (preview_on_ && !preview_valid_) {
        ImVec2 at2(at.x, at.y + ts.y + 10 * dpi_);
        ImVec2 ts2 = ImGui::CalcTextSize(preview_reason_.c_str());
        dl->AddRectFilled(ImVec2(at2.x - pad, at2.y - pad * 0.5f), ImVec2(at2.x + ts2.x + pad, at2.y + ts2.y + pad * 0.5f), ImGui::ColorConvertFloat4ToU32(ImVec4(theme::tok().error.x * 0.45f, theme::tok().error.y * 0.45f, theme::tok().error.z * 0.45f, 0.85f)), 2 * dpi_);
        dl->AddText(at2, ImGui::ColorConvertFloat4ToU32(theme::tok().text_bright), preview_reason_.c_str());
    }
    // The armed thing's name and footprint under the ghost, so similar palette rows can be told apart on the map.
    if (preview_on_ && tool_ == Tool::PlaceObject) {
        std::string label;
        float fw = 0, fl = 0;
        if (!place_road_.empty()) {
            if (const RoadPiece* rp = find_road_piece(place_road_)) {
                label = rp->friendly;
                // Size from the model: the .road header's width/length are only rough for bridge sections ("2 x 1 m").
                if (GpuModel* gm = rp->model.empty() ? nullptr : models_.get(rp->model, ""); gm && gm->state == GpuModel::Ready)
                    fw = gm->bmax[0] - gm->bmin[0], fl = gm->bmax[2] - gm->bmin[2];
            }
        } else if (const TemplateInfo* t = cat_ ? cat_->find(place_template_) : nullptr) {
            // The palette's plain name ("Hesperus arena outer wall"), without its "(Env_HES_...)" official suffix.
            label = palette_plain_name(*t);
            place_footprint(fw, fl);
        }
        if (fw > 0 && fl > 0) {
            char sz[48];
            snprintf(sz, sizeof sz, "  %.0f x %.0f m", fw, fl);
            label += sz;
        }
        float sx, sy;
        if (!label.empty() && cam_.project({preview_pos_.x, preview_pos_.y, preview_pos_.z}, vp_w, vp_h, sx, sy)) {  // the footprint centre
            ImVec2 ls = ImGui::CalcTextSize(label.c_str());
            // Above the ghost's centre, so the join label (below the cursor, draw_snap_feedback) and the joint marker are never covered; kept
            // inside the view so a ghost at the map edge is not cut off.
            ImVec2 lp(vp_x + sx - ls.x * 0.5f, vp_y + sy - 24 * dpi_ - ls.y);
            lp.x = std::clamp(lp.x, vp_x + pad + 2 * dpi_, std::max(vp_x + pad + 2 * dpi_, vp_x + vp_w - ls.x - pad - 2 * dpi_));
            lp.y = std::clamp(lp.y, vp_y + pad, std::max(vp_y + pad, vp_y + vp_h - ls.y - pad));
            dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
            dl->AddRectFilled(ImVec2(lp.x - pad, lp.y - pad * 0.4f), ImVec2(lp.x + ls.x + pad, lp.y + ls.y + pad * 0.4f), IM_COL32(0, 0, 0, 150), 3 * dpi_);
            dl->AddText(lp, IM_COL32(255, 255, 255, 235), label.c_str());
            dl->PopClipRect();
        }
    }
}

// Toolbar strip, multiplayer maps: which game type's objects the view shows.
void App::view_type_combo(float w) {
    ImGui::SetNextItemWidth(w);
    std::string cur = view_type_ < 0 ? std::string("Showing: all game types") : std::string("Showing: ") + game_types()[size_t(view_type_)].label;
    if (view_type_ >= 0) cur += " (" + std::to_string(view_type_hidden_) + " hidden)";
    if (begin_combo("##viewtype", cur.c_str())) {
        if (ImGui::Selectable("All game types", view_type_ < 0)) { view_type_ = -1; objects_dirty_ = true; }
        for (size_t gi = 0; gi < game_types().size(); gi++) {
            const GameTypeDef& g = game_types()[gi];
            if (ImGui::Selectable(g.label, view_type_ == int(gi))) { view_type_ = int(gi); objects_dirty_ = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Some objects only exist in one game type (for example the Snatch It base or the Capture the Flag flags). Pick a type to see just what the game shows in it. This only changes the view, not the saved map.");
}

// Esc closes the window that has focus, unless the player is typing in a box or dragging a value.
// That Esc is then used up for the frame (the map's Esc = leave the tool / clear the selection does not see it).
static bool esc_closes_window() {
    const bool r = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                   !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput;
    if (r) ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##esc_closed_window"), ImGuiInputFlags_LockThisFrame);
    return r;
}

// Help > Keyboard shortcuts (or "?"): one page built from the rebindable action table, so it always shows the user's
// own keys, plus the mouse grammar that is not rebindable.
void App::ui_keys() {
    // The sheet opens as wide as the screen allows (min(1100, 92 % of the window) with two columns, 1500 with three on a wide
    // one) and 85 % of its height, so L2 / L3 need no scrolling; a window the user resized keeps its own size.
    {
        const ImVec2 vs = ImGui::GetMainViewport()->Size;
        const float logical_w = vs.x / dpi_;
        const float want_w = logical_w >= 1700.f ? 1500.f : 1100.f;
        ImGui::SetNextWindowSize(ImVec2(std::min(want_w * dpi_, 0.92f * vs.x), 0.85f * vs.y), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    if (theme::begin_window("Keyboard shortcuts", &show_keys_)) {
        if (esc_closes_window()) show_keys_ = false;
        // The hold-key rows read the user's own hold keys (Settings > Keys).
        const Hold h = settings_.hold();
        const std::string add = h.through == KM_SHIFT ? "Ctrl+click" : h.through == KM_CTRL ? "Shift+click" : "Shift+click / Ctrl+click";
        const std::vector<MouseRow> mouse = mouse_rows(mod_name(h.through), mod_name(h.free), mod_name(h.quarter), mod_name(h.reverse), turn_keys_text(), add, mod_name(h.orbit));
        // Blocks: the mouse sheet, then every section of the key table (an action with no handler yet is not listed).
        struct Block { std::string title; std::vector<std::pair<std::string, std::string>> rows; };
        std::vector<Block> blocks;
        blocks.push_back({"Mouse", {}});
        for (auto& m : mouse) blocks.back().rows.push_back({m.gesture, m.does});
        for (const char* section : kSections) {
            Block b{section, {}};
            for (int a = 0; a < A_COUNT; a++) {
                if (strcmp(act_section(a), section) != 0 || kActTable[a].pending) continue;
                const std::string k = kt2(a);
                b.rows.push_back({k.empty() ? "(none)" : k, act_label(a)});
            }
            if (!b.rows.empty()) blocks.push_back(std::move(b));
        }
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const int ncols = avail_w >= 1400 * dpi_ ? 3 : avail_w >= 880 * dpi_ ? 2 : 1;
        const float col_w = (avail_w - float(ncols - 1) * ImGui::GetStyle().ItemSpacing.x * 2.f) / float(ncols);
        const float key_w = std::max(190 * dpi_, 0.4f * col_w);
        // Heights (estimated with the real wrap widths) so the columns balance; a block is never split.
        std::vector<float> bh(blocks.size());
        float total = 0;
        for (size_t i = 0; i < blocks.size(); i++) {
            float hh = ImGui::GetTextLineHeightWithSpacing() * 1.6f;
            for (auto& r : blocks[i].rows)
                hh += std::max(ImGui::CalcTextSize(r.first.c_str(), nullptr, false, key_w - 16 * dpi_).y, ImGui::CalcTextSize(r.second.c_str(), nullptr, false, col_w - key_w - 16 * dpi_).y) + ImGui::GetStyle().CellPadding.y * 2.f;
            bh[i] = hh;
            total += hh;
        }
        std::vector<int> col_of(blocks.size(), 0);
        {   // tallest block first into the shortest column; inside a column the blocks keep their order
            std::vector<size_t> order(blocks.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return bh[a] > bh[b]; });
            std::vector<float> colh(size_t(ncols), 0.f);
            for (size_t i : order) {
                const size_t c = size_t(std::min_element(colh.begin(), colh.end()) - colh.begin());
                col_of[i] = int(c);
                colh[c] += bh[i];
            }
        }
        if (ImGui::BeginTable("##keycols", ncols, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            for (int c = 0; c < ncols; c++) {
                ImGui::TableSetColumnIndex(c);
                for (size_t i = 0; i < blocks.size(); i++) {
                    if (col_of[i] != c) continue;
                    ImGui::SeparatorText(blocks[i].title.c_str());
                    ImGui::PushID(int(i));
                    if (ImGui::BeginTable("##kb", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, key_w);
                        ImGui::TableSetupColumn("Does");
                        for (auto& r : blocks[i].rows) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", r.first.c_str());  // wraps instead of being cut
                            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", r.second.c_str());
                        }
                        ImGui::EndTable();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Change any of these in File > Settings > Keys.");
    }
    theme::end_window();
}

KeyText App::help_keys() const {
    KeyText k;
    for (int a = 0; a < A_COUNT; a++)
        for (int s = 0; s < 2; s++) k.keys[a][s] = settings_.keys[a][s];
    k.hold = settings_.hold();
    return k;
}

// The Help topic for the tab and tool you are on; "Start here" when nothing matches.
int App::help_context_topic() const {
    const char* want = "Start here";
    if (doc_) {
        const Tab t = tab_;
        if (t == Tab::Build || t == Tab::Terrain || t == Tab::Players || t == Tab::Units) want = help_topic_for_page(page_);
        else if (t == Tab::Triggers || t == Tab::Messages) want = "Missions";
        else if (t == Tab::Cameras) want = "Cutscenes";
        else if (t == Tab::Environment) want = "Look and weather";
    }
    const std::vector<HelpTopic>& raw = help_raw();
    for (size_t i = 0; i < raw.size(); i++)
        if (std::string(raw[i].title) == want) return int(i);
    return 0;
}

// Help (F1): the topic list on the left with "Find in Help" over it, the page on the right. The text is help_text.h's, with the
// user's own keys; Getting Started and docs\Scenario editor.txt read the same text.
void App::ui_help() {
    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(std::min(820.f * dpi_, 0.9f * vp.x), std::min(640.f * dpi_, 0.85f * vp.y)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (theme::begin_window("Help", &show_help_)) {
        if (esc_closes_window()) show_help_ = false;
        const std::vector<HelpPage> pages = help_topics(help_keys());
        if (ImGui::IsWindowAppearing() || help_ctx_) {  // opens at the topic of where you are (F1 on the Environment tab: Look and weather), scrolled to its top
            help_topic_ = help_context_topic();
            help_filter_.clear();
            help_shown_ = -1;
            help_ctx_ = false;
        }
        std::vector<int> shown;
        for (size_t i = 0; i < pages.size(); i++)
            if (help_filter_.empty() || help_matches(pages[i], help_filter_)) shown.push_back(int(i));
        if (!shown.empty() && std::find(shown.begin(), shown.end(), help_topic_) == shown.end()) help_topic_ = shown[0];
        const float list_w = 180 * dpi_;
        const float foot = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##helplist", ImVec2(list_w, -foot), ImGuiChildFlags_None);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##helpfind", "Find in Help", &help_filter_);
        if (help_filter_.empty() && !ImGui::IsItemActive()) ImGui::SetItemTooltip("Type a word to list only the topics that mention it.");  // not while typing: it covered the first result line
        for (int i : shown) {
            if (ImGui::Selectable((pages[size_t(i)].title + "##topic" + std::to_string(i)).c_str(), help_topic_ == i)) help_topic_ = i;
        }
        if (shown.empty()) ImGui::TextDisabled("Nothing found.");
        ImGui::EndChild();
        ImGui::SameLine();
        if (help_shown_ != help_topic_) ImGui::SetNextWindowScroll(ImVec2(0, 0));
        ImGui::BeginChild("##helppage", ImVec2(0, -foot), ImGuiChildFlags_Borders);
        if (shown.empty()) {
            // No hit says what to do next, with the topics to open (the words of the UI are not always the words of the text).
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(("Nothing in Help mentions \"" + help_filter_ + "\". Try a shorter or simpler word, or open a topic:").c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            for (size_t i = 0; i < pages.size(); i++)
                if (ImGui::Selectable((pages[i].title + "##sugg" + std::to_string(i)).c_str())) { help_topic_ = int(i); help_filter_.clear(); help_shown_ = -1; }
        } else {
            help_shown_ = help_topic_;
            const HelpPage& pg = pages[size_t(help_topic_)];
            theme::heading(pg.title.c_str());
            ImGui::Separator();
            ImGui::PushTextWrapPos(0);
            size_t i = 0;
            while (i <= pg.body.size()) {
                size_t e = pg.body.find('\n', i);
                if (e == std::string::npos) e = pg.body.size();
                const std::string para = pg.body.substr(i, e - i);
                i = e + 1;
                if (para.empty()) continue;
                if (para.compare(0, 2, "- ") == 0) {
                    ImGui::Bullet();
                    ImGui::TextUnformatted(para.c_str() + 2);
                } else if (para.size() > 3 && para[0] >= '0' && para[0] <= '9' && para[1] == '.' && para[2] == ' ') {
                    ImGui::Indent(14 * dpi_);
                    ImGui::TextUnformatted(para.c_str());
                    ImGui::Unindent(14 * dpi_);
                } else {
                    ImGui::TextUnformatted(para.c_str());
                }
                ImGui::Dummy(ImVec2(0, 3 * dpi_));
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        if (ImGui::Button("Show Getting Started")) show_getting_started_ = true;
        ImGui::SameLine();
        if (ImGui::Button("User guide")) open_user_guide();
        ImGui::SetItemTooltip("Opens the same text as a plain file, to read or print.");
    }
    theme::end_window();
}

// Help > User guide: the plain-text guide the installer puts next to the game (Docs\Scenario editor.txt).
void App::open_user_guide() {
    std::error_code ec;
    const fs::path guide = gd_.install() / L"Docs" / L"Scenario editor.txt";
    if (!fs::exists(guide, ec)) {
        status_ = "The user guide is installed with the game in Docs\\Scenario editor.txt.";
        status_time_ = 8;
        return;
    }
    if (background) { message("User guide", "User guide: " + path_utf8(guide)); return; }  // hidden editor: no Notepad window
    ShellExecuteW(hwnd_, L"open", guide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Help > About...: version, what this is, the licence line, and where the files live (with Copy buttons for a bug report).
void App::ui_about() {
    ImGui::SetNextWindowSize(ImVec2(560 * dpi_, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (theme::begin_window("About", &show_about_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (esc_closes_window()) show_about_ = false;
        ImGui::TextUnformatted("MechAssault 2 Scenario Editor " MA2SE_VERSION " (" MA2SE_GIT ")");
        ImGui::Spacing();
        ImGui::TextWrapped("Part of the unofficial MechAssault 2 PC recompilation. Needs your own copy of the game. No game data files are "
                           "included: the game's program, art, sound and levels come from your own disc.");
        ImGui::Spacing();
        ImGui::TextWrapped("Uses Dear ImGui (MIT licence, see the Licenses folder).");
        ImGui::TextWrapped("The editor's own code is MIT-licensed; third-party licences are in the Licenses folder.");
        ImGui::Spacing();
        const fs::path logs = Settings::local_dir() / L"Logs";
        struct Row { const char* label; std::string text; };
        const Row rows[] = {{"Game folder", path_utf8(gd_.install())},
                            {"Scenario folder", path_utf8(settings_.scenarios())},
                            {"Settings and logs", path_utf8(Settings::local_dir())}};
        if (ImGui::BeginTable("##about_rows", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, 130 * dpi_);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, 70 * dpi_);
            for (int i = 0; i < 3; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rows[i].label);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", rows[i].text.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(rows[i].text.c_str());
                ImGui::SetItemTooltip("Copies this path to the clipboard.");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Open logs folder")) {
            std::error_code ec;
            fs::create_directories(logs, ec);
            if (background) message("Open folder", "Logs folder: " + path_utf8(logs));
            else ShellExecuteW(hwnd_, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SetItemTooltip("Crash reports and recovery notes go here.");
    }
    theme::end_window();
}

void App::ui_getting_started() {
    ImGui::SetNextWindowSize(ImVec2(560 * dpi_, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (theme::begin_window("Getting Started", &show_getting_started_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (esc_closes_window()) show_getting_started_ = false;
        ImGui::TextWrapped("Welcome to the MechAssault 2 Scenario Editor. Build your own multiplayer maps to play against bots, "
                           "or your own missions with briefings, objectives and enemies.");
        ImGui::Spacing();
        ImGui::BulletText("New map or mission: start from flat ground, random hills, or a copy of a real level.");
        ImGui::BulletText("The Next steps list (right, under Properties) then takes you through it:");
        ImGui::Indent();
        ImGui::TextWrapped("%s", help_getting_started_steps(help_keys()).c_str());
        ImGui::Unindent();
        ImGui::BulletText("Hover over any button to see what it does. Help (%s) has the rest.", kt(A_HELP).c_str());
        ImGui::Spacing();
        float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
        if (ImGui::Button("New multiplayer map...", ImVec2(w, 0))) {
            show_getting_started_ = false;
            cmd_new_kind(true);
        }
        ImGui::SetItemTooltip("A multiplayer map to play against bots: Red and Blue spawn points are placed for you.");
        ImGui::SameLine();
        if (ImGui::Button("New mission...", ImVec2(w, 0))) {
            show_getting_started_ = false;
            cmd_new_kind(false);
        }
        ImGui::SetItemTooltip("A single-player mission: a player start, enemies, a briefing and objectives.");
        ImGui::SameLine();
        if (ImGui::Button("Open a saved one...", ImVec2(w, 0))) {
            show_getting_started_ = false;
            cmd_open();
        }
        ImGui::SetItemTooltip("Opens a .ma2map scenario from your scenario folder (File > Settings > General sets the folder).");
        if (ImGui::Button("Look at a real level...", ImVec2(-1, 0))) {
            show_getting_started_ = false;
            show_retail_ = true;
        }
        ImGui::SetItemTooltip("Opens a level from the game read-only, to see how it is made. \"Make an editable copy\" then lets you change it.");
        ImGui::TextDisabled("Help > Getting Started opens this again.");
    }
    theme::end_window();
}

void App::ui_message_box() {
    if (show_msg_) { ImGui::OpenPopup("##msg"); show_msg_ = false; }
    ImGui::SetNextWindowSize(ImVec2(520 * dpi_, 0));
    if (ImGui::BeginPopupModal("##msg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        theme::bracket_window();
        theme::heading(msg_title_.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", msg_text_.c_str());
        if (ImGui::Button("OK", ImVec2(120 * dpi_, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        // The "Game started" note is only a heads-up: it closes by itself once the game's window is up, when the game is gone, or after 8 s
        // (a game that never shows a normal window, or an editor that was not drawing while the game ran, must not leave it stuck).
        if (msg_title_ == "Game started" && (test_game_up_ || !test_proc_ || ImGui::GetTime() - test_started_ > 8.0)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::ui_validation() {
    // The window fits its list when it opens and when the list changes length (a tall empty window
    // for one item looked broken); dragging its edge in between keeps the user's size.
    const int check_rows = int(std::max<size_t>(validation_.size(), 1));
    // (One size call only: a second ImGuiCond_Appearing call used to replace this one, so it always opened 230 px tall.)
    if (g_check_rows != check_rows) {
        const float h = std::clamp(110.f + float(check_rows) * 64.f, 180.f, 520.f) * dpi_;
        ImGui::SetNextWindowSize(ImVec2(std::min(560 * dpi_, vp_w - 20 * dpi_), std::min(h, vp_h - 60 * dpi_)), ImGuiCond_Always);
        g_check_rows = check_rows;
    }
    // Top-right corner of the map view, clear of the middle where the reported objects usually are.
    ImGui::SetNextWindowPos(ImVec2(vp_x + vp_w - 10 * dpi_, vp_y + 40 * dpi_), ImGuiCond_Appearing, ImVec2(1, 0));
    // W3 (note D): it stays over the map after a resize or a panel change: when the window has drifted out of the map view
    // (window resized, panels moved), it is moved back inside the view's edges.
    if (ImGuiWindow* cw = ImGui::FindWindowByName("Check scenario"); cw && cw->WasActive) {
        const float m = 4 * dpi_;
        const float maxw = std::max(120 * dpi_, vp_w - 2 * m), maxh = std::max(120 * dpi_, vp_h - 2 * m);
        const ImVec2 sz(std::min(cw->Size.x, maxw), std::min(cw->Size.y, maxh));
        const ImVec2 want(std::clamp(cw->Pos.x, vp_x + m, std::max(vp_x + m, vp_x + vp_w - sz.x - m)),
                          std::clamp(cw->Pos.y, vp_y + m, std::max(vp_y + m, vp_y + vp_h - sz.y - m)));
        if (std::fabs(want.x - cw->Pos.x) > 0.5f || std::fabs(want.y - cw->Pos.y) > 0.5f) ImGui::SetNextWindowPos(want, ImGuiCond_Always);
        if (sz.x < cw->Size.x - 0.5f || sz.y < cw->Size.y - 0.5f) ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    }
    if (theme::begin_window("Check scenario", &show_validation_)) {
        if (esc_closes_window()) show_validation_ = false;
        check_rect_[0] = ImGui::GetWindowPos().x; check_rect_[1] = ImGui::GetWindowPos().y;
        check_rect_[2] = ImGui::GetWindowSize().x; check_rect_[3] = ImGui::GetWindowSize().y;
        // Follows the map: re-checked after edits (not mid-drag, at most twice a second), so a fixed item does not
        // stay red; "Check again" still runs it at once.
        const bool refresh = check_stale_ && !ImGui::IsAnyItemActive() && ImGui::GetTime() - check_time_ > 0.5;
        if ((ImGui::Button("Check again") || refresh) && doc_) {
            validation_ = check_now();
            check_stale_ = false;
            check_time_ = ImGui::GetTime();
        }
        int errs = 0, warns = 0, fixable = 0;
        for (auto& v : validation_) { errs += v.level == 2; warns += v.level == 1; fixable += v.fix_team >= 0; }
        if (fixable > 1 && doc_ && !doc_->read_only) {
            ImGui::SameLine();
            if (ImGui::Button("Add all missing spawn points")) {
                push_undo("spawn rings");
                add_missing_spawns();
                world_changed();
                validation_ = check_now();
                check_fixed_ = true;
                errs = warns = 0;
                for (auto& v : validation_) { errs += v.level == 2; warns += v.level == 1; }
                status_ = "Added spawn points for every team. Move or turn them on the map if you like; " + kt(A_UNDO) + " undoes.";
            }
            ImGui::SetItemTooltip("Places a ring of spawn points on each team's side of the map, as many as each game type needs.");
        }
        ImGui::SameLine();
        // Nothing left to fix after a fix button: close and say so on the status line (the window would only say "Ready").
        if (check_fixed_ && !errs && !warns) {
            show_validation_ = false;
            status_ = "Ready to test: press Test (" + kt(A_TEST) + ").";
            status_time_ = 8;
        }
        check_fixed_ = false;
        if (errs) ImGui::TextColored(theme::tok().error, "%d problem(s) to fix before testing, %d warning(s)", errs, warns);
        else if (warns) ImGui::TextColored(theme::tok().warn, "Ready to test; %d warning(s) worth a look", warns);
        else ImGui::TextColored(theme::tok().ok, "Ready to test");
        // A long list (retail copies carry many warnings): show only what blocks testing.
        if (errs && warns) {
            ImGui::SameLine();
            if (theme::toggle_button("Must fix only", check_errors_only_)) check_errors_only_ = !check_errors_only_;
            ImGui::SetItemTooltip(check_errors_only_ ? "Showing only the problems to fix before testing. Click to show the warnings too."
                                                     : "Hide the warnings and show only the problems to fix before testing.");
        }
        const bool errors_only = check_errors_only_ && errs && warns;
        ImGui::Separator();
        ImGui::BeginChild("##vlist");
        for (auto& v : validation_) {
            if (errors_only && v.level != 2) continue;
            ImVec4 c = v.level == 2 ? theme::tok().error : v.level == 1 ? theme::tok().warn : theme::tok().ok;
            ImGui::PushID(&v);
            // Labels left: the level in a fixed column, the text and its Show button in one column after it.
            ImGui::TextColored(c, "%s", v.level == 2 ? "Must fix" : v.level == 1 ? "Warning" : "OK");
            ImGui::SameLine(80 * dpi_);
            ImGui::BeginGroup();
            ImGui::TextWrapped("%s", v.text.c_str());
            if (v.uid) {
                const bool group = v.uids.size() > 1;
                if (!v.uids.empty()) {  // a placement item: a low side view of the object, and on to the next one with the next press
                    if (ImGui::SmallButton(group ? "Show next" : "Show")) check_show(v);
                    ImGui::SetItemTooltip(group ? "Select the next object of this warning and move the camera to it." : "Select it and move the camera to it.");
                } else if (ImGui::SmallButton("Show")) {
                    selection_ = {v.uid};
                    for (auto& o : doc_->world.objs)
                        if (o.uid() == v.uid) { Vec3 p; if (get_pos(o, p)) { cam_.tx = p.x; cam_.tz = p.z; cam_.dist = std::min(cam_.dist, 250.f); } }
                    ImGui::SetItemTooltip("Select it and move the camera to it.");
                }
            }
            if (v.fix && doc_ && !doc_->read_only) {
                if (v.uid) ImGui::SameLine();
                const char* label = v.fix == 4 ? "Play from its own view" : v.fix == 2 ? "Put the newer one on top" : v.fix == 3 ? (v.uids.size() > 1 ? "Put them on the ground" : "Put it on the ground")
                                                                                         : (v.uids.size() > 1 ? "Lift them to the ground" : "Lift it to the ground");
                if (ImGui::SmallButton(label)) {
                    apply_placement_fix(v.fix, v.uids);
                    validation_ = check_now();
                    check_time_ = ImGui::GetTime();
                    ImGui::EndGroup();
                    ImGui::PopID();
                    break;
                }
                ImGui::SetItemTooltip(v.fix == 4 ? "The camera path starts at the camera instead of at the marker, and the camera looks at the marker itself. The undo key undoes it." : v.fix == 2 ? "Brings the newer piece forward so it lies 0.03 m above the other." : "Puts the object back on the ground or paving under it (the same as pressing the Put on the ground key). The undo key undoes it.");
            }
            ImGui::EndGroup();
            if (v.fix_team >= 0 && doc_ && !doc_->read_only) {
                ImGui::SetCursorPosX(80 * dpi_);
                if (ImGui::Button("Add the missing spawn points for me")) {
                    // Red on the west side, Blue on the east, free-for-all in the middle.
                    float fx = v.fix_team == 1 ? 0.25f : v.fix_team == 2 ? 0.75f : 0.5f;
                    push_undo("spawn ring");
                    io_->add_spawn_ring(*doc_, v.fix_team, {doc_->terrain.size_x() * fx, 0, doc_->terrain.size_z() * 0.5f}, 30.f, v.fix_missing);
                    world_changed();
                    validation_ = check_now();
                    check_fixed_ = true;
                    status_ = "Added spawn points. Move or turn them on the map if you like; " + kt(A_UNDO) + " undoes.";
                    ImGui::PopID();
                    break;
                }
                ImGui::SetItemTooltip("Places them in a ring on that team's side of the map.");
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    theme::end_window();
}

// Adds every missing spawn ring the Check lists (Red west, Blue east, free-for-all in the middle). Returns how many rings.
int App::add_missing_spawns() {
    if (!doc_ || doc_->read_only || !io_) return 0;
    int rings = 0;
    for (int guard = 0; guard < 8; guard++) {
        const ValidationItem* fix = nullptr;
        auto vs = io_->validate(*doc_);
        for (auto& v : vs) if (v.fix_team >= 0 && v.fix_missing > 0) { fix = &v; break; }
        if (!fix) break;
        float fx = fix->fix_team == 1 ? 0.15f : fix->fix_team == 2 ? 0.85f : 0.5f;  // opposite ends, clear of the edge (G-e)
        io_->add_spawn_ring(*doc_, fix->fix_team, {doc_->terrain.size_x() * fx, 0, doc_->terrain.size_z() * 0.5f}, 30.f, fix->fix_missing);
        rings++;
    }
    return rings;
}

void App::ui_retail_dialog() {
    ImGui::SetNextWindowSize(ImVec2(420 * dpi_, 560 * dpi_), ImGuiCond_FirstUseEver);
    if (theme::begin_window("Look at a real level (read-only)", &show_retail_)) {
        if (esc_closes_window()) show_retail_ = false;
        ImGui::TextWrapped("Look at any level from the game. To change one, open it here and press \"Make an editable copy\".");
        bool mp_header = false, ms_header = false;
        for (auto& m : gd_.maps()) {
            if (m.multiplayer && !mp_header) { ImGui::SeparatorText("Multiplayer"); mp_header = true; }
            if (!m.multiplayer && !ms_header) { ImGui::SeparatorText("Missions"); ms_header = true; }
            std::string label = pretty_name(m.name) + (m.resource_present ? "" : "  (download content, not on this disc)");
            ImGui::BeginDisabled(!m.resource_present);
            if (ImGui::Selectable(label.c_str())) {
                if (confirm_close()) { open_retail(m); show_retail_ = false; }
            }
            why_disabled(!m.resource_present, "This level's files are not in your copy of the game, so it cannot be opened.");
            ImGui::EndDisabled();
        }
    }
    theme::end_window();
}

void App::ui_minimap() {
    float size = std::min(std::min(ImGui::GetContentRegionAvail().x, 220.f * dpi_), std::max(ImGui::GetContentRegionAvail().y, 60.f));  // what the right panel left it
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - size) * 0.5f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (!doc_) { ImGui::Dummy(ImVec2(size, size)); return; }
    const Terrain& t = doc_->terrain;
    float sx = t.size_x(), sz = t.size_z(), m = std::max(sx, sz);
    float w = size * sx / m, h = size * sz / m;
    ImVec2 o(p0.x + (size - w) * 0.5f, p0.y + (size - h) * 0.5f);
    if (r_.minimap_srv()) ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)r_.minimap_srv(), o, ImVec2(o.x + w, o.y + h));
    ImGui::InvisibleButton("##minimap", ImVec2(size, size));
    auto to_screen = [&](float x, float z) { return ImVec2(o.x + x / sx * w, o.y + (1 - z / sz) * h); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (auto& v : views_) {
        if (v.index != v.root || v.kind == 2) continue;
        ImVec2 p = to_screen(v.pos.x, v.pos.z);
        bool sel = selection_.count(v.uid) != 0;
        ImU32 c = sel ? IM_COL32(255, 255, 0, 255) : IM_COL32(v.color & 255, (v.color >> 8) & 255, (v.color >> 16) & 255, 220);
        float r = v.kind == 1 ? 3.f : 1.5f;
        if (sel) r += 1.5f;
        dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), c);
    }
    // Camera footprint: the viewport corners projected onto the ground plane.
    {
        ImVec2 q[4];
        const float cx[4] = {0, vp_w, vp_w, 0}, cy[4] = {0, 0, vp_h, vp_h};
        // The corner rays meet the ground under the view centre (not the orbit target's height, which an orbit about a pivot moves off
        // the ground); a ray that does not come down within reach (the sky above the horizon) stops at the reach along its own
        // bearing, so the outline stays the trapezoid the view really is instead of collapsing to a sliver.
        const float gy = t.inside(std::clamp(cam_.tx, 0.f, sx), std::clamp(cam_.tz, 0.f, sz)) ? t.height_at(std::clamp(cam_.tx, 0.f, sx), std::clamp(cam_.tz, 0.f, sz)) : cam_.ty;
        const float reach = std::max(cam_.dist * 3.f, 100.f);
        for (int k = 0; k < 4; k++) {
            XMFLOAT3 ro, rd;
            cam_.ray(cx[k], cy[k], vp_w, vp_h, ro, rd);
            float hl = std::hypot(rd.x, rd.z);
            float bx = hl > 1e-4f ? rd.x / hl : std::sin(cam_.yaw), bz = hl > 1e-4f ? rd.z / hl : std::cos(cam_.yaw);
            float H = rd.y < -0.02f ? (gy - ro.y) / rd.y * hl : reach;
            H = std::clamp(H, 0.f, reach);
            q[k] = to_screen(ro.x + bx * H, ro.z + bz * H);
        }
        dl->PushClipRect(o, ImVec2(o.x + w, o.y + h), true);
        dl->AddPolyline(q, 4, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, 1.5f);
        dl->PopClipRect();
    }
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), IM_COL32(200, 200, 200, 160));
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        cam_.tx = std::clamp((mp.x - o.x) / w * sx, 0.f, sx);
        cam_.tz = std::clamp((1 - (mp.y - o.y) / h) * sz, 0.f, sz);
    }
}

}  // namespace ed

namespace ed {

// An object ticked TransparentBlockingCamera (MABuilding walls, gates) that the camera-to-target segment passes through
// (or starts inside): the game fades it, so View > Walls that block the camera skips it.
bool App::blocks_camera(const ObjView& v) const {
    if (v.index < 0 || v.kind != 0 || !v.model) return false;
    if (doc_->world.objs[size_t(v.index)].i32("TransparentBlockingCamera") == 0) return false;
    float ch = std::cos(v.heading), sh = std::sin(v.heading);
    auto loc = [&](float x, float y, float z, float* o) {
        x -= v.pos.x; z -= v.pos.z;
        o[0] = ch * x - sh * z; o[1] = y - v.pos.y; o[2] = sh * x + ch * z;
    };
    float a[3], b[3];
    loc(cam_.eye.x, cam_.eye.y, cam_.eye.z, a);
    loc(cam_.tx, cam_.ty, cam_.tz, b);
    float t0 = 0, t1 = 1;
    for (int k = 0; k < 3; k++) {
        float d = b[k] - a[k];
        if (std::fabs(d) < 1e-6f) { if (a[k] < v.lo[k] || a[k] > v.hi[k]) return false; continue; }
        float ta = (v.lo[k] - a[k]) / d, tb = (v.hi[k] - a[k]) / d;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

std::string App::debug_state() const {
    std::string o;
    char b[512];
    auto line = [&](const char* fmt, auto... args) { snprintf(b, sizeof b, fmt, args...); o += b; o += "\n"; };
    line("tab=%d tool=%d", int(tab_), int(tool_));
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiIO& io = ImGui::GetIO();
        line("imgui mouse=%.0f,%.0f down=%d hovered=%s active=%08X focused=%s nav_disabled=%d", io.MousePos.x, io.MousePos.y, io.MouseDown[0],
             g.HoveredWindow ? g.HoveredWindow->Name : "-", g.ActiveId, g.NavWindow ? g.NavWindow->Name : "-", (int)io.AppFocusLost);
    }
    line("cam tx=%.1f tz=%.1f dist=%.1f yaw=%.3f pitch=%.3f", cam_.tx, cam_.tz, cam_.dist, cam_.yaw, cam_.pitch);
    line("env_map=%s cube=%d", env_map_name_.c_str(), r_.env_cube != nullptr);
    if (doc_ && !doc_->lightmap.px.empty()) {
        double sr = 0, sa = 0; int mnr = 255, mxr = 0, mna = 255, mxa = 0;
        for (uint32_t c : doc_->lightmap.px) { int r = int(c & 255), al = int(c >> 24); sr += r; sa += al; mnr = std::min(mnr, r); mxr = std::max(mxr, r); mna = std::min(mna, al); mxa = std::max(mxa, al); }
        double n = double(doc_->lightmap.px.size());
        line("lightmap_stats r mean=%.1f min=%d max=%d a mean=%.1f min=%d max=%d", sr / n, mnr, mxr, sa / n, mna, mxa);
    }
    if (doc_ && !doc_->colormap.px.empty()) {
        double sr = 0, sg = 0, sb = 0, sa = 0;
        for (uint32_t c : doc_->colormap.px) { sr += c & 255; sg += (c >> 8) & 255; sb += (c >> 16) & 255; sa += c >> 24; }
        double n = double(doc_->colormap.px.size());
        line("colormap_stats %dx%d mean rgba=%.1f,%.1f,%.1f,%.1f detail blend=%.3f modulate=%d rep=%.1f,%.1f,%.1f,%.1f tex=%d%d%d%d", doc_->colormap.w, doc_->colormap.h, sr / n, sg / n, sb / n, sa / n, r_.detail.blend, int(r_.detail.modulate), r_.detail.rep[0], r_.detail.rep[1], r_.detail.rep[2], r_.detail.rep[3], int(r_.detail.tex[0] != nullptr), int(r_.detail.tex[1] != nullptr), int(r_.detail.tex[2] != nullptr), int(r_.detail.tex[3] != nullptr));
    }
    line("light amb=%.3f,%.3f,%.3f sun=%.3f,%.3f,%.3f dir=%.3f,%.3f,%.3f lighting=%d env_light=%d lm=%dx%d", r_.params.ambient.x, r_.params.ambient.y, r_.params.ambient.z, r_.params.sun.x, r_.params.sun.y, r_.params.sun.z, r_.params.sun_dir.x, r_.params.sun_dir.y, r_.params.sun_dir.z, int(r_.params.lighting), int(r_.params.env_light), doc_ ? int(doc_->lightmap.w) : 0, doc_ ? int(doc_->lightmap.h) : 0);
    for (const auto& w : water_dbg_) line("water %s refl=%s", w.first.c_str(), w.second.c_str());
    line("windows help=%d getting_started=%d new=%d validation=%d retail=%d", show_help_, show_getting_started_, show_new_, show_validation_, show_retail_);
    line("popup_open=%d msg_title=%s", ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), msg_title_.c_str());
    line("status=%s", status_time_ > 0 ? status_.c_str() : "");
    line("place_heading_deg=%.1f preview_on=%d preview_valid=%d", place_heading_ * 57.2958f, preview_on_, preview_valid_);
    if (const TemplateInfo* pt = cat_ && place_road_.empty() ? cat_->find(place_template_) : nullptr)
        line("place_template=%u name=%s model=%s class=%s category=%s snap_family=%d piece_snapped=%d", place_template_, pt->name.c_str(),
             pt->model.c_str(), pt->cls_name.c_str(), pt->category.c_str(), snap_family(*pt), piece_snapped_);
    if (const TemplateInfo* pt = cat_ && doc_ && place_road_.empty() ? cat_->find(place_template_) : nullptr; pt && snap_family(*pt)) {
        App* me = const_cast<App*>(this);
        std::vector<SnapEnd> ends;
        std::vector<int> po;
        me->snap_ends(snap_family(*pt), ends, po);
        SnapFoot f{};
        const bool fok = me->snap_foot(*pt, f);
        line("snap_ends n=%d pieces=%d foot_ok=%d half=%.2f", int(ends.size()), int(po.size()), fok, f.half);
        if (GpuModel* gm = me->models_.get(pt->model, pt->mtb); gm && gm->state == GpuModel::Failed)
            line("  model failed: %s (meshes=%d)", gm->error.c_str(), int(gm->meshes.size()));
        if (GpuModel* gm = me->models_.get(pt->model, pt->mtb))
            line("  model state=%d bmin=%.2f,%.2f,%.2f bmax=%.2f,%.2f,%.2f has_vbox=%d vmin=%.2f,%.2f,%.2f vmax=%.2f,%.2f,%.2f", int(gm->state), gm->bmin[0], gm->bmin[1], gm->bmin[2],
                 gm->bmax[0], gm->bmax[1], gm->bmax[2], int(gm->has_vbox), gm->vmin[0], gm->vmin[1], gm->vmin[2], gm->vmax[0], gm->vmax[1], gm->vmax[2]);
    }
    line("preview_pos=%.2f,%.2f,%.2f road_snapped=%d", preview_pos_.x, preview_pos_.y, preview_pos_.z, road_snapped_);
    line("undo=%zu redo=%zu clipboard=%zu", undo_.size(), redo_.size(), clipboard_.size());
    line("prefs_open=%d capture=%d edge_scrolling=%d right_dragged=%d test_running=%d", prefs_open_, capture_act_,
         edge_scrolling_, rdragged_, test_proc_ != nullptr);
    line("last_save graphics_mb=%.1f objects=%zu asset_files=%zu", last_report_.graphics_mb, last_report_.objects, last_report_.asset_files);
    for (auto& v : validation_) line("check level=%d %s", v.level, v.text.c_str());
    line("culture map=%dx%d types=%zu", culture_.w, culture_.h, culture_.types.size());
    for (auto& c : culture_.types) {
        int hist[16] = {};
        for (uint16_t v : culture_.map) hist[(v >> c.shift) & ((1 << c.bits) - 1) & 15]++;
        int nd[5] = {};
        for (auto& b : c.inst) { float dx = b.r0.w - cam_.eye.x, dz = b.r2.w - cam_.eye.z, d = std::sqrt(dx * dx + dz * dz); nd[d < 5 ? 0 : d < 10 ? 1 : d < 20 ? 2 : d < 50 ? 3 : 4]++; }
        line("culture %s startfade=%.0f inst by dist <5,<10,<20,<50,more=%d,%d,%d,%d,%d", c.name.c_str(), c.start_fade, nd[0], nd[1], nd[2], nd[3], nd[4]);
        line("culture %s template=%u inst=%zu density=%.0f shift=%d bits=%d scale=%.2f stopfade=%.0f slope=%.0f..%.0f levels0..15=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", c.name.c_str(), c.templ, c.inst.size(), c.density, c.shift, c.bits, c.scale_range, c.stop_fade, c.min_slope, c.max_slope,
             hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9], hist[10], hist[11], hist[12], hist[13], hist[14], hist[15]);
    }
    line("id_conflicts=%zu", id_conflicts_.size());
    for (auto& c : id_conflicts_) line("id_conflict %s", c.c_str());
    if (!doc_) { line("doc=none"); return o; }
    double hsum = 0;
    for (int gz = 0; gz < doc_->terrain.h; gz++)
        for (int gx = 0; gx < doc_->terrain.w; gx++) hsum += double(doc_->terrain.step_at(gx, gz));  // the saved steps, not the brushes' fine layer
    uint64_t csum = 0;
    for (uint32_t c : doc_->colormap.px) csum = csum * 31 + c;
    size_t ntrig = 0;
    for (auto& o : doc_->world.objs) ntrig += gd_.names().label(o.cls).rfind("MATrigger", 0) == 0;
    line("doc name=%s file=%s dirty=%d mp=%d objects=%zu triggers=%zu objectives=%zu clips=%zu", doc_->name.c_str(),
         path_utf8(doc_->file).c_str(), doc_->dirty, doc_->multiplayer, doc_->world.objs.size(), ntrig,
         doc_->objectives.size(), doc_->audio.size());
    if (io_) {
        uint32_t sc = io_->start_container(*doc_);
        std::string lab = "(other)", all;
        for (auto& c : io_->start_choices()) {
            if (c.container == sc) lab = c.label;
            // Every choice can start on any Base: templates come from the full GlobalEntities (the culled set is no gate) and
            // save mounts the start unit's exact models (wiki t14_start_vehicle start-rule; R9 'missing container' was this stale check).
            line("start_choice %s", c.label.c_str());
        }
        line("start_in=%s (container %u)", lab.c_str(), sc);
    }
    line("terrain hsum=%.1f min=%.1f max=%.1f colorsum=%llu", hsum, doc_->terrain.min_h, doc_->terrain.max_h, (unsigned long long)csum);
    for (auto& o2 : doc_->world.objs)
        if (selection_.count(o2.uid())) {
            Vec3 p;
            get_pos(o2, p);
            line("sel uid=%u name=%s x=%.2f y=%.2f z=%.2f heading_deg=%.1f nation=%d", o2.uid(), o2.description().c_str(), p.x, p.y, p.z,
                 world::heading(o2) * 57.2958f, o2.find("Nation") ? o2.i32("Nation") : -99);
        }
    Vec3 g;
    if (const_cast<App*>(this)->cursor_ground(g)) line("cursor_ground x=%.2f y=%.2f z=%.2f", g.x, g.y, g.z);
    return o;
}

}  // namespace ed

namespace ed {

const char* App::tab_label() const {
    static const char* names[] = {"Map", "Build", "Build", "Build", "Objectives & Triggers", "Cameras & Cutscenes", "Objectives & Triggers", "Environment", "Options", "Build"};
    return names[std::clamp(int(tab_), 0, 9)];
}

// --automation "state": what a test needs to check, as JSON.
std::string App::automation_state() const {
    static const char* tools[] = {"Select", "Raise", "Lower", "Flatten", "Smooth", "Roughen", "Plateau", "PlateauDown", "PaintColor", "PaintTexture", "PaintType", "PaintCulture",
                                  "PlaceObject", "ScatterTrees", "PlaceSpawn", "PlacePad", "PlaceStart", "PlaceWaypoint", "PickPoint",
                                  "MakePond", "PlaceWater", "PasteGhost", "Ramp"};
    Json j = Json::object();
    j.with("tab", Json::string(tab_label()));
    j.with("tool", Json::string(tools[std::clamp(int(tool_), 0, int(sizeof tools / sizeof tools[0]) - 1)]));
    j.with("status_hint", Json::string(const_cast<App*>(this)->tool_hint_text()));
    j.with("left_panel_ms", Json::number(double(left_panel_ms_))).with("frame_ms", Json::number(double(frame_ms_)));
    j.with("page", Json::string(page_name(page_)));  // the Build tab's page
    {
        auto rect = [](const float* r) { return Json::object().with("x", Json::number(r[0])).with("y", Json::number(r[1])).with("w", Json::number(r[2])).with("h", Json::number(r[3])); };
        Json pn = Json::object();
        pn.with("strip", rect(panels_.strip)).with("left", rect(panels_.left)).with("right", rect(panels_.right));
        pn.with("viewport", Json::object().with("x", Json::number(vp_x)).with("y", Json::number(vp_y)).with("w", Json::number(vp_w)).with("h", Json::number(vp_h)));
        pn.with("left_scroll_max", Json::number(panels_.left_scroll_max));
        j.with("panels", pn);
        Json ss = Json::array();
        for (uint32_t u : scatter_set_) ss.push(Json::number(double(u)));
        j.with("scatter_set", ss);
    }
    {   // The scatter brush's options, the footprint radius of each kind in the set, and the counts of the last stroke
        Json rad = Json::object();
        for (uint32_t u : scatter_set_) {
            char k[16];
            std::snprintf(k, sizeof k, "%u", u);
            const auto ri = scatter_r_.find(u);
            rad.with(k, Json::number(ri == scatter_r_.end() ? 0.0 : std::round(ri->second * 100.f) / 100.f));   // 0 until the kind's model has loaded
        }
        j.with("scatter", Json::object()
            .with("density", Json::number(settings_.sc_density)).with("spacing", Json::number(settings_.sc_spacing))
            .with("facing", Json::boolean(settings_.sc_facing)).with("lean", Json::number(settings_.sc_lean))
            .with("follow", Json::boolean(settings_.sc_follow)).with("keep_off", Json::boolean(settings_.sc_keep_off))
            .with("skip_steep", Json::boolean(settings_.sc_skip_steep)).with("slope_limit", Json::number(settings_.sc_slope))
            .with("radii", rad).with("active", Json::boolean(sc_run_ != nullptr)).with("so_far", Json::number(scatter_so_far()))
            .with("erase", Json::boolean(sc_last_.erase)).with("placed", Json::number(sc_last_.placed)).with("erased", Json::number(sc_last_.erased))
            .with("kinds", Json::number(sc_last_.kinds)).with("skipped_close", Json::number(sc_last_.close))
            .with("skipped_keepoff", Json::number(sc_last_.keepoff)).with("skipped_slope", Json::number(sc_last_.steep)));
    }
    Json cam = Json::object();
    cam.with("x", Json::number(cam_.tx)).with("z", Json::number(cam_.tz)).with("dist", Json::number(cam_.dist));
    cam.with("yaw", Json::number(cam_.yaw)).with("pitch", Json::number(cam_.pitch));
    {   // The target height, the eye and the vertical angle (what a shot taken from this view must match)
        const ma2::Vec3 ce = cam_eye_now();
        Json eye = Json::array();
        eye.push(Json::number(ce.x));
        eye.push(Json::number(ce.y));
        eye.push(Json::number(ce.z));
        cam.with("y", Json::number(cam_.ty)).with("eye", std::move(eye)).with("fov_deg", Json::number(cam_.fov * 57.29578f));
    }
    j.with("camera", cam);
    Json win = Json::object();
    win.with("help", Json::boolean(show_help_)).with("getting_started", Json::boolean(show_getting_started_)).with("new", Json::boolean(show_new_));
    win.with("about", Json::boolean(show_about_)).with("keys", Json::boolean(show_keys_));
    win.with("check", Json::boolean(show_validation_)).with("retail", Json::boolean(show_retail_)).with("preferences", Json::boolean(prefs_open_));
    win.with("message", Json::boolean(show_msg_ || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)));
    j.with("windows", win);
    {   // the Help window: which topic is open and which topics "Find in Help" leaves
        Json hs = Json::object();
        Json titles = Json::array();
        const std::vector<HelpPage> pages = help_topics(help_keys());
        for (const HelpPage& p : pages)
            if (help_filter_.empty() || help_matches(p, help_filter_)) titles.push(Json::string(p.title));
        hs.with("topic", Json::string(help_topic_ >= 0 && help_topic_ < int(pages.size()) ? pages[size_t(help_topic_)].title : std::string()))
          .with("filter", Json::string(help_filter_)).with("shown", titles).with("topics", Json::number(double(pages.size())));
        j.with("help", hs);
    }
    j.with("popup_open", Json::boolean(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)));
    j.with("message_title", Json::string(msg_title_)).with("message_text", Json::string(msg_text_));
    j.with("confirm_text", Json::string(confirm_text_));
    j.with("status", Json::string(status_time_ > 0 ? status_ : std::string()));
    j.with("last_dialog", Json::string(automation_dialogs().last_question));
    j.with("hotkey_capture", Json::string(capture_act_ >= 0 ? act_label(capture_act_) : ""));
    j.with("edge_scrolling", Json::boolean(edge_scrolling_)).with("right_dragged", Json::boolean(rdragged_));
    j.with("test_running", Json::boolean(test_proc_ != nullptr));
    j.with("audition", Json::object().with("playing", Json::boolean(audition_playing())).with("clip", Json::number(audition_clip_))
                           .with("ms", Json::number(audition_clip_ >= 0 ? audition_ms_ : 0)));
    j.with("test_launch_skipped", Json::boolean(test_launch_skipped_));
    if (test_launch_skipped_) j.with("test_command", Json::string(narrow(test_command_))).with("test_dir", Json::string(narrow(test_dir_)));
    j.with("gizmo", Json::object().with("mode", Json::number(gizmo_mode_)).with("hot", Json::number(gizmo_hot_)).with("drag", Json::number(gizmo_drag_))
                       .with("local", Json::boolean(gizmo_local_)).with("turn_each", Json::boolean(turn_each_)).with("place_lift", Json::number(place_lift_))
                       .with("readout", Json::string(ImGui::GetTime() <= readout_until_ ? readout_ : std::string()))
                       .with("knob", knob_x_ < 0 ? Json() : Json::object().with("x", Json::number(knob_x_)).with("y", Json::number(knob_y_)))
                       .with("pivot", pivot_x_ < 0 ? Json() : Json::object().with("x", Json::number(pivot_x_)).with("y", Json::number(pivot_y_))));
    j.with("size_gizmo", size_gizmo_json()).with("model_cache_crops", Json::number(const_cast<App*>(this)->size_selection_variants()));
    j.with("preview", Json::object().with("on", Json::boolean(preview_on_)).with("valid", Json::boolean(preview_valid_))
                         .with("reason", Json::string(preview_reason_)).with("heading_deg", Json::number(ghost_yaw() * 57.2958f))
                         .with("x", Json::number(std::round(preview_pos_.x * 100.f) / 100.f)).with("z", Json::number(std::round(preview_pos_.z * 100.f) / 100.f))
                         .with("y", std::isfinite(preview_y_) ? Json::number(std::round(preview_y_ * 1000.f) / 1000.f) : Json())
                         .with("slope", slope_plan_.active
                                            ? Json::object().with("mode", Json::string(slope_plan_.mode == SlopeMode::Level ? "level" : slope_plan_.mode == SlopeMode::Tilt ? "tilt" : "float"))
                                                  .with("wanted", Json::string(slope_plan_.wanted == SlopeMode::Level ? "level" : slope_plan_.wanted == SlopeMode::Tilt ? "tilt" : "float"))
                                                  .with("fell_back", Json::boolean(slope_plan_.fell_back))
                                                  .with("gap", Json::number(std::round(slope_plan_.gap * 100.f) / 100.f))
                                                  .with("left", Json::number(std::round(slope_plan_.left * 100.f) / 100.f))
                                                  .with("pitch_deg", Json::number(std::round(slope_plan_.pitch * 5729.58f) / 100.f))
                                                  .with("roll_deg", Json::number(std::round(slope_plan_.roll * 5729.58f) / 100.f))
                                                  .with("up", Json::number(std::round(slope_plan_.up * 100.f) / 100.f))
                                                  .with("down", Json::number(std::round(slope_plan_.down * 100.f) / 100.f))
                                                  .with("kept", Json::number(double(slope_plan_.edit.kept.size())))
                                            : Json()));
    j.with("undo", Json::number(double(undo_.size()))).with("redo", Json::number(double(redo_.size())));
    j.with("undo_cap", Json::number(double(kUndoSteps))).with("undo_bytes", Json::number(double(undo_bytes())));
    j.with("saved_state", Json::boolean(cur_state_id_ == saved_state_id_));
    {
        uint64_t p[7] = {};
        undo_bytes(p);
        j.with("undo_parts", Json::object().with("world_meta", Json::number(double(p[0]))).with("heights", Json::number(double(p[1]))).with("colours", Json::number(double(p[2])))
                                           .with("types", Json::number(double(p[3]))).with("ground_weights", Json::number(double(p[4]))).with("ground_layers", Json::number(double(p[5])))
                                           .with("audio", Json::number(double(p[6]))));
    }
    j.with("snap", snap_state_json());
    // What the key dispatcher and the viewport are doing.
    j.with("undo_label", Json::string(undo_.empty() ? std::string() : undo_.back().label));
    j.with("helpers_faded", Json::number(helpers_faded_));  // Start slabs drawn faded (camera within 20 m)
    j.with("later_hidden", Json::number(later_hidden_));  // Objects left off the map because they come later (or not yet, in a cutscene preview)
    j.with("viewport", Json::object().with("x", Json::number(vp_x)).with("y", Json::number(vp_y)).with("w", Json::number(vp_w)).with("h", Json::number(vp_h)));
    j.with("brush", Json::object().with("radius", Json::number(brush_radius_)).with("strength", Json::number(brush_strength_)).with("hardness", Json::number(brush_hardness_))
                                  .with("amount", Json::number(settings_.raise_amount)).with("build_up", Json::boolean(settings_.build_up))
                                  .with("min_radius", Json::number([&] { float lo, hi; brush_limits(tool_, lo, hi); return lo; }())).with("readout", Json::string(brush_readout_))
                                  .with("strengths", Json::object().with("texture", Json::number(strength_of(Tool::PaintTexture))).with("tint", Json::number(strength_of(Tool::PaintColor)))
                                                                   .with("smooth", Json::number(strength_of(Tool::Smooth))).with("flatten", Json::number(strength_of(Tool::Flatten)))
                                                                   .with("plants", Json::number(strength_of(Tool::PaintCulture))))
                                  .with("reverse", Json::boolean(stroke_active_ ? stroke_rev_ : holding(settings_.hold_reverse))));
    // The mean of the last 30 frames (ms) and, of the time the brush spent in them (applying a stroke and uploading what it touched), plus the layer stats.
    {
        float fm = 0, bm = 0;
        const int n = std::min(ring_n_, 30);
        for (int i = 0; i < n; i++) { fm += frame_ring_[i]; bm += brush_ring_[i]; }
        j.with("perf", Json::object().with("frame_ms", Json::number(n ? std::round(fm / float(n) * 100.f) / 100.f : 0.0))
                                     .with("brush_ms", Json::number(n ? std::round(bm / float(n) * 1000.f) / 1000.f : 0.0))
                                     .with("full_upload", Json::boolean(paint_full_upload_))
                                     .with("stroke", Json::object().with("frames", Json::number(perf_stroke_n_))   // the last stroke: its frames, mean frame time, mean brush time per frame
                                                                   .with("frame_ms", Json::number(perf_stroke_n_ ? std::round(perf_stroke_frame_ / float(perf_stroke_n_) * 100.f) / 100.f : 0.0))
                                                                   .with("brush_ms", Json::number(perf_stroke_n_ ? std::round(perf_stroke_brush_ / float(perf_stroke_n_) * 1000.f) / 1000.f : 0.0))));
    }
    // The Ramp tool and the overlays.
    j.with("ramp", Json::object().with("stage", Json::number(ramp_stage_)).with("ax", Json::number(std::round(ramp_a_.x * 100.f) / 100.f)).with("az", Json::number(std::round(ramp_a_.z * 100.f) / 100.f))
                                 .with("ha", Json::number(std::round(ramp_ha_ * 100.f) / 100.f)).with("hb", Json::number(std::round(ramp_hb_ * 100.f) / 100.f))
                                 .with("width", Json::number(settings_.ramp_width)).with("slope", Json::number(std::round(ramp_slope_ * 1000.f) / 1000.f))
                                 .with("length", Json::number(std::round(ramp_len_ * 10.f) / 10.f)).with("label", Json::string(ramp_label_)).with("typed", Json::boolean(ramp_typed_)));
    j.with("overlay", Json::object().with("steep", Json::boolean(show_steep_)).with("typemap", Json::boolean(show_typemap_)).with("plants", Json::boolean(overlay_plants_ >= 0)));
    j.with("box_selecting", Json::boolean(box_selecting_)).with("dragging", Json::boolean(dragging_));
    j.with("orbiting", Json::boolean(orbiting_)).with("orb_press", Json::boolean(orb_press_));  // Alt+left-drag turning the view / an Alt press waiting to be a click or a turn
    j.with("context_menu_open", Json::boolean(objmenu_open_)).with("paste_armed", Json::boolean(tool_ == Tool::PasteGhost));
    j.with("paste", Json::object().with("count", Json::number(double(paste_src_.size()))).with("heading_deg", Json::number(std::round(paste_heading_ * 57.2958f)))
                                  .with("duplicate", Json::boolean(paste_dup_)).with("over_map", Json::boolean(paste_on_)));
    {
        Json lab = Json::array();  // helper labels on the map (pad / power-up names): a click on one selects what it names
        for (const LabelHit& l : label_hits_)
            lab.push(Json::object().with("text", Json::string(l.text)).with("uid", Json::number(double(l.uid))).with("x0", Json::number(l.x0)).with("y0", Json::number(l.y0))
                                   .with("x1", Json::number(l.x1)).with("y1", Json::number(l.y1)).with("cx", Json::number(std::floor((l.x0 + l.x1) * 0.5f)))
                                   .with("cy", Json::number(std::floor((l.y0 + l.y1) * 0.5f))));
        j.with("labels", lab);
    }
    {
        Json fired = Json::object();  // actions the dispatcher ran, by label: each key press must show +1 (a key never runs two actions)
        for (int a = 0; a < A_COUNT + 2; a++)
            if (key_counts_[a]) fired.with(a < A_COUNT ? act_label(a) : (a == A_TURN_QUARTER_L ? "Quarter turn left" : "Quarter turn right"), Json::number(key_counts_[a]));
        j.with("keys_fired", fired);
        j.with("last_action", Json::string(last_act_ == A_NONE ? "" : last_act_ < A_COUNT ? act_label(last_act_) : last_act_ == A_TURN_QUARTER_L ? "Quarter turn left" : "Quarter turn right"));
        j.with("keymap_ok", Json::boolean(keymap_ok_));
        j.with("palette_filter", Json::string(unit_filter_)).with("text_input", Json::boolean(ImGui::GetIO().WantTextInput));
    }
    j.with("id_conflicts", Json::number(double(id_conflicts_.size())));
    Json chk = Json::array();
    for (auto& v : validation_) {
        Json ids = Json::array();
        for (uint32_t u : v.uids) ids.push(Json::number(double(u)));
        chk.push(Json::object().with("level", Json::number(v.level)).with("text", Json::string(v.text)).with("uid", Json::number(double(v.uid))).with("uids", ids).with("fix", Json::number(v.fix)));
    }
    j.with("check_items", chk);
    if (doc_) {
        Json d = Json::object();
        d.with("name", Json::string(doc_->name)).with("file", Json::string(path_utf8(doc_->file))).with("dirty", Json::boolean(doc_->dirty));
        d.with("multiplayer", Json::boolean(doc_->multiplayer)).with("read_only", Json::boolean(doc_->read_only));
        d.with("id", Json::string(doc_->id_hex()));  // scenario Id (the game skips a second file with the same Id)
        d.with("objects", Json::number(double(doc_->world.objs.size()))).with("triggers", Json::number(double(doc_->triggers.size())));
        d.with("graphics_mb_last_save", Json::number(last_report_.graphics_mb));
        d.with("graphics_mb_live", Json::number(gfx_live_total()));
        // Ground-texture blend (colour map alpha): mean and the share of texels at 255 (texture A).
        double asum = 0, azero = 0;
        for (uint32_t c : doc_->colormap.px) { asum += c >> 24; azero += (c >> 24) == 255; }
        double n = doc_->colormap.px.empty() ? 1 : double(doc_->colormap.px.size());
        d.with("texture_blend_mean", Json::number(asum / n)).with("texture_a_share", Json::number(azero / n));
        {   // world-space box of the texels at texture A (colour map row 0 = far z)
            const Image& cm = doc_->colormap;
            int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
            for (int y = 0; y < cm.h; y++)
                for (int x = 0; x < cm.w; x++)
                    if ((cm.px[size_t(y) * cm.w + x] >> 24) == 255) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
            if (x1 >= 0) {
                float sx = doc_->terrain.size_x() / float(cm.w), sz = doc_->terrain.size_z() / float(cm.h);
                Json b = Json::array();
                for (float v : {x0 * sx, doc_->terrain.size_z() - (y1 + 1) * sz, (x1 + 1) * sx, doc_->terrain.size_z() - y0 * sz}) b.push(Json::number(v));
                d.with("texture_a_box", b);
            }
        }
        if (int ti = find_class(doc_->world, K_TERRAIN); ti >= 0)
            d.with("texture_a", Json::string(doc_->world.objs[size_t(ti)].str("TerrainDetailMap")))
                .with("texture_b", Json::string(doc_->world.objs[size_t(ti)].str("TerrainDetailMap 1")));
        j.with("doc", d);
        Json sel = Json::array();
        bool sel_sheets = false;
        SheetSet sel_ss;
        for (auto& o : doc_->world.objs)
            if (selection_.count(o.uid())) {
                Vec3 p;
                get_pos(o, p);
                Json row = Json::object();
                row.with("uid", Json::number(o.uid())).with("name", Json::string(o.description()))
                   .with("x", Json::number(p.x)).with("y", Json::number(p.y)).with("z", Json::number(p.z));
                if (!sel_sheets) { sel_sheets = true; sel_ss = const_cast<App*>(this)->sheets(true); }
                const_cast<App*>(this)->stand_fields(row, o, sel_ss, true);
                sel.push(row
                             .with("layer", [&] { int ly = 0; bool fx = false; return place::layer_of(o, ly, fx) ? Json::string(place::format_layer(ly, fx)) : Json(); }())
                             .with("heading_deg", Json::number(std::remainder(world::heading(o) * 57.2958f, 360.f)))
                             .with("pitch_deg", [&] { float tp, tr; world::get_tilt(o, tp, tr); return Json::number(tp * 57.29578f); }())
                             .with("roll_deg", [&] { float tp, tr; world::get_tilt(o, tp, tr); return Json::number(tr * 57.29578f); }()));
                if (sel.a.size() >= 50) break;
            }
        j.with("selection", sel);
        j.with("selection_count", Json::number(double(selection_.size())));
    } else {
        j.with("doc", Json());
    }
    return j.dump();
}

}  // namespace ed

namespace ed {
// The armed palette object's footprint (model bounds, x by z in metres); false until its model has loaded.
bool App::place_footprint(float& w, float& l) const {
    const TemplateInfo* t = cat_ && place_road_.empty() ? cat_->find(place_template_) : nullptr;
    if (!t || t->model.empty()) return false;
    GpuModel* gm = const_cast<ModelCache&>(models_).get(t->model, t->mtb);
    if (!gm || gm->state != GpuModel::Ready) return false;
    w = gm->bmax[0] - gm->bmin[0];
    l = gm->bmax[2] - gm->bmin[2];
    return w > 0 && l > 0;
}

ma2::Vec3 App::place_origin_offset() const {
    if (!cat_) return {0, 0, 0};
    float lx = 0, lz = 0;  // origin relative to the footprint centre
    if (!place_road_.empty()) {
        // A road piece's origin is its entry end; the circle and arrow sit on its middle (playtest 10-26), so it turns about its middle.
        // The middle is the model's box, or before the model loads the box round its ends (entry at 0,0 plus every exit).
        const RoadPiece* rp = const_cast<App*>(this)->find_road_piece(place_road_);
        if (!rp) return {0, 0, 0};
        GpuModel* gm = rp->model.empty() ? nullptr : const_cast<ModelCache&>(models_).get(rp->model, "");
        if (gm && gm->state == GpuModel::Ready && gm->bmax[0] > gm->bmin[0]) {
            lx = -0.5f * (gm->bmin[0] + gm->bmax[0]);
            lz = -0.5f * (gm->bmin[2] + gm->bmax[2]);
        } else {
            float x0 = 0, x1 = 0, z0 = 0, z1 = 0;
            if (rp->exits.empty()) z0 = -rp->l;
            for (auto& e : rp->exits) { x0 = std::min(x0, e.x); x1 = std::max(x1, e.x); z0 = std::min(z0, e.z); z1 = std::max(z1, e.z); }
            lx = -0.5f * (x0 + x1);
            lz = -0.5f * (z0 + z1);
        }
    } else {
        const TemplateInfo* t = cat_->find(place_template_);
        if (!t || t->model.empty()) return {0, 0, 0};
        GpuModel* gm = const_cast<ModelCache&>(models_).get(t->model, t->mtb);
        if (!gm || gm->state != GpuModel::Ready) return {0, 0, 0};
        lx = -0.5f * (gm->bmin[0] + gm->bmax[0]);
        lz = -0.5f * (gm->bmin[2] + gm->bmax[2]);
    }
    const float ch = std::cos(ghost_yaw()), sh = std::sin(ghost_yaw());
    return {ch * lx + sh * lz, 0, -sh * lx + ch * lz};
}
}  // namespace ed

