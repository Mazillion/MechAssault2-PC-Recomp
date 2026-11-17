// Sky picker (MASky2). The catalog comes from the user's own archives: every .MGMODEL under
// Models/Environment/Sky in the level archives, and the MASky2 objects of every world file in
// common.mgf (MP maps, missions, cinemas, the lobby) for how each sky is set up.
#include "scenario.h"
#include "weather.h"

#include <algorithm>
#include <map>

namespace ma2 {

using namespace world;

// "\Models\Environment\Sky\Env_SkyDusk02\Env_SkyDusk02_MP.MGMODEL" -> "env_skydusk02".
static std::string sky_key(std::string_view model_path) {
    std::string p(model_path);
    size_t s = p.find_last_of("/\\");
    if (s != std::string::npos) p.erase(0, s + 1);
    size_t d = p.find_last_of('.');
    if (d != std::string::npos) p.erase(d);
    if (iends_with(p, "_mp")) p.resize(p.size() - 3);
    return lower(p);
}

static std::string sky_folder(std::string_view model_path) {
    std::string p(model_path);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    auto parts = split(p, '/');
    return parts.size() >= 5 ? std::string(parts[3]) : std::string();
}

// World-file spelling of an archive path: leading backslash, backslash separators.
static std::string world_path(std::string_view archive_path) {
    std::string p = "\\" + std::string(archive_path);
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
}

static int world_kind(std::string_view path) {  // 0 MP map, 1 mission, 2 other (cinemas, lobby, demo)
    if (istarts_with(path, "Multiplay/")) return 0;
    if (istarts_with(path, "Missions/Mission_")) return 1;
    return 2;
}

const std::vector<ScenarioIO::SkyInfo>& ScenarioIO::skies() {
    if (skies_ready_) return skies_;
    skies_ready_ = true;
    std::map<std::string, SkyInfo> by_key;
    std::map<std::string, std::vector<std::string>> mtbs;  // folder (lower) -> .mtb paths
    for (const std::string& an : gd_.level_archives()) {
        MgfReader* a = gd_.archive(an);
        if (!a) continue;
        for (auto& f : a->files()) {
            if (!istarts_with(f.path, "Models/Environment/Sky/")) continue;
            std::string folder = sky_folder(f.path);
            if (folder.empty()) continue;
            if (iends_with(f.path, ".mtb")) {
                auto& v = mtbs[lower(folder)];
                if (std::none_of(v.begin(), v.end(), [&](auto& x) { return iequals(x, f.path); })) v.push_back(f.path);
                continue;
            }
            if (!iends_with(f.path, ".mgmodel")) continue;
            SkyInfo& si = by_key[sky_key(f.path)];
            if (si.folder.empty()) {
                si.folder = folder;
                std::string base = f.path.substr(f.path.find_last_of('/') + 1);
                base.erase(base.find_last_of('.'));
                if (iends_with(base, "_mp")) base.resize(base.size() - 3);
                si.name = base;
            }
            if (std::none_of(si.models.begin(), si.models.end(), [&](auto& x) { return iequals(x, f.path); })) si.models.push_back(f.path);
            if (std::find(si.archives.begin(), si.archives.end(), an) == si.archives.end()) si.archives.push_back(an);
        }
    }
    for (auto& [k, si] : by_key) {
        auto it = mtbs.find(lower(si.folder));
        if (it != mtbs.end()) si.mtbs = it->second;
    }
    // How the retail worlds set each sky up.
    for (auto& f : gd_.common().files()) {
        if (!iends_with(f.path, ".wdf") || !(istarts_with(f.path, "Missions/") || istarts_with(f.path, "Multiplay/"))) continue;
        Bytes b;
        Wdf w;
        if (!gd_.common().read_file(f.path, b) || !w.parse(b)) continue;
        int ei = find_class(w, K_ENV);
        for (auto& o : w.objs) {
            if (o.cls != K_SKY) continue;
            std::string model = o.str("Corporal3DObj");
            auto it = by_key.find(sky_key(model));
            if (model.empty() || it == by_key.end()) continue;
            SkyUse u;
            u.world = f.path;
            u.kind = world_kind(f.path);
            u.model = model;
            u.mtb = o.str("CorporalMTB");
            u.scale = o.vec3("SkyTerrainSkyScale");
            u.offset = o.vec3("SkyTerrainSkyOffset");
            u.pos = o.vec3("CorporalPos");
            u.heading_deg = o.f32("CorporalHeading");
            if (ei >= 0 && w.objs[size_t(ei)].find("FogColor")) {
                u.fog_color = w.objs[size_t(ei)].u32("FogColor");
                u.has_fog = true;
            }
            if (ei >= 0 && w.objs[size_t(ei)].find("DirectionalLightColor")) {
                u.sun_color = w.objs[size_t(ei)].u32("DirectionalLightColor");
                u.has_sun = true;
            }
            it->second.uses.push_back(std::move(u));
        }
    }
    for (auto& [k, si] : by_key) {
        std::sort(si.uses.begin(), si.uses.end(), [](const SkyUse& a, const SkyUse& b) {
            return a.kind != b.kind ? a.kind < b.kind : lower(a.world) < lower(b.world);
        });
        skies_.push_back(std::move(si));
    }
    // Skies some level uses first, then alphabetical.
    std::sort(skies_.begin(), skies_.end(), [](const SkyInfo& a, const SkyInfo& b) {
        if (a.uses.empty() != b.uses.empty()) return b.uses.empty();
        return lower(a.folder + a.name) < lower(b.folder + b.name);
    });
    return skies_;
}

const ScenarioIO::SkyInfo* ScenarioIO::find_sky(std::string_view name) {
    for (auto& si : skies()) if (iequals(si.name, name)) return &si;
    const SkyInfo* by_folder = nullptr;
    int n = 0;
    for (auto& si : skies()) if (iequals(si.folder, name)) { by_folder = &si; n++; }
    return n == 1 ? by_folder : nullptr;
}

std::string ScenarioIO::sky_name(const Scenario& s) {
    int i = find_class(s.world, K_SKY);
    if (i < 0) return {};
    std::string key = sky_key(s.world.objs[size_t(i)].str("Corporal3DObj"));
    for (auto& si : skies()) if (lower(si.name) == key) return si.name;
    return {};
}

const Wdf* ScenarioIO::base_world(const Scenario& s) {
    const MapInfo* best = nullptr;
    for (auto& m : gd_.maps())
        if (iequals(m.resource, s.base) && (!best || (m.multiplayer == s.multiplayer && best->multiplayer != s.multiplayer))) best = &m;
    return best ? retail_world(best->name) : nullptr;
}

std::string ScenarioIO::base_sky_name(const Scenario& s) {
    const Wdf* w = base_world(s);
    if (!w) return {};
    int i = find_class(*w, K_SKY);
    if (i < 0) return {};
    std::string key = sky_key(w->objs[size_t(i)].str("Corporal3DObj"));
    for (auto& si : skies()) if (lower(si.name) == key) return si.name;
    return {};
}

// Model variant for the scenario: the _MP model for MP maps when there is one, else the plain one.
static std::string pick_model(const ScenarioIO::SkyInfo& si, bool multiplayer) {
    std::string plain, mp;
    for (auto& m : si.models) {
        std::string f = m.substr(m.find_last_of('/') + 1);
        f.erase(f.find_last_of('.'));
        (iends_with(f, "_mp") ? mp : plain) = m;
    }
    if (multiplayer) return !mp.empty() ? mp : plain;
    return !plain.empty() ? plain : mp;
}

const ScenarioIO::SkyUse* ScenarioIO::sky_source(const Scenario& s, const SkyInfo& si) {
    if (si.uses.empty()) return nullptr;
    // The Base level's own world first (restoring its sky restores it exactly), then a world of
    // the same kind using the same model variant, then the same kind, then any.
    std::string base_world;
    for (auto& m : gd_.maps())
        if (iequals(m.resource, s.base) && m.multiplayer == s.multiplayer) base_world = m.world_path;
    std::string model = pick_model(si, s.multiplayer);
    const SkyUse* best = nullptr;
    int best_score = -1;
    for (auto& u : si.uses) {
        int score = 0;
        if (!base_world.empty() && iequals(u.world, base_world)) score += 100;
        if (u.kind == (s.multiplayer ? 0 : 1)) score += 10;
        else if (u.kind != 2) score += 5;
        if (iequals(sky_folder(u.model) + "/" + u.model.substr(u.model.find_last_of("/\\") + 1),
                    sky_folder(model) + "/" + model.substr(model.find_last_of('/') + 1)))
            score += 3;
        if (score > best_score) { best_score = score; best = &u; }
    }
    return best;
}

bool ScenarioIO::sky_fog(const Scenario& s, const std::string& name, uint32_t& color) {
    const SkyInfo* si = find_sky(name);
    const SkyUse* u = si ? sky_source(s, *si) : nullptr;
    if (!u || !u->has_fog) return false;
    color = u->fog_color;
    return true;
}

bool ScenarioIO::sky_is_night(const std::string& name) {
    const SkyInfo* si = find_sky(name);
    if (!si) return false;
    // A look names its time: the sky of a night look is a night sky, the sky of a day or evening look (and no night look) is not.
    for (auto& l : weather::looks()) if (l.night && iequals(weather::look_sky(l), si->name)) return true;
    for (auto& l : weather::looks()) if (!l.night && iequals(weather::look_sky(l), si->name)) return false;
    // A sky no look uses: the typical sun of the worlds that use it (the median, so one odd cutscene does not decide). The space sky's
    // cinemas sit at 0.35 to 0.51 (median 0.355), the night skies of the looks at 0.25 to 0.31, the day skies at 0.43 and above.
    std::vector<double> lum;
    for (auto& u : si->uses)
        if (u.has_sun) lum.push_back((0.299 * ((u.sun_color >> 16) & 255) + 0.587 * ((u.sun_color >> 8) & 255) + 0.114 * (u.sun_color & 255)) / 255.0);
    if (lum.empty()) return false;
    std::sort(lum.begin(), lum.end());
    const double med = lum.size() % 2 ? lum[lum.size() / 2] : (lum[lum.size() / 2 - 1] + lum[lum.size() / 2]) * 0.5;
    return med < 0.36;
}

bool ScenarioIO::set_sky(Scenario& s, const std::string& name, bool use_fog, std::string* err) {
    const SkyInfo* si = find_sky(name);
    if (!si) { if (err) *err = "unknown sky " + name; return false; }
    int i = find_class(s.world, K_SKY);
    if (i < 0) { if (err) *err = "this world has no MASky2"; return false; }
    WObj& sky = s.world.objs[size_t(i)];
    std::string model = pick_model(*si, s.multiplayer);
    const SkyUse* u = sky_source(s, *si);
    // The source world's spelling when it uses the same file.
    if (u && iequals(sky_folder(u->model) + "/" + u->model.substr(u->model.find_last_of("/\\") + 1),
                     sky_folder(model) + "/" + model.substr(model.find_last_of('/') + 1)))
        sky.set_str("Corporal3DObj", u->model);
    else
        sky.set_str("Corporal3DObj", world_path(model));
    if (u) {
        sky.set_str("CorporalMTB", u->mtb);
        sky.set_vec3("SkyTerrainSkyScale", u->scale);
        sky.set_vec3("SkyTerrainSkyOffset", u->offset);
        sky.set_vec3("CorporalPos", u->pos);
        sky.set_f32("CorporalHeading", u->heading_deg);
    } else {
        // No level uses it as a sky: keep the current placement; the folder's .mtb if it has one.
        sky.set_str("CorporalMTB", si->mtbs.empty() ? std::string() : world_path(si->mtbs.front()));
    }
    if (use_fog && u && u->has_fog) {
        int ei = find_class(s.world, K_ENV);
        if (ei >= 0) s.world.objs[size_t(ei)].set_u32("FogColor", u->fog_color);
    }
    s.dirty = true;
    return true;
}

}  // namespace ma2
