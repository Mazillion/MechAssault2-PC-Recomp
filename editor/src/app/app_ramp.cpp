// The Ramp tool: two clicks lay a sloping strip of ground from A to B. Click 1 sets A, click 2 sets B and
// applies it as one undo step (a GroundEdit, so the objects on the ground follow). Esc or a right-click after click 1 drops A and the
// tool stays armed. The preview after click 1 shows the changed cells (outline), the strip coloured by how steep it is and a label.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "app.h"
#include "../core/brush.h"
#include "imgui.h"
#include "theme.h"

namespace ed {

using namespace ma2;

namespace {
constexpr float kRampMaxWidth = 100.f;
const float kNaNr = std::numeric_limits<float>::quiet_NaN();
}  // namespace

// "64 m long, rises 20.0 m: 31%. 'Mechs can climb it." (the label, the status after the click and the automation text)
std::string App::ramp_sentence(float len, float rise, float slope) {
    char b[480], cls[260];
    switch (steep_tier(slope)) {
        case 0: std::snprintf(cls, sizeof cls, "'Mechs and bots can climb it."); break;
        case 1: std::snprintf(cls, sizeof cls, "Bots' 'Mechs won't WALK up this (jump-jet 'Mechs may jet up a short face), over %.0f%%; Battle Armor and pilots walk up to %.0f%%.", double(kSteepBot) * 100.0, double(kSteepLight) * 100.0); break;
        default: std::snprintf(cls, sizeof cls, "Very steep (over %.0f%%): bots won't route here; some 'Mechs may still climb it.", double(kSteepLight) * 100.0); break;
    }
    if (std::fabs(rise) < 0.05f) std::snprintf(b, sizeof b, "%.0f m long, level: 0%%. %s", double(len), cls);
    else std::snprintf(b, sizeof b, "%.0f m long, %s %.1f m: %.0f%%. %s", double(len), rise > 0 ? "rises" : "falls", double(std::fabs(rise)), double(slope) * 100.0, cls);
    return b;
}

// The heights the ramp runs between: the drawn ground at A and at B, or the typed ones.
void App::ramp_heights(const Vec3& a, const Vec3& b, float& ha, float& hb) const {
    const Terrain& t = doc_->terrain;
    if (ramp_typed_) {
        ha = t.stored(std::clamp(ramp_h_from_, t.min_h, t.max_h));
        hb = t.stored(std::clamp(ramp_h_to_, t.min_h, t.max_h));
    } else {
        ha = t.height_at(a.x, a.z);
        hb = t.height_at(b.x, b.z);
    }
}

// The plan: the target of ramp_target at every grid point the ramp reaches (the strip, then a shoulder of 1.5 cells round it).
App::GroundEdit App::plan_ramp(const Vec3& a, const Vec3& b, float ha, float hb) {
    GroundEdit e;
    if (!doc_) return e;
    const Terrain& t = doc_->terrain;
    // The ground has a height point only every cell, so a point just outside the drawn strip would pull the surface at the strip's edge
    // down: the flat top reaches half a cell past the strip (the drawn width stays the width the user set).
    const float W = std::clamp(settings_.ramp_width, 2.f * t.scale, kRampMaxWidth) + 0.5f * t.scale, blend = 1.5f * t.scale, reach = 0.5f * W + blend;
    e.gx0 = std::max(0, int(std::floor((std::min(a.x, b.x) - reach) / t.scale)));
    e.gx1 = std::min(t.w - 1, int(std::ceil((std::max(a.x, b.x) + reach) / t.scale)));
    e.gz0 = std::max(0, int(std::floor((std::min(a.z, b.z) - reach) / t.scale)));
    e.gz1 = std::min(t.h - 1, int(std::ceil((std::max(a.z, b.z) + reach) / t.scale)));
    if (e.gx1 < e.gx0 || e.gz1 < e.gz0) { e.gx1 = e.gz1 = -1; e.gx0 = e.gz0 = 0; return e; }
    const int nx = e.gx1 - e.gx0 + 1, nz = e.gz1 - e.gz0 + 1;
    e.want.assign(size_t(nx) * size_t(nz), kNaNr);
    e.core.assign(size_t(nx) * size_t(nz), 0);
    for (int gz = e.gz0; gz <= e.gz1; gz++)
        for (int gx = e.gx0; gx <= e.gx1; gx++) {
            float out;
            if (!ramp_target(a.x, a.z, b.x, b.z, ha, hb, W, blend, float(gx) * t.scale, float(gz) * t.scale, t.at(gx, gz), out)) continue;
            const size_t i = size_t(gz - e.gz0) * size_t(nx) + size_t(gx - e.gx0);
            e.want[i] = out;
            float inside;
            e.core[i] = ramp_target(a.x, a.z, b.x, b.z, ha, hb, W, 1e-3f, float(gx) * t.scale, float(gz) * t.scale, t.at(gx, gz), inside) ? 1 : 0;
        }
    return e;
}

void App::ramp_cancel(bool say) {
    if (ramp_stage_ == 0) return;
    ramp_stage_ = 0;
    ramp_label_.clear();
    if (say) { status_ = "Ramp start dropped. Click where the ramp starts."; status_time_ = 4; }
}

// [ and ]: the ramp's width, two metres at a time, from two ground cells up.
void App::ramp_width_step(int dir) {
    const float lo = doc_ ? 2.f * doc_->terrain.scale : 16.f;
    settings_.ramp_width = std::clamp(std::round(settings_.ramp_width) + 2.f * float(dir), lo, kRampMaxWidth);
    settings_.save();
}

void App::ramp_click(const Vec3& g) {
    if (!doc_ || !editable()) return;
    const Terrain& t = doc_->terrain;
    if (ramp_stage_ == 0) {
        ramp_a_ = {g.x, t.height_at(g.x, g.z), g.z};
        ramp_stage_ = 1;
        status_ = "Ramp starts here. Click where it ends (Esc starts again).";
        status_time_ = 6;
        return;
    }
    const Vec3 b{g.x, t.height_at(g.x, g.z), g.z};
    const float len = std::hypot(b.x - ramp_a_.x, b.z - ramp_a_.z);
    if (len < 2.f * t.scale) {
        status_ = "That is too short for a ramp: click further from the start (at least " + std::to_string(int(2.f * t.scale)) + " m).";
        status_time_ = 5;
        return;
    }
    float ha, hb;
    ramp_heights(ramp_a_, b, ha, hb);
    GroundEdit e = plan_ramp(ramp_a_, b, ha, hb);
    if (e.empty()) { status_ = "The ramp is outside the map."; status_time_ = 4; return; }
    forecast_ground(e);
    const float rise = hb - ha, slope = std::fabs(rise) / len;
    // A brush, so no protection: whatever stands on the ground follows it (apply_ground_edit). One undo step.
    apply_ground_edit(e, "Ramp");
    ns_shaped_ = true;
    io_->bake_lightmap(*doc_);
    r_.set_lightmap(doc_->lightmap);
    r_.params.lighting = true;
    minimap_dirty_ = true;
    objects_dirty_ = true;
    overlay_dirty_ = true;
    status_ = "Ramp built: " + ramp_sentence(len, rise, slope) + " " + ground_edit_note(e) + " " + kt(A_UNDO) + " undoes it.";
    status_time_ = 8;
    ramp_stage_ = 0;
    ramp_label_.clear();
}

// The preview over the map and the cursor label. Called every frame the Ramp tool is armed and the mouse is over the ground.
void App::draw_ramp_preview(const Vec3& cur) {
    if (!doc_) return;
    const Terrain& t = doc_->terrain;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    draw_height_points(cur, 0.5f * settings_.ramp_width);
    ramp_label_.clear();
    std::string l1 = "Ramp " + std::to_string(int(settings_.ramp_width)) + " m wide";
    if (ramp_stage_ == 1) {
        const Vec3 a = ramp_a_, b{cur.x, t.height_at(cur.x, cur.z), cur.z};
        const float len = std::hypot(b.x - a.x, b.z - a.z);
        float ha, hb;
        ramp_heights(a, b, ha, hb);
        ramp_ha_ = ha;
        ramp_hb_ = hb;
        ramp_len_ = len;
        ramp_slope_ = len > 0.5f ? std::fabs(hb - ha) / len : 0.f;
        if (len >= 1.f) {
            const float W = std::clamp(settings_.ramp_width, 2.f * t.scale, kRampMaxWidth);
            GroundEdit e = plan_ramp(a, b, ha, hb);
            if (!e.empty()) draw_ground_edit(e);
            // The strip, as the ramp makes it: coloured by its slope tier (green all can climb, yellow bots' 'Mechs will not walk up it, orange very steep: no bot routes).
            const int tier = steep_tier(ramp_slope_);
            static const ImU32 kFill[3] = {IM_COL32(70, 210, 90, 110), IM_COL32(255, 225, 60, 120), IM_COL32(255, 128, 0, 125)};
            static const ImU32 kEdge[3] = {IM_COL32(120, 240, 130, 235), IM_COL32(255, 240, 120, 235), IM_COL32(255, 170, 70, 235)};
            const ImU32 fill = kFill[tier], edge = kEdge[tier];
            const float ux = (b.x - a.x) / len, uz = (b.z - a.z) / len, nxp = -uz, nzp = ux;
            const int n = std::clamp(int(len / 4.f), 1, 200);
            auto proj = [&](float u, float side, ImVec2& p) {
                const float x = a.x + ux * u + nxp * side, z = a.z + uz * u + nzp * side;
                const float y = ha + (hb - ha) * (u / len) + 0.3f;
                float sx, sy;
                if (!cam_.project({x, y, z}, vp_w, vp_h, sx, sy)) return false;
                p = ImVec2(sx + vp_x, sy + vp_y);
                return true;
            };
            for (int k = 0; k < n; k++) {
                const float u0 = len * float(k) / float(n), u1 = len * float(k + 1) / float(n);
                ImVec2 q[4];
                if (proj(u0, -0.5f * W, q[0]) && proj(u1, -0.5f * W, q[1]) && proj(u1, 0.5f * W, q[2]) && proj(u0, 0.5f * W, q[3])) {
                    dl->AddConvexPolyFilled(q, 4, fill);
                    dl->AddLine(q[0], q[1], edge, 1.5f * dpi_);
                    dl->AddLine(q[2], q[3], edge, 1.5f * dpi_);
                }
            }
            ramp_label_ = len < 2.f * t.scale ? "Too short for a ramp (at least " + std::to_string(int(2.f * t.scale)) + " m)."
                                              : "Ramp " + ramp_sentence(len, hb - ha, ramp_slope_);
            ImVec2 mid;
            if (proj(0.5f * len, 0.f, mid)) {
                const ImVec2 ts = ImGui::CalcTextSize(ramp_label_.c_str());
                const ImVec2 p0(mid.x - ts.x * 0.5f - 6.f * dpi_, mid.y - ts.y - 14.f * dpi_);
                dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + 12.f * dpi_, p0.y + ts.y + 8.f * dpi_), IM_COL32(15, 15, 15, 215), 4.f * dpi_);
                dl->AddRect(p0, ImVec2(p0.x + ts.x + 12.f * dpi_, p0.y + ts.y + 8.f * dpi_), edge, 4.f * dpi_);
                dl->AddText(ImVec2(p0.x + 6.f * dpi_, p0.y + 4.f * dpi_), edge, ramp_label_.c_str());
            }
            // A marks the start.
            float sx, sy;
            if (cam_.project({a.x, ha + 0.3f, a.z}, vp_w, vp_h, sx, sy)) {
                dl->AddCircleFilled(ImVec2(sx + vp_x, sy + vp_y), 5.f * dpi_, IM_COL32(255, 255, 255, 240));
                dl->AddText(ImVec2(sx + vp_x + 8.f * dpi_, sy + vp_y - 8.f * dpi_), IM_COL32(255, 255, 255, 255), "A");
            }
            l1.clear();   // The label on the strip already says the ramp's slope; the cursor shows only the ground here
        }
    }
    char l2[120];
    const float here = slope_at(t, cur.x, cur.z);
    if (ramp_stage_ == 1) std::snprintf(l2, sizeof l2, "ground %.2f m", double(t.height_at(cur.x, cur.z)));
    else if (steep_tier(here) > 0) std::snprintf(l2, sizeof l2, "ground %.2f m  slope %.0f%%: %s", double(t.height_at(cur.x, cur.z)), double(here) * 100.0, steep_tag(here));
    else std::snprintf(l2, sizeof l2, "ground %.2f m  slope %.0f%%", double(t.height_at(cur.x, cur.z)), double(here) * 100.0);
    brush_readout_ = l1.empty() ? std::string(l2) : l1 + "\n" + l2;
    const ImVec2 at(io.MousePos.x + 18.f * dpi_, io.MousePos.y + 14.f * dpi_);
    const float lh = ImGui::GetTextLineHeight();
    const char* lines[2] = {l1.c_str(), l2};
    for (int k = l1.empty() ? 1 : 0, row = 0; k < 2; k++, row++) {
        dl->AddText(ImVec2(at.x + 1, at.y + float(row) * (lh + 1.f) + 1), IM_COL32(0, 0, 0, 220), lines[k]);
        dl->AddText(ImVec2(at.x, at.y + float(row) * (lh + 1.f)), IM_COL32(255, 255, 255, 255), lines[k]);
    }
    dl->PopClipRect();
}

}  // namespace ed
