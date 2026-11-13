#pragma once
#include <string>

#include "imgui.h"

// A greyed-out control says why on hover (UX-SPEC 2: disabled = 40 % text with the reason in a tooltip).
// Call right after the item; it shows only while `disabled` is true (normal tips stay as they are).
inline void why_disabled(bool disabled, const char* why) {
    if (disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
}

// A tooltip that shows also while the item is greyed out (ImGui::SetItemTooltip does not).
inline void tip_always(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

// Labels left of fields (UX-SPEC M1): the label in a fixed column, the field filling the rest of the row.
// Give the widget a hidden label ("##name"). Every form uses the same column (App::form_col()).
inline void form_label(const char* label, float col_w) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(col_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// A greyed hint that wraps at the panel edge (ImGui::TextDisabled runs off a narrow panel and is cut off).
inline void hint_text(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

inline const char* kReadOnlyReason = "Greyed out: this is a real level and cannot be changed. Press \"Make an editable copy\" (Map tab) to change it.";

// A palette picture's name on up to two lines of width w, broken at a space; only a word that still does not fit
// is cut with ".." ("Military Rommel Mk2..." was cut to "Military Rom..", D1; road pieces to "long straight (6..").
inline std::string palette_cell_name(std::string nm, float w) {
    auto fits = [&](const std::string& x) { return ImGui::CalcTextSize(x.c_str()).x <= w; };
    std::string nm2;
    if (!fits(nm)) {
        size_t cut = nm.rfind(' ');
        while (cut != std::string::npos && cut > 0 && !fits(nm.substr(0, cut))) cut = nm.rfind(' ', cut - 1);
        if (cut != std::string::npos && cut > 0) { nm2 = nm.substr(cut + 1); nm.resize(cut); }
        std::string& last = nm2.empty() ? nm : nm2;
        while (last.size() > 3 && !fits(last)) last.resize(last.size() - 4), last += "..";
    }
    return nm2.empty() ? nm : nm + "\n" + nm2;
}
