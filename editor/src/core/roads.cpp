// Road pieces (MARoad). A road is a chain of prefab pieces, each naming a .road file in its
// RoadDefinition; there are no templates for them, so they never appeared in the template-driven
// object palette. Placing one clones a retail MARoad as the prototype and sets its three keys
// (RoadDefinition, RoadPosition, RoadRotation); saving ships the .road file and its model.
#include <algorithm>
#include <cmath>
#include <set>

#include "crop.h"
#include "ma2models.h"
#include "placement.h"
#include "scenario.h"

namespace ma2 {

using namespace world;

struct ScenarioIO::ArchiveSetHolder { ma2m::ArchiveSet set; };

static const uint32_t K_ROAD = key_of("MARoad"), K_ROADGROUP = key_of("MARoadGroup");

// A retail MARoad / MARoadGroup to copy the property set from: this world's own, else Mission_02's.
static const WObj* find_proto(ScenarioIO& io, const Scenario& s, uint32_t cls) {
    for (auto& o : s.world.objs) if (o.cls == cls) return &o;
    for (const char* m : {"Mission_02", "NewRiverCity", "Mission_16", "RushHour"}) {
        const Wdf* w = io.retail_world(m);
        int i = w ? find_class(*w, cls) : -1;
        if (i >= 0) return &w->objs[size_t(i)];
    }
    return nullptr;
}

float ScenarioIO::road_rest_height(const Scenario& s, const std::string& road_path, Vec3 at, float yaw) {
    // Local footprint: boundary point1/point2 (x -W..+W, z 0..-L); a 30 m straight when unreadable.
    float x0 = -13, x1 = 13, z0 = -30, z1 = 0;
    ensure_deps_archives(s);
    std::vector<uint8_t> b;
    if (deps_archives_->set.Read(ma2m::NormalizePath(road_path), b)) {
        float p[2][3];
        int seen = 0;
        for (auto& line : split(std::string(b.begin(), b.end()), '\n')) {
            std::string l = trim(line.substr(0, line.find(';')));
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = lower(trim(l.substr(0, eq))), v = trim(l.substr(eq + 1));
            int i = k == "point1" ? 0 : k == "point2" ? 1 : -1;
            if (i >= 0 && sscanf(v.c_str(), "%f, %f, %f", &p[i][0], &p[i][1], &p[i][2]) == 3) seen |= 1 << i;
        }
        if (seen == 3) {
            x0 = std::min(p[0][0], p[1][0]); x1 = std::max(p[0][0], p[1][0]);
            z0 = std::min(p[0][2], p[1][2]); z1 = std::max(p[0][2], p[1][2]);
        }
    }
    // The road rests kSheetLift above the highest ground under its footprint (placement.h); the stack of sheets over it is the
    // app's (App::reseat_sheets), this is the height on bare ground.
    place::Sheet sh;
    sh.pos = at;
    sh.yaw = yaw;
    sh.fp = {x0, x1, z0, z1};
    return place::ground_max(s.terrain, sh) + place::kSheetLift;
}

uint32_t ScenarioIO::place_road(Scenario& s, const std::string& road_path, Vec3 at, float yaw) {
    const WObj* rp = find_proto(*this, s, K_ROAD);
    const WObj* gp = find_proto(*this, s, K_ROADGROUP);
    if (!rp || !gp) return 0;
    WObj road = *rp;
    WObj group = *gp;
    road.erase(key_of(crop::kProp));  // the prototype can be a cropped piece of this world: a new piece is whole
    road.erase(key_of(place::kLayerProp));  // ...and a layered one: a new piece gets its own layer
    uint32_t next = next_uid(s.world);
    uint32_t placed = ensure_group(s.world, terrain_uid(s.world), "Placed Roads", next);
    // One road group for all the pieces placed here.
    WTree t;
    t.build(s.world);
    uint32_t rg = 0;
    int pi = t.find(placed);
    if (pi >= 0)
        for (int k : t.kids[size_t(pi)])
            if (s.world.objs[size_t(k)].cls == K_ROADGROUP) { rg = s.world.objs[size_t(k)].uid(); break; }
    if (!rg) {
        rg = next_uid(s.world);
        group.set_u32("UID", rg);
        group.set_u32("child", 0);
        wedit::append_child(s.world, placed, std::move(group));
    }
    uint32_t uid = next_uid(s.world);
    road.set_u32("UID", uid);
    road.set_u32("child", 0);
    // The game's own spelling: backslashes, as the prototype writes it.
    std::string def = road_path;
    for (char& c : def) if (c == '/') c = '\\';
    road.set_str("RoadDefinition", def);
    at.y = road_rest_height(s, def, at, yaw);
    set_pos(road, at);
    if (WProp* r = road.find("RoadRotation")) {
        if (r->data.size() >= 4) {
            Bytes d(r->data.size(), 0);
            memcpy(d.data(), &yaw, 4);
            r->data = d;
        }
    }
    wedit::append_child(s.world, rg, std::move(road));
    s.dirty = true;
    return uid;
}

void ScenarioIO::road_assets(const Scenario& s, std::vector<std::pair<std::string, std::string>>& out,
                             std::vector<AssetUse>* uses, uint64_t* bytes) {
    ensure_deps_archives(s);
    const ma2m::ArchiveSet& ar = deps_archives_->set;  // 0 = Base, 1 = common.mgf
    std::set<std::string> seen;
    for (auto& [a, p] : out) seen.insert(lower(a) + "|" + lower(p));
    std::set<std::string> done;
    for (auto& o : s.world.objs) {
        if (o.cls != K_ROAD) continue;
        std::string def = ma2m::NormalizePath(o.str("RoadDefinition"));
        if (def.empty() || !done.insert(lower(def)).second) continue;
        if (ar.ExistsIn(0, def) || ar.ExistsIn(1, def)) continue;  // Base or common.mgf already has the piece
        int a = ar.Find(def);
        if (a < 0) continue;
        // The holder first, so the exact files win; then Base and common for shared textures.
        ma2m::ArchiveSet mini;
        mini.Add(ar.ArchivePath(size_t(a)));
        mini.Add(ar.ArchivePath(0));
        mini.Add(ar.ArchivePath(1));
        std::string holder = lower(ar.ArchiveName(size_t(a)));
        auto add = [&](const std::string& arch, const std::string& path, bool graphics) {
            if (!seen.insert(lower(arch) + "|" + lower(path)).second) return;
            out.push_back({lower(arch), path});
            uint32_t sz = mini.FileSize(path);
            if (bytes) *bytes += sz;
            if (uses) uses->push_back({"Road piece " + def.substr(def.rfind('/') + 1), graphics, sz});
        };
        add(holder, ar.CanonicalPath(def), false);
        std::vector<uint8_t> b;
        std::string model;
        if (ar.Read(def, b)) {
            for (auto& line : split(std::string(b.begin(), b.end()), '\n')) {
                std::string l = trim(line.substr(0, line.find(';')));
                size_t eq = l.find('=');
                if (eq != std::string::npos && lower(trim(l.substr(0, eq))) == "model") model = ma2m::ResolveRelative(def, trim(l.substr(eq + 1)));
            }
        }
        if (model.empty()) continue;
        std::vector<ma2m::Dependency> deps;
        ma2m::CollectDependencies(mini, model, "", deps);
        for (auto& d : deps) {
            if (!d.found || iequals(d.archive, deps_base_) || iequals(d.archive, "common.mgf")) continue;
            std::string lp = lower(d.path);
            bool gfx = iends_with(lp, ".tif") || iends_with(lp, ".dds") || lp.find(" verts}") != std::string::npos || lp.find(" indices}") != std::string::npos;
            add(d.archive, d.path, gfx);
        }
    }
}

}  // namespace ma2
