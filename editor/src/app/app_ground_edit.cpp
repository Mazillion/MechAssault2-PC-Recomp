// Ground tools: one way to carry what stands on the ground, one protection rule, one preview.
//
//   - Every tool that moves the ground (the sculpt brushes, Fit ground to road, the height range) takes `ground_standing()`
//     first and calls `ground_changed()` after. The objects move by the change of the surface under them, worked out from the
//     values taken before, so asking again (every frame of a stroke) cannot add up twice.
//   - Fit ground (and Level, water basins) never change the ground under another building or flat piece: `protect_ground`
//     takes those grid points out of the plan, and `draw_ground_edit` shows the plan before the click.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include "app.h"
#include "../core/triggers.h"
#include "imgui.h"
#include "theme.h"
#include "ui_tips.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

namespace {
const float kNaN = std::numeric_limits<float>::quiet_NaN();
const uint32_t K_BUILDING_G = key_of("MABuilding"), K_MECH_G = key_of("MAMech"), K_TANK_G = key_of("MATank"), K_TURRET_G = key_of("MATurret"),
               K_START_G = key_of("MALevelStart"), K_ROAD_G2 = key_of("MARoad");
}  // namespace

// Everything that stands on the ground or on a flat piece, map wide, as it is now; a flat piece that keeps its height (a tilted
// road, an old file's road) is listed too, with the highest ground under it, so it moves with the ground as one rigid piece.
std::vector<App::Standing> App::ground_standing() {
    if (!doc_) return {};
    const SheetSet ss = sheets(true);
    std::vector<Standing> r = standing_now(ss, true);
    const Terrain& t = doc_->terrain;
    for (const place::Sheet& s : ss.s) {
        if (!s.fixed) continue;
        const float gm = place::ground_max(t, s), under = s.pos.y + s.under;
        if (under - gm > 3.f || under - gm < -30.f) continue;
        r.push_back({s.uid, s.pos.x, s.pos.z, s.pos.y, 0.f, gm, 0, true});
    }
    // Pieces the edit is made for (Fit ground to road) do not follow it, and neither does what stands on them: the road is the fixed thing the
    // ground is fitted to (a tilted road used to ride up with the ground it had just raised and float more with every press).
    if (!ground_still_.empty())
        r.erase(std::remove_if(r.begin(), r.end(), [&](const Standing& b) { return ground_still_.count(b.uid) || (b.on0 && ground_still_.count(b.on0)); }), r.end());
    return r;
}

// The ground (or a flat piece's own ground) moved: fixed pieces follow first, then the stack seats itself on the new ground and
// everything standing follows its surface. Returns how many objects moved.
int App::ground_changed(const std::vector<Standing>& before) {
    if (!doc_) return 0;
    std::vector<Standing> fixed, rest;
    for (const Standing& b : before) (b.fixed_sheet ? fixed : rest).push_back(b);
    int moved = 0;
    if (!fixed.empty()) {
        const SheetSet s0 = sheets(true);
        moved += follow(fixed, s0);
    }
    SheetSet s1 = sheets(true);
    if (s1.s.empty()) moved += follow(rest, s1);
    else {
        reseat_sheets(&s1, &rest);
        moved += reseat_moved_ + follow_moved_;
    }
    if (moved) { doc_->dirty = true; objects_dirty_ = true; }
    return moved;
}

bool App::object_footprint(const WObj& o, place::Sheet& s) {
    if (const int k = sheet_kind(o)) return sheet_geom(o, k, s);
    Vec3 p;
    if (!get_pos(o, p)) return false;
    GpuModel* gm = obj_model(o);
    if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
    if (!gm || gm->state != GpuModel::Ready || gm->bmax[0] <= gm->bmin[0]) return false;
    s = place::Sheet{};
    s.uid = o.uid();
    s.pos = p;
    s.yaw = heading(o);
    get_tilt(o, s.pitch, s.roll);
    s.fp = {gm->bmin[0], gm->bmax[0], gm->bmin[2], gm->bmax[2]};
    s.under = gm->bmin[1];
    s.top = gm->bmax[1];
    return true;
}

// Takes out of the plan every grid point under (and one cell around) a building or a flat piece that rests on the ground, unless it
// is one of the pieces the tool works for (or overlaps one of them: it rides with that piece). `Everything` (water basins) also keeps
// units, turrets and the player starts.
void App::protect_ground(GroundEdit& e, const std::set<uint32_t>& own, Protect mode, const std::vector<place::Sheet>* extra_own) {
    if (!doc_ || e.empty()) return;
    const Terrain& t = doc_->terrain;
    std::vector<place::Sheet> mine;
    for (uint32_t u : own)
        if (const WObj* o = object_by_uid(doc_->world, u))
            if (place::Sheet s; object_footprint(*o, s)) mine.push_back(s);
    if (extra_own) mine.insert(mine.end(), extra_own->begin(), extra_own->end());
    WTree wt;
    wt.build(doc_->world);
    const int nx = e.gx1 - e.gx0 + 1;
    for (size_t i = 0; i < doc_->world.objs.size(); i++) {
        const WObj& o = doc_->world.objs[i];
        if (own.count(o.uid()) || o.cls == K_WATER) continue;
        Vec3 p, q;
        if (!get_pos(o, p)) continue;
        bool part = false;
        for (int a = wt.parent_of[i]; a >= 0 && !part; a = wt.parent_of[size_t(a)]) part = get_pos(doc_->world.objs[size_t(a)], q);
        if (part) continue;
        const int kind = (o.cls == K_ROAD_G2 || is_foundation_obj(o)) ? sheet_kind(o) : 0;
        const bool unit = o.cls == K_MECH_G || o.cls == K_TANK_G || o.cls == K_TURRET_G || o.cls == K_START_G;
        if (!kind && o.cls != K_BUILDING_G && !(mode == Protect::Everything && unit)) continue;
        place::Sheet s;
        if (!object_footprint(o, s)) {
            if (!(mode == Protect::Everything && unit)) continue;
            s = place::Sheet{};  // no model: a small square round its spot
            s.uid = o.uid();
            s.pos = p;
            s.fp = {-2.f, 2.f, -2.f, 2.f};
        }
        // A road piece joined end to end to one of ours: its own footprint is kept, but not the margin round it, which
        // ran on under our piece (a lower slope kept for 6 m past the joint = a float). Cells under our own footprint are ours.
        // Decided first (F-V5R-2): a joined road is the fixed thing the new piece meets whatever else is true of it. On a cross
        // slope it failed "resting on the ground" below, or counted as one of ours (it touches our footprint), was skipped there and
        // never reached e.joined: the ground fit then carried it up with the hill and the joint got a step (0.5 m on a 24 deg roll).
        bool joined = false;
        if (kind == 1)
            for (const place::Sheet& m : mine) joined |= m.kind == 1 && place::overlap(s, m, -0.6f);
        // Resting on the ground (a sheet in the air is no neighbour of the ground below it).
        if (kind) {
            const float under = s.pos.y + s.under - place::ground_max(t, s);
            if (!joined && (under > 3.f || under < -30.f)) continue;
        } else {
            float x, z;
            stand_xz(o, p, x, z);
            const float foot = p.y - stand_foot(o), g = t.height_at(x, z);
            if (foot - g > 1.5f || foot - g < -30.f) continue;
        }
        bool works_for = false;
        for (const place::Sheet& m : mine) works_for |= place::overlap(s, m);
        if (works_for && !joined) continue;
        if (joined) e.joined.push_back(o.uid());
        int removed = 0;
        for (auto [gx, gz] : place::grid_under(s, t.scale, joined ? 0 : 1)) {
            if (gx < e.gx0 || gx > e.gx1 || gz < e.gz0 || gz > e.gz1) continue;
            if (joined) {
                bool in_mine = false;
                for (const place::Sheet& m : mine) in_mine |= place::contains(m, float(gx) * t.scale, float(gz) * t.scale);
                if (in_mine) continue;
            }
            float& w = e.want[size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0)];
            if (!std::isnan(w)) { w = kNaN; removed++; }
        }
        if (!removed) continue;
        e.kept.push_back(o.uid());
        float cx[4], cz[4];
        place::corners(s, cx, cz);
        for (int k = 0; k < 4; k++) { e.kept_xz.push_back(cx[k]); e.kept_xz.push_back(cz[k]); }
    }
}

// How far the drawn ground would rise and fall, and how many objects would move with it. Nothing is written.
void App::forecast_ground(GroundEdit& e, bool with_followers) {
    e.up = e.down = 0;
    e.followers = 0;
    if (!doc_ || e.empty()) return;
    const Terrain& t = doc_->terrain;
    Terrain tmp;
    if (with_followers && settings_.objects_follow_terrain) tmp = t;
    const int nx = e.gx1 - e.gx0 + 1;
    bool any = false;
    for (int gz = e.gz0; gz <= e.gz1; gz++)
        for (int gx = e.gx0; gx <= e.gx1; gx++) {
            const float w = e.want[size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0)];
            if (std::isnan(w)) continue;
            const float d = t.stored(std::clamp(w, t.min_h, t.max_h)) - t.step_at(gx, gz);
            e.up = std::max(e.up, d);
            e.down = std::max(e.down, -d);
            if (with_followers && settings_.objects_follow_terrain) tmp.ref(gx, gz) = w;
            any = true;
        }
    if (!any || !with_followers || !settings_.objects_follow_terrain) return;
    const SheetSet ss = sheets(true);
    std::unordered_map<uint32_t, size_t> by_uid;
    std::set<uint32_t> counted;
    for (size_t i = 0; i < ss.s.size(); i++) by_uid[ss.s[i].uid] = i;
    auto sheet_moves = [&](uint32_t uid) {
        auto it = by_uid.find(uid);
        return it != by_uid.end() && std::fabs(place::ground_max(tmp, ss.s[it->second]) - place::ground_max(t, ss.s[it->second])) > 1e-4f;
    };
    for (const Standing& b : ground_standing()) {
        bool moves;
        if (b.fixed_sheet || b.on0) moves = sheet_moves(b.fixed_sheet ? b.uid : b.on0);
        else moves = std::fabs(tmp.height_at(b.x, b.z) - b.surface0) > 1e-4f;
        if (moves) { e.followers++; counted.insert(b.uid); }
    }
    for (const place::Sheet& s : ss.s)
        if (!s.fixed && !counted.count(s.uid) && !ground_still_.count(s.uid) && sheet_moves(s.uid)) e.followers++;
}

int App::apply_ground_edit(const GroundEdit& e, const char* undo_label, bool push) {
    if (!doc_ || !editable() || e.empty()) return 0;
    if (push) push_undo(undo_label);
    std::vector<Standing> before;
    if (settings_.objects_follow_terrain) before = ground_standing();
    Terrain& t = doc_->terrain;
    const int nx = e.gx1 - e.gx0 + 1;
    for (int gz = e.gz0; gz <= e.gz1; gz++)
        for (int gx = e.gx0; gx <= e.gx1; gx++) {
            const float w = e.want[size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0)];
            if (!std::isnan(w)) t.ref(gx, gz) = std::clamp(w, t.min_h, t.max_h);
        }
    r_.update_terrain_rect(t, e.gx0, e.gz0, e.gx1, e.gz1);
    doc_->dirty = true;
    lightmap_stale_ = true;
    stale_sun_only_ = false;
    objects_dirty_ = true;
    if (show_steep_) overlay_dirty_ = true;
    return settings_.objects_follow_terrain ? ground_changed(before) : 0;
}

namespace {
std::string desc_of(const WObj& o) {
    std::string d = o.description();
    return d.empty() ? std::string("a piece") : d;
}
}  // namespace

// "Raises the ground up to 3.2 m, lowers it up to 1.1 m. 5 objects move with it. The ground under 2 buildings stays."
std::string App::ground_edit_note(const GroundEdit& e) {
    char b[400];
    std::string s;
    if (e.up < 0.005f && e.down < 0.005f) s = "The ground here already fits.";
    else {
        if (e.up >= 0.005f && e.down >= 0.005f) snprintf(b, sizeof b, "Raises the ground up to %.1f m, lowers it up to %.1f m.", e.up, e.down);
        else if (e.up >= 0.005f) snprintf(b, sizeof b, "Raises the ground up to %.1f m.", e.up);
        else snprintf(b, sizeof b, "Lowers the ground up to %.1f m.", e.down);
        s = b;
    }
    if (settings_.objects_follow_terrain) {
        if (e.followers > 0) { snprintf(b, sizeof b, " %d object%s move with it.", e.followers, e.followers == 1 ? "" : "s"); s += b; }
    } else s += " Objects stay where they are (\"Objects follow the ground\" is off).";
    if (!e.kept.empty()) { snprintf(b, sizeof b, " The ground under %zu building%s or flat piece%s stays.", e.kept.size(), e.kept.size() == 1 ? "" : "s", e.kept.size() == 1 ? "" : "s"); s += b; }
    return s;
}

// The plan, over the map: the grid cells that will change (yellow) and a white outline round each object whose ground is left alone.
void App::draw_ground_edit(const GroundEdit& e) {
    if (!doc_ || e.empty()) return;
    const Terrain& t = doc_->terrain;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    const int nx = e.gx1 - e.gx0 + 1;
    auto changed = [&](int gx, int gz) {
        if (gx < e.gx0 || gx > e.gx1 || gz < e.gz0 || gz > e.gz1) return false;
        const float w = e.want[size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0)];
        return !std::isnan(w) && std::fabs(t.stored(std::clamp(w, t.min_h, t.max_h)) - t.step_at(gx, gz)) > 1e-4f;
    };
    // A cell is "in" when any corner changes; draw the edges that border a cell that is not.
    auto cell_in = [&](int cx, int cz) { return changed(cx, cz) || changed(cx + 1, cz) || changed(cx, cz + 1) || changed(cx + 1, cz + 1); };
    auto line = [&](float x0, float z0, float x1, float z1, ImU32 col, float th) {
        float ax, ay, bx, by;
        if (!cam_.project({x0, t.height_at(x0, z0) + 0.15f, z0}, vp_w, vp_h, ax, ay)) return;
        if (!cam_.project({x1, t.height_at(x1, z1) + 0.15f, z1}, vp_w, vp_h, bx, by)) return;
        dl->AddLine(ImVec2(ax + vp_x, ay + vp_y), ImVec2(bx + vp_x, by + vp_y), col, th * dpi_);
    };
    const ImU32 yellow = IM_COL32(255, 220, 60, 235);
    for (int cz = e.gz0 - 1; cz <= e.gz1; cz++)
        for (int cx = e.gx0 - 1; cx <= e.gx1; cx++) {
            if (!cell_in(cx, cz)) continue;
            const float x0 = float(cx) * t.scale, x1 = x0 + t.scale, z0 = float(cz) * t.scale, z1 = z0 + t.scale;
            if (!cell_in(cx, cz - 1)) line(x0, z0, x1, z0, yellow, 2.f);
            if (!cell_in(cx, cz + 1)) line(x0, z1, x1, z1, yellow, 2.f);
            if (!cell_in(cx - 1, cz)) line(x0, z0, x0, z1, yellow, 2.f);
            if (!cell_in(cx + 1, cz)) line(x1, z0, x1, z1, yellow, 2.f);
        }
    const ImU32 white = IM_COL32(255, 255, 255, 245);
    for (size_t k = 0; k + 7 < e.kept_xz.size(); k += 8)
        for (int c = 0; c < 4; c++) {
            const int d = (c + 1) & 3;
            // subdivide so the outline follows the ground
            for (int m = 0; m < 6; m++) {
                const float a = float(m) / 6.f, b = float(m + 1) / 6.f;
                const float ax = e.kept_xz[k + size_t(c) * 2], az = e.kept_xz[k + size_t(c) * 2 + 1], bx = e.kept_xz[k + size_t(d) * 2], bz = e.kept_xz[k + size_t(d) * 2 + 1];
                line(ax + (bx - ax) * a, az + (bz - az) * a, ax + (bx - ax) * b, az + (bz - az) * b, white, 2.f);
            }
        }
    dl->PopClipRect();
}

std::string App::fit_summary_names(const GroundEdit& e) {
    if (e.kept.empty()) return {};
    const WObj* first = object_by_uid(doc_->world, e.kept[0]);
    std::string s = first ? desc_of(*first) : "a piece";
    if (e.kept.size() > 1) s += " and " + std::to_string(e.kept.size() - 1) + " other" + (e.kept.size() > 2 ? "s" : "");
    return s;
}

}  // namespace ed
