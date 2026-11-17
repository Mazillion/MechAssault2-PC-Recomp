// Transform gizmo: Move / Rotate / Tilt handles on the selection's centroid, drawn over the map and
// used with the left mouse button. One gizmo serves one object or a group (it moves or turns the
// whole group about the centroid). The objects' format has no scale property, so there is no
// Scale mode (water size comes from its model: see the pond tool in water_ux.cpp).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.h"
#include "../core/triggers.h"
#include "imgui.h"
#include "theme.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static const float kDegPerRad = 57.2957795f;
static const float kPi = 3.14159265f;

// Handles.
enum { H_NONE = 0, H_X, H_Z, H_Y, H_PLANE, H_YAW, H_PITCH, H_ROLL };

// Rotation = Ry(heading) * Rx(pitch) * Rz(roll), column vectors; with pitch = roll = 0 it is the
// heading-only matrix the editor always used (local +Z -> (sin h, 0, cos h)).
void orient_instance(BoxInstance& mi, float h, float p, float r, const Vec3& pos) {
    float ch = std::cos(h), sh = std::sin(h), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r);
    mi.r0 = {ch * cr + sh * sp * sr, -ch * sr + sh * sp * cr, sh * cp, pos.x};
    mi.r1 = {cp * sr, cp * cr, -sp, pos.y};
    mi.r2 = {-sh * cr + ch * sp * sr, sh * sr + ch * sp * cr, ch * cp, pos.z};
}

bool App::gizmo_pivot(Vec3& out) const {
    if (!doc_ || selection_.empty()) return false;
    Vec3 sum{};
    int n = 0;
    for (const auto& o : doc_->world.objs) {
        if (!selection_.count(o.uid()) || positionless_trigger(o)) continue;  // its TriggerPoint is unused
        Vec3 p;
        if (!get_pos(o, p)) continue;
        // The handles sit at the middle of the object's footprint on its base (the model bounds), not at its origin, which can be far from
        // the visible model (buildings / bridge pieces whose origin is at a corner or off to the side).
        const size_t oi = size_t(&o - doc_->world.objs.data());
        for (const ObjView& v : views_)
            if (v.index == int(oi) && ((v.kind == 0 && v.model) || v.kind == 5)) {  // road pieces (5): the middle of the piece, not its start end
                BoxInstance mi;
                orient_instance(mi, v.heading, v.pitch, v.roll, v.pos);
                const float cx = 0.5f * (v.lo[0] + v.hi[0]), cy = v.lo[1], cz = 0.5f * (v.lo[2] + v.hi[2]);
                p = {mi.r0.x * cx + mi.r0.y * cy + mi.r0.z * cz + mi.r0.w, mi.r1.x * cx + mi.r1.y * cy + mi.r1.z * cz + mi.r1.w,
                     mi.r2.x * cx + mi.r2.y * cy + mi.r2.z * cz + mi.r2.w};
                break;
            }
        sum.x += p.x; sum.y += p.y; sum.z += p.z;
        n++;
    }
    if (!n) {
        // Selected objects with no position of their own (a waypoint group, an AI group...): the
        // centre of the positioned parts below them, so the handles still show and Zoom works.
        WTree t;
        t.build(doc_->world);
        std::vector<int> stack;
        for (size_t i = 0; i < doc_->world.objs.size(); i++)
            if (selection_.count(doc_->world.objs[i].uid()) && !positionless_trigger(doc_->world.objs[i])) stack.push_back(int(i));
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            for (int k : t.kids[size_t(i)]) stack.push_back(k);
            Vec3 p;
            if (!selection_.count(doc_->world.objs[size_t(i)].uid()) && get_pos(doc_->world.objs[size_t(i)], p)) {
                sum.x += p.x; sum.y += p.y; sum.z += p.z;
                n++;
            }
        }
    }
    if (!n) return false;
    out = {sum.x / float(n), sum.y / float(n), sum.z / float(n)};
    return true;
}

Vec3 App::footprint_offset(int oi) {
    auto offset = [](float h, float pitch, float roll, const float* lo, const float* hi) -> Vec3 {
        BoxInstance mi;
        orient_instance(mi, h, pitch, roll, {0, 0, 0});
        const float cx = 0.5f * (lo[0] + hi[0]), cy = lo[1], cz = 0.5f * (lo[2] + hi[2]);
        return {mi.r0.x * cx + mi.r0.y * cy + mi.r0.z * cz, mi.r1.x * cx + mi.r1.y * cy + mi.r1.z * cz, mi.r2.x * cx + mi.r2.y * cy + mi.r2.z * cz};
    };
    // A cut wall or bridge shows its position and height from the WHOLE model, so cutting an end (or going whole again) never
    // moves the numbers the user did not touch (the cut model's own box changes with the cut).
    if (doc_ && oi >= 0 && oi < int(doc_->world.objs.size())) {
        const WObj& co = doc_->world.objs[size_t(oi)];
        crop::Rect cr;
        if (crop::of(co, cr) && !is_foundation_obj(co)) {
            const GpuModel* gm = whole_model(co);
            if (gm && gm->state == GpuModel::Ready)
                return offset(heading(co), co.f32("CorporalPitch") * 0.0174533f, co.f32("CorporalRoll") * 0.0174533f, gm->bmin, gm->bmax);
        }
    }
    bool drawn = false;
    for (const ObjView& v : views_)
        if (v.index == oi) {
            drawn = true;
            if ((v.kind == 0 && v.model) || v.kind == 5) return offset(v.heading, v.pitch, v.roll, v.lo, v.hi);
        }
    // Not drawn (hidden until a trigger shows it, View > Objects the mission shows later off): the same offset from its
    // model, so the inspector Position does not jump when 'Show it on the map' draws it (M03 Cinema APC 784.4 -> 780.5).
    if (!drawn && show_models_ && doc_ && oi >= 0 && oi < int(doc_->world.objs.size())) {
        const WObj& o = doc_->world.objs[size_t(oi)];
        if (has_ground_model(o)) {
            const GpuModel* gm = obj_model(o);
            return offset(heading(o), o.f32("CorporalPitch") * 0.0174533f, o.f32("CorporalRoll") * 0.0174533f, gm->bmin, gm->bmax);
        }
    }
    return {0, 0, 0};
}

// Moves the camera to the selection (centre of the group) and comes close enough to see it.
void App::zoom_to_selection() {
    Vec3 pv;
    if (!gizmo_pivot(pv)) {
        status_ = selection_positionless() ? std::string("This trigger has no place on the map. Edit it in the Triggers panel on the left.")
                                           : "Select something first, then press " + kt(A_ZOOM_SEL) + " to zoom to it.";
        status_time_ = 5;
        return;
    }
    // Frame the selection's bounding sphere with a margin: the sphere is centred half way up the tallest object, and the
    // camera stands as far back as the field of view needs, close for a barricade and far for a hall (the old rule kept a 45 m
    // distance, so a 4 m barricade stayed a speck and a big building filled the view).
    float hmax = 0;
    for (const auto& v : views_)
        if (v.root >= 0 && selection_.count(doc_->world.objs[size_t(v.root)].uid()) && v.kind != 2 && v.kind != 3) hmax = std::max(hmax, v.hi[1] - v.lo[1]);
    cam_.tx = pv.x;
    cam_.tz = pv.z;
    cam_.ty = pv.y + 0.5f * hmax;
    float r = 0;
    for (const auto& v : views_)
        if (v.root >= 0 && selection_.count(doc_->world.objs[size_t(v.root)].uid())) {
            float dx = v.hi[0] - v.lo[0], dy = v.hi[1] - v.lo[1], dz = v.hi[2] - v.lo[2];
            // Water and trigger areas keep the 4 m default box in lo/hi: frame them by their real extent
            // (F on a 500 m lake came in at 45 m, inside the water).
            if (v.kind == 2) { dx = std::max(dx, v.size.x); dz = std::max(dz, v.size.z); }
            if (v.kind == 3 && v.radius > 0) { dx = std::max(dx, 2 * v.radius); dz = std::max(dz, 2 * v.radius); }
            r = std::max(r, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) + std::hypot(v.pos.x - pv.x, v.pos.z - pv.z));
        }
    if (r <= 0) r = 8.f;  // nothing drawn (a hidden object): a small default
    const float fit = r / std::sin(0.5f * std::min(cam_.fov, 1.2f));  // the sphere just fits the vertical field of view
    cam_.dist = std::clamp(1.2f * fit, 12.f, 1500.f);
}

// Turns every selected object (and its parts) by `delta` radians about the vertical axis through `pivot`.
void App::rotate_group(float delta, const Vec3& pv) {
    WTree t;
    t.build(doc_->world);
    float cs = std::cos(delta), sn = std::sin(delta);
    for (uint32_t u : glue_group(selection_)) {  // what is glued to the selection turns with it
        int ri = t.find(u);
        if (ri < 0) continue;
        std::vector<int> sub;
        t.subtree(ri, sub);
        for (int i : sub) {
            WObj& o = doc_->world.objs[size_t(i)];
            Vec3 p;
            if (get_pos(o, p)) {
                float dx = p.x - pv.x, dz = p.z - pv.z;
                set_pos(o, {pv.x + cs * dx + sn * dz, p.y, pv.z - sn * dx + cs * dz});
            }
            set_heading(o, heading(o) + delta);
            if (WProp* rr = o.find("RoadRotation"))
                if (rr->data.size() >= 4) {
                    float yaw;
                    memcpy(&yaw, rr->data.data(), 4);
                    yaw += delta;
                    memcpy(rr->data.data(), &yaw, 4);
                }
        }
    }
    doc_->dirty = true;
    objects_dirty_ = true;
}

// which 0 = pitch, 1 = roll (CorporalPitch / CorporalRoll, degrees).
void App::tilt_selection(int which, float delta) {
    bool road = false;
    for (auto& o : doc_->world.objs) {
        if (!selection_.count(o.uid()) || !can_tilt(o)) continue;
        float p, r;
        get_tilt(o, p, r);
        (which == 0 ? p : r) += delta;
        set_tilt(o, p, r);
        road |= o.find("RoadRotation") != nullptr;
    }
    if (road) road_tilt_notice();
    mark_selection_fixed();  // a tilted flat piece keeps its height
    doc_->dirty = true;
    objects_dirty_ = true;
}

namespace {

struct Frame {
    XMFLOAT3 right, fwd, up{0, 1, 0};  // the first selected object's axes (tilt rings follow it)
};

float seg_dist(ImVec2 p, ImVec2 a, ImVec2 b) {
    float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
    float c = vx * vx + vy * vy;
    float t = c > 1e-6f ? std::clamp((vx * wx + vy * wy) / c, 0.f, 1.f) : 0.f;
    float dx = a.x + vx * t - p.x, dy = a.y + vy * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool in_quad(ImVec2 p, const ImVec2* q) {
    bool in = false;
    for (int i = 0, j = 3; i < 4; j = i++)
        if (((q[i].y > p.y) != (q[j].y > p.y)) && (p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x)) in = !in;
    return in;
}

float wrap_pi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

}  // namespace

// Everything about where the handles are, shared by drawing, hit-testing and dragging.
struct GizmoGeom {
    Vec3 pv;
    float L = 10;
    Frame fr;
    bool vis = false;
    // The yellow move-along-the-ground square, from sa to sb metres from the pivot on x and z. Usually beside the
    // arrows (0.30 L .. 0.65 L); for a spawn / player start marker it is the marker's own footprint, centred on the
    // pivot (playtest 11-01: the square beside the start's icon looked like the start itself and was confusing to drag).
    float sa = 0, sb = 0;
    bool centred = false;
    XMFLOAT3 ax{1, 0, 0}, az{0, 0, 1};  // the red and blue arrows' directions: the world axes, or the first object's own (Local)
};

static bool screen(const Camera& cam, float vx, float vy, float vw, float vh, const XMFLOAT3& p, ImVec2& out) {
    float sx, sy;
    if (!cam.project(p, vw, vh, sx, sy)) return false;
    out = ImVec2(sx + vx, sy + vy);
    return true;
}

static void make_geom(const App* app, GizmoGeom& g, const Camera& cam, const std::vector<WObj>& objs, const std::set<uint32_t>& sel, bool local) {
    g.L = 10;
    float dx = cam.eye.x - g.pv.x, dy = cam.eye.y - g.pv.y, dz = cam.eye.z - g.pv.z;
    g.L = std::max(2.f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.12f);
    float h = 0;
    for (auto& o : objs)
        if (sel.count(o.uid())) { h = heading(o); break; }
    g.fr.fwd = {std::sin(h), 0, std::cos(h)};
    g.fr.right = {std::cos(h), 0, -std::sin(h)};
    if (local) { g.ax = g.fr.right; g.az = g.fr.fwd; }
    (void)app;
    g.sa = g.L * 0.30f;
    g.sb = g.L * 0.65f;
}

// The selection is one spawn / player start marker (App::gizmo_marker_half > 0): the square is the marker's footprint.
static void marker_square(GizmoGeom& g, float half) {
    if (half <= 0) return;
    g.centred = true;
    g.sa = -half;
    g.sb = half;
}

// Rings: points of a circle of radius R around pv in the plane spanned by unit vectors a and b.
static void ring_points(const Vec3& pv, float R, const XMFLOAT3& a, const XMFLOAT3& b, std::vector<XMFLOAT3>& out) {
    const int N = 64;
    out.clear();
    for (int i = 0; i <= N; i++) {
        float t = float(i) / N * 2 * kPi, c = std::cos(t) * R, s = std::sin(t) * R;
        out.push_back({pv.x + a.x * c + b.x * s, pv.y + a.y * c + b.y * s, pv.z + a.z * c + b.z * s});
    }
}

// The lift knob: a square at the green arrow's tip. When the arrow is seen within 20 degrees of a ground arrow (or end-on, from
// above) it would hide among them, so the knob moves sideways on screen by 24 px, joined to the tip by a thin line.
struct LiftKnob { ImVec2 c, tip, centre; bool offset = false; bool ok = false; };

static LiftKnob lift_knob(const Camera& cam, float vx, float vy, float vw, float vh, const GizmoGeom& g, float dpi) {
    LiftKnob k;
    auto P = [&](const XMFLOAT3& d, ImVec2& o) { return screen(cam, vx, vy, vw, vh, XMFLOAT3{g.pv.x + d.x, g.pv.y + d.y, g.pv.z + d.z}, o); };
    if (!P({0, 0, 0}, k.c) || !P({0, g.L * 0.75f, 0}, k.tip)) return k;
    k.ok = true;
    k.centre = k.tip;
    float ux = k.tip.x - k.c.x, uy = k.tip.y - k.c.y;
    const float ul = std::sqrt(ux * ux + uy * uy);
    float sx = 0, sy = 0;  // where the ground arrows point (the knob goes to the other side)
    bool close = ul < 3.f * dpi;
    for (const XMFLOAT3& a : {g.ax, g.az}) {
        ImVec2 e;
        if (!P({a.x * g.L, 0, a.z * g.L}, e)) continue;
        float gx = e.x - k.c.x, gy = e.y - k.c.y;
        const float gl = std::sqrt(gx * gx + gy * gy);
        if (gl < 3.f * dpi) continue;
        sx += gx / gl; sy += gy / gl;
        if (ul >= 3.f * dpi && (ux * gx + uy * gy) / (ul * gl) > std::cos(20.f * 3.14159265f / 180.f)) close = true;
    }
    if (close) {
        float px = ul > 1e-3f ? -uy / ul : 1.f, py = ul > 1e-3f ? ux / ul : 0.f;
        if (px * sx + py * sy > 0) { px = -px; py = -py; }  // away from the ground arrows
        k.offset = true;
        k.centre = ImVec2(k.tip.x + px * 24.f * dpi, k.tip.y + py * 24.f * dpi);
    }
    return k;
}

// The handle under `m` (0 = none) for the current mode. `knob` is set when the lift handle was hit on its knob.
static int hit_test(const Camera& cam, const App* app, float vx, float vy, float vw, float vh, const GizmoGeom& g, int mode, ImVec2 m, float tol, float dpi, bool* knob) {
    (void)app;
    if (knob) *knob = false;
    auto P = [&](float x, float y, float z, ImVec2& o) { return screen(cam, vx, vy, vw, vh, XMFLOAT3{g.pv.x + x, g.pv.y + y, g.pv.z + z}, o); };
    ImVec2 c;
    if (!P(0, 0, 0, c)) return 0;
    int best = 0;
    float bd = tol;
    auto line_hit = [&](ImVec2 a, ImVec2 b, int id) {
        float d = seg_dist(m, a, b);
        if (d < bd) { bd = d; best = id; }
    };
    if (mode == 0) {
        ImVec2 q[4];
        const bool in_sq = P(g.sa, 0, g.sa, q[0]) && P(g.sb, 0, g.sa, q[1]) && P(g.sb, 0, g.sb, q[2]) && P(g.sa, 0, g.sb, q[3]) && in_quad(m, q);
        // The centred square (a marker) covers the arrows' roots: inside it, it wins; the arrows are grabbed beyond it.
        if (in_sq) return H_PLANE;
        const LiftKnob lk = lift_knob(cam, vx, vy, vw, vh, g, dpi);
        if (lk.ok && std::fabs(m.x - lk.centre.x) <= 8.f * dpi && std::fabs(m.y - lk.centre.y) <= 8.f * dpi) {
            if (knob) *knob = true;
            return H_Y;
        }
        ImVec2 e;
        if (P(g.ax.x * g.L, 0, g.ax.z * g.L, e)) line_hit(c, e, H_X);
        if (P(g.az.x * g.L, 0, g.az.z * g.L, e)) line_hit(c, e, H_Z);
        if (P(0, g.L * 0.75f, 0, e)) line_hit(c, e, H_Y);
        return best;
    }
    std::vector<XMFLOAT3> pts;
    auto ring = [&](const XMFLOAT3& a, const XMFLOAT3& b, int id) {
        ring_points(g.pv, g.L, a, b, pts);
        ImVec2 prev;
        bool have = false;
        for (auto& p : pts) {
            ImVec2 s;
            if (!screen(cam, vx, vy, vw, vh, p, s)) { have = false; continue; }
            if (have) line_hit(prev, s, id);
            prev = s;
            have = true;
        }
    };
    if (mode == 1) {
        ring({1, 0, 0}, {0, 0, 1}, H_YAW);
    } else {
        ring(g.fr.fwd, g.fr.up, H_PITCH);
        ring(g.fr.right, g.fr.up, H_ROLL);
    }
    return best;
}

// The mode actually shown: Tilt falls back to Move when nothing selected has a pitch and roll.
bool App::gizmo_can_tilt() const {
    for (const auto& o : doc_->world.objs)
        if (selection_.count(o.uid()) && can_tilt(o)) return true;
    return false;
}

int App::gizmo_effective_mode() const { return gizmo_mode_ == 2 && !gizmo_can_tilt() ? 0 : gizmo_mode_; }

bool App::gizmo_update(bool lclick, bool ldown) {
    ImGuiIO& io = ImGui::GetIO();
    Vec3 pv;
    // A trigger box or circle has its own handles (app_trigger_gizmo.cpp).
    if (tool_ != Tool::Select || !editable() || single_trigger_shape() || !gizmo_pivot(pv)) {
        if (!ldown) gizmo_drag_ = 0;
        gizmo_hot_ = 0;
        return false;
    }
    // A glued piece turns and tilts about its first glued joint (app_glue.cpp): the rings sit there.
    if (gizmo_effective_mode() != 0)
        if (Vec3 hw; glue_hinge(hw)) pv = hw;
    GizmoGeom g;
    g.pv = pv;
    make_geom(this, g, cam_, doc_->world.objs, selection_, gizmo_local_);
    marker_square(g, gizmo_marker_half());
    ImVec2 m = io.MousePos;
    XMFLOAT3 ro, rd;
    cam_.ray(m.x - vp_x, m.y - vp_y, vp_w, vp_h, ro, rd);
    if (gizmo_drag_) {
        if (!ldown) {
            gizmo_drag_ = 0;
            lift_started_ = false;
            objects_dirty_ = minimap_dirty_ = true;
            return true;
        }
        const Vec3 p0 = gz_.pivot;
        auto ensure_undo = [&](const char* label) {
            if (!gz_.pushed) { push_undo(label); gz_.pushed = true; }
        };
        if (gz_.pushed) readout_label(gizmo_drag_text());  // the label follows the drag; the status line is set where something changes
        switch (gizmo_drag_) {
            case H_X: case H_Z: case H_Y: {
                float total;
                if (gizmo_drag_ == H_Y && gz_.knob) {
                    // The lift knob: the height follows the vertical mouse motion (metres per pixel at the pivot's distance), whatever the view angle.
                    if (gz_.start > 1e29f) { gz_.start = 0; lift_begin(); return true; }
                    total = (gz_.knob_y0 - m.y) * gz_.knob_mpp;
                } else {
                    const XMFLOAT3 A = gizmo_drag_ == H_X ? g.ax : gizmo_drag_ == H_Y ? XMFLOAT3{0, 1, 0} : g.az;
                    // Parameter on the axis line closest to the mouse ray.
                    float wx = p0.x - ro.x, wy = p0.y - ro.y, wz = p0.z - ro.z;
                    float b = A.x * rd.x + A.y * rd.y + A.z * rd.z, d = A.x * wx + A.y * wy + A.z * wz, e = rd.x * wx + rd.y * wy + rd.z * wz;
                    float den = 1.f - b * b;
                    if (den < 1e-4f) return true;
                    // Closest point between the axis line P0 + s*A and the mouse ray O + u*D (A, D unit):
                    // s = (b*e - d) / (1 - b^2) with w0 = P0 - O, b = A.D, d = A.w0, e = D.w0.
                    float s = (b * e - d) / den;
                    if (gz_.start > 1e29f) { gz_.start = s; if (gizmo_drag_ == H_Y) lift_begin(); return true; }
                    total = s - gz_.start;
                }
                if (gizmo_drag_ == H_Y) total = lift_clamp(total);  // not below the surface under it, unless the hold key (Shift) is down
                else {  // The arrows snap to the grid along their own axis only
                    const XMFLOAT3 SA = gizmo_drag_ == H_X ? g.ax : g.az;
                    float st;
                    if (snap_move_axis({SA.x, SA.y, SA.z}, p0, total, st)) total = st;
                }
                float delta = total - gz_.applied;
                if (std::fabs(delta) < 1e-4f) return true;
                ensure_undo("move");
                gz_.applied = total;
                if (gizmo_drag_ == H_Y) {
                    mark_selection_fixed();  // a lifted flat piece keeps that height
                    move_selection({0, delta, 0}, false);
                    const std::string r = lift_readout(total);
                    readout_label(r);
                    if (!lift_stopped_) {  // (a stop already said "Stopped at the ground")
                        status_ = r;
                        status_time_ = 1.f;
                        if (lift_amber_ && holding(settings_.hold_free)) {  // pushed below: say so in amber
                            const size_t at = r.find(" m below ");
                            const size_t from = r.rfind(", ", at);
                            if (at != std::string::npos && from != std::string::npos) {
                                std::string nm = "It";
                                for (uint32_t u : selection_)
                                    if (const WObj* so = object_by_uid(doc_->world, u)) { nm = "The " + (so->description().empty() ? std::string("piece") : so->description()); break; }
                                status_ = nm + " is " + r.substr(from + 2, at - from - 2) + " m under the ground (hidden in the game).";
                                status_amber_ = status_;
                                status_time_ = 3;
                            }
                        }
                    }
                } else {
                    const XMFLOAT3 A = gizmo_drag_ == H_X ? g.ax : g.az;
                    move_selection({A.x * delta, 0, A.z * delta}, true);
                    set_readout(gizmo_drag_text());
                }
                return true;
            }
            case H_PLANE: {
                if (std::fabs(rd.y) < 1e-4f) return true;
                float t = (p0.y - ro.y) / rd.y;
                if (t < 0) return true;
                Vec3 h{ro.x + rd.x * t, p0.y, ro.z + rd.z * t};
                Vec3 total{h.x - gz_.hit0.x, 0, h.z - gz_.hit0.z};
                Vec3 delta{total.x - gz_.applied_v.x, 0, total.z - gz_.applied_v.z};
                if (std::fabs(delta.x) + std::fabs(delta.z) < 1e-4f) return true;
                ensure_undo("move");
                gz_.applied_v = total;
                if (move_snap_.on) move_snap_step(h.x, h.z);   // Joints and the grid, like a body drag
                else move_selection(delta, true);
                set_readout(gizmo_drag_text());
                return true;
            }
            case H_YAW: case H_PITCH: case H_ROLL: {
                // Ring basis (a, b) with the turn angle t: point = pivot + R (cos t a + sin t b).
                XMFLOAT3 a, b, n;
                if (gizmo_drag_ == H_YAW) { a = {0, 0, 1}; b = {1, 0, 0}; n = {0, 1, 0}; }
                else if (gizmo_drag_ == H_PITCH) { a = g.fr.fwd; b = {0, -1, 0}; n = g.fr.right; }
                else { a = g.fr.right; b = {0, 1, 0}; n = g.fr.fwd; }
                float den = n.x * rd.x + n.y * rd.y + n.z * rd.z;
                float ang = 0;
                bool plane = std::fabs(den) >= 0.25f;
                if (plane) {  // the mouse ray meets the ring's plane: the angle is where it lands
                    float t = (n.x * (p0.x - ro.x) + n.y * (p0.y - ro.y) + n.z * (p0.z - ro.z)) / den;
                    if (t < 0) return true;
                    float qx = ro.x + rd.x * t - p0.x, qy = ro.y + rd.y * t - p0.y, qz = ro.z + rd.z * t - p0.z;
                    ang = std::atan2(qx * b.x + qy * b.y + qz * b.z, qx * a.x + qy * a.y + qz * a.z);
                    if (gz_.start > 1e29f || !gz_.plane) { gz_.start = ang; gz_.t = ang; gz_.plane = true; return true; }
                    float d = wrap_pi(ang - gz_.start);
                    gz_.start = ang;
                    gz_.t = ang;
                    gz_.accum += d;
                } else {  // plane seen (nearly) edge-on: follow the ring's tangent on screen instead
                    auto P = [&](float t, ImVec2& o2) {
                        float c = std::cos(t) * g.L, sn = std::sin(t) * g.L;
                        return screen(cam_, vp_x, vp_y, vp_w, vp_h, XMFLOAT3{p0.x + a.x * c + b.x * sn, p0.y + a.y * c + b.y * sn, p0.z + a.z * c + b.z * sn}, o2);
                    };
                    ImVec2 s0, s1;
                    if (!P(gz_.t, s0) || !P(gz_.t + 0.02f, s1)) return true;
                    float vx = s1.x - s0.x, vy = s1.y - s0.y, len = std::sqrt(vx * vx + vy * vy);
                    gz_.plane = false;
                    gz_.start = 1e30f;
                    if (len < 1e-3f) return true;
                    float pxrad = std::max(len / 0.02f, 40.f * dpi_);
                    float d = std::clamp((io.MouseDelta.x * vx + io.MouseDelta.y * vy) / len / pxrad, -0.3f, 0.3f);
                    gz_.t += d;
                    gz_.accum += d;
                }
                float snap = 15.f / kDegPerRad;
                float target = holding(settings_.hold_quarter) ? std::round(gz_.accum / snap) * snap : gz_.accum;
                if (gizmo_drag_ == H_YAW && angle_step_rad() > 0 && !holding(settings_.hold_free)) {
                    // Absolute steps: the heading lands on a multiple of the step from north, not 15 degrees from wherever it started.
                    const float st = angle_step_rad();
                    target = std::round((gz_.h0 + gz_.accum) / st) * st - gz_.h0;
                }
                float delta = target - gz_.applied;
                if (std::fabs(delta) < 1e-5f) return true;
                ensure_undo(gizmo_drag_ == H_YAW ? "rotate" : "tilt");
                gz_.applied = target;
                if (gizmo_drag_ == H_YAW) { if (!glue_turn(0, delta, p0)) rotate_group(delta, p0); }
                else if (!glue_turn(gizmo_drag_ == H_PITCH ? 1 : 2, delta, p0)) tilt_selection(gizmo_drag_ == H_PITCH ? 0 : 1, delta);
                set_readout(gizmo_drag_text());
                return true;
            }
        }
        return true;
    }
    if (glue_click(lclick)) return true;  // a lock icon on a joint (app_glue.cpp) wins over the handles under it
    bool on_knob = false;
    gizmo_hot_ = vp_hover_ && !size_hot_ ? hit_test(cam_, this, vp_x, vp_y, vp_w, vp_h, g, gizmo_effective_mode(), m, 10.f * dpi_, dpi_, &on_knob) : 0;
    if (gizmo_hot_) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (gizmo_hot_ && lclick) {
        gizmo_drag_ = gizmo_hot_;
        gz_ = GizmoDrag{};
        gz_.pivot = pv;
        gz_.start = 1e30f;
        if (gizmo_drag_ == H_Y && on_knob) {
            gz_.knob = true;
            gz_.knob_y0 = m.y;
            const float ddx = cam_.eye.x - pv.x, ddy = cam_.eye.y - pv.y, ddz = cam_.eye.z - pv.z;
            gz_.knob_mpp = 2.f * std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz) * std::tan(cam_.fov * 0.5f) / std::max(1.f, vp_h);
        }
        if (gizmo_drag_ == H_YAW)
            for (uint32_t u : selection_)
                if (const WObj* so = object_by_uid(doc_->world, u); so && so->find("CorporalHeading")) { gz_.h0 = world::heading(*so); break; }
        if (gizmo_drag_ >= H_YAW) {
            // Start on the ring point nearest the mouse (used when the ring is seen edge-on).
            XMFLOAT3 a2, b2;
            if (gizmo_drag_ == H_YAW) { a2 = {0, 0, 1}; b2 = {1, 0, 0}; }
            else if (gizmo_drag_ == H_PITCH) { a2 = g.fr.fwd; b2 = {0, -1, 0}; }
            else { a2 = g.fr.right; b2 = {0, 1, 0}; }
            float best = 1e30f;
            for (int i = 0; i < 128; i++) {
                float t = float(i) / 128 * 2 * kPi, c = std::cos(t) * g.L, sn = std::sin(t) * g.L;
                ImVec2 sp;
                if (!screen(cam_, vp_x, vp_y, vp_w, vp_h, XMFLOAT3{pv.x + a2.x * c + b2.x * sn, pv.y + a2.y * c + b2.y * sn, pv.z + a2.z * c + b2.z * sn}, sp)) continue;
                float d2 = (sp.x - m.x) * (sp.x - m.x) + (sp.y - m.y) * (sp.y - m.y);
                if (d2 < best) { best = d2; gz_.t = t; }
            }
        }
        if (gizmo_drag_ == H_PLANE && std::fabs(rd.y) > 1e-4f) {
            float t = (pv.y - ro.y) / rd.y;
            gz_.hit0 = {ro.x + rd.x * t, pv.y, ro.z + rd.z * t};
            Vec3 tpos;
            for (uint32_t u : selection_)   // The first selected object is the anchor that snaps
                if (const WObj* so = object_by_uid(doc_->world, u); so && get_pos(*so, tpos)) { begin_move_snap(u, gz_.hit0.x, gz_.hit0.z); break; }
        }
        return true;
    }
    return false;
}

// "Heading 105 deg (+15)": the value the drag in progress gives, and how far it has moved from where it began.
std::string App::gizmo_drag_text() const {
    char t[200];
    if (gizmo_drag_ == H_YAW) {
        float h = 0;
        for (uint32_t u : selection_)
            if (const WObj* o = object_by_uid(doc_->world, u); o && o->find("CorporalHeading")) { h = world::heading(*o); break; }
        snprintf(t, sizeof t, "Heading %.0f deg (%+.0f)", std::fmod(std::fmod(h * kDegPerRad, 360.f) + 360.f, 360.f), gz_.applied * kDegPerRad);
    } else if (gizmo_drag_ == H_PITCH || gizmo_drag_ == H_ROLL) {
        float v = 0;
        for (uint32_t u : selection_)
            if (const WObj* o = object_by_uid(doc_->world, u); o && can_tilt(*o)) { float p, r; get_tilt(*o, p, r); v = gizmo_drag_ == H_PITCH ? p : r; break; }
        snprintf(t, sizeof t, "%s %.0f deg (%+.0f)", gizmo_drag_ == H_PITCH ? "Tip" : "Lean", v, gz_.applied * kDegPerRad);
    } else if (gizmo_drag_ == H_PLANE) snprintf(t, sizeof t, "Move %.1f, %.1f m", gz_.applied_v.x, gz_.applied_v.z);
    else if (gizmo_drag_ == H_Y) return readout_;  // set with the height it gave (lift_readout)
    else snprintf(t, sizeof t, "Move %.1f m", gz_.applied);
    return t;
}

// The floating label: beside the mouse while something is dragged, else beside the handles (a key nudge), for about a second.
void App::draw_readout() {
    if (readout_.empty() || ImGui::GetTime() > readout_until_) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    ImVec2 at = ImGui::GetIO().MousePos;
    if (!(gizmo_drag_ || dragging_ || water_drag_)) {
        Vec3 pv;
        float sx, sy;
        if (!gizmo_pivot(pv) || !cam_.project({pv.x, pv.y, pv.z}, vp_w, vp_h, sx, sy)) { dl->PopClipRect(); return; }
        at = ImVec2(vp_x + sx - 14 * dpi_, vp_y + sy - 40 * dpi_);
    }
    ImVec2 ts = ImGui::CalcTextSize(readout_.c_str());
    dl->AddRectFilled(ImVec2(at.x + 14 * dpi_, at.y + 14 * dpi_), ImVec2(at.x + 22 * dpi_ + ts.x, at.y + 18 * dpi_ + ts.y), IM_COL32(0, 0, 0, 190), 3 * dpi_);
    dl->AddText(ImVec2(at.x + 18 * dpi_, at.y + 16 * dpi_), gizmo_drag_ == H_Y && lift_amber_ ? IM_COL32(255, 190, 60, 255) : IM_COL32(255, 255, 255, 255), readout_.c_str());
    dl->PopClipRect();
}

// Top of the Properties panel while something is selected: which handles the gizmo shows, Deselect all and Zoom
// (nothing floats over the 3D view).
void App::transform_tools_ui() {
    if (!(tool_ == Tool::Select && editable() && !selection_.empty())) return;
    static const char* names[3] = {"Move", "Rotate", "Tilt"};
    const std::string step = mod_name(settings_.hold_quarter);
    const std::string tips[3] = {"Move: drag an arrow along one direction, or the yellow square along the ground. The green arrow lifts it. (" + kt(A_GIZMO_MOVE) + ")",
                                 "Rotate: drag the ring to turn it (" + step + " = steps of 15 degrees). A group turns about its centre. (" + kt(A_GIZMO_ROTATE) + ")",
                                 "Tilt: drag the red ring to tip it forward or back, the blue ring to lean it sideways (" + step + " = 15 degree steps). (" + kt(A_GIZMO_TILT) + ")"};
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    // A trigger box or circle: its handles do everything (no Move / Rotate / Tilt modes).
    if (!single_trigger_shape() && !selection_positionless()) {  // no place on the map: nothing to move
        const float w = (ImGui::GetContentRegionAvail().x - 2 * sp) / 3;
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            bool on = gizmo_mode_ == i;
            if (i == 2 && !gizmo_can_tilt()) ImGui::BeginDisabled();
            if (theme::toggle_button(names[i], on, ImVec2(w, 0))) gizmo_mode_ = i;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", i == 2 && !gizmo_can_tilt() ? "Tilt is greyed out because nothing selected can lean: spawn points, paths and trigger areas always stay level. Select a building, tree or prop to tilt it." : tips[i].c_str());
            if (i == 2 && !gizmo_can_tilt()) ImGui::EndDisabled();
        }
    }
    if (!single_trigger_shape() && !selection_positionless()) {
        // World / Local: which way the red and blue Move arrows point; a group turns about its centre unless told to turn each in place.
        if (gizmo_mode_ == 0) {
            const float hw = (ImGui::GetContentRegionAvail().x - sp) / 2;
            if (theme::toggle_button("World", !gizmo_local_, ImVec2(hw, 0))) gizmo_local_ = false;
            ImGui::SetItemTooltip("The red and blue Move arrows point along the map's own east and north.");
            ImGui::SameLine();
            if (theme::toggle_button("Local", gizmo_local_, ImVec2(hw, 0))) gizmo_local_ = true;
            ImGui::SetItemTooltip("The red and blue Move arrows follow the object's own heading (the first one selected, for a group).");
        }
        if (selection_.size() > 1) {
            ImGui::Checkbox("Turn each in place", &turn_each_);
            ImGui::SetItemTooltip("%s / %s turn a group about its centre. Tick this to turn each object where it stands.", kt(A_TURN_LEFT).c_str(), kt(A_TURN_RIGHT).c_str());
        }
    }
    const bool zoom = !selection_positionless();
    const float w2 = zoom ? (ImGui::GetContentRegionAvail().x - sp) / 2 : 0;
    if (ImGui::Button("Deselect all", ImVec2(w2, 0))) selection_.clear();
    ImGui::SetItemTooltip("Clear the selection (also: right-click an empty spot, or %s).", kt(A_CANCEL).c_str());
    if (zoom) {
        ImGui::SameLine();
        if (ImGui::Button(act_text(A_ZOOM_SEL).c_str(), ImVec2(w2, 0))) zoom_to_selection();
        ImGui::SetItemTooltip("Bring the camera to the selection (%s).", kt(A_ZOOM_SEL).c_str());
    }
    glue_tool_ui();
    ImGui::Separator();
}

void App::gizmo_draw() {
    knob_x_ = knob_y_ = pivot_x_ = pivot_y_ = -1;
    label_hits_.clear();  // refilled below with the helper labels drawn this frame: pick_object lets them take the click
    if (!doc_) return;
    if (ground_preview_frame_ >= ImGui::GetFrameCount() - 1) draw_ground_edit(ground_preview_);  // a ground tool's plan while its button is hovered
    draw_slope_ghost();  // a flat piece being placed on a slope in Float: stilt lines and the gap
    if (r_.params.grid && vp_w > 300.f) {  // the display grid says how far apart its lines are; the snap grid has its own size on the snap bar
        const char* t = "Map grid: lines 64 m apart";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        ImDrawList* fl = ImGui::GetBackgroundDrawList();
        const ImVec2 at(vp_x + vp_w - ts.x - 14.f * dpi_, vp_y + vp_h - ts.y - 10.f * dpi_);
        fl->AddRectFilled(ImVec2(at.x - 4 * dpi_, at.y - 2 * dpi_), ImVec2(at.x + ts.x + 4 * dpi_, at.y + ts.y + 2 * dpi_), IM_COL32(0, 0, 0, 110), 3 * dpi_);
        fl->AddText(at, IM_COL32(215, 225, 225, 200), t);
    }
    // Water tiles: a blue diamond at each centre (click it to select the tile; the surface works too).
    {
        ImDrawList* bl = ImGui::GetBackgroundDrawList();
        bl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
        for (auto& v : views_) {
            if (v.kind != 2) continue;
            float sx, sy;
            if (!cam_.project({v.pos.x, v.pos.y, v.pos.z}, vp_w, vp_h, sx, sy)) continue;
            ImVec2 c(sx + vp_x, sy + vp_y);
            bool sel = selection_.count(doc_->world.objs[size_t(v.root)].uid()) != 0;
            bool hov = hover_uid_ && doc_->world.objs[size_t(v.root)].uid() == hover_uid_;
            if (!show_helpers_ && !sel) continue;  // H hides the blue handle unless this water is selected
            float r = (hov || sel ? 10.f : 7.f) * dpi_;
            ImVec2 q[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
            bl->AddConvexPolyFilled(q, 4, sel ? IM_COL32(255, 235, 90, 235) : IM_COL32(70, 150, 235, 220));
            bl->AddPolyline(q, 4, IM_COL32(255, 255, 255, 230), ImDrawFlags_Closed, 1.5f * dpi_);
            if (hov || sel) bl->AddText(ImVec2(c.x + r + 4 * dpi_, c.y - 7 * dpi_), IM_COL32(255, 255, 255, 240), "Water");
        }
        // Vehicle pads and power-ups: their name over the ring (app.cpp), on a dark plate; H (helpers off) hides them.
        // A label that would cover one already drawn is left out (pads stand in clusters); hovered / selected go first.
        std::vector<ImVec4> placed;
        for (int pass = 0; pass < 2 && show_helpers_; pass++)
            for (auto& v : views_) {
                if (v.tag.empty() || v.index != v.root) continue;
                uint32_t u = doc_->world.objs[size_t(v.root)].uid();
                bool front = selection_.count(u) || (hover_uid_ && u == hover_uid_);
                if (front != (pass == 0)) continue;
                float sx, sy;
                if (!cam_.project({v.pos.x, v.pos.y + 2.f, v.pos.z}, vp_w, vp_h, sx, sy)) continue;
                bool pad = v.tag.rfind("Pad: ", 0) == 0;
                ImVec2 ts = ImGui::CalcTextSize(v.tag.c_str());
                ImVec2 p(vp_x + sx - ts.x * 0.5f, vp_y + sy - ts.y - 8 * dpi_);
                float pd = 3 * dpi_;
                // A label of something near the view's edge slides inwards so its whole text shows (it was cut at the panel edge).
                if (sx >= -10 && sx <= vp_w + 10 && ts.x + 2 * pd + 4 * dpi_ < vp_w) p.x = std::clamp(p.x, vp_x + pd + 2 * dpi_, vp_x + vp_w - ts.x - pd - 2 * dpi_);
                ImVec4 rc(p.x - pd, p.y - pd * 0.5f, p.x + ts.x + pd, p.y + ts.y + pd * 0.5f);
                if (std::any_of(placed.begin(), placed.end(), [&](const ImVec4& o) { return rc.x < o.z && o.x < rc.z && rc.y < o.w && o.y < rc.w; })) continue;
                placed.push_back(rc);
                label_hits_.push_back({rc.x, rc.y, rc.z, rc.w, v.root, u, v.tag});
                bl->AddRectFilled(ImVec2(rc.x, rc.y), ImVec2(rc.z, rc.w), IM_COL32(0, 0, 0, 140), 3 * dpi_);
                bl->AddText(p, pad ? IM_COL32(120, 230, 230, 255) : IM_COL32(250, 230, 90, 255), v.tag.c_str());
            }
        glue_draw(bl);  // glue lines and locks (app_glue.cpp), also while nothing is selected (Glue tool)
        bl->PopClipRect();
    }
    paste_ghost_draw();
    draw_snap_feedback(ImGui::GetForegroundDrawList());
    bool show = tool_ == Tool::Select && editable() && !selection_.empty();
    Vec3 pv;
    if (!show || single_trigger_shape() || !gizmo_pivot(pv)) return;
    if (gizmo_effective_mode() != 0)
        if (Vec3 hw; glue_hinge(hw)) pv = hw;
    GizmoGeom g;
    g.pv = pv;
    make_geom(this, g, cam_, doc_->world.objs, selection_, gizmo_local_);
    marker_square(g, gizmo_marker_half());
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    // A small object (a barricade, a rock) is smaller than the handles: thin lines, a see-through square and a smaller arrow head, so the model
    // stays visible under them. Grabbing is unchanged.
    float obj_r = 0;
    for (const auto& v : views_)
        if (v.root >= 0 && selection_.count(doc_->world.objs[size_t(v.root)].uid()) && v.kind != 2 && v.kind != 3) {
            const float ex = v.hi[0] - v.lo[0], ey = v.hi[1] - v.lo[1], ez = v.hi[2] - v.lo[2];
            obj_r = std::max(obj_r, 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez));
        }
    const bool tiny = obj_r > 0 && selection_.size() == 1 && obj_r < 0.7f * g.L;
    float th = (tiny ? 1.6f : 3.f) * dpi_;
    auto P = [&](float x, float y, float z, ImVec2& o) { return screen(cam_, vp_x, vp_y, vp_w, vp_h, XMFLOAT3{pv.x + x, pv.y + y, pv.z + z}, o); };
    ImVec2 c;
    if (!P(0, 0, 0, c)) { dl->PopClipRect(); return; }
    int active = gizmo_drag_ ? gizmo_drag_ : gizmo_hot_;
    auto col = [&](int id, ImU32 base) { return id == active ? IM_COL32(255, 235, 90, 255) : base; };
    // A selected water's up arrow and knob are its Level handle, drawn in a water blue (the Z arrow is a deeper blue).
    bool one_water = false;
    if (selection_.size() == 1 && doc_)
        if (const WObj* wo = object_by_uid(doc_->world, *selection_.begin())) one_water = wo->cls == K_WATER;
    const ImU32 red = IM_COL32(235, 70, 70, 255), blue = IM_COL32(70, 120, 245, 255), green = one_water ? IM_COL32(60, 205, 255, 255) : IM_COL32(90, 215, 90, 255);
    dl->AddCircleFilled(c, (tiny ? 2.5f : 4.f) * dpi_, IM_COL32(255, 255, 255, 230));
    pivot_x_ = c.x; pivot_y_ = c.y;
    const int mode = gizmo_effective_mode();
    if (mode == 0) {
        struct Ax { int id; float x, y, z; ImU32 color; const char* label; };
        Ax axes[3] = {{H_X, g.ax.x * g.L, 0, g.ax.z * g.L, red, "X"}, {H_Z, g.az.x * g.L, 0, g.az.z * g.L, blue, "Z"}, {H_Y, 0, g.L * 0.75f, 0, green, "up"}};
        for (auto& a : axes) {
            ImVec2 e;
            if (!P(a.x, a.y, a.z, e)) continue;
            ImU32 cc = col(a.id, a.color);
            dl->AddLine(c, e, IM_COL32(0, 0, 0, 160), th + 2 * dpi_);
            dl->AddLine(c, e, cc, th);
            float dx = e.x - c.x, dy = e.y - c.y, len = std::sqrt(dx * dx + dy * dy);
            if (a.id == H_Y) {  // the lift knob: a square at the tip, moved aside when the arrow would hide among the ground arrows
                const LiftKnob lk = lift_knob(cam_, vp_x, vp_y, vp_w, vp_h, g, dpi_);
                if (lk.ok) {
                    knob_x_ = lk.centre.x; knob_y_ = lk.centre.y;
                    const float hs = 6.f * dpi_;
                    if (lk.offset) {
                        dl->AddLine(lk.tip, lk.centre, IM_COL32(0, 0, 0, 160), 3.f * dpi_);
                        dl->AddLine(lk.tip, lk.centre, cc, 1.5f * dpi_);
                    }
                    dl->AddRectFilled(ImVec2(lk.centre.x - hs - dpi_, lk.centre.y - hs - dpi_), ImVec2(lk.centre.x + hs + dpi_, lk.centre.y + hs + dpi_), IM_COL32(0, 0, 0, 200), 2 * dpi_);
                    dl->AddRectFilled(ImVec2(lk.centre.x - hs, lk.centre.y - hs), ImVec2(lk.centre.x + hs, lk.centre.y + hs), cc, 2 * dpi_);
                }
                continue;
            }
            if (len > 1) {
                dx /= len; dy /= len;
                float s = (tiny ? 8 : 11) * dpi_;
                dl->AddTriangleFilled(ImVec2(e.x + dx * s, e.y + dy * s), ImVec2(e.x - dx * s * 0.2f - dy * s * 0.55f, e.y - dy * s * 0.2f + dx * s * 0.55f),
                                      ImVec2(e.x - dx * s * 0.2f + dy * s * 0.55f, e.y - dy * s * 0.2f - dx * s * 0.55f), cc);
            }
        }
        ImVec2 q[4];
        if (P(g.sa, 0, g.sa, q[0]) && P(g.sb, 0, g.sa, q[1]) && P(g.sb, 0, g.sb, q[2]) && P(g.sa, 0, g.sb, q[3])) {
            ImU32 cc = col(H_PLANE, IM_COL32(255, 210, 60, 255));
            // A centred square sits on the marker itself: lighter fill so the marker shows through.
            dl->AddConvexPolyFilled(q, 4, (cc & 0x00FFFFFFu) | (tiny ? 0x1Cu << 24 : g.centred ? 0x50000000u : 0x80000000u));
            dl->AddPolyline(q, 4, cc, ImDrawFlags_Closed, 2 * dpi_);
        }
    } else {
        std::vector<XMFLOAT3> pts;
        // A ring seen within 15 degrees of edge-on is a thin sliver: it gets a 20 px band behind it, which is also what the hit test grabs.
        const float vdx = pv.x - cam_.eye.x, vdy = pv.y - cam_.eye.y, vdz = pv.z - cam_.eye.z, vdl = std::max(1e-4f, std::sqrt(vdx * vdx + vdy * vdy + vdz * vdz));
        auto ring = [&](const XMFLOAT3& a, const XMFLOAT3& b, int id, ImU32 base) {
            ring_points(pv, g.L, a, b, pts);
            std::vector<ImVec2> sp;
            for (auto& p : pts) {
                ImVec2 s;
                if (screen(cam_, vp_x, vp_y, vp_w, vp_h, p, s)) sp.push_back(s);
            }
            if (sp.size() < 2) return;
            const XMFLOAT3 n{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
            if (std::fabs(n.x * vdx + n.y * vdy + n.z * vdz) / vdl < std::sin(15.f / kDegPerRad))
                dl->AddPolyline(sp.data(), int(sp.size()), (col(id, base) & 0x00FFFFFFu) | 0x50000000u, 0, 20.f * dpi_);
            dl->AddPolyline(sp.data(), int(sp.size()), IM_COL32(0, 0, 0, 160), 0, th + 2 * dpi_);
            dl->AddPolyline(sp.data(), int(sp.size()), col(id, base), 0, th);
        };
        if (mode == 1) {
            ring({1, 0, 0}, {0, 0, 1}, H_YAW, green);
        } else {
            ring(g.fr.fwd, g.fr.up, H_PITCH, red);
            ring(g.fr.right, g.fr.up, H_ROLL, blue);
        }
        // Facing arrow of the first selected object.
        ImVec2 e;
        if (P(g.fr.fwd.x * g.L * 1.25f, 0, g.fr.fwd.z * g.L * 1.25f, e)) {
            dl->AddLine(c, e, IM_COL32(255, 255, 255, 200), 2 * dpi_);
            dl->AddCircleFilled(e, 4 * dpi_, IM_COL32(255, 255, 255, 230));
        }
    }
    draw_readout();
    dl->PopClipRect();
}

}  // namespace ed
