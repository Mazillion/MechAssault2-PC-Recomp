#pragma once
#include <string>
#include <vector>

#include "imgui.h"

// A greyed-out control says why on hover (disabled = 40 % text with the reason in a tooltip).
// Call right after the item; it shows only while `disabled` is true (normal tips stay as they are).
inline void why_disabled(bool disabled, const char* why) {
    if (disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
}

// A tooltip that shows also while the item is greyed out (ImGui::SetItemTooltip does not).
inline void tip_always(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

// Labels left of fields: the label in a fixed column, the field filling the rest of the row.
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

// The same line, but findable by the automation `items` read (the text itself is no item): an invisible button over it carries the text as its label.
inline void hint_item(const char* text, const char* id) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    hint_text(text);
    const float h = ImGui::GetItemRectMax().y - p.y;
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton((std::string(text) + "##" + id).c_str(), ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x), std::max(1.f, h)));
}

inline const char* kReadOnlyReason = "Greyed out: this is a real level and cannot be changed. Press \"Make an editable copy\" (Map tab) to change it.";

// A palette picture's name on exactly three lines of width w (equal rows for the list clipper), broken at spaces; only a word
// that still does not fit is cut with ".." ("Canal embankment, end cap" is Canal / embankment, / end cap,: two lines cut
// four different canal pieces to the same "Canal embankment,.."). The full name is on the tooltip.
inline std::string palette_cell_name(const std::string& nm, float w) {
    auto fits = [&](const std::string& x) { return ImGui::CalcTextSize(x.c_str()).x <= w; };
    auto trim = [&](std::string x) {
        while (x.size() > 3 && !fits(x + "..")) x.pop_back();
        return x.size() < 3 ? x : x + "..";
    };
    std::vector<std::string> lines(1);
    size_t i = 0;
    while (i < nm.size()) {
        size_t e = nm.find(' ', i);
        if (e == std::string::npos) e = nm.size();
        const std::string word = nm.substr(i, e - i);
        i = e + 1;
        if (word.empty()) continue;
        std::string& cur = lines.back();
        const std::string with = cur.empty() ? word : cur + " " + word;
        if (fits(with)) { cur = with; continue; }
        if (lines.size() == 3) {  // out of lines: what is left is cut
            cur = trim(cur + " " + word + (i < nm.size() ? " " + nm.substr(i) : std::string()));
            i = nm.size();
            break;
        }
        if (cur.empty()) { cur = fits(word) ? word : trim(word); }
        else lines.push_back(fits(word) ? word : trim(word));
    }
    std::string out;
    for (size_t k = 0; k < 3; k++) out += (k ? "\n" : "") + (k < lines.size() ? lines[k] : std::string(" "));
    return out;
}
