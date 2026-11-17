// Selecting: the right-click menu of an object and the paste ghost's drawing. The press / click / box rules are in
// handle_viewport_input (app.cpp) and select_rules.h.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../core/triggers.h"
#include "../core/util.h"
#include "app.h"
#include "imgui.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

// True when something selected is a flat piece (road, base, plate): only those have a layer order.
bool App::selection_has_sheet() {
    if (!doc_) return false;
    for (uint32_t u : selection_)
        if (const WObj* o = object_by_uid(doc_->world, u); o && sheet_kind(*o)) return true;
    return false;
}

// The menu that opens when an object is right-clicked in the Select tool. The labels end in ##objmenu so the automation reaches
// them as "##objmenu/Duplicate".
void App::ui_object_menu() {
    if (open_objmenu_) {
        ImGui::OpenPopup("##objmenu");
        open_objmenu_ = false;
    }
    objmenu_open_ = false;
    if (!ImGui::BeginPopup("##objmenu")) return;
    objmenu_open_ = true;
    if (!doc_ || selection_.empty() || !editable() || tool_ != Tool::Select) {  // what was right-clicked is gone, or the tool changed
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("%s", selection_.size() == 1 ? "This object" : (std::to_string(selection_.size()) + " objects").c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(act_text(A_DELETE, "##objmenu").c_str(), kt(A_DELETE).c_str())) delete_selection();
    else if (ImGui::MenuItem(act_text(A_DUPLICATE, "##objmenu").c_str(), kt(A_DUPLICATE).c_str())) duplicate_selected();
    else if (ImGui::MenuItem(act_text(A_COPY, "##objmenu").c_str(), kt(A_COPY).c_str())) copy_selection();
    else {
        ImGui::Separator();
        const bool sheet = selection_has_sheet();
        ImGui::BeginDisabled(!sheet);
        bool act = ImGui::MenuItem(act_text(A_BRING_FORWARD, "##objmenu").c_str(), kt(A_BRING_FORWARD).c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", sheet ? "Put it on top of the flat pieces it overlaps." : "Only road pieces and bases have a layer order.");
        if (act) layer_forward();
        else {
            act = ImGui::MenuItem(act_text(A_SEND_BACK, "##objmenu").c_str(), kt(A_SEND_BACK).c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", sheet ? "Put it under the flat pieces it overlaps." : "Only road pieces and bases have a layer order.");
            if (act) layer_back();
        }
        ImGui::EndDisabled();
        if (!act) {
            ImGui::Separator();
            if (ImGui::MenuItem(act_text(A_SNAP, "##objmenu").c_str(), kt(A_SNAP).c_str())) snap_selection_to_ground();
            else if (ImGui::MenuItem(act_text(A_SELECT_SAME, "##objmenu").c_str(), kt(A_SELECT_SAME).c_str())) select_same_type(selection_.count(objmenu_uid_) ? objmenu_uid_ : *selection_.begin(), true);
            else if (ImGui::MenuItem("Properties##objmenu")) focus_props_ = true;
        }
    }
    ImGui::EndPopup();
}

// The paste ghost (Ctrl+V / Ctrl+D): each copy's box where it will land, the whole group turned by the ghost's heading about its centre.
void App::paste_ghost_draw() {
    if (tool_ != Tool::PasteGhost || !paste_on_ || paste_src_.empty() || !doc_) return;
    Vec3 centre{0, 0, 0};
    for (auto& c : paste_src_) { centre.x += c.pos.x; centre.z += c.pos.z; }
    centre.x /= float(paste_src_.size());
    centre.z /= float(paste_src_.size());
    const float gc = std::cos(paste_turn_), gs = std::sin(paste_turn_);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    const ImU32 col = IM_COL32(120, 230, 255, 255);
    for (const Clip& c : paste_src_) {
        const float dx = c.pos.x - centre.x, dz = c.pos.z - centre.z;
        const float px = paste_at_.x + gc * dx + gs * dz, pz = paste_at_.z - gs * dx + gc * dz;
        const float py = doc_->terrain.height_at(px, pz) + c.above;
        const float h = c.heading + paste_turn_, ch = std::cos(h), sh = std::sin(h);
        const float x0 = std::min(c.lo[0], -1.f), x1 = std::max(c.hi[0], 1.f), y0 = std::min(c.lo[1], 0.f), y1 = std::max(c.hi[1], 1.f);
        const float z0 = std::min(c.lo[2], -1.f), z1 = std::max(c.hi[2], 1.f);
        ImVec2 q[8];
        bool ok = true;
        for (int k = 0; k < 8; k++) {
            const float lx = (k & 1) ? x1 : x0, lz = (k & 2) ? z1 : z0, ly = (k & 4) ? y1 : y0;
            float sx, sy;
            if (!cam_.project({px + ch * lx + sh * lz, py + ly, pz - sh * lx + ch * lz}, vp_w, vp_h, sx, sy)) { ok = false; break; }
            q[k] = ImVec2(vp_x + sx, vp_y + sy);
        }
        if (!ok) continue;
        static const int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& ed_ : e) dl->AddLine(q[ed_[0]], q[ed_[1]], col, 2.f * dpi_);
    }
    dl->PopClipRect();
    // The label beside the mouse: what it is and which keys do what.
    char t[200];
    snprintf(t, sizeof t, "%s %zu %s: click to place - %s turn - %s stops", paste_dup_ ? "Duplicate" : "Paste", paste_src_.size(), paste_src_.size() == 1 ? "object" : "objects",
             turn_keys_text().c_str(), kt(A_CANCEL).c_str());
    ImDrawList* fl = ImGui::GetForegroundDrawList();
    const ImVec2 mp = ImGui::GetIO().MousePos, ts = ImGui::CalcTextSize(t), p(mp.x + 16 * dpi_, mp.y + 18 * dpi_), pd(4 * dpi_, 2 * dpi_);
    fl->AddRectFilled(ImVec2(p.x - pd.x, p.y - pd.y), ImVec2(p.x + ts.x + pd.x, p.y + ts.y + pd.y), IM_COL32(0, 0, 0, 170), 3 * dpi_);
    fl->AddText(p, IM_COL32(255, 255, 255, 255), t);
}

}  // namespace ed
