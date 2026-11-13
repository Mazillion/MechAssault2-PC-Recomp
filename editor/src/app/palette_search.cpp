// Palette extras: road pieces (prefab .road files, which have no template and so never reached the
// template-driven palette), overlapping groups ("Roads & pavement", "AI-controllable units",
// "Defensive structures"...) and hidden search aliases (file, internal and friendly names plus
// synonyms: "road" finds the 90turn piece, "soldier" finds infantry and troopers).
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

#include "app.h"
#include "../core/crop.h"
#include "imgui.h"
#include "theme.h"
#include "ui_tips.h"
#include "ui_util.h"

namespace ed {
using namespace ma2;

// ---------------------------------------------------------------------------------------------
// Words

// "Env_NOR_PrisonWall_Gate (2)" -> env nor prison wall gate 2: lower-case words, split on anything
// that is not a letter or digit, on camelCase and on letter/digit changes.
static std::vector<std::string> tokens_of(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) out.push_back(cur); cur.clear(); };
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (!std::isalnum(c)) { flush(); continue; }
        bool up = std::isupper(c) != 0, dig = std::isdigit(c) != 0;
        if (!cur.empty()) {
            unsigned char p = (unsigned char)cur.back();
            bool pdig = std::isdigit(p) != 0;
            // the camelCase test must see the original case: look at the source character before this one
            bool prev_lower = std::islower((unsigned char)s[i - 1]) != 0;
            if (dig != pdig || (up && prev_lower)) flush();
        }
        cur += char(std::tolower(c));
    }
    flush();
    return out;
}

static bool has_prefix_word(const std::vector<std::string>& toks, std::initializer_list<const char*> ws) {
    for (auto& t : toks)
        for (const char* w : ws) {
            size_t n = strlen(w);
            if (t.size() >= n && t.compare(0, n, w) == 0) return true;
        }
    return false;
}
static bool has_exact_word(const std::vector<std::string>& toks, std::initializer_list<const char*> ws) {
    for (auto& t : toks) for (const char* w : ws) if (t == w) return true;
    return false;
}
static bool contains_any(const std::string& s, std::initializer_list<const char*> ws) {
    for (const char* w : ws) if (s.find(w) != std::string::npos) return true;
    return false;
}

static std::string stem_of(const std::string& path) {
    std::string p = norm_path(path);
    size_t sl = p.rfind('/');
    std::string f = p.substr(sl == std::string::npos ? 0 : sl + 1);
    return f.substr(0, f.rfind('.'));
}

// ---------------------------------------------------------------------------------------------
// Groups (an object can be in several, and keeps its own category too)

static const char* kGroupRoads = "Roads & pavement";
static const char* kGroupAI = "AI-controllable units";
static const char* kGroupDefense = "Defensive structures";
static const char* kGroupLights = "Lights & lamps";
static const char* kGroupSigns = "Signs & banners";
static const char* kGroupPickups = "Pickups & power-ups";
static const char* kGroupDrivable = "Player-drivable";

const std::vector<std::pair<const char*, const char*>>& palette_extra_groups() {
    static const std::vector<std::pair<const char*, const char*>> g = {
        {kGroupRoads, "Road pieces to build streets from, pavement and parking lots, the concrete foundations buildings stand on, "
                      "and bridges."},
        {kGroupAI, "Everything the computer can steer through the Objectives & Triggers tab: people (civilians, troopers, infantry), "
                   "tanks and vehicles, 'Mechs and aircraft."},
        {kGroupDefense, "Walls, gates, fences, barriers, bunkers and the stationary turrets."},
        {kGroupLights, "Street lights, floodlights, search lights and light columns."},
        {kGroupSigns, "Signs, banners and billboards."},
        {kGroupPickups, "Weapon, armour and ammo pickups, salvage and bomb pods."},
        {kGroupDrivable, "'Mechs and vehicles the player can climb into (a neutral one stands empty until then)."},
    };
    return g;
}

bool App::palette_group_has(const TemplateInfo& t, const std::string& group) const {
    bool known = false;
    for (auto& g : palette_extra_groups()) if (group == g.first) known = true;
    if (!known) return false;
    // Cutscene stand-ins, wrecks and look-alikes are not game pieces.
    auto it = palette_kind_.find(t.uid);
    const std::string kind = it == palette_kind_.end() ? std::string() : it->second;
    std::string n = lower(t.name);
    if (kind == "cinematic_prop" || kind == "destroyed_husk" || t.category.find("Cinema Versions") != std::string::npos || n.find("set only") != std::string::npos ||
        n.find("nearclip") != std::string::npos)
        return false;
    const std::string& c = t.cls_name;
    auto toks = tokens_of(t.name);
    std::string glued = n + " " + lower(stem_of(t.model));
    bool unit = c == "MAPerson" || c == "MATank" || c == "MAMech" || c == "MAFlyer" || c == "MAVehicle" || c == "MAMiniSpider" ||
                c == "MASpider" || c == "MAUberBoss";
    if (group == kGroupAI) return unit;
    if (group == kGroupDrivable)
        return (c == "MAMech" || c == "MATank" || c == "MAFlyer" || c == "MAVehicle") && kind != "ai_only_actor" && kind != "pad_spawned_vehicle";
    if (group == kGroupPickups) return c == "MAPowerUp" || c == "MACrateGenerator" || lower(t.category).rfind("salvage", 0) == 0;
    if (group == kGroupDefense) {
        if (c == "MATurret") return true;
        if (contains_any(glued, {"wallvent", "wallblower", "videowall", "vidscreen", "vid screen", "climbingwall"})) return false;
        if (has_exact_word(toks, {"tower"}) && contains_any(glued, {"perimeter", "prison", "bunker", "comm"})) return true;
        return has_prefix_word(toks, {"wall", "gate", "barrier", "barricade", "fence", "guardrail", "bunker", "rampart"}) ||
               contains_any(glued, {"forest_barrier", "prisonwall", "arenawall", "arena_tallwall"});
    }
    if (group == kGroupLights) {
        if (c == "MAPointLight") return true;
        if (has_prefix_word(toks, {"flight"})) return false;
        return has_exact_word(toks, {"light", "lights", "lamp", "searchlight", "streetlight", "floodlight", "lantern", "beacon"}) ||
               contains_any(glued, {"streetlight", "searchlight", "lightcolumn", "floodlight", "highway_light"});
    }
    if (group == kGroupSigns)
        return has_exact_word(toks, {"sign", "signs", "banner", "banners", "billboard", "plaque", "poster"}) ||
               lower(t.category).find("banners") != std::string::npos || lower(t.category).find("signs") != std::string::npos;
    if (group == kGroupRoads) {
        if (c == "MATurret" || unit) return false;
        std::string cat = lower(t.category);
        if (cat.find("building base") != std::string::npos || cat.find("docks and bridges") != std::string::npos) return true;
        if (n.rfind("bb_", 0) == 0 || n.rfind("bb ", 0) == 0 || n.rfind("wst_bb", 0) == 0) return true;  // building bases = concrete foundations
        if (contains_any(glued, {"parkinglot", "asphalt", "pavement", "footing", "foundation", "sidewalk", "tarmac", "runway", "helipad",
                                 "patternbrick", "concretesquare", "hextile", "pontoon", "causeway", "vtolpad"}))
            return true;
        return has_exact_word(toks, {"road", "roads", "rd", "street", "highway", "bridge", "dock", "pier", "span"}) && !contains_any(glued, {"light", "sign"});
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Search

namespace {
struct Alias { const char* frag; const char* words; };
// Matched against the lower-case official name, the model file name and the category path.
const Alias kNameAliases[] = {
    {"trooper", "soldier infantry troops person people human foot"},
    {"infantry", "soldier trooper troops person people human foot"},
    {"elemental", "soldier infantry trooper power armor battle armor"},
    {"battlearmor", "soldier infantry trooper power armor"},
    {"power armor", "soldier infantry trooper battle armor"},
    {"pep_civ", "civilian npc pedestrian citizen person people bystander"},
    {"civfemale", "civilian npc pedestrian citizen woman person people bystander"},
    {"civmale", "civilian npc pedestrian citizen man person people bystander"},
    {"vciv", "civilian"},
    {"pep_", "person people human"},
    {"rommel", "tank armor armour ground"},
    {"manticore", "tank armor armour ground"},
    {"partisan", "tank armor armour ground"},
    {"tank", "armor armour ground vehicle"},
    {"apc", "armored personnel carrier vehicle transport ground"},
    {"truck", "vehicle transport lorry ground"},
    {"utility", "vehicle van ground"},
    {"car_", "vehicle automobile ground"},
    {"barge", "boat ship vessel vehicle water"},
    {"carrier", "ship vehicle transport"},
    {"vtol", "aircraft plane air flyer"},
    {"jet", "aircraft plane fighter air flyer"},
    {"copter", "aircraft helicopter chopper air flyer"},
    {"helicopter", "aircraft chopper copter air flyer"},
    {"dropship", "aircraft air flyer transport ship"},
    {"plane", "aircraft air flyer"},
    {"cargoplane", "aircraft plane air flyer"},
    {"turret", "gun cannon emplacement defense defence stationary"},
    {"bunker", "defense defence shelter fortification"},
    {"tree", "plant vegetation foliage forest wood"},
    {"conifer", "tree pine plant foliage forest"},
    {"mangrove", "tree plant foliage swamp"},
    {"bush", "plant vegetation foliage shrub"},
    {"grass", "plant vegetation foliage"},
    {"rock", "stone boulder cliff"},
    {"cliff", "rock stone boulder"},
    {"crystal", "gem rock ice"},
    {"wall", "barrier fence defense defence"},
    {"fence", "wall barrier"},
    {"barricade", "barrier wall block"},
    {"barrier", "wall barricade block"},
    {"gate", "door entrance entry opening"},
    {"guardrail", "barrier rail fence"},
    {"bridge", "crossing span river road"},
    {"pontoon", "bridge crossing water"},
    {"dock", "pier harbor harbour quay water"},
    {"footing", "foundation concrete base slab"},
    {"bb_", "building base foundation concrete slab pad"},
    {"parkinglot", "parking lot pavement asphalt car park"},
    {"asphalt", "pavement road tarmac ground"},
    {"patternbrick", "pavement brick paving ground"},
    {"concretesquare", "pavement concrete paving ground"},
    {"hextile", "pavement tile paving ground"},
    {"streetlight", "street light lamp lighting"},
    {"searchlight", "search light lamp spotlight lighting"},
    {"lightcolumn", "light lamp pillar lighting"},
    {"highway_light", "light lamp lighting road"},
    {"sign", "signpost board"},
    {"banner", "flag sign cloth"},
    {"billboard", "sign advertisement board"},
    {"hangar", "hangar aircraft shed building"},
    {"hanger", "hangar aircraft shed building"},
    {"warehouse", "storage building depot"},
    {"office", "building"},
    {"factory", "industrial building plant"},
    {"powerplant", "power plant generator industrial building"},
    {"generator", "power electric industrial"},
    {"pipe", "pipeline industrial"},
    {"statue", "monument sculpture"},
    {"streetlamp", "street light lamp"},
    {"antenna", "radio mast comm tower"},
    {"comm", "radio communications antenna"},
    {"radar", "dish antenna scanner"},
    {"helipad", "landing pad aircraft"},
    {"vtolpad", "landing pad aircraft helipad"},
    {"flag", "ctf objective banner"},
    {"snatch", "objective capture"},
    {"salvage", "pickup loot drop"},
    {"deadtree", "tree plant dead wood"},
    {"bomb", "pickup power-up pod"},
    {"barrel", "drum container explosive"},
    {"crate", "box container pickup"},
    {"dumpster", "trash bin container"},
    {"tower", "watchtower lookout spire turret defense defence"},
    {"barracks", "base military building housing troops"},
    {"depot", "base military storage building"},
    {"core", "base objective generator"},
    {"pilotbunker", "bunker shelter fortification base"},
    {"corebunker", "bunker shelter fortification base"},
    {"silo", "storage tank industrial building"},
    {"dome", "building structure"},
    {"ramp", "slope incline"},
    {"platform", "pad deck landing"},
    {"lamp", "light lighting"},
    {"wreck", "destroyed burnt junk ruin"},
    {"ruin", "wreck destroyed rubble"},
    {"house", "building home"},
    {"shed", "building hut storage"},
    {"tent", "shelter camp"},
    {"container", "crate box shipping"},
    {"armor", "health repair shield protection pickup"},
    {"weapon", "ammo ammunition gun upgrade pickup"},
    {"powerplant", "reactor nuclear generator energy"},
    {"generator", "reactor power energy"},
    {"digester", "fuel oil refinery storage tank industrial"},
    {"smokestack", "chimney factory industrial"},
    {"pipe", "pipeline industrial"},
    {"climbingwall", "ladder climb stairs"},
    {"rubble", "debris crater wreck ruin"},
    {"maddog", "mad dog mech player"},
    {"bloodasp", "blood asp mech player"},
    {"loki", "mech"},
    {"runway", "road airstrip landing strip"},
};
const Alias kClassAliases[] = {
    {"MAPerson", "person people human npc ai unit"},
    {"MAMech", "mech battlemech robot walker ai unit"},
    {"MATank", "tank armor armour vehicle ground ai unit"},
    {"MAVehicle", "vehicle ground ai unit jeep buggy truck car"},
    {"MAFlyer", "aircraft flyer plane air helicopter chopper vtol jet drone gunship ai unit"},
    {"MATurret", "turret gun cannon emplacement defense defence stationary ai sam aa flak antiair mortar pillbox searchlight"},
    {"MAPowerUp", "pickup power-up powerup item"},
    {"MACrateGenerator", "crate pickup box"},
    {"MAWater", "water lake river sea ocean pond"},
    {"MASky2", "sky skybox clouds"},
    {"MABuilding", "building structure"},
    {"MAMiniSpider", "spider boss enemy ai unit"},
    {"MASpider", "spider boss enemy ai unit"},
    {"MAUberBoss", "boss enemy ai unit"},
    {"MAFlagCTF", "flag ctf objective"},
    {"MASnatchItBase", "base objective"},
    {"MASnatchItDataCore", "data core objective"},
    // Glossary words (ux\glossary-units-roads.md) a newcomer types for the classes that have no such word in their names.
    {"MASpawner", "spawner reinforcements respawn spawn ai unit"},
    {"MALevelStart", "start spawn player begin"},
};
}  // namespace

// Lower-case search text of a template: shown name, official name, category path, model file name,
// class and name synonyms, and the names of the groups it belongs to.
static std::string join_words(const std::vector<std::string>& t) {
    std::string s;
    for (auto& w : t) { s += w; s += ' '; }
    return s;
}

bool App::palette_search_match(const std::string& filt, const TemplateInfo& t, const std::string& shown) const {
    if (filt.empty()) return true;
    auto it = search_text_.find(t.uid);
    if (it == search_text_.end()) {
        std::string s = lower(shown) + " " + lower(t.name) + " " + lower(t.category) + " " + lower(stem_of(t.model)) + " ";
        s += join_words(tokens_of(t.name)) + join_words(tokens_of(stem_of(t.model))) + " ";
        // A short key must be a whole word ("plane" is not "planet", "rock" is not "rocket"); a long,
        // distinctive one may sit inside a glued name ("Streetlight", "ParkingLot").
        std::string glued = lower(t.name) + " " + lower(stem_of(t.model));
        std::set<std::string> words;
        for (auto& w : tokens_of(t.name)) words.insert(w);
        for (auto& w : tokens_of(stem_of(t.model))) words.insert(w);
        for (auto& w : tokens_of(t.category)) words.insert(w);
        // Effects are named after what explodes ("Explosion battle armor"): the unit synonyms would list them under
        // "soldier" / "tank"; their own name words still find them.
        for (auto& a : kNameAliases) {
            if (t.cls_name == "MAEffect") break;
            std::string key = a.frag;
            while (!key.empty() && key.back() == '_') key.pop_back();
            bool hit = words.count(key) || (key.size() >= 7 && glued.find(key) != std::string::npos);
            if (hit) { s += a.words; s += ' '; }
        }
        for (auto& a : kClassAliases) if (t.cls_name == a.frag) { s += a.words; s += ' '; }
        it = search_text_.emplace(t.uid, std::move(s)).first;
    }
    // Every word typed must be found.
    size_t pos = 0;
    while (pos < filt.size()) {
        while (pos < filt.size() && filt[pos] == ' ') pos++;
        size_t e = filt.find(' ', pos);
        std::string w = filt.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (!w.empty() && it->second.find(w) == std::string::npos) return false;
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Road pieces

namespace {
struct RoadName { const char* stem; const char* friendly; const char* words; };
const RoadName kRoadNames[] = {
    {"90turn", "90-degree turn", "turn corner bend right angle L elbow"},
    {"shortstraight", "short straight (30 m)", "straight segment piece"},
    {"longstraight", "long straight (60 m)", "straight segment piece"},
    {"shortcurve", "short curve", "curve bend turn corner"},
    {"shorttee", "short T-junction", "tee t junction intersection fork three way"},
    {"intersection", "crossroads", "cross four way junction intersection"},
    {"end", "road end cap", "end cap dead end terminator"},
    {"straight15xroot2over2", "short straight (10.6 m)", "straight segment piece"},
    {"straight15xroot2", "straight (21 m)", "straight segment piece"},
    {"straight15", "straight (15 m)", "straight segment piece"},
    {"straight5", "straight (5 m)", "straight segment piece"},
    {"up10deg", "long straight (60 m; the game's name is up10deg, not tilted)", "straight"},
    {"down10deg", "long straight (60 m; the game's name is down10deg, not tilted)", "straight"},
    {"runway_straight", "runway, straight (350 m)", "airstrip airfield tarmac airport landing strip straight"},
    {"runway_approach", "runway approach (130 m)", "airstrip airfield tarmac airport landing strip taper"},
    {"runway_endzone", "runway end zone (90 m)", "airstrip airfield tarmac airport landing strip end"},
};
}  // namespace

const std::vector<App::RoadPiece>& App::road_pieces() {
    if (road_ready_) return road_pieces_;
    road_ready_ = true;
    std::map<std::string, RoadPiece> by;
    auto scan = [&](int index, ma2::MgfReader* r) {
        if (!r) return;
        for (auto& f : r->files()) {
            if (!iends_with(f.path, ".road")) continue;
            RoadPiece& p = by[lower(norm_path(f.path))];
            if (p.path.empty()) {
                p.path = f.path;
                for (char& c : p.path) if (c == '\\') c = '/';
                p.official = p.path.substr(p.path.rfind('/') + 1);
                Bytes b;
                if (r->read_file(f.path, b)) {
                    float p1[3] = {13, 0, 0}, p2[3] = {-13, 0, -30};
                    int seen = 0;
                    std::vector<std::array<float, 3>> att_pts, att_vecs;  // [attachments]: point / vector pairs, entry first
                    for (auto& line : split(std::string(b.begin(), b.end()), '\n')) {
                        std::string l = trim(line.substr(0, line.find(';')));
                        size_t eq = l.find('=');
                        if (eq == std::string::npos) continue;
                        std::string k = lower(trim(l.substr(0, eq))), val = trim(l.substr(eq + 1));
                        if (k == "model") p.model = ma2m::ResolveRelative(p.path, val);
                        else if (k == "point1" && sscanf(val.c_str(), "%f, %f, %f", &p1[0], &p1[1], &p1[2]) == 3) seen |= 1;
                        else if (k == "point2" && sscanf(val.c_str(), "%f, %f, %f", &p2[0], &p2[1], &p2[2]) == 3) seen |= 2;
                        else if (k == "point" || k == "vector") {
                            std::array<float, 3> v{};
                            if (sscanf(val.c_str(), "%f, %f, %f", &v[0], &v[1], &v[2]) == 3) (k == "point" ? att_pts : att_vecs).push_back(v);
                        }
                    }
                    if (seen == 3) { p.w = std::max(1.f, std::fabs(p1[0] - p2[0]) * 0.5f); p.l = std::max(1.f, std::fabs(p1[2] - p2[2])); }
                    for (size_t i = 1; i < att_pts.size() && i < att_vecs.size(); i++)
                        p.exits.push_back({att_pts[i][0], att_pts[i][2], att_vecs[i][0], att_vecs[i][2]});
                }
            }
            p.archives.push_back(index);
        }
    };
    scan(-1, &gd_.common());
    for (size_t i = 0; i < gd_.level_archives().size(); i++) scan(int(i), gd_.archive(gd_.level_archives()[i]));
    for (auto& [k, p] : by) {
        std::string stem = lower(stem_of(p.path));
        std::string words;
        for (auto& n : kRoadNames)
            if (stem == n.stem) { p.friendly = n.friendly; words = n.words; break; }
        if (p.friendly.empty()) {
            // Anything else (the wide-bridge section): its words, spaced.
            std::string f = join_words(tokens_of(stem_of(p.path)));
            if (!f.empty()) f.pop_back();
            p.friendly = f;
            words = lower(p.path).find("bridge") != std::string::npos ? "bridge crossing span" : "";
        }
        p.search = "road piece street pavement asphalt path lane tile prefab " + lower(p.path) + " " + lower(p.official) + " " +
                   lower(p.friendly) + " " + join_words(tokens_of(stem_of(p.path))) + " " + words;
        road_pieces_.push_back(p);
    }
    // Country roads first (the ones retail cities use), then the rest, each by name.
    std::sort(road_pieces_.begin(), road_pieces_.end(), [](const RoadPiece& a, const RoadPiece& b) {
        bool ca = lower(a.path).rfind("roads/country/", 0) == 0, cb = lower(b.path).rfind("roads/country/", 0) == 0;
        if (ca != cb) return ca;
        return lower(a.path) < lower(b.path);
    });
    return road_pieces_;
}

const App::RoadPiece* App::find_road_piece(const std::string& path) {
    for (auto& p : road_pieces()) if (iequals(p.path, path)) return &p;
    return nullptr;
}

// End-to-end snapping. Every .road file lists where the next piece attaches ([attachments]: its entry
// at the origin facing +z, then each exit with an outward direction; confirmed by the research).
// Near a free exit of a placed road, the new piece's entry goes on that exit and it turns to face on.
// World = Ry(yaw) * local, Ry(t) maps +z to (sin t, cos t), as the placement preview draws it.
bool App::road_snap(const Vec3& cursor, Vec3& out_pos, float& out_yaw) {
    if (!doc_ || place_road_.empty()) return false;
    static const uint32_t K_ROAD_ = key_of("MARoad");
    struct Cand { float x, z, vx, vz; int piece; float y = 0; bool tilted = false; };
    // Every end of every placed piece, its entry included (a road can be extended at either end). An end is open
    // when no end of another piece sits on it.
    std::vector<Cand> ends;
    std::vector<bool> chained;  // per end: it belongs to the piece placed last (the road being built)
    int piece = 0;
    for (auto& o : doc_->world.objs) {
        if (o.cls != K_ROAD_) continue;
        Vec3 p;
        if (!ma2::world::get_pos(o, p)) continue;
        std::string def = o.str("RoadDefinition");
        for (char& ch : def) if (ch == 0x5C) ch = '/';
        const RoadPiece* rp = find_road_piece(def);
        if (!rp) continue;
        size_t first_end = ends.size();
        // A tilted piece (playtest 11-04): its ends in 3D, so the next piece joins its real end, at its height.
        if (float tp, tr; (ma2::world::get_tilt(o, tp, tr), std::fabs(tp) + std::fabs(tr) > 1e-4f)) {
            std::vector<Joint> js;
            if (object_joints(o, js)) {
                for (auto& j : js) {
                    Cand k;
                    k.x = j.p.x; k.z = j.p.z; k.vx = j.ox; k.vz = j.oz; k.piece = piece; k.y = j.p.y; k.tilted = true;
                    ends.push_back(k);
                }
                chained.resize(ends.size(), false);
                if (o.uid() == road_chain_uid_)
                    for (size_t i = first_end; i < ends.size(); i++) chained[i] = true;
                piece++;
                continue;
            }
        }
        float yaw = 0;
        if (const WProp* r = o.find("RoadRotation")) if (r->data.size() >= 4) memcpy(&yaw, r->data.data(), 4);
        float c = std::cos(yaw), s = std::sin(yaw);
        auto to_world = [&](float lx, float lz, float& wx, float& wz) { wx = c * lx + s * lz; wz = -s * lx + c * lz; };
        std::vector<RoadPiece::Exit> ex{{0, 0, 0, 1}};  // the entry: local 0,0, facing +z (the piece runs along -z)
        if (rp->exits.empty()) ex.push_back({0, -rp->l, 0, -1});  // no attachment list: a straight piece
        else ex.insert(ex.end(), rp->exits.begin(), rp->exits.end());
        // A cropped piece (app_crop.cpp): its ends move onto the cut edges, as the saved .road's attachments do (crop.cpp).
        if (ma2::crop::Rect cr; ma2::crop::of(o, cr))
            for (auto& e : ex) ma2::crop::clamp_point(cr, e.x, e.z);
        for (auto& e : ex) {
            Cand k;
            float wx, wz;
            to_world(e.x, e.z, wx, wz);
            k.x = p.x + wx;
            k.z = p.z + wz;
            to_world(e.vx, e.vz, k.vx, k.vz);
            k.piece = piece;
            ends.push_back(k);
        }
        chained.resize(ends.size(), false);
        if (o.uid() == road_chain_uid_)
            for (size_t i = first_end; i < ends.size(); i++) chained[i] = true;
        piece++;
    }
    // How close the cursor must be to an open end: 30 m, or 90 m for the piece placed last, so the road being
    // built keeps growing from its end (chain) while the cursor wanders ahead of it.
    float best = 1e9f;
    const Cand* hit = nullptr;
    road_open_ends_.clear();
    int hit_end = -1;
    for (size_t i = 0; i < ends.size(); i++) {
        const Cand& k = ends[i];
        bool taken = false;
        for (auto& o2 : ends)
            if (o2.piece != k.piece && std::fabs(o2.x - k.x) < 1.5f && std::fabs(o2.z - k.z) < 1.5f) { taken = true; break; }
        if (taken) continue;
        road_open_ends_.push_back({k.x, k.z, k.vx, k.vz, false});
        float d = std::hypot(cursor.x - k.x, cursor.z - k.z);
        if (d > (chained[i] ? 90.f : 30.f)) continue;
        if (chained[i]) d -= 60.f;  // the road being built wins over other open ends in reach
        if (d < best) { best = d; hit = &k; hit_end = int(road_open_ends_.size()) - 1; }
    }
    if (!hit) return false;
    road_open_ends_[size_t(hit_end)].joined = true;
    road_snap_y_ = hit->tilted ? hit->y : NAN;  // joined to a tilted piece: at its end's height, not on the ground
    // The piece joins by one of its own ends: its entry (local 0,0 facing +z) or one of its exits (road_attach_, Q / E).
    // Its yaw turns that end's outward direction onto minus the open end's, and the piece is moved so the two meet.
    const RoadPiece* np = find_road_piece(place_road_);
    std::vector<RoadPiece::Exit> mine{{0, 0, 0, 1}};
    if (np) {
        if (np->exits.empty()) mine.push_back({0, -np->l, 0, -1});
        else mine.insert(mine.end(), np->exits.begin(), np->exits.end());
    }
    int n = int(mine.size());
    const RoadPiece::Exit& a = mine[size_t(((road_attach_ % n) + n) % n)];
    float yaw = std::atan2(-hit->vx, -hit->vz) - std::atan2(a.vx, a.vz);
    float c = std::cos(yaw), s = std::sin(yaw);
    out_pos = {hit->x - (c * a.x + s * a.z), cursor.y, hit->z - (-s * a.x + c * a.z)};
    out_yaw = yaw;
    return true;
}

// Wall and bridge pieces join end to end like road pieces (playtest 10-26: snap bridge pieces and walls too).
// Family 1 = walls, fences, gates, barriers, barricades, guardrails (the linear Defenses pieces); 2 = bridge
// pieces and docks (the Bridges button, not the .road bridge sections, which snap as roads). 0 = places freely.
int App::snap_family(const TemplateInfo& t) const {
    static std::unordered_map<uint32_t, int> cache;
    auto it = cache.find(t.uid);
    if (it != cache.end()) return it->second;
    int f = 0;
    auto kit = palette_kind_.find(t.uid);
    const std::string kind = kit == palette_kind_.end() ? std::string() : kit->second;
    const std::string& c = t.cls_name;
    std::string n = lower(t.name), glued = n + " " + lower(stem_of(t.model));
    bool unit = c == "MAPerson" || c == "MATank" || c == "MAMech" || c == "MAFlyer" || c == "MAVehicle" || c == "MATurret" || c == "MAEffect" ||
                c == "MAPowerUp" || c == "MAWater";
    bool cine = kind == "cinematic_prop" || kind == "destroyed_husk" || t.category.find("Cinema Versions") != std::string::npos ||
                n.find("set only") != std::string::npos || n.find("nearclip") != std::string::npos;
    if (!unit && !cine && !t.model.empty()) {
        auto toks = tokens_of(t.name);
        if (is_foundation(t)) f = 3;  // playtest 11-01: foundations snap too (edge to edge, see snap_ends)
        else if (lower(t.category).find("docks and bridges") != std::string::npos || glued.find("bridge") != std::string::npos) f = 2;
        else if (!contains_any(glued, {"wallvent", "wallblower", "videowall", "vidscreen", "vid screen", "climbingwall"}) &&
                 (has_prefix_word(toks, {"wall", "gate", "barrier", "barricade", "fence", "guardrail"}) ||
                  contains_any(glued, {"forest_barrier", "prisonwall", "arenawall", "arena_tallwall"})))
            f = 1;
    }
    cache[t.uid] = f;
    return f;
}

bool App::snap_foot(const TemplateInfo& t, SnapFoot& f) {
    GpuModel* gm = models_.get(t.model, t.mtb);
    if (!gm || gm->state != GpuModel::Ready) return false;
    float hx = 0.5f * (gm->bmax[0] - gm->bmin[0]), hz = 0.5f * (gm->bmax[2] - gm->bmin[2]);
    f = {0.5f * (gm->bmin[0] + gm->bmax[0]), 0.5f * (gm->bmin[2] + gm->bmax[2]), std::max(hx, hz), hx >= hz};
    return f.half > 0.25f;
}

void App::snap_ends(int fam, std::vector<SnapEnd>& ends, std::vector<int>& piece_obj) {
    ends.clear();
    piece_obj.clear();
    for (size_t i = 0; i < doc_->world.objs.size(); i++) {
        const WObj& o = doc_->world.objs[i];
        const TemplateInfo* t = cat_->find(o.template_id());
        if (!t || snap_family(*t) != fam) continue;
        SnapFoot f;
        Vec3 p;
        if (fam == 3) {
            // Foundations (square plates) join on all four edges: each edge's middle (on the plate's top) is an end, facing
            // out of the plate. The footprint is the drawn one, so a cropped plate offers its cut edges; a tilted plate (a
            // ramp) offers its real edges, so a plate joined to its top edge sits on the top (object_joints, app_glue.cpp).
            GpuModel* gm = obj_model(o);
            std::vector<Joint> js;
            if (!gm || gm->state != GpuModel::Ready || !ma2::world::get_pos(o, p) || !object_joints(o, js) || js.size() != 4) continue;
            const float yaw = ma2::world::heading(o);
            const float hx = 0.5f * (gm->bmax[0] - gm->bmin[0]), hz = 0.5f * (gm->bmax[2] - gm->bmin[2]);
            if (hx < 0.25f || hz < 0.25f) continue;
            for (int k = 0; k < 4; k++) {
                SnapEnd e;
                e.x = js[size_t(k)].p.x;
                e.z = js[size_t(k)].p.z;
                e.ty = js[size_t(k)].p.y;
                e.vx = js[size_t(k)].ox;
                e.vz = js[size_t(k)].oz;
                e.axis = yaw;  // a joined plate takes this one's heading, so the two grids line up
                e.half = k < 2 ? hx : hz;
                e.y = p.y;
                e.piece = int(piece_obj.size());
                e.side = k;
                ends.push_back(e);
            }
            piece_obj.push_back(int(i));
            continue;
        }
        if (!snap_foot(*t, f) || !ma2::world::get_pos(o, p)) continue;
        const float yaw = ma2::world::heading(o), c = std::cos(yaw), s = std::sin(yaw);
        auto to_world = [&](float lx, float lz, float& wx, float& wz) { wx = c * lx + s * lz; wz = -s * lx + c * lz; };
        float cx, cz;
        to_world(f.cx, f.cz, cx, cz);
        for (float sgn : {1.f, -1.f}) {
            SnapEnd e;
            float lx = f.along_x ? sgn : 0, lz = f.along_x ? 0 : sgn;
            to_world(lx, lz, e.vx, e.vz);
            e.x = p.x + cx + e.vx * f.half;
            e.z = p.z + cz + e.vz * f.half;
            e.axis = std::atan2(e.vx, e.vz) + (sgn < 0 ? 3.14159265f : 0.f);
            e.half = f.half;
            e.y = e.ty = p.y;
            e.piece = int(piece_obj.size());
            ends.push_back(e);
        }
        piece_obj.push_back(int(i));
    }
}

// Two ends of different pieces meet: overlapped retail joins sit up to ~5 m apart.
bool App::ends_meet(const SnapEnd& a, const SnapEnd& b) {
    const float tol = std::max(1.5f, 0.2f * std::min(a.half, b.half));
    return a.piece != b.piece && std::fabs(a.x - b.x) < tol && std::fabs(a.z - b.z) < tol && std::fabs(a.ty - b.ty) < std::max(tol, 3.f);
}

std::vector<int> App::snap_runs(int fam) {
    std::vector<int> run(doc_ ? doc_->world.objs.size() : 0, -1);
    if (!doc_ || !cat_) return run;
    std::vector<SnapEnd> ends;
    std::vector<int> piece_obj;
    snap_ends(fam, ends, piece_obj);
    std::vector<int> up(piece_obj.size());
    for (size_t i = 0; i < up.size(); i++) up[i] = int(i);
    auto root = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
    for (size_t i = 0; i < ends.size(); i++)
        for (size_t j = i + 1; j < ends.size(); j++)
            if (ends_meet(ends[i], ends[j])) up[size_t(root(ends[i].piece))] = root(ends[j].piece);
    for (size_t k = 0; k < piece_obj.size(); k++) run[size_t(piece_obj[k])] = root(int(k));
    return run;
}

// The piece being placed (place_template_) near an open end of a placed piece of its own family: it lines up with
// that piece, end to end. Join points are the two ends of the model's long horizontal footprint axis (centre of each
// short face); an end is open when no other piece's end is within 1.5 m (a fifth of the shorter piece's half length for big ones). Q / E (snap_turn_) turn a wall corner by
// 90 deg. out_pos is the footprint centre (the placing circle's spot), out_yaw the heading. Fills road_open_ends_.
bool App::piece_snap(const Vec3& cursor, Vec3& out_pos, float& out_yaw) {
    if (!doc_ || !cat_ || !place_road_.empty()) return false;
    const TemplateInfo* nt = cat_->find(place_template_);
    const int fam = nt ? snap_family(*nt) : 0;
    if (!fam) return false;
    SnapFoot mine;
    if (!snap_foot(*nt, mine)) return false;
    std::vector<SnapEnd> ends;
    std::vector<int> piece_obj;
    snap_ends(fam, ends, piece_obj);
    // A new object gets the next free uid: the newest piece is the run being built.
    int last_piece = -1;
    uint32_t last_uid = 0;
    for (size_t k = 0; k < piece_obj.size(); k++)
        if (uint32_t u = doc_->world.objs[size_t(piece_obj[k])].uid(); u > last_uid) { last_uid = u; last_piece = int(k); }
    float best = 1e9f;
    const SnapEnd* hit = nullptr;
    int hit_end = -1;
    road_open_ends_.clear();
    for (const SnapEnd& k : ends) {
        bool taken = false;
        for (auto& o2 : ends)
            if (ends_meet(k, o2)) { taken = true; break; }
        if (taken) continue;
        road_open_ends_.push_back({k.x, k.z, k.vx, k.vz, false});
        if (fam == 3) {
            // A foundation goes to the joined spot nearest the cursor: the distance is from the cursor to where the plate's
            // centre would land, with no bonus for the last plate placed (that bonus made a 2x2 tile's third plate jump to the
            // second plate's far edge, playtest 11-04). Reach: anywhere over the ghost, plus 20 m.
            GpuModel* gm = models_.get(nt->model, nt->mtb);
            const float r = k.side < 2 ? 0.5f * (gm->bmax[0] - gm->bmin[0]) : 0.5f * (gm->bmax[2] - gm->bmin[2]);
            const float d = std::hypot(cursor.x - (k.x + k.vx * r), cursor.z - (k.z + k.vz * r));
            if (d > mine.half + 20.f) continue;
            if (d < best) { best = d; hit = &k; hit_end = int(road_open_ends_.size()) - 1; }
            continue;
        }
        const bool chained = k.piece == last_piece;
        // Reach is measured from the ghost's near end, not its centre (the cursor): an 85 m bridge piece's centre sits 43 m
        // past the end it joins, so a 30 m reach from the cursor never caught it (game check 10-26).
        float d = std::hypot(cursor.x - k.x, cursor.z - k.z) - mine.half;  // order kept: the nearest end still wins
        if (d > (chained ? 90.f : 30.f)) continue;
        if (chained) d -= 60.f;
        if (d < best) { best = d; hit = &k; hit_end = int(road_open_ends_.size()) - 1; }
    }
    if (!hit) return false;
    road_open_ends_[size_t(hit_end)].joined = true;
    if (fam == 3) {
        // A foundation lines up edge to edge with the one it joins: same heading, its own edge on that edge, the two edge
        // middles together. Its centre sits its own half size (across that edge) out from the edge.
        GpuModel* gm = models_.get(nt->model, nt->mtb);
        const float reach = hit->side < 2 ? 0.5f * (gm->bmax[0] - gm->bmin[0]) : 0.5f * (gm->bmax[2] - gm->bmin[2]);
        out_yaw = hit->axis;
        out_pos = {hit->x + hit->vx * reach, cursor.y, hit->z + hit->vz * reach};
        snap_origin_y_ = hit->ty - gm->bmax[1];  // its top on the edge's top (the top of a ramp, for a tilted neighbour)
        return true;
    }
    // The run continues along the open end's direction, or turns a quarter left / right at a wall corner (Q / E).
    int turn = fam == 1 ? ((snap_turn_ % 4) + 4) % 4 : 0;
    if (turn == 2) turn = 0;  // straight back onto itself is no corner
    const float ang = std::atan2(hit->vx, hit->vz) + turn * 1.5707963f;
    const float ex = std::sin(ang), ez = std::cos(ang);
    // The new piece's long axis lies along the run with the same heading as the piece it joins (retail runs never flip a
    // piece: research 10-26, ~2,000 neighbour pairs); its centre sits half a length past the join, less the overlap
    // retail uses for a few models (spacing / length: LargeWall 0.94, TallWall 0.91, WideBridge02/04 0.97; the rest butt).
    out_yaw = hit->axis + turn * 1.5707963f - (mine.along_x ? 1.5707963f : 0.f);
    const std::string stem = lower(stem_of(nt->model));
    float ratio = 1.f;
    if (stem.find("largewall_span") != std::string::npos) ratio = 0.94f;
    else if (stem.find("tallwall_span") != std::string::npos) ratio = 0.91f;
    else if (stem.find("widebridge02") != std::string::npos || stem.find("widebridge04") != std::string::npos) ratio = 0.97f;
    const float reach = mine.half - (turn ? 0.f : (1.f - ratio) * 2.f * mine.half);
    out_pos = {hit->x + ex * reach, cursor.y, hit->z + ez * reach};
    snap_origin_y_ = hit->y;  // retail neighbours share one height: a run on a slope does not step (game check 10-26)
    return true;
}

// A bridge piece (snap family 2) placed on its own stands with its pillar bases (the model's lowest point) on the clicked
// surface, so it can be lowered by hand or built from a hilltop across a valley (playtest 10-26). Measured at the
// click, not under the origin: Wide bridge 02's origin is at one END, 43 m from the middle, so on a slope it sank or
// floated. The bottom is the lower of the file's box and the drawn meshes (Wide bridge 01 section two's box stops
// ~12 m short of its pillars, and it sank that far).
bool App::bridge_foot_height(const Vec3& click, float& origin_y) {
    if (!doc_ || !cat_ || !place_road_.empty()) return false;
    const TemplateInfo* t = cat_->find(place_template_);
    if (!t || snap_family(*t) != 2) return false;
    GpuModel* gm = models_.get(t->model, t->mtb);
    if (!gm || gm->state != GpuModel::Ready) return false;
    const float bottom = gm->has_vbox ? std::min(gm->bmin[1], gm->vmin[1]) : gm->bmin[1];
    origin_y = click.y - bottom;
    return true;
}

// Road piece rows of the Units list. Returns how many were shown.
int App::palette_road_rows(const std::string& filt, const std::string& category, bool base_only, int base_idx) {
    if (!category.empty() && category != kGroupRoads) return 0;
    int shown = 0;
    float grid_x = 0;
    for (auto& p : road_pieces()) {
        // up10deg / down10deg are the 60 m straight under another name (same model, flat ends; no retail piece is tilted):
        // they only confused (playtest 11-04). Retail maps that use them still load and draw.
        if (lower(p.path).find("10deg.road") != std::string::npos) continue;
        if (base_only && std::find(p.archives.begin(), p.archives.end(), base_idx) == p.archives.end() &&
            std::find(p.archives.begin(), p.archives.end(), -1) == p.archives.end())
            continue;
        // Every word typed must be found.
        bool ok = true;
        size_t pos = 0;
        while (ok && pos < filt.size()) {
            while (pos < filt.size() && filt[pos] == ' ') pos++;
            size_t e = filt.find(' ', pos);
            std::string w = filt.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            if (!w.empty() && p.search.find(w) == std::string::npos) ok = false;
            if (e == std::string::npos) break;
            pos = e + 1;
        }
        if (!ok) continue;
        // The row says what the piece is; the file name ("longStraight.road") stays in the tooltip.
        std::string name = p.friendly;
        if (!name.empty()) name[0] = char(std::toupper(uint8_t(name[0])));
        ImGui::PushID(p.path.c_str());
        bool sel = tool_ == Tool::PlaceObject && iequals(place_road_, p.path);
        if (palette_grid_) {
            // Picture cell, as for the other objects (tab_units).
            const ImGuiStyle& st = ImGui::GetStyle();
            float cell = 76 * dpi_, cw = cell + st.FramePadding.x * 2;
            if (grid_x > 0 && grid_x + st.ItemSpacing.x + cw <= ImGui::GetContentRegionAvail().x) { ImGui::SameLine(); grid_x += st.ItemSpacing.x + cw; }
            else grid_x = cw;
            ID3D11ShaderResourceView* th = models_.thumbnail_cached(p.model, "") || palette_thumb_budget_-- > 0 ? models_.thumbnail(p.model, "") : nullptr;
            ImGui::BeginGroup();
            if (sel) theme::push_toggle_on();
            bool clicked = th ? ImGui::ImageButton("##pic", (ImTextureID)(intptr_t)th, ImVec2(cell, cell))
                              : ImGui::Button("...", ImVec2(cw, cell + st.FramePadding.y * 2));
            if (sel) theme::pop_toggle_on();
            ImGui::TextUnformatted(palette_cell_name(p.friendly, cw).c_str());
            ImGui::EndGroup();
            if (clicked) {
                place_road_ = p.path;
                place_template_ = 0;
                tool_ = Tool::PlaceObject;
            }
            if (ImGui::BeginItemTooltip()) {
                ImGui::Text("Road: %s", p.friendly.c_str());
                ImGui::Text("About %.0f m long, %.0f m wide. The next piece joins the open end of the last one; Q / E turn it.", p.l, p.w * 2);
                ImGui::TextDisabled("Game name: %s", p.path.c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
            shown++;
            continue;
        }
        // Same row layout as the other objects (tick box first); a road piece cannot join the scatter brush, so the box is greyed.
        {
            bool off = false;
            ImGui::BeginDisabled();
            ImGui::Checkbox("##sc", &off);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Road pieces are laid one after another with the mouse; they cannot be used in the scatter brush");
            ImGui::SameLine();
        }
        if (ImGui::Selectable((name + "  (road piece)").c_str(), sel)) {
            place_road_ = p.path;
            place_template_ = 0;
            tool_ = Tool::PlaceObject;
        }
        if (ImGui::BeginItemTooltip()) {
            if (ID3D11ShaderResourceView* th = models_.thumbnail(p.model, "")) ImGui::Image((ImTextureID)(intptr_t)th, ImVec2(128 * dpi_, 128 * dpi_));
            ImGui::Text("Road: %s", p.friendly.c_str());
            ImGui::Text("About %.0f m long, %.0f m wide. The next piece joins the open end of the last one; Q / E turn it.", p.l, p.w * 2);
            ImGui::TextDisabled("Game name: %s", p.path.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
        shown++;
    }
    return shown;
}

}  // namespace ed
