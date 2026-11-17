// The editor's document: a world (retail format) plus terrain textures and scenario metadata,
// saved as a .ma2map (SPEC.md: one plain MGF v4 archive).
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gamedata.h"
#include "ground_layers.h"
#include "mgic.h"
#include "pathfind.h"
#include "templates.h"
#include "terrain.h"
#include "water_custom.h"
#include "wdf.h"

namespace ma2 {

// Game-type names used in .ini files, and the MAMissionTypeGroup / MARoot flag property for each.
struct GameTypeDef { const char* ini; const char* flag; const char* label; bool team; };
const std::vector<GameTypeDef>& game_types();
const GameTypeDef* find_game_type(std::string_view ini_name);
// Which objects the game keeps in game type g: the ones with no MAMissionTypeGroup ancestor that has
// g's flag off (the game detaches the other groups at level load). 1 = present, 0 = left out.
std::vector<char> objects_in_game_type(const Wdf& w, const WTree& t, const GameTypeDef& g);
// What the game shows of each object when the mission starts (checked against live entity dumps of
// the retail missions): START_HIDDEN = hidden by a show/hide action of a timer that fires at once
// (shown later by a trigger), START_SPAWNED = a spawner's unit, not in the world until spawned,
// START_REMOVED = removed by such a timer, START_DESTROYED = a building it destroys (the game shows its
// DestroyedBuilding template in its place), START_CUTSCENE = a cinema prop the game removes when the
// cinema ends.
enum : char { START_SHOWN = 0, START_HIDDEN = 1, START_SPAWNED = 2, START_REMOVED = 3, START_DESTROYED = 4, START_CUTSCENE = 5 };
std::vector<char> objects_at_start(const Wdf& w, const WTree& t);

struct AudioClip {
    std::string source;   // original file name (informational)
    Bytes wav;            // PCM 16-bit WAV bytes
};

// Editor-side trigger model (AoE2 style). Compiled into native MATrigger*/MAAction* objects.
struct EdCondition {
    std::string kind;                    // "timer", "radius", "volume", "destroyed", "gametime", "variable", ...
    std::map<std::string, std::string> args;
};
struct EdEffect {
    std::string kind;                    // "message", "voice", "sound", "audio", "music", "objective", "win", "lose", ...
    std::map<std::string, std::string> args;
};
struct EdTrigger {
    std::string name = "Trigger";
    bool enabled = true, loop = false, any = false;  // any = OR of conditions
    std::vector<EdCondition> conditions;
    std::vector<EdEffect> effects;
    uint32_t compiled_uid = 0;           // root of the compiled native subtree (0 = not compiled)
};

// The game cuts a scenario's Name and Description (95 and 255 characters); the editor stops at less so nothing is cut mid-word.
constexpr int kNameMax = 60, kDescMax = 250;  // characters (UTF-8 code points), not bytes
// Cuts s to at most max_chars code points; returns the number of code points it has now.
inline int clip_chars(std::string& s, int max_chars) {
    int n = 0;
    size_t at = 0;
    while (at < s.size()) {
        if (n == max_chars) { s.resize(at); break; }
        unsigned char c = (unsigned char)s[at];
        at += c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        n++;
    }
    return n;
}

// Bump when the export changes what the game needs. scenario.ini [Scenario] SavedWith records it; a file with a lower number
// (or none: 1) was saved by an older editor and is offered an update (File > Update old scenarios...). The game reads it too.
constexpr int kSaveRevision = 2;

struct Scenario {
    // Metadata (scenario.ini)
    uint32_t id = 0;
    int saved_with = kSaveRevision;      // the revision of the editor that wrote the file this was loaded from (1 = no SavedWith line)
    std::string name = "New Scenario", description, author;
    bool multiplayer = true;
    std::vector<std::string> types;      // ini game-type names
    std::string base = "boggeddown.mgf"; // retail resource archive
    std::string briefing_title, briefing;
    std::string win_text, lose_text;     // [Mission] WinText / LoseText: the line on the end-of-mission screen (empty = the game's own)
    std::map<int, std::string> strings;  // [Strings]
    std::map<int, AudioClip> audio;      // [Audio]
    std::vector<EdTrigger> triggers;
    std::vector<std::string> objectives; // mission objectives (index = mission variable)

    // Content
    Wdf world;
    Terrain terrain;
    Image colormap, lightmap;            // decoded; empty when unknown
    bool colormap_custom = false, lightmap_custom = false;
    bool colormap_lossless = false;      // F3: the colour map came from editor/colormap.bin, not from the game's DXT file (old file / changed TIF: false)
    Mgic colormap_style, lightmap_style; // container/format to encode with
    Picture colormap_layout, lightmap_layout;
    GroundLayers ground;                 // More than two ground textures (PC only, opt-in; ground_layers.h)
    std::vector<uint16_t> typemap;       // MATerrainInfo 512x512 flags, row 0 = z 0 (as stored); empty if none
    int typemap_w = 0, typemap_h = 0;
    bool typemap_dirty = false;

    // State
    fs::path file;
    bool read_only = false;              // a retail map opened for viewing
    std::string retail_name;             // for read-only views
    bool dirty = false;
    std::vector<uint32_t> old_cut_roads;  // Cut road pieces whose saved .road / .node / .mesh have bare '\n' line ends (editor before); a save clears it

    std::string base_level() const;      // "boggeddown"
    std::string id_hex() const;          // "1A2B3C4D"
    std::string size_text() const;       // "257x257"
};

struct NewMapParams {
    bool multiplayer = true;
    int w = 129, h = 129;
    std::string base = "boggeddown.mgf";
    float min_h = -20.f, max_h = 107.5f;   // 127.5 m over 255 steps: exactly 0.5 m a step, so flat ground reads 10.00
    enum Start { Flat, Noise, CopyRetail } start = Flat;
    float flat_height = 10.f;
    float noise_amp = 0.35f;             // fraction of the height range
    float noise_scale = 220.f;           // metres per noise feature
    uint64_t seed = 1;
    std::string copy_map;                // retail map name for CopyRetail
    std::vector<std::string> types{"TeamDestruction"};
    std::string name = "New Map";
    // Keep the Base world's own heights and textures (size from Base); used by bisect variants.
    bool keep_base_terrain = false;
    // Remove the Base level's culture system (MACultureSystem subtree); bisect only.
    bool drop_culture = false;
};

struct SaveReport {
    size_t objects = 0, templates = 0, asset_files = 0;
    uint64_t asset_bytes = 0, base_bytes = 0, file_bytes = 0;
    int nav_nodes = 0;
    double graphics_mb = 0;  // ScenarioIO::graphics_estimate total
    std::vector<std::string> warnings;
    // Over what the game can load (more than kMaxObjects objects, or graphics_mb past kGraphicsErrorMb): the game has no
    // guard of its own and crashes at load. save() refuses unless told to go ahead; limit_text says why.
    bool over_limit = false;
    std::string limit_text;
};

// MATerrainInfo type-map values (bisect runs, 2026-10-20). Every retail MP spawn, vehicle
// pad and mode objective stands on 0 or 8; bots never enter 1. Other values (16, 32, ...) appear in
// retail maps with unknown meanings.
namespace tmtype {
// 8 = shallow water players wade in, 16 = deep water (retail campaign harbours/lakes 2-8 m; wiki: research 10-26 depths).
constexpr uint16_t Playable = 0, OutOfBounds = 1, Border = 2, Special = 8, DeepWater = 16;
inline bool playable(uint16_t v) { return v == Playable || v == Special; }
const char* label(uint16_t v);  // "Playable", "Out of bounds", ... or nullptr for other values
}  // namespace tmtype

// Sets a unit's side (Nation 6 player's side, 5 enemy, 0 neutral). Mechs and vehicles also get a
// pilot or not: neutral ones start empty (StartAbandoned, the player can climb in); others are
// crewed, full detail and collidable like retail mission units. An empty unit counts as lost at
// once for "units destroyed" triggers.
void set_unit_side(WObj& o, int nation);

// Type-map value under a world position (row 0 = z 0), or -1 when the world has no map.
int typemap_at(const Scenario& s, float x, float z);
// Makes the whole terrain one playable area: `margin` cells of 1 at the edge, then `band` cells
// of 2, 0 inside (retail edges are 1; their 2-bands are 6-14 cells wide at the 90th percentile).
void fill_playable_typemap(Scenario& s, int margin = 4, int band = 6);
// The layout the save writes the game's colour map in: the level's size and mips, swizzled A8R8G8B8 (never DXT,
// whose 5:6:5 rounding shifted dark ground colours in the game).
Picture colormap_game_layout(const Picture& level);
// Which colour-map pixels lie on the playable ground of the type map tm (tw x th, row 0 = z 0); all of them when it has none.
std::vector<uint8_t> playable_colormap_mask(const Image& cm, const std::vector<uint16_t>& tm, int tw, int th);
// Share of the ground (mask) showing TerrainDetailMap (alpha 255) rather than TerrainDetailMap 1; on green ground, weighted by green.
float first_texture_share(const Image& cm, const std::vector<uint8_t>& mask);

// level: 0 info, 1 warning, 2 error. uids: every object of the item (uid = the first); fix: 0 none, 1 put them on the ground,
// 2 put the newer one on top, 3 put it on the ground (hanging object); the editor's placement checks (app_checks.cpp) fill both.
struct ValidationItem { int level; std::string text; uint32_t uid; int fix_team = -1; int fix_missing = 0; std::vector<uint32_t> uids; int fix = 0; };

// What the game's list shows of a saved scenario, read from scenario.ini alone (no world, no textures): cheap enough to
// scan the whole scenario folder.
struct ScenarioHead {
    uint32_t id = 0;
    std::string name;
    bool multiplayer = true;
    int saved_with = 1;
};

class ScenarioIO {
public:
    ScenarioIO(GameData& gd, TemplateCatalog& cat) : gd_(gd), cat_(cat) {}

    bool open_retail(const MapInfo& m, Scenario& out, std::string* err);
    bool create(const NewMapParams& p, Scenario& out, std::string* err);
    // warnings (optional): the archive entries whose stored checksum did not match; optional ones (ground texture, shadow map)
    // are replaced by defaults, so the load still succeeds.
    bool load(const fs::path& file, Scenario& out, std::string* err, std::vector<std::string>* warnings = nullptr);
    // Reads only scenario.ini of a .ma2map; false when the file is not a scenario archive.
    static bool peek(const fs::path& file, ScenarioHead& out);
    // force_over_limit: write the file although it is over the game's limits (SaveReport::over_limit; save() then fails with rep->over_limit set).
    bool save(Scenario& s, const fs::path& file, SaveReport* rep, std::string* err, bool force_over_limit = false);
    // Builds the .ma2map bytes without writing (used by save and tests). ready = false writes "Ready = 0" (the game does not
    // list the scenario); so does a scenario over the game's limits. save() passes false while Check shows a Must fix item.
    bool build(Scenario& s, Bytes& out, SaveReport* rep, std::string* err, bool ready = true);
    // Tests only: the next build() throws std::bad_alloc, as when the machine runs out of memory (the guard turns it into a message).
    static bool test_fail_alloc;

    std::vector<ValidationItem> validate(const Scenario& s);

    // Terrain textures for a world (colormap/lightmap paths -> Base archive, else defaults).
    void load_terrain_textures(Scenario& s);
    // The ground texture a new map on Base level `base` starts on (a per-level table chosen by looking; else the texture
    // covering most of its playable ground). terrain = its MATerrain, cm / mask = its colour map and playable pixels.
    std::string new_map_ground(const MapInfo& base, const WObj& terrain, const Image& cm, const std::vector<uint8_t>& mask);
    // The New dialog's preview of that ground: texture path and the colour it shows in the game (0xAABBGGRR). False when unreadable.
    bool theme_ground(const MapInfo& m, std::string& path, uint32_t& rgb);
    // A ground texture's average colour (0..1), from the Base archive, common.mgf or any level archive.
    bool ground_texture_average(const std::string& base, const std::string& path, double rgb[3]);
    // Generates a colormap from the Base colormap palette and the terrain shape; use (optional, one per colour-map pixel)
    // picks the pixels the palette is read from (a new map: the Base level's playable ground).
    // centred: heights count from the map's mean (a new map: its ground takes the theme's middle colours).
    Image procedural_colormap(const Scenario& s, int size, const std::vector<uint8_t>* use = nullptr, bool centred = false);
    void bake_lightmap(Scenario& s);
    NavGraph build_nav(const Scenario& s);

    // MP game-mode kit: clones the retail MAMissionTypeGroup subtrees that carry the mode from the
    // retail map `source`, scaling positions into this map. Returns the number of objects added.
    int add_mode_kit(Scenario& s, const std::string& game_type, const std::string& source, std::string* err);
    // Retail maps that have a game type.
    std::vector<std::string> maps_with_mode(const std::string& game_type);
    // Whether the game type needs its own objects (flags, bases, data core) that Check asks for.
    static bool mode_needs_kit(const std::string& game_type);

    // Spawn points (MALevelStart). team: 1 Red, 2 Blue, 0 free-for-all. Spawns live in
    // MAMissionTypeGroups "Team Spawns" (team modes) / "FFA Spawns" (FFA modes).
    uint32_t add_spawn(Scenario& s, int team, Vec3 pos, float heading);
    // Places count spawns of a team on a ring.
    void add_spawn_ring(Scenario& s, int team, Vec3 center, float radius, int count);
    // Re-flags the spawn groups for the scenario's enabled modes.
    void sync_spawn_groups(Scenario& s);

    // Vehicle pads found in the retail MP maps (MACorporeal pad -> MASpawner -> vehicle).
    struct PadKind { std::string label, map; uint32_t pad_uid; };
    const std::vector<PadKind>& pad_kinds();
    uint32_t add_pad(Scenario& s, const PadKind& kind, Vec3 pos, float heading);
    // Reinforcements, retail pattern B (wiki reinforcements-spawner-pattern): moves a placed unit under a new
    // NULL corporeal (template 32093) > MASpawner (Enabled 0, one copy, TriggerEnableID 0 until a trigger is
    // picked). Returns the spawner's UID, 0 if the NULL template or the spawner prototype is missing.
    uint32_t make_reinforcement(Scenario& s, uint32_t unit_uid);

    // Clones a retail world subtree (from map `source`, root `uid`) under parent_uid in s,
    // fixing spawner ids and entity-set membership. Returns the new root UID.
    uint32_t clone_from_retail(Scenario& s, const std::string& source, uint32_t uid, uint32_t parent_uid,
                               std::vector<WObj>* out_objs = nullptr);
    // Same for an already-loaded source world.
    uint32_t clone_from_world(Scenario& s, const Wdf& src, uint32_t uid, uint32_t parent_uid);
    // Places a template at pos (ground-relative heights kept from the retail instance it copies,
    // or from the template itself) under the "Placed Objects" group. Returns the new root UID.
    uint32_t place_template(Scenario& s, uint32_t template_uid, Vec3 pos, float heading);
    // Places one MARoad piece (a prefab .road file, "roads/country/90turn.road") at pos, turned by
    // yaw radians, in a "Road Group" under "Placed Roads". Roads have no template. 0 if no retail
    // world offers a prototype. (roads.cpp)
    uint32_t place_road(Scenario& s, const std::string& road_path, Vec3 pos, float yaw);
    // The height a road piece's start point needs so its flat slab clears the ground under its whole
    // footprint (the .road boundary, turned by yaw): the highest ground there + 0.15 m. Placing on a
    // slope at the start point's height alone buried part of the slab. (roads.cpp)
    float road_rest_height(const Scenario& s, const std::string& road_path, Vec3 pos, float yaw);
    // Moves a subtree so its root sits at `at`; parts keep their height above the ground
    // (measured on src_terrain when given, else on the scenario's terrain).
    static void move_subtree_to(Scenario& s, uint32_t root_uid, Vec3 at, float heading, bool set_heading_too,
                                const Terrain* src_terrain);
    // Part kinds: every class the retail worlds use, with a prototype copied from a retail instance
    // (links reset to 0). Built on first use from all retail worlds. (protolib.cpp)
    struct PartKind {
        uint32_t cls = 0;
        std::string name;                // class name, "MATriggerTimer"
        int count = 0, worlds = 0;       // instances in all retail worlds, worlds that have one
        bool positioned = false;         // has a place on the map
        WObj proto;
        std::string sample_map;          // the retail world the prototype came from
        std::vector<std::pair<uint32_t, int>> parent_classes;  // classes retail puts it under (0 = top), most used first
    };
    const std::vector<PartKind>& part_kinds();
    const PartKind* find_part_kind(uint32_t cls);
    // Adds a new object of that class (the retail prototype, UID fresh) as the last child of parent_uid;
    // positioned classes go to `at` on the ground. Returns the new UID, 0 when the class or parent is unknown.
    uint32_t add_part(Scenario& s, uint32_t cls, uint32_t parent_uid, const Vec3* at);
    // Copies an object with all its parts next to the original (same parent): fresh UIDs, links inside the
    // copy remapped, links outside kept, entity-set membership kept. offset (m) shifts positioned parts.
    // Returns the new root UID, 0 for the root or an unknown object.
    uint32_t duplicate_subtree(Scenario& s, uint32_t uid, float offset);
    // True for properties that name another object of the same world (statistics over the retail
    // worlds: ActionEntity, TriggerID, eyeUID...); the inspector offers an object picker for them.
    bool is_object_link(uint32_t key);
    // Cutscenes (protolib.cpp): a real MACinema tree built from a retail one (Mission_05's IcarusGoBoom
    // Cutscene: Camera Controllers > one shot with its eye / focus / viewport / filter steps, Cameras, Triggers
    // "start" and "end", Objects (a focus marker and a flyby path owner with its waypoints), Paths), placed
    // around `at`: the camera flies a path over the focus marker. Returns the cinema's UID (0 on failure).
    // behind: a heading (radians, forward = (sin h, cos h)); the flight path is turned about `at` so it
    // lies behind that heading (an intro over the player start); null = the retail template's own layout.
    uint32_t add_cutscene(Scenario& s, Vec3 at, const std::string& name, const float* behind = nullptr, bool detach_flyby = true);  // detach_flyby false: the shot keeps hanging under the template marker (tests of old files)
    // Adds another shot to a cinema: a copy of the cinema's last shot (controller with its steps, camera,
    // path with its waypoints, focus marker, start/end triggers) appended after it, links remapped, 25 m aside.
    // Returns the new controller's UID (0 when the cinema has no shot to copy).
    // The new shot is a STILL 5 s shot: its pieces are copies of the last shot's, but nothing it carries is
    // copied (the start / end triggers are cloned empty; end actions move from the old last shot to the new one, 2.1), its length
    // is `len`, and its eye stands at `eye` looking at `look` (core/cutscene: set_shot_view). Null eye / look: the pieces move
    // 25 m +X of the last shot (old behaviour). Returns the new controller's UID (0 when the cinema has no shot to copy).
    uint32_t add_cutscene_shot(Scenario& s, uint32_t cinema_uid, const Vec3* eye = nullptr, const Vec3* look = nullptr, float len = 5.f);
    // Gives a shot an eye path again when its type-2 step's eyeUID is 0 or dangling (the path was deleted): clones the
    // path and waypoint group of the Mission_05 template cutscene into the cinema (the waypoints start at the camera's spot)
    // and relinks. Returns the path's UID, the bare eye object's when the shot flies to one object, 0 when it cannot.
    uint32_t ensure_shot_eye_path(Scenario& s, uint32_t ctrl_uid);
    // Deletes one shot (the controller and the pieces only it uses: camera, start/end triggers, eye path
    // with its waypoints, focus marker). Returns how many objects went away (0: not a shot).
    int remove_cutscene_shot(Scenario& s, uint32_t controller_uid);
    // Retail world cache.
    const Wdf* retail_world(const std::string& map);
    const std::vector<std::string>* retail_layout(const Scenario& s);  // the real level's layout warnings, if s is a copy of it
    // An instance of a template in some retail world (to copy its child structure), or null.
    struct Instance { std::string map; uint32_t uid; };
    const Instance* retail_instance(uint32_t template_uid, bool prefer_mp);
    void index_instances();

    // Skies (MASky2). The catalog lists every sky model under Models/Environment/Sky in the level
    // archives, and how the retail worlds (MP maps, missions, cinemas, lobby) set each one up.
    struct SkyUse {
        std::string world;                // "Multiplay/NewRiverCity.wdf"
        int kind = 0;                     // 0 MP map, 1 mission, 2 cinema / lobby / demo
        std::string model, mtb;           // Corporal3DObj / CorporalMTB as the world spells them
        Vec3 scale{}, offset{}, pos{};    // SkyTerrainSkyScale, SkyTerrainSkyOffset, CorporalPos
        float heading_deg = 0;            // CorporalHeading
        uint32_t fog_color = 0;           // that world's MAEnvironment FogColor
        bool has_fog = false;
        uint32_t sun_color = 0;           // that world's DirectionalLightColor (a night world's sun is dark)
        bool has_sun = false;
    };
    struct SkyInfo {
        std::string name;                 // model name without _MP: "Env_SkyAurora_01"
        std::string folder;               // Models/Environment/Sky/<folder>
        std::vector<std::string> models;  // archive paths of its .MGMODEL files (plain and _MP)
        std::vector<std::string> mtbs;    // .mtb files in the folder
        std::vector<std::string> archives;
        std::vector<SkyUse> uses;
    };
    const std::vector<SkyInfo>& skies();
    const SkyInfo* find_sky(std::string_view name);  // by name, or by folder when that is unique
    std::string sky_name(const Scenario& s);          // the world's sky ("" when not in the catalog)
    std::string base_sky_name(const Scenario& s);     // the Base level's own sky
    const Wdf* base_world(const Scenario& s);         // the Base level's retail world (the MP one for an MP map), or null
    // The retail world a sky's setup is copied from: the Base world when it uses the sky, else a
    // world of the same kind (MP map / mission) with the same model variant.
    const SkyUse* sky_source(const Scenario& s, const SkyInfo& si);
    bool sky_fog(const Scenario& s, const std::string& name, uint32_t& color);
    // Whether a sky is a night sky: the sky of a night look, else the sky of a retail world whose sun is dark
    // (median DirectionalLightColor luminance of the worlds that use it under 0.36).
    bool sky_is_night(const std::string& name);
    // Points the world's MASky2 at a sky: the _MP model for MP maps when there is one, and the
    // source world's .mtb, scale, offset, position and heading. use_fog also copies its FogColor.
    // The model's files outside Base go to [Assets] on save.
    bool set_sky(Scenario& s, const std::string& name, bool use_fog, std::string* err);

    // Asset closure (files outside Base that the world and templates reference).
    // per_file (optional, same order as the result): the template or sky that first needs the file,
    // and whether the game puts it in graphics memory (textures, vertex/index buffers).
    struct AssetUse { std::string owner; bool graphics = false; uint32_t bytes = 0; };
    // The files a placed road piece needs that Base and common.mgf do not carry: the .road file and
    // its model's dependencies, appended to `out` as (archive, path). (roads.cpp)
    void road_assets(const Scenario& s, std::vector<std::pair<std::string, std::string>>& out,
                     std::vector<AssetUse>* uses, uint64_t* bytes);
    std::vector<std::pair<std::string, std::string>> asset_closure(const Scenario& s, const std::unordered_set<uint32_t>& tpl,
                                                                   uint64_t* bytes, std::vector<AssetUse>* per_file = nullptr);
    // Graphics (contiguous) memory the level needs in the game, measured 2026-10-21 on 13 runs
    // (notes about 8 + 0.88 x (texture + vertex/index MB,
    // unpacked, of the Base archive and [Assets]) of a 64 MB window; past ~63.5 MB the game crashes
    // while loading the level.
    struct GraphicsEstimate {
        double base_mb = 0, assets_mb = 0, total_mb = 0;
        std::vector<std::pair<std::string, double>> biggest;  // [Assets] owners by graphics MB, largest first
    };
    static constexpr double kGraphicsWarnMb = 48, kGraphicsErrorMb = 58;
    static constexpr size_t kMaxObjects = 3000;  // what the game loads
    GraphicsEstimate graphics_estimate(const Scenario& s);
    // Graphics MB (x 0.88) of the files one placed object of this template adds outside the Base archive and common.mgf:
    // 0 for a template the Base level already carries. Cached per (Base, uid); ms-scale on first call.
    double template_graphics_mb(const Scenario& s, uint32_t uid);
    // The Base archive's own graphics MB (x 0.88), cached per Base: the live total is 8 + this + the other levels' templates.
    double base_graphics_mb(const Scenario& s);
    // Ground plants of every retail level: each MACultureType of a world with a culture system, with its generators.
    // add_plant gives the scenario's own culture system one more layer (free bits of the culture map, the cultureBF<n> fields on
    // MATerrainInfo, a copy of the type and its generators); the plant's model files reach [Assets] through fresh_templates.
    struct PlantSource {
        std::string map, level;            // the retail world ("Mission_02") and its level archive without ".mgf"
        std::string name;                  // MACultureType Name
        int nbits = 3;                     // the layer's bits in the source map (at most 4 are used)
        uint32_t color0 = 0xFF00FF00u;     // the source layer's display colour
        std::vector<uint32_t> templates;   // each generator's GenerationTemplate (the first is the picture)
        Wdf tree;                          // the MACultureType and its generators, type first
    };
    enum class PlantResult { Added, NoCultureSystem, NoFreeBits, AlreadyHere };
    const std::vector<PlantSource>& plant_sources();
    // dry_run: only the verdict, nothing changes. new_layer: set when a culture-map layer was allocated (its bits start at 0).
    PlantResult add_plant(Scenario& s, const PlantSource& src, bool dry_run = false, bool* new_layer = nullptr);
    // Error at kGraphicsErrorMb (blocks Test), warning at kGraphicsWarnMb.
    void graphics_check(const Scenario& s, std::vector<ValidationItem>& v, double* mb = nullptr);

    GameData& gd() { return gd_; }
    TemplateCatalog& catalog() { return cat_; }

    // Templates new to the scenario's Base (placed objects whose template the Base level does not
    // already carry, and the templates they reach): their files go to [Assets].
    std::unordered_set<uint32_t> fresh_templates(const Scenario& s, const Wdf& world);
    // Points each Corporal3DObj at the exact file [Assets] ships for it (the same variant the
    // dependency collector picks: Base first, then other levels, _low and .NODE/.MGMODEL
    // alternates). `templates` = true for templates.wdf objects, false for the world. Paths that
    // exist in Base or common.mgf stay. Returns the number changed.
    int resolve_model_paths(const Scenario& s, Wdf& w, const std::unordered_set<uint32_t>& fresh, bool templates);
    // True when the template's model is on no archive of the disc in any folder or variant (cut
    // before release: the game draws a magenta placeholder, and no retail world places one).
    bool model_absent(const Scenario& s, uint32_t template_uid);
    // For such a template: a placeable template of the same class whose model is on the disc and
    // that is the same object under another name (team colour, _MP or not, arctic, "no glow"...),
    // else 0.
    uint32_t similar_template(const Scenario& s, uint32_t template_uid);
    // Units whose template model is mounted only as "<name>_low" get ForceUseLowDetail (the game
    // draws the template's path); a model that is not mounted at all becomes a warning.
    void pick_detail_levels(const Scenario& s, Wdf& w, const std::vector<std::pair<std::string, std::string>>& assets,
                            std::vector<std::string>* warnings);
    // Side of a placed unit (6 player's side, 5 enemy, 0 neutral); neutral 'Mechs and vehicles
    // become enterable (empty, with their type's owner controller).
    void apply_unit_side(Scenario& s, uint32_t uid, int nation);
    // A unit a trigger moves to a side must have a pilot. An empty unit (StartAbandoned 1) gets the retail
    // "changes side later" form: Nation unchanged (0), crewed (StartAbandoned 0, AttackPilot 1), AI asleep
    // (m_bAIEnabled 0, woken by the trigger), no owner controller. Returns true when it changed the unit.
    bool crew_for_side_change(Scenario& s, uint32_t uid);
    // Whether the player can climb into units of this template (a retail world or the templates
    // give it an owner controller), whether this placed unit has one, and adding/removing it.
    bool can_board(uint32_t template_uid);
    // What the player starts in (missions; MP maps: on foot or Battle Armor): MARoot's player
    // configuration container. kind 0 on foot, 1 Battle Armor, 2 'Mech, 3 vehicle.
    struct StartChoice { uint32_t container = 0, unit = 0; int kind = 0; std::string label; };
    const std::vector<StartChoice>& start_choices();
    // Unit templates the player can start in and their player configurations (every start
    // container MARoot references).
    std::vector<uint32_t> start_units(const Scenario& s);
    bool start_is_retail(const Scenario& s);  // MARoot's start containers are the Base level's own
    uint32_t start_container(const Scenario& s);      // the Map tab's choice (an Avatar chain shows as on foot)
    uint32_t start_container_raw(const Scenario& s);  // what the game reads: StartVehiclesGeneric for missions, the majority slot for MP maps
    void set_start_container(Scenario& s, uint32_t container);
    uint32_t start_unit_template(const Scenario& s);  // the unit the player starts in (0 = unknown)
    std::vector<uint32_t> start_templates(const Scenario& s);  // container, reference, configuration, unit
    bool boardable(const Scenario& s, uint32_t uid);
    void set_boardable(Scenario& s, uint32_t uid, bool on);

    // Water of any size (water_custom.h). A water object's size lives at the end of its
    // Description, "<name> [300 x 120 m]" (an oval, inscribed in that box: "[300 x 120 m oval]"); at save the
    // scenario's archive ships a model of that size and shape. water_name is the Description without the tag.
    static bool water_custom_size(const WObj& o, float& w, float& l);
    static wcustom::Shape water_custom_shape(const WObj& o);
    static std::string water_description(const std::string& name, float w, float l, wcustom::Shape shape = wcustom::Shape::Rect);
    static std::string water_name(const WObj& o);
    // The name somebody gave this water, or "" while it still has the type's own description
    // ("Water_Sw_1000x1000 NEW" and the like).
    static std::string water_custom_name(const WObj& o, const std::string& template_name);
    // True when the water type's model is a plain flat grid this editor can resample.
    bool water_resizable(const Scenario& s, uint32_t template_uid);
    void add_custom_water(const Scenario& s, Wdf& w, MgfWriter& mgf, std::vector<std::string>* warnings);
    // Cropped road pieces and foundations (crop.h): each object with an EditorCrop gets a cut model (a cut .road for a
    // road piece) in the scenario's archive and the world points at it; EditorCrop is removed from every object. Returns
    // what editor/project.json keeps so reopening restores the crop and the original path.
    struct CropSaved { uint32_t uid = 0; std::string rect, orig; bool road = false; };
    std::vector<CropSaved> add_cropped(const Scenario& s, Wdf& w, MgfWriter& mgf, std::vector<std::string>* warnings);
    // The save's own model test (crop::cuttable) against this scenario's archives, so the editor offers a cut only where the save can make it.
    // why = the tail of "This piece cannot be cut: its model ...". Results are cached per model path (until the base changes).
    bool model_cuttable(const Scenario& s, const std::string& model, std::string* why = nullptr);

private:
    // build / validate / save without the memory guard (the public ones catch std::bad_alloc and std::exception).
    bool build_unguarded(Scenario& s, Bytes& out, SaveReport* rep, std::string* err, bool ready);
    bool save_unguarded(Scenario& s, const fs::path& file, SaveReport* rep, std::string* err, bool force_over_limit);
    std::vector<ValidationItem> validate_unguarded(const Scenario& s);
    void ensure_deps_archives(const Scenario& s);
    void index_owner_controllers();
    void fixup_clone(Scenario& s, std::vector<WObj>& clone, const Wdf& src);
    void build_part_kinds();
    std::unordered_set<uint32_t> part_refs_;
    std::vector<PartKind> parts_;
    bool parts_ready_ = false;
    struct ArchiveSetHolder;
    std::shared_ptr<ArchiveSetHolder> deps_archives_;
    std::string deps_base_;
    std::string gfx_base_name_;                         // graphics_estimate: Base archive cached
    uint64_t gfx_base_bytes_ = 0;
    std::unordered_map<uint32_t, bool> absent_;          // model_absent cache
    std::unordered_map<std::string, std::pair<bool, std::string>> cuttable_;  // model_cuttable cache: lower-case model path (+ base) -> verdict, reason
    std::vector<StartChoice> start_choices_;
    std::unordered_map<uint32_t, uint32_t> similar_;     // similar_template cache
    GameData& gd_;
    TemplateCatalog& cat_;
    std::map<std::string, std::unique_ptr<Wdf>> worlds_;
    // Layout warnings (spawn spacing, walled-off areas) of each retail MP map, by resource archive: a same-size copy
    // does not repeat the real level's own layout as a warning (validate).
    std::map<std::string, std::vector<std::string>> retail_layout_;
    std::vector<PadKind> pads_;
    bool pads_ready_ = false;
    std::unordered_map<uint32_t, std::vector<Instance>> instances_;
    bool instances_ready_ = false;
    std::vector<PlantSource> plants_;
    bool plants_ready_ = false;
    std::vector<SkyInfo> skies_;
    bool skies_ready_ = false;
    std::unordered_map<uint32_t, Instance> oc_index_;  // unit template -> a retail owner controller
    bool oc_index_ready_ = false;
};

// ----------------------------------------------------------------------------- world helpers
namespace world {
extern const uint32_t K_ROOT, K_ENV, K_TERRAIN, K_TINFO, K_SKY, K_GROUP, K_MTG, K_LEVELSTART, K_PATHDB, K_WATER;
// Position property of an object (CorporalPos, ObjPosition, TriggerPoint, RoadPosition...).
WProp* position(WObj& o);
const WProp* position(const WObj& o);
bool get_pos(const WObj& o, Vec3& out);
void set_pos(WObj& o, Vec3 p);
float heading(const WObj& o);            // radians
// Pitch and roll in radians: CorporalPitch / CorporalRoll (degrees), or for a road piece (MARoad) the second and third
// floats of RoadRotation (the game reads RoadRotation as one rotation record: yaw, pitch, roll, like Corporal objects).
bool can_tilt(const WObj& o);
void get_tilt(const WObj& o, float& pitch, float& roll);
void set_tilt(WObj& o, float pitch, float roll);
void set_heading(WObj& o, float rad);
int find_class(const Wdf& w, uint32_t cls);
uint32_t root_uid(const Wdf& w);
uint32_t terrain_uid(const Wdf& w);
uint32_t next_uid(const Wdf& w);
// Ensures a top-level MAGroup with the given description under parent; returns its UID.
uint32_t ensure_group(Wdf& w, uint32_t parent_uid, const std::string& name, uint32_t& next);
WObj make_group(const std::string& name, uint32_t uid);
}  // namespace world

}  // namespace ma2
