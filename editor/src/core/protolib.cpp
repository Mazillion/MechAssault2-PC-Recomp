// Part kinds: every class the retail worlds use, each with a prototype copied from a retail
// instance, so any object or part a retail level has can be added to a scenario the same way
// (Level tree > Add part), and any subtree can be duplicated. Nothing here is editor-only: an added
// part is the retail class with the retail fields, links reset to 0.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "cutscene.h"
#include "scenario.h"
#include "triggers.h"

namespace ma2 {

using namespace world;

// Classes that exist once per level (or are generated at save) and are not added by hand.
static bool singleton_class(const std::string& n) {
    static const std::set<std::string> s = {"MARoot", "MATerrain", "MATerrainInfo", "MAEnvironment", "MAPathDB", "MACultureSystem",
                                            "MACultureType", "MACultureGenerator", "MoeGroupSet", "MarkerInfo", "MoeEntity"};
    return s.count(n) > 0;
}

void ScenarioIO::build_part_kinds() {
    if (parts_ready_) return;
    parts_ready_ = true;
    const auto& names = gd_.names();
    struct Acc {
        PartKind k;
        bool have_tpl = false;
        std::map<uint32_t, int> parents;
    };
    std::map<uint32_t, Acc> acc;
    std::unordered_map<uint32_t, std::pair<int, int>> ref_stat;  // key -> (nonzero values, values naming a UID of the same world)
    // Maps in this order: missions first (their instances carry the AI/trigger/cinema structure), then multiplayer.
    std::vector<const MapInfo*> order;
    for (auto& m : gd_.maps()) if (!m.multiplayer) order.push_back(&m);
    for (auto& m : gd_.maps()) if (m.multiplayer) order.push_back(&m);
    for (const MapInfo* m : order) {
        Wdf w;
        if (!gd_.load_world(*m, w)) continue;
        WTree t;
        t.build(w);
        std::set<uint32_t> seen;
        for (size_t i = 0; i < w.objs.size(); i++) {
            const WObj& o = w.objs[i];
            Acc& a = acc[o.cls];
            a.k.cls = o.cls;
            a.k.count++;
            if (seen.insert(o.cls).second) a.k.worlds++;
            int p = t.parent_of[i];
            a.parents[p >= 0 ? w.objs[size_t(p)].cls : 0]++;
            bool has_tpl = o.template_id() != 0;
            if (a.k.proto.props.empty() || (has_tpl && !a.have_tpl)) {
                a.k.proto = o;
                a.have_tpl = has_tpl;
                a.k.positioned = position(o) != nullptr;
                a.k.sample_map = m->name;
            }
            for (auto& pr : o.props) {
                if (pr.data.size() != 4 || (pr.type != PT_U32 && pr.type != PT_I32)) continue;
                uint32_t v = uint32_t(pr.as_int());
                if (v < 16) continue;
                auto& st = ref_stat[pr.key];
                st.first++;
                if (t.find(v) >= 0) st.second++;
            }
        }
    }
    static const uint32_t K_UID = key_of("UID"), K_PARENT = key_of("parent"), K_CHILD = key_of("child"), K_SIBLING = key_of("sibling"),
                          K_TEMPLATE = key_of("TemplateID");
    std::unordered_set<uint32_t> refs;
    for (auto& [k, st] : ref_stat)
        if (st.second >= 2 && st.second * 10 >= st.first * 6 && k != K_UID && k != K_PARENT && k != K_CHILD && k != K_SIBLING && k != K_TEMPLATE)
            refs.insert(k);
    part_refs_ = refs;
    for (auto& [cls, a] : acc) {
        PartKind k = std::move(a.k);
        k.name = names.label(cls);
        if (singleton_class(k.name)) continue;
        WObj& o = k.proto;
        for (const char* n : {"parent", "child", "sibling", "UID"}) o.set_u32(n, 0);
        for (auto& p : o.props)
            if (refs.count(p.key)) p.set_int(0);
        // Run-time state of the retail instance.
        if (o.find("TriggerFired")) o.set_i32("TriggerFired", 0);
        if (o.find("NumberOfTimesActivated")) o.set_u32("NumberOfTimesActivated", 0);
        if (o.find("TriggerStarted")) o.set_i32("TriggerStarted", 0);
        std::vector<std::pair<int, uint32_t>> ps;
        for (auto& [pc, n] : a.parents) ps.push_back({n, pc});
        std::sort(ps.begin(), ps.end(), [](auto& x, auto& y) { return x.first > y.first; });
        for (auto& [n, pc] : ps) k.parent_classes.push_back({pc, n});
        parts_.push_back(std::move(k));
    }
    std::sort(parts_.begin(), parts_.end(), [](const PartKind& a, const PartKind& b) { return a.name < b.name; });
}

const std::vector<ScenarioIO::PartKind>& ScenarioIO::part_kinds() {
    build_part_kinds();
    return parts_;
}

const ScenarioIO::PartKind* ScenarioIO::find_part_kind(uint32_t cls) {
    build_part_kinds();
    for (auto& k : parts_) if (k.cls == cls) return &k;
    return nullptr;
}

// Spawner run-time ids (0xF80000xx) must stay unique in the world.
static void renumber_spawners(const Wdf& w, std::vector<WObj>& objs) {
    static const uint32_t K_SPAWNER = key_of("MASpawner");
    uint32_t max_id = 0xF8000000u;
    for (auto& o : w.objs)
        if (o.cls == K_SPAWNER) max_id = std::max({max_id, o.u32("InitialID"), o.u32("InitialSecondID")});
    for (auto& o : objs)
        if (o.cls == K_SPAWNER) {
            if (o.find("InitialID")) o.set_u32("InitialID", ++max_id);
            if (o.find("InitialSecondID")) o.set_u32("InitialSecondID", ++max_id);
        }
}

uint32_t ScenarioIO::add_part(Scenario& s, uint32_t cls, uint32_t parent_uid, const Vec3* at) {
    const PartKind* k = find_part_kind(cls);
    if (!k) return 0;
    Wdf& w = s.world;
    WTree t;
    t.build(w);
    if (t.find(parent_uid) < 0) return 0;
    WObj o = k->proto;
    uint32_t uid = next_uid(w);
    o.set_u32("UID", uid);
    std::string plain = k->name.size() > 2 && k->name[0] == 'M' && k->name[1] == 'A' ? k->name.substr(2) : k->name;
    if (o.find("Description")) o.set_str("Description", "New " + plain);
    // Some retail instances are tiny (a 1 m high box): a part you add should be usable as it lands.
    if (o.cls == key_of("MATriggerCuboidalVolume")) {
        if (o.f32("VolumeWidth") < 10) o.set_f32("VolumeWidth", 40);
        if (o.f32("VolumeLength") < 10) o.set_f32("VolumeLength", 40);
        if (o.f32("VolumeHeight") < 10) o.set_f32("VolumeHeight", 40);
    } else if (o.cls == key_of("MATriggerRadius") && o.f32("EncounterRadius") < 5) {
        o.set_f32("EncounterRadius", 30);
    }
    // A light you add should look like the level's street lamps: retail M03 (18 of 21 lights) uses LightMap 1 with a 20 m
    // ground pool in the same colour as the light (LightMapColor = LightColor) and 60 m reach for the objects around it.
    if (o.cls == key_of("MAPointLight")) {
        if (o.find("LightMap")) o.set_u32("LightMap", 1);
        if (o.find("LightMapRadius") && o.f32("LightMapRadius") < 5) o.set_f32("LightMapRadius", 20);
        if (o.find("LightMapColor") && o.find("LightColor")) o.set_u32("LightMapColor", o.u32("LightColor"));
        if (o.find("LightRadius") && o.f32("LightRadius") < 30) o.set_f32("LightRadius", 60);
        if (o.find("LightOn")) o.set_u32("LightOn", 1);
    }
    if (position(o)) {
        Vec3 p = at ? *at : Vec3{};
        float g = at ? s.terrain.height_at(p.x, p.z) : 0.f;
        float y = g;
        if (o.cls == key_of("MATriggerCuboidalVolume")) y = g + o.f32("VolumeHeight", 60.f) * 0.5f;
        else if (o.cls == key_of("MAPointLight")) y = g + 6.f;
        else if (o.cls == key_of("MAFreeCamera")) y = g + 25.f;
        else if (o.cls == key_of("MAWaypoint") || o.cls == key_of("MADummyObj")) y = g;
        set_pos(o, {p.x, y, p.z});
    }
    std::vector<WObj> one;
    one.push_back(std::move(o));
    renumber_spawners(w, one);
    wedit::append_child(w, parent_uid, std::move(one.front()));
    s.dirty = true;
    return uid;
}

uint32_t ScenarioIO::duplicate_subtree(Scenario& s, uint32_t uid, float offset) {
    Wdf& w = s.world;
    WTree t;
    t.build(w);
    int i = t.find(uid);
    if (i < 0) return 0;
    int p = t.parent_of[size_t(i)];
    if (p < 0) return 0;  // the root cannot be duplicated
    uint32_t parent_uid = w.objs[size_t(p)].uid();
    std::unordered_set<uint32_t> keep = cat_.ref_keys();
    keep.erase(key_of("ReferenceUID"));
    keep.insert(key_of("TemplateID"));
    uint32_t next = next_uid(w);
    auto clone = wedit::clone_subtree(w, uid, next, &keep);
    if (clone.empty()) return 0;
    renumber_spawners(w, clone);
    uint32_t root = clone.front().uid();
    // Members of an entity set stay members: the set lists the copy too.
    const auto& names = gd_.names();
    static const uint32_t K_SET = key_of("MoeEntitySet");
    std::vector<std::pair<uint32_t, uint32_t>> joins;  // (set uid, member uid)
    for (auto& o : clone)
        for (auto& pr : o.props) {
            if (pr.data.size() != 4 || (pr.type != PT_U32 && pr.type != PT_I32)) continue;
            const std::string* n = names.name(pr.key);
            if (!n || n->compare(0, 9, "SetMember") != 0) continue;
            uint32_t su = uint32_t(pr.as_int());
            int si = su ? t.find(su) : -1;
            if (si >= 0 && w.objs[size_t(si)].cls == K_SET) joins.push_back({su, o.uid()});
        }
    if (clone.front().find("Description")) clone.front().set_str("Description", ma2::copy_name(clone.front().description()));
    wedit::insert_subtree(w, parent_uid, std::move(clone));
    for (auto& [su, mu] : joins)
        for (auto& o : w.objs)
            if (o.uid() == su) {
                int n = o.i32("MoeEntitySetCount");
                o.set_u32("MoeEntitySetMember" + std::to_string(n), mu);
                o.set_i32("MoeEntitySetCount", n + 1);
                break;
            }
    if (offset != 0.f) {
        WTree t2;
        t2.build(w);
        std::vector<int> sub;
        t2.subtree(t2.find(root), sub);
        for (int k : sub) {
            Vec3 q;
            WObj& o = w.objs[size_t(k)];
            if (get_pos(o, q)) set_pos(o, {q.x + offset, s.terrain.height_at(q.x + offset, q.z + offset) + (q.y - s.terrain.height_at(q.x, q.z)), q.z + offset});
        }
    }
    s.dirty = true;
    return root;
}


bool ScenarioIO::is_object_link(uint32_t key) {
    build_part_kinds();
    return part_refs_.count(key) > 0;
}

// ----------------------------------------------------------------------------- cutscenes

// Relinks child/sibling of a pre-order subtree after objects were dropped from it.
static void relink(std::vector<WObj>& v) {
    std::unordered_map<uint32_t, std::vector<size_t>> kids;
    for (size_t i = 1; i < v.size(); i++) kids[v[i].parent()].push_back(i);
    for (auto& o : v) o.set_u32("child", 0);
    for (auto& [par, ks] : kids) {
        for (size_t k = 0; k < ks.size(); k++) v[ks[k]].set_u32("sibling", k + 1 < ks.size() ? v[ks[k + 1]].uid() : 0);
        for (auto& o : v) if (o.uid() == par) { o.set_u32("child", v[ks[0]].uid()); break; }
    }
}

uint32_t ScenarioIO::add_cutscene(Scenario& s, Vec3 at, const std::string& name, const float* behind, bool detach_flyby) {
    part_kinds();
    const Wdf* src = retail_world("Mission_05");
    if (!src) return 0;
    uint32_t K_CINEMA = key_of("MACinema"), K_CTRL = key_of("MAFreeCameraController"), K_PROP = key_of("MACameraProperty"),
             K_TRIG = key_of("MATrigger");
    uint32_t cin = 0;
    for (auto& o : src->objs) if (o.cls == K_CINEMA && o.description() == "IcarusGoBoom Cutscene") { cin = o.uid(); break; }
    if (!cin) return 0;
    std::unordered_set<uint32_t> keep = cat_.ref_keys();
    keep.erase(key_of("ReferenceUID"));
    keep.insert(key_of("TemplateID"));
    uint32_t grp_next = next_uid(s.world);
    uint32_t grp = ensure_group(s.world, terrain_uid(s.world), "Cutscenes", grp_next);
    uint32_t next = next_uid(s.world);
    auto clone = wedit::clone_subtree(*src, cin, next, &keep);
    if (clone.empty()) return 0;
    // The retail start/end triggers carry that cutscene's own actions: the copy keeps the triggers only.
    std::unordered_set<uint32_t> own, drop;
    for (auto& o : clone) own.insert(o.uid());
    for (auto& o : clone) {
        // links to objects outside the copy (that level's units) would dangle
        for (auto& p : o.props)
            if (part_refs_.count(p.key) && p.data.size() == 4 && p.as_int() && !own.count(uint32_t(p.as_int()))) p.set_int(0);
    }
    for (auto& o : clone)
        if (o.cls != K_TRIG && gd_.names().label(o.cls).compare(0, 8, "MAAction") == 0 && own.count(o.parent())) drop.insert(o.uid());
    {
        std::vector<WObj> kept;
        for (auto& o : clone) if (!drop.count(o.uid())) kept.push_back(std::move(o));
        clone = std::move(kept);
        relink(clone);
    }
    // Where it stands: the focus marker goes to `at` (2 m above the ground), the rest keeps its offsets.
    Vec3 ref{};
    bool have_ref = false;
    for (auto& o : clone)
        if (o.cls == K_PROP && o.i32("type") == 3 && o.u32("focusUID")) {
            for (auto& m : clone) if (m.uid() == o.u32("focusUID") && get_pos(m, ref)) have_ref = true;
        }
    if (!have_ref) for (auto& o : clone) if (o.cls != K_CINEMA && get_pos(o, ref)) { have_ref = true; break; }
    Vec3 to{at.x, s.terrain.height_at(at.x, at.z) + 2.f, at.z};
    for (auto& o : clone) {
        Vec3 p;
        if (o.cls == K_CINEMA) { if (position(o)) set_pos(o, at); continue; }
        if (get_pos(o, p)) set_pos(o, {p.x - ref.x + to.x, p.y - ref.y + to.y, p.z - ref.z + to.z});
    }
    if (behind) {
        // The eye path (eyeUID, its waypoint group m_wptGroupUID and their points): turn the whole layout
        // about the focus so the path's middle sits behind the heading; heights follow the ground at the new spots.
        std::unordered_set<uint32_t> eye;
        for (auto& o : clone) if (o.cls == K_PROP) if (uint32_t e = o.u32("eyeUID")) eye.insert(e);
        for (size_t n = 0; n != eye.size();) {
            n = eye.size();
            for (auto& o : clone) {
                if (eye.count(o.parent())) eye.insert(o.uid());
                if (eye.count(o.uid())) if (uint32_t g = o.u32("m_wptGroupUID")) eye.insert(g);
            }
        }
        float cx = 0, cz = 0;
        int cn = 0;
        for (auto& o : clone) {
            Vec3 p;
            if (eye.count(o.uid()) && get_pos(o, p)) { cx += p.x - to.x; cz += p.z - to.z; cn++; }
        }
        if (cn && (cx != 0 || cz != 0)) {
            float r = (*behind + 3.14159265f) - std::atan2(cx, cz), c = std::cos(r), sn = std::sin(r);
            for (auto& o : clone) {
                Vec3 p;
                if (o.cls == K_CINEMA || !get_pos(o, p)) continue;
                float dx = p.x - to.x, dz = p.z - to.z;
                // x' = x cos r + z sin r, z' = -x sin r + z cos r turns the (sin a, cos a) direction to a + r
                float nx = to.x + dx * c + dz * sn, nz = to.z - dx * sn + dz * c;
                float up = p.y - s.terrain.height_at(p.x, p.z);
                set_pos(o, {nx, s.terrain.height_at(nx, nz) + up, nz});
            }
        }
    }
    clone.front().set_str("Description", name);
    // Name the parts after the new cutscene ("Intro - camera"), not the retail template's ("IcarusCam - 01").
    {
        std::unordered_map<uint32_t, std::string> role;
        for (auto& o : clone) {
            if (o.cls == K_CTRL) {
                role[o.uid()] = "shot 1";
                if (uint32_t c = o.u32("Camera")) role[c] = "camera";
                if (uint32_t c = o.u32("triggerInit")) role[c] = "starts";
                if (uint32_t c = o.u32("triggerExit")) role[c] = "ends";
            }
            if (o.cls == K_PROP) {
                if (uint32_t e = o.u32("eyeUID")) role.emplace(e, "flight path");
                if (uint32_t f = o.u32("focusUID")) role.emplace(f, "looks at");
            }
        }
        int pt = 0;
        for (auto& o : clone) {
            if (&o == &clone.front() || !o.find("Description")) continue;
            auto it = role.find(o.uid());
            std::string d = o.description();
            if (it != role.end()) o.set_str("Description", name + " - " + it->second);
            else if (d.find("Icarus") != std::string::npos) o.set_str("Description", name + " - part " + std::to_string(++pt));
        }
    }
    uint32_t root = clone.front().uid();
    wedit::insert_subtree(s.world, grp, std::move(clone));
    // A fresh cutscene is not tied to the template's retail flyby marker (the game would fly from it first), so its shot 1
    // plays from its own points and the Check row for old maps does not list it.
    if (detach_flyby)
        for (const ShotInfo& sh : cutscene_shots(s.world, root)) cutscene_play_from_own_view(s, sh.ctrl);
    s.dirty = true;
    return root;
}

// The shot's pieces, as retail ties them: controller (camera, start/end triggers, its steps' eye path
// and focus object); the path's waypoint group sits under some owner (flyby marker or the camera).
// `sub` = the cinema's subtree (only its own focus objects count).
static std::vector<uint32_t> shot_roots(const Wdf& w, const WTree& t, int ctrl, const std::vector<int>& sub) {
    static const uint32_t K_PROP = key_of("MACameraProperty"), K_GROUP_ = key_of("MAGroup"), K_CINEMA = key_of("MACinema"), K_PATH = key_of("MAPath");
    std::vector<uint32_t> roots;
    auto add = [&](uint32_t uid) {
        if (!uid) return;
        int i = t.find(uid);
        if (i < 0) return;
        for (uint32_t r : roots) {  // skip what is inside an already chosen subtree, and replace subtrees it contains
            int ri = t.find(r);
            std::vector<int> rs;
            t.subtree(ri, rs);
            if (std::find(rs.begin(), rs.end(), i) != rs.end()) return;
        }
        std::vector<int> mine;
        t.subtree(i, mine);
        roots.erase(std::remove_if(roots.begin(), roots.end(), [&](uint32_t r) {
            return std::find(mine.begin(), mine.end(), t.find(r)) != mine.end(); }), roots.end());
        roots.push_back(uid);
    };
    const WObj& c = w.objs[size_t(ctrl)];
    add(c.uid());
    add(c.u32("Camera"));
    add(c.u32("triggerInit"));
    add(c.u32("triggerExit"));
    std::vector<int> csub;
    t.subtree(ctrl, csub);
    for (int i : csub) {
        const WObj& pr = w.objs[size_t(i)];
        if (pr.cls != K_PROP) continue;
        if (uint32_t e = pr.u32("eyeUID")) {
            int pi = t.find(e);
            if (pi >= 0 && w.objs[size_t(pi)].cls == K_PATH) {
                add(e);
                uint32_t g = w.objs[size_t(pi)].u32("m_wptGroupUID");
                int gi = t.find(g);
                if (gi >= 0) {
                    int par = t.parent_of[size_t(gi)];
                    add(par >= 0 && w.objs[size_t(par)].cls != K_GROUP_ && w.objs[size_t(par)].cls != K_CINEMA ? w.objs[size_t(par)].uid() : g);
                }
            } else add(e);
        }
        if (uint32_t f = pr.u32("focusUID")) {
            int fi = t.find(f);
            if (fi >= 0 && std::find(sub.begin(), sub.end(), fi) != sub.end()) add(f);  // only the cinema's own objects
        }
    }
    return roots;
}

uint32_t ScenarioIO::add_cutscene_shot(Scenario& s, uint32_t cinema_uid, const Vec3* eye, const Vec3* look, float len) {
    Wdf& w = s.world;
    static const uint32_t K_CTRL = key_of("MAFreeCameraController"), K_CINEMA = key_of("MACinema"), K_PATH = key_of("MAPath");
    WTree t;
    t.build(w);
    int ci = t.find(cinema_uid);
    if (ci < 0 || w.objs[size_t(ci)].cls != K_CINEMA) return 0;
    std::vector<int> sub;
    t.subtree(ci, sub);
    int ctrl = -1;
    for (int i : sub) if (w.objs[size_t(i)].cls == K_CTRL) ctrl = i;  // the last shot
    if (ctrl < 0) return 0;
    std::vector<uint32_t> roots = shot_roots(w, t, ctrl, sub);
    const WObj& c = w.objs[size_t(ctrl)];
    const uint32_t old_init = c.u32("triggerInit"), old_exit = c.u32("triggerExit");
    std::unordered_set<uint32_t> keep = cat_.ref_keys();
    keep.erase(key_of("ReferenceUID"));
    keep.insert(key_of("TemplateID"));
    uint32_t next = next_uid(w);
    std::vector<std::vector<WObj>> copies;
    std::unordered_map<uint32_t, uint32_t> all;
    std::vector<uint32_t> parents;
    for (uint32_t r : roots) {
        std::unordered_map<uint32_t, uint32_t> m;
        copies.push_back(wedit::clone_subtree(w, r, next, &keep, &m));
        all.insert(m.begin(), m.end());
        int ri = t.find(r);
        parents.push_back(w.objs[size_t(t.parent_of[size_t(ri)])].uid());
    }
    uint32_t new_ctrl = all.count(c.uid()) ? all[c.uid()] : 0;
    // Nothing the last shot carries is copied: the new shot's start / end triggers are cloned empty.
    {
        std::unordered_set<uint32_t> trig;
        for (uint32_t u : {old_init, old_exit}) if (all.count(u)) trig.insert(all[u]);
        for (auto& v : copies) {
            std::unordered_set<uint32_t> drop;
            for (size_t i = 1; i < v.size(); i++)
                if (trig.count(v[i].parent()) && is_trigger_action(gd_.names(), v[i])) drop.insert(v[i].uid());
            for (bool more = !drop.empty(); more;) {
                more = false;
                for (size_t i = 1; i < v.size(); i++) if (!drop.count(v[i].uid()) && drop.count(v[i].parent())) { drop.insert(v[i].uid()); more = true; }
            }
            if (drop.empty()) continue;
            std::vector<WObj> kept;
            for (auto& o : v) if (!drop.count(o.uid())) kept.push_back(std::move(o));
            v = std::move(kept);
            relink(v);
        }
    }
    // With a given eye the view is written below; without one the pieces move 25 m +X of the last shot (the old behaviour).
    const float dx = eye ? 0.f : 25.f, dz = 0.f;
    int shot_no = 1;
    for (int i : sub) shot_no += w.objs[size_t(i)].cls == K_CTRL;
    const std::string tag = " (shot " + std::to_string(shot_no) + ")";
    for (auto& v : copies)
        for (auto& o : v) {
            for (auto& p : o.props)  // links between the shot's pieces point at the copies
                if (p.data.size() == 4 && (p.type == PT_U32 || p.type == PT_I32) && !keep.count(p.key) && p.as_int()) {
                    auto it = all.find(uint32_t(p.as_int()));
                    if (it != all.end()) p.set_int(it->second);
                }
            Vec3 q;
            if (dx != 0.f && get_pos(o, q)) set_pos(o, {q.x + dx, q.y + s.terrain.height_at(q.x + dx, q.z + dz) - s.terrain.height_at(q.x, q.z), q.z + dz});
            // Names say which shot they belong to: "Intro - camera (shot 2)", "Intro - shot 2" (no bare " 2").
            std::string d = o.description();
            if (d.empty() || !o.find("Description")) continue;
            if (size_t k = d.rfind(" (shot "); k != std::string::npos && d.back() == ')') d.resize(k);
            if (size_t k = d.rfind(" - shot "); k != std::string::npos && d.find_first_not_of("0123456789", k + 8) == std::string::npos)
                d = d.substr(0, k) + " - shot " + std::to_string(shot_no);
            else
                d += tag;
            o.set_str("Description", d);
        }
    for (size_t i = 0; i < copies.size(); i++) wedit::insert_subtree(w, parents[i], std::move(copies[i]));
    // The cutscene's end actions belong to the LAST shot (2.1): they move from the old last shot's end trigger to the new one's.
    if (all.count(old_exit)) cutscene_move_end_actions(s, gd_.names(), old_exit, all[old_exit]);
    if (new_ctrl) {
        set_shot_length(s, new_ctrl, len);
        if (eye) set_shot_view(s, new_ctrl, 0, *eye, look ? *look : Vec3{}, look != nullptr, this);
        // The move spans the whole shot: the copied path may carry the last shot's own duration.
        for (auto& sh : cutscene_shots(w, cinema_uid))
            if (sh.ctrl == new_ctrl)
                if (WObj* p = sh.eye_path ? object_by_uid(w, sh.eye_path) : nullptr; p && p->cls == K_PATH) p->set_f32("duration", std::clamp(len, kShotMin, kShotMax));
    }
    s.dirty = true;
    return new_ctrl;
}

uint32_t ScenarioIO::ensure_shot_eye_path(Scenario& s, uint32_t ctrl_uid) {
    Wdf& w = s.world;
    static const uint32_t K_CTRL = key_of("MAFreeCameraController"), K_PROP = key_of("MACameraProperty"), K_PATH = key_of("MAPath"),
                          K_WPG = key_of("MAWaypointGroup"), K_CINEMA = key_of("MACinema"), K_GROUP_ = key_of("MAGroup");
    WTree t;
    t.build(w);
    int ci = t.find(ctrl_uid);
    if (ci < 0 || w.objs[size_t(ci)].cls != K_CTRL) return 0;
    int step = -1;
    for (int k : t.kids[size_t(ci)]) if (w.objs[size_t(k)].cls == K_PROP && w.objs[size_t(k)].i32("type") == 2) { step = k; break; }
    if (step < 0) return 0;
    const uint32_t eye = w.objs[size_t(step)].u32("eyeUID");
    int pi = t.find(eye);
    if (pi >= 0 && w.objs[size_t(pi)].cls != K_PATH) return eye;  // a shot that flies to one object: nothing to rebuild
    int gi = pi >= 0 ? t.find(w.objs[size_t(pi)].u32("m_wptGroupUID")) : -1;
    if (gi >= 0 && w.objs[size_t(gi)].cls == K_WPG && !t.kids[size_t(gi)].empty()) return eye;  // a path with points: fine as it is
    // The template: Mission_05's cutscene, its first shot's eye path and the owner of the waypoint group.
    part_kinds();
    const Wdf* src = retail_world("Mission_05");
    if (!src) return 0;
    WTree st;
    st.build(*src);
    uint32_t tpl_path = 0;
    for (auto& o : src->objs)
        if (o.cls == K_CINEMA && o.description() == "IcarusGoBoom Cutscene") {
            std::vector<int> sb;
            st.subtree(st.find(o.uid()), sb);
            for (int k : sb)
                if (src->objs[size_t(k)].cls == K_CTRL)
                    for (int m : st.kids[size_t(k)])
                        if (src->objs[size_t(m)].cls == K_PROP && src->objs[size_t(m)].i32("type") == 2 && !tpl_path) tpl_path = src->objs[size_t(m)].u32("eyeUID");
            break;
        }
    int tp = st.find(tpl_path);
    if (tp < 0) return 0;
    const uint32_t tpl_group = src->objs[size_t(tp)].u32("m_wptGroupUID");
    int tg = st.find(tpl_group);
    if (tg < 0) return 0;
    int owner = st.parent_of[size_t(tg)];
    const uint32_t tpl_owner = owner >= 0 && src->objs[size_t(owner)].cls != K_GROUP_ && src->objs[size_t(owner)].cls != K_CINEMA ? src->objs[size_t(owner)].uid() : tpl_group;
    // The cinema the shot is in, its camera (where the new points start) and the groups the pieces go under.
    int cin = ci;
    while (cin >= 0 && w.objs[size_t(cin)].cls != K_CINEMA) cin = t.parent_of[size_t(cin)];
    if (cin < 0) return 0;
    auto group_named = [&](const char* name) -> uint32_t {
        for (int k : t.kids[size_t(cin)]) if (w.objs[size_t(k)].cls == K_GROUP_ && w.objs[size_t(k)].description() == name) return w.objs[size_t(k)].uid();
        return w.objs[size_t(cin)].uid();
    };
    Vec3 base{};
    const WObj* cam = object_by_uid(w, w.objs[size_t(ci)].u32("Camera"));
    if (!cam || !get_pos(*cam, base)) get_pos(w.objs[size_t(cin)], base);
    std::unordered_set<uint32_t> keep = cat_.ref_keys();
    keep.erase(key_of("ReferenceUID"));
    keep.insert(key_of("TemplateID"));
    uint32_t next = next_uid(w);
    std::unordered_map<uint32_t, uint32_t> m_owner;
    std::vector<WObj> own = wedit::clone_subtree(*src, tpl_owner, next, &keep, &m_owner);
    if (own.empty() || !m_owner.count(tpl_group)) return 0;
    const uint32_t new_group = m_owner[tpl_group];
    Vec3 first{};
    bool have_first = false;
    for (auto& o : own)  // the first waypoint of the group goes to the camera; the rest of the shape follows
        if (!have_first && o.parent() == new_group && get_pos(o, first)) have_first = true;
    if (!have_first) first = base;
    for (auto& o : own) {
        Vec3 q;
        if (get_pos(o, q)) set_pos(o, {q.x - first.x + base.x, q.y - first.y + base.y, q.z - first.z + base.z});
        if (o.uid() == new_group) o.set_str("Description", "Camera path points");
    }
    {   // two points are enough to start from (the first and the last of the template); the caller's view or Draw flight path shapes them
        std::vector<uint32_t> wp;
        for (auto& o : own) if (o.parent() == new_group) wp.push_back(o.uid());
        if (wp.size() > 2) {
            std::unordered_set<uint32_t> drop(wp.begin() + 1, wp.end() - 1);
            std::vector<WObj> kept;
            for (auto& o : own) if (!drop.count(o.uid())) kept.push_back(std::move(o));
            own = std::move(kept);
            relink(own);
        }
        for (auto& o : own) if (o.uid() == new_group && !wp.empty()) o.set_u32("WaypointID", wp.back());
    }
    const uint32_t objects_grp = group_named("Objects");
    if (pi >= 0) {  // the path object is still there: only its points were lost
        wedit::insert_subtree(w, objects_grp, std::move(own));
        if (WObj* p = object_by_uid(w, eye)) p->set_u32("m_wptGroupUID", new_group);
        detach_path_group(w, eye);  // The template's flyby marker would be the game's first point
        s.dirty = true;
        return eye;
    }
    std::vector<WObj> pv = wedit::clone_subtree(*src, tpl_path, next, &keep);
    if (pv.empty()) return 0;
    const uint32_t new_path = pv.front().uid();
    pv.front().set_u32("m_wptGroupUID", new_group);
    pv.front().set_str("Description", w.objs[size_t(cin)].description() + " - flight path");
    pv.front().set_f32("duration", 8.f);
    const uint32_t step_uid = w.objs[size_t(step)].uid();
    wedit::insert_subtree(w, objects_grp, std::move(own));
    wedit::insert_subtree(w, group_named("Paths"), std::move(pv));
    if (WObj* stp = object_by_uid(w, step_uid)) stp->set_u32("eyeUID", new_path);
    detach_path_group(w, new_path);  // The template's flyby marker would be the game's first point
    s.dirty = true;
    return new_path;
}

int ScenarioIO::remove_cutscene_shot(Scenario& s, uint32_t controller_uid) {
    Wdf& w = s.world;
    static const uint32_t K_CTRL = key_of("MAFreeCameraController"), K_CINEMA = key_of("MACinema");
    WTree t;
    t.build(w);
    int ci = t.find(controller_uid);
    if (ci < 0 || w.objs[size_t(ci)].cls != K_CTRL) return 0;
    int cin = ci;
    while (cin >= 0 && w.objs[size_t(cin)].cls != K_CINEMA) cin = t.parent_of[size_t(cin)];
    if (cin < 0) return 0;
    std::vector<int> sub;
    t.subtree(cin, sub);
    std::unordered_set<uint32_t> others;  // pieces another shot of the same cinema also uses stay
    for (int i : sub)
        if (i != ci && w.objs[size_t(i)].cls == K_CTRL)
            for (uint32_t r : shot_roots(w, t, i, sub)) others.insert(r);
    int before = int(w.objs.size());
    {  // the cutscene's end actions belong to the last shot: when the last one goes they move to the new last
        std::vector<ShotInfo> shots = cutscene_shots(w, w.objs[size_t(cin)].uid());
        if (shots.size() >= 2 && shots.back().ctrl == controller_uid) cutscene_move_end_actions(s, gd_.names(), shots.back().exit, shots[shots.size() - 2].exit);
    }
    const WObj* ctrl_obj = object_by_uid(w, controller_uid);
    const uint32_t init = ctrl_obj ? ctrl_obj->u32("triggerInit") : 0, exit = ctrl_obj ? ctrl_obj->u32("triggerExit") : 0;
    std::vector<uint32_t> cues;  // The shot's cue timers and the orders waiting for its triggers go with it (they live outside the shot's objects)
    for (auto& c : cutscene_cue_timers(w, init)) cues.push_back(c.first);
    for (uint32_t r : shot_roots(w, t, ci, sub))
        if (!others.count(r) && object_by_uid(w, r)) wedit::remove_subtree(w, r);
    cutscene_drop_shot_triggers(s, others.count(init) ? 0 : init, others.count(exit) ? 0 : exit, cues);
    s.dirty = true;
    return before - int(w.objs.size());
}

}  // namespace ma2
