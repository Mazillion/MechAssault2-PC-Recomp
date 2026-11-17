// Glue (playtest 11-04): a lock on a snapped join point ties two pieces of one snap family together - foundations (edge
// middles), walls / gates / fences and bridge pieces (their two ends) and road pieces (entry and exits). Glued pieces
// move together (Move arrows, the yellow square, dragging, raising), turn together, and Rotate / Tilt on a glued piece
// turn it about its first glued joint (a hinge) instead of its middle; what is glued beyond it follows (a turn carries it
// round, a tilt keeps its own lean and moves with the joint, so a flat plate after a ramp stays flat).
//
// The glue is one editor-only string property on each of the two pieces (kGlueProp, "mine:uid:theirs;..."): undo,
// delete and copy handle it with the world. A link counts only while both pieces record it (a copy's links point at
// the original's partner, which does not list the copy). Saving strips it from the world the game reads and keeps it in
// editor/project.json ("glue"), like EditorCrop (scenario.cpp).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "../core/crop.h"
#include "../core/triggers.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static const uint32_t K_ROAD_G = key_of("MARoad");
static const float kDeg = 57.2957795f;

int App::glue_family(const WObj& o) {
    if (o.cls == K_ROAD_G) return 4;
    const TemplateInfo* t = cat_ ? cat_->find(o.template_id()) : nullptr;
    return t ? snap_family(*t) : 0;
}

// World join points of a piece, in a fixed order per family (the glue stores the index).
bool App::object_joints(const WObj& o, std::vector<Joint>& js) {
    js.clear();
    Vec3 p;
    if (!get_pos(o, p)) return false;
    const int fam = glue_family(o);
    if (!fam) return false;
    float pitch, roll;
    get_tilt(o, pitch, roll);
    BoxInstance mi;
    orient_instance(mi, heading(o), pitch, roll, p);
    auto world = [&](float x, float y, float z) {
        return Vec3{mi.r0.x * x + mi.r0.y * y + mi.r0.z * z + mi.r0.w, mi.r1.x * x + mi.r1.y * y + mi.r1.z * z + mi.r1.w,
                    mi.r2.x * x + mi.r2.y * y + mi.r2.z * z + mi.r2.w};
    };
    auto add = [&](float x, float y, float z, float dx, float dz) {
        Joint j;
        j.p = world(x, y, z);
        float ox = mi.r0.x * dx + mi.r0.z * dz, oz = mi.r2.x * dx + mi.r2.z * dz, l = std::hypot(ox, oz);
        j.ox = l > 1e-5f ? ox / l : 0;
        j.oz = l > 1e-5f ? oz / l : 0;
        js.push_back(j);
    };
    if (fam == 4) {
        std::string def = o.str("RoadDefinition");
        for (char& ch : def) if (ch == 0x5C) ch = '/';
        const RoadPiece* rp = find_road_piece(def);
        if (!rp) return false;
        std::vector<RoadPiece::Exit> ex{{0, 0, 0, 1}};
        if (rp->exits.empty()) ex.push_back({0, -rp->l, 0, -1});
        else ex.insert(ex.end(), rp->exits.begin(), rp->exits.end());
        if (crop::Rect cr; crop::of(o, cr))
            for (auto& e : ex) crop::clamp_point(cr, e.x, e.z);
        for (auto& e : ex) add(e.x, 0, e.z, e.vx, e.vz);
        return true;
    }
    if (fam == 3) {
        // Foundations: the middle of each edge of the drawn (cropped) footprint, on its top (snap_ends' order).
        GpuModel* gm = obj_model(o);
        if (!gm || gm->state != GpuModel::Ready) return false;
        const float cx = 0.5f * (gm->bmin[0] + gm->bmax[0]), cz = 0.5f * (gm->bmin[2] + gm->bmax[2]);
        const float hx = 0.5f * (gm->bmax[0] - gm->bmin[0]), hz = 0.5f * (gm->bmax[2] - gm->bmin[2]), top = gm->bmax[1];
        add(cx + hx, top, cz, 1, 0);
        add(cx - hx, top, cz, -1, 0);
        add(cx, top, cz + hz, 0, 1);
        add(cx, top, cz - hz, 0, -1);
        return true;
    }
    // Walls, gates, fences, bridge pieces: the two ends of the long footprint axis, at the base.
    const TemplateInfo* t = cat_ ? cat_->find(o.template_id()) : nullptr;
    SnapFoot f;
    if (!t || !object_foot(o, f)) return false;  // a cut piece's joints are its cut ends
    GpuModel* gm = models_.get(t->model, t->mtb);
    const float base = gm && gm->state == GpuModel::Ready ? gm->bmin[1] : 0.f;
    for (float s : {1.f, -1.f}) {
        const float lx = f.along_x ? s : 0, lz = f.along_x ? 0 : s;
        add(f.cx + lx * f.half, base, f.cz + lz * f.half, lx, lz);
    }
    return true;
}

// Two joints of different pieces meet (the snap put them together): walls and bridges overlap up to a few metres in retail runs.
static bool joints_meet(const App::Joint& a, const App::Joint& b, int fam) {
    const float tol = fam == 1 || fam == 2 ? 3.f : 1.5f;
    // Road pieces each rest on their own ground (road_rest_height), so their ends join on the map, at any height.
    return std::fabs(a.p.x - b.p.x) < tol && std::fabs(a.p.z - b.p.z) < tol && (fam == 4 || std::fabs(a.p.y - b.p.y) < tol);
}

static std::vector<App::GlueLink> parse_links(const WObj& o) {
    std::vector<App::GlueLink> out;
    const std::string s = o.str(App::kGlueProp);
    size_t a = 0;
    while (a < s.size()) {
        size_t e = s.find(';', a);
        if (e == std::string::npos) e = s.size();
        App::GlueLink l{};
        unsigned u = 0;
        if (sscanf(s.substr(a, e - a).c_str(), "%d:%u:%d", &l.mine, &u, &l.theirs) == 3 && u) { l.uid = u; out.push_back(l); }
        a = e + 1;
    }
    return out;
}

static void write_links(WObj& o, const std::vector<App::GlueLink>& ls) {
    if (ls.empty()) { o.erase(key_of(App::kGlueProp)); return; }
    std::string s;
    for (auto& l : ls) {
        char b[48];
        snprintf(b, sizeof b, "%s%d:%u:%d", s.empty() ? "" : ";", l.mine, l.uid, l.theirs);
        s += b;
    }
    o.set_str(App::kGlueProp, s);
}

std::vector<App::GlueLink> App::glue_links(const WObj& o) {
    std::vector<GlueLink> out;
    if (!doc_ || !o.find(kGlueProp)) return out;
    for (auto& l : parse_links(o)) {
        const WObj* other = object_by_uid(doc_->world, l.uid);
        if (!other || other == &o) continue;
        for (auto& r : parse_links(*other))
            if (r.uid == o.uid() && r.mine == l.theirs && r.theirs == l.mine) { out.push_back(l); break; }
    }
    return out;
}

void App::glue_set(uint32_t a, int ja, uint32_t b, int jb, bool on) {
    WObj* oa = object_by_uid(doc_->world, a);
    WObj* ob = object_by_uid(doc_->world, b);
    if (!oa || !ob) return;
    auto edit = [&](WObj& o, int mine, uint32_t uid, int theirs) {
        auto ls = parse_links(o);
        // Drop this link and any stale one (its partner gone or not listing it back).
        ls.erase(std::remove_if(ls.begin(), ls.end(), [&](const GlueLink& l) {
                     if (l.mine == mine && l.uid == uid && l.theirs == theirs) return true;
                     const WObj* p = object_by_uid(doc_->world, l.uid);
                     if (!p) return true;
                     for (auto& r : parse_links(*p)) if (r.uid == o.uid()) return false;
                     return true;
                 }),
                 ls.end());
        if (on) ls.push_back({mine, uid, theirs});
        write_links(o, ls);
    };
    edit(*oa, ja, b, jb);
    edit(*ob, jb, a, ja);
    if (on)  // glued flat pieces keep the height they have
        for (WObj* po : {oa, ob})
            if (sheet_kind(*po)) {
                int ly = 0;
                bool fx = true;
                const bool has = place::layer_of(*po, ly, fx);
                write_layer(*po, has ? ly : 0, true);
            }
    doc_->dirty = true;
    objects_dirty_ = true;
}

std::vector<uint32_t> App::glue_group(const std::set<uint32_t>& seeds) {
    std::vector<uint32_t> out(seeds.begin(), seeds.end());
    std::set<uint32_t> seen(seeds.begin(), seeds.end());
    for (size_t i = 0; i < out.size(); i++) {
        const WObj* o = object_by_uid(doc_->world, out[i]);
        if (!o) continue;
        for (auto& l : glue_links(*o))
            if (seen.insert(l.uid).second) out.push_back(l.uid);
    }
    return out;
}

// Everything glued to `from` (any kind of link) without entering `block`.
std::set<uint32_t> App::glue_reach(uint32_t from, const std::set<uint32_t>& block) {
    std::set<uint32_t> seen{from};
    std::vector<uint32_t> st{from};
    while (!st.empty()) {
        uint32_t u = st.back();
        st.pop_back();
        const WObj* o = object_by_uid(doc_->world, u);
        if (!o) continue;
        for (auto& l : glue_links(*o))
            if (!block.count(l.uid) && seen.insert(l.uid).second) st.push_back(l.uid);
    }
    return seen;
}

// One rigid body: the piece and everything held to it by whole-object glue (Glue tool links, joint -1).
std::set<uint32_t> App::glue_body(uint32_t uid) {
    std::set<uint32_t> seen{uid};
    std::vector<uint32_t> st{uid};
    while (!st.empty()) {
        uint32_t u = st.back();
        st.pop_back();
        const WObj* o = object_by_uid(doc_->world, u);
        if (!o) continue;
        for (auto& l : glue_links(*o))
            if (l.mine < 0 && seen.insert(l.uid).second) st.push_back(l.uid);
    }
    return seen;
}

// One piece with its attached parts (subtree): moved by d, or turned about the vertical through w by `turn`.
void App::glue_shift(uint32_t uid, const Vec3& d, const Vec3& w, float turn) {
    WTree t;
    t.build(doc_->world);
    int ri = t.find(uid);
    if (ri < 0) return;
    std::vector<int> sub;
    t.subtree(ri, sub);
    const float cs = std::cos(turn), sn = std::sin(turn);
    for (int i : sub) {
        WObj& o = doc_->world.objs[size_t(i)];
        Vec3 p;
        if (get_pos(o, p)) {
            if (turn != 0) {
                float dx = p.x - w.x, dz = p.z - w.z;
                p = {w.x + cs * dx + sn * dz, p.y, w.z - sn * dx + cs * dz};
            }
            set_pos(o, {p.x + d.x, p.y + d.y, p.z + d.z});
        }
        if (turn != 0 && (o.find("CorporalHeading") || o.find("RoadRotation") || i == ri)) set_heading(o, heading(o) + turn);
    }
}

// Rotation matrices (world = M * local, the editor's Ry(heading) * Rx(pitch) * Rz(roll): gizmo.cpp orient_instance).
namespace {
struct M3 { float m[3][3]; };
M3 orient(float h, float p, float r) {
    BoxInstance mi;
    orient_instance(mi, h, p, r, {0, 0, 0});
    return {{{mi.r0.x, mi.r0.y, mi.r0.z}, {mi.r1.x, mi.r1.y, mi.r1.z}, {mi.r2.x, mi.r2.y, mi.r2.z}}};
}
M3 axis_angle(float x, float y, float z, float a) {
    const float c = std::cos(a), s = std::sin(a), t = 1 - c;
    return {{{t * x * x + c, t * x * y - s * z, t * x * z + s * y}, {t * x * y + s * z, t * y * y + c, t * y * z - s * x}, {t * x * z - s * y, t * y * z + s * x, t * z * z + c}}};
}
M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
Vec3 apply(const M3& a, const Vec3& v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z, a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}
}  // namespace

// The piece and its attached parts turned rigidly by R about the point w: positions, heading, pitch and roll.
void App::glue_rigid(uint32_t uid, const void* rot, const Vec3& w) {
    const M3& R = *static_cast<const M3*>(rot);
    WTree t;
    t.build(doc_->world);
    int ri = t.find(uid);
    if (ri < 0) return;
    std::vector<int> sub;
    t.subtree(ri, sub);
    for (int i : sub) {
        WObj& o = doc_->world.objs[size_t(i)];
        Vec3 p;
        if (get_pos(o, p)) {
            const Vec3 q = apply(R, {p.x - w.x, p.y - w.y, p.z - w.z});
            set_pos(o, {w.x + q.x, w.y + q.y, w.z + q.z});
        }
        if (!(o.find("CorporalHeading") || o.find("RoadRotation") || o.find("StartOrientation") || i == ri)) continue;
        const bool tilts = can_tilt(o);
        float op = 0, orl = 0;
        if (tilts) get_tilt(o, op, orl);
        const M3 m = mul(R, orient(heading(o), op, orl));
        set_heading(o, std::atan2(m.m[0][2], m.m[2][2]));
        if (tilts) set_tilt(o, std::asin(std::clamp(-m.m[1][2], -1.f, 1.f)), std::atan2(m.m[1][0], m.m[1][1]));
    }
}

// The hinge of the one selected piece's rigid body: its first joint glued to a piece outside the body.
bool App::glue_find_hinge(uint32_t me, uint32_t& owner, GlueLink& hinge) {
    std::vector<uint32_t> order{me};
    for (uint32_t u : glue_body(me)) if (u != me) order.push_back(u);
    const std::set<uint32_t> body(order.begin(), order.end());
    for (uint32_t u : order) {
        const WObj* o = object_by_uid(doc_->world, u);
        if (!o) continue;
        for (auto& l : glue_links(*o))
            if (l.mine >= 0 && !body.count(l.uid)) { owner = u; hinge = l; return true; }
    }
    return false;
}

bool App::glue_hinge(Vec3& w) {
    if (!doc_ || selection_.size() != 1) return false;
    uint32_t owner;
    GlueLink h;
    if (!glue_find_hinge(*selection_.begin(), owner, h)) return false;
    const WObj* o = object_by_uid(doc_->world, owner);
    std::vector<Joint> js;
    if (!o || !object_joints(*o, js) || h.mine >= int(js.size())) return false;
    w = js[size_t(h.mine)].p;
    return true;
}

// Rotate (kind 0, about the vertical) or tilt (1 pitch, 2 roll) the one selected piece with everything glued to it.
// Its rigid body (Glue tool links) turns as one about its hinge (first glued joint) or, with none, about `pv`;
// pieces joined to the body by other joints follow (a turn carries them round, a tilt moves them with the joint and
// keeps their own lean). Returns false when nothing is glued to it (the normal Rotate / Tilt applies).
bool App::glue_turn(int kind, float delta, const Vec3& pv) {
    if (!doc_ || selection_.size() != 1) return false;
    const uint32_t me = *selection_.begin();
    WObj* b = object_by_uid(doc_->world, me);
    if (!b || glue_links(*b).empty()) return false;
    if (kind > 0 && !can_tilt(*b)) return false;
    if (kind > 0 && b->cls == K_ROAD_G) road_tilt_notice();
    const std::set<uint32_t> body = glue_body(me);
    Vec3 w = pv;
    uint32_t howner = 0;
    GlueLink hinge;
    std::set<uint32_t> anchor;
    const bool hinged = glue_find_hinge(me, howner, hinge);
    if (hinged) {
        std::vector<Joint> js;
        const WObj* ho = object_by_uid(doc_->world, howner);
        if (!ho || !object_joints(*ho, js) || hinge.mine >= int(js.size())) return false;
        w = js[size_t(hinge.mine)].p;
        anchor = glue_reach(hinge.uid, body);
    }
    // Pieces joined to the body (not by its hinge) follow their joint.
    struct Follow { uint32_t owner; int joint; uint32_t partner; Vec3 before; };
    std::vector<Follow> follow;
    for (uint32_t u : body) {
        const WObj* o = object_by_uid(doc_->world, u);
        if (!o) continue;
        std::vector<Joint> js;
        object_joints(*o, js);
        for (auto& l : glue_links(*o)) {
            if (l.mine < 0 || body.count(l.uid) || (hinged && u == howner && l.uid == hinge.uid && l.mine == hinge.mine)) continue;
            if (anchor.count(l.uid)) {
                status_ = "These pieces are glued in a closed loop, so this one cannot turn on its own. Unlock one of its joints first.";
                status_time_ = 6;
                return true;
            }
            if (l.mine < int(js.size())) follow.push_back({u, l.mine, l.uid, js[size_t(l.mine)].p});
        }
    }
    // The turn as a rotation about a world axis: the vertical, or the piece's own across / along axis on the ground.
    const float h = heading(*b);
    M3 R = kind == 0 ? axis_angle(0, 1, 0, delta) : kind == 1 ? axis_angle(std::cos(h), 0, -std::sin(h), delta) : axis_angle(std::sin(h), 0, std::cos(h), delta);
    for (uint32_t u : body) glue_rigid(u, &R, w);
    std::set<uint32_t> moved = body;
    for (auto& f : follow) {
        const WObj* o = object_by_uid(doc_->world, f.owner);
        std::vector<Joint> js;
        if (!o || !object_joints(*o, js) || f.joint >= int(js.size())) continue;
        const Vec3& a = js[size_t(f.joint)].p;
        const Vec3 d{a.x - f.before.x, a.y - f.before.y, a.z - f.before.z};
        for (uint32_t u : glue_reach(f.partner, body)) {
            if (!moved.insert(u).second) continue;
            if (kind == 0) glue_rigid(u, &R, w);
            else glue_shift(u, d, {0, 0, 0}, 0);
        }
    }
    doc_->dirty = true;
    objects_dirty_ = true;
    return true;
}

// The middle of an object as drawn (its footprint centre, half way up its model): where a whole-object glue line ends.
Vec3 App::glue_anchor_point(uint32_t uid) {
    const WObj* o = object_by_uid(doc_->world, uid);
    Vec3 p{};
    if (!o || !get_pos(*o, p)) return p;
    const int oi = int(o - doc_->world.objs.data());
    const Vec3 off = footprint_offset(oi);
    float up = 1.f;
    for (const ObjView& v : views_)
        if (v.index == oi) { up = std::max(1.f, 0.5f * (v.hi[1] - v.lo[1])); break; }
    return {p.x + off.x, p.y + off.y + up, p.z + off.z};
}

// Lock icons: for the one selected piece, one per joint that meets a joint of another piece of its family (open
// lock) or is glued (closed); and a closed lock in the middle of every whole-object glue line of its group (or of the
// whole map while the Glue tool is on).
std::vector<App::GlueIcon> App::glue_icons() {
    std::vector<GlueIcon> out;
    if (!doc_ || tool_ != Tool::Select || !editable()) return out;
    std::set<uint32_t> group;
    if (glue_mode_) {
        for (const WObj& o : doc_->world.objs) if (o.find(kGlueProp)) group.insert(o.uid());
    } else if (selection_.size() == 1) {
        for (uint32_t u : glue_group(selection_)) group.insert(u);
    }
    for (uint32_t u : group) {
        const WObj* o = object_by_uid(doc_->world, u);
        if (!o) continue;
        for (auto& l : glue_links(*o))
            if (l.mine < 0 && u < l.uid) {
                GlueIcon g{};
                g.pa = glue_anchor_point(u);
                g.pb = glue_anchor_point(l.uid);
                g.p = {0.5f * (g.pa.x + g.pb.x), 0.5f * (g.pa.y + g.pb.y), 0.5f * (g.pa.z + g.pb.z)};
                g.a = u; g.b = l.uid; g.ja = g.jb = -1; g.locked = true; g.line = true;
                out.push_back(g);
            }
    }
    if (selection_.size() != 1 || glue_mode_) return out;
    const uint32_t me = *selection_.begin();
    const WObj* o = object_by_uid(doc_->world, me);
    if (!o) return out;
    const int fam = glue_family(*o);
    std::vector<Joint> mine;
    if (!fam || !object_joints(*o, mine)) return out;
    std::vector<bool> done(mine.size(), false);
    for (auto& l : glue_links(*o))
        if (l.mine >= 0 && l.mine < int(mine.size()) && !done[size_t(l.mine)]) {
            GlueIcon g{};
            g.p = mine[size_t(l.mine)].p; g.a = me; g.b = l.uid; g.ja = l.mine; g.jb = l.theirs; g.locked = true;
            out.push_back(g);
            done[size_t(l.mine)] = true;
        }
    std::vector<Joint> other;
    for (const WObj& w : doc_->world.objs) {
        if (w.uid() == me || glue_family(w) != fam || !object_joints(w, other)) continue;
        for (size_t a = 0; a < mine.size(); a++) {
            if (done[a]) continue;
            for (size_t k = 0; k < other.size(); k++)
                if (joints_meet(mine[a], other[k], fam)) {
                    GlueIcon g{};
                    g.p = mine[a].p; g.a = me; g.b = w.uid(); g.ja = int(a); g.jb = int(k); g.locked = false;
                    out.push_back(g);
                    done[a] = true;
                    break;
                }
        }
    }
    return out;
}

static bool icon_screen(const Camera& cam, float vx, float vy, float vw, float vh, const Vec3& p, ImVec2& s) {
    float sx, sy;
    if (!cam.project({p.x, p.y + 0.3f, p.z}, vw, vh, sx, sy)) return false;
    s = ImVec2(vx + sx, vy + sy);
    return true;
}

bool App::glue_click(bool lclick) {
    if (!lclick || !vp_hover_) return false;
    const ImVec2 m = ImGui::GetIO().MousePos;
    for (const GlueIcon& g : glue_icons()) {
        ImVec2 s;
        if (!icon_screen(cam_, vp_x, vp_y, vp_w, vp_h, g.p, s)) continue;
        if ((m.x - s.x) * (m.x - s.x) + (m.y - s.y) * (m.y - s.y) > 121.f * dpi_ * dpi_) continue;
        push_undo(g.locked ? "unglue" : "glue");
        glue_set(g.a, g.ja, g.b, g.jb, !g.locked);
        // Glued joints meet in 3D: road pieces each rest on their own ground, so two snapped pieces on a slope can be
        // metres apart in height at their joint (playtest 11-04: a ramp hinged from the upper one floated over the lower).
        // The selected piece, and what is glued on its side, moves up or down onto its partner's joint.
        if (!g.locked && g.ja >= 0 && g.jb >= 0) {
            const WObj* oa = object_by_uid(doc_->world, g.a);
            const WObj* ob = object_by_uid(doc_->world, g.b);
            std::vector<Joint> ja, jb;
            if (oa && ob && object_joints(*oa, ja) && object_joints(*ob, jb) && g.ja < int(ja.size()) && g.jb < int(jb.size())) {
                const float dy = jb[size_t(g.jb)].p.y - ja[size_t(g.ja)].p.y;
                if (std::fabs(dy) > 0.01f)
                    for (uint32_t u : glue_reach(g.a, {g.b})) glue_shift(u, {0, dy, 0}, {0, 0, 0}, 0);
            }
        }
        status_ = g.locked ? "Unglued: the two move on their own again."
                           : "Glued: moving, raising, turning or tilting either piece now carries the other. Rotate and Tilt turn it about its first glued joint.";
        status_time_ = 6;
        return true;
    }
    return false;
}

// The Glue tool (Properties > Glue objects together): click one object, then each object to glue to it.
bool App::glue_pick(bool lclick) {
    if (!glue_mode_ || tool_ != Tool::Select || !editable()) return false;
    if (vp_hover_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {  // Esc ends Glue in dispatch_keys (A_CANCEL)
        glue_mode_ = false;
        glue_first_ = 0;
        status_ = "Glue tool off.";
        status_time_ = 3;
        return false;
    }
    if (!lclick) return false;
    if (glue_click(lclick)) return true;  // a lock on a glue line: unglue
    if (!hover_uid_) {
        status_ = "Glue: click an object (a foundation, a building...). " + kt(A_CANCEL) + " or right-click: done.";
        status_time_ = 5;
        return true;
    }
    const WObj* h = object_by_uid(doc_->world, hover_uid_);
    const std::string name = h ? h->description() : std::string();
    if (!glue_first_ || !object_by_uid(doc_->world, glue_first_)) {
        glue_first_ = hover_uid_;
        selection_ = {hover_uid_};
        status_ = "Glue: now click each object to glue to " + name + ". " + kt(A_CANCEL) + " or right-click: done.";
        status_time_ = 30;
        return true;
    }
    if (hover_uid_ == glue_first_) return true;
    const WObj* f = object_by_uid(doc_->world, glue_first_);
    bool already = false;
    if (f) for (auto& l : glue_links(*f)) if (l.mine < 0 && l.uid == hover_uid_) already = true;
    const std::string fname = f ? f->description() : std::string();
    push_undo(already ? "unglue" : "glue");
    glue_set(glue_first_, -1, hover_uid_, -1, !already);
    status_ = (already ? "Unglued " : "Glued ") + name + (already ? " from " : " to ") + fname +
              ". Click more objects to glue them too, or " + kt(A_CANCEL) + " / right-click when done.";
    status_time_ = 30;
    return true;
}

void App::glue_draw(ImDrawList* dl) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    for (const GlueIcon& g : glue_icons()) {
        if (g.line) {
            ImVec2 a, b;
            if (icon_screen(cam_, vp_x, vp_y, vp_w, vp_h, g.pa, a) && icon_screen(cam_, vp_x, vp_y, vp_w, vp_h, g.pb, b)) {
                dl->AddLine(a, b, IM_COL32(0, 0, 0, 170), 5 * dpi_);
                dl->AddLine(a, b, IM_COL32(250, 200, 40, 235), 2.5f * dpi_);
                dl->AddCircleFilled(a, 4 * dpi_, IM_COL32(250, 200, 40, 255));
                dl->AddCircleFilled(b, 4 * dpi_, IM_COL32(250, 200, 40, 255));
            }
        }
        ImVec2 c;
        if (!icon_screen(cam_, vp_x, vp_y, vp_w, vp_h, g.p, c)) continue;
        const bool hov = vp_hover_ && (m.x - c.x) * (m.x - c.x) + (m.y - c.y) * (m.y - c.y) <= 121.f * dpi_ * dpi_;
        const float s = dpi_ * (hov ? 1.2f : 1.f);
        const ImU32 body = g.locked ? IM_COL32(250, 200, 40, 255) : IM_COL32(200, 205, 215, 235);
        const ImU32 edge = IM_COL32(0, 0, 0, 220);
        dl->AddCircleFilled(c, 11 * s, IM_COL32(20, 24, 32, hov ? 230 : 190));
        // Padlock: a shackle (closed: both legs in the body; open: lifted, right leg out) over a body.
        const float bw = 6 * s, bh = 5 * s, top = c.y - 1 * s;
        const float lift = g.locked ? 0.f : 3 * s;
        dl->PathArcTo(ImVec2(c.x, top - 2 * s - lift), 3.5f * s, 3.14159f, 2 * 3.14159f, 10);
        dl->PathStroke(body, 0, 2 * s);
        dl->AddLine(ImVec2(c.x - 3.5f * s, top - 2 * s - lift), ImVec2(c.x - 3.5f * s, top), body, 2 * s);
        if (g.locked) dl->AddLine(ImVec2(c.x + 3.5f * s, top - 2 * s), ImVec2(c.x + 3.5f * s, top), body, 2 * s);
        dl->AddRectFilled(ImVec2(c.x - bw, top), ImVec2(c.x + bw, top + 2 * bh - s), body, 1.5f * s);
        dl->AddRect(ImVec2(c.x - bw, top), ImVec2(c.x + bw, top + 2 * bh - s), edge, 1.5f * s, 0, 1.f);
        if (hov) {
            const char* t = g.locked ? "Glued: click to unglue" : "Click to glue these two pieces together";
            ImVec2 ts = ImGui::CalcTextSize(t);
            ImVec2 p(c.x + 14 * dpi_, c.y - ts.y * 0.5f);
            dl->AddRectFilled(ImVec2(p.x - 4 * dpi_, p.y - 2 * dpi_), ImVec2(p.x + ts.x + 4 * dpi_, p.y + ts.y + 2 * dpi_), IM_COL32(0, 0, 0, 200), 3 * dpi_);
            dl->AddText(p, IM_COL32(255, 255, 255, 255), t);
        }
    }
}

}  // namespace ed

namespace ed {

// Properties: the Glue tool's button, and for one selected object what it is glued to (Unglue all).
void App::glue_tool_ui() {
    if (!doc_ || tool_ != Tool::Select || !editable()) return;
    if (selection_.empty() && !glue_mode_) return;  // nothing to glue yet (the button stood alone under "Nothing selected")
    const float w = ImGui::GetContentRegionAvail().x;
    if (theme::toggle_button(glue_mode_ ? ("Gluing: click objects (" + kt(A_CANCEL) + ": done)").c_str() : "Glue objects together", glue_mode_, ImVec2(w, 0))) {
        glue_mode_ = !glue_mode_;
        glue_first_ = 0;
        if (glue_mode_) {
            if (selection_.size() == 1) glue_first_ = *selection_.begin();
            status_ = glue_first_ ? "Glue: click each object to glue to the selected one. " + kt(A_CANCEL) + " or right-click: done."
                                  : "Glue: click an object (a foundation...), then each object to glue to it (the buildings on it). " + kt(A_CANCEL) + " or right-click: done.";
            status_time_ = 30;
        }
    }
    ImGui::SetItemTooltip("Glued objects move, raise, turn and tilt together as one, wherever you grab them. Click one object, then each object to glue to it; "
                          "click a glued object again (or the lock on its gold line) to unglue it.");
    if (selection_.size() != 1) return;
    const WObj* o = object_by_uid(doc_->world, *selection_.begin());
    if (!o) return;
    const auto ls = glue_links(*o);
    if (ls.empty()) return;
    const size_t group = glue_group(selection_).size() - 1;
    ImGui::TextColored(ImVec4(0.98f, 0.78f, 0.16f, 1), "Glued: %s with it", n_things(group, "object").c_str());
    if (ImGui::Button("Unglue it", ImVec2(w, 0))) {
        push_undo("unglue");
        for (auto& l : ls) glue_set(o->uid(), l.mine, l.uid, l.theirs, false);
        status_ = "Unglued: it moves on its own again.";
        status_time_ = 5;
    }
    ImGui::SetItemTooltip("Remove every glue between this object and the objects it is glued to.");
}

}  // namespace ed

namespace ed {

// A road piece's drawn surface: its four corners on the map and the plane through them (playtest 11-04: tilted roads).
using RoadSurf = App::GroundSurf;

static bool road_surface(const WObj& o, const GpuModel* gm, float w, float l, RoadSurf& s) {
    Vec3 p;
    if (!get_pos(o, p)) return false;
    float x0 = -w, x1 = w, z0 = -l, z1 = 0;
    if (gm && gm->state == GpuModel::Ready && gm->bmax[0] > gm->bmin[0]) { x0 = gm->bmin[0]; x1 = gm->bmax[0]; z0 = gm->bmin[2]; z1 = gm->bmax[2]; }
    if (crop::Rect cr; crop::of(o, cr)) { x0 = std::max(x0, cr.x0); x1 = std::min(x1, cr.x1); z0 = std::max(z0, cr.z0); z1 = std::min(z1, cr.z1); }
    float pitch, roll;
    get_tilt(o, pitch, roll);
    BoxInstance mi;
    orient_instance(mi, heading(o), pitch, roll, p);
    auto wp = [&](float x, float z) { return Vec3{mi.r0.x * x + mi.r0.z * z + mi.r0.w, mi.r1.x * x + mi.r1.z * z + mi.r1.w, mi.r2.x * x + mi.r2.z * z + mi.r2.w}; };
    s.c[0] = wp(x0, z0); s.c[1] = wp(x1, z0); s.c[2] = wp(x1, z1); s.c[3] = wp(x0, z1);
    s.p = p;
    s.n = {mi.r0.y, mi.r1.y, mi.r2.y};
    return std::fabs(s.n.y) > 0.05f;
}

static float plane_y(const RoadSurf& s, float x, float z) { return s.p.y - (s.n.x * (x - s.p.x) + s.n.z * (z - s.p.z)) / s.n.y; }

// 0 inside the footprint on the map, else the distance to it; (bx, bz) = the nearest point of its outline.
static float outside_dist(const RoadSurf& s, float x, float z, float& bx, float& bz) {
    bool in = true;
    float best = 1e30f, area = 0;
    for (int i = 0; i < 4; i++) { const Vec3 &a = s.c[i], &b = s.c[(i + 1) % 4]; area += (b.x - a.x) * (b.z + a.z); }
    for (int i = 0; i < 4; i++) {
        const Vec3 &a = s.c[i], &b = s.c[(i + 1) % 4];
        const float ex = b.x - a.x, ez = b.z - a.z, cr = ex * (z - a.z) - ez * (x - a.x);
        if (std::fabs(cr) > 1e-6f && (area > 0) == (cr > 0)) in = false;
        const float L2 = ex * ex + ez * ez, t = L2 > 0 ? std::clamp(((x - a.x) * ex + (z - a.z) * ez) / L2, 0.f, 1.f) : 0.f;
        const float px = a.x + ex * t, pz = a.z + ez * t, d = std::hypot(px - x, pz - z);
        if (d < best) { best = d; bx = px; bz = pz; }
    }
    return in ? 0.f : best;
}

bool App::road_surf_of(const WObj& o, void* out) {
    if (o.cls != K_ROAD_G) return false;
    std::string def = o.str("RoadDefinition");
    for (char& ch : def) if (ch == 0x5C) ch = '/';
    const RoadPiece* rp = find_road_piece(def);
    if (!rp) return false;
    GpuModel* gm = rp->model.empty() ? nullptr : models_.get(rp->model, "");
    return road_surface(o, gm, rp->w, rp->l, *static_cast<RoadSurf*>(out));
}

float App::road_float_gap(const WObj& o) {
    RoadSurf s;
    if (!doc_ || !road_surf_of(o, &s)) return 0;
    float gap = 0;
    for (int i = 0; i <= 8; i++)
        for (int j = 0; j <= 8; j++) {
            const float a = i / 8.f, b = j / 8.f;
            const float x = (s.c[0].x * (1 - a) + s.c[1].x * a) * (1 - b) + (s.c[3].x * (1 - a) + s.c[2].x * a) * b;
            const float z = (s.c[0].z * (1 - a) + s.c[1].z * a) * (1 - b) + (s.c[3].z * (1 - a) + s.c[2].z * a) * b;
            if (!doc_->terrain.inside(x, z)) continue;
            gap = std::max(gap, plane_y(s, x, z) - place::kSheetLift - doc_->terrain.height_at(x, z));
        }
    return gap;
}

void App::road_tilt_notice() {
    status_ = "Road pieces have no collision: 'Mechs and vehicles walk on the ground under them. Properties > Fit ground to road shapes the ground to match, or leave it floating.";
    status_time_ = 10;
}

// The Fit ground button: hovering it draws what it will change and says it in one line under the button.
void App::fit_button() {
    if (ImGui::Button("Fit ground to road", ImVec2(ImGui::GetContentRegionAvail().x, 0))) fit_ground_to_roads();
    ImGui::SetItemTooltip("Shapes the ground under this road piece (and the ones selected with it or glued to it) to its surface, blending into the ground "
                          "around it, so 'Mechs and vehicles can drive on it. Other buildings and flat pieces keep their ground. %s undoes it.", kt(A_UNDO).c_str());
    if (ImGui::IsItemHovered()) {
        ground_preview_ = fit_plan_selection(nullptr, nullptr);
        ground_preview_frame_ = ImGui::GetFrameCount();
        ground_preview_text_ = ground_edit_note(ground_preview_);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.86f, 0.3f, 1.f));
        ImGui::TextWrapped("%s", ground_preview_text_.c_str());
        ImGui::PopStyleColor();
    }
}

void App::road_ground_ui(WObj& o) {
    if (o.cls != K_ROAD_G || !editable()) return;
    float p, r;
    get_tilt(o, p, r);
    const bool tilted = std::fabs(p) + std::fabs(r) > 0.005f;
    const float gap = road_float_gap(o);
    if (!tilted && gap < 0.75f) {
        // Resting on the ground: no warning, but the ground can still be refitted (after moving or tilting a glued neighbour).
        fit_button();
        return;
    }
    ImGui::Spacing();
    char t[320];
    snprintf(t, sizeof t, "No collision: 'Mechs and vehicles walk on the ground, not on road pieces. This piece %s up to %.1f m above the ground, so units pass under or through it.",
             tilted ? "is tilted and floats" : "floats", std::max(gap, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.78f, 0.16f, 1));
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
    fit_button();
    ImGui::TextDisabled("Or leave it floating: it shows, but nothing can drive on it.");
}

// The plan for Fit ground to road: the grid points under the pieces (+ shoulder, + blend) and what they should become. Writes nothing.
App::GroundEdit App::plan_fit_ground(const std::vector<uint32_t>& roads) {
    GroundEdit e;
    if (!doc_) return e;
    std::vector<RoadSurf> surfs;
    for (uint32_t u : roads) {
        const WObj* o = object_by_uid(doc_->world, u);
        RoadSurf s;
        if (o && road_surf_of(*o, &s)) surfs.push_back(s);
    }
    if (surfs.empty()) return e;
    // Full height out to a shoulder half a terrain cell (+0.5 m) past the road on every side, then a blend into the ground
    // around it over 14 m: with full height only under the road, its sides and ends hung over the slope between the 8 m
    // grid points (playtest 11-04); a whole cell + 20 m made a mound ~30 m past the road (far too much).
    return plan_surfaces(surfs, 0.5f * doc_->terrain.scale + 0.5f);
}

// The ground that fits a set of surfaces: full height out to `shoulder` past each, then a 14 m blend. Writes nothing.
App::GroundEdit App::plan_surfaces(const std::vector<GroundSurf>& surfs, float shoulder) {
    GroundEdit e;
    if (!doc_ || surfs.empty()) return e;
    Terrain& t = doc_->terrain;
    const float margin = 14.f, reach = shoulder + margin, step = (t.max_h - t.min_h) / 255.f;
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (auto& s : surfs)
        for (auto& c : s.c) { x0 = std::min(x0, c.x); x1 = std::max(x1, c.x); z0 = std::min(z0, c.z); z1 = std::max(z1, c.z); }
    e.gx0 = std::clamp(int((x0 - reach) / t.scale), 0, t.w - 1); e.gx1 = std::clamp(int((x1 + reach) / t.scale) + 1, 0, t.w - 1);
    e.gz0 = std::clamp(int((z0 - reach) / t.scale), 0, t.h - 1); e.gz1 = std::clamp(int((z1 + reach) / t.scale) + 1, 0, t.h - 1);
    const int nx = e.gx1 - e.gx0 + 1;
    e.want.assign(size_t(nx) * size_t(e.gz1 - e.gz0 + 1), std::numeric_limits<float>::quiet_NaN());
    e.core.assign(e.want.size(), 0);
    for (int gz = e.gz0; gz <= e.gz1; gz++)
        for (int gx = e.gx0; gx <= e.gx1; gx++) {
            const float x = gx * t.scale, z = gz * t.scale;
            // The nearest road wins (a point under a road takes that road's height): with the highest one winning, the
            // top piece's shoulder covered the top of the ramp joined to it (playtest 11-04 game check).
            float best_w = 0, target = 0, best_d = 1e30f;
            for (auto& s : surfs) {
                float bx = x, bz = z;
                const float d = outside_dist(s, x, z, bx, bz);
                if (d >= reach) continue;
                float wgt = 1.f;
                if (d > shoulder) { const float k = (d - shoulder) / margin; wgt = 1.f - k * k * (3 - 2 * k); }
                // Outside the footprint: the height of the nearest point of its outline (the plane is not carried on).
                const float y = (d > 0 ? plane_y(s, bx, bz) : plane_y(s, x, z)) - place::kSheetLift;  // the road sits kSheetLift above its ground
                if (d < best_d - 0.01f || (std::fabs(d - best_d) <= 0.01f && y > target)) { best_d = d; best_w = wgt; target = y; }
            }
            if (best_w <= 0) continue;
            if (target < t.min_h || target > t.max_h) e.clamped = true;
            target = std::clamp(target, t.min_h, t.max_h);
            float h = t.at(gx, gz);
            if (best_w >= 1.f) {
                float q = t.stored(target);
                if (q > target + 1e-4f) q -= step;  // the stored step at or below the road: the ground never pokes through it
                h = std::max(q, t.min_h);
                e.core[size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0)] = 1;
            } else {
                h += (target - h) * best_w;
            }
            e.want[size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0)] = h;
        }
    return e;
}

float App::GroundEdit::core_change(const Terrain& t) const {
    float m = 0;
    const int nx = gx1 - gx0 + 1;
    for (int gz = gz0; gz <= gz1; gz++)
        for (int gx = gx0; gx <= gx1; gx++) {
            const size_t i = size_t(gz - gz0) * size_t(nx) + size_t(gx - gx0);
            if (i < want.size() && i < core.size() && core[i] && !std::isnan(want[i])) m = std::max(m, std::fabs(std::clamp(want[i], t.min_h, t.max_h) - t.at(gx, gz)));
        }
    return m;
}

// The selected road pieces (and the ones glued to them), planned, protected and forecast.
App::GroundEdit App::fit_plan_selection(std::set<uint32_t>* own, int* road_count) {
    std::vector<uint32_t> roads;
    for (uint32_t u : glue_group(selection_)) {
        const WObj* o = object_by_uid(doc_->world, u);
        RoadSurf s;
        if (o && road_surf_of(*o, &s)) roads.push_back(u);
    }
    if (road_count) *road_count = int(roads.size());
    GroundEdit e = plan_fit_ground(roads);
    const std::set<uint32_t> mine(roads.begin(), roads.end());
    if (own) *own = mine;
    protect_ground(e, mine, Protect::OthersOnly);
    ground_still_ = mine;  // the pieces themselves stay where they are
    forecast_ground(e);
    ground_still_.clear();
    return e;
}

// Tilt at the click: the road just placed is the fixed thing, the ground is shaped to meet it inside the placing's undo step.
bool App::fit_ground_under_placed(uint32_t u, SlopePlan& sp) {
    if (!doc_ || !editable()) return false;
    GroundEdit e = plan_fit_ground({u});
    if (e.empty()) return false;
    const std::set<uint32_t> mine{u};
    protect_ground(e, mine, Protect::OthersOnly);
    ground_still_ = mine;
    for (uint32_t j : e.joined) ground_still_.insert(j);  // The road it joins keeps its place, so the joint has no step
    forecast_ground(e);
    sp.fit_moved = apply_ground_edit(e, "fit ground to road", false);
    ground_still_.clear();
    sp.fit_up = e.up;
    sp.fit_down = e.down;
    if (const WObj* o = object_by_uid(doc_->world, u)) sp.fit_left = road_float_gap(*o);
    return true;
}

void App::fit_ground_to_roads() {
    if (!doc_ || !editable()) return;
    int n = 0;
    std::set<uint32_t> mine;
    GroundEdit e = fit_plan_selection(&mine, &n);
    if (n == 0 || e.empty()) return;
    // A second press changes nothing: the blend into the ground round the piece is made once, with the full-height points.
    if (e.core_change(doc_->terrain) < 0.005f) {
        status_ = std::string("The ground under ") + (n == 1 ? "this road piece" : "these road pieces") + " already fits it.";
        status_time_ = 5;
        return;
    }
    const bool follow = settings_.objects_follow_terrain;
    ground_still_ = mine;
    for (uint32_t j : e.joined) ground_still_.insert(j);  // A road joined to the selection but not in it keeps its place too
    const int moved = apply_ground_edit(e, "fit ground to road");
    ground_still_.clear();
    char msg[420];
    std::string s;
    snprintf(msg, sizeof msg, "Shaped the ground under %d road piece%s (up to +%.1f / -%.1f m).", n, n == 1 ? "" : "s", e.up, e.down);
    s = msg;
    if (follow) {
        if (moved > 0) { snprintf(msg, sizeof msg, " %d object%s moved with it.", moved, moved == 1 ? "" : "s"); s += msg; }
    } else s += " Objects stayed where they were (\"Objects follow the ground\" is off).";
    if (!e.kept.empty()) s += " The ground under " + fit_summary_names(e) + " was left alone.";
    else s += n == 1 ? " 'Mechs and vehicles can drive on it now." : " 'Mechs and vehicles can drive on them now.";
    s += " " + kt(A_UNDO) + " undoes it.";
    if (e.clamped) s += " Part of it is past the map's height limit (Sculpt and Paint tools): the ground stops there.";
    status_ = s;
    status_time_ = 8;
}

}  // namespace ed
