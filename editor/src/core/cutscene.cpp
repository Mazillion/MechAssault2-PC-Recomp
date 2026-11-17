#include "cutscene.h"

#include <algorithm>
#include <cmath>

#include "triggers.h"

namespace ma2 {

using namespace world;

namespace {

const uint32_t K_CINEMA = key_of("MACinema"), K_CTRL = key_of("MAFreeCameraController"), K_PROP = key_of("MACameraProperty"),
               K_PATH = key_of("MAPath"), K_WPG = key_of("MAWaypointGroup"), K_MAGROUP = key_of("MAGroup"), K_DUMMY = key_of("MADummyObj");
const uint32_t kFovKey = 0xD186B2CAu;  // MAFreeCamera "Field of view" (degrees)

// The points of an eye path (MAPath > its waypoint group > the waypoints, after the group's owner when it has a place:
// path_owner_point), or one object's own spot.
std::vector<Vec3> path_points(const Wdf& w, const WTree& t, uint32_t uid, float* dur) {
    std::vector<Vec3> v;
    int k = uid ? t.find(uid) : -1;
    if (k < 0) return v;
    if (w.objs[size_t(k)].cls == K_PATH) {
        if (dur) *dur = w.objs[size_t(k)].f32("duration");
        k = t.find(w.objs[size_t(k)].u32("m_wptGroupUID"));
    }
    if (k < 0) return v;
    if (w.objs[size_t(k)].cls == K_WPG) {
        if (Vec3 o; path_owner_point(w, t, k, o)) v.push_back(o);
        for (int c : t.kids[size_t(k)]) if (Vec3 p; get_pos(w.objs[size_t(c)], p)) v.push_back(p);
    } else if (Vec3 p; get_pos(w.objs[size_t(k)], p)) v.push_back(p);
    return v;
}

// The shot's type-3 step Offset (the game looks at the focus object's spot + Offset).
Vec3 focus_offset(const Wdf& w, const WTree& t, int ctrl) {
    for (int m : t.kids[size_t(ctrl)]) {
        const WObj& st = w.objs[size_t(m)];
        if (st.cls == K_PROP && st.i32("type") == 3) return st.find("Offset") ? st.vec3("Offset") : Vec3{};
    }
    return {};
}

// Zeroes the shot's focus Offset so the marker is the look point itself.
void zero_focus_offset(Wdf& w, uint32_t ctrl) {
    for (auto& o : w.objs)
        if (o.cls == K_PROP && o.parent() == ctrl && o.i32("type") == 3)
            if (WProp* p = o.find(key_of("Offset")); p && p->type == PT_VEC3) p->set_vec3({});
}

float span(const std::vector<Vec3>& v) {
    float d = 0;
    for (size_t i = 1; i < v.size(); i++) d += std::sqrt((v[i].x - v[i - 1].x) * (v[i].x - v[i - 1].x) + (v[i].y - v[i - 1].y) * (v[i].y - v[i - 1].y) +
                                                         (v[i].z - v[i - 1].z) * (v[i].z - v[i - 1].z));
    return d;
}

// The uids of an eye path's waypoints in order (empty when the path or its group is gone).
std::vector<uint32_t> waypoint_uids(const Wdf& w, uint32_t path, uint32_t* group = nullptr) {
    std::vector<uint32_t> out;
    WTree t;
    t.build(w);
    int pi = t.find(path);
    if (pi < 0 || w.objs[size_t(pi)].cls != K_PATH) return out;
    uint32_t g = w.objs[size_t(pi)].u32("m_wptGroupUID");
    int gi = t.find(g);
    if (gi < 0 || w.objs[size_t(gi)].cls != K_WPG) return out;
    if (group) *group = g;
    for (int k : t.kids[size_t(gi)]) out.push_back(w.objs[size_t(k)].uid());
    return out;
}

}  // namespace

bool path_owner_point(const Wdf& w, const WTree& t, int group, Vec3& at) {
    if (group < 0 || size_t(group) >= t.parent_of.size()) return false;
    const int p = t.parent_of[size_t(group)];
    if (p < 0) return false;
    const WObj& o = w.objs[size_t(p)];
    if (o.cls == K_MAGROUP || o.cls == K_CINEMA) return false;
    return get_pos(o, at);
}

bool detach_path_group(Wdf& w, uint32_t path) {
    WTree t;
    t.build(w);
    const int pi = t.find(path);
    if (pi < 0 || w.objs[size_t(pi)].cls != K_PATH) return false;
    const uint32_t g = w.objs[size_t(pi)].u32("m_wptGroupUID");
    const int gi = t.find(g);
    Vec3 at;
    if (gi < 0 || w.objs[size_t(gi)].cls != K_WPG || !path_owner_point(w, t, gi, at)) return false;
    // Where it goes: the cinema's "Paths" group, else the plain group the path object sits in.
    uint32_t dest = 0;
    int cin = pi;
    while (cin >= 0 && w.objs[size_t(cin)].cls != K_CINEMA) cin = t.parent_of[size_t(cin)];
    if (cin >= 0)
        for (int k : t.kids[size_t(cin)])
            if (w.objs[size_t(k)].cls == K_MAGROUP && w.objs[size_t(k)].description() == "Paths") { dest = w.objs[size_t(k)].uid(); break; }
    if (!dest) {
        const int pp = t.parent_of[size_t(pi)];
        if (pp >= 0 && w.objs[size_t(pp)].cls == K_MAGROUP) dest = w.objs[size_t(pp)].uid();
    }
    if (!dest) return false;
    const int owner = t.parent_of[size_t(gi)];
    const uint32_t owner_uid = w.objs[size_t(owner)].uid();
    // An owner marker that only carried this group (the template's "flyby" marker) goes with it, unless something links to it.
    bool drop_owner = w.objs[size_t(owner)].cls == K_DUMMY && t.kids[size_t(owner)].size() == 1;
    if (drop_owner) {
        static const uint32_t K_UID = key_of("UID"), K_PAR = key_of("parent"), K_CHILD = key_of("child"), K_SIB = key_of("sibling");
        for (auto& o : w.objs) {
            for (auto& p : o.props)
                if (p.data.size() == 4 && (p.type == PT_U32 || p.type == PT_I32) && p.key != K_UID && p.key != K_PAR && p.key != K_CHILD && p.key != K_SIB &&
                    uint32_t(p.as_int()) == owner_uid) { drop_owner = false; break; }
            if (!drop_owner) break;
        }
    }
    wedit::reparent(w, g, dest);
    if (drop_owner) wedit::remove_subtree(w, owner_uid);
    return true;
}

bool shot_hangs_under_marker(const Wdf& w, const WTree& t, const ShotInfo& sh) {
    const int pi = sh.eye_path ? t.find(sh.eye_path) : -1;
    if (pi < 0 || w.objs[size_t(pi)].cls != K_PATH) return false;
    const int gi = t.find(w.objs[size_t(pi)].u32("m_wptGroupUID"));
    Vec3 at;
    return gi >= 0 && w.objs[size_t(gi)].cls == K_WPG && path_owner_point(w, t, gi, at);
}

bool cutscene_play_from_own_view(Scenario& s, uint32_t ctrl) {
    Wdf& w = s.world;
    ShotInfo si;
    bool found = false;
    for (auto& sh : cutscene_shots(w, shot_cinema(w, ctrl))) if (sh.ctrl == ctrl) { si = sh; found = true; }
    if (!found) return false;
    bool changed = false;
    if (si.eye_path) changed |= detach_path_group(w, si.eye_path);
    if (si.focus) if (const WObj* f = object_by_uid(w, si.focus); f && f->cls == K_PATH) changed |= detach_path_group(w, si.focus);
    WTree t;
    t.build(w);
    const Vec3 off = focus_offset(w, t, t.find(ctrl));
    if (off.x != 0.f || off.y != 0.f || off.z != 0.f) { zero_focus_offset(w, ctrl); changed = true; }
    if (changed) s.dirty = true;
    return changed;
}

uint32_t shot_cinema(const Wdf& w, uint32_t ctrl) {
    WTree t;
    t.build(w);
    for (int p = t.find(ctrl); p >= 0; p = t.parent_of[size_t(p)])
        if (w.objs[size_t(p)].cls == K_CINEMA) return w.objs[size_t(p)].uid();
    return 0;
}

std::vector<ShotInfo> cutscene_shots(const Wdf& w, uint32_t cinema) {
    WTree t;
    t.build(w);
    return cutscene_shots(w, t, cinema);
}

std::vector<ShotInfo> cutscene_shots(const Wdf& w, const WTree& t, uint32_t cinema) {
    std::vector<ShotInfo> out;
    int ci = t.find(cinema);
    if (ci < 0 || w.objs[size_t(ci)].cls != K_CINEMA) return out;
    std::vector<int> sub;
    t.subtree(ci, sub);
    float at = 0;
    for (int k : sub) {
        const WObj& c = w.objs[size_t(k)];
        if (c.cls != K_CTRL) continue;
        ShotInfo sh;
        sh.ctrl = c.uid();
        sh.cam = c.u32("Camera");
        sh.init = c.u32("triggerInit");
        sh.exit = c.u32("triggerExit");
        float path_dur = 0;
        bool have_eye = false, have_focus = false;
        for (int m : t.kids[size_t(k)]) {
            const WObj& st = w.objs[size_t(m)];
            if (st.cls != K_PROP) continue;
            if (st.i32("type") == 2 && !have_eye) {
                have_eye = true;
                sh.eye_path = st.u32("eyeUID");
                sh.eye = path_points(w, t, sh.eye_path, &path_dur);
            }
            if (st.i32("type") == 3 && !have_focus) {
                have_focus = true;
                sh.focus = st.u32("focusUID");
                sh.focus_pts = path_points(w, t, sh.focus, nullptr);
            }
        }
        // A deleted eye path or focus leaves a dangling link: the shot is still, at the camera's own spot.
        if (sh.eye_path && t.find(sh.eye_path) < 0) sh.eye_path = 0;
        if (sh.focus && t.find(sh.focus) < 0) sh.focus = 0;
        if (sh.eye_path && w.objs[size_t(t.find(sh.eye_path))].cls == K_PATH) sh.move_len = path_dur;
        const WObj* cam = object_by_uid(w, sh.cam);
        if (sh.eye.empty() && cam) if (Vec3 p; get_pos(*cam, p)) sh.eye.push_back(p);
        if (cam) if (const WProp* f = cam->find(kFovKey)) sh.fov = float(f->as_float());
        const float d = c.f32("CameraDuration");
        sh.len = d > 0.05f ? d : path_dur > 0.05f ? path_dur : 8.f;  // the game's default shot length is 8 s
        const Vec3 off = focus_offset(w, t, k);
        for (Vec3& p : sh.focus_pts) p = {p.x + off.x, p.y + off.y, p.z + off.z};
        if (!sh.focus_pts.empty()) { sh.look = sh.focus_pts.front(); sh.has_look = true; }
        sh.still = !sh.eye_path || sh.eye.size() < 2 || span(sh.eye) < 0.1f;
        sh.start = at;
        at += sh.len;
        out.push_back(std::move(sh));
    }
    return out;
}

bool is_trigger_action(const NameTable& names, const WObj& o) {
    static const uint32_t K_EVENT = key_of("MAEvent");
    return o.cls == K_EVENT || names.label(o.cls).compare(0, 8, "MAAction") == 0;
}

int cutscene_move_end_actions(Scenario& s, const NameTable& names, uint32_t from_trigger, uint32_t to_trigger) {
    if (!from_trigger || !to_trigger || from_trigger == to_trigger) return 0;
    if (!object_by_uid(s.world, from_trigger) || !object_by_uid(s.world, to_trigger)) return 0;
    std::vector<uint32_t> mv;
    for (auto& o : s.world.objs) if (o.parent() == from_trigger && is_trigger_action(names, o)) mv.push_back(o.uid());
    for (uint32_t u : mv) wedit::reparent(s.world, u, to_trigger);
    // AI orders waiting for the old end trigger (MAEvent under their units) wait for the new one.
    static const uint32_t K_EVENT = key_of("MAEvent");
    int orders = 0;
    for (auto& o : s.world.objs) if (o.cls == K_EVENT && o.u32("TriggerID") == from_trigger) { o.set_u32("TriggerID", to_trigger); orders++; }
    if (!mv.empty() || orders) s.dirty = true;
    return int(mv.size()) + orders;
}

std::set<uint32_t> cutscene_owned_triggers(const Wdf& w) {
    static const uint32_t K_AI = key_of("MAActionAI"), K_TIMER = key_of("MATriggerTimer");
    std::set<uint32_t> own;
    for (auto& o : w.objs)
        if (o.cls == K_CTRL) { own.insert(o.u32("triggerInit")); own.insert(o.u32("triggerExit")); }
    own.erase(0);
    std::set<uint32_t> cue;
    for (auto& a : w.objs) {
        if (a.cls != K_AI || !own.count(a.parent())) continue;
        const WObj* t = object_by_uid(w, a.u32("ActionEntity"));
        if (t && t->cls == K_TIMER && t->find("TriggerEnabled") && !t->i32("TriggerEnabled")) cue.insert(t->uid());
    }
    own.insert(cue.begin(), cue.end());
    return own;
}

float cutscene_total(const std::vector<ShotInfo>& shots) {
    float t = 0;
    for (auto& s : shots) t += s.len;
    return t;
}

bool set_shot_length(Scenario& s, uint32_t ctrl, float sec) {
    Wdf& w = s.world;
    WObj* c = object_by_uid(w, ctrl);
    if (!c || c->cls != K_CTRL) return false;
    sec = std::clamp(sec, kShotMin, kShotMax);
    float old_len = 8, path_dur = -1;
    uint32_t path = 0;
    for (auto& sh : cutscene_shots(w, shot_cinema(w, ctrl)))
        if (sh.ctrl == ctrl) { old_len = sh.len; path = sh.eye_path; path_dur = sh.move_len; }
    c->set_f32("CameraDuration", sec);
    if (WObj* p = path ? object_by_uid(w, path) : nullptr; p && p->cls == K_PATH)
        if (path_dur <= 0.05f || std::fabs(path_dur - old_len) < 0.05f) p->set_f32("duration", sec);  // linked: the move spans the whole shot
    s.dirty = true;
    return true;
}

bool carry_path_duration(Scenario& s, uint32_t ctrl, float old_len) {
    Wdf& w = s.world;
    const WObj* c = object_by_uid(w, ctrl);
    if (!c || c->cls != K_CTRL) return false;
    const float len = std::clamp(c->f32("CameraDuration"), kShotMin, kShotMax);
    for (auto& sh : cutscene_shots(w, shot_cinema(w, ctrl)))
        if (sh.ctrl == ctrl) {
            WObj* p = sh.eye_path ? object_by_uid(w, sh.eye_path) : nullptr;
            if (!p || p->cls != K_PATH) return false;
            if (sh.move_len <= 0.05f || std::fabs(sh.move_len - old_len) < 0.05f) {
                p->set_f32("duration", len);
                s.dirty = true;
                return true;
            }
        }
    return false;
}

bool set_shot_fov(Scenario& s, uint32_t ctrl, float deg) {
    WObj* c = object_by_uid(s.world, ctrl);
    WObj* cam = c ? object_by_uid(s.world, c->u32("Camera")) : nullptr;
    if (!cam || !cam->find(kFovKey)) return false;
    cam->find(kFovKey)->set_float(std::clamp(deg, 5.f, 120.f));
    s.dirty = true;
    return true;
}

bool set_shot_move_len(Scenario& s, uint32_t ctrl, float sec) {
    Wdf& w = s.world;
    const WObj* c = object_by_uid(w, ctrl);
    if (!c || c->cls != K_CTRL) return false;
    for (auto& sh : cutscene_shots(w, shot_cinema(w, ctrl)))
        if (sh.ctrl == ctrl) {
            WObj* p = sh.eye_path ? object_by_uid(w, sh.eye_path) : nullptr;
            if (!p || p->cls != K_PATH) return false;
            p->set_f32("duration", std::clamp(sec, kShotMin, std::max(kShotMin, sh.len)));
            s.dirty = true;
            return true;
        }
    return false;
}

bool set_shot_look(Scenario& s, uint32_t ctrl, Vec3 look) {
    Wdf& w = s.world;
    if (!object_by_uid(w, ctrl)) return false;
    for (auto& sh : cutscene_shots(w, shot_cinema(w, ctrl)))
        if (sh.ctrl == ctrl) {
            WObj* f = sh.focus ? object_by_uid(w, sh.focus) : nullptr;
            if (!f) return false;
            if (f->cls == K_PATH) {
                const uint32_t fp = f->uid();
                detach_path_group(w, fp);  // a focus path flies from its group's owner first too
                std::vector<uint32_t> pts = waypoint_uids(w, fp);
                for (uint32_t u : pts) if (WObj* o = object_by_uid(w, u)) set_pos(*o, look);
                if (pts.empty()) return false;
            } else {
                set_pos(*f, look);
            }
            zero_focus_offset(w, ctrl);  // the game looks at the focus + Offset: the point itself now
            s.dirty = true;
            return true;
        }
    return false;
}

bool set_shot_view(Scenario& s, uint32_t ctrl, int which, Vec3 eye, Vec3 look, bool set_look, ScenarioIO* io) {
    Wdf& w = s.world;
    WObj* c = object_by_uid(w, ctrl);
    if (!c || c->cls != K_CTRL) return false;
    const uint32_t cin = shot_cinema(w, ctrl);
    ShotInfo si;
    for (auto& sh : cutscene_shots(w, cin)) if (sh.ctrl == ctrl) si = sh;
    bool wrote = false;
    uint32_t group = 0;
    std::vector<uint32_t> pts = si.eye_path ? waypoint_uids(w, si.eye_path, &group) : std::vector<uint32_t>();
    if (pts.empty() && io) {
        if (uint32_t p = io->ensure_shot_eye_path(s, ctrl)) {
            si.eye_path = p;
            pts = waypoint_uids(w, p, &group);
        }
    }
    // The game flies from the waypoint group's owner first (path_owner_point); the shot's own points must be the whole path.
    if (!pts.empty() && detach_path_group(w, si.eye_path)) pts = waypoint_uids(w, si.eye_path, &group);
    const bool bare_eye = pts.empty() && si.eye_path && object_by_uid(w, si.eye_path) && object_by_uid(w, si.eye_path)->cls != K_PATH;
    if (bare_eye) {  // retail shots that fly to one object
        if (WObj* o = object_by_uid(w, si.eye_path)) { set_pos(*o, eye); wrote = true; }
    }
    if (!pts.empty()) {
        WObj* g = object_by_uid(w, group);
        const bool was_last = g && g->u32("WaypointID") == pts.back();
        auto add_point = [&](const WObj& like, Vec3 at) {
            WObj n = like;
            uint32_t uid = next_uid(w);
            n.set_u32("UID", uid);
            set_pos(n, at);
            wedit::append_child(w, group, std::move(n));
            pts.push_back(uid);
        };
        if (which == 0) {
            while (pts.size() > 2) { wedit::remove_subtree(w, pts.back()); pts.pop_back(); }
            if (pts.size() == 1) { if (const WObj* o = object_by_uid(w, pts[0])) { WObj like = *o; add_point(like, eye); } }
            if (WObj* o = object_by_uid(w, pts[0])) set_pos(*o, eye);
            if (WObj* o = object_by_uid(w, pts[1])) set_pos(*o, {eye.x + kStillGap, eye.y, eye.z});
        } else if (which == 1) {
            if (WObj* o = object_by_uid(w, pts[0])) set_pos(*o, eye);
        } else if (pts.size() == 1) {
            if (const WObj* o = object_by_uid(w, pts[0])) { WObj like = *o; add_point(like, eye); }
        } else if (WObj* o = object_by_uid(w, pts.back())) {
            set_pos(*o, eye);
        }
        // Retail cinema path groups name their last waypoint; keep that true.
        g = object_by_uid(w, group);
        if (g && (was_last || !object_by_uid(w, g->u32("WaypointID")))) g->set_u32("WaypointID", pts.back());
        wrote = true;
    }
    if (which != 2)
        if (WObj* cam = object_by_uid(w, c->u32("Camera"))) { set_pos(*cam, eye); wrote = true; }  // The camera object stands at the eye too
    if (set_look && si.focus)
        if (WObj* f = object_by_uid(w, si.focus); f && f->cls != K_PATH) { set_pos(*f, look); zero_focus_offset(w, ctrl); wrote = true; }
    if (wrote) s.dirty = true;
    return wrote;
}


// ----------------------------------------------------------------------------- events

namespace {

const uint32_t K_AI = key_of("MAActionAI"), K_TIMER = key_of("MATriggerTimer"), K_EVENT = key_of("MAEvent");
const char* kCueGroup = "Cutscene timers";

bool is_unit_asleep(const Wdf& w, uint32_t unit) {
    const WObj* u = object_by_uid(w, unit);
    return u && u->find("m_bAIEnabled") && u->i32("m_bAIEnabled") == 0;
}

// A cue timer: a disabled MATriggerTimer some shot's start trigger switches on.
bool timer_started_by(const Wdf& w, uint32_t timer, uint32_t* init_out = nullptr) {
    const WObj* t = object_by_uid(w, timer);
    if (!t || t->cls != K_TIMER || !t->find("TriggerEnabled") || t->i32("TriggerEnabled")) return false;
    for (auto& a : w.objs) {
        if (a.cls != K_AI || a.u32("ActionEntity") != timer || a.i32("EnableAI") != 1) continue;
        for (auto& c : w.objs)
            if (c.cls == K_CTRL && c.u32("triggerInit") == a.parent()) { if (init_out) *init_out = a.parent(); return true; }
    }
    return false;
}

// Plumbing under a holder: starts of cue timers, the timer's own switch-off, a unit's wake for an order of this holder.
bool is_plumbing(const Wdf& w, const WObj& a, uint32_t holder) {
    if (a.cls != K_AI) return false;
    const uint32_t ent = a.u32("ActionEntity");
    if (a.i32("EnableAI") == 0) return ent == holder;
    if (timer_started_by(w, ent)) return true;
    for (auto& e : w.objs)
        if (e.cls == K_EVENT && e.u32("TriggerID") == holder && order_unit(w, e.uid()) == ent) return true;
    return false;
}

// The self-off action stays the timer's last child.
void keep_selfoff_last(Wdf& w, uint32_t timer) {
    uint32_t off = 0;
    for (auto& o : w.objs)
        if (o.parent() == timer && o.cls == K_AI && o.i32("EnableAI") == 0 && o.u32("ActionEntity") == timer) off = o.uid();
    if (!off) return;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t last = 0;
        for (auto& o : w.objs) if (o.parent() == timer) last = o.uid();
        if (last == off || !wedit::move_sibling(w, off, +1)) break;
    }
}

bool has_event_content(const Wdf& w, const NameTable& names, uint32_t timer) {
    for (auto& o : w.objs)
        if (o.parent() == timer && is_trigger_action(names, o) && !is_plumbing(w, o, timer)) return true;
    for (auto& e : w.objs) if (e.cls == K_EVENT && e.u32("TriggerID") == timer) return true;
    return false;
}

// An emptied cue timer goes, with the action that starts it.
void drop_if_empty_cue(Scenario& s, const NameTable& names, uint32_t holder) {
    Wdf& w = s.world;
    if (!timer_started_by(w, holder) || has_event_content(w, names, holder)) return;
    std::vector<uint32_t> starts;
    for (auto& a : w.objs) if (a.cls == K_AI && a.u32("ActionEntity") == holder && a.parent() != holder) starts.push_back(a.uid());
    for (uint32_t u : starts) wedit::remove_subtree(w, u);
    wedit::remove_subtree(w, holder);
}

// A unit that sleeps needs a wake with its order (as add_order does); one with no order left on this holder loses it.
void sync_wake(Scenario& s, uint32_t holder, uint32_t unit) {
    Wdf& w = s.world;
    if (!object_by_uid(w, holder)) return;
    bool need = false;
    for (auto& e : w.objs) if (e.cls == K_EVENT && e.u32("TriggerID") == holder && order_unit(w, e.uid()) == unit) need = true;
    std::vector<uint32_t> wakes;
    for (auto& a : w.objs) if (a.parent() == holder && a.cls == K_AI && a.u32("ActionEntity") == unit && a.i32("EnableAI") == 1) wakes.push_back(a.uid());
    if (!need) { for (uint32_t u : wakes) wedit::remove_subtree(w, u); return; }
    if (wakes.empty() && is_unit_asleep(w, unit))
        if (uint32_t a = add_action_object(s, holder, "MAActionAI", "Wake")) {
            if (WObj* ao = object_by_uid(w, a)) { ao->set_u32("ActionEntity", unit); ao->set_i32("EnableAI", 1); }
        }
}

}  // namespace

std::vector<std::pair<uint32_t, float>> cutscene_cue_timers(const Wdf& w, uint32_t init_trigger) {
    std::vector<std::pair<uint32_t, float>> out;
    if (!init_trigger) return out;
    for (auto& a : w.objs) {
        if (a.parent() != init_trigger || a.cls != K_AI || a.i32("EnableAI") != 1) continue;
        const uint32_t tu = a.u32("ActionEntity");
        const WObj* t = object_by_uid(w, tu);
        if (!t || t->cls != K_TIMER || !t->find("TriggerEnabled") || t->i32("TriggerEnabled")) continue;
        bool dup = false;
        for (auto& p : out) dup |= p.first == tu;
        if (!dup) out.push_back({tu, t->f32("TrgTimerDurationSeconds")});
    }
    return out;
}

bool cutscene_locate(const std::vector<ShotInfo>& shots, float t, int& shot, float& offset) {
    if (shots.empty()) return false;
    t = std::max(0.f, t);
    int k = 0;
    for (int i = 0; i < int(shots.size()); i++) if (t >= shots[size_t(i)].start - 1e-4f) k = i;
    shot = k;
    offset = std::max(0.f, t - shots[size_t(k)].start);
    offset = std::round(offset * 100.f) / 100.f;
    return true;
}

std::vector<CutEvent> cutscene_events(const Wdf& w, const NameTable& names, uint32_t cinema) {
    std::vector<CutEvent> out;
    const std::vector<ShotInfo> shots = cutscene_shots(w, cinema);
    for (int k = 0; k < int(shots.size()); k++) {
        const ShotInfo& sh = shots[size_t(k)];
        std::vector<std::pair<uint32_t, float>> holders;
        if (sh.init) holders.push_back({sh.init, 0.f});
        for (auto& p : cutscene_cue_timers(w, sh.init)) holders.push_back(p);
        for (auto& [h, off] : holders) {
            for (auto& o : w.objs) {
                if (o.parent() != h || !is_trigger_action(names, o) || o.cls == K_EVENT || is_plumbing(w, o, h)) continue;
                CutEvent e;
                e.obj = o.uid(); e.shot = k; e.offset = off; e.t = sh.start + off; e.holder = h;
                out.push_back(e);
            }
            for (uint32_t ev : orders_of_trigger(w, h)) {
                CutEvent e;
                e.obj = ev; e.order = true; e.shot = k; e.offset = off; e.t = sh.start + off; e.holder = h; e.unit = order_unit(w, ev);
                out.push_back(e);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const CutEvent& a, const CutEvent& b) { return a.t < b.t; });
    return out;
}

std::vector<CutEvent> cutscene_end_actions(const Wdf& w, const NameTable& names, uint32_t cinema) {
    std::vector<CutEvent> out;
    const std::vector<ShotInfo> shots = cutscene_shots(w, cinema);
    if (shots.empty() || !shots.back().exit) return out;
    const ShotInfo& sh = shots.back();
    const float total = cutscene_total(shots);
    for (auto& o : w.objs) {
        if (o.parent() != sh.exit || !is_trigger_action(names, o) || o.cls == K_EVENT || is_plumbing(w, o, sh.exit)) continue;
        CutEvent e;
        e.obj = o.uid(); e.shot = int(shots.size()) - 1; e.offset = sh.len; e.t = total; e.holder = sh.exit;
        out.push_back(e);
    }
    for (uint32_t ev : orders_of_trigger(w, sh.exit)) {
        CutEvent e;
        e.obj = ev; e.order = true; e.shot = int(shots.size()) - 1; e.offset = sh.len; e.t = total; e.holder = sh.exit; e.unit = order_unit(w, ev);
        out.push_back(e);
    }
    return out;
}

uint32_t cutscene_event_holder(Scenario& s, uint32_t cinema, int shot, float offset, bool create) {
    Wdf& w = s.world;
    const std::vector<ShotInfo> shots = cutscene_shots(w, cinema);
    if (shot < 0 || shot >= int(shots.size()) || !shots[size_t(shot)].init) return 0;
    const ShotInfo& sh = shots[size_t(shot)];
    offset = std::round(std::max(0.f, offset) * 100.f) / 100.f;
    if (offset < 0.05f) return sh.init;
    for (auto& [tu, off] : cutscene_cue_timers(w, sh.init)) if (std::fabs(off - offset) < 0.005f) return tu;
    if (!create) return 0;
    uint32_t next = next_uid(w);
    const uint32_t grp = ensure_group(w, root_uid(w), kCueGroup, next);
    const WObj* cin = object_by_uid(w, cinema);
    char nm[160];
    snprintf(nm, sizeof nm, "%s - shot %d + %g s", cin ? cin->description().c_str() : "Cutscene", shot + 1, double(offset));
    const uint32_t tu = gen_add_trigger(w, "MATriggerTimer", nm, grp);
    if (!tu) return 0;
    if (WObj* t = object_by_uid(w, tu)) {
        t->set_i32("TriggerEnabled", 0);   // asleep until the shot starts
        t->set_i32("TriggerAutoReset", 1);  // so a cutscene played twice fires twice; its last action switches it off again
        t->set_f32("TrgTimerDurationSeconds", offset);
        t->set_f32("TrgTimerDuration", offset / 60.f);
        t->set_f32("TrgTimerStart", 0);
    }
    char sn[96];
    snprintf(sn, sizeof sn, "Start the shot %d timer (%g s)", shot + 1, double(offset));
    if (uint32_t a = gen_add_action(w, sh.init, "MAActionAI", sn))
        if (WObj* ao = object_by_uid(w, a)) { ao->set_u32("ActionEntity", tu); ao->set_i32("EnableAI", 1); }
    if (uint32_t a = gen_add_action(w, tu, "MAActionAI", "Switch this timer off"))
        if (WObj* ao = object_by_uid(w, a)) { ao->set_u32("ActionEntity", tu); ao->set_i32("EnableAI", 0); }
    s.dirty = true;
    return tu;
}

uint32_t cutscene_add_action(Scenario& s, uint32_t cinema, float t, const char* cls, const std::string& name) {
    const std::vector<ShotInfo> shots = cutscene_shots(s.world, cinema);
    int k;
    float off;
    if (!cutscene_locate(shots, t, k, off)) return 0;
    const uint32_t h = cutscene_event_holder(s, cinema, k, off);
    if (!h) return 0;
    const uint32_t a = add_action_object(s, h, cls, name);
    if (a) keep_selfoff_last(s.world, h);
    return a;
}

uint32_t cutscene_add_order(Scenario& s, uint32_t cinema, float t, uint32_t unit, const char* behaviour_cls, bool loop) {
    const std::vector<ShotInfo> shots = cutscene_shots(s.world, cinema);
    int k;
    float off;
    if (!cutscene_locate(shots, t, k, off)) return 0;
    const uint32_t h = cutscene_event_holder(s, cinema, k, off);
    if (!h) return 0;
    const uint32_t ev = add_order(s, h, unit, behaviour_cls);
    if (ev && std::string(behaviour_cls) == "MAPatrol" && !order_path(s.world, ev)) {
        const WObj* u = object_by_uid(s.world, unit);
        if (uint32_t g = add_ai_path(s, {}, (u ? u->description() : std::string("Unit")) + (loop ? " - patrol path" : " - walk path"))) {
            if (WObj* go = object_by_uid(s.world, g)) go->set_i32("TraverseType", loop ? 2 : 1);  // 1 = walk the route once, then stand; 2 = loop it
            wedit::reparent(s.world, g, ev);
        }
    }
    keep_selfoff_last(s.world, h);
    return ev;
}

uint32_t cutscene_add_end_action(Scenario& s, uint32_t cinema, const char* cls, const std::string& name) {
    const std::vector<ShotInfo> shots = cutscene_shots(s.world, cinema);
    if (shots.empty() || !shots.back().exit) return 0;
    return add_action_object(s, shots.back().exit, cls, name);
}

bool cutscene_move_event(Scenario& s, const NameTable& names, uint32_t cinema, uint32_t obj, float t) {
    Wdf& w = s.world;
    CutEvent ev;
    bool found = false;
    for (auto& e : cutscene_events(w, names, cinema)) if (e.obj == obj) { ev = e; found = true; }
    if (!found) return false;
    const std::vector<ShotInfo> shots = cutscene_shots(w, cinema);
    int k;
    float off;
    if (!cutscene_locate(shots, t, k, off)) return false;
    const uint32_t nh = cutscene_event_holder(s, cinema, k, off);
    if (!nh) return false;
    if (nh == ev.holder) return true;
    if (ev.order) {
        WObj* o = object_by_uid(w, obj);
        if (!o) return false;
        o->set_u32("TriggerID", nh);
        sync_wake(s, ev.holder, ev.unit);
        sync_wake(s, nh, ev.unit);
    } else {
        wedit::reparent(w, obj, nh);
    }
    keep_selfoff_last(w, nh);
    drop_if_empty_cue(s, names, ev.holder);
    s.dirty = true;
    return true;
}

bool cutscene_remove_event(Scenario& s, const NameTable& names, uint32_t cinema, uint32_t obj) {
    Wdf& w = s.world;
    CutEvent ev;
    bool found = false;
    for (auto& e : cutscene_events(w, names, cinema)) if (e.obj == obj) { ev = e; found = true; }
    for (auto& e : cutscene_end_actions(w, names, cinema)) if (e.obj == obj) { ev = e; found = true; }
    if (!found) return false;
    wedit::remove_subtree(w, obj);
    if (ev.order) sync_wake(s, ev.holder, ev.unit);
    drop_if_empty_cue(s, names, ev.holder);
    s.dirty = true;
    return true;
}

void cutscene_drop_shot_triggers(Scenario& s, uint32_t init_trigger, uint32_t exit_trigger, const std::vector<uint32_t>& cue_timers) {
    Wdf& w = s.world;
    std::vector<uint32_t> trig = cue_timers, kill;
    if (init_trigger) trig.push_back(init_trigger);
    if (exit_trigger) trig.push_back(exit_trigger);
    for (auto& e : w.objs)
        if (e.cls == K_EVENT && e.u32("TriggerID") && std::find(trig.begin(), trig.end(), e.u32("TriggerID")) != trig.end()) kill.push_back(e.uid());
    for (uint32_t u : kill) if (object_by_uid(w, u)) wedit::remove_subtree(w, u);
    for (uint32_t u : cue_timers) if (object_by_uid(w, u)) wedit::remove_subtree(w, u);  // outside the shot: they go with it
    s.dirty = true;
}

}  // namespace ma2
