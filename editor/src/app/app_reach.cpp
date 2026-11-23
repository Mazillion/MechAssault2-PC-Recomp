// "Can it get here?": the reachability overlay in the editor (spec: reach\REACH-SPEC.md sections 1 and 4).
//
// The numbers and the flood are in src/core/reach.* and reach_rules.h (pure, tested in fmt_test). This file is the app side:
//   - main thread, once per change: copies what the worker needs (ground, solid model triangles by model, object placements, water,
//     type map, spawns, VTOLs) and hands it over after a 250 ms quiet time;
//   - one worker thread: rasterises the objects, floods the chosen unit first, then every other movement group for "Compare all";
//   - main thread again: swaps the finished result in (never waits for the worker), paints the ground tint, the face arrows, the roof
//     outlines, the hover readout and the floating panel.
// The worker only ever touches its own copies: never the document, the object views or the GPU models.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <windows.h>
#include "app.h"
#include "../core/json.h"
#include "../core/reach.h"
#include "../core/terrain.h"
#include "imgui.h"
#include "theme.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;
using ma2::Json;

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kDebounceS = 0.25;

uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}
uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    uint64_t w = 0;
    if (i < n) std::memcpy(&w, b + i, n - i);
    return (h ^ w) * 1099511628211ull;
}
uint64_t hash_f(uint64_t h, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return mix64(h, u);
}

// Adds the time of its scope to a smoothed millisecond figure.
struct EmaTimer {
    double& v;
    Clock::time_point t0 = Clock::now();
    explicit EmaTimer(double& x) : v(x) {}
    ~EmaTimer() { v += (std::chrono::duration<double, std::milli>(Clock::now() - t0).count() - v) * 0.05; }
};

// The overlay's own text scale: the editor's UI scale is picked once (first run) so a 3840 x 1600 screen with a 1.0 scale got tiny panels and
// labels. The same fit rule as the editor's first-run auto scale, applied on top when the UI scale is smaller than the screen asks for.
float reach_text_k(float ui_scale, float base_dpi, ImVec2 display) {
    const float lw = display.x / std::max(0.5f, base_dpi), lh = display.y / std::max(0.5f, base_dpi);
    const float want = std::clamp(std::floor(std::min(lw / 1920.f, lh / 1000.f) * 4.f) / 4.f, 1.f, 2.f);
    return std::clamp(want / std::max(0.5f, ui_scale), 1.f, 2.f);
}
struct ReachTextScale {
    float& dpi;
    float old;
    bool on;
    ReachTextScale(float& d, float k) : dpi(d), old(d), on(k > 1.01f) {
        if (!on) return;
        dpi = old * k;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * k);
    }
    ~ReachTextScale() {
        if (!on) return;
        ImGui::PopFont();
        dpi = old;
    }
};

enum : uint8_t { IK_PLAIN = 0, IK_LATCH = 1, IK_TREE = 2 };

struct Inst {  // one solid object for the worker
    std::shared_ptr<const std::vector<float>> tris;  // model space, 9 floats per triangle
    float m[12] = {};                                // 3x4 model -> world, row major
    uint8_t kind = IK_PLAIN;
};
struct Water {
    float cx = 0, cz = 0, w = 0, l = 0, heading = 0, surface = 0;
    bool oval = false;
};
struct Roof {  // the top of an object, as an outline (main thread only)
    float x[4] = {}, z[4] = {}, y = 0;
    int cell_x = 0, cell_z = 0;
};

struct Run {  // faces that continue each other along one side of one cliff: one arrow per ~28 m and one height label
    std::vector<uint32_t> faces;  // indices into ReachResult::faces, in order along the cliff
    float rmin = 0, rmax = 0, len_m = 0;
    uint8_t dir = 0;
};

struct Job {
    uint64_t gen = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    Terrain terrain;  // a lean copy: sizes and heights only
    std::vector<uint16_t> typemap;
    int tm_w = 0, tm_h = 0;
    bool multiplayer = true;
    std::vector<Inst> insts;
    std::vector<Water> waters;
    std::vector<ReachSpawn> spawns, flyers;
    float vtol_cap = 55.f;
    int unit = 0, team = 1;
    ReachDriver driver = ReachDriver::Player;
    bool compare = true;
};

struct GroupRes {
    std::string name;     // "Jet 'Mechs, about 23 m"
    std::string members;  // "Bowman, Catapult"
    ReachProfile profile;
    std::shared_ptr<const ReachResult> res;
};
struct Output {
    uint64_t gen = 0;
    bool complete = false;
    std::shared_ptr<const ReachInput> input;
    std::shared_ptr<const ReachResult> sel;
    ReachProfile sel_profile;
    std::shared_ptr<const ReachResult> other;  // the same unit with the OTHER driver (player <-> bots): "you can, bots can't"
    ReachProfile other_profile;
    std::vector<GroupRes> groups;
    double raster_ms = 0, total_ms = 0;
};

struct Cached {
    std::shared_ptr<const std::vector<float>> tris;
    size_t n = 0;
};

}  // namespace

struct ReachState {
    // worker side (guarded by mu)
    std::thread th;
    std::mutex mu;
    std::condition_variable cv;
    std::shared_ptr<Job> pending;
    std::shared_ptr<const Output> latest;
    bool quit = false;
    // main thread only
    std::shared_ptr<std::atomic<bool>> cancel;  // of the newest submitted job
    uint64_t gen = 0;
    std::shared_ptr<const Output> cur;
    const void* doc = nullptr;
    uint32_t coll_rev = 0xFFFFFFFFu;
    std::unordered_map<const GpuModel*, Cached> tri_cache;
    std::vector<Inst> insts;
    std::vector<Water> waters;
    std::vector<Roof> roofs;
    std::vector<ReachSpawn> spawns, flyers;
    float vtol_cap = 55.f;
    uint64_t world_hash = 0, seen_key = 0, submitted_key = 0;
    Clock::time_point dirty_since = Clock::now();
    bool waiting_gesture = false;
    int models_ready = 0;
    std::string readout;  // the hover text now (automation state)
    double t_frame = 0, t_arrows = 0, t_paint = 0, t_readout = 0;  // main-thread cost, ms, smoothed (automation state)
    int roofs_drawn = 0;
    int glyphs_prev = 0;  // glyphs drawn last frame: a crowded view thins itself out
    const Output* runs_for = nullptr;  // which result `runs` belongs to
    std::vector<Run> runs;             // the selected result's face runs, the ones drawn first first

    ~ReachState() {
        {
            std::lock_guard<std::mutex> lk(mu);
            quit = true;
            pending.reset();
        }
        if (cancel) cancel->store(true);
        cv.notify_all();
        if (th.joinable()) th.join();
    }
};

namespace {

// ---- the movement groups of "Compare all" -------------------------------------------------------------------------------------------
struct GroupDef {
    ReachGroup group = ReachGroup::Mech;
    int climb10 = 0;
    std::string name, members;
    int unit = 0;  // the first unit of the group: its profile stands for all
    int count = 0;
    std::vector<std::string> names;
};

std::string member_name(const char* label) {
    std::string s = label;
    const size_t p = s.find(" (");
    if (p != std::string::npos) s.resize(p);
    return s;
}

std::vector<GroupDef> reach_groups(ReachDriver d) {
    std::vector<GroupDef> out;
    for (int i = 0; i < kReachUnitCount; i++) {
        const ReachUnit& u = kReachUnits[i];
        if (u.group == ReachGroup::Turret) continue;
        if (std::strstr(u.label, "no bomb")) continue;
        const ReachProfile p = reach_profile(i, d);
        const int c10 = u.group == ReachGroup::JetMech ? int(std::lround(p.climb)) : 0;
        GroupDef* g = nullptr;
        for (GroupDef& x : out)
            if (x.group == u.group && x.climb10 == c10) { g = &x; break; }
        if (!g) {
            out.emplace_back();
            g = &out.back();
            g->group = u.group;
            g->climb10 = c10;
            g->unit = i;
            char b[64];
            switch (u.group) {
                case ReachGroup::JetMech: std::snprintf(b, sizeof b, "Jet 'Mechs ~%.0f m", double(p.climb)); g->name = b; break;
                case ReachGroup::Mech: g->name = "'Mechs without jets"; break;
                case ReachGroup::BattleArmor: g->name = "Battle Armor"; break;
                case ReachGroup::Foot: g->name = "Pilot on foot"; break;
                case ReachGroup::Tank: g->name = "Tanks"; break;
                case ReachGroup::Vtol: g->name = "VTOL"; break;
                default: g->name = reach_group_name(u.group); break;
            }
        }
        const std::string mn = member_name(u.label);
        if (std::find(g->names.begin(), g->names.end(), mn) != g->names.end()) continue;
        g->names.push_back(mn);
    }
    for (GroupDef& g : out) {
        g.count = int(g.names.size());
        for (size_t i = 0; i < g.names.size(); i++) g.members += (i ? ", " : "") + g.names[i];
    }
    return out;
}

std::string short_members(const std::string& m, int keep = 4) {
    size_t pos = 0;
    for (int k = 0; k < keep; k++) {
        const size_t c = m.find(", ", pos);
        if (c == std::string::npos) return m;
        pos = c + 2;
    }
    return m.substr(0, pos) + "...";
}

// The row label of "Compare all": "Jet 'Mechs ~23 m: Stiletto, Raptor, Bowman, Catapult".
std::string group_label(const GroupRes& g) {
    const bool named = g.profile.group == ReachGroup::JetMech || g.profile.group == ReachGroup::Mech || g.profile.group == ReachGroup::Tank;
    if (!named || g.members.find(", ") == std::string::npos) return g.name;
    return g.name + ": " + short_members(g.members, 4);
}

// Faces that touch (tops within 14 m), face the same way, are the same kind (up / down) and have a similar height make one run.
std::vector<Run> build_runs(const ReachResult& r) {
    const size_t n = r.faces.size();
    std::vector<int> parent(n);
    for (size_t i = 0; i < n; i++) parent[i] = int(i);
    auto find = [&](int a) {
        while (parent[size_t(a)] != a) a = parent[size_t(a)] = parent[size_t(parent[size_t(a)])];
        return a;
    };
    auto cell_xz = [&](const ReachFace& f, float& x, float& z) { x = (float(f.top % r.w) + 0.5f) * r.cell; z = (float(f.top / r.w) + 0.5f) * r.cell; };
    auto key_of = [](int bx, int bz, const ReachFace& f) {
        return (uint64_t(bz + 4) << 40) | (uint64_t(bx + 4) << 16) | uint64_t(f.dir) | (uint64_t(f.up) << 3) | (uint64_t(f.down) << 5);
    };
    std::unordered_map<uint64_t, int> at;
    at.reserve(n * 2);
    std::vector<int> bxs(n), bzs(n);
    for (size_t i = 0; i < n; i++) {
        float x, z;
        cell_xz(r.faces[i], x, z);
        bxs[i] = int(x / 8.f);
        bzs[i] = int(z / 8.f);
        at[key_of(bxs[i], bzs[i], r.faces[i])] = int(i);
    }
    for (size_t i = 0; i < n; i++) {
        const ReachFace& f = r.faces[i];
        float x, z;
        cell_xz(f, x, z);
        for (int dz = -2; dz <= 2; dz++)
            for (int dx = -2; dx <= 2; dx++) {
                if (!dx && !dz) continue;
                const auto it = at.find(key_of(bxs[i] + dx, bzs[i] + dz, f));
                if (it == at.end()) continue;
                const ReachFace& g = r.faces[size_t(it->second)];
                float gx, gz;
                cell_xz(g, gx, gz);
                if (std::hypot(gx - x, gz - z) > 14.f || std::fabs(g.rise - f.rise) > 4.f) continue;
                parent[size_t(find(int(i)))] = find(it->second);
            }
    }
    std::unordered_map<int, size_t> index;
    std::vector<Run> runs;
    for (size_t i = 0; i < n; i++) {
        const int root = find(int(i));
        auto it = index.find(root);
        if (it == index.end()) {
            it = index.emplace(root, runs.size()).first;
            runs.emplace_back();
            runs.back().dir = r.faces[i].dir;
            runs.back().rmin = runs.back().rmax = r.faces[i].rise;
        }
        Run& run = runs[it->second];
        run.faces.push_back(uint32_t(i));
        run.rmin = std::min(run.rmin, r.faces[i].rise);
        run.rmax = std::max(run.rmax, r.faces[i].rise);
    }
    static const float kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, kDz[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    for (Run& run : runs) {
        // Order along the cliff (the direction at right angles to the way the face looks).
        const float ax = -kDz[run.dir], az = kDx[run.dir];
        auto proj = [&](uint32_t fi) {
            float x, z;
            cell_xz(r.faces[fi], x, z);
            return x * ax + z * az;
        };
        std::sort(run.faces.begin(), run.faces.end(), [&](uint32_t a, uint32_t b) { return proj(a) < proj(b); });
        run.len_m = (proj(run.faces.back()) - proj(run.faces.front())) / std::hypot(ax, az) + 8.f;
    }
    // A corner artefact: a tiny run (one or two faces) of another verdict right beside a longer run of a similar height is the same cliff seen
    // from its corner (a stray red X at the end of a green face). Its facts stay in the hover readout; it is not drawn.
    {
        std::vector<char> drop(runs.size(), 0);
        for (size_t a = 0; a < runs.size(); a++) {
            if (runs[a].faces.size() > 2) continue;
            for (size_t b = 0; b < runs.size() && !drop[a]; b++) {
                if (b == a || runs[b].faces.size() < 3 || std::fabs(runs[a].rmin - runs[b].rmin) > 5.f) continue;
                for (uint32_t fa : runs[a].faces) {
                    float xa, za;
                    cell_xz(r.faces[fa], xa, za);
                    for (uint32_t fb : runs[b].faces) {
                        float xb, zb;
                        cell_xz(r.faces[fb], xb, zb);
                        if (std::hypot(xa - xb, za - zb) <= 18.f) { drop[a] = 1; break; }
                    }
                    if (drop[a]) break;
                }
            }
        }
        std::vector<Run> kept;
        for (size_t a = 0; a < runs.size(); a++)
            if (!drop[a]) kept.push_back(std::move(runs[a]));
        runs.swap(kept);
    }
    // Straight cliffs first (a diagonal run beside them is the same cliff seen from a corner), then the longer runs.
    std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
        const int ca = a.dir & 1, cb = b.dir & 1;
        if (ca != cb) return ca < cb;
        return a.faces.size() > b.faces.size();
    });
    return runs;
}

// ---- the worker ----------------------------------------------------------------------------------------------------------------------

void transform_tris(const Inst& in, float* dst, const float* src, size_t tris) {
    const float* m = in.m;
    for (size_t t = 0; t < tris * 3; t++) {
        const float x = src[t * 3], y = src[t * 3 + 1], z = src[t * 3 + 2];
        dst[t * 3] = m[0] * x + m[1] * y + m[2] * z + m[3];
        dst[t * 3 + 1] = m[4] * x + m[5] * y + m[6] * z + m[7];
        dst[t * 3 + 2] = m[8] * x + m[9] * y + m[10] * z + m[11];
    }
}

void raster_insts(ReachInput& in, const std::vector<Inst>& insts, uint8_t want_kind, const std::atomic<bool>& cancel) {
    std::vector<float> buf;
    constexpr size_t kChunk = 3000;
    for (const Inst& I : insts) {
        if ((I.kind == IK_TREE) != (want_kind == IK_TREE)) continue;
        if (cancel.load()) return;
        const std::vector<float>& src = *I.tris;
        const size_t n = src.size() / 9;
        for (size_t at = 0; at < n; at += kChunk) {
            const size_t k = std::min(kChunk, n - at);
            buf.resize(k * 9);
            transform_tris(I, buf.data(), src.data() + at * 9, k);
            reach_raster_tris(in, buf.data(), k, I.kind == IK_LATCH);
        }
    }
}

std::shared_ptr<ReachInput> build_input(const Job& job, double* raster_ms) {
    const auto t0 = Clock::now();
    auto in = std::make_shared<ReachInput>();
    reach_input_from_terrain(*in, job.terrain);
    const std::atomic<bool>& cancel = *job.cancel;
    raster_insts(*in, job.insts, IK_PLAIN, cancel);
    // Trees: breakable. Where a tree is the highest thing in a cell and nothing else stands there, the cell's top is breakable.
    bool trees = false;
    for (const Inst& I : job.insts) trees = trees || I.kind == IK_TREE;
    if (trees && !cancel.load()) {
        ReachInput tin;
        tin.init(in->w, in->h, in->cell);
        raster_insts(tin, job.insts, IK_TREE, cancel);
        for (size_t i = 0; i < in->count(); i++) {
            if (tin.top[i] < -1e8f) continue;
            const bool bare = in->top[i] <= in->th[i] + reach_rule(RL_OBJ_MIN).player;  // no solid thing of its own here
            if (tin.top[i] > in->top[i]) {
                in->top[i] = tin.top[i];
                if (bare) in->flags[i] |= F_BREAKABLE;
            }
        }
    }
    if (in->low.size() != in->count()) in->low.assign(in->count(), 1e9f);
    reach_raster_finish(*in);
    // Water: a flat plane at the object's height, rotated, rectangular or oval; the ground under it is the bed.
    const float cell = in->cell;
    for (const Water& w : job.waters) {
        if (w.w <= 0 || w.l <= 0) continue;
        const float ch = std::cos(w.heading), sh = std::sin(w.heading), hw = w.w * 0.5f, hl = w.l * 0.5f;
        const float r = std::sqrt(hw * hw + hl * hl);
        const int x0 = std::max(0, int(std::floor((w.cx - r) / cell))), x1 = std::min(in->w - 1, int(std::floor((w.cx + r) / cell)));
        const int z0 = std::max(0, int(std::floor((w.cz - r) / cell))), z1 = std::min(in->h - 1, int(std::floor((w.cz + r) / cell)));
        for (int cz = z0; cz <= z1; cz++)
            for (int cx = x0; cx <= x1; cx++) {
                const float dx = (float(cx) + 0.5f) * cell - w.cx, dz = (float(cz) + 0.5f) * cell - w.cz;
                const float u = (ch * dx - sh * dz) / hw, v = (sh * dx + ch * dz) / hl;
                if (w.oval ? u * u + v * v > 1.f : std::max(std::fabs(u), std::fabs(v)) > 1.f) continue;
                const size_t i = size_t(cz) * in->w + cx;
                if (w.surface <= in->th[i]) continue;
                in->water_y[i] = std::isnan(in->water_y[i]) ? w.surface : std::max(in->water_y[i], w.surface);
                in->flags[i] |= F_WATER;
            }
    }
    // The type map: Border (and, in a multiplayer map, Out of bounds) is off limits; 16 is deep water.
    if (!job.typemap.empty() && job.tm_w > 0 && job.tm_h > 0) {
        const float sx = job.terrain.size_x(), sz = job.terrain.size_z();
        for (int cz = 0; cz < in->h; cz++) {
            const int tz = std::clamp(int((float(cz) + 0.5f) * cell / sz * float(job.tm_h)), 0, job.tm_h - 1);
            for (int cx = 0; cx < in->w; cx++) {
                const int tx = std::clamp(int((float(cx) + 0.5f) * cell / sx * float(job.tm_w)), 0, job.tm_w - 1);
                const uint16_t v = job.typemap[size_t(tz) * job.tm_w + tx];
                const size_t i = size_t(cz) * in->w + cx;
                if (v == tmtype::Border || (v == tmtype::OutOfBounds && job.multiplayer)) in->flags[i] |= F_OOB;
                else if (v == tmtype::DeepWater) in->flags[i] |= F_WATER;
            }
        }
    }
    in->spawns = job.spawns;
    in->flyers = job.flyers;
    in->vtol_cap = job.vtol_cap;
    if (raster_ms) *raster_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return in;
}

std::shared_ptr<const ReachResult> slim(ReachResult r) {
    // r.comp stays: the readout names a red cell's walk component to say why nothing gets there.
    r.links.clear();
    r.links.shrink_to_fit();
    return std::make_shared<const ReachResult>(std::move(r));
}

void publish(ReachState& S, const std::shared_ptr<Output>& o) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.latest = std::make_shared<const Output>(*o);
}

void run_job(ReachState& S, const std::shared_ptr<Job>& job) {
    const auto t0 = Clock::now();
    const std::atomic<bool>& cancel = *job->cancel;
    auto out = std::make_shared<Output>();
    out->gen = job->gen;
    double raster_ms = 0;
    std::shared_ptr<ReachInput> in = build_input(*job, &raster_ms);
    if (cancel.load()) return;
    out->input = in;
    out->raster_ms = raster_ms;
    const int unit = std::clamp(job->unit, 0, kReachUnitCount - 1);
    out->sel_profile = reach_profile(unit, job->driver);
    ReachResult r = reach_compute(*in, out->sel_profile, job->team, &cancel);
    if (cancel.load() || r.cancelled) return;
    out->sel = slim(std::move(r));
    out->total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    out->complete = !job->compare;
    publish(S, out);  // the chosen unit first: the tint can show now
    {
        out->other_profile = reach_profile(unit, job->driver == ReachDriver::Player ? ReachDriver::Bots : ReachDriver::Player);
        ReachResult x = reach_compute(*in, out->other_profile, job->team, &cancel);
        if (cancel.load() || x.cancelled) return;
        out->other = slim(std::move(x));
        if (!job->compare) publish(S, out);
    }
    if (!job->compare) return;
    const std::vector<GroupDef> defs = reach_groups(job->driver);
    const int sel_c10 = out->sel_profile.group == ReachGroup::JetMech ? int(std::lround(out->sel_profile.climb)) : 0;
    for (const GroupDef& g : defs) {
        if (cancel.load()) return;
        GroupRes gr;
        gr.name = g.name;
        gr.members = g.members;
        gr.profile = reach_profile(g.unit, job->driver);
        if (g.group == out->sel_profile.group && g.climb10 == sel_c10) {
            gr.res = out->sel;
        } else {
            ReachResult x = reach_compute(*in, gr.profile, job->team, &cancel);
            if (cancel.load() || x.cancelled) return;
            gr.res = slim(std::move(x));
        }
        out->groups.push_back(std::move(gr));
    }
    out->total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    out->complete = true;
    publish(S, out);
}

void reach_worker(ReachState* S) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // the editor stays smooth while the answer is made
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(S->mu);
            S->cv.wait(lk, [&] { return S->quit || S->pending; });
            if (S->quit) return;
            job = std::move(S->pending);
            S->pending.reset();
        }
        if (job && !job->cancel->load()) run_job(*S, job);
    }
}

// ---- colours and glyphs ----------------------------------------------------------------------------------------------------------------

constexpr uint32_t kGreenABGR = (110u << 24) | (150u << 16) | (236u << 8) | 40u;    // walks / drives / lands
constexpr uint32_t kAmberABGR = (122u << 24) | (0u << 16) | (176u << 8) | 255u;    // only with jets, claw or flight
constexpr uint32_t kRedABGR = (140u << 24) | (40u << 16) | (46u << 8) | 232u;      // never gets here
constexpr ImU32 kGreenCol = IM_COL32(70, 240, 150, 255), kAmberCol = IM_COL32(255, 178, 36, 255), kRedCol = IM_COL32(240, 70, 66, 255);

const char* class_name(int c) {
    switch (c) {
        case R_WALK: return "walk";
        case R_JET: return "jet";
        case R_LATCH: return "latch";
        case R_FLIGHT: return "flight";
        case R_NONE: return "none";
        case R_WALL: return "wall";
        case R_OOB: return "oob";
    }
    return "outside";
}

// kind: 0 = double arrow (up and down), 1 = single arrow to b (down only), 2 = X.
void draw_glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, int kind, ImU32 col, float dpi) {
    const float th = 2.2f * dpi;
    if (kind == 2) {
        const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float s = std::min(8.f * dpi, 0.5f * std::hypot(b.x - a.x, b.y - a.y));
        for (int pass = 0; pass < 2; pass++) {
            const ImU32 c = pass ? col : IM_COL32(0, 0, 0, 200);
            const float w = pass ? th : th + 2.f * dpi;
            dl->AddLine(ImVec2(m.x - s, m.y - s), ImVec2(m.x + s, m.y + s), c, w);
            dl->AddLine(ImVec2(m.x - s, m.y + s), ImVec2(m.x + s, m.y - s), c, w);
        }
        return;
    }
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 1.f) return;
    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len), n(-d.y, d.x);
    const float hs = std::min(6.5f * dpi, len * 0.45f);
    auto head = [&](ImVec2 tip, ImVec2 dir, ImU32 c, float grow) {
        const ImVec2 base(tip.x - dir.x * (hs + grow), tip.y - dir.y * (hs + grow));
        const float wv = (hs + grow) * 0.62f;
        dl->AddTriangleFilled(ImVec2(tip.x + dir.x * grow, tip.y + dir.y * grow), ImVec2(base.x + n.x * wv, base.y + n.y * wv), ImVec2(base.x - n.x * wv, base.y - n.y * wv), c);
    };
    for (int pass = 0; pass < 2; pass++) {
        const ImU32 c = pass ? col : IM_COL32(0, 0, 0, 200);
        const float w = pass ? th : th + 2.f * dpi, grow = pass ? 0.f : 1.2f * dpi;
        dl->AddLine(a, b, c, w);
        head(b, d, c, grow);
        if (kind == 0) head(a, ImVec2(-d.x, -d.y), c, grow);
    }
}

ImU32 class_col(int c) {
    if (c == R_WALK) return kGreenCol;
    if (c == R_JET || c == R_LATCH || c == R_FLIGHT) return kAmberCol;
    return kRedCol;
}

}  // namespace

// ---- App: settings helpers -------------------------------------------------------------------------------------------------------------

int App::reach_unit_index() const {
    int i = reach_find_unit(settings_.reach_unit.c_str());
    if (i < 0) i = reach_find_unit("Stiletto");
    return std::max(0, i);
}

ReachDriver App::reach_driver() const { return settings_.reach_driver == 1 ? ReachDriver::Bots : ReachDriver::Player; }

bool App::reach_script(const std::string& arg) {
    // "<unit label>,<red|blue>,<player|bots>[,compare|single]": the label may hold commas ("Pilot, no bomb"), so read from the right.
    std::vector<std::string> t;
    size_t from = 0;
    while (true) {
        const size_t c = arg.find(',', from);
        std::string s = arg.substr(from, c == std::string::npos ? std::string::npos : c - from);
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ') s.pop_back();
        t.push_back(s);
        if (c == std::string::npos) break;
        from = c + 1;
    }
    auto low = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    bool compare = true;
    if (t.size() > 1 && (low(t.back()) == "compare" || low(t.back()) == "single")) {
        compare = low(t.back()) == "compare";
        t.pop_back();
    }
    int team = settings_.reach_team, driver = settings_.reach_driver;
    if (t.size() >= 2 && (low(t.back()) == "player" || low(t.back()) == "bots")) {
        driver = low(t.back()) == "bots" ? 1 : 0;
        t.pop_back();
    }
    if (t.size() >= 2 && (low(t.back()) == "red" || low(t.back()) == "blue")) {
        team = low(t.back()) == "blue" ? 2 : 1;
        t.pop_back();
    }
    std::string unit;
    for (size_t i = 0; i < t.size(); i++) unit += (i ? "," : "") + t[i];
    int idx = -1;
    for (int i = 0; i < kReachUnitCount; i++)
        if (low(kReachUnits[i].label) == low(unit)) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < kReachUnitCount && !unit.empty(); i++)  // a prefix is enough ("Stil")
            if (low(kReachUnits[i].label).rfind(low(unit), 0) == 0) { idx = i; break; }
    if (idx < 0 && !unit.empty()) return false;
    if (idx >= 0) settings_.reach_unit = kReachUnits[idx].label;
    settings_.reach_team = team;
    settings_.reach_driver = driver;
    settings_.reach_compare = compare;
    settings_.reach_show = true;
    overlay_dirty_ = true;
    return true;
}

// ---- App: the per-frame driver (main thread) ----------------------------------------------------------------------------------------

namespace {
std::shared_ptr<const std::vector<float>> tris_of(ReachState& S, const GpuModel* m) {
    Cached& c = S.tri_cache[m];
    if (!c.tris || c.n != m->solid_tris.size()) {
        c.tris = std::make_shared<const std::vector<float>>(m->solid_tris);
        c.n = m->solid_tris.size();
    }
    return c.tris;
}
std::string lower_s(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

// Copies what the worker needs from the open scenario (the objects' solid triangles by model, placements, water, spawns, VTOLs).
void App::reach_collect() {
    ReachState& S = *reach_;
    S.coll_rev = objects_rev_;
    S.insts.clear();
    S.roofs.clear();
    S.waters.clear();
    S.spawns.clear();
    S.flyers.clear();
    S.vtol_cap = 55.f;
    S.models_ready = 0;
    if (!doc_) return;
    static const uint32_t K_MECH_ = key_of("MAMech"), K_TANK_ = key_of("MATank"), K_FLYER_ = key_of("MAFlyer"), K_VEHICLE_ = key_of("MAVehicle"),
                          K_PERSON_ = key_of("MAPerson"), K_TURRET_ = key_of("MATurret"), K_POWERUP_ = key_of("MAPowerUp"),
                          K_CRATE_ = key_of("MACrateGenerator"), K_EFFECT_ = key_of("MAEffect"), K_FLAG_ = key_of("MAFlagCTF"),
                          K_SNATCH_B = key_of("MASnatchItBase"), K_SNATCH_C = key_of("MASnatchItDataCore"), K_CORP_ = key_of("MACorporeal"),
                          K_BUILDING_ = key_of("MABuilding");
    auto skip_class = [&](uint32_t c) {
        return c == K_MECH_ || c == K_TANK_ || c == K_FLYER_ || c == K_VEHICLE_ || c == K_PERSON_ || c == K_TURRET_ || c == K_POWERUP_ ||
               c == K_CRATE_ || c == K_EFFECT_ || c == K_FLAG_ || c == K_SNATCH_B || c == K_SNATCH_C;
    };
    const Wdf& w = doc_->world;
    const Terrain& terr = doc_->terrain;
    uint64_t h = 0x51ed270b;
    for (const ObjView& v : views_) {
        if ((v.kind != 0 && v.kind != 5) || v.helper >= 0 || !v.model || v.index < 0 || size_t(v.index) >= w.objs.size()) continue;
        const GpuModel& m = *v.model;
        if (m.state != GpuModel::Ready || m.solid_tris.empty()) continue;
        const WObj& o = w.objs[size_t(v.index)];
        if (skip_class(o.cls)) continue;
        if (v.root >= 0 && size_t(v.root) < w.objs.size() && skip_class(w.objs[size_t(v.root)].cls)) continue;
        S.models_ready++;
        Inst I;
        BoxInstance mi;
        orient_instance(mi, v.heading, v.pitch, v.roll, v.pos);
        const float rows[12] = {mi.r0.x, mi.r0.y, mi.r0.z, mi.r0.w, mi.r1.x, mi.r1.y, mi.r1.z, mi.r1.w, mi.r2.x, mi.r2.y, mi.r2.z, mi.r2.w};
        std::memcpy(I.m, rows, sizeof rows);
        I.tris = tris_of(S, &m);
        bool tree = false;
        if (const TemplateInfo* ti = cat_ ? cat_->find(o.template_id()) : nullptr) {
            const std::string d = lower_s(o.description());
            tree = ti->category.find("Trees") != std::string::npos || d.find("tree") != std::string::npos || d.find("mangrove") != std::string::npos;
        }
        I.kind = tree ? IK_TREE : (o.cls == K_CORP_ || o.cls == K_BUILDING_) ? IK_LATCH : IK_PLAIN;
        h = mix64(h, reinterpret_cast<uintptr_t>(I.tris.get()));
        for (float f : I.m) h = hash_f(h, f);
        h = mix64(h, I.kind);
        // The outline of the top, from the model's solid bounds.
        if (!tree && m.has_sbox) {
            float wx[8], wy[8], wz[8];
            float ymax = -1e9f, ymin = 1e9f;
            for (int k = 0; k < 8; k++) {
                const float lx = (k & 1) ? m.smax[0] : m.smin[0], ly = (k & 4) ? m.smax[1] : m.smin[1], lz = (k & 2) ? m.smax[2] : m.smin[2];
                wx[k] = I.m[0] * lx + I.m[1] * ly + I.m[2] * lz + I.m[3];
                wy[k] = I.m[4] * lx + I.m[5] * ly + I.m[6] * lz + I.m[7];
                wz[k] = I.m[8] * lx + I.m[9] * ly + I.m[10] * lz + I.m[11];
                ymax = std::max(ymax, wy[k]);
                ymin = std::min(ymin, wy[k]);
            }
            const float cxm = (wx[0] + wx[7]) * 0.5f, czm = (wz[0] + wz[7]) * 0.5f;
            if (ymax - terr.height_at(cxm, czm) >= 3.f && ymax - ymin >= 2.f) {
                Roof R;
                const int order[4] = {0, 1, 3, 2};  // the bottom rectangle, in order
                for (int k = 0; k < 4; k++) { R.x[k] = wx[order[k]]; R.z[k] = wz[order[k]]; }
                R.y = ymax;
                const float rcell = std::max(2.f, std::max(terr.size_x(), terr.size_z()) / 1024.f);
                R.cell_x = int(std::floor(cxm / rcell));
                R.cell_z = int(std::floor(czm / rcell));
                S.roofs.push_back(R);
            }
        }
        S.insts.push_back(std::move(I));
    }
    for (const WObj& o : w.objs) {
        Vec3 p;
        if (o.cls == K_WATER) {
            BasinFoot f;
            if (water_foot(o, f)) {
                S.waters.push_back({f.cx, f.cz, f.w, f.l, f.heading, f.surface, f.oval});
                h = hash_f(hash_f(hash_f(hash_f(hash_f(h, f.cx), f.cz), f.w), f.l), f.surface + f.heading);
            }
        } else if (o.cls == K_LEVELSTART && get_pos(o, p)) {
            S.spawns.push_back({p.x, p.z, o.i32("TeamIndex")});
            h = hash_f(hash_f(mix64(h, uint64_t(o.i32("TeamIndex")) + 77), p.x), p.z);
        } else if (o.cls == key_of("MAFlyer") && get_pos(o, p) && (o.template_id() == 33119 || lower_s(o.description()).find("vtol") != std::string::npos)) {
            S.flyers.push_back({p.x, p.z, 0});
            if (S.flyers.size() == 1) S.vtol_cap = o.f32("MaxVTOLHeight", 55.f);
            h = hash_f(hash_f(hash_f(h, p.x), p.z), S.vtol_cap);
        }
    }
    S.world_hash = h;
}

void App::reach_frame(float dt) {
    (void)dt;
    const bool want = doc_ && settings_.reach_show;
    if (!reach_) {
        if (!want) return;
        reach_ = std::make_shared<ReachState>();
    }
    ReachState& S = *reach_;
    EmaTimer timer_(S.t_frame);
    if (want != reach_on_) {
        reach_on_ = want;
        overlay_dirty_ = true;
        if (!want) {
            if (S.cancel) S.cancel->store(true);
            S.cur.reset();
            S.submitted_key = S.seen_key = 0;
            S.readout.clear();
        }
    }
    if (!reach_on_) return;
    if (S.doc != doc_.get()) {  // another scenario: nothing carries over
        S.doc = doc_.get();
        if (S.cancel) S.cancel->store(true);
        S.tri_cache.clear();
        S.cur.reset();
        S.coll_rev = 0xFFFFFFFFu;
        S.submitted_key = S.seen_key = 0;
        overlay_dirty_ = true;
    }
    // Take the newest finished result (a short lock: the worker holds it only to swap pointers).
    {
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.latest && S.latest->gen == S.gen && S.latest != S.cur) {
            S.cur = S.latest;
            overlay_dirty_ = true;
        }
    }
    if (S.coll_rev != objects_rev_) reach_collect();
    // What the answer depends on: the ground, the type map, the objects, the spawns, and the picks in the panel.
    const Terrain& t = doc_->terrain;
    uint64_t key = hash_bytes(t.hgt.data(), t.hgt.size() * sizeof(float));
    key = hash_f(hash_f(hash_f(mix64(key, uint64_t(t.w) * 4099u + uint64_t(t.h)), t.scale), t.min_h), t.max_h);
    if (!doc_->typemap.empty()) key = hash_bytes(doc_->typemap.data(), doc_->typemap.size() * sizeof(uint16_t), key);
    key = mix64(key, S.world_hash);
    key = mix64(key, doc_->multiplayer ? 1u : 0u);
    key = mix64(key, uint64_t(reach_unit_index()) * 131u + uint64_t(settings_.reach_team) * 17u + uint64_t(settings_.reach_driver) * 5u +
                         (settings_.reach_compare ? 1u : 0u));
    const auto now = Clock::now();
    if (key != S.seen_key) {
        S.seen_key = key;
        S.dirty_since = now;
    }
    if (key == S.submitted_key) { S.waiting_gesture = false; return; }
    if (std::chrono::duration<double>(now - S.dirty_since).count() < kDebounceS) return;
    if (stroke_active_ || dragging_ || gizmo_drag_ || place_drag_ || water_drag_) { S.waiting_gesture = true; return; }
    S.waiting_gesture = false;
    // Submit a new job; the older one is told to stop.
    auto job = std::make_shared<Job>();
    job->gen = ++S.gen;
    if (S.cancel) S.cancel->store(true);
    S.cancel = std::make_shared<std::atomic<bool>>(false);
    job->cancel = S.cancel;
    job->terrain.w = t.w;
    job->terrain.h = t.h;
    job->terrain.scale = t.scale;
    job->terrain.min_h = t.min_h;
    job->terrain.max_h = t.max_h;
    job->terrain.hgt = t.hgt;
    job->typemap = doc_->typemap;
    job->tm_w = doc_->typemap_w;
    job->tm_h = doc_->typemap_h;
    job->multiplayer = doc_->multiplayer;
    job->insts = S.insts;
    job->waters = S.waters;
    job->spawns = S.spawns;
    job->flyers = S.flyers;
    job->vtol_cap = S.vtol_cap;
    job->unit = reach_unit_index();
    job->team = doc_->multiplayer ? (settings_.reach_team == 2 ? 2 : 1) : 0;  // a campaign map has one player start (team 0)
    job->driver = reach_driver();
    job->compare = settings_.reach_compare;
    S.submitted_key = key;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        S.pending = job;
    }
    if (!S.th.joinable()) S.th = std::thread(reach_worker, &S);
    S.cv.notify_one();
}

bool App::reach_computing() const {
    if (!reach_ || !reach_on_) return false;
    const ReachState& S = *reach_;
    if (S.seen_key != S.submitted_key || S.waiting_gesture) return true;
    return !S.cur || S.cur->gen != S.gen || !S.cur->complete;
}

// ---- App: the ground tint -------------------------------------------------------------------------------------------------------------

bool App::reach_paint(Image& img) const {
    if (!reach_ || !reach_on_ || !reach_->cur || !reach_->cur->sel) return false;
    EmaTimer timer_(reach_->t_paint);
    const ReachResult& r = *reach_->cur->sel;
    if (r.w <= 0 || r.h <= 0 || r.cls.size() != size_t(r.w) * size_t(r.h)) return false;
    img.w = r.w;
    img.h = r.h;
    img.px.assign(size_t(r.w) * size_t(r.h), 0u);
    for (int cz = 0; cz < r.h; cz++)
        for (int cx = 0; cx < r.w; cx++) {
            uint32_t c = 0;
            switch (r.cls[size_t(cz) * r.w + cx]) {
                case R_WALK: c = kGreenABGR; break;
                case R_JET:
                case R_LATCH:
                case R_FLIGHT: c = kAmberABGR; break;
                case R_NONE: c = ((cx + cz) & 1) ? 0u : kRedABGR; break;  // stippled: every other texel
                default: break;                                            // cliff faces, blocked ground, out of bounds: untinted
            }
            img.px[size_t(r.h - 1 - cz) * size_t(r.w) + size_t(cx)] = c;  // texture row 0 = the far edge (z max)
        }
    return true;
}

// ---- App: face arrows and roof outlines (from gizmo_draw, on the background list under the panels) -----------------------------------

void App::draw_reach_arrows() {
    if (!reach_on_ || !reach_ || !reach_->cur || !reach_->cur->sel || !reach_->cur->input) return;
    ReachState& S = *reach_;
    EmaTimer timer_(S.t_arrows);
    ReachTextScale tscale_(dpi_, reach_text_k(settings_.ui_scale_run > 0 ? settings_.ui_scale_run : settings_.ui_scale, base_dpi_, ImGui::GetIO().DisplaySize));
    // A crowded view (a dense retail map: arrows over every roof) thins itself out: wider glyph spacing, thin roof outlines, labels only
    // for the run under the mouse. The threshold grows with the screen so a big window is not thinned for nothing.
    const bool dense = float(S.glyphs_prev) > 70.f * std::max(1.f, (vp_w * vp_h) / (1280.f * 720.f * dpi_ * dpi_));
    int glyphs_now = 0;
    const ReachResult& r = *S.cur->sel;
    const ReachInput& in = *S.cur->input;
    if (r.w != in.w || r.cls.size() != in.count()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    const float cell = r.cell, m = 40.f * dpi_;
    auto inside = [&](ImVec2 p) { return p.x >= vp_x - m && p.y >= vp_y - m && p.x <= vp_x + vp_w + m && p.y <= vp_y + vp_h + m; };
    auto surf = [&](int i) {
        const float tp = in.top.empty() ? -1e9f : in.top[size_t(i)];
        return tp > in.th[size_t(i)] + 0.4f ? tp : in.th[size_t(i)];
    };
    S.roofs_drawn = 0;
    // Roofs: the top of an object the unit reaches (green / amber) or never reaches (red).
    for (const Roof& R : S.roofs) {
        if (R.cell_x < 0 || R.cell_z < 0 || R.cell_x >= r.w || R.cell_z >= r.h) continue;
        const size_t ci = size_t(R.cell_z) * size_t(r.w) + size_t(R.cell_x);
        const uint8_t c = r.cls[ci];
        if (c == R_WALL || c == R_OOB) continue;
        if (!in.top.empty() && in.top[ci] < R.y - 2.5f) continue;  // the middle of the bounds is not under the roof (an arch, a court)
        if (!in.flags.empty() && (in.flags[ci] & F_BREAKABLE)) continue;
        ImVec2 q[4];
        bool ok = true, any = false;
        for (int k = 0; k < 4 && ok; k++) {
            float sx, sy;
            ok = cam_.project({R.x[k], R.y + 0.25f, R.z[k]}, vp_w, vp_h, sx, sy);
            q[k] = ImVec2(sx + vp_x, sy + vp_y);
            any = any || inside(q[k]);
        }
        if (!ok || !any) continue;
        S.roofs_drawn++;
        if (dense) {
            dl->AddPolyline(q, 4, (class_col(c) & 0x00FFFFFFu) | 0x90000000u, ImDrawFlags_Closed, 1.4f * dpi_);
        } else {
            dl->AddPolyline(q, 4, IM_COL32(0, 0, 0, 190), ImDrawFlags_Closed, 4.f * dpi_);
            dl->AddPolyline(q, 4, class_col(c), ImDrawFlags_Closed, 2.2f * dpi_);
        }
    }
    // Faces: contiguous faces (one side of one cliff, same verdict, similar height) are one run: one glyph per ~28 m of it, one height label
    // for the run, no glyph over another, no label over another. A glyph under about 6 px (zoomed far out) is skipped.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float best_d = 16.f * dpi_;
    int best_run = -1;
    ImVec2 best_a{};
    if (S.runs_for != S.cur.get()) {
        S.runs_for = S.cur.get();
        S.runs = build_runs(r);
    }
    const float gcell = (dense ? 44.f : 24.f) * dpi_;
    const int gw = int(vp_w / gcell) + 3, gh2 = int(vp_h / gcell) + 3;
    std::vector<uint8_t> taken(size_t(gw) * size_t(gh2), 0);
    std::vector<ImVec4> labels;  // rects of the height labels drawn
    std::vector<ImVec4> glyph_boxes;  // rects of the glyphs drawn (a label keeps off the other runs' glyphs)
    const bool latch_unit = S.cur->sel_profile.latch;
    (void)latch_unit;
    for (size_t ri = 0; ri < S.runs.size(); ri++) {
        const Run& run = S.runs[ri];
        const int nf = int(run.faces.size());
        const int ng = std::clamp(int(std::lround(run.len_m / 28.f)), 1, nf);
        ImVec2 label_at{};
        const size_t own_first = glyph_boxes.size();
        float label_len = 0, label_L = 0;
        bool have_label = false;
        for (int k = 0; k < ng; k++) {
            const int pick = std::clamp(int((float(k) + 0.5f) * float(nf) / float(ng)), 0, nf - 1);
            const ReachFace& f = r.faces[run.faces[size_t(pick)]];
            if (f.top < 0 || f.bot < 0 || f.top >= int(in.count()) || f.bot >= int(in.count())) continue;
            const float tx = (float(f.top % r.w) + 0.5f) * cell, tz = (float(f.top / r.w) + 0.5f) * cell;
            const float bx = (float(f.bot % r.w) + 0.5f) * cell, bz = (float(f.bot / r.w) + 0.5f) * cell;
            const float by = surf(f.bot), ty = by + f.rise;
            float ax, ay, bx2, by2;
            if (!cam_.project({tx, ty + 0.4f, tz}, vp_w, vp_h, ax, ay) || !cam_.project({bx, by + 0.4f, bz}, vp_w, vp_h, bx2, by2)) continue;
            const ImVec2 a(ax + vp_x, ay + vp_y), b(bx2 + vp_x, by2 + vp_y);
            const float len = std::hypot(b.x - a.x, b.y - a.y);
            if (len < 6.f * dpi_) continue;
            const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            if (!inside(mid)) continue;
            // A glyph is at least 18 px and at most 60 px long, along the face (top to bottom), through its middle.
            const float L = std::clamp(len, 18.f * dpi_, 60.f * dpi_);
            const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len);
            const ImVec2 p0(mid.x - d.x * L * 0.5f, mid.y - d.y * L * 0.5f), p1(mid.x + d.x * L * 0.5f, mid.y + d.y * L * 0.5f);
            {  // the grid squares under the glyph's box must all be free
                const float pad = 4.f * dpi_;
                const int x0 = int((std::min(p0.x, p1.x) - pad - vp_x) / gcell) + 1, x1 = int((std::max(p0.x, p1.x) + pad - vp_x) / gcell) + 1;
                const int z0 = int((std::min(p0.y, p1.y) - pad - vp_y) / gcell) + 1, z1 = int((std::max(p0.y, p1.y) + pad - vp_y) / gcell) + 1;
                if (x0 < 0 || z0 < 0 || x1 >= gw || z1 >= gh2) continue;
                bool free_box = true;
                for (int zz = z0; zz <= z1 && free_box; zz++)
                    for (int xx = x0; xx <= x1; xx++)
                        if (taken[size_t(zz) * size_t(gw) + size_t(xx)]) { free_box = false; break; }
                if (!free_box) continue;
                for (int zz = z0; zz <= z1; zz++)
                    for (int xx = x0; xx <= x1; xx++) taken[size_t(zz) * size_t(gw) + size_t(xx)] = 1;
            }
            glyphs_now++;
            glyph_boxes.push_back(ImVec4(std::min(p0.x, p1.x) - 3.f * dpi_, std::min(p0.y, p1.y) - 3.f * dpi_, std::max(p0.x, p1.x) + 3.f * dpi_, std::max(p0.y, p1.y) + 3.f * dpi_));
            const bool up = f.up != R_NONE, down = f.down != R_NONE;
            if (up && down) draw_glyph(dl, p0, p1, 0, kGreenCol, dpi_);
            else if (down) draw_glyph(dl, p0, p1, 1, kAmberCol, dpi_);
            else if (up) draw_glyph(dl, p1, p0, 1, kGreenCol, dpi_);  // up only (a jet leg or the claw): the arrowhead at the top
            else draw_glyph(dl, p0, p1, 2, kRedCol, dpi_);
            if (!have_label || std::fabs(float(k) + 0.5f - 0.5f * float(ng)) < std::fabs(label_len)) {  // the glyph nearest the run's middle
                label_at = mid;
                label_len = float(k) + 0.5f - 0.5f * float(ng);
                label_L = L;
                have_label = true;
                if (len < 46.f * dpi_) label_L = 0;  // too small on screen: no label (the hover shows it)
            }
            const float dm = std::hypot(mouse.x - mid.x, mouse.y - mid.y);
            if (dm < best_d) {
                best_d = dm;
                best_run = int(ri);
                best_a = mid;
            }
        }
        // One height label for the run ("30-34 m"), only when zoomed in, and never over another label.
        if (have_label && label_L >= 40.f * dpi_ && !dense) {
            char b2[32];
            const int lo = int(std::lround(run.rmin)), hi = int(std::lround(run.rmax));
            if (lo == hi) std::snprintf(b2, sizeof b2, "%d m", lo);
            else std::snprintf(b2, sizeof b2, "%d-%d m", lo, hi);
            const ImVec2 ts = ImGui::CalcTextSize(b2);
            const ImVec4 rc(label_at.x + 10.f * dpi_, label_at.y - ts.y * 0.5f - 1.f, label_at.x + 12.f * dpi_ + ts.x, label_at.y + ts.y * 0.5f + 1.f);
            bool clash = false;
            for (const ImVec4& o : labels)
                if (rc.x < o.z && rc.z > o.x && rc.y < o.w && rc.w > o.y) { clash = true; break; }
            for (size_t gi = 0; gi < glyph_boxes.size() && !clash; gi++) {
                const ImVec4& o = glyph_boxes[gi];
                if (gi >= own_first && gi < own_first + size_t(ng)) continue;  // its own run's glyphs
                if (rc.x < o.z && rc.z > o.x && rc.y < o.w && rc.w > o.y) clash = true;
            }
            if (!clash) {
                labels.push_back(rc);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(label_at.x + 11.f * dpi_ + 1, label_at.y - ts.y * 0.5f + 1), IM_COL32(0, 0, 0, 220), b2);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(label_at.x + 11.f * dpi_, label_at.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 245), b2);
                if (best_run == int(ri)) best_run = -1;  // already labelled
            }
        }
    }
    if (best_run >= 0 && vp_hover_) {  // the run under the mouse shows its height when it has no label
        const Run& run = S.runs[size_t(best_run)];
        char b2[32];
        const int lo = int(std::lround(run.rmin)), hi = int(std::lround(run.rmax));
        if (lo == hi) std::snprintf(b2, sizeof b2, "%d m", lo);
        else std::snprintf(b2, sizeof b2, "%d-%d m", lo, hi);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(best_a.x + 9.f * dpi_ + 1, best_a.y - 6.f * dpi_ + 1), IM_COL32(0, 0, 0, 220), b2);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(best_a.x + 9.f * dpi_, best_a.y - 6.f * dpi_), IM_COL32(255, 255, 255, 245), b2);
    }
    S.glyphs_prev = glyphs_now;
    dl->PopClipRect();
}

// ---- App: the hover readout --------------------------------------------------------------------------------------------------------------

namespace {
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t from = 0;
    while (from <= s.size()) {
        const size_t nl = s.find('\n', from);
        out.push_back(s.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        if (nl == std::string::npos) break;
        from = nl + 1;
    }
    return out;
}

// The "Here?" cell colour of the compare table: the legend colours, grey where nobody can stand.
ImU32 cell_col(const ReachAnswer& a, const ReachProfile& p) {
    if (a.outside) return IM_COL32(60, 60, 60, 255);
    switch (a.cls) {
        case R_WALK: return IM_COL32(28, 130, 78, 255);
        case R_JET:
        case R_LATCH:
        case R_FLIGHT: return IM_COL32(168, 112, 8, 255);
        case R_NONE: return p.immobile ? IM_COL32(60, 60, 60, 255) : IM_COL32(158, 42, 40, 255);
        default: return IM_COL32(60, 60, 60, 255);
    }
}
}  // namespace

// The readout lines at a world point for the chosen unit: the verdict for the cell, then the nearest cliffs.
std::vector<std::string> App::reach_lines(float x, float z) const {
    std::vector<std::string> lines;
    if (!reach_ || !reach_->cur || !reach_->cur->sel || !reach_->cur->input) {
        lines.push_back("Can it get here? Working out the answer...");
        return lines;
    }
    const Output& o = *reach_->cur;
    return split_lines(reach_explain(*o.input, *o.sel, o.sel_profile, x, z));
}

void App::draw_reach_readout(const Vec3& g, float y_extra) {
    if (!reach_on_ || !reach_) return;
    ReachState& S = *reach_;
    EmaTimer timer_(S.t_readout);
    ReachTextScale tscale_(dpi_, reach_text_k(settings_.ui_scale_run > 0 ? settings_.ui_scale_run : settings_.ui_scale, base_dpi_, ImGui::GetIO().DisplaySize));
    std::vector<std::string> lines = reach_lines(g.x, g.z);
    if (S.cur && S.cur->sel && S.cur->other && S.cur->input && lines.size() > 0) {  // you and the bots do not get the same answer here
        const Output& o = *S.cur;
        const bool bots_sel = o.sel_profile.driver == ReachDriver::Bots;
        const ReachAnswer pl = reach_answer(*o.input, bots_sel ? *o.other : *o.sel, bots_sel ? o.other_profile : o.sel_profile, g.x, g.z);
        const ReachAnswer bt = reach_answer(*o.input, bots_sel ? *o.sel : *o.other, bots_sel ? o.sel_profile : o.other_profile, g.x, g.z);
        if (!pl.outside && pl.cls != bt.cls && !o.sel_profile.immobile) {
            auto way = [&](const ReachAnswer& a, bool bots) -> std::string {
                switch (a.cls) {
                    case R_WALK: return o.sel_profile.flies ? "land here" : (o.sel_profile.group == ReachGroup::Tank ? "drive here" : "walk here");
                    case R_JET: return "get here with jets only";
                    case R_LATCH: return "get here by claw-climbing";
                    case R_FLIGHT: return o.sel_profile.flies ? "only fly over" : "get here by VTOL ride";
                    default: return bots ? "can't plan a way here" : "can't get here";
                }
            };
            const bool pl_in = pl.cls == R_WALK || pl.cls == R_JET || pl.cls == R_LATCH || pl.cls == R_FLIGHT;
            const bool bt_in = bt.cls == R_WALK || bt.cls == R_JET || bt.cls == R_LATCH || bt.cls == R_FLIGHT;
            std::string s = "You and the bots differ here: you ";
            s += pl_in ? "can " : "";
            s += way(pl, false) + "; bots " + (bt_in ? "can " : "") + way(bt, true) + ".";
            lines.insert(lines.begin() + 1, s);
        }
    }
    // "Compare all": one table row per movement group.
    struct Row {
        std::string unit;
        ReachAnswer a;
        ReachProfile p;
        bool sel = false;
    };
    std::vector<Row> rows;
    bool working = false;
    if (settings_.reach_compare && S.cur && S.cur->sel && S.cur->input) {
        const Output& o = *S.cur;
        const int sel_c10 = o.sel_profile.group == ReachGroup::JetMech ? int(std::lround(o.sel_profile.climb)) : 0;
        for (const GroupRes& gr : o.groups) {
            Row r;
            r.unit = group_label(gr);
            r.p = gr.profile;
            r.p.driver = ReachDriver::Player;  // the top line already says "bots" once
            r.a = reach_answer(*o.input, *gr.res, r.p, g.x, g.z);
            const int c10 = gr.profile.group == ReachGroup::JetMech ? int(std::lround(gr.profile.climb)) : 0;
            r.sel = gr.profile.group == o.sel_profile.group && c10 == sel_c10;
            rows.push_back(std::move(r));
        }
        working = !o.complete;
    }
    S.readout.clear();
    for (size_t i = 0; i < lines.size(); i++) S.readout += (i ? "\n" : "") + lines[i];
    for (const Row& r : rows) S.readout += "\n| " + r.unit + " | " + r.a.here + " | " + r.a.how + " | " + r.a.cliff;

    static ImVec2 last_size(0, 0);  // last frame's size: keeps the box on the screen (a window only knows its size after it is laid out)
    const ImGuiIO& io = ImGui::GetIO();
    float x = io.MousePos.x + 18.f * dpi_, y = io.MousePos.y + 14.f * dpi_ + y_extra;
    if (x + last_size.x > io.DisplaySize.x - 4.f) x = io.MousePos.x - 18.f * dpi_ - last_size.x;
    if (y + last_size.y > io.DisplaySize.y - 4.f) y = io.MousePos.y - 14.f * dpi_ - last_size.y;
    x = std::clamp(x, 4.f, std::max(4.f, io.DisplaySize.x - last_size.x - 4.f));
    y = std::clamp(y, 4.f, std::max(4.f, io.DisplaySize.y - last_size.y - 4.f));
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##reachtip", nullptr, fl)) {
        const float wrap_w = std::min(480.f * dpi_, io.DisplaySize.x * 0.45f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
        for (size_t i = 0; i < lines.size(); i++) {
            if (i == 0) ImGui::TextUnformatted(lines[i].c_str());
            else ImGui::TextColored(ImVec4(0.80f, 0.84f, 0.87f, 1.f), "%s", lines[i].c_str());
        }
        ImGui::PopTextWrapPos();
        if (!rows.empty()) {
            ImGui::Spacing();
            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
            if (ImGui::BeginTable("##reachcmp", 4, tf)) {
                ImGui::TableSetupColumn("Unit(s)", ImGuiTableColumnFlags_WidthFixed, 235.f * dpi_);
                ImGui::TableSetupColumn("Here?", ImGuiTableColumnFlags_WidthFixed, 74.f * dpi_);
                ImGui::TableSetupColumn("How", ImGuiTableColumnFlags_WidthFixed, 128.f * dpi_);
                ImGui::TableSetupColumn("Nearest cliff", ImGuiTableColumnFlags_WidthFixed, 140.f * dpi_);
                ImGui::TableHeadersRow();
                for (const Row& r : rows) {
                    ImGui::TableNextRow();
                    if (r.sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(255, 255, 255, 30));
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextUnformatted(r.unit.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_col(r.a, r.p));
                    ImGui::TextUnformatted(r.a.here.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextUnformatted(r.a.how.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::TableSetColumnIndex(3);
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextUnformatted(r.a.cliff.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndTable();
            }
            if (working) ImGui::TextDisabled("Still working on the other units...");
        } else if (settings_.reach_compare && working) {
            ImGui::TextDisabled("Still working on the other units...");
        }
        last_size = ImGui::GetWindowSize();
    }
    ImGui::End();
}

// ---- App: the floating panel -------------------------------------------------------------------------------------------------------------

namespace {
void legend_row(ImDrawList* dl, uint32_t col, const char* text, float dpi) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h * 1.6f, p.y + h - 2), col);
    ImGui::Dummy(ImVec2(h * 1.6f, h));
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
    (void)dpi;
}
void glyph_row(ImDrawList* dl, int kind, ImU32 col, const char* text, float dpi) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight(), w = h * 1.6f;
    draw_glyph(dl, ImVec2(p.x + 2, p.y + h * 0.5f), ImVec2(p.x + w - 2, p.y + h * 0.5f), kind, col, dpi * 0.8f);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
}
}  // namespace

void App::ui_reach_panel() {
    if (!doc_ || !reach_on_) return;
    ReachTextScale tscale_(dpi_, reach_text_k(settings_.ui_scale_run > 0 ? settings_.ui_scale_run : settings_.ui_scale, base_dpi_, ImGui::GetIO().DisplaySize));
    bool open = true;
    ImGui::SetNextWindowPos(ImVec2(vp_x + 12.f * dpi_, vp_y + 12.f * dpi_), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    char title[160];
    {
        const ReachUnit& tu = kReachUnits[reach_unit_index()];
        std::snprintf(title, sizeof title, "Can it get here?  %s%s###reach", tu.label, doc_->multiplayer ? (settings_.reach_team == 2 ? " (Blue)" : " (Red)") : "");
    }
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        const float wrap = 330.f * dpi_;
        auto muted = [&](const std::string& s) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
            ImGui::TextUnformatted(s.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        };
        const int cur = reach_unit_index();
        const ReachUnit& cu = kReachUnits[cur];
        auto unit_text = [](const ReachUnit& u) {
            char b[96];
            if (u.group == ReachGroup::JetMech) std::snprintf(b, sizeof b, "%s (jets ~%.0f m)", u.label, double(u.climb));
            else std::snprintf(b, sizeof b, "%s", u.label);
            return std::string(b);
        };
        ImGui::TextUnformatted("Unit");
        if (reach_computing()) {  // visible even with the key folded
            ImGui::SameLine();
            ImGui::TextDisabled("working...");
        }
        ImGui::SetNextItemWidth(wrap);
        if (ImGui::BeginCombo("##reachunit", unit_text(cu).c_str(), ImGuiComboFlags_HeightLargest)) {
            ReachGroup last = ReachGroup::Turret;
            bool first = true;
            static char find[48] = "";
            if (ImGui::IsWindowAppearing()) { find[0] = 0; ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##reachfind", "Type to find a unit", find, sizeof find);
            auto lower = [](std::string t) { for (char& c : t) c = char(std::tolower((unsigned char)c)); return t; };
            const std::string want = lower(find);
            for (int i = 0; i < kReachUnitCount; i++) {
                const ReachUnit& u = kReachUnits[i];
                if (!want.empty() && lower(unit_text(u)).find(want) == std::string::npos && lower(reach_group_name(u.group)).find(want) == std::string::npos) continue;
                if (first || u.group != last) {
                    ImGui::SeparatorText(reach_group_name(u.group));
                    last = u.group;
                    first = false;
                }
                const bool sel = i == cur;
                if (ImGui::Selectable((unit_text(u) + "##u" + std::to_string(i)).c_str(), sel)) {
                    settings_.reach_unit = u.label;
                    settings_.save();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Which unit to ask about. The map is tinted for this one; the readout under the mouse can compare them all.");
        ImGui::TextUnformatted("Team");
        ImGui::SameLine();
        if (doc_->multiplayer) {
            if (ImGui::RadioButton("Red", settings_.reach_team != 2)) { settings_.reach_team = 1; settings_.save(); }
            ImGui::SameLine();
            if (ImGui::RadioButton("Blue", settings_.reach_team == 2)) { settings_.reach_team = 2; settings_.save(); }
            ImGui::SetItemTooltip("Where the unit starts: this team's start points (Units and Props > Starts).");
        } else {
            ImGui::TextDisabled("starts at the player's start point");
        }
        ImGui::TextUnformatted("Who drives");
        ImGui::SameLine();
        if (ImGui::RadioButton("Player", settings_.reach_driver != 1)) { settings_.reach_driver = 0; settings_.save(); }
        ImGui::SetItemTooltip("What a person at the controls can do.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Bots", settings_.reach_driver == 1)) { settings_.reach_driver = 1; settings_.save(); }
        ImGui::SetItemTooltip("What the bots in a bot match will plan. Never more than the player can do: bot 'Mechs stop at gentler slopes and jump less far than a player.");
        if (ImGui::Checkbox("Compare all", &settings_.reach_compare)) settings_.save();
        ImGui::SetItemTooltip("The readout under the mouse also says, for each kind of unit, whether it gets to that spot.");
        ImGui::Separator();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool vtol = cu.group == ReachGroup::Vtol;
        char b0[220];
        static int key_open = -1;  // the key starts open on a tall window, folded on a short one; the user's choice then sticks for this run
        if (key_open < 0) key_open = vp_h >= 700.f * dpi_ ? 1 : 0;
        ImGui::SetNextItemOpen(key_open == 1, ImGuiCond_Always);
        const bool key_shown = ImGui::TreeNodeEx("What the colours and arrows mean##reachkey", ImGuiTreeNodeFlags_SpanAvailWidth);
        key_open = key_shown ? 1 : 0;
        if (key_shown) {
        legend_row(dl, kGreenABGR | 0xFF000000u, vtol ? "Can land here" : cu.group == ReachGroup::Turret ? "Where it sits" : "Walks or drives here", dpi_);
        legend_row(dl, kAmberABGR | 0xFF000000u, vtol ? "Can fly over, can't land" : "Only with jets, claws or a VTOL ride", dpi_);
        legend_row(dl, kRedABGR | 0xFF000000u, vtol ? "Can't fly here" : "Stands on it, never gets here (dotted)", dpi_);
        if (!vtol) {
            glyph_row(dl, 0, kGreenCol, "Cliff: up and down", dpi_);
            glyph_row(dl, 1, kAmberCol, "Cliff: down only (arrow points down the drop)", dpi_);
            glyph_row(dl, 2, kRedCol, "Cliff: no way either way", dpi_);
            muted("No tint: cliff faces and ground nobody can stand on.");
            if (reach_profile(cur, reach_driver()).terrain_slope > 0.f) {
                std::snprintf(b0, sizeof b0, "Terrain faces up to %.0f%% are just ground to walk (no arrow): arrows mark steeper ground and object faces (roofs, walls).",
                              double(reach_profile(cur, reach_driver()).terrain_slope) * 100.0);
                muted(b0);
            }
        }
            ImGui::TreePop();
        }
        ImGui::Separator();
        const bool loading = models_.pending() > 0;
        if (loading) muted("Loading models... objects still loading are not counted yet.");
        if (reach_ && reach_->cur && reach_->cur->sel) {
            const ReachResult& r = *reach_->cur->sel;
            if (vtol) {
                if (r.spawn_total == 0 && reach_->flyers.empty()) muted("No start point and no VTOL placed: nothing to fly from.");
                else if (r.spawn_total > 0 && r.spawn_ok < r.spawn_total) muted("A start point has no clear air for a VTOL (out of bounds or under its ceiling).");
            } else if (cu.group != ReachGroup::Turret && r.spawn_total == 0) {
                muted("This team has no start point, so nothing can be reached.");
            } else if (cu.group != ReachGroup::Turret && r.spawn_total > 0 && r.spawn_ok < r.spawn_total) {
                muted("A start point stands on ground this unit can't use.");
            }
        }
        // One line for each estimated rule that decides this unit's answer.
        const ReachProfile p = reach_profile(cur, reach_driver());
        char b[200];
        if (p.est & E_STEP) {
            if (p.terrain_slope > 0.f) std::snprintf(b, sizeof b, "Terrain slopes up to %.0f%% are measured. Object faces (roofs, wall tops) are an estimate (about %.0f%%).", double(p.terrain_slope) * 100.0, double(p.step) * 50.0);
            else std::snprintf(b, sizeof b, "The steepest slope it can walk is an estimate (about %.0f%%).", double(p.step) * 50.0);
            muted(b);
        }
        if (p.est & E_DROP) {
            std::snprintf(b, sizeof b, "How far it can step off a ledge is an estimate (about %.0f m).", double(p.drop));
            muted(b);
        }
        if (p.est & E_CLIMB) {
            std::snprintf(b, sizeof b, "How high its jets lift it is an estimate (about %.0f m).", double(p.climb));
            muted(b);
        }
        if (p.est & E_LATCH) muted("Claw-climbing is an estimate: a building may not allow it.");
        if (p.est & E_VTOL) muted("VTOL clearance and landing slope are estimates.");
        if (reach_driver() == ReachDriver::Bots) muted("Bots: what the bot planner will route; never more than a player can do.");
    }
    ImGui::End();
    if (!open) { settings_.reach_show = false; settings_.save(); overlay_dirty_ = true; }
}

// ---- App: automation -----------------------------------------------------------------------------------------------------------------------

Json App::reach_state_json() const {
    Json j = Json::object();
    j.with("on", Json::boolean(reach_on_));
    j.with("unit", Json::string(settings_.reach_unit)).with("team", Json::string(settings_.reach_team == 2 ? "blue" : "red"));
    j.with("driver", Json::string(settings_.reach_driver == 1 ? "bots" : "player")).with("compare", Json::boolean(settings_.reach_compare));
    j.with("computing", Json::boolean(reach_computing()));
    j.with("generation", Json::number(reach_ ? double(reach_->gen) : 0.0));
    j.with("shown_generation", Json::number(reach_ && reach_->cur ? double(reach_->cur->gen) : 0.0));
    j.with("models_pending", Json::number(double(models_.pending())));
    if (reach_ && reach_->cur && reach_->cur->sel) {
        const ReachResult& r = *reach_->cur->sel;
        j.with("counts", Json::object()
                             .with("walk", Json::number(r.counts[R_WALK])).with("jet", Json::number(r.counts[R_JET])).with("latch", Json::number(r.counts[R_LATCH]))
                             .with("flight", Json::number(r.counts[R_FLIGHT])).with("none", Json::number(r.counts[R_NONE])).with("wall", Json::number(r.counts[R_WALL]))
                             .with("oob", Json::number(r.counts[R_OOB])));
        j.with("roofs", Json::object().with("outlines", Json::number(double(reach_->roofs.size()))).with("drawn", Json::number(reach_->roofs_drawn)));
        j.with("faces", Json::number(double(r.faces.size()))).with("groups", Json::number(double(reach_->cur->groups.size())));
        j.with("cells", Json::object().with("w", Json::number(r.w)).with("h", Json::number(r.h)).with("size_m", Json::number(r.cell)));
        j.with("spawns", Json::object().with("total", Json::number(r.spawn_total)).with("usable", Json::number(r.spawn_ok)));
        j.with("ms", Json::object().with("rasterise", Json::number(std::round(reach_->cur->raster_ms * 10.0) / 10.0))
                                  .with("all", Json::number(std::round(reach_->cur->total_ms * 10.0) / 10.0)).with("selected", Json::number(std::round(r.ms * 10.0) / 10.0)));
        j.with("complete", Json::boolean(reach_->cur->complete));
        j.with("main_ms", Json::object().with("frame", Json::number(std::round(reach_->t_frame * 1000.0) / 1000.0)).with("arrows", Json::number(std::round(reach_->t_arrows * 1000.0) / 1000.0))
                                       .with("paint", Json::number(std::round(reach_->t_paint * 1000.0) / 1000.0)).with("readout", Json::number(std::round(reach_->t_readout * 1000.0) / 1000.0)));
    } else {
        j.with("counts", Json::object().with("walk", Json::number(0)).with("jet", Json::number(0)).with("latch", Json::number(0)).with("flight", Json::number(0)).with("none", Json::number(0)));
    }
    j.with("readout", Json::string(reach_ ? reach_->readout : std::string()));
    return j;
}

std::string App::automation_reach(float x, float z) const {
    Json out = Json::object();
    if (!doc_) return out.with("error", Json::string("no scenario is open")).dump();
    if (!reach_on_ || !reach_ || !reach_->cur || !reach_->cur->sel || !reach_->cur->input)
        return out.with("error", Json::string(reach_on_ ? "no answer yet: wait until state.overlay.reach.computing is false" : "the overlay is off: View > Can it get here? (or the --reach flag)"))
            .with("computing", Json::boolean(reach_computing())).dump();
    const Output& o = *reach_->cur;
    const ReachResult& r = *o.sel;
    auto cls_at = [&](const ReachResult& rr) {
        const int cx = int(std::floor(x / rr.cell)), cz = int(std::floor(z / rr.cell));
        if (cx < 0 || cz < 0 || cx >= rr.w || cz >= rr.h) return "outside";
        return class_name(rr.cls[size_t(cz) * rr.w + cx]);
    };
    out.with("x", Json::number(std::round(x * 100.f) / 100.0)).with("z", Json::number(std::round(z * 100.f) / 100.0));
    out.with("unit", Json::string(settings_.reach_unit)).with("team", Json::string(settings_.reach_team == 2 ? "blue" : "red"));
    out.with("driver", Json::string(settings_.reach_driver == 1 ? "bots" : "player"));
    out.with("class", Json::string(cls_at(r)));
    out.with("explain", Json::string(reach_explain(*o.input, r, o.sel_profile, x, z)));
    out.with("computing", Json::boolean(reach_computing())).with("complete", Json::boolean(o.complete));
    // Heights: the ground, the highest solid object top (if any) at that cell.
    const int cx = int(std::floor(x / r.cell)), cz = int(std::floor(z / r.cell));
    if (cx >= 0 && cz >= 0 && cx < o.input->w && cz < o.input->h) {
        const size_t i = size_t(cz) * size_t(o.input->w) + size_t(cx);
        out.with("ground", Json::number(std::round(o.input->th[i] * 100.f) / 100.0));
        if (!o.input->top.empty() && o.input->top[i] > -1e8f) out.with("top", Json::number(std::round(o.input->top[i] * 100.f) / 100.0));
    }
    Json groups = Json::array();
    for (const GroupRes& g : o.groups) {
        ReachUnit label_unit = *g.profile.unit;
        const std::string name = g.name + " (" + short_members(g.members) + ")";
        label_unit.label = name.c_str();
        ReachProfile p = g.profile;
        p.unit = &label_unit;
        p.driver = ReachDriver::Player;
        Json row = Json::object();
        row.with("name", Json::string(g.name)).with("members", Json::string(g.members)).with("class", Json::string(cls_at(*g.res)));
        row.with("explain", Json::string(reach_explain(*o.input, *g.res, p, x, z)));
        groups.push(std::move(row));
    }
    out.with("groups", groups);
    return out.dump();
}

}  // namespace ed
