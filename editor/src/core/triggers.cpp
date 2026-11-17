#include "triggers.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "cutscene.h"
#include "gamedata.h"
#include "scenario.h"

namespace ma2 {

static int free_objective_var(const Wdf& w, const WObj& r, int i);

const char* kEditorTriggerGroup = "Editor Triggers";
const char* kEditorPrefix = "[ed] ";
const char* kReachMarkerSuffix = " (radar marker)";
const char* kTriggerShapeGroup = "Trigger Volumes";
const char* kAIPathGroup = "AI Paths";

using namespace world;

// ----------------------------------------------------------------------------- kinds

const std::vector<TriggerKind>& condition_kinds() {
    static const std::vector<TriggerKind> v = {
        {"start", "When the mission starts", {}},
        {"timer", "After a delay", {{"seconds", "Seconds (counted from when this trigger is switched on)", "float", "10"}}},
        // The shape is a real MATriggerRadius / MATriggerCuboidalVolume on the map ("vol" = its UID).
        {"area", "Player is in a trigger circle", {{"vol", "Trigger circle", "shape", ""}}},
        {"box", "Player is in a trigger box (volume)", {{"vol", "Trigger box", "shape", ""}}},
        {"destroyed", "Units are destroyed", {{"units", "Units", "units", ""}, {"all", "All of them (untick: any one)", "bool", "1"},
                                          {"jacking", "Stealing a unit counts too", "bool", "1"}}},
        {"damaged", "A unit is damaged", {{"units", "Units", "units", ""}, {"percent", "Fires at health left (%)", "int", "50"}}},
        {"trigger", "Another trigger has fired", {{"trigger", "Trigger", "trigger", ""}}},
        {"not_trigger", "Another trigger has not fired", {{"trigger", "Trigger", "trigger", ""}}},
    };
    return v;
}

const std::vector<TriggerKind>& effect_kinds() {
    static const std::vector<TriggerKind> v = {
        {"message", "Show a message", {{"text", "Text", "text", ""}, {"caption", "Speaker (optional)", "text", ""},
                                        {"seconds", "Seconds on screen (0 = until closed)", "float", "6"}, {"pause", "Pause the game while it shows", "bool", "0"}}},
        {"voice", "Play a voice or sound from the game", {{"cue", "Sound", "cue", ""}}},
        {"audio", "Play your own sound clip", {{"clip", "Clip", "clip", "1"}}},
        {"music", "Change the music", {{"cue", "Music", "music", ""}, {"intensity", "Intensity", "int", "3000"}}},
        {"objective", "Complete objective", {{"objective", "Objective", "objective", "0"}}},
        {"win", "Win the mission", {}},
        {"lose", "Lose the mission", {}},
        {"show", "Show a hidden unit", {{"unit", "Unit", "unit", ""}}},
        {"hide", "Hide a unit", {{"unit", "Unit", "unit", ""}}},
        {"remove", "Remove a unit", {{"unit", "Unit", "unit", ""}, {"effect", "With an explosion", "bool", "0"}}},
        {"ai", "Give a unit orders", {{"unit", "Unit", "unit", ""}, {"order", "Order", "order", "patrol"}, {"path", "Patrol path", "path", ""},
                            {"point", "Defend point", "point", ""}, {"radius", "Radius (m)", "float", "60"},
                            {"target", "Attack or follow (none = the player)", "unit", ""}}},
        {"enable_unit", "Switch a unit on or off", {{"unit", "Unit", "unit", ""}, {"on", "Switch on (untick: off)", "bool", "1"}}},
        {"cinema", "Play a cutscene", {{"cinema", "Cutscene", "cinema", ""}}},
        {"teleport", "Move the player to a point", {{"point", "Destination", "point", ""}, {"heading", "Facing (degrees)", "float", "0"}}},
        {"nation", "Change a unit's side", {{"unit", "Unit", "unit", ""}, {"nation", "Side", "side", "5"}}},
        {"reinforce", "Bring in reinforcements", {{"units", "Units (placed on the map; hidden and asleep until this fires)", "units", ""},
                                                  {"nation", "They arrive on the side of", "side", "5"},
                                                  {"hide", "Hide them when the mission starts", "bool", "1"}}},
        {"checkpoint", "Save a checkpoint", {{"name", "Name", "string", "cp1"}}},
        {"variable", "Set a mission variable (advanced)", {{"var", "Variable (0-29)", "int", "20"}, {"state", "State", "int", "1"}}},
        {"trigger_on", "Switch another trigger on", {{"trigger", "Trigger", "trigger", ""}}},
        {"trigger_off", "Switch another trigger off", {{"trigger", "Trigger", "trigger", ""}}},
    };
    return v;
}

float trigger_box_base(const std::map<std::string, std::string>& a) {
    auto num = [&](const char* k, float d) { auto it = a.find(k); return it == a.end() || it->second.empty() ? d : float(atof(it->second.c_str())); };
    float h = num("height", 60);
    auto fl = a.find("flat"), ba = a.find("base");
    if ((fl != a.end() && fl->second == "1") || ba == a.end() || ba->second.empty()) return -h * 0.5f;  // flat square / older files: centred on the ground
    return float(atof(ba->second.c_str()));
}

const TriggerKind* find_condition_kind(const std::string& id) {
    for (auto& k : condition_kinds()) if (id == k.id) return &k;
    return nullptr;
}
const TriggerKind* find_effect_kind(const std::string& id) {
    for (auto& k : effect_kinds()) if (id == k.id) return &k;
    return nullptr;
}

// ----------------------------------------------------------------------------- prototypes

static std::map<std::string, WObj> g_proto;
static std::vector<CueInfo> g_sound, g_music, g_voice;
static bool g_ready = false;

bool trigger_prototypes_ready() { return g_ready; }
const std::vector<CueInfo>& retail_sound_cues() { return g_sound; }
const std::vector<CueInfo>& retail_music_cues() { return g_music; }
const std::vector<CueInfo>& retail_voice_cues() { return g_voice; }

static const char* kProtoClasses[] = {
    "MATriggerTimer", "MATriggerRadius", "MATriggerCuboidalVolume", "MATriggerDestroyed", "MATriggerLogic",
    "MoeEntityReference", "MAActionBroadcast", "MAActionSound", "MAActionMusic", "MAActionShow", "MAActionRemoveEntity",
    "MAActionAI", "MAActionActivateCinema", "MAActionMissionVariableSet", "MAActionGameCondition", "MAActionCheckPointSave", "MAActionTeleport",
    "MAActionNation", "MAActionPlayVoice", "MAAIGroup", "MAEvent", "MAPatrol", "MADefend", "MAFollow", "MAAttack", "MAWaypointGroup",
    "MAWaypoint", "MADummyObj", "MAObjectiveType", "MoeEntitySet", "MATrigger"};

// A subtitle for a label: whitespace squeezed, cut at 70 characters (on a word).
static std::string trim_words(const std::string& in) {
    std::string o;
    for (unsigned char ch : in) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        if (ch == ' ' && (o.empty() || o.back() == ' ')) continue;
        o += char(ch);
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.size() > 70) {
        o.resize(70);
        const size_t sp = o.rfind(' ');
        if (sp != std::string::npos && sp > 40) o.resize(sp);
        o += "...";
    }
    return o;
}

// By mission, then the cues with words first, then by label.
static bool cue_less(const CueInfo& a, const CueInfo& b) {
    if (a.mission != b.mission) return a.mission < b.mission;
    if (a.words.empty() != b.words.empty()) return !a.words.empty();
    return a.label < b.label;
}

bool load_trigger_prototypes(GameData& gd, std::string* err) {
    if (g_ready) return true;
    const auto& names = gd.names();
    std::unordered_map<uint32_t, std::string> want;
    for (const char* c : kProtoClasses) want[key_of(c)] = c;
    static const uint32_t K_BROADCAST = key_of("MAActionBroadcast");
    static const uint32_t K_CORP = key_of("MACorporeal"), K_SOUND = key_of("MAActionSound"), K_MUSIC = key_of("MAActionMusic"), K_VOICE = key_of("MAActionPlayVoice");
    std::set<uint32_t> seen_sound, seen_music, seen_voice;
    for (auto& m : gd.maps()) {
        if (m.multiplayer) continue;
        Wdf w;
        if (!gd.load_world(m, w)) continue;
        WTree t;
        t.build(w);
        for (size_t i = 0; i < w.objs.size(); i++) {
            const WObj& o = w.objs[i];
            auto it = want.find(o.cls);
            if (it != want.end()) {
                bool take = !g_proto.count(it->second);
                // Prefer the "player in area" form of the area triggers.
                if (it->second == "MATriggerRadius" || it->second == "MATriggerCuboidalVolume") {
                    bool player = o.i32("PlayerEntitesOnly") == 1 && o.i32("TypeOfCheck") == 3;
                    if (!player) take = false;
                    else if (g_proto.count(it->second)) take = false;
                }
                if (it->second == "MATriggerDestroyed" && !(o.i32("TypeOfCheck") == 1 && o.u32("DestroySet") == 0)) take = false;
                if (take) g_proto[it->second] = o;
            }
            if (o.cls == K_CORP && o.template_id() == 32093 && !g_proto.count("NULLMarker")) g_proto["NULLMarker"] = o;
            int p = t.parent_of[i];
            std::string ctx = m.name + ": " + (p >= 0 ? w.objs[size_t(p)].description() : std::string()) + " / " + o.description();
            if (o.cls == K_SOUND || o.cls == K_MUSIC || o.cls == K_VOICE) {
                const bool snd = o.cls == K_SOUND, mus = o.cls == K_MUSIC;
                const uint32_t c = o.u32(snd ? "CueEvent" : mus ? "MusicCue" : "CueID");  // voice: CueID (wiki action-cue-offsets)
                auto& seen = snd ? seen_sound : mus ? seen_music : seen_voice;
                auto& list = snd ? g_sound : mus ? g_music : g_voice;
                if (c) {
                    // The subtitle: a broadcast action under the same parent as the cue action.
                    std::string words;
                    if (p >= 0)
                        for (int k : t.kids[size_t(p)])
                            if (w.objs[size_t(k)].cls == K_BROADCAST)
                                if (uint32_t tid = w.objs[size_t(k)].u32("BodyTextID")) { words = trim_words(gd.game_text(tid)); if (!words.empty()) break; }
                    if (seen.insert(c).second) {
                        list.push_back({c, ctx, m.name, words, ctx});
                    } else if (!words.empty()) {
                        for (auto& e : list) if (e.id == c && e.words.empty()) { e.words = words; e.mission = m.name; }
                    }
                }
            }
        }
    }
    (void)names;
    for (auto& [n, o] : g_proto) {
        for (const char* k : {"parent", "child", "sibling", "UID"}) if (o.find(k)) o.set_u32(k, 0);
        if (o.find("TriggerFired")) o.set_i32("TriggerFired", 0);
        if (o.find("NumberOfTimesActivated")) o.set_u32("NumberOfTimesActivated", 0);
        if (o.find("TriggerStarted")) o.set_i32("TriggerStarted", 0);
    }
    for (auto* list : {&g_sound, &g_music, &g_voice})
        for (auto& e : *list) {
            std::string pretty = e.mission;
            for (char& ch : pretty) if (ch == '_') ch = ' ';
            if (!e.words.empty()) e.label = "\xE2\x80\x9C" + e.words + "\xE2\x80\x9D - " + pretty;
        }
    std::sort(g_sound.begin(), g_sound.end(), cue_less);
    std::sort(g_music.begin(), g_music.end(), cue_less);
    std::sort(g_voice.begin(), g_voice.end(), cue_less);
    std::vector<std::string> missing;
    for (const char* c : kProtoClasses) if (!g_proto.count(c)) missing.push_back(c);
    if (!missing.empty() && err) {
        *err = "missing trigger prototypes:";
        for (auto& m : missing) *err += " " + m;
    }
    g_ready = true;
    return missing.empty();
}

// ----------------------------------------------------------------------------- trigger shapes

static const uint32_t K_VOLUME = key_of("MATriggerCuboidalVolume"), K_RADIUS = key_of("MATriggerRadius");

bool is_trigger_shape(uint32_t cls) { return cls == K_VOLUME || cls == K_RADIUS; }

static std::string ascii_name(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (c >= 32 && c < 127) ? char(c) : '?';
    return o;
}

uint32_t add_trigger_shape(Scenario& s, const TriggerShape& sh) {
    auto it = g_proto.find(sh.box ? "MATriggerCuboidalVolume" : "MATriggerRadius");
    if (it == g_proto.end()) return 0;
    Wdf& w = s.world;
    uint32_t next = next_uid(w);
    uint32_t group = ensure_group(w, root_uid(w), kTriggerShapeGroup, next);
    WObj o = it->second;  // a retail "player in area" trigger: same class, same fields
    float ground = s.terrain.height_at(sh.x, sh.z);
    o.set_str("Description", ascii_name(sh.name));
    if (sh.box) {
        // TriggerPoint is the box centre (retail).
        o.set_vec3("TriggerPoint", {sh.x, ground + sh.base + sh.h * 0.5f, sh.z});
        o.set_f32("VolumeWidth", sh.w);
        o.set_f32("VolumeLength", sh.l);
        o.set_f32("VolumeHeight", sh.h);
        o.set_f32("VolumeHeading", sh.heading);
        if (o.find("VolumeStateTest")) o.set_i32("VolumeStateTest", sh.inside ? 1 : 2);  // 1 inside, 2 outside (dictionary)
    } else {
        o.set_vec3("TriggerPoint", {sh.x, ground, sh.z});
        o.set_f32("EncounterRadius", sh.radius);
        o.set_i32("InRadiusCheck", sh.inside ? 1 : 2);  // the game tests inside = 1, outside = 2 (never 0)
        o.set_u32("PointCorporeal", 0);
        o.set_u32("RadiusSet", 0);
    }
    if (o.find("TriggerEnabled")) o.set_i32("TriggerEnabled", 1);
    if (o.find("TriggerAutoReset")) o.set_i32("TriggerAutoReset", 0);
    uint32_t uid = next_uid(w);
    if (uid < next) uid = next;
    o.set_u32("UID", uid);
    wedit::append_child(w, group, std::move(o));
    return uid;
}

WObj* condition_shape(Scenario& s, const std::map<std::string, std::string>& args) {
    auto it = args.find("vol");
    if (it == args.end() || it->second.empty()) return nullptr;
    uint32_t u = uint32_t(strtoul(it->second.c_str(), nullptr, 10));
    if (!u) return nullptr;
    for (auto& o : s.world.objs) if (o.uid() == u) return is_trigger_shape(o.cls) ? &o : nullptr;
    return nullptr;
}

int adopt_trigger_shapes(Scenario& s) {
    int n = 0;
    for (auto& t : s.triggers)
        for (auto& c : t.conditions) {
            if (c.kind != "area" && c.kind != "box") continue;
            auto& a = c.args;
            if (a.count("vol") && !a.at("vol").empty()) continue;
            auto pit = a.find("point");
            float x, z;
            if (pit == a.end() || sscanf(pit->second.c_str(), "%f,%f", &x, &z) != 2) continue;  // never placed: the UI asks for a point
            auto num = [&](const char* k, float d) { auto i = a.find(k); return i == a.end() || i->second.empty() ? d : float(atof(i->second.c_str())); };
            TriggerShape sh;
            sh.box = c.kind == "box";
            sh.x = x;
            sh.z = z;
            sh.name = t.name;  // retail volumes carry their trigger's name ("TB - in docks")
            if (sh.box) {
                sh.w = num("width", 40); sh.l = num("length", 40); sh.h = num("height", 60); sh.heading = num("heading", 0);
                sh.base = trigger_box_base(a);  // flat squares and old files: centred on the ground, as they compiled
            } else {
                sh.radius = num("radius", 30);
                sh.inside = num("inside", 1) != 0;
            }
            uint32_t u = add_trigger_shape(s, sh);
            if (!u) return n;  // prototypes not loaded yet
            c.args.clear();
            c.args["vol"] = std::to_string(u);
            n++;
        }
    return n;
}

// ----------------------------------------------------------------------------- AI paths and markers

static const uint32_t K_WPTGROUP = key_of("MAWaypointGroup"), K_WPT = key_of("MAWaypoint");

static WObj* find_obj(Wdf& w, uint32_t uid) {
    if (!uid) return nullptr;
    for (auto& o : w.objs) if (o.uid() == uid) return &o;
    return nullptr;
}

static uint32_t ai_group(Wdf& w) {
    uint32_t next = next_uid(w);
    return ensure_group(w, root_uid(w), kAIPathGroup, next);
}

static void add_waypoint(Scenario& s, uint32_t group, float x, float z) {
    auto it = g_proto.find("MAWaypoint");
    if (it == g_proto.end()) return;
    WObj p = it->second;
    p.set_vec3("ObjPosition", {x, s.terrain.height_at(x, z) + 1.f, z});
    p.set_f32("WptPauseTime", 0);
    p.set_f32("WptSpeed", 0);
    uint32_t uid = next_uid(s.world);
    p.set_u32("UID", uid);
    wedit::append_child(s.world, group, std::move(p));
    if (WObj* g = find_obj(s.world, group))
        if (!g->u32("WaypointID")) g->set_u32("WaypointID", uid);  // retail: the first waypoint
}

uint32_t add_ai_path(Scenario& s, const std::vector<std::pair<float, float>>& pts, const std::string& name) {
    auto it = g_proto.find("MAWaypointGroup");
    if (it == g_proto.end() || !g_proto.count("MAWaypoint")) return 0;
    uint32_t parent = ai_group(s.world);
    WObj g = it->second;
    g.set_str("Description", ascii_name(name));
    g.set_u32("WaypointID", 0);
    g.set_i32("IsSpline", 0);
    g.set_i32("IsSplineLocked", 0);
    g.set_f32("WaypointSpeed", 0);
    uint32_t uid = next_uid(s.world);
    g.set_u32("UID", uid);
    wedit::append_child(s.world, parent, std::move(g));
    for (auto& [x, z] : pts) add_waypoint(s, uid, x, z);
    return uid;
}

uint32_t add_ai_marker(Scenario& s, float x, float z, const std::string& name, bool null_marker) {
    auto it = g_proto.find(null_marker && g_proto.count("NULLMarker") ? "NULLMarker" : "MADummyObj");
    if (it == g_proto.end()) return 0;
    uint32_t parent = ai_group(s.world);
    WObj m = it->second;
    m.set_str("Description", ascii_name(name));
    set_pos(m, {x, s.terrain.height_at(x, z) + (null_marker ? 0.f : 1.f), z});
    uint32_t uid = next_uid(s.world);
    m.set_u32("UID", uid);
    wedit::append_child(s.world, parent, std::move(m));
    return uid;
}

// Puts every path group / marker the effects name back under "AI Paths" (a loaded file keeps them
// inside the compiled "[ed]" event, and the compile recreates those).
void park_ai_objects(Wdf& w, const Scenario& s) {
    std::vector<uint32_t> want;
    for (auto& t : s.triggers)
        for (auto& e : t.effects) {
            for (const char* k : {"wpt", "marker"}) {
                auto it = e.args.find(k);
                if (it != e.args.end() && !it->second.empty()) want.push_back(uint32_t(strtoul(it->second.c_str(), nullptr, 10)));
            }
        }
    if (want.empty()) return;
    uint32_t grp = 0;
    for (uint32_t u : want) {
        if (!find_obj(w, u)) continue;
        if (!grp) grp = ai_group(w);
        if (find_obj(w, u)->parent() != grp) wedit::reparent(w, u, grp);
    }
}

int adopt_ai_objects(Scenario& s) {
    int n = 0;
    if (!g_ready) return 0;
    park_ai_objects(s.world, s);
    for (size_t ti = 0; ti < s.triggers.size(); ti++)
        for (auto& e : s.triggers[ti].effects) {
            auto& a = e.args;
            std::string nm = s.triggers[ti].name;
            if (e.kind == "ai") {
                auto pit = a.find("path");
                if (pit != a.end() && !pit->second.empty()) {
                    std::vector<std::pair<float, float>> pts;
                    for (auto& part : split(pit->second, ';')) {
                        float x, z;
                        if (sscanf(part.c_str(), "%f,%f", &x, &z) == 2) pts.push_back({x, z});
                    }
                    uint32_t g = a.count("wpt") ? uint32_t(strtoul(a["wpt"].c_str(), nullptr, 10)) : 0;
                    if (g && !find_obj(s.world, g)) g = 0;
                    if (g) for (auto& [x, z] : pts) add_waypoint(s, g, x, z);
                    else if ((g = add_ai_path(s, pts, nm + " - patrol path"))) a["wpt"] = std::to_string(g);
                    else continue;
                    pit->second.clear();
                    n++;
                }
            }
            if (e.kind == "ai" || e.kind == "teleport") {
                auto pit = a.find("point");
                float x, z;
                if (pit != a.end() && sscanf(pit->second.c_str(), "%f,%f", &x, &z) == 2) {
                    uint32_t m = a.count("marker") ? uint32_t(strtoul(a["marker"].c_str(), nullptr, 10)) : 0;
                    if (WObj* o = m ? find_obj(s.world, m) : nullptr) {
                        Vec3 p;
                        get_pos(*o, p);
                        float dy = s.terrain.height_at(x, z) - s.terrain.height_at(p.x, p.z);
                        set_pos(*o, {x, p.y + dy, z});
                    } else if ((m = add_ai_marker(s, x, z, nm + (e.kind == "ai" ? " - defend point" : " - teleport marker"), e.kind == "ai"))) {
                        a["marker"] = std::to_string(m);
                    } else continue;
                    pit->second.clear();
                    n++;
                }
            }
        }
    return n;
}

int ai_path_length(Scenario& s, uint32_t group) {
    WTree t;
    t.build(s.world);
    int i = t.find(group);
    return i < 0 ? 0 : int(t.kids[size_t(i)].size());
}

// ----------------------------------------------------------------------------- compiler

namespace {

struct Ctx {
    const Scenario& s;
    Wdf& w;
    std::vector<std::string>* warn;
    std::map<int, std::string>* strings;
    uint32_t next;
    uint32_t group = 0;
    uint32_t player = 0;
    int msg = 0;

    void warning(const std::string& t) { if (warn) warn->push_back(t); }
    bool proto(const char* cls, WObj& out) {
        auto it = g_proto.find(cls);
        if (it == g_proto.end()) { warning(std::string("no retail prototype for ") + cls); return false; }
        out = it->second;
        return true;
    }
    uint32_t add(uint32_t parent, WObj o) {
        uint32_t uid = o.uid() ? o.uid() : next++;
        o.set_u32("UID", uid);
        wedit::append_child(w, parent, std::move(o));
        return uid;
    }
    bool exists(uint32_t uid) {
        if (!uid) return false;
        for (auto& o : w.objs) if (o.uid() == uid) return true;
        return false;
    }
    float ground(float x, float z) { return s.terrain.height_at(x, z); }
};

float fnum(const std::map<std::string, std::string>& a, const char* k, float def = 0) {
    auto it = a.find(k);
    if (it == a.end() || it->second.empty()) return def;
    return float(atof(it->second.c_str()));
}
uint32_t unum(const std::map<std::string, std::string>& a, const char* k, uint32_t def = 0) {
    auto it = a.find(k);
    if (it == a.end() || it->second.empty()) return def;
    return uint32_t(strtoul(it->second.c_str(), nullptr, 0));
}
std::string sval(const std::map<std::string, std::string>& a, const char* k) {
    auto it = a.find(k);
    return it == a.end() ? std::string() : it->second;
}
bool point(const std::map<std::string, std::string>& a, const char* k, float& x, float& z) {
    std::string v = sval(a, k);
    return sscanf(v.c_str(), "%f,%f", &x, &z) == 2;
}
std::vector<std::pair<float, float>> path(const std::map<std::string, std::string>& a, const char* k) {
    std::vector<std::pair<float, float>> out;
    for (auto& part : split(sval(a, k), ';')) {
        float x, z;
        if (sscanf(part.c_str(), "%f,%f", &x, &z) == 2) out.push_back({x, z});
    }
    return out;
}
std::vector<uint32_t> units(const std::map<std::string, std::string>& a, const char* k) {
    std::vector<uint32_t> out;
    for (auto& part : split(sval(a, k), ',')) {
        uint32_t u = uint32_t(strtoul(trim(part).c_str(), nullptr, 10));
        if (u) out.push_back(u);
    }
    return out;
}

std::string ascii(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (c >= 32 && c < 127) ? char(c) : '?';
    return o;
}

void set_common(WObj& o, const std::string& desc, bool enabled, bool autoreset) {
    o.set_str("Description", ascii(desc));
    if (o.find("TriggerEnabled")) o.set_i32("TriggerEnabled", enabled ? 1 : 0);
    if (o.find("TriggerAutoReset")) o.set_i32("TriggerAutoReset", autoreset ? 1 : 0);
}

uint32_t add_ref(Ctx& c, uint32_t parent, uint32_t target, const std::string& desc) {
    WObj r;
    if (!c.proto("MoeEntityReference", r)) return 0;
    r.set_str("Description", ascii(desc) + " (ref)");
    r.set_u32("ReferenceUID", target);
    return c.add(parent, std::move(r));
}

uint32_t add_marker(Ctx& c, float x, float z, const std::string& desc) {
    WObj m;
    if (!c.proto("MADummyObj", m)) return 0;
    m.set_str("Description", ascii(desc));
    set_pos(m, {x, c.ground(x, z) + 1.f, z});
    return c.add(c.group, std::move(m));
}

}  // namespace

static void compile_expanded(const Scenario& s, Wdf& w, std::vector<std::string>* warnings, std::map<int, std::string>* extra);

// Multiplayer objective slots name their targets by UID as well: Base War = slot 0 (ObjectiveType 9) -> the Red power
// plant, slot 1 (10) -> the Blue one (retail BoggedDown ob0obj0 126031 / ob1obj0 124373). A kit copied from a retail
// level gets new UIDs, so the slots kept pointing at nothing and the game ended the match at once ('Blue Team Wins!'
// ~5 s in). Every slot that has MAObjectiveType targets lists exactly those; other slots are left alone.
static void sync_mp_objective_targets(WObj& r, const Wdf& w, const Scenario& s) {
    // Base War: a building is only a Base War objective when the slot's type is 9 (Red) / 10 (Blue); a from-scratch base
    // level has 0 there and the match ended after 5 s.
    const bool base_war = std::find(s.types.begin(), s.types.end(), "BaseWar") != s.types.end();
    for (int i = 0; i < 30; i++) {
        auto targets = objective_targets(w, i);
        if (targets.empty()) continue;
        char k[64];
        snprintf(k, sizeof k, "NumObjectiveObjects%d", i);
        r.set_i32(k, int32_t(targets.size()));
        if (base_war && i < 2) r.set_i32(i ? "ObjectiveType1" : "ObjectiveType0", i ? 10 : 9);
        for (size_t j = 0;; j++) {
            snprintf(k, sizeof k, "ob%dobj%zu", i, j);
            if (j < targets.size()) r.set_u32(k, targets[j]);
            else if (r.find(k)) r.set_u32(k, 0);
            else break;
        }
    }
}

// Entity sets link both ways (the set's MoeEntitySetMember0..n, the member's SetMember0..n). A kit clone points its
// objects back at the matching set here, but the set (kept from the base level, its old members gone) listed none, so
// the retail start-up actions on 'Red Stuff' / 'Blue Stuff' / 'Invulnerabilty at Start' hit nothing. Add every object
// that names a set to that set's list; existing members are kept.
static void fill_set_members(Wdf& w) {
    static const uint32_t K_SET = key_of("MoeEntitySet");
    std::unordered_map<uint32_t, std::vector<uint32_t>> named;
    for (auto& o : w.objs) {
        int n = o.i32("SetCount");
        for (int k = 0; k < n; k++)
            if (uint32_t set = o.u32("SetMember" + std::to_string(k))) named[set].push_back(o.uid());
    }
    for (auto& s : w.objs) {
        if (s.cls != K_SET) continue;
        auto it = named.find(s.uid());
        if (it == named.end()) continue;
        int n = s.i32("MoeEntitySetCount");
        std::vector<uint32_t> m;
        for (int k = 0; k < n; k++) m.push_back(s.u32("MoeEntitySetMember" + std::to_string(k)));
        size_t before = m.size();
        for (uint32_t u : it->second) if (std::find(m.begin(), m.end(), u) == m.end()) m.push_back(u);
        if (m.size() == before) continue;
        for (size_t k = before; k < m.size(); k++) s.set_u32("MoeEntitySetMember" + std::to_string(k), m[k]);
        s.set_i32("MoeEntitySetCount", int(m.size()));
    }
}

int fix_outside_circles(Wdf& w) {
    static const uint32_t K_RAD = key_of("MATriggerRadius");
    int n = 0;
    for (auto& o : w.objs)
        if (o.cls == K_RAD && o.find("InRadiusCheck") && o.i32("InRadiusCheck") == 0) { o.set_i32("InRadiusCheck", 2); n++; }
    return n;
}

bool win_all_idle(const Wdf& w) {
    static const uint32_t K_LOGIC = key_of("MATriggerLogic");
    for (auto& o : w.objs)
        if (o.cls == K_LOGIC && o.description() == kWinAllTrigger && reference_children(w, o.uid()).empty()) return true;
    return false;
}

int fix_win_all(Wdf& w) {
    static const uint32_t K_LOGIC = key_of("MATriggerLogic");
    int n = 0;
    for (auto& o : w.objs) {
        if (o.cls != K_LOGIC || o.description() != kWinAllTrigger || !o.find("TriggerEnabled")) continue;
        const int want = reference_children(w, o.uid()).empty() ? 0 : 1;
        if (o.i32("TriggerEnabled") != want) { o.set_i32("TriggerEnabled", want); n++; }
    }
    return n;
}

// "Bring in reinforcements" (Mission_05 "To the Rescue", wiki reinforcements-dormant-set-pattern):
// the units are placed from the start with their AI off (m_bAIEnabled 0), a start-up trigger hides
// them (MAActionShow Enable 0), and the trigger shows them (MAActionShow Enable 1, EnableTree 1),
// switches their AI on (MAActionAI EnableAI 1) and sets their side (MAActionNation). Expanded here
// into the ordinary show / hide / enable_unit / nation effects, one per unit.
void compile_triggers(const Scenario& s, Wdf& w, std::vector<std::string>* warnings, std::map<int, std::string>* extra) {
    fix_outside_circles(w);  // files from 1.0.9 wrote 0 for "outside", which the game never matches
    fix_win_all(w);
    bool any = false;
    for (auto& t : s.triggers) for (auto& e : t.effects) any |= e.kind == "reinforce";
    if (!any) { compile_expanded(s, w, warnings, extra); return; }
    Scenario x;
    x.multiplayer = s.multiplayer;
    x.objectives = s.objectives;
    x.audio = s.audio;
    x.strings = s.strings;
    x.briefing = s.briefing;
    x.briefing_title = s.briefing_title;
    x.terrain = s.terrain;  // trigger heights
    x.triggers = s.triggers;
    EdTrigger hide;
    hide.name = "[ed] Reinforcements wait hidden";
    hide.conditions.push_back({"start", {}});
    for (auto& t : x.triggers) {
        std::vector<EdEffect> out;
        for (auto& e : t.effects) {
            if (e.kind != "reinforce") { out.push_back(e); continue; }
            auto it = e.args.find("units");
            std::string nation = e.args.count("nation") ? e.args.at("nation") : "5";
            bool hide_at_start = !e.args.count("hide") || e.args.at("hide") != "0";
            int n = 0;
            for (auto& u : split(it == e.args.end() ? std::string() : it->second, ',')) {
                std::string id = trim(u);
                uint32_t uid = uint32_t(strtoul(id.c_str(), nullptr, 10));
                if (!uid) continue;
                for (auto& o : w.objs)
                    if (o.uid() == uid) { o.set_i32("m_bAIEnabled", 0); break; }  // asleep until the trigger
                if (hide_at_start) hide.effects.push_back({"hide", {{"unit", id}}});
                out.push_back({"show", {{"unit", id}}});
                out.push_back({"enable_unit", {{"unit", id}, {"on", "1"}}});
                out.push_back({"nation", {{"unit", id}, {"nation", nation}}});
                n++;
            }
            if (!n && warnings) warnings->push_back("\"" + t.name + "\": \"Bring in reinforcements\" has no units (select units on the map, then press Add selected)");
        }
        t.effects = std::move(out);
    }
    if (!hide.effects.empty()) x.triggers.insert(x.triggers.begin(), std::move(hide));
    compile_expanded(x, w, warnings, extra);
}

static void compile_expanded(const Scenario& s, Wdf& w, std::vector<std::string>* warnings, std::map<int, std::string>* extra) {
    // Only a legacy trigger list is compiled (convert_legacy_triggers); the world's own triggers are never touched.
    if (!s.triggers.empty()) {
        park_ai_objects(w, s);
        // Remove the previous compilation: the group and every prefixed object in units.
        for (;;) {
            uint32_t victim = 0;
            for (auto& o : w.objs) {
                std::string d = o.description();
                if ((o.cls == K_GROUP && d == kEditorTriggerGroup) || d.compare(0, strlen(kEditorPrefix), kEditorPrefix) == 0) { victim = o.uid(); break; }
            }
            if (!victim) break;
            wedit::remove_subtree(w, victim);
        }
    }
    Ctx c{s, w, warnings, extra, next_uid(w)};
    c.player = player_start_uid(w);
    // Root: briefing and objectives (missions).
    int ri = find_class(w, K_ROOT);
    if (ri >= 0 && s.multiplayer) {
        sync_mp_objective_targets(w.objs[size_t(ri)], w, s);
        fill_set_members(w);
    }
    if (ri >= 0 && !s.multiplayer) {
        WObj& r = w.objs[size_t(ri)];
        if (!s.briefing_title.empty()) { (*extra)[kStrBriefingTitle] = s.briefing_title; r.set_u32("TitleText", kTextBase + kStrBriefingTitle); }
        if (!s.briefing.empty()) { (*extra)[kStrBriefing] = s.briefing; r.set_u32("BriefingText", kTextBase + kStrBriefing); }
        for (size_t i = 0; i < s.objectives.size() && i < 30; i++) {
            char k[64];
            int n = kStrObjective + int(i);
            // A slot whose line the user has not retyped keeps the level's own text (or stays a hidden step).
            ObjectiveSlot sl = objective_slot(s, int(i));
            bool keep = sl.hidden || sl.game_text;
            // The completing variable belongs to the slot: keep it; a slot without one (a keep-alive objective never claims one) gets a free one.
            snprintf(k, sizeof k, "ob%zuvar0", i);
            int var = r.find(k) ? r.i32(k, -1) : -1;
            if (var < 0) { var = free_objective_var(w, r, int(i)); r.set_i32(k, var); }
            if (!keep) {
                (*extra)[n] = s.objectives[i];
                snprintf(k, sizeof k, "ObjectiveItemDesc%zu", i);
                r.set_u32(k, kTextBase + uint32_t(n));
                snprintf(k, sizeof k, "ObjectiveEnabledBool%zu", i);  // new maps leave slots 2-29 switched off
                if (r.find(k)) r.set_i32(k, 1);
                snprintf(k, sizeof k, "ObjectiveEditorName%zu", i);
                if (r.find(k)) r.set_str(k, ascii(s.objectives[i]).substr(0, 60));
                snprintf(k, sizeof k, "mfname%d", var);
                if (r.find(k)) r.set_str(k, ascii(s.objectives[i]).substr(0, 60));
            }
            // Radar targets: the units carrying this objective's MAObjectiveType (retail ob%dobj%d list).
            // Left alone when they match the slot's list already (retail lists also hold template UIDs).
            auto targets = objective_targets(w, int(i));
            std::vector<uint32_t> listed;
            for (size_t j = 0; j < 32; j++) {
                snprintf(k, sizeof k, "ob%zuobj%zu", i, j);
                uint32_t u = r.u32(k);
                if (u && find_obj(w, u) && std::find(listed.begin(), listed.end(), u) == listed.end()) listed.push_back(u);
            }
            std::vector<uint32_t> a = targets, b = listed;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a == b) continue;
            snprintf(k, sizeof k, "NumObjectiveObjects%zu", i);
            if (r.find(k) || !targets.empty()) r.set_i32(k, int32_t(targets.size()));
            for (size_t j = 0;; j++) {
                snprintf(k, sizeof k, "ob%zuobj%zu", i, j);
                if (j < targets.size()) r.set_u32(k, targets[j]);
                else if (r.find(k)) r.set_u32(k, 0);
                else break;
            }
        }
    }
    if (s.triggers.empty()) return;
    if (!g_ready) { c.warning("trigger prototypes not loaded; triggers were not compiled"); return; }
    uint32_t root = root_uid(w);
    c.group = c.next++;
    wedit::append_child(w, root, make_group(kEditorTriggerGroup, c.group));

    // Pass 1: the native trigger that carries each editor trigger's actions.
    struct Plan { uint32_t main = 0; bool logic = false; };
    std::vector<Plan> plan(s.triggers.size());
    auto by_name = [&](const std::string& n) -> int {
        for (size_t i = 0; i < s.triggers.size(); i++) if (s.triggers[i].name == n) return int(i);
        return -1;
    };
    // A box or circle condition's map object (MATriggerCuboidalVolume / MATriggerRadius, "vol").
    auto shape_of = [&](const EdCondition& cd) -> WObj* {
        if (cd.kind != "area" && cd.kind != "box") return nullptr;
        uint32_t u = unum(cd.args, "vol");
        if (!u) return nullptr;
        for (auto& o : w.objs) if (o.uid() == u) return is_trigger_shape(o.cls) ? &o : nullptr;
        return nullptr;
    };
    std::set<uint32_t> shape_main;  // shapes that carry a trigger's actions (one trigger each)
    for (size_t i = 0; i < s.triggers.size(); i++) {
        const EdTrigger& t = s.triggers[i];
        bool refs = false;
        for (auto& cd : t.conditions) if (cd.kind == "trigger" || cd.kind == "not_trigger") refs = true;
        plan[i].logic = t.conditions.size() > 1 || refs;
        // One box or circle: the map object is the trigger (retail: the volume carries the actions).
        WObj* sh = !plan[i].logic && t.conditions.size() == 1 ? shape_of(t.conditions[0]) : nullptr;
        if (sh && shape_main.insert(sh->uid()).second) { plan[i].main = sh->uid(); continue; }
        if (sh) plan[i].logic = true;  // the same object already carries another trigger: use it as an input
        plan[i].main = c.next++;
    }
    // Pass 2: conditions and logic.
    for (size_t i = 0; i < s.triggers.size(); i++) {
        const EdTrigger& t = s.triggers[i];
        std::string base = t.name.empty() ? "Trigger " + std::to_string(i + 1) : t.name;
        std::vector<uint32_t> inputs;
        auto make_condition = [&](const EdCondition& cd, uint32_t uid, bool enabled, bool loop) -> bool {
            WObj o;
            const auto& a = cd.args;
            if (cd.kind == "start" || cd.kind == "timer") {
                if (!c.proto("MATriggerTimer", o)) return false;
                float sec = cd.kind == "start" ? 0.f : fnum(a, "seconds", 10);
                o.set_f32("TrgTimerDurationSeconds", sec);
                o.set_f32("TrgTimerDuration", sec / 60.f);
                o.set_f32("TrgTimerStart", 0);
            } else if (WObj* sh = shape_of(cd)) {
                // The map object itself: as the trigger (uid == its own) or as an input of a logic.
                if (sh->uid() != uid) {
                    set_common(*sh, sh->description(), true, false);
                    inputs.push_back(sh->uid());
                    return false;  // nothing added: the caller must not list it twice
                }
                set_common(*sh, sh->description(), enabled, loop);
                return true;
            } else if ((cd.kind == "area" || cd.kind == "box") && !sval(a, "vol").empty()) {
                c.warning("\"" + base + "\": its trigger " + (cd.kind == "box" ? "box" : "circle") + " was deleted from the map");
                return false;
            } else if (cd.kind == "area") {
                float x, z;
                if (!point(a, "point", x, z)) { c.warning("\"" + base + "\": its circle has no centre point (press Pick on map)"); return false; }
                if (!c.proto("MATriggerRadius", o)) return false;
                o.set_vec3("TriggerPoint", {x, c.ground(x, z), z});
                o.set_f32("EncounterRadius", fnum(a, "radius", 30));
                o.set_i32("InRadiusCheck", unum(a, "inside", 1) ? 1 : 2);
                o.set_u32("PointCorporeal", 0);
                o.set_u32("RadiusSet", 0);
            } else if (cd.kind == "box") {
                float x, z;
                if (!point(a, "point", x, z)) { c.warning("\"" + base + "\": its box has no centre point (press Pick on map)"); return false; }
                if (!c.proto("MATriggerCuboidalVolume", o)) return false;
                // TriggerPoint is the box centre (retail): bottom edge + half the height above the ground.
                float bh = fnum(a, "height", 60);
                o.set_vec3("TriggerPoint", {x, c.ground(x, z) + trigger_box_base(a) + bh * 0.5f, z});
                o.set_f32("VolumeWidth", fnum(a, "width", 40));
                o.set_f32("VolumeLength", fnum(a, "length", 40));
                o.set_f32("VolumeHeight", bh);
                o.set_f32("VolumeHeading", fnum(a, "heading", 0));
            } else if (cd.kind == "destroyed" || cd.kind == "damaged") {
                auto us = units(a, "units");
                std::vector<uint32_t> ok;
                for (uint32_t u : us) if (c.exists(u)) ok.push_back(u); else c.warning("\"" + base + "\": a unit it watches was deleted");
                if (ok.empty()) { c.warning("\"" + base + "\": no units are picked to watch (select them on the map, then press Add selected)"); return false; }
                if (!c.proto("MATriggerDestroyed", o)) return false;
                o.set_i32("TypeOfCheck", 1);
                o.set_u32("DestroySet", 0);
                o.set_i32("CombineMethod", cd.kind == "destroyed" && unum(a, "all", 1) ? 2 : 1); // 1 any, 2 all (retail "All ... Dead" = 2)
                o.set_i32("AllowJacking", unum(a, "jacking", 1) ? 1 : 0);
                o.set_u32("DamageLevel", cd.kind == "damaged" ? unum(a, "percent", 50) : 0);
                set_common(o, base, enabled, loop);
                o.set_u32("UID", uid);
                c.add(c.group, std::move(o));
                for (uint32_t u : ok) {
                    std::string ud;
                    for (auto& x : w.objs) if (x.uid() == u) { ud = x.description(); break; }
                    add_ref(c, uid, u, ud);
                }
                return true;
            } else {
                return false;
            }
            set_common(o, base, enabled, loop);
            o.set_u32("UID", uid);
            c.add(c.group, std::move(o));
            return true;
        };
        if (!plan[i].logic) {
            EdCondition start{"start", {}};
            const EdCondition& cd = t.conditions.empty() ? start : t.conditions.front();
            if (!make_condition(cd, plan[i].main, t.enabled, t.loop)) {
                // Fall back to a timer that never runs, so actions stay attached somewhere valid.
                EdCondition never{"timer", {{"seconds", "0"}}};
                make_condition(never, plan[i].main, false, false);
            }
            continue;
        }
        // Logic: every condition becomes an input.
        for (size_t k = 0; k < t.conditions.size(); k++) {
            const EdCondition& cd = t.conditions[k];
            if (cd.kind == "trigger" || cd.kind == "not_trigger") {
                int target = by_name(sval(cd.args, "trigger"));
                if (target < 0) { c.warning("\"" + base + "\": it waits for a trigger called '" + sval(cd.args, "trigger") + "' that does not exist"); continue; }
                uint32_t tu = plan[size_t(target)].main;
                if (cd.kind == "trigger") { inputs.push_back(tu); continue; }
                WObj inv;
                if (!c.proto("MATriggerLogic", inv)) continue;
                set_common(inv, base + " / not " + s.triggers[size_t(target)].name, true, false);
                inv.set_i32("LogicType", 0);
                inv.set_u32("Inverse", 1);
                uint32_t iu = c.add(c.group, std::move(inv));
                add_ref(c, iu, tu, s.triggers[size_t(target)].name);
                inputs.push_back(iu);
                continue;
            }
            uint32_t cu = c.next++;
            if (make_condition(cd, cu, true, false)) inputs.push_back(cu);
        }
        WObj lg;
        if (!c.proto("MATriggerLogic", lg)) continue;
        set_common(lg, base, t.enabled, t.loop);
        lg.set_i32("LogicType", t.any ? 1 : 0);
        lg.set_u32("Inverse", 0);
        lg.set_u32("UID", plan[i].main);
        c.add(c.group, std::move(lg));
        for (size_t k = 0; k < inputs.size(); k++) add_ref(c, plan[i].main, inputs[k], base + " input " + std::to_string(k + 1));
    }
    // Pass 3: actions, in order, under each trigger's main object.
    for (size_t i = 0; i < s.triggers.size(); i++) {
        const EdTrigger& t = s.triggers[i];
        uint32_t main = plan[i].main;
        std::string base = t.name.empty() ? "Trigger " + std::to_string(i + 1) : t.name;
        for (auto& e : t.effects) {
            const auto& a = e.args;
            WObj o;
            const char* cls = nullptr;
            if (e.kind == "message") {
                if (!c.proto(cls = "MAActionBroadcast", o)) continue;
                int n = kStrMessage + c.msg++;
                (*extra)[n] = sval(a, "text");
                o.set_u32("BodyTextID", kTextBase + uint32_t(n));
                std::string cap = sval(a, "caption");
                if (!cap.empty()) {
                    int m = kStrMessage + c.msg++;
                    (*extra)[m] = cap;
                    o.set_u32("CaptionTextID", kTextBase + uint32_t(m));
                } else {
                    o.set_u32("CaptionTextID", 0);
                }
                o.set_f32("TimeDisplayedFor", fnum(a, "seconds", 6));
                o.set_i32("PauseGame", unum(a, "pause", 0) ? 1 : 0);
            } else if (e.kind == "voice") {
                if (!c.proto(cls = "MAActionSound", o)) continue;
                o.set_u32("CueEvent", unum(a, "cue"));
            } else if (e.kind == "audio") {
                if (!c.proto(cls = "MAActionSound", o)) continue;
                uint32_t n = unum(a, "clip", 1);
                if (!s.audio.count(int(n))) c.warning("\"" + base + "\": its sound clip is not imported");
                o.set_u32("CueEvent", kAudioBase + n);
            } else if (e.kind == "music") {
                if (!c.proto(cls = "MAActionMusic", o)) continue;
                if (unum(a, "cue")) o.set_u32("MusicCue", unum(a, "cue"));
                o.set_u32("IntensityLevel", unum(a, "intensity", 3000));
            } else if (e.kind == "objective" || e.kind == "variable") {
                if (!c.proto(cls = "MAActionMissionVariableSet", o)) continue;
                o.set_i32("MissionVariableSet", int32_t(e.kind == "objective" ? unum(a, "objective") : unum(a, "var")));
                o.set_i32("VariableState", int32_t(e.kind == "objective" ? 1 : unum(a, "state", 1)));
            } else if (e.kind == "win" || e.kind == "lose") {
                if (!c.proto(cls = "MAActionGameCondition", o)) continue;
                o.set_i32("GameCondition", e.kind == "win" ? 2 : 1);
            } else if (e.kind == "show" || e.kind == "hide") {
                uint32_t u = unum(a, "unit");
                if (!c.exists(u)) { c.warning("\"" + base + "\": a show or hide effect has no unit (select one, then press Use selection)"); continue; }
                if (!c.proto(cls = "MAActionShow", o)) continue;
                o.set_i32("Enable", e.kind == "show" ? 1 : 0);
                o.set_u32("ActionEntity", u);
                o.set_u32("AISet", 0);
                o.set_i32("EnableTree", 1);
            } else if (e.kind == "remove") {
                uint32_t u = unum(a, "unit");
                if (!c.exists(u)) { c.warning("\"" + base + "\": a remove effect has no unit (select one, then press Use selection)"); continue; }
                if (!c.proto(cls = "MAActionRemoveEntity", o)) continue;
                o.set_u32("ActionEntity", u);
                o.set_u32("ActionSet", 0);
                o.set_i32("ShowKillEffect", unum(a, "effect", 0) ? 1 : 0);
            } else if (e.kind == "enable_unit" || e.kind == "trigger_on" || e.kind == "trigger_off") {
                uint32_t target = 0;
                bool on = true;
                if (e.kind == "enable_unit") { target = unum(a, "unit"); on = unum(a, "on", 1) != 0; }
                else {
                    int ti = by_name(sval(a, "trigger"));
                    if (ti >= 0) target = plan[size_t(ti)].main;
                    on = e.kind == "trigger_on";
                }
                if (!target || (e.kind == "enable_unit" && !c.exists(target))) { c.warning("\"" + base + "\": a switch on/off effect has nothing picked"); continue; }
                if (!c.proto(cls = "MAActionAI", o)) continue;
                o.set_u32("ActionEntity", target);
                o.set_u32("AISet", 0);
                o.set_i32("EnableAI", on ? 1 : 0);
            } else if (e.kind == "cinema") {
                uint32_t cin = unum(a, "cinema");
                if (!c.exists(cin)) { c.warning("\"" + base + "\": a camera-sequence effect has no cutscene picked (this level may have none)"); continue; }
                if (!c.proto(cls = "MAActionActivateCinema", o)) continue;
                o.set_u32("cinemaID", cin);
            } else if (e.kind == "teleport") {
                float x, z;
                uint32_t mk = unum(a, "marker");
                if (mk && !c.exists(mk)) mk = 0;
                if (!mk && !point(a, "point", x, z)) { c.warning("\"" + base + "\": a move-the-player effect has no destination (press Pick on map)"); continue; }
                if (!c.proto(cls = "MAActionTeleport", o)) continue;
                if (!mk) mk = add_marker(c, x, z, base + " teleport marker");
                o.set_u32("MarkerUID", mk);
                o.set_f32("Heading", fnum(a, "heading", 0));
            } else if (e.kind == "nation") {
                uint32_t u = unum(a, "unit");
                if (!c.exists(u)) { c.warning("\"" + base + "\": a change-side effect has no unit (select one, then press Use selection)"); continue; }
                if (!c.proto(cls = "MAActionNation", o)) continue;
                o.set_u32("ActionEntity", u);
                o.set_u32("AISet", 0);
                o.set_i32("Nation", int32_t(unum(a, "nation", 1)));
            } else if (e.kind == "checkpoint") {
                if (!c.proto(cls = "MAActionCheckPointSave", o)) continue;
                o.set_str("suffix", ascii(sval(a, "name")));
            } else if (e.kind == "ai") {
                uint32_t u = unum(a, "unit");
                if (!c.exists(u)) { c.warning("\"" + base + "\": an orders effect has no unit (select the unit on the map, then press Use selection)"); continue; }
                std::string order = sval(a, "order");
                WObj grp, ev, beh;
                if (!c.proto("MAAIGroup", grp) || !c.proto("MAEvent", ev)) continue;
                // One "[ed] AI Orders" group per unit.
                uint32_t gu = 0;
                {
                    WTree tr;
                    tr.build(w);
                    int ui = tr.find(u);
                    if (ui >= 0)
                        for (int k : tr.kids[size_t(ui)])
                            if (w.objs[size_t(k)].description() == std::string(kEditorPrefix) + "AI Orders") gu = w.objs[size_t(k)].uid();
                }
                if (!gu) {
                    grp.set_str("Description", std::string(kEditorPrefix) + "AI Orders");
                    gu = c.add(u, std::move(grp));
                }
                ev.set_str("Description", std::string(kEditorPrefix) + ascii(base) + " - " + order);
                ev.set_u32("TriggerID", main);
                ev.set_u32("Priority", 2);
                uint32_t eu = c.add(gu, std::move(ev));
                if (order == "patrol") {
                    uint32_t real = unum(a, "wpt");
                    if (real) {
                        // The path is a real MAWaypointGroup of the map: it goes under this event (retail).
                        int n = 0;
                        for (auto& o : w.objs) if (o.parent() == real) n++;
                        if (!c.exists(real) || n < 2) { c.warning("\"" + base + "\": a patrol path needs 2 or more points (redraw it on the map)"); continue; }
                        if (!c.proto("MAPatrol", beh)) continue;
                        c.add(eu, std::move(beh));
                        wedit::reparent(w, real, eu);
                        continue;
                    }
                    auto pts = path(a, "path");
                    if (pts.size() < 2) { c.warning("\"" + base + "\": a patrol path needs 2 or more points (press Draw on map)"); continue; }
                    if (!c.proto("MAPatrol", beh)) continue;
                    c.add(eu, std::move(beh));
                    WObj wg, wp;
                    if (!c.proto("MAWaypointGroup", wg) || !c.proto("MAWaypoint", wp)) continue;
                    uint32_t first = c.next + 1;  // the group takes c.next, then waypoints follow
                    wg.set_u32("WaypointID", first);
                    wg.set_i32("IsSpline", 0);
                    wg.set_i32("IsSplineLocked", 0);
                    wg.set_f32("WaypointSpeed", 0);
                    wg.set_i32("TraverseType", 2);  // loop the route
                    uint32_t wgu = c.add(eu, std::move(wg));
                    for (auto& [x, z] : pts) {
                        WObj p = wp;
                        p.set_vec3("ObjPosition", {x, c.ground(x, z) + 1.f, z});
                        p.set_f32("WptPauseTime", 0);
                        p.set_f32("WptSpeed", 0);
                        c.add(wgu, std::move(p));
                    }
                } else if (order == "defend") {
                    float x, z;
                    uint32_t mu = unum(a, "marker");
                    if (mu && !c.exists(mu)) mu = 0;
                    if (!mu && !point(a, "point", x, z)) { c.warning("\"" + base + "\": a defend order has no point (press Pick on map)"); continue; }
                    if (!c.proto("MADefend", beh)) continue;
                    WObj mk;
                    if (mu) {
                    } else if (c.proto("NULLMarker", mk)) {
                        mk.set_str("Description", ascii(base) + " defend point");
                        set_pos(mk, {x, c.ground(x, z), z});
                        mu = c.add(c.group, std::move(mk));
                    } else {
                        mu = add_marker(c, x, z, base + " defend point");
                    }
                    beh.set_u32("DefendUID", mu);
                    beh.set_f32("Radius", fnum(a, "radius", 60));
                    c.add(eu, std::move(beh));
                } else if (order == "follow") {
                    if (!c.proto("MAFollow", beh)) continue;
                    uint32_t tg = unum(a, "target");
                    beh.set_u32("FollowUID", c.exists(tg) ? tg : c.player);
                    beh.set_f32("FollowRadius", fnum(a, "radius", 30));
                    beh.set_f32("FollowDefenseRadius", fnum(a, "radius", 30));
                    c.add(eu, std::move(beh));
                } else {  // attack
                    if (!c.proto("MAAttack", beh)) continue;
                    uint32_t tg = unum(a, "target");
                    if (c.exists(tg)) { beh.set_u32("AttackUID", tg); beh.set_u32("AttackPlayer", 1); }
                    else { beh.set_u32("AttackUID", c.player); beh.set_u32("AttackPlayer", 2); }
                    beh.set_u32("AttackSet", 0);
                    beh.set_f32("AttackTimeout", 9999);  // as set_order_kind: keep attacking
                    c.add(eu, std::move(beh));
                }
                continue;  // AI orders carry no action object
            } else {
                c.warning(base + ": unknown effect " + e.kind);
                continue;
            }
            // "[ed] ": rebuilt on every save, also when it hangs under a trigger box of the map.
            o.set_str("Description", std::string(kEditorPrefix) + "Action - " + ascii(base) + " - " + e.kind);
            c.add(main, std::move(o));
            (void)cls;
        }
    }
}

// ----------------------------------------------------------------------------- native triggers

const char* kTriggerGroup = "Triggers";

WObj* object_by_uid(Wdf& w, uint32_t uid) { return find_obj(w, uid); }
const WObj* object_by_uid(const Wdf& w, uint32_t uid) { return find_obj(const_cast<Wdf&>(w), uid); }

uint32_t gen_add_trigger(Wdf& w, const char* cls, const std::string& name, uint32_t parent) {
    auto it = g_proto.find(cls);
    if (it == g_proto.end()) return 0;
    if (!parent) {
        uint32_t next = next_uid(w);
        parent = ensure_group(w, root_uid(w), kTriggerGroup, next);
    }
    WObj o = it->second;
    o.set_str("Description", ascii_name(name));
    if (o.find("TriggerEnabled")) o.set_i32("TriggerEnabled", 1);
    if (o.find("TriggerAutoReset")) o.set_i32("TriggerAutoReset", 0);
    std::string c = cls;
    if (c == "MATriggerTimer") {
        o.set_f32("TrgTimerDurationSeconds", 0);
        o.set_f32("TrgTimerDuration", 0);
        o.set_f32("TrgTimerStart", 0);
    } else if (c == "MATriggerDestroyed") {
        o.set_i32("TypeOfCheck", 1);
        o.set_u32("DestroySet", 0);
        o.set_i32("CombineMethod", 2);
        o.set_i32("AllowJacking", 1);
        o.set_u32("DamageLevel", 0);
    } else if (c == "MATriggerLogic") {
        o.set_i32("LogicType", 0);
        o.set_u32("Inverse", 0);
    }
    uint32_t uid = next_uid(w);
    o.set_u32("UID", uid);
    wedit::append_child(w, parent, std::move(o));
    return uid;
}

uint32_t gen_add_proto(Wdf& w, const char* cls, const std::string& name, uint32_t parent) {
    auto it = g_proto.find(cls);
    if (it == g_proto.end() || !find_obj(w, parent)) return 0;
    WObj o = it->second;
    o.set_str("Description", ascii_name(name));
    uint32_t uid = next_uid(w);
    o.set_u32("UID", uid);
    wedit::append_child(w, parent, std::move(o));
    return uid;
}

uint32_t add_trigger_object(Scenario& s, const char* cls, const std::string& name, uint32_t parent) {
    uint32_t uid = gen_add_trigger(s.world, cls, name, parent);
    if (uid) s.dirty = true;
    return uid;
}

uint32_t gen_add_action(Wdf& w, uint32_t trigger_uid, const char* cls, const std::string& name) {
    auto it = g_proto.find(cls);
    if (it == g_proto.end() || !find_obj(w, trigger_uid)) return 0;
    WObj o = it->second;
    o.set_str("Description", ascii_name(name));
    std::string c = cls;
    if (c == "MAActionBroadcast") {
        o.set_u32("BodyTextID", 0);  // add_action_object / the caller gives it a text
        o.set_u32("CaptionTextID", 0);
        o.set_f32("TimeDisplayedFor", 6);
        o.set_i32("PauseGame", 0);
    } else if (c == "MAActionMissionVariableSet") {
        o.set_i32("MissionVariableSet", 0);
        o.set_i32("VariableState", 1);
    } else if (c == "MAActionGameCondition") {
        o.set_i32("GameCondition", 2);
    } else if (c == "MAActionShow") {
        o.set_i32("Enable", 1);
        o.set_u32("ActionEntity", 0);
        o.set_u32("AISet", 0);
        o.set_i32("EnableTree", 1);
    } else if (c == "MAActionRemoveEntity") {
        o.set_u32("ActionEntity", 0);
        o.set_u32("ActionSet", 0);
        o.set_i32("ShowKillEffect", 0);
    } else if (c == "MAActionAI") {
        o.set_u32("ActionEntity", 0);
        o.set_u32("AISet", 0);
        o.set_i32("EnableAI", 1);
    } else if (c == "MAActionActivateCinema") {
        o.set_u32("cinemaID", 0);
    } else if (c == "MAActionTeleport") {
        o.set_u32("MarkerUID", 0);
        o.set_f32("Heading", 0);
    } else if (c == "MAActionNation") {
        o.set_u32("ActionEntity", 0);
        o.set_u32("AISet", 0);
        o.set_i32("Nation", 5);
    } else if (c == "MAActionCheckPointSave") {
        o.set_str("suffix", "cp1");
    } else if (c == "MAActionMusic") {
        o.set_u32("IntensityLevel", 3000);
    }
    uint32_t uid = next_uid(w);
    o.set_u32("UID", uid);
    wedit::append_child(w, trigger_uid, std::move(o));
    return uid;
}

uint32_t add_action_object(Scenario& s, uint32_t trigger_uid, const char* cls, const std::string& name) {
    uint32_t uid = gen_add_action(s.world, trigger_uid, cls, name);
    if (!uid) return 0;
    if (std::string(cls) == "MAActionBroadcast")
        if (WObj* o = find_obj(s.world, uid)) o->set_u32("BodyTextID", add_text(s, "New message"));
    s.dirty = true;
    return uid;
}

std::vector<uint32_t> objective_targets(const Wdf& w, int objective) {
    static const uint32_t K_OBJT = key_of("MAObjectiveType");
    std::vector<uint32_t> out;
    for (auto& o : w.objs)
        if (o.cls == K_OBJT && o.i32("ObjectiveNumber") == objective && o.parent() &&
            std::find(out.begin(), out.end(), o.parent()) == out.end()) out.push_back(o.parent());
    return out;
}

uint32_t add_objective_target(Scenario& s, uint32_t unit_uid, int objective) {
    auto it = g_proto.find("MAObjectiveType");
    const WObj* u = find_obj(s.world, unit_uid);
    if (it == g_proto.end() || !u) return 0;
    auto have = objective_targets(s.world, objective);
    if (std::find(have.begin(), have.end(), unit_uid) != have.end()) return 0;
    WObj o = it->second;
    o.set_str("Description", "Objective " + std::to_string(objective + 1));
    o.set_i32("ObjectiveNumber", objective);
    o.set_i32("ObjectiveType", 0);
    uint32_t uid = next_uid(s.world);
    o.set_u32("UID", uid);
    wedit::append_child(s.world, unit_uid, std::move(o));
    s.dirty = true;
    return uid;
}

void sync_reach_markers(Scenario& s) {
    static const uint32_t K_NULLC = key_of("MACorporeal"), K_VOL = key_of("MATriggerCuboidalVolume"), K_RAD = key_of("MATriggerRadius");
    const size_t n = strlen(kReachMarkerSuffix);
    for (auto& m : s.world.objs) {
        if (m.cls != K_NULLC) continue;
        const std::string d = m.description();
        if (d.size() <= n || d.compare(d.size() - n, n, kReachMarkerSuffix) != 0) continue;
        const std::string shape = d.substr(0, d.size() - n);
        for (auto& t : s.world.objs) {
            Vec3 p, q;
            if ((t.cls != K_RAD && t.cls != K_VOL) || t.description() != shape || !get_pos(t, p) || !get_pos(m, q)) continue;
            if (p.x != q.x || p.z != q.z) { set_pos(m, {p.x, s.terrain.height_at(p.x, p.z), p.z}); s.dirty = true; }
            break;
        }
    }
}

int remove_objective_targets(Scenario& s, int objective, uint32_t unit_uid) {
    static const uint32_t K_OBJT = key_of("MAObjectiveType");
    std::vector<uint32_t> victims;
    for (auto& o : s.world.objs)
        if (o.cls == K_OBJT && o.i32("ObjectiveNumber") == objective && (!unit_uid || o.parent() == unit_uid)) victims.push_back(o.uid());
    for (uint32_t v : victims) wedit::remove_subtree(s.world, v);
    if (!victims.empty()) s.dirty = true;
    return int(victims.size());
}

static std::string slot_key(const char* fmt, int i, int j = 0) {
    char k[64];
    snprintf(k, sizeof k, fmt, i, j);
    return k;
}

ObjectiveSlot objective_slot(const Scenario& s, int i) {
    ObjectiveSlot o;
    int ri = find_class(s.world, K_ROOT);
    if (ri < 0) return o;
    const WObj& r = s.world.objs[size_t(ri)];
    if (r.find(slot_key("ob%dvar0", i))) o.var = r.i32(slot_key("ob%dvar0", i), -1);
    uint32_t t = r.u32(slot_key("ObjectiveItemDesc%d", i));
    bool same = i < int(s.objectives.size()) && s.objectives[size_t(i)] == r.str(slot_key("ObjectiveEditorName%d", i));  // line not retyped
    if (same && t && t < kTextBase) o.game_text = t;
    // A retail step with no text (Mission_03 "Intro Over", "JG Blank 2"): completed by triggers, never shown.
    o.hidden = same && !t && s.objectives[size_t(i)] != "Objective " + std::to_string(i);
    return o;
}

int objective_var(const Scenario& s, int i) {
    int v = objective_slot(s, i).var;
    return v >= 0 ? v : i;
}

static int free_objective_var(const Wdf& w, const WObj& r, int i) {
    // The game has 30 mission variables (0..29, one byte each) shared by objectives, triggers that set them and
    // triggers/cameras gated on them. A retail copy already uses many (Mission_01b: 16 objective slots + others),
    // so the slot index may belong to someone else: take the first variable nothing uses, the slot index first.
    bool used[30] = {};
    auto mark = [&](int n) { if (n >= 0 && n < 30) used[n] = true; };
    static const uint32_t K_VSET = key_of("MAActionMissionVariableSet"), K_MVAR = key_of("MissionVariable");
    for (auto& o : w.objs) {
        if (o.cls == K_VSET) mark(o.i32("MissionVariableSet", -1));
        if (const WProp* p = o.find(K_MVAR); p && p->numeric()) mark(int(p->as_float()));
    }
    for (int j = 0; j < 30; j++)
        for (int n = 0; n < 10; n++)
            if (j != i && r.find(slot_key("ob%dvar%d", j, n))) mark(r.i32(slot_key("ob%dvar%d", j, n), -1));
    int pick = i;
    if (used[i]) {
        for (int n = 1; n < 30; n++)  // 0 is settable but never gates a trigger: keep it last
            if (!used[n]) { pick = n; break; }
        if (pick == i && !used[0]) pick = 0;
    }
    return pick;
}

int claim_objective_var(Scenario& s, int i) {
    int ri = find_class(s.world, K_ROOT);
    if (ri < 0 || i < 0 || i >= 30) return i;
    WObj& r = s.world.objs[size_t(ri)];
    std::string k = slot_key("ob%dvar0", i);
    int v = r.find(k) ? r.i32(k, -1) : -1;
    // Retail roots fill their unused slots with variable 0 (Mission_14 slot 10): that is no claim.
    if (v == 0 && i > 0 && !r.u32(slot_key("ObjectiveItemDesc%d", i))) v = -1;
    if (v >= 0) return v;
    int pick = free_objective_var(s.world, r, i);
    r.set_i32(k, pick);
    return pick;
}

void load_retail_objectives(Scenario& s) {
    int ri = find_class(s.world, K_ROOT);
    if (ri < 0 || s.multiplayer) return;
    const WObj& r = s.world.objs[size_t(ri)];
    int n = 0;
    for (int i = 0; i < 30; i++)
        if (r.u32(slot_key("ObjectiveItemDesc%d", i))) n = i + 1;
    s.objectives.clear();
    for (int i = 0; i < n; i++) {
        std::string name = r.str(slot_key("ObjectiveEditorName%d", i));
        s.objectives.push_back(name.empty() ? "Objective " + std::to_string(i + 1) : name);
    }
}

void objective_removed(Scenario& s, int objective) {
    static const uint32_t K_OBJT = key_of("MAObjectiveType");
    remove_objective_targets(s, objective);
    // Later MARoot slots move up one (each keeps its variable, text and radar list); the last slot is cleared.
    if (int ri = find_class(s.world, K_ROOT); ri >= 0) {
        WObj& r = s.world.objs[size_t(ri)];
        static const char* kOne[] = {"ObjectiveItemDesc%d", "ObjectiveItemText%d", "ObjectiveEnabledBool%d", "ObjectiveEditorName%d",
                                     "ObjectiveType%d", "dwVoiceCueID%d", "ObjectiveImage%d", "NumObjectiveVars%d", "NumObjectiveObjects%d"};
        auto move = [&](const std::string& from, const std::string& to) {
            const WProp* f = r.find(from);
            if (f) { WProp c = *f; r.set(key_of(to), c.type, c.data); }
            else r.erase(key_of(to));
        };
        for (int i = objective; i < 29; i++) {
            for (const char* f : kOne) move(slot_key(f, i + 1), slot_key(f, i));
            for (int j = 0; j < 10; j++) move(slot_key("ob%dvar%d", i + 1, j), slot_key("ob%dvar%d", i, j));
            for (int j = 0; j < 32; j++) move(slot_key("ob%dobj%d", i + 1, j), slot_key("ob%dobj%d", i, j));
        }
        if (r.find(slot_key("ob%dvar0", 29))) r.set_i32(slot_key("ob%dvar0", 29), -1);
        if (r.find(slot_key("ObjectiveItemDesc%d", 29))) r.set_u32(slot_key("ObjectiveItemDesc%d", 29), 0);
        if (r.find(slot_key("ObjectiveEnabledBool%d", 29))) r.set_i32(slot_key("ObjectiveEnabledBool%d", 29), 0);
    }
    for (auto& o : s.world.objs)
        if (o.cls == K_OBJT) {
            int k = o.i32("ObjectiveNumber");
            if (k > objective) {
                o.set_i32("ObjectiveNumber", k - 1);
                o.set_str("Description", "Objective " + std::to_string(k));
            }
        }
}

std::vector<uint32_t> objective_losers(const Wdf& w, int objective) {
    static const uint32_t K_DEST = key_of("MATriggerDestroyed"), K_COND = key_of("MAActionGameCondition");
    auto targets = objective_targets(w, objective);
    std::vector<uint32_t> out;
    if (targets.empty()) return out;
    for (auto& t : w.objs) {
        if (t.cls != K_DEST) continue;
        bool lose = false, watches = false;
        for (auto& c : w.objs)
            if (c.parent() == t.uid() && c.cls == K_COND && c.i32("GameCondition") == 1) lose = true;
        if (!lose) continue;
        for (uint32_t r : reference_children(w, t.uid()))
            if (const WObj* ro = object_by_uid(w, r))
                watches |= std::find(targets.begin(), targets.end(), ro->u32("ReferenceUID")) != targets.end();
        if (watches) out.push_back(t.uid());
    }
    return out;
}

std::vector<uint32_t> objective_completers(const Wdf& w, int var, const std::function<bool(uint32_t)>& is_trigger_cls) {
    static const uint32_t K_SET = key_of("MAActionMissionVariableSet");
    std::vector<uint32_t> out;
    for (auto& o : w.objs) {
        if (o.cls != K_SET || o.i32("MissionVariableSet") != var || o.i32("VariableState") == 0) continue;
        uint32_t p = o.parent();
        for (int n = 0; p && n < 32; n++) {
            const WObj* x = object_by_uid(w, p);
            if (!x) { p = 0; break; }
            if (is_trigger_cls(x->cls)) break;
            p = x->parent();
        }
        if (p && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

uint32_t gen_add_reference(Wdf& w, uint32_t parent, uint32_t target) {
    auto it = g_proto.find("MoeEntityReference");
    if (it == g_proto.end() || !find_obj(w, parent)) return 0;
    WObj r = it->second;
    const WObj* t = find_obj(w, target);
    r.set_str("Description", ascii_name(t ? t->description() : std::string("input")) + " (ref)");
    r.set_u32("ReferenceUID", target);
    uint32_t uid = next_uid(w);
    r.set_u32("UID", uid);
    wedit::append_child(w, parent, std::move(r));
    return uid;
}

uint32_t add_reference(Scenario& s, uint32_t parent, uint32_t target) {
    uint32_t uid = gen_add_reference(s.world, parent, target);
    if (uid) s.dirty = true;
    return uid;
}

std::vector<uint32_t> reference_children(const Wdf& w, uint32_t uid) {
    static const uint32_t K_REF = key_of("MoeEntityReference");
    std::vector<uint32_t> out;
    for (auto& o : w.objs) if (o.cls == K_REF && o.parent() == uid) out.push_back(o.uid());
    return out;
}

uint32_t add_text(Scenario& s, const std::string& text) {
    int n = kStrMessage;
    for (;; n++) {
        if (n >= kStrGenerated && n < kStrGeneratedEnd) n = kStrGeneratedEnd;  // the export's own texts (mission_extras)
        if (!s.strings.count(n)) break;
    }
    s.strings[n] = text;
    s.dirty = true;
    return kTextBase + uint32_t(n);
}

std::string* own_text(Scenario& s, uint32_t id) {
    if (id < kTextBase || id - kTextBase > 0x7FFFFFFFu) return nullptr;
    auto it = s.strings.find(int(id - kTextBase));
    return it == s.strings.end() ? nullptr : &it->second;
}

// ----------------------------------------------------------------------------- orders

static const uint32_t K_EVENT = key_of("MAEvent"), K_AIGROUP = key_of("MAAIGroup"), K_PATROL = key_of("MAPatrol"),
                      K_DEFEND = key_of("MADefend"), K_FOLLOW = key_of("MAFollow"), K_ATTACK = key_of("MAAttack");

bool is_order_behaviour(uint32_t cls) { return cls == K_PATROL || cls == K_DEFEND || cls == K_FOLLOW || cls == K_ATTACK; }

std::vector<uint32_t> orders_of_trigger(const Wdf& w, uint32_t trigger_uid) {
    std::vector<uint32_t> out;
    if (!trigger_uid) return out;
    for (auto& o : w.objs) if (o.cls == K_EVENT && o.u32("TriggerID") == trigger_uid) out.push_back(o.uid());
    return out;
}

uint32_t order_unit(const Wdf& w, uint32_t event_uid) {
    const WObj* e = object_by_uid(w, event_uid);
    const WObj* g = e ? object_by_uid(w, e->parent()) : nullptr;
    return g && g->cls == K_AIGROUP ? g->parent() : 0;
}

uint32_t order_behaviour(const Wdf& w, uint32_t event_uid) {
    for (auto& o : w.objs) if (o.parent() == event_uid && is_order_behaviour(o.cls)) return o.uid();
    return 0;
}

uint32_t order_path(const Wdf& w, uint32_t event_uid) {
    for (auto& o : w.objs) if (o.parent() == event_uid && o.cls == K_WPTGROUP) return o.uid();
    return 0;
}

uint32_t player_start_uid(const Wdf& w) {
    uint32_t first = 0;
    for (auto& o : w.objs)
        if (o.cls == K_LEVELSTART) {
            if (o.description() == "Player Spawner") return o.uid();
            if (!first) first = o.uid();
        }
    return first;
}

bool set_order_kind(Scenario& s, uint32_t event_uid, const char* cls) {
    Wdf& w = s.world;
    auto it = g_proto.find(cls);
    if (it == g_proto.end() || !find_obj(w, event_uid)) return false;
    if (uint32_t old = order_behaviour(w, event_uid)) wedit::remove_subtree(w, old);
    WObj b = it->second;
    std::string c = cls;
    uint32_t player = player_start_uid(w);
    if (c == "MADefend") {
        b.set_u32("DefendUID", 0);
        b.set_f32("Radius", 60);
    } else if (c == "MAFollow") {
        b.set_u32("FollowUID", player);
        b.set_f32("FollowRadius", 30);
        b.set_f32("FollowDefenseRadius", 30);
    } else if (c == "MAAttack") {
        b.set_u32("AttackUID", player);
        b.set_u32("AttackPlayer", 2);
        b.set_u32("AttackSet", 0);
        b.set_f32("AttackTimeout", 9999);  // the class default 3 s = one volley, then back to other orders (game-proven); retail's long attacks use 9999
    }
    b.set_u32("UID", next_uid(w));
    wedit::append_child(w, event_uid, std::move(b));
    s.dirty = true;
    return true;
}

uint32_t add_order(Scenario& s, uint32_t trigger_uid, uint32_t unit_uid, const char* behaviour_cls) {
    Wdf& w = s.world;
    WObj* trig = find_obj(w, trigger_uid);
    WObj* unit = find_obj(w, unit_uid);
    if (!trig || !unit || !g_proto.count("MAAIGroup") || !g_proto.count("MAEvent")) return 0;
    uint32_t grp = 0;
    for (auto& o : w.objs) if (o.parent() == unit_uid && o.cls == K_AIGROUP) { grp = o.uid(); break; }
    if (!grp) {
        WObj g = g_proto["MAAIGroup"];
        g.set_str("Description", "AI Orders");
        grp = next_uid(w);
        g.set_u32("UID", grp);
        wedit::append_child(w, unit_uid, std::move(g));
    }
    WObj ev = g_proto["MAEvent"];
    // Retail names: "AI Event", then "AI Event_00000", "AI Event_00001"... under the same group.
    int same = 0;
    for (auto& o : w.objs) if (o.parent() == grp && o.description().compare(0, 8, "AI Event") == 0) same++;
    char evname[32];
    if (same) snprintf(evname, sizeof evname, "AI Event_%05d", same - 1);
    else snprintf(evname, sizeof evname, "AI Event");
    ev.set_str("Description", evname);
    ev.set_u32("TriggerID", trigger_uid);
    ev.set_u32("Priority", 2);
    uint32_t eu = next_uid(w);
    ev.set_u32("UID", eu);
    wedit::append_child(w, grp, std::move(ev));
    set_order_kind(s, eu, behaviour_cls);
    // A unit whose AI starts off ignores its orders: as retail does, the same trigger wakes it (MAActionAI EnableAI 1).
    const WObj* u = find_obj(w, unit_uid);
    if (u && u->find("m_bAIEnabled") && u->i32("m_bAIEnabled") == 0) {
        bool woken = false;
        for (auto& o : w.objs)
            if (o.parent() == trigger_uid && o.cls == key_of("MAActionAI") && o.u32("ActionEntity") == unit_uid && o.i32("EnableAI") == 1) woken = true;
        if (!woken)
            if (uint32_t a = add_action_object(s, trigger_uid, "MAActionAI", "Wake " + u->description())) {
                WObj* ao = find_obj(w, a);
                ao->set_u32("ActionEntity", unit_uid);
                ao->set_i32("EnableAI", 1);
            }
    }
    return eu;
}

void order_add_point(Scenario& s, uint32_t event_uid, float x, float z) {
    uint32_t g = order_path(s.world, event_uid);
    if (!g) {
        const WObj* u = find_obj(s.world, order_unit(s.world, event_uid));
        g = add_ai_path(s, {}, (u ? u->description() : std::string("Unit")) + " - patrol path");
        if (!g) return;
        if (WObj* go = find_obj(s.world, g)) go->set_i32("TraverseType", 2);  // a patrol loops (1 = walk the route once, then stand)
        wedit::reparent(s.world, g, event_uid);
    }
    add_waypoint(s, g, x, z);
    s.dirty = true;
}

void order_clear_path(Scenario& s, uint32_t event_uid) {
    if (uint32_t g = order_path(s.world, event_uid)) wedit::remove_subtree(s.world, g);
    s.dirty = true;
}

bool camera_path_point(Scenario& s, uint32_t path_uid, float x, float z, bool restart) {
    static const uint32_t K_PATH = key_of("MAPath"), K_WPG = key_of("MAWaypointGroup");
    detach_path_group(s.world, path_uid);  // The drawn points alone are the game's path
    const WObj* p = find_obj(s.world, path_uid);
    uint32_t g = p && p->cls == K_PATH ? p->u32("m_wptGroupUID") : 0;
    WTree t;
    t.build(s.world);
    int gi = t.find(g);
    if (gi < 0 || s.world.objs[size_t(gi)].cls != K_WPG || t.kids[size_t(gi)].empty()) return false;
    std::vector<uint32_t> pts;
    for (int k : t.kids[size_t(gi)]) pts.push_back(s.world.objs[size_t(k)].uid());
    auto above = [&](uint32_t u) {  // the point's height above the ground under it
        Vec3 q{};
        if (const WObj* o = find_obj(s.world, u)) get_pos(*o, q);
        return q.y - s.terrain.height_at(q.x, q.z);
    };
    float y = s.terrain.height_at(x, z);
    if (restart) {
        y += above(pts[0]);
        for (size_t i = 1; i < pts.size(); i++) wedit::remove_subtree(s.world, pts[i]);
        if (WObj* o = find_obj(s.world, pts[0])) set_pos(*o, {x, y, z});
        if (WObj* go = find_obj(s.world, g)) go->set_u32("WaypointID", pts[0]);  // first == last now
    } else {
        y += above(pts.back());
        const WObj* last = find_obj(s.world, pts.back());
        if (!last) return false;
        WObj c = *last;
        uint32_t uid = next_uid(s.world);
        c.set_u32("UID", uid);
        set_pos(c, {x, y, z});
        wedit::append_child(s.world, g, std::move(c));
        // Retail cinema path groups name their LAST waypoint (cinema-path-pattern); keep that when it was so.
        if (WObj* go = find_obj(s.world, g); go && go->u32("WaypointID") == pts.back()) go->set_u32("WaypointID", uid);
    }
    s.dirty = true;
    return true;
}

uint32_t set_marker_link(Scenario& s, uint32_t owner_uid, const char* key, float x, float z, bool null_marker, const std::string& name) {
    WObj* o = find_obj(s.world, owner_uid);
    if (!o) return 0;
    uint32_t m = o->u32(key);
    if (WObj* mo = m ? find_obj(s.world, m) : nullptr) {
        Vec3 p;
        get_pos(*mo, p);
        float dy = s.terrain.height_at(x, z) - s.terrain.height_at(p.x, p.z);
        set_pos(*mo, {x, p.y + dy, z});
    } else {
        m = add_ai_marker(s, x, z, name, null_marker);
        o = find_obj(s.world, owner_uid);  // the vector may have grown
        if (!m || !o) return 0;
        o->set_u32(key, m);
    }
    s.dirty = true;
    return m;
}

// ----------------------------------------------------------------------------- legacy conversion

static void strip_editor_names(Wdf& w) {
    for (auto& o : w.objs) {
        std::string d = o.description();
        if (o.cls == K_GROUP && d == kEditorTriggerGroup) { o.set_str("Description", kTriggerGroup); continue; }
        if (d.compare(0, strlen(kEditorPrefix), kEditorPrefix) != 0) continue;
        d = d.substr(strlen(kEditorPrefix));
        if (d.compare(0, 9, "Action - ") == 0) {  // "Action - <trigger> - <kind>": the kind's plain name
            size_t cut = d.rfind(" - ");
            std::string kind = cut == std::string::npos ? std::string() : d.substr(cut + 3);
            if (const TriggerKind* k = find_effect_kind(kind)) d = d.substr(9, cut - 9) + " - " + k->label;
            else d = d.substr(9);
        }
        o.set_str("Description", d);
    }
}

int convert_legacy_triggers(Scenario& s) {
    bool compiled = false;
    for (auto& o : s.world.objs) {
        std::string d = o.description();
        if ((o.cls == K_GROUP && d == kEditorTriggerGroup) || d.compare(0, strlen(kEditorPrefix), kEditorPrefix) == 0) { compiled = true; break; }
    }
    int n = int(s.triggers.size());
    if (!n && !compiled) return 0;
    if (n) {
        if (!g_ready) return -1;  // prototypes not loaded yet: the caller loads them and tries again
        adopt_trigger_shapes(s);
        adopt_ai_objects(s);
        std::map<int, std::string> extra;
        compile_triggers(s, s.world, nullptr, &extra);
        for (auto& [k, t] : extra) if (k >= kStrMessage) s.strings[k] = t;  // message texts are now the map's own strings
        s.triggers.clear();
    }
    strip_editor_names(s.world);
    s.dirty = true;
    return n ? n : 1;
}

std::vector<UsedBy> used_by(const Wdf& w, uint32_t uid, const std::function<bool(uint32_t)>& is_trigger_cls) {
    static const uint32_t K_SET = key_of("MoeEntitySet");
    std::vector<UsedBy> out;
    if (!uid) return out;
    static const char* kActionKeys[] = {"ActionEntity", "MarkerUID", "cinemaID"};
    static const char* kTargetKeys[] = {"DefendUID", "AttackUID", "FollowUID"};
    static const char* kSetKeys[] = {"RadiusSet", "EntitySetUID", "DestroySet", "AISet", "ActionSet"};
    auto trigger_of = [&](const WObj& o) {
        uint32_t p = o.uid();
        for (int n = 0; p && n < 32; n++) {
            const WObj* x = object_by_uid(w, p);
            if (!x) break;
            if (x->uid() != uid && is_trigger_cls(x->cls)) return p;
            p = x->parent();
        }
        return uint32_t(0);
    };
    for (auto& o : w.objs) {
        if (o.uid() == uid) continue;
        if (o.cls == K_EVENT) {
            uint32_t unit = order_unit(w, o.uid());
            if (o.u32("TriggerID") == uid) out.push_back({UsedBy::Starts, o.uid(), uid, unit, ""});
            if (unit == uid) out.push_back({UsedBy::OrderOf, o.uid(), o.u32("TriggerID"), unit, ""});
            continue;
        }
        if (is_order_behaviour(o.cls)) {
            for (const char* k : kTargetKeys)
                if (o.find(k) && o.u32(k) == uid) {
                    uint32_t ev = o.parent();
                    const WObj* e = object_by_uid(w, ev);
                    out.push_back({UsedBy::OrderTarget, o.uid(), e ? e->u32("TriggerID") : 0, order_unit(w, ev), k});
                }
            continue;
        }
        if (o.cls == K_SET) {
            int n = o.i32("MoeEntitySetCount");
            for (int k = 0; k < n; k++)
                if (o.u32("MoeEntitySetMember" + std::to_string(k)) == uid) { out.push_back({UsedBy::Member, o.uid(), 0, 0, ""}); break; }
        }
        if (o.find("dwEntitySwapSet") && o.u32("dwEntitySwapSet") == uid) out.push_back({UsedBy::SwapOf, o.uid(), 0, 0, "dwEntitySwapSet"});
        if (o.find("dwSwapSet") && o.u32("dwSwapSet") == uid) out.push_back({UsedBy::Action, o.uid(), trigger_of(o), 0, "dwSwapSet"});
        if (o.find("ReferenceUID") && o.u32("ReferenceUID") == uid) out.push_back({UsedBy::Input, o.uid(), o.parent(), 0, "ReferenceUID"});
        for (const char* k : kActionKeys)
            if (o.find(k) && o.u32(k) == uid) out.push_back({UsedBy::Action, o.uid(), trigger_of(o), 0, k});
        // Unit sets held by trigger shapes (RadiusSet, EntitySetUID), destroyed-triggers (DestroySet) and actions (AISet, ActionSet).
        for (const char* k : kSetKeys)
            if (o.find(k) && o.u32(k) == uid) out.push_back({UsedBy::Action, o.uid(), trigger_of(o), 0, k});
        if (o.find("m_wptGroupUID") && o.u32("m_wptGroupUID") == uid) out.push_back({UsedBy::Ref, o.uid(), trigger_of(o), 0, "m_wptGroupUID"});
        if (o.find("TriggerEnableID") && o.u32("TriggerEnableID") == uid) out.push_back({UsedBy::Enables, o.uid(), uid, 0, "TriggerEnableID"});
    }
    return out;
}

}  // namespace ma2
