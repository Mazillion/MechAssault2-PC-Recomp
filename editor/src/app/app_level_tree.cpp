// Level tree (View > Level tree): the whole world file as the game reads it, group by group:
// triggers and the actions under them, cinemas with their cameras, controllers and paths, sets,
// AI groups, everything. A click selects the object (Properties then shows every setting, also
// for objects with no place on the map); a double click moves the camera to it when it has one.
// Selecting on the map opens the tree down to that object.
#include <algorithm>
#include <functional>

#include "../core/triggers.h"
#include "../core/util.h"
#include "ui_tips.h"
#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "imgui_stdlib.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

void App::level_tree_window() {
    if (!doc_ || !show_level_tree_) return;
    ImGui::SetNextWindowSize(ImVec2(380 * dpi_, 520 * dpi_), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp_x + vp_w - 390 * dpi_, vp_y + 90 * dpi_), ImGuiCond_FirstUseEver);
    if (!theme::begin_window("Level tree", &show_level_tree_)) { theme::end_window(); return; }
    const Wdf& w = doc_->world;
    const auto& names = gd_.names();
    WTree t;
    t.build(w);
    static const uint32_t K_ROOT_ = key_of("MARoot");

    static std::string filter;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ltf", "Find (name or kind, e.g. cinema, timer)", &filter);
    ImGui::SetItemTooltip("Every object of the level, as the game reads it. Click one to select it and see it in Properties; "
                          "double-click to move the camera to it (objects with a place on the map).");
    std::string lf = lower(filter);

    auto cls_of = [&](int i) { return names.label(w.objs[size_t(i)].cls); };
    auto text_of = [&](int i) {
        const WObj& o = w.objs[size_t(i)];
        std::string d = o.description(), c = cls_of(i);
        std::string plain = c.size() > 2 && c[0] == 'M' && c[1] == 'A' ? c.substr(2) : c;
        return (d.empty() ? "(" + plain + ")" : d) + "##" + std::to_string(o.uid());
    };
    static uint32_t followed = 0;  // the selection the tree last opened itself to
    auto pick = [&](int i, bool zoom) {
        const WObj& o = w.objs[size_t(i)];
        if (o.cls == K_ROOT_) return;  // the root holds everything: never a selection (Delete would empty the level)
        select_uid(o.uid());
        followed = o.uid();  // picked here: it is in view already
        Vec3 p;
        if (zoom && get_pos(o, p)) zoom_to_selection();
    };

    // A new selection (from the map or elsewhere) opens the tree down to it, once.
    std::vector<char> open_path(w.objs.size(), 0);
    int target = -1;
    uint32_t sel = selection_.size() == 1 ? *selection_.begin() : 0;
    if (sel && sel != followed) {
        target = t.find(sel);
        for (int p = target >= 0 ? t.parent_of[size_t(target)] : -1; p >= 0; p = t.parent_of[size_t(p)]) open_path[size_t(p)] = 1;
        followed = sel;
    }

    {
        uint32_t sel1 = selection_.size() == 1 ? *selection_.begin() : 0;
        ImGui::BeginDisabled(!editable() || !sel1);
        const char* why_off = !editable() ? "This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab first." : "Select one object in the list first.";
        if (ImGui::Button("Add part...")) { add_part_parent_ = sel1; add_part_open_ = true; add_part_filter_.clear(); }
        ImGui::SetItemTooltip("Adds a new object under the selected one: any class the game's own levels use, as a copy of an object from them. "
                              "Ones with a place on the map (triggers, lights, cameras...) land at the view centre.");
        why_disabled(!editable() || !sel1, why_off);
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) duplicate_selected_now();
        ImGui::SetItemTooltip("Copies the selected object with all its parts next to it. Links to other objects stay; links between its own parts are remapped.");
        why_disabled(!editable() || !sel1, why_off);
        ImGui::SameLine();
        if (theme::danger_button("Delete")) delete_selection();
        ImGui::SetItemTooltip("%s", ("Deletes the selected object with all its parts (" + kt(A_UNDO) + " undoes).").c_str());
        why_disabled(!editable() || !sel1, why_off);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!editable());
        if (ImGui::Button("New cutscene")) new_cutscene();
        ImGui::SetItemTooltip("Adds a real cutscene (a cinema with its camera, shot, path, focus marker and start/end triggers, built like the game's own) at the view centre. "
                              "Play it with the trigger effect \"Play a cutscene\".");
        why_disabled(!editable(), "This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab first.");
        ImGui::EndDisabled();
    }
    // Right-click menu of a row: selects the row first.
    auto row_menu = [&](int i) {
        if (!ImGui::BeginPopupContextItem()) return;
        const WObj& o = w.objs[size_t(i)];
        if (!selection_.count(o.uid())) pick(i, false);
        ImGui::TextDisabled("%s", cls_of(i).c_str());
        ImGui::Separator();
        bool ed = editable();
        if (ImGui::MenuItem("Add part...", nullptr, false, ed)) { add_part_parent_ = o.uid(); add_part_open_ = true; add_part_filter_.clear(); }
        if (ImGui::MenuItem("Duplicate with its parts", nullptr, false, ed && o.cls != K_ROOT_)) duplicate_selected_now();
        if (o.cls == key_of("MACinema") && ImGui::MenuItem("Add a shot (copy of the last)", nullptr, false, ed)) add_cutscene_shot(o.uid());
        if (o.cls == key_of("MAFreeCameraController")) {
            int pi = t.parent_of[size_t(i)];
            while (pi >= 0 && w.objs[size_t(pi)].cls != key_of("MACinema")) pi = t.parent_of[size_t(pi)];
            if (pi >= 0 && ImGui::MenuItem("Add a shot after this cutscene's last", nullptr, false, ed)) add_cutscene_shot(w.objs[size_t(pi)].uid());
        }
        if (ImGui::MenuItem("Delete", nullptr, false, ed && o.cls != K_ROOT_)) delete_selection();
        Vec3 p;
        if (ImGui::MenuItem("Show on the map", nullptr, false, get_pos(o, p))) zoom_to_selection();
        ImGui::EndPopup();
    };
    ImGui::BeginChild("##lt", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (!lf.empty()) {
        // Flat list of matches with the group they sit in.
        int shown = 0;
        for (size_t i = 0; i < w.objs.size(); i++) {
            const WObj& o = w.objs[i];
            if (lower(o.description() + " " + cls_of(int(i))).find(lf) == std::string::npos) continue;
            if (++shown > 500) { ImGui::TextDisabled("More than 500 match: type more."); break; }
            int p = t.parent_of[i];
            std::string in = p >= 0 ? "  in " + w.objs[size_t(p)].description() : "";
            if (ImGui::Selectable(text_of(int(i)).c_str(), selection_.count(o.uid()) > 0, ImGuiSelectableFlags_AllowDoubleClick)) pick(int(i), false);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) pick(int(i), true);  // the Selectable fires on release
            row_menu(int(i));
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s", cls_of(int(i)).c_str(), in.c_str());
        }
        if (!shown) ImGui::TextDisabled("Nothing matches.");
    } else {
        std::function<void(int)> node = [&](int i) {
            const WObj& o = w.objs[size_t(i)];
            const auto& kids = t.kids[size_t(i)];
            ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
            if (kids.empty()) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (selection_.count(o.uid())) fl |= ImGuiTreeNodeFlags_Selected;
            if (open_path[size_t(i)]) ImGui::SetNextItemOpen(true);
            bool open = ImGui::TreeNodeEx(text_of(i).c_str(), fl);
            if (i == target) ImGui::SetScrollHereY(0.4f);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                pick(i, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            row_menu(i);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                std::string c = cls_of(i);
                const DictClass* dc = dict_.cls(o.cls);
                ImGui::SetTooltip("%s  (UID %u, %zu part%s)%s%s", c.c_str(), o.uid(), kids.size(), kids.size() == 1 ? "" : "s",
                                  dc && !dc->description.empty() ? "\n" : "", dc ? dc->description.c_str() : "");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cls_of(i).c_str());
            if (open && !kids.empty()) {
                for (int k : kids) node(k);
                ImGui::TreePop();
            }
        };
        for (size_t i = 0; i < w.objs.size(); i++)
            if (t.parent_of[i] < 0) node(int(i));
    }
    ImGui::EndChild();
    theme::end_window();
    add_part_popup();
}

}  // namespace ed
