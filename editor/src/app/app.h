// The editor application: state, layout, tools and tab panels.
#pragma once
#include <chrono>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace ma2 { struct Json; }
#include "../core/brush.h"
#include "../core/crop.h"
#include "../core/cutscene.h"
#include "../core/dictionary.h"
#include "../core/gamedata.h"
#include "../core/placement.h"
#include "../core/scenario.h"
#include "../core/snap.h"
#include "../core/templates.h"
#include "model_cache.h"
#include "help_text.h"
#include "render.h"
#include "select_rules.h"
#include "settings.h"
#include "text_util.h"
#include "tool_hints.h"
#include "page_map.h"
#include "palette_rank.h"
#include "palette_rows.h"
#include "undo_rules.h"

namespace ma2::weather { struct Look; }

struct ImVec2;
struct ImDrawList;
struct ImGuiStyle;

namespace ed {

// Overlapping palette groups beyond the two in app_tabs.cpp: (name, tooltip). palette_search.cpp.
const std::vector<std::pair<const char*, const char*>>& palette_extra_groups();

enum class Tab { Map, Terrain, Players, Units, Triggers, Cameras, Messages, Environment, Options, Build };  // Terrain / Players / Units are the old numbers (--tab N): the per-frame normaliser maps them to Build and a page

enum class BrowserMode { Place, RoadsWalls, Scatter };  // what a click on a palette cell does, and which cells are listed


// A combo's list opens downward within the window: its height stops at the window bottom (at least four rows of row_h);
// when less than four rows are left below, the panel scrolls the combo up first. Call right before ImGui::BeginCombo(id, ...).
void combo_opens_down(const char* id, float min_width, float row_h);
// ImGui::BeginCombo whose list never flips upward (over the menu bar or the tabs): its height stops at the window bottom
// (or at the usual height for its flags when that is less) and scrolls inside. Every combo outside the Look / Sky lists uses it.
bool begin_combo(const char* label, const char* preview, ImGuiComboFlags flags = 0);
// The sky as a player reads it ("Clear blue day", else the art name), app_tabs.cpp.
std::string sky_words(const std::string& name);

// Model matrix rows (3x4, translation in .w) for a yaw / pitch / roll turn at `pos` (gizmo.cpp).
void orient_instance(BoxInstance& mi, float heading, float pitch, float roll, const ma2::Vec3& pos);

// "BoggedDown" -> "Bogged Down" (retail level names as the game shows them).
std::string pretty_name(const std::string& n);
std::string strip_copy_counter(const std::string& name);  // "Powerup respawner_00002" -> "Powerup respawner" (display only)
std::string plain_object_name(const std::string& raw, bool effect = false);  // "Env_THE_MossTree_XLg" -> "Thestria moss tree extra large", else raw

// Terrain-type overlay colour for a type-map value (ABGR).
uint32_t type_overlay_color(uint16_t v);

struct ObjView {
    int index = -1;          // in world.objs
    uint32_t uid = 0;
    int root = -1;           // selectable root (world.objs index)
    ma2::Vec3 pos;
    float heading = 0;
    float pitch = 0, roll = 0;  // radians (CorporalPitch / CorporalRoll)
    XMFLOAT3 size{4, 4, 4};  // full extents; base at pos.y
    uint32_t color = 0xFFFFFFFF;
    int kind = 0;            // 0 box, 1 spawn marker, 2 water, 3 trigger area, 4 waypoint, 5 road piece
    int helper = -1;         // App::HelperGroup, -1 = drawn in the game
    float road_w = 0, road_l = 0;  // road piece fallback strip (half width, length) when its model is missing
    float radius = 0;        // trigger radius
    std::string tag;         // map label + ring for what the game draws small: vehicle pads, power-ups ("Pad: Atlas")
    GpuModel* model = nullptr;
    std::string fx_model, fx_mtb;  // the model whose .mtb effects this object starts (View > Effects)
    float lo[3] = {-2, 0, -2}, hi[3] = {2, 4, 2};  // local bounds (model bbox or box)
};

// A trigger circle or box as the map gizmo sees it (app_trigger_gizmo.cpp).
struct TrigGeo {
    bool box = false, flat = false;
    float x = 0, z = 0, y0 = 0, w = 40, l = 40, h = 60, r = 30, hd = 0;  // y0 = bottom edge; hd in degrees
    ma2::WObj* o = nullptr;
};

struct Snapshot {
    ma2::Wdf world;
    // The big buffers are shared between snapshots (and with the live document's last snapshot): a new snapshot reuses the
    // previous buffer when the bytes are equal, so an action that leaves the ground alone costs no ground memory.
    std::shared_ptr<const std::vector<float>> hgt;
    float min_h = 0, max_h = 0;
    std::shared_ptr<const ma2::Image> colormap;
    // The light map as it was, so undo / redo put it back exactly instead of baking a new one.
    std::shared_ptr<const ma2::Image> lightmap;
    bool lightmap_custom = false;
    bool lightmap_stale = false, stale_sun_only = false;
    ma2::Vec3 lightmap_sun{};
    ma2::GroundLayers ground;  // (weights shared copy-on-write)
    std::shared_ptr<const std::vector<uint16_t>> typemap;
    bool colormap_custom = false;
    bool shaped = false;  // Next steps '1. Shape the ground' was ticked, so undoing the only stroke unticks it
    // Scenario data outside the world, so trigger, message and objective edits undo too.
    std::string name, description, author, briefing_title, briefing, win_text, lose_text;
    std::vector<std::string> types, objectives;
    std::map<int, std::string> strings;
    std::shared_ptr<const std::map<int, ma2::AudioClip>> audio;
    std::set<uint32_t> selection;  // what was selected when this state was saved, so undoing a delete reselects it
    std::string label;
    int state_id = 0;        // the document state this snapshot holds (the clean mark follows it)
    size_t small_bytes = 0;  // world + strings estimate, computed once when the snapshot is made
};

// True when `w` starts a word of `text` (the start, or right after a non-letter/digit): "tree" is in "dante city tree 01" but
// not in "street". Both lower-case.

// What a piece can be sized as (sizing by dragging).
enum class SizeKind { None, Road, Plate, Run, Water };
struct SizeInfo {
    SizeKind kind = SizeKind::None;
    bool along_x = true;       // Run: the long axis is the model's x
    bool fixed_water = false;  // None because it is a fixed-size water type
    std::string why;           // None: the one line that says why (the Size section shows it)
};

// The orange handles on a selected road piece, plate or run piece (app_size_gizmo.cpp). id = the sides it moves as a bitmask:
// kSideX0 | kSideX1 across the model (left, right), kSideZ0 | kSideZ1 along it (a corner has one of each).
inline constexpr int kSideX0 = 1, kSideX1 = 2, kSideZ0 = 4, kSideZ1 = 8;
struct SizeHandle {
    int id = 0;
    std::string label;           // "start", "left", "front-right", "end 1"
    bool corner = false;
    bool hidden = false;         // its axis is too short on screen
    bool glued = false;          // its end is glued to another piece: it does not drag
    bool near_lock = false;      // a lock icon sits on this end: the handle stands further out
    ma2::Vec3 w{};               // world position
    float sx = 0, sy = 0;        // screen position (viewport + offset, pixels), valid when on_screen
    bool on_screen = false;
};
struct SizeLayout {
    bool show = false;
    std::string reason;          // when !show: "rotate mode", "too small on screen", "not sizable", ...
    SizeInfo si;
    uint32_t uid = 0;
    ma2::crop::Rect whole, kept;
    float y_corner = 0;          // model-space height the handles sit at (kind dependent)
    XMFLOAT3 ax{1, 0, 0}, ay{0, 1, 0}, az{0, 0, 1};   // the model's own axes in the world (tilt and heading followed)
    ma2::Vec3 pos{};             // the object's origin
    std::vector<SizeHandle> h;
    bool part_hidden = false;    // some handles hidden for being too close together
};

class App {
public:
    size_t models_pending() const { return models_.pending(); }
    bool init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, float dpi_scale, std::string* err);
    void shutdown();
    // Builds the UI and handles input. Returns the 3D viewport rectangle in pixels.
    void frame(float dt, int fb_w, int fb_h);
    void render3d();
    bool quit_requested() const { return quit_; }
    bool confirm_close();  // false = cancel closing
    void open_path(const fs::path& p);
    // Scripted actions for --shot runs (developer checks without driving the UI).
    void script(const std::wstring& cmd, const std::wstring& arg);
    void clear_quit() { quit_ = false; }
    // Developer checks (--play dump): a text snapshot of the editor state.
    std::string debug_state() const;
    // --automation (automation.cpp): the editor state as JSON, and the open tab's name.
    std::string automation_state() const;
    // --automation lean reads (app_automation.cpp): selected objects (mode 0) or all objects (mode 1, filter / near / limit) as compact JSON.
    std::string automation_palette(int limit) const;  // the lean `palette` read (app_palette.cpp)
    std::string automation_objects(int mode, const std::string& filter, bool use_near, float cx, float cz, float r, int limit) const;
    std::string automation_handles();  // The size handles that show for the selection, JSON
    std::string automation_joints(float near_px, int limit);  // Join points of placed pieces near the mouse, JSON
    std::string automation_project(float x, float y, float z) const;  // world point -> mouse-command coordinates, JSON
    std::string automation_ground(float x, float z) const;  // heights and ground type at a world point, JSON
    std::string automation_triggers() const;             // The trigger list, sections and counts, JSON
    std::string automation_trigger(uint32_t uid) const;  // One trigger: actions, orders, warnings, JSON (uid 0 = the picked one)
    std::string automation_env();  // The lean `env` read (look, fog, weather, lightning, sun), JSON
    std::string automation_cutscene(uint32_t uid);  // A cutscene's shots, lengths and end actions, JSON (uid 0 = the picked one)
    // --automation 'dialogs' answer for the in-editor "Please confirm" popup, when one is open: the text, or "".
    std::string answer_confirm(bool yes);
    const char* tab_label() const;
    std::string palette_name(const ma2::TemplateInfo& t) const;  // "Friendly (Official)"
    std::string palette_name_base(const ma2::TemplateInfo& t) const;  // palette_name before "variant 2" numbering
    mutable std::map<uint32_t, std::pair<float, float>> measured_dims_;  // template uid -> model box (x, z) for names that hold a "34x34" size
    mutable std::map<uint32_t, int> dup_variant_;                      // template uid -> 1, or 2.. for a later same-name template
    std::string palette_plain_name(const ma2::TemplateInfo& t) const;  // palette_name without the "(Official)" suffix
    std::string water_type_name(const ma2::TemplateInfo& t) const;  // plain name + a look word where two waters share it
    mutable std::map<std::string, std::string> water_names_;        // water model (lower) -> water_type_name
    void dump_palette(const std::string& path);
    void dump_search(const std::string& words, const std::string& path);  // developer: --dump-search "word|FILE"
    void dump_sweep(const std::string& dir);  // developer: --dump-sweep DIR (palette_search.cpp)
    // Hidden / --automation runs: the editor never takes the foreground (Test's return included).
    bool background = false;
    // --automation runs only (not --shot / --play): Test builds its command line but never starts the game (cmd_test).
    bool automation = false;
    // Opened from the game's Custom Scenarios screen (--from-game): File > Exit reads
    // 'Back to the game', Test saves and returns with exit code 2 (the game plays it), and at exit the editor writes
    // the result file (--saved-list: opened=, saved=, test=) the game reads.
    bool from_game = false;
    fs::path from_game_opened;  // the file the game opened it on ("" = start page)
    void start_from_game(const fs::path& file);  // opens `file` (if any) and says how to get back
    int exit_code() const { return exit_code_; }
    void write_saved_list(const fs::path& list) const;
    // --from-game-live (from_game_live.h): the game stays up; Test asks it to play (take_live_test) instead of ending the
    // editor, and closing the editor hands back to it. live_game_event: the game's news (from_game_live.h GS_*).
    bool from_game_live = false;
    struct LiveTest { fs::path file; std::string mode; int red = -1, blue = -1, skill = -1; };
    bool take_live_test(LiveTest& out) { if (!live_test_pending_) return false; out = live_test_; live_test_pending_ = false; return true; }
    void live_game_event(int state, const std::wstring& note);
    void game_ended();   // the game closed under a live editor: it carries on as the standalone editor
    fs::path doc_path() const;
    std::vector<uint32_t> dev_placed_;  // units placed by --place (developer checks)
    // Releases GPU objects retired during the frame (after Present).
    void end_frame();
    // Device loss: drop every GPU object, then rebuild them on the new device.
    void device_lost();
    bool device_restored(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string* err);
    // Autosave / crash recovery (%LOCALAPPDATA%\MechAssault2\Autosave).
    bool emergency_save();  // true when the recovery slot was written
    bool has_unsaved() const;  // an editable scenario with changes not saved to its file
    void set_base_dpi(float scale);  // WM_DPICHANGED: the new monitor scale (1 = 96 dpi)
    void set_raw_style(const ImGuiStyle& unscaled);  // the theme before DPI scaling (prefs.cpp)
    void offer_recovery();
    // Offers to re-save the scenarios in the scenario folder that an older editor wrote (File > Update old scenarios... passes from_menu).
    void offer_update_old_scenarios(bool from_menu);

    // a real MACinema at the view centre (Level tree, trigger effect); intro = over the mission's player start,
    // path behind its facing; selects it, returns its UID
    void play_cutscene_at_start(uint32_t cutscene_uid);  // a 0 s timer + Play action for it (the caller holds the undo step)
    uint32_t new_cutscene(bool intro = false, bool own_undo = true);  // own_undo false: part of the caller's undo step
private:
    bool autosave(bool force);
    void clear_autosave();
    // --- undo helpers for scenario data edited outside the world
    void push_undo_once(const std::string& label);  // one undo step per widget edit (call when it changes)
    // --- lifecycle
    bool load_game(std::string* err);
    void set_document(std::unique_ptr<ma2::Scenario> s);
    void reset_view();
    void document_changed(bool terrain = true, bool textures = true);
    void world_changed();
    // --- file ops
    void cmd_new();
    void cmd_new_kind(bool multiplayer);  // the New dialog with map / mission already chosen
    void cmd_open();
    void cmd_save(bool save_as);
    void cmd_test();
    void open_retail(const ma2::MapInfo& m);
    void make_editable_copy();
    // --- undo
    void push_undo(const std::string& label);
    // One undo step for a run of the same quick action on the same selection (Q / E turns, nudges): the first pushes, the
    // ones after it (same tag, nothing undone, under 1.5 s apart) change the document inside that step.
    void push_undo_merge(const std::string& label, const std::string& tag);
    Snapshot drop_last_undo();                    // takes the step just pushed back (the action found nothing to do)
    void mark_untracked_change();                 // the document changed with no undo step: the saved mark can never come back
    void check_untracked_change();                // ... noticed from doc_->dirty with no step since the save
    void note_saved();                            // the file on disk now holds this state
    uint64_t undo_bytes(uint64_t* parts = nullptr) const;  // parts[7]: world+meta, heights, colours, types, ground weights, ground layers, audio                  // snapshot memory, shared buffers counted once
    void trim_undo();                             // step cap, byte budget, the "history is full" note
    void sky_picker(ma2::Scenario& s);
    void undo();
    void redo();
    Snapshot snapshot(const std::string& label) const;
    void restore(const Snapshot& s);
    // --- views
    void rebuild_objects();
    void rebuild_overlay();
    void rebuild_minimap();
    void build_frame_geometry();
    // --- input / tools
    void handle_viewport_input(float dt);
    bool cursor_ground(ma2::Vec3& out);
    bool cursor_object_top(ma2::Vec3& out);  // placing: roof / deck under the cursor above the ground
    int pick_object(float mx, float my, std::vector<int>* all = nullptr);  // all: every root under the cursor, nearest first (Alt+click cycling)
    void apply_brush(const ma2::Vec3& c, float dt, bool first);
    void place_object(const ma2::Vec3& at);
    // The scatter brush (app_scatter.cpp)
    struct ScatterKeep { ma2::place::Sheet s; float x0 = 0, x1 = 0, z0 = 0, z1 = 0; };   // an area to keep off, with its box on the map
    struct ScatterErase { uint32_t uid; float x, z; };
    struct ScatterRun {
        bool erase = false;
        ma2::Coverage cov;
        ma2::SpacingGrid grid;
        float accum = 0;
        int placed = 0, erased = 0, close = 0, keepoff = 0, steep = 0, frame_placed = 0;
        std::set<uint32_t> kinds;
        std::vector<ScatterKeep> keep;
        std::vector<ma2::place::Sheet> sheets;
        std::unique_ptr<ma2::place::SurfaceIndex> surf;
        std::vector<ScatterErase> cand;
        std::set<std::string> used;
        std::map<std::string, int> name_n;
    };
    struct ScatterLast { int placed = 0, erased = 0, close = 0, keepoff = 0, steep = 0, kinds = 0; bool erase = false, ran = false; };
    std::unique_ptr<ScatterRun> sc_run_;
    ScatterLast sc_last_;
    std::unordered_map<uint32_t, float> scatter_r_;     // footprint radius per template (cached once its model is loaded)
    ma2::Rng sc_rng_{777};
    float scatter_radius(uint32_t tpl);
    void scatter_frame(const ma2::Vec3& g, bool lclick, bool ldown, bool lrelease, bool have_ground);
    void scatter_begin(bool erase);
    void scatter_end();
    void scatter_path(const ma2::Vec3& a, const ma2::Vec3& b);
    void scatter_dab(const ma2::Vec3& p);
    void scatter_try_one(const std::vector<std::pair<float, float>>& cells);
    bool scatter_blocked(const ScatterRun& r, float x, float z, float rad) const;
    uint32_t place_scattered(uint32_t tpl, ma2::Vec3 at, float heading, float pitch, float roll);
    int scatter_so_far() const { return sc_run_ ? (sc_run_->erase ? sc_run_->erased : sc_run_->placed) : -1; }
    void move_selection(const ma2::Vec3& delta, bool snap_ground);
    void rotate_selection(float delta);
    void delete_selection(const char* undo_label = "delete");
    void cut_selection();  // copy + delete, one undo step
    void copy_selection();
    void paste(const ma2::Vec3& at);
    bool place_drag_ = false;      // left button down while placing: the drag aims the heading
    ma2::Vec3 place_drag_pos_;
    bool place_drag_joined_ = false;  // the drag started on a road piece / wall / bridge / foundation joined to its neighbour: it keeps the join's facing
    float place_drag_mx_ = 0, place_drag_my_ = 0;  // mouse (client px) at the press: a drag aims only once the mouse really moved
    void snap_selection_to_ground();
    float model_foot(const ma2::WObj& o);
    void lift_to_foot(uint32_t root);
    int add_missing_spawns();
    bool add_type_kit(const std::string& game_type);  // the type's flags/bases from a real level
    bool check_errors_only_ = false;  // Check window: "Must fix only" toggle
    bool check_fixed_ = false;  // a Check fix button ran this frame: close the window if nothing is left
    bool has_ground_model(const ma2::WObj& o);
    // Cropped road pieces and foundations (app_crop.cpp, core/crop.h). playtest 11-01: roads and foundations can be cut to
    // fit a gap; foundations snap edge to edge and rest a little above the ground.
    static std::string crop_key(const ma2::WObj& o);  // its EditorCrop, "" = whole
    GpuModel* obj_model(const ma2::WObj& o);          // Corporal3DObj + CorporalMTB as drawn: cut to the crop
    bool is_foundation(const ma2::TemplateInfo& t) const;  // building bases, parking lots, footings (flat ground plates)
    bool is_foundation_obj(const ma2::WObj& o);
    bool croppable(const ma2::WObj& o);                    // size_info(o).kind is Road, Plate or Run
    // Who can be cut and why not. The same model test the save uses (crop::cuttable through ScenarioIO::model_cuttable), so the
    // editor never offers a cut the game will not get. `why` is the one dim line the Size section shows when kind is None.
    SizeInfo size_info(const ma2::WObj& o);
    GpuModel* whole_model(const ma2::WObj& o);             // the uncut model of a road piece / plate / run piece (null while unknown)
    bool whole_rect(const ma2::WObj& o, const SizeInfo& si, ma2::crop::Rect& r);   // the uncut rectangle in model x / z (a run's other axis has 5 cm of margin)
    // The one place a crop changes (panel fields now, handles later): crop::set (nullptr = whole again), the Indestructible rule for a cut
    // run piece whose wreck would be the whole model, sheets re-seated, dirty flags. The caller has pushed the undo step.
    void apply_crop(ma2::WObj& o, const ma2::crop::Rect* r);
    // Origin height that puts a plate model (at x, z, heading yaw) place::kSheetLift above the highest ground under its footprint
    // (a thick foundation; thin plates and roads are sheets, below).
    float plate_rest_y(const GpuModel* gm, float x, float z, float yaw);
    bool foundation_rest(const ma2::WObj& o, float& origin_y);
    void size_ui(ma2::WObj& o);   // Properties > Size: spatial fields around a top-down outline, the why line, Whole again
    void size_targets(const ma2::WObj& o, const SizeInfo& si, std::vector<ma2::snap::EdgeTarget>& out);   // What a dragged cut edge can meet
    // Orange size handles on the map (app_size_gizmo.cpp). size_hot_ (a handle id, 0 = none) is shared with the Size panel, which
    // lights a field's handle and reads the hot handle for its own highlight.
    bool size_layout(SizeLayout& lo);                       // what shows for the selection now (empty + reason when nothing)
    bool size_gizmo_update(bool lclick, bool ldown);        // true when it used the mouse
    void size_gizmo_draw();
    ma2::Json size_gizmo_json() const;                      // state.size_gizmo
    int size_selection_variants();                    // cut variants the model cache holds of the selected piece's model (state.model_cache_crops)
    struct SizeDrag {
        int id = 0;                 // handle id being dragged (0 = none)
        uint32_t uid = 0;
        ma2::crop::Rect start, whole;
        SizeInfo si;
        XMFLOAT3 p0{}, ax{}, az{}, up{};   // the handle's world point at the press, the model axes in the world
        float sx = 0, sz = 0, press_x = 0, press_y = 0;
        float hx = 0, hz = 0;       // plane hit at the press (corners)
        bool began = false, moved = false, pushed = false, keep_status = false, over = false;
        std::string model, mtb;
        // Joints for the cut edge. Targets are built once at the press (world frame); hit[] / jt[] hold the hold-over of each of the
        // (up to) two dragged sides (an X side, a Z side) and what the drawing shows for it.
        ma2::Vec3 pos{};
        ma2::Vec3 start_pos{};     // water: its centre at the press (a water drag moves the centre by half the change)
        std::vector<ma2::snap::EdgeTarget> targets;
        ma2::snap::EdgeHit hit[2];
        struct JointFx { bool on = false; float ex = 0, ez = 0, tx = 0, tz = 0, dx = 0, dz = 0; int kind = 0; uint32_t uid = 0; } jt[2];
        std::string joint_text;    // " - meets the end of X" / " - flush with X" while a joint holds
    };
    SizeDrag size_drag_;
    int size_hot_ = 0;
    std::string size_readout_, size_label_, size_cursor_;
    double size_readout_until_ = 0;
    // The handles scale with the UI scale and, on a very wide map view (3840 x 1600), up to 1.8 x more, so they stay easy to see and grab.
    float size_scale() const { const float f = float(vp_w) / 1300.f; return dpi_ * (f < 1.f ? 1.f : f > 1.8f ? 1.8f : f); }
    int size_panel_frame_ = -100;   // the frame the Size panel last ran (a stale size_panel_hot_ is dropped)
    int size_panel_hot_ = 0;        // the Size panel sets this every frame to the handle id of the field under the mouse (0 = none): that handle lights up on the map
    int size_last_press_ = 0, size_cursor_id_ = 0, size_icons_frame_ = -1;
    uint32_t size_icons_uid_ = 0;
    uint32_t size_last_press_uid_ = 0;
    bool size_sizable_ = false;     // the size handles show for the selection (for the hint line)
    bool size_water_ = false;       // ... and the selection is a resizable water (hint: size and Level)
    bool size_small_ = false;       // ... but every handle is hidden for being too small on screen
    std::string size_hint_;         // the hint line while a handle is hot
    void size_free_variants(uint32_t uid);                  // after a drag: frees the cut variants of that model that no object uses
    bool size_side_label(const SizeLayout& lo, int id, std::string& out) const;
    // Sheets and layers (app_sheets.cpp, core/placement.h): roads and thin plates are stacked by the order they were
    // put down in, each resting on the ground or on the sheets below it.
public:
    struct GroundSurf { ma2::Vec3 c[4]; ma2::Vec3 p, n; };      // a flat or tilted surface the ground is shaped to: four corners and a plane
    struct SheetSet { std::vector<ma2::place::Sheet> s; std::vector<int> obj; };  // obj[i] = world object index of s[i]
private:
    int sheet_kind(const ma2::WObj& o);                       // 0 not a sheet, 1 road piece, 2 thin plate
    bool sheet_geom(const ma2::WObj& o, int kind, ma2::place::Sheet& s);
    SheetSet sheets(bool with_runs = true);                   // every sheet of the map; runs = plates joined edge to edge
    bool sheet_is_fixed(const ma2::WObj& o);                  // keeps the height it has (tilted, glued, lifted by hand, old file)
    void write_layer(ma2::WObj& o, int layer, bool fixed);
    struct Standing { uint32_t uid; float x, z, y0, foot0, surface0; uint32_t on0; bool fixed_sheet = false; uint32_t root_uid = 0; };  // an object resting on a sheet (or the ground), as it was before a change
    int reseat_sheets(SheetSet* given = nullptr, const std::vector<Standing>* before = nullptr);  // runs place::seat, writes the heights, carries what stood on them, returns how many sheets moved
    void adopt_sheets();                                      // a map opened for editing: sheets without a layer get "<rank> fixed"
    void sheet_dropped(uint32_t root, bool force_fixed = false);  // a new / pasted sheet goes on top of what it overlaps
    void mark_selection_fixed();                              // lifted or tilted by hand: it keeps that height
    void layer_forward();
    void layer_back();
    void reseat_command();                                    // "Put flat pieces in order"
    bool sheet_ghost_y(const ma2::Vec3& origin, float& y, const ma2::Terrain* ground = nullptr);  // ground: a terrain with a planned edit applied    // the height the click will give the road piece / plate being placed
    void sheet_layer_ui(ma2::WObj& o);                        // Properties: layer row and buttons
    void sheet_to_ground(uint32_t root);                      // G on a sheet
    void commit_layers(const SheetSet& ss, const std::vector<int>& before);  // writes the layers that changed since `before` (drop_layer may renumber)
    // What objects stand on (app_sheets.cpp): the top sheet under their origin, else the ground.
    float stand_foot(const ma2::WObj& o);
    float span_float(const ma2::WObj& o);  // A long wall / fence over a slope: how far the lowest surface under its footprint is below its foot (0 = fine or not a wall)
    std::set<uint32_t> ground_still_;      // Pieces a ground edit must not carry (Fit ground to road: the road is what the ground is fitted to)
    void stand_xz(const ma2::WObj& o, const ma2::Vec3& p, float& x, float& z);  // the middle of the model's base, where it stands                     // origin height above the model's lowest point (+2 m for a player start)
    float surface_at(float x, float z, float below_y = 1e30f, uint32_t* on_uid = nullptr);   // the top surface under a map point
    std::vector<Standing> standing_now(const SheetSet& ss, bool whole_map = false);  // objects (no positioned ancestor) resting on a sheet or the ground near sheets
    int follow(const std::vector<Standing>& before, const SheetSet& now);
    int follow_moved_ = 0;                                   // how many objects the last reseat_sheets carried
    // Ground tools (app_ground_edit.cpp): one plan, one protection rule, one way to carry what stands on the ground.
    struct GroundEdit {
        int gx0 = 0, gz0 = 0, gx1 = -1, gz1 = -1;            // grid rectangle
        std::vector<float> want;                             // new fine height per grid point of the rectangle; NaN = leave
        std::vector<unsigned char> core;                     // per grid point: 1 = at full height (under / right beside a piece), 0 = only blended
        float up = 0, down = 0;                              // largest rise and fall of the drawn ground (m)
        std::vector<uint32_t> kept;                          // buildings and sheets whose ground was left alone
        std::vector<float> kept_xz;                          // their footprints, 8 floats each, for the preview
        std::vector<uint32_t> joined;                        // road pieces joined end to end to the pieces the edit is for (they must not ride with the ground)
        int followers = 0;                                   // objects that would move with the ground
        bool clamped = false;                                // part of it hit the height range
        bool empty() const { return gx1 < gx0 || gz1 < gz0; }
        float core_change(const ma2::Terrain& t) const;      // the biggest change at a full-height point (0 = the ground already fits; blends alone do not count)
    };
    enum class Protect { OthersOnly, Everything };
    std::vector<Standing> ground_standing();                 // everything that stands on the ground or on a sheet, map wide, plus the fixed sheets
    int ground_changed(const std::vector<Standing>& before); // after the ground moved: fixed sheets follow, the stack re-seats, objects follow
    bool object_footprint(const ma2::WObj& o, ma2::place::Sheet& s);  // the turned footprint of an object with a model (sheets: their own)
    void protect_ground(GroundEdit& e, const std::set<uint32_t>& own, Protect mode, const std::vector<ma2::place::Sheet>* extra_own = nullptr);  // extra_own: pieces not placed yet
    void forecast_ground(GroundEdit& e, bool with_followers = true);  // up, down, followers
    int apply_ground_edit(const GroundEdit& e, const char* undo_label, bool push = true);  // returns how many objects moved with the ground; push = false: the caller already pushed the undo step
    void draw_ground_edit(const GroundEdit& e);              // the preview: changed cells and kept objects
    GroundEdit plan_fit_ground(const std::vector<uint32_t>& roads);
    GroundEdit plan_surfaces(const std::vector<GroundSurf>& surfs, float shoulder);  // the shared plan: full height out to `shoulder` past each surface, a 14 m blend
    GroundEdit fit_plan_selection(std::set<uint32_t>* own, int* road_count);  // plan + protection + forecast for the selected road pieces
    std::string ground_edit_note(const GroundEdit& e);
    std::string fit_summary_names(const GroundEdit& e);     // "Office 07 and 1 other"       // "Raises the ground up to 3.2 m, ..."
    std::vector<Standing> stroke_standing_;                  // what stood on the ground when the stroke began (absolute: every frame is safe)
    std::vector<Standing> range_standing_;                   // the same while the height range is dragged
    std::vector<float> range_steps0_;
    bool range_active_ = false;
    GroundEdit ground_preview_;                              // the hovered tool's plan, drawn over the map
    int ground_preview_frame_ = -10;
    std::string ground_preview_text_;
    void fit_button();  // each moves with its surface; glued groups and bridge runs as one
    // Slopes (app_slope.cpp): a flat piece placed where the ground slopes is levelled into it, tilted onto it, or left floating.
    struct SlopePlan {
        bool active = false;                 // the ground slopes under the piece (float_gap over float_tol): a choice is on offer
        SlopeMode wanted = SlopeMode::Float, mode = SlopeMode::Float;  // what the user chose / what will happen
        bool fell_back = false;              // Tilt asked, the piece cannot: it floats
        bool plate = false, joined = false;
        float gap = 0;                       // float_gap of the level piece
        float left = 0;                      // Tilt: what still floats after the tilt
        float buried = 0;                    // Float, joined: how far the ground rises over the piece kept level at the joint
        bool joint_kept = false;             // Float, joined: kept at the joint's height
        bool fits_ground = false;            // Tilt, road piece: the click also shapes the ground under it to meet it
        float fit_up = 0, fit_down = 0, fit_left = 0;  // after that fit: how far the ground rose / fell, what still floats
        int fit_moved = 0;                   // objects that rode with the ground
        float up = 0, down = 0;              // Level: how far the ground rises / falls
        float pitch = 0, roll = 0, y = 0;    // Tilt: the pose; y = origin height the ghost is drawn at (Level and Float too)
        float T = 0;                         // Level: the height the ground goes to
        bool pin_y = false;                  // Level: ground left alone beside the piece is higher than the levelled plate; the piece rests on the plate (y), not on that ground
        ma2::Vec3 pos;                       // the origin as planned (Tilt: after tilting)
        ma2::place::Sheet sheet;             // the piece in its planned pose
        GroundEdit edit;                     // Level: the ground edit
        std::vector<ma2::place::Sheet> pieces;
    };
    SlopePlan slope_plan_;                   // this frame's plan for the ghost (handle_viewport_input)
    bool ghost_sheet(const ma2::Vec3& origin, ma2::place::Sheet& g, bool& plate);  // the piece being placed, level, at that origin
    SlopePlan plan_slope(const ma2::Vec3& origin);
    GroundEdit plan_level(const std::vector<ma2::place::Sheet>& pieces, float T);
    SlopeMode& slope_mode_ref();             // the remembered mode of the family being placed
    void cycle_slope_mode();
    void apply_slope_ground(const SlopePlan& p);          // Level: shape the ground (inside the placing's undo step)
    void apply_slope_pose(uint32_t root, const SlopePlan& p);  // Tilt: the placed piece takes the planned pose and keeps it
    void lower_to_plate(uint32_t root, float y);               // Level,: the placed piece (and what is glued to it) rests on the levelled plate at origin height y
    std::string slope_status(const SlopePlan& p, int moved);
    void draw_slope_ghost();                 // stilt lines and the gap label (Float), drawn over the map
    void slope_ui(ma2::WObj& o);             // Properties: the floating line and the two buttons
    void slope_level_selected();
    void slope_tilt_selected();
    void slope_placing_row();                // the "On a slope:" row of the placing options
    int slope_moved_ = 0;                    // objects the last Level carried
    std::string slope_status_text_;          // the status line the last placing on a slope gave (the join hint does not overwrite it)
    std::string slope_kept_;                 // "Office 07 and 1 other": whose ground it left alone
    float road_joint_y_ = 0, road_joint_roll_ = 0, road_joint_px_ = 0, road_joint_pz_ = 0;  // ...and the own end (local x, z) the ghost joins by  // the open end the road ghost is joined to: its height and roll
    bool road_joint_ok_ = false;
    float above_surface(const ma2::WObj& root);              // origin height over the surface it stands on (kept when it is copied)
    void put_on_surface(uint32_t root, float above);         // origin = surface under it + above; the whole subtree moves
    void where_it_sits_ui(ma2::WObj& o, int idx, float foot);
    void stand_fields(ma2::Json& row, const ma2::WObj& o, const SheetSet& ss, bool full);  // state / lean reads: ground, surface, gap, on (and a sheet's layer, fixed, float_gap)
    double paving_time_ = -1; float paving_x_ = -1e9f, paving_z_ = -1e9f, paving_ = std::numeric_limits<float>::quiet_NaN();  // Properties: the dim line under Position
    bool stand_text(const ma2::WObj& o, int idx, float foot, std::string& text, bool& amber);
    std::wstring window_title_;
    bool auto_scale_done_ = false;
    bool status_two_ = false;                                 // narrow window: the status bar has two lines
    std::string status_amber_;                                // when status_ equals this it is drawn amber
    // The lift handle's floor: the lowest total rise a drag may give (0 = not lower than where it started, unless it began below the surface).
    float lift_floor_ = -1e30f;
    bool lift_started_ = false;
    bool lift_amber_ = false;
    void lift_begin();                                        // at the first frame of a lift drag / the "h" box drag
    std::string lift_readout(float delta = std::numeric_limits<float>::quiet_NaN());  // "Height 12.40 m (+2.00), 2.28 m above the ground"
    float lift_clamp(float total_wanted);                    // the floor applied (Shift = free) + the status line
    bool lift_stopped_ = false;                               // the last lift_clamp stopped at the floor
    // Nudge / raise keys, the readout label, ghost lift, World / Local arrows, group pivot.
    void nudge_selection(int fwd, int right);                 // arrow keys: 1 m (Shift 10 m) along the world axis nearest the camera
    void raise_selection(float dy);                           // PgUp / PgDn: 0.5 m (Shift 5 m), stops at the floor
    void set_readout(const std::string& text);                // the floating label (about 1 s) and the status line
    void readout_label(const std::string& text);              // the floating label only
    std::string gizmo_drag_text() const;                      // "Heading 105 deg (+15)" for the drag in progress
    void draw_readout();                                      // the label, near the mouse while dragging, else at the handles
    std::string readout_;
    double readout_until_ = 0;
    bool readout_amber_ = false;
    bool gizmo_local_ = false;                                // the Move arrows follow the first selected object's heading
    float knob_x_ = -1, knob_y_ = -1, pivot_x_ = -1, pivot_y_ = -1;  // where the lift knob and the handles' centre were drawn (screen px; -1 = not drawn), for the automation
    bool turn_each_ = false;                                  // Q / E turn each selected object in place (else the group about its centre)
    float place_lift_ = 0;                                    // PgUp / PgDn while placing: how far above the click the next piece lands
    int place_lift_tool_ = -1; uint32_t place_lift_tmpl_ = 0; // what place_lift_ belongs to (it resets when either changes)
    ma2::Vec3 body_move_{};                                   // a body drag's total move, for the readout
    float lift_floor_y(const ma2::WObj& root);               // origin height at which the object's foot is on the surface under it (a sheet: the height the stack gives it)
    std::map<uint32_t, std::set<uint32_t>> move_overlap0_;    // while a drag runs: the sheets each moved sheet overlapped when it began
    std::map<uint32_t, float> reseat_moves_;                  // last reseat: uid -> how far it moved (m)
    int reseat_moved_ = 0;
    float reseat_most_ = 0;
    int piece_snap_obj_ = -1;                                 // world object index of the plate the ghost is joined to
    float preview_y_ = std::numeric_limits<float>::quiet_NaN();  // origin height of the ghost as drawn (state JSON preview.y)
    std::string sheet_note_;                                  // shown when the status line is free
    bool fixed_notice_ = false;
    std::unordered_map<uint32_t, char> foundation_cache_;     // template uid -> is_foundation
    void crop_draw_full(const ObjView& v);  // the uncut outline, dim, while a cropped piece is selected
    std::vector<int> selected_roots() const;
    // --- UI
    void ui_menu();
    void ui_tabs();
    void ui_left_panel();
    void ui_right_panel();
    void ui_status();
    void ui_new_dialog();
    void ui_retail_dialog();
    void ui_message_box();
    void tab_map();
    void tab_terrain();
    // The Build tab (tool strip + one page at a time; app_strip.cpp, pages in app_tabs.cpp)
    void ui_strip(float x, float y, float w, float h);
    void open_page(Page p);
    void normalise_tab();
    void ui_build_tab();
    void page_select();
    void page_browser(BrowserMode m);
    void palette_header(BrowserMode m);
    void palette_body(BrowserMode m);
    void palette_footer(BrowserMode m);
    // Graphics memory: the live total from the per-template figures, the cell cost and the footer line.
    void gfx_reset();
    struct ThemeContent { int buildings = 0, bases = 0, trees = 0; std::vector<uint32_t> sample; };  // New dialog: what a theme comes with
    const ThemeContent& theme_content(const ma2::MapInfo& m);
    std::map<std::string, ThemeContent> theme_content_;
    // The ground colour a new map on this theme shows (ScenarioIO::theme_ground), 0 when unknown; cached per level.
    uint32_t theme_ground_rgb(const ma2::MapInfo& m, bool compute);
    std::map<std::string, uint32_t> theme_ground_;
    void gfx_live_update(bool force = false);       // each frame: recount when the template set changed (4 uncached templates a frame)
    void gfx_calibrate(double exact_total);         // Check / save: the exact figure replaces the live one (the difference is kept as an offset)
    float gfx_cell_mb(uint32_t uid, bool compute);  // MB one other-level object adds; -1 until computed
    void gfx_footer_line();                         // "Graphics memory: about N of 64 MB" (Place, Scatter, Roads & walls)
    double gfx_live_total() const { return gfx_raw_ + gfx_offset_; }
    void page_sculpt();
    void page_paint();
    void page_water();
    void page_starts();
    void water_new_ui();
    void water_list_ui();
    void page_body_begin(const char* id);
    void page_body_end();
    void page_footer_begin();
    void page_footer_end();
    void scatter_header_ui();
    void ui_splitter(float x, float y, float h);
    void tab_environment();
    void draw_sun_arrow();  // Environment tab: where the sunlight comes from, at the view centre (app_environment.cpp)  // Environment tab: time of day, weather, fog, sun, sky (app_environment.cpp)
    void tab_players();
    void tab_units();
    void player_starts_ui(ma2::Scenario& s);  // Units tab: start per side + game types (MP), Move player start (missions)
    void tab_triggers();
    void tab_cameras();              // Cameras & Cutscenes tab: cutscenes + cameras (app_cutscenes.cpp)
    void tab_cutscenes();            // its Cutscenes part (app_cutscenes.cpp)
    void tab_sets();                 // Triggers tab > Sets (app_sets.cpp)
    int drop_dead_set_members();     // after deleting objects: remove their uids from every MoeEntitySet; returns how many
    uint32_t add_entity_set(const std::string& name);  // an empty MoeEntitySet (no blank member slots); 0 on failure
    uint32_t entity_set_from(const std::string& name, const std::vector<uint32_t>& units);  // a set holding these units
    uint32_t set_uid_ = 0;           // the set the Sets view shows
    void tab_orders_by_unit();       // Triggers tab > AI orders, by unit (app_cutscenes.cpp)
    int trig_mode_ = 0;              // 0 = Triggers, 2 = AI orders by unit, 3 = Sets (1 = Cutscenes moved to its own tab)
    uint32_t cine_uid_ = 0;          // the cutscene the Cutscenes view shows
    // Cutscene preview (Cutscenes > Preview): the editor camera flies each shot's eye path looking at its focus; Esc stops.
    struct CutPreview { bool on = false, paused = false; uint32_t cin = 0, still = 0; float t = 0, stop_at = 0; Camera saved; bool helpers = true; } cpv_;  // still: Look through that camera
    void cutscene_preview_start(uint32_t cinema_uid, int shot = -1);  // shot >= 0: only that shot (0-based)
    void camera_look_through(uint32_t cam_uid);  // Cameras list: view from a camera no shot uses (cpv_.still)
    void cutscene_preview_at(uint32_t cinema_uid, float t, bool paused);  // timeline: show the camera at second t (paused = a scrub), from where the preview left off
    void reorder_shot(uint32_t cinema_uid, int idx, int dir, bool own_undo = true);  // swap shot idx (0-based) with its neighbour; end actions follow the last shot
    // The cutscene timeline strip under the map view (app_timeline.cpp). tl_h_ is the height it takes from the map (0 = not shown).
    struct Timeline {
        int shot = 0;                    // the selected shot (0-based)
        uint32_t ev = 0;                 // the selected event
        float playhead = 0;              // seconds
        int drag = 0;                    // 0 none, 1 shot length edge, 2 shot body (reorder), 3 ruler
        int drag_shot = -1;              // the shot being dragged
        float drag_x0 = 0, drag_len0 = 0, drag_scale = 0;
        bool drag_pushed = false, drag_moved = false, auto_folded = false;
        float rect[4] = {0, 0, 0, 0};    // x, y, w, h of the strip on screen
        uint32_t last_cin = 0;
        uint32_t drag_ev = 0;            // drag == 4: the event marker being dragged
        float drag_ev_t = 0;             // where it would land (seconds)
        float add_t = 0;                 // the moment the open "+ Add at" menu adds at
        bool add_end = false;            // ... and whether it adds to "When it ends"
    } tl_;
    // Events on the timeline: the Add menu, the Event editor of the Cameras & Cutscenes panel, the unit pick for a unit order.
    void cut_add_menu(uint32_t cin, float t, bool at_end);       // the menu items of "+ Add at t" and "When it ends [+]"
    void cut_event_panel(uint32_t cin);                          // left panel: the picked event (or "When it ends" action) with its editor
    std::string cut_event_label(const ma2::CutEvent& e);         // a short label for the strip and the panel
    void pick_cut_unit(float mx, float my);                      // PickTarget::CutUnit: the click on the unit that gets the order
    float tl_h_ = 0;
    std::function<void()> tl_later_;      // a structural edit from the strip's or the panel's menus: runs at the top of the next frame
    float timeline_height();              // height the strip takes at the bottom of the map view for the current tab, 0 when hidden
    void timeline_draw();                 // draws the strip and handles its mouse; call once the map rectangle (without the strip) is known
    bool timeline_wanted() const;
    // Framing a shot (app_cutscenes.cpp): the map view becomes the shot's camera view with the game band drawn; Enter writes the
    // view into the shot, Esc leaves. which: 0 = still ("Frame it"), 1 = start, 2 = end.
    struct Framing { bool on = false; uint32_t cin = 0, ctrl = 0; int which = 0; Camera saved; } frame_;
    void frame_begin(uint32_t ctrl, int which);
    void frame_leave();                          // back to the editor camera
    void frame_use();                            // Enter: write the framed view, then leave
    void frame_step();                           // frame(): the zoom, the band and the banner
    void shot_use_view(uint32_t ctrl, int which);  // the view as it is now (framed or not) becomes the shot's camera
    ma2::Vec3 cam_eye_now() const;                    // where the editor camera's eye is, from its target / angles / distance
    void game_band(float& x, float& y, float& w, float& h) const;  // the 2.35:1 picture the game shows, inside the map view
    struct ShotGlyph { std::string label; float sx, sy; uint32_t uid; };  // "2a" / "2b" labels drawn on the map (screen px)
    std::vector<ShotGlyph> shot_glyphs_;
    void shot_glyph_lines();                     // build_frame_geometry: a small frustum at each shot's start and end
    void shot_glyph_labels();                    // frame(): the labels over the map
    void cutscene_preview_stop();
    void cutscene_preview_step(float dt);  // frame(): sets cam_ from the shot at cpv_.t, draws the letterbox
    bool ui_field(ma2::WObj& o, const char* key);  // one setting by game name, dictionary widget (app_tabs.cpp)
    bool ui_common_fields(ma2::WObj& o);  // dictionary widgets for an object's Common settings (app_tabs.cpp)
    // Action editors shared by the Triggers tab and the cutscene timeline (app_actions.cpp). They write straight into the
    // object and wrap each edit in one undo step. Structural edits go into `later`, which the caller runs after its lists are done.
    void trig_touch(const char* label);
    void ui_float(ma2::WObj& o, const char* key, const char* label, const char* tip = nullptr);
    void ui_int(ma2::WObj& o, const char* key, const char* label, const char* tip = nullptr);
    void ui_bool(ma2::WObj& o, const char* key, const char* label, const char* tip = nullptr);
    void ui_str(ma2::WObj& o, const char* key, const char* label);
    void ui_enum(ma2::WObj& o, const char* key, const char* label, std::vector<std::pair<int, const char*>> opts);
    void ui_link(ma2::WObj& o, const char* key, const char* label, bool triggers, const char* set_key, std::function<void()>& later);
    void ui_cue(ma2::WObj& o, const char* key, bool music, bool voice = false);
    void ui_text(ma2::WObj& o, const char* key, const char* label, bool optional);
    void ui_action_body(ma2::WObj& trigger, ma2::WObj& a, std::function<void()>& later);  // the settings of one action (the if-chain per class)
    void pick_link_unit(float mx, float my);  // PickTarget::LinkUnit: the click on a unit sets the link
    void go_to_object(uint32_t uid);          // the camera moves to a placed object
    uint32_t mark_trigger_ = 0, open_cue_for_ = 0, open_action_ = 0;  // a 'Complete objective' needing its radar target; a sound picker to open; a new action to open
    int mark_objective_ = -1;
    bool trig_name_focus_ = false;  // the Name box of the picked trigger takes the keyboard (New trigger / Duplicate / Rename)
    // The trigger list: every trigger, nested ones under their parent; level = one of the base retail level's own.
    struct TrigRow { int idx, depth; bool level; };
    struct TrigList { std::vector<TrigRow> rows; int cutscene_hidden = 0; };
    TrigList trigger_list(const ma2::WTree& t, const std::vector<char>& kind, uint32_t keep_uid) const;
    bool trig_level_open_ = false;                      // the "From the level" section is open
    mutable const ma2::Wdf* level_cache_world_ = nullptr;
    mutable std::unordered_map<uint32_t, uint32_t> level_cache_;  // the base level's trigger uid -> class
    bool ui_unit_arrives(uint32_t uid, const std::vector<uint32_t>& hiders);  // Properties: one 'Arrives' row for a unit (app_spawner.cpp)
    uint32_t arrival_circle();  // a new 'Reinforcements arrive' circle, placed by the next map click
    uint32_t pad_vehicle(uint32_t uid);  // a vehicle pad's stand, spawner or vehicle -> the waiting vehicle, else 0 (app_spawner.cpp)
    void ui_spawner(uint32_t uid);  // Properties: a spawner's start (at the start / when a trigger fires) and counts (app_spawner.cpp)
    void ui_unit_orders(uint32_t uid);    // unit Properties: 'Starting orders' summary + Give orders (app_triggers.cpp)
    // The orders chooser (Walk a path once / Patrol / Defend / Follow the player / Attack the player / Attack a unit).
    void order_menu_items(uint32_t trig, uint32_t unit, bool turret);  // the menu entries; a pick calls give_order
    void give_order(uint32_t trig, uint32_t unit, int kind);           // asks first when the unit is Neutral (it has no AI)
    uint32_t make_order(uint32_t trig, uint32_t unit, int kind, int side = -1);  // trig 0 = a new start-of-mission trigger, opened
    void pick_order_unit(float mx, float my);                          // PickTarget::OrderUnit: the click on the unit that gets orders
    struct OrderReq { uint32_t unit = 0, trig = 0; int kind = -1; bool valid = false; } order_req_;  // a chooser pick, handled after the panels
    bool neutral_ask_active_ = false;
    void order_button(uint32_t trig, uint32_t unit, const char* label, const char* tip, bool full_width);  // opens the chooser
    void ui_neutral_orders();                                          // the "Neutral units ignore orders" popup
    int ui_neutral_fix(uint32_t unit);                                 // red line + side buttons for a Neutral unit with orders: the side chosen, else -1
    void set_unit_side_now(uint32_t unit, int nation);                 // one undo step; side change from a Neutral fix button
    struct NeutralAsk { uint32_t unit = 0, trig = 0; int kind = -1; bool open = false; } neutral_ask_;
    bool order_menu_open_ = false;                                     // open the chooser for the selected unit (after Pick a unit on the map)
    // Arrives > After a delay (app_spawner.cpp)
    uint32_t arrival_delay_timer(const std::string& who, float seconds);
    uint32_t unit_arrival_trigger(uint32_t unit) const;                // the trigger that brings a reinforcement in (0: it starts on the map)
    bool retarget_unit_orders(uint32_t unit, uint32_t to, uint32_t from = 0);  // its orders start on `to` instead
    void ui_arrival_delay(uint32_t trigger_uid);                       // the typed seconds box when the trigger is such a timer
    void ui_used_by(uint32_t uid);  // Properties: what refers to the selected object, clickable (app_triggers.cpp)
    void tab_messages();
    void settings_folders();  // Settings > General: scenario folder, game folder, author
    void ui_inspector();
    void ui_inspector_multi(const std::vector<int>& roots);
    void ui_minimap();
    void ui_validation();
    void message(const std::string& title, const std::string& text);
    bool editable() const { return doc_ && !doc_->read_only; }
    float form_col() const { return 150 * dpi_; }  // label column of every form (form_label, labels left)
    // Size factor for markers (spawns, path points, the placement preview) so they read at any zoom.
    float marker_scale() const { return std::max(1.f, cam_.dist / 180.f); }

    // --- data
    HWND hwnd_ = nullptr;
    float dpi_ = 1.f;
    Renderer r_;
    ModelCache models_;
    bool show_models_ = true;
    // View > Show objects for game type: -1 = all objects; else an index into game_types(). Display only,
    // the saved map is unchanged. view_type_hidden_ counts the objects left out of the last rebuild.
    int view_type_ = -1;
    int view_type_hidden_ = 0;
    void view_type_combo(float w);
    void transform_tools_ui();
    // View > Objects the mission shows later: off = leave out what the game does not show when the mission
    // starts (objects_at_start: hidden by a SETUP show/hide action, or a spawner's units). Display only.
    bool show_later_ = false;
    int later_hidden_ = 0;
    std::unordered_map<uint32_t, char> start_state_;  // uid -> START_* of objects not shown when play starts (inspector hint)
    bool show_sky_ = true;
    // Helper layer: objects the game never draws (trigger areas, AI paths, markers, lights,
    // cameras, other logic), each group on its own switch; see helper_group().
    enum HelperGroup { HG_TRIGGERS, HG_PATHS, HG_MARKERS, HG_LIGHTS, HG_CAMERAS, HG_OTHER, HG_COUNT };
    bool show_helpers_ = true;
    bool show_helper_group_[HG_COUNT] = {true, true, true, true, true, true};  // all on: the retail levels show everything they hold
    bool show_legend_ = false;
    bool show_level_tree_ = false;
    float fog_scale_ = 1;  // the game's [Display] DrawDistance: fog x 2^((DrawDistance - 50) / 50)
    bool show_weather_ = true;  // View > Weather: the map's snow/rain (MAEnvironment EnableRain) over the view
    struct PrecipStats { size_t sprites = 0; float rate = 0, life = 0, size = 0; } precip_stats_;  // The precipitation drawn this frame (automation `precip`)
    int lightning_hold_ = 0;    // developer (--lightning-hold): 1 holds the lightning model flash, 2 the sky flash, 3 both
    // View > Ground scatter: the culture map's grass / bushes / rocks / tree clusters near the camera
    // (app_culture.cpp). Generated per culture type, cached until the eye moves 4 m.
    bool show_culture_ = true;
    struct CultureType {
        std::string name;
        int shift = 0, bits = 1;      // the type's level field in the culture map
        uint32_t templ = 0;           // GenerationTemplate
        float density = 8000, grid[2] = {10, 10}, scale_range = 0, min_slope = 0, max_slope = 90, stop_fade = 300, start_fade = 200;
        bool random_rot = true, align = true, snap = false;
        std::vector<BoxInstance> inst;
    };
    struct Culture {
        const void* doc = nullptr;    // the document (and its object count) the data was read from
        size_t objs = 0;
        int w = 0, h = 0;
        std::vector<uint16_t> map;
        std::vector<CultureType> types;
        float at[2] = {1e30f, 1e30f}; // eye xz of the last generation
    } culture_;
    void load_culture();
    void add_culture();
    void store_culture();   // the edited culture map back into MATerrainInfo (end of a Plants stroke)
    void plants_ui();       // Terrain tab > Plants options
    std::string plant_label(const CultureType& c) const;  // plain name, as in the palette
    // Plants from other levels. kCrossLevelPlants = false hides the add button and leaves "Scatter it as objects" (
    // fallback, if the game turns out not to draw an added layer).
    static constexpr bool kCrossLevelPlants = true;
    bool plant_picker_open_ = false;
    std::string plant_msg_;                        // why the last add did not happen
    void plant_picker_ui();                        // the modal listing every plant of every level
    void plant_add(const ma2::ScenarioIO::PlantSource& src);
    void plant_scatter(uint32_t tid);              // add a plant's template to the scatter brush and open Scatter
    int plant_pick_ = 0;    // Plants tool: culture type (index into culture_.types)
    bool plant_erase_ = false;
    // View > Effects (app_effects.cpp): the game's looped particle effects as sprites.
    bool show_effects_ = true;
    bool fade_blockers_ = false;  // View > Walls that block the camera: objects ticked TransparentBlockingCamera vanish while between the camera and its target, as the game fades them
    bool blocks_camera(const ObjView& v) const;
    bool precip_3d_ = false;  // the snow / rain particle effect is being drawn in 3D, so View > Weather skips its flat overlay
    void add_effects();
    void add_precip_effect(std::vector<ma2::PSpriteOut>& out, size_t& budget, FILE* dump);  // View > Weather: the snow effect at the camera
    std::string dump_effects_;  // developer flag --dump-effects FILE: every .mtb particle effect of the placed objects and whether it is drawn
    void ui_weather();
    // Environment tab pieces (app_weather.cpp)
    void time_of_day_panel(ma2::Scenario& s, int env_index);
    void weather_controls(ma2::Scenario& s, int env_index);
    void building_lights_control(ma2::Scenario& s, int env_index);
    // Every Environment control says what it did (8 s), with the graphics-memory change when a look or sky moved it.
    void env_status(const char* fmt, ...);
    double env_gfx_before();                      // the exact graphics figure before a look / sky change
    std::string env_gfx_note(double before);      // calibrates to the exact figure; " Graphics memory: about 25.6 of 64 MB (+1.2)." when it moved by 0.1 MB or more
    const ma2::weather::Look* look_for_time(ma2::Scenario& s, int time);  // 0 day, 1 evening, 2 night: the base level's own look of that time, else the default
    std::string look_label_now(ma2::Scenario& s);  // the Look row's text: "Warm sun", "Warm sun, with the Starry space sky", "This map's own light"
    bool env_mismatch(ma2::Scenario& s, bool* sky_night);  // the sky's time disagrees with the light's (and "Keep it" was not pressed for this sky)
    std::string mismatch_kept_sky_;               // "Keep it": the sky the amber line is hidden for
    int helpers_faded_ = 0;                       // start slabs drawn faded this frame (the camera is within 20 m of them)
    void apply_look_ui(ma2::Scenario& s, const ma2::weather::Look* look, const char* what, const std::string& keep_sky = std::string());  // null: the base level's own light
    // Retail-reach #41 / row 6: a sky's own picture for the sky and look lists, and a list row with it (app_environment.cpp)
    ID3D11ShaderResourceView* sky_thumb(const std::string& sky_name);
    bool thumb_row(ID3D11ShaderResourceView* thumb, const std::string& label, const std::string& id, bool selected);
    void show_sky_view(bool only_if_top_down);  // tilts the camera to the horizon so sky/fog changes are visible (app_weather.cpp)
    std::string dump_boxes_;   // developer flag --dump-boxes FILE: game objects drawn as plain boxes, rewritten each frame
    std::string dump_lights_;  // developer flag --dump-lights FILE: the Scene lights list, rewritten each frame
    ma2::Dictionary dict_;
    int helper_group(uint32_t cls) const;    // -1 = drawn in the game (not a helper)
    bool has_side(const ma2::WObj& o) const;  // a unit that can belong to a side (not a road, effect, water or plain scenery)
    void ui_legend();
    void level_tree_window();  // View > Level tree (app_level_tree.cpp)
    // Level tree: Add part (any class the retail worlds use, a copy of a retail object) and Duplicate (app_add_part.cpp).
    uint32_t add_part_parent_ = 0;  // != 0: the Add part picker is open for this parent
    bool add_part_open_ = false;
    std::string add_part_filter_;
    void add_part_popup();
    void add_part_to(uint32_t parent_uid, uint32_t cls);
    void duplicate_selected();      // Ctrl+D: arms the paste ghost with copies of the selection (parts with no place on the map are copied at once)
    void duplicate_selected_now();  // the copy beside the original, no ghost (Level tree > Duplicate with its parts)
    void add_cutscene_shot(uint32_t cinema_uid);
    void select_all();
    void invert_selection();  // Ctrl+I: every drawn root that is not selected
    void road_view(const ma2::WObj& o, ObjView& v);
    bool sky_use_fog_ = false;            // Map tab: picking a sky also takes its fog colour
    Camera cam_;
    Settings settings_;
    ma2::GameData gd_;
    std::unique_ptr<ma2::TemplateCatalog> cat_;
    std::unique_ptr<ma2::ScenarioIO> io_;
    std::unique_ptr<ma2::Scenario> doc_;
    std::string game_error_;
    std::string install_note_;  // what the last 'Automatic' looked at and found
    std::string install_typed_; // Settings > Game folder: the path typed or pasted into the box
    bool use_game_folder(const std::wstring& folder);  // Choose... and the typed path: set it, load the game, say what happened
    bool quit_ = false;

    Tab tab_ = Tab::Map;
    Page page_ = Page::Place;                 // the Build tab's page (what the panel shows); tool_ is what a map click does
    Tool sculpt_last_ = Tool::Raise, paint_last_ = Tool::PaintTexture;
    float page_footer_h_[kPageCount] = {};     // the footer height measured last frame (the body takes what is left)
    float page_footer_y0_ = 0;
    struct PanelRects { float strip[4] = {}, left[4] = {}, right[4] = {}; float left_scroll_max = 0; } panels_;
    bool split_drag_ = false;
    struct PalState { bool init = false; int chip = 0; std::string category, filter; };
    PalState pal_state_[3];                   // each browser page keeps its own chip, narrow-to and search
    // One entry of the palette browser (a template, a road piece or a water type), the rows it packs into, and the drag state.
    struct PalEntry {
        enum Kind { Template, Road, Water } kind = Template;
        const ma2::TemplateInfo* t = nullptr;  // Template / Water
        int road = -1;                         // Road: index into road_pieces_
        mutable std::string name;              // the shown name (no official suffix; a size in it follows the measured model)
        int tier = 0;                          // 0 = name words matched (search ranking; refines it)
        bool theme = true;                     // comes with this map's theme (else: from another level)
        bool cut = false;                      // cut from the game (no model on the disc)
        bool special = false;                  // a special category (cutscene props, wrecks, decoration, skies)
        mutable float gfx_mb = -1;             // graphics memory it adds (MB, other levels only); -1 = not known yet
        mutable std::string size;              // "60 m" / "38 x 38 m" for modular pieces (roads at build, walls / bridges / bases once their model is loaded)
    };
    struct PalCache {
        std::string key;
        std::vector<PalEntry> entries;
        std::vector<std::string> headings;
        std::vector<std::pair<int, int>> heading_at;  // (entry index, heading id)
        std::vector<PackRow> rows;
        int cols = 0;
        std::string chip_label;
    };
    PalCache pal_cache_[3];
    void palette_entry_gfx(const PalEntry& e, bool force);  // fills e.gfx_mb for an other-level object
    float frame_ms_ = 0;                      // smoothed frame time
    float left_panel_ms_ = 0;                 // smoothed ms the left panel takes per frame
    bool pal_drag_live_ = false;              // a palette cell is being dragged (set by the cell's drag source each frame)
    int pal_drag_frame_ = -1;
    bool pal_drop_ = false;                   // the frame the mouse was released after such a drag
    const std::vector<const ma2::TemplateInfo*>& palette_sorted();
    void palette_arm(const PalEntry& e);
    bool palette_model_dims(const ma2::TemplateInfo& t, float& dx, float& dy, float& dz);  // the model box in metres once it is loaded
    void palette_entry_size(const PalEntry& e, bool force = false);  // fills e.size for modular pieces; loads their model on the thumbnail budget unless forced
    void palette_cell(const PalEntry& e, BrowserMode m, float cell, float cw);
    void palette_context_menu(const PalEntry& e, BrowserMode m);
    void palette_build(BrowserMode m, PalCache& pc, int chip, const std::string& filt);
    void scatter_toggle(uint32_t uid, bool arm = true);
    bool browser_mode_of_page(BrowserMode& m) const;

    Tool tool_ = Tool::Select;
    float vp_x = 0, vp_y = 0, vp_w = 1, vp_h = 1;
    bool vp_hover_ = false;

    std::vector<ObjView> views_;
    std::set<uint32_t> selection_;      // root UIDs
    uint32_t hover_uid_ = 0;
    bool objects_dirty_ = true, minimap_dirty_ = true, overlay_dirty_ = true;
    bool minimap_open_ = true;  // the overview under Properties is showing
    int overlay_plants_ = -1;  // culture type the overlay shows (Plants tool), -1 = the type map

    // Terrain tools
    bool plateau_auto_ = false;  // this stroke set the plateau height itself (it was not above / below the ground there)
    Tool stroke_tool_ = Tool::Select;  // the brush a stroke started with: a paired tool keeps that half until the button is let go
    float sculpt_slide_[3] = {0, 0, 0};  // Terrain tab pairs: each highlight's place between its top (0) and bottom (1) half
    Tool sculpt_now() const;  // the shaping brush a click would use now (the pair's other half while 'Reverse the brush' is held)
    float stroke_change_ = 0;  // largest height step of the current Raise / Lower stroke (status line)
    // Sculpting by the stroke (core/brush.h): the ground at the stroke's start, the biggest weight each point has felt, the time held.
    std::vector<float> stroke_h0_;
    ma2::StrokeCap stroke_cap_;
    float stroke_held_ = 0;
    float stroke_top_ = -1e9f, stroke_bot_ = 1e9f;   // highest / lowest drawn ground among the points the stroke touched
    ma2::Vec3 stroke_last_{};                         // where the brush was last applied in this stroke
    ma2::Vec3 last_stroke_end_[int(Tool::Count)] = {};  // per armed tool: where its last stroke ended (Shift+click draws a line from it)
    bool last_stroke_ok_[int(Tool::Count)] = {};
    int sculpt_rect_[4] = {1 << 30, 1 << 30, -1, -1};   // grid rectangle touched since the last flush_brush
    bool sculpt_pending_ = false, stroke_stepped_ = false;
    // Painting by the stroke: the colour map and the layer weights as they were when the stroke began, the biggest brush weight each
    // texel has felt, and the rectangle touched since the last upload.
    std::vector<uint32_t> paint_base_;
    std::shared_ptr<std::vector<uint8_t>> paint_base_w_;
    ma2::StrokeCap paint_cap_, paint_wcap_;
    bool stroke_rev_ = false;                           // the stroke began with the reverse key held (Texture, Boundaries, Plants)
    int paint_rect_[4] = {1 << 30, 1 << 30, -1, -1}, paint_wrect_[4] = {1 << 30, 1 << 30, -1, -1};
    bool paint_pending_ = false, paint_w_pending_ = false;
    bool paint_full_upload_ = false;                    // developer (script --full-upload 1): the old whole-texture upload, to compare speeds
    void flush_paint();
    // Strength per tool: brush_strength_ mirrors the current tool's value (slot: Texture 0, Colour tint 1, Smooth 2, Flatten 3, Plants 4).
    static int strength_slot(Tool t);
    float& strength_ref(Tool t) { return tool_ == t ? brush_strength_ : settings_.tool_strength[std::max(0, strength_slot(t))]; }
    float strength_of(Tool t) const { return tool_ == t ? brush_strength_ : settings_.tool_strength[std::max(0, strength_slot(t))]; }
    bool strength_dirty_ = false;
    float frame_ring_[30] = {};                         // the last 30 frame times (ms) and the time the brush spent per frame (state.perf)
    float brush_ring_[30] = {};
    int ring_n_ = 0;
    float brush_frame_ms_ = 0;
    bool perf_in_stroke_ = false;
    int perf_stroke_n_ = 0;
    float perf_stroke_frame_ = 0, perf_stroke_brush_ = 0;
    std::chrono::steady_clock::time_point brush_t0_;
    void flush_brush();                                // uploads the touched ground, carries what stands on it, marks the light map stale
    void brush_path(const ma2::Vec3& a, const ma2::Vec3& b, float dt);  // the brush from a to b in sub-steps
    void brush_limits(Tool t, float& lo, float& hi) const;  // the size range of a brush tool on this map
    std::string brush_floor_note(Tool t) const;        // "Smallest brush on this map: 12 m (the ground has a height point every 8 m)."
    float brush_size_clamped(Tool t, bool say);        // brush_radius_ into the tool's range (says once when it had to grow)
    bool brush_size_slider(Tool t);                    // the shared "Brush size" slider
    Tool brush_seen_tool_ = Tool::Select;
    std::string brush_readout_;                        // the cursor label (two lines)
    void draw_height_points(const ma2::Vec3& g, float radius = -1.f);       // dots at the height points near a sculpt brush (radius < 0: the brush size)
    bool stroke_clamped_ = false;  // the stroke ran into the map's lowest / highest height (Advanced > Height range)
    uint32_t roughen_seed_ = 0;  // Roughen: one bump pattern per stroke
    bool brush_square_ = false;  // Shape tools: square brush (straight edges, e.g. round square pools)
    float brush_radius_ = 40.f, brush_strength_ = 0.5f, brush_hardness_ = 0.f, flatten_height_ = 10.f, plateau_height_ = 30.f;
    float paint_color_[3] = {0.35f, 0.42f, 0.30f};
    float level_tints_[3][3] = {};  // dark / average / light tint taken from a real level (Terrain tab)
    int level_tints_n_ = 0;
    std::string level_tints_name_;
    bool tint_pick_ = false;  // Colour tint eyedropper: the next click on the map picks the colour there
    int paint_type_value_ = 0;            // value the terrain-type brush writes (0 Playable by default)
    int paint_texture_ = 1;               // ground texture the texture brush paints: 0 = A (TerrainDetailMap, alpha 255), 1 = B (alpha 0)
    bool paint_neutral_colour_ = true;    // texture brush also moves the colour map to neutral so the texture shows in its own colours
    // Every terrain detail texture on the disc: archive path, archive holding it.
    std::vector<std::pair<std::string, std::string>> detail_textures_;
    // Paint section (app_paint.cpp): Texture / Colour tint / Boundaries; the palette backs onto the retail
    // colour-map alpha (<= 2 textures) or the P4 layers (3+, PC only).
    void paint_section_ui();
    void terrain_whole_map_ui();
    void copy_level_tint(const std::string& level);  // Whole map: a real level's colour tint
    void terrain_read_only_ui();  // a real level: how its ground is made (textures, tint, plants, heights)
    void terrain_advanced_paint_ui();
    void ground_textures_ui();
    void tint_ui();
    void boundaries_ui();
    void ground_picker_ui();
    void ensure_detail_textures();
    bool ground_texture_average(int slot, float rgb[3]);   // The overview map's ground colour (0..255 per channel); false when the texture cannot be read
    void enable_ground_layers();
    void disable_ground_layers();
    bool add_ground_texture(const std::string& path, const std::string& archive);
    bool replace_ground_texture(int k, const std::string& path, const std::string& archive);
    void remove_ground_layer(int k);
    void fill_ground(int layer);
    int pick_target_ = -1;       // texture picker: -1 = add, else the palette entry being changed
    bool open_pick_ = false;
    std::string pick_filter_;
    bool water_look_ui(ma2::WObj& o);  // true when it replaced the object
    ID3D11ShaderResourceView* palette_picture(const ma2::TemplateInfo& t);
    uint32_t water_type_combo(const char* id, uint32_t cur_uid, float tile_px = 64.f);
    // The one list of water types (palette Water group, Terrain > Water, water Properties): one per model,
    // the ones that can be any size first, then the fixed-size ones, A-Z by water_type_name within each.
    const std::vector<const ma2::TemplateInfo*>& water_types();
    std::vector<const ma2::TemplateInfo*> water_types_;
    void arm_water(uint32_t template_uid);  // pick a water type: resizable -> Place water, fixed -> place it like an object
    // Steep ground: the overlay tints cells over kSteepBot yellow and over kSteepLight orange; the Ramp tool turns it on while armed.
    bool show_steep_ = false;    // what is drawn now (settings_.show_steep, or on automatically while Ramp is armed)
    bool steep_auto_ = false;    // Ramp turned it on; leaving the tool turns it off again
    float steep_acc_ = 0;        // time since the overlay was rebuilt during a stroke (at most 4 times a second)
    // The Ramp tool (app_ramp.cpp): click 1 = start A, click 2 = end B (applied on click 2). Esc / right-click after click 1 drops A.
    int ramp_stage_ = 0;
    ma2::Vec3 ramp_a_{};
    bool ramp_typed_ = false;                       // "Typed heights": from / to boxes instead of the drawn ground at A and B
    float ramp_h_from_ = 10.f, ramp_h_to_ = 30.f;
    float ramp_ha_ = 0, ramp_hb_ = 0, ramp_slope_ = 0, ramp_len_ = 0;  // what the preview shows now (automation state.ramp)
    std::string ramp_label_;
    GroundEdit plan_ramp(const ma2::Vec3& a, const ma2::Vec3& b, float ha, float hb);
    void ramp_heights(const ma2::Vec3& a, const ma2::Vec3& b, float& ha, float& hb) const;
    void ramp_click(const ma2::Vec3& g);
    void draw_ramp_preview(const ma2::Vec3& cursor);
    void ramp_cancel(bool say);
    void ramp_width_step(int dir);
    static std::string ramp_sentence(float len, float rise, float slope);  // "64 m long, rises 20.0 m: 31%. 'Mechs can climb it."
    bool show_typemap_ = false;
    bool typemap_auto_ = false;  // the Boundaries tool turned the overlay on; leaving the tool turns it off again
    bool stroke_active_ = false;
    int dirty_x0_ = 1 << 30, dirty_z0_ = 1 << 30, dirty_x1_ = -1, dirty_z1_ = -1;
    bool lightmap_stale_ = false;
    bool stale_sun_only_ = false;  // the light map is stale only because the sun moved (from lightmap_sun_)
    ma2::Vec3 lightmap_sun_{};

    // Object tools
    std::string unit_filter_, unit_category_;
    int unit_group_ = 0;         // palette task group (palette_groups in app_tabs.cpp); 0 = 'Mechs
    int palette_thumb_budget_ = 3;
    int palette_gfx_budget_ = 4;              // other-level objects measured for their cells this frame
    std::unordered_map<uint32_t, float> gfx_tpl_mb_;  // computed MB per template (this Base)
    std::string gfx_cache_base_;
    std::set<uint32_t> gfx_on_map_;           // distinct templates on the map that add graphics memory
    std::vector<uint32_t> gfx_tids_;          // distinct template ids on the map (the recount walks this)
    size_t gfx_pos_ = 0;                      // recount progress through gfx_tids_
    double gfx_sum_ = 0;                      // partial sum of the recount
    bool gfx_scan_ = true;                    // the world changed: collect the template ids again
    bool gfx_counting_ = false;               // a recount is under way
    bool gfx_calibrated_ = false;
    double gfx_raw_ = 0;                      // 8 + Base + distinct other-level templates (MB)
    double gfx_offset_ = 0;                   // exact figure at the last Check / save minus the raw one then
    uint32_t place_template_ = 0;
    Tool last_place_tool_ = Tool::Select;  // Space (A_PLACE_AGAIN) re-arms it after Esc
    void place_again();
    void pick_selected_type();
    // Palette extras (palette_search.cpp): road pieces (prefab .road files; they have no template),
    // overlapping groups and hidden search aliases.
    struct RoadPiece {
        std::string path;      // as the archive spells it: "roads/country/90turn.road"
        std::string official;  // "90turn.road"
        std::string friendly;  // "90-degree turn"
        std::string model;     // its model, from the .road file
        std::string search;    // lower-case search text
        float w = 13, l = 30;  // half width, length
        std::vector<int> archives;  // level_archives() indices (-1 = common.mgf) holding it
        struct Exit { float x, z, vx, vz; };  // [attachments] 1..N-1: where the next piece joins (local x,z) and the outward direction
        std::vector<Exit> exits;
    };
    // Snapping (app_snap.cpp): one solver for roads, walls, bridges and plates. The joint chosen this frame, for the ghost, the click,
    // the hint and the state read. place_heading_ stays the user's heading; ghost_heading_ is the joined piece's.
    struct SnapState {
        bool on = false;                       // a joint is active (not the grid)
        ma2::snap::Cand c;                     // the winner
        ma2::snap::Prev prev;                  // last frame's choice, and the Q / E pin
        std::vector<ma2::snap::Cand> cands;    // every candidate of the last solve (next_yaw)
        int win = -1;
        std::string label;                     // "Joins Barricade security 3"
        float fcx = 0, fcz = 0;                // the free ghost's footprint centre
        float jx = 0, jz = 0;                  // the winner's own joint on the free ghost
        float d_px = 0, d_m = 0, reach_px = 0, cap = 0;
        std::string grid_label;                // "Grid 5 m" while the grid rounded the ghost
    } snap_;
    float ghost_heading_ = 0;
    float ghost_yaw() const { return snap_.on ? ghost_heading_ : place_heading_; }
    ma2::snap::Mover mover_for_place();
    void snap_sockets(float x, float z, float radius, int fam, const std::set<uint32_t>& skip, std::vector<ma2::snap::Socket>& out, std::vector<char>& open);
    void snap_targets(const ma2::snap::Mover& m, float x, float z, const std::set<uint32_t>& skip, std::vector<ma2::snap::Socket>& open);
    void snap_anchors(const ma2::snap::Mover& m, float x, float z, std::vector<ma2::snap::Anchor>& out);  // plates and buildings near (x, z) that pieces sit on or beside
    bool solve_snap(const ma2::snap::Mover& m, ma2::Vec3& centre, float& yaw);  // centre / yaw in: the free ghost; out: the joined one
    void snap_clear();
    bool snap_turn(int dir);                  // Q / E while joined: the next way to join at that joint (false: nothing, turn plainly)
    void draw_snap_feedback(ImDrawList* dl);
    // Grid, angle steps and the snap bar (app_snap.cpp). Shift (place freely) turns all of them off while it is held.
    void snap_controls_ui(bool compact);      // the three toggles and their size / step menus (snap bar, Edit menu, placing panel)
    void snap_bar();                          // the small window in the viewport's top-left corner
    void snap_prefs_save() { settings_.save(); }
    void grid_toggle();
    void grid_step(int dir);                  // [ / ]: the next smaller / bigger grid size
    float angle_step_rad() const { return settings_.snap.angle ? settings_.snap.angle_deg * 0.01745329252f : 0.f; }
    float angle_snapped(float yaw) const;     // yaw rounded to the angle step (unchanged when the steps are off or the place-freely key is down)
    float turn_step_rad() const;              // what one Q / E press turns a placing ghost: 15 degrees, or the angle step when that is bigger
    void turn_place_heading(int dir);         // Q / E on a free ghost
    bool grid_active() const;                 // the grid is on and the place-freely key is not down
    ma2::Json snap_state_json() const;
    const ma2::snap::ToPx* snap_px_override_ = nullptr;  // developer paths with no camera
    // Moving snaps the way placing does. begin_move_snap at the press of a body drag or the plane square, move_snap_step each frame
    // (the free target = the ground under the cursor + the grab offset, solved like a placed ghost, applied as a delta), end_move_snap at release.
    struct MoveSnap {
        bool on = false;
        uint32_t uid = 0;                 // the anchor: the object pressed (the first selected root for the plane square)
        ma2::snap::Mover mover;           // its shape (fam 0: grid only)
        float yaw0 = 0;                   // its heading at the press (the user's heading H)
        float gx = 0, gz = 0;             // grab offset: the anchor's footprint centre minus the ground under the press
        bool single = false;              // one unglued object: the full table; else slides only
        std::set<uint32_t> skip;          // the selection and what is glued to it (never targets)
        struct Member { ma2::snap::Mover m; float yaw = 0, ox = 0, oz = 0; std::vector<char> interior; };  // One piece of a group (its centre offset from the anchor's), interior ends flagged
        std::vector<Member> members;
    } move_snap_;
    std::set<uint32_t> snap_skip_;        // solve_snap: pieces that are never targets (the mover itself)
    bool snap_translate_only_ = false;    // solve_snap: drop candidates that would turn the piece
    bool begin_move_snap(uint32_t anchor_uid, float press_x, float press_z);
    void build_group_members(const ma2::WObj& anchor, const std::set<uint32_t>& members, float acx, float acz);  // A moving group joins by any of its open ends
    bool move_snap_step(float ground_x, float ground_z);   // true: the selection was moved
    void end_move_snap();
    bool move_snap_centre(const ma2::WObj& o, float& x, float& z);   // the anchor's footprint centre now
    bool snap_move_axis(const ma2::Vec3& axis, const ma2::Vec3& pivot, float total, float& out_total);   // arrow drags: the grid along the axis
    ma2::snap::Mover mover_from_road(const RoadPiece& rp, const ma2::WObj* cropped);
    struct SnapFoot;
    ma2::snap::Mover mover_from_template(const ma2::TemplateInfo& t, GpuModel* gm, const SnapFoot* foot = nullptr);
    ma2::snap::Mover mover_for_wobj(const ma2::WObj& o);   // a placed (or copied) object as the solver sees it
    void turn_root_about(uint32_t root_uid, float delta, float pvx, float pvz);   // turn one root and its parts about a point, no undo step
    float paste_turn_ = 0;                // the ghost's effective turn (the joined heading when a joint holds it)
    bool paste_solve(ma2::Vec3& at);      // the single-object paste ghost joins like a placed piece; several: the grid on the group centre
    bool road_snapped_ = false;
    int snap_family(const ma2::TemplateInfo& t) const;  // 1 = wall/fence/gate piece, 2 = bridge piece, 0 = places freely
    // Join points of the placed pieces of one snap family (piece_snap's rule): a piece's footprint in its own axes, and its two ends.
    struct SnapFoot { float cx, cz, half; bool along_x; };
    struct SnapEnd { float x, z, vx, vz, axis, half, y; int piece; int side = -1; float ty = 0; float len = 0; };  // ty: height of the join point itself (a foundation's edge top)  // side (foundations): 0 +x, 1 -x, 2 +z, 3 -z edge of the piece  // y: the piece's origin height; axis: heading of its own +long axis
    bool snap_foot(const ma2::TemplateInfo& t, SnapFoot& f);
    bool object_foot(const ma2::WObj& o, SnapFoot& f);     // snap_foot of its template with the crop applied (walls, bridge pieces: where the cut ends are)
    void snap_ends(int fam, std::vector<SnapEnd>& ends, std::vector<int>& piece_obj);  // piece_obj[piece] = world object index
    static bool ends_meet(const SnapEnd& a, const SnapEnd& b);
    std::vector<int> snap_runs(int fam);  // per world object: id of the joined run it is in (-1 = not of this family)
    bool piece_snapped_ = false;
    // Glue (app_glue.cpp, playtest 11-04): a lock on a snapped join point ties two pieces of one snap family together.
public:
    static constexpr const char* kGlueProp = "EditorGlue";  // editor-only, "mine:uid:theirs;..." on both pieces
    struct Joint { ma2::Vec3 p; float ox = 0, oz = 0; };  // world join point and its outward direction on the ground
    struct GlueLink { int mine = -1; uint32_t uid = 0; int theirs = -1; };
    struct GlueIcon { ma2::Vec3 p, pa, pb; uint32_t a = 0, b = 0; int ja = -1, jb = -1; bool locked = false, line = false; };  // line: a whole-object glue (pa-pb)
    std::vector<GlueIcon> size_icons_;   // Glue_icons of the frame size_icons_frame_ (the size handles stand clear of the locks)
private:
    int glue_family(const ma2::WObj& o);  // 1 wall / gate, 2 bridge, 3 foundation, 4 road piece, 0 none
    bool object_joints(const ma2::WObj& o, std::vector<Joint>& js);
    std::vector<GlueLink> glue_links(const ma2::WObj& o);  // links both pieces record
    void glue_set(uint32_t a, int ja, uint32_t b, int jb, bool on);
    std::vector<uint32_t> glue_group(const std::set<uint32_t>& seeds);  // seeds and everything glued to them
    std::set<uint32_t> glue_reach(uint32_t from, const std::set<uint32_t>& block);
    std::set<uint32_t> glue_body(uint32_t uid);  // rigid body: whole-object glue (joint -1) only
    void glue_rigid(uint32_t uid, const void* rot, const ma2::Vec3& w);
    bool glue_find_hinge(uint32_t me, uint32_t& owner, GlueLink& hinge);
    ma2::Vec3 glue_anchor_point(uint32_t uid);
    bool glue_pick(bool lclick);  // the Glue tool's clicks
    void glue_tool_ui();
    // Road pieces have no collision ('Mechs walk on the ground): a tilted or floating one says so, and Fit ground to road
    // shapes the terrain under it (and the road pieces glued to it) to its surface (app_glue.cpp, playtest 11-04).
    bool road_surf_of(const ma2::WObj& o, void* surf);  // app_glue.cpp RoadSurf
    float road_float_gap(const ma2::WObj& o);  // how far its surface is above the ground at most (m); tilted counts
    void road_ground_ui(ma2::WObj& o);
    void fit_ground_to_roads();
    bool fit_ground_under_placed(uint32_t road, SlopePlan& sp);  // Tilt, at the click: the ground meets the road just placed (the caller's undo step)
    void road_tilt_notice();
    float road_snap_y_ = std::numeric_limits<float>::quiet_NaN();  // NaN: the road piece being placed joined a flat piece (rests on the ground)
    bool glue_mode_ = false;      // Properties > Glue objects together: click one object, then those to glue to it
    uint32_t glue_first_ = 0;
    void glue_shift(uint32_t uid, const ma2::Vec3& d, const ma2::Vec3& w, float turn);
    bool glue_hinge(ma2::Vec3& w);  // the one selected glued piece's first glued joint (Rotate / Tilt pivot)
    bool glue_turn(int kind, float delta, const ma2::Vec3& pv);  // 0 turn, 1 pitch, 2 roll with what is glued; false = nothing glued
    std::vector<GlueIcon> glue_icons();
    bool glue_click(bool lclick);
    void glue_draw(ImDrawList* dl);
    float snap_origin_y_ = 0;
    bool bridge_foot_height(const ma2::Vec3& click, float& origin_y);  // place_template_ is a bridge: origin height with its pillar bases on `click`  // height of the piece a joined wall / bridge piece takes
    struct RoadOpenEnd { float x, z, vx, vz; bool joined; };  // drawn while placing a road: where a click near it will join
    std::vector<RoadOpenEnd> road_open_ends_;
    bool place_on_top_ = false;  // the placing ghost stands on an object
    int place_on_top_of_ = -1;   // ...this one (world.objs index), named in the hint
    std::string place_road_;   // road piece being placed (empty = place_template_ applies)
    bool road_ready_ = false;
    std::vector<RoadPiece> road_pieces_;
    const std::vector<RoadPiece>& road_pieces();
    const RoadPiece* find_road_piece(const std::string& path);
    bool palette_group_has(const ma2::TemplateInfo& t, const std::string& group) const;
    bool palette_in_group(const ma2::TemplateInfo& t, bool special, int g) const;  // palette button g lists t (app_tabs.cpp)
    // The tier of the typed words against a template (palette_rank.h; -1 = not listed). `shown` = its plain palette name.
    int palette_rank_of(const std::string& filt, const ma2::TemplateInfo& t, const std::string& shown) const;
    mutable std::unordered_map<uint32_t, RankWords> rank_words_;
    float place_heading_ = 0;
    int place_side_ = 5;                  // missions: side of placed units (5 enemy, 6 player's side, 0 neutral)
    int place_team_ = 1;                  // multiplayer: team of placed turrets / people (Nation 1 Red, 2 Blue, 0 neutral)
    std::vector<uint32_t> scatter_set_;
    // above: origin height over its surface when copied; lo / hi / heading: the root's box as the paste ghost draws it
    struct Clip { std::vector<ma2::WObj> objs; ma2::Vec3 pos; float above = 0; bool sheet = false; float lo[3] = {-1.5f, 0, -1.5f}, hi[3] = {1.5f, 2, 1.5f}; float heading = 0; };
    std::vector<Clip> clipboard_;
    bool clipboard_cut_ = false;  // the clipboard came from Cut: a paste is a move, so the names stay
    bool dragging_ = false, drag_moved_ = false, box_selecting_ = false;
    float drag_start_x_ = 0, drag_start_y_ = 0;
    ma2::Vec3 drag_last_{};
    // The press in the Select tool is decided on release (a click) or past 6 px (a box, or a move when it began on a selected object).
    bool sel_press_ = false, sel_press_on_selected_ = false, sel_press_shift_ = false, sel_press_ctrl_ = false;
    uint32_t sel_press_uid_ = 0;  // the root under the press, 0 = empty ground
    // A left press with the turn-the-view key (Alt) held waits: past 6 px it turns the view, released before that it is an ordinary click.
    bool orb_press_ = false, orbiting_ = false, orb_click_ = false, orb_through_ = false;
    float orb_x_ = 0, orb_y_ = 0, wheel_acc_ = 0;
    ma2::Vec3 orbit_pivot_{};      // The point a middle / Alt drag turns the view about (the selection, else the ground under the cursor)
    bool orbit_pivot_on_ = false;
    int orbit_last_frame_ = -10;
    uint32_t last_click_uid_ = 0;  // the object the last click selected (a double-click on it selects its type)
    struct LabelHit { float x0, y0, x1, y1; int root; uint32_t uid; std::string text; };  // a helper label drawn on the map (pads, power-ups): it takes the click
    std::vector<LabelHit> label_hits_;
    bool open_objmenu_ = false, objmenu_open_ = false;  // the right-click menu of an object
    uint32_t objmenu_uid_ = 0;                           // the object that was right-clicked
    bool focus_props_ = false;                           // the menu's Properties: the inspector scrolls to its top
    void ui_object_menu();
    bool selection_has_sheet();
    void select_same_type(uint32_t uid, bool whole_map);
    std::vector<Clip> clips_of_selection();
    void arm_paste(std::vector<Clip> src, bool dup);
    void paste_clips(const std::vector<Clip>& clips, const ma2::Vec3& at, float heading, bool dup, bool name_copy);
    void paste_ghost_draw();
    std::vector<Clip> paste_src_;      // what hangs on the mouse
    float paste_heading_ = 0;          // the ghost's turn (Q / E), about its centre
    bool paste_dup_ = false;           // armed by Duplicate (else Paste)
    bool paste_on_ = false;            // the mouse is over the map: the ghost is drawn at paste_at_
    bool last_place_was_paste_ = false;  // Space (Place again) re-arms the paste ghost
    ma2::Vec3 paste_at_{};
    uint32_t list_anchor_ = 0;         // "Everything in this map": the row a Shift+click ranges from

    // Transform gizmo (gizmo.cpp): Move / Rotate / Tilt handles on the selection's centroid.
    int gizmo_mode_ = 0;                  // 0 move, 1 rotate, 2 tilt
    int gizmo_hot_ = 0, gizmo_drag_ = 0;  // handle under the mouse / being dragged (0 none)
    struct GizmoDrag {
        ma2::Vec3 pivot{}, hit0{}, applied_v{};  // pivot, grab point when the drag started, plane move already applied
        float applied = 0, start = 0;     // amount already applied / the start angle or axis parameter
        float accum = 0;                  // raw angle turned so far (rotate / tilt)
        float h0 = 0;                     // the first selected object's heading when a turn ring was pressed (absolute angle steps)
        float t = 0;                      // ring parameter at the mouse (edge-on rings)
        bool plane = false;               // last frame used the ring-plane method
        bool pushed = false;
        bool knob = false;                // the lift knob was grabbed: height follows the vertical mouse motion
        float knob_y0 = 0;                // mouse y when the knob was grabbed
        float knob_mpp = 0;               // metres per pixel at the pivot
    } gz_;
    bool gizmo_pivot(ma2::Vec3& out) const;
    // World offset from an object's origin to the middle of its model's base (its footprint centre); zero without a model.
    ma2::Vec3 footprint_offset(int obj_index);
    void zoom_to_selection();  // F, View menu, gizmo toolbar
    int gizmo_effective_mode() const;
    bool gizmo_can_tilt() const;
    // > 0 when the selection is one spawn / player start marker: half the side of its footprint, where the gizmo's
    // yellow move square goes (centred on the marker) instead of beside the arrows.
    float gizmo_marker_half() const;
    bool gizmo_update(bool lclick, bool ldown);   // true when the gizmo used the mouse
    void gizmo_draw();
    void rotate_group(float delta, const ma2::Vec3& pivot);
    void tilt_selection(int which, float delta_rad);
    // Water (Terrain tab > Water): any size (a model of that size ships in the scenario; see
    // water_custom.h). The Place water tool previews the rectangle under the mouse.
    uint32_t water_new_tpl_ = 0;               // type the tool places (0 = the first resizable one)
    float water_new_w_ = 60.f, water_new_l_ = 40.f;   // a pond that fits one screen at working zoom; a drag sets the size of one placing
    // Place water by dragging. Press = one corner of the rectangle, the cursor the opposite one, in the Q / E turn's frame.
    bool water_drag_ = false, water_drag_moved_ = false;
    ma2::Vec3 water_drag_press_, water_drag_centre_;
    float water_drag_w_ = 0, water_drag_l_ = 0, water_drag_mx_ = 0, water_drag_my_ = 0;
    static constexpr float kWadeDepth = 1.2f, kWadeMaxDepth = 1.6f, kDeepLakeDepth = 4.f;  // dug basin depths (m)
    static constexpr float kDigMaxCut = 3.f;  // a basin never cuts ground more than this above the surface (hills stay)
    bool water_new_deep_ = false;  // dug basin up to kDeepLakeDepth (pilots end up under); default kWadeDepth so a pilot on foot can wade
    bool water_new_dig_ = true;                // placing water digs a basin under it (flat ground: 1 m deep water is near invisible)
    bool water_new_oval_ = false;
    struct NewWater { uint32_t uid = 0; ma2::Vec3 at{}; float w = 0, l = 0, heading = 0; } water_new_last_;  // the water the tool placed last
    uint32_t water_fixed_pending_ = 0;         // fixed-size water just placed from the palette: dig its basin once its size is known
    int water_fixed_tries_ = 0;
    void finish_fixed_water();
    // Water ground follows the water: each water's footprint last frame; when one moves, resizes or is deleted, the
    // water ground (type map 8) it left uncovered goes back to plain ground and its new spot is marked (water_ux.cpp).
    struct WaterFoot { ma2::Vec3 c; float w = 0, l = 0, heading = 0; bool oval = false; };
    std::map<uint32_t, WaterFoot> water_feet_;
    const ma2::Scenario* water_feet_doc_ = nullptr;
    void track_water_ground();
    bool water_over_new_ = false;              // the mouse is over that water: the tool shows no ghost and a click places nothing
    bool over_new_water(const ma2::Vec3& g) const;
    int mark_water_ground(const ma2::Vec3& c, float w, float l, bool oval, float heading);  // type map under water = water ground              // the tool places an oval (cropped) water instead of a rectangle
    uint32_t water_rename_uid_ = 0;            // the water whose name is being edited in the list
    std::string water_rename_buf_;
    std::map<uint32_t, bool> water_resizable_;  // template uid -> its model can be resampled
    bool water_can_resize(uint32_t template_uid);
    void water_size_of(const ma2::WObj& o, const ma2::Vec3* pos, float& w, float& l);  // shown size (custom, else the view's)
    void set_water_size(ma2::WObj& o, float w, float l, int shape = -1);  // shape: -1 keeps the water's own, else a wcustom::Shape
    void water_section_ui();
    // Water basins (water_ux.cpp): the bowl is a GroundEdit (protected like Fit ground), remembered on the water as
    // EditorBasin so a resize, move, Level change or delete fills it and digs it again.
    static constexpr float kShoreDrop = 0.15f;  // the surface of a dug basin lies this far below the ground that was clicked
    struct BasinFoot { float cx = 0, cz = 0, w = 0, l = 0, heading = 0, surface = 0; bool oval = false; };
    struct BasinPlan { GroundEdit e; BasinFoot foot; int bx0 = 0, bz0 = 0, bx1 = -1, bz1 = -1; std::vector<float> pre; };  // bx..: the dug rectangle; pre: the ground there before the dig
    bool water_foot(const ma2::WObj& o, BasinFoot& f);                 // the water's place, size and surface now
    BasinPlan plan_basin(const BasinFoot& f, const GroundEdit* fill);  // the bowl (on top of `fill`, the old basin given back); writes nothing
    GroundEdit plan_basin_fill(const ma2::WObj& water);                // the remembered basin given back: points still at the dug step return to their old one
    int make_basin(ma2::WObj& water, bool refit, bool push, const char* undo_label);  // plan + protect + apply + remember; returns objects moved
    void refit_basin(ma2::WObj& water, bool deleted);                   // fill the remembered basin; unless deleted, dig again for the water as it is now
    void refit_moved_basins();                                          // every frame: a water whose place or size differs from its basin's gets it refitted (not mid-drag)
    std::string basin_note(const GroundEdit& e);                        // "N objects ride down with it. The ground under Office 07 ... stays."
    std::string basin_kept_ = {};                                       // names the last basin left alone (for the status line)
    int basin_moved_ = 0;
    GroundEdit basin_preview_edit(const BasinFoot& f, const ma2::WObj* water = nullptr);                  // the plan + protection + forecast the ghost and the button hover draw
    // Check scenario, what the user would see about heights (app_checks.cpp): floating, same-height, buried, deep, hanging.
    void placement_checks(std::vector<ma2::ValidationItem>& out);
    void apply_placement_fix(int fix, const std::vector<uint32_t>& uids);
    float check_rect_[4] = {0, 0, 0, 0};                                // the Check window's rectangle (screen px) as last drawn: "Show" aims clear of it
    std::map<uint32_t, size_t> check_show_pos_;                         // "Show next" steps through an item's objects (key: the item's first uid)
    void check_show(const ma2::ValidationItem& v);
    bool solid_under_point(float x, float z, float foot, int skip_root);  // a solid upward face of another model within 0.5 m under the foot
    void place_water(const ma2::Vec3& at, float w = 0, float l = 0);   // w, l > 0: this size instead of the boxes' (dragged out)
    uint32_t water_place_tpl();                // the type the tool places: the picked one, else the first resizable (0 = none)
    // Ponds (kept for developer probes; the Terrain tab no longer offers it): a basin plus the smallest water tile.
    float pond_radius_ = 25.f, pond_depth_ = 8.f;  // shallow water is faint in the game, so a new pond starts deep
    void make_pond(const ma2::Vec3& at);
    void select_uid(uint32_t uid);
    int pick_water(float mx, float my, float t_other, bool marker_only);
    void ui_placed_list();
    size_t placed_count() const { return placed_n_; }  // status bar 'N placed', counted in rebuild_objects
    size_t placed_n_ = 0;

    // Players tools
    int spawn_team_ = 1;
    int pad_kind_ = 0;
    int kit_source_[16] = {};

    // Triggers tab
    uint32_t trig_uid_ = 0;             // the trigger the Triggers tab shows (a MATrigger* object of the world)
    bool trig_scroll_to_ = false;  // Triggers tab: a list click scrolls the picked trigger's editor into view
    // Map gizmo for circles, boxes and trigger volumes (app_trigger_gizmo.cpp).
    bool trig_gizmo_update(bool lclick, bool ldown);   // true when it used the mouse
    void trig_gizmo_draw();
    struct TrigDrag { int handle = 0; float start = 1e30f, applied = 0; ma2::Vec3 hit0{}, applied_v{}; float v0[8]{}; bool pushed = false; const char* label = ""; };
    TrigDrag tdrag_;
    bool trig_target(TrigGeo& g);
    void trig_apply(const TrigGeo& g);
    int trig_hot_ = 0;
    // What the next click(s) on the map (Tool::PickPoint / Tool::PlaceWaypoint) set.
    struct PickTarget {
        enum Kind { None, MoveShape, LinkMarker, OrderPath, CamPath, ObjTargets, LinkUnit, CutUnit, OrderUnit } kind = None;
        int objective = -1;             // ObjTargets: the objective whose 'destroy' trigger (uid) gets each clicked unit
        uint32_t uid = 0;               // MoveShape: the trigger box / circle; LinkMarker: the object whose link names the marker; OrderPath: the MAEvent; CamPath: the MAPath
        std::string key, name;          // LinkMarker: the link property, the marker's name; LinkUnit: the link property of the action (uid)
        std::string set_key;            // LinkUnit: the action's set property that clears when a unit is picked ("" = none)
        bool null_marker = false;
        bool chain = false;             // LinkMarker: a Defend order from the chooser: the unit walks to the spot first (make_defend_walk)
        uint32_t follow = 0;            // MoveShape: an object that moves with the shape (a 'Reach a place' radar marker)
        bool restart = false;           // CamPath: the next click starts the path over
        float t = 0;                    // CutUnit: the moment (s into the cutscene) the order starts; uid = the cutscene
        bool once = false;              // CutUnit: the order walks its path once (MAPatrol, TraverseType 1) instead of looping it
                                        // (CutUnit: key = the behaviour class MAPatrol / MAAttack ...)
        bool redraw = false;            // CamPath: started by Draw flight path (that button stays lit, not Add points)
        Tab back = Tab::Triggers;       // the tab to return to after the click
    };
    PickTarget pick_;
    // "Defend a point" from the chooser: the unit walks to the clicked spot (a walk-once path), and a circle trigger there (it fires when this unit is
    // inside) starts the MADefend that guards it. Returns the marker (0 on failure). A unit already at the spot just gets the plain Defend.
    uint32_t make_defend_walk(uint32_t beh, float x, float z, const std::string& name);
    // A map-pick mode for this target is waiting for clicks (its button shows the toggle "on" look).
    bool picking(PickTarget::Kind k, uint32_t uid) const { return (tool_ == Tool::PickPoint || tool_ == Tool::PlaceWaypoint) && pick_.kind == k && pick_.uid == uid; }
    void ui_helper_buttons();       // Units tab: add lights, cameras, checkpoints, markers, menu triggers (app_add_part.cpp)
    void place_helper(const char* cls, const char* label, const char* group);
    // A trigger box / circle (the retail MATriggerCuboidalVolume / MATriggerRadius, as Objectives & Triggers > New trigger
    // makes it) put down with the next click; its actions are set on Objectives & Triggers (placeable from Units and Props too).
    void new_trigger_area(bool box);
    void trigger_area_buttons(const char* id);
    void draw_trigger_overlay();
    bool single_trigger_shape() const;              // the selection is one trigger box or circle (its own gizmo)
    bool positionless_trigger(const ma2::WObj& o) const;  // a trigger with no place on the map (units destroyed, timer, logic...): no gizmo
    bool selection_positionless() const;            // everything selected is such a trigger
    int add_destroy_target(ma2::Scenario& s, uint32_t trig, uint32_t unit, int objective);  // Objectives: watch + radar-mark one unit; 1 = it got an enemy crew
    void pick_objective_target(float mx, float my);  // ObjTargets pick mode: the unit under the cursor joins the objective

    // Undo
    std::deque<Snapshot> undo_, redo_;
    int cur_state_id_ = 0, saved_state_id_ = 0, next_state_id_ = 1;  // the clean mark: dirty = cur != saved
    std::string merge_tag_;
    uint64_t merge_sel_ = 0;
    double merge_time_ = -1e9;
    int merge_state_ = -1;                        // cur_state_id_ right after the last merging push
    double undo_note_time_ = -1e9;
    std::string undo_note_;                       // "Undo history is full..." waiting to be shown
    mutable std::shared_ptr<const std::vector<float>> snap_hgt_;        // the buffers of the last snapshot taken or restored
    mutable std::shared_ptr<const ma2::Image> snap_cm_;
    mutable std::shared_ptr<const ma2::Image> snap_lm_;
    mutable std::shared_ptr<const std::vector<uint16_t>> snap_tm_;
    mutable std::shared_ptr<const std::map<int, ma2::AudioClip>> snap_audio_;

    // Dialogs
    bool show_new_ = false, show_retail_ = false, show_validation_ = false;
    bool check_stale_ = false;  // an edit since the last Check: the open Check window re-runs it
    double check_time_ = 0;
    ma2::NewMapParams new_params_;
    int new_size_ = 0, new_base_ = 0, new_start_ = 0, new_copy_ = 0;
    int new_base_kind_ = -1;  // the kind the Theme list was last ordered for
    std::string msg_title_, msg_text_;
    bool show_msg_ = false;
    std::vector<ma2::ValidationItem> validation_;
    // Name clashes: the game lists two scenarios with one Name as "Name (file)"; the editor says so before the player sees it.
    struct HeadCache { fs::file_time_type time; ma2::ScenarioHead head; bool ok = false; };
    std::map<fs::path, HeadCache> head_cache_;
    const ma2::ScenarioHead* head_of(const fs::path& f);  // scenario.ini of f, cached by last write time; null if unreadable
    std::string placeholder_named_;  // F3: set when a save gave a placeholder name the file's name; the status line says so, then clears it
    std::string name_clash(const std::string& name, const fs::path& self, bool multiplayer);  // file name holding the same Name and Type, or ""
    std::vector<ma2::ValidationItem> check_now();  // validate() plus the name clash
    std::string default_new_name(bool multiplayer);  // "New Mission", "New Mission 2"...: no clash, no file of that name yet
    static bool is_default_name(const std::string& n);  // "New Map", "New Mission", "New Scenario", with or without a number
    ma2::SaveReport last_report_;
    std::string status_;
    // Developer dump: the level env map name and each drawn water model's reflection source.
    std::string env_map_name_;
    std::map<std::string, std::string> water_dbg_;
    float status_time_ = 0;
    bool spawner_ui_done_ = false;                           // Properties already drew the unit's spawner section this frame
    bool pick_obj_was_ = false;                              // The objective-target pick was armed last frame
    // Test options
    int test_mode_ = 0, test_red_ = 2, test_blue_ = 3, test_skill_ = 2;
    // Autosave
    float autosave_timer_ = 0;
    long long autosaved_at_ = 0;  // time() of the last recovery write (status bar "Autosaved hh:mm")
    std::string tool_hint_text();  // status bar left cell (tool_hints.h template)
    // Help
    bool show_help_ = false, show_getting_started_ = false, show_keys_ = false, show_about_ = false;
    void ui_about();
    void open_user_guide();
    void ui_help();
    int help_topic_ = 0;             // Help window: the topic shown
    bool help_ctx_ = false;          // F1 / Help > Help: open the topic of the tab and tool you are on
    int help_context_topic() const;  // its index in help_topics()
    int help_shown_ = -1;            // the topic whose page was last drawn (the page scrolls to the top when it changes)
    std::string help_filter_;        // "Find in Help"
    KeyText help_keys() const;       // the user's own keys and hold keys, for the Help text
    void ui_keys();
    void ui_getting_started();
    // Next steps card: in Properties on a map made with New while nothing is selected;
    // each step jumps to its tab and ticks itself once it is true.
    bool ns_on_ = false, ns_shaped_ = false, ns_tested_ = false, ns_placed_said_ = false, ns_looked_ = false;
    size_t ns_base_ = 0;  // things on the map when it was made (spawn rings, a copied level's units)
    size_t ns_things() const;
    void ns_start();
    void ui_next_steps();
    void ui_viewport_hint();
    void ui_start_page();  // no map open: New map / New mission / Open / Look at a real level + recent files
    // Confirmation popup for destructive actions: confirm(text, action) runs action on Yes.
    std::string confirm_text_;
    std::function<void()> confirm_action_;
    bool open_confirm_ = false;
    int confirm_answer_ = 0;  // automation: 1 yes, 2 no, applied on the next frame of the popup
    void confirm(const std::string& text, std::function<void()> action);
    void ui_confirm();
    // Placement preview under the mouse (the object about to be placed).
    // World offset from the placement point (the green circle) to the object's origin so the model's footprint centre sits on the circle.
    ma2::Vec3 place_origin_offset() const;
    bool place_footprint(float& w, float& l) const;
    void test_settings_ui();        // game type / bots / skill for Test (the Test button's arrow)
    bool preview_valid_ = true;
    std::string preview_reason_;
    ma2::Vec3 preview_pos_{};
    bool preview_on_ = false;
    // Undo for scenario-data widgets (names, triggers, messages): meta_pre_ holds that data as it
    // was before the active widget started editing; last_undo_id_ is the widget that pushed a step.
    Snapshot meta_pre_;
    unsigned last_undo_id_ = 0;
    void capture_meta(Snapshot& s) const;
    void apply_meta(const Snapshot& s);
    // Keys: one table (input_map.h), one dispatcher (app_input.cpp). dispatch_keys runs once a frame and runs each
    // key press through resolve(), so a key reaches exactly one action; kt() / kt2() are the on-screen names of an action's keys.
    void dispatch_keys(float dt);
    void brush_size_step(int dir);               // [ ], Ctrl+wheel: 12 % per step, whole metres, within the tool's range
    void brush_strength_step(int dir);           // Shift+[ ], Shift+wheel: 5 points per step
    void turn_view(float dx, float dy);          // the middle-drag / Alt+drag turn of the camera
    void draw_brush_rings(const ma2::Vec3& g);   // the half-strength ring and the size / strength readout
    void run_action(int act, float dt);          // the one switch: every key action
    int active_ctx() const;                      // Ctx bits that are active now
    std::string kt(int act) const;               // first key of an action as shown on screen ("" when unbound)
    std::string kt2(int act) const;              // "X or Y" of both keys
    void goto_tab(Tab t);
    int tab_strip_index() const;                 // 0..5 in the strip's order
    bool focus_filter_ = false;                  // FIND: the next palette search box takes the keyboard focus
    bool keymap_checked_ = false, keymap_ok_ = true;
    int key_counts_[A_COUNT + 2] = {};           // actions run by the dispatcher (automation: proof a key ran exactly once)
    int last_act_ = A_NONE;
    bool esc_closed_popup_ = false;  // ui_menu's Esc closed a pop-up this frame: map shortcuts skip it
    bool holding(int mod) const;  // a hold key (Settings::hold_*) is down
    std::string hold_swap_note_;    // Settings > Keys: which hold key the last choice swapped with
    std::string turn_keys_text() const;  // "Q / E" from the user's Turn left / right keys
    std::string pan_text() const;        // "W A S D or the arrow keys" from the Pan keys
    // Preferences window (View > Preferences, prefs.cpp).
    bool open_prefs_ = false, prefs_open_ = false;
    int capture_act_ = -1, capture_slot_ = 0;  // hotkey row waiting for a key
    void ui_preferences();
    // UI scale (Preferences > General): style and panel sizes from the start-up DPI scale.
    float base_dpi_ = 1.f;
    ImGuiStyle* base_style_ = nullptr;
    ImGuiStyle* raw_style_ = nullptr;  // the theme before any scaling (set by main)
    bool ui_scale_dirty_ = false;
    void apply_ui_scale();
    // The automatic size for the window (1 to 2 in quarter steps, 1920 x 1000 UI pixels per step of 1); 0.25 steps so a
    // 3840 x 1600 window gets 1.5. auto_fb_* is the last frame's client size (0 before the first frame).
    float auto_ui_scale() const;
    float auto_fb_w_ = 0, auto_fb_h_ = 0, auto_scale_cur_ = 0;
    // Mouse: right button pressed over the map, and whether it has been dragged (= pan, no cancel).
    bool rpress_vp_ = false, rdragged_ = false;
    bool edge_scrolling_ = false;
    std::vector<std::string> id_conflicts_;
    // Units palette: the dictionary audit's verdict per template (resource 202) and the category
    // each entry is listed under (special kinds get labelled categories; nothing is hidden).
    std::unordered_map<uint32_t, std::string> palette_kind_;
    std::string palette_category(const ma2::TemplateInfo& t) const;
    const char* palette_note(const ma2::TemplateInfo& t) const;  // label suffix, or nullptr  // MA2SE_ID_AUDIT (main.cpp)
    int exit_code_ = 0;                  // 2 = Test under --from-game (play test_file_)
    LiveTest live_test_;                 // --from-game-live: the Test waiting for main.cpp to pass on
    bool live_test_pending_ = false;
    std::vector<fs::path> saved_files_;  // every .ma2map saved this session, in order (--from-game list)
    fs::path test_file_;
    // Test: the running game (quick test returns to the editor when it exits).
    HANDLE test_proc_ = nullptr;
    bool test_quick_ = false;
    // Listening: own clips play from memory (the converted WAV); under --automation nothing is heard but the state is kept.
    Bytes audition_buf_;  // the WAV while it plays (PlaySound SND_MEMORY|SND_ASYNC reads it until it ends)
    int audition_clip_ = -1;   // the clip playing, else -1
    int audition_ms_ = 0;      // its length
    double audition_end_ = 0;  // ImGui time it ends
    void audition_play(int clip);
    void audition_stop();
    bool audition_playing(int clip = -1) const;  // clip -1: any
    bool audition_button(int clip, const char* id);  // a Play / Stop small button for an own clip; true when pressed
    void audition_retail_button(const char* id);     // the greyed Play beside a game cue
    std::wstring test_command_, test_dir_;  // automation: what Test would have run, and where
    bool test_launch_skipped_ = false;
    bool save_failed_ = false;  // F-V5R-1: the last cmd_save could not write (out of memory, disk): Test then does not run the old file
    bool test_game_up_ = false;  // the test game has its window up: the "Game started" note closes itself
    double test_started_ = 0;    // ImGui time the test game was launched (the "Game started" note also closes itself after a few seconds)
    void poll_test_process();
    static const char* skill_name(int s);
};

}  // namespace ed
