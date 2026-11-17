// Sheets and layers (placement.h).
#include "placement.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>

namespace ma2::place {

namespace {

// A footprint as a quad on the map, plus its box.
struct Poly {
    float x[4] = {}, z[4] = {};
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0;
};

// Rotation = Ry(yaw) * Rx(pitch) * Rz(roll), column vectors (orient_instance, gizmo.cpp): rows of the model -> map matrix.
struct Mat {
    float r0[3], r1[3], r2[3];
};
Mat mat_of(float h, float p, float r) {
    const float ch = std::cos(h), sh = std::sin(h), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r);
    return {{ch * cr + sh * sp * sr, -ch * sr + sh * sp * cr, sh * cp},
            {cp * sr, cp * cr, -sp},
            {-sh * cr + ch * sp * sr, sh * sr + ch * sp * cr, ch * cp}};
}

Poly poly_of(const Sheet& s, float shrink) {
    Rect r = s.fp;
    // Shrink each side by `shrink` but never past 40% of the width.
    const float sx = std::min(shrink, 0.4f * (r.x1 - r.x0)), sz = std::min(shrink, 0.4f * (r.z1 - r.z0));
    r.x0 += sx; r.x1 -= sx; r.z0 += sz; r.z1 -= sz;
    const Mat m = mat_of(s.yaw, s.pitch, s.roll);
    const float lx[4] = {r.x0, r.x1, r.x1, r.x0}, lz[4] = {r.z0, r.z0, r.z1, r.z1};
    Poly p;
    for (int i = 0; i < 4; i++) {
        p.x[i] = m.r0[0] * lx[i] + m.r0[2] * lz[i] + s.pos.x;
        p.z[i] = m.r2[0] * lx[i] + m.r2[2] * lz[i] + s.pos.z;
    }
    p.x0 = p.x1 = p.x[0];
    p.z0 = p.z1 = p.z[0];
    for (int i = 1; i < 4; i++) {
        p.x0 = std::min(p.x0, p.x[i]); p.x1 = std::max(p.x1, p.x[i]);
        p.z0 = std::min(p.z0, p.z[i]); p.z1 = std::max(p.z1, p.z[i]);
    }
    return p;
}

bool poly_contains(const Poly& p, float x, float z, float eps = 1e-4f) {
    if (x < p.x0 - eps || x > p.x1 + eps || z < p.z0 - eps || z > p.z1 + eps) return false;
    bool pos = false, neg = false;
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) & 3;
        const float c = (p.x[j] - p.x[i]) * (z - p.z[i]) - (p.z[j] - p.z[i]) * (x - p.x[i]);
        if (c > eps) pos = true;
        if (c < -eps) neg = true;
    }
    return !(pos && neg);
}

// Separating axis test over the edge normals of both quads; touching counts as apart.
bool poly_overlap(const Poly& a, const Poly& b) {
    if (a.x1 <= b.x0 || b.x1 <= a.x0 || a.z1 <= b.z0 || b.z1 <= a.z0) return false;
    for (const Poly* q : {&a, &b})
        for (int i = 0; i < 4; i++) {
            const int j = (i + 1) & 3;
            const float ax = -(q->z[j] - q->z[i]), az = q->x[j] - q->x[i];  // edge normal
            float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
            for (int k = 0; k < 4; k++) {
                const float da = a.x[k] * ax + a.z[k] * az, db = b.x[k] * ax + b.z[k] * az;
                a0 = std::min(a0, da); a1 = std::max(a1, da);
                b0 = std::min(b0, db); b1 = std::max(b1, db);
            }
            const float len = std::sqrt(ax * ax + az * az);
            if (len < 1e-9f) continue;
            if (a1 <= b0 + 1e-6f * len || b1 <= a0 + 1e-6f * len) return false;
        }
    return true;
}

// The highest (or lowest) ground under a footprint. The ground is a mesh of triangles split on (gx,gz)-(gx+1,gz+1), so the
// extremes of a convex footprint are at its corners, at the grid points inside it, or where its edges cross a grid line or a
// diagonal: all of those are visited.
float ground_extreme(const Terrain& t, const Sheet& s, bool want_max) {
    const Poly p = poly_of(s, 0.f);
    float best = want_max ? -1e30f : 1e30f;
    auto take = [&](float x, float z) {
        if (!t.inside(x, z)) return;
        const float h = t.height_at(x, z);
        best = want_max ? std::max(best, h) : std::min(best, h);
    };
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) & 3;
        const float ax = p.x[i], az = p.z[i], dx = p.x[j] - ax, dz = p.z[j] - az;
        std::vector<float> ts{0.f, 1.f};
        auto lines = [&](float a, float d, float step) {  // t where a + d t crosses a multiple of step
            if (std::fabs(d) < 1e-6f) return;
            const float lo = std::min(a, a + d), hi = std::max(a, a + d);
            for (int k = int(std::ceil(lo / step)); float(k) * step <= hi; k++) {
                const float tt = (float(k) * step - a) / d;
                if (tt > 0.f && tt < 1.f) ts.push_back(tt);
            }
        };
        lines(ax, dx, t.scale);
        lines(az, dz, t.scale);
        lines(ax - az, dx - dz, t.scale);  // the diagonals: x - z = k scale
        for (float tt : ts) take(ax + dx * tt, az + dz * tt);
    }
    const int gx0 = std::max(0, int(std::floor(p.x0 / t.scale))), gx1 = std::min(t.w - 1, int(std::ceil(p.x1 / t.scale)));
    const int gz0 = std::max(0, int(std::floor(p.z0 / t.scale))), gz1 = std::min(t.h - 1, int(std::ceil(p.z1 / t.scale)));
    for (int gz = gz0; gz <= gz1; gz++)
        for (int gx = gx0; gx <= gx1; gx++) {
            const float x = float(gx) * t.scale, z = float(gz) * t.scale;
            if (poly_contains(p, x, z)) {
                const float h = t.step_at(gx, gz);
                best = want_max ? std::max(best, h) : std::min(best, h);
            }
        }
    if (best > 1e29f || best < -1e29f) return t.height_at(s.pos.x, s.pos.z);  // entirely off the map: the nearest ground
    return best;
}

// Seat order: by layer; sheets sharing a layer put the plate under the road, then the older one under the newer.
bool less_than(const Sheet& a, const Sheet& b) {
    if (a.layer != b.layer) return a.layer < b.layer;
    if ((a.kind == 1) != (b.kind == 1)) return b.kind == 1;
    return a.uid < b.uid;
}

}  // namespace

Vec3 up_of(float yaw, float pitch, float roll) {
    const Mat m = mat_of(yaw, pitch, roll);
    return {m.r0[1], m.r1[1], m.r2[1]};
}

Vec3 rotate_local(float yaw, float pitch, float roll, float x, float y, float z) {
    const Mat m = mat_of(yaw, pitch, roll);
    return {m.r0[0] * x + m.r0[1] * y + m.r0[2] * z, m.r1[0] * x + m.r1[1] * y + m.r1[2] * z, m.r2[0] * x + m.r2[1] * y + m.r2[2] * z};
}

void tilt_for_up(Vec3 up, float yaw, float& pitch, float& roll) {
    const float ch = std::cos(yaw), sh = std::sin(yaw);
    const float a = ch * up.x - sh * up.z, c = sh * up.x + ch * up.z;
    roll = -std::asin(std::clamp(a, -1.f, 1.f));
    pitch = std::atan2(c, up.y);
}

float face_y(const Sheet& s, float face, float x, float z) {
    const Vec3 u = up_of(s.yaw, s.pitch, s.roll);
    const float px = s.pos.x + u.x * face, py = s.pos.y + u.y * face, pz = s.pos.z + u.z * face;
    if (std::fabs(u.y) < 1e-4f) return py;
    return py - (u.x * (x - px) + u.z * (z - pz)) / u.y;
}

bool contains(const Sheet& s, float x, float z) { return poly_contains(poly_of(s, 0.f), x, z); }

bool overlap(const Sheet& a, const Sheet& b, float margin) { return poly_overlap(poly_of(a, margin * 0.5f), poly_of(b, margin * 0.5f)); }

void corners(const Sheet& s, float x[4], float z[4]) {
    const Poly p = poly_of(s, 0.f);
    for (int i = 0; i < 4; i++) { x[i] = p.x[i]; z[i] = p.z[i]; }
}

std::vector<std::pair<int, int>> grid_under(const Sheet& s, float scale, int grow_cells) {
    std::vector<std::pair<int, int>> out;
    if (scale <= 0.f) return out;
    const Poly p = poly_of(s, -scale * float(grow_cells));
    const int gx0 = int(std::floor(p.x0 / scale)), gx1 = int(std::ceil(p.x1 / scale));
    const int gz0 = int(std::floor(p.z0 / scale)), gz1 = int(std::ceil(p.z1 / scale));
    for (int gz = gz0; gz <= gz1; gz++)
        for (int gx = gx0; gx <= gx1; gx++)
            if (poly_contains(p, float(gx) * scale, float(gz) * scale)) out.push_back({gx, gz});
    return out;
}

float ground_max(const Terrain& t, const Sheet& s) { return ground_extreme(t, s, true); }
float ground_min(const Terrain& t, const Sheet& s) { return ground_extreme(t, s, false); }

float top_over(const Sheet& lower, const Sheet& upper) {
    if (lower.level()) return lower.pos.y + lower.top;
    // A tilted lower sheet: its top face is a plane, so its highest point over the part of the upper footprint that lies on
    // it is at a corner of the overlap (the two footprints cut against each other).
    const Poly lo = poly_of(lower, 0.f);
    std::vector<std::pair<float, float>> poly;
    {
        const Poly up = poly_of(upper, 0.f);
        for (int k = 0; k < 4; k++) poly.push_back({up.x[k], up.z[k]});
        float area = 0;
        for (int i = 0; i < 4; i++) { const int j = (i + 1) & 3; area += lo.x[i] * lo.z[j] - lo.x[j] * lo.z[i]; }
        const float sgn = area >= 0 ? 1.f : -1.f;
        for (int i = 0; i < 4 && !poly.empty(); i++) {  // Sutherland-Hodgman against each edge of the lower footprint
            const int j = (i + 1) & 3;
            const float ex = lo.x[j] - lo.x[i], ez = lo.z[j] - lo.z[i];
            auto side = [&](const std::pair<float, float>& p) { return sgn * (ex * (p.second - lo.z[i]) - ez * (p.first - lo.x[i])); };
            std::vector<std::pair<float, float>> out;
            for (size_t k = 0; k < poly.size(); k++) {
                const auto &a2 = poly[k], &b2 = poly[(k + 1) % poly.size()];
                const float sa = side(a2), sb = side(b2);
                if (sa >= 0) out.push_back(a2);
                if ((sa >= 0) != (sb >= 0)) {
                    const float f = sa / (sa - sb);
                    out.push_back({a2.first + (b2.first - a2.first) * f, a2.second + (b2.second - a2.second) * f});
                }
            }
            poly.swap(out);
        }
    }
    float best = -1e30f;
    for (auto& p : poly) best = std::max(best, face_y(lower, lower.top, p.first, p.second));
    return best < -1e29f ? lower.pos.y + lower.top : best;
}

int top_layer_under(const std::vector<Sheet>& all, const Sheet& s) {
    int best = INT_MIN;
    const Poly ps = poly_of(s, kOverlapMargin * 0.5f);
    for (const Sheet& j : all) {
        if (j.uid == s.uid && s.uid != 0) continue;
        if (poly_overlap(poly_of(j, kOverlapMargin * 0.5f), ps)) best = std::max(best, j.layer);
    }
    return best;
}

int bottom_layer_under(const std::vector<Sheet>& all, const Sheet& s) {
    int best = INT_MAX;
    const Poly ps = poly_of(s, kOverlapMargin * 0.5f);
    for (const Sheet& j : all) {
        if (j.uid == s.uid && s.uid != 0) continue;
        if (poly_overlap(poly_of(j, kOverlapMargin * 0.5f), ps)) best = std::min(best, j.layer);
    }
    return best;
}

float rest_y(const Terrain& t, const std::vector<Sheet>& all, const Sheet& s) {
    if (s.run >= 0) {  // a joined plate: the whole run is seated together, so ask what the seating gives it
        std::vector<Sheet> w = all;
        size_t me = w.size();
        for (size_t i = 0; i < w.size(); i++)
            if (w[i].uid == s.uid && s.uid != 0) me = i;
        if (me == w.size()) w.push_back(s);
        else w[me] = s;
        seat(t, w);
        return w[me].pos.y;
    }
    float need = ground_max(t, s) + kSheetLift;
    const Poly ps = poly_of(s, kOverlapMargin * 0.5f);
    for (const Sheet& j : all) {
        if (j.uid == s.uid && s.uid != 0) continue;
        if (!less_than(j, s)) continue;
        if (!poly_overlap(poly_of(j, kOverlapMargin * 0.5f), ps)) continue;
        need = std::max(need, top_over(j, s) + kSheetStep);
    }
    return need - s.under;
}

std::vector<float> seat(const Terrain& t, std::vector<Sheet>& all) {
    const size_t n = all.size();
    std::vector<float> moved(n, 0.f);
    if (!n) return moved;
    // Units: a run is one unit, a sheet alone is its own.
    std::map<int, std::vector<size_t>> runs;
    std::vector<std::vector<size_t>> units;
    for (size_t i = 0; i < n; i++) {
        if (all[i].run >= 0) runs[all[i].run].push_back(i);
        else units.push_back({i});
    }
    for (auto& [id, m] : runs) units.push_back(m);
    struct U { std::vector<size_t> m; int layer; uint32_t uid; bool fixed; bool road; };
    std::vector<U> us;
    for (auto& m : units) {
        U u{m, INT_MIN, 0xFFFFFFFFu, false, false};
        for (size_t i : m) {
            u.layer = std::max(u.layer, all[i].layer);
            u.uid = std::min(u.uid, all[i].uid);
            u.fixed = u.fixed || all[i].fixed;
            u.road = u.road || all[i].kind == 1;
        }
        for (size_t i : m) all[i].layer = u.layer;  // members of a run share one layer
        us.push_back(std::move(u));
    }
    std::sort(us.begin(), us.end(), [](const U& a, const U& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.road != b.road) return b.road;
        return a.uid < b.uid;
    });
    std::vector<Poly> polys(n);
    for (size_t i = 0; i < n; i++) polys[i] = poly_of(all[i], kOverlapMargin * 0.5f);
    std::vector<size_t> done;  // sheets of the units already seated, fixed ones included
    for (const U& u : us) {
        if (!u.fixed) {
            std::vector<float> ys(u.m.size());
            float top = -1e30f;
            for (size_t k = 0; k < u.m.size(); k++) {
                const Sheet& m = all[u.m[k]];
                float need = ground_max(t, m) + kSheetLift;
                for (size_t j : done)
                    if (poly_overlap(polys[j], polys[u.m[k]])) need = std::max(need, top_over(all[j], m) + kSheetStep);
                ys[k] = need - m.under;
                top = std::max(top, ys[k] + m.top);
            }
            for (size_t k = 0; k < u.m.size(); k++) {
                Sheet& m = all[u.m[k]];
                const float y = top - m.top;  // the tops of a run are level
                if (std::fabs(y - m.pos.y) > 0.0001f) {
                    moved[u.m[k]] = y - m.pos.y;
                    m.pos.y = y;
                }
            }
        }
        for (size_t i : u.m) done.push_back(i);
    }
    return moved;
}

namespace {
// Points of the underside (a 2 m grid over the footprint, edges included) on the map: x, z, and the height of the underside there.
struct Smp { float lx, lz, x, y, z; };
void samples(const Sheet& s, std::vector<Smp>& out) {
    out.clear();
    const Mat m = mat_of(s.yaw, s.pitch, s.roll);
    const int nx = std::max(2, int(std::ceil((s.fp.x1 - s.fp.x0) / 2.f)) + 1), nz = std::max(2, int(std::ceil((s.fp.z1 - s.fp.z0) / 2.f)) + 1);
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++) {
            Smp p;
            p.lx = s.fp.x0 + (s.fp.x1 - s.fp.x0) * float(i) / float(nx - 1);
            p.lz = s.fp.z0 + (s.fp.z1 - s.fp.z0) * float(j) / float(nz - 1);
            p.x = s.pos.x + m.r0[0] * p.lx + m.r0[1] * s.under + m.r0[2] * p.lz;
            p.y = s.pos.y + m.r1[0] * p.lx + m.r1[1] * s.under + m.r1[2] * p.lz;
            p.z = s.pos.z + m.r2[0] * p.lx + m.r2[1] * s.under + m.r2[2] * p.lz;
            out.push_back(p);
        }
}
}  // namespace

float min_clearance(const Terrain& t, const Sheet& s) {
    std::vector<Smp> v;
    samples(s, v);
    float best = 1e30f;
    for (const Smp& p : v)
        if (t.inside(p.x, p.z)) best = std::min(best, p.y - t.height_at(p.x, p.z));
    return best > 1e29f ? 0.f : best;
}

float float_gap(const Terrain& t, const Sheet& s) {
    if (s.level()) return ground_max(t, s) - ground_min(t, s);
    std::vector<Smp> v;
    samples(s, v);
    float worst = 0;
    for (const Smp& p : v)
        if (t.inside(p.x, p.z)) worst = std::max(worst, p.y - t.height_at(p.x, p.z) - kSheetLift);
    return worst;
}

float hang_gap(const Terrain& t, const Sheet& s) {
    if (!s.level()) return float_gap(t, s);
    return std::max(0.f, s.pos.y + s.under - kSheetLift - ground_min(t, s));
}

void fit_tilt(const Terrain& t, Sheet& s, bool joint_fixed, float joint_roll, float pivot_x, float pivot_z) {
    std::vector<Smp> v;
    if (!joint_fixed) {
        Sheet lv = s;
        lv.pitch = lv.roll = 0;
        samples(lv, v);
        double n = 0, mx = 0, mz = 0, mh = 0;
        std::vector<std::array<float, 3>> pts;
        for (const Smp& p : v)
            if (t.inside(p.x, p.z)) { pts.push_back({p.x, p.z, t.height_at(p.x, p.z)}); mx += p.x; mz += p.z; mh += pts.back()[2]; n += 1; }
        float b = 0, c = 0;
        if (n >= 3) {
            mx /= n; mz /= n; mh /= n;
            double sxx = 0, sxz = 0, szz = 0, sxh = 0, szh = 0;
            for (auto& q : pts) {
                const double dx = q[0] - mx, dz = q[1] - mz, dh = q[2] - mh;
                sxx += dx * dx; sxz += dx * dz; szz += dz * dz; sxh += dx * dh; szh += dz * dh;
            }
            const double det = sxx * szz - sxz * sxz;
            if (std::fabs(det) > 1e-9) { b = float((sxh * szz - szh * sxz) / det); c = float((szh * sxx - sxh * sxz) / det); }
        }
        Vec3 up{-b, 1.f, -c};
        const float L = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
        up = {up.x / L, up.y / L, up.z / L};
        tilt_for_up(up, s.yaw, s.pitch, s.roll);
        s.pitch = std::clamp(s.pitch, -kMaxTilt, kMaxTilt);
        s.roll = std::clamp(s.roll, -kMaxTilt, kMaxTilt);
        s.pos.y = 0;
        const float c0 = min_clearance(t, s);
        s.pos.y = kSheetLift - c0;
        return;
    }
    s.roll = std::clamp(joint_roll, -kMaxTilt, kMaxTilt);
    const Mat ml = mat_of(s.yaw, 0, 0);
    const Vec3 pw{s.pos.x + ml.r0[0] * pivot_x + ml.r0[2] * pivot_z, s.pos.y, s.pos.z + ml.r2[0] * pivot_x + ml.r2[2] * pivot_z};
    auto place_at = [&](float pitch) {
        s.pitch = pitch;
        const Mat m = mat_of(s.yaw, s.pitch, s.roll);
        s.pos = {pw.x - (m.r0[0] * pivot_x + m.r0[2] * pivot_z), pw.y - (m.r1[0] * pivot_x + m.r1[2] * pivot_z), pw.z - (m.r2[0] * pivot_x + m.r2[2] * pivot_z)};
    };
    auto clearance = [&](float pitch) {
        place_at(pitch);
        samples(s, v);
        float far_best = 1e30f, all_best = 1e30f;
        for (const Smp& p : v) {
            if (!t.inside(p.x, p.z)) continue;
            const float cl = p.y - t.height_at(p.x, p.z);
            all_best = std::min(all_best, cl);
            if (std::fabs(p.lz - pivot_z) >= 4.f) far_best = std::min(far_best, cl);
        }
        return (far_best < 1e29f ? far_best : all_best) - kSheetLift;
    };
    float lo = -kMaxTilt, hi = kMaxTilt, result = 0;
    {
        const float flo = clearance(lo), fhi = clearance(hi);
        const bool rising = fhi >= flo;  // clearance grows with the pitch (samples beyond the pivot) or shrinks (behind it)
        if (rising ? flo >= 0 : fhi >= 0) result = rising ? lo : hi;       // clear even at the extreme: take it (tightest allowed)
        else if (rising ? fhi < 0 : flo < 0) result = rising ? hi : lo;    // the ground is too steep: clamped
        else {
            float a = lo, z = hi;  // f(a) and f(z) have opposite signs
            for (int k = 0; k < 40; k++) {
                const float mid = 0.5f * (a + z);
                const bool ok = clearance(mid) >= 0;
                if (ok == rising) z = mid; else a = mid;
            }
            result = 0.5f * (a + z);
        }
    }
    place_at(result);
}

int drop_layer(std::vector<Sheet>& all, const Sheet& s, const std::vector<uint32_t>* only) {
    const Poly ps = poly_of(s, kOverlapMargin * 0.5f);
    int top = INT_MIN, road_low = INT_MAX, plate_top = INT_MIN;
    for (const Sheet& j : all) {
        if (j.uid == s.uid && s.uid != 0) continue;
        if (s.run >= 0 && j.run == s.run) continue;
        if (only && std::find(only->begin(), only->end(), j.uid) == only->end()) continue;
        if (!poly_overlap(poly_of(j, kOverlapMargin * 0.5f), ps)) continue;
        top = std::max(top, j.layer);
        if (j.kind == 1) road_low = std::min(road_low, j.layer);
        else plate_top = std::max(plate_top, j.layer);
    }
    const bool fresh = s.layer == INT_MIN;
    if (top == INT_MIN) return fresh ? 0 : s.layer;
    const int on_top = fresh ? top + 1 : std::max(s.layer, top + 1);
    if (s.kind == 1 || road_low == INT_MAX || plate_top >= road_low) return on_top;
    // A plate that lands on roads: under the lowest of them, over the plates it overlaps.
    if (!fresh && s.layer < road_low && (plate_top == INT_MIN || s.layer > plate_top)) return s.layer;  // already there
    const int slot = plate_top == INT_MIN ? road_low : plate_top + 1;
    if (slot >= road_low)  // no free layer between: every layer from the road's up moves one up (order unchanged)
        for (Sheet& j : all)
            if (!(j.uid == s.uid && s.uid != 0) && j.layer >= slot) j.layer++;
    return slot;
}

SurfaceIndex::SurfaceIndex(const std::vector<Sheet>& all) : all_(&all) {
    for (const Sheet& s : all) {
        const Poly p = poly_of(s, 0.f);
        bx0_.push_back(p.x0); bx1_.push_back(p.x1); bz0_.push_back(p.z0); bz1_.push_back(p.z1);
    }
}

bool SurfaceIndex::covers(float x, float z) const {
    for (size_t i = 0; i < bx0_.size(); i++)
        if (x >= bx0_[i] - 1e-3f && x <= bx1_[i] + 1e-3f && z >= bz0_[i] - 1e-3f && z <= bz1_[i] + 1e-3f) return true;
    return false;
}

float SurfaceIndex::at(const Terrain& t, float x, float z, float below_y, uint32_t* on, float min_y) const {
    float best = t.height_at(x, z);
    uint32_t who = 0;
    for (size_t i = 0; i < bx0_.size(); i++) {
        if (x < bx0_[i] - 1e-3f || x > bx1_[i] + 1e-3f || z < bz0_[i] - 1e-3f || z > bz1_[i] + 1e-3f) continue;
        const Sheet& s = (*all_)[i];
        if (!contains(s, x, z)) continue;
        const float top = face_y(s, s.top, x, z);
        if (top > below_y || top < min_y || top <= best) continue;
        best = top;
        who = s.uid;
    }
    if (on) *on = who;
    return best;
}

float surface_at(const Terrain& t, const std::vector<Sheet>& all, float x, float z, float below_y, uint32_t* on_uid) {
    return SurfaceIndex(all).at(t, x, z, below_y, on_uid);
}

bool parse_layer(std::string_view s, int& layer, bool& fixed) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) j++;
    const size_t d0 = j;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
    if (j == d0) return false;
    layer = std::atoi(std::string(s.substr(i, j - i)).c_str());
    fixed = s.find("fixed", j) != std::string_view::npos;
    return true;
}

std::string format_layer(int layer, bool fixed) { return std::to_string(layer) + (fixed ? " fixed" : ""); }

bool layer_of(const WObj& o, int& layer, bool& fixed) {
    const WProp* p = o.find(kLayerProp);
    return p && parse_layer(o.str(kLayerProp), layer, fixed);
}

void set_layer(WObj& o, int layer, bool fixed) { o.set_str(kLayerProp, format_layer(layer, fixed)); }

// The basin a water remembers: "gx0 gz0 nx nz <hex: step before, per grid point> <hex: step after>".
std::string basin_encode(int gx0, int gz0, int nx, int nz, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, const std::string& extra) {
    static const char* hx = "0123456789abcdef";
    std::string s = std::to_string(gx0) + " " + std::to_string(gz0) + " " + std::to_string(nx) + " " + std::to_string(nz) + " ";
    for (uint8_t b : before) { s += hx[b >> 4]; s += hx[b & 15]; }
    s += ' ';
    for (uint8_t b : after) { s += hx[b >> 4]; s += hx[b & 15]; }
    if (!extra.empty()) { s += ' '; s += extra; }
    return s;
}

bool basin_decode(const std::string& str, int& gx0, int& gz0, int& nx, int& nz, std::vector<uint8_t>& before, std::vector<uint8_t>& after, std::string* extra) {
    int a = 0, b = 0, c = 0, d = 0, n = 0;
    if (std::sscanf(str.c_str(), "%d %d %d %d %n", &a, &b, &c, &d, &n) < 4 || c <= 0 || d <= 0 || c > 4096 || d > 4096) return false;
    const size_t cnt = size_t(c) * size_t(d);
    auto hexv = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
    size_t i = size_t(n);
    auto read = [&](std::vector<uint8_t>& out) {
        if (i + cnt * 2 > str.size()) return false;
        out.resize(cnt);
        for (size_t k = 0; k < cnt; k++) {
            const int hi = hexv(str[i + k * 2]), lo = hexv(str[i + k * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            out[k] = uint8_t(hi * 16 + lo);
        }
        i += cnt * 2;
        return true;
    };
    std::vector<uint8_t> bf, af;
    if (!read(bf)) return false;
    while (i < str.size() && str[i] == ' ') i++;
    if (!read(af)) return false;
    if (extra) {
        while (i < str.size() && str[i] == ' ') i++;
        *extra = str.substr(i);
    }
    gx0 = a; gz0 = b; nx = c; nz = d;
    before = std::move(bf);
    after = std::move(af);
    return true;
}

// Pairs of overlapping level sheets whose top faces are within `tol` of each other (they would flicker).
std::vector<std::pair<int, int>> same_height_pairs(const std::vector<Sheet>& all, float tol) {
    std::vector<std::pair<int, int>> out;
    for (size_t i = 0; i < all.size(); i++) {
        if (!all[i].level()) continue;
        for (size_t j = i + 1; j < all.size(); j++) {
            if (!all[j].level() || (all[i].run >= 0 && all[i].run == all[j].run)) continue;
            if (std::fabs((all[i].pos.y + all[i].top) - (all[j].pos.y + all[j].top)) > tol) continue;
            if (overlap(all[i], all[j])) out.push_back({int(i), int(j)});
        }
    }
    return out;
}

}  // namespace ma2::place
