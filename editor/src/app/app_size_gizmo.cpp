// Sizing by dragging: orange handles on the sides (and corners of a plate) of the one selected road piece,
// base, wall, fence, gate, barricade, guardrail, canal wall or bridge, in Move mode. The side follows the mouse, the opposite side
// stays; the cut-off part is hatched; the crop only shrinks. The cut itself is the piece's EditorCrop (app_crop.cpp apply_crop), the
// same property the Size panel edits and the save turns into a cut model. One undo step per drag, "size", pushed at the first real
// change. Double-click a handle: that side whole again. Look and feel follow the trigger box's size handles (app_trigger_gizmo.cpp),
// larger (a square 18 px at 100 % scale, a 14 px hit radius, both times the UI scale).
#include <algorithm>
#include <cmath>

#include "../core/crop.h"
#include "../core/json.h"
#include "../core/triggers.h"
#include "../core/water_custom.h"
#include "app.h"
#include "imgui.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;
namespace sn = ma2::snap;

namespace {

const float kPi = 3.14159265f;
const float kHandleHalf = 9.f;   // px at 100 % UI scale: the square is 18 px
const float kHitRadius = 14.f;   // px at 100 % UI scale
const float kPairGap = 36.f;     // px at 100 % UI scale: a pair of handles closer than this on screen is hidden (they would overlap)

std::string trim_m(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.1f", v);
    std::string s = b;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

// The label of one side of a piece, by what the panel and the handles call it.
const char* side_name(SizeKind kind, bool along_x, int bit) {
    if (kind == SizeKind::Run) {
        if (along_x) return bit == kSideX1 ? "end 1" : bit == kSideX0 ? "end 2" : "";
        return bit == kSideZ1 ? "end 1" : bit == kSideZ0 ? "end 2" : "";
    }
    switch (bit) {
        case kSideX0: return "left";
        case kSideX1: return "right";
        case kSideZ1: return kind == SizeKind::Road ? "start" : "front";
        case kSideZ0: return kind == SizeKind::Road ? "end" : "back";
    }
    return "";
}

std::string handle_label(SizeKind kind, bool along_x, int id) {
    const int xb = id & (kSideX0 | kSideX1), zb = id & (kSideZ0 | kSideZ1);
    if (xb && zb) return std::string(side_name(kind, along_x, zb)) + "-" + side_name(kind, along_x, xb);
    return side_name(kind, along_x, xb ? xb : zb);
}

ImGuiMouseCursor cursor_for(float dx, float dy) {
    float a = std::atan2(dy, dx);
    if (a < 0) a += kPi;
    if (a >= kPi) a -= kPi;
    const int s = int(std::lround(a / (kPi * 0.25f))) & 3;  // 0 EW, 1 down-right (NWSE), 2 NS, 3 down-left (NESW)
    return s == 0 ? ImGuiMouseCursor_ResizeEW : s == 1 ? ImGuiMouseCursor_ResizeNWSE : s == 2 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeNESW;
}

const char* cursor_name(ImGuiMouseCursor c) {
    return c == ImGuiMouseCursor_ResizeEW ? "ResizeEW" : c == ImGuiMouseCursor_ResizeNS ? "ResizeNS" : c == ImGuiMouseCursor_ResizeNESW ? "ResizeNESW" : c == ImGuiMouseCursor_ResizeNWSE ? "ResizeNWSE" : "Arrow";
}

float dot3(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Parameter of the point on the line P0 + s*A nearest the mouse ray (as the trigger gizmo's size handles).
bool axis_param(const XMFLOAT3& ro, const XMFLOAT3& rd, const XMFLOAT3& P0, const XMFLOAT3& A, float& s) {
    const float wx = P0.x - ro.x, wy = P0.y - ro.y, wz = P0.z - ro.z;
    const float b = dot3(A, rd), d = A.x * wx + A.y * wy + A.z * wz, e = rd.x * wx + rd.y * wy + rd.z * wz;
    const float den = 1.f - b * b;
    if (den < 1e-4f) return false;
    s = (b * e - d) / den;
    return true;
}

// The ray's hit on the plane through P0 with normal n, as offsets along the model's x and z axes from P0.
bool plane_hit(const XMFLOAT3& ro, const XMFLOAT3& rd, const XMFLOAT3& P0, const XMFLOAT3& n, const XMFLOAT3& ax, const XMFLOAT3& az, float& hx, float& hz) {
    const float den = dot3(rd, n);
    if (std::fabs(den) < 0.05f) return false;
    const float t = ((P0.x - ro.x) * n.x + (P0.y - ro.y) * n.y + (P0.z - ro.z) * n.z) / den;
    if (t < 0) return false;
    const XMFLOAT3 d{ro.x + rd.x * t - P0.x, ro.y + rd.y * t - P0.y, ro.z + rd.z * t - P0.z};
    hx = dot3(d, ax);
    hz = dot3(d, az);
    return true;
}

}  // namespace

// What shows for the selection now. Fills `reason` when nothing does. The model-space frame (ax / ay / az) is the object's own, with heading
// and tilt (orient_instance), so the handles follow a turned or tilted piece.
bool App::size_layout(SizeLayout& lo) {
    lo = SizeLayout{};
    auto no = [&](const char* r) { lo.reason = r; return false; };
    if (!doc_) return no("no map");
    if (tool_ != Tool::Select || glue_mode_) return no("not the Select tool");
    if (!editable()) return no("map is read-only");
    if (selection_.empty()) return no("nothing selected");
    if (selection_.size() != 1) return no("more than one selected");
    WObj* o = object_by_uid(doc_->world, *selection_.begin());
    if (!o) return no("nothing selected");
    Vec3 p;
    if (!get_pos(*o, p)) return no("not sizable");
    lo.si = size_info(*o);
    const bool water = lo.si.kind == SizeKind::Water;
    if (lo.si.kind != SizeKind::Road && lo.si.kind != SizeKind::Plate && lo.si.kind != SizeKind::Run && !water) return no("not sizable");
    lo.uid = o->uid();
    if (gizmo_effective_mode() != 0) return no(gizmo_effective_mode() == 1 ? "rotate mode" : "tilt mode");
    GpuModel* gm = nullptr;
    if (water) {   // The water's own rectangle, centred on its origin (no model, no crop: it grows as well as shrinks)
        float ww = 0, wl = 0;
        water_size_of(*o, nullptr, ww, wl);
        if (ww <= 0 || wl <= 0) return no("loading its model");
        lo.whole = lo.kept = {-0.5f * ww, 0.5f * ww, -0.5f * wl, 0.5f * wl};
    } else {
        if (!whole_rect(*o, lo.si, lo.whole)) return no("loading its model");
        crop::Rect cur;
        lo.kept = crop::of(*o, cur) ? cur : lo.whole;
        gm = whole_model(*o);
        if (!gm) return no("loading its model");
    }
    if (cpv_.on) return no("cutscene preview");
    float pitch = 0, roll = 0;
    get_tilt(*o, pitch, roll);
    BoxInstance mi;
    orient_instance(mi, heading(*o), pitch, roll, p);
    lo.pos = p;
    lo.ax = {mi.r0.x, mi.r1.x, mi.r2.x};
    lo.ay = {mi.r0.y, mi.r1.y, mi.r2.y};
    lo.az = {mi.r0.z, mi.r1.z, mi.r2.z};
    auto world = [&](float x, float y, float z) {
        return Vec3{mi.r0.x * x + mi.r0.y * y + mi.r0.z * z + mi.r0.w, mi.r1.x * x + mi.r1.y * y + mi.r1.z * z + mi.r1.w,
                    mi.r2.x * x + mi.r2.y * y + mi.r2.z * z + mi.r2.w};
    };
    const bool road = lo.si.kind == SizeKind::Road, plate = lo.si.kind == SizeKind::Plate;
    // Road: the carriageway (origin y). Plate: its top. Run: where the next piece joins (object_joints), not 60 m up at an arch.
    lo.y_corner = water ? 0.3f : road ? 0.f : plate ? gm->bmax[1] : gm->bmin[1] + std::min(1.5f, 0.5f * (gm->bmax[1] - gm->bmin[1]));   // water: just above its surface
    const crop::Rect& k = lo.kept;
    const float xm = 0.5f * (k.x0 + k.x1), zm = 0.5f * (k.z0 + k.z1);
    auto add = [&](int id, float x, float z) {
        SizeHandle h;
        h.id = id;
        h.corner = (id & (kSideX0 | kSideX1)) && (id & (kSideZ0 | kSideZ1));
        h.label = handle_label(lo.si.kind, lo.si.along_x, id);
        h.w = world(x, lo.y_corner, z);
        lo.h.push_back(h);
    };
    if (lo.si.kind == SizeKind::Run) {
        if (lo.si.along_x) { add(kSideX1, k.x1, zm); add(kSideX0, k.x0, zm); }
        else { add(kSideZ1, xm, k.z1); add(kSideZ0, xm, k.z0); }
    } else {
        add(kSideZ1, xm, k.z1);
        add(kSideZ0, xm, k.z0);
        add(kSideX0, k.x0, zm);
        add(kSideX1, k.x1, zm);
        if (plate || water) {
            add(kSideZ1 | kSideX0, k.x0, k.z1);
            add(kSideZ1 | kSideX1, k.x1, k.z1);
            add(kSideZ0 | kSideX0, k.x0, k.z0);
            add(kSideZ0 | kSideX1, k.x1, k.z0);
        }
    }
    // Screen positions; a pair of sides closer than kPairGap on screen would sit on each other (and on the Move arrows): hidden.
    for (SizeHandle& h : lo.h) {
        float sx, sy;
        h.on_screen = cam_.project({h.w.x, h.w.y, h.w.z}, vp_w, vp_h, sx, sy);
        h.sx = vp_x + sx;
        h.sy = vp_y + sy;
    }
    auto find = [&](int id) -> SizeHandle* {
        for (SizeHandle& h : lo.h) if (h.id == id) return &h;
        return nullptr;
    };
    auto too_close = [&](int a, int b) {
        const SizeHandle *ha = find(a), *hb = find(b);
        return ha && hb && ha->on_screen && hb->on_screen && std::hypot(ha->sx - hb->sx, ha->sy - hb->sy) < kPairGap * size_scale();
    };
    const bool xhide = lo.si.kind != SizeKind::Run && too_close(kSideX0, kSideX1);
    const bool zhide = lo.si.kind != SizeKind::Run && too_close(kSideZ0, kSideZ1);
    const bool runhide = lo.si.kind == SizeKind::Run && (lo.h.size() == 2 && lo.h[0].on_screen && lo.h[1].on_screen &&
                                                        std::hypot(lo.h[0].sx - lo.h[1].sx, lo.h[0].sy - lo.h[1].sy) < kPairGap * size_scale());
    int shown = 0;
    for (SizeHandle& h : lo.h) {
        const bool onx = h.id & (kSideX0 | kSideX1), onz = h.id & (kSideZ0 | kSideZ1);
        h.hidden = runhide || (onx && xhide) || (onz && zhide);
        if (h.hidden) lo.part_hidden = true;
        shown += !h.hidden;
    }
    // A lock icon on this end (app_glue.cpp: open = the two ends meet, closed = glued): the handle stands 28 px further out so the lock stays
    // clickable. A glued end does not drag.
    if (glue_family(*o)) {
        const int fc = ImGui::GetFrameCount();
        if (size_icons_frame_ != fc || size_icons_uid_ != lo.uid) { size_icons_ = glue_icons(); size_icons_frame_ = fc; size_icons_uid_ = lo.uid; }
        ImVec2 cs;
        float csx, csy;
        const bool have_c = cam_.project({world(xm, lo.y_corner, zm).x, world(xm, lo.y_corner, zm).y, world(xm, lo.y_corner, zm).z}, vp_w, vp_h, csx, csy);
        cs = ImVec2(vp_x + csx, vp_y + csy);
        for (const GlueIcon& g : size_icons_) {
            if (g.line || g.a != lo.uid) continue;
            for (SizeHandle& h : lo.h) {
                if (h.corner || std::hypot(h.w.x - g.p.x, h.w.z - g.p.z) > 1.5f) continue;
                h.near_lock = true;
                if (g.locked) h.glued = true;
                if (h.on_screen && have_c) {
                    float dx = h.sx - cs.x, dy = h.sy - cs.y;
                    const float l = std::hypot(dx, dy);
                    if (l > 1e-3f) { h.sx += dx / l * 28.f * size_scale(); h.sy += dy / l * 28.f * size_scale(); }
                }
            }
        }
    }
    if (!shown) return no("too small on screen");
    lo.show = true;
    return true;
}

bool App::size_side_label(const SizeLayout& lo, int id, std::string& out) const {
    out = handle_label(lo.si.kind, lo.si.along_x, id);
    return !out.empty();
}

void App::size_free_variants(uint32_t uid) {
    if (!doc_) return;
    const WObj* o = object_by_uid(doc_->world, uid);
    if (!o) return;
    std::string model, mtb;
    if (o->cls == key_of("MARoad")) {
        std::string def = o->str("RoadDefinition");
        for (char& ch : def) if (ch == '\\') ch = '/';
        const RoadPiece* rp = find_road_piece(def);
        if (!rp) return;
        model = rp->model;
    } else {
        model = o->str("Corporal3DObj");
        mtb = o->str("CorporalMTB");
    }
    if (model.empty()) return;
    std::set<std::string> keep;
    for (const WObj& w : doc_->world.objs) {
        if (w.cls == key_of("MARoad") ? false : (w.str("Corporal3DObj") != model || w.str("CorporalMTB") != mtb)) continue;
        if (w.cls == key_of("MARoad")) {
            std::string def = w.str("RoadDefinition");
            for (char& ch : def) if (ch == '\\') ch = '/';
            const RoadPiece* rp = find_road_piece(def);
            if (!rp || rp->model != model) continue;
        }
        if (std::string k = crop_key(w); !k.empty()) keep.insert(k);
    }
    models_.drop_crop_variants(model, mtb, keep);
}

int App::size_selection_variants() {
    if (!doc_ || selection_.size() != 1) return 0;
    const WObj* o = object_by_uid(doc_->world, *selection_.begin());
    if (!o) return 0;
    if (o->cls == key_of("MARoad")) {
        std::string def = o->str("RoadDefinition");
        for (char& ch : def) if (ch == 0x5C) ch = '/';
        const RoadPiece* rp = find_road_piece(def);
        return rp ? models_.crop_variants(rp->model, "") : 0;
    }
    return models_.crop_variants(o->str("Corporal3DObj"), o->str("CorporalMTB"));
}

// What a dragged cut edge can meet, in the world: the open ends of the same family for a road piece or run piece
// (the cut end lands flush on them), the edge lines of other plates (cropped footprints), of building footprints and the kerb lines of straight
// road pieces for a plate. Never the piece itself nor what it is glued to.
void App::size_targets(const WObj& o, const SizeInfo& si, std::vector<sn::EdgeTarget>& out) {
    out.clear();
    Vec3 p0;
    if (!doc_ || !cat_ || !get_pos(o, p0)) return;
    std::set<uint32_t> skip{o.uid()};
    for (const GlueLink& l : glue_links(o)) skip.insert(l.uid);
    if (si.kind == SizeKind::Road || si.kind == SizeKind::Run) {
        std::vector<sn::Socket> all;
        std::vector<char> open;
        snap_sockets(p0.x, p0.z, 250.f, glue_family(o), skip, all, open);
        for (size_t i = 0; i < all.size(); i++) {
            if (!open[i]) continue;
            sn::EdgeTarget t;
            t.kind = 0; t.x = all[i].x; t.z = all[i].z; t.uid = all[i].uid; t.index = all[i].index;
            out.push_back(t);
        }
        // a road piece or run piece (wall, bridge) goes on: it also meets base edges and building faces
    }
    if (si.kind != SizeKind::Plate && si.kind != SizeKind::Run && si.kind != SizeKind::Road) return;
    const uint32_t k_road = key_of("MARoad"), k_building = key_of("MABuilding");
    for (const ObjView& v : views_) {
        if (v.helper >= 0 || v.index < 0 || size_t(v.index) >= doc_->world.objs.size()) continue;
        if (std::fabs(v.pos.x - p0.x) > 400.f || std::fabs(v.pos.z - p0.z) > 400.f) continue;
        const WObj& w = doc_->world.objs[size_t(v.index)];
        if (skip.count(w.uid())) continue;
        const bool road = w.cls == k_road;
        if (road && si.kind != SizeKind::Plate) continue;   // a wall or road does not stop at a kerb
        if (!road && !(glue_family(w) == 3 || (w.cls == k_building && !is_foundation_obj(w)))) continue;
        Vec3 p;
        float tp = 0, tr = 0;
        if (!get_pos(w, p)) continue;
        get_tilt(w, tp, tr);
        if (std::fabs(tp) + std::fabs(tr) > 1e-4f) continue;
        float cx, cz, hx, hz;
        if (road) {
            std::string def = w.str("RoadDefinition");
            for (char& ch : def) if (ch == 0x5C) ch = '/';
            const RoadPiece* rp = find_road_piece(def);
            if (!rp) continue;
            if (!mover_from_road(*rp, &w).straight) continue;
            GpuModel* gm = whole_model(w);
            if (!gm || gm->state != GpuModel::Ready) continue;
            float x0 = gm->bmin[0], x1 = gm->bmax[0];
            if (crop::Rect cr; crop::of(w, cr)) { x0 = std::max(x0, cr.x0); x1 = std::min(x1, cr.x1); }
            cx = 0.5f * (x0 + x1); hx = 0.5f * (x1 - x0); cz = 0; hz = 10.f;   // the kerbs: the two lines along the piece
        } else {
            GpuModel* gm = obj_model(w);
            if (!gm) gm = v.model;
            if (!gm || gm->state != GpuModel::Ready) continue;
            cx = 0.5f * (gm->bmin[0] + gm->bmax[0]); cz = 0.5f * (gm->bmin[2] + gm->bmax[2]);
            hx = 0.5f * (gm->bmax[0] - gm->bmin[0]); hz = 0.5f * (gm->bmax[2] - gm->bmin[2]);
        }
        if (hx < 0.5f || hz < 0.5f) continue;
        const float yaw = heading(w), c = std::cos(yaw), s = std::sin(yaw);
        auto wx = [&](float lx, float lz) { return p.x + c * lx + s * lz; };
        auto wz = [&](float lx, float lz) { return p.z - s * lx + c * lz; };
        auto line = [&](int idx, float lx, float lz, float dlx, float dlz) {
            sn::EdgeTarget t;
            t.kind = 1; t.uid = w.uid(); t.index = idx;
            t.x = wx(lx, lz); t.z = wz(lx, lz);
            t.dx = c * dlx + s * dlz; t.dz = -s * dlx + c * dlz;
            out.push_back(t);
        };
        line(0, cx - hx, cz, 0, 1);
        line(1, cx + hx, cz, 0, 1);
        if (!road) { line(2, cx, cz - hz, 1, 0); line(3, cx, cz + hz, 1, 0); }
    }
}

bool App::size_gizmo_update(bool lclick, bool ldown) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    XMFLOAT3 ro, rd;
    cam_.ray(m.x - vp_x, m.y - vp_y, vp_w, vp_h, ro, rd);
    SizeDrag& d = size_drag_;

    if (d.id) {   // a drag in progress: the side follows the mouse
        WObj* o = doc_ ? object_by_uid(doc_->world, d.uid) : nullptr;
        if (!o || !editable()) { d = SizeDrag{}; return true; }
        ImGui::SetMouseCursor(size_cursor_id_);
        if (!ldown) {
            if (d.pushed && d.si.kind != SizeKind::Water) size_free_variants(d.uid);
            objects_dirty_ = minimap_dirty_ = true;
            size_readout_until_ = ImGui::GetTime() + 2.0;
            d = SizeDrag{};
            return true;
        }
        const int xb = d.id & (kSideX0 | kSideX1), zb = d.id & (kSideZ0 | kSideZ1);
        if (!d.moved && std::hypot(m.x - d.press_x, m.y - d.press_y) >= 4.f * dpi_) d.moved = true;
        if (!d.moved) return true;
        float tx = 0, tz = 0;
        if (xb && zb) {
            float hx, hz;
            if (!plane_hit(ro, rd, d.p0, d.up, d.ax, d.az, hx, hz)) return true;
            tx = hx - d.hx;
            tz = hz - d.hz;
        } else if (xb) {
            float s;
            if (!axis_param(ro, rd, d.p0, d.ax, s)) return true;
            tx = s - d.sx;
        } else {
            float s;
            if (!axis_param(ro, rd, d.p0, d.az, s)) return true;
            tz = s - d.sz;
        }
        if (d.si.kind == SizeKind::Water) {   // The edge follows the mouse, the opposite edge stays, the centre moves by half the change
            const bool wfree = holding(settings_.hold_free);
            const float wgrid = !wfree && grid_active() ? settings_.snap.grid_m : 0.f;
            bool capped = false;
            auto side = [&](float c0, float opp, bool maxside, float t) {
                float len = (maxside ? c0 + t - opp : opp - (c0 + t));
                if (wgrid > 0) len = snap::size_step(len, wgrid, wcustom::kMinSide);
                len = std::round(len);
                if (len > wcustom::kMaxSide) { len = wcustom::kMaxSide; capped = true; }
                len = std::max(len, wcustom::kMinSide);
                return maxside ? opp + len : opp - len;
            };
            crop::Rect n = d.start;
            if (xb == kSideX0) n.x0 = side(d.start.x0, d.start.x1, false, tx);
            if (xb == kSideX1) n.x1 = side(d.start.x1, d.start.x0, true, tx);
            if (zb == kSideZ0) n.z0 = side(d.start.z0, d.start.z1, false, tz);
            if (zb == kSideZ1) n.z1 = side(d.start.z1, d.start.z0, true, tz);
            const float nw = n.x1 - n.x0, nl = n.z1 - n.z0, cx = 0.5f * (n.x0 + n.x1), cz = 0.5f * (n.z0 + n.z1);
            const Vec3 np{d.start_pos.x + d.ax.x * cx + d.az.x * cz, d.start_pos.y, d.start_pos.z + d.ax.z * cx + d.az.z * cz};
            float cw, cl;
            Vec3 cp;
            water_size_of(*o, nullptr, cw, cl);
            if (get_pos(*o, cp) && (std::fabs(cw - nw) > 0.01f || std::fabs(cl - nl) > 0.01f || std::fabs(cp.x - np.x) > 0.01f || std::fabs(cp.z - np.z) > 0.01f)) {
                if (!d.pushed) { push_undo("size"); d.pushed = true; }
                set_pos(*o, np);
                set_water_size(*o, nw, nl);   // the basin is refitted once, when the mouse comes up (refit_moved_basins)
            }
            char b[160];
            snprintf(b, sizeof b, "Width %.0f m, Length %.0f m%s", nw, nl, capped ? " - the largest water is 1000 m" : "");
            size_readout_ = b;
            size_readout_until_ = ImGui::GetTime() + 2.0;
            status_ = size_readout_;
            status_time_ = 0.5f;
            return true;
        }
        const bool free = holding(settings_.hold_free);
        const float grid = grid_active() ? settings_.snap.grid_m : 0.f;
        const float kMin = crop::kMinSide;
        bool over = false;
        d.joint_text.clear();
        for (auto& j : d.jt) j.on = false;
        // The joint for one side: the side's middle at raw coordinate `raw` on its axis, its outward normal and its line in the world,
        // the targets of the press; priority joint > grid > fine steps. Returns true and the corrected coordinate when a joint holds.
        auto joint_for = [&](int bit, int slot, float raw, const crop::Rect& cur, float& out) {
            if (free || d.targets.empty()) { d.hit[slot] = sn::EdgeHit{}; return false; }
            const bool onx = bit & (kSideX0 | kSideX1);
            const bool maxs = bit & (kSideX1 | kSideZ1);
            const XMFLOAT3& na = onx ? d.ax : d.az;      // the axis the side moves along
            const XMFLOAT3& ta = onx ? d.az : d.ax;      // along the side
            const float zm = 0.5f * (cur.z0 + cur.z1), xm = 0.5f * (cur.x0 + cur.x1);
            const float mx = onx ? raw : xm, mz = onx ? zm : raw;
            const float la = std::hypot(na.x, na.z), lt = std::hypot(ta.x, ta.z);
            if (la < 0.2f || lt < 0.2f) return false;
            sn::EdgeDrag e;
            e.ex = d.pos.x + d.ax.x * mx + d.az.x * mz;
            e.ez = d.pos.z + d.ax.z * mx + d.az.z * mz;
            const float sg = maxs ? 1.f : -1.f;
            e.nx = sg * na.x / la; e.nz = sg * na.z / la;
            e.tx = ta.x / lt; e.tz = ta.z / lt;
            e.half_t = 0.5f * (onx ? cur.z1 - cur.z0 : cur.x1 - cur.x0);
            e.uid = d.uid;
            float sx0, sy0, sx1, sy1;
            if (!cam_.project({e.ex, d.p0.y, e.ez}, vp_w, vp_h, sx0, sy0) || !cam_.project({e.ex + e.nx, d.p0.y, e.ez + e.nz}, vp_w, vp_h, sx1, sy1)) return false;
            const float ppm = std::hypot(sx1 - sx0, sy1 - sy0);
            if (ppm < 1e-3f) return false;
            sn::EdgeHit h;
            const sn::EdgeHit* pv = d.hit[slot].kind >= 0 ? &d.hit[slot] : nullptr;
            if (!sn::edge_snap(e, d.targets, sn::kReachPx * dpi_, ppm, pv, h)) { d.hit[slot] = sn::EdgeHit{}; return false; }
            out = raw + sg * h.s / la;   // s runs along the outward world normal; the model coordinate changes by sg * s / |axis|
            d.hit[slot] = h;
            auto& fx = d.jt[slot];
            fx.on = true; fx.kind = h.kind; fx.uid = h.uid;
            fx.ex = e.ex + e.nx * h.s; fx.ez = e.ez + e.nz * h.s;
            for (const sn::EdgeTarget& t : d.targets)
                if (t.kind == h.kind && t.uid == h.uid && t.index == h.index) { fx.tx = t.x; fx.tz = t.z; fx.dx = t.dx; fx.dz = t.dz; break; }
            return true;
        };
        // One side: its new coordinate for the mouse travel t. `maxside` = the far side of its axis (x1, z1), whose kept length is c - opp.
        auto move_side = [&](float c0, float opp, float lo_lim, float hi_lim, bool maxside, float t, int bit, int slot, const crop::Rect& cur) {
            float raw = c0 + t;
            if (maxside ? raw > hi_lim + 0.05f : raw < lo_lim - 0.05f) over = true;   // pulled past the whole piece
            float jc;
            if (joint_for(bit, slot, std::clamp(raw, lo_lim, hi_lim), cur, jc)) {
                const float cj = std::clamp(jc, lo_lim, hi_lim);
                if (std::fabs(cj - jc) < 0.01f && std::fabs((maxside ? cj - opp : opp - cj)) >= kMin - 1e-4f) {
                    const float edge = maxside ? hi_lim : lo_lim;
                    return std::fabs(cj - edge) < 0.05f ? edge : cj;
                }
                d.hit[slot] = sn::EdgeHit{};
                d.jt[slot].on = false;
            }
            float c = std::clamp(raw, lo_lim, hi_lim);
            float len = maxside ? c - opp : opp - c;
            if (free) len = std::round(len * 100.f) / 100.f;
            else if (grid > 0) len = snap::size_step(len, grid, kMin);
            else len = std::round(len * 10.f) / 10.f;
            c = maxside ? opp + len : opp - len;
            c = std::clamp(c, lo_lim, hi_lim);
            const float edge = maxside ? hi_lim : lo_lim;
            if (std::fabs(c - edge) < 0.05f) c = edge;   // whole
            return c;
        };
        crop::Rect n = d.start;
        const crop::Rect& w = d.whole;
        if (xb == kSideX0) n.x0 = move_side(d.start.x0, d.start.x1, w.x0, d.start.x1 - kMin, false, tx, kSideX0, 0, n);
        if (xb == kSideX1) n.x1 = move_side(d.start.x1, d.start.x0, d.start.x0 + kMin, w.x1, true, tx, kSideX1, 0, n);
        if (zb == kSideZ0) n.z0 = move_side(d.start.z0, d.start.z1, w.z0, d.start.z1 - kMin, false, tz, kSideZ0, 1, n);
        if (zb == kSideZ1) n.z1 = move_side(d.start.z1, d.start.z0, d.start.z0 + kMin, w.z1, true, tz, kSideZ1, 1, n);
        d.over = over;
        auto near_whole = [&](float a, float b) { return std::fabs(a - b) < 0.05f; };
        const bool all_whole = near_whole(n.x0, w.x0) && near_whole(n.x1, w.x1) && near_whole(n.z0, w.z0) && near_whole(n.z1, w.z1);
        crop::Rect cur;
        const bool has = crop::of(*o, cur);
        const std::string cur_key = has ? crop::format(cur) : std::string(), new_key = all_whole ? std::string() : crop::format(n);
        if (cur_key != new_key) {
            if (!d.pushed) { push_undo("size"); d.pushed = true; }
            const std::string before = status_;
            apply_crop(*o, all_whole ? nullptr : &n);
            if (status_ != before) d.keep_status = true;   // "Cut pieces cannot be destroyed..." stays on the status line
        }
        // The readout: what the piece measures now, and how the cut stands.
        const crop::Rect kept = all_whole ? w : n;
        char b[200];
        const SizeKind kind = d.si.kind;
        std::string text;
        if (kind == SizeKind::Plate || (xb && zb)) {
            snprintf(b, sizeof b, "%.1f x %.1f m (whole %s x %s)", kept.w(), kept.l(), trim_m(w.w()).c_str(), trim_m(w.l()).c_str());
            text = b;
        } else {
            const bool zaxis = zb != 0;
            const int bit = zaxis ? zb : xb;
            const float cut = bit == kSideX0 ? kept.x0 - w.x0 : bit == kSideX1 ? w.x1 - kept.x1 : bit == kSideZ0 ? kept.z0 - w.z0 : w.z1 - kept.z1;
            snprintf(b, sizeof b, "%s %.1f m of %s (%s cut %.1f)", (zaxis || kind == SizeKind::Run) ? "Length" : "Width", zaxis ? kept.l() : kept.w(), trim_m(zaxis ? w.l() : w.w()).c_str(),
                     side_name(kind, d.si.along_x, bit), std::max(0.f, cut));
            text = b;
        }
        if (over) text += kind == SizeKind::Plate ? " - Whole size: add another piece to go further" : " - Whole length: add another piece to go further";
        {   // a joint holds: what the cut edge meets
            std::string jt;
            for (const auto& j : d.jt) {
                if (!j.on) continue;
                const WObj* ow = object_by_uid(doc_->world, j.uid);
                std::string nm = ow ? plain_object_name(ow->description()) : std::string("another piece");
                if (nm.empty()) nm = "another piece";
                jt += jt.empty() ? " - " : ", ";
                if (j.kind == 0) jt += "meets the end of " + nm;
                else if (d.si.kind == SizeKind::Run || d.si.kind == SizeKind::Road) jt += "meets " + nm;
                else jt += std::string("flush with ") + (ow && ow->cls == key_of("MARoad") ? "the kerb of " : "") + nm;
            }
            d.joint_text = jt;
            text += jt;
        }
        size_readout_ = text;
        size_readout_until_ = ImGui::GetTime() + 2.0;
        if (!d.keep_status) { status_ = text; status_time_ = 0.5f; }
        return true;
    }

    SizeLayout lo;
    const bool ok = size_layout(lo);
    size_hot_ = 0;
    size_label_.clear();
    size_cursor_.clear();
    size_sizable_ = ok;   // the handles show
    size_water_ = ok && lo.si.kind == SizeKind::Water;
    size_small_ = !ok && lo.reason == "too small on screen";
    size_hint_.clear();
    if (!ok) return false;
    if (!vp_hover_ || gizmo_drag_ || dragging_ || box_selecting_ || holding(settings_.hold_through)) return false;

    int best = 0;
    const SizeHandle* bh = nullptr;
    float bd = kHitRadius * size_scale();
    for (const SizeHandle& h : lo.h) {
        if (h.hidden || !h.on_screen) continue;
        const float dist = std::hypot(h.sx - m.x, h.sy - m.y);
        if (dist < bd) { bd = dist; best = h.id; bh = &h; }
    }
    // The lift knob sits at the top of the green arrow: where the mouse is over it, it keeps the click unless a handle is right under it.
    if (bh && knob_x_ >= 0 && std::fabs(m.x - knob_x_) <= 8.f * dpi_ && std::fabs(m.y - knob_y_) <= 8.f * dpi_ &&
        std::hypot(bh->sx - knob_x_, bh->sy - knob_y_) > 10.f * dpi_) { best = 0; bh = nullptr; }
    if (!bh) return false;
    size_hot_ = best;
    size_label_ = bh->label;
    // The cursor shows the drag axis: the side's own axis on screen (a corner: from the piece's middle out to the corner).
    {
        const bool corner = bh->corner;
        XMFLOAT3 a = (bh->id & (kSideX0 | kSideX1)) && !corner ? lo.ax : lo.az;
        float cx = 0, cy = 0;
        ImVec2 p1, p0(bh->sx, bh->sy);
        float sx, sy;
        bool have = false;
        if (corner) {
            const Vec3 mid{lo.pos.x, lo.pos.y, lo.pos.z};
            const float xm = 0.5f * (lo.kept.x0 + lo.kept.x1), zm = 0.5f * (lo.kept.z0 + lo.kept.z1);
            const XMFLOAT3 c{bh->w.x - lo.ax.x * (((bh->id & kSideX1) ? lo.kept.x1 : lo.kept.x0) - xm) - lo.az.x * (((bh->id & kSideZ1) ? lo.kept.z1 : lo.kept.z0) - zm),
                             bh->w.y - lo.ax.y * (((bh->id & kSideX1) ? lo.kept.x1 : lo.kept.x0) - xm) - lo.az.y * (((bh->id & kSideZ1) ? lo.kept.z1 : lo.kept.z0) - zm),
                             bh->w.z - lo.ax.z * (((bh->id & kSideX1) ? lo.kept.x1 : lo.kept.x0) - xm) - lo.az.z * (((bh->id & kSideZ1) ? lo.kept.z1 : lo.kept.z0) - zm)};
            (void)mid;
            if (cam_.project(c, vp_w, vp_h, sx, sy)) { p1 = ImVec2(vp_x + sx, vp_y + sy); cx = p0.x - p1.x; cy = p0.y - p1.y; have = true; }
        } else if (cam_.project({bh->w.x + a.x, bh->w.y + a.y, bh->w.z + a.z}, vp_w, vp_h, sx, sy)) {
            cx = vp_x + sx - p0.x;
            cy = vp_y + sy - p0.y;
            have = true;
        }
        const ImGuiMouseCursor cur = have ? cursor_for(cx, cy) : ImGuiMouseCursor_ResizeAll;
        size_cursor_id_ = cur;
        ImGui::SetMouseCursor(cur);
        size_cursor_ = cursor_name(cur);
    }
    // The hint line (status bar) says what a drag does here.
    {
        const std::string fr = mod_name(settings_.hold_free);
        const crop::Rect& w = lo.whole;
        std::string whole;
        if (bh->corner) whole = "whole piece: " + trim_m(w.w()) + " x " + trim_m(w.l()) + " m";
        else whole = "whole piece: " + trim_m((bh->id & (kSideX0 | kSideX1)) ? w.w() : w.l()) + " m";
        if (lo.si.kind == SizeKind::Water)
            size_hint_ = std::string("Drag to size the water: the ") + (bh->corner ? "corner changes width and length" : (bh->id & (kSideX0 | kSideX1)) ? bh->label + " edge changes its width" : bh->label + " edge changes its length") +
                         " (now " + (bh->corner ? trim_m(w.w()) + " x " + trim_m(w.l()) : trim_m((bh->id & (kSideX0 | kSideX1)) ? w.w() : w.l())) + " m), the opposite side stays - hold " + fr + ": no snapping";
        else
        size_hint_ = bh->glued ? "Glued at this end: click the lock to unglue it first"
                               : std::string("Drag to cut ") + (bh->corner ? "this corner (two sides)" : (bh->label.rfind("end ", 0) == 0 ? "" : "the ") + bh->label) + " (" + whole + ") - double-click: whole again - hold " + fr + ": no snapping";
    }
    if (!lclick) return false;

    WObj* o = object_by_uid(doc_->world, lo.uid);
    if (!o) return false;
    if (bh->glued) {
        std::string other;
        for (const GlueLink& l : glue_links(*o))
            if (l.mine >= 0) if (const WObj* ow = object_by_uid(doc_->world, l.uid)) { other = plain_object_name(ow->description()); break; }
        status_ = "Glued to " + (other.empty() ? std::string("another piece") : other) + " at this end: click the lock to unglue it first.";
        status_time_ = 5.f;
        return true;
    }
    // A double-click on a handle: that side whole again (one undo step).
    if (lo.si.kind != SizeKind::Water && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && size_last_press_ == best && size_last_press_uid_ == lo.uid) {
        crop::Rect n = lo.kept;
        if (best & kSideX0) n.x0 = lo.whole.x0;
        if (best & kSideX1) n.x1 = lo.whole.x1;
        if (best & kSideZ0) n.z0 = lo.whole.z0;
        if (best & kSideZ1) n.z1 = lo.whole.z1;
        if (crop::format(n) != crop::format(lo.kept)) {
            push_undo("size");
            const bool all = std::fabs(n.x0 - lo.whole.x0) < 0.05f && std::fabs(n.x1 - lo.whole.x1) < 0.05f && std::fabs(n.z0 - lo.whole.z0) < 0.05f &&
                             std::fabs(n.z1 - lo.whole.z1) < 0.05f;
            apply_crop(*o, all ? nullptr : &n);
            size_free_variants(lo.uid);
            size_readout_ = bh->label.rfind("end ", 0) == 0 ? "End " + bh->label.substr(4) + " is whole again" : std::string("The ") + bh->label + (bh->corner ? " corner is" : " side is") + " whole again";
            size_readout_until_ = ImGui::GetTime() + 2.0;
            status_ = size_readout_;
            status_time_ = 3.f;
        }
        size_last_press_ = 0;
        return true;
    }
    size_last_press_ = best;
    size_last_press_uid_ = lo.uid;
    d = SizeDrag{};
    d.id = best;
    d.uid = lo.uid;
    d.start = lo.kept;
    d.whole = lo.whole;
    d.si = lo.si;
    d.p0 = {bh->w.x, bh->w.y, bh->w.z};
    d.ax = lo.ax;
    d.az = lo.az;
    d.up = lo.ay;
    d.pos = lo.pos;
    d.start_pos = lo.pos;
    size_targets(*o, lo.si, d.targets);
    d.press_x = m.x;
    d.press_y = m.y;
    const bool corner = bh->corner;
    if (corner) { if (!plane_hit(ro, rd, d.p0, d.up, d.ax, d.az, d.hx, d.hz)) d.hx = d.hz = 0; }
    else if (best & (kSideX0 | kSideX1)) { if (!axis_param(ro, rd, d.p0, d.ax, d.sx)) d.sx = 0; }
    else if (!axis_param(ro, rd, d.p0, d.az, d.sz)) d.sz = 0;
    return true;
}

void App::size_gizmo_draw() {
    SizeLayout lo;
    if (!size_layout(lo)) {
        if (!size_drag_.id) { size_hot_ = 0; size_hint_.clear(); size_label_.clear(); size_cursor_.clear(); size_sizable_ = false; size_water_ = false; }
        return;
    }
    if (!vp_hover_ && !size_drag_.id) { size_hot_ = 0; size_hint_.clear(); size_label_.clear(); size_cursor_.clear(); }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    auto scr = [&](float x, float y, float z, ImVec2& o) {   // model-space point -> screen
        const XMFLOAT3 w{lo.pos.x + lo.ax.x * x + lo.ay.x * y + lo.az.x * z, lo.pos.y + lo.ax.y * x + lo.ay.y * y + lo.az.y * z,
                         lo.pos.z + lo.ax.z * x + lo.ay.z * y + lo.az.z * z};
        float sx, sy;
        if (!cam_.project(w, vp_w, vp_h, sx, sy)) return false;
        o = ImVec2(vp_x + sx, vp_y + sy);
        return true;
    };
    if (ImGui::GetFrameCount() - size_panel_frame_ > 1) size_panel_hot_ = 0;   // the Size panel is not showing: no stale field highlight
    const int hot = size_drag_.id ? size_drag_.id : size_hot_ ? size_hot_ : size_panel_hot_;
    const bool dragging = size_drag_.id != 0;
    const float y = lo.y_corner;
    const crop::Rect &k = lo.kept, &w = lo.whole;
    const ImU32 kOrange = IM_COL32(255, 150, 30, 255), kYellow = IM_COL32(255, 235, 90, 255);

    // The kept rectangle's outline.
    ImVec2 q[4];
    if (scr(k.x0, y, k.z0, q[0]) && scr(k.x1, y, k.z0, q[1]) && scr(k.x1, y, k.z1, q[2]) && scr(k.x0, y, k.z1, q[3])) {
        dl->AddPolyline(q, 4, IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, 4.f * dpi_);
        dl->AddPolyline(q, 4, IM_COL32(255, 150, 30, 210), ImDrawFlags_Closed, 2.f * dpi_);
    }
    // The part that goes (hover: the hot side's strip; drag: everything cut off): translucent with hatch lines 2 m apart.
    auto strip = [&](float a0, float a1, float b0, float b1, bool ghost = false) {
        if (a1 - a0 < 0.01f || b1 - b0 < 0.01f) return;
        ImVec2 s[4];
        if (!scr(a0, y, b0, s[0]) || !scr(a1, y, b0, s[1]) || !scr(a1, y, b1, s[2]) || !scr(a0, y, b1, s[3])) return;
        if (ghost) {   // Before the cut, only a thin outline of where the next cut would go: no fill, no hatch (it is not cut yet)
            dl->AddPolyline(s, 4, IM_COL32(0, 0, 0, 120), ImDrawFlags_Closed, 3.f * dpi_);
            dl->AddPolyline(s, 4, IM_COL32(255, 235, 90, 200), ImDrawFlags_Closed, 1.5f * dpi_);
            return;
        }
        dl->AddConvexPolyFilled(s, 4, IM_COL32(255, 90, 40, 55));
        dl->AddPolyline(s, 4, IM_COL32(255, 120, 60, 150), ImDrawFlags_Closed, 1.5f * dpi_);
        float step = 2.f;
        while ((a1 - a0 + b1 - b0) / step > 160.f) step *= 2.f;
        for (float u = a0 + b0 + step * 0.5f; u < a1 + b1; u += step) {   // lines x + z = u
            const float xa = std::max(a0, u - b1), xb = std::min(a1, u - b0);
            if (xb <= xa) continue;
            ImVec2 p, r;
            if (scr(xa, y, u - xa, p) && scr(xb, y, u - xb, r)) dl->AddLine(p, r, IM_COL32(255, 120, 60, 170), 1.5f * dpi_);
        }
    };
    const int shown_sides = dragging || hot ? hot : 0;
    if (dragging || hot) {
        if (dragging ? true : (hot & kSideX0)) strip(w.x0, k.x0, w.z0, w.z1);
        if (dragging ? true : (hot & kSideX1)) strip(k.x1, w.x1, w.z0, w.z1);
        if (dragging ? true : (hot & kSideZ0)) strip(k.x0, k.x1, w.z0, k.z0);
        if (dragging ? true : (hot & kSideZ1)) strip(k.x0, k.x1, k.z1, w.z1);
    }
    // The ghost: before the click, the part the next cut would take off the hot side, predicted as a fifth of what is kept there (at most
    // 10 m). The cut edge's own thick line and the handle say which side it is; dragging replaces it with the real cut.
    if (!dragging && hot && lo.si.kind != SizeKind::Water) {
        auto ghost = [&](int bit) {
            const bool onx = bit & (kSideX0 | kSideX1);
            const float ext = onx ? k.x1 - k.x0 : k.z1 - k.z0;
            const float d = std::min(std::clamp(0.2f * ext, 0.5f, 10.f), ext - crop::kMinSide);
            if (d < 0.2f) return;
            if (bit == kSideX0) strip(k.x0, k.x0 + d, k.z0, k.z1, true);
            if (bit == kSideX1) strip(k.x1 - d, k.x1, k.z0, k.z1, true);
            if (bit == kSideZ0) strip(k.x0, k.x1, k.z0, k.z0 + d, true);
            if (bit == kSideZ1) strip(k.x0, k.x1, k.z1 - d, k.z1, true);
        };
        for (int bit : {kSideX0, kSideX1, kSideZ0, kSideZ1})
            if (hot & bit) ghost(bit);
    }
    // The hot side(s): a thick line along the edge.
    auto side_line = [&](float xa, float za, float xb, float zb) {
        ImVec2 a, b;
        if (!scr(xa, y, za, a) || !scr(xb, y, zb, b)) return;
        dl->AddLine(a, b, IM_COL32(0, 0, 0, 200), 7.f * dpi_);
        dl->AddLine(a, b, kYellow, 4.5f * dpi_);
    };
    if (shown_sides) {
        if (shown_sides & kSideX0) side_line(k.x0, k.z0, k.x0, k.z1);
        if (shown_sides & kSideX1) side_line(k.x1, k.z0, k.x1, k.z1);
        if (shown_sides & kSideZ0) side_line(k.x0, k.z0, k.x1, k.z0);
        if (shown_sides & kSideZ1) side_line(k.x0, k.z1, k.x1, k.z1);
    }
    // The edges the cut could meet: the piece being cut can hide the edge you aim for, so the targets near the dragged side are
    // drawn on top, thin, until one holds (then the joint colour below takes over).
    if (dragging && !size_drag_.targets.empty()) {
        const SizeDrag& dd = size_drag_;
        const int bit = dd.id & (kSideX0 | kSideX1 | kSideZ0 | kSideZ1);
        const bool onx = bit & (kSideX0 | kSideX1);
        const float mx = onx ? ((bit & kSideX1) ? k.x1 : k.x0) : 0.5f * (k.x0 + k.x1), mz = onx ? 0.5f * (k.z0 + k.z1) : ((bit & kSideZ1) ? k.z1 : k.z0);
        {
            const float wx = lo.pos.x + lo.ax.x * mx + lo.az.x * mz, wz = lo.pos.z + lo.ax.z * mx + lo.az.z * mz;
            const ImU32 kCand = IM_COL32(120, 255, 235, 170);
            for (const sn::EdgeTarget& tg : dd.targets) {
                bool is_held = false;
                for (const auto& j : dd.jt) is_held |= j.on && j.kind == tg.kind && j.uid == tg.uid;
                if (is_held) continue;
                float sx, sy, sx2, sy2;
                if (tg.kind == 0) {
                    if (std::hypot(tg.x - wx, tg.z - wz) > 30.f) continue;
                    if (cam_.project({tg.x, dd.p0.y, tg.z}, vp_w, vp_h, sx, sy)) dl->AddCircle(ImVec2(vp_x + sx, vp_y + sy), 6.f * dpi_, kCand, 16, 2.f * dpi_);
                } else {
                    const float l = std::hypot(tg.dx, tg.dz);
                    if (l < 1e-4f) continue;
                    const float ux = tg.dx / l, uz = tg.dz / l, t0 = (wx - tg.x) * ux + (wz - tg.z) * uz;
                    const float px = tg.x + ux * t0, pz = tg.z + uz * t0;
                    // lines near the dragged side
                    if (std::hypot(px - wx, pz - wz) > 30.f) continue;
                    if (cam_.project({px - ux * 8.f, dd.p0.y, pz - uz * 8.f}, vp_w, vp_h, sx, sy) && cam_.project({px + ux * 8.f, dd.p0.y, pz + uz * 8.f}, vp_w, vp_h, sx2, sy2)) {
                        dl->AddLine(ImVec2(vp_x + sx, vp_y + sy), ImVec2(vp_x + sx2, vp_y + sy2), IM_COL32(0, 0, 0, 150), 4.f * dpi_);
                        dl->AddLine(ImVec2(vp_x + sx, vp_y + sy), ImVec2(vp_x + sx2, vp_y + sy2), kCand, 2.f * dpi_);
                    }
                }
            }
        }
    }
    // A joint holds: the target point or line and the cut edge, in the joint colour of the placing snap.
    const ImU32 kJoint = IM_COL32(120, 255, 235, 255);
    int joint_sides = 0;
    if (dragging) {
        for (int sl = 0; sl < 2; sl++) {
            const auto& j = size_drag_.jt[sl];
            if (!j.on) continue;
            float sx, sy, sx2, sy2;
            if (j.kind == 0) {
                if (cam_.project({j.tx, size_drag_.p0.y, j.tz}, vp_w, vp_h, sx, sy)) {
                    dl->AddCircle(ImVec2(vp_x + sx, vp_y + sy), 10.f * dpi_, IM_COL32(0, 0, 0, 200), 24, 4.f * dpi_);
                    dl->AddCircle(ImVec2(vp_x + sx, vp_y + sy), 10.f * dpi_, kJoint, 24, 2.2f * dpi_);
                }
            } else {
                const float l = std::hypot(j.dx, j.dz);
                if (l > 1e-4f) {
                    const float ux = j.dx / l, uz = j.dz / l, a = 18.f;
                    // centre the drawn line on the cut edge (the point of the line nearest it)
                    const float t0 = (j.ex - j.tx) * ux + (j.ez - j.tz) * uz;
                    const float px = j.tx + ux * t0, pz = j.tz + uz * t0;
                    if (cam_.project({px - ux * a, size_drag_.p0.y, pz - uz * a}, vp_w, vp_h, sx, sy) && cam_.project({px + ux * a, size_drag_.p0.y, pz + uz * a}, vp_w, vp_h, sx2, sy2)) {
                        dl->AddLine(ImVec2(vp_x + sx, vp_y + sy), ImVec2(vp_x + sx2, vp_y + sy2), IM_COL32(0, 0, 0, 200), 5.f * dpi_);
                        dl->AddLine(ImVec2(vp_x + sx, vp_y + sy), ImVec2(vp_x + sx2, vp_y + sy2), kJoint, 2.5f * dpi_);
                    }
                }
            }
            joint_sides |= sl == 0 ? (size_drag_.id & (kSideX0 | kSideX1)) : (size_drag_.id & (kSideZ0 | kSideZ1));
        }
    }
    // The handles.
    const float hs = kHandleHalf * size_scale();
    for (const SizeHandle& h : lo.h) {
        if (h.hidden || !h.on_screen) continue;
        const bool is_hot = h.id == hot;
        ImU32 cc = h.glued ? IM_COL32(150, 150, 150, 255) : (is_hot && dragging && joint_sides) ? kJoint : is_hot ? kYellow : kOrange;
        const ImVec2 c(h.sx, h.sy);
        if (h.corner) {
            dl->AddCircleFilled(c, hs * 1.05f, cc, 20);
            dl->AddCircle(c, hs * 1.05f, IM_COL32(0, 0, 0, 235), 20, 1.8f * dpi_);
        } else {
            dl->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), cc, 3.f * dpi_);
            dl->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), IM_COL32(0, 0, 0, 235), 3.f * dpi_, 0, 1.8f * dpi_);
        }
        if (h.glued) {   // a small padlock
            const float b = 4.5f * dpi_;
            dl->AddRectFilled(ImVec2(c.x - b, c.y - b * 0.2f), ImVec2(c.x + b, c.y + b), IM_COL32(40, 40, 40, 255), dpi_);
            dl->AddCircle(ImVec2(c.x, c.y - b * 0.2f), b * 0.65f, IM_COL32(40, 40, 40, 255), 12, 1.6f * dpi_);
            continue;
        }
        // A double arrow along the way it moves, on screen.
        float dx = 1, dy = 0, sx, sy;
        if (!h.corner) {
            const XMFLOAT3 a = (h.id & (kSideX0 | kSideX1)) ? lo.ax : lo.az;
            if (cam_.project({h.w.x + a.x, h.w.y + a.y, h.w.z + a.z}, vp_w, vp_h, sx, sy)) { dx = vp_x + sx - h.sx; dy = vp_y + sy - h.sy; }
        } else {
            dx = (h.id & kSideX1) ? 1.f : -1.f;
            dy = (h.id & kSideZ1) ? -1.f : 1.f;
        }
        const float len = std::hypot(dx, dy);
        if (len > 1e-3f) {
            dx /= len; dy /= len;
            const float r = hs * 0.62f, ah = hs * 0.3f;
            const ImVec2 a(c.x - dx * r, c.y - dy * r), b(c.x + dx * r, c.y + dy * r);
            const ImU32 ink = IM_COL32(25, 25, 25, 255);
            dl->AddLine(a, b, ink, 1.8f * dpi_);
            for (const ImVec2& e : {a, b}) {
                const float sgn = (e.x == a.x && e.y == a.y) ? 1.f : -1.f;
                dl->AddLine(e, ImVec2(e.x + sgn * (dx * ah) - dy * ah * 0.8f, e.y + sgn * (dy * ah) + dx * ah * 0.8f), ink, 1.8f * dpi_);
                dl->AddLine(e, ImVec2(e.x + sgn * (dx * ah) + dy * ah * 0.8f, e.y + sgn * (dy * ah) - dx * ah * 0.8f), ink, 1.8f * dpi_);
            }
        }
    }
    // The label beside the hot handle (hover or the panel's field), and the readout beside the cursor while dragging.
    auto tag = [&](ImVec2 at, const std::string& t, ImU32 ink) {
        const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        dl->AddRectFilled(ImVec2(at.x, at.y), ImVec2(at.x + ts.x + 8.f * dpi_, at.y + ts.y + 4.f * dpi_), IM_COL32(0, 0, 0, 205), 3.f * dpi_);
        dl->AddText(ImVec2(at.x + 4.f * dpi_, at.y + 2.f * dpi_), ink, t.c_str());
    };
    if (hot)
        for (const SizeHandle& h : lo.h)
            if (h.id == hot && !h.hidden && h.on_screen) {
                std::string t = h.label;
                if (h.glued) t += " (glued)";
                tag(ImVec2(h.sx + hs + 6.f * dpi_, h.sy - hs), t, kYellow);
            }
    if (dragging && !size_readout_.empty()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        tag(ImVec2(mp.x + 18.f * dpi_, mp.y + 18.f * dpi_), size_readout_, IM_COL32(255, 255, 255, 255));
    }
    dl->PopClipRect();
}

// The `handles` read: one row per handle that shows, or why none does.
std::string App::automation_handles() {
    Json out = Json::object();
    Json rows = Json::array();
    SizeLayout lo;
    const bool ok = size_layout(lo);
    auto r2 = [](double v) { return std::round(v * 100.0) / 100.0; };
    if (ok)
        for (const SizeHandle& h : lo.h) {
            if (h.hidden) continue;
            Json row = Json::object();
            row.with("id", Json::number(h.id)).with("label", Json::string(h.label)).with("kind", Json::string(h.corner ? "corner" : "side"));
            row.with("sx", Json::number(std::round(h.sx))).with("sy", Json::number(std::round(h.sy)));
            row.with("x", Json::number(r2(h.w.x))).with("y", Json::number(r2(h.w.y))).with("z", Json::number(r2(h.w.z)));
            row.with("glued", Json::boolean(h.glued)).with("on_screen", Json::boolean(h.on_screen && h.sx >= vp_x && h.sy >= vp_y && h.sx < vp_x + vp_w && h.sy < vp_y + vp_h));
            rows.push(std::move(row));
        }
    if (ok && lo.si.kind == SizeKind::Water && knob_x_ >= 0) {   // the Move gizmo's lift knob is the water's Level handle
        Json row = Json::object();
        row.with("id", Json::number(16)).with("label", Json::string("level")).with("kind", Json::string("level"));
        row.with("sx", Json::number(std::round(knob_x_))).with("sy", Json::number(std::round(knob_y_)));
        row.with("x", Json::number(r2(lo.pos.x))).with("y", Json::number(r2(lo.pos.y))).with("z", Json::number(r2(lo.pos.z)));
        row.with("glued", Json::boolean(false)).with("on_screen", Json::boolean(true));
        rows.push(std::move(row));
    }
    out.with("rows", rows).with("count", Json::number(double(rows.a.size())));
    if (!ok) out.with("hidden_reason", Json::string(lo.reason));
    return out.dump();
}

Json App::size_gizmo_json() const {
    Json j = Json::object();
    const bool live = ImGui::GetTime() <= size_readout_until_;
    j.with("hot", Json::number(size_hot_ ? size_hot_ : size_panel_hot_)).with("drag", Json::number(size_drag_.id));
    j.with("label", Json::string(size_label_)).with("readout", Json::string(live ? size_readout_ : std::string()));
    j.with("cursor", Json::string(size_cursor_)).with("hint", Json::string(size_hint_));
    j.with("joint", Json::string(size_drag_.joint_text)).with("panel_hot", Json::number(size_panel_hot_));
    return j;
}

}  // namespace ed
