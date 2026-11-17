// Check scenario, the placement half: what the user would see about heights. Five checks, all warnings (the map still
// tests): flat pieces that hang in the air, flat pieces that overlap at one height (they flicker), objects under the ground, objects
// deep in the ground, and objects that hang with nothing under them. Each item names its objects (`uids`) and may carry a fix.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "../core/triggers.h"
#include "imgui.h"
#include "theme.h"
#include "ui_tips.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

namespace {
const uint32_t K_BUILDING_C = key_of("MABuilding"), K_POWERUP_C = key_of("MAPowerUp"), K_FLYER_C = key_of("MAFlyer"),
               K_MECH_C = key_of("MAMech"), K_TANK_C = key_of("MATank"), K_VEHICLE_C = key_of("MAVehicle"), K_TURRET_C = key_of("MATurret");

bool is_unit_c(uint32_t c) { return c == K_MECH_C || c == K_TANK_C || c == K_VEHICLE_C || c == K_TURRET_C || c == K_FLYER_C; }

struct Hit {
    uint32_t uid;
    float v;
    std::string name;
};

// "A, B and 2 others" for the first few names of a list.
std::string name_list(const std::vector<Hit>& h, size_t n = 3) {
    std::string s;
    for (size_t i = 0; i < h.size() && i < n; i++) s += (i ? ", " : "") + h[i].name;
    if (h.size() > n) s += " and " + std::to_string(h.size() - n) + " more";
    return s;
}

std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }
}  // namespace

// Whether a solid, upward-looking face of some other model lies within 0.5 m of `foot` under the map point (x, z): a vertical ray down
// from 0.5 m over the foot against the models' opaque triangles (as cursor_object_top does for the mouse ray).
bool App::solid_under_point(float x, float z, float foot, int skip_root) {
    const float top = foot + 0.5f, bottom = foot - 0.5f;
    for (const auto& v : views_) {
        if (v.kind != 0 || v.helper >= 0 || !v.model || v.pitch != 0 || v.roll != 0 || v.root == skip_root) continue;
        if (v.index < 0 || size_t(v.index) >= doc_->world.objs.size()) continue;
        const GpuModel& m = *v.model;
        if (m.state != GpuModel::Ready || !m.has_sbox) continue;
        const float ch = std::cos(v.heading), sh = std::sin(v.heading);
        const float ox = x - v.pos.x, oz = z - v.pos.z;
        const float lx = ch * ox - sh * oz, lz = sh * ox + ch * oz;
        if (lx < m.smin[0] || lx > m.smax[0] || lz < m.smin[2] || lz > m.smax[2]) continue;
        if (v.pos.y + m.smax[1] < bottom || v.pos.y + m.smin[1] > top) continue;
        const float* tr = m.solid_tris.data();
        for (size_t i = 0; i + 9 <= m.solid_tris.size(); i += 9, tr += 9) {
            // Barycentric test in the xz plane, then the face's height there.
            const float ax = tr[0], az = tr[2], bx = tr[3], bz = tr[5], cx = tr[6], cz = tr[8];
            const float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
            if (std::fabs(d) < 1e-9f) continue;
            const float u = ((bz - cz) * (lx - cx) + (cx - bx) * (lz - cz)) / d, w = ((cz - az) * (lx - cx) + (ax - cx) * (lz - cz)) / d;
            if (u < 0 || w < 0 || u + w > 1) continue;
            const float e1[3] = {tr[3] - tr[0], tr[4] - tr[1], tr[5] - tr[2]}, e2[3] = {tr[6] - tr[0], tr[7] - tr[1], tr[8] - tr[2]};
            const float ny = e1[2] * e2[0] - e1[0] * e2[2], nl = std::sqrt(std::pow(e1[1] * e2[2] - e1[2] * e2[1], 2) + ny * ny + std::pow(e1[0] * e2[1] - e1[1] * e2[0], 2));
            if (nl < 1e-8f || std::fabs(ny) < 0.5f * nl) continue;  // a wall holds nothing up
            const float y = v.pos.y + u * tr[1] + w * tr[4] + (1.f - u - w) * tr[7];
            if (y >= bottom && y <= top) return true;
        }
    }
    return false;
}

// How far the lowest surface under a long wall's footprint is below its foot (m); 0 for anything that is not a wall or fence, or is short.
// (The standing readout looks at the middle only, so a wall level at its centre over a slope read "On the ground" with one end hanging.)
float App::span_float(const WObj& o) {
    if (!doc_ || !cat_ || sheet_kind(o)) return 0.f;
    const TemplateInfo* ti = cat_->find(o.template_id());
    if (!ti || snap_family(*ti) != 1) return 0.f;
    Vec3 p;
    if (!get_pos(o, p)) return 0.f;
    GpuModel* gm = obj_model(o);
    if (!gm || gm->state != GpuModel::Ready || gm->bmax[0] <= gm->bmin[0]) return 0.f;
    const float ex = gm->bmax[0] - gm->bmin[0], ez = gm->bmax[2] - gm->bmin[2];
    if (std::max(ex, ez) < 8.f) return 0.f;
    const float yaw = heading(o), c = std::cos(yaw), s = std::sin(yaw), foot = p.y - stand_foot(o);
    float low = 1e30f;
    for (int i = 0; i <= 8; i++)
        for (int j = 0; j <= 2; j++) {
            const float lx = gm->bmin[0] + ex * float(i) / 8.f, lz = gm->bmin[2] + ez * float(j) / 2.f;
            const float wx = p.x + c * lx + s * lz, wz = p.z - s * lx + c * lz;
            if (!doc_->terrain.inside(wx, wz)) continue;
            low = std::min(low, surface_at(wx, wz, foot + place::kStandTol));
        }
    return low > 1e29f ? 0.f : std::max(0.f, foot - low);
}

void App::placement_checks(std::vector<ValidationItem>& out) {
    if (!doc_ || doc_->read_only || !cat_) return;
    const Terrain& t = doc_->terrain;
    auto name_of_obj = [&](const WObj& o) {
        std::string d = o.description();
        if (!d.empty() && d != "NULL") return plain_object_name(d, false);
        const TemplateInfo* ti = cat_->find(o.template_id());
        return ti ? plain_object_name(ti->name, false) : std::string("An object");
    };
    const SheetSet ss = sheets(true);

    // 1. Flat pieces that hang: floating over the ground, and no other flat piece under them to explain it.
    {
        std::vector<Hit> hang;
        for (size_t i = 0; i < ss.s.size(); i++) {
            const place::Sheet& s = ss.s[i];
            const float gap = place::hang_gap(t, s);
            if (gap <= place::float_tol(t)) continue;
            bool stacked = false;
            for (size_t j = 0; j < ss.s.size() && !stacked; j++) stacked = j != i && place::overlap(ss.s[j], s);
            if (stacked) continue;
            hang.push_back({s.uid, gap, name_of_obj(doc_->world.objs[size_t(ss.obj[i])])});
        }
        if (!hang.empty()) {
            std::sort(hang.begin(), hang.end(), [](const Hit& a, const Hit& b) { return a.v > b.v; });
            char b[360];
            if (hang.size() == 1) snprintf(b, sizeof b, "1 flat piece hangs in the air (%s, %.1f m up). In the game a floating piece cannot be seen from below and units walk under it.", hang[0].name.c_str(), hang[0].v);
            else snprintf(b, sizeof b, "%zu flat pieces hang in the air (the worst: %s, %.1f m up). In the game a floating piece cannot be seen from below and units walk under it.", hang.size(), hang[0].name.c_str(), hang[0].v);
            ValidationItem it{1, b, hang[0].uid};
            for (auto& h : hang) it.uids.push_back(h.uid);
            out.push_back(std::move(it));
        }
    }
    // 2. Flat pieces that overlap at one height: they flicker where they meet.
    {
        // Two road pieces that meet at one height are a retail junction (140 such pairs in the River City copy): not a mistake to report.
        std::vector<std::pair<int, int>> pairs;
        for (auto pr : place::same_height_pairs(ss.s))
            if (!(ss.s[size_t(pr.first)].kind == 1 && ss.s[size_t(pr.second)].kind == 1)) pairs.push_back(pr);
        size_t shown = 0;
        for (auto [i, j] : pairs) {
            if (shown++ >= 10) break;
            uint32_t a = ss.s[size_t(i)].uid, b = ss.s[size_t(j)].uid;
            if (a > b) std::swap(a, b);  // b = the newer one
            const WObj* oa = object_by_uid(doc_->world, a);
            const WObj* ob = object_by_uid(doc_->world, b);
            ValidationItem it{1, "2 flat pieces overlap at the same height and will flicker: " + (oa ? name_of_obj(*oa) : std::string("a piece")) + " and " +
                                     (ob ? name_of_obj(*ob) : std::string("a piece")) + ".", a};
            it.uids = {a, b};
            it.fix = 2;
            out.push_back(std::move(it));
        }
        if (pairs.size() > 10) out.push_back({1, "and " + std::to_string(pairs.size() - 10) + " more pairs of flat pieces overlap at the same height.", 0});
    }
    // 3-5. Objects against the ground: under it, deep in it, hanging over it. Roots only (a part goes where its root goes).
    std::vector<Hit> buried, deep, hanging, spans;
    {
        WTree wt;
        wt.build(doc_->world);
        for (size_t i = 0; i < doc_->world.objs.size(); i++) {
            const WObj& o = doc_->world.objs[i];
            if (o.cls == K_WATER || o.cls == K_LEVELSTART) continue;
            const bool unit = is_unit_c(o.cls), pickup = o.cls == K_POWERUP_C, building = o.cls == K_BUILDING_C;
            bool pad = false;
            int padk = -1;  // the unit waiting on the pad
            std::string padname;
            if (!unit && !building && !pickup) {  // a vehicle pad: a plain object with the unit that waits on it somewhere below (Corporeal > Spawner > vehicle)
                std::vector<int> stack(wt.kids[i].begin(), wt.kids[i].end());
                for (int depth = 0; depth < 3 && !stack.empty() && !pad; depth++) {
                    std::vector<int> next;
                    for (int k : stack) {
                        const uint32_t c = doc_->world.objs[size_t(k)].cls;
                        if (!pad && (c == K_MECH_C || c == K_TANK_C || c == K_VEHICLE_C || c == K_FLYER_C)) { pad = true; padk = k; padname = name_of_obj(doc_->world.objs[size_t(k)]); }
                        next.insert(next.end(), wt.kids[size_t(k)].begin(), wt.kids[size_t(k)].end());
                    }
                    stack = std::move(next);
                }
            }
            Vec3 p, q;
            if (!get_pos(o, p) || sheet_kind(o) || helper_group(o.cls) >= 0 || !t.inside(p.x, p.z)) continue;
            bool part = false;
            for (int a = wt.parent_of[i]; a >= 0 && !part; a = wt.parent_of[size_t(a)]) part = get_pos(doc_->world.objs[size_t(a)], q);
            if (part) continue;
            GpuModel* gm = (!pad && has_ground_model(o)) ? obj_model(o) : nullptr;
            float x, z;
            stand_xz(o, p, x, z);
            // Under the ground: nothing of the model is above it.
            if (gm) {
                const float g = t.height_at(x, z), top = p.y + gm->bmax[1];
                if (top < g - 0.05f) { buried.push_back({o.uid(), g - top, name_of_obj(o)}); continue; }
            }
            if (!(unit || pickup || pad || building)) {
                // A wall joined up a slope keeps its neighbour's height and sinks into the hill: "deep" like a unit that is sunk.
                if (gm) {
                    const TemplateInfo* ti = cat_->find(o.template_id());
                    if (ti && snap_family(*ti) == 1) {
                        const float wfoot = p.y - stand_foot(o), wsf = surface_at(x, z, wfoot + place::kStandTol);
                        const float wh = std::max(0.f, gm->bmax[1] - gm->bmin[1]);
                        if (wsf - wfoot > std::max(1.f, 0.25f * wh)) { deep.push_back({o.uid(), wsf - wfoot, name_of_obj(o)}); continue; }
                    }
                }
                // A long wall or fence on a slope: one end may hang in the air while its middle rests on the ground.
                if (const float sp = span_float(o); sp > place::float_tol(t)) spans.push_back({o.uid(), sp, name_of_obj(o)});
                continue;
            }
            float foot = p.y - stand_foot(o);
            // A pad is judged by the lowest of the pad and the vehicle on it: a vehicle moved down relative to its pad is buried too.
            if (pad && padk >= 0 && doc_->world.objs[size_t(padk)].cls != K_FLYER_C) {  // (a VTOL sits on its pad at its own height)
                const WObj& vo = doc_->world.objs[size_t(padk)];
                if (Vec3 vp; get_pos(vo, vp)) {
                    float vx, vz;
                    stand_xz(vo, vp, vx, vz);
                    const float vfoot = vp.y - stand_foot(vo);
                    if (vfoot < foot) { foot = vfoot; x = vx; z = vz; }
                }
            }
            const float sf = surface_at(x, z, foot + place::kStandTol);
            const float height = gm ? std::max(0.f, gm->bmax[1] - gm->bmin[1]) : 0.f;
            // A building is meant to sink its foundations (the new map's VTOL pads sit 5 m down): only "under the ground" (above) reports one.
            if (!building && sf - foot > std::max(1.f, 0.25f * height)) { deep.push_back({o.uid(), sf - foot, pad ? (padname.empty() ? std::string("Vehicle pad") : padname + " pad") : name_of_obj(o)}); continue; }
            if (o.cls == K_FLYER_C || pad) continue;
            if (foot - sf > 0.5f && !solid_under_point(x, z, foot, int(i)))
                hanging.push_back({o.uid(), foot - sf, name_of_obj(o)});
        }
    }
    auto sorted = [](std::vector<Hit>& h) { std::sort(h.begin(), h.end(), [](const Hit& a, const Hit& b) { return a.v > b.v; }); };
    if (!buried.empty()) {
        sorted(buried);
        ValidationItem it{1, plural(buried.size(), "object is", "objects are") + " under the ground and cannot be seen (" + name_list(buried) + ").", buried[0].uid};
        for (auto& h : buried) it.uids.push_back(h.uid);
        it.fix = 1;
        out.push_back(std::move(it));
    }
    if (!deep.empty()) {
        sorted(deep);
        char b[360];
        snprintf(b, sizeof b, "%s stand%s deep in the ground (the worst: %s, %.1f m). If that is not on purpose, select %s and press %s.", plural(deep.size(), "object", "objects").c_str(),
                 deep.size() == 1 ? "s" : "", deep[0].name.c_str(), deep[0].v, deep.size() == 1 ? "it" : "them", kt(A_SNAP).c_str());
        ValidationItem it{1, b, deep[0].uid};
        for (auto& h : deep) it.uids.push_back(h.uid);
        it.fix = 1;
        out.push_back(std::move(it));
    }
    if (!spans.empty()) {
        sorted(spans);
        char b[360];
        snprintf(b, sizeof b, "%s hang%s in the air over the slope (%s%s, up to %.1f m at one end). Put %s on flatter ground, or raise the ground under %s.", plural(spans.size(), "wall", "walls").c_str(),
                 spans.size() == 1 ? "s" : "", spans.size() > 1 ? "the worst: " : "", spans[0].name.c_str(), spans[0].v, spans.size() == 1 ? "it" : "them", spans.size() == 1 ? "it" : "them");
        ValidationItem it{1, b, spans[0].uid};
        for (auto& h : spans) it.uids.push_back(h.uid);
        out.push_back(std::move(it));
    }
    if (!hanging.empty()) {
        sorted(hanging);
        char b[300];
        snprintf(b, sizeof b, "%s hang%s in the air with nothing under %s (%s%s, %.1f m up).", plural(hanging.size(), "object", "objects").c_str(), hanging.size() == 1 ? "s" : "",
                 hanging.size() == 1 ? "it" : "them", hanging.size() > 1 ? "the worst: " : "", hanging[0].name.c_str(), hanging[0].v);
        ValidationItem it{1, b, hanging[0].uid};
        for (auto& h : hanging) it.uids.push_back(h.uid);
        it.fix = 3;
        out.push_back(std::move(it));
    }
}

// The fix button of a placement item.
void App::apply_placement_fix(int fix, const std::vector<uint32_t>& uids) {
    if (!doc_ || !editable() || uids.empty()) return;
    if (fix == 4) {  // shots that fly from the template's flyby marker: play from their own view (core/cutscene)
        push_undo("play from its own view");
        for (uint32_t u : uids) cutscene_play_from_own_view(*doc_, u);
        world_changed();
    } else if (fix == 2) {  // the newer of two flat pieces goes on top
        selection_ = {uids.back()};
        layer_forward();
    } else if (fix == 1 || fix == 3) {  // G on them
        selection_ = std::set<uint32_t>(uids.begin(), uids.end());
        snap_selection_to_ground();
    }
}

// "Show" / "Show next": selects the object, aims the camera at it from a low side view (a gap under a piece shows), and steps on to
// the next object of the item on the next press.
void App::check_show(const ValidationItem& v) {
    if (!doc_) return;
    const std::vector<uint32_t> list = v.uids.empty() ? std::vector<uint32_t>{v.uid} : v.uids;
    size_t& pos = check_show_pos_[v.uid];
    const uint32_t uid = list[pos % list.size()];
    pos = (pos + 1) % list.size();
    const WObj* o = object_by_uid(doc_->world, uid);
    if (!o) return;
    selection_ = {uid};
    Vec3 pv;
    if (gizmo_pivot(pv)) { cam_.tx = pv.x; cam_.ty = pv.y; cam_.tz = pv.z; }
    else if (Vec3 p; get_pos(*o, p)) { cam_.tx = p.x; cam_.ty = p.y; cam_.tz = p.z; }
    float side = 10.f;
    if (place::Sheet s; object_footprint(*o, s)) side = std::max(s.fp.x1 - s.fp.x0, s.fp.z1 - s.fp.z0);
    cam_.dist = std::clamp(3.f * side, 30.f, 250.f);
    cam_.pitch = 0.45f;
    // Aim clear of the Check window: the object goes to the middle of the biggest free part of the view (left of the window,
    // else above it, else below it). The view is shifted so the object, not the target, lands there.
    if (check_rect_[2] > 0 && vp_w > 0 && vp_h > 0) {
        const float wx0 = check_rect_[0] - vp_x, wy0 = check_rect_[1] - vp_y, wx1 = wx0 + check_rect_[2], wy1 = wy0 + check_rect_[3];
        float gx = vp_w * 0.5f, gy = vp_h * 0.5f;
        const float left = wx0, above = wy0, right = vp_w - wx1, below = vp_h - wy1;
        if (left >= 140.f * dpi_ && left >= above) gx = left * 0.5f;
        else if (right >= 140.f * dpi_ && right >= above) gx = wx1 + right * 0.5f;
        else if (above >= 120.f * dpi_ && above >= below) gy = above * 0.5f;
        else if (below >= 120.f * dpi_) gy = wy1 + below * 0.5f;
        if (gx != vp_w * 0.5f || gy != vp_h * 0.5f) {
            // Walk the target until the object's own screen spot is the free spot (the ground plane at the object's height; a few passes: the
            // view is a perspective, so one shift is not exact).
            const XMFLOAT3 P{cam_.tx, cam_.ty, cam_.tz};
            for (int it = 0; it < 12; it++) {
                cam_.update(vp_w / vp_h);
                float sx = 0, sy = 0;
                if (!cam_.project(P, vp_w, vp_h, sx, sy)) break;
                const float ex = gx - sx, ey = gy - sy;
                if (std::fabs(ex) + std::fabs(ey) < 2.f) break;
                XMFLOAT3 o0, d0, o1, d1;
                cam_.ray(vp_w * 0.5f - ex, vp_h * 0.5f - ey, vp_w, vp_h, o1, d1);
                cam_.ray(vp_w * 0.5f, vp_h * 0.5f, vp_w, vp_h, o0, d0);
                if (std::fabs(d1.y) < 1e-4f || std::fabs(d0.y) < 1e-4f) break;
                const float k1 = (P.y - o1.y) / d1.y, k0 = (P.y - o0.y) / d0.y;
                if (k1 <= 0.f || k0 <= 0.f) break;
                cam_.tx += (o1.x + d1.x * k1) - (o0.x + d0.x * k0);
                cam_.tz += (o1.z + d1.z * k1) - (o0.z + d0.z * k0);
            }
        }
    }
}

}  // namespace ed
