// Multiplayer authoring helpers: spawns, vehicle pads, cloning retail subtrees.
#include <algorithm>
#include <cmath>
#include <set>

#include "scenario.h"

namespace ma2 {

using namespace world;

static const uint32_t K_SPAWNER = key_of("MASpawner"), K_CORP = key_of("MACorporeal"), K_SET = key_of("MoeEntitySet"),
                      K_GROUPSET = key_of("MoeGroupSet"), K_MECH = key_of("MAMech"), K_TANK = key_of("MATank"),
                      K_FLYER = key_of("MAFlyer"), K_VEHICLE = key_of("MAVehicle");

const Wdf* ScenarioIO::retail_world(const std::string& map) {
    std::string key = lower(map);
    auto it = worlds_.find(key);
    if (it != worlds_.end()) return it->second.get();
    const MapInfo* m = gd_.find_map(map);
    if (!m) return nullptr;
    auto w = std::make_unique<Wdf>();
    if (!gd_.load_world(*m, *w)) return nullptr;
    if (worlds_.size() >= 6) worlds_.erase(worlds_.begin());
    const Wdf* raw = w.get();
    worlds_[key] = std::move(w);
    return raw;
}

void ScenarioIO::index_instances() {
    if (instances_ready_) return;
    instances_ready_ = true;
    for (auto& m : gd_.maps()) {
        if (!m.resource_present) continue;
        Wdf w;
        if (!gd_.load_world(m, w)) continue;
        for (auto& o : w.objs) {
            uint32_t t = o.template_id();
            if (!t || !position(o)) continue;
            // Up to 4 instances from MP maps and 4 from missions: MP 'Mechs are empty pickups
            // (StartAbandoned), mission ones have pilots.
            auto& v = instances_[t];
            int same = 0;
            for (auto& i : v) { const MapInfo* im = gd_.find_map(i.map); same += im && im->multiplayer == m.multiplayer; }
            if (same < 4) v.push_back({m.name, o.uid()});
        }
    }
}

const ScenarioIO::Instance* ScenarioIO::retail_instance(uint32_t tpl, bool prefer_mp) {
    index_instances();
    auto it = instances_.find(tpl);
    if (it == instances_.end() || it->second.empty()) return nullptr;
    for (auto& i : it->second) {
        const MapInfo* m = gd_.find_map(i.map);
        if (m && m->multiplayer == prefer_mp) return &i;
    }
    return &it->second.front();
}

// ----------------------------------------------------------------------------- cloning

void ScenarioIO::fixup_clone(Scenario& s, std::vector<WObj>& clone, const Wdf& src) {
    // Spawner runtime ids (0xF80000xx) must be unique in the world.
    uint32_t max_id = 0xF8000000u;
    for (auto& o : s.world.objs)
        if (o.cls == K_SPAWNER) max_id = std::max({max_id, o.u32("InitialID"), o.u32("InitialSecondID")});
    for (auto& o : clone)
        if (o.cls == K_SPAWNER) {
            if (o.find("InitialID")) o.set_u32("InitialID", ++max_id);
            if (o.find("InitialSecondID")) o.set_u32("InitialSecondID", ++max_id);
        }
    // Entity-set membership: map the source set to the set with the same name here.
    std::unordered_map<uint32_t, std::string> src_sets;
    for (auto& o : src.objs) if (o.cls == K_SET) src_sets[o.uid()] = o.description();
    // A new set takes an id above the clone's own: the clone was numbered from the world's next id, and a set
    // sharing one of those ids had its links renumbered with the clone (Grinder kit: every wave 'Mech's
    // SetMember pointed at the kit's group or its path data, bots F47).
    uint32_t clone_top = 0;
    for (auto& o : clone) clone_top = std::max(clone_top, o.uid());
    auto our_set = [&](const std::string& name) -> uint32_t {
        for (auto& o : s.world.objs) if (o.cls == K_SET && o.description() == name) return o.uid();
        // Create it in the world's MoeGroupSet by cloning the source set object.
        int gs = find_class(s.world, K_GROUPSET);
        if (gs < 0) return 0;
        for (auto& o : src.objs)
            if (o.cls == K_SET && o.description() == name) {
                WObj c = o;
                uint32_t uid = std::max(next_uid(s.world), clone_top + 1);
                c.set_u32("UID", uid);
                c.set_u32("child", 0);
                // The source's members are source ids (here they name nothing, or the wrong object); the save
                // lists the objects that name this set (fill_set_members).
                const auto& nm = gd_.names();
                for (size_t k = c.props.size(); k-- > 0;)
                    if (const std::string* pn = nm.name(c.props[k].key); pn && pn->compare(0, 18, "MoeEntitySetMember") == 0) c.erase(c.props[k].key);
                if (c.find("MoeEntitySetCount")) c.set_i32("MoeEntitySetCount", 0);
                wedit::append_child(s.world, s.world.objs[size_t(gs)].uid(), std::move(c));
                return uid;
            }
        return 0;
    };
    const auto& names = gd_.names();
    for (auto& o : clone)
        for (auto& p : o.props) {
            if (p.data.size() != 4 || (p.type != PT_U32 && p.type != PT_I32)) continue;
            const std::string* n = names.name(p.key);
            if (!n) continue;
            // Every '...Set' field names an entity set: AISet, ActionSet, and the Grinder wave triggers'
            // DestroySet / RadiusSet ('wave cleared' -> next wave; unmapped, the waves never advanced).
            bool setref = n->compare(0, 9, "SetMember") == 0 || (n->size() > 3 && n->compare(n->size() - 3, 3, "Set") == 0);
            if (!setref) continue;
            auto it = src_sets.find(uint32_t(p.as_int()));
            if (it == src_sets.end()) continue;
            p.set_int(our_set(it->second));
        }
}

uint32_t ScenarioIO::clone_from_world(Scenario& s, const Wdf& src, uint32_t uid, uint32_t parent_uid) {
    std::unordered_set<uint32_t> keep = cat_.ref_keys();
    keep.erase(key_of("ReferenceUID"));
    keep.insert(key_of("TemplateID"));
    uint32_t next = next_uid(s.world);
    auto clone = wedit::clone_subtree(src, uid, next, &keep);
    if (clone.empty()) return 0;
    fixup_clone(s, clone, src);
    // fixup may have created sets (new UIDs): renumber the clone above them if needed.
    uint32_t root = clone.front().uid();
    if (root < next_uid(s.world)) {
        uint32_t base = next_uid(s.world);
        std::unordered_map<uint32_t, uint32_t> remap;
        for (auto& o : clone) remap[o.uid()] = base++;
        for (auto& o : clone)
            for (auto& p : o.props) {
                if (p.data.size() != 4 || (p.type != PT_U32 && p.type != PT_I32) || keep.count(p.key)) continue;
                auto it = remap.find(uint32_t(p.as_int()));
                if (it != remap.end() && p.as_int()) p.set_int(it->second);
            }
        root = clone.front().uid();
    }
    wedit::insert_subtree(s.world, parent_uid, std::move(clone));
    s.dirty = true;
    return root;
}

uint32_t ScenarioIO::clone_from_retail(Scenario& s, const std::string& source, uint32_t uid, uint32_t parent_uid,
                                       std::vector<WObj>* out_objs) {
    const Wdf* src = retail_world(source);
    if (!src) return 0;
    uint32_t r = clone_from_world(s, *src, uid, parent_uid);
    if (out_objs && r) {
        WTree t;
        t.build(s.world);
        std::vector<int> sub;
        t.subtree(t.find(r), sub);
        for (int i : sub) out_objs->push_back(s.world.objs[size_t(i)]);
    }
    return r;
}

// ----------------------------------------------------------------------------- spawns

static uint32_t ensure_mtg(ScenarioIO& io, Scenario& s, const std::string& desc, const std::vector<std::string>& modes) {
    for (auto& o : s.world.objs)
        if (o.cls == K_MTG && o.description() == desc) {
            for (auto& g : game_types()) if (WProp* f = o.find(g.flag)) f->set_int(0);
            for (auto& m : modes) if (auto* g = find_game_type(m)) if (WProp* f = o.find(g->flag)) f->set_int(1);
            return o.uid();
        }
    // Prototype: any retail MAMissionTypeGroup.
    const Wdf* proto = io.retail_world("BoggedDown");
    int pi = proto ? find_class(*proto, K_MTG) : -1;
    if (pi < 0) return 0;
    WObj g = proto->objs[size_t(pi)];
    uint32_t uid = next_uid(s.world);
    g.set_u32("UID", uid);
    g.set_u32("child", 0);
    g.set_str("Description", desc);
    for (auto& t : game_types()) if (WProp* f = g.find(t.flag)) f->set_int(0);
    for (auto& m : modes) if (auto* t = find_game_type(m)) if (WProp* f = g.find(t->flag)) f->set_int(1);
    wedit::append_child(s.world, terrain_uid(s.world), std::move(g));
    return uid;
}

static std::vector<std::string> modes_of(const Scenario& s, bool team) {
    std::vector<std::string> v;
    for (auto& t : s.types) if (auto* g = find_game_type(t)) if (g->team == team) v.push_back(g->ini);
    if (v.empty()) for (auto& g : game_types()) if (g.team == team) v.push_back(g.ini);
    return v;
}

void ScenarioIO::sync_spawn_groups(Scenario& s) {
    for (auto& o : s.world.objs) {
        if (o.cls != K_MTG) continue;
        bool team = o.description() == "Team Spawns", ffa = o.description() == "FFA Spawns";
        if (!team && !ffa) continue;
        for (auto& g : game_types()) if (WProp* f = o.find(g.flag)) f->set_int(0);
        for (auto& m : modes_of(s, team)) if (auto* g = find_game_type(m)) if (WProp* f = o.find(g->flag)) f->set_int(1);
    }
    // Root flags follow the scenario's list.
    int ri = find_class(s.world, K_ROOT);
    if (ri >= 0) {
        WObj& r = s.world.objs[size_t(ri)];
        for (auto& g : game_types()) if (WProp* f = r.find(g.flag)) f->set_int(0);
        for (auto& t : s.types) if (auto* g = find_game_type(t)) if (WProp* f = r.find(g->flag)) f->set_int(1);
    }
}

uint32_t ScenarioIO::add_spawn(Scenario& s, int team, Vec3 pos, float heading) {
    bool is_team = team == 1 || team == 2;
    uint32_t mtg = ensure_mtg(*this, s, is_team ? "Team Spawns" : "FFA Spawns", modes_of(s, is_team));
    if (!mtg) return 0;
    uint32_t next = next_uid(s.world);
    const char* gname = team == 1 ? "Red Spawn Points" : team == 2 ? "Blue Spawn Points" : "Spawn Points";
    uint32_t grp = ensure_group(s.world, mtg, gname, next);
    // Prototype MALevelStart: Base world first, then BoggedDown.
    const WObj* proto = nullptr;
    for (auto& o : s.world.objs) if (o.cls == K_LEVELSTART) { proto = &o; break; }
    WObj ls;
    if (proto) ls = *proto;
    else {
        const Wdf* bw = retail_world("BoggedDown");
        int li = bw ? find_class(*bw, K_LEVELSTART) : -1;
        if (li < 0) return 0;
        ls = bw->objs[size_t(li)];
    }
    uint32_t uid = next_uid(s.world);
    ls.set_u32("UID", uid);
    ls.set_u32("child", 0);
    ls.set_i32("TeamIndex", team);
    ls.set_str("Description", "Start Point");
    pos.y = s.terrain.height_at(pos.x, pos.z) + 2.f;
    set_pos(ls, pos);
    set_heading(ls, heading);
    wedit::append_child(s.world, grp, std::move(ls));
    s.dirty = true;
    return uid;
}

void ScenarioIO::add_spawn_ring(Scenario& s, int team, Vec3 c, float radius, int count) {
    for (int i = 0; i < count; i++) {
        float a = float(i) / float(count) * 6.2831853f;
        Vec3 p{c.x + std::cos(a) * radius, 0, c.z + std::sin(a) * radius};
        p.x = std::clamp(p.x, 10.f, s.terrain.size_x() - 10.f);
        p.z = std::clamp(p.z, 10.f, s.terrain.size_z() - 10.f);
        // Face the middle of the map.
        float hx = s.terrain.size_x() * 0.5f - p.x, hz = s.terrain.size_z() * 0.5f - p.z;
        add_spawn(s, team, p, std::atan2(hx, hz));
    }
}

// ----------------------------------------------------------------------------- placing objects

void ScenarioIO::move_subtree_to(Scenario& s, uint32_t root_uid, Vec3 at, float heading, bool set_heading_too,
                                 const Terrain* src_terrain) {
    WTree t;
    t.build(s.world);
    int ri = t.find(root_uid);
    Vec3 old;
    if (ri < 0 || !get_pos(s.world.objs[size_t(ri)], old)) return;
    std::vector<int> sub;
    t.subtree(ri, sub);
    for (int i : sub) {
        Vec3 p;
        WObj& o = s.world.objs[size_t(i)];
        if (!get_pos(o, p)) continue;
        float above = src_terrain ? p.y - src_terrain->height_at(p.x, p.z) : p.y - s.terrain.height_at(p.x, p.z);
        Vec3 q{p.x - old.x + at.x, 0, p.z - old.z + at.z};
        q.y = s.terrain.height_at(q.x, q.z) + std::clamp(above, -50.f, 400.f);
        set_pos(o, q);
    }
    if (set_heading_too) world::set_heading(s.world.objs[size_t(ri)], heading);
}

uint32_t ScenarioIO::place_template(Scenario& s, uint32_t tpl, Vec3 at, float heading) {
    const TemplateInfo* ti = cat_.find(tpl);
    if (!ti) return 0;
    uint32_t next = next_uid(s.world);
    uint32_t grp = ensure_group(s.world, terrain_uid(s.world), "Placed Objects", next);
    uint32_t root = 0;
    if (const Instance* inst = retail_instance(tpl, s.multiplayer)) {
        root = clone_from_retail(s, inst->map, inst->uid, grp);
        if (root) {
            Terrain st;
            const Wdf* src = retail_world(inst->map);
            bool have = src && st.load_from_world(*src);
            move_subtree_to(s, root, at, heading, true, have ? &st : nullptr);
            // The source mission's AI orders, paths, objective markers and kill references
            // point at its own triggers and objects: drop them from the copy.
            static const uint32_t strip[] = {key_of("MAAIGroup"), key_of("MAWaypointGroup"), key_of("MAObjectiveType"),
                                             key_of("MAEvent"), key_of("MAPath"), key_of("MAKillReference"),
                                             key_of("MoeEntityReference")};
            for (;;) {
                WTree t;
                t.build(s.world);
                std::vector<int> sub;
                t.subtree(t.find(root), sub);
                uint32_t victim = 0;
                for (int i : sub)
                    if (i != t.find(root) && std::find(std::begin(strip), std::end(strip), s.world.objs[size_t(i)].cls) != std::end(strip)) {
                        victim = s.world.objs[size_t(i)].uid();
                        break;
                    }
                if (!victim) break;
                wedit::remove_subtree(s.world, victim);
            }
        }
    }
    if (!root) {
        // No retail instance: instantiate the template object itself.
        WObj o = cat_.global().objs[size_t(ti->index)];
        root = next_uid(s.world);
        o.set_u32("UID", root);
        o.set_u32("TemplateID", tpl);
        o.set_u32("child", 0);
        o.set_u32("parent", grp);
        set_pos(o, {at.x, s.terrain.height_at(at.x, at.z), at.z});
        world::set_heading(o, heading);
        wedit::append_child(s.world, grp, std::move(o));
    }
    s.dirty = true;
    return root;
}

// ----------------------------------------------------------------------------- vehicle pads

const std::vector<ScenarioIO::PadKind>& ScenarioIO::pad_kinds() {
    if (pads_ready_) return pads_;
    pads_ready_ = true;
    std::unordered_set<std::string> seen;
    for (auto& m : gd_.maps()) {
        if (!m.multiplayer || !m.resource_present) continue;
        Wdf w;
        if (!gd_.load_world(m, w)) continue;
        WTree t;
        t.build(w);
        for (size_t i = 0; i < w.objs.size(); i++) {
            if (w.objs[i].cls != K_SPAWNER) continue;
            int pad = t.parent_of[i];
            if (pad < 0 || w.objs[size_t(pad)].cls != K_CORP || t.kids[i].empty()) continue;
            const WObj& v = w.objs[size_t(t.kids[i].front())];
            if (v.cls != K_MECH && v.cls != K_TANK && v.cls != K_FLYER && v.cls != K_VEHICLE) continue;
            std::string label = v.description();
            if (!seen.insert(lower(label) + "|" + m.name).second) continue;  // one per vehicle and map
            pads_.push_back({label, m.name, w.objs[size_t(pad)].uid()});
        }
    }
    std::sort(pads_.begin(), pads_.end(), [](const PadKind& a, const PadKind& b) { return lower(a.label) < lower(b.label); });
    return pads_;
}

uint32_t ScenarioIO::add_pad(Scenario& s, const PadKind& kind, Vec3 pos, float heading) {
    uint32_t next = next_uid(s.world);
    uint32_t grp = ensure_group(s.world, terrain_uid(s.world), "Vehicle Pads", next);
    uint32_t root = clone_from_retail(s, kind.map, kind.pad_uid, grp);
    if (!root) return 0;
    WTree t;
    t.build(s.world);
    int ri = t.find(root);
    Vec3 old;
    if (ri < 0 || !get_pos(s.world.objs[size_t(ri)], old)) return root;
    // Keep the pad's height above the ground it had in the source map.
    const Wdf* src = retail_world(kind.map);
    float above = 0.f;
    if (src) {
        Terrain st;
        if (st.load_from_world(*src)) above = old.y - st.height_at(old.x, old.z);
    }
    pos.y = s.terrain.height_at(pos.x, pos.z) + above;
    // Move and turn the whole pad: the vehicle waiting on it is a child with its own heading (it kept the source
    // map's facing, e.g. -27 deg, while only the pad turned).
    const float turn = heading - world::heading(s.world.objs[size_t(ri)]);
    const float c = std::cos(turn), sn = std::sin(turn);
    std::vector<int> sub;
    t.subtree(ri, sub);
    for (int i : sub) {
        WObj& o = s.world.objs[size_t(i)];
        Vec3 p;
        if (get_pos(o, p)) {
            const float ox = p.x - old.x, oz = p.z - old.z;  // forward is (sin h, cos h)
            set_pos(o, {pos.x + ox * c + oz * sn, p.y + pos.y - old.y, pos.z + oz * c - ox * sn});
        }
        if (i != ri) set_heading(o, world::heading(o) + turn);
    }
    set_heading(s.world.objs[size_t(ri)], heading);
    return root;
}

}  // namespace ma2

namespace ma2 {

// Sets a placed unit's side. An empty (neutral) 'Mech or vehicle also needs its type's owner
// controller (MAOwnerController, MoverTemplateUID = the unit's template): that is what lets the
// player climb in. Every retail empty unit has one (Mission 13 "Abandoned Mech 01", every MP pad
// vehicle); the one without ("Star Adder No OC") cannot be entered, and neither could a placed
// Stiletto copied from a crewed mission enemy (player report 2026-10-21).
void ScenarioIO::apply_unit_side(Scenario& s, uint32_t uid, int nation) {
    static const uint32_t K_OC = key_of("MAOwnerController");
    static const uint32_t vehicles[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer")};
    int idx = -1;
    for (size_t i = 0; i < s.world.objs.size(); i++) if (s.world.objs[i].uid() == uid) { idx = int(i); break; }
    if (idx < 0) return;
    WObj& o = s.world.objs[size_t(idx)];
    set_unit_side(o, nation);
    bool vehicle = std::find(std::begin(vehicles), std::end(vehicles), o.cls) != std::end(vehicles);
    if (!vehicle || nation != 0) return;
    if (o.find("AttackPilot")) o.set_i32("AttackPilot", 0);
    if (o.find("InitiallyCollidable")) o.set_i32("InitiallyCollidable", 1);
    if (o.find("OverrideEnable")) o.set_i32("OverrideEnable", 0);  // every retail empty unit has 0
    uint32_t tpl = o.template_id();
    // A type no retail world and no template gives an owner controller cannot be boarded (Partisan,
    // APCs, trucks, civilian cars...): it stays crewed and neutral, as the dictionary audit found.
    if (!can_board(tpl)) {
        if (o.find("StartAbandoned")) o.set_i32("StartAbandoned", 0);
        return;
    }
    if (o.find("m_bAIEnabled")) o.set_i32("m_bAIEnabled", 0);
    set_boardable(s, uid, true);
}

bool ScenarioIO::can_board(uint32_t tpl) {
    static const uint32_t K_OC = key_of("MAOwnerController");
    index_owner_controllers();
    if (oc_index_.count(tpl)) return true;
    for (auto& g : cat_.global().objs) if (g.cls == K_OC && g.u32("MoverTemplateUID") == tpl) return true;
    return false;
}

bool ScenarioIO::boardable(const Scenario& s, uint32_t uid) {
    static const uint32_t K_OC = key_of("MAOwnerController");
    WTree t;
    t.build(s.world);
    for (size_t i = 0; i < s.world.objs.size(); i++)
        if (s.world.objs[i].uid() == uid) {
            for (int k : t.kids[i]) if (s.world.objs[size_t(k)].cls == K_OC) return true;
            return false;
        }
    return false;
}

void ScenarioIO::index_owner_controllers() {
    static const uint32_t K_OC = key_of("MAOwnerController");
    if (oc_index_ready_) return;
    oc_index_ready_ = true;
    for (auto& m : gd_.maps()) {
        if (!m.resource_present) continue;
        const Wdf* w = retail_world(m.name);
        if (!w) continue;
        for (auto& x : w->objs)
            if (x.cls == K_OC && x.u32("MoverTemplateUID") && !oc_index_.count(x.u32("MoverTemplateUID")))
                oc_index_[x.u32("MoverTemplateUID")] = {m.name, x.uid()};
    }
}

// Adds (or removes) the unit's owner controller: the child that lets the player climb in.
void ScenarioIO::set_boardable(Scenario& s, uint32_t uid, bool on) {
    static const uint32_t K_OC = key_of("MAOwnerController");
    int idx = -1;
    for (size_t i = 0; i < s.world.objs.size(); i++) if (s.world.objs[i].uid() == uid) { idx = int(i); break; }
    if (idx < 0) return;
    WTree t;
    t.build(s.world);
    std::vector<uint32_t> ocs;
    for (int k : t.kids[size_t(idx)]) if (s.world.objs[size_t(k)].cls == K_OC) ocs.push_back(s.world.objs[size_t(k)].uid());
    if (!on) {
        for (uint32_t u : ocs) wedit::remove_subtree(s.world, u);
        return;
    }
    if (!ocs.empty()) return;  // already enterable
    uint32_t tpl = s.world.objs[size_t(idx)].template_id();
    // The type's owner controller: from a retail world (its offsets fit this unit), else the template.
    index_owner_controllers();
    auto it = oc_index_.find(tpl);
    if (it != oc_index_.end()) {
        if (const Wdf* w = retail_world(it->second.map)) { clone_from_world(s, *w, it->second.uid, uid); return; }
    }
    for (auto& g : cat_.global().objs)
        if (g.cls == K_OC && g.u32("MoverTemplateUID") == tpl) {
            WObj c = g;
            uint32_t n = next_uid(s.world);
            c.set_u32("TemplateID", g.uid());
            c.set_u32("UID", n);
            c.set_u32("child", 0);
            wedit::append_child(s.world, uid, std::move(c));
            return;
        }
}

}  // namespace ma2

namespace ma2 {

// ----------------------------------------------------------------------------- player start vehicle
// What the player starts in is not on the player start: MARoot holds about 18 references (unnamed
// keys) to one MAPlayerConfigurationContainer template, which points (MoeEntityReference ->
// MAPlayerConfiguration -> CorporealTemplateUID) at the unit. Retail: "__Low Rez Avatar Container"
// = on foot (Mission 09b, most MP maps), "BattleArmor Container" (Mission 02/07/16, GoCommando,
// OnTheRocks), "<'Mech> Container" (Mission 05 Stiletto, 08 Mad Dog, 12/14/15 Blood Asp...),
// "RommelMk2 Container" (Mission 13a/b).
static const uint32_t K_CONTAINER = key_of("MAPlayerConfigurationContainer"), K_PCONFIG = key_of("MAPlayerConfiguration"),
                      K_ENTREF = key_of("MoeEntityReference");
// Single player reads MARoot.StartVehiclesGeneric (key 1F13DE84, research t14_start_vehicle); the other ~17 keys are the
// per-game-type slots multiplayer uses and may name something else (Mission 11a: Mad Dog x17, on foot in Generic).
static const uint32_t K_START_GENERIC = 0x1F13DE84u;

const std::vector<ScenarioIO::StartChoice>& ScenarioIO::start_choices() {
    if (!start_choices_.empty()) return start_choices_;
    WTree t;
    t.build(cat_.global());
    const auto& objs = cat_.global().objs;
    for (size_t i = 0; i < objs.size(); i++) {
        if (objs[i].cls != K_CONTAINER) continue;
        uint32_t cuid = objs[i].uid();
        std::string name = objs[i].description();
        StartChoice c;
        c.container = cuid;
        if (cuid == 123999) { c.label = "On foot (pilot)"; c.kind = 0; }
        else if (cuid == 124001) { c.label = "Battle Armor"; c.kind = 1; }
        else {
            // One reference to one configuration whose unit is on the disc; no cinema, lobby or
            // special variants.
            static const char* skip[] = {"Avatar", "Cinema", "Lobby", "NoBomb", "NearClip", "M16", "Kurita", "WOB", "Turret", "Elemental"};
            bool bad = false;
            for (const char* sk : skip) if (name.find(sk) != std::string::npos) bad = true;
            if (bad || t.kids[i].size() != 1) continue;
            const WObj& ref = objs[size_t(t.kids[i][0])];
            const WObj* cfg = ref.cls == K_ENTREF ? cat_.object(ref.u32("ReferenceUID")) : nullptr;
            const TemplateInfo* unit = cfg && cfg->cls == K_PCONFIG ? cat_.find(cfg->u32("CorporealTemplateUID")) : nullptr;
            if (!unit || unit->model.empty()) continue;
            c.unit = unit->uid;
            c.kind = unit->cls_name == "MAMech" ? 2 : 3;
            c.label = name.substr(0, name.rfind(" Container"));
            if (c.label == "RommelMk2") c.label = "Rommel Mk2 tank";
        }
        start_choices_.push_back(c);
    }
    std::sort(start_choices_.begin(), start_choices_.end(), [](const StartChoice& a, const StartChoice& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.label < b.label;
    });
    return start_choices_;
}

// Every unit template (and its player configuration) a start container MARoot references can hand the player (each game type's
// StartVehicles slot; single player reads StartVehiclesGeneric, which may differ, e.g. NearClip BA).
std::vector<uint32_t> ScenarioIO::start_units(const Scenario& s) {
    std::vector<uint32_t> out;
    int ri = find_class(s.world, K_ROOT);
    if (ri < 0) return out;
    std::set<uint32_t> containers;
    for (auto& p : s.world.objs[size_t(ri)].props) {
        if (p.data.size() != 4 || (p.type != PT_U32 && p.type != PT_I32)) continue;
        const WObj* t = cat_.object(uint32_t(p.as_int()));
        if (!t || t->cls != K_CONTAINER) continue;
        if (p.key == K_START_GENERIC && !s.multiplayer) { containers = {uint32_t(p.as_int())}; break; }  // the only slot a mission reads
        containers.insert(uint32_t(p.as_int()));
    }
    if (containers.empty()) return out;
    WTree t;
    t.build(cat_.global());
    const auto& objs = cat_.global().objs;
    for (size_t i = 0; i < objs.size(); i++) {
        if (!containers.count(objs[i].uid())) continue;
        for (int k : t.kids[i]) {
            const WObj& ref = objs[size_t(k)];
            const WObj* cfg = ref.cls == K_ENTREF ? cat_.object(ref.u32("ReferenceUID")) : nullptr;
            if (cfg && cfg->cls == K_PCONFIG && cfg->u32("CorporealTemplateUID")) {
                // The configuration too: it names the transition and abandoned models.
                for (uint32_t u : {cfg->uid(), cfg->u32("CorporealTemplateUID")})
                    if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
            }
        }
    }
    return out;
}

uint32_t ScenarioIO::start_container_raw(const Scenario& s) {
    int ri = find_class(s.world, K_ROOT);
    if (ri < 0) return 0;
    std::map<uint32_t, int> votes;
    for (auto& p : s.world.objs[size_t(ri)].props) {
        if (p.data.size() != 4 || (p.type != PT_U32 && p.type != PT_I32)) continue;
        const WObj* t = cat_.object(uint32_t(p.as_int()));
        if (!t || t->cls != K_CONTAINER) continue;
        if (p.key == K_START_GENERIC && !s.multiplayer) return uint32_t(p.as_int());
        votes[uint32_t(p.as_int())]++;
    }
    uint32_t best = 0;
    int n = 0;
    for (auto& [u, c] : votes) if (c > n) { best = u; n = c; }
    return best;
}

// The choice the Map tab shows: Missions 06a/06b/11a/13b point at "Avatar Ingame Container" (123726), the on-foot chain behind 123999.
uint32_t ScenarioIO::start_container(const Scenario& s) {
    uint32_t c = start_container_raw(s);
    return c == 123726 ? 123999 : c;
}

// The slots are StartVehicles<GameType> (key_of; all 17 resolved 2026-10-27): CheckIt 00D9BE1E, TeamSkirmish 2979F2D0,
// LMS 3E1C0123, SnatchIt 6D8E10EB, TeamDestruction 749BC5D7, Skirmish 8C10D7B3, TeamScoutMatch 989624DE, ScoutMatch 9D426D94,
// GiantKillers A06E7E42, Grinder BA4F078A, CTF C39D0BED, TakeIt D1F54F96, BaseWar D225CEF7, KickIt DC2FB1FC,
// Destruction E297B910, NotIt F2845C32, TeamLMS F705FE79. Retail on-foot maps still start Not It, LMS, Destruction and
// Grinder in Battle Armor (the value in most of the 61 worlds; NRC Not It): free-for-all players spread over the map with no
// rides near them otherwise fight on foot all match (b2_allmodes_fresh Not It: 11 pilots, 0 free vehicles).
static bool ba_when_on_foot(uint32_t key) {
    return key == 0xF2845C32u || key == 0x3E1C0123u || key == 0xE297B910u || key == 0xBA4F078Au;
}

void ScenarioIO::set_start_container(Scenario& s, uint32_t container) {
    int ri = find_class(s.world, K_ROOT);
    if (ri < 0) return;
    for (auto& p : s.world.objs[size_t(ri)].props) {
        if (p.data.size() != 4 || (p.type != PT_U32 && p.type != PT_I32)) continue;
        const WObj* t = cat_.object(uint32_t(p.as_int()));
        if (!t || t->cls != K_CONTAINER) continue;
        // Mission 02's Battle Armor start keeps one slot on the near-clip variant; keep that pairing.
        uint32_t v = container;
        if (container == 124001 && p.key == 0x1F13DE84u) v = 125894;
        if (s.multiplayer && container == 123999 && ba_when_on_foot(p.key)) v = 124001;
        p.set_int(v);
    }
    s.dirty = true;
}

std::vector<uint32_t> ScenarioIO::start_templates(const Scenario& s) {
    std::vector<uint32_t> out;
    uint32_t c = start_container_raw(s);
    WTree t;
    t.build(cat_.global());
    const auto& objs = cat_.global().objs;
    for (size_t i = 0; i < objs.size(); i++) {
        if (objs[i].uid() != c) continue;
        out.push_back(c);
        for (int k : t.kids[i]) {
            const WObj* cfg = objs[size_t(k)].cls == K_ENTREF ? cat_.object(objs[size_t(k)].u32("ReferenceUID")) : nullptr;
            if (cfg && cfg->cls == K_PCONFIG) {
                out.push_back(objs[size_t(k)].uid());
                out.push_back(cfg->uid());
                if (cfg->u32("CorporealTemplateUID")) out.push_back(cfg->u32("CorporealTemplateUID"));
                return out;
            }
        }
    }
    return out;
}

uint32_t ScenarioIO::start_unit_template(const Scenario& s) {
    auto v = start_templates(s);
    return v.size() == 4 ? v[3] : 0;
}

// Mission_05 "TR - Return from 3": the trigger enables spawners 141817 (Cougar), 126237 and 126240 (tanks), each
// a NULL corporeal > MASpawner (Enabled 0, TriggerEnableID = the trigger, MaxSpawners 1, MaxActiveSpawners 1,
// SpawnRate 0, SpawnDelay 0) > the unit. 157 of the 167 campaign spawners work this way.
uint32_t ScenarioIO::make_reinforcement(Scenario& s, uint32_t unit_uid) {
    Vec3 at;
    std::string name;
    {
        WTree t;
        t.build(s.world);
        int i = t.find(unit_uid);
        if (i < 0 || !get_pos(s.world.objs[size_t(i)], at)) return 0;
        name = s.world.objs[size_t(i)].description();
    }
    uint32_t holder = place_template(s, 32093, at, 0.f);
    if (!holder) return 0;
    uint32_t sp = add_part(s, K_SPAWNER, holder, &at);
    if (!sp) {
        wedit::remove_subtree(s.world, holder);
        return 0;
    }
    WTree t;
    t.build(s.world);
    WObj& h = s.world.objs[size_t(t.find(holder))];
    h.set_str("Description", "Reinforcements: " + name);
    WObj& so = s.world.objs[size_t(t.find(sp))];
    so.set_str("Description", "Spawner: " + name);
    so.set_u32("Enabled", 0);
    so.set_u32("TriggerEnableID", 0);
    so.set_u32("MaxSpawners", 1);
    so.set_u32("MaxActiveSpawners", 1);
    so.set_f32("SpawnRate", 0.f);
    so.set_f32("SpawnDelay", 0.f);
    wedit::reparent(s.world, unit_uid, sp);
    s.dirty = true;
    return sp;
}

}  // namespace ma2
