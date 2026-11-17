// Water handling in the viewport: picking (centre diamond and visible surface), sizes and the
// Place water tool (any size: the scenario ships a model of that size, see core/water_custom.h),
// the developer pond probe (a basin plus the smallest water tile) and the "Everything in this map" list.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "imgui_stdlib.h"
#include "../core/triggers.h"
#include "../core/water_custom.h"

namespace ed {

std::string kind_label(const std::string& cls);  // app_tabs.cpp: plain kind of an engine class

using namespace ma2;
using namespace ma2::world;

void App::select_uid(uint32_t uid) {
    selection_ = {uid};
    tool_ = Tool::Select;
}

// Water view under the mouse. The blue diamond at each tile's centre always picks it (even with
// something standing in the water); the visible surface picks it when nothing else is hit first
// (t_other = distance of the nearest other hit) and the ground does not hide it.
int App::pick_water(float mx, float my, float t_other, bool marker_only) {
    XMFLOAT3 o, d;
    cam_.ray(mx - vp_x, my - vp_y, vp_w, vp_h, o, d);
    int best = -1;
    float best_t = t_other;
    for (auto& v : views_) {
        if (v.kind != 2) continue;
        float sx, sy;
        // The blue handle is a helper: with H off only a selected water has one.
        if ((show_helpers_ || selection_.count(doc_->world.objs[size_t(v.root)].uid())) && cam_.project({v.pos.x, v.pos.y, v.pos.z}, vp_w, vp_h, sx, sy)) {
            float dx = sx + vp_x - mx, dy = sy + vp_y - my;
            if (dx * dx + dy * dy < 14.f * dpi_ * 14.f * dpi_) return v.root;
        }
        if (marker_only || std::fabs(d.y) < 1e-4f) continue;
        float t = (v.pos.y - o.y) / d.y;
        if (t <= 0 || t >= best_t) continue;
        float ch = std::cos(v.heading), sh = std::sin(v.heading);
        float px = o.x + d.x * t - v.pos.x, pz = o.z + d.z * t - v.pos.z;
        float lx = ch * px - sh * pz, lz = sh * px + ch * pz;
        if (std::fabs(lx) > v.size.x * 0.5f || std::fabs(lz) > v.size.z * 0.5f) continue;
        float tg;
        if (doc_->terrain.raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, tg) && tg < t) continue;  // the ground is in front of the water
        best = v.root;
        best_t = t;
    }
    return best;
}

// Whether the water type's model is a plain grid that can be given any size (cached).
bool App::water_can_resize(uint32_t template_uid) {
    auto it = water_resizable_.find(template_uid);
    if (it != water_resizable_.end()) return it->second;
    bool ok = io_ && doc_ && io_->water_resizable(*doc_, template_uid);
    water_resizable_[template_uid] = ok;
    return ok;
}

// The size the list shows: the one in the description, else the plane the view drew from the model.
void App::water_size_of(const WObj& o, const Vec3*, float& w, float& l) {
    w = l = 0;
    if (ScenarioIO::water_custom_size(o, w, l)) return;
    for (auto& v : views_)
        if (v.kind == 2 && v.index >= 0 && v.index < int(doc_->world.objs.size()) && doc_->world.objs[size_t(v.index)].uid() == o.uid()) {
            w = v.size.x;
            l = v.size.z;
            return;
        }
}

void App::set_water_size(WObj& o, float w, float l, int shape) {
    w = std::clamp(std::round(w), wcustom::kMinSide, wcustom::kMaxSide);
    l = std::clamp(std::round(l), wcustom::kMinSide, wcustom::kMaxSide);
    std::string name = ScenarioIO::water_name(o);
    if (name.empty()) {
        const TemplateInfo* ti = cat_->find(o.template_id());
        name = ti ? ti->name : std::string("Water");
    }
    wcustom::Shape sh = shape < 0 ? ScenarioIO::water_custom_shape(o) : wcustom::Shape(shape);
    o.set_str("Description", ScenarioIO::water_description(name, w, l, sh));
    doc_->dirty = true;
    objects_dirty_ = true;
    minimap_dirty_ = true;
}

// The type the Place water tool uses: the one picked in the list, else the first that can be resized.
uint32_t App::water_place_tpl() {
    if (water_new_tpl_) return water_new_tpl_;
    const TemplateInfo* pick = nullptr;
    for (auto& ti : cat_->all()) {
        if (ti.cls_name != "MAWater" || ti.model.empty() || ti.archives.empty() || !water_can_resize(ti.uid)) continue;
        if (!pick || ti.name == "Water_1000x1000 NEW") pick = &ti;
    }
    return pick ? pick->uid : 0;
}

// The Place water tool: the chosen type at the chosen size, centred where you click. With "Dig a basin" its surface is
// kShoreDrop under the ground you clicked and a basin is dug under it (never under another building or flat piece);
// without, the surface is 1 m above that ground.
void App::place_water(const Vec3& at, float dw, float dl) {
    if (!editable()) return;
    const float W = dw > 0 ? dw : water_new_w_, L = dl > 0 ? dl : water_new_l_;   // a dragged-out size, else the boxes'
    if (!water_place_tpl()) { status_ = "No water type that can be resized was found in this game's files."; status_time_ = 6; return; }
    water_new_tpl_ = water_place_tpl();
    if (!doc_->terrain.inside(at.x, at.z)) return;
    push_undo("place water");
    bool dig = water_new_dig_ && water_can_resize(water_new_tpl_);
    float surf = dig ? at.y - kShoreDrop : at.y + 1.f;
    uint32_t root = io_->place_template(*doc_, water_new_tpl_, {at.x, surf, at.z}, place_heading_);
    if (!root) return;
    for (auto& o : doc_->world.objs)
        if (o.uid() == root) {
            set_pos(o, {at.x, surf, at.z});
            set_heading(o, place_heading_);
            if (water_can_resize(water_new_tpl_)) {
                set_water_size(o, W, L, water_new_oval_ ? int(wcustom::Shape::Oval) : int(wcustom::Shape::Rect));
                // A plain default name ("Pond 2", "Lake 3") instead of the template's "Water_1000x1000 NEW".
                int n = 0;
                for (auto& q : doc_->world.objs) n += q.cls == K_WATER;
                std::string nm = std::string(std::min(W, L) < 60 ? "Pond " : "Lake ") + std::to_string(n);
                o.set_str("Description", ScenarioIO::water_description(nm, W, L, water_new_oval_ ? wcustom::Shape::Oval : wcustom::Shape::Rect));
            }
            break;
        }
    basin_kept_.clear();
    basin_moved_ = 0;
    if (WObj* wo = object_by_uid(doc_->world, root)) {
        if (dig) make_basin(*wo, false, false, "place water");
        else if (water_can_resize(water_new_tpl_)) mark_water_ground({at.x, surf, at.z}, W, L, water_new_oval_, place_heading_);
    }
    selection_ = {root};
    water_new_last_ = {root, at, W, L, place_heading_};
    world_changed();
    std::string msg = std::string(water_new_oval_ ? "Oval water placed (" : "Water placed (") + std::to_string(int(W)) + " x " + std::to_string(int(L)) + " m)";
    if (dig) {
        msg += " in a basin dug under it; players wade in it.";
        if (!basin_kept_.empty()) msg += " The basin stops short of " + basin_kept_ + ", which stand at the water's edge.";
        if (basin_moved_ > 0) msg += " " + std::to_string(basin_moved_) + " object" + (basin_moved_ == 1 ? " rides" : "s ride") + " down with the bed.";
        msg += " " + kt(A_UNDO) + " undoes both. Right-click to stop placing.";
    } else {
        msg += ". Its surface is 1 m above where you clicked: drag the green arrow to raise or lower it. Right-click to stop placing.";
    }
    status_ = msg;
    status_time_ = 8;
}

// Gameplay water is the type map, not the MAWater model (wiki: MATerrainInfo type map, 8 = playable water ground):
// playable cells under the water's footprint whose ground is below its surface become water ground, as in the
// retail maps. Border / out-of-bounds cells stay. Returns how many cells changed.
int App::mark_water_ground(const Vec3& c, float w, float l, bool oval, float heading) {
    if (!doc_ || doc_->typemap.empty()) return 0;
    const Terrain& t = doc_->terrain;
    int W = doc_->typemap_w, H = doc_->typemap_h, n = 0;
    float hw = w * 0.5f, hl = l * 0.5f, ch = std::cos(heading), sh = std::sin(heading);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float wx = (x + 0.5f) / W * t.size_x(), wz = (y + 0.5f) / H * t.size_z();
            float dx = wx - c.x, dz = wz - c.z;
            float u = (ch * dx - sh * dz) / hw, v = (sh * dx + ch * dz) / hl;
            if (oval ? u * u + v * v > 1.f : std::max(std::fabs(u), std::fabs(v)) > 1.f) continue;
            uint16_t& tv = doc_->typemap[size_t(y) * W + x];
            float gh = t.height_at(wx, wz);
            if (tv != tmtype::Playable || gh >= c.y) continue;
            tv = c.y - gh > kWadeMaxDepth ? tmtype::DeepWater : tmtype::Special;  // retail marks deep water 16, not 8
            n++;
        }
    if (n) { doc_->typemap_dirty = true; overlay_dirty_ = true; }
    return n;
}

void App::track_water_ground() {
    auto foot_covers = [](const WaterFoot& f, float wx, float wz) {
        float ch = std::cos(f.heading), sh = std::sin(f.heading), dx = wx - f.c.x, dz = wz - f.c.z;
        float u = (ch * dx - sh * dz) / (f.w * 0.5f), v = (sh * dx + ch * dz) / (f.l * 0.5f);
        return f.oval ? u * u + v * v <= 1.f : std::max(std::fabs(u), std::fabs(v)) <= 1.f;
    };
    if (!doc_ || doc_->read_only || doc_->typemap.empty()) { water_feet_.clear(); water_feet_doc_ = nullptr; return; }
    std::map<uint32_t, WaterFoot> now;
    for (auto& o : doc_->world.objs) {
        if (o.cls != K_WATER) continue;
        WaterFoot f;
        if (!get_pos(o, f.c)) continue;
        water_size_of(o, nullptr, f.w, f.l);
        if (f.w <= 0 || f.l <= 0) continue;  // model size not known yet
        f.heading = heading(o);
        f.oval = ScenarioIO::water_custom_shape(o) == wcustom::Shape::Oval;
        now[o.uid()] = f;
    }
    if (water_feet_doc_ != doc_.get()) {  // another map: start from what it has
        water_feet_doc_ = doc_.get();
        water_feet_ = std::move(now);
        return;
    }
    auto same = [](const WaterFoot& a, const WaterFoot& b) {
        return a.c.x == b.c.x && a.c.y == b.c.y && a.c.z == b.c.z && a.w == b.w && a.l == b.l && a.heading == b.heading && a.oval == b.oval;
    };
    std::vector<WaterFoot> gone, moved;
    for (auto& [uid, f] : water_feet_) {
        auto it = now.find(uid);
        if (it == now.end()) gone.push_back(f);
        else if (!same(f, it->second)) gone.push_back(f), moved.push_back(it->second);
    }
    water_feet_ = std::move(now);
    if (gone.empty()) return;
    // Cells of the old footprints that no water covers now (ground below its surface) are plain ground again.
    const Terrain& t = doc_->terrain;
    int W = doc_->typemap_w, H = doc_->typemap_h, n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t& tv = doc_->typemap[size_t(y) * W + x];
            if (tv != tmtype::Special && tv != tmtype::DeepWater) continue;
            float wx = (x + 0.5f) / W * t.size_x(), wz = (y + 0.5f) / H * t.size_z();
            float gh = t.height_at(wx, wz);
            // Only cells the old water could have marked (under its surface): retail marks on higher ground stay.
            if (std::none_of(gone.begin(), gone.end(), [&](const WaterFoot& f) { return gh < f.c.y && foot_covers(f, wx, wz); })) continue;
            bool covered = std::any_of(water_feet_.begin(), water_feet_.end(),
                                       [&](auto& kv) { return gh < kv.second.c.y && foot_covers(kv.second, wx, wz); });
            if (!covered) tv = tmtype::Playable, n++;
        }
    if (n) { doc_->typemap_dirty = true; overlay_dirty_ = true; }
    for (const WaterFoot& f : moved) mark_water_ground(f.c, f.w, f.l, f.oval, f.heading);
}

// A fixed-size water placed from the palette: its size comes from its model (the object view), known a frame or more
// after placing. Then its basin is dug and its ground marked, inside the same undo step as the placing.
void App::finish_fixed_water() {
    const WObj* o = doc_ ? object_by_uid(doc_->world, water_fixed_pending_) : nullptr;
    float w = 0, l = 0;
    if (o) water_size_of(*o, nullptr, w, l);
    if (o && (w <= 0 || l <= 0) && ++water_fixed_tries_ < 300) return;  // model still loading
    water_fixed_pending_ = 0;
    if (!o || w <= 0 || l <= 0) return;
    Vec3 p;
    if (!get_pos(*o, p)) return;
    basin_kept_.clear();
    basin_moved_ = 0;
    if (WObj* wo = object_by_uid(doc_->world, o->uid())) make_basin(*wo, false, false, "place water");
    objects_dirty_ = minimap_dirty_ = true;
    status_ = "Water placed in a basin dug under it; players wade in it." +
              (basin_kept_.empty() ? std::string() : " The basin stops short of " + basin_kept_ + ", which stand at the water's edge.") + " " + kt(A_UNDO) + " undoes both.";
    status_time_ = 8;
}

// Whether the ground point is inside the footprint of the water the tool placed last (while that water still exists
// where it was put), so a second click there does not drop another water on top of it.
bool App::over_new_water(const Vec3& g) const {
    const NewWater& n = water_new_last_;
    if (!n.uid || !doc_) return false;
    bool found = false;
    for (auto& o : doc_->world.objs)
        if (o.uid() == n.uid) { Vec3 q; found = get_pos(o, q) && q.x == n.at.x && q.z == n.at.z; break; }
    if (!found) return false;
    float ch = std::cos(n.heading), sh = std::sin(n.heading), dx = g.x - n.at.x, dz = g.z - n.at.z;
    return std::fabs(ch * dx - sh * dz) <= n.w * 0.5f && std::fabs(sh * dx + ch * dz) <= n.l * 0.5f;
}

// ---- Water basins -------------------------------------------------------------------------------------------------------------
// A basin is a bowl dug under water (w x l box, or the ellipse inside it) so the surface has depth under it: 1 m deep water is a faint
// tint in the game and the editor. It is planned as a GroundEdit (nothing written), protected like Fit ground (never under another
// building or flat piece, units, turrets, spawn points), and remembered on the water as EditorBasin so the water can give it back.
// Ground already lower stays, and so does ground more than kDigMaxCut above the surface; the rim meets the surface near the edge, so the
// water's square corners hide under the ground. The surface itself lies kShoreDrop under the ground that was clicked.

namespace {
int step_index(const Terrain& t, float h) {
    const float range = std::max(0.001f, t.max_h - t.min_h);
    return std::clamp(int(std::lround((h - t.min_h) / range * 255.f)), 0, 255);
}
float step_height(const Terrain& t, int s) { return t.min_h + float(s) / 255.f * std::max(0.001f, t.max_h - t.min_h); }
const float kNaNw = std::numeric_limits<float>::quiet_NaN();
}  // namespace

bool App::water_foot(const WObj& o, BasinFoot& f) {
    Vec3 p;
    if (!doc_ || o.cls != K_WATER || !get_pos(o, p)) return false;
    float w = 0, l = 0;
    water_size_of(o, nullptr, w, l);
    if (w <= 0 || l <= 0) return false;
    f.cx = p.x;
    f.cz = p.z;
    f.w = w;
    f.l = l;
    f.heading = heading(o);
    f.surface = p.y;
    f.oval = ScenarioIO::water_custom_shape(o) == wcustom::Shape::Oval;
    return true;
}

App::BasinPlan App::plan_basin(const BasinFoot& f, const GroundEdit* fill) {
    BasinPlan P;
    P.foot = f;
    if (!doc_) return P;
    const Terrain& t = doc_->terrain;
    const float hw = std::max(2.f, f.w * 0.5f), hl = std::max(2.f, f.l * 0.5f);
    float depth = std::min(pond_depth_, std::max(1.f, std::min(hw, hl) * 0.3f));  // a 16 m pond is ~2.5 m deep, a lake up to 8 m
    // A pilot on foot sank fully in an 8 m lake (a test run). Retail shallow (wade) water is a median
    // 0.3-1.2 m, p90 <= 1.6 m; MP lakes up to 4.4 m (research, every retail world): wadeable by default, Deep up to 4 m.
    depth = std::min(depth, water_new_deep_ ? kDeepLakeDepth : kWadeDepth);
    // Turned water (heading): the box's extent on the map, and grid points taken into the water's own axes.
    const float ch = std::cos(f.heading), sh = std::sin(f.heading);
    const float ex = std::fabs(ch) * hw + std::fabs(sh) * hl, ez = std::fabs(sh) * hw + std::fabs(ch) * hl;
    P.bx0 = std::max(0, int((f.cx - ex) / t.scale) - 1);
    P.bx1 = std::min(t.w - 1, int((f.cx + ex) / t.scale) + 2);
    P.bz0 = std::max(0, int((f.cz - ez) / t.scale) - 1);
    P.bz1 = std::min(t.h - 1, int((f.cz + ez) / t.scale) + 2);
    if (P.bx1 < P.bx0 || P.bz1 < P.bz0) return P;
    GroundEdit& e = P.e;
    e.gx0 = P.bx0; e.gx1 = P.bx1; e.gz0 = P.bz0; e.gz1 = P.bz1;
    if (fill && !fill->empty()) {
        e.gx0 = std::min(e.gx0, fill->gx0); e.gx1 = std::max(e.gx1, fill->gx1);
        e.gz0 = std::min(e.gz0, fill->gz0); e.gz1 = std::max(e.gz1, fill->gz1);
    }
    const int nx = e.gx1 - e.gx0 + 1;
    e.want.assign(size_t(nx) * size_t(e.gz1 - e.gz0 + 1), kNaNw);
    auto at_i = [&](int gx, int gz) { return size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0); };
    if (fill && !fill->empty()) {
        const int fnx = fill->gx1 - fill->gx0 + 1;
        for (int gz = fill->gz0; gz <= fill->gz1; gz++)
            for (int gx = fill->gx0; gx <= fill->gx1; gx++) e.want[at_i(gx, gz)] = fill->want[size_t(gz - fill->gz0) * size_t(fnx) + size_t(gx - fill->gx0)];
    }
    const int bnx = P.bx1 - P.bx0 + 1;
    P.pre.assign(size_t(bnx) * size_t(P.bz1 - P.bz0 + 1), 0.f);
    for (int gz = P.bz0; gz <= P.bz1; gz++)
        for (int gx = P.bx0; gx <= P.bx1; gx++) {
            const float w0 = e.want[at_i(gx, gz)];
            const float h = std::isnan(w0) ? t.step_at(gx, gz) : w0;  // the ground as it is once the old basin is given back
            P.pre[size_t(gz - P.bz0) * size_t(bnx) + size_t(gx - P.bx0)] = h;
            const float dx = gx * t.scale - f.cx, dz = gz * t.scale - f.cz;
            const float u = (ch * dx - sh * dz) / hw, v = (sh * dx + ch * dz) / hl;
            const float r = f.oval ? std::sqrt(u * u + v * v) : std::max(std::fabs(u), std::fabs(v));
            if (r >= 1.f) continue;
            // 0.5 m over the surface at the edge, crossing it at e ~0.9, full depth in the middle
            const float target = f.surface + 0.5f - (depth + 0.5f) * (1.f - r * r);
            // Ground well above the surface (a hillside under a big water) stays and hides the water there, as in the retail
            // levels; cutting it down carved a flat terrace with stepped walls into the hill.
            if (h > f.surface + kDigMaxCut) continue;
            const float nv = t.stored(std::clamp(std::min(h, target), t.min_h, t.max_h));
            if (nv != t.step_at(gx, gz) || !std::isnan(w0)) e.want[at_i(gx, gz)] = nv;
        }
    return P;
}

// The remembered basin given back: every grid point of its rectangle that is still at the dug step returns to the step it had before.
App::GroundEdit App::plan_basin_fill(const WObj& water) {
    GroundEdit e;
    if (!doc_ || !water.find(place::kBasinProp)) return e;
    int gx0, gz0, nx, nz;
    std::vector<uint8_t> before, after;
    if (!place::basin_decode(water.str(place::kBasinProp), gx0, gz0, nx, nz, before, after)) return e;
    const Terrain& t = doc_->terrain;
    if (gx0 < 0 || gz0 < 0 || gx0 + nx > t.w || gz0 + nz > t.h) return e;
    e.gx0 = gx0; e.gz0 = gz0; e.gx1 = gx0 + nx - 1; e.gz1 = gz0 + nz - 1;
    e.want.assign(size_t(nx) * size_t(nz), kNaNw);
    for (int j = 0; j < nz; j++)
        for (int i = 0; i < nx; i++) {
            const size_t k = size_t(j) * size_t(nx) + size_t(i);
            if (before[k] == after[k]) continue;
            if (step_index(t, t.step_at(gx0 + i, gz0 + j)) == int(after[k])) e.want[k] = step_height(t, before[k]);  // still the dug step: not sculpted since
        }
    return e;
}

// The plan the ghost and the button hover draw: the bowl, protected, with what would ride down with it counted.
App::GroundEdit App::basin_preview_edit(const BasinFoot& f, const WObj* water) {
    GroundEdit fill;
    const bool had = water && water->find(place::kBasinProp) && !(fill = plan_basin_fill(*water)).empty();
    BasinPlan P = plan_basin(f, had ? &fill : nullptr);
    protect_ground(P.e, {}, Protect::Everything);
    forecast_ground(P.e, false);  // cheap enough for every frame; followers are counted when it is applied
    return P.e;
}

// "The basin stops short of Office 07 and 1 other."
std::string App::basin_note(const GroundEdit& e) {
    std::string s = e.kept.empty() ? std::string() : "The basin stops short of " + fit_summary_names(e) + ", which stand at the water's edge.";
    return s;
}

// Digs the basin for this water as it is now. refit: its remembered basin is given back first (one plan, one undo step). push: the caller
// has not pushed the undo step yet. Returns how many objects rode with the ground.
int App::make_basin(WObj& water, bool refit, bool push, const char* undo_label) {
    BasinFoot f;
    if (!doc_ || !water_foot(water, f)) return 0;
    GroundEdit fill;
    const bool had = refit && water.find(place::kBasinProp) && !(fill = plan_basin_fill(water)).empty();
    BasinPlan P = plan_basin(f, had ? &fill : nullptr);
    protect_ground(P.e, {}, Protect::Everything);
    forecast_ground(P.e, false);
    basin_moved_ = apply_ground_edit(P.e, undo_label, push);
    basin_kept_ = P.e.kept.empty() ? std::string() : fit_summary_names(P.e);
    // Remember the dug rectangle: the step of each point before and after, and where the water stood (to notice a later change).
    const Terrain& t = doc_->terrain;
    const int nx = P.bx1 - P.bx0 + 1, nz = P.bz1 - P.bz0 + 1;
    if (nx > 0 && nz > 0) {
        std::vector<uint8_t> before(size_t(nx) * size_t(nz)), after(before.size());
        for (int j = 0; j < nz; j++)
            for (int i = 0; i < nx; i++) {
                const size_t k = size_t(j) * size_t(nx) + size_t(i);
                before[k] = uint8_t(step_index(t, P.pre[k]));
                after[k] = uint8_t(step_index(t, t.step_at(P.bx0 + i, P.bz0 + j)));
            }
        char ex[160];
        snprintf(ex, sizeof ex, "%.3f %.3f %.3f %.3f %.4f %d %.3f", f.cx, f.cz, f.w, f.l, f.heading, f.oval ? 1 : 0, f.surface);
        water.set_str(place::kBasinProp, place::basin_encode(P.bx0, P.bz0, nx, nz, before, after, ex));
    }
    mark_water_ground({f.cx, f.surface, f.cz}, f.w, f.l, f.oval, f.heading);
    io_->bake_lightmap(*doc_);
    r_.set_lightmap(doc_->lightmap);
    lightmap_stale_ = false;
    stale_sun_only_ = false;
    doc_->dirty = true;
    objects_dirty_ = minimap_dirty_ = true;
    return basin_moved_;
}

// The water's basin follows it. deleted: only give the basin back (the water is about to go). Else fill it and dig again for the water as
// it is now. No undo step of its own: the edit that changed the water has already pushed one.
void App::refit_basin(WObj& water, bool deleted) {
    if (!doc_ || !editable() || !water.find(place::kBasinProp)) return;
    const std::string name = [&] { std::string d = ScenarioIO::water_name(water); return d.empty() || d == "NULL" ? std::string("The water") : d; }();
    if (deleted) {
        GroundEdit e = plan_basin_fill(water);
        if (e.empty()) return;
        protect_ground(e, {}, Protect::Everything);
        forecast_ground(e, false);
        basin_moved_ = apply_ground_edit(e, "fill basin", false);
        io_->bake_lightmap(*doc_);
        r_.set_lightmap(doc_->lightmap);
        lightmap_stale_ = false;
        stale_sun_only_ = false;
        objects_dirty_ = minimap_dirty_ = true;
        return;
    }
    BasinFoot f;
    if (!water_foot(water, f)) return;
    make_basin(water, true, false, "refit basin");
    char b[300];
    snprintf(b, sizeof b, "%s is now %d x %d m. Its basin was refitted: filled and dug again to fit.", name.c_str(), int(f.w + 0.5f), int(f.l + 0.5f));
    status_ = b;
    if (!basin_kept_.empty()) status_ += " The basin stops short of " + basin_kept_ + ".";
    status_ += " " + kt(A_UNDO) + " undoes it.";
    status_time_ = 8;
}

// Every frame: a water whose place, size, shape or level differs from where its basin was dug gets the basin refitted, once the mouse is up
// and no field is being edited (a drag or a typed number is one change, not one per frame).
void App::refit_moved_basins() {
    if (!doc_ || doc_->read_only || !editable()) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive() || dragging_ || gizmo_drag_ || stroke_active_ || place_drag_) return;
    for (auto& o : doc_->world.objs) {
        if (o.cls != K_WATER || !o.find(place::kBasinProp)) continue;
        int gx0, gz0, nx, nz;
        std::vector<uint8_t> b, a;
        std::string ex;
        if (!place::basin_decode(o.str(place::kBasinProp), gx0, gz0, nx, nz, b, a, &ex)) continue;
        float cx, cz, w, l, hd, sf;
        int ov = 0;
        if (std::sscanf(ex.c_str(), "%f %f %f %f %f %d %f", &cx, &cz, &w, &l, &hd, &ov, &sf) != 7) continue;
        BasinFoot f;
        if (!water_foot(o, f)) continue;
        const bool same = std::fabs(f.cx - cx) < 0.02f && std::fabs(f.cz - cz) < 0.02f && std::fabs(f.w - w) < 0.5f && std::fabs(f.l - l) < 0.5f &&
                          std::fabs(f.heading - hd) < 0.002f && (f.oval ? 1 : 0) == ov && std::fabs(f.surface - sf) < 0.01f;
        if (same) continue;
        refit_basin(o, false);
        break;  // one per frame; the world may have changed
    }
}

// The smallest water tile in the catalogue (Env_Water_250_250), else the smallest by name.
static const TemplateInfo* smallest_water(const TemplateCatalog& cat, int* tile = nullptr) {
    const TemplateInfo* best = nullptr;
    int best_n = 1 << 30;
    for (auto& t : cat.all()) {
        if (t.cls_name != "MAWater" || t.model.empty()) continue;
        std::string m = lower(t.model);
        int n = m.find("250") != std::string::npos ? 250 : m.find("500") != std::string::npos ? 500 : m.find("1000") != std::string::npos ? 1000 : 700;
        if (m.find("hesperus") != std::string::npos) n += 1;  // plain water first
        if (n < best_n) { best_n = n; best = &t; }
    }
    if (tile) *tile = best_n >= 1000 ? 1000 : best_n >= 500 ? 500 : 250;
    return best;
}

void App::make_pond(const Vec3& at) {
    if (!editable()) return;
    Terrain& t = doc_->terrain;
    int tile = 250;
    const TemplateInfo* wt = smallest_water(*cat_, &tile);
    if (!wt) { status_ = "No water type found in this game's catalogue."; status_time_ = 5; return; }
    if (!t.inside(at.x, at.z)) return;
    float R = std::max(4.f, pond_radius_);
    // The surface sits just under the lowest rim point so the ground hides the rest of the tile.
    float lo = 1e30f;
    for (int k = 0; k < 24; k++) {
        float a = float(k) / 24 * 6.2831853f;
        float x = std::clamp(at.x + std::cos(a) * R, 0.f, t.size_x() - 0.01f), z = std::clamp(at.z + std::sin(a) * R, 0.f, t.size_z() - 0.01f);
        lo = std::min(lo, t.height_at(x, z));
    }
    float L = lo - 0.3f;
    float depth = std::min(pond_depth_, std::max(0.5f, L - t.min_h));
    push_undo("make pond");
    // The pond is one GroundEdit like a lake's basin: the bowl, and the low spots round it lifted so the tile does not flood them. The
    // ground under another building or flat piece (and units) stays as it is.
    int gx0 = std::max(0, int((at.x - R) / t.scale) - 1), gx1 = std::min(t.w - 1, int((at.x + R) / t.scale) + 2);
    int gz0 = std::max(0, int((at.z - R) / t.scale) - 1), gz1 = std::min(t.h - 1, int((at.z + R) / t.scale) + 2);
    const float half = float(tile) * 0.5f, fade = 20.f, keep = L + 0.3f;  // 0.3 m: no water shows through in the game (Bogged Down test)
    const int fx0 = std::max(0, int((at.x - half - fade) / t.scale) - 1), fx1 = std::min(t.w - 1, int((at.x + half + fade) / t.scale) + 2);
    const int fz0 = std::max(0, int((at.z - half - fade) / t.scale) - 1), fz1 = std::min(t.h - 1, int((at.z + half + fade) / t.scale) + 2);
    GroundEdit e;
    e.gx0 = std::min(gx0, fx0); e.gx1 = std::max(gx1, fx1); e.gz0 = std::min(gz0, fz0); e.gz1 = std::max(gz1, fz1);
    const int enx = e.gx1 - e.gx0 + 1;
    e.want.assign(size_t(enx) * size_t(e.gz1 - e.gz0 + 1), std::numeric_limits<float>::quiet_NaN());
    auto wi = [&](int gx, int gz) { return size_t(gz - e.gz0) * size_t(enx) + size_t(gx - e.gx0); };
    for (int gz = gz0; gz <= gz1; gz++)
        for (int gx = gx0; gx <= gx1; gx++) {
            float dx = gx * t.scale - at.x, dz = gz * t.scale - at.z, d = std::sqrt(dx * dx + dz * dz);
            if (d > R) continue;
            float f = d / R;
            float target = L - depth * (1.f - f * f);
            e.want[wi(gx, gz)] = t.stored(std::clamp(std::min(t.step_at(gx, gz), target), t.min_h, t.max_h));
        }
    // The tile floods any ground in its square that is below the surface, so lift low spots around the pond to just above it (fading
    // out over 20 m past the tile's edge so no wall is left).
    for (int gz = fz0; gz <= fz1; gz++)
        for (int gx = fx0; gx <= fx1; gx++) {
            float dx = gx * t.scale - at.x, dz = gz * t.scale - at.z;
            if (dx * dx + dz * dz <= R * R) continue;  // the basin itself
            float out = std::max(std::fabs(dx), std::fabs(dz)) - half;  // metres outside the tile square
            float w = out <= 0 ? 1.f : std::max(0.f, 1.f - out / fade);
            if (w <= 0) continue;
            // Heights are saved as 8-bit steps (about 0.5 m), so compare and lift by the stored value: ground left a few cm over
            // the surface can round to under it in the game.
            const float h = t.step_at(gx, gz);
            if (h >= keep) continue;
            e.want[wi(gx, gz)] = w >= 1 ? t.stored_at_least(keep) : t.stored(std::clamp(h + (keep - h) * w, t.min_h, t.max_h));
        }
    protect_ground(e, {}, Protect::Everything);
    forecast_ground(e, false);
    int lifted = 0;
    for (int gz = fz0; gz <= fz1; gz++)
        for (int gx = fx0; gx <= fx1; gx++) {
            float dx = gx * t.scale - at.x, dz = gz * t.scale - at.z;
            if (dx * dx + dz * dz > R * R && !std::isnan(e.want[wi(gx, gz)]) && e.want[wi(gx, gz)] > t.step_at(gx, gz) + 1e-4f) lifted++;
        }
    apply_ground_edit(e, "make pond", false);
    io_->bake_lightmap(*doc_);
    r_.set_lightmap(doc_->lightmap);
    // Players and bots wade in water: mark the basin as water ground where it was plain playable ground.
    if (!doc_->typemap.empty()) {
        int W = doc_->typemap_w, H = doc_->typemap_h;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float wx = (x + 0.5f) / W * t.size_x(), wz = (y + 0.5f) / H * t.size_z();
                if ((wx - at.x) * (wx - at.x) + (wz - at.z) * (wz - at.z) > R * R) continue;
                uint16_t& v = doc_->typemap[size_t(y) * W + x];
                if (v == tmtype::Playable) v = tmtype::Special;
            }
        doc_->typemap_dirty = true;
        overlay_dirty_ = true;
    }
    uint32_t root = io_->place_template(*doc_, wt->uid, {at.x, L, at.z}, 0);
    if (root)
        for (auto& o : doc_->world.objs)
            if (o.uid() == root) set_pos(o, {at.x, L, at.z});
    doc_->dirty = true;
    lightmap_stale_ = false;
    stale_sun_only_ = false;
    if (root) selection_ = {root};
    world_changed();
    minimap_dirty_ = true;
    status_ = "Pond made: " + std::to_string(int(R * 2)) + " m wide." +
              (lifted ? " Low ground around it was raised so the water stays in the pond." : "") + " " + kt(A_UNDO) + " undoes.";
    status_time_ = 8;
}

// "Everything in this map": every placed thing that is not part of another (parts hide under their
// root), in collapsible groups by kind with counts. Units carry a dot in their side's colour (the same
// colours as the side rings on the map). Click selects, double-click (or right-click > Show on the map)
// frames it; right-click also renames, duplicates and deletes.
void App::ui_placed_list() {
    if (!doc_) return;
    if (!theme::section("Everything in this map", ImGuiTreeNodeFlags_DefaultOpen)) return;
    static std::string filt;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##placedfilt", "Find (for example: water, tower, Loki)", &filt);
    std::string lf = lower(filt);
    const auto& names = gd_.names();
    static const uint32_t side_cls[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"),
                                        key_of("MATurret"), key_of("MAPerson")};
    struct Row { std::string kind, label; uint32_t uid; Vec3 pos; int nation; bool helper; };
    std::vector<Row> rows;
    size_t total = 0, helpers = 0;
    for (auto& v : views_) {
        if (v.index != v.root) continue;
        const WObj& o = doc_->world.objs[size_t(v.index)];
        // Plain kind names ("Scenery", "Spawn point"), not the engine's ("Corporeal", "LevelStart").
        std::string kind = v.kind == 2 ? "Water" : kind_label(names.label(o.cls));
        if (v.kind != 2 && o.cls == K_LEVELSTART && doc_->multiplayer) kind = "spawn point";
        if (kind == "AI path point" && v.helper == HG_CAMERAS) kind = "camera path point";  // cutscene camera points: their own category
        const bool pad = v.tag.rfind("Pad: ", 0) == 0;  // a vehicle pad (Corporeal > Spawner > vehicle), not scenery
        if (pad) kind = "vehicle pad";
        if (!kind.empty()) kind[0] = char(toupper((unsigned char)kind[0]));
        bool helper = helper_group(o.cls) >= 0;
        total++;
        if (helper) helpers++;
        std::string d = o.description();
        if (d == "NULL") d.clear();  // template placeholder (vehicle pads), not a name
        const TemplateInfo* ti = cat_ ? cat_->find(o.template_id()) : nullptr;
        std::string label = d.empty() ? (pad ? v.tag.substr(5) + " pad" : ti ? ti->name : kind) : d;
        if (v.kind == 2 && ti && o.description().empty()) label = ti->name;
        std::string raw = label;
        label = plain_object_name(label, o.cls == key_of("MAEffect"));  // plain words for code-style names; the raw name still matches the filter
        if (!lf.empty() && lower(label).find(lf) == std::string::npos && lower(raw).find(lf) == std::string::npos && lower(kind).find(lf) == std::string::npos) continue;
        int nation = std::find(std::begin(side_cls), std::end(side_cls), o.cls) != std::end(side_cls) && o.find("Nation") ? o.i32("Nation") : -1;
        rows.push_back({kind, label, o.uid(), v.pos, nation, helper});
    }
    // Things the user placed first (water, then by kind), helpers (trigger areas, paths, markers...) after them.
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.helper != b.helper) return !a.helper;
        if ((a.kind == "Water") != (b.kind == "Water")) return a.kind == "Water";
        return a.kind < b.kind;
    });
    if (total == 0) {
        ImGui::TextWrapped("Nothing placed yet: pick something in the palette above and click the map.");
        return;
    }
    std::string match = lf.empty() ? std::string() : ", " + std::to_string(rows.size()) + " match";
    ImGui::TextDisabled("%zu things (%zu helpers)%s. Click selects (Shift: a range, Ctrl: add or remove), double-click shows it, right-click for more.", total, helpers, match.c_str());
    // Drag the bottom edge to make the list taller.
    ImGui::BeginChild("##placed", ImVec2(0, 260 * dpi_), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    bool mp = doc_->multiplayer;
    auto side_col = [mp](int n) -> ImU32 {  // same colours as the side rings (app.cpp side_color)
        bool red = mp ? n == 1 : n == 5, blue = mp ? n == 2 : n == 6;
        return red ? IM_COL32(255, 90, 90, 255) : blue ? IM_COL32(110, 160, 255, 255) : IM_COL32(200, 200, 200, 255);
    };
    static uint32_t rename_uid = 0;
    static std::string rename_text;
    uint32_t act_uid = 0;
    int act = 0;  // 1 delete, 2 duplicate, 3 show on the map
    std::vector<uint32_t> shown;  // the rows drawn, top to bottom: a Shift+click selects the range from the anchor row
    uint32_t range_to = 0;
    bool helper_heading = false;
    for (size_t i = 0; i < rows.size();) {
        size_t j = i;
        while (j < rows.size() && rows[j].kind == rows[i].kind && rows[j].helper == rows[i].helper) j++;
        if (rows[i].helper && !helper_heading) { ImGui::SeparatorText("Helpers (not shown in the game)"); helper_heading = true; }
        std::string head = rows[i].kind + " (" + std::to_string(j - i) + ")###grp" + rows[i].kind;
        if (!lf.empty()) ImGui::SetNextItemOpen(true);
        bool open = ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | (j - i <= 8 ? ImGuiTreeNodeFlags_DefaultOpen : 0));
        if (open) {
            int n = 0;
            for (size_t k = i; k < j; k++) {
                const Row& r = rows[k];
                ImGui::PushID(int(r.uid));
                if (r.nation >= 0) {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float h = ImGui::GetTextLineHeight();
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.55f), h * 0.3f, side_col(r.nation));
                    ImGui::Dummy(ImVec2(h * 0.8f, h));
                    ImGui::SameLine(0, 2);
                }
                bool sel = selection_.count(r.uid) != 0;
                if (rename_uid == r.uid) {
                    ImGui::SetNextItemWidth(-1);
                    if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                    if (ImGui::InputText("##rn", &rename_text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                        for (auto& o : doc_->world.objs)
                            if (o.uid() == r.uid) { push_undo("rename"); o.set_str("Description", rename_text); world_changed(); break; }
                        rename_uid = 0;
                    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered())) {
                        rename_uid = 0;
                    }
                } else if (ImGui::Selectable(r.label.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                    // Like a file list: Ctrl toggles a row, Shift selects the rows from the last clicked one, a plain click selects just this row.
                    if (ImGui::GetIO().KeyCtrl) { if (sel) selection_.erase(r.uid); else selection_.insert(r.uid); list_anchor_ = r.uid; }
                    else if (ImGui::GetIO().KeyShift && list_anchor_) range_to = r.uid;  // applied below, when the rows after this one are known
                    else { selection_ = {r.uid}; list_anchor_ = r.uid; }
                }
                shown.push_back(r.uid);
                // The Selectable reports the click on release, when IsMouseDoubleClicked is already false.
                if (rename_uid != r.uid && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { selection_ = {r.uid}; act_uid = r.uid; act = 3; }
                if (rename_uid != r.uid && sel && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) { rename_uid = r.uid; rename_text = r.label; }
                if (ImGui::BeginPopupContextItem("##rowmenu")) {
                    if (!sel) selection_ = {r.uid};
                    if (ImGui::MenuItem("Show on the map")) { act_uid = r.uid; act = 3; }
                    if (ImGui::MenuItem("Rename", "F2")) { rename_uid = r.uid; rename_text = r.label; }
                    if (ImGui::MenuItem(act_text(A_DUPLICATE).c_str(), kt(A_DUPLICATE).c_str())) { act_uid = r.uid; act = 2; }
                    ImGui::Separator();
                    if (ImGui::MenuItem(act_text(A_DELETE).c_str(), kt(A_DELETE).c_str())) { act_uid = r.uid; act = 1; }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                if (++n >= 400) { ImGui::TextDisabled("... type in Find to see the rest."); break; }
            }
            ImGui::TreePop();
        }
        i = j;
    }
    ImGui::EndChild();
    if (range_to) {
        const std::vector<uint32_t> rng = list_range(shown, list_anchor_, range_to);
        selection_ = std::set<uint32_t>(rng.begin(), rng.end());
    }
    // Acted on after the list: these change the world (and views_) the rows were built from.
    if (act && act_uid) {
        selection_ = {act_uid};
        if (act == 1) delete_selection();
        else if (act == 2) duplicate_selected();
        else zoom_to_selection();
    }
}

}  // namespace ed
