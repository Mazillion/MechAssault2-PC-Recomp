// Map gizmo for trigger shapes: every trigger box (MATriggerCuboidalVolume) and trigger circle
// (MATriggerRadius) of the map, retail ones and the Objectives & Triggers tab's alike (they are the same
// objects), gets the full set: move (X / Z / up / ground plane), turn (boxes) and size handles; the
// ordinary gizmo stays away from them. Conditions of older files that still keep their shape in
// their arguments get the same handles. Every drag is one undo step.
#include <algorithm>
#include <cmath>

#include "../core/triggers.h"
#include "app.h"
#include "imgui.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

namespace {

const float kPi = 3.14159265f, kDeg = 57.2957795f;

enum { T_NONE = 0, T_X, T_Z, T_Y, T_PLANE, T_YAW, T_FACE_RP, T_FACE_RN, T_FACE_FP, T_FACE_FN, T_TOP, T_RAD };

float wrap_pi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

float seg_dist(ImVec2 p, ImVec2 a, ImVec2 b) {
    float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y, c = vx * vx + vy * vy;
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

float numa(const std::map<std::string, std::string>& a, const char* k, float d) {
    auto it = a.find(k);
    return it == a.end() || it->second.empty() ? d : float(atof(it->second.c_str()));
}

std::string fnum(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.1f", v);
    return b;
}

struct Hnd {
    int id;
    XMFLOAT3 p;     // handle position (arrow tip for the move arrows)
    XMFLOAT3 axis;  // drag direction (outward for size handles)
};

struct Layout {
    std::vector<Hnd> sizes, arrows;
    XMFLOAT3 pv{};
    float L = 10;
    bool ring = false;
};

void make_layout(const TrigGeo& g, const Camera& cam, float ground, Layout& lo) {
    float h = g.hd / kDeg, ch = std::cos(h), sh = std::sin(h);
    XMFLOAT3 right{ch, 0, -sh}, fwd{sh, 0, ch};
    float my = g.box && !g.flat ? g.y0 + g.h * 0.5f : ground + 1.5f;
    lo.pv = {g.x, my, g.z};
    float dx = cam.eye.x - g.x, dy = cam.eye.y - my, dz = cam.eye.z - g.z;
    lo.L = std::max(2.f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.12f);
    if (g.box) {
        float hw = g.w * 0.5f, hl = g.l * 0.5f;
        lo.sizes.push_back({T_FACE_RP, {g.x + right.x * hw, my, g.z + right.z * hw}, right});
        lo.sizes.push_back({T_FACE_RN, {g.x - right.x * hw, my, g.z - right.z * hw}, {-right.x, 0, -right.z}});
        lo.sizes.push_back({T_FACE_FP, {g.x + fwd.x * hl, my, g.z + fwd.z * hl}, fwd});
        lo.sizes.push_back({T_FACE_FN, {g.x - fwd.x * hl, my, g.z - fwd.z * hl}, {-fwd.x, 0, -fwd.z}});
        if (!g.flat) lo.sizes.push_back({T_TOP, {g.x, g.y0 + g.h, g.z}, {0, 1, 0}});
    } else {
        lo.sizes.push_back({T_RAD, {g.x + g.r, my, g.z}, {1, 0, 0}});
    }
    {
        lo.arrows.push_back({T_X, {g.x + lo.L, my, g.z}, {1, 0, 0}});
        lo.arrows.push_back({T_Z, {g.x, my, g.z + lo.L}, {0, 0, 1}});
        if (g.box && !g.flat) lo.arrows.push_back({T_Y, {g.x, my + lo.L * 0.75f, g.z}, {0, 1, 0}});
        lo.ring = g.box;
    }
}

}  // namespace

// The shape being edited, in world terms. y0 = bottom edge of a box (a flat square or a circle lies on the ground).
bool App::trig_target(TrigGeo& g) {
    g = TrigGeo{};
    if (!doc_ || !editable() || tool_ != Tool::Select) return false;
    if (selection_.size() != 1) return false;
    static const uint32_t K_VOL = key_of("MATriggerCuboidalVolume"), K_RAD = key_of("MATriggerRadius"), K_NRAD = key_of("MATriggerNationRadius");
    for (auto& o : doc_->world.objs) {
        if (o.uid() != *selection_.begin() || (o.cls != K_VOL && o.cls != K_RAD && o.cls != K_NRAD)) continue;
        Vec3 p;
        if (!get_pos(o, p)) return false;
        g.o = &o;
        g.x = p.x; g.z = p.z;
        g.box = o.cls == K_VOL;
        if (g.box) {
            g.w = o.f32("VolumeWidth", 20); g.l = o.f32("VolumeLength", 20); g.h = o.f32("VolumeHeight", 10);
            g.hd = o.f32("VolumeHeading", 0);
            g.y0 = p.y - g.h * 0.5f;
        } else {
            g.r = o.cls == K_NRAD ? o.f32("m_fRadius", 20) : o.f32("EncounterRadius", 20);
            g.y0 = p.y;  // the circle's own height (drawn on the ground)
        }
        return true;
    }
    return false;
}

void App::trig_apply(const TrigGeo& g) {
    if (g.o) {
        if (g.box) {
            g.o->set_f32("VolumeWidth", g.w); g.o->set_f32("VolumeLength", g.l); g.o->set_f32("VolumeHeight", g.h);
            g.o->set_f32("VolumeHeading", g.hd);
            set_pos(*g.o, {g.x, g.y0 + g.h * 0.5f, g.z});
        } else {
            g.o->set_f32(g.o->find("m_fRadius") ? "m_fRadius" : "EncounterRadius", g.r);  // MATriggerNationRadius / MATriggerRadius
            set_pos(*g.o, {g.x, g.y0, g.z});
        }
        objects_dirty_ = minimap_dirty_ = true;
    }
    doc_->dirty = true;
}

bool App::trig_gizmo_update(bool lclick, bool ldown) {
    TrigGeo g;
    if (!trig_target(g)) {
        if (!ldown) tdrag_ = TrigDrag{};
        trig_hot_ = 0;
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    XMFLOAT3 ro, rd;
    cam_.ray(m.x - vp_x, m.y - vp_y, vp_w, vp_h, ro, rd);
    Layout lo;
    make_layout(g, cam_, doc_->terrain.height_at(g.x, g.z), lo);
    auto scr = [&](const XMFLOAT3& p, ImVec2& o) {
        float sx, sy;
        if (!cam_.project(p, vp_w, vp_h, sx, sy)) return false;
        o = ImVec2(sx + vp_x, sy + vp_y);
        return true;
    };
    const XMFLOAT3 pv = lo.pv;
    // Parameter of the point on the line P0 + s*A nearest the mouse ray.
    auto axis_param = [&](const XMFLOAT3& P0, const XMFLOAT3& A, float& s) {
        float wx = P0.x - ro.x, wy = P0.y - ro.y, wz = P0.z - ro.z;
        float b = A.x * rd.x + A.y * rd.y + A.z * rd.z, d = A.x * wx + A.y * wy + A.z * wz, e = rd.x * wx + rd.y * wy + rd.z * wz;
        float den = 1.f - b * b;
        if (den < 1e-4f) return false;
        s = (b * e - d) / den;
        return true;
    };

    if (tdrag_.handle) {
        if (!ldown) {
            tdrag_ = TrigDrag{};
            return true;
        }
        const float* v = tdrag_.v0;  // x z y0 w l h r hd
        TrigGeo n = g;
        n.x = v[0]; n.z = v[1]; n.y0 = v[2]; n.w = v[3]; n.l = v[4]; n.h = v[5]; n.r = v[6]; n.hd = v[7];
        float hr = v[7] / kDeg, ch = std::cos(hr), sh = std::sin(hr);
        const char* label = "move";
        bool changed = false;
        const XMFLOAT3 piv0{tdrag_.hit0.x, tdrag_.hit0.y, tdrag_.hit0.z};  // pivot (size handles: the handle) when the drag began
        switch (tdrag_.handle) {
            case T_X: case T_Z: case T_Y: {
                XMFLOAT3 A = tdrag_.handle == T_X ? XMFLOAT3{1, 0, 0} : tdrag_.handle == T_Y ? XMFLOAT3{0, 1, 0} : XMFLOAT3{0, 0, 1};
                float s;
                if (!axis_param(piv0, A, s)) return true;
                if (tdrag_.start > 1e29f) { tdrag_.start = s; return true; }
                float t = s - tdrag_.start;
                if (tdrag_.handle == T_X) n.x += t; else if (tdrag_.handle == T_Z) n.z += t; else n.y0 += t;
                changed = true;
                break;
            }
            case T_PLANE: {
                if (std::fabs(rd.y) < 1e-4f) return true;
                float t = (piv0.y - ro.y) / rd.y;
                if (t < 0) return true;
                XMFLOAT3 h{ro.x + rd.x * t, piv0.y, ro.z + rd.z * t};
                if (tdrag_.start > 1e29f) { tdrag_.start = 0; tdrag_.applied_v = {h.x, 0, h.z}; return true; }
                n.x += h.x - tdrag_.applied_v.x;
                n.z += h.z - tdrag_.applied_v.z;
                changed = true;
                break;
            }
            case T_YAW: {
                if (std::fabs(rd.y) < 0.25f) return true;
                float t = (piv0.y - ro.y) / rd.y;
                if (t < 0) return true;
                float ang = std::atan2(ro.x + rd.x * t - piv0.x, ro.z + rd.z * t - piv0.z);
                if (tdrag_.start > 1e29f) { tdrag_.start = ang; return true; }
                float d = wrap_pi(ang - tdrag_.start);
                if (io.KeyShift) d = std::round(d / (15.f / kDeg)) * (15.f / kDeg);
                n.hd = v[7] + d * kDeg;
                tdrag_.applied = d;
                label = "turn";
                changed = true;
                break;
            }
            default: {  // size handles: the face follows the mouse, the opposite side stays
                XMFLOAT3 A;
                switch (tdrag_.handle) {
                    case T_FACE_RP: A = {ch, 0, -sh}; break;
                    case T_FACE_RN: A = {-ch, 0, sh}; break;
                    case T_FACE_FP: A = {sh, 0, ch}; break;
                    case T_FACE_FN: A = {-sh, 0, -ch}; break;
                    case T_TOP: A = {0, 1, 0}; break;
                    default: A = {1, 0, 0}; break;
                }
                float s;
                if (!axis_param(piv0, A, s)) return true;
                if (tdrag_.start > 1e29f) { tdrag_.start = s; return true; }
                float t = s - tdrag_.start, grow = 0;
                switch (tdrag_.handle) {
                    case T_FACE_RP: case T_FACE_RN: n.w = std::max(1.f, v[3] + t); grow = n.w - v[3]; tdrag_.applied = n.w; label = "width"; break;
                    case T_FACE_FP: case T_FACE_FN: n.l = std::max(1.f, v[4] + t); grow = n.l - v[4]; tdrag_.applied = n.l; label = "length"; break;
                    case T_TOP: n.h = std::max(1.f, v[5] + t); tdrag_.applied = n.h; label = "height"; break;
                    default: n.r = std::max(1.f, v[6] + t); tdrag_.applied = n.r; label = "radius"; break;
                }
                n.x += A.x * grow * 0.5f;
                n.z += A.z * grow * 0.5f;
                changed = true;
                break;
            }
        }
        if (changed) {
            if (!tdrag_.pushed) { push_undo("resize trigger"); tdrag_.pushed = true; }
            trig_apply(n);
            tdrag_.label = label;
        }
        return true;
    }

    // Hover: size handles first, then arrows and the ground square, the turn ring last.
    int best = 0;
    float bd = 9.f * dpi_;
    for (auto& h : lo.sizes) {
        ImVec2 s;
        if (!scr(h.p, s)) continue;
        float d = std::hypot(s.x - m.x, s.y - m.y);
        if (d < bd) { bd = d; best = h.id; }
    }
    ImVec2 c;
    if (!best && vp_hover_ && scr(pv, c)) {
        bd = 8.f * dpi_;
        ImVec2 q[4];
        if (scr({pv.x + lo.L * 0.3f, pv.y, pv.z + lo.L * 0.3f}, q[0]) && scr({pv.x + lo.L * 0.65f, pv.y, pv.z + lo.L * 0.3f}, q[1]) &&
            scr({pv.x + lo.L * 0.65f, pv.y, pv.z + lo.L * 0.65f}, q[2]) && scr({pv.x + lo.L * 0.3f, pv.y, pv.z + lo.L * 0.65f}, q[3]) && in_quad(m, q))
            best = T_PLANE;
        else {
            for (auto& a : lo.arrows) {
                ImVec2 e;
                if (!scr(a.p, e)) continue;
                float d = seg_dist(m, c, e);
                if (d < bd) { bd = d; best = a.id; }
            }
            if (!best && lo.ring) {
                ImVec2 prev;
                bool have = false;
                for (int i = 0; i <= 64; i++) {
                    float t = float(i) / 64 * 2 * kPi;
                    ImVec2 s;
                    if (!scr({pv.x + std::sin(t) * lo.L * 1.45f, pv.y, pv.z + std::cos(t) * lo.L * 1.45f}, s)) { have = false; continue; }
                    if (have) { float d = seg_dist(m, prev, s); if (d < bd) { bd = d; best = T_YAW; } }
                    prev = s;
                    have = true;
                }
            }
        }
    }
    trig_hot_ = vp_hover_ ? best : 0;
    if (trig_hot_) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (trig_hot_ && lclick) {
        tdrag_ = TrigDrag{};
        tdrag_.handle = trig_hot_;
        tdrag_.start = 1e30f;
        float* v = tdrag_.v0;
        v[0] = g.x; v[1] = g.z; v[2] = g.y0; v[3] = g.w; v[4] = g.l; v[5] = g.h; v[6] = g.r; v[7] = g.hd;
        tdrag_.hit0 = {pv.x, pv.y, pv.z};
        for (auto& h : lo.sizes) if (h.id == trig_hot_) tdrag_.hit0 = {h.p.x, h.p.y, h.p.z};
        return true;
    }
    return false;
}

void App::trig_gizmo_draw() {
    TrigGeo g;
    if (!trig_target(g)) return;
    Layout lo;
    make_layout(g, cam_, doc_->terrain.height_at(g.x, g.z), lo);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    auto scr = [&](const XMFLOAT3& p, ImVec2& o) {
        float sx, sy;
        if (!cam_.project(p, vp_w, vp_h, sx, sy)) return false;
        o = ImVec2(sx + vp_x, sy + vp_y);
        return true;
    };
    int active = tdrag_.handle ? tdrag_.handle : trig_hot_;
    auto col = [&](int id, ImU32 base) { return id == active ? IM_COL32(255, 235, 90, 255) : base; };
    const XMFLOAT3 pv = lo.pv;
    ImVec2 c;
    if (scr(pv, c)) {
        dl->AddCircleFilled(c, 4 * dpi_, IM_COL32(255, 255, 255, 230));
        for (auto& a : lo.arrows) {
            ImVec2 e;
            if (!scr(a.p, e)) continue;
            ImU32 base = a.id == T_X ? IM_COL32(235, 70, 70, 255) : a.id == T_Z ? IM_COL32(70, 120, 245, 255) : IM_COL32(90, 215, 90, 255);
            ImU32 cc = col(a.id, base);
            dl->AddLine(c, e, IM_COL32(0, 0, 0, 160), 5 * dpi_);
            dl->AddLine(c, e, cc, 3 * dpi_);
            float dx = e.x - c.x, dy = e.y - c.y, len = std::sqrt(dx * dx + dy * dy);
            if (len > 1) {
                dx /= len; dy /= len;
                float s = 11 * dpi_;
                dl->AddTriangleFilled(ImVec2(e.x + dx * s, e.y + dy * s), ImVec2(e.x - dx * s * 0.2f - dy * s * 0.55f, e.y - dy * s * 0.2f + dx * s * 0.55f),
                                      ImVec2(e.x - dx * s * 0.2f + dy * s * 0.55f, e.y - dy * s * 0.2f - dx * s * 0.55f), cc);
            }
        }
        ImVec2 q[4];
        if (scr({pv.x + lo.L * 0.3f, pv.y, pv.z + lo.L * 0.3f}, q[0]) && scr({pv.x + lo.L * 0.65f, pv.y, pv.z + lo.L * 0.3f}, q[1]) &&
            scr({pv.x + lo.L * 0.65f, pv.y, pv.z + lo.L * 0.65f}, q[2]) && scr({pv.x + lo.L * 0.3f, pv.y, pv.z + lo.L * 0.65f}, q[3])) {
            ImU32 cc = col(T_PLANE, IM_COL32(255, 210, 60, 255));
            dl->AddConvexPolyFilled(q, 4, (cc & 0x00FFFFFFu) | 0x80000000u);
            dl->AddPolyline(q, 4, cc, ImDrawFlags_Closed, 2 * dpi_);
        }
        if (lo.ring) {
            std::vector<ImVec2> sp;
            for (int i = 0; i <= 64; i++) {
                float t = float(i) / 64 * 2 * kPi;
                ImVec2 s;
                if (scr({pv.x + std::sin(t) * lo.L * 1.45f, pv.y, pv.z + std::cos(t) * lo.L * 1.45f}, s)) sp.push_back(s);
            }
            if (sp.size() > 1) {
                dl->AddPolyline(sp.data(), int(sp.size()), IM_COL32(0, 0, 0, 160), 0, 5 * dpi_);
                dl->AddPolyline(sp.data(), int(sp.size()), col(T_YAW, IM_COL32(90, 215, 90, 255)), 0, 3 * dpi_);
            }
        }
    }
    for (auto& h : lo.sizes) {
        ImVec2 s;
        if (!scr(h.p, s)) continue;
        float r = 6 * dpi_;
        ImU32 cc = col(h.id, IM_COL32(255, 150, 30, 255));
        dl->AddRectFilled(ImVec2(s.x - r, s.y - r), ImVec2(s.x + r, s.y + r), cc, 2 * dpi_);
        dl->AddRect(ImVec2(s.x - r, s.y - r), ImVec2(s.x + r, s.y + r), IM_COL32(0, 0, 0, 230), 2 * dpi_, 0, 1.5f * dpi_);
    }
    if (tdrag_.handle) {
        char t[64];
        if (tdrag_.handle == T_YAW) snprintf(t, sizeof t, "Turn %.0f deg", tdrag_.applied * kDeg);
        else if (tdrag_.handle >= T_FACE_RP) snprintf(t, sizeof t, "%s %.1f m", tdrag_.label, tdrag_.applied);
        else snprintf(t, sizeof t, "Move");
        ImVec2 mp = ImGui::GetIO().MousePos, ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2(mp.x + 14 * dpi_, mp.y + 14 * dpi_), ImVec2(mp.x + 22 * dpi_ + ts.x, mp.y + 18 * dpi_ + ts.y), IM_COL32(0, 0, 0, 190), 3 * dpi_);
        dl->AddText(ImVec2(mp.x + 18 * dpi_, mp.y + 16 * dpi_), IM_COL32(255, 255, 255, 255), t);
    }
    dl->PopClipRect();
}

}  // namespace ed
