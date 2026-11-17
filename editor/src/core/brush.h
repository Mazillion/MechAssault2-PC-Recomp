// Brush maths that needs no window (F4): sculpting by the stroke, the smallest brush, the ground slope.
// Everything here is pure so fmt_test can check it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "terrain.h"

namespace ma2 {

// The smallest brush is 1.5 cells of what the tool edits (12 m on 8 m cells: from any cursor position the nearest height point
// gets at least 55% of the weight).
constexpr float kMinBrushCells = 1.5f;
inline float min_brush_radius(float cell) { return std::ceil(kMinBrushCells * cell); }

// Slope tiers (playtest feedback: say WHO cannot get up;: only what the bot code proves). Change these three only.
//  kSteepBot   0.55: above this the bot planner will not route a 'Mech or a tank over the ground. Read-only citations in the bot tree
//              (MechAssault 2\bringup\logs\game tree\source-v35-edgame\src\bots): the 'Mech / crush plan's grid blocks a cell whose
//              neighbour step is over TG_STEP_HEAVY 1.1 m per 2 m cell = 0.55 rise/run (ma2_bot_nav.c:142, tg_steps ma2_bot_nav.c:255-282,
//              built at :412; PATH_WALK_GRADE_HEAVY 0.55 ma2_bot_path.c:284); tanks add slope_max 55 (ma2_bot_ctrl.c:2337, applied in
//              ma2_bot_path.c:152). Battle Armor and pilots plan on the light grid, TG_STEEP 0.80 (ma2_bot_nav.c:137, :411; ma2_bot_ctrl.c:2332
//              sets crush only for 'Mech / VTOL): they route up to 80% (kSteepLight, shown in the tooltips, not a tier). Hover: no limit of its
//              own in the bot code, not measured. The bot grid measures the largest neighbour step per 2 m cell, the editor the triangle
//              gradient per height cell: the same rise over run, within a few percent.
//  kSteepLight 0.80: above this no bot body routes (the orange tier). Nothing here says what the PLAYER can climb: the V4 verify claim of a
//              171% limit is superseded by the reach rerun (the player's Atlas walked up 169% and 825% faces),
//              so no tier claims a player limit. The old "player cannot climb" tier (150%) and the "wall" tier (170%) are gone.
constexpr float kSteepBot = 0.55f, kSteepLight = 0.80f;
// 0 no bot is stopped, 1 bot 'Mechs will not walk up it (jump-jet ones may jet a short face: ma2_bot_path.c:28-30, 256-260), 2 very steep: no bot body routes here.
inline int steep_tier(float s) { return s > kSteepLight ? 2 : s > kSteepBot ? 1 : 0; }
// The short form, for the cursor readout.
inline const char* steep_tag(float s) {
    switch (steep_tier(s)) {
        case 1: return "bots' 'Mechs won't walk up this (lighter bodies: up to 80%)";
        case 2: return "very steep: bots won't route here; some 'Mechs may still climb it";
        default: return "";
    }
}

// The steepness of one height cell (cx, cz = its low corner): the larger gradient (rise / run) of its two triangles, split on
// (gx,gz)-(gx+1,gz+1) as Terrain::height_at does. On the saved ground steps.
inline float cell_slope(const Terrain& t, int cx, int cz) {
    if (t.w < 2 || t.h < 2) return 0.f;
    cx = std::clamp(cx, 0, t.w - 2);
    cz = std::clamp(cz, 0, t.h - 2);
    const float a = t.step_at(cx, cz), b = t.step_at(cx + 1, cz), c = t.step_at(cx, cz + 1), d = t.step_at(cx + 1, cz + 1);
    const float s = t.scale;
    // Triangle (a, b, d): gradient (b - a, d - b) per cell; triangle (a, c, d): (d - c, c - a).
    const float g1 = std::sqrt((b - a) * (b - a) + (d - b) * (d - b)) / s;
    const float g2 = std::sqrt((d - c) * (d - c) + (c - a) * (c - a)) / s;
    return std::max(g1, g2);
}
// The slope of the triangle under (x, z): what the ground is like exactly there (the cursor readout; the overlay uses cell_slope).
inline float slope_at(const Terrain& t, float x, float z) {
    if (t.w < 2 || t.h < 2) return 0.f;
    const float fx = std::clamp(x / t.scale, 0.f, float(t.w - 1)), fz = std::clamp(z / t.scale, 0.f, float(t.h - 1));
    const int x0 = std::min(int(fx), t.w - 2), z0 = std::min(int(fz), t.h - 2);
    const float tx = fx - float(x0), tz = fz - float(z0);
    const float a = t.step_at(x0, z0), b = t.step_at(x0 + 1, z0), c = t.step_at(x0, z0 + 1), d = t.step_at(x0 + 1, z0 + 1);
    const float gx = tx >= tz ? (b - a) : (d - c), gz = tx >= tz ? (d - b) : (c - a);
    return std::sqrt(gx * gx + gz * gz) / t.scale;
}

// One stroke = one pass: the biggest brush weight each grid point has felt during the stroke. Holding still or going back and forth
// adds nothing, because the height is worked out from the ground at the stroke's start and this maximum.
struct StrokeCap {
    std::vector<float> w;
    void reset(size_t n) { w.assign(n, 0.f); }
    float touch(size_t i, float wt) {
        if (i >= w.size()) return wt;
        if (wt > w[i]) w[i] = wt;
        return w[i];
    }
};

// How many equal steps the brush takes from one cursor point to the next so that drag speed cannot leave gaps: one every
// max(0.5, R / 8) metres, at least one.
inline int substeps(float ax, float az, float bx, float bz, float R) {
    const float d = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    const float step = std::max(0.5f, R / 8.f);
    return std::clamp(int(std::ceil(d / step - 1e-4f)), 1, 4000);
}

// The height of a Raise (dir +1) / Lower (-1) stroke at a point: the ground at the stroke's start moved by amount x the biggest
// weight the point has felt. (Roughen puts its noise in place of dir.)
inline float stroke_height(float h0, float dir, float amount, float wmax) { return h0 + dir * amount * wmax; }

// "Keep raising while held": the stroke's cap grows by one amount per second held; still free of frame rate and drag speed.
inline float cap_amount(float amount, bool build_up, float held_s) { return build_up ? amount * (1.f + std::max(0.f, held_s)) : amount; }

// The sculpt weight at an offset from the brush centre: the Edge slider (hardness 0 = the smoothstep every sculpt brush had).
inline float sculpt_falloff(float dx, float dz, float R, float hardness, bool square) {
    const float d = square ? std::max(std::abs(dx), std::abs(dz)) : std::sqrt(dx * dx + dz * dz);
    if (d >= R) return 0.f;
    float f = (1.f - d / R) / std::max(0.02f, 1.f - hardness);
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    return f * f * (3 - 2 * f);
}

// Radius of the inner ring: where a sculpt brush is half strength (or, for Plateau, where its flat top ends: weight 0.35).
inline float half_strength_radius(float R, float hardness) { return R * (1.f - 0.5f * (1.f - hardness)); }
inline float plateau_flat_radius(float R, float hardness) { return R * (1.f - 0.4f * (1.f - hardness)); }

// Painting by the stroke: a painted value is the one the texel had when the stroke began, moved toward the target
// by strength x (the biggest brush weight the texel has felt). The same wmax gives the same value, so holding still adds nothing. Rounds to
// the nearest step, a tie going toward the target, so strength 1 at full weight is exactly the target.
constexpr float kPaintFull = 0.99f;
inline uint8_t paint_value(uint8_t base, float target, float strength, float wmax) {
    float f = strength * wmax;
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    if (f >= kPaintFull) f = 1.f;   // within a rounding step of full: a click on the brush centre reaches the target
    const float v = float(base) + (target - float(base)) * f;
    const float r = target >= float(base) ? std::floor(v + 0.5f) : std::ceil(v - 0.5f);
    return uint8_t(r < 0.f ? 0.f : r > 255.f ? 255.f : r);
}

// The Ramp tool: the ground a ramp from A (height ha) to B (hb) wants at (x, z). Along the axis the target is
// ha + (hb - ha) * t (t = 0 at A, 1 at B); it is level across, exactly that within width/2 of the axis and between the two ends, and
// past that (the sides and beyond each end) it blends to `old` with a smoothstep over `blend` metres. Returns false (out = old) where
// the ramp does not reach.
inline bool ramp_target(float ax, float az, float bx, float bz, float ha, float hb, float width, float blend, float x, float z, float old, float& out) {
    out = old;
    const float dx = bx - ax, dz = bz - az, L = std::sqrt(dx * dx + dz * dz);
    if (L < 1e-3f) return false;
    const float ux = dx / L, uz = dz / L;
    const float px = x - ax, pz = z - az;
    const float u = px * ux + pz * uz;          // along the axis (m)
    const float v = std::fabs(-px * uz + pz * ux);  // across (m)
    const float over_u = u < 0 ? -u : u > L ? u - L : 0.f;
    const float over_v = std::max(0.f, v - 0.5f * width);
    const float d = std::sqrt(over_u * over_u + over_v * over_v);  // distance outside the strip
    if (d >= blend) return false;
    const float t = std::clamp(u / L, 0.f, 1.f);
    const float target = ha + (hb - ha) * t;
    float s = 1.f;
    if (d > 0.f) {
        const float f = 1.f - d / std::max(1e-3f, blend);
        s = f * f * (3.f - 2.f * f);
    }
    out = old + (target - old) * s;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The scatter brush: count by area covered, spacing by model size.

constexpr float kScatterCell = 2.f;   // the coverage grid: one cell is 2 x 2 m

// Footprint radius of a model box: half its longer side across the ground, at least 0.5 m (a pebble still takes room).
inline float footprint_radius(const float bmin[3], const float bmax[3]) {
    const float w = bmax[0] - bmin[0], d = bmax[2] - bmin[2];
    return std::max(0.5f, 0.5f * std::max(w, d));
}

// The objects a stroke has to keep apart: roots near the stroke and the ones it places. An object at (x, z) of radius r is refused when
// some other one lies closer than k * (r + its radius). A hash of 16 m cells keeps each question to the neighbours.
class SpacingGrid {
public:
    void add(float x, float z, float r) {
        const int i = int(pts_.size());
        pts_.push_back({x, z, r});
        cells_[key(cell(x), cell(z))].push_back(i);
        if (r > max_r_) max_r_ = r;
    }
    bool clear(float x, float z, float r, float k) const {
        if (pts_.empty()) return true;
        const float reach = k * (r + max_r_);
        const int x0 = cell(x - reach), x1 = cell(x + reach), z0 = cell(z - reach), z1 = cell(z + reach);
        for (int cz = z0; cz <= z1; cz++)
            for (int cx = x0; cx <= x1; cx++) {
                auto it = cells_.find(key(cx, cz));
                if (it == cells_.end()) continue;
                for (int i : it->second) {
                    const P& q = pts_[size_t(i)];
                    const float dx = q.x - x, dz = q.z - z, need = k * (r + q.r);
                    if (dx * dx + dz * dz < need * need) return false;
                }
            }
        return true;
    }
    size_t size() const { return pts_.size(); }
private:
    struct P { float x, z, r; };
    static int cell(float v) { return int(std::floor(v / 16.f)); }
    static int64_t key(int cx, int cz) { return (int64_t(cx) << 32) ^ int64_t(uint32_t(cz)); }
    std::vector<P> pts_;
    std::unordered_map<int64_t, std::vector<int>> cells_;
    float max_r_ = 0.f;
};

// The ground a stroke has covered, in kScatterCell cells: a cell is covered when its centre is inside a ring. `newly` marks the cells of
// a ring and returns how many were not covered before (their centres go to `centres` when given), so holding still or going back over
// ground adds nothing.
struct Coverage {
    std::unordered_set<int64_t> done;
    void reset() { done.clear(); }
    int newly(float x, float z, float R, std::vector<std::pair<float, float>>* centres = nullptr) {
        const int x0 = int(std::floor((x - R) / kScatterCell)), x1 = int(std::floor((x + R) / kScatterCell));
        const int z0 = int(std::floor((z - R) / kScatterCell)), z1 = int(std::floor((z + R) / kScatterCell));
        int n = 0;
        for (int cz = z0; cz <= z1; cz++)
            for (int cx = x0; cx <= x1; cx++) {
                const float mx = (float(cx) + 0.5f) * kScatterCell, mz = (float(cz) + 0.5f) * kScatterCell;
                if ((mx - x) * (mx - x) + (mz - z) * (mz - z) > R * R) continue;
                if (!done.insert((int64_t(cx) << 32) ^ int64_t(uint32_t(cz))).second) continue;
                n++;
                if (centres) centres->push_back({mx, mz});
            }
        return n;
    }
};

// How many objects a newly covered cell earns: `density` per hectare over the cell's area.
inline float scatter_per_cell(float density_per_ha) { return density_per_ha / 10000.f * kScatterCell * kScatterCell; }

// A random lean: up vector `lean_deg` or less from `base_up` (a unit vector; (0, 1, 0) is straight up), at azimuth `az` (radians) and
// `frac` (0..1) of the way to the limit. Unit length.
inline void lean_up(const float base_up[3], float lean_deg, float az, float frac, float out[3]) {
    const float th = lean_deg * 0.0174532925f * frac;
    // Two unit vectors perpendicular to base_up.
    float ax[3] = {1.f, 0.f, 0.f};
    if (std::fabs(base_up[0]) > 0.9f) { ax[0] = 0.f; ax[2] = 1.f; }
    float u[3] = {base_up[1] * ax[2] - base_up[2] * ax[1], base_up[2] * ax[0] - base_up[0] * ax[2], base_up[0] * ax[1] - base_up[1] * ax[0]};
    float lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& c : u) c /= lu;
    const float v[3] = {base_up[1] * u[2] - base_up[2] * u[1], base_up[2] * u[0] - base_up[0] * u[2], base_up[0] * u[1] - base_up[1] * u[0]};
    const float s = std::sin(th), c = std::cos(th), ca = std::cos(az), sa = std::sin(az);
    for (int i = 0; i < 3; i++) out[i] = base_up[i] * c + (u[i] * ca + v[i] * sa) * s;
}

}  // namespace ma2
