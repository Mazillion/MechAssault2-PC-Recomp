// Road pieces and foundations that fit a gap (playtest 11-01): the Crop rows in Properties, the cropped model the view
// draws, foundations resting just above the ground, and the Size section of Properties. The cut itself and the saved game files are core/crop.cpp.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "../core/crop.h"
#include "imgui.h"
#include "theme.h"
#include "ui_tips.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static const uint32_t K_ROAD_C = key_of("MARoad");

std::string App::crop_key(const WObj& o) {
    crop::Rect r;
    return crop::of(o, r) ? crop::format(r) : std::string();
}

GpuModel* App::obj_model(const WObj& o) {
    std::string mp = o.str("Corporal3DObj");
    return mp.empty() ? nullptr : models_.get(mp, o.str("CorporalMTB"), crop_key(o));
}

// Flat ground plates the city maps lay buildings and lots on: the Building base templates (Bb_ / Bd_ / Wst_bb), parking
// lots, footings and paved squares. The model check (a plate, not a building) is croppable()'s.
bool App::is_foundation(const TemplateInfo& t) const {
    if (t.model.empty() || (t.cls_name != "MACorporeal" && t.cls_name != "MABuilding")) return false;
    auto it = palette_kind_.find(t.uid);
    if (it != palette_kind_.end() && (it->second == "cinematic_prop" || it->second == "destroyed_husk")) return false;
    const std::string n = lower(t.name), c = lower(t.category);
    std::string m = lower(t.model);
    m = m.substr(m.find_last_of("/\\") + 1);
    if (n.find("set only") != std::string::npos || n.find("nearclip") != std::string::npos) return false;
    if (c.find("building base") != std::string::npos) return true;
    for (const char* p : {"bb_", "bd_", "wst_bb"})
        if (n.rfind(p, 0) == 0 || m.rfind(p, 0) == 0) return true;
    for (const char* w : {"parkinglot", "parking_lot", "footing", "foundation", "concretesquare", "patternbrick", "hextile"})
        if (n.find(w) != std::string::npos || m.find(w) != std::string::npos) return true;
    return false;
}

bool App::is_foundation_obj(const WObj& o) {
    const TemplateInfo* t = cat_ ? cat_->find(o.template_id()) : nullptr;
    if (!t) return false;
    auto it = foundation_cache_.find(t->uid);
    if (it != foundation_cache_.end()) return it->second != 0;
    const bool f = is_foundation(*t);
    foundation_cache_[t->uid] = f ? 1 : 0;
    return f;
}

bool App::croppable(const WObj& o) {
    const SizeKind k = size_info(o).kind;
    return k == SizeKind::Road || k == SizeKind::Plate || k == SizeKind::Run;
}

static const uint32_t K_WATER_C = key_of("MAWater");

// The uncut model: a road piece's prefab model, else the object's own model.
GpuModel* App::whole_model(const WObj& o) {
    if (o.cls == K_ROAD_C) {
        std::string def = o.str("RoadDefinition");
        for (char& ch : def) if (ch == '\\') ch = '/';
        const RoadPiece* rp = find_road_piece(def);
        return rp && !rp->model.empty() ? models_.get(rp->model, "") : nullptr;
    }
    const std::string mp = o.str("Corporal3DObj");
    return mp.empty() ? nullptr : models_.get(mp, o.str("CorporalMTB"));
}

// Who can be sized, and why not. The model test is crop::cuttable, the save's own.
SizeInfo App::size_info(const WObj& o) {
    SizeInfo si;
    const char* kKeep = "Buildings, props and units keep their model's size: the game has no way to scale them. Roads, bases, walls, fences, bridges, canal walls and water can be sized.";
    auto refuse = [&](const std::string& tail) {
        si.kind = SizeKind::None;
        si.why = "This piece cannot be cut: its model " + tail + ".";
        return si;
    };
    if (!doc_ || !cat_ || !io_) return si;
    if (o.cls == K_WATER_C) {
        if (water_can_resize(o.template_id())) { si.kind = SizeKind::Water; return si; }
        si.fixed_water = true;
        float fw = 0, fl = 0;
        water_size_of(o, nullptr, fw, fl);
        char sz[48] = "";
        if (fw > 0 && fl > 0) snprintf(sz, sizeof sz, " (%.0f x %.0f m)", double(fw), double(fl));
        si.why = std::string("This water type has a fixed size") + sz + " and cannot be resized. Types in the Type list that are not marked \"Fixed size\" can be.";
        return si;
    }
    SizeKind kind = SizeKind::None;
    // switched road cuts off; found the cause (the cut .road / .node / .mesh had bare '\n' line ends, which the game's
    // INI reader does not split) and crop.cpp now writes "\r\n", so road pieces can be cut again.
    if (o.cls == K_ROAD_C) kind = SizeKind::Road;
    else if (const TemplateInfo* t = cat_->find(o.template_id()); t && (t->cls_name == "MACorporeal" || t->cls_name == "MABuilding") && !t->model.empty()) {
        if (is_foundation_obj(o)) kind = SizeKind::Plate;
        else if (const int fam = snap_family(*t); fam == 1 || fam == 2) kind = SizeKind::Run;
    }
    if (kind == SizeKind::None) { si.why = kKeep; return si; }
    GpuModel* gm = whole_model(o);
    if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
    if (gm && gm->state == GpuModel::Failed) return refuse("is not in the game's files");  // the placeholder box the view draws for cut content
    if (!gm || gm->state != GpuModel::Ready) { si.why = "Loading its model..."; return si; }
    const float w = gm->bmax[0] - gm->bmin[0], l = gm->bmax[2] - gm->bmin[2];
    if (kind == SizeKind::Plate) {
        // A plate: a few metres high at most (Env_Footing and the bases are flat; anything taller is a building).
        if (gm->bmax[1] - gm->bmin[1] > 3.f || w < 2.f || l < 2.f) { si.why = kKeep; return si; }
    } else if (kind == SizeKind::Run) {
        si.along_x = w >= l;
        if (std::max(w, l) < 2.f) { si.why = kKeep; return si; }
    }
    // The save's own test, on the model path the game loads (the road piece's prefab model for a road).
    std::string mp;
    if (kind == SizeKind::Road) {
        std::string def = o.str("RoadDefinition");
        for (char& ch : def) if (ch == '\\') ch = '/';
        const RoadPiece* rp = find_road_piece(def);
        mp = rp ? rp->model : std::string();
    } else {
        mp = o.str("Corporal3DObj");
    }
    std::string why;
    if (!mp.empty() && !io_->model_cuttable(*doc_, mp, &why)) return refuse(why);
    si.kind = kind;
    return si;
}

// The uncut rectangle. A run piece (a wall, a bridge) is cut along its long axis only: its other axis keeps 5 cm of margin so no
// vertex on the box edge is clipped by rounding.
bool App::whole_rect(const WObj& o, const SizeInfo& si, crop::Rect& r) {
    GpuModel* gm = whole_model(o);
    if (!gm || gm->state != GpuModel::Ready) return false;
    r = {gm->bmin[0], gm->bmax[0], gm->bmin[2], gm->bmax[2]};
    if (si.kind == SizeKind::Run) {
        if (si.along_x) { r.z0 -= 0.05f; r.z1 += 0.05f; }
        else { r.x0 -= 0.05f; r.x1 += 0.05f; }
    }
    return true;
}

// Join points of a wall or bridge piece follow its crop: the two ends of the long axis of the kept part (the whole piece when uncut).
bool App::object_foot(const WObj& o, SnapFoot& f) {
    const TemplateInfo* t = cat_ ? cat_->find(o.template_id()) : nullptr;
    if (!t || !snap_foot(*t, f)) return false;
    crop::Rect r;
    if (!crop::of(o, r)) return true;
    float a0 = f.along_x ? f.cx - f.half : f.cz - f.half, a1 = f.along_x ? f.cx + f.half : f.cz + f.half;
    a0 = std::max(a0, f.along_x ? r.x0 : r.z0);
    a1 = std::min(a1, f.along_x ? r.x1 : r.z1);
    if (a1 - a0 < 0.5f) return true;  // a crop that left nothing: keep the whole piece's joints
    (f.along_x ? f.cx : f.cz) = 0.5f * (a0 + a1);
    f.half = 0.5f * (a1 - a0);
    return f.half > 0.25f;
}

// The one place a crop changes. The caller has pushed the undo step.
void App::apply_crop(WObj& o, const crop::Rect* r) {
    crop::Rect old;
    const bool was_cut = crop::of(o, old);
    crop::set(o, r);
    // A cut run piece that has a wreck: the game replaces a destroyed piece with its whole model, so a cut one is made indestructible
    // by default (the Size section has the tick to undo it). Going whole again does not touch it.
    if (r && !was_cut && size_info(o).kind == SizeKind::Run && o.u32("DestroyedBuilding") != 0 && o.i32("Indestructible") == 0) {
        o.set_i32("Indestructible", 1);
        status_ = "Cut pieces cannot be destroyed (the game's wreck would be the whole piece). Untick 'Cannot be destroyed' under Size to allow it.";
        status_time_ = 6.f;
    }
    if (sheet_kind(o)) reseat_sheets();  // another footprint: the stack settles again
    doc_->dirty = true;
    objects_dirty_ = minimap_dirty_ = true;
}

// Playtest 11-01: foundations partly sank into flat ground. The retail plates are
// at y ~0.02-0.05 in their own model (Bb_34x34_Generic01 0.016, Bb_100x100_Airbase 0.050), so with the model bottom on the
// ground (lift_to_foot) they sat in the terrain and the two fought for the same pixels; on a slope the ground under the
// far corners came up through them. The plate now goes place::kSheetLift above the highest ground under its footprint.
// Terrain::height_at is the ground the game and the view draw (8-bit steps, cells split on one diagonal), so it is used as is.
float App::plate_rest_y(const GpuModel* gm, float x, float z, float yaw) {
    const Terrain& t = doc_->terrain;
    float x0 = gm->bmin[0], x1 = gm->bmax[0], z0 = gm->bmin[2], z1 = gm->bmax[2];
    const float c = std::cos(yaw), s = std::sin(yaw);
    float top = t.height_at(x, z);
    // Every metre or two over the turned footprint, edges included (the terrain is a mesh of ~8 m cells: its highest
    // point under the plate is a grid point inside or a point on the edge).
    const int nx = std::clamp(int((x1 - x0) / 1.5f) + 2, 2, 160), nz = std::clamp(int((z1 - z0) / 1.5f) + 2, 2, 160);
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++) {
            float lx = x0 + (x1 - x0) * float(i) / float(nx - 1), lz = z0 + (z1 - z0) * float(j) / float(nz - 1);
            float wx = x + c * lx + s * lz, wz = z - s * lx + c * lz;
            if (t.inside(wx, wz)) top = std::max(top, t.height_at(wx, wz));
        }
    // The grid points under the plate (the drawn surface's peaks are there).
    const float r = std::sqrt(std::max(x0 * x0, x1 * x1) + std::max(z0 * z0, z1 * z1));
    for (int gz = std::max(0, int((z - r) / t.scale)); gz <= std::min(t.h - 1, int((z + r) / t.scale) + 1); gz++)
        for (int gx = std::max(0, int((x - r) / t.scale)); gx <= std::min(t.w - 1, int((x + r) / t.scale) + 1); gx++) {
            float dx = float(gx) * t.scale - x, dz = float(gz) * t.scale - z;
            float lx = c * dx - s * dz, lz = s * dx + c * dz;  // into the plate's own axes
            if (lx >= x0 && lx <= x1 && lz >= z0 && lz <= z1) top = std::max(top, t.step_at(gx, gz));
        }
    return top + place::kSheetLift - gm->bmin[1];
}

bool App::foundation_rest(const WObj& o, float& origin_y) {
    if (!doc_ || !is_foundation_obj(o)) return false;
    GpuModel* gm = obj_model(o);
    if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
    if (!gm || gm->state != GpuModel::Ready || gm->bmax[1] - gm->bmin[1] > 3.f) return false;
    if (o.f32("CorporalPitch") != 0 || o.f32("CorporalRoll") != 0) return false;  // tilted on purpose: leave it
    Vec3 p;
    if (!get_pos(o, p)) return false;
    origin_y = plate_rest_y(gm, p.x, p.z, heading(o));
    return true;
}

float App::gizmo_marker_half() const {
    if (selection_.size() != 1) return 0.f;
    const uint32_t u = *selection_.begin();
    for (const ObjView& v : views_)
        if (v.uid == u && v.index == v.root && v.kind == 1) return 0.5f * std::max(v.size.x, v.size.z) * marker_scale() * 1.2f;
    return 0.f;
}

// Properties > Size: the number fields stand around a small top-down outline of the piece, each beside the side it
// cuts (the piece faces up the page: front / start on top, left on the left). Hovering a field (or its caption) lights its handle on the map and
// the side of the outline; hovering or dragging a handle lights its field (size_hot_ / size_panel_hot_). The kept rectangle is in the model's own
// axes and stays where it is on the map; saving cuts the model (crop.cpp), the view draws the same cut. Pieces that cannot be sized say why.
void App::size_ui(WObj& o) {
    const SizeInfo si = size_info(o);
    const bool sizable = si.kind == SizeKind::Road || si.kind == SizeKind::Plate || si.kind == SizeKind::Run;
    if (!sizable) {
        // The why line: only for things that have a model (buildings, props, units, pads, water), not for every helper object.
        const TemplateInfo* t = cat_ ? cat_->find(o.template_id()) : nullptr;
        const bool road_piece = o.cls == K_ROAD_C;
        if (si.why.empty() || !(si.fixed_water || road_piece || (t && !t->model.empty()))) return;
        if (!theme::section("Size##sizewhy", ImGuiTreeNodeFlags_DefaultOpen)) return;
        hint_item(si.why.c_str(), "sizewhytext");
        crop::Rect cut;
        if (road_piece && crop::of(o, cut)) {   // a cut road piece whose model can no longer be cut (or is still loading): take the cut off
            if (ImGui::Button("Whole again##cropreset")) {
                push_undo("size");
                apply_crop(o, nullptr);
            }
            tip_always("Removes the cut: the road piece is whole again.");
        }
        return;
    }
    const bool road = si.kind == SizeKind::Road, run = si.kind == SizeKind::Run, plate = si.kind == SizeKind::Plate;
    crop::Rect whole;
    if (!whole_rect(o, si, whole)) return;
    const float WW = whole.x1 - whole.x0 - (run && !si.along_x ? 0.1f : 0.f), LL = whole.z1 - whole.z0 - (run && si.along_x ? 0.1f : 0.f);
    const float kMin = crop::kMinSide;
    if (run ? (si.along_x ? WW : LL) < 2 * kMin : (WW < 2 * kMin || LL < 2 * kMin)) return;
    if (!theme::section("Size##sizesec", ImGuiTreeNodeFlags_DefaultOpen)) return;
    size_panel_frame_ = ImGui::GetFrameCount();
    hint_text("Drag the orange handles on the map, or type here.");

    crop::Rect r = whole, cur;
    const bool has = crop::of(o, cur);
    if (has) r = cur;
    auto cut_of = [&](const crop::Rect& k, int bit) {
        return bit == kSideX0 ? k.x0 - whole.x0 : bit == kSideX1 ? whole.x1 - k.x1 : bit == kSideZ0 ? k.z0 - whole.z0 : whole.z1 - k.z1;
    };
    auto set_cut = [&](crop::Rect& k, int bit, float v) {
        v = std::max(0.f, v);
        if (bit == kSideX0) k.x0 = std::clamp(whole.x0 + v, whole.x0, k.x1 - kMin);
        if (bit == kSideX1) k.x1 = std::clamp(whole.x1 - v, k.x0 + kMin, whole.x1);
        if (bit == kSideZ0) k.z0 = std::clamp(whole.z0 + v, whole.z0, k.z1 - kMin);
        if (bit == kSideZ1) k.z1 = std::clamp(whole.z1 - v, k.z0 + kMin, whole.z1);
    };
    struct Field { int bit; int place; const char* id; const char* cap; const char* tip; };   // place: 0 above the outline, 1 below, 2 left, 3 right
    std::vector<Field> fields;
    if (road) {
        fields = {{kSideZ1, 0, "##size_start", "Cut at start", "Metres cut off the end it was laid from (its start). Drag, or double-click to type."},
                  {kSideZ0, 1, "##size_end", "Cut at end", "Metres cut off the far end, where the next piece joins."},
                  {kSideX0, 2, "##size_left", "Cut at left", "Metres cut off its left side (facing along it)."},
                  {kSideX1, 3, "##size_right", "Cut at right", "Metres cut off its right side."}};
    } else if (plate) {
        // "front" is where its heading points: north at heading 0, so with the view looking north it is the far side on the screen.
        fields = {{kSideZ1, 0, "##size_front", "Cut at front", "Metres cut off its front: the side its heading points at (north at heading 0, the far side when the view looks north). Drag, or double-click to type."},
                  {kSideZ0, 1, "##size_back", "Cut at back", "Metres cut off its back, opposite the front (south at heading 0)."},
                  {kSideX0, 2, "##size_left", "Cut at left", "Metres cut off its left side as seen from its back looking toward its front (west at heading 0)."},
                  {kSideX1, 3, "##size_right", "Cut at right", "Metres cut off its right side as seen from its back looking toward its front (east at heading 0)."}};
    } else if (si.along_x) {   // end 1 = the +long-axis end (object_joints' first joint)
        fields = {{kSideX0, 2, "##size_end2", "Cut at end 2", "Metres cut off its second end. Drag, or double-click to type."},
                  {kSideX1, 3, "##size_end1", "Cut at end 1", "Metres cut off its first end."}};
    } else {
        fields = {{kSideZ1, 0, "##size_end1", "Cut at end 1", "Metres cut off its first end. Drag, or double-click to type."},
                  {kSideZ0, 1, "##size_end2", "Cut at end 2", "Metres cut off its second end."}};
    }
    // The outline is turned the way the piece lies on the map view (quarter turns): the side that is on the left of the piece on the
    // map is on the left of the outline, and its field stands there. `rot` = clockwise quarter turns from "front up".
    int rot = 0;
    {
        Vec3 pp;
        const float hd = heading(o);
        float sx0, sy0, sx1, sy1;
        if (get_pos(o, pp) && cam_.project({pp.x, pp.y, pp.z}, vp_w, vp_h, sx0, sy0) &&
            cam_.project({pp.x + std::sin(hd) * 10.f, pp.y, pp.z + std::cos(hd) * 10.f}, vp_w, vp_h, sx1, sy1) &&
            std::hypot(sx1 - sx0, sy1 - sy0) > 1.f)
            rot = (int(std::lround(std::atan2(sx1 - sx0, -(sy1 - sy0)) / (3.14159265f * 0.5f))) % 4 + 4) % 4;
    }
    static const int kPlaceRot[4][4] = {{0, 1, 2, 3}, {3, 2, 0, 1}, {1, 0, 3, 2}, {2, 3, 1, 0}};   // top, bottom, left, right after `rot` turns
    for (Field& f : fields) f.place = kPlaceRot[rot][f.place];
    // The layout: fields round the outline.
    const float dp = dpi_, gap = 6.f * dp;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float line_h = ImGui::GetTextLineHeight(), row_h = line_h + st.ItemInnerSpacing.y + ImGui::GetFrameHeight();
    const float W = ImGui::GetContentRegionAvail().x;
    bool has_top = false, has_bottom = false, has_side = false;
    for (const Field& f : fields) { has_top |= f.place == 0; has_bottom |= f.place == 1; has_side |= f.place >= 2; }
    const float boxmax = 76.f * dp, thin = 16.f * dp;
    float bw, bh;   // the outline in the piece's own axes (front up)
    if (run) { bw = si.along_x ? boxmax : thin; bh = si.along_x ? thin : boxmax; }
    else {
        const float sc = boxmax / std::max(WW, LL);
        bw = std::max(thin, WW * sc);
        bh = std::max(thin, LL * sc);
    }
    const bool odd = (rot & 1) != 0;
    const float dw = odd ? bh : bw, dh = odd ? bw : bh;   // as drawn
    float fw = has_side ? std::clamp((W - dw - 2 * gap) * 0.5f, 56.f * dp, 100.f * dp) : std::clamp(W * 0.6f, 80.f * dp, 140.f * dp);
    if (!has_side) fw = std::min(fw, W);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float xc = origin.x + W * 0.5f;
    const float mid_h = std::max(dh + 2 * gap, has_side ? row_h : 0.f);
    float y = origin.y;
    const float y_top = y;
    if (has_top) y += row_h + gap;
    const float y_mid = y;
    y += mid_h;
    if (has_bottom) y += gap;
    const float y_bot = y;
    if (has_bottom) y += row_h;
    const ImVec2 box0(xc - dw * 0.5f, y_mid + (mid_h - dh) * 0.5f);   // the drawn outline's top-left
    const ImVec2 bctr(box0.x + dw * 0.5f, box0.y + dh * 0.5f);
    // A point of the outline given in the piece's own axes (u right, v down, front at v = 0), turned onto the drawn one.
    auto to_draw = [&](float u, float v) {
        const float x = u - bw * 0.5f, yy = v - bh * 0.5f;
        switch (rot) {
            case 1: return ImVec2(bctr.x - yy, bctr.y + x);
            case 2: return ImVec2(bctr.x - x, bctr.y - yy);
            case 3: return ImVec2(bctr.x + yy, bctr.y - x);
            default: return ImVec2(bctr.x + x, bctr.y + yy);
        }
    };
    auto rect_draw = [&](float u0, float v0, float u1, float v1, ImVec2& a, ImVec2& b) {
        const ImVec2 p = to_draw(u0, v0), q = to_draw(u1, v1);
        a = ImVec2(std::min(p.x, q.x), std::min(p.y, q.y));
        b = ImVec2(std::max(p.x, q.x), std::max(p.y, q.y));
    };

    const int hot_in = size_drag_.id ? size_drag_.id : size_hot_;   // what the map hovers or drags
    int hot_out = 0;
    bool changed = false;
    crop::Rect n = r;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 kYellow = IM_COL32(255, 235, 90, 255), kOrange = IM_COL32(255, 150, 30, 255);
    for (const Field& f : fields) {
        ImVec2 at;
        if (f.place == 0) at = ImVec2(xc - fw * 0.5f, y_top);
        else if (f.place == 1) at = ImVec2(xc - fw * 0.5f, y_bot);
        else if (f.place == 2) at = ImVec2(std::max(origin.x, box0.x - gap - fw), y_mid + (mid_h - row_h) * 0.5f);
        else at = ImVec2(std::min(origin.x + W - fw, box0.x + dw + gap), y_mid + (mid_h - row_h) * 0.5f);
        // The caption is an item of its own (hoverable like the field).
        const ImVec2 cs = ImGui::CalcTextSize(f.cap);
        ImGui::SetCursorScreenPos(at);
        const std::string capid = std::string(f.cap) + "##sizecap" + (f.id + 6);
        ImGui::InvisibleButton(capid.c_str(), ImVec2(fw, line_h));
        const bool cap_hover = ImGui::IsItemHovered();
        dl->AddText(ImVec2(at.x + (fw - cs.x) * 0.5f, at.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), f.cap);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + line_h + st.ItemInnerSpacing.y));
        ImGui::SetNextItemWidth(fw);
        float v = cut_of(r, f.bit);
        const int opp = f.bit == kSideX0 ? kSideX1 : f.bit == kSideX1 ? kSideX0 : f.bit == kSideZ0 ? kSideZ1 : kSideZ0;
        const float total = (f.bit & (kSideX0 | kSideX1)) ? WW : LL;
        const float maxv = std::max(0.f, total - kMin - cut_of(r, opp));
        if (ImGui::DragFloat(f.id, &v, 0.1f, 0.f, maxv, "%.1f m")) {
            changed = true;
            set_cut(n, f.bit, v);
        }
        const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive() || cap_hover;
        tip_always(f.tip);
        if (hov) hot_out = f.bit;
        if (hov || hot_in == f.bit) dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kYellow, 3.f * dp, 0, 2.f * dp);
    }
    size_panel_hot_ = hot_out;
    // The outline: the whole piece dim, the kept part filled, the hot side thick yellow; the arrowhead marks the front (start).
    {
        ImVec2 b0, b1;
        rect_draw(0, 0, bw, bh, b0, b1);
        dl->AddRectFilled(b0, b1, IM_COL32(255, 90, 40, 40));   // what is cut off, under the kept part
        const float kx0 = (r.x0 - whole.x0) / (whole.x1 - whole.x0) * bw, kx1 = (r.x1 - whole.x0) / (whole.x1 - whole.x0) * bw;
        const float kz1 = (whole.z1 - r.z1) / (whole.z1 - whole.z0) * bh, kz0 = (whole.z1 - r.z0) / (whole.z1 - whole.z0) * bh;
        ImVec2 k0, k1;
        rect_draw(kx0, kz1, kx1, kz0, k0, k1);
        dl->AddRectFilled(k0, k1, IM_COL32(255, 150, 30, 150));
        dl->AddRect(b0, b1, IM_COL32(200, 200, 200, 170), 0, 0, 1.2f * dp);
        dl->AddRect(k0, k1, kOrange, 0, 0, 1.8f * dp);
        const int hot = hot_out ? hot_out : hot_in;
        const float tkn = 3.5f * dp;
        auto side = [&](int bit, float ua, float va, float ub, float vb) {
            if (!(hot & bit)) return;
            const ImVec2 a = to_draw(ua, va), b = to_draw(ub, vb);
            dl->AddLine(a, b, IM_COL32(0, 0, 0, 200), tkn + 2 * dp);
            dl->AddLine(a, b, kYellow, tkn);
        };
        side(kSideX0, kx0, kz1, kx0, kz0);
        side(kSideX1, kx1, kz1, kx1, kz0);
        side(kSideZ1, kx0, kz1, kx1, kz1);
        side(kSideZ0, kx0, kz0, kx1, kz0);
        // an arrowhead on the front side: the way the piece faces (a road: the end it was laid from)
        if (!run || !si.along_x) {
            const float ah = 4.f * dp, ay = -2.f * dp;
            dl->AddTriangleFilled(to_draw(bw * 0.5f, ay - ah), to_draw(bw * 0.5f - ah, ay), to_draw(bw * 0.5f + ah, ay), IM_COL32(200, 200, 200, 200));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    ImGui::Dummy(ImVec2(W, 2.f * dp));
    if (changed) {
        push_undo_once("size");
        const bool all = std::fabs(n.x0 - whole.x0) < 0.005f && std::fabs(n.x1 - whole.x1) < 0.005f && std::fabs(n.z0 - whole.z0) < 0.005f &&
                         std::fabs(n.z1 - whole.z1) < 0.005f;
        apply_crop(o, all ? nullptr : &n);
        r = all ? whole : n;
    }
    char size[128];
    if (run) {
        const float len = si.along_x ? r.x1 - r.x0 : r.z1 - r.z0;
        snprintf(size, sizeof size, "Size now %.1f m long (whole %.0f m).", len, si.along_x ? WW : LL);
    } else {
        snprintf(size, sizeof size, "Size now %.1f x %.1f m (whole %.0f x %.0f m).", r.x1 - r.x0, r.z1 - r.z0, WW, LL);
    }
    hint_text(size);
    if (crop::of(o, cur)) {
        if (ImGui::Button("Whole again##cropreset")) {
            push_undo("size");
            apply_crop(o, nullptr);
            return;
        }
        tip_always("Removes the cut: the piece is whole again. (Double-click an orange handle to restore just that side.)");
    }
    hint_text(road || run ? "The texture is cut, not squeezed. The next piece joins the cut end." : "The texture is cut, not squeezed. Other bases snap to its cut edges.");
    {   // On a short window the Size section starts below the fold: the first time this piece is shown, the list scrolls just far enough to bring all of it into view.
        static uint32_t scrolled_for = 0;
        static int last_frame = -10, settle = 0;
        const bool fresh = last_frame < ImGui::GetFrameCount() - 1;   // the section was not shown last frame: a new selection or a tab change
        last_frame = ImGui::GetFrameCount();
        if (scrolled_for != o.uid() || fresh) { scrolled_for = o.uid(); settle = 12; }
        // For a dozen frames (the list's height is only known once everything under it has been laid out) nudge the list until the Size
        // section and the row under it (To ground / Delete) are in view; a turn of the mouse wheel over the panel ends it.
        if (settle > 0) {
            settle--;
            if (ImGui::GetIO().MouseWheel != 0.f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) settle = 0;
            const float bottom = ImGui::GetCursorScreenPos().y + 2.f * ImGui::GetFrameHeight() + 2.f * ImGui::GetStyle().ItemSpacing.y;
            const float view_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
            if (bottom > view_bottom) ImGui::SetScrollY(ImGui::GetScrollY() + (bottom - view_bottom));
        }
    }
    if (run && o.u32("DestroyedBuilding") != 0) {
        bool keep = o.i32("Indestructible") != 0;
        if (ImGui::Checkbox("Cannot be destroyed##sizeindestr", &keep)) {
            push_undo("indestructible");
            o.set_i32("Indestructible", keep ? 1 : 0);
            doc_->dirty = true;
        }
        tip_always("A cut piece's wreck is the whole uncut model in the game. Leave this on unless you want that.");
    }
}

}  // namespace ed
