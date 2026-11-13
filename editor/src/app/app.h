// The editor application: state, layout, tools and tab panels.
#pragma once
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "../core/dictionary.h"
#include "../core/gamedata.h"
#include "../core/scenario.h"
#include "../core/templates.h"
#include "model_cache.h"
#include "render.h"
#include "settings.h"
#include "tool_hints.h"

namespace ma2::weather { struct Look; }

struct ImVec2;
struct ImDrawList;
struct ImGuiStyle;

namespace ed {

// Overlapping palette groups beyond the two in app_tabs.cpp: (name, tooltip). palette_search.cpp.
const std::vector<std::pair<const char*, const char*>>& palette_extra_groups();

enum class Tab { Map, Terrain, Players, Units, Triggers, Cameras, Messages, Environment, Options };


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
    std::vector<float> hgt;
    float min_h = 0, max_h = 0;
    ma2::Image colormap;
    ma2::GroundLayers ground;  // P4 (weights shared copy-on-write)
    std::vector<uint16_t> typemap;
    bool colormap_custom = false;
    bool shaped = false;  // Next steps '1. Shape the ground' was ticked, so undoing the only stroke unticks it
    // Scenario data outside the world, so trigger, message and objective edits undo too.
    std::string name, description, author, briefing_title, briefing;
    std::vector<std::string> types, objectives;
    std::map<int, std::string> strings;
    std::map<int, ma2::AudioClip> audio;
    std::set<uint32_t> selection;  // what was selected when this state was saved, so undoing a delete reselects it
    std::string label;
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
    // --automation 'dialogs' answer for the in-editor "Please confirm" popup, when one is open: the text, or "".
    std::string answer_confirm(bool yes);
    const char* tab_label() const;
    std::string palette_name(const ma2::TemplateInfo& t) const;  // "Friendly (Official)"
    std::string palette_name_base(const ma2::TemplateInfo& t) const;  // palette_name before "variant 2" numbering
    mutable std::map<uint32_t, int> dup_variant_;                      // template uid -> 1, or 2.. for a later same-name template
    std::string palette_plain_name(const ma2::TemplateInfo& t) const;  // palette_name without the "(Official)" suffix
    std::string water_type_name(const ma2::TemplateInfo& t) const;  // plain name + a look word where two waters share it
    mutable std::map<std::string, std::string> water_names_;        // water model (lower) -> water_type_name
    void dump_palette(const std::string& path);
    void dump_search(const std::string& words, const std::string& path);  // developer: --dump-search "word|FILE"
    void dump_sweep(const std::string& dir);  // developer: --dump-sweep DIR (palette_search.cpp)
    // Hidden / --automation runs: the editor never takes the foreground (Test's return included).
    bool background = false;
    // Opened from the game's Custom Scenarios screen (--from-game, ux/spec-ingame-editor.md 6): File > Exit reads
    // 'Back to the game', Test saves and returns with exit code 2 (the game plays it), and at exit the editor writes
    // the result file (--saved-list: opened=, saved=, test=) the game reads.
    bool from_game = false;
    fs::path from_game_opened;  // the file the game opened it on ("" = start page)
    void start_from_game(const fs::path& file);  // opens `file` (if any) and says how to get back
    int exit_code() const { return exit_code_; }
    void write_saved_list(const fs::path& list) const;
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
    bool cursor_object_top(ma2::Vec3& out);  // placing: roof / deck under the cursor above the ground (UX G10)
    int pick_object(float mx, float my, std::vector<int>* all = nullptr);  // all: every root under the cursor, nearest first (Alt+click cycling)
    void apply_brush(const ma2::Vec3& c, float dt, bool first);
    void place_object(const ma2::Vec3& at);
    void scatter(const ma2::Vec3& at);
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
    bool add_type_kit(const std::string& game_type);  // the type's flags/bases from a real level (M7)
    bool check_errors_only_ = false;  // Check window: "Must fix only" toggle
    bool check_fixed_ = false;  // a Check fix button ran this frame: close the window if nothing is left
    bool has_ground_model(const ma2::WObj& o);
    // Cropped road pieces and foundations (app_crop.cpp, core/crop.h). playtest 11-01: roads and foundations can be cut to
    // fit a gap; foundations snap edge to edge and rest a little above the ground.
    static std::string crop_key(const ma2::WObj& o);  // its EditorCrop, "" = whole
    GpuModel* obj_model(const ma2::WObj& o);          // Corporal3DObj + CorporalMTB as drawn: cut to the crop
    bool is_foundation(const ma2::TemplateInfo& t) const;  // building bases, parking lots, footings (flat ground plates)
    bool is_foundation_obj(const ma2::WObj& o);
    bool croppable(const ma2::WObj& o);                    // a road piece, or a foundation whose model has loaded flat
    static constexpr float kFoundationLift = 0.08f;        // metres between the plate and the highest ground under it
    // Origin height that puts a plate model (at x, z, heading yaw) kFoundationLift above the highest ground under its footprint.
    float plate_rest_y(const GpuModel* gm, float x, float z, float yaw);
    bool foundation_rest(const ma2::WObj& o, float& origin_y);
    void crop_ui(ma2::WObj& o);   // Properties: the Crop rows of a road piece or foundation
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
    struct CutPreview { bool on = false; uint32_t cin = 0, still = 0; float t = 0, stop_at = 0; Camera saved; bool helpers = true; } cpv_;  // still: Look through that camera
    void cutscene_preview_start(uint32_t cinema_uid, int shot = -1);  // shot >= 0: only that shot (0-based)
    void camera_look_through(uint32_t cam_uid);  // Cameras list: view from a camera no shot uses (cpv_.still)
    void cutscene_preview_stop();
    void cutscene_preview_step(float dt);  // frame(): sets cam_ from the shot at cpv_.t, draws the letterbox
    bool ui_field(ma2::WObj& o, const char* key);  // one setting by game name, dictionary widget (app_tabs.cpp)
    bool ui_common_fields(ma2::WObj& o);  // dictionary widgets for an object's Common settings (app_tabs.cpp)
    bool ui_unit_arrives(uint32_t uid, const std::vector<uint32_t>& hiders);  // Properties: one 'Arrives' row for a unit (app_spawner.cpp)
    uint32_t arrival_circle();  // a new 'Reinforcements arrive' circle, placed by the next map click
    uint32_t pad_vehicle(uint32_t uid);  // a vehicle pad's stand, spawner or vehicle -> the waiting vehicle, else 0 (app_spawner.cpp)
    void ui_spawner(uint32_t uid);  // Properties: a spawner's start (at the start / when a trigger fires) and counts (app_spawner.cpp)
    void ui_unit_orders(uint32_t uid);    // unit Properties: 'Starting orders' summary + Give orders (app_triggers.cpp)
    uint32_t give_unit_orders(uint32_t uid);  // new start-of-mission patrol order, opens it and starts drawing the path
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
    void lit_windows_control(ma2::Scenario& s, int env_index);
    void apply_look_ui(ma2::Scenario& s, const ma2::weather::Look* look, const char* what);  // null: the base level's own light
    // Retail-reach #41 / UX-SPEC row 6: a sky's own picture for the sky and look lists, and a list row with it (app_environment.cpp)
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
    void duplicate_selected();
    void add_cutscene_shot(uint32_t cinema_uid);
    void select_all();
    void road_view(const ma2::WObj& o, ObjView& v);
    bool sky_use_fog_ = false;            // Map tab: picking a sky also takes its fog colour
    Camera cam_;
    Settings settings_;
    ma2::GameData gd_;
    std::unique_ptr<ma2::TemplateCatalog> cat_;
    std::unique_ptr<ma2::ScenarioIO> io_;
    std::unique_ptr<ma2::Scenario> doc_;
    std::string game_error_;
    bool quit_ = false;

    Tab tab_ = Tab::Map;
    Tool tool_ = Tool::Select;
    float vp_x = 0, vp_y = 0, vp_w = 1, vp_h = 1;
    bool vp_hover_ = false;

    std::vector<ObjView> views_;
    std::set<uint32_t> selection_;      // root UIDs
    uint32_t hover_uid_ = 0;
    bool objects_dirty_ = true, minimap_dirty_ = true, overlay_dirty_ = true;
    int overlay_plants_ = -1;  // culture type the overlay shows (Plants tool), -1 = the type map

    // Terrain tools
    bool plateau_auto_ = false;  // this stroke set the plateau height itself (it was not above / below the ground there)
    Tool stroke_tool_ = Tool::Select;  // the brush a stroke started with: a paired tool keeps that half until the button is let go
    float sculpt_slide_[3] = {0, 0, 0};  // Terrain tab pairs: each highlight's place between its top (0) and bottom (1) half
    Tool sculpt_now() const;  // the shaping brush a click would use now (the pair's other half while 'Reverse the brush' is held)
    float stroke_change_ = 0;  // largest height step of the current Raise / Lower stroke (status line)
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
    // Paint section (app_paint.cpp, P1): Texture / Colour tint / Boundaries; the palette backs onto the retail
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
    void enable_ground_layers();
    void disable_ground_layers();
    bool add_ground_texture(const std::string& path, const std::string& archive);
    bool replace_ground_texture(int k, const std::string& path, const std::string& archive);
    void remove_ground_layer(int k);
    void fill_ground(int layer);
    int pick_target_ = -1;       // texture picker: -1 = add, else the palette entry being changed
    bool open_pick_ = false;
    std::string pick_filter_;
    void paint_ground_layers(float cx, float cz, float radius, float strength);
    bool water_look_ui(ma2::WObj& o);  // true when it replaced the object
    ID3D11ShaderResourceView* palette_picture(const ma2::TemplateInfo& t);
    uint32_t water_type_combo(const char* id, uint32_t cur_uid);
    // The one list of water types (palette Water group, Terrain > Water, water Properties): one per model,
    // the ones that can be any size first, then the fixed-size ones, A-Z by water_type_name within each.
    const std::vector<const ma2::TemplateInfo*>& water_types();
    std::vector<const ma2::TemplateInfo*> water_types_;
    void arm_water(uint32_t template_uid);  // pick a water type: resizable -> Place water, fixed -> place it like an object
    bool show_typemap_ = false;
    bool typemap_auto_ = false;  // the Boundaries tool turned the overlay on; leaving the tool turns it off again
    bool stroke_active_ = false;
    int dirty_x0_ = 1 << 30, dirty_z0_ = 1 << 30, dirty_x1_ = -1, dirty_z1_ = -1;
    bool lightmap_stale_ = false;
    bool stale_sun_only_ = false;  // the light map is stale only because the sun moved (from lightmap_sun_)
    ma2::Vec3 lightmap_sun_{};

    // Object tools
    std::string unit_filter_, unit_category_;
    bool unit_base_only_ = true;
    int unit_group_ = 0;         // palette task group (palette_groups in app_tabs.cpp); 0 = 'Mechs
    bool palette_grid_ = true;   // pictures (grid) or list
    int palette_thumb_budget_ = 3;
    uint32_t place_template_ = 0;
    bool grid_snap_ = false;                // UX G9: placing snaps to a kGridSnap grid (Edit menu / placing panel)
    static constexpr float kGridSnap = 5.f;
    Tool last_place_tool_ = Tool::Select;  // Space (A_PLACE_AGAIN) re-arms it after Esc (UX G7)
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
    bool road_snap(const ma2::Vec3& cursor, ma2::Vec3& out_pos, float& out_yaw);  // end-to-end snapping for the piece being placed
    bool road_snapped_ = false;
    int snap_family(const ma2::TemplateInfo& t) const;  // 1 = wall/fence/gate piece, 2 = bridge piece, 0 = places freely
    bool piece_snap(const ma2::Vec3& cursor, ma2::Vec3& out_pos, float& out_yaw);  // wall / bridge pieces join end to end
    // Join points of the placed pieces of one snap family (piece_snap's rule): a piece's footprint in its own axes, and its two ends.
    struct SnapFoot { float cx, cz, half; bool along_x; };
    struct SnapEnd { float x, z, vx, vz, axis, half, y; int piece; int side = -1; float ty = 0; };  // ty: height of the join point itself (a foundation's edge top)  // side (foundations): 0 +x, 1 -x, 2 +z, 3 -z edge of the piece  // y: the piece's origin height; axis: heading of its own +long axis
    bool snap_foot(const ma2::TemplateInfo& t, SnapFoot& f);
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
    int snap_turn_ = 0;  // Q / E while a wall piece is joined: 0 straight, 1 / 3 a quarter right / left
    struct RoadOpenEnd { float x, z, vx, vz; bool joined; };  // drawn while placing a road: where a click near it will join
    std::vector<RoadOpenEnd> road_open_ends_;
    bool place_on_top_ = false;  // the placing ghost stands on an object (UX G10)
    int place_on_top_of_ = -1;   // ...this one (world.objs index), named in the hint
    int road_attach_ = 0;          // which end of the piece being placed joins the open end (Q / E cycle it while snapped)
    uint32_t road_chain_uid_ = 0;  // the last road piece placed: its open end catches the next piece from further away
    std::string place_road_;   // road piece being placed (empty = place_template_ applies)
    bool road_ready_ = false;
    std::vector<RoadPiece> road_pieces_;
    const std::vector<RoadPiece>& road_pieces();
    const RoadPiece* find_road_piece(const std::string& path);
    int palette_road_rows(const std::string& filt, const std::string& category, bool base_only, int base_idx);
    bool palette_group_has(const ma2::TemplateInfo& t, const std::string& group) const;
    bool palette_in_group(const ma2::TemplateInfo& t, bool special, int g) const;  // palette button g lists t (app_tabs.cpp)
    bool palette_search_match(const std::string& filt, const ma2::TemplateInfo& t, const std::string& shown) const;
    mutable std::unordered_map<uint32_t, std::string> search_text_;
    float place_heading_ = 0;
    int place_side_ = 5;                  // missions: side of placed units (5 enemy, 6 player's side, 0 neutral)
    int place_team_ = 1;                  // multiplayer: team of placed turrets / people (Nation 1 Red, 2 Blue, 0 neutral)
    std::vector<uint32_t> scatter_set_;
    float scatter_density_ = 0.4f;
    float scatter_accum_ = 0;
    struct Clip { std::vector<ma2::WObj> objs; ma2::Vec3 pos; };
    std::vector<Clip> clipboard_;
    bool dragging_ = false, drag_moved_ = false, box_selecting_ = false;
    float drag_start_x_ = 0, drag_start_y_ = 0;
    ma2::Vec3 drag_last_{};

    // Transform gizmo (gizmo.cpp): Move / Rotate / Tilt handles on the selection's centroid.
    int gizmo_mode_ = 0;                  // 0 move, 1 rotate, 2 tilt
    int gizmo_hot_ = 0, gizmo_drag_ = 0;  // handle under the mouse / being dragged (0 none)
    struct GizmoDrag {
        ma2::Vec3 pivot{}, hit0{}, applied_v{};  // pivot, grab point when the drag started, plane move already applied
        float applied = 0, start = 0;     // amount already applied / the start angle or axis parameter
        float accum = 0;                  // raw angle turned so far (rotate / tilt)
        float t = 0;                      // ring parameter at the mouse (edge-on rings)
        bool plane = false;               // last frame used the ring-plane method
        bool pushed = false;
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
    float water_new_w_ = 200.f, water_new_l_ = 120.f;
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
    void dig_water_basin(const ma2::Vec3& c, float w, float l, bool oval, float surface, float heading = 0);
    void place_water(const ma2::Vec3& at);
    uint32_t water_place_tpl();                // the type the tool places: the picked one, else the first resizable (0 = none)
    // Ponds (kept for developer probes; the Terrain tab no longer offers it): a basin plus the smallest water tile.
    float pond_radius_ = 25.f, pond_depth_ = 8.f;  // shallow water is faint in the game, so a new pond starts deep
    void make_pond(const ma2::Vec3& at);
    void select_uid(uint32_t uid);
    int pick_water(float mx, float my, float t_other, bool marker_only);
    void ui_placed_list();
    size_t placed_count() const { return placed_n_; }  // status bar 'N placed' (UX-SPEC 5, M9), counted in rebuild_objects
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
        enum Kind { None, MoveShape, LinkMarker, OrderPath, CamPath, ObjTargets } kind = None;
        int objective = -1;             // ObjTargets: the objective whose 'destroy' trigger (uid) gets each clicked unit
        uint32_t uid = 0;               // MoveShape: the trigger box / circle; LinkMarker: the object whose link names the marker; OrderPath: the MAEvent; CamPath: the MAPath
        std::string key, name;          // LinkMarker: the link property, the marker's name
        bool null_marker = false;
        uint32_t follow = 0;            // MoveShape: an object that moves with the shape (a 'Reach a place' radar marker)
        bool restart = false;           // CamPath: the next click starts the path over
        bool redraw = false;            // CamPath: started by Draw flight path (that button stays lit, not Add points)
        Tab back = Tab::Triggers;       // the tab to return to after the click
    };
    PickTarget pick_;
    // A map-pick mode for this target is waiting for clicks (its button shows the toggle "on" look).
    bool picking(PickTarget::Kind k, uint32_t uid) const { return (tool_ == Tool::PickPoint || tool_ == Tool::PlaceWaypoint) && pick_.kind == k && pick_.uid == uid; }
    void ui_helper_buttons();       // Units tab: add lights, cameras, checkpoints, markers, menu triggers (app_add_part.cpp)
    void place_helper(const char* cls, const char* label, const char* group);
    // A trigger box / circle (the retail MATriggerCuboidalVolume / MATriggerRadius, as Objectives & Triggers > New trigger
    // makes it) put down with the next click; its actions are set on Objectives & Triggers (G8: placeable from Units and Props too).
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
    // Name clashes (W-12b): the game lists two scenarios with one Name as "Name (file)"; the editor says so before the player sees it.
    struct HeadCache { fs::file_time_type time; ma2::ScenarioHead head; bool ok = false; };
    std::map<fs::path, HeadCache> head_cache_;
    const ma2::ScenarioHead* head_of(const fs::path& f);  // scenario.ini of f, cached by last write time; null if unreadable
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
    void ui_keys();
    void ui_getting_started();
    // Next steps card (UX-SPEC 8): in Properties on a map made with New while nothing is selected;
    // each step jumps to its tab and ticks itself once it is true.
    bool ns_on_ = false, ns_shaped_ = false, ns_tested_ = false, ns_placed_said_ = false;
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
    // World offset from the placement point (the green circle) to the object's origin so the model's footprint centre sits on the circle (U6).
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
    // Hotkeys (Settings::keys): pressed = this frame, exact modifiers; held = key down, Shift ignored
    // (Shift pans faster).
    bool pressed(int act, bool repeat = false) const;
    bool esc_closed_popup_ = false;  // ui_menu's Esc closed a pop-up this frame: map shortcuts skip it
    bool held(int act) const;
    bool pressed_quarter(int act, bool repeat = false) const;  // the same key with the quarter-turn hold key added (Ctrl+Q / E turn 90 deg)
    bool holding(int mod) const;  // a hold key (Settings::hold_*) is down
    std::string hold_swap_note_;    // Settings > Keys: which hold key the last choice swapped with
    std::string turn_keys_text() const;  // "Q / E" from the user's Turn left / right keys
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
    std::vector<fs::path> saved_files_;  // every .ma2map saved this session, in order (--from-game list)
    fs::path test_file_;
    // Test: the running game (quick test returns to the editor when it exits).
    HANDLE test_proc_ = nullptr;
    bool test_quick_ = false;
    bool test_game_up_ = false;  // the test game has its window up: the "Game started" note closes itself
    double test_started_ = 0;    // ImGui time the test game was launched (the "Game started" note also closes itself after a few seconds)
    void poll_test_process();
    static const char* skill_name(int s);
};

}  // namespace ed
