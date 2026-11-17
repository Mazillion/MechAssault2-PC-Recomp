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
#include "palette_defs.h"
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

// The ranked search (palette_rank.h). Per template the text is split once and cached: `visible` = the shown name and
// the kind label, `official` = the official name and the model stem, `aliases` = the synonym words of the name and class,
// the category path and the groups it sits in.
int App::palette_rank_of(const std::string& filt, const TemplateInfo& t, const std::string& shown) const {
    if (filt.empty()) return 0;
    auto it = rank_words_.find(t.uid);
    if (it == rank_words_.end()) {
        RankText rt;
        rt.visible = shown + " " + kind_label(t.cls_name);
        rt.official = t.name + "\n" + stem_of(t.model);
        // A short key must be a whole word ("plane" is not "planet", "rock" is not "rocket"); a long,
        // distinctive one may sit inside a glued name ("Streetlight", "ParkingLot").
        const std::string glued = lower(t.name) + " " + lower(stem_of(t.model));
        std::set<std::string> words;
        for (auto& w : tokens_of(t.name)) words.insert(w);
        for (auto& w : tokens_of(stem_of(t.model))) words.insert(w);
        for (auto& w : tokens_of(t.category)) words.insert(w);
        rt.aliases = t.category + " ";
        // Effects are named after what explodes ("Explosion battle armor"): the unit synonyms would list them under
        // "soldier" / "tank"; their own name words still find them.
        for (auto& a : kNameAliases) {
            if (t.cls_name == "MAEffect") break;
            std::string key = a.frag;
            while (!key.empty() && key.back() == '_') key.pop_back();
            if (words.count(key) || (key.size() >= 7 && glued.find(key) != std::string::npos)) { rt.aliases += a.words; rt.aliases += ' '; }
        }
        for (auto& a : kClassAliases) if (t.cls_name == a.frag) { rt.aliases += a.words; rt.aliases += ' '; }
        // The names of the palette groups it sits in ("defenses" finds a turret, "natural" a tree).
        const bool special = special_category_tip(palette_category(t)) != nullptr;
        for (int g = 0; g < PG_ALL; g++)
            if (palette_in_group(t, special, g)) { rt.aliases += kPaletteGroups[g][0]; rt.aliases += ' '; }
        if (in_group(t, kGroupNatural)) { rt.aliases += "natural foliage "; }
        if (in_group(t, kGroupCity)) { rt.aliases += "city man-made "; }
        it = rank_words_.emplace(t.uid, rank_prepare(rt)).first;
    }
    return palette_rank(filt, it->second);
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
            // "Env widebridge 01 sectiontwo" reads as a file name: say it in words.
            f = lower(f);
            auto rep = [&](const char* a, const char* b) { for (size_t at = f.find(a); at != std::string::npos; at = f.find(a, at + strlen(b))) f.replace(at, strlen(a), b); };
            rep("env ", "");
            rep("widebridge", "wide bridge");
            rep("sectiontwo", "section two");
            rep("sectionone", "section one");
            rep("sectionthree", "section three");
            p.friendly = f;
            words = lower(p.path).find("bridge") != std::string::npos ? "bridge crossing span" : "";
        }
        if (!p.friendly.empty()) p.friendly[0] = char(std::toupper(uint8_t(p.friendly[0])));   // One spelling in the palette, the ghost label and the panel
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
                e.len = k < 2 ? hz : hx;  // along the edge (snap_runs' touching rule)
                e.y = p.y;
                e.piece = int(piece_obj.size());
                e.side = k;
                ends.push_back(e);
            }
            piece_obj.push_back(int(i));
            continue;
        }
        if (!object_foot(o, f) || !ma2::world::get_pos(o, p)) continue;  // a cut wall or bridge piece offers its cut ends
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
        for (size_t j = i + 1; j < ends.size(); j++) {
            bool joined = ends_meet(ends[i], ends[j]);
            // Plates also join when their edges touch along at least 2 m (flush with a corner: the middles are not together).
            if (!joined && fam == 3 && ends[i].piece != ends[j].piece && std::fabs(ends[i].ty - ends[j].ty) < 3.f)
                joined = ma2::snap::edge_overlap(ends[i].x, ends[i].z, ends[i].vx, ends[i].vz, ends[i].len, ends[j].x, ends[j].z, ends[j].vx, ends[j].vz, ends[j].len) >= 2.f;
            if (joined) up[size_t(root(ends[i].piece))] = root(ends[j].piece);
        }
    for (size_t k = 0; k < piece_obj.size(); k++) run[size_t(piece_obj[k])] = root(int(k));
    return run;
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

}  // namespace ed
