// Road pieces and foundations that fit a gap (playtest 11-01): the Crop rows in Properties, the cropped model the view
// draws, foundations resting just above the ground. The cut itself and the saved game files are core/crop.cpp.
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
    return t && is_foundation(*t);
}

bool App::croppable(const WObj& o) {
    if (o.cls == K_ROAD_C) return true;
    if (!is_foundation_obj(o)) return false;
    GpuModel* gm = models_.get(o.str("Corporal3DObj"), o.str("CorporalMTB"));
    if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
    // A plate: a few metres high at most (Env_Footing and the bases are flat; anything taller is a building).
    return gm && gm->state == GpuModel::Ready && gm->bmax[1] - gm->bmin[1] <= 3.f && gm->bmax[0] - gm->bmin[0] >= 2.f && gm->bmax[2] - gm->bmin[2] >= 2.f;
}

// Playtest 11-01: foundations partly sank into flat ground. The retail plates are
// at y ~0.02-0.05 in their own model (Bb_34x34_Generic01 0.016, Bb_100x100_Airbase 0.050), so with the model bottom on the
// ground (lift_to_foot) they sat in the terrain and the two fought for the same pixels; on a slope the ground under the
// far corners came up through them. The plate now goes kFoundationLift above the highest ground under its footprint.
// The ground as the game (and the editor view, Renderer::build_vertex) draws it: the 8-bit stored heights, each cell
// split along its (gx, gz)-(gx+1, gz+1) diagonal. Terrain::height_at is the unquantized bilinear surface, which can sit up
// to one height step (range / 255: 0.47 m on a -20..100 m map) below the drawn ground. That step is why a plate put "on the
// ground" with height_at vanished into flat ground (editor check 11-01: a 38 m base at 10.064 m on 10.0 m flat ground drew
// nothing; at +1 m it showed).
static float drawn_height_at(const Terrain& t, float x, float z) {
    if (t.w < 2 || t.h < 2) return 0.f;
    float fx = std::clamp(x / t.scale, 0.f, float(t.w - 1)), fz = std::clamp(z / t.scale, 0.f, float(t.h - 1));
    int x0 = std::min(int(fx), t.w - 2), z0 = std::min(int(fz), t.h - 2);
    float tx = fx - float(x0), tz = fz - float(z0);
    float a = t.stored(t.at(x0, z0)), b = t.stored(t.at(x0 + 1, z0)), c = t.stored(t.at(x0, z0 + 1)), d = t.stored(t.at(x0 + 1, z0 + 1));
    return tx >= tz ? a + (b - a) * tx + (d - b) * tz : a + (c - a) * tz + (d - c) * tx;
}

float App::plate_rest_y(const GpuModel* gm, float x, float z, float yaw) {
    const Terrain& t = doc_->terrain;
    float x0 = gm->bmin[0], x1 = gm->bmax[0], z0 = gm->bmin[2], z1 = gm->bmax[2];
    const float c = std::cos(yaw), s = std::sin(yaw);
    float top = drawn_height_at(t, x, z);
    // Every metre or two over the turned footprint, edges included (the terrain is a mesh of ~8 m cells: its highest
    // point under the plate is a grid point inside or a point on the edge).
    const int nx = std::clamp(int((x1 - x0) / 1.5f) + 2, 2, 160), nz = std::clamp(int((z1 - z0) / 1.5f) + 2, 2, 160);
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++) {
            float lx = x0 + (x1 - x0) * float(i) / float(nx - 1), lz = z0 + (z1 - z0) * float(j) / float(nz - 1);
            float wx = x + c * lx + s * lz, wz = z - s * lx + c * lz;
            if (t.inside(wx, wz)) top = std::max(top, drawn_height_at(t, wx, wz));
        }
    // The grid points under the plate (the drawn surface's peaks are there).
    const float r = std::sqrt(std::max(x0 * x0, x1 * x1) + std::max(z0 * z0, z1 * z1));
    for (int gz = std::max(0, int((z - r) / t.scale)); gz <= std::min(t.h - 1, int((z + r) / t.scale) + 1); gz++)
        for (int gx = std::max(0, int((x - r) / t.scale)); gx <= std::min(t.w - 1, int((x + r) / t.scale) + 1); gx++) {
            float dx = float(gx) * t.scale - x, dz = float(gz) * t.scale - z;
            float lx = c * dx - s * dz, lz = s * dx + c * dz;  // into the plate's own axes
            if (lx >= x0 && lx <= x1 && lz >= z0 && lz <= z1) top = std::max(top, t.stored(t.at(gx, gz)));
        }
    return top + kFoundationLift - gm->bmin[1];
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

// Properties > Crop: trim each side of a road piece or foundation. The rectangle is in the model's own axes; the kept
// part stays where it is on the map. Saving cuts the model (crop.cpp); here the view draws the same cut.
void App::crop_ui(WObj& o) {
    if (!croppable(o)) return;
    const bool road = o.cls == K_ROAD_C;
    GpuModel* full = nullptr;
    if (road) {
        std::string def = o.str("RoadDefinition");
        for (char& ch : def) if (ch == '\\') ch = '/';
        if (const RoadPiece* rp = find_road_piece(def); rp && !rp->model.empty()) full = models_.get(rp->model, "");
    } else {
        full = models_.get(o.str("Corporal3DObj"), o.str("CorporalMTB"));
    }
    if (full && full->state == GpuModel::Pending) models_.pump(5000);
    if (!full || full->state != GpuModel::Ready) return;
    const float fx0 = full->bmin[0], fx1 = full->bmax[0], fz0 = full->bmin[2], fz1 = full->bmax[2];
    if (fx1 - fx0 < 2 * crop::kMinSide || fz1 - fz0 < 2 * crop::kMinSide) return;
    crop::Rect r{fx0, fx1, fz0, fz1}, cur;
    const bool has = crop::of(o, cur);
    if (has) r = cur;
    if (!theme::section(road ? "Crop (fit it into a gap)##croproad" : "Crop (fit it into a gap)##cropbase", ImGuiTreeNodeFlags_DefaultOpen)) return;
    // Trims: how much is cut off each side. Left / right are the piece's own sides (facing along its heading);
    // a road's start is the end it was laid from (its origin), its far end where the next piece joins.
    float tl = r.x0 - fx0, tr = fx1 - r.x1, tf = road ? fz1 - r.z1 : fz1 - r.z1, tb = r.z0 - fz0;
    const float W = fx1 - fx0, L = fz1 - fz0;
    bool changed = false;
    auto pair_row = [&](const char* label, const char* ida, const char* idb, float& a, float& b, float total, const char* tipa, const char* tipb) {
        form_label(label, form_col());
        float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(half);
        if (ImGui::DragFloat(ida, &a, 0.25f, 0.f, std::max(0.f, total - crop::kMinSide - b), "%.1f m")) changed = true;
        tip_always(tipa);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat(idb, &b, 0.25f, 0.f, std::max(0.f, total - crop::kMinSide - a), "%.1f m")) changed = true;
        tip_always(tipb);
    };
    pair_row("Trim left / right", "##crl", "##crr", tl, tr, W, "Metres cut off its left side (drag, or double-click to type).",
             "Metres cut off its right side.");
    if (road)
        pair_row("Trim start / far end", "##crs", "##cre", tf, tb, L, "Metres cut off the end it was laid from (its start).",
                 "Metres cut off the far end, where the next piece joins.");
    else
        pair_row("Trim front / back", "##crf", "##crb", tf, tb, L, "Metres cut off its front (the way its heading points).", "Metres cut off its back.");
    char size[96];
    snprintf(size, sizeof size, "Size now %.1f x %.1f m (whole: %.0f x %.0f m).", W - tl - tr, L - tf - tb, W, L);
    hint_text(size);
    if (has) {
        if (ImGui::Button("Whole again##cropreset")) {
            push_undo("uncrop");
            crop::set(o, nullptr);
            doc_->dirty = true;
            objects_dirty_ = minimap_dirty_ = true;
            return;
        }
        tip_always("Removes the crop: the piece is whole again.");
        ImGui::SameLine();
    }
    hint_text(road ? "The texture is cut, not squeezed. The next piece joins the cut end." : "The texture is cut, not squeezed. Other foundations snap to its cut edges.");
    if (!changed) return;
    push_undo_once("crop");
    tl = std::clamp(tl, 0.f, W - crop::kMinSide);
    tr = std::clamp(tr, 0.f, W - crop::kMinSide - tl);
    tf = std::clamp(tf, 0.f, L - crop::kMinSide);
    tb = std::clamp(tb, 0.f, L - crop::kMinSide - tf);
    if (tl + tr + tf + tb < 0.05f) crop::set(o, nullptr);
    else {
        crop::Rect n{fx0 + tl, fx1 - tr, fz0 + tb, fz1 - tf};
        crop::set(o, &n);
    }
    doc_->dirty = true;
    objects_dirty_ = minimap_dirty_ = true;
}

}  // namespace ed
