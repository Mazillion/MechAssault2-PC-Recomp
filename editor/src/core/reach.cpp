// "Can it get here?" flood: see reach.h and REACH-SPEC.md section 3. Rule numbers come from reach_rules.h; a comment names the planner
// source (PATH = ma2_bot_path.c, NAV = ma2_bot_nav.c, ...) where a rule is copied from the bots.
#include "reach.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include "terrain.h"

namespace ma2 {

// ---- ReachInput ----------------------------------------------------------------------------------------------------------------

void ReachInput::init(int cells_x, int cells_z, float cell_m) {
    w = cells_x;
    h = cells_z;
    cell = cell_m;
    const size_t n = count();
    th.assign(n, 0.f);
    top.assign(n, -1e9f);
    flags.assign(n, 0);
    water_y.assign(n, std::numeric_limits<float>::quiet_NaN());
    low.clear();
}

bool ReachInput::valid() const {
    const size_t n = count();
    return w > 0 && h > 0 && cell > 0.f && th.size() == n && (top.empty() || top.size() == n) && (flags.empty() || flags.size() == n) &&
           (water_y.empty() || water_y.size() == n);
}

void reach_input_from_terrain(ReachInput& in, const Terrain& t) {
    const float sx = t.size_x(), sz = t.size_z();
    const float cell = std::max(2.f, std::max(sx, sz) / 1024.f);
    const int w = std::max(1, int(std::ceil(sx / cell - 1e-4f))), h = std::max(1, int(std::ceil(sz / cell - 1e-4f)));
    in.init(w, h, cell);
    for (int cz = 0; cz < h; cz++)
        for (int cx = 0; cx < w; cx++) in.th[size_t(cz) * w + cx] = t.height_at((float(cx) + 0.5f) * cell, (float(cz) + 0.5f) * cell);
}

// Raster rule: up-facing triangles at cell centres -> top; steep faces -> their xz cells get the highest vertex and F_LATCH.
void reach_raster_tris(ReachInput& in, const float* tris, size_t tri_count, bool latch) {
    if (!in.valid()) return;
    const size_t n = in.count();
    if (in.top.size() != n) in.top.assign(n, -1e9f);
    if (in.flags.size() != n) in.flags.assign(n, 0);
    if (in.low.size() != n) in.low.assign(n, 1e9f);
    const float cell = in.cell;
    for (size_t t = 0; t < tri_count; t++) {
        const float* v = tris + t * 9;
        const float ux = v[3] - v[0], uy = v[4] - v[1], uz = v[5] - v[2], wx = v[6] - v[0], wy = v[7] - v[1], wz = v[8] - v[2];
        const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9f) continue;
        const float minx = std::min({v[0], v[3], v[6]}), maxx = std::max({v[0], v[3], v[6]});
        const float minz = std::min({v[2], v[5], v[8]}), maxz = std::max({v[2], v[5], v[8]});
        const float ymax = std::max({v[1], v[4], v[7]});
        if (std::fabs(ny) / len >= 0.5f) {
            const int x0 = std::max(0, int(std::floor(minx / cell))), x1 = std::min(in.w - 1, int(std::floor(maxx / cell)));
            const int z0 = std::max(0, int(std::floor(minz / cell))), z1 = std::min(in.h - 1, int(std::floor(maxz / cell)));
            for (int cz = z0; cz <= z1; cz++)
                for (int cx = x0; cx <= x1; cx++) {
                    const float px = (float(cx) + 0.5f) * cell - v[0], pz = (float(cz) + 0.5f) * cell - v[2];
                    // p = aa * (v1 - v0) + bb * (v2 - v0) in xz; det = (x1-x0)(z2-z0) - (x2-x0)(z1-z0) = ny, never 0 for a floor-like triangle.
                    const float det = (v[3] - v[0]) * (v[8] - v[2]) - (v[6] - v[0]) * (v[5] - v[2]);
                    const float aa = (px * (v[8] - v[2]) - (v[6] - v[0]) * pz) / det;
                    const float bb = ((v[3] - v[0]) * pz - px * (v[5] - v[2])) / det;
                    if (aa < -1e-4f || bb < -1e-4f || aa + bb > 1.0001f) continue;
                    const float y = v[1] + aa * (v[4] - v[1]) + bb * (v[7] - v[1]);
                    const size_t i = size_t(cz) * in.w + cx;
                    in.top[i] = std::max(in.top[i], y);
                    in.low[i] = std::min(in.low[i], y);
                }
        } else {
            // A steep face: sample its xz footprint every half cell (edges and interior) and mark the cells with its highest vertex.
            const float ext = std::max(maxx - minx, maxz - minz);
            const int steps = std::clamp(int(std::ceil(ext / (0.5f * cell))), 1, 256);
            for (int i = 0; i <= steps; i++)
                for (int j = 0; j <= steps - i; j++) {
                    const float a = float(i) / float(steps), b = float(j) / float(steps);
                    const float px = v[0] + a * (v[3] - v[0]) + b * (v[6] - v[0]), pz = v[2] + a * (v[5] - v[2]) + b * (v[8] - v[2]);
                    const int cx = int(std::floor(px / cell)), cz = int(std::floor(pz / cell));
                    if (cx < 0 || cz < 0 || cx >= in.w || cz >= in.h) continue;
                    const size_t k = size_t(cz) * in.w + cx;
                    in.top[k] = std::max(in.top[k], ymax);
                    if (latch) in.flags[k] |= F_LATCH;
                }
        }
    }
}

void reach_raster_finish(ReachInput& in) {
    if (!in.valid() || in.low.size() != in.count() || in.top.size() != in.count() || in.flags.size() != in.count()) return;
    const float deck_h = reach_rule(RL_UNDER_DECK).player;
    for (size_t i = 0; i < in.count(); i++) {
        if (in.low[i] > 1e8f || (in.flags[i] & F_LATCH)) continue;
        // Open air under a horizontal surface at least 8 m over the ground, with the lowest surface at least 4 m up: a thin deck.
        if (in.top[i] - in.th[i] >= deck_h && in.low[i] - in.th[i] >= 4.f) in.flags[i] |= F_DECK;
    }
}

const char* reach_compass(int d) {
    static const char* const k[8] = {"east", "north-east", "north", "north-west", "west", "south-west", "south", "south-east"};
    return k[d & 7];
}

namespace {

constexpr int kDx8[8] = {1, 1, 0, -1, -1, -1, 0, 1};  // E NE N NW W SW S SE
constexpr int kDz8[8] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr size_t kMaxLinks = 200000;

struct RayStep {
    int dx, dz;
    float run;  // metres from the start cell centre to this cell centre
};

// The cells a straight line from a cell centre crosses, in order, for 16 directions (k * 22.5 degrees; even k are the 8 compass ones).
void build_rays(std::vector<RayStep> rays[16], float cell, float max_run) {
    const float pi = 3.14159265358979f;
    for (int k = 0; k < 16; k++) {
        const float ux = std::cos(float(k) * pi / 8.f), uz = std::sin(float(k) * pi / 8.f);
        int lx = 0, lz = 0;
        for (float t = 0.5f * cell; t <= max_run + cell; t += 0.5f * cell) {
            const int dx = int(std::floor(0.5f + ux * t / cell)), dz = int(std::floor(0.5f + uz * t / cell));
            if ((dx == lx && dz == lz) || (dx == 0 && dz == 0)) continue;
            lx = dx;
            lz = dz;
            rays[k].push_back({dx, dz, std::sqrt(float(dx * dx + dz * dz)) * cell});
        }
    }
}

// ---- the VTOL flood (4 m flight cells; ceiling = terrain + MaxVTOLHeight - 6.6) -------------------------------------------------
struct VtolField {
    int k = 1, fw = 0, fh = 0;
    std::vector<uint8_t> reached;
};

float obj_surface(const ReachInput& in, size_t i, float obj_min) {
    const float t = in.th[i];
    const float tp = in.top.empty() ? -1e9f : in.top[i];
    return tp > t + obj_min ? tp : t;
}
uint8_t flag_at(const ReachInput& in, size_t i) { return in.flags.empty() ? uint8_t(0) : in.flags[i]; }

void vtol_flood(const ReachInput& in, const ReachProfile& vp, float rider_extra, const std::vector<std::pair<float, float>>& seeds, VtolField& F,
                const std::atomic<bool>* cancel, std::vector<uint8_t>* seed_ok = nullptr) {
    const int w = in.w, h = in.h;
    const size_t N = in.count();
    const float cell = in.cell, obj_min = reach_rule(RL_OBJ_MIN).player;
    const float clear = vp.vtol_clear + rider_extra;
    const int k = cell >= 4.f ? 1 : int(std::ceil(4.f / cell - 1e-4f));
    F.k = k;
    F.fw = (w + k - 1) / k;
    F.fh = (h + k - 1) / k;
    const size_t FN = size_t(F.fw) * F.fh;
    F.reached.assign(FN, 0);
    // The obstacle height inside (clear + hull) of a flight cell: a box max over the surface (rows, then columns at the centres).
    const int R = std::max(1, int(std::ceil((clear + vp.vtol_hull) / cell - 1e-4f)));
    std::vector<float> rm(N);
    for (int cz = 0; cz < h; cz++)
        for (int cx = 0; cx < w; cx++) {
            float m = -1e9f;
            const int x0 = std::max(0, cx - R), x1 = std::min(w - 1, cx + R);
            for (int xx = x0; xx <= x1; xx++) m = std::max(m, obj_surface(in, size_t(cz) * w + xx, obj_min));
            rm[size_t(cz) * w + cx] = m;
        }
    std::vector<float> fmax(FN), fceil(FN);
    std::vector<uint8_t> local(FN);
    for (int fz = 0; fz < F.fh; fz++)
        for (int fx = 0; fx < F.fw; fx++) {
            const int cx = std::min(w - 1, fx * k + k / 2), cz = std::min(h - 1, fz * k + k / 2);
            float m = -1e9f;
            for (int zz = std::max(0, cz - R); zz <= std::min(h - 1, cz + R); zz++) m = std::max(m, rm[size_t(zz) * w + cx]);
            const size_t f = size_t(fz) * F.fw + fx, c = size_t(cz) * w + cx;
            fmax[f] = m;
            fceil[f] = in.th[c] + in.vtol_cap - vp.ceil_offset;
            local[f] = !(flag_at(in, c) & F_OOB) && m + clear <= fceil[f];
        }
    std::vector<int> q;
    const int snap_r = int(std::ceil(reach_rule(RL_SPAWN_SNAP).player / (float(k) * cell)));
    if (seed_ok) seed_ok->assign(seeds.size(), 0);
    for (size_t si = 0; si < seeds.size(); si++) {
        const auto& s = seeds[si];
        const int sx = std::clamp(int(s.first / (float(k) * cell)), 0, F.fw - 1), sz = std::clamp(int(s.second / (float(k) * cell)), 0, F.fh - 1);
        int best = -1;
        float bd = 1e18f;
        for (int dz = -snap_r; dz <= snap_r; dz++)
            for (int dx = -snap_r; dx <= snap_r; dx++) {
                const int fx = sx + dx, fz = sz + dz;
                if (fx < 0 || fz < 0 || fx >= F.fw || fz >= F.fh || !local[size_t(fz) * F.fw + fx]) continue;
                const float d = float(dx * dx + dz * dz);
                if (d < bd) { bd = d; best = fz * F.fw + fx; }
            }
        if (best >= 0 && seed_ok) (*seed_ok)[si] = 1;  // a craft starting here has clear air to fly in
        if (best >= 0 && !F.reached[best]) { F.reached[best] = 1; q.push_back(best); }
    }
    for (size_t head = 0; head < q.size(); head++) {
        if ((head & 4095) == 0 && cancel && cancel->load(std::memory_order_relaxed)) return;
        const int f = q[head], fx = f % F.fw, fz = f / F.fw;
        for (int d = 0; d < 8; d++) {
            const int nx = fx + kDx8[d], nz = fz + kDz8[d];
            if (nx < 0 || nz < 0 || nx >= F.fw || nz >= F.fh) continue;
            const int g = nz * F.fw + nx;
            // The game drags the craft down to the new ceiling, so the obstacle must clear the lower of the two ceilings.
            if (F.reached[g] || !local[g] || fmax[g] + clear > fceil[f]) continue;
            F.reached[g] = 1;
            q.push_back(g);
        }
    }
}

// ---- the ground solver ------------------------------------------------------------------------------------------------------------
class Solver {
public:
    Solver(const ReachInput& in, const ReachProfile& pr, int team, const std::atomic<bool>* cancel, ReachResult& out)
        : in_(in), pr_(pr), team_(team), cancel_(cancel), out_(out), w_(in.w), h_(in.h), N_(int(in.count())), cell_(in.cell) {}
    void run();

private:
    const ReachInput& in_;
    const ReachProfile& pr_;
    int team_;
    const std::atomic<bool>* cancel_;
    ReachResult& out_;
    int w_, h_, N_;
    float cell_;
    float stepc_ = 0, obj_min_ = 0.4f, natural_ = 0.5f;
    int need_ = 0, need2_ = 0;
    int off8_[8] = {};
    std::vector<float> S_;
    std::vector<uint8_t> blk_, dist_, free_, nat_, wmask_, done_, rv_, nearL_;
    std::vector<RayStep> rays_[16];
    std::vector<int> reached_, queue_;
    std::vector<int> comp_size_;  // cells per walk component (PocketDrop: a small region)
    int stage_max_ = 1;
    int latch_k_ = 3;  // a cell is "near a wall" within this many cells of an F_LATCH cell: 2 m plus the clearance band the grid keeps free

    // The largest surface change allowed across the edge a -> b (len 1 orthogonal, sqrt 2 diagonal). A terrain face (both cells bare ground:
    // surface - terrain < natural) uses the terrain slope cap when the profile has one (the player); every other edge keeps the step rule.
    float edge_limit(int a, int b, float len) const {
        if (pr_.terrain_slope > 0.f && nat_[size_t(a)] && nat_[size_t(b)]) return std::max(stepc_, pr_.terrain_slope * cell_) * len;
        return stepc_ * len;
    }
    bool is_water(int i) const { return flag_at(in_, size_t(i)) & F_WATER; }
    bool oob_flag(int i) const { return flag_at(in_, size_t(i)) & F_OOB; }
    bool cancelled() const { return cancel_ && cancel_->load(std::memory_order_relaxed); }
    int need_for(float radius) const { return int(std::ceil((radius + cell_ * 0.5f) * 5.f / cell_ - 1e-3f)); }
    bool is_deck(int i) const {
        return (flag_at(in_, size_t(i)) & F_DECK) && !in_.top.empty() && in_.top[i] - in_.th[i] >= reach_rule(RL_UNDER_DECK).player;
    }

    void build_surface();
    void build_walls();
    void build_masks();
    int snap_free(float x, float z, float max_d) const;
    void seeds_and_flood();
    void flood(int stage);
    void gen_links(int a, int stage);
    void take(const ReachLink& L);
    bool drop_scan(int a, int d8, ReachLink& L) const;
    bool jet_scan(int a, int k16, ReachLink& L) const;
    bool under_scan(int a, int d8, ReachLink& L) const;
    void latch_expand(int a);
    void ride_stage();
    void components();
    void eval_face(ReachFace& f) const;
    float drop_limit(int a) const;
    void build_faces();
    void finish_classes();
    void run_vtol();
};

void Solver::build_surface() {
    S_.resize(size_t(N_));
    for (int i = 0; i < N_; i++) {
        const float t = in_.th[i], tp = in_.top.empty() ? -1e9f : in_.top[i];
        const bool brk = flag_at(in_, size_t(i)) & F_BREAKABLE;
        // Surface = a collider top more than obj_min over the ground; heavy bodies crush breakable scenery (NAV:143,392-399).
        S_[i] = (tp > t + obj_min_ && !(pr_.heavy && brk)) ? tp : t;
    }
    nat_.assign(size_t(N_), 0);
    for (int i = 0; i < N_; i++) nat_[i] = S_[i] - in_.th[i] < natural_;
}

void Solver::build_walls() {
    blk_.assign(size_t(N_), 0);
    for (int cz = 0; cz < h_; cz++)
        for (int cx = 0; cx < w_; cx++) {
            const int i = cz * w_ + cx;
            bool b = oob_flag(i);
            if (!b) {
                const float s = S_[i];
                b = (cx > 0 && std::fabs(S_[i - 1] - s) > edge_limit(i, i - 1, 1.f)) || (cx < w_ - 1 && std::fabs(S_[i + 1] - s) > edge_limit(i, i + 1, 1.f)) ||
                    (cz > 0 && std::fabs(S_[i - w_] - s) > edge_limit(i, i - w_, 1.f)) || (cz < h_ - 1 && std::fabs(S_[i + w_] - s) > edge_limit(i, i + w_, 1.f));
            }
            blk_[i] = b;
        }
    // Chamfer distance (5 per orthogonal cell, 7 per diagonal) from blocked cells; the map edge counts as blocked (NAV:199-229).
    dist_.assign(size_t(N_), 100);
    for (int cz = 0; cz < h_; cz++)
        for (int cx = 0; cx < w_; cx++) {
            const int i = cz * w_ + cx;
            if (blk_[i]) dist_[i] = 0;
            else if (cx == 0 || cz == 0 || cx == w_ - 1 || cz == h_ - 1) dist_[i] = 5;
        }
    for (int cz = 0; cz < h_; cz++)
        for (int cx = 0; cx < w_; cx++) {
            const int i = cz * w_ + cx;
            int v = dist_[i];
            if (!v) continue;
            if (cx > 0) v = std::min(v, dist_[i - 1] + 5);
            if (cz > 0) {
                v = std::min(v, dist_[i - w_] + 5);
                if (cx > 0) v = std::min(v, dist_[i - w_ - 1] + 7);
                if (cx < w_ - 1) v = std::min(v, dist_[i - w_ + 1] + 7);
            }
            dist_[i] = uint8_t(v);
        }
    for (int cz = h_ - 1; cz >= 0; cz--)
        for (int cx = w_ - 1; cx >= 0; cx--) {
            const int i = cz * w_ + cx;
            int v = dist_[i];
            if (!v) continue;
            if (cx < w_ - 1) v = std::min(v, dist_[i + 1] + 5);
            if (cz < h_ - 1) {
                v = std::min(v, dist_[i + w_] + 5);
                if (cx < w_ - 1) v = std::min(v, dist_[i + w_ + 1] + 7);
                if (cx > 0) v = std::min(v, dist_[i + w_ - 1] + 7);
            }
            dist_[i] = uint8_t(v);
        }
    free_.assign(size_t(N_), 0);
    for (int i = 0; i < N_; i++) {
        free_[i] = !blk_[i] && dist_[i] >= need_;
        nat_[i] = S_[i] - in_.th[i] < natural_;
    }
}

// wmask_ bit d = a walk edge from a free cell to its free neighbour d (8-neighbour, no corner cutting, step and slope limits).
void Solver::build_masks() {
    wmask_.assign(size_t(N_), 0);
    const float diag = 1.41421356f;
    for (int cz = 0; cz < h_; cz++)
        for (int cx = 0; cx < w_; cx++) {
            const int a = cz * w_ + cx;
            if (!free_[a]) continue;
            uint8_t m = 0;
            for (int d = 0; d < 8; d++) {
                const int nx = cx + kDx8[d], nz = cz + kDz8[d];
                if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
                const int b = nz * w_ + nx;
                if (!free_[b]) continue;
                const bool dg = (kDx8[d] != 0 && kDz8[d] != 0);
                if (dg && (!free_[cz * w_ + nx] || !free_[nz * w_ + cx])) continue;  // no corner cutting (PATH:684-685)
                const float dd = std::fabs(S_[b] - S_[a]), len = dg ? diag : 1.f;
                if (dd > edge_limit(a, b, len)) continue;
                if (pr_.slope_cap > 0.f && dd > pr_.slope_cap * cell_ * len) continue;
                m |= uint8_t(1u << d);
            }
            wmask_[a] = m;
        }
}

int Solver::snap_free(float x, float z, float max_d) const {
    const int cx = int(std::floor(x / cell_)), cz = int(std::floor(z / cell_));
    const int R = int(std::ceil(max_d / cell_));
    int best = -1;
    float bd = max_d * max_d + 1e-3f;
    for (int dz = -R; dz <= R; dz++)
        for (int dx = -R; dx <= R; dx++) {
            const int nx = cx + dx, nz = cz + dz;
            if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_ || !free_[nz * w_ + nx]) continue;
            const float ex = (float(nx) + 0.5f) * cell_ - x, ez = (float(nz) + 0.5f) * cell_ - z;
            const float d = ex * ex + ez * ez;
            if (d < bd) { bd = d; best = nz * w_ + nx; }
        }
    return best;
}

void Solver::take(const ReachLink& L) {
    if (out_.cls[L.to]) return;
    out_.cls[L.to] = std::max(out_.cls[L.from], L.cls);
    reached_.push_back(L.to);
    queue_.push_back(L.to);
    if (out_.links.size() < kMaxLinks) out_.links.push_back(L);
}

// One-way step-off over a blocked band onto ground 1..drop lower (PATH:705-755, row 13/14 of PLANNER-RULES).
bool Solver::drop_scan(int a, int d8, ReachLink& L) const {
    const float fall_min = reach_rule(RL_DIVE_FALL_MIN).player;
    const float dlim = drop_limit(a);
    if (dlim < fall_min || !nat_[a]) return false;
    const int cx = a % w_, cz = a / w_;
    bool first = true;
    for (const RayStep& s : rays_[2 * d8]) {
        if (s.run > pr_.drop_run + 1e-3f) break;
        const int nx = cx + s.dx, nz = cz + s.dz;
        if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) break;
        const int n = nz * w_ + nx;
        if (first) {
            if (free_[n]) return false;  // the edge of the free ground is where a step-off starts
            first = false;
        }
        if (S_[n] > S_[a] + pr_.band_tol || !nat_[n]) break;  // the band never rises above the start, and it is bare ground
        if (free_[n] && dist_[n] >= need2_ && !(pr_.refuse_water && is_water(n))) {
            const float fall = S_[a] - S_[n];
            if (fall >= fall_min && fall <= dlim) {
                L = {a, n, L_DROP, R_WALK, -fall, s.run};
                return true;
            }
        }
    }
    return false;
}

// Jet legs (PATH:251-274 up-link, 28 hop, 858-887 dive): the first usable landing along a ray over a blocked band.
bool Solver::jet_scan(int a, int k16, ReachLink& L) const {
    if (!pr_.jets) return false;
    const float fall_min = reach_rule(RL_DIVE_FALL_MIN).player;
    // Over a bare-terrain band (every planner up-link, PATH:797-821) the Bots' reach is the planner's (climb_terr, drop_terr: the physical cap there
    // is the slope rule); a band with an object in it keeps the measured limits (climb, drop).
    const float s0 = S_[a], limit_top = s0 + std::max(pr_.climb, pr_.climb_terr) + pr_.band_tol;
    const float max_run = std::max(std::max(pr_.up_run, pr_.dive_run), pr_.hop_run);
    const int cx = a % w_, cz = a / w_;
    float band = -1e9f;
    bool band_nat = true;
    bool first = true;
    for (const RayStep& s : rays_[k16]) {
        if (s.run > max_run + 1e-3f) break;
        const int nx = cx + s.dx, nz = cz + s.dz;
        if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) break;
        const int n = nz * w_ + nx;
        if (first) {
            if (free_[n]) return false;
            first = false;
        }
        const float sn = S_[n];
        if (free_[n] && dist_[n] >= need2_ && nat_[n] && !(pr_.refuse_water && is_water(n))) {  // landing: bare, clearance r + 2
            const float rise = sn - s0;
            const float lim_up = band_nat ? pr_.climb_terr : pr_.climb;
            if (rise >= pr_.up_min) {
                if (nat_[a] && s.run <= pr_.up_run && rise <= lim_up && band <= s0 + lim_up + pr_.band_tol && band <= sn + pr_.band_tol) {
                    L = {a, n, L_UP, R_JET, rise, s.run};
                    return true;
                }
            } else if (rise > -fall_min) {
                if (s.run <= pr_.hop_run && band <= s0 + pr_.hop_climb) {
                    L = {a, n, L_HOP, R_JET, rise, s.run};
                    return true;
                }
            } else if (-rise <= (band_nat ? pr_.drop_terr : pr_.drop)) {
                if (nat_[a] && s.run <= pr_.dive_run && band <= s0 + pr_.band_tol) {
                    L = {a, n, L_DIVE, R_JET, rise, s.run};
                    return true;
                }
                if (s.run <= pr_.hop_run && band <= s0 + pr_.hop_climb) {
                    L = {a, n, L_HOP, R_JET, rise, s.run};
                    return true;
                }
            }
        }
        band = std::max(band, sn);
        band_nat = band_nat && nat_[n];
        if (band > limit_top) break;
    }
    return false;
}

// Walk under a deck (PATH:254-255,758-785): a cardinal run beneath a deck >= 8 m over bare ground, landing within 1.5 m of the start height.
bool Solver::under_scan(int a, int d8, ReachLink& L) const {
    if (pr_.under_run <= 0.f || !nat_[a]) return false;
    const float land_tol = reach_rule(RL_UNDER_LAND).player;
    const int cx = a % w_, cz = a / w_;
    bool first = true, saw_deck = false;
    for (const RayStep& s : rays_[2 * d8]) {
        if (s.run > pr_.under_run + 1e-3f) break;
        const int nx = cx + s.dx, nz = cz + s.dz;
        if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) break;
        const int n = nz * w_ + nx;
        if (first) {
            if (free_[n]) return false;
            first = false;
        }
        if (is_deck(n)) { saw_deck = true; continue; }
        if (free_[n]) {
            if (saw_deck && nat_[n] && std::fabs(in_.th[n] - in_.th[a]) <= land_tol && !(pr_.refuse_water && is_water(n))) {
                L = {a, n, L_UNDER, R_WALK, in_.th[n] - in_.th[a], s.run};
                return true;
            }
            return false;
        }
        if (!nat_[n]) return false;  // a wall or an object that is not a deck: no way through
    }
    return false;
}

// The BA claw (wiki t12): from a reached cell near an object wall face, every free cell of that object (its roof) is reached.
void Solver::latch_expand(int a) {
    if (nearL_.empty() || !nearL_[a]) return;
    const int cx = a % w_, cz = a / w_;
    const int K = latch_k_;
    for (int dz = -K; dz <= K; dz++)
        for (int dx = -K; dx <= K; dx++) {
            const int nx = cx + dx, nz = cz + dz;
            if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
            const int L0 = nz * w_ + nx;
            if (!(flag_at(in_, size_t(L0)) & F_LATCH) || rv_[L0]) continue;
            std::vector<int> st{L0};
            rv_[L0] = 1;
            while (!st.empty()) {
                const int c = st.back();
                st.pop_back();
                if (free_[c]) take({a, c, L_LATCH, R_LATCH, S_[c] - S_[a], 0.f});
                const int x = c % w_, z = c / w_;
                for (int d = 0; d < 8; d++) {
                    const int mx = x + kDx8[d], mz = z + kDz8[d];
                    if (mx < 0 || mz < 0 || mx >= w_ || mz >= h_) continue;
                    const int m = mz * w_ + mx;
                    if (rv_[m]) continue;
                    const bool built = (flag_at(in_, size_t(m)) & F_LATCH) || (!in_.top.empty() && in_.top[m] > in_.th[m] + obj_min_);
                    if (!built) continue;
                    rv_[m] = 1;
                    st.push_back(m);
                }
            }
        }
}

void Solver::gen_links(int a, int stage) {
    if (wmask_[a] == 0xFF) return;  // an interior cell: nothing to bridge
    ReachLink L;
    if (stage == 1) {
        for (int d = 0; d < 8; d++) {
            if (!(wmask_[a] & (1u << d)) && drop_scan(a, d, L)) take(L);
            if ((d & 1) == 0 && under_scan(a, d, L)) take(L);
        }
    } else if (stage == 2) {
        if (pr_.jets)
            for (int k = 0; k < 16; k++)
                if (jet_scan(a, k, L)) take(L);
    } else if (stage == 3) {
        if (pr_.latch) latch_expand(a);
    }
}

// Breadth-first flood over every reached cell: walk edges, then the links of the stages up to `stage` (1 walk-class, 2 jets, 3 claw).
// A cell opened by a link keeps the highest class on its way; walking on from it keeps that class.
void Solver::flood(int stage) {
    queue_.assign(reached_.begin(), reached_.end());
    for (size_t head = 0; head < queue_.size(); head++) {
        if ((head & 4095) == 0 && cancelled()) { out_.cancelled = true; return; }
        const int a = queue_[head];
        const uint8_t cl = out_.cls[a];
        uint8_t m = wmask_[a];
        while (m) {
            const int d = std::countr_zero(m);
            m = uint8_t(m & (m - 1));
            const int b = a + off8_[d];
            if (!out_.cls[b]) {
                out_.cls[b] = cl;
                reached_.push_back(b);
                queue_.push_back(b);
            }
        }
        for (int s = done_[a] + 1; s <= stage; s++) gen_links(a, s);
        done_[a] = uint8_t(std::max<int>(done_[a], stage));
    }
}

// A BA can ride a VTOL that it can reach: every free cell under the VTOL's flight region (with the rider hanging below) is reached.
void Solver::ride_stage() {
    if (!pr_.ride || in_.flyers.empty() || out_.cancelled) return;
    std::vector<std::pair<float, float>> fs;
    for (const ReachSpawn& f : in_.flyers) {
        if (f.team != team_ && f.team != 0) continue;
        const int c = snap_free(f.x, f.z, 6.f);
        if (c >= 0 && out_.cls[c]) fs.push_back({f.x, f.z});
    }
    if (fs.empty()) return;
    const ReachProfile vp = reach_vtol_profile(pr_.driver);
    VtolField F;
    vtol_flood(in_, vp, vp.rider_extra, fs, F, cancel_);
    for (int i = 0; i < N_; i++) {
        if (!free_[i] || out_.cls[i]) continue;
        const int f = ((i / w_) / F.k) * F.fw + (i % w_) / F.k;
        if (!F.reached[f]) continue;
        out_.cls[i] = R_FLIGHT;
        reached_.push_back(i);
    }
    flood(stage_max_);
}

void Solver::seeds_and_flood() {
    const float snap = reach_rule(RL_SPAWN_SNAP).player;
    for (const ReachSpawn& s : in_.spawns) {
        if (s.team != team_) continue;
        out_.spawn_total++;
        const int c = snap_free(s.x, s.z, snap);
        if (c < 0) continue;
        out_.spawn_ok++;
        if (!out_.cls[c]) { out_.cls[c] = R_WALK; reached_.push_back(c); }
    }
    if (pr_.immobile || reached_.empty()) return;
    stage_max_ = pr_.latch ? 3 : pr_.jets ? 2 : 1;
    if (pr_.latch) {
        nearL_.assign(size_t(N_), 0);
        rv_.assign(size_t(N_), 0);
        const int K = latch_k_;
        bool any = false;
        for (int i = 0; i < N_; i++) {
            if (!(flag_at(in_, size_t(i)) & F_LATCH)) continue;
            any = true;
            const int cx = i % w_, cz = i / w_;
            for (int dz = std::max(0, cz - K); dz <= std::min(h_ - 1, cz + K); dz++)
                for (int dx = std::max(0, cx - K); dx <= std::min(w_ - 1, cx + K); dx++) nearL_[dz * w_ + dx] = 1;
        }
        if (!any) { nearL_.clear(); stage_max_ = pr_.jets ? 2 : 1; }
    }
    done_.assign(size_t(N_), 0);
    flood(1);
    if (stage_max_ >= 2 && !out_.cancelled) flood(2);
    if (stage_max_ >= 3 && !out_.cancelled) flood(3);
    ride_stage();
}

void Solver::components() {
    out_.comp.assign(size_t(N_), -1);
    comp_size_.clear();
    int id = 0;
    std::vector<int> q;
    for (int i = 0; i < N_; i++) {
        if (!free_[i] || out_.comp[i] >= 0) continue;
        q.assign(1, i);
        out_.comp[i] = id;
        for (size_t head = 0; head < q.size(); head++) {
            const int a = q[head];
            uint8_t m = wmask_[a];
            while (m) {
                const int d = std::countr_zero(m);
                m = uint8_t(m & (m - 1));
                const int b = a + off8_[d];
                if (out_.comp[b] < 0) { out_.comp[b] = id; q.push_back(b); }
            }
        }
        comp_size_.push_back(int(q.size()));
        id++;
    }
}

// The fall limit of a step-off from cell a over bare terrain: the body's own (the Bots' planner value, the player's drop), raised to the
// PocketDrop limit while the walk region of a is small (PATH:658-663,905-925: a dry search that ended short after < 40000 expansions restarts at 40 m).
float Solver::drop_limit(int a) const {
    float lim = pr_.drop_terr;
    if (pr_.drop_pocket > lim && pr_.pocket_cells > 0 && out_.comp.size() == size_t(N_)) {
        const int c = out_.comp[size_t(a)];
        if (c >= 0 && size_t(c) < comp_size_.size() && comp_size_[size_t(c)] < pr_.pocket_cells) lim = pr_.drop_pocket;
    }
    return lim;
}

// Faces: from a free cell at the edge of the free ground, a march over blocked cells (<= 28 m) to the first free cell at least 2 m lower.
// One face per 8 m block per direction (the one with the smallest rise). The verdicts use the same scans as the flood.
void Solver::build_faces() {
    const int bw = int(float(w_) * cell_ / 8.f) + 2, bh = int(float(h_) * cell_ / 8.f) + 2;
    std::vector<int32_t> best(size_t(bw) * bh * 8, -1);
    std::vector<uint8_t> evald;  // per face: up / down already evaluated (only a tie needs it before the end)
    auto score = [](const ReachFace& f) { return (f.up != R_NONE ? 2 : 0) + (f.down != R_NONE ? 1 : 0); };
    const float min_rise = std::max(2.f, stepc_);
    for (int a = 0; a < N_; a++) {
        if (!free_[a] || wmask_[a] == 0xFF) continue;
        if ((a & 4095) == 0 && cancelled()) break;
        const int cx = a % w_, cz = a / w_;
        for (int d = 0; d < 8; d++) {
            const auto& ray = rays_[2 * d];
            float band = -1e9f;
            bool first = true;
            for (const RayStep& s : ray) {
                if (s.run > pr_.up_run + 1e-3f) break;
                const int nx = cx + s.dx, nz = cz + s.dz;
                if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) break;
                const int n = nz * w_ + nx;
                if (first) {
                    if (free_[n]) break;
                    first = false;
                }
                if (free_[n]) {
                    const float rise = S_[a] - S_[n];
                    if (rise >= min_rise) {
                        const int key = (int((float(cz) + 0.5f) * cell_ / 8.f) * bw + int((float(cx) + 0.5f) * cell_ / 8.f)) * 8 + d;
                        ReachFace f;
                        f.top = a;
                        f.bot = n;
                        f.rise = rise;
                        f.run = s.run;
                        f.band_top = band;
                        f.dir = uint8_t(d);
                        // One face per block and direction: the smallest rise. Faces of the same rise (within 0.25 m) are the same cliff: the one whose
                        // verdict is best wins, not the first scanned (the first is always the south / west end, whose corner cells have no room to land).
                        if (best[key] < 0) { best[key] = int32_t(out_.faces.size()); out_.faces.push_back(f); evald.push_back(0); }
                        else {
                            ReachFace& cur = out_.faces[best[key]];
                            if (rise < cur.rise - 0.25f) { cur = f; evald[best[key]] = 0; }
                            else if (rise < cur.rise + 0.25f) {
                                if (!evald[best[key]]) { eval_face(cur); evald[best[key]] = 1; }
                                eval_face(f);
                                if (score(f) > score(cur) || (score(f) == score(cur) && rise < cur.rise)) { cur = f; evald[best[key]] = 1; }
                            }
                        }
                    }
                    break;
                }
                band = std::max(band, S_[n]);
            }
        }
    }
    for (size_t i = 0; i < out_.faces.size(); i++)
        if (!evald[i]) eval_face(out_.faces[i]);
}

// A face's verdicts: can a unit at the top step or dive down it, can a unit at the bottom jet (or claw) up it. Uses the same scans as the flood.
void Solver::eval_face(ReachFace& f) const {
    {
        ReachLink L;
        f.up = R_NONE;
        f.down = R_NONE;
        if (drop_scan(f.top, f.dir, L)) f.down = R_WALK;
        else if (pr_.jets && jet_scan(f.top, 2 * f.dir, L) && L.rise <= -reach_rule(RL_DIVE_FALL_MIN).player) f.down = R_JET;
        if (pr_.jets && jet_scan(f.bot, (2 * f.dir + 8) & 15, L) && L.kind == L_UP) f.up = R_JET;
        // The claw (a Battle Armor suit gripping an object wall) is its own leg: a face with a gripable wall in it, and gripable ground at both
        // ends, is "up by claws" whatever its height, unless the jets already make it.
        if (f.up == R_NONE && pr_.latch && !nearL_.empty() && nearL_[size_t(f.bot)] && nearL_[size_t(f.top)]) {
            const int tx = f.top % w_, tz = f.top / w_;
            for (const RayStep& s : rays_[2 * f.dir]) {
                if (s.run > pr_.up_run + 1e-3f) break;
                const int nx = tx + s.dx, nz = tz + s.dz;
                if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) break;
                const int n = nz * w_ + nx;
                if (n == f.bot) break;
                if (flag_at(in_, size_t(n)) & F_LATCH) { f.up = R_LATCH; break; }
            }
        }
    }
}

void Solver::finish_classes() {
    std::vector<int> soft;
    for (int i = 0; i < N_; i++) {
        if (oob_flag(i)) out_.cls[i] = R_OOB;
        else if (blk_[i]) out_.cls[i] = R_WALL;
        else if (!free_[i]) { out_.cls[i] = 255; soft.push_back(i); }
    }
    // Cells that only fail the clearance (next to a wall) take the class of the ground beside them, so a plateau edge is tinted like its top.
    std::vector<uint8_t> next(soft.size());
    for (int pass = 0; pass < 4; pass++) {
        for (size_t k = 0; k < soft.size(); k++) {
            const int i = soft[k];
            next[k] = out_.cls[i];
            if (out_.cls[i] != 255) continue;
            const int cx = i % w_, cz = i / w_;
            int best = 255;
            for (int d = 0; d < 8; d++) {
                const int nx = cx + kDx8[d], nz = cz + kDz8[d];
                if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
                const int c = out_.cls[nz * w_ + nx];
                if (c <= R_FLIGHT) best = best == 255 ? c : std::max(best, c);
            }
            next[k] = uint8_t(best);
        }
        for (size_t k = 0; k < soft.size(); k++) out_.cls[soft[k]] = next[k];
    }
    for (int i : soft)
        if (out_.cls[i] == 255) out_.cls[i] = R_WALL;
    for (int i = 0; i < N_; i++) out_.counts[std::min<int>(out_.cls[i], 6)]++;
}

void Solver::run_vtol() {
    std::vector<std::pair<float, float>> seeds;
    for (const ReachSpawn& s : in_.spawns)
        if (s.team == team_) { out_.spawn_total++; seeds.push_back({s.x, s.z}); }
    for (const ReachSpawn& f : in_.flyers)
        if (f.team == team_ || f.team == 0) seeds.push_back({f.x, f.z});
    VtolField F;
    std::vector<uint8_t> seed_ok;
    vtol_flood(in_, pr_, 0.f, seeds, F, cancel_, &seed_ok);
    out_.cancelled = cancelled();
    // A VTOL start is usable when its craft starts in the air or on a pad with clear air round it (the first spawn_total seeds are the starts).
    for (int k = 0; k < out_.spawn_total && size_t(k) < seed_ok.size(); k++) out_.spawn_ok += seed_ok[size_t(k)];
    // Landable: slope <= land_slope over land_radius, no water, no out-of-bounds. Worked out for every ground cell (2 m), not for every
    // 4 m flight cell, so the edge of the landable ground follows the terrain at the texel size (the terrain itself is 8 m cells).
    const int R = int(std::ceil(pr_.land_radius / cell_ - 1e-4f));
    auto landable_at = [&](int cx, int cz) {
        float mn = 1e9f, mx = -1e9f;
        for (int dz = -R; dz <= R; dz++)
            for (int dx = -R; dx <= R; dx++) {
                if (float(dx * dx + dz * dz) * cell_ * cell_ > pr_.land_radius * pr_.land_radius) continue;
                const int nx = std::clamp(cx + dx, 0, w_ - 1), nz = std::clamp(cz + dz, 0, h_ - 1), n = nz * w_ + nx;
                if (flag_at(in_, size_t(n)) & (F_WATER | F_OOB)) return false;
                mn = std::min(mn, S_[n]);
                mx = std::max(mx, S_[n]);
            }
        return (mx - mn) / (2.f * pr_.land_radius) <= pr_.land_slope;
    };
    for (int i = 0; i < N_; i++) {
        const size_t f = size_t((i / w_) / F.k) * F.fw + size_t((i % w_) / F.k);
        if (oob_flag(i)) out_.cls[i] = R_OOB;
        else if (F.reached[f]) out_.cls[i] = landable_at(i % w_, i / w_) ? R_WALK : R_FLIGHT;
        else out_.cls[i] = R_NONE;
    }
    for (int i = 0; i < N_; i++) out_.counts[std::min<int>(out_.cls[i], 6)]++;
    out_.comp.assign(size_t(N_), -1);
}

void Solver::run() {
    const auto t0 = std::chrono::steady_clock::now();
    out_.w = w_;
    out_.h = h_;
    out_.cell = cell_;
    out_.cls.assign(size_t(N_), R_NONE);
    obj_min_ = reach_rule(RL_OBJ_MIN).player;
    natural_ = reach_rule(RL_NATURAL).player;
    stepc_ = pr_.step * cell_ * 0.5f;  // the table's step is per 2 m of ground
    need_ = need_for(pr_.radius);
    need2_ = need_for(pr_.radius + pr_.land_extra);
    latch_k_ = std::max(1, int(std::ceil((float(need_) * cell_ / 5.f + 2.f) / cell_ - 1e-4f)));
    for (int d = 0; d < 8; d++) off8_[d] = kDz8[d] * w_ + kDx8[d];
    build_surface();
    if (pr_.flies) {
        run_vtol();
    } else {
        build_rays(rays_, cell_, std::max(std::max(pr_.up_run, pr_.dive_run), std::max(pr_.hop_run, pr_.under_run)));
        build_walls();
        build_masks();
        components();  // before the flood: PocketDrop needs the size of the walk region a step-off starts from
        seeds_and_flood();
        if (!out_.cancelled) build_faces();
        finish_classes();
    }
    out_.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

ReachResult reach_compute(const ReachInput& in, const ReachProfile& profile, int team, const std::atomic<bool>* cancel) {
    ReachResult out;
    if (!in.valid()) return out;
    Solver s(in, profile, team, cancel, out);
    s.run();
    return out;
}


// ---- reach_explain ------------------------------------------------------------------------------------------------------------------

namespace {

std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", double(v));
    return b;
}

std::string range_text(float lo, float hi) { return lo + 0.5f >= hi ? num(lo) : num(lo) + "-" + num(hi); }

// 0 = east, 1 = north-east, 2 = north ... (+x east, +z north), the side of (dx, dz).
int side8_of(float dx, float dz) {
    const float a = std::atan2(dz, dx);
    const int k = int(std::floor(a / 0.78539816f + 0.5f));
    return ((k % 8) + 8) % 8;
}

const char* short_side(int d) {
    static const char* const k[8] = {"E", "NE", "N", "NW", "W", "SW", "S", "SE"};
    return k[d & 7];
}

}  // namespace

ReachAnswer reach_answer(const ReachInput& in, const ReachResult& r, const ReachProfile& pr, float x, float z) {
    ReachAnswer A;
    A.label = pr.unit ? pr.unit->label : reach_group_name(pr.group);
    if (pr.driver == ReachDriver::Bots) A.label += " (bots)";
    const int cx = int(std::floor(x / r.cell)), cz = int(std::floor(z / r.cell));
    if (r.w <= 0 || cx < 0 || cz < 0 || cx >= r.w || cz >= r.h || r.cls.size() != size_t(r.w) * r.h) {
        A.outside = true;
        A.verdict = "outside the map";
        A.here = "-";
        A.how = "outside the map";
        A.cliff = "-";
        return A;
    }
    const int i = cz * r.w + cx;
    const uint8_t c = r.cls[size_t(i)];
    A.cls = c;
    const bool jetter = pr.jets;
    const uint32_t lim_est = jetter ? E_CLIMB : E_STEP;
    // The limit named next to a cliff: "jets lift about 23 m" / "walks at most 140%".
    // A unit with a terrain rule (the player's walkers) names both: terrain faces and object faces are different rules.
    const bool two_rules = !jetter && pr.terrain_slope > 0.f;
    const float climb_max = std::max(pr.climb, pr.climb_terr);  // terrain faces (the Bots' planner reach) / object faces (measured)
    const std::string limit = jetter ? "jets lift about " + num(climb_max) + " m" + (pr.climb_terr > pr.climb + 0.5f ? ", " + num(pr.climb) + " m over objects" : std::string())
                              : two_rules ? "walks terrain up to " + num(pr.terrain_slope * 100.f) + "%, object faces (roofs, walls) up to about " + num(pr.step * 50.f) + "%, est."
                                          : "walks at most " + num(pr.step * 50.f) + "%";

    // ---- the verdict for the cell under the cursor
    bool est = false;
    if (pr.immobile) {
        A.verdict = c == R_WALK ? "yes, it sits here (it doesn't move)" : "it doesn't move";
        A.here = c == R_WALK ? "Sits here" : "-";
        A.how = "doesn't move";
    } else if (pr.flies) {
        switch (c) {
            case R_WALK: A.verdict = "yes, can land"; A.here = "Lands"; A.how = "lands"; break;
            case R_FLIGHT: A.verdict = "can fly over, can't land"; A.here = "Flies over"; A.how = "flies, can't land"; break;
            case R_OOB: A.verdict = "out of bounds"; A.here = "No"; A.how = "out of bounds"; break;
            default: A.verdict = "can't fly here"; A.here = "No"; A.how = "can't fly here"; break;
        }
        if (c == R_WALK || c == R_FLIGHT) est = est || (pr.est & E_VTOL);
    } else {
        switch (c) {
            case R_WALK: A.verdict = pr.group == ReachGroup::Tank ? "yes, drives" : "yes, walks"; A.here = "Yes"; A.how = pr.group == ReachGroup::Tank ? "drives" : "walks"; break;
            case R_JET: A.verdict = "yes, jets only"; A.here = "Yes"; A.how = "jets only"; est = est || (pr.est & E_CLIMB); break;
            case R_LATCH: A.verdict = "yes, only by claw-climbing a wall"; A.here = "Yes"; A.how = "claws only"; est = est || (pr.est & E_LATCH); break;
            case R_FLIGHT: A.verdict = "yes, only with a VTOL ride"; A.here = "Yes"; A.how = "VTOL ride only"; est = est || (pr.est & E_VTOL); break;
            case R_NONE: A.verdict = "can stand here, can't get here"; A.here = "No"; A.how = "never gets here"; break;
            case R_WALL: A.verdict = "can't stand here (cliff face or blocked)"; A.here = "Can't stand"; A.how = "cliff face"; break;
            default: A.verdict = "out of bounds"; A.here = "No"; A.how = "out of bounds"; break;
        }
    }

    // ---- red cell: why it never gets there, in one clause
    if (c == R_NONE && !pr.flies && !pr.immobile) {
        if (r.spawn_total == 0) {
            A.why = "this team has no start point";
            A.how = "no start point";
        } else {
            int cc = -1;
            if (r.comp.size() == r.cls.size()) {
                cc = r.comp[size_t(i)];
                for (int ring = 1; cc < 0 && ring <= 3; ring++)  // a cell beside a wall has no component of its own: take the nearest one
                    for (int dz = -ring; dz <= ring; dz++)
                        for (int dx = -ring; dx <= ring; dx++) {
                            const int nx = cx + dx, nz = cz + dz;
                            if (cc >= 0 || nx < 0 || nz < 0 || nx >= r.w || nz >= r.h) continue;
                            cc = r.comp[size_t(nz) * r.w + nx];
                        }
            }
            float up_min = 1e9f, in_min = 1e9f;
            int n_top = 0, n_bot = 0;
            bool up_ok = false;
            if (cc >= 0)
                for (const ReachFace& f : r.faces) {
                    if (r.comp[size_t(f.top)] == cc) {
                        n_top++;
                        up_min = std::min(up_min, f.rise);
                        up_ok = up_ok || f.up != R_NONE;
                    }
                    if (r.comp[size_t(f.bot)] == cc) {
                        n_bot++;
                        in_min = std::min(in_min, f.rise);
                    }
                }
            if (cc < 0 || (n_top == 0 && n_bot == 0)) {
                A.why = "no way across to it";
                A.how = "no way across";
            } else if (n_top > 0) {
                if (up_ok) {
                    A.why = "the ground below its cliffs is never reached either";
                    A.how = "lower ground unreached";
                } else if (jetter && up_min <= climb_max + 0.5f) {
                    A.why = "no way up has a clear place to land";
                    A.how = "no clear landing";
                } else if (jetter) {
                    A.why = "every way up is a wall above " + num(climb_max) + " m";
                    A.how = "wall above " + num(climb_max) + " m";
                } else {
                    A.why = "every way up is too steep to walk (lowest cliff " + num(up_min) + " m)";
                    A.how = "too steep to walk";
                }
                est = est || (pr.est & lim_est);
            } else {
                A.why = "every way in is a drop it can't make (lowest " + num(in_min) + " m)";
                A.how = "drop too long";
                est = est || (pr.est & E_DROP);
            }
        }
    }

    // ---- the nearest cliffs (within 40 m), one per compass side as seen from the cursor
    if (!pr.flies && !pr.immobile) {
        constexpr float kReachM = 40.f;
        struct Hit { const ReachFace* f = nullptr; float d = 1e9f; };
        Hit best[8];
        auto at = [&](int cell, float& px, float& pz) { px = (float(cell % r.w) + 0.5f) * r.cell; pz = (float(cell / r.w) + 0.5f) * r.cell; };
        auto face_info = [&](const ReachFace& f, int& side, float& d) {
            float tx, tz, bx, bz;
            at(f.top, tx, tz);
            at(f.bot, bx, bz);
            d = std::min(std::hypot(tx - x, tz - z), std::hypot(bx - x, bz - z));
            side = side8_of((tx + bx) * 0.5f - x, (tz + bz) * 0.5f - z);
        };
        for (const ReachFace& f : r.faces) {
            int side;
            float d;
            face_info(f, side, d);
            if (d > kReachM) continue;
            if (d < best[side].d) best[side] = {&f, d};
        }
        std::vector<int> sides;
        for (int d = 0; d < 8; d++) {
            if (!best[d].f) continue;
            if ((d & 1) && (best[(d + 1) & 7].f || best[(d + 7) & 7].f)) continue;  // a straight cliff also shows up in the diagonals
            sides.push_back(d);
        }
        std::sort(sides.begin(), sides.end(), [&](int a, int b) { return best[a].d < best[b].d; });
        if (sides.size() > 3) sides.resize(3);
        for (int d : sides) {
            const ReachFace& f0 = *best[d].f;
            float lo = f0.rise, hi = f0.rise;
            // The stretch of cliff on this side: faces of about the same height within 16 m of the nearest one. One face of it (a corner cell with no
            // room to land, a diagonal ray) does not speak for the whole stretch: it is climbable / steppable where any of its faces is, so the text
            // does not depend on which face happens to be nearest (north vs south of a symmetric tier).
            uint8_t g_up = f0.up, g_down = f0.down;
            for (const ReachFace& f : r.faces) {
                int side;
                float dd;
                face_info(f, side, dd);
                if (side != d || dd > best[d].d + 16.f || std::fabs(f.rise - f0.rise) > 4.f) continue;
                lo = std::min(lo, f.rise);
                hi = std::max(hi, f.rise);
                if (f.up == R_JET || (f.up == R_LATCH && g_up != R_JET)) g_up = f.up;
                if (f.down != R_NONE && g_down == R_NONE) g_down = f.down;
            }
            bool e = false;
            std::string what, detail, shortv;
            if (g_up == R_JET) {
                what = g_down != R_NONE ? "up and down" : "up only";
                shortv = "up";
                detail = limit;
                e = (pr.est & E_CLIMB) != 0;
            } else if (g_up == R_LATCH) {
                what = g_down != R_NONE ? "up by claws, and down" : "up by claws";
                shortv = "up by claws";
                e = (pr.est & E_LATCH) != 0;
            } else if (g_down != R_NONE) {
                what = "down only";
                shortv = "down only";
                detail = limit;
                e = (pr.est & lim_est) != 0;
            } else {
                what = "no way up or down";
                shortv = "no way up";
                detail = limit;
                e = (pr.est & lim_est) != 0;
            }
            if (two_rules) e = false;  // the limit text already carries its own "est."
            std::string line = std::string("Cliff to the ") + reach_compass(d) + ", " + range_text(lo, hi) + " m: " + what;
            if (!detail.empty()) line += " (" + detail + (e ? ", est.)" : ")");
            else if (e) line += " (est.)";
            A.cliffs.push_back(line);
            if (A.cliff.empty()) A.cliff = std::string(short_side(d)) + " " + range_text(lo, hi) + " m: " + shortv;
        }
        if (A.cliff.empty()) A.cliff = "none within 40 m";
    } else {
        A.cliff = "-";
    }

    if (!in.water_y.empty() && (flag_at(in, size_t(i)) & F_WATER) && !std::isnan(in.water_y[size_t(i)])) {
        char b[48];
        std::snprintf(b, sizeof b, "water %.1f m deep", double(in.water_y[size_t(i)] - in.th[size_t(i)]));
        A.notes = b;
    }
    if (pr.flies) {
        if (r.spawn_total > 0 && r.spawn_ok < r.spawn_total) A.notes += std::string(A.notes.empty() ? "" : "; ") + "a start point has no clear air for a VTOL";
    } else if (r.spawn_total == 0) {
        A.notes += std::string(A.notes.empty() ? "" : "; ") + "no start point for this team";
    } else if (r.spawn_ok < r.spawn_total) {
        A.notes += std::string(A.notes.empty() ? "" : "; ") + "a start point is on unusable ground";
    }
    A.est = est;
    return A;
}

std::string reach_explain(const ReachInput& in, const ReachResult& r, const ReachProfile& pr, float x, float z) {
    const ReachAnswer A = reach_answer(in, r, pr, x, z);
    std::string out = A.label + (A.outside ? ": " : " here: ") + A.verdict;
    if (!A.why.empty()) out += " - " + A.why;
    if (!A.notes.empty()) out += "; " + A.notes;
    if (A.est) out += " (est.)";
    for (const std::string& l : A.cliffs) out += "\n" + l;
    return out;
}

}  // namespace ma2
