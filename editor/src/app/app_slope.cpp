// Slopes (core/placement.h fit_tilt). A flat piece (road piece or base, "sheet": app_sheets.cpp) placed where the ground
// slopes under it cannot lie flat on it: one side is buried or the other floats. Three ways out, chosen before the click:
//   Level  - shape the ground under it (and a 14 m blend round it) to one height; things standing there ride along, buildings and
//            flat pieces beside it keep their ground (app_ground_edit.cpp). The default for bases.
//   Tilt   - lay the piece on the slope (road pieces: it was written for them). A piece joined to another keeps that joint. Default for roads.
//   Float  - level, on the highest ground (what the editor always did).
// The plan is computed every frame for the ghost and again by place_object, so the click gives what the ghost showed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "app.h"
#include "../core/triggers.h"
#include "imgui.h"
#include "theme.h"
#include "tool_hints.h"
#include "ui_tips.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

namespace {
float floor_step(const Terrain& t, float v) {  // the step at or below v
    float q = t.stored(v);
    if (q > v + 1e-4f) q -= t.step();
    return std::max(q, t.min_h);
}
constexpr float kSlopeOverlapMargin = 8.f;  // each footprint is shrunk by half of it before "over another flat piece" is asked
Vec3 centre_of(const place::Sheet& s) {
    float x[4], z[4];
    place::corners(s, x, z);
    return {0.25f * (x[0] + x[1] + x[2] + x[3]), 0.f, 0.25f * (z[0] + z[1] + z[2] + z[3])};
}
}  // namespace

SlopeMode& App::slope_mode_ref() { return place_road_.empty() ? settings_.slope_plate : settings_.slope_road; }

void App::cycle_slope_mode() {
    place::Sheet g;
    bool plate = false;
    if (tool_ != Tool::PlaceObject || !ghost_sheet({0, 0, 0}, g, plate)) return;  // only while placing a flat piece
    SlopeMode& m = slope_mode_ref();
    m = SlopeMode((int(m) + 1) % 3);
    settings_.save();
}

// The ground that levels the pieces to height T: full height one cell (+0.5 m) past each, then the 14 m blend of Fit ground.
// (A cell, not half a cell as Fit ground: every grid point of every cell the piece touches must be at T, or the piece would
// rest on a stray blended point at its edge.)
App::GroundEdit App::plan_level(const std::vector<place::Sheet>& pieces, float T) {
    GroundEdit e;
    if (!doc_) return e;
    std::vector<GroundSurf> surfs;
    for (const place::Sheet& s : pieces) {
        GroundSurf g;
        float x[4], z[4];
        place::corners(s, x, z);
        for (int i = 0; i < 4; i++) g.c[i] = {x[i], T + place::kSheetLift, z[i]};
        const Vec3 c = centre_of(s);
        g.p = {c.x, T + place::kSheetLift, c.z};
        g.n = {0.f, 1.f, 0.f};
        surfs.push_back(g);
    }
    return plan_surfaces(surfs, doc_->terrain.scale + 0.5f);
}

App::SlopePlan App::plan_slope(const Vec3& origin) {
    SlopePlan p;
    if (!doc_ || !cat_ || !editable() || tool_ != Tool::PlaceObject) return p;
    place::Sheet g;
    bool plate = false;
    if (!ghost_sheet(origin, g, plate)) return p;
    const Terrain& t = doc_->terrain;
    p.plate = plate;
    p.joined = plate ? piece_snapped_ : road_snapped_;
    p.gap = place::float_gap(t, g);
    p.pos = {origin.x, 0.f, origin.z};
    if (p.gap <= place::float_tol(t)) return p;  // flat enough: placing is exactly the stack rule
    const SheetSet ss = sheets(plate && piece_snapped_);
    // On top of another flat piece: its stack decides the height. A road's footprint includes its kerbs and shoulders, so a margin of a few
    // metres keeps a base that only reaches the shoulder from losing its slope plan.
    for (const place::Sheet& s : ss.s)
        if (place::overlap(s, g, kSlopeOverlapMargin)) return p;
    p.active = true;
    p.wanted = slope_mode_ref();
    p.mode = p.wanted;
    const bool joint = !plate && road_snapped_ && road_joint_ok_;
    bool can_tilt_piece = true;
    if (plate) {
        const WObj* tpl = cat_->object(place_template_);
        can_tilt_piece = tpl && can_tilt(*tpl) && !piece_snapped_;  // a plate in a run stays level with it
    }
    if (p.mode == SlopeMode::Tilt && !can_tilt_piece) { p.mode = SlopeMode::Float; p.fell_back = true; }

    if (p.mode == SlopeMode::Tilt) {
        g.pos = {origin.x, joint ? road_joint_y_ : 0.f, origin.z};
        if (joint) {
            // The piece turns on the end it joins by (its entry, or the exit the solver chose).
            const float px = road_joint_px_, pz = road_joint_pz_;
            place::fit_tilt(t, g, true, road_joint_roll_, px, pz);
        } else {
            place::fit_tilt(t, g, false, 0.f);
        }
        p.pitch = g.pitch;
        p.roll = g.roll;
        p.pos = g.pos;
        p.y = g.pos.y;
        p.left = place::float_gap(t, g);
        p.sheet = g;
        p.fits_ground = !plate;  // a road piece: the click shapes the ground to it, so nothing floats afterwards
        return p;
    }
    if (p.mode == SlopeMode::Level) {
        const Vec3 c = centre_of(g);
        float T;
        if (joint) {
            T = floor_step(t, road_joint_y_ - place::kSheetLift);
        } else if (plate && piece_snapped_ && piece_snap_obj_ >= 0) {
            T = t.stored(t.height_at(c.x, c.z));
            for (size_t j = 0; j < ss.s.size(); j++)
                if (ss.obj[j] == piece_snap_obj_) T = floor_step(t, ss.s[j].pos.y + ss.s[j].under - place::kSheetLift);
        } else {
            T = t.stored(t.height_at(c.x, c.z));
        }
        p.T = T;
        g.pos = {origin.x, 0.f, origin.z};
        p.pieces = {g};
        p.edit = plan_level(p.pieces, T);
        protect_ground(p.edit, {}, Protect::OthersOnly, &p.pieces);
        forecast_ground(p.edit, false);
        p.up = p.edit.up;
        p.down = p.edit.down;
        // The height the piece will rest at on the ground as planned (the same functions as the click).
        Terrain tmp = t;
        {
            const int nx = p.edit.gx1 - p.edit.gx0 + 1;
            for (int gz = p.edit.gz0; gz <= p.edit.gz1; gz++)
                for (int gx = p.edit.gx0; gx <= p.edit.gx1; gx++) {
                    const float w = p.edit.want[size_t(gz - p.edit.gz0) * size_t(nx) + size_t(gx - p.edit.gx0)];
                    if (!std::isnan(w)) tmp.ref(gx, gz) = std::clamp(w, t.min_h, t.max_h);
                }
        }
        float y = 0;
        if (joint) y = road_joint_y_;
        else if (!sheet_ghost_y(origin, y, &tmp)) y = place::ground_max(tmp, g) + place::kSheetLift - g.under;
        // Ground left alone beside the piece (a road's strip, a building's) can stand higher than the plate that was levelled.
        // The piece rests on the plate, the way the level says, and that ground covers its edge; it must not hang up on it (6.5 m in the J1 map).
        if (!joint && !(plate && piece_snapped_) && !p.edit.kept.empty()) {
            const float s0 = t.scale;
            float plate_h = -1e30f;
            for (int gz = p.edit.gz0; gz <= p.edit.gz1; gz++)
                for (int gx = p.edit.gx0; gx <= p.edit.gx1; gx++) {
                    const float w = p.edit.want[size_t(gz - p.edit.gz0) * size_t(p.edit.gx1 - p.edit.gx0 + 1) + size_t(gx - p.edit.gx0)];
                    if (!std::isnan(w) && place::contains(g, float(gx) * s0, float(gz) * s0)) plate_h = std::max(plate_h, tmp.at(gx, gz));
                }
            if (plate_h > -1e29f) {
                const float yp = plate_h + place::kSheetLift - g.under;
                if (yp < y - 0.05f) { y = yp; p.pin_y = true; }
            }
        }
        p.y = y;
        p.pos = {origin.x, y, origin.z};
        g.pos.y = y;
        p.sheet = g;
        // What hangs and what is covered (the piece is level at y): a neighbour's protected ground can leave part of it over a slope.
        const float under_y = y + g.under;
        p.left = std::max(0.f, under_y - place::kSheetLift - place::ground_min(tmp, g));
        p.buried = std::max(0.f, place::ground_max(tmp, g) + place::kSheetLift - under_y);
        return p;
    }
    // Float: level on the highest ground, as before. A road joined to another keeps the joint's height instead: left on the highest
    // ground it sat 6.75 m above the road it was joined to, so the "joined" ends had a step.
    float y = 0;
    if (joint) y = road_joint_y_;
    else if (!sheet_ghost_y(origin, y)) y = place::ground_max(t, g) + place::kSheetLift - g.under;
    p.y = y;
    p.pos = {origin.x, y, origin.z};
    g.pos.y = y;
    p.sheet = g;
    p.left = p.gap;
    if (joint) {  // kept level at the joint: the low side may hang, the high side may be under the ground (neither is the full gap)
        p.joint_kept = true;
        const float under_y = y + g.under;
        p.gap = p.left = std::max(0.f, under_y - place::kSheetLift - place::ground_min(t, g));
        p.buried = std::max(0.f, place::ground_max(t, g) + place::kSheetLift - under_y);
    }
    return p;
}

// Level, before the piece is placed: the ground takes the planned shape (inside the placing's undo step), what stood on it follows.
void App::apply_slope_ground(const SlopePlan& p) {
    slope_moved_ = 0;
    slope_kept_.clear();
    if (!p.active || p.mode != SlopeMode::Level || p.edit.empty()) return;
    for (uint32_t j : p.edit.joined) ground_still_.insert(j);  // A road joined to this piece keeps its place, so the joint has no step
    slope_moved_ = apply_ground_edit(p.edit, "level the ground", false);
    ground_still_.clear();
    if (!p.edit.kept.empty()) slope_kept_ = fit_summary_names(p.edit);
    if (!settings_.objects_follow_terrain) reseat_sheets();
}

// Tilt, after the piece is placed: it takes the planned pose (origin and tilt) and keeps it (the caller marks it fixed).
void App::apply_slope_pose(uint32_t root, const SlopePlan& p) {
    WObj* o = object_by_uid(doc_->world, root);
    if (!o || !p.active || p.mode != SlopeMode::Tilt) return;
    Vec3 cur;
    if (!get_pos(*o, cur)) return;
    const Vec3 d{p.pos.x - cur.x, p.pos.y - cur.y, p.pos.z - cur.z};
    WTree t;
    t.build(doc_->world);
    std::vector<int> sub;
    if (int ri = t.find(root); ri >= 0) t.subtree(ri, sub);
    for (int k : sub) {
        Vec3 q;
        if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x + d.x, q.y + d.y, q.z + d.z});
    }
    set_tilt(*o, p.pitch, p.roll);
}

// Level, ground left alone beside the piece stands higher than the plate: the piece (and what is glued to it) goes to the plate's height.
void App::lower_to_plate(uint32_t root, float y) {
    WObj* o = object_by_uid(doc_->world, root);
    Vec3 cur;
    if (!o || !get_pos(*o, cur) || std::fabs(y - cur.y) < 1e-4f) return;
    const float dy = y - cur.y;
    WTree t;
    t.build(doc_->world);
    std::vector<int> sub;
    if (int ri = t.find(root); ri >= 0) t.subtree(ri, sub);
    for (int k : sub) {
        Vec3 q;
        if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x, q.y + dy, q.z});
    }
}

std::string App::slope_status(const SlopePlan& p, int moved) {
    char b[300];
    std::string s;
    if (p.mode == SlopeMode::Level) {
        if (p.up >= 0.005f && p.down >= 0.005f) snprintf(b, sizeof b, "Levelled the ground under it (up to +%.1f / -%.1f m).", p.up, p.down);
        else if (p.up >= 0.005f) snprintf(b, sizeof b, "Levelled the ground under it (raised up to %.1f m).", p.up);
        else if (p.down >= 0.005f) snprintf(b, sizeof b, "Levelled the ground under it (lowered up to %.1f m).", p.down);
        else snprintf(b, sizeof b, "The ground under it was already level.");
        s = b;
        if (p.left > 0.3f) { snprintf(b, sizeof b, " The ground next to another piece stays, so %.1f m of it still floats.", p.left); s += b; }
        if (settings_.objects_follow_terrain) {
            if (moved > 0) { snprintf(b, sizeof b, " %d object%s moved with it.", moved, moved == 1 ? "" : "s"); s += b; }
        } else s += " Objects stayed where they were (\"Objects follow the ground\" is off).";
        if (!slope_kept_.empty()) s += " The ground under " + slope_kept_ + " was left alone.";
        if (p.buried > 0.3f) { snprintf(b, sizeof b, " Part of it ends under that ground (up to %.1f m).", p.buried); s += b; }
    } else if (p.mode == SlopeMode::Tilt) {
        snprintf(b, sizeof b, "Tilted %.0f deg to lie on the slope.", std::fabs(p.pitch) > std::fabs(p.roll) ? std::fabs(p.pitch) * 57.2958f : std::fabs(p.roll) * 57.2958f);
        s = b;
        if (p.fits_ground) {
            if (p.fit_up >= 0.005f || p.fit_down >= 0.005f) { snprintf(b, sizeof b, " Shaped the ground under it to meet it (up to +%.1f / -%.1f m).", p.fit_up, p.fit_down); s += b; }
            // (the ground moves in steps, so up to one step of air under the road is the fit itself, not worth a word)
            if (p.fit_left > std::max(0.3f, (doc_ ? doc_->terrain.step() : 0.5f) * 1.2f)) { snprintf(b, sizeof b, " %.1f m of it still floats where the ground cannot be shaped up to it.", p.fit_left); s += b; }
            if (p.fit_moved > 0) { snprintf(b, sizeof b, " %d object%s moved with the ground.", p.fit_moved, p.fit_moved == 1 ? "" : "s"); s += b; }
        } else if (p.left > 0.05f) { snprintf(b, sizeof b, " %.1f m of it still floats: Properties > Fit ground to road fixes that.", p.left); s += b; }
    } else if (p.joint_kept) {
        snprintf(b, sizeof b, "Kept level with the road it joins.");
        s = b;
        if (p.left > 0.3f) { snprintf(b, sizeof b, " The low side floats %.1f m.", p.left); s += b; }
        if (p.buried > 0.3f) { snprintf(b, sizeof b, " Up to %.1f m of it is under the ground.", p.buried); s += b; }
    } else {
        snprintf(b, sizeof b, p.fell_back ? "This piece cannot tilt: it floats %.1f m on one side." : "Left floating: one side is %.1f m above the ground.", p.gap);
        s = b;
    }
    return s + " " + kt(A_UNDO) + " undoes it.";
}

// Float: four thin lines from the underside's corners to the ground, and the gap at the worst corner.
void App::draw_slope_ghost() {
    if (!doc_ || !slope_plan_.active || slope_plan_.mode != SlopeMode::Float || !preview_on_) return;
    const Terrain& t = doc_->terrain;
    const place::Sheet& s = slope_plan_.sheet;
    float cx[4], cz[4];
    place::corners(s, cx, cz);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    const ImU32 col = IM_COL32(255, 200, 60, 240);
    float worst = -1, wx = 0, wy = 0;
    for (int i = 0; i < 4; i++) {
        const float top = s.pos.y + s.under, ground = t.inside(cx[i], cz[i]) ? t.height_at(cx[i], cz[i]) : top;
        float ax, ay, bx, by;
        if (!cam_.project({cx[i], top, cz[i]}, vp_w, vp_h, ax, ay) || !cam_.project({cx[i], ground, cz[i]}, vp_w, vp_h, bx, by)) continue;
        dl->AddLine(ImVec2(ax + vp_x, ay + vp_y), ImVec2(bx + vp_x, by + vp_y), col, 1.5f * dpi_);
        if (top - ground - place::kSheetLift > worst) { worst = top - ground - place::kSheetLift; wx = (ax + bx) * 0.5f + vp_x; wy = (ay + by) * 0.5f + vp_y; }
    }
    if (worst > 0.05f) {
        char b[32];
        snprintf(b, sizeof b, "%.1f m", worst);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddRectFilled(ImVec2(wx - 3 * dpi_, wy - ts.y * 0.5f - 1), ImVec2(wx + ts.x + 3 * dpi_, wy + ts.y * 0.5f + 1), IM_COL32(0, 0, 0, 170), 3.f);
        dl->AddText(ImVec2(wx, wy - ts.y * 0.5f), IM_COL32(255, 220, 90, 255), b);
    }
    dl->PopClipRect();
}

// ---------------------------------------------------------------------------------------------------------------------
// The placing options: "On a slope:" with the three choices (only while the thing being placed is a flat piece).

void App::slope_placing_row() {
    place::Sheet g;
    bool plate = false;
    if (tool_ != Tool::PlaceObject || !ghost_sheet({0, 0, 0}, g, plate)) return;
    SlopeMode& m = slope_mode_ref();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("On a slope:");
    ImGui::SameLine();
    int v = int(m);
    bool changed = false;
    changed |= ImGui::RadioButton("Level the ground##slp0", &v, 0);
    ImGui::SetItemTooltip("Shapes the ground under the piece to one height (and blends it into the ground round it); buildings and flat pieces beside it keep their ground. %s undoes the piece and the ground together.", kt(A_UNDO).c_str());
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Tilt##slp1", &v, 1);
    ImGui::SetItemTooltip("Lays the piece on the slope. A piece joined to another keeps the joint. Units walk on the ground under it, not on it (use Fit ground to road for that).");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Float##slp2", &v, 2);
    ImGui::SetItemTooltip("Leaves it level on the highest ground: the low side hangs in the air. In the game a floating piece cannot be seen from below.");
    if (changed) { m = SlopeMode(v); settings_.save(); }
    ImGui::TextDisabled("%s cycles them while placing.", kt(A_SLOPE_MODE).c_str());
}

// ---------------------------------------------------------------------------------------------------------------------
// Properties: a selected flat piece that floats.

void App::slope_ui(WObj& o) {
    if (!editable() || !doc_ || selection_.size() != 1) return;
    const int kind = sheet_kind(o);
    if (!kind) return;
    place::Sheet me;
    if (!sheet_geom(o, kind, me)) return;
    const Terrain& t = doc_->terrain;
    const float gap = place::hang_gap(t, me);
    if (gap <= place::float_tol(t)) return;
    // A tilted road piece has one fix, Fit ground to road (road_ground_ui, which also says how far it floats): Level would flatten it and
    // break its joints, and Tilt is what it already is.
    if (kind == 1 && !me.level()) return;
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
    ImGui::TextWrapped(me.level() ? "Floats up to %.1f m. In the game it cannot be seen from below, and units walk on the ground under it."
                                  : "Tilted, but up to %.1f m of it still floats. In the game it cannot be seen from below, and units walk on the ground under it.", gap);
    ImGui::PopStyleColor();
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Level the ground under it", ImVec2(w, 0))) slope_level_selected();
    ImGui::SetItemTooltip("Shapes the ground under the piece to one height and lays the piece flat on it. Buildings and flat pieces beside it keep their ground. %s undoes it.", kt(A_UNDO).c_str());
    if (ImGui::IsItemHovered()) {
        place::Sheet lv = me;
        lv.pitch = lv.roll = 0;
        const Vec3 c = centre_of(lv);
        float T = t.stored(t.height_at(c.x, c.z));
        if (lv.run >= 0) {
            const SheetSet ss = sheets(true);
            for (const place::Sheet& s : ss.s)
                if (s.run == lv.run && s.uid != lv.uid) T = floor_step(t, s.pos.y + s.under - place::kSheetLift);
        }
        ground_preview_ = plan_level({lv}, T);
        protect_ground(ground_preview_, {o.uid()}, Protect::OthersOnly);
        forecast_ground(ground_preview_);
        ground_preview_frame_ = ImGui::GetFrameCount();
        ground_preview_text_ = ground_edit_note(ground_preview_);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.86f, 0.3f, 1.f));
        ImGui::TextWrapped("%s", ground_preview_text_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    const bool glued = o.find(kGlueProp) != nullptr;
    ImGui::BeginDisabled(!can_tilt(o) || glued || me.run >= 0);
    if (ImGui::Button("Tilt it to the slope", ImVec2(w, 0))) slope_tilt_selected();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(glued ? "A piece glued to others moves with them: tilt it with the handles." :
                          !can_tilt(o) ? "This piece cannot tilt: it stays level." : "Lays the piece on the slope (it keeps that pose until you put it back on the ground). %s undoes it.", kt(A_UNDO).c_str());
}

void App::slope_level_selected() {
    if (!doc_ || !editable() || selection_.size() != 1) return;
    WObj* o = object_by_uid(doc_->world, *selection_.begin());
    if (!o) return;
    const int kind = sheet_kind(*o);
    place::Sheet me;
    if (!kind || !sheet_geom(*o, kind, me)) return;
    const Terrain& t = doc_->terrain;
    push_undo("level the ground under a flat piece");
    // A tilted piece lies flat again, back in the stack.
    int layer = 0;
    bool fixed = true;
    const bool has = place::layer_of(*o, layer, fixed);
    if (!me.level() && can_tilt(*o)) set_tilt(*o, 0.f, 0.f);
    if (!o->find(kGlueProp)) write_layer(*o, has ? layer : 0, false);
    me.pitch = me.roll = 0;
    const Vec3 c = centre_of(me);
    float T = t.stored(t.height_at(c.x, c.z));
    if (me.run >= 0) {
        const SheetSet ss = sheets(true);
        for (const place::Sheet& s : ss.s)
            if (s.run == me.run && s.uid != me.uid) T = floor_step(t, s.pos.y + s.under - place::kSheetLift);
    }
    GroundEdit e = plan_level({me}, T);
    const std::set<uint32_t> own{o->uid()};
    protect_ground(e, own, Protect::OthersOnly);
    forecast_ground(e);
    const int moved = apply_ground_edit(e, "level the ground under a flat piece", false);
    reseat_sheets();  // the piece lies on the new ground even when objects are told to stay (the box is for what stands on it)
    SlopePlan p;
    p.mode = SlopeMode::Level;
    p.up = e.up;
    p.down = e.down;
    slope_kept_ = e.kept.empty() ? std::string() : fit_summary_names(e);
    status_ = slope_status(p, moved);
    status_time_ = 8;
    world_changed();
}

void App::slope_tilt_selected() {
    if (!doc_ || !editable() || selection_.size() != 1) return;
    WObj* o = object_by_uid(doc_->world, *selection_.begin());
    if (!o || !can_tilt(*o) || o->find(kGlueProp)) return;
    const int kind = sheet_kind(*o);
    place::Sheet me;
    if (!kind || !sheet_geom(*o, kind, me)) return;
    push_undo("tilt a flat piece to the slope");
    me.pitch = me.roll = 0;
    place::fit_tilt(doc_->terrain, me, false, 0.f);
    SlopePlan p;
    p.active = true;
    p.mode = SlopeMode::Tilt;
    p.pitch = me.pitch;
    p.roll = me.roll;
    p.pos = me.pos;
    p.left = place::float_gap(doc_->terrain, me);
    apply_slope_pose(o->uid(), p);
    int layer = 0;
    bool fixed = true;
    const bool has = place::layer_of(*o, layer, fixed);
    write_layer(*o, has ? layer : 0, true);
    status_ = slope_status(p, 0);
    status_time_ = 8;
    world_changed();
}

}  // namespace ed
