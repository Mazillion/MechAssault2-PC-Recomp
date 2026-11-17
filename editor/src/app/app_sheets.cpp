// Sheets and layers (core/placement.h). Roads and thin foundation plates are "sheets": flat pieces that lie on the
// ground or on each other. Each carries a layer (the order they were put down in); a sheet rests place::kSheetLift above the
// highest ground under it, or place::kSheetStep above the top of every lower sheet it overlaps. The last one placed goes on
// top; Bring forward / Send back change the order. The layer is one editor-only string property (place::kLayerProp) that
// the save moves to editor/project.json ("layers").
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

#include "app.h"
#include "../core/crop.h"
#include "../core/json.h"
#include "../core/triggers.h"
#include "imgui.h"
#include "theme.h"
#include "ui_tips.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static const uint32_t K_ROAD_S = key_of("MARoad");

namespace {
std::string road_path_of(const WObj& o) {
    std::string def = o.str("RoadDefinition");
    for (char& ch : def) if (ch == '\\') ch = '/';
    return def;
}
}  // namespace

int App::sheet_kind(const WObj& o) {
    if (o.cls == K_ROAD_S) return find_road_piece(road_path_of(o)) ? 1 : 0;
    if (!is_foundation_obj(o)) return 0;
    GpuModel* gm = obj_model(o);
    if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
    if (!gm || gm->state != GpuModel::Ready) return 0;
    const float thick = gm->bmax[1] - gm->bmin[1];
    return thick <= place::kSheetMaxThick && gm->bmax[0] - gm->bmin[0] >= 2.f && gm->bmax[2] - gm->bmin[2] >= 2.f ? 2 : 0;
}

bool App::sheet_geom(const WObj& o, int kind, place::Sheet& s) {
    Vec3 p;
    if (!get_pos(o, p)) return false;
    s.uid = o.uid();
    s.kind = kind;
    s.pos = p;
    s.yaw = heading(o);
    get_tilt(o, s.pitch, s.roll);
    if (kind == 1) {
        const RoadPiece* rp = find_road_piece(road_path_of(o));
        if (!rp) return false;
        float x0 = -rp->w, x1 = rp->w, z0 = -rp->l, z1 = 0;
        GpuModel* gm = rp->model.empty() ? nullptr : models_.get(rp->model, "");
        if (gm && gm->state == GpuModel::Ready && gm->bmax[0] > gm->bmin[0]) { x0 = gm->bmin[0]; x1 = gm->bmax[0]; z0 = gm->bmin[2]; z1 = gm->bmax[2]; }
        if (crop::Rect cr; crop::of(o, cr)) { x0 = std::max(x0, cr.x0); x1 = std::min(x1, cr.x1); z0 = std::max(z0, cr.z0); z1 = std::min(z1, cr.z1); }
        s.fp = {x0, x1, z0, z1};
        s.under = s.top = 0;
    } else {
        GpuModel* gm = obj_model(o);  // already cut to its crop
        if (!gm || gm->state != GpuModel::Ready) return false;
        s.fp = {gm->bmin[0], gm->bmax[0], gm->bmin[2], gm->bmax[2]};
        s.under = gm->bmin[1];
        s.top = gm->bmax[1];
    }
    int layer = 0;
    bool fixed = true;
    if (!place::layer_of(o, layer, fixed)) { layer = 0; fixed = true; }  // no layer (a read-only view): it keeps its height
    if (std::fabs(s.pitch) + std::fabs(s.roll) > 1e-4f || o.find(kGlueProp)) fixed = true;  // tilted and glued pieces keep their height
    s.layer = layer;
    s.fixed = fixed;
    return true;
}

bool App::sheet_is_fixed(const WObj& o) {
    int layer = 0;
    bool fixed = true;
    if (!place::layer_of(o, layer, fixed)) return true;
    if (fixed) return true;
    float p, r;
    get_tilt(o, p, r);
    return std::fabs(p) + std::fabs(r) > 1e-4f || o.find(kGlueProp) != nullptr;
}

App::SheetSet App::sheets(bool with_runs) {
    SheetSet r;
    if (!doc_ || !cat_) return r;
    std::vector<int> kinds;
    for (size_t i = 0; i < doc_->world.objs.size(); i++) {
        const WObj& o = doc_->world.objs[i];
        if (o.cls != K_ROAD_S && !is_foundation_obj(o)) continue;
        const int k = sheet_kind(o);
        if (!k) continue;
        place::Sheet s;
        if (!sheet_geom(o, k, s)) continue;
        r.s.push_back(s);
        r.obj.push_back(int(i));
        kinds.push_back(k);
    }
    int plates = 0;
    for (int k : kinds) plates += k == 2;
    if (with_runs && plates > 1) {
        const std::vector<int> run = snap_runs(3);
        for (size_t i = 0; i < r.s.size(); i++)
            if (kinds[i] == 2 && size_t(r.obj[i]) < run.size()) r.s[i].run = run[size_t(r.obj[i])];
        // A run is plates joined edge to edge. Two that really overlap (a Shift-placed plate whose edge middle happens to sit
        // near another's) are not joined: the newer one leaves the run and stacks like any sheet.
        for (size_t i = 0; i < r.s.size(); i++) {
            if (r.s[i].run < 0) continue;
            for (size_t j = 0; j < r.s.size(); j++)
                if (j != i && r.s[j].run == r.s[i].run && r.s[j].uid < r.s[i].uid && place::overlap(r.s[j], r.s[i])) { r.s[i].run = -1; break; }
        }
    }
    return r;
}

void App::write_layer(WObj& o, int layer, bool fixed) { place::set_layer(o, layer, fixed); }

int App::reseat_sheets(SheetSet* given, const std::vector<Standing>* before) {
    reseat_moves_.clear();
    reseat_moved_ = 0;
    reseat_most_ = 0;
    if (!doc_) return 0;
    SheetSet own;
    if (!given) own = sheets(true);
    SheetSet& ss = given ? *given : own;
    if (ss.s.empty()) return 0;
    // What rests on the sheets, as it is now (or as the caller saw it before its change): it keeps its place on its surface.
    std::vector<Standing> own_before;
    if (!before) own_before = standing_now(ss);
    const std::vector<Standing>& stood = before ? *before : own_before;
    const std::vector<float> moved = place::seat(doc_->terrain, ss.s);
    WTree t;
    bool built = false;
    bool changed = false;
    for (size_t i = 0; i < ss.s.size(); i++) {
        WObj& o = doc_->world.objs[size_t(ss.obj[i])];
        int ly = 0;
        bool fx = false;
        if (place::layer_of(o, ly, fx) && ly != ss.s[i].layer) { write_layer(o, ss.s[i].layer, fx); changed = true; }  // a run shares one layer
        if (moved[i] == 0.f) continue;
        if (!built) { t.build(doc_->world); built = true; }
        std::vector<int> sub;
        if (int ri = t.find(o.uid()); ri >= 0) t.subtree(ri, sub);
        for (int k : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x, q.y + moved[i], q.z});
        }
        reseat_moves_[ss.s[i].uid] = moved[i];
        reseat_moved_++;
        reseat_most_ = std::max(reseat_most_, std::fabs(moved[i]));
        changed = true;
    }
    follow_moved_ = 0;
    if (!stood.empty() && (follow_moved_ = follow(stood, ss)) > 0) changed = true;
    if (changed) { doc_->dirty = true; objects_dirty_ = true; }
    return reseat_moved_;
}

// Writes the layer of every sheet in `ss` that differs from `before` (place::drop_layer renumbers to make room); the stored
// "keeps its height" flag stays as it was.
void App::commit_layers(const SheetSet& ss, const std::vector<int>& before) {
    for (size_t i = 0; i < ss.s.size() && i < before.size(); i++) {
        if (ss.s[i].layer == before[i]) continue;
        WObj& o = doc_->world.objs[size_t(ss.obj[i])];
        int ly = 0;
        bool fx = ss.s[i].fixed;
        place::layer_of(o, ly, fx);
        write_layer(o, ss.s[i].layer, fx);
    }
}

// A map opened for editing: every sheet without a layer gets "<rank> fixed", rank = its place by underside height (then
// uid) among such sheets. So the order matches what is drawn, nothing moves, and nothing is marked changed.
void App::adopt_sheets() {
    if (!doc_ || doc_->read_only || !io_) return;
    SheetSet ss = sheets(false);
    struct R { float y; uint32_t uid; int obj; size_t idx; };
    std::vector<R> missing;
    size_t have = 0;
    for (size_t i = 0; i < ss.s.size(); i++) {
        int ly;
        bool fx;
        if (place::layer_of(doc_->world.objs[size_t(ss.obj[i])], ly, fx)) have++;
        else missing.push_back({ss.s[i].pos.y + ss.s[i].under, ss.s[i].uid, ss.obj[i], i});
    }
    if (missing.empty()) return;
    std::sort(missing.begin(), missing.end(), [](const R& a, const R& b) { return a.y != b.y ? a.y < b.y : a.uid < b.uid; });
    for (size_t k = 0; k < missing.size(); k++) {
        write_layer(doc_->world.objs[size_t(missing[k].obj)], int(k), true);
        ss.s[missing[k].idx].layer = int(k);
    }
    // An older map with flat pieces lying at one height: say so once (a map with layers saved is already ordered).
    if (have == 0) {
        int pairs = 0;
        for (size_t i = 0; i < ss.s.size() && pairs < 50; i++)
            for (size_t j = i + 1; j < ss.s.size() && pairs < 50; j++) {
                const place::Sheet &a = ss.s[i], &b = ss.s[j];
                if (std::fabs((a.pos.y + a.top) - (b.pos.y + b.top)) <= 0.015f && place::overlap(a, b)) pairs++;
            }
        if (pairs > 0) {
            char m[300];
            snprintf(m, sizeof m, "This map is from an older editor: %d flat piece%s overlap at the same height. Edit > Put flat pieces in order stacks them.",
                     pairs, pairs == 1 ? "" : "s");
            sheet_note_ = m;
        }
    }
}

// A new, pasted or duplicated sheet goes on top of the sheets it overlaps; a plate joined edge to edge takes its run's layer
// and joins the run (a run that holds a fixed sheet keeps the height the join gave it).
void App::sheet_dropped(uint32_t root, bool force_fixed) {
    if (!doc_) return;
    SheetSet ss = sheets(true);
    size_t me = ss.s.size();
    for (size_t i = 0; i < ss.s.size(); i++)
        if (ss.s[i].uid == root) me = i;
    if (me == ss.s.size()) return;
    place::Sheet& n = ss.s[me];
    int layer = 0;
    bool fixed = false;
    bool joined = false;
    if (n.run >= 0) {
        int top = INT_MIN;
        for (size_t i = 0; i < ss.s.size(); i++)
            if (i != me && ss.s[i].run == n.run) { top = std::max(top, ss.s[i].layer); fixed = fixed || ss.s[i].fixed; joined = true; }
        if (joined) layer = top;
    }
    std::vector<int> was;
    for (const place::Sheet& s : ss.s) was.push_back(s.layer);
    if (!joined) {
        // On top of what it overlaps; a plate goes under the roads it overlaps (they always lie above the plates near them).
        place::Sheet probe = n;
        probe.layer = INT_MIN;
        layer = place::drop_layer(ss.s, probe);
        commit_layers(ss, was);
    }
    if (force_fixed) fixed = true;
    n.layer = layer;
    n.fixed = fixed;
    write_layer(doc_->world.objs[size_t(ss.obj[me])], layer, fixed);
    reseat_sheets(&ss);
}

void App::mark_selection_fixed() {
    if (!doc_) return;
    bool any = false;
    for (uint32_t u : selection_) {
        WObj* o = object_by_uid(doc_->world, u);
        if (!o || !sheet_kind(*o)) continue;
        int layer = 0;
        bool fixed = true;
        const bool has = place::layer_of(*o, layer, fixed);
        if (has && fixed) continue;
        write_layer(*o, has ? layer : 0, true);
        any = true;
    }
    if (any && !fixed_notice_) {
        status_ = "This piece keeps the height you gave it. " + kt(A_SNAP) + " puts it back on the ground.";
        status_time_ = 6;
        fixed_notice_ = true;
    }
}

// Selected sheet indices into ss, one per object.
static std::vector<size_t> selected_sheets(const App::SheetSet& ss, const std::set<uint32_t>& sel) {
    std::vector<size_t> r;
    for (size_t i = 0; i < ss.s.size(); i++)
        if (sel.count(ss.s[i].uid)) r.push_back(i);
    return r;
}

namespace {
std::string name_of(const WObj& o) {
    std::string d = o.description();
    return d.empty() ? std::string("This piece") : d;
}
}  // namespace

void App::layer_forward() {
    if (!doc_ || !editable()) return;
    SheetSet ss = sheets(true);
    const std::vector<size_t> sel = selected_sheets(ss, selection_);
    if (sel.empty()) { status_ = "Select a road piece or a base first."; status_time_ = 4; return; }
    for (size_t i : sel)
        if (sheet_is_fixed(doc_->world.objs[size_t(ss.obj[i])]) && !ss.s[i].level()) { status_ = "Tilted and glued pieces keep the height you give them."; status_time_ = 5; return; }
    for (size_t i : sel)
        if (doc_->world.objs[size_t(ss.obj[i])].find(kGlueProp)) { status_ = "Tilted and glued pieces keep the height you give them."; status_time_ = 5; return; }
    struct Plan { size_t i; int layer; size_t over; };
    std::vector<Plan> plan;
    for (size_t i : sel) {
        const place::Sheet& c = ss.s[i];
        int top = INT_MIN;
        size_t over = ss.s.size();
        for (size_t j = 0; j < ss.s.size(); j++) {
            if (j == i || (c.run >= 0 && ss.s[j].run == c.run) || selection_.count(ss.s[j].uid)) continue;
            if (!place::overlap(ss.s[j], c)) continue;
            if (ss.s[j].layer > top || (ss.s[j].layer == top && ss.s[j].uid > ss.s[over].uid)) { top = ss.s[j].layer; over = j; }
        }
        if (over < ss.s.size()) plan.push_back({i, top + 1, over});
    }
    if (plan.empty()) { status_ = "Already on top: nothing flat overlaps it."; status_time_ = 4; return; }
    push_undo("bring forward");
    for (const Plan& p : plan) {
        const int run = ss.s[p.i].run;
        for (size_t k = 0; k < ss.s.size(); k++)
            if (k == p.i || (run >= 0 && ss.s[k].run == run)) {
                ss.s[k].layer = p.layer;
                ss.s[k].fixed = false;
                write_layer(doc_->world.objs[size_t(ss.obj[k])], p.layer, false);
            }
    }
    reseat_sheets(&ss);
    char m[260];
    const WObj& a = doc_->world.objs[size_t(ss.obj[plan[0].i])];
    const WObj& b = doc_->world.objs[size_t(ss.obj[plan[0].over])];
    snprintf(m, sizeof m, "%s is now on top of %s. %s undoes it.", name_of(a).c_str(), name_of(b).c_str(), kt(A_UNDO).c_str());
    status_ = m;
    status_time_ = 6;
}

void App::layer_back() {
    if (!doc_ || !editable()) return;
    SheetSet ss = sheets(true);
    const std::vector<size_t> sel = selected_sheets(ss, selection_);
    if (sel.empty()) { status_ = "Select a road piece or a base first."; status_time_ = 4; return; }
    for (size_t i : sel)
        if (!ss.s[i].level() || doc_->world.objs[size_t(ss.obj[i])].find(kGlueProp)) { status_ = "Tilted and glued pieces keep the height you give them."; status_time_ = 5; return; }
    struct Plan { size_t i; int layer; size_t under; };
    std::vector<Plan> plan;
    for (size_t i : sel) {
        const place::Sheet& c = ss.s[i];
        int low = INT_MAX;
        size_t under = ss.s.size();
        for (size_t j = 0; j < ss.s.size(); j++) {
            if (j == i || (c.run >= 0 && ss.s[j].run == c.run) || selection_.count(ss.s[j].uid)) continue;
            if (!place::overlap(ss.s[j], c)) continue;
            if (ss.s[j].layer < low || (ss.s[j].layer == low && ss.s[j].uid < ss.s[under].uid)) { low = ss.s[j].layer; under = j; }
        }
        if (under < ss.s.size()) plan.push_back({i, low - 1, under});
    }
    if (plan.empty()) { status_ = "Already at the back: nothing flat overlaps it."; status_time_ = 4; return; }
    push_undo("send back");
    std::vector<size_t> sent;
    for (const Plan& p : plan) {
        const int run = ss.s[p.i].run;
        for (size_t k = 0; k < ss.s.size(); k++)
            if (k == p.i || (run >= 0 && ss.s[k].run == run)) {
                ss.s[k].layer = p.layer;
                ss.s[k].fixed = false;
                write_layer(doc_->world.objs[size_t(ss.obj[k])], p.layer, false);
                sent.push_back(k);
            }
    }
    // The sheets that were under it are above it now. A fixed one that would sit at or below the sent-back sheet's top is
    // set free (a level one re-seats above it); a tilted one stays and the status line says so.
    int stuck = 0;
    {
        SheetSet what = ss;
        place::seat(doc_->terrain, what.s);
        for (size_t j = 0; j < ss.s.size(); j++) {
            if (!ss.s[j].fixed || std::find(sent.begin(), sent.end(), j) != sent.end()) continue;
            for (size_t k : sent) {
                if (!place::overlap(ss.s[j], what.s[k])) continue;
                if (what.s[k].layer >= ss.s[j].layer) continue;  // it is still above this one
                const float top_k = place::top_over(what.s[k], ss.s[j]);
                if (ss.s[j].pos.y + ss.s[j].under >= top_k + place::kSheetStep - 1e-4f) continue;  // already clear above it
                if (ss.s[j].level() && !doc_->world.objs[size_t(ss.obj[j])].find(kGlueProp)) {
                    ss.s[j].fixed = false;
                    write_layer(doc_->world.objs[size_t(ss.obj[j])], ss.s[j].layer, false);
                } else stuck++;
            }
        }
    }
    reseat_sheets(&ss);
    const WObj& a = doc_->world.objs[size_t(ss.obj[plan[0].i])];
    const WObj& b = doc_->world.objs[size_t(ss.obj[plan[0].under])];
    char m[300];
    const auto mv = reseat_moves_.find(b.uid());
    if (mv != reseat_moves_.end() && mv->second > 0.001f)
        snprintf(m, sizeof m, "%s is now under %s (which moved up %.2f m). %s undoes it.", name_of(a).c_str(), name_of(b).c_str(), mv->second, kt(A_UNDO).c_str());
    else
        snprintf(m, sizeof m, "%s is now under %s. %s undoes it.", name_of(a).c_str(), name_of(b).c_str(), kt(A_UNDO).c_str());
    status_ = m;
    if (stuck) status_ += " A tilted piece over it keeps its own height.";
    status_time_ = 7;
}

// Map > Put flat pieces in order: the selected sheets, or all of them, lose "keeps its height" and are seated by layer.
void App::reseat_command() {
    if (!doc_ || !editable()) return;
    SheetSet ss = sheets(true);
    if (ss.s.empty()) { status_ = "There are no flat pieces (road pieces or bases) to put in order."; status_time_ = 5; return; }
    const bool whole = selected_sheets(ss, selection_).empty();
    push_undo("put flat pieces in order");
    int freed = 0, left = 0;
    for (size_t i = 0; i < ss.s.size(); i++) {
        if (!whole && !selection_.count(ss.s[i].uid)) continue;
        WObj& o = doc_->world.objs[size_t(ss.obj[i])];
        if (!ss.s[i].level() || o.find(kGlueProp)) { left++; continue; }
        ss.s[i].fixed = false;
        write_layer(o, ss.s[i].layer, false);
        freed++;
    }
    reseat_sheets(&ss);
    char m[300];
    snprintf(m, sizeof m, "Put %d flat piece%s in order (moved up to %.2f m).%s %s undoes it.", freed, freed == 1 ? "" : "s", reseat_most_,
             left ? (" " + std::to_string(left) + " tilted or glued piece" + (left == 1 ? " was" : "s were") + " left alone.").c_str() : "", kt(A_UNDO).c_str());
    status_ = m;
    status_time_ = 8;
}

// The piece being placed as a level sheet at that origin (the ghost, and the plan for sloping ground), or false when it is no sheet.
bool App::ghost_sheet(const Vec3& o, place::Sheet& g, bool& plate) {
    if (!doc_ || !cat_) return false;
    g = place::Sheet{};
    plate = false;
    g.uid = 0xFFFFFF00u;
    g.pos = {o.x, 0, o.z};
    g.yaw = ghost_yaw();
    g.kind = place_road_.empty() ? 2 : 1;
    if (!place_road_.empty()) {
        const RoadPiece* rp = find_road_piece(place_road_);
        if (!rp) return false;
        float x0 = -rp->w, x1 = rp->w, z0 = -rp->l, z1 = 0;
        GpuModel* gm = rp->model.empty() ? nullptr : models_.get(rp->model, "");
        if (gm && gm->state == GpuModel::Ready && gm->bmax[0] > gm->bmin[0]) { x0 = gm->bmin[0]; x1 = gm->bmax[0]; z0 = gm->bmin[2]; z1 = gm->bmax[2]; }
        g.fp = {x0, x1, z0, z1};
    } else {
        const TemplateInfo* t = cat_->find(place_template_);
        if (!t || t->model.empty() || !is_foundation(*t) || place_on_top_) return false;
        GpuModel* gm = models_.get(t->model, t->mtb);
        if (!gm || gm->state != GpuModel::Ready) return false;
        if (gm->bmax[1] - gm->bmin[1] > place::kSheetMaxThick || gm->bmax[0] - gm->bmin[0] < 2.f || gm->bmax[2] - gm->bmin[2] < 2.f) return false;
        g.fp = {gm->bmin[0], gm->bmax[0], gm->bmin[2], gm->bmax[2]};
        g.under = gm->bmin[1];
        g.top = gm->bmax[1];
        plate = true;
    }
    return true;
}

// The ghost: the height place_object will give the piece, by the same functions.
bool App::sheet_ghost_y(const Vec3& o, float& y, const Terrain* ground) {
    place::Sheet g;
    bool plate = false;
    if (!ghost_sheet(o, g, plate)) return false;
    const bool joined_plate = plate && piece_snapped_;
    SheetSet ss = sheets(joined_plate);
    if (!place_road_.empty() && road_snapped_ && !std::isnan(road_snap_y_)) { y = road_snap_y_; return true; }  // after a tilted piece
    bool in_run = false;
    if (joined_plate && piece_snap_obj_ >= 0)
        for (size_t j = 0; j < ss.s.size(); j++)
            if (ss.obj[j] == piece_snap_obj_ && ss.s[j].run >= 0) {
                g.run = ss.s[j].run;
                int top = INT_MIN;
                for (size_t k = 0; k < ss.s.size(); k++)
                    if (ss.s[k].run == g.run) { top = std::max(top, ss.s[k].layer); g.fixed = g.fixed || ss.s[k].fixed; }
                g.layer = top;
                in_run = true;
                break;
            }
    if (!in_run) {  // the layer the click will give it (drop_layer on a copy: it may renumber to slip a plate under a road)
        place::Sheet probe = g;
        probe.layer = INT_MIN;
        g.layer = place::drop_layer(ss.s, probe);
    }
    if (g.fixed) g.pos.y = snap_origin_y_;
    y = place::rest_y(ground ? *ground : doc_->terrain, ss.s, g);
    if (joined_plate && !in_run) y = std::max(y, snap_origin_y_);  // joined to a block foundation: at its height, as before
    return true;
}

// G on a sheet: a level, unglued one goes back to the stack (the layer stays); a tilted one keeps its tilt and moves straight
// down or up until its lowest corner clears the ground by kSheetLift; a glued one takes its glue group along.
void App::sheet_to_ground(uint32_t root) {
    WObj* o = object_by_uid(doc_->world, root);
    if (!o) return;
    const int kind = sheet_kind(*o);
    place::Sheet s;
    if (!kind || !sheet_geom(*o, kind, s)) return;
    int layer = 0;
    bool fixed = true;
    const bool has = place::layer_of(*o, layer, fixed);
    const bool glued = o->find(kGlueProp) != nullptr;
    if (s.level() && !glued) {
        write_layer(*o, has ? layer : 0, false);
        return;
    }
    float dy = 0;
    if (s.level()) {
        dy = place::ground_max(doc_->terrain, s) + place::kSheetLift - s.under - s.pos.y;
    } else {
        // Lowest clearance of the underside over the ground (the samples fit_tilt works on, so a tilt and G agree).
        dy = place::kSheetLift - place::min_clearance(doc_->terrain, s);
    }
    WTree t;
    t.build(doc_->world);
    std::vector<uint32_t> movers = glued ? glue_group(selection_) : std::vector<uint32_t>{root};
    for (uint32_t u : movers) {
        int ri = t.find(u);
        if (ri < 0) continue;
        std::vector<int> sub;
        t.subtree(ri, sub);
        for (int k : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x, q.y + dy, q.z});
        }
    }
    write_layer(*o, has ? layer : 0, true);
}

// The sheets of the map that overlap `me` (not `me` itself), and where `me` ranks among them.
namespace {
struct Rank { int n = 1, rank = 1; int below = -1; };  // below = index (into ss.s) of the highest sheet under it
Rank rank_of(const App::SheetSet& ss, const place::Sheet& me) {
    Rank r;
    for (size_t i = 0; i < ss.s.size(); i++) {
        const place::Sheet& s = ss.s[i];
        if (s.uid == me.uid || !place::overlap(s, me)) continue;
        r.n++;
        const bool lower = s.layer < me.layer || (s.layer == me.layer && ((s.kind == 1) == (me.kind == 1) ? s.uid < me.uid : me.kind == 1));
        if (lower) {
            r.rank++;
            if (r.below < 0 || s.layer > ss.s[size_t(r.below)].layer || (s.layer == ss.s[size_t(r.below)].layer && s.uid > ss.s[size_t(r.below)].uid)) r.below = int(i);
        }
    }
    return r;
}
}  // namespace

// Properties: the layer row under Position: the buttons, and what a piece that keeps its own height means.
void App::sheet_layer_ui(WObj& o) {
    if (!editable() || !doc_ || selection_.size() != 1) return;
    const int kind = sheet_kind(o);
    if (!kind) return;
    place::Sheet me;
    if (!sheet_geom(o, kind, me)) return;
    const SheetSet ss = sheets(true);
    const Rank r = rank_of(ss, me);
    form_label("Stacking", form_col());
    if (me.fixed) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Keeps the height it has. %s puts it back on the ground.", kt(A_SNAP).c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextDisabled("%s", r.n > 1 ? "It overlaps other flat pieces. Bring forward / Send back decides which one shows on top." : "Nothing else is stacked here.");
    }
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(r.n < 2);
    if (ImGui::Button(act_text(A_BRING_FORWARD).c_str(), ImVec2(w, 0))) layer_forward();
    ImGui::SetItemTooltip("Put it on top of the flat pieces it overlaps (%s).", kt(A_BRING_FORWARD).c_str());
    ImGui::SameLine();
    if (ImGui::Button(act_text(A_SEND_BACK).c_str(), ImVec2(w, 0))) layer_back();
    ImGui::SetItemTooltip("Put it under the flat pieces it overlaps (%s).", kt(A_SEND_BACK).c_str());
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------------------------------------------------
// What objects stand on. The top sheet under an object's origin, else the ground; a tree, rock or roof is never
// one unless the placing key (Alt) asks for it. Objects keep their place on their surface when a sheet moves under them.

// The map point an object stands at: the middle of its model's base (where a click or the gizmo puts it), not its origin, which
// can be metres off (a rock, a bridge).
void App::stand_xz(const WObj& o, const Vec3& p, float& x, float& z) {
    x = p.x;
    z = p.z;
    if (!doc_ || doc_->world.objs.empty() || &o < &doc_->world.objs.front() || &o > &doc_->world.objs.back()) return;
    const Vec3 fo = footprint_offset(int(&o - &doc_->world.objs.front()));
    x += fo.x;
    z += fo.z;
}

float App::stand_foot(const WObj& o) { return (o.cls == K_LEVELSTART ? 2.f : 0.f) + model_foot(o); }

float App::surface_at(float x, float z, float below_y, uint32_t* on_uid) {
    if (!doc_) return 0.f;
    const SheetSet ss = sheets(false);
    return place::surface_at(doc_->terrain, ss.s, x, z, below_y, on_uid);
}

// Objects that rest on a sheet (foot within kStandTol of its top) or on the ground (from 30 m under to 1.5 m over) near a
// sheet, taken as they are now. Only roots count: a part follows the object it belongs to.
static bool is_unit_class(uint32_t cls) {
    static const uint32_t k[] = {key_of("MAMech"), key_of("MATank"), key_of("MAFlyer"), key_of("MATurret"), key_of("MAVehicle")};
    for (uint32_t c : k) if (c == cls) return true;
    return false;
}

std::vector<App::Standing> App::standing_now(const SheetSet& ss, bool whole_map) {
    std::vector<Standing> out;
    if (!doc_ || (ss.s.empty() && !whole_map)) return out;
    const Terrain& terr = doc_->terrain;
    const place::SurfaceIndex idx(ss.s);
    std::set<uint32_t> sheet_uids;
    for (const place::Sheet& s : ss.s) sheet_uids.insert(s.uid);
    WTree t;
    t.build(doc_->world);
    for (size_t i = 0; i < doc_->world.objs.size(); i++) {
        const WObj& o = doc_->world.objs[i];
        if (o.cls == K_WATER || sheet_uids.count(o.uid())) continue;
        Vec3 p, q;
        if (!get_pos(o, p)) continue;
        int top = -1;  // the outermost positioned ancestor: a unit on a pad belongs to the pad (it moves with it), but stands on its own spot
        for (int a = t.parent_of[i]; a >= 0; a = t.parent_of[size_t(a)])
            if (get_pos(doc_->world.objs[size_t(a)], q)) top = a;
        if (top >= 0 && (!whole_map || !is_unit_class(o.cls))) continue;
        const Vec3 fo = footprint_offset(int(i));
        const float x = p.x + fo.x, z = p.z + fo.z;
        if (!whole_map && !idx.covers(x, z)) continue;
        const float foot = p.y - stand_foot(o);
        uint32_t on = 0;
        const float sf = idx.at(terr, x, z, foot + place::kStandTol, &on, foot - place::kStandTol);
        if (!on) {  // on the ground (a sheet may be put down over it)
            const float g = terr.height_at(x, z);
            if (foot - g > 1.5f || foot - g < -30.f) continue;
            out.push_back({o.uid(), x, z, p.y, foot, g, 0});
        } else {
            out.push_back({o.uid(), x, z, p.y, foot, sf, on});
        }
        if (top >= 0) out.back().root_uid = doc_->world.objs[size_t(top)].uid();
    }
    return out;
}

// Each object of `before` goes to where its surface is now: new origin y = y0 + (surface now - surface0), from the values taken
// before the change, so asking again changes nothing. The surface is the sheet it stood on
// (while that still holds its spot), else whatever lies at its foot now: another sheet within reach, or the ground.
// Glued objects and the pieces of a joined bridge are one body: they move by the mean change of the ones that stood.
int App::follow(const std::vector<Standing>& before, const SheetSet& now) {
    if (!doc_ || before.empty()) return 0;
    const Terrain& terr = doc_->terrain;
    const place::SurfaceIndex idx(now.s);
    std::unordered_map<uint32_t, size_t> by_uid;
    for (size_t i = 0; i < now.s.size(); i++) by_uid[now.s[i].uid] = i;
    WTree t;
    t.build(doc_->world);
    std::map<uint32_t, float> dy;
    std::map<uint32_t, std::pair<uint32_t, float>> kid_dy;  // units on pads: (their pad, the change of their own surface)
    for (const Standing& b : before) {
        const int ri = t.find(b.uid);
        if (ri < 0) continue;
        Vec3 p;
        if (!get_pos(doc_->world.objs[size_t(ri)], p)) continue;
        if (b.fixed_sheet) {  // a sheet that keeps its height moves rigidly with the highest ground under it
            if (auto fs = by_uid.find(b.uid); fs != by_uid.end()) dy[b.uid] = (b.y0 + (place::ground_max(terr, now.s[fs->second]) - b.surface0)) - p.y;
            continue;
        }
        const Vec3 fo = footprint_offset(ri);
        const float x = p.x + fo.x, z = p.z + fo.z;
        float sn;
        auto it = b.on0 ? by_uid.find(b.on0) : by_uid.end();
        if (it != by_uid.end() && place::contains(now.s[it->second], x, z)) {
            // Its sheet still holds its spot: it keeps standing on the top surface there, which is that sheet or, when the order of the
            // flat pieces changed, one that now lies within reach above it (a road laid over the base it stood on).
            const float base = place::face_y(now.s[it->second], now.s[it->second].top, x, z);
            sn = idx.at(terr, x, z, base + place::kStandTol, nullptr, base - 1e-3f);
        } else sn = idx.at(terr, x, z, b.foot0 + place::kStandTol, nullptr, b.foot0 - place::kStandTol);
        const float want_dy = (b.y0 + (sn - b.surface0)) - p.y;  // absolute from the values taken before: calling it again changes nothing
        if (b.root_uid) kid_dy[b.uid] = {b.root_uid, want_dy};
        else dy[b.uid] = want_dy;
    }
    if (dy.empty() && kid_dy.empty()) return 0;
    // Bodies: glue groups and joined bridge runs.
    std::map<uint32_t, float> body_dy;
    std::set<uint32_t> done;
    std::vector<int> bridge_run;
    for (auto& [uid, d] : dy) {
        if (done.count(uid)) continue;
        const int ri = t.find(uid);
        const WObj& o = doc_->world.objs[size_t(ri)];
        std::vector<uint32_t> group;
        if (o.find(kGlueProp)) group = glue_group({uid});
        else if (const TemplateInfo* bt = cat_ ? cat_->find(o.template_id()) : nullptr; bt && snap_family(*bt) == 2) {
            if (bridge_run.empty()) bridge_run = snap_runs(2);
            const int r = size_t(ri) < bridge_run.size() ? bridge_run[size_t(ri)] : -1;
            if (r >= 0)
                for (size_t k = 0; k < bridge_run.size(); k++)
                    if (bridge_run[k] == r) group.push_back(doc_->world.objs[k].uid());
        }
        if (group.size() < 2) { body_dy[uid] = d; done.insert(uid); continue; }
        float sum = 0;
        int n = 0;
        for (uint32_t u : group)
            if (auto f = dy.find(u); f != dy.end()) { sum += f->second; n++; }
        const float mean = n ? sum / float(n) : 0.f;
        for (uint32_t u : group) { body_dy[u] = mean; done.insert(u); }
    }
    int moved = 0;
    for (auto& [uid, d] : body_dy) {
        if (std::fabs(d) < 1e-5f) continue;
        const int ri = t.find(uid);
        if (ri < 0) continue;
        std::vector<int> sub;
        t.subtree(ri, sub);
        for (int k : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x, q.y + d, q.z});
        }
        moved++;
    }
    // A unit on a pad moved with the pad; it still stands on its own spot, so it makes up the difference (a slope under the pad).
    for (auto& [uid, kd] : kid_dy) {
        const auto rd = body_dy.find(kd.first);
        const float extra = kd.second - (rd != body_dy.end() ? rd->second : 0.f);
        if (std::fabs(extra) < 1e-5f) continue;
        const int ri = t.find(uid);
        if (ri < 0) continue;
        std::vector<int> sub;
        t.subtree(ri, sub);
        for (int k : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x, q.y + extra, q.z});
        }
    }
    if (moved || !kid_dy.empty()) objects_dirty_ = true;
    return moved;
}

// How far an object's origin is above the surface it stands on (at its origin), kept when it is copied somewhere else.
float App::above_surface(const WObj& root) {
    Vec3 p;
    if (!get_pos(root, p)) return 0.f;
    float x, z;
    stand_xz(root, p, x, z);
    return p.y - surface_at(x, z, p.y - stand_foot(root) + place::kStandTol);
}

void App::put_on_surface(uint32_t root, float above) {
    WObj* o = object_by_uid(doc_->world, root);
    Vec3 p;
    if (!o || !get_pos(*o, p) || sheet_kind(*o)) return;
    float x, z;
    stand_xz(*o, p, x, z);
    const float dy = surface_at(x, z) + above - p.y;
    if (std::fabs(dy) < 1e-5f) return;
    WTree t;
    t.build(doc_->world);
    std::vector<int> sub;
    if (int ri = t.find(root); ri >= 0) t.subtree(ri, sub);
    for (int k : sub) {
        Vec3 q;
        if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x, q.y + dy, q.z});
    }
}

// The origin height at which the object's foot is on the surface under it; a sheet: the height the stack gives it.
float App::lift_floor_y(const WObj& root) {
    Vec3 p;
    if (!get_pos(root, p)) return -1e30f;
    const int kind = sheet_kind(root);
    if (kind) {
        const SheetSet ss = sheets(true);
        for (const place::Sheet& s : ss.s)
            if (s.uid == root.uid()) return place::rest_y(doc_->terrain, ss.s, s);
        return p.y;
    }
    float x, z;
    stand_xz(root, p, x, z);
    return surface_at(x, z, p.y - stand_foot(root) + place::kStandTol) + stand_foot(root);
}

// A lift drag begins: the lowest total rise it may take. Each object may not go below the surface it stands on, but one that
// already sits below it (a cliff sunk on purpose) can move freely under where it began. The first object to reach its floor stops all.
void App::lift_begin() {
    lift_floor_ = -1e30f;
    lift_started_ = true;
    if (!doc_) return;
    for (uint32_t u : selection_) {
        const WObj* o = object_by_uid(doc_->world, u);
        Vec3 p;
        if (!o || !get_pos(*o, p)) continue;
        if (o->cls == K_WATER) continue;   // A water's Level may go below the ground (a lake bed)
        const float fl = lift_floor_y(*o);
        if (fl < -1e29f) continue;
        lift_floor_ = std::max(lift_floor_, std::min(0.f, fl - p.y));
    }
}

float App::lift_clamp(float wanted) {
    if (!lift_started_) lift_begin();
    lift_stopped_ = false;
    if (holding(settings_.hold_free)) return wanted;
    if (wanted >= lift_floor_) return wanted;
    status_ = std::string("Stopped at the ground. Hold ") + mod_name(settings_.hold_free) + " to push it under.";
    status_time_ = 3;
    lift_stopped_ = true;
    return lift_floor_;
}

// What an object stands on, in words: "On the ground", "0.15 m above Building base 3", "0.40 m below the ground" (amber).
bool App::stand_text(const WObj& o, int idx, float foot, std::string& text, bool& amber) {
    (void)foot;
    Vec3 p;
    if (!get_pos(o, p)) return false;
    const Vec3 fo = footprint_offset(idx);
    const float x = p.x + fo.x, z = p.z + fo.z;
    const float f = p.y - stand_foot(o);  // the foot used for standing (a player start stands 2 m over the ground)
    uint32_t on = 0;
    const float sf = surface_at(x, z, f + place::kStandTol, &on);
    const float gap = f - sf;
    std::string what = "the ground";
    if (on)
        if (const WObj* so = object_by_uid(doc_->world, on)) what = name_of(*so);
    char b[200];
    amber = false;
    if (std::fabs(gap) <= 0.005f) snprintf(b, sizeof b, "On %s", what.c_str());
    else if (gap > 0) snprintf(b, sizeof b, "%.2f m above %s", gap, what.c_str());
    else { snprintf(b, sizeof b, "%.2f m below %s", -gap, what.c_str()); amber = true; }
    text = b;
    return true;
}

// State / lean reads: what the object stands on. gap = its foot minus the surface under it (0 = resting on it), on = the
// uid of the sheet it stands on (0 = the ground). A flat piece reports its layer, whether it keeps its height, how far the lowest
// ground is below it (float_gap) and its underside's height over the highest ground under it.
void App::stand_fields(Json& row, const WObj& o, const SheetSet& ss, bool full) {
    Vec3 p;
    if (!doc_ || !get_pos(o, p)) return;
    auto r3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    const place::Sheet* me = nullptr;
    for (const place::Sheet& s : ss.s)
        if (s.uid == o.uid()) me = &s;
    if (me) {
        int ly = 0;
        bool fx = false;
        place::layer_of(o, ly, fx);
        row.with("fixed", Json::boolean(me->fixed));
        row.with("float_gap", Json::number(r3(place::float_gap(doc_->terrain, *me))));
        row.with("above_ground", Json::number(r3(me->level() ? me->pos.y + me->under - place::ground_max(doc_->terrain, *me) : place::min_clearance(doc_->terrain, *me))));
        return;
    }
    int idx = -1;
    for (size_t i = 0; i < doc_->world.objs.size(); i++)
        if (doc_->world.objs[i].uid() == o.uid()) { idx = int(i); break; }
    const Vec3 fo = idx >= 0 ? footprint_offset(idx) : Vec3{0, 0, 0};
    const float x = p.x + fo.x, z = p.z + fo.z, foot = p.y - stand_foot(o);
    uint32_t on = 0;
    const float sf = place::SurfaceIndex(ss.s).at(doc_->terrain, x, z, foot + place::kStandTol, &on);
    row.with("gap", Json::number(r3(foot - sf))).with("on", Json::number(double(on)));
    if (const float sp = span_float(o); sp > 0.f) row.with("span_float", Json::number(r3(sp)));
    if (full) row.with("ground", Json::number(r3(doc_->terrain.height_at(x, z)))).with("surface", Json::number(r3(sf))).with("foot", Json::number(r3(foot)));
}

// The readout while the lift handle is dragged: "Height 12.40 m, 2.28 m above the ground" / "Height 10.20 m, on Building base 3".
std::string App::lift_readout(float delta) {
    for (uint32_t u : selection_) {
        const WObj* o = object_by_uid(doc_->world, u);
        Vec3 p;
        if (!o || !get_pos(*o, p)) continue;
        int idx = -1;
        for (size_t i = 0; i < doc_->world.objs.size(); i++)
            if (doc_->world.objs[i].uid() == u) { idx = int(i); break; }
        if (o->cls == K_WATER) {   // "Level 10.2 m (ground under the middle 10.1)"
            char wb[200];
            const float gh = doc_->terrain.height_at(p.x, p.z);
            snprintf(wb, sizeof wb, "Level %.1f m (ground under the middle %.1f)", p.y, gh);
            lift_amber_ = false;
            return wb;
        }
        const Vec3 fo = footprint_offset(idx);
        std::string t;
        bool amber = false;
        stand_text(*o, idx, 0, t, amber);
        if (t.rfind("On ", 0) == 0) t[0] = 'o', t[1] = 'n';
        char b[300];
        if (std::isnan(delta)) snprintf(b, sizeof b, "Height %.2f m, %s", p.y + fo.y, t.c_str());
        else snprintf(b, sizeof b, "Height %.2f m (%+.2f), %s", p.y + fo.y, delta, t.c_str());
        lift_amber_ = amber;
        return b;
    }
    return "";
}

// Properties: the dim line under Position. A flat piece reads its place in the stack; anything else what it stands on.
void App::where_it_sits_ui(WObj& o, int idx, float foot) {
    if (o.cls == K_WATER || !doc_) return;
    const int kind = sheet_kind(o);
    std::string text;
    bool amber = false;
    if (kind) {
        place::Sheet me;
        if (!sheet_geom(o, kind, me)) return;
        const SheetSet ss = sheets(true);
        const Rank r = rank_of(ss, me);
        const float gap = me.level() ? me.pos.y + me.under - place::ground_max(doc_->terrain, me) : place::min_clearance(doc_->terrain, me);  // tilted: its lowest point
        char b[260];
        std::string on;
        if (r.below >= 0)
            if (const WObj* so = object_by_uid(doc_->world, ss.s[size_t(r.below)].uid)) on = ", on " + name_of(*so);
        if (r.n > 1) snprintf(b, sizeof b, "No. %d of %d flat pieces stacked here (1 is the lowest)%s. %.2f m above the %s.", r.rank, r.n, on.c_str(), gap, me.level() ? "highest ground under it" : "ground at its lowest point");
        else snprintf(b, sizeof b, "Alone here, nothing stacked with it. %.2f m above the %s.", gap, me.level() ? "highest ground under it" : "ground at its lowest point");
        text = b;
        // A tilted piece that still floats says how far, the same number the status line and the warning use.
        if (const float fg = place::float_gap(doc_->terrain, me); !me.level() && fg > place::float_tol(doc_->terrain)) {
            snprintf(b, sizeof b, " Up to %.1f m of it floats.", fg);
            text += b;
            amber = true;
        }
    } else if (!stand_text(o, idx, foot, text, amber)) {
        return;
    } else if (const float sp = span_float(o); sp > place::float_tol(doc_->terrain)) {
        // The middle rests on the ground but a long wall's end hangs over the slope (it said "On the ground").
        char b[200];
        snprintf(b, sizeof b, "%s, but one end hangs up to %.1f m in the air over the slope.", text.c_str(), sp);
        text = b;
        amber = true;
    }
    // While the mouse drags something, this line keeps the tallest height it had, so the lines under it do not jump when the
    // wording changes between "No. 2 of 2 ..." and "Alone here ...".
    static uint32_t hold_uid = 0;
    static float hold_h = 0.f;
    if (hold_uid != o.uid() || !ImGui::IsMouseDown(0)) { hold_uid = o.uid(); hold_h = 0.f; }
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float th = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_w).y;
    ImGui::PushTextWrapPos(0.f);
    if (amber) ImGui::TextColored(theme::tok().warn, "%s", text.c_str());
    else ImGui::TextDisabled("%s", text.c_str());
    ImGui::PopTextWrapPos();
    hold_h = std::max(hold_h, th);
    if (th < hold_h) ImGui::Dummy(ImVec2(0.f, std::max(0.f, hold_h - th - ImGui::GetStyle().ItemSpacing.y)));
}

}  // namespace ed
