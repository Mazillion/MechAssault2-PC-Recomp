// The scatter brush: count by the area a stroke covers, spacing by model size, keep off roads / bases / buildings
// (never water), lean and follow the slope, Ctrl erases the brush set's kinds. One stroke = one undo step; the selection is never touched.
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include "app.h"
#include "../core/triggers.h"
#include "imgui.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

// The footprint radius of a template's model (cached once the model is loaded; 2.5 m while it is still loading, 1 m without a model).
float App::scatter_radius(uint32_t tpl) {
    auto it = scatter_r_.find(tpl);
    if (it != scatter_r_.end()) return it->second;
    const TemplateInfo* ti = cat_ ? cat_->find(tpl) : nullptr;
    if (!ti || ti->model.empty()) return scatter_r_[tpl] = 1.f;
    GpuModel* gm = models_.get(ti->model, ti->mtb);
    if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
    if (!gm || gm->state == GpuModel::Pending) return 2.5f;
    if (gm->state == GpuModel::Failed) return scatter_r_[tpl] = 1.f;
    return scatter_r_[tpl] = footprint_radius(gm->bmin, gm->bmax);
}

// Stroke start: what the stroke needs to know about the map (roots to keep apart from, areas to keep off, the sheets objects may stand on),
// or, for an erase stroke, the roots of the brush set's kinds.
void App::scatter_begin(bool erase) {
    sc_run_ = std::make_unique<ScatterRun>();
    ScatterRun& r = *sc_run_;
    r.erase = erase;
    r.accum = 0.5f;   // the expected count rounds to the nearest object
    WTree t;
    t.build(doc_->world);
    auto positioned_root = [&](size_t i, Vec3& p) {
        const WObj& o = doc_->world.objs[i];
        if (!get_pos(o, p)) return false;
        Vec3 q;
        for (int a = t.parent_of[i]; a >= 0; a = t.parent_of[size_t(a)])
            if (get_pos(doc_->world.objs[size_t(a)], q)) return false;   // a part follows the object it belongs to
        return true;
    };
    if (erase) {
        const std::set<uint32_t> kinds(scatter_set_.begin(), scatter_set_.end());
        for (size_t i = 0; i < doc_->world.objs.size(); i++) {
            const WObj& o = doc_->world.objs[i];
            Vec3 p;
            if (!kinds.count(o.template_id()) || !positioned_root(i, p)) continue;
            r.cand.push_back({o.uid(), p.x, p.z});
        }
        return;
    }
    for (uint32_t u : scatter_set_) scatter_radius(u);   // loads the models of the set before the first dab
    SheetSet ss = sheets(false);
    r.sheets = std::move(ss.s);
    r.surf = std::make_unique<place::SurfaceIndex>(r.sheets);
    const bool keep_off = settings_.sc_keep_off;
    auto add_keep = [&](const place::Sheet& s) {
        ScatterKeep k;
        k.s = s;
        float x[4], z[4];
        place::corners(s, x, z);
        k.x0 = k.x1 = x[0];
        k.z0 = k.z1 = z[0];
        for (int c = 1; c < 4; c++) { k.x0 = std::min(k.x0, x[c]); k.x1 = std::max(k.x1, x[c]); k.z0 = std::min(k.z0, z[c]); k.z1 = std::max(k.z1, z[c]); }
        r.keep.push_back(k);
    };
    if (keep_off) for (const place::Sheet& s : r.sheets) add_keep(s);
    static const uint32_t K_BUILD = key_of("MABuilding");
    for (size_t i = 0; i < doc_->world.objs.size(); i++) {
        const WObj& o = doc_->world.objs[i];
        r.used.insert(o.description());
        Vec3 p;
        if (!positioned_root(i, p)) continue;
        // Water blocks nothing (playtest 11-17): its rectangle reaches over dry banks (a 60 x 40 m lake on a slope covers grass),
        // and "Keep off" kept trees off all of it with no way to tell why. Trees, rocks and props go anywhere a water is.
        if (o.cls == K_WATER) continue;
        if (o.str("Corporal3DObj").empty() || sheet_kind(o)) continue;   // markers, triggers and flat pieces are no neighbour
        float rad = 1.f;
        GpuModel* gm = obj_model(o);
        if (gm && gm->state == GpuModel::Ready) rad = footprint_radius(gm->bmin, gm->bmax);
        r.grid.add(p.x, p.z, rad);
        if (keep_off && (o.cls == K_BUILD || is_foundation_obj(o))) {
            place::Sheet s;
            if (object_footprint(o, s)) add_keep(s);
        }
    }
}

// True when an object of radius `rad` at (x, z) would stand on a keep-off area: the point or any of eight points on its rim.
bool App::scatter_blocked(const ScatterRun& r, float x, float z, float rad) const {
    if (r.keep.empty()) return false;
    static const float dx[9] = {0.f, 1.f, -1.f, 0.f, 0.f, 0.7071f, -0.7071f, 0.7071f, -0.7071f};
    static const float dz[9] = {0.f, 0.f, 0.f, 1.f, -1.f, 0.7071f, 0.7071f, -0.7071f, -0.7071f};
    for (const ScatterKeep& k : r.keep) {
        if (x + rad < k.x0 || x - rad > k.x1 || z + rad < k.z0 || z - rad > k.z1) continue;
        for (int i = 0; i < 9; i++)
            if (place::contains(k.s, x + dx[i] * rad, z + dz[i] * rad)) return true;
    }
    return false;
}

// One object: placed through the template (no selection change, no status), the pose and the height set at once.
uint32_t App::place_scattered(uint32_t tpl, Vec3 at, float heading_rad, float pitch, float roll) {
    ScatterRun& r = *sc_run_;
    const uint32_t root = io_->place_template(*doc_, tpl, at, heading_rad);
    if (!root) return 0;
    WTree t;
    t.build(doc_->world);
    const int ri = t.find(root);
    if (ri < 0) return 0;
    std::vector<int> sub;
    t.subtree(ri, sub);
    for (int i : sub) {
        // A template carries the set links of the retail object it was cut from; a new object belongs to no set (place_object does the same).
        WObj& o = doc_->world.objs[size_t(i)];
        if (int n = o.i32("SetCount"); n > 0) {
            for (int k = 0; k < n; k++) o.erase(key_of("SetMember" + std::to_string(k)));
            o.set_i32("SetCount", 0);
        }
    }
    WObj& ro = doc_->world.objs[size_t(ri)];
    if ((pitch != 0.f || roll != 0.f) && can_tilt(ro)) set_tilt(ro, pitch, roll);
    else pitch = roll = 0.f;
    Vec3 p;
    if (!get_pos(ro, p)) return root;
    float lift = 0.f, sx = at.x, sz = at.z;   // lift: the origin's height above what the middle of the model's base stands on
    if (!ro.str("Corporal3DObj").empty()) {
        GpuModel* gm = obj_model(ro);
        if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
        if (gm && gm->state == GpuModel::Ready) {
            // The middle of the model's base, turned with the object: it goes on the surface ("stands on" reads the same point; the
            // origin can be metres off it). A tilted object keeps that point on the ground, so a rock lying along a slope has gap 0.
            const Vec3 fo = place::rotate_local(heading_rad, pitch, roll, 0.5f * (gm->bmin[0] + gm->bmax[0]), gm->bmin[1], 0.5f * (gm->bmin[2] + gm->bmax[2]));
            lift = -fo.y;
            sx += fo.x;
            sz += fo.z;
        }
    }
    // It stands on the top sheet under it, else the ground (surface_at; with "Keep off" on that is the ground).
    const float dy = (r.surf ? r.surf->at(doc_->terrain, sx, sz) : doc_->terrain.height_at(sx, sz)) + lift - p.y;
    if (std::fabs(dy) > 1e-4f)
        for (int i : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(i)], q)) set_pos(doc_->world.objs[size_t(i)], {q.x, q.y + dy, q.z});
        }
    if (const TemplateInfo* ti = cat_->find(tpl); ti && !ti->name.empty()) {   // named after its palette entry, numbered ("Name 2", "Name 3")
        const std::string base = palette_plain_name(*ti);
        int& n = r.name_n[base];
        if (n < 1) n = 1;
        std::string nm = n == 1 ? base : base + " " + std::to_string(n);
        while (r.used.count(nm)) nm = base + " " + std::to_string(++n);
        r.used.insert(nm);
        n++;
        ro.set_str("Description", nm);
    }
    return root;
}

// One object for the accumulator: up to 8 tries at random points of the newly covered cells. Counts why a spot was skipped.
void App::scatter_try_one(const std::vector<std::pair<float, float>>& cells) {
    ScatterRun& r = *sc_run_;
    const uint32_t tpl = scatter_set_[size_t(sc_rng_.next() % scatter_set_.size())];
    const float rad = scatter_radius(tpl);
    const float lim = settings_.sc_slope / 100.f;
    int why = 0;   // 1 close, 2 keep off, 3 steep, 4 off the map (not counted)
    for (int tries = 0; tries < 8; tries++) {
        const auto& c = cells[size_t(sc_rng_.next() % cells.size())];
        const float x = c.first + (sc_rng_.unit() - 0.5f) * kScatterCell, z = c.second + (sc_rng_.unit() - 0.5f) * kScatterCell;
        if (!doc_->terrain.inside(x, z)) { why = 4; continue; }
        if (settings_.sc_keep_off && scatter_blocked(r, x, z, rad)) { why = 2; continue; }
        if (settings_.sc_skip_steep && slope_at(doc_->terrain, x, z) > lim) { why = 3; continue; }
        if (!r.grid.clear(x, z, rad, settings_.sc_spacing)) { why = 1; continue; }
        const float heading_rad = settings_.sc_facing ? sc_rng_.unit() * 6.2831853f : 0.f;
        float pitch = 0.f, roll = 0.f;
        const WObj* to = cat_ ? cat_->object(tpl) : nullptr;
        if (to && can_tilt(*to) && (settings_.sc_follow || settings_.sc_lean > 0.f)) {
            float up[3] = {0.f, 1.f, 0.f};
            if (settings_.sc_follow) { const Vec3 n = doc_->terrain.normal_at(x, z); up[0] = n.x; up[1] = n.y; up[2] = n.z; }
            float out[3] = {up[0], up[1], up[2]};
            if (settings_.sc_lean > 0.f) lean_up(up, settings_.sc_lean, sc_rng_.unit() * 6.2831853f, sc_rng_.unit(), out);
            place::tilt_for_up({out[0], out[1], out[2]}, heading_rad, pitch, roll);
        }
        if (!place_scattered(tpl, {x, 0.f, z}, heading_rad, pitch, roll)) return;
        r.grid.add(x, z, rad);
        r.placed++;
        r.frame_placed++;
        r.kinds.insert(tpl);
        return;
    }
    if (why == 1) r.close++;
    else if (why == 2) r.keepoff++;
    else if (why == 3) r.steep++;
}

// The brush at p: a ring of newly covered cells earns objects by area; an erase ring removes the set's kinds whose origin is inside it.
void App::scatter_dab(const Vec3& p) {
    ScatterRun& r = *sc_run_;
    if (r.erase) {
        std::vector<uint32_t> gone;
        const float R2 = brush_radius_ * brush_radius_;
        for (ScatterErase& c : r.cand) {
            if (!c.uid) continue;
            if ((c.x - p.x) * (c.x - p.x) + (c.z - p.z) * (c.z - p.z) > R2) continue;
            gone.push_back(c.uid);
            c.uid = 0;
        }
        for (uint32_t u : gone) {
            if (const WObj* o = object_by_uid(doc_->world, u)) r.kinds.insert(o->template_id());
            wedit::remove_subtree(doc_->world, u);
            r.erased++;
            r.frame_placed++;
        }
        return;
    }
    std::vector<std::pair<float, float>> cells;
    const int n = r.cov.newly(p.x, p.z, brush_radius_, &cells);
    if (!n) return;
    r.accum += float(n) * scatter_per_cell(settings_.sc_density);
    while (r.accum >= 1.f && !cells.empty() && !scatter_set_.empty()) {
        r.accum -= 1.f;
        scatter_try_one(cells);
    }
}

void App::scatter_path(const Vec3& a, const Vec3& b) {
    const int n = substeps(a.x, a.z, b.x, b.z, brush_radius_);
    for (int k = 1; k <= n; k++) {
        const float f = float(k) / float(n);
        scatter_dab({a.x + (b.x - a.x) * f, 0.f, a.z + (b.z - a.z) * f});
    }
}

static std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

void App::scatter_end() {
    stroke_active_ = false;
    if (!sc_run_) return;
    ScatterRun& r = *sc_run_;
    sc_last_ = {r.placed, r.erased, r.close, r.keepoff, r.steep, int(r.kinds.size()), r.erase, true};
    last_stroke_end_[int(Tool::ScatterTrees)] = stroke_last_;
    last_stroke_ok_[int(Tool::ScatterTrees)] = true;
    std::string msg;
    if (r.erase) {
        if (r.erased) msg = "Removed " + plural(r.erased, "object", "objects") + " (" + plural(int(r.kinds.size()), "kind", "kinds") + "). " + kt(A_UNDO) + " puts them back.";
        else msg = "Nothing removed: only the kinds in the brush set are taken away, and none of them stands under the brush.";
    } else {
        const int skipped = r.close + r.keepoff + r.steep;
        if (r.placed) msg = "Scattered " + plural(r.placed, "object", "objects") + " (" + plural(int(r.kinds.size()), "kind", "kinds") + ").";
        else msg = "Nothing scattered.";
        if (skipped) {
            msg += " Skipped " + plural(skipped, "spot", "spots") + ":";
            bool first = true;
            auto add = [&](int n, const char* what) {
                if (!n) return;
                msg += std::string(first ? " " : ", ") + std::to_string(n) + " " + what;
                first = false;
            };
            add(r.close, "too close to others");
            add(r.keepoff, "on a road, base or building");
            add(r.steep, "on steep ground");
            msg += ".";
        }
        if (r.placed) msg += " " + kt(A_UNDO) + " removes them. From straight above they look like dark shapes: tilt the view (middle-drag) to see them.";   // 
        bool other = false;
        for (uint32_t u : scatter_set_) other = other || gfx_cell_mb(u, true) > 0.001f;
        if (other) {
            char b[96];
            std::snprintf(b, sizeof b, " Graphics memory: about %.1f of 64 MB.", gfx_live_total());
            msg += b;
        }
    }
    status_ = msg;
    status_time_ = 8;
    if (!r.placed && !r.erased && !undo_.empty()) drop_last_undo();   // nothing changed: no undo step for it
    if (r.erased) drop_dead_set_members();
    sc_run_.reset();
    world_changed();
}

// Called every frame the Scatter brush is armed over the map (app.cpp): the press starts a stroke (Ctrl = erase, Shift = a line from the last
// stroke's end), the held mouse adds the ground it passes over, the release ends it.
void App::scatter_frame(const Vec3& g, bool lclick, bool ldown, bool lrelease, bool have_ground) {
    if (lclick && have_ground && !scatter_set_.empty()) {
        if (sc_run_) scatter_end();
        const bool erase = holding(settings_.hold_reverse);
        const bool line = holding(settings_.hold_free) && last_stroke_ok_[int(Tool::ScatterTrees)];
        const Vec3 from = line ? last_stroke_end_[int(Tool::ScatterTrees)] : g;
        push_undo(erase ? "scatter erase" : "scatter");
        stroke_active_ = true;
        scatter_begin(erase);
        scatter_dab(from);
        if (line) scatter_path(from, g);
        stroke_last_ = g;
    } else if (stroke_active_ && sc_run_ && ldown && have_ground) {
        scatter_path(stroke_last_, g);
        stroke_last_ = g;
    }
    if (sc_run_ && sc_run_->frame_placed) {
        sc_run_->frame_placed = 0;
        world_changed();
    }
    if (stroke_active_ && (lrelease || !ldown)) scatter_end();
}

}  // namespace ed
