// Transform gizmo: Move / Rotate / Tilt handles on the selection's centroid, drawn over the map and
// used with the left mouse button. One gizmo serves one object or a group (it moves or turns the
// whole group about the centroid). The objects' format has no scale property, so there is no
// Scale mode (water size comes from its model: see the pond tool in water_ux.cpp).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.h"
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
        if (!selection_.count(o.uid()) || positionless_trigger(o)) continue;  // its TriggerPoint is unused (O5)
        Vec3 p;
        if (!get_pos(o, p)) continue;
        // The handles sit at the middle of the object's footprint on its base (the model bounds), not at its origin, which can be far from
        // the visible model (U1: buildings / bridge pieces whose origin is at a corner or off to the side).
        const size_t oi = size_t(&o - doc_->world.objs.data());
        for (const ObjView& v : views_)
            if (v.index == int(oi) && ((v.kind == 0 && v.model) || v.kind == 5)) {  // road pieces (5): the middle of the piece, not its start end (R9)
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
        status_ = selection_positionless() ? "This trigger has no place on the map. Edit it in the Triggers panel on the left."
                                           : "Select something first, then press F to zoom to it.";
        status_time_ = 5;
        return;
    }
    cam_.tx = pv.x;
    cam_.tz = pv.z;
    cam_.ty = pv.y;
    cam_.dist = std::min(cam_.dist, selection_.size() > 1 ? 120.f : 45.f);
    // Far enough to see the whole thing: at 45 m the camera sat inside a big building or bridge and it vanished.
    float r = 0;
    for (const auto& v : views_)
        if (v.root >= 0 && selection_.count(doc_->world.objs[size_t(v.root)].uid())) {
            float dx = v.hi[0] - v.lo[0], dy = v.hi[1] - v.lo[1], dz = v.hi[2] - v.lo[2];
            // Water and trigger areas keep the 4 m default box in lo/hi: frame them by their real extent
            // (review: F on a 500 m lake came in at 45 m, inside the water).
            if (v.kind == 2) { dx = std::max(dx, v.size.x); dz = std::max(dz, v.size.z); }
            if (v.kind == 3 && v.radius > 0) { dx = std::max(dx, 2 * v.radius); dz = std::max(dz, 2 * v.radius); }
            r = std::max(r, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) + std::hypot(v.pos.x - pv.x, v.pos.z - pv.z));
        }
    cam_.dist = std::max(cam_.dist, std::min(2.2f * r, 1500.f));
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
};

static bool screen(const Camera& cam, float vx, float vy, float vw, float vh, const XMFLOAT3& p, ImVec2& out) {
    float sx, sy;
    if (!cam.project(p, vw, vh, sx, sy)) return false;
    out = ImVec2(sx + vx, sy + vy);
    return true;
}

static void make_geom(const App* app, GizmoGeom& g, const Camera& cam, const std::vector<WObj>& objs, const std::set<uint32_t>& sel) {
    g.L = 10;
    float dx = cam.eye.x - g.pv.x, dy = cam.eye.y - g.pv.y, dz = cam.eye.z - g.pv.z;
    g.L = std::max(2.f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.12f);
    float h = 0;
    for (auto& o : objs)
        if (sel.count(o.uid())) { h = heading(o); break; }
    g.fr.fwd = {std::sin(h), 0, std::cos(h)};
    g.fr.right = {std::cos(h), 0, -std::sin(h)};
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

// The handle under `m` (0 = none) for the current mode.
static int hit_test(const Camera& cam, const App* app, float vx, float vy, float vw, float vh, const GizmoGeom& g, int mode, ImVec2 m, float tol) {
    (void)app;
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
        ImVec2 e;
        if (P(g.L, 0, 0, e)) line_hit(c, e, H_X);
        if (P(0, 0, g.L, e)) line_hit(c, e, H_Z);
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
    make_geom(this, g, cam_, doc_->world.objs, selection_);
    marker_square(g, gizmo_marker_half());
    ImVec2 m = io.MousePos;
    XMFLOAT3 ro, rd;
    cam_.ray(m.x - vp_x, m.y - vp_y, vp_w, vp_h, ro, rd);
    if (gizmo_drag_) {
        if (!ldown) {
            gizmo_drag_ = 0;
            objects_dirty_ = minimap_dirty_ = true;
            return true;
        }
        const Vec3 p0 = gz_.pivot;
        auto ensure_undo = [&](const char* label) {
            if (!gz_.pushed) { push_undo(label); gz_.pushed = true; }
        };
        switch (gizmo_drag_) {
            case H_X: case H_Z: case H_Y: {
                XMFLOAT3 A = gizmo_drag_ == H_X ? XMFLOAT3{1, 0, 0} : gizmo_drag_ == H_Y ? XMFLOAT3{0, 1, 0} : XMFLOAT3{0, 0, 1};
                // Parameter on the axis line closest to the mouse ray.
                float wx = p0.x - ro.x, wy = p0.y - ro.y, wz = p0.z - ro.z;
                float b = A.x * rd.x + A.y * rd.y + A.z * rd.z, d = A.x * wx + A.y * wy + A.z * wz, e = rd.x * wx + rd.y * wy + rd.z * wz;
                float den = 1.f - b * b;
                if (den < 1e-4f) return true;
                // Closest point between the axis line P0 + s*A and the mouse ray O + u*D (A, D unit):
                // s = (b*e - d) / (1 - b^2) with w0 = P0 - O, b = A.D, d = A.w0, e = D.w0.
                float s = (b * e - d) / den;
                if (gz_.start > 1e29f) { gz_.start = s; return true; }
                float total = s - gz_.start, delta = total - gz_.applied;
                if (std::fabs(delta) < 1e-4f) return true;
                ensure_undo("move");
                gz_.applied = total;
                if (gizmo_drag_ == H_Y) move_selection({0, delta, 0}, false);
                else move_selection({A.x * delta, 0, A.z * delta}, true);
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
                move_selection(delta, true);
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
                float target = io.KeyShift ? std::round(gz_.accum / snap) * snap : gz_.accum;
                float delta = target - gz_.applied;
                if (std::fabs(delta) < 1e-5f) return true;
                ensure_undo(gizmo_drag_ == H_YAW ? "rotate" : "tilt");
                gz_.applied = target;
                if (gizmo_drag_ == H_YAW) { if (!glue_turn(0, delta, p0)) rotate_group(delta, p0); }
                else if (!glue_turn(gizmo_drag_ == H_PITCH ? 1 : 2, delta, p0)) tilt_selection(gizmo_drag_ == H_PITCH ? 0 : 1, delta);
                return true;
            }
        }
        return true;
    }
    if (glue_click(lclick)) return true;  // a lock icon on a joint (app_glue.cpp) wins over the handles under it
    gizmo_hot_ = vp_hover_ ? hit_test(cam_, this, vp_x, vp_y, vp_w, vp_h, g, gizmo_effective_mode(), m, 8.f * dpi_) : 0;
    if (gizmo_hot_) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (gizmo_hot_ && lclick) {
        gizmo_drag_ = gizmo_hot_;
        gz_ = GizmoDrag{};
        gz_.pivot = pv;
        gz_.start = 1e30f;
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
        }
        return true;
    }
    return false;
}

// Top of the Properties panel while something is selected: which handles the gizmo shows, Deselect all and Zoom
// (UX-SPEC S4: nothing floats over the 3D view).
void App::transform_tools_ui() {
    if (!(tool_ == Tool::Select && editable() && !selection_.empty())) return;
    static const char* names[3] = {"Move", "Rotate", "Tilt"};
    static const char* tips[3] = {"Move: drag an arrow along one direction, or the yellow square along the ground. The green arrow lifts it.",
                                  "Rotate: drag the ring to turn it (Shift = steps of 15 degrees). A group turns about its centre.",
                                  "Tilt: drag the red ring to tip it forward or back, the blue ring to lean it sideways (Shift = 15 degree steps)."};
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    // A trigger box or circle: its handles do everything (no Move / Rotate / Tilt modes).
    if (!single_trigger_shape() && !selection_positionless()) {  // no place on the map: nothing to move (O5)
        const float w = (ImGui::GetContentRegionAvail().x - 2 * sp) / 3;
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            bool on = gizmo_mode_ == i;
            if (i == 2 && !gizmo_can_tilt()) ImGui::BeginDisabled();
            if (theme::toggle_button(names[i], on, ImVec2(w, 0))) gizmo_mode_ = i;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", i == 2 && !gizmo_can_tilt() ? "Tilt is greyed out because nothing selected can lean: spawn points, paths and trigger areas always stay level. Select a building, tree or prop to tilt it." : tips[i]);
            if (i == 2 && !gizmo_can_tilt()) ImGui::EndDisabled();
        }
    }
    const bool zoom = !selection_positionless();
    const float w2 = zoom ? (ImGui::GetContentRegionAvail().x - sp) / 2 : 0;
    if (ImGui::Button("Deselect all", ImVec2(w2, 0))) selection_.clear();
    ImGui::SetItemTooltip("Clear the selection (also: right-click an empty spot, or Esc).");
    if (zoom) {
        ImGui::SameLine();
        if (ImGui::Button("Zoom to it", ImVec2(w2, 0))) zoom_to_selection();
        ImGui::SetItemTooltip("Bring the camera to the selection (F).");
    }
    glue_tool_ui();
    ImGui::Separator();
}

void App::gizmo_draw() {
    if (!doc_) return;
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
                ImVec4 rc(p.x - pd, p.y - pd * 0.5f, p.x + ts.x + pd, p.y + ts.y + pd * 0.5f);
                if (std::any_of(placed.begin(), placed.end(), [&](const ImVec4& o) { return rc.x < o.z && o.x < rc.z && rc.y < o.w && o.y < rc.w; })) continue;
                placed.push_back(rc);
                bl->AddRectFilled(ImVec2(rc.x, rc.y), ImVec2(rc.z, rc.w), IM_COL32(0, 0, 0, 140), 3 * dpi_);
                bl->AddText(p, pad ? IM_COL32(120, 230, 230, 255) : IM_COL32(250, 230, 90, 255), v.tag.c_str());
            }
        glue_draw(bl);  // glue lines and locks (app_glue.cpp), also while nothing is selected (Glue tool)
        bl->PopClipRect();
    }
    bool show = tool_ == Tool::Select && editable() && !selection_.empty();
    Vec3 pv;
    if (!show || single_trigger_shape() || !gizmo_pivot(pv)) return;
    if (gizmo_effective_mode() != 0)
        if (Vec3 hw; glue_hinge(hw)) pv = hw;
    GizmoGeom g;
    g.pv = pv;
    make_geom(this, g, cam_, doc_->world.objs, selection_);
    marker_square(g, gizmo_marker_half());
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    float th = 3.f * dpi_;
    auto P = [&](float x, float y, float z, ImVec2& o) { return screen(cam_, vp_x, vp_y, vp_w, vp_h, XMFLOAT3{pv.x + x, pv.y + y, pv.z + z}, o); };
    ImVec2 c;
    if (!P(0, 0, 0, c)) { dl->PopClipRect(); return; }
    int active = gizmo_drag_ ? gizmo_drag_ : gizmo_hot_;
    auto col = [&](int id, ImU32 base) { return id == active ? IM_COL32(255, 235, 90, 255) : base; };
    const ImU32 red = IM_COL32(235, 70, 70, 255), blue = IM_COL32(70, 120, 245, 255), green = IM_COL32(90, 215, 90, 255);
    dl->AddCircleFilled(c, 4 * dpi_, IM_COL32(255, 255, 255, 230));
    const int mode = gizmo_effective_mode();
    if (mode == 0) {
        struct Ax { int id; float x, y, z; ImU32 color; const char* label; };
        Ax axes[3] = {{H_X, g.L, 0, 0, red, "X"}, {H_Z, 0, 0, g.L, blue, "Z"}, {H_Y, 0, g.L * 0.75f, 0, green, "up"}};
        for (auto& a : axes) {
            ImVec2 e;
            if (!P(a.x, a.y, a.z, e)) continue;
            ImU32 cc = col(a.id, a.color);
            dl->AddLine(c, e, IM_COL32(0, 0, 0, 160), th + 2 * dpi_);
            dl->AddLine(c, e, cc, th);
            float dx = e.x - c.x, dy = e.y - c.y, len = std::sqrt(dx * dx + dy * dy);
            if (len > 1) {
                dx /= len; dy /= len;
                float s = 11 * dpi_;
                dl->AddTriangleFilled(ImVec2(e.x + dx * s, e.y + dy * s), ImVec2(e.x - dx * s * 0.2f - dy * s * 0.55f, e.y - dy * s * 0.2f + dx * s * 0.55f),
                                      ImVec2(e.x - dx * s * 0.2f + dy * s * 0.55f, e.y - dy * s * 0.2f - dx * s * 0.55f), cc);
            }
        }
        ImVec2 q[4];
        if (P(g.sa, 0, g.sa, q[0]) && P(g.sb, 0, g.sa, q[1]) && P(g.sb, 0, g.sb, q[2]) && P(g.sa, 0, g.sb, q[3])) {
            ImU32 cc = col(H_PLANE, IM_COL32(255, 210, 60, 255));
            // A centred square sits on the marker itself: lighter fill so the marker shows through.
            dl->AddConvexPolyFilled(q, 4, (cc & 0x00FFFFFFu) | (g.centred ? 0x50000000u : 0x80000000u));
            dl->AddPolyline(q, 4, cc, ImDrawFlags_Closed, 2 * dpi_);
        }
    } else {
        std::vector<XMFLOAT3> pts;
        auto ring = [&](const XMFLOAT3& a, const XMFLOAT3& b, int id, ImU32 base) {
            ring_points(pv, g.L, a, b, pts);
            std::vector<ImVec2> sp;
            for (auto& p : pts) {
                ImVec2 s;
                if (screen(cam_, vp_x, vp_y, vp_w, vp_h, p, s)) sp.push_back(s);
            }
            if (sp.size() < 2) return;
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
    if (gizmo_drag_) {
        char t[64];
        if (gizmo_drag_ == H_YAW) snprintf(t, sizeof t, "Turn %.0f deg", gz_.applied * kDegPerRad);
        else if (gizmo_drag_ == H_PITCH || gizmo_drag_ == H_ROLL) snprintf(t, sizeof t, "%s %.0f deg", gizmo_drag_ == H_PITCH ? "Tip" : "Lean", gz_.applied * kDegPerRad);
        else if (gizmo_drag_ == H_PLANE) snprintf(t, sizeof t, "Move %.1f, %.1f m", gz_.applied_v.x, gz_.applied_v.z);
        else snprintf(t, sizeof t, "Move %.1f m", gz_.applied);
        ImVec2 mp = ImGui::GetIO().MousePos, ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2(mp.x + 14 * dpi_, mp.y + 14 * dpi_), ImVec2(mp.x + 22 * dpi_ + ts.x, mp.y + 18 * dpi_ + ts.y), IM_COL32(0, 0, 0, 190), 3 * dpi_);
        dl->AddText(ImVec2(mp.x + 18 * dpi_, mp.y + 16 * dpi_), IM_COL32(255, 255, 255, 255), t);
    }
    dl->PopClipRect();
}

}  // namespace ed
