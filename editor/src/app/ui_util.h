#pragma once
#include <windows.h>

#include <string>

#include "../core/util.h"
#include "imgui.h"

namespace ed {
// Text that never runs past `width`. fit_text returns the text cut with "..." when its drawn width exceeds
// `width` (empty or non-positive width: the text as it is); text_ellipsis draws it and shows the full text as the tooltip when cut.
inline std::string fit_text(const std::string& text, float width) {
    if (width <= 0 || ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const float dots = ImGui::CalcTextSize("...").x;
    size_t lo = 0, hi = text.size();   // longest prefix whose width plus "..." fits
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > lo && (static_cast<unsigned char>(text[mid]) & 0xC0) == 0x80) mid--;   // never inside a UTF-8 sequence
        if (mid == lo) break;
        if (ImGui::CalcTextSize(text.c_str(), text.c_str() + mid).x + dots <= width) lo = mid; else hi = mid - 1;
    }
    while (lo > 0 && text[lo - 1] == ' ') lo--;
    return text.substr(0, lo) + "...";
}
// SameLine for the next small button only when it still fits on the line; else it starts its own line (no overflow, no clip).
inline void same_line_if_fits(const char* button_label) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float bw = ImGui::CalcTextSize(button_label).x + st.FramePadding.x * 2.f;
    const float right = ImGui::GetWindowPos().x - ImGui::GetScrollX() + ImGui::GetContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + bw <= right) ImGui::SameLine();
}
inline void text_ellipsis(const std::string& text, float width) {
    const std::string s = fit_text(text, width);
    ImGui::TextUnformatted(s.c_str());
    if (s.size() != text.size() || s != text) ImGui::SetItemTooltip("%s", text.c_str());
}
std::wstring pick_folder(HWND owner, const fs::path& start);
std::wstring pick_wav(HWND owner);
// Names the last item for automation (image buttons carry only an id): "##cell_123" is found by `items filter=##cell_`.
void name_item(const std::string& label);
}  // namespace ed
