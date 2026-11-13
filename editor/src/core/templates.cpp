#include "templates.h"

#include <algorithm>
#include <cstring>

namespace ma2 {

static const uint32_t K_GROUP = key_of("MAGroup"), K_PCFG = key_of("MAPlayerConfiguration"),
                      K_EREF = key_of("MoeEntityReference"), K_ROOT = key_of("MARoot");

static bool is_link_key(uint32_t k) {
    static const uint32_t keys[] = {key_of("UID"), key_of("parent"), key_of("child"), key_of("sibling"), key_of("TemplateID")};
    for (uint32_t x : keys) if (x == k) return true;
    return false;
}

static bool placeable_class(const std::string& c) {
    static const char* ok[] = {"MACorporeal", "MABuilding", "MAMech", "MATank", "MAFlyer", "MAVehicle", "MATurret",
                               "MAPowerUp", "MAPerson", "MAWater", "MASky2", "MACrateGenerator", "MAFlagCTF",
                               "MASnatchItBase", "MASnatchItDataCore", "MAMiniSpider", "MASpider", "MAUberBoss"};
    for (const char* x : ok) if (c == x) return true;
    return false;
}

// Category folders the palette leaves out. Matched on whole path components ("Lights" must not hide
// "Turrets/SearchLights"; retail Mission_03/04 place those). "Weapons/Vehicles/VTOL Bombs" is
// offered: retail multiplayer maps place its Heavy Bomb pickup (34 times in 14 maps).
static bool skip_category(const std::string& cat) {
    static const char* bad[] = {"Destroyed", "Loadouts", "Weapons", "Effects", "Cameras", "Objectives",
                                "Owner Controllers", "Cinema", "Avatars", "Lights", "Transitions", "zz_"};
    if (cat.find("VTOL Bombs") != std::string::npos) return false;
    if (cat.find("Cinema Versions") != std::string::npos) return false;  // staged 'Mechs of the cinemas (Cutscene props)
    size_t pos = 0;
    while (pos <= cat.size()) {
        size_t e = cat.find('/', pos);
        std::string part = cat.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        for (const char* b : bad) {
            if (std::string_view(b) == "Lights" || std::string_view(b) == "Weapons") {
                if (part == b) return true;  // the whole folder, not "SearchLights"
            } else if (part.find(b) != std::string::npos) {
                return true;
            }
        }
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return false;
}

bool TemplateCatalog::load(GameData& gd, std::string* err) {
    gd_ = &gd;
    mtb_refs_.clear();
    Bytes b;
    if (!gd.read_common("Templates/GlobalEntities.wdf", b, err)) return false;
    if (!ge_.parse(b, err)) return false;
    tree_.build(ge_);
    infos_.clear();
    by_uid_.clear();
    infos_.reserve(ge_.objs.size());
    for (size_t i = 0; i < ge_.objs.size(); i++) {
        const WObj& o = ge_.objs[i];
        TemplateInfo t;
        t.uid = o.uid();
        t.cls = o.cls;
        t.cls_name = gd.names().label(o.cls);
        t.name = o.description();
        t.model = o.str("Corporal3DObj");
        t.mtb = o.str("CorporalMTB");
        t.index = int(i);
        // Category: group descriptions from below the root.
        std::vector<std::string> parts;
        int p = tree_.parent_of[i];
        while (p >= 0 && ge_.objs[p].cls != K_ROOT) {
            parts.push_back(ge_.objs[p].description());
            p = tree_.parent_of[p];
        }
        for (size_t k = parts.size(); k-- > 0;) t.category += trim(parts[k]) + (k ? "/" : "");
        // Effect objects (fires, smoke, explosions, set effects) are placeable like the retail levels do; the
        // weapon-fire and weapon-hit effects belong to the weapons, not to a map.
        bool effect = t.cls_name == "MAEffect" && ((t.category.rfind("Effects/", 0) == 0 && t.category.rfind("Effects/Weapons", 0) != 0) ||
                                                   t.name == "Set - Minispider carcass");
        t.placeable = effect || (placeable_class(t.cls_name) && !skip_category(t.category) && t.cls != K_GROUP);
        by_uid_[t.uid] = int(infos_.size());
        infos_.push_back(std::move(t));
        if (o.cls == K_PCFG) player_configs_.push_back(int(i));
        if (o.cls == K_EREF) entity_refs_.push_back(int(i));
    }
    // Learn template-reference keys.
    std::unordered_map<uint32_t, std::pair<int, int>> stat;
    for (auto& o : ge_.objs)
        for (auto& p : o.props) {
            if ((p.type != PT_I32 && p.type != PT_U32) || p.data.size() != 4 || is_link_key(p.key)) continue;
            uint32_t v = uint32_t(p.as_int());
            if (!v) continue;
            auto& s = stat[p.key];
            s.first++;
            if (by_uid_.count(v)) s.second++;
        }
    ref_keys_.clear();
    for (auto& [k, s] : stat) {
        if (s.second < 1 || s.second * 10 < s.first * 9) continue;
        const std::string* n = gd.names().name(k);
        if (n && n->size() > 2 && n->compare(0, 2, "ob") == 0 && n->find("obj") != std::string::npos) continue;  // objective slots
        ref_keys_.insert(k);
    }
    // Culled sets.
    culled_.clear();
    if (MgfReader* tm = gd.templates_archive()) {
        for (auto& f : tm->files()) {
            std::string n = lower(f.path);
            const std::string pre = "culledtemplates/globalentities.";
            if (n.compare(0, pre.size(), pre) != 0 || !iends_with(n, ".wdf")) continue;
            std::string level = n.substr(pre.size(), n.size() - pre.size() - 4);
            Bytes cb;
            Wdf cw;
            if (!tm->read_file(f.path, cb) || !cw.parse(cb)) continue;
            auto& set = culled_[level];
            for (auto& o : cw.objs) set.insert(o.uid());
        }
    }
    auto intersect = [&](bool mp) {
        std::unordered_set<uint32_t> acc;
        bool first = true;
        for (auto& m : gd.maps()) {
            if (m.multiplayer != mp) continue;
            auto it = culled_.find(lower(m.name));
            if (it == culled_.end()) continue;
            if (first) { acc = it->second; first = false; continue; }
            std::unordered_set<uint32_t> next;
            for (uint32_t u : acc) if (it->second.count(u)) next.insert(u);
            acc.swap(next);
        }
        return acc;
    };
    common_mp_ = intersect(true);
    common_mission_ = intersect(false);
    // Archive file index.
    archive_files_.clear();
    archive_sets_.clear();
    file_archives_.clear();
    const auto& arcs = gd.level_archives();
    for (size_t a = 0; a < arcs.size(); a++) {
        std::vector<std::string> files;
        if (MgfReader* r = gd.archive(arcs[a])) for (auto& f : r->files()) files.push_back(norm_path(f.path));
        std::unordered_set<std::string> set(files.begin(), files.end());
        for (auto& f : files) file_archives_[f].push_back(int(a));
        archive_files_.push_back(std::move(files));
        archive_sets_.push_back(std::move(set));
    }
    for (auto& t : infos_) {
        if (t.model.empty()) continue;
        auto it = file_archives_.find(norm_path(t.model));
        if (it != file_archives_.end()) t.archives = it->second;
    }
    return true;
}

const TemplateInfo* TemplateCatalog::find(uint32_t uid) const {
    auto it = by_uid_.find(uid);
    return it == by_uid_.end() ? nullptr : &infos_[size_t(it->second)];
}

const WObj* TemplateCatalog::object(uint32_t uid) const {
    const TemplateInfo* t = find(uid);
    return t ? &ge_.objs[size_t(t->index)] : nullptr;
}

std::vector<std::string> TemplateCatalog::culled_levels() const {
    std::vector<std::string> v;
    for (auto& [k, s] : culled_) v.push_back(k);
    return v;
}

const std::unordered_set<uint32_t>& TemplateCatalog::culled_set(const std::string& level) const {
    auto it = culled_.find(lower(level));
    return it == culled_.end() ? empty_ : it->second;
}

size_t TemplateCatalog::model_template_count() const {
    size_t n = 0;
    for (auto& t : infos_) if (!t.archives.empty()) n++;
    return n;
}

std::unordered_set<uint32_t> TemplateCatalog::seed_of(const Wdf& world) const {
    std::unordered_set<uint32_t> s;
    for (auto& o : world.objs) {
        uint32_t tid = o.template_id();
        if (tid && by_uid_.count(tid)) s.insert(tid);
        for (auto& p : o.props) {
            if (!ref_keys_.count(p.key) || p.data.size() != 4) continue;
            uint32_t v = uint32_t(p.as_int());
            if (v && by_uid_.count(v)) s.insert(v);
        }
    }
    return s;
}

// The .mtb of a template is a WDF of the model's pieces; a piece can carry entity properties that
// name templates (TemplateID of an embedded object, DestroyedEffect, MAEffectID, ...). Example:
// BldCom_DAN_Office18_WS_2_CS.mtb gives 12 chunks DestroyedEffect = 0x1E0DE (BD_Debris_Crash_LG_01,
// files BD_Debris_Crash_LG_01.MGMODEL/.mtb/.mgparticle). The game builds those objects with the
// building (MABuilding slot 14 creates DestroyedBuilding and runs sub_B5E80 over it); a missing file
// fails that build, B4720 passes false to vtable slot 0 (sub_D8221) and the building draws as the
// pink placeholder: a 40 m pole and a 10 m cube (sub_D8391), colour 0xFFFE6BE8.
const std::vector<uint32_t>& TemplateCatalog::mtb_refs(uint32_t uid) const {
    auto hit = mtb_refs_.find(uid);
    if (hit != mtb_refs_.end()) return hit->second;
    std::vector<uint32_t>& out = mtb_refs_[uid];
    const TemplateInfo* t = find(uid);
    if (!t || !gd_) return out;
    std::string mtb = t->mtb;
    if (mtb.empty() && !t->model.empty()) {
        size_t dot = t->model.rfind('.');
        mtb = (dot == std::string::npos ? t->model : t->model.substr(0, dot)) + ".mtb";
    }
    if (mtb.empty()) return out;
    auto fa = file_archives_.find(norm_path(mtb));
    if (fa == file_archives_.end() || fa->second.empty()) return out;
    Bytes data;
    MgfReader* r = gd_->archive(gd_->level_archives()[size_t(fa->second.front())]);
    if (!r || !r->read_file(mtb, data)) return out;
    // Records: 02 2F 02 FF, u32 type, u32 key, u32 length, value (the WDF property layout).
    // MAEffectID (rubble and dust effects of collapse sequences) only occurs in .mtb files, so the
    // reference keys learned from GlobalEntities do not include it.
    static const uint32_t K_TID = key_of("TemplateID"), K_EFFECT = key_of("MAEffectID");
    std::unordered_set<uint32_t> got;
    for (size_t i = 0; i + 20 <= data.size(); i++) {
        if (data[i] != 0x02 || data[i + 1] != 0x2F || data[i + 2] != 0x02 || data[i + 3] != 0xFF) continue;
        uint32_t type, key, len, v;
        memcpy(&type, &data[i + 4], 4);
        memcpy(&key, &data[i + 8], 4);
        memcpy(&len, &data[i + 12], 4);
        if (len != 4 || (type != PT_I32 && type != PT_U32) || (key != K_TID && key != K_EFFECT && !ref_keys_.count(key))) continue;
        memcpy(&v, &data[i + 16], 4);
        if (v && v != uid && by_uid_.count(v) && got.insert(v).second) out.push_back(v);
    }
    return out;
}

// Hard-coded asset paths in the XBE (2026-11-09 survey of every *.mgparticle/.mat/.mgmodel/.node/.tif/.mtb
// literal): only MABuilding's are needed to BUILD an object. The others are runtime spawns or HUD art that every
// Base or common.mgf already holds (Env_LeafScatter(_Snowy), Env_Mech_FootStep_01, HUD/reticle sprites), a texture
// the data also names (Wpn_Camoflouge_Anim.tif, CamouflageTexture of 8 templates), or files on no disc archive
// (Mortar_reticle, shadow.node).
std::vector<std::string> TemplateCatalog::code_required(uint32_t uid) const {
    const TemplateInfo* t = find(uid);
    if (t && t->cls_name == "MABuilding")
        return {"\\SpecialEffects\\BuildingDestruction\\BD_Holefire_Loop_01.mgparticle",
                "\\SpecialEffects\\BuildingDestruction\\BD_WindowShatter.mgparticle",
                "\\SpecialEffects\\BuildingDestruction\\BD_HoleDamage.mgparticle",
                "\\SpecialEffects\\Effects_Decals\\BD_HoleDecal.mat"};
    return {};
}

std::unordered_set<uint32_t> TemplateCatalog::closure(const std::unordered_set<uint32_t>& seed, bool with_mtb) const {
    std::unordered_set<uint32_t> need;
    std::vector<uint32_t> stack(seed.begin(), seed.end());
    static const uint32_t K_CORP_TPL = key_of("CorporealTemplateUID"), K_REF = key_of("ReferenceUID");
    for (;;) {
        while (!stack.empty()) {
            uint32_t u = stack.back();
            stack.pop_back();
            auto it = by_uid_.find(u);
            if (it == by_uid_.end() || need.count(u)) continue;
            need.insert(u);
            int idx = infos_[size_t(it->second)].index;
            const WObj& o = ge_.objs[size_t(idx)];
            for (auto& p : o.props) {
                if (!ref_keys_.count(p.key) || p.data.size() != 4) continue;
                uint32_t v = uint32_t(p.as_int());
                if (v && by_uid_.count(v) && !need.count(v)) stack.push_back(v);
            }
            if (with_mtb)
                for (uint32_t v : mtb_refs(u)) if (!need.count(v)) stack.push_back(v);
            if (o.cls != K_GROUP)
                for (int k : tree_.kids[size_t(idx)]) stack.push_back(ge_.objs[size_t(k)].uid());
        }
        for (int i : player_configs_) {
            const WObj& o = ge_.objs[size_t(i)];
            const WProp* p = o.find(K_CORP_TPL);
            if (p && need.count(uint32_t(p->as_int())) && !need.count(o.uid())) stack.push_back(o.uid());
        }
        for (int i : entity_refs_) {
            const WObj& o = ge_.objs[size_t(i)];
            const WProp* p = o.find(K_REF);
            if (p && need.count(uint32_t(p->as_int())) && !need.count(o.uid())) stack.push_back(o.uid());
        }
        if (stack.empty()) break;
    }
    std::unordered_set<uint32_t> full = need;
    for (uint32_t u : need) {
        int idx = infos_[size_t(by_uid_.at(u))].index;
        int p = tree_.parent_of[size_t(idx)];
        while (p >= 0) {
            full.insert(ge_.objs[size_t(p)].uid());
            p = tree_.parent_of[size_t(p)];
        }
    }
    if (tree_.root >= 0) full.insert(ge_.objs[size_t(tree_.root)].uid());
    return full;
}

std::unordered_set<uint32_t> TemplateCatalog::closure_for_world(const Wdf& world, const std::string& base_level, bool mp) const {
    std::unordered_set<uint32_t> seed = seed_of(world);
    for (uint32_t u : common_set(mp)) seed.insert(u);
    for (uint32_t u : culled_set(base_level)) seed.insert(u);
    std::unordered_set<uint32_t> out = closure(seed);
    for (uint32_t u : culled_set(base_level)) out.insert(u);  // includes groups closure() skips
    for (uint32_t u : common_set(mp)) out.insert(u);
    return out;
}

Bytes TemplateCatalog::write_culled(const std::unordered_set<uint32_t>& uids) const { return culled(uids).serialize(); }

Wdf TemplateCatalog::culled(const std::unordered_set<uint32_t>& uids) const {
    Wdf out;
    out.header = ge_.header;
    std::vector<int> keep;
    for (size_t i = 0; i < ge_.objs.size(); i++) if (uids.count(ge_.objs[i].uid())) keep.push_back(int(i));
    std::unordered_set<int> kept(keep.begin(), keep.end());
    out.objs.reserve(keep.size());
    for (int i : keep) {
        WObj o = ge_.objs[size_t(i)];
        uint32_t child = 0, sibling = 0;
        for (int k : tree_.kids[size_t(i)]) if (kept.count(k)) { child = ge_.objs[size_t(k)].uid(); break; }
        int p = tree_.parent_of[size_t(i)];
        if (p >= 0) {
            const auto& sib = tree_.kids[size_t(p)];
            auto it = std::find(sib.begin(), sib.end(), i);
            for (++it; it != sib.end(); ++it) if (kept.count(*it)) { sibling = ge_.objs[size_t(*it)].uid(); break; }
        }
        if (WProp* c = o.find("child")) c->set_int(child);
        if (WProp* s = o.find("sibling")) s->set_int(sibling);
        out.objs.push_back(std::move(o));
    }
    return out;
}

int TemplateCatalog::archive_of(const std::string& path) const {
    auto it = file_archives_.find(norm_path(path));
    return it == file_archives_.end() || it->second.empty() ? -1 : it->second.front();
}

std::vector<int> TemplateCatalog::archives_of(const std::string& path) const {
    auto it = file_archives_.find(norm_path(path));
    return it == file_archives_.end() ? std::vector<int>{} : it->second;
}

const std::vector<std::string>* TemplateCatalog::archive_files(int a) const {
    return a >= 0 && size_t(a) < archive_files_.size() ? &archive_files_[size_t(a)] : nullptr;
}

bool TemplateCatalog::archive_has(int a, const std::string& path) const {
    return a >= 0 && size_t(a) < archive_sets_.size() && archive_sets_[size_t(a)].count(norm_path(path));
}

}  // namespace ma2
