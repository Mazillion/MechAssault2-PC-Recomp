#include "scenario.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <regex>
#include <set>

#include "../app/version.h"
#include "crop.h"
#include "json.h"
#include "ma2models.h"
#include "triggers.h"
#include "water_custom.h"
#include "weather.h"

namespace ma2 {

// ----------------------------------------------------------------------------- game types

std::vector<char> objects_in_game_type(const Wdf& w, const WTree& t, const GameTypeDef& g) {
    const uint32_t mtg = key_of("MAMissionTypeGroup");
    std::vector<char> in(w.objs.size(), 1);
    // Parents are not necessarily stored before their children, so resolve each object by walking up.
    for (size_t i = 0; i < w.objs.size(); i++)
        for (int p = t.parent_of[i]; p >= 0; p = t.parent_of[size_t(p)]) {
            const WObj& a = w.objs[size_t(p)];
            if (a.cls != mtg) continue;
            const WProp* f = a.find(g.flag);
            if (!f || !f->as_int()) { in[i] = 0; break; }
        }
    return in;
}

std::vector<char> objects_at_start(const Wdf& w, const WTree& t) {
    const uint32_t k_show = key_of("MAActionShow"), k_remove = key_of("MAActionRemoveEntity"), k_timer = key_of("MATriggerTimer"), k_spawner = key_of("MASpawner"), k_collect = key_of("MACollectionArea"),
                   k_swap = key_of("MASwapSet"), k_cinema = key_of("MACinema"),
                   k_activate = key_of("MAActionActivateCinema");
    std::vector<char> st(w.objs.size(), START_SHOWN);
    auto mark = [&](int i, bool tree, char v) {
        if (i < 0) return;
        std::vector<int> sub;
        if (tree) t.subtree(i, sub); else sub.push_back(i);
        for (int k : sub) if (st[size_t(k)] == START_SHOWN) st[size_t(k)] = v;
    };
    // A spawner's units stay out of the world until it spawns them (live entity dump: no scene node);
    // so does a collection area's cargo until it is delivered (Mission_11b "Cargo Collection Area").
    for (size_t i = 0; i < w.objs.size(); i++)
        if (w.objs[i].cls == k_spawner || w.objs[i].cls == k_collect)
            for (int c : t.kids[i]) mark(c, true, START_SPAWNED);
    // Set membership: an object lists the sets it belongs to (SetCount, SetMember0..).
    std::unordered_map<uint32_t, std::vector<int>> members;
    for (size_t i = 0; i < w.objs.size(); i++) {
        int n = w.objs[i].i32("SetCount");
        for (int k = 0; k < n && k < 64; k++)
            if (uint32_t s = w.objs[i].u32("SetMember" + std::to_string(k))) members[s].push_back(int(i));
    }
    // An enabled timer that fires at once (the missions' SETUP timers).
    auto start_timer = [&](int p) {
        if (p < 0) return false;
        const WObj& tr = w.objs[size_t(p)];
        return tr.cls == k_timer && tr.i32("TriggerEnabled") && tr.f32("TrgTimerStart") <= 1.f &&
               std::max(tr.f32("TrgTimerDuration"), tr.f32("TrgTimerDurationSeconds")) <= 1.f;
    };
    // A swap set's units (MASwapSet dwEntitySwapSet, e.g. Mission_06b "Uller swap dummy") are hidden
    // until the set is swapped in (bSwappedIn = 1: Mission_06b "nat and fos" starts swapped in).
    for (size_t i = 0; i < w.objs.size(); i++)
        if (w.objs[i].cls == k_swap && !w.objs[i].i32("bSwappedIn"))
            if (auto it = members.find(w.objs[i].u32("dwEntitySwapSet")); it != members.end())
                for (int m : it->second) mark(m, true, START_HIDDEN);
    // "Hide" and "Remove entity" actions of an enabled timer that fires at once (the missions' SETUP
    // timers): the game hides those objects before the first frame (entity flag 0x2000 in the live dump).
    for (size_t i = 0; i < w.objs.size(); i++) {
        const WObj& a = w.objs[i];
        bool show = a.cls == k_show && a.i32("Enable", 1) == 0, remove = a.cls == k_remove;
        if (!show && !remove) continue;
        if (!start_timer(t.parent_of[i])) continue;
        // Remove Entity: the object is gone; with the kill effect a building is left as its
        // DestroyedBuilding template instead (Mission_03 "tunnel end": live entity of that template).
        auto hit = [&](int m) {
            if (m < 0) return;
            if (show) { mark(m, a.i32("EnableTree", 1) != 0, START_HIDDEN); return; }
            bool wreck = a.i32("ShowKillEffect") && w.objs[size_t(m)].u32("DestroyedBuilding");
            mark(m, !wreck, wreck ? START_DESTROYED : START_REMOVED);
        };
        if (uint32_t e = a.u32("ActionEntity")) hit(t.find(e));
        if (uint32_t s = a.u32(show ? "AISet" : "ActionSet")) {
            auto it = members.find(s);
            if (it != members.end()) for (int m : it->second) hit(m);
        }
    }
    // Cutscene-only objects: the game removes them when their cinema ends (Mission_03 "Pilot - Ducker",
    // Mission_04 "... CINEMA" tank, Mission_09A intro Icarus and turrets). Either the object sits in a
    // MACinema and some Remove Entity action targets it, or the Remove Entity belongs to a trigger
    // inside a MACinema (its 'Triggers' group: "TG Start Cinema Over", "TG - Intro Ended").
    // Only intro cinemas (activated by a start timer): later cinemas remove units that are in play until then.
    std::set<int> intro;
    for (size_t i = 0; i < w.objs.size(); i++)
        if (w.objs[i].cls == k_activate && start_timer(t.parent_of[i])) intro.insert(t.find(w.objs[i].u32("cinemaID")));
    std::vector<char> in_cinema(w.objs.size(), 0);
    for (size_t i = 0; i < w.objs.size(); i++)
        for (int p = t.parent_of[i]; p >= 0; p = t.parent_of[size_t(p)])
            if (w.objs[size_t(p)].cls == k_cinema) { in_cinema[i] = intro.count(p) ? 1 : 0; break; }
    for (size_t i = 0; i < w.objs.size(); i++) {
        const WObj& a = w.objs[i];
        if (a.cls != k_remove || a.i32("ShowKillEffect")) continue;
        int p = t.parent_of[i];
        bool cin_trigger = p >= 0 && in_cinema[size_t(p)];
        auto hit = [&](int m) { if (m >= 0 && (cin_trigger || in_cinema[size_t(m)])) mark(m, true, START_CUTSCENE); };
        if (uint32_t e = a.u32("ActionEntity")) hit(t.find(e));
        if (auto it = members.find(a.u32("ActionSet")); it != members.end()) for (int m : it->second) hit(m);
    }
    return st;
}

const std::vector<GameTypeDef>& game_types() {
    static const std::vector<GameTypeDef> v = {
        {"TeamDestruction", "TeamDestruction", "Team Destruction", true},
        {"CTF", "CTF", "Capture the Flag", true},
        {"BaseWar", "BaseWar", "Base War", true},
        {"SnatchIt", "SnatchIt", "Snatch It", true},
        {"CheckIt", "CheckIt", "Check It", true},
        {"TeamLastManStanding", "TeamLMS", "Team Last Man Standing", true},
        {"Destruction", "Destruction", "Destruction (free for all)", false},
        {"LastManStanding", "LMS", "Last Man Standing", false},
        {"NotIt", "NotIt", "Not It", false},
        {"Grinder", "Grinder", "Grinder", false},
    };
    return v;
}

const GameTypeDef* find_game_type(std::string_view n) {
    for (auto& g : game_types()) if (iequals(g.ini, n) || iequals(g.flag, n)) return &g;
    return nullptr;
}

// ----------------------------------------------------------------------------- Scenario

std::string Scenario::base_level() const {
    std::string b = lower(base);
    if (iends_with(b, ".mgf")) b.resize(b.size() - 4);
    return b;
}
std::string Scenario::id_hex() const { char b[16]; snprintf(b, sizeof b, "%08X", id); return b; }
std::string Scenario::size_text() const { return std::to_string(terrain.w) + "x" + std::to_string(terrain.h); }

// ----------------------------------------------------------------------------- world helpers

namespace world {
const uint32_t K_ROOT = key_of("MARoot"), K_ENV = key_of("MAEnvironment"), K_TERRAIN = key_of("MATerrain"),
               K_TINFO = key_of("MATerrainInfo"), K_SKY = key_of("MASky2"), K_GROUP = key_of("MAGroup"),
               K_MTG = key_of("MAMissionTypeGroup"), K_LEVELSTART = key_of("MALevelStart"), K_PATHDB = key_of("MAPathDB"),
               K_WATER = key_of("MAWater");
static const uint32_t kPosKeys[] = {key_of("CorporalPos"), key_of("ObjPosition"), key_of("TriggerPoint"),
                                    key_of("RoadPosition"), key_of("Position"), key_of("CameraPosition")};
static const uint32_t kHeadKeys[] = {key_of("CorporalHeading"), key_of("StartOrientation"), key_of("VolumeHeading")};

WProp* position(WObj& o) {
    for (uint32_t k : kPosKeys)
        if (WProp* p = o.find(k)) if (p->type == PT_VEC3 && p->data.size() >= 12) return p;
    return nullptr;
}
const WProp* position(const WObj& o) { return position(const_cast<WObj&>(o)); }
bool get_pos(const WObj& o, Vec3& out) {
    const WProp* p = position(o);
    if (!p) return false;
    out = p->as_vec3();
    return true;
}
void set_pos(WObj& o, Vec3 v) { if (WProp* p = position(o)) p->set_vec3(v); }
// CorporalHeading and VolumeHeading are degrees (retail: 0, 90, 180, -90...); MALevelStart's
// StartOrientation is radians (0..2pi). The editor works in radians.
static const float kDeg = 3.14159265f / 180.f;
float heading(const WObj& o) {
    // MARoad: RoadRotation's first float is the yaw in radians (the other two are 0).
    if (const WProp* p = o.find("RoadRotation")) if (p->data.size() >= 4) { float y; memcpy(&y, p->data.data(), 4); return y; }
    for (size_t i = 0; i < 3; i++)
        if (const WProp* p = o.find(kHeadKeys[i]))
            if (p->type == PT_F32) return float(p->as_float()) * (i == 1 ? 1.f : kDeg);
    return 0.f;
}
bool can_tilt(const WObj& o) {
    if (const WProp* p = o.find("RoadRotation")) return p->data.size() >= 12;
    return o.find("CorporalPitch") != nullptr;
}
void get_tilt(const WObj& o, float& pitch, float& roll) {
    pitch = roll = 0;
    if (const WProp* p = o.find("RoadRotation")) {
        if (p->data.size() >= 12) { memcpy(&pitch, p->data.data() + 4, 4); memcpy(&roll, p->data.data() + 8, 4); }
        return;
    }
    pitch = o.f32("CorporalPitch") * kDeg;
    roll = o.f32("CorporalRoll") * kDeg;
}
void set_tilt(WObj& o, float pitch, float roll) {
    if (WProp* p = o.find("RoadRotation")) {
        if (p->data.size() >= 12) { memcpy(p->data.data() + 4, &pitch, 4); memcpy(p->data.data() + 8, &roll, 4); }
        return;
    }
    if (!o.find("CorporalPitch")) return;
    auto deg = [](float r) { return std::remainder(r / kDeg, 360.f); };
    o.set_f32("CorporalPitch", deg(pitch));
    o.set_f32("CorporalRoll", deg(roll));
}
void set_heading(WObj& o, float r) {
    if (WProp* p = o.find("RoadRotation")) if (p->data.size() >= 4) { memcpy(p->data.data(), &r, 4); return; }
    for (size_t i = 0; i < 3; i++)
        if (WProp* p = o.find(kHeadKeys[i]))
            if (p->type == PT_F32) {
                if (i == 1) { float t = std::fmod(r, 6.2831853f); p->set_float(t < 0 ? t + 6.2831853f : t); }
                else { float d = std::fmod(r / kDeg, 360.f); p->set_float(d > 180 ? d - 360 : d < -180 ? d + 360 : d); }
                return;
            }
}
int find_class(const Wdf& w, uint32_t cls) {
    for (size_t i = 0; i < w.objs.size(); i++) if (w.objs[i].cls == cls) return int(i);
    return -1;
}
uint32_t root_uid(const Wdf& w) { int i = find_class(w, K_ROOT); return i >= 0 ? w.objs[size_t(i)].uid() : 0; }
uint32_t terrain_uid(const Wdf& w) { int i = find_class(w, K_TERRAIN); return i >= 0 ? w.objs[size_t(i)].uid() : 0; }
uint32_t next_uid(const Wdf& w) {
    uint32_t m = 130000;
    for (auto& o : w.objs) m = std::max(m, o.uid());
    return m + 1;
}
WObj make_group(const std::string& name, uint32_t uid) {
    // Same property set as a retail MAGroup ("Trees" in BoggedDown), sorted by key.
    WObj g;
    g.cls = K_GROUP;
    g.set_i32("IsCollidable", 0);
    g.set_str("Description", name);
    g.set_f32("ShadowDiffuseFactor", 1.f);
    g.set_u32("CreationTime", 0);
    g.set(0x2A68614F, PT_I32, Bytes(4, 0));
    g.set_i32("VisibleEntity", 1);
    g.set_i32("Nation", 0);
    g.set_u32("UID", uid);
    g.set_u32("TemplateID", 0);
    g.set_i32("MAMaxHitPoints", 1);
    g.set(key_of("OverrideTemplate"), PT_I8, Bytes{0});
    g.set_i32("m_nBattleScoreBucking", 1);
    g.set_u32("sibling", 0);
    g.set_i32("ShadowReceiverType", 0);
    g.set_i32("IsRoadOrTarmac", 0);
    g.set_i32("SetCount", 0);
    g.set_i32("MACurHitPoints", 1);
    g.set_i32("Indestructible", 1);
    g.set_i32("m_nBattleScoreJacking", 1);
    g.set_u32("parent", 0);
    g.set_u32("child", 0);
    g.set_i32("m_nBattleScore", 1);
    g.set_i32("ShadowCasterType", 0);
    return g;
}
uint32_t ensure_group(Wdf& w, uint32_t parent_uid, const std::string& name, uint32_t& next) {
    WTree t;
    t.build(w);
    int p = t.find(parent_uid);
    if (p >= 0)
        for (int k : t.kids[size_t(p)])
            if (w.objs[size_t(k)].cls == K_GROUP && w.objs[size_t(k)].description() == name) return w.objs[size_t(k)].uid();
    uint32_t uid = next++;
    wedit::append_child(w, parent_uid, make_group(name, uid));
    return uid;
}
}  // namespace world

using namespace world;

static const uint32_t K_TYPEMAP = 0x1F1646EAu, K_CULTUREMAP = 0x89FB5118u;

// ----------------------------------------------------------------------------- terrain textures

static bool decode_file(const Bytes& b, Image& img, Mgic& style, Picture& layout) {
    // The container allows 16384 squared, a 1 GB RGBA image; the editor makes at most 4096 squared.
    if (!style.parse(b) || !read_picture(style, layout) || layout.width > 4096 || layout.height > 4096 ||
        !decode_picture(layout, 0, img)) return false;
    layout.bits.clear();
    layout.bits.shrink_to_fit();
    return true;
}

static void load_typemap(Scenario& s) {
    s.typemap.clear();
    s.typemap_w = s.typemap_h = 0;
    int ti = find_class(s.world, K_TINFO);
    if (ti < 0) return;
    const WProp* p = s.world.objs[size_t(ti)].find(K_TYPEMAP);
    if (!p) return;
    Picture pic;
    if (!read_picture(p->data, pic) || pic.dxt() || pic.swizzled() || pic.format() != PF_ARGB4444) return;
    s.typemap_w = int(pic.width);
    s.typemap_h = int(pic.height);
    s.typemap.resize(size_t(pic.width) * pic.height);
    std::memcpy(s.typemap.data(), pic.bits.data(), std::min(pic.bits.size(), s.typemap.size() * 2));
}

static void store_typemap(const std::vector<uint16_t>& tm, Wdf& w) {
    if (tm.empty()) return;
    int ti = find_class(w, K_TINFO);
    if (ti < 0) return;
    WProp* p = w.objs[size_t(ti)].find(K_TYPEMAP);
    if (!p) return;
    Mgic m;
    if (!m.parse(p->data)) return;
    MgicSection* pic = m.section(fourcc("PIC "));
    if (!pic) return;
    MgicChunk* bits = pic->chunk(fourcc("bits"));
    if (!bits || bits->data.size() < tm.size() * 2) return;
    std::memcpy(bits->data.data(), tm.data(), tm.size() * 2);
    p->data = m.serialize();
}

const char* tmtype::label(uint16_t v) {
    switch (v) {
        case Playable: return "Playable";
        case OutOfBounds: return "Out of bounds";
        case Border: return "Border";
        case Special: return "Water/special";
        case DeepWater: return "Deep water";
        default: return nullptr;
    }
}

int typemap_at(const Scenario& s, float x, float z) {
    if (s.typemap.empty() || s.typemap_w <= 0 || s.typemap_h <= 0) return -1;
    int tx = std::clamp(int(x / s.terrain.size_x() * float(s.typemap_w)), 0, s.typemap_w - 1);
    int tz = std::clamp(int(z / s.terrain.size_z() * float(s.typemap_h)), 0, s.typemap_h - 1);  // row 0 = z 0
    return s.typemap[size_t(tz) * s.typemap_w + tx];
}

void fill_playable_typemap(Scenario& s, int margin, int band) {
    int W = s.typemap_w, H = s.typemap_h;
    if (s.typemap.empty() || W <= 0 || H <= 0) return;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int d = std::min(std::min(x, W - 1 - x), std::min(y, H - 1 - y));
            s.typemap[size_t(y) * W + x] = d < margin ? tmtype::OutOfBounds : d < margin + band ? tmtype::Border : tmtype::Playable;
        }
    s.typemap_dirty = true;
}

void ScenarioIO::load_terrain_textures(Scenario& s) {
    MgfReader* base = gd_.archive(s.base);
    int ti = find_class(s.world, K_TERRAIN), ei = find_class(s.world, K_ENV);
    std::string cpath = ti >= 0 ? s.world.objs[size_t(ti)].str("TerrainBaseMap") : "";
    std::string lpath = ei >= 0 ? s.world.objs[size_t(ei)].str("LightMapName") : "";
    Bytes b;
    if (!s.colormap_custom) {
        s.colormap = Image();
        if (base && !cpath.empty() && base->read_file(cpath, b)) decode_file(b, s.colormap, s.colormap_style, s.colormap_layout);
    }
    if (!s.lightmap_custom) {
        s.lightmap = Image();
        if (base && !lpath.empty() && base->read_file(lpath, b)) decode_file(b, s.lightmap, s.lightmap_style, s.lightmap_layout);
    }
    // Formats to encode custom textures with, when the world's own were not found.
    if (s.colormap_layout.width == 0 && base) {
        for (auto& f : base->files())
            if (istarts_with(f.path, "Terrain/") && lower(f.path).find("colormap") != std::string::npos && base->read_file(f.path, b)) {
                Image tmp;
                if (decode_file(b, tmp, s.colormap_style, s.colormap_layout)) { if (s.colormap.px.empty()) s.colormap = tmp; break; }
            }
    }
    if (s.lightmap_layout.width == 0 && base) {
        for (auto& f : base->files())
            if (istarts_with(f.path, "Terrain/") && lower(f.path).find("lightmap") != std::string::npos && base->read_file(f.path, b)) {
                Image tmp;
                if (decode_file(b, tmp, s.lightmap_style, s.lightmap_layout)) { if (s.lightmap.px.empty()) s.lightmap = tmp; break; }
            }
    }
}

Image ScenarioIO::procedural_colormap(const Scenario& s, int size) {
    // Palette from the Base colormap: dark / mid / bright luminance bands. Its alpha is a blend
    // mask between the two detail textures (BoggedDown mean 182, GoCommando 22); the generated map
    // uses the Base mean so the ground keeps the Base level's detail look.
    uint32_t pal[3] = {0xFF2A3A30u, 0xFF4A6050u, 0xFF8A9A90u};
    uint32_t alpha = 0xFFu;
    if (!s.colormap.px.empty()) {
        uint64_t asum = 0;
        for (uint32_t c : s.colormap.px) asum += c >> 24;
        alpha = uint32_t(asum / s.colormap.px.size());
    }
    if (!s.colormap.px.empty()) {
        std::vector<std::pair<int, uint32_t>> px;
        for (size_t i = 0; i < s.colormap.px.size(); i += 7) {
            uint32_t c = s.colormap.px[i];
            int l = int(c & 255) * 3 + int((c >> 8) & 255) * 6 + int((c >> 16) & 255);
            px.push_back({l, c});
        }
        std::sort(px.begin(), px.end(), [](auto& a, auto& b) { return a.first < b.first; });
        auto band = [&](double a, double b) {
            size_t i0 = size_t(a * px.size()), i1 = std::max(i0 + 1, size_t(b * px.size()));
            uint64_t acc[3] = {};
            for (size_t i = i0; i < i1 && i < px.size(); i++) for (int c = 0; c < 3; c++) acc[c] += (px[i].second >> (8 * c)) & 255;
            size_t n = std::max<size_t>(1, std::min(i1, px.size()) - i0);
            return uint32_t(acc[0] / n) | uint32_t(acc[1] / n) << 8 | uint32_t(acc[2] / n) << 16 | 0xFF000000u;
        };
        pal[0] = band(0.10, 0.30);
        pal[1] = band(0.40, 0.60);
        pal[2] = band(0.75, 0.95);
    }
    Image img;
    img.w = img.h = size;
    img.px.resize(size_t(size) * size);
    const Terrain& t = s.terrain;
    float range = std::max(1.f, t.max_h - t.min_h);
    Rng rng(s.id ? s.id : 7);
    std::vector<float> noise(size_t(size) * size);
    for (auto& n : noise) n = rng.unit();
    auto lerp = [](uint32_t a, uint32_t b, float f) {
        uint32_t r = 0xFF000000u;
        for (int c = 0; c < 3; c++) {
            float x = float((a >> (8 * c)) & 255) * (1 - f) + float((b >> (8 * c)) & 255) * f;
            r |= uint32_t(std::clamp(int(x), 0, 255)) << (8 * c);
        }
        return r;
    };
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            float x = (float(px) + 0.5f) / float(size) * t.size_x();
            float z = (1.f - (float(py) + 0.5f) / float(size)) * t.size_z();
            float hf = (t.height_at(x, z) - t.min_h) / range;
            float slope = 1.f - t.normal_at(x, z).y;
            float f = std::clamp(hf * 1.2f + (noise[size_t(py) * size + px] - 0.5f) * 0.08f, 0.f, 1.f);
            uint32_t c = f < 0.5f ? lerp(pal[0], pal[1], f * 2) : lerp(pal[1], pal[2], (f - 0.5f) * 2);
            c = lerp(c, pal[0], std::clamp(slope * 2.5f, 0.f, 0.8f));
            img.px[size_t(py) * size + px] = (c & 0xFFFFFFu) | alpha << 24;
        }
    return img;
}

void ScenarioIO::bake_lightmap(Scenario& s) {
    int ei = find_class(s.world, K_ENV);
    Vec3 sun{-0.6f, -0.5f, 0.6f};
    if (ei >= 0) if (const WProp* p = s.world.objs[size_t(ei)].find("DirectionalLightDirection")) sun = p->as_vec3();
    int lw = s.lightmap_layout.width ? int(s.lightmap_layout.width) : 256;
    int lh = s.lightmap_layout.height ? int(s.lightmap_layout.height) : 256;
    Terrain q = s.terrain;
    q.from_samples(q.samples());
    s.lightmap = ma2::bake_lightmap(q, sun, lw, lh);
    s.lightmap_custom = true;
}

// ----------------------------------------------------------------------------- navigation

static bool parse_water_extent(const std::string& name, float& wx, float& wz) {
    std::smatch m;
    static const std::regex re(R"((\d{2,5})\s*[xX]\s*(\d{2,5}))");
    if (std::regex_search(name, m, re)) { wx = std::stof(m[1]); wz = std::stof(m[2]); return true; }
    return false;
}

NavGraph ScenarioIO::build_nav(const Scenario& s) {
    struct Water { float x, z, hx, hz, y; };
    std::vector<Water> waters;
    std::vector<Vec3> buildings;
    static const uint32_t K_BUILDING = key_of("MABuilding");
    for (auto& o : s.world.objs) {
        Vec3 p;
        if (!get_pos(o, p)) continue;
        if (o.cls == K_WATER) {
            float wx = 500, wz = 500;
            if (!water_custom_size(o, wx, wz)) {  // the type's own size, from its template name (the Description may be renamed)
                const TemplateInfo* wt = cat_.find(o.template_id());
                if (!wt || !parse_water_extent(wt->name, wx, wz)) parse_water_extent(o.description(), wx, wz);
            }
            waters.push_back({p.x, p.z, wx * 0.5f, wz * 0.5f, p.y});
        } else if (o.cls == K_BUILDING) {
            buildings.push_back(p);
        }
    }
    // Heights as the game will see them (8-bit), so saves are reproducible.
    Terrain t = s.terrain;
    t.from_samples(t.samples());
    auto blocked = [&](float x, float z) {
        float g = t.height_at(x, z);
        for (auto& w : waters)
            if (std::fabs(x - w.x) <= w.hx && std::fabs(z - w.z) <= w.hz && w.y - g > 4.f) return true;
        for (auto& b : buildings) if ((x - b.x) * (x - b.x) + (z - b.z) * (z - b.z) < 14.f * 14.f) return true;
        int v = typemap_at(s, x, z);
        if (v > 0) {
            // 2 = border band, 16 = deep water (guess, NOTES.md). 1 = out of bounds blocks MP only:
            // retail mission graphs cross type 1 (Mission_04: 456 of 1,504 nodes).
            if (v & (tmtype::Border | 16)) return true;
            if (s.multiplayer && (v & tmtype::OutOfBounds)) return true;
        }
        return false;
    };
    NavGenParams p;
    // Only ground reachable from a player start or spawn point (maintainer, 2026-10-20: keep
    // campaign-AI graphs near retail density, at most ~2,500 nodes).
    // AI units are seeds too: they need paths where they stand.
    static const uint32_t ai_classes[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAPerson")};
    for (auto& o : s.world.objs) {
        Vec3 sp;
        bool seed = o.cls == K_LEVELSTART || std::find(std::begin(ai_classes), std::end(ai_classes), o.cls) != std::end(ai_classes);
        if (seed && get_pos(o, sp) && t.inside(sp.x, sp.z)) p.seeds.push_back(sp);
    }
    return generate_nav(t, p, blocked);
}

// ----------------------------------------------------------------------------- open / create

static const MapInfo* base_world_for(GameData& gd, const std::string& base, bool prefer_mp) {
    const MapInfo* best = nullptr;
    for (auto& m : gd.maps()) {
        if (!iequals(m.resource, base)) continue;
        if (!best || (m.multiplayer == prefer_mp && best->multiplayer != prefer_mp)) best = &m;
    }
    return best;
}

bool ScenarioIO::open_retail(const MapInfo& m, Scenario& s, std::string* err) {
    s = Scenario();
    if (!gd_.load_world(m, s.world, err)) return false;
    if (!s.terrain.load_from_world(s.world, err)) return false;
    s.base = m.resource;
    s.multiplayer = m.multiplayer;
    s.types = m.game_types;
    s.name = m.name;
    s.retail_name = m.name;
    s.read_only = true;
    load_retail_objectives(s);  // the level's own objective slots, named as the developers named them
    load_terrain_textures(s);
    load_typemap(s);
    return true;
}

static uint32_t random_id() {
    std::random_device rd;
    uint32_t v = 0;
    while (!v) v = rd() ^ uint32_t(time(nullptr) * 2654435761u);
    return v;
}

// Objective/variable slots on MARoot: reset to the values of a clean retail MP root.
static bool objective_key(const std::string& n) {
    static const char* pre[] = {"ob", "NumObjective", "Objective", "mf", "dwVoiceCueID", "NumberFEMissionObjectives"};
    for (const char* p : pre) if (n.compare(0, strlen(p), p) == 0) return true;
    return false;
}

bool ScenarioIO::create(const NewMapParams& p, Scenario& s, std::string* err) {
    if (p.start == NewMapParams::CopyRetail) {
        const MapInfo* m = gd_.find_map(p.copy_map);
        if (!m) { if (err) *err = "unknown retail map " + p.copy_map; return false; }
        if (!open_retail(*m, s, err)) return false;
        s.read_only = false;
        s.id = random_id();
        s.name = p.name.empty() ? "Copy of " + m->name : p.name;
        s.retail_name.clear();
        if (int ei = find_class(s.world, K_ENV); ei >= 0) weather::set_lightning(s.world.objs[size_t(ei)], false);  // new maps start without lightning
        s.dirty = true;
        return true;
    }
    const MapInfo* bm = base_world_for(gd_, p.base, p.multiplayer);
    if (!bm) { if (err) *err = "no retail world uses " + p.base; return false; }
    Wdf src;
    if (!gd_.load_world(*bm, src, err)) return false;
    WTree t;
    t.build(src);
    std::set<int> keep;
    int root = t.root;
    if (root < 0) { if (err) *err = "base world has no root"; return false; }
    keep.insert(root);
    auto keep_sub = [&](int i) { std::vector<int> sub; t.subtree(i, sub); keep.insert(sub.begin(), sub.end()); };
    static const uint32_t K_GROUPSET = key_of("MoeGroupSet");
    for (int c : t.kids[size_t(root)]) {
        const WObj& o = src.objs[size_t(c)];
        if (o.cls == K_ENV || o.cls == K_PATHDB || o.cls == K_GROUPSET) keep_sub(c);
        else if (o.cls == K_TERRAIN) {
            keep.insert(c);
            for (int k : t.kids[size_t(c)]) {
                uint32_t kc = src.objs[size_t(k)].cls;
                if (kc == K_SKY || kc == K_TINFO || kc == K_PATHDB) keep_sub(k);
            }
        } else if (p.multiplayer && bm->multiplayer && o.cls == K_GROUP && o.description() == "Triggers") {
            keep_sub(c);  // MP setup triggers: music, nations, game-over sound
        }
    }
    // Relink child/sibling over the kept objects (as in culled template files).
    Wdf w;
    w.header = src.header;
    for (int i : keep) {
        WObj o = src.objs[size_t(i)];
        uint32_t child = 0, sibling = 0;
        for (int k : t.kids[size_t(i)]) if (keep.count(k)) { child = src.objs[size_t(k)].uid(); break; }
        int par = t.parent_of[size_t(i)];
        if (par >= 0) {
            const auto& sib = t.kids[size_t(par)];
            auto it = std::find(sib.begin(), sib.end(), i);
            for (++it; it != sib.end(); ++it) if (keep.count(*it)) { sibling = src.objs[size_t(*it)].uid(); break; }
        }
        o.set_u32("child", child);
        o.set_u32("sibling", sibling);
        w.objs.push_back(std::move(o));
    }
    // Drop references to removed objects (set members, kill references).
    {
        std::unordered_set<uint32_t> uids;
        for (auto& o : w.objs) uids.insert(o.uid());
        static const uint32_t K_EREF = key_of("MoeEntityReference");
        std::vector<uint32_t> dead;
        for (auto& o : w.objs)
            if (o.cls == K_EREF && !uids.count(o.u32("ReferenceUID"))) dead.push_back(o.uid());
        for (uint32_t u : dead) wedit::remove_subtree(w, u);
        // Review B16: the kept MoeGroupSet brings the base level's sets ("Red Stuff" 15, "Invulnerabilty at Start" 4)
        // whose MoeEntitySetMember uids were not kept: a new map listed them full of missing units. Drop those members;
        // a mission keeps none of the base level's triggers, so its emptied sets go too (MP setup triggers may name theirs).
        static const uint32_t K_ESET = key_of("MoeEntitySet");
        std::vector<uint32_t> empty;
        for (auto& o : w.objs) {
            if (o.cls != K_ESET) continue;
            int n = o.i32("MoeEntitySetCount");
            std::vector<uint32_t> keep_m;
            for (int k = 0; k < n; k++) {
                uint32_t m = o.u32("MoeEntitySetMember" + std::to_string(k));
                if (uids.count(m)) keep_m.push_back(m);
                o.erase(key_of("MoeEntitySetMember" + std::to_string(k)));
            }
            for (size_t k = 0; k < keep_m.size(); k++) o.set_u32("MoeEntitySetMember" + std::to_string(k), keep_m[k]);
            o.set_i32("MoeEntitySetCount", int(keep_m.size()));
            if (keep_m.empty() && !(p.multiplayer && bm->multiplayer)) empty.push_back(o.uid());
        }
        for (uint32_t u : empty) wedit::remove_subtree(w, u);
    }
    // Root: game types and clean objective slots.
    int ri = find_class(w, K_ROOT);
    if (ri >= 0) {
        WObj& r = w.objs[size_t(ri)];
        if (!bm->multiplayer || !p.multiplayer) {
            const MapInfo* clean = gd_.find_map("BoggedDown");
            Wdf cw;
            if (clean && gd_.load_world(*clean, cw)) {
                int cri = find_class(cw, K_ROOT);
                for (auto& prop : r.props) {
                    const std::string* n = gd_.names().name(prop.key);
                    if (!n || !objective_key(*n)) continue;
                    if (const WProp* cp = cw.objs[size_t(cri)].find(prop.key)) prop.data = cp->data;
                }
            }
        }
        for (auto& g : game_types()) if (WProp* f = r.find(g.flag)) f->set_int(0);
        if (p.multiplayer) {
            for (auto& ty : p.types) if (auto* g = find_game_type(ty)) if (WProp* f = r.find(g->flag)) f->set_int(1);
            if (WProp* f = r.find("Generic")) f->set_int(0);
        } else {
            if (WProp* f = r.find("Generic")) f->set_int(1);
            if (WProp* f = r.find("MissionType")) f->set_int(0);
        }
    }
    s = Scenario();
    s.world = std::move(w);
    s.id = random_id();
    s.name = p.name;
    s.multiplayer = p.multiplayer;
    s.types = p.multiplayer ? p.types : std::vector<std::string>{};
    s.base = lower(p.base);
    // Terrain
    if (!s.terrain.load_from_world(s.world, err)) return false;
    Terrain base_terrain = s.terrain;
    if (!p.keep_base_terrain) {
        s.terrain.min_h = p.min_h;
        s.terrain.max_h = p.max_h;
        s.terrain.resize_flat(p.w, p.h, std::clamp(p.flat_height, p.min_h, p.max_h));
    }
    if (p.start == NewMapParams::Noise && !p.keep_base_terrain) {
        // Value noise, 4 octaves.
        Rng rng(p.seed ? p.seed : 1);
        const int G = 64;
        std::vector<float> lat(size_t(G) * G);
        for (auto& v : lat) v = rng.unit();
        auto smooth = [&](float x, float z) {
            int x0 = int(std::floor(x)), z0 = int(std::floor(z));
            float fx = x - float(x0), fz = z - float(z0);
            fx = fx * fx * (3 - 2 * fx); fz = fz * fz * (3 - 2 * fz);
            auto L = [&](int a, int b) { return lat[size_t((b & (G - 1)) * G + (a & (G - 1)))]; };
            float a = L(x0, z0), b = L(x0 + 1, z0), c = L(x0, z0 + 1), d = L(x0 + 1, z0 + 1);
            return (a * (1 - fx) + b * fx) * (1 - fz) + (c * (1 - fx) + d * fx) * fz;
        };
        float range = p.max_h - p.min_h;
        // Four averaged octaves stay near 0.5 (about +-13 m at the default 'up to 42 m'): from the editor's camera the
        // map read as flat (campaign dogfood M2-1). Stretch the noise to 0..1 so the hills reach the height the dialog shows.
        std::vector<float> nv(size_t(p.w) * p.h);
        float vmin = 1, vmax = 0;
        for (int gz = 0; gz < p.h; gz++)
            for (int gx = 0; gx < p.w; gx++) {
                float x = gx * s.terrain.scale / p.noise_scale, z = gz * s.terrain.scale / p.noise_scale, v = 0, amp = 1, tot = 0;
                for (int o = 0; o < 4; o++) { v += smooth(x * float(1 << o), z * float(1 << o)) * amp; tot += amp; amp *= 0.5f; }
                v /= tot;
                nv[size_t(gz) * p.w + gx] = v;
                vmin = std::min(vmin, v), vmax = std::max(vmax, v);
            }
        const float vspan = std::max(vmax - vmin, 1e-3f);
        for (int gz = 0; gz < p.h; gz++)
            for (int gx = 0; gx < p.w; gx++) {
                const float v = (nv[size_t(gz) * p.w + gx] - vmin) / vspan;
                float h = p.flat_height + (v - 0.5f) * 2.f * p.noise_amp * range;
                // Raise the border a little so the edge reads as a boundary.
                float ex = std::min(gx, p.w - 1 - gx) / float(std::max(1, p.w / 10)), ez = std::min(gz, p.h - 1 - gz) / float(std::max(1, p.h / 10));
                float edge = std::clamp(std::min(ex, ez), 0.f, 1.f);
                h += (1.f - edge) * range * 0.15f;
                s.terrain.ref(gx, gz) = std::clamp(h, p.min_h, p.max_h);
            }
    }
    // Terrain info. The 512x512 type map always spans the whole terrain (retail 129x129 levels
    // use 512x512 too). Its values: 0 playable, 8 playable water/special, 2 border band, 1 out of
    // bounds (nothing moves there). The Base level's own map fits only the Base's own terrain:
    // on generated terrain its out-of-bounds areas fall anywhere, and editor spawns on 1 never
    // start a round (maintainer bisects X5, G1, B10, G10). Generated terrain gets one playable
    // area with a retail-style edge; the culture map is kept.
    load_typemap(s);
    if (!p.keep_base_terrain) fill_playable_typemap(s);
    if (p.drop_culture) {
        static const uint32_t K_CULTSYS = key_of("MACultureSystem");
        int ci = find_class(s.world, K_CULTSYS);
        if (ci >= 0) wedit::remove_subtree(s.world, s.world.objs[size_t(ci)].uid());
    } else if (!p.keep_base_terrain) {
        // The Base level's plant patches follow its own hills, not the new ground: start bare (the plant
        // types stay, for the Plants tool).
        int ti = find_class(s.world, K_TINFO);
        WProp* cp = ti >= 0 ? s.world.objs[size_t(ti)].find(K_CULTUREMAP) : nullptr;
        Mgic m;
        if (cp && m.parse(cp->data)) {
            MgicSection* pic = m.section(fourcc("PIC "));
            if (MgicChunk* bits = pic ? pic->chunk(fourcc("bits")) : nullptr) {
                std::fill(bits->data.begin(), bits->data.end(), uint8_t(0));
                cp->data = m.serialize();
            }
        }
    }
    // Textures: formats from the Base level, pixels generated.
    load_terrain_textures(s);
    if (!p.keep_base_terrain) {
        int csize = s.colormap_layout.width ? int(s.colormap_layout.width) : 256;
        s.colormap = procedural_colormap(s, csize);
        s.colormap_custom = true;
        // One ground texture (design note 10-26, AoE-style palette): the theme's own covers the whole map, and the
        // engine's second slot holds the same file at 0% (colour-map alpha 255 = TerrainDetailMap everywhere), so the
        // palette shows one swatch and the game shows nothing odd. Adding a texture in the editor takes that slot.
        if (int ti = find_class(s.world, K_TERRAIN); ti >= 0) {
            WObj& t = s.world.objs[size_t(ti)];
            // Texture repeats count across the whole map: scale them so a pattern keeps the Base level's size on
            // the ground (a 1 km map from a 2 km level would show its cracks and stones at half size).
            const float k = s.terrain.size_x() / std::max(1.f, base_terrain.size_x());
            for (const char* key : {"TerrainDetailMult", "TerrainDetailMult1", "TerrainMicroDetailTiling0", "TerrainMicroDetailTiling1"})
                if (t.find(key)) t.set_f32(key, std::clamp(t.f32(key) * k, 1.f, 512.f));
            t.set_str("TerrainDetailMap 1", t.str("TerrainDetailMap"));
            if (t.find("TerrainDetailMult1") && t.find("TerrainDetailMult")) t.set_f32("TerrainDetailMult1", t.f32("TerrainDetailMult"));
            for (auto& px : s.colormap.px) px |= 0xFF000000u;
        }
        bake_lightmap(s);
        // Height fog (MAEnvironment FogBottom..FogTop, Mission_05: FogMode 2, 0.93-7.48 m) is tuned
        // to the Base terrain's valleys. On generated terrain the player and the fights sit in the
        // low ground, and a band anywhere inside the terrain's range washes the scene out
        // (maintainer runs bi_M2). Put the band's top 2 m under the lowest ground, keeping its
        // thickness; distance fog (FogStart/FogEnd) stays as the Base level has it.
        int ei = find_class(s.world, K_ENV);
        if (ei >= 0) {
            WObj& env = s.world.objs[size_t(ei)];
            float top = env.f32("FogTop"), bottom = env.f32("FogBottom");
            if (env.find("FogTop") && env.find("FogBottom") && top > bottom) {
                float lowest = *std::min_element(s.terrain.hgt.begin(), s.terrain.hgt.end());
                env.set_f32("FogTop", lowest - 2.f);
                env.set_f32("FogBottom", lowest - 2.f - (top - bottom));
            }
        }
        (void)base_terrain;
    }
    // Missions: a player start in the middle.
    uint32_t next = next_uid(s.world);
    if (!p.multiplayer) {
        int si = -1;
        Wdf mw;
        if (gd_.load_world(*bm, mw)) si = find_class(mw, K_LEVELSTART);
        if (si >= 0) {
            WObj ls = mw.objs[size_t(si)];
            ls.set_u32("UID", next++);
            ls.set_u32("child", 0);
            ls.set_i32("TeamIndex", 0);
            float cx = s.terrain.size_x() * 0.5f, cz = s.terrain.size_z() * 0.5f;
            set_pos(ls, {cx, s.terrain.height_at(cx, cz) + 2.f, cz});
            uint32_t g = ensure_group(s.world, terrain_uid(s.world), "Player Start", next);
            wedit::append_child(s.world, g, std::move(ls));
        }
    }
    // A new map starts in daylight unless the base level is itself a night level: a night look is
    // replaced by the default day look, and a day look no longer lights the windows and lamps (the
    // Night switch and "Lit windows and lamps" on the Map tab then agree).
    {
        const weather::Look* cur = weather::current_look(*this, s);
        int ei = find_class(s.world, K_ENV);
        bool night_base = false;
        for (const char* n : {"IllWind", "GoCommando", "NewRiverCity", "WitchCauldron"}) night_base |= iequals(bm->name, n);
        if (!night_base && ei >= 0) {
            if (cur && cur->night) {
                if (const weather::Look* day = weather::default_look(false)) {
                    std::string werr;
                    if (!weather::apply_look(*this, s, *day, false, &werr)) log_stderr("new map: day look not applied: " + werr + "\n");
                }
            } else if (!(cur && cur->lit)) {
                s.world.objs[size_t(ei)].set_i32("IsNightTime", 0);
            }
        }
    }
    // Lightning is off in every new map, whatever theme level it started from (the Weather tab turns it on).
    if (int ei = find_class(s.world, K_ENV); ei >= 0) weather::set_lightning(s.world.objs[size_t(ei)], false);
    // A new mission starts the player in a 'Mech: on foot (the Base's own start) a pilot can hardly finish a
    // 'Destroy these' objective (dogfood M2-2). The Mad Dog (the campaign's own start 'Mech), else the first 'Mech;
    // the save ships the unit's files when the Base lacks them (fresh_templates: start_templates), as for the Map tab pick.
    if (!p.multiplayer && p.start != NewMapParams::CopyRetail) {
        const StartChoice* pick = nullptr;
        for (auto& c : start_choices()) {
            if (c.kind != 2) continue;
            if (!pick) pick = &c;
            if (c.label.find("Mad Dog") != std::string::npos || c.label.find("MadDog") != std::string::npos) { pick = &c; break; }
        }
        if (pick) set_start_container(s, pick->container);
    }
    // A new multiplayer map on foot: the free-for-all types start in Battle Armor as on retail maps (set_start_container).
    if (p.multiplayer && p.start != NewMapParams::CopyRetail && start_container(s) == 123999) set_start_container(s, 123999);
    s.dirty = true;
    return true;
}

// ----------------------------------------------------------------------------- kits

std::vector<std::string> ScenarioIO::maps_with_mode(const std::string& gt) {
    std::vector<std::string> v;
    for (auto& m : gd_.maps()) {
        if (!m.multiplayer || !m.resource_present) continue;
        for (auto& g : m.game_types) if (iequals(g, gt)) { v.push_back(m.name); break; }
    }
    return v;
}

int ScenarioIO::add_mode_kit(Scenario& s, const std::string& gt, const std::string& source, std::string* err) {
    const GameTypeDef* def = find_game_type(gt);
    const MapInfo* m = gd_.find_map(source);
    if (!def || !m) { if (err) *err = "unknown game type or map"; return 0; }
    Wdf src;
    if (!gd_.load_world(*m, src, err)) return 0;
    Terrain st;
    if (!st.load_from_world(src, err)) return 0;
    WTree t;
    t.build(src);
    uint32_t next = next_uid(s.world);
    uint32_t parent = terrain_uid(s.world);
    if (!parent) { if (err) *err = "world has no terrain"; return 0; }
    float sx = s.terrain.size_x() / std::max(1.f, st.size_x()), sz = s.terrain.size_z() / std::max(1.f, st.size_z());
    int added = 0;
    std::unordered_set<uint32_t> keep_keys = cat_.ref_keys();
    keep_keys.erase(key_of("ReferenceUID"));  // world references inside the kit (trigger -> generator)
    keep_keys.insert(key_of("TemplateID"));
    // This map's own spawn points (MP dogfood 10-26): a team that already has 6 gets no copied
    // spawns, and each team's copied pieces (flags, bases...) go next to that team's spawns
    // instead of where the source level had them.
    int have[3] = {0, 0, 0};
    Vec3 home[3] = {};
    for (auto& o : s.world.objs) {
        Vec3 p;
        int ti = o.i32("TeamIndex");
        if (o.cls != K_LEVELSTART || ti < 0 || ti > 2 || !get_pos(o, p)) continue;
        have[ti]++;
        home[ti].x += p.x;
        home[ti].z += p.z;
    }
    for (int k = 0; k < 3; k++) if (have[k]) { home[k].x /= float(have[k]); home[k].z /= float(have[k]); }
    bool skip_spawns = def->team ? have[1] >= 6 && have[2] >= 6 : have[0] >= 6;
    int src_have[3] = {0, 0, 0};  // the source level's spawn ring centres, per team
    Vec3 src_home[3] = {};
    for (auto& o : src.objs) {
        Vec3 p;
        int ti = o.i32("TeamIndex");
        if (o.cls != K_LEVELSTART || ti < 1 || ti > 2 || !get_pos(o, p)) continue;
        src_have[ti]++;
        src_home[ti].x += p.x;
        src_home[ti].z += p.z;
    }
    for (int k = 1; k < 3; k++) if (src_have[k]) { src_home[k].x /= float(src_have[k]); src_home[k].z /= float(src_have[k]); }
    std::vector<uint32_t> drop;  // copied spawn points to remove after insertion
    for (size_t i = 0; i < src.objs.size(); i++) {
        const WObj& o = src.objs[i];
        if (o.cls != K_MTG) continue;
        const WProp* f = o.find(def->flag);
        if (!f || !f->as_int()) continue;
        // Skip a group whose description already exists in this world (kit added before).
        // A copy made for another game type (each copy keeps only its own type's flag) is shared instead.
        bool exists = false;
        for (auto& x : s.world.objs)
            if (x.cls == K_MTG && x.description() == o.description() + " (" + m->name + ")") {
                exists = true;
                if (WProp* xf = x.find(def->flag); xf && !xf->as_int()) { xf->set_int(1); added++; }
            }
        if (exists) continue;
        next = std::max(next, next_uid(s.world));
        auto clone = wedit::clone_subtree(src, o.uid(), next, &keep_keys);
        if (clone.empty()) continue;
        fixup_clone(s, clone, src);
        if (clone.front().uid() < next_uid(s.world)) {  // a set was created above the clone's ids
            uint32_t base = next_uid(s.world);
            std::unordered_map<uint32_t, uint32_t> remap;
            for (auto& c : clone) remap[c.uid()] = base++;
            for (auto& c : clone)
                for (auto& p : c.props) {
                    if (p.data.size() != 4 || (p.type != PT_U32 && p.type != PT_I32) || keep_keys.count(p.key)) continue;
                    auto it = remap.find(uint32_t(p.as_int()));
                    if (it != remap.end() && p.as_int()) p.set_int(it->second);
                }
            next = base;
        }
        clone.front().set_str("Description", o.description() + " (" + m->name + ")");
        // Only this game type uses the copy: a group the source shared with other types (CTF towers
        // also flagged for Team Destruction) would show up in those matches too.
        for (auto& g : game_types())
            if (std::string_view(g.flag) != def->flag)
                if (WProp* f = clone.front().find(g.flag)) f->set_int(0);
        // Placed objects move to the scaled spot; their attached parts keep their offsets to them
        // (scaling every part squeezed turrets and flag stands together on smaller maps).
        Wdf ct;
        ct.objs = clone;
        WTree tt;
        tt.build(ct);
        std::vector<int> anchor(clone.size(), -1);
        for (size_t k = 0; k < clone.size(); k++) {
            if (!position(clone[k])) continue;
            int a = int(k), p = tt.parent_of[k];
            while (p >= 0 && position(clone[size_t(p)]) && clone[size_t(p)].cls != K_MTG && clone[size_t(p)].cls != K_GROUP) {
                a = p;
                p = tt.parent_of[size_t(p)];
            }
            anchor[k] = a;
        }
        std::vector<Vec3> orig(clone.size()), moved(clone.size());
        for (size_t k = 0; k < clone.size(); k++) get_pos(clone[k], orig[k]);
        for (size_t k = 0; k < clone.size(); k++) {
            if (anchor[k] != int(k)) continue;
            const Vec3& p = orig[k];
            float above = p.y - st.height_at(p.x, p.z);
            Vec3 q{p.x * sx, 0, p.z * sz};
            q.y = s.terrain.height_at(q.x, q.z) + above;
            moved[k] = q;
        }
        // Team pieces next to this map's spawns (MP dogfood 10-26). A piece's team is the team set in it
        // (TeamIndex, Nation), else Red/Blue in its name (Bogged Down's "CTF Flagbase Blue" has no Nation),
        // else the source spawn ring it stood within 150 m of (the vehicles and turrets by each base).
        // Pieces by a ring keep their real offsets around this map's ring; far ones (flags, light columns)
        // keep their layout, scaled, around a point 40 m from the ring toward the middle of the map.
        if (def->team) {
            auto team_of = [&](size_t k) {
                std::vector<int> sub;
                tt.subtree(int(k), sub);
                for (int j : sub) {
                    const WObj& c = clone[size_t(j)];
                    int v = c.cls == K_LEVELSTART ? c.i32("TeamIndex") : c.find("Nation") ? c.i32("Nation") : 0;
                    if (v == 1 || v == 2) return v;
                }
                std::string n = " " + lower(clone[k].description()) + " ";
                for (char& ch : n) if (!std::isalnum((unsigned char)ch)) ch = ' ';
                if (n.find(" red ") != std::string::npos) return 1;
                if (n.find(" blue ") != std::string::npos) return 2;
                return 0;
            };
            auto dist = [](const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); };
            std::vector<int> team(clone.size(), 0);
            std::vector<char> by_ring(clone.size(), 0);
            Vec3 ctr[3] = {};
            int cnt[3] = {0, 0, 0};
            for (size_t k = 0; k < clone.size(); k++) {
                if (anchor[k] != int(k) || clone[k].cls == K_LEVELSTART) continue;
                int tm = team_of(k);
                for (int h = 1; h <= 2; h++)
                    if (src_have[h] && dist(orig[k], src_home[h]) <= 150.f && (!tm || tm == h)) { tm = h; by_ring[k] = 1; }
                if (!tm || !have[tm] || !src_have[tm]) continue;
                team[k] = tm;
                if (by_ring[k]) continue;
                ctr[tm].x += orig[k].x;
                ctr[tm].z += orig[k].z;
                cnt[tm]++;
            }
            float mx = s.terrain.size_x() * 0.5f, mz = s.terrain.size_z() * 0.5f, sc = std::min(sx, sz);
            for (int tm = 1; tm <= 2; tm++) {
                if (!have[tm]) continue;
                float dx = mx - home[tm].x, dz = mz - home[tm].z, d = std::sqrt(dx * dx + dz * dz);
                Vec3 to{home[tm].x + (d > 1 ? dx / d * 40.f : 0), 0, home[tm].z + (d > 1 ? dz / d * 40.f : 0)};
                Vec3 c{cnt[tm] ? ctr[tm].x / float(cnt[tm]) : 0, 0, cnt[tm] ? ctr[tm].z / float(cnt[tm]) : 0};
                for (size_t k = 0; k < clone.size(); k++) {
                    if (team[k] != tm) continue;
                    Vec3 q = by_ring[k] ? Vec3{home[tm].x + orig[k].x - src_home[tm].x, 0, home[tm].z + orig[k].z - src_home[tm].z}
                                     : Vec3{to.x + (orig[k].x - c.x) * sc, 0, to.z + (orig[k].z - c.z) * sc};
                    moved[k].x = std::clamp(q.x, 8.f, s.terrain.size_x() - 8.f);
                    moved[k].z = std::clamp(q.z, 8.f, s.terrain.size_z() - 8.f);
                }
            }
        }
        // Snatch It data cores keep their real spacing around the pile's scaled middle: scaled, a retail pile's 10 m
        // spacing became 5 m on a 1 km map and the five big core models made one solid block 'Mechs stopped 9-13 m off,
        // so only Battle Armor ever grabbed one (bots batch 3).
        {
            static const uint32_t K_CORE = key_of("MASnatchItDataCore");
            Vec3 co{}, cm{};
            int nc = 0;
            for (size_t k = 0; k < clone.size(); k++)
                if (anchor[k] == int(k) && clone[k].cls == K_CORE) { co.x += orig[k].x; co.z += orig[k].z; cm.x += moved[k].x; cm.z += moved[k].z; nc++; }
            if (nc > 1)
                for (size_t k = 0; k < clone.size(); k++)
                    if (anchor[k] == int(k) && clone[k].cls == K_CORE) {
                        moved[k].x = cm.x / float(nc) + orig[k].x - co.x / float(nc);
                        moved[k].z = cm.z / float(nc) + orig[k].z - co.z / float(nc);
                    }
        }
        // Spawn points squeezed closer than 5 m are pushed apart (players spawned inside each other).
        for (int iter = 0; iter < 8; iter++)
            for (size_t a = 0; a < clone.size(); a++) {
                if (anchor[a] != int(a) || clone[a].cls != K_LEVELSTART) continue;
                for (size_t b = a + 1; b < clone.size(); b++) {
                    if (anchor[b] != int(b) || clone[b].cls != K_LEVELSTART) continue;
                    float dx = moved[b].x - moved[a].x, dz = moved[b].z - moved[a].z, d = std::sqrt(dx * dx + dz * dz);
                    if (d >= 5.f) continue;
                    if (d < 0.01f) { dx = 1; dz = 0; d = 1; }
                    float push = (5.f - d) * 0.5f;
                    moved[a].x -= dx / d * push; moved[a].z -= dz / d * push;
                    moved[b].x += dx / d * push; moved[b].z += dz / d * push;
                }
            }
        for (size_t k = 0; k < clone.size(); k++) {
            if (anchor[k] < 0) continue;
            size_t a = size_t(anchor[k]);
            if (a == k) {
                Vec3 q = moved[k];
                float above = orig[k].y - st.height_at(orig[k].x, orig[k].z);
                q.y = s.terrain.height_at(q.x, q.z) + above;
                set_pos(clone[k], q);
            } else {
                set_pos(clone[k], {orig[k].x - orig[a].x + moved[a].x, orig[k].y - orig[a].y + moved[a].y, orig[k].z - orig[a].z + moved[a].z});
            }
        }
        for (size_t k = 0; k < clone.size(); k++) if (anchor[k] == int(k)) added++;  // placed pieces, as the status bar counts them
        if (skip_spawns)
            for (auto& c : clone) if (c.cls == K_LEVELSTART) drop.push_back(c.uid());
        wedit::insert_subtree(s.world, parent, std::move(clone));
    }
    for (uint32_t u : drop) wedit::remove_subtree(s.world, u);
    added -= int(drop.size());
    // Enable the mode on the root and in the scenario's list.
    int ri = find_class(s.world, K_ROOT);
    if (ri >= 0) if (WProp* f = s.world.objs[size_t(ri)].find(def->flag)) f->set_int(1);
    bool listed = false;
    for (auto& x : s.types) if (iequals(x, def->ini)) listed = true;
    if (!listed) s.types.push_back(def->ini);
    s.dirty = true;
    return added;
}

// ----------------------------------------------------------------------------- assets

struct ScenarioIO::ArchiveSetHolder {
    ma2m::ArchiveSet set;
    std::unordered_map<std::string, std::vector<ma2m::Dependency>> deps;  // asset_closure: per template (Check runs it often)
};

// Every archive on the disc, Base first (dependency collection and model-path resolution).
void ScenarioIO::ensure_deps_archives(const Scenario& s) {
    if (deps_archives_ && iequals(deps_base_, s.base)) return;
    deps_archives_ = std::make_shared<ArchiveSetHolder>();
    deps_base_ = lower(s.base);
    deps_archives_->set.Add(path_utf8(gd_.game_files() / widen(deps_base_)));
    deps_archives_->set.Add(path_utf8(gd_.game_files() / L"common.mgf"));
    for (auto& a : gd_.level_archives()) if (!iequals(a, deps_base_)) deps_archives_->set.Add(path_utf8(gd_.game_files() / widen(a)));
}

std::unordered_set<uint32_t> ScenarioIO::fresh_templates(const Scenario& s, const Wdf& world) {
    // The Base level's own objects and its retail culled set already work with the Base archive as
    // it is (the wider template closure includes alternates that retail levels ship without their
    // files), so only objects new to this Base and the templates they reach need files.
    std::unordered_set<uint32_t> known = cat_.culled_set(s.base_level());
    for (uint32_t u : cat_.common_set(s.multiplayer)) known.insert(u);
    // A known template whose model is in neither the Base archive nor common.mgf still needs its files: Base
    // levels list 'Mechs they never ship (Mission_01b names the Kit Fox and Cougar, the disc has their models
    // only in other missions' archives), and a placed one drew as a placeholder (review 10-26).
    ensure_deps_archives(s);
    const ma2m::ArchiveSet& ar = deps_archives_->set;
    std::unordered_map<uint32_t, bool> unshipped;
    auto missing_here = [&](uint32_t tid) {
        auto it = unshipped.find(tid);
        if (it != unshipped.end()) return it->second;
        const WObj* t = cat_.object(tid);
        std::string m = t ? ma2m::NormalizePath(t->str("Corporal3DObj")) : std::string();
        bool miss = false;
        if (!m.empty() && !ar.ExistsIn(0, m) && !ar.ExistsIn(1, m)) {
            size_t dot = m.rfind('.');
            std::string low = dot == std::string::npos ? m + "_low" : m.substr(0, dot) + "_low" + m.substr(dot);
            miss = !ar.ExistsIn(0, low) && !ar.ExistsIn(1, low) && !model_absent(s, tid);  // on the disc somewhere else
        }
        return unshipped[tid] = miss;
    };
    Wdf fresh;
    std::unordered_set<uint32_t> out;
    for (auto& o : world.objs) {
        uint32_t tid = o.template_id();
        if (!tid || (known.count(tid) && !missing_here(tid))) continue;
        fresh.objs.push_back(o);
        out.insert(tid);
    }
    // The unit the player starts in (MARoot's player configuration container) and what it reaches.
    {
        std::unordered_set<uint32_t> seed;
        if (!start_is_retail(s))
            for (uint32_t u : start_templates(s)) if (!known.count(u)) seed.insert(u);
        if (!seed.empty()) {
            for (uint32_t u : seed) out.insert(u);
            for (uint32_t u : cat_.closure(seed)) if (!known.count(u)) out.insert(u);
        }
    }
    // A template the closure reaches (its own references, its children, and since 11-09 the templates its .mtb
    // names) needs files when it is new to this Base, or known but with its model on neither Base nor common.mgf.
    for (uint32_t u : cat_.closure(cat_.seed_of(fresh))) if (!known.count(u) || missing_here(u)) out.insert(u);
    // Snow or dust from another level (rain ships as the snow effect for now, see build).
    if (int ei = find_class(world, K_ENV); ei >= 0) {
        weather::Precip p = weather::precip(world.objs[size_t(ei)]);
        uint32_t id = p == weather::Precip::Rain ? weather::kSnowTemplate : world.objs[size_t(ei)].u32("PrecipEffectID");
        if (p != weather::Precip::None && !known.count(id))
            for (uint32_t u : cat_.closure({id})) if (!known.count(u)) out.insert(u);
    }
    return out;
}

// Model paths the disc only has as a variant: multiplayer levels ship 'Mechs and vehicles as
// "<name>_low.NODE" while templates name "<name>.NODE". The world names the very file [Assets]
// ships (the variant the dependency collector resolves). The game itself draws the TEMPLATE's
// path, though (see pick_detail_levels).
int ScenarioIO::resolve_model_paths(const Scenario& s, Wdf& w, const std::unordered_set<uint32_t>& fresh, bool templates) {
    ensure_deps_archives(s);
    static const uint32_t K_MODEL = key_of("Corporal3DObj");
    const ma2m::ArchiveSet& ar = deps_archives_->set;  // 0 = Base, 1 = common.mgf
    int changed = 0;
    for (auto& o : w.objs)
        for (auto& p : o.props) {
            if (p.key != K_MODEL || p.type != PT_STRING) continue;
            std::string path = p.as_string();
            if (path.empty() || ar.ExistsIn(0, path) || ar.ExistsIn(1, path)) continue;
            std::string r = ma2m::ResolveModelPath(ar, path);
            if (r.empty() || iequals(norm_path(r), norm_path(path))) continue;
            // A file outside Base/common ships only for templates new to this Base; the Base
            // level's own templates keep their retail paths.
            int ai = ar.Find(r);
            bool shipped = ai == 0 || ai == 1 || fresh.count(templates ? o.uid() : o.template_id());
            if (!shipped) continue;
            std::string game = "\\" + r;
            for (char& c : game) if (c == '/') c = '\\';
            p.set_string(game);
            changed++;
        }
    return changed;
}

bool ScenarioIO::model_absent(const Scenario& s, uint32_t uid) {
    auto it = absent_.find(uid);
    if (it != absent_.end()) return it->second;
    ensure_deps_archives(s);
    const WObj* t = cat_.object(uid);
    std::string m = t ? t->str("Corporal3DObj") : std::string();
    bool absent = !m.empty() && ma2m::ResolveModelPath(deps_archives_->set, m).empty();
    absent_[uid] = absent;
    return absent;
}

// "Env_MedWall_End_Red.MGMODEL" -> "env_medwall_end": the object without its colour, multiplayer,
// detail, state or "no glow" tags.
static std::string model_core(const std::string& path) {
    std::string n = norm_path(path);
    size_t slash = n.rfind('/');
    std::string stem = n.substr(slash == std::string::npos ? 0 : slash + 1);
    stem = stem.substr(0, stem.find('.'));
    if (stem.compare(0, 4, "arc_") == 0) stem = stem.substr(4);
    static const char* tags[] = {"_no_glow", "_geo_only", "_low", "_mp", "_red", "_blue", "_yellow", "_grey", "_green",
                                 "_arctic", "_locked", "_off", "_new", "_sp", "_game"};
    for (bool again = true; again;) {
        again = false;
        for (const char* t : tags) {
            size_t k = strlen(t);
            if (stem.size() > k && stem.compare(stem.size() - k, k, t) == 0) { stem.resize(stem.size() - k); again = true; }
        }
    }
    return stem;
}

uint32_t ScenarioIO::similar_template(const Scenario& s, uint32_t uid) {
    auto it = similar_.find(uid);
    if (it != similar_.end()) return it->second;
    uint32_t best = 0;
    const TemplateInfo* src = cat_.find(uid);
    if (src && !src->model.empty() && model_absent(s, uid)) {
        std::string core = model_core(src->model);
        for (auto& t : cat_.all()) {
            if (!t.placeable || t.uid == uid || t.cls != src->cls || t.model.empty()) continue;
            std::string n = lower(t.name);
            // Wrecked, collapsed and destroyed states are not the same object.
            if (n.find("_ws") != std::string::npos || n.find("_cs") != std::string::npos || n.find("_ds") != std::string::npos ||
                n.find("destroyed") != std::string::npos)
                continue;
            if (model_core(t.model) != core || model_absent(s, t.uid)) continue;
            best = t.uid;
            break;
        }
    }
    similar_[uid] = best;
    return best;
}

// The game draws a unit with its template's Corporal3DObj (GlobalEntities; the index archive
// leaves the scenario's templates.wdf out) and the instance's detail flags (0xD7584):
// ForceUseHighDetail loads the path as named, ForceUseLowDetail tries "<name>_low.<ext>" first
// (0xD7508) and falls back to the name. A file that is not mounted draws as a magenta pillar
// (2026-10-21: every enemy 'Mech and the Battle Armor in a mission; the empty Loki, which had
// ForceUseLowDetail=1, drew). Templates name "<name>.NODE", which multiplayer levels ship only as
// "<name>_low.NODE": such units get ForceUseLowDetail, the way every retail multiplayer pad does.
void ScenarioIO::pick_detail_levels(const Scenario& s, Wdf& w, const std::vector<std::pair<std::string, std::string>>& assets,
                                    std::vector<std::string>* warnings) {
    ensure_deps_archives(s);
    const ma2m::ArchiveSet& ar = deps_archives_->set;  // 0 = Base, 1 = common.mgf
    std::unordered_set<std::string> shipped;
    for (auto& [arc, path] : assets) shipped.insert(norm_path(path));
    auto mounted = [&](const std::string& p) { return ar.ExistsIn(0, p) || ar.ExistsIn(1, p) || shipped.count(norm_path(p)); };
    static const uint32_t K_HIGH = key_of("ForceUseHighDetail"), K_LOW = key_of("ForceUseLowDetail");
    std::set<std::string> told;
    for (auto& o : w.objs) {
        if (!o.find(K_HIGH) && !o.find(K_LOW)) continue;
        const WObj* t = o.template_id() ? cat_.object(o.template_id()) : nullptr;
        std::string model = t ? t->str("Corporal3DObj") : std::string();
        if (model.empty() || mounted(model)) continue;
        std::string n = norm_path(model);
        size_t dot = n.rfind('.');
        std::string low = dot == std::string::npos ? n + "_low" : n.substr(0, dot) + "_low" + n.substr(dot);
        if (mounted(low)) {
            if (WProp* p = o.find(K_HIGH)) p->set_int(0);
            if (WProp* p = o.find(K_LOW)) p->set_int(1);
            else o.set_i32("ForceUseLowDetail", 1);
        } else if (warnings && told.insert(n).second) {
            warnings->push_back(o.description() + ": its model (" + model + ") is not on the disc for this level; the game will draw a placeholder.");
        }
    }
}

// What the game uploads to graphics memory: textures (.tif/.dds) and the MGMODEL vertex/index
// sub-entries ("{... verts}", "{... indices}").
static bool graphics_file(const std::string& path) {
    std::string p = lower(path);
    return iends_with(p, ".tif") || iends_with(p, ".dds") || p.find(" verts}") != std::string::npos || p.find(" indices}") != std::string::npos;
}

static const uint32_t K_START_CONTAINER = key_of("MAPlayerConfigurationContainer");

// Whether MARoot names exactly the start containers the Base level's retail MARoot names. Then the start units are
// the ones the Base level already runs with, whatever archives happen to hold their models (a retail copy must need no
// [Assets]; the container also lists cinema avatars that the level never loads).
bool ScenarioIO::start_is_retail(const Scenario& s) {
    auto containers = [&](const Wdf& w) {
        std::set<uint32_t> out;
        int ri = find_class(w, K_ROOT);
        if (ri < 0) return out;
        for (auto& p : w.objs[size_t(ri)].props) {
            if (p.data.size() != 4 || (p.type != PT_U32 && p.type != PT_I32)) continue;
            const WObj* t = cat_.object(uint32_t(p.as_int()));
            if (t && t->cls == K_START_CONTAINER) out.insert(uint32_t(p.as_int()));
        }
        return out;
    };
    const MapInfo* bm = base_world_for(gd_, s.base, s.multiplayer);
    const Wdf* bw = bm ? retail_world(bm->name) : nullptr;
    return bw && containers(*bw) == containers(s.world);
}

std::vector<std::pair<std::string, std::string>> ScenarioIO::asset_closure(const Scenario& s, const std::unordered_set<uint32_t>& tpl,
                                                                           uint64_t* bytes, std::vector<AssetUse>* per_file) {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<AssetUse> uses;
    if (bytes) *bytes = 0;
    // Objects and templates new to this Base (see below), fed through the models library's
    // dependency collector (model, sub-buffers, materials, textures, .mtb, particles, .gr2).
    {
        ensure_deps_archives(s);
        (void)tpl;
        std::unordered_set<uint32_t> fresh = fresh_templates(s, s.world);
        Wdf scan;
        for (auto& o : s.world.objs) if (o.template_id() && fresh.count(o.template_id())) scan.objs.push_back(o);
        for (uint32_t u : fresh) if (const WObj* o = cat_.object(u)) scan.objs.push_back(*o);
        Bytes sb = scan.serialize();
        std::vector<ma2m::TemplateModel> models;
        ma2m::ReadTemplateModels(sb.data(), sb.size(), models);
        std::set<std::string> seen;
        // The sky is a world object with its own model (set by the Sky picker). The Base level's
        // own sky needs nothing: its .mtb may be empty, and the library would guess one from
        // another archive (GoCommando's CityNight -> mission_04.mgf).
        std::string base_sky_model, base_sky_mtb;
        if (const MapInfo* bm = base_world_for(gd_, s.base, s.multiplayer))
            if (const Wdf* bw = retail_world(bm->name)) {
                int bi = find_class(*bw, K_SKY);
                if (bi >= 0) { base_sky_model = bw->objs[size_t(bi)].str("Corporal3DObj"); base_sky_mtb = bw->objs[size_t(bi)].str("CorporalMTB"); }
            }
        for (auto& o : s.world.objs) {
            if (o.cls != K_SKY || o.str("Corporal3DObj").empty()) continue;
            if (iequals(o.str("Corporal3DObj"), base_sky_model) && iequals(o.str("CorporalMTB"), base_sky_mtb)) continue;
            ma2m::TemplateModel sky;
            sky.name = "Sky";
            sky.model = ma2m::NormalizePath(o.str("Corporal3DObj"));
            sky.mtb = ma2m::NormalizePath(o.str("CorporalMTB"));
            models.push_back(std::move(sky));
        }
        for (auto& t : models) {
            // Reading and parsing a template's model from the archives is the cost: keep the result per template.
            std::string dk = std::to_string(t.uid) + "|" + std::to_string(t.class_key) + "|" + t.model + "|" + t.mtb;
            for (auto& [k2, v2] : t.extra_files) dk += "|" + k2 + "=" + v2;
            for (auto& [k2, v2] : t.links) dk += "|" + k2 + "#" + std::to_string(v2);
            auto dit = deps_archives_->deps.find(dk);
            if (dit == deps_archives_->deps.end()) {
                std::vector<ma2m::Dependency> fresh_deps;
                ma2m::CollectTemplateDependencies(deps_archives_->set, t, fresh_deps);
                dit = deps_archives_->deps.emplace(std::move(dk), std::move(fresh_deps)).first;
            }
            const std::vector<ma2m::Dependency>& deps = dit->second;
            for (auto& d : deps) {
                if (!d.found || iequals(d.archive, deps_base_) || iequals(d.archive, "common.mgf")) continue;
                std::string k = lower(d.archive) + "|" + lower(d.path);
                if (!seen.insert(k).second) continue;
                out.push_back({lower(d.archive), d.path});
                uint32_t sz = deps_archives_->set.FileSize(d.path);
                if (bytes) *bytes += sz;
                // The owner is the placed template, not a child template it reaches (its own
                // Description, or "Sky").
                std::string owner = t.name;
                if (const TemplateInfo* ti = cat_.find(t.uid)) owner = ti->name;
                uses.push_back({owner, graphics_file(d.path), sz});
            }
        }
        // The reflection map (MAEnvironment EnvironmentMapName), e.g. after a Map tab look from
        // another level.
        if (int ei = find_class(s.world, K_ENV); ei >= 0) {
            std::string em = ma2m::NormalizePath(s.world.objs[size_t(ei)].str("EnvironmentMapName"));
            const ma2m::ArchiveSet& ar = deps_archives_->set;
            int a = em.empty() || ar.ExistsIn(0, em) || ar.ExistsIn(1, em) ? -1 : ar.Find(em);
            if (a >= 0) {
                std::string an = lower(ar.ArchiveName(size_t(a)));
                std::string path = ar.CanonicalPath(em);
                if (!iequals(an, deps_base_) && seen.insert(an + "|" + lower(path)).second) {
                    out.push_back({an, path});
                    uint32_t sz = ar.FileSize(path);
                    if (bytes) *bytes += sz;
                    uses.push_back({"Reflection map", true, sz});
                }
            }
        }
        // The player's start unit: the game loads its template's body, transition and abandoned
        // models exactly as named (no instance detail flags), so those exact files must be mounted;
        // the closure above prefers an archive's _low variant (2026-10-21: Battle Armor and Rommel
        // starts on Mission 09b had no player; research t14_start_vehicle). Each exact path
        // missing from Base and common.mgf is collected from the archive that holds it as named.
        for (uint32_t unit : start_is_retail(s) ? std::vector<uint32_t>() : start_units(s)) {
            const WObj* uo = cat_.object(unit);
            if (!uo) continue;
            Wdf one;
            one.objs.push_back(*uo);
            Bytes ub = one.serialize();
            std::vector<ma2m::TemplateModel> tm;
            ma2m::ReadTemplateModels(ub.data(), ub.size(), tm);
            for (auto& t : tm) {
                std::vector<std::pair<std::string, std::string>> bodies = {{t.model, t.mtb}};
                for (auto& kv : t.extra_files) {
                    if (kv.first == "AbandonedModel" || kv.first == "TransitionModel") {
                        std::string mtb_key = kv.first == "AbandonedModel" ? "AbandonedMTB" : "TransitionMTB", mtb;
                        for (auto& kv2 : t.extra_files) if (kv2.first == mtb_key) mtb = kv2.second;
                        bodies.push_back({kv.second, mtb});
                    }
                }
                for (auto& [path, mtb] : bodies) {
                    if (path.empty()) continue;
                    const ma2m::ArchiveSet& ar = deps_archives_->set;
                    if (ar.ExistsIn(0, path) || ar.ExistsIn(1, path)) continue;
                    int a = ar.Find(path);  // exact name only
                    if (a < 0) continue;
                    ma2m::ArchiveSet mini;  // the holder first, so the exact file wins
                    mini.Add(ar.ArchivePath(size_t(a)));
                    mini.Add(ar.ArchivePath(0));
                    mini.Add(ar.ArchivePath(1));
                    std::vector<ma2m::Dependency> deps;
                    ma2m::CollectDependencies(mini, path, mtb, deps);
                    std::string owner = t.name.empty() ? "Player start unit" : t.name;
                    // The unit's own folder ships its low-detail model, low NODE and footprint material next to the
                    // body ("<unit>/model_low", "<unit>_low.NODE", "Model/vtr_footprint.mat"): the game draws and
                    // animates the player from them. A mech start on a level that lacked them (Mission 11a, Mad Dog)
                    // had no HUD or control and a magenta placeholder body (2026-10-25, proven in game).
                    {
                        std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
                        if (iends_with(lower(dir), "/model")) dir = dir.substr(0, dir.size() - 6);
                        std::string pre = lower(dir) + "/";
                        for (auto& f : ar.List(size_t(a))) {
                            std::string lf = lower(f);
                            if (lf.compare(0, pre.size(), pre) != 0 || f.find('{') != std::string::npos) continue;
                            bool is_model = iends_with(lf, ".mgmodel") || iends_with(lf, ".node");
                            if (!is_model && !iends_with(lf, ".mat")) continue;
                            if (ar.ExistsIn(0, f) || ar.ExistsIn(1, f)) continue;
                            if (is_model) {
                                std::vector<ma2m::Dependency> more;  // CollectDependencies replaces what it is given
                                ma2m::CollectDependencies(mini, f, mtb, more);
                                deps.insert(deps.end(), more.begin(), more.end());
                            }
                            else {
                                ma2m::Dependency dd;
                                dd.found = true;
                                dd.archive = ar.ArchiveName(size_t(a));
                                dd.path = f;
                                deps.push_back(dd);
                            }
                        }
                    }
                    for (auto& d : deps) {
                        if (!d.found || iequals(d.archive, deps_base_) || iequals(d.archive, "common.mgf")) continue;
                        std::string k = lower(d.archive) + "|" + lower(d.path);
                        if (!seen.insert(k).second) continue;
                        out.push_back({lower(d.archive), d.path});
                        uint32_t sz = mini.FileSize(d.path);
                        if (bytes) *bytes += sz;
                        uses.push_back({owner, graphics_file(d.path), sz});
                    }
                }
            }
        }
        // Files the game's code needs for a class (TemplateCatalog::code_required): every MABuilding needs the four
        // building-destruction effects; a Base without them (Mission_08, Mission_06b, In Your Face) drew every placed
        // building as the pink placeholder (2026-11-09, Office15 on a Mission_08 copy).
        {
            std::set<std::string> req;
            for (uint32_t u : cat_.closure(cat_.seed_of(s.world)))
                for (auto& f : cat_.code_required(u)) req.insert(f);
            const ma2m::ArchiveSet& ar = deps_archives_->set;
            ma2m::TemplateModel code;
            code.name = "Buildings (game code)";
            for (auto& f : req) {
                std::string n = ma2m::NormalizePath(f);
                if (!ar.ExistsIn(0, n) && !ar.ExistsIn(1, n)) code.extra_files.push_back({"CodeRequired", n});
            }
            if (!code.extra_files.empty()) {
                std::vector<ma2m::Dependency> deps;
                ma2m::CollectTemplateDependencies(ar, code, deps);
                for (auto& d : deps) {
                    if (!d.found || iequals(d.archive, deps_base_) || iequals(d.archive, "common.mgf")) continue;
                    std::string k = lower(d.archive) + "|" + lower(d.path);
                    if (!seen.insert(k).second) continue;
                    out.push_back({lower(d.archive), d.path});
                    uint32_t sz = ar.FileSize(d.path);
                    if (bytes) *bytes += sz;
                    uses.push_back({code.name, graphics_file(d.path), sz});
                }
            }
        }
        road_assets(s, out, &uses, bytes);  // road pieces (roads.cpp)
        // Sorted by (archive, path), with per_file kept in step.
        std::vector<size_t> order(out.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return out[a] < out[b]; });
        std::vector<std::pair<std::string, std::string>> sorted;
        sorted.reserve(out.size());
        for (size_t i : order) {
            sorted.push_back(out[i]);
            if (per_file) per_file->push_back(uses[i]);
        }
        return sorted;
    }
}

// ----------------------------------------------------------------------------- custom-size water

bool ScenarioIO::water_custom_size(const WObj& o, float& w, float& l) {
    static const std::regex re(R"(\s*\[(\d+)\s*x\s*(\d+)\s*m(?:\s+[a-z]+)?\]\s*$)");
    std::string d = o.description();
    std::smatch m;
    if (!std::regex_search(d, m, re)) return false;
    w = std::stof(m[1]);
    l = std::stof(m[2]);
    return w >= wcustom::kMinSide - 0.01f && l >= wcustom::kMinSide - 0.01f;
}

wcustom::Shape ScenarioIO::water_custom_shape(const WObj& o) {
    static const std::regex re(R"(\[\d+\s*x\s*\d+\s*m\s+oval\]\s*$)");
    std::string d = o.description();
    return std::regex_search(d, re) ? wcustom::Shape::Oval : wcustom::Shape::Rect;
}

std::string ScenarioIO::water_description(const std::string& name, float w, float l, wcustom::Shape shape) {
    char b[64];
    snprintf(b, sizeof b, " [%d x %d m%s]", int(std::lround(w)), int(std::lround(l)), shape == wcustom::Shape::Oval ? " oval" : "");
    return name + b;
}

std::string ScenarioIO::water_name(const WObj& o) {
    static const std::regex re(R"(\s*\[\d+\s*x\s*\d+\s*m(?:\s+[a-z]+)?\]\s*$)");
    return std::regex_replace(o.description(), re, "");
}

std::string ScenarioIO::water_custom_name(const WObj& o, const std::string& template_name) {
    static const std::regex typed(R"(^(env_)?water[ _a-z]*\d+\s*[x_]\s*\d+)", std::regex::icase);
    std::string n = trim(water_name(o));
    if (n.empty() || iequals(n, template_name) || std::regex_search(n, typed)) return std::string();
    return n;
}

namespace {
struct WaterSource {
    std::string canon, vname, iname;  // the model as spelled in its archive; its vertex and index entries
    Bytes xml;
    wcustom::Source g;
};
}  // namespace

// The files of a plain water model (one grid, one material_water) from the disc.
static bool read_water_source(const ma2m::ArchiveSet& ar, const std::string& model, WaterSource& ws) {
    std::string canon = ar.CanonicalPath(ma2m::NormalizePath(model));
    if (canon.empty()) return false;
    std::string v, i;
    int nv = 0, ni = 0;
    for (auto& f : ar.ListAll(canon + "{")) {
        if (!istarts_with(f, canon + "{")) continue;
        if (iends_with(f, " verts}")) { v = f; nv++; }
        else if (iends_with(f, " indices}")) { i = f; ni++; }
    }
    if (nv != 1 || ni != 1) return false;
    if (!ar.Read(canon, ws.xml) || !ar.Read(v, ws.g.verts) || !ar.Read(i, ws.g.idx)) return false;
    std::string x(ws.xml.begin(), ws.xml.end());
    if (x.find("<material_water") == std::string::npos || x.find("<meshxml", x.find("<meshxml") + 1) != std::string::npos) return false;
    ws.canon = canon;
    ws.vname = v;
    ws.iname = i;
    return wcustom::plain(ws.g);
}

bool ScenarioIO::water_resizable(const Scenario& s, uint32_t template_uid) {
    ensure_deps_archives(s);
    const TemplateInfo* t = cat_.find(template_uid);
    WaterSource ws;
    return t && !t->model.empty() && read_water_source(deps_archives_->set, t->model, ws);
}

// Each water object with a size in its Description gets a model of that size in the scenario's
// archive, beside the type's model (MA2SE_<type>_<W>x<L>.MGMODEL), and its instance Corporal3DObj points at it.
// MAWater has no size property: the plane is its model's bounding box. Tested in game 2026-10-24
// (UnderCover copy, 60 x 20 m): the game draws the instance's model, not only the template's;
// replacing the template's own geometry at its retail path draws nothing. Waters of one type and
// size share one model. The editor keeps the template's model in memory (the Description holds
// the size), so only the saved world carries the generated path.
void ScenarioIO::add_custom_water(const Scenario& s, Wdf& w, MgfWriter& mgf, std::vector<std::string>* warnings) {
    ensure_deps_archives(s);
    const ma2m::ArchiveSet& ar = deps_archives_->set;
    std::map<std::string, std::string> made;  // "model|W x L" -> the game path written
    for (auto& o : w.objs) {
        float cw, cl;
        if (o.cls != K_WATER || !water_custom_size(o, cw, cl)) continue;
        const TemplateInfo* t = cat_.find(o.template_id());
        std::string model = t ? t->model : o.str("Corporal3DObj");
        WaterSource ws;
        if (!read_water_source(ar, model, ws)) {
            if (warnings) warnings->push_back(o.description() + ": this water type cannot be resized; it keeps its own size.");
            continue;
        }
        char sz[32];
        wcustom::Shape shape = water_custom_shape(o);
        snprintf(sz, sizeof sz, "_%dx%d%s", int(std::lround(cw)), int(std::lround(cl)), shape == wcustom::Shape::Oval ? "oval" : "");
        std::string key = lower(ws.canon) + sz, game;
        auto it = made.find(key);
        if (it != made.end()) game = it->second;
        else {
            Bytes v, i;
            std::string err;
            bool as_list = false;
            if (!wcustom::generate(ws.g, float(std::lround(cw)), float(std::lround(cl)), shape, v, i, &as_list, &err)) {
                if (warnings) warnings->push_back(o.description() + ": " + err);
                continue;
            }
            size_t slash = ws.canon.find_last_of('/'), dot = ws.canon.rfind('.');
            std::string stem = ws.canon.substr(slash + 1, dot - slash - 1);
            // Beside the source model, not in a folder of its own: the model's texture paths are relative to its folder.
            std::string base = ws.canon.substr(0, slash + 1) + "MA2SE_" + stem + sz + ".MGMODEL";
            Bytes xml = ws.xml;
            if (as_list) {  // the generated triangles are a list, not the source's strips
                std::string x(xml.begin(), xml.end());
                size_t t = x.find("indexedstrip");
                if (t != std::string::npos) x.replace(t, 12, "indexedlist");
                xml.assign(x.begin(), x.end());
            }
            mgf.add(base, xml);
            mgf.add(base + ws.vname.substr(ws.canon.size()), v);
            mgf.add(base + ws.iname.substr(ws.canon.size()), i);
            game = "\\" + base;
            for (char& c : game) if (c == '/') c = '\\';
            made[key] = game;
        }
        o.set_str("Corporal3DObj", game);
    }
}

// Cropped road pieces and foundations (crop.h, playtest 11-01). The cut files sit beside their source, so the relative
// texture and material paths inside them still resolve (the source's own files come from Base, common.mgf or [Assets],
// which asset_closure lists from the uncut world before this runs). Pieces cut the same way share one set of files.
std::vector<ScenarioIO::CropSaved> ScenarioIO::add_cropped(const Scenario& s, Wdf& w, MgfWriter& mgf, std::vector<std::string>* warnings) {
    static const uint32_t K_ROAD = key_of("MARoad");
    std::vector<CropSaved> saved;
    ensure_deps_archives(s);
    const ma2m::ArchiveSet& ar = deps_archives_->set;
    std::map<std::string, std::string> made;  // "source|rect" -> the game path written
    for (auto& o : w.objs) {
        crop::Rect r;
        const bool has = crop::of(o, r);
        o.erase(key_of(crop::kProp));  // editor-only: the game never sees it
        if (!has) continue;
        const bool road = o.cls == K_ROAD;
        const std::string orig = o.str(road ? "RoadDefinition" : "Corporal3DObj");
        if (orig.empty()) continue;
        const std::string key = lower(ma2m::NormalizePath(orig)) + "|" + crop::format(r);
        std::string game;
        if (auto it = made.find(key); it != made.end()) game = it->second;
        else {
            std::vector<crop::OutFile> files;
            std::string cut, err;
            bool ok = road ? crop::make_road(ar, orig, r, files, cut, &err) : crop::make_model(ar, orig, r, files, cut, &err);
            if (!ok) {
                if (warnings) warnings->push_back(o.description() + ": could not be cropped (" + err + "); the game shows it whole.");
                continue;
            }
            for (auto& f : files) mgf.add(f.path, std::move(f.data));
            // The game's own spelling: backslashes; a road piece's RoadDefinition has no leading one (place_road), a model path does.
            game = std::string(road ? "" : "\\") + cut;
            for (char& c : game) if (c == '/') c = '\\';
            made[key] = game;
        }
        o.set_str(road ? "RoadDefinition" : "Corporal3DObj", game);
        // Reopening restores the path as the editor had it (resolve_model_paths may have respelled the saved one).
        std::string mine = orig;
        for (auto& so : s.world.objs)
            if (so.uid() == o.uid()) { if (std::string p = so.str(road ? "RoadDefinition" : "Corporal3DObj"); !p.empty()) mine = p; break; }
        saved.push_back({o.uid(), crop::format(r), mine, road});
    }
    return saved;
}

// Check scenario / save: the graphics-memory estimate against the game's 64 MB.
void ScenarioIO::graphics_check(const Scenario& s, std::vector<ValidationItem>& v, double* mb) {
    GraphicsEstimate g = graphics_estimate(s);
    if (mb) *mb = g.total_mb;
    char n[16];
    snprintf(n, sizeof n, "%.0f", g.total_mb);
    if (g.total_mb >= kGraphicsErrorMb) {
        std::string list;
        for (size_t i = 0; i < g.biggest.size() && i < 5; i++) {
            char mb[16];
            snprintf(mb, sizeof mb, "%.1f", g.biggest[i].second);
            list += (i ? ", " : "") + g.biggest[i].first + " (" + mb + " MB)";
        }
        v.push_back({2, "This map needs about " + std::string(n) + " MB of graphics memory, but the game only has 64 MB. It will crash while "
                        "loading. Remove some objects that come from other levels; the biggest are: " + (list.empty() ? "(none)" : list) + ".", 0});
    } else if (g.total_mb >= kGraphicsWarnMb) {
        // Game test (g4, 2026-10-25): a Mission_16 copy with 80 distinct models from other levels (estimate ~50 MB) made the
        // game exit while loading; every file with an estimate below 48 MB (15 of them, 40 models each) loaded.
        v.push_back({1, "This map uses a lot of graphics memory: about " + std::string(n) + " of the game's 64 MB (retail maps use about 25 MB). "
                        "The game may not load it (a test map of about 50 MB closed the game while loading). Each extra 'Mech or building "
                        "from another level adds about 1-5 MB: remove some to get under 48 MB.", 0});
    }
}

ScenarioIO::GraphicsEstimate ScenarioIO::graphics_estimate(const Scenario& s) {
    GraphicsEstimate g;
    ensure_deps_archives(s);
    // Base archive (index 0 of the dependency set), cached per Base.
    if (!iequals(gfx_base_name_, s.base)) {
        gfx_base_name_ = s.base;
        uint64_t b = 0;
        for (auto& p : deps_archives_->set.List(0))
            if (graphics_file(p)) b += deps_archives_->set.FileSize(p);
        gfx_base_bytes_ = b;
    }
    std::vector<AssetUse> uses;
    asset_closure(s, {}, nullptr, &uses);
    std::map<std::string, uint64_t> by_owner;
    uint64_t a = 0;
    for (auto& u : uses)
        if (u.graphics) { a += u.bytes; by_owner[u.owner] += u.bytes; }
    g.base_mb = double(gfx_base_bytes_) / 1048576.0;
    g.assets_mb = double(a) / 1048576.0;
    g.total_mb = 8.0 + 0.88 * (g.base_mb + g.assets_mb);
    for (auto& [o, b] : by_owner) g.biggest.push_back({o, double(b) / 1048576.0});
    std::sort(g.biggest.begin(), g.biggest.end(), [](auto& x, auto& y) { return x.second > y.second; });
    return g;
}

// ----------------------------------------------------------------------------- validation

static const std::map<std::string, std::vector<std::string>>& mode_requirements() {
    // Classes that every retail map with the mode places inside its MAMissionTypeGroups.
    static const std::map<std::string, std::vector<std::string>> req = {
        {"CTF", {"MAFlagCTF"}},
        {"SnatchIt", {"MASnatchItBase", "MASnatchItDataCore"}},
        {"BaseWar", {"MAObjectiveType"}},
        // Check It (bots F13): the control plates (MACheckpoint) and their 'Check It!' terminals sit in the
        // retail maps' "Check-It! Common" group; without them a Check It match ended 0-0 after ~5 s.
        {"CheckIt", {"MACheckpoint"}},
    };
    return req;
}

// MP ground rules (maintainer, 2026-10-20). Spawns, vehicle pads and the mode objects players
// carry or reach (CTF flags, SnatchIt bases and cores) must stand on type 0 or 8: every retail one
// does (590 spawns, 1,022 pads, 63 flags and SnatchIt objects), and bots never move onto 1.
// BaseWar targets (MAObjectiveType buildings) are exempt: Deliverance's power plants stand on 1. They should also share one connected area. Cells of other values
// (16, 32, ...) join areas: retail Deliverance and TroubledWaters link their bases through 32.
// Retail KillingField, OnTheRocks, RushHour and UnderCover keep a few spawns on separate ledges.
static void validate_ground(const Scenario& s, const WTree& t, std::vector<ValidationItem>& v) {
    int W = s.typemap_w, H = s.typemap_h;
    if (s.typemap.empty() || W <= 0 || H <= 0) return;
    static const uint32_t K_SPAWNER = key_of("MASpawner");
    static const uint32_t objective_classes[] = {key_of("MAFlagCTF"), key_of("MASnatchItBase"), key_of("MASnatchItDataCore")};
    struct Spot { uint32_t uid; const char* what; std::string name; Vec3 p; };
    std::vector<Spot> spots;
    for (size_t i = 0; i < s.world.objs.size(); i++) {
        const WObj& o = s.world.objs[i];
        Vec3 p;
        if (o.cls == K_LEVELSTART) {
            if (get_pos(o, p)) spots.push_back({o.uid(), "Spawn point", o.description(), p});
        } else if (o.cls == K_SPAWNER) {
            int par = t.parent_of[i];
            if (par >= 0 && get_pos(s.world.objs[size_t(par)], p))
                spots.push_back({s.world.objs[size_t(par)].uid(), "Vehicle pad", s.world.objs[size_t(par)].description(), p});
        } else if (std::find(std::begin(objective_classes), std::end(objective_classes), o.cls) != std::end(objective_classes)) {
            if (get_pos(o, p)) spots.push_back({o.uid(), "Objective", o.description(), p});
        }
    }
    auto cell = [&](const Vec3& p) {
        int tx = std::clamp(int(p.x / s.terrain.size_x() * float(W)), 0, W - 1);
        int tz = std::clamp(int(p.z / s.terrain.size_z() * float(H)), 0, H - 1);
        return size_t(tz) * W + tx;
    };
    int bad = 0;
    for (auto& sp : spots) {
        if (!s.terrain.inside(sp.p.x, sp.p.z)) continue;  // reported as "outside the map"
        uint16_t val = s.typemap[cell(sp.p)];
        if (tmtype::playable(val)) continue;
        if (++bad > 20) continue;
        const char* l = tmtype::label(val);
        v.push_back({2, std::string(sp.what) + " \"" + sp.name + "\" stands where players cannot go (" + (l ? std::string(l) : "type " + std::to_string(val)) +
                            " ground). Move it further into the map, or on the Terrain tab pick Ground type > Playable and paint over it.",
                     sp.uid});
    }
    if (bad > 20) v.push_back({2, std::to_string(bad - 20) + " more spawn points, pads or objectives stand where players cannot go.", 0});
    // Connected areas (4-neighbour) over cells that are neither out of bounds nor border.
    std::vector<int> lab(s.typemap.size(), -1);
    std::vector<size_t> sizes, queue;
    size_t total = 0;
    for (size_t i = 0; i < s.typemap.size(); i++) {
        if ((s.typemap[i] & 3) || lab[i] >= 0) continue;
        int L = int(sizes.size());
        lab[i] = L;
        queue.assign(1, i);
        for (size_t q = 0; q < queue.size(); q++) {
            size_t k = queue[q];
            int x = int(k % size_t(W)), y = int(k / size_t(W));
            const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
            for (int d = 0; d < 4; d++) {
                if (nx[d] < 0 || nx[d] >= W || ny[d] < 0 || ny[d] >= H) continue;
                size_t j = size_t(ny[d]) * W + nx[d];
                if (lab[j] < 0 && !(s.typemap[j] & 3)) { lab[j] = L; queue.push_back(j); }
            }
        }
        sizes.push_back(queue.size());
        total += queue.size();
    }
    if (!total) {
        v.push_back({2, "No ground on this map is marked Playable. On the Terrain tab press \"Make the whole map playable\".", 0});
        return;
    }
    std::set<int> used;
    for (auto& sp : spots)
        if (s.terrain.inside(sp.p.x, sp.p.z) && lab[cell(sp.p)] >= 0) used.insert(lab[cell(sp.p)]);
    int big = 0;
    size_t largest = 0;
    for (size_t n : sizes) { if (n * 100 >= total) big++; largest = std::max(largest, n); }
    char buf[400];
    if (used.size() > 1) {
        snprintf(buf, sizeof buf,
                 "Spawn points, pads and objectives sit in %zu separate areas that are walled off from each other by Border or Out of "
                 "bounds ground, so bots cannot walk between them. Paint a Playable path between them on the Terrain tab "
                 "(Show where players may go helps).",
                 used.size());
        v.push_back({1, buf, 0});
    } else if (big > 1) {
        snprintf(buf, sizeof buf, "The playable ground is split into %d separate parts (the largest holds %d%% of it); bots cannot walk "
                                  "between them. Paint a Playable path between them on the Terrain tab.",
                 big, int(largest * 100 / total));
        v.push_back({1, buf, 0});
    }
}

// Every game type copies its retail objects, not only the types with required pieces: Team Destruction's
// vehicle bays and turrets sit in its own group ("TD Specific"), so a TD map got no vehicles (bots F15).
bool ScenarioIO::mode_needs_kit(const std::string& gt) {
    return find_game_type(gt) != nullptr;
}

// Plain-language checks shown in the editor's Check scenario window. Level 2 (error) stops Test;
// every message says how to fix the problem.
static const char* requirement_label(const std::string& cls) {
    if (cls == "MAFlagCTF") return "the Red and Blue flags";
    if (cls == "MASnatchItBase") return "the Snatch It bases";
    if (cls == "MASnatchItDataCore") return "the Snatch It data core";
    if (cls == "MAObjectiveType") return "the team bases to destroy";
    if (cls == "MACheckpoint") return "the checkpoints (control plates) to take and hold";
    return "its objects";
}

void set_unit_side(WObj& o, int nation) {
    if (o.find("Nation")) o.set_i32("Nation", nation);
    bool crewed = nation != 0;
    if (o.find("StartAbandoned")) o.set_i32("StartAbandoned", crewed ? 0 : 1);
    if (crewed) {
        if (o.find("AttackPilot")) o.set_i32("AttackPilot", 1);
        if (o.find("InitiallyCollidable")) o.set_i32("InitiallyCollidable", 1);
        if (o.find("ForceUseLowDetail")) o.set_i32("ForceUseLowDetail", 0);
    }
}

// MP layout checks: spawn spacing and spots in walled-off areas.
static void layout_warnings(const Scenario& s, const WTree& t, std::vector<ValidationItem>& v) {
    // Spawn spacing: stacked spawns, and enemy teams spawning on top of each other.
    struct Sp { uint32_t uid; int team; Vec3 p; };
    std::vector<Sp> sp;
    for (auto& o : s.world.objs) {
        Vec3 p;
        if (o.cls == K_LEVELSTART && get_pos(o, p)) sp.push_back({o.uid(), o.i32("TeamIndex"), p});
    }
    int stacked = 0, close = 0;
    uint32_t stacked_uid = 0, close_uid = 0;
    for (size_t i = 0; i < sp.size(); i++)
        for (size_t j = i + 1; j < sp.size(); j++) {
            float dx = sp[i].p.x - sp[j].p.x, dz = sp[i].p.z - sp[j].p.z, d2 = dx * dx + dz * dz;
            // Retail maps put a free-for-all start (team 0) on a team start: they are never used in the same match.
            if (d2 < 3.f * 3.f) { if (sp[i].team == sp[j].team && !stacked++) stacked_uid = sp[i].uid; }
            else if (sp[i].team && sp[j].team && sp[i].team != sp[j].team && d2 < 60.f * 60.f) { if (!close++) close_uid = sp[i].uid; }
        }
    if (stacked)
        v.push_back({1, std::to_string(stacked) + " pair(s) of spawn points stand on top of each other, so players spawn inside each other. "
                                                  "Click here to see one, then drag it a few metres away.", stacked_uid});
    if (close)
        v.push_back({1, "Red and Blue spawn points are less than 60 m apart, so players get shot the moment they spawn. Move one team's "
                        "spawns to the other side of the map (click here to see one).", close_uid});
    validate_ground(s, t, v);
}

const std::vector<std::string>* ScenarioIO::retail_layout(const Scenario& s) {
    if (!s.multiplayer || s.read_only || s.typemap.empty()) return nullptr;
    const std::string res = lower(s.base);
    const MapInfo* m = nullptr;
    for (auto& mi : gd_.maps()) if (mi.multiplayer && mi.resource == res) { m = &mi; break; }
    if (!m) return nullptr;
    auto it = retail_layout_.find(res);
    if (it == retail_layout_.end()) {
        std::vector<std::string> texts;
        Scenario r;
        r.multiplayer = true;
        if (gd_.load_world(*m, r.world) && r.terrain.load_from_world(r.world)) {
            load_typemap(r);
            WTree rt;
            rt.build(r.world);
            std::vector<ValidationItem> lay;
            layout_warnings(r, rt, lay);
            for (auto& x : lay) texts.push_back(x.text);
            texts.push_back(std::to_string(r.terrain.w) + "x" + std::to_string(r.terrain.h) + "/" + std::to_string(r.typemap_w));
        }
        it = retail_layout_.emplace(res, std::move(texts)).first;
    }
    // Only a copy: same ground size and type-map size as the real level (a new map on its archive is not one).
    const std::string size = std::to_string(s.terrain.w) + "x" + std::to_string(s.terrain.h) + "/" + std::to_string(s.typemap_w);
    if (it->second.empty() || it->second.back() != size) return nullptr;
    return &it->second;
}

std::vector<ValidationItem> ScenarioIO::validate(const Scenario& s) {
    std::vector<ValidationItem> v;
    size_t n = s.world.objs.size();
    if (n > 3000)
        v.push_back({2, "The map has " + std::to_string(n) + " objects; the game cannot load more than 3,000. Delete some (scattered trees add up fast).", 0});
    else if (n > 2300)  // the biggest real level, Mission_03, has 2,098 (a copy plus a cutscene is ~2,140)
        v.push_back({1, "The map has " + std::to_string(n) + " objects, more than any real level (the biggest has about 2,100); the game may slow down.", 0});
    // Two objects with one UID: links (triggers, orders, markers) go to whichever the game finds first.
    {
        std::set<uint32_t> seen_uid, told;
        for (auto& o : s.world.objs)
            if (o.uid() && !seen_uid.insert(o.uid()).second && told.insert(o.uid()).second)
                v.push_back({1, o.description() + ": another object has the same internal ID, so triggers and orders that point at it may act on the wrong one. Delete one of the two (or copy it again).", o.uid()});
    }
    // The game reads only sound clips 1-63 ([Audio] ids above that are silently missing in the game).
    {
        int over = 0;
        for (auto& [id, clip] : s.audio) over += id > 63 || id < 1;
        if (over)
            v.push_back({1, std::to_string(over) + " imported sound clip(s) have a number above 63; the game plays only clips 1 to 63, so those stay silent. Delete some clips and import them again.", 0});
    }
    // Objects whose model was cut before release: the game draws a magenta placeholder.
    {
        std::set<uint32_t> told;
        for (auto& o : s.world.objs) {
            uint32_t tid = o.template_id();
            if (!tid || !model_absent(s, tid) || !told.insert(tid).second) continue;
            std::string text = o.description() + ": this object's 3D model is not on the game disc (it was cut before release), so the game shows a pink placeholder. Delete it";
            if (uint32_t alt = similar_template(s, tid)) if (const TemplateInfo* a = cat_.find(alt)) text += ", or use \"" + a->name + "\" (the same object) instead";
            v.push_back({1, text + ".", o.uid()});
        }
    }
    // A patrol route with fewer than 2 points: the unit has nowhere to go. (A patrol with no route at all is left alone:
    // retail has those, e.g. Mission_01b "tank at wall (was raptor)".)
    {
        static const uint32_t K_PATROL = key_of("MAPatrol");
        for (auto& o : s.world.objs) {
            if (o.cls != K_PATROL) continue;
            uint32_t ev = o.parent(), g = order_path(s.world, ev);
            int pts = 0;
            if (!g) continue;
            for (auto& w : s.world.objs) if (w.parent() == g) pts++;
            if (pts >= 2) continue;
            uint32_t u = order_unit(s.world, ev);
            std::string name = "A unit";
            for (auto& w : s.world.objs) if (w.uid() == u) name = w.description();
            v.push_back({1, name + ": its patrol route has " + std::to_string(pts) + (pts == 1 ? " point" : " points") +
                                ", so it will not move. On the Objectives & Triggers tab, open its orders and press Draw on map (2 or more points), or change the order.", u});
        }
    }
    WTree t;
    t.build(s.world);
    // Which modes is each object active in? (intersection of MAMissionTypeGroup ancestors)
    auto active_in = [&](int i, const GameTypeDef& g) {
        int p = t.parent_of[size_t(i)];
        while (p >= 0) {
            const WObj& o = s.world.objs[size_t(p)];
            if (o.cls == K_MTG) {
                const WProp* f = o.find(g.flag);
                if (!f || !f->as_int()) return false;
            }
            p = t.parent_of[size_t(p)];
        }
        return true;
    };
    if (s.multiplayer) {
        if (s.types.empty()) v.push_back({2, "No game type is ticked. On the Map tab, tick at least one (for example Team Destruction).", 0});
        for (auto& ty : s.types) {
            const GameTypeDef* g = find_game_type(ty);
            if (!g) { v.push_back({1, "Unknown game type \"" + ty + "\"; untick it on the Map tab.", 0}); continue; }
            int teams[3] = {0, 0, 0};
            for (size_t i = 0; i < s.world.objs.size(); i++) {
                const WObj& o = s.world.objs[i];
                if (o.cls != K_LEVELSTART || !active_in(int(i), *g)) continue;
                int ti = o.i32("TeamIndex");
                if (ti >= 0 && ti <= 2) teams[ti]++;
            }
            auto fix = [](const char* team) {
                return std::string(" Units and Props tab, Starts: choose ") + team + ", then \"Add 6 on a ring around the camera\" or \"Place spawn point\".";
            };
            if (g->team) {
                if (teams[1] < 6) v.push_back({2, std::string(g->label) + ": Red has " + std::to_string(teams[1]) + " spawn points and needs at least 6." + fix("Red"), 0, 1, 6 - teams[1]});
                if (teams[2] < 6) v.push_back({2, std::string(g->label) + ": Blue has " + std::to_string(teams[2]) + " spawn points and needs at least 6." + fix("Blue"), 0, 2, 6 - teams[2]});
            } else if (teams[0] < 6) {
                v.push_back({2, std::string(g->label) + ": there are " + std::to_string(teams[0]) +
                                    " free-for-all spawn points and it needs at least 6." + fix("Free-for-all"), 0, 0, 6 - teams[0]});
            }
            auto it = mode_requirements().find(g->ini);
            if (it != mode_requirements().end())
                for (auto& cls : it->second) {
                    uint32_t k = key_of(cls);
                    int c = 0;
                    for (size_t i = 0; i < s.world.objs.size(); i++) if (s.world.objs[i].cls == k && active_in(int(i), *g)) c++;
                    if (!c)
                        v.push_back({2, std::string(g->label) + " needs " + requirement_label(cls) + ". Units and Props tab, Starts: press \"Add " + g->label +
                                            "\" under Game-mode objects, then move the pieces where you want them.", 0});
                }
            // Per-team pieces. Base War: every retail map has objective 0 (Red's power plant) and 1
            // (Blue's); without one the match ends at once (seen in game: "Red Team Wins", 0 kills).
            if (g->ini == std::string("BaseWar")) {
                bool have[2] = {false, false};
                for (size_t i = 0; i < s.world.objs.size(); i++) {
                    const WObj& o = s.world.objs[i];
                    if (o.cls != key_of("MAObjectiveType") || !active_in(int(i), *g)) continue;
                    int k = o.i32("ObjectiveNumber");
                    if (k == 0 || k == 1) have[k] = true;
                }
                bool any = false;
                for (auto& o : s.world.objs) any |= o.cls == key_of("MAObjectiveType");
                if (any)
                    for (int k = 0; k < 2; k++)
                        if (!have[k])
                            v.push_back({2, std::string("Base War: ") + (k ? "Blue" : "Red") + "'s power plant is missing (deleted?), so the match would end the "
                                            "moment it starts. Undo the delete (Ctrl+Z); if that is no longer possible, untick Base War on the Map tab.", 0});
            }
            // Check It: each retail plate has a menu-activate terminal whose 'Check It!' action (MAActionCheckItGoal)
            // names it in CheckPointUID; the bots claim plates through that action.
            if (g->ini == std::string("CheckIt")) {
                std::unordered_set<uint32_t> goals;
                for (auto& o : s.world.objs)
                    if (o.cls == key_of("MAActionCheckItGoal")) goals.insert(o.u32("CheckPointUID"));
                for (size_t i = 0; i < s.world.objs.size(); i++) {
                    const WObj& o = s.world.objs[i];
                    if (o.cls != key_of("MACheckpoint") || !active_in(int(i), *g) || goals.count(o.uid())) continue;
                    v.push_back({1, "Check It: checkpoint \"" + o.description() + "\" has no 'Check It!' terminal, so no one can claim it. Delete it and press \"Add Check It\" "
                                    "under Game-mode objects (Units and Props tab), which brings each checkpoint with its terminal.", o.uid()});
                }
            }
            // Capture the Flag: a flag for each team (flags are named Red / Blue).
            if (g->ini == std::string("CTF")) {
                int red = 0, blue = 0, all = 0;
                for (size_t i = 0; i < s.world.objs.size(); i++) {
                    const WObj& o = s.world.objs[i];
                    if (o.cls != key_of("MAFlagCTF") || !active_in(int(i), *g)) continue;
                    all++;
                    std::string d = lower(o.description());
                    red += d.find("red") != std::string::npos;
                    blue += d.find("blue") != std::string::npos;
                }
                if (all && (red + blue ? (!red || !blue) : all < 2))
                    v.push_back({2, std::string("Capture the Flag needs a flag for each team, but ") + (!red ? "Red's" : "Blue's") +
                                    " flag is missing. Undo the delete (Ctrl+Z); if that is no longer possible, untick Capture the Flag on the Map tab.", 0});
            }
        }
        // Spawn spacing + walled-off areas; on a same-size copy of a real MP level the warnings that level itself
        // gives (UnderCover's ledges, RushHour's stacked spawns) are left out: the user did not make them.
        {
            std::vector<ValidationItem> lay;
            layout_warnings(s, t, lay);
            const std::vector<std::string>* base = retail_layout(s);
            for (auto& it : lay)
                if (!base || it.level == 2 || std::find(base->begin(), base->end(), it.text) == base->end()) v.push_back(std::move(it));
        }
        // Players who start on foot need a vehicle pad (a spawner holding a 'Mech or vehicle) to climb into (MP dogfood 10-26).
        {
            static const uint32_t K_SPAWNER = key_of("MASpawner"), K_MECH = key_of("MAMech"), K_TANK = key_of("MATank"),
                                  K_FLYER = key_of("MAFlyer"), K_VEHICLE = key_of("MAVehicle");
            uint32_t sc = start_container(s);
            bool on_foot = false;
            for (auto& c : start_choices()) if (c.container == sc) on_foot = c.kind == 0;
            bool pad = false;
            for (size_t i = 0; i < s.world.objs.size() && !pad; i++) {
                if (s.world.objs[i].cls != K_SPAWNER) continue;
                for (int k : t.kids[i]) {
                    uint32_t c = s.world.objs[size_t(k)].cls;
                    if (c == K_MECH || c == K_TANK || c == K_FLYER || c == K_VEHICLE) pad = true;
                }
            }
            if (on_foot && !pad)
                v.push_back({1, "Players start on foot, but the map has no vehicle pads, so nobody can get a 'Mech. Units and Props tab: "
                                "\"Place vehicle pad\" and click near each team's spawn points (real maps have 2-4 per team).", 0});
            // Free-for-all types read their own StartVehicles<GameType> slot (players.cpp): one that starts on foot with fewer
            // pads than players leaves most of them fighting on foot all match (b2_allmodes_fresh LMS: 6 rides for 11 pilots).
            if (pad) {
                int npads = 0;
                for (size_t i = 0; i < s.world.objs.size(); i++) {
                    if (s.world.objs[i].cls != K_SPAWNER) continue;
                    for (int k : t.kids[i]) {
                        uint32_t c = s.world.objs[size_t(k)].cls;
                        if (c == K_MECH || c == K_TANK || c == K_FLYER || c == K_VEHICLE) { npads++; break; }
                    }
                }
                int ri = find_class(s.world, K_ROOT);
                for (auto& ty : s.types) {
                    const GameTypeDef* g = find_game_type(ty);
                    if (!g || g->team || ri < 0 || npads >= 12) continue;
                    uint32_t key = key_of("StartVehicles" + ty);
                    for (auto& p : s.world.objs[size_t(ri)].props)
                        if (p.key == key && p.data.size() == 4 && (p.as_int() == 123999 || p.as_int() == 123726))
                            v.push_back({1, ty + ": players start on foot and the map has " + std::to_string(npads) + " vehicle pads for up to 12 players, "
                                            "so most of them fight on foot. Map tab: \"Players start as\" Battle Armor, or place more vehicle pads.", 0});
                }
            }
            // Per team (bots F33/F34): a pad near each team's spawns, and a helipad by a team that has VTOLs. The bots
            // take the nearest pad, so a team without one walks across the map (or stays on foot); a damaged VTOL
            // flies to the nearest helipad (an MABuilding with an MACollectionArea), the enemy's if its own has none.
            static const uint32_t K_BLD = key_of("MABuilding"), K_COLL = key_of("MACollectionArea");
            auto pos_up = [&](int i, Vec3& p) {
                for (int d = 0; i >= 0 && d < 8; d++, i = t.parent_of[size_t(i)])
                    if (get_pos(s.world.objs[size_t(i)], p)) return true;
                return false;
            };
            Vec3 home[3] = {};
            int nhome[3] = {0, 0, 0};
            for (auto& o : s.world.objs) {
                Vec3 p;
                int ti = o.i32("TeamIndex");
                if (o.cls != K_LEVELSTART || ti < 1 || ti > 2 || !get_pos(o, p)) continue;
                home[ti].x += p.x; home[ti].z += p.z; nhome[ti]++;
            }
            bool any_team_type = false;
            for (auto& ty : s.types) if (const GameTypeDef* g = find_game_type(ty); g && g->team) any_team_type = true;
            if (any_team_type && pad && nhome[1] && nhome[2]) {
                int near_pad[3] = {0, 0, 0}, vtol[3] = {0, 0, 0}, heli[3] = {0, 0, 0};
                for (int k = 1; k <= 2; k++) { home[k].x /= float(nhome[k]); home[k].z /= float(nhome[k]); }
                auto near_team = [&](const Vec3& p, float r) {
                    float d1 = std::hypot(p.x - home[1].x, p.z - home[1].z), d2 = std::hypot(p.x - home[2].x, p.z - home[2].z);
                    return d1 <= r && d1 <= d2 ? 1 : d2 <= r ? 2 : 0;
                };
                for (size_t i = 0; i < s.world.objs.size(); i++) {
                    const WObj& o = s.world.objs[i];
                    Vec3 p;
                    if (o.cls == K_SPAWNER && pos_up(int(i), p)) {
                        bool unit = false, flyer = false;
                        for (int k : t.kids[i]) {
                            uint32_t c = s.world.objs[size_t(k)].cls;
                            unit |= c == K_MECH || c == K_TANK || c == K_FLYER || c == K_VEHICLE;
                            flyer |= c == K_FLYER;
                        }
                        if (int tm = near_team(p, 200.f); tm && unit) { near_pad[tm]++; vtol[tm] += flyer; }
                    } else if (o.cls == K_BLD && pos_up(int(i), p)) {
                        bool coll = false;
                        for (int k : t.kids[i]) coll |= s.world.objs[size_t(k)].cls == K_COLL;
                        if (int tm = near_team(p, 300.f); tm && coll) heli[tm]++;
                    }
                }
                for (int k = 1; k <= 2; k++) {
                    const char* team = k == 1 ? "Red" : "Blue";
                    if (on_foot && !near_pad[k])
                        v.push_back({1, std::string(team) + " has no vehicle pad within 200 m of its spawn points, so its players start on foot far from "
                                        "any 'Mech. Units and Props tab: \"Place vehicle pad\" and click near " + team + "'s spawns.", 0});
                    if (vtol[k] && !heli[k])
                        v.push_back({1, std::string(team) + " has VTOLs but no helipad within 300 m of its spawn points: a damaged VTOL flies to the "
                                        "other team's pad to repair. Units and Props tab: search \"helipad\" and place one near " + team + "'s spawns.", 0});
                }
            }
        }
    } else {
        int starts = 0;
        for (auto& o : s.world.objs) if (o.cls == K_LEVELSTART) starts++;
        if (!starts) v.push_back({2, "The mission has no player start. Units and Props tab, Player start: press \"Move player start\" and click the map.", 0});
        if (starts > 1) v.push_back({1, "There is more than one player start; the game uses the first one.", 0});
        // A unit asleep at the start (AI off) that nothing wakes stands still and does not fire back, even when shot (playtest
        // P0 10-26, Hill Ambush). The Base level's own units, set members (retail wakes sets: MAActionAI AISet) and pad
        // units inside spawners are left alone; an order on the unit adds its wake-up action (set_order_trigger).
        {
            static const uint32_t K_ACT_AI = key_of("MAActionAI"), K_SPAWNER = key_of("MASpawner");
            static const uint32_t units[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"), key_of("MATurret"), key_of("MAPerson")};
            std::unordered_set<uint32_t> woken;
            std::unordered_map<uint32_t, uint32_t> retail;  // uid -> template: a new unit may reuse a retail uid
            for (auto& o : s.world.objs) if (o.cls == K_ACT_AI && o.i32("EnableAI")) woken.insert(o.u32("ActionEntity"));
            if (const MapInfo* bm = base_world_for(gd_, s.base, s.multiplayer))
                if (const Wdf* bw = retail_world(bm->name)) for (auto& o : bw->objs) retail[o.uid()] = o.template_id();
            for (auto& o : s.world.objs) {
                if (std::find(std::begin(units), std::end(units), o.cls) == std::end(units) || !o.find("m_bAIEnabled") || o.i32("m_bAIEnabled")) continue;
                if (o.i32("Nation") == 0 || o.i32("StartAbandoned") || o.find("SetMember0") || woken.count(o.uid())) continue;
                if (auto it = retail.find(o.uid()); it != retail.end() && it->second == o.template_id()) continue;
                if (const WObj* p = object_by_uid(s.world, o.parent()); p && p->cls == K_SPAWNER) continue;
                v.push_back({1, "\"" + o.description() + "\" starts asleep (AI off) and no trigger wakes it: it stands still and does not fire back, "
                                "even when shot. Select it and tick \"AI starts on\" in Properties, or wake it from a trigger (\"Enemies attack when "
                                "the player comes close\" also wakes them when hit).", o.uid()});
            }
        }
        static const uint32_t K_VARSET = key_of("MAActionMissionVariableSet"), K_GAMECOND = key_of("MAActionGameCondition"),
                              K_TRIG_RAD = key_of("MATriggerRadius"), K_TRIG_DESTR = key_of("MATriggerDestroyed"), K_REFERENCE = key_of("MoeEntityReference");
        bool win = false;
        for (auto& o : s.world.objs) if (o.cls == K_GAMECOND && o.i32("GameCondition") == 2) win = true;
        if (!win)
            v.push_back({1, "Nothing wins the mission, so it can only end by losing. Objectives & Triggers tab: add a trigger with the action \"Win the mission\".", 0});
        // A "player is in a circle" trigger around the start fires the moment the mission begins.
        Vec3 start{};
        bool have_start = false;
        for (auto& o : s.world.objs) if (o.cls == K_LEVELSTART && get_pos(o, start)) { have_start = true; break; }
        if (have_start)
            for (auto& o : s.world.objs) {
                if (o.cls != K_TRIG_RAD || !o.i32("TriggerEnabled", 1) || !o.i32("InRadiusCheck", 1) || o.i32("PlayerEntitesOnly") != 1) continue;
                bool has_action = false;
                for (auto& c : s.world.objs) has_action |= c.parent() == o.uid() && c.cls != K_REFERENCE;
                Vec3 p;
                if (!has_action || !get_pos(o, p)) continue;
                float r = o.f32("EncounterRadius", 30), dx = start.x - p.x, dz = start.z - p.z;
                if (dx * dx + dz * dz < r * r)
                    v.push_back({1, "Trigger \"" + o.description() + "\" waits for the player to enter a circle, but the player starts inside it, so it "
                                    "fires the moment the mission begins. Move the circle or make it smaller.", o.uid()});
            }
        // "Units are destroyed" that watches nothing (no units, no unit set) counts as already met: its actions run the
        // moment the mission starts (seen: a Win trigger won at once). "Include player units" alone does not help: a Lose
        // trigger with only that ticked lost at load, an endless restart loop (dogfood D11, in game).
        for (auto& tr : s.world.objs) {
            if (tr.cls != K_TRIG_DESTR || !tr.i32("TriggerEnabled", 1) || tr.u32("DestroySet")) continue;
            bool watches = false, acts = false;
            for (auto& c : s.world.objs) {
                if (c.parent() != tr.uid()) continue;
                if (c.cls == K_REFERENCE) watches = true;
                else acts = true;
            }
            if (acts && !watches)
                v.push_back({2, "Trigger \"" + tr.description() + "\" waits for units to be destroyed but watches none" +
                                    std::string(tr.i32("IncludePlayers") ? " (\"Include player units\" alone does not count)" : "") +
                                    ", so it fires the moment the mission starts. Objectives & Triggers tab: select it, click the units on the map, then press \"Add selected\".",
                             tr.uid()});
        }
        // A logic trigger ("all of" / "any of" other triggers) with no inputs counts as already met: its actions run the moment
        // the mission starts (e.g. "Win the mission when every objective is done" left over with no objectives = instant win).
        static const uint32_t K_TRIG_LOGIC = key_of("MATriggerLogic");
        for (auto& tr : s.world.objs) {
            if (tr.cls != K_TRIG_LOGIC || !tr.i32("TriggerEnabled", 1)) continue;
            bool inputs = false, acts = false;
            for (auto& c : s.world.objs) {
                if (c.parent() != tr.uid()) continue;
                if (c.cls == K_REFERENCE) inputs = true;
                else acts = true;
            }
            if (acts && !inputs)
                v.push_back({2, "Trigger \"" + tr.description() + "\" waits for other triggers but has none to wait for, so it fires the moment the "
                                "mission starts. Give it triggers to wait for, or switch it off (Enabled) or delete it.", tr.uid()});
        }
        // "Units are destroyed" on an empty 'Mech fires at once (it counts as lost): the mission
        // then wins or loses the moment it starts. Only with "Stealing a unit counts too" (AllowJacking, the editor's
        // default): retail Mission_03's 'TD - BlewUpTankMoron' watches an empty Rommel with it off and plays fine.
        for (auto& tr : s.world.objs) {
            if (tr.cls != K_TRIG_DESTR || !tr.i32("AllowJacking", 1)) continue;
            for (auto& r : s.world.objs) {
                if (r.cls != K_REFERENCE || r.parent() != tr.uid()) continue;
                int ui = t.find(r.u32("ReferenceUID"));
                if (ui < 0) continue;
                const WObj& u = s.world.objs[size_t(ui)];
                if (u.find("StartAbandoned") && u.i32("StartAbandoned"))
                    v.push_back({2, "Trigger \"" + tr.description() + "\" watches \"" + u.description() + "\", which is an empty 'Mech or vehicle (no pilot). "
                                    "Empty units count as lost the moment the mission starts, so the trigger fires at once. Select it and set "
                                    "it to Enemy on the Units and Props tab.", u.uid()});
            }
        }
        // Objectives: each needs a trigger that completes it.
        std::set<int> completed;
        for (auto& o : s.world.objs) if (o.cls == K_VARSET && o.i32("VariableState") != 0) completed.insert(o.i32("MissionVariableSet"));
        for (int k = 0; k < int(s.objectives.size()); k++)
            if (ObjectiveSlot sl = objective_slot(s, k); sl.game_text || sl.hidden) continue;  // the level's own slot, as shipped
            else if (!completed.count(objective_var(s, k)) && objective_losers(s.world, k).empty())  // "keep these alive" objectives only lose
                v.push_back({1, "Objective \"" + s.objectives[size_t(k)] + "\" is never completed" +
                                    // The game shows objectives one at a time, in order (D12): a stuck one hides the rest.
                                    (k + 1 < int(s.objectives.size()) ? std::string(", so the objectives after it never appear") : std::string()) +
                                    ". Objectives & Triggers tab: add the action \"Complete objective\" to the trigger that should complete it.", 0});
        // "Keep these alive" on enemy units: the palette places for Enemy by default, so a convoy to protect
        // can end up on the enemy side, where the player and allies shoot it and the mission is lost.
        for (int k = 0; k < int(s.objectives.size()); k++) {
            // Only "keep alive" ones: lost through a watched unit and completed by nothing (a "Destroy these" shares the targets).
            if (ObjectiveSlot sl = objective_slot(s, k); sl.game_text || sl.hidden || completed.count(objective_var(s, k)) ||
                                                         objective_losers(s.world, k).empty())
                continue;
            for (uint32_t t : objective_targets(s.world, k))
                if (const WObj* u = object_by_uid(s.world, t); u && u->find("Nation") && u->i32("Nation") == 5) {
                    v.push_back({1, "Objective \"" + s.objectives[size_t(k)] + "\" must keep \"" + u->description() + "\" alive, but it is on the enemy side, "
                                    "so the player and allies will shoot it. Select it and set Side to Player's side in Properties.", u->uid()});
                    break;  // one line per objective
                }
        }
        // A "Complete objective" action with no objective picked (its objective was deleted) does nothing.
        if (!s.multiplayer)
            for (auto& o : s.world.objs)
                if (o.cls == K_VARSET && o.i32("MissionVariableSet", -1) < 0) {
                    const WObj* tr = object_by_uid(s.world, o.parent());  // the trigger that holds the action
                    v.push_back({1, "Trigger \"" + (tr ? tr->description() : std::string("?")) + "\" has a \"Complete objective\" action with no objective picked,"
                                    " so that action does nothing (the trigger's other actions still work). Objectives & Triggers tab: pick the objective in that action, or remove the action.", tr ? tr->uid() : o.uid()});
                }
    }
    {
        static const uint32_t K_SOUND = key_of("MAActionSound"), K_BROADCAST = key_of("MAActionBroadcast"),
                              K_CINE_ACT = key_of("MAActionActivateCinema");
        auto owner = [&](const WObj& o) {
            int i = t.find(o.parent());
            return i >= 0 && !s.world.objs[size_t(i)].description().empty() ? s.world.objs[size_t(i)].description() : o.description();
        };
        for (auto& o : s.world.objs) {
            if (o.cls == K_SOUND) {
                uint32_t c = o.u32("CueEvent");
                if (c >= kAudioBase && c < kAudioBase + 100000u && !s.audio.count(int(c - kAudioBase)))
                    v.push_back({1, "Trigger \"" + owner(o) + "\" plays a sound clip that is not imported. Import it on the Objectives & Triggers tab, "
                                    "then pick it in the trigger.", o.uid()});
            } else if (o.cls == K_BROADCAST) {
                uint32_t id = o.u32("BodyTextID");
                auto it = id >= kTextBase ? s.strings.find(int(id - kTextBase)) : s.strings.end();
                if (it != s.strings.end() && trim(it->second).empty())
                    v.push_back({1, "Trigger \"" + owner(o) + "\" shows a message with no text. Type the text in its \"Show a message\" action.", o.uid()});
            } else if (o.cls == K_CINE_ACT && !o.u32("cinemaID")) {
                // No retail level has one (all 94 retail uses pick a cinema); in the game it plays nothing.
                v.push_back({1, "Trigger \"" + owner(o) + "\" plays a cutscene but none is picked. Pick one in its \"Play a camera sequence\" "
                                "action, or make one on the Cameras & Cutscenes tab.", o.uid()});
            }
        }
        // Links of triggers, actions and orders to objects that were deleted.
        static const char* kLinks[] = {"ActionEntity", "MarkerUID", "cinemaID", "ReferenceUID", "TriggerID", "DefendUID", "AttackUID", "FollowUID"};
        std::set<std::string> seen;
        for (auto& o : s.world.objs) {
            std::string cn = gd_.names().label(o.cls);
            if (cn.compare(0, 8, "MAAction") != 0 && cn.compare(0, 9, "MATrigger") != 0 && cn != "MoeEntityReference" && cn != "MAEvent" &&
                cn != "MADefend" && cn != "MAAttack" && cn != "MAFollow")
                continue;
            if (cn == "MAAttack" && o.u32("AttackPlayer") == 2) continue;
            for (const char* k : kLinks) {
                uint32_t u = o.find(k) ? o.u32(k) : 0;
                if (u && t.find(u) < 0 && seen.insert(std::to_string(o.uid()) + k).second)
                    v.push_back({1, "\"" + (o.description().empty() ? cn : o.description()) + "\" (" + owner(o) + ") points at an object that is not on this map (deleted, or already missing in "
                                    "the real level it was copied from). That part does nothing in the game until you pick an object again.", o.uid()});
            }
        }
    }
    for (size_t i = 0; i < s.world.objs.size(); i++) {
        const WObj& o = s.world.objs[i];
        Vec3 p;
        if (!get_pos(o, p) || o.cls == K_SKY || o.cls == K_WATER) continue;
        // Only placed objects (attached children such as cargo use their own frames).
        int par = t.parent_of[i];
        uint32_t pc = par >= 0 ? s.world.objs[size_t(par)].cls : 0;
        if (pc != K_GROUP && pc != K_MTG && pc != K_TERRAIN && pc != K_ROOT) continue;
        if (!s.terrain.inside(p.x, p.z)) {
            v.push_back({1, "\"" + o.description() + "\" is outside the map. Click here to select it, then delete it or drag it back in.", o.uid()});
            if (v.size() > 200) break;
        }
    }
    // Weather, fog and trigger setups the game ignores or mishandles (wiki: weather-strength, unknown-precip-id,
    // lightning-is-lightflash, fog loader, trigger vehicle filter).
    {
        int ei = find_class(s.world, K_ENV);
        if (ei >= 0) {
            const WObj& env = s.world.objs[size_t(ei)];
            if (env.i32("EnableRain") != 0 && env.u32("PrecipEffectID") == 0)
                v.push_back({1, "Rain/snow is switched on but no weather effect is chosen, so nothing falls. Environment tab > Weather: pick Rain, Snow or Dust.", 0});
            if (env.i32("LightFlash") == 0 && env.i32("EnableLightning") != 0)
                v.push_back({1, "Lightning is ticked in the file but the game only flashes when \"Flash lights\" is on. Untick and tick Lightning on the Environment tab to fix it.", 0});
            if (env.i32("FogMode") != 0 && env.find("FogStart") && env.find("FogEnd") && env.f32("FogEnd") <= env.f32("FogStart"))
                v.push_back({1, "Fog ends before (or where) it starts, so the fog is a hard wall. Environment tab > Fog: make the fog end distance larger than the start.", 0});
        }
        std::set<std::string> told;
        int placed_turrets = 0;
        for (auto& o : s.world.objs) placed_turrets += o.cls == key_of("MATurret");
        for (auto& o : s.world.objs) {
            // Circles and boxes share the unit-type filter; the game matches each unit by its exact class.
            if ((o.cls != key_of("MATriggerRadius") && o.cls != key_of("MATriggerCuboidalVolume")) || !o.find("CheckVehicleType")) continue;
            bool any = false;
            for (const char* f : {"Mech", "Tank", "Turret", "VTOL", "Avatar", "BattleArmor"}) any |= o.find(f) && o.i32(f) != 0;
            bool filter = o.i32("CheckVehicleType") != 0;
            std::string d = o.description().empty() ? "A trigger" : "Trigger \"" + o.description() + "\"";
            if (any && !filter && told.insert(d + "a").second)
                v.push_back({1, d + " has vehicle-type flags (Mech, Tank...) set, but they only count when \"Filter by vehicle type\" is on; right now they are ignored.", o.uid()});
            else if (filter && !any && told.insert(d + "b").second)
                v.push_back({1, d + " filters by vehicle type but no type is ticked, so nothing can set it off.", o.uid()});
            else if (filter && o.find("Turret") && o.i32("Turret") != 0 && told.insert(d + "t").second)
                v.push_back({1, d + " has \"Test turrets\" ticked, but that only catches tank-type units that cannot move, not the Turret objects from the palette" +
                                    (placed_turrets ? " (this map has " + std::to_string(placed_turrets) + ")" : std::string()) +
                                    ". To react to those, watch them with a \"destroyed\" trigger or an Entity set instead.", o.uid()});
        }
    }
    std::error_code ec;
    uint64_t base_bytes = fs::file_size(gd_.game_files() / widen(s.base), ec);
    if (ec) v.push_back({2, "The level this map is built on (" + s.base + ") is missing from the game's game_files folder. Reinstall the game files.", 0});
    if (!ec) graphics_check(s, v);
    if (v.empty()) v.push_back({0, "No problems found. Save and press Test to play it.", 0});
    (void)base_bytes;
    return v;
}

// ----------------------------------------------------------------------------- save / load

static std::string ini_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '\r') continue;
        else o += c;
    }
    return o;
}
static std::string ini_unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; i++; }
        else o += s[i];
    }
    return o;
}

static Image resample(const Image& src, int w, int h) {
    if (src.w == w && src.h == h) return src;
    Image d;
    d.w = w; d.h = h;
    d.px.resize(size_t(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) d.at(x, y) = src.at(std::min(src.w - 1, x * src.w / w), std::min(src.h - 1, y * src.h / h));
    return d;
}

static Bytes encode_texture(const Image& img, const Mgic& style, Picture layout) {
    Image top = resample(img, int(layout.width), int(layout.height));
    Bytes bits = encode_levels(layout, top);
    return build_picture(style, layout.flags, layout.width, layout.height, layout.mips, bits, layout.row_align);
}

static Bytes make_preview(const Scenario& s) {
    const int N = 256;
    Image img;
    img.w = img.h = N;
    img.px.resize(size_t(N) * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            uint32_t c = 0xFF406040u;
            if (!s.colormap.px.empty()) c = s.colormap.at(x * s.colormap.w / N, y * s.colormap.h / N);
            float l = 1.f;
            if (!s.lightmap.px.empty()) {
                uint32_t lc = s.lightmap.at(x * s.lightmap.w / N, y * s.lightmap.h / N);
                l = (float(lc & 255) / 255.f) * (0.55f + 0.45f * float(lc >> 24) / 255.f) * 1.4f;
            }
            uint32_t o = 0xFF000000u;
            for (int k = 0; k < 3; k++) o |= uint32_t(std::clamp(int(float((c >> (8 * k)) & 255) * l), 0, 255)) << (8 * k);
            img.at(x, y) = o;
        }
    Picture layout;
    layout.flags = 0x80000000u | 0x10 | PF_ARGB8888;
    layout.width = layout.height = N;
    layout.mips = 1;
    Mgic empty;
    return build_picture(empty, layout.flags, N, N, 1, encode_levels(layout, img), 1);
}

bool ScenarioIO::build(Scenario& s, Bytes& out, SaveReport* rep, std::string* err, bool ready) {
    sync_reach_markers(s);  // a 'Reach a place' circle moved since: its radar marker goes with it
    SaveReport local;
    SaveReport& R = rep ? *rep : local;
    R = SaveReport();
    if (!s.id) s.id = random_id();
    const std::string id = s.id_hex(), scn = "Scn" + id;
    Wdf w = s.world;
    if (!s.terrain.store_to_world(w, err)) return false;
    store_typemap(s.typemap, w);
    MgfWriter mgf;
    // Terrain textures.
    int ti = find_class(w, K_TERRAIN), ei = find_class(w, K_ENV);
    // P4 ground layers: the file for the PC runtime, and the colour-map alpha a runtime without
    // them shows (the pair of retail textures, mixed like the weights).
    const bool layers = s.ground.enabled && s.ground.count() >= 2 && s.ground.weights;
    if (layers && !s.colormap.px.empty()) { s.ground.map_w = s.colormap.w; s.ground.map_h = s.colormap.h; }  // the runtime matches T0 by this size
    if (layers) mgf.add(kGroundLayersFile, encode_ground_layers(s.ground));
    if ((s.colormap_custom || layers) && !s.colormap.px.empty() && s.colormap_layout.width) {
        std::string p = "Terrain/" + scn + "_ColorMap.tif";
        Image cm = s.colormap;
        if (layers) s.ground.bake_colormap_alpha(cm);
        mgf.add(p, encode_texture(cm, s.colormap_style, s.colormap_layout));
        if (ti >= 0) w.objs[size_t(ti)].set_str("TerrainBaseMap", "\\Terrain\\" + scn + "_ColorMap.tif");
    }
    // Ground textures picked from another level (Terrain tab): the game reads MATerrain's paths
    // from the mounted archives, so a texture the Base level and common.mgf lack goes into the
    // scenario's own archive under the same path.
    if (ti >= 0) {
        MgfReader* base = gd_.archive(s.base);
        MgfReader* common = gd_.archive("common.mgf");
        static const char* keys[4] = {"TerrainDetailMap", "TerrainDetailMap 1", "TerrainMicroDetailMap0", "TerrainMicroDetailMap1"};
        std::set<std::string> added;
        for (const char* k : keys) {
            std::string path = norm_path(w.objs[size_t(ti)].str(k));
            if (path.empty() || !added.insert(path).second) continue;
            if ((base && base->find(path) >= 0) || (common && common->find(path) >= 0)) continue;
            Bytes tb;
            std::string stored;
            for (auto& an : gd_.level_archives()) {
                MgfReader* a = gd_.archive(an);
                int fi = a ? a->find(path) : -1;
                if (fi >= 0 && a->read_file(fi, tb)) { stored = a->files()[size_t(fi)].path; break; }
            }
            if (tb.empty()) { R.warnings.push_back("Ground texture " + path + " is not on the disc; the game will show no texture there."); continue; }
            mgf.add(stored, tb);
        }
    }
    if (s.lightmap_custom && !s.lightmap.px.empty() && s.lightmap_layout.width) {
        std::string p = "Terrain/" + scn + "_Lightmap.tif";
        mgf.add(p, encode_texture(s.lightmap, s.lightmap_style, s.lightmap_layout));
        if (ei >= 0) w.objs[size_t(ei)].set_str("LightMapName", "\\Terrain\\" + scn + "_Lightmap.tif");
    }
    // Pathfinding (campaign AI) and its MAPathDB. A world that keeps its retail terrain and names
    // graphs that the Base archive ships (copies of retail missions) keeps those graphs.
    bool keep_retail_nav = false;
    {
        int pi = find_class(w, K_PATHDB);
        MgfReader* base = gd_.archive(s.base);
        std::string mech = pi >= 0 ? w.objs[size_t(pi)].str("Mech Map") : "";
        if (!mech.empty() && base && base->find(mech) >= 0 && !istarts_with(norm_path(mech), "pathfinding/scn")) {
            Wdf bw;
            Terrain bt;
            const MapInfo* bm = base_world_for(gd_, s.base, s.multiplayer);
            if (bm && gd_.load_world(*bm, bw) && bt.load_from_world(bw) && bt.w == s.terrain.w && bt.h == s.terrain.h &&
                bt.min_h == s.terrain.min_h && bt.max_h == s.terrain.max_h && bt.samples() == s.terrain.samples())
                keep_retail_nav = true;
        }
    }
    if (keep_retail_nav) R.nav_nodes = -1;
    else {
        Scenario tmp_view;  // nav uses the world being saved
        NavGraph g = build_nav(s);
        R.nav_nodes = int(g.nodes.size());
        std::string mech = g.serialize(), traffic = NavGraph().serialize();
        mgf.add("Pathfinding/" + scn + "_Mech.map", Bytes(mech.begin(), mech.end()));
        mgf.add("Pathfinding/" + scn + "_Traffic.map", Bytes(traffic.begin(), traffic.end()));
        int pi = find_class(w, K_PATHDB);
        if (pi < 0) {
            // Prototype from a retail world that has one.
            for (auto& m : gd_.maps()) {
                Wdf mw;
                if (!m.multiplayer || !gd_.load_world(m, mw)) continue;
                int k = find_class(mw, K_PATHDB);
                if (k < 0) continue;
                WObj db = mw.objs[size_t(k)];
                db.set_u32("UID", next_uid(w));
                db.set_u32("child", 0);
                wedit::append_child(w, root_uid(w), std::move(db));
                pi = find_class(w, K_PATHDB);
                break;
            }
        }
        if (pi >= 0) {
            w.objs[size_t(pi)].set_str("Mech Map", "\\Pathfinding\\" + scn + "_Mech.map");
            w.objs[size_t(pi)].set_str("Road Map", "\\Pathfinding\\" + scn + "_Traffic.map");
        } else {
            R.warnings.push_back("No MAPathDB prototype found; the campaign AI will have no path graph.");
        }
    }
    // Briefing and objective text (and, for a caller that still fills Scenario::triggers, that list compiled
    // into the copy); inline text becomes extra [Strings] entries.
    std::map<int, std::string> strings = s.strings;
    {
        std::map<int, std::string> extra;
        compile_triggers(s, w, &R.warnings, &extra);
        for (auto& [n, t] : extra) strings[n] = t;
    }
    // Templates.
    auto tpl = cat_.closure_for_world(w, s.base_level(), s.multiplayer);
    {
        std::vector<uint32_t> extra;  // the precipitation template (PrecipEffectID is no learned reference key)
        weather::extra_template_seeds(w, extra);
        if (!extra.empty()) for (uint32_t u : cat_.closure(std::unordered_set<uint32_t>(extra.begin(), extra.end()))) tpl.insert(u);
    }
    {
        Wdf tw = cat_.culled(tpl);
        // Rain ships its own particle again (E1, 2026-10-26): the magenta quads came from the renamed particle, which now
        // keeps the snow effect's name.
        if (int ei = find_class(w, K_ENV); ei >= 0 && weather::precip(w.objs[size_t(ei)]) == weather::Precip::Rain) {
            std::string werr;
            if (!weather::add_rain(gd_, mgf, weather::rain_level(w.objs[size_t(ei)]), &werr)) R.warnings.push_back(werr);
        }
        mgf.add("templates.wdf", tw.serialize());
    }
    R.templates = tpl.size();
    // World (model paths pointed at the files [Assets] ships).
    resolve_model_paths(s, w, fresh_templates(s, w), false);
    R.objects = w.objs.size();
    // Assets outside Base.
    s.world.objs.swap(w.objs);  // asset_closure reads s.world
    auto assets = asset_closure(s, tpl, &R.asset_bytes);
    s.world.objs.swap(w.objs);
    add_custom_water(s, w, mgf, &R.warnings);
    const std::vector<CropSaved> crops = add_cropped(s, w, mgf, &R.warnings);
    // Glued pieces (editor app_glue.cpp): editor-only, kept in editor/project.json ("glue"), never in the game's world.
    std::vector<std::pair<uint32_t, std::string>> glue;
    for (auto& o : w.objs)
        if (o.find("EditorGlue")) {
            glue.push_back({o.uid(), o.str("EditorGlue")});
            o.erase(key_of("EditorGlue"));
        }
    // A file the scenario carries itself (the rain particle made from Env_Snow_01) must not also be listed under [Assets]:
    // the game then loads the retail copy over it and the edited rain never shows (E1, 2026-10-26).
    assets.erase(std::remove_if(assets.begin(), assets.end(), [&](const auto& a) { return mgf.has(a.second); }), assets.end());
    R.asset_files = assets.size();
    pick_detail_levels(s, w, assets, &R.warnings);
    {
        // The game cuts an [Assets] entry at 191 characters, which makes the file missing (a long imported-model path).
        int longest = 0, n_long = 0;
        for (auto& [arc, path] : assets) {
            int len = int(arc.size() + 1 + path.size());
            if (len > 190) { n_long++; longest = std::max(longest, len); }
        }
        if (n_long)
            R.warnings.push_back(std::to_string(n_long) + " asset path(s) are longer than the game reads (" + std::to_string(longest) +
                                 " characters, the limit is 190), so those files will be missing in the game.");
    }
    weather::to_game(w);
    mgf.add("world.wdf", w.serialize());
    std::error_code ec;
    R.base_bytes = fs::file_size(gd_.game_files() / widen(s.base), ec);
    {
        std::vector<ValidationItem> gv;
        graphics_check(s, gv, &R.graphics_mb);
        for (auto& x : gv) R.warnings.push_back(x.text);
    }
    if (R.objects > kMaxObjects) {
        R.warnings.push_back("More than 3,000 objects.");
        R.over_limit = true;
        R.limit_text = "It has " + std::to_string(R.objects) + " objects; the game cannot load more than 3,000.";
    }
    if (R.graphics_mb >= kGraphicsErrorMb) {
        R.over_limit = true;
        char gb[160];
        snprintf(gb, sizeof gb, "Its graphics need about %.0f MB; the game's limit is about %.0f MB.", R.graphics_mb, kGraphicsErrorMb);
        R.limit_text += (R.limit_text.empty() ? "" : " ") + std::string(gb);
    }
    // Audio.
    for (auto& [n, clip] : s.audio) mgf.add("Audio/" + std::to_string(n) + ".wav", clip.wav);
    // scenario.ini
    // The game shows "NULL" for an empty map description, and an empty name as a blank row.
    std::string shown_name = trim(s.name).empty() ? std::string("Custom scenario") : s.name;
    std::string shown_desc = trim(s.description).empty()
                                 ? (trim(s.author).empty() ? std::string("A custom scenario.") : "A custom scenario by " + s.author + ".")
                                 : s.description;
    std::string ini = "[Scenario]\r\nFormatVersion = 1\r\nSavedWith = " + std::to_string(kSaveRevision) + "\r\nEditor = " MA2SE_VERSION " (" MA2SE_GIT
                      ")\r\nReady = " + (ready && !R.over_limit ? "1" : "0") + "\r\nId = " + id + "\r\nName = " + ini_escape(shown_name) +
                      "\r\nDescription = " + ini_escape(shown_desc) + "\r\nAuthor = " + ini_escape(s.author) +
                      "\r\nType = " + (s.multiplayer ? "Multiplayer" : "Mission") + "\r\n";
    if (s.multiplayer) for (auto& g : s.types) ini += "GameType = " + g + "\r\n";
    ini += "Base = " + lower(s.base) + "\r\nSize = " + s.size_text() + "\r\n\r\n[Assets]\r\n";
    for (auto& [arc, path] : assets) ini += "File = " + arc + "|" + path + "\r\n";
    if (layers) ini += std::string("\r\n[Terrain]\r\nLayers = ") + kGroundLayersFile + "\r\n";
    ini += "\r\n[Strings]\r\n";
    for (auto& [n, t] : strings) ini += std::to_string(n) + " = " + ini_escape(t) + "\r\n";
    ini += "\r\n[Audio]\r\n";
    for (auto& [n, c] : s.audio) ini += std::to_string(n) + " = Audio/" + std::to_string(n) + ".wav\r\n";
    if (!s.multiplayer) {
        ini += "\r\n[Mission]\r\nBriefingTitle = " + ini_escape(s.briefing_title) + "\r\nBriefing = " + ini_escape(s.briefing) + "\r\n";
        for (auto& o : w.objs) if (o.cls == K_LEVELSTART) { ini += "PlayerStart = " + std::to_string(o.uid()) + "\r\n"; break; }
    }
    mgf.add("scenario.ini", Bytes(ini.begin(), ini.end()));
    // Editor project data.
    Json pj = Json::object();
    pj.set("editor", Json::string("MechAssault 2 Scenario Editor"));
    pj.set("version", Json::number(1));
    Json trig = Json::array();
    for (auto& t : s.triggers) {
        Json jt = Json::object();
        jt.set("name", Json::string(t.name));
        jt.set("enabled", Json::boolean(t.enabled));
        jt.set("loop", Json::boolean(t.loop));
        jt.set("any", Json::boolean(t.any));
        jt.set("compiled_uid", Json::number(t.compiled_uid));
        Json cs = Json::array(), es = Json::array();
        for (auto& c : t.conditions) {
            Json jc = Json::object();
            jc.set("kind", Json::string(c.kind));
            Json a = Json::object();
            for (auto& [k, v] : c.args) a.set(k, Json::string(v));
            jc.set("args", a);
            cs.push(jc);
        }
        for (auto& e : t.effects) {
            Json je = Json::object();
            je.set("kind", Json::string(e.kind));
            Json a = Json::object();
            for (auto& [k, v] : e.args) a.set(k, Json::string(v));
            je.set("args", a);
            es.push(je);
        }
        jt.set("conditions", cs);
        jt.set("effects", es);
        trig.push(jt);
    }
    pj.set("triggers", trig);
    Json objs = Json::array();
    for (auto& o : s.objectives) objs.push(Json::string(o));
    pj.set("objectives", objs);
    // Strings the user typed (the compiler's own ids 60000+ are regenerated on save).
    Json us = Json::object();
    for (auto& [n, t] : s.strings) us.set(std::to_string(n), Json::string(t));
    pj.set("strings", us);
    Json au = Json::object();
    for (auto& [n, c] : s.audio) au.set(std::to_string(n), Json::string(c.source));
    pj.set("audio_sources", au);
    // Rain is saved as snow (see above); this keeps it Rain in the editor.
    if (int ei = find_class(s.world, K_ENV); ei >= 0 && weather::precip(s.world.objs[size_t(ei)]) == weather::Precip::Rain) {
        pj.set("rain", Json::boolean(true));
        if (weather::rain_level(s.world.objs[size_t(ei)]) == weather::RainLevel::Light) pj.set("rain_level", Json::string("light"));
    }
    if (!crops.empty()) {
        Json cj = Json::object();
        for (auto& c : crops) cj.set(std::to_string(c.uid), Json::object().with("rect", Json::string(c.rect)).with("orig", Json::string(c.orig)));
        pj.set("crops", cj);
    }
    if (!glue.empty()) {
        Json gj = Json::object();
        for (auto& [u, v] : glue) gj.set(std::to_string(u), Json::string(v));
        pj.set("glue", gj);
    }
    std::string pjs = pj.dump(1);
    mgf.add("editor/project.json", Bytes(pjs.begin(), pjs.end()));
    mgf.add("preview.tif", make_preview(s));
    out = mgf.build();
    R.file_bytes = out.size();
    return true;
}

bool ScenarioIO::save(Scenario& s, const fs::path& file, SaveReport* rep, std::string* err, bool force_over_limit) {
    Bytes b;
    SaveReport local;
    SaveReport& R = rep ? *rep : local;
    // The game lists a scenario only when it is ready: no Must fix item left in Check (autosave builds with the default, ready).
    bool ready = true;
    for (auto& v : validate(s)) ready = ready && v.level != 2;
    if (!build(s, b, &R, err, ready)) return false;
    if (R.over_limit && !force_over_limit) {
        if (err) *err = "This scenario is over the game's limits and would crash it at load. " + R.limit_text;
        return false;
    }
    // The last good version stays as <file>.bak (the game lists only *.ma2map, so it never shows). A save that wrote a
    // map which will not reopen or plays wrong used to be the only copy.
    {
        std::error_code ec;
        if (fs::exists(file, ec)) {
            fs::path bak = file;
            bak += L".bak";
            fs::copy_file(file, bak, fs::copy_options::overwrite_existing, ec);
        }
    }
    if (!write_file(file, b, err)) return false;
    s.file = file;
    s.dirty = false;
    s.read_only = false;
    s.saved_with = kSaveRevision;
    return true;
}

bool ScenarioIO::peek(const fs::path& file, ScenarioHead& h) {
    MgfReader r;
    Bytes b;
    if (!r.open(file, nullptr) || !r.read_file("scenario.ini", b, nullptr)) return false;
    IniFile ini;
    ini.parse(std::string(b.begin(), b.end()), false);
    if (ini.get("Scenario", "FormatVersion") != "1") return false;
    h.id = uint32_t(strtoul(ini.get("Scenario", "Id").c_str(), nullptr, 16));
    h.name = ini_unescape(ini.get("Scenario", "Name"));
    std::string type = ini.get("Scenario", "Type");
    size_t c = type.find(" ;");
    if (c != std::string::npos) type.erase(c);
    h.multiplayer = !iequals(trim(type), "Mission");
    h.saved_with = atoi(ini.get("Scenario", "SavedWith", "1").c_str());
    return true;
}

bool ScenarioIO::load(const fs::path& file, Scenario& s, std::string* err, std::vector<std::string>* warnings) {
    MgfReader r;
    if (!r.open(file, err)) return false;
    r.set_check_crc(true);  // our own archives carry a checksum per file (MgfWriter)
    if (warnings) warnings->clear();
    Bytes b;
    if (!r.read_file("scenario.ini", b, err)) return false;
    IniFile ini;
    ini.parse(std::string(b.begin(), b.end()), false);
    if (ini.get("Scenario", "FormatVersion") != "1") { if (err) *err = "unsupported scenario FormatVersion"; return false; }
    s = Scenario();
    s.file = file;
    s.id = uint32_t(strtoul(ini.get("Scenario", "Id").c_str(), nullptr, 16));
    s.saved_with = atoi(ini.get("Scenario", "SavedWith", "1").c_str());  // a file from before the stamp: 1
    auto strip_comment = [](std::string v) {
        size_t c = v.find(" ;");
        if (c != std::string::npos) v.erase(c);
        return trim(v);
    };
    s.name = ini_unescape(ini.get("Scenario", "Name"));
    s.description = ini_unescape(ini.get("Scenario", "Description"));
    // Placeholders written for the game when the fields were empty (see build) load back as empty.
    if (s.description == "A custom scenario." || (s.description.rfind("A custom scenario by ", 0) == 0 && s.description.back() == '.'))
        s.description.clear();
    s.author = ini_unescape(ini.get("Scenario", "Author"));
    s.multiplayer = !iequals(strip_comment(ini.get("Scenario", "Type")), "Mission");
    for (auto& g : ini.get_all("Scenario", "GameType")) s.types.push_back(strip_comment(g));
    s.base = lower(strip_comment(ini.get("Scenario", "Base")));
    // Base is joined to the game_files folder (archives, sizes): a shared file must name a bare archive, not a path.
    if (s.base.empty() || s.base.find_first_of("/\\:") != std::string::npos) { if (err) *err = "bad Base in scenario.ini"; return false; }
    for (auto* e : ini.section("Strings")) s.strings[atoi(e->key.c_str())] = ini_unescape(e->value);
    s.briefing_title = ini_unescape(ini.get("Mission", "BriefingTitle"));
    s.briefing = ini_unescape(ini.get("Mission", "Briefing"));
    if (!r.read_file("world.wdf", b, err) || !s.world.parse(b, err)) return false;
    weather::from_game(s.world, r);
    // Water saved with a model of its own size draws the template's model in the editor again (the size is in the Description).
    for (auto& o : s.world.objs) {
        float cw, cl;
        if (o.cls != K_WATER || !water_custom_size(o, cw, cl) || o.str("Corporal3DObj").find("MA2SE_") == std::string::npos) continue;
        if (const TemplateInfo* t = cat_.find(o.template_id())) o.set_str("Corporal3DObj", t->model);
    }
    if (!s.terrain.load_from_world(s.world, err)) return false;
    // Our own terrain textures, when present.
    const std::string scn = "Scn" + s.id_hex();
    if (r.read_file("Terrain/" + scn + "_ColorMap.tif", b) && decode_file(b, s.colormap, s.colormap_style, s.colormap_layout))
        s.colormap_custom = true;
    if (r.read_file("Terrain/" + scn + "_Lightmap.tif", b) && decode_file(b, s.lightmap, s.lightmap_style, s.lightmap_layout))
        s.lightmap_custom = true;
    load_terrain_textures(s);
    load_typemap(s);
    std::string layers = strip_comment(ini.get("Terrain", "Layers"));
    if (!layers.empty()) {
        std::string lerr;
        if (!r.read_file(layers, b) || !decode_ground_layers(b, s.ground, &lerr)) s.ground = GroundLayers();  // the retail pair, like the game
    }
    for (auto* e : ini.section("Audio")) {
        AudioClip c;
        if (r.read_file(e->value, c.wav)) s.audio[atoi(e->key.c_str())] = std::move(c);
    }
    if (r.read_file("editor/project.json", b)) {
        Json pj;
        if (Json::parse(std::string(b.begin(), b.end()), pj)) {
            if (int ei = find_class(s.world, K_ENV); ei >= 0 && pj.flag("rain") && weather::precip(s.world.objs[size_t(ei)]) == weather::Precip::Snow)
                weather::set_precip(s.world.objs[size_t(ei)], weather::Precip::Rain);
            if (int ei = find_class(s.world, K_ENV); ei >= 0 && weather::precip(s.world.objs[size_t(ei)]) == weather::Precip::Rain &&
                pj.str("rain_level") == "light")
                weather::set_rain_level(s.world.objs[size_t(ei)], weather::RainLevel::Light);
            if (const Json* tr = pj.get("triggers"))
                for (auto& jt : tr->a) {
                    EdTrigger t;
                    t.name = jt.str("name", "Trigger");
                    t.enabled = jt.flag("enabled", true);
                    t.loop = jt.flag("loop");
                    t.any = jt.flag("any");
                    t.compiled_uid = uint32_t(jt.num("compiled_uid"));
                    if (const Json* cs = jt.get("conditions"))
                        for (auto& jc : cs->a) {
                            EdCondition c;
                            c.kind = jc.str("kind");
                            if (const Json* a = jc.get("args")) for (auto& [k, v] : a->o) c.args[k] = v.s;
                            t.conditions.push_back(std::move(c));
                        }
                    if (const Json* es = jt.get("effects"))
                        for (auto& je : es->a) {
                            EdEffect e;
                            e.kind = je.str("kind");
                            if (const Json* a = je.get("args")) for (auto& [k, v] : a->o) e.args[k] = v.s;
                            t.effects.push_back(std::move(e));
                        }
                    s.triggers.push_back(std::move(t));
                }
            // Cropped road pieces and foundations (add_cropped): the crop goes back on the object, and its original model /
            // .road path replaces the cut one the game reads, so the editor cuts it again from the source.
            if (const Json* cr = pj.get("crops")) {
                static const uint32_t K_ROAD = key_of("MARoad");
                for (auto& [k, v] : cr->o) {
                    crop::Rect r;
                    const uint32_t uid = uint32_t(strtoul(k.c_str(), nullptr, 10));
                    if (!uid || !crop::parse(v.str("rect"), r)) continue;
                    for (auto& o : s.world.objs) {
                        if (o.uid() != uid) continue;
                        crop::set(o, &r);
                        const std::string orig = v.str("orig");
                        if (!orig.empty()) o.set_str(o.cls == K_ROAD ? "RoadDefinition" : "Corporal3DObj", orig);
                        break;
                    }
                }
            }
            if (const Json* gl = pj.get("glue"))
                for (auto& [k, v] : gl->o) {
                    const uint32_t uid = uint32_t(strtoul(k.c_str(), nullptr, 10));
                    for (auto& o : s.world.objs)
                        if (o.uid() == uid) { o.set_str("EditorGlue", v.s); break; }
                }
            if (const Json* ob = pj.get("objectives")) for (auto& j : ob->a) s.objectives.push_back(j.s);
            if (const Json* us = pj.get("strings")) {
                s.strings.clear();
                for (auto& [k, v] : us->o) s.strings[atoi(k.c_str())] = v.s;
            }
            if (const Json* au = pj.get("audio_sources"))
                for (auto& [k, v] : au->o) { auto it = s.audio.find(atoi(k.c_str())); if (it != s.audio.end()) it->second.source = v.s; }
        }
    }
    // Files from before the Objectives & Triggers tab worked on the world itself: their trigger list is compiled once
    // into ordinary objects (the file is unchanged until it is saved again).
    if (!s.triggers.empty()) load_trigger_prototypes(gd_, nullptr);
    convert_legacy_triggers(s);
    // Repair: empty units placed by earlier editor builds lacked an owner controller and could not
    // be entered in the game. The repair marks the scenario changed so it is saved fixed.
    bool repaired = false;
    {
        static const uint32_t K_OC = key_of("MAOwnerController");
        static const uint32_t vehicles[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer")};
        WTree t;
        t.build(s.world);
        std::vector<uint32_t> fix;
        for (size_t i = 0; i < s.world.objs.size(); i++) {
            const WObj& o = s.world.objs[i];
            if (std::find(std::begin(vehicles), std::end(vehicles), o.cls) == std::end(vehicles)) continue;
            if (!o.find("StartAbandoned") || !o.i32("StartAbandoned") || o.i32("Nation") != 0) continue;
            int par = t.parent_of[i];
            if (par < 0 || s.world.objs[size_t(par)].cls != K_GROUP || s.world.objs[size_t(par)].description() != "Placed Objects") continue;
            bool has = false;
            for (int k : t.kids[i]) has |= s.world.objs[size_t(k)].cls == K_OC;
            if (!has || o.i32("AttackPilot")) fix.push_back(o.uid());
        }
        for (uint32_t u : fix) apply_unit_side(s, u, 0);
        repaired = !fix.empty();
    }
    s.dirty = repaired;
    if (warnings) *warnings = r.damaged();
    return true;
}

}  // namespace ma2
