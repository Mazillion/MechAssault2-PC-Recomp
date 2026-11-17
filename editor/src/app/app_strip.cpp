// The tool strip, the Build tab's pages (which one is open, what arms when it opens), the panel splitter and the
// header / body / footer layout helpers the pages share. The pages themselves are in app_tabs.cpp.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "ui_tips.h"

namespace ed {

using namespace ma2;

// The strip's tooltips: what each page is for. The key is added from the table (Settings > Keys changes it).
static const char* const kPageTips[kPageCount] = {
    "Click objects on the map to select, move and delete them. The list of everything in this map is here too.",
    "Pick an object (units, buildings, lights, props...) and click the map to place it.",
    "Drop many trees, rocks or plants at once: pick what to scatter, then drag a brush over the map.",
    "Road pieces, walls and fences, bridges and docks: they join end to end.",
    "Shape the ground: raise, lower, level and smooth it with a brush.",
    "Paint the ground: textures, colour tint, boundaries and plants.",
    "Place lakes and ponds, and see the waters this map has.",
    "Where the player or each team starts: spawn points, vehicle pads and game-mode objects.",
};

// Opening a page: Sculpt and Paint arm the tool they had last, Scatter arms its brush when it has something to drop; the
// other pages arm nothing until the user picks something. Another page than the one shown stops the armed tool.
void App::open_page(Page p) {
    if (!doc_) return;
    if (!editable() && p != Page::Select) p = Page::Select;
    tab_ = Tab::Build;
    if (p != page_ || (tool_ != Tool::Select && page_of(tool_) != p && tool_ != Tool::PickPoint)) {
        if (tool_ != Tool::Select && tool_ != Tool::PickPoint) tool_ = Tool::Select;
    }
    page_ = p;
    if (!editable()) return;
    switch (p) {
        case Page::Sculpt: if (!sculpt_page_tool(tool_)) tool_ = sculpt_last_; break;
        case Page::Paint: if (page_of(tool_) != Page::Paint) tool_ = paint_last_; break;
        case Page::Scatter: if (tool_ != Tool::ScatterTrees && !scatter_set_.empty()) tool_ = Tool::ScatterTrees; break;
        default: break;
    }
    if (tool_ == Tool::PaintType && !show_typemap_) { show_typemap_ = typemap_auto_ = true; overlay_dirty_ = true; }
}

// Every frame, before the panels: the old tab numbers (--tab N, saved scripts) become Build and a page, and a tool another
// part of the editor armed (a hotkey, the palette's water, a script) brings its page along.
void App::normalise_tab() {
    if (!doc_) return;
    if (tab_ == Tab::Terrain) {
        tab_ = Tab::Build;
        if (tool_ != Tool::Select && (page_of(tool_) == Page::Sculpt || page_of(tool_) == Page::Paint)) page_ = page_of(tool_);
        else open_page(Page::Sculpt);
    } else if (tab_ == Tab::Players) {
        tab_ = Tab::Build;
        if (page_of(tool_) != Page::Starts && tool_ != Tool::Select && tool_ != Tool::PickPoint) tool_ = Tool::Select;
        page_ = Page::Starts;
    } else if (tab_ == Tab::Units) {
        tab_ = Tab::Build;
        const bool keep = tool_ == Tool::PlaceObject || tool_ == Tool::ScatterTrees;  // a tool of another page is stopped: the old tab was the object palette
        if (!keep && tool_ != Tool::Select && tool_ != Tool::PickPoint) tool_ = Tool::Select;
        if (tool_ == Tool::ScatterTrees) page_ = Page::Scatter;
        else if (page_ != Page::Place && page_ != Page::RoadsWalls && page_ != Page::Scatter) page_ = Page::Place;
    }
    if (tab_ != Tab::Build) return;
    if (!editable()) { if (page_ != Page::Select) page_ = Page::Select; return; }   // A real level opened over the Place page shows Select at once
    switch (tool_) {
        case Tool::Select: case Tool::PickPoint: case Tool::PlaceWaypoint: case Tool::PasteGhost: break;
        case Tool::PlaceObject: if (page_ != Page::Place && page_ != Page::RoadsWalls) page_ = Page::Place; break;
        default: if (page_of(tool_) != page_) page_ = page_of(tool_); break;
    }
}

void App::ui_strip(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * dpi_, 4 * dpi_));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##strip", nullptr, f);
    const float bw = w - 8 * dpi_, bh = std::min(52.f * dpi_, (h - 8 * dpi_) / float(kPageCount) - ImGui::GetStyle().ItemSpacing.y);
    const char* why_off = doc_ && doc_->read_only ? "This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab first." : nullptr;
    for (int i = 0; i < kPageCount; i++) {
        const Page p = Page(i);
        const bool lit = tab_ == Tab::Build && page_ == p;
        const bool off = !editable() && p != Page::Select;
        ImGui::BeginDisabled(off);
        if (lit) theme::push_toggle_on();
        // The label is the item's name for automation; the words are drawn wrapped over the button ("Roads & / walls").
        const ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        const bool clicked = ImGui::Button(page_strip_label(p), ImVec2(bw, bh));
        ImGui::PopStyleColor();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        if (lit) theme::pop_toggle_on();
        {
            ImFont* font = theme::tab_font() ? theme::tab_font() : ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            ImGui::PushFont(font, 0.f);
            const char* name = page_name(p);
            const float wrap = bw - 8 * dpi_;
            const ImVec2 ts = ImGui::CalcTextSize(name, nullptr, false, wrap);
            const ImVec2 at(mn.x + (mx.x - mn.x - std::min(ts.x, wrap)) * 0.5f, mn.y + (mx.y - mn.y - ts.y) * 0.5f);
            ImU32 col = ImGui::GetColorU32(text_col);
            if (off) col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            ImGui::GetWindowDrawList()->AddText(font, ImGui::GetFontSize(), at, col, name, nullptr, wrap);
            ImGui::PopFont();
            (void)fs;
        }
        ImGui::EndDisabled();
        if (clicked) open_page(p);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", off ? (why_off ? why_off : "Make or open a map first.") : (std::string(kPageTips[i]) + " (" + kt(A_PAGE_SELECT + i) + ")").c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// The splitter on the left panel's right edge: drag to resize (300 to 720 px at 100%), double-click for the default.
void App::ui_splitter(float x, float y, float h) {
    const float gw = 6 * dpi_;
    ImGui::SetNextWindowPos(ImVec2(x - gw * 0.5f, y));
    ImGui::SetNextWindowSize(ImVec2(gw, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##splitter", nullptr, f);
    ImGui::InvisibleButton("##split", ImVec2(gw, h));
    const bool act = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        settings_.left_panel = 0;
        settings_.save();
    } else if (act && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
        settings_.left_panel = std::clamp((ImGui::GetIO().MousePos.x - panels_.left[0]) / dpi_, 300.f, 720.f);
        split_drag_ = true;
    }
    if (split_drag_ && !act) {
        split_drag_ = false;
        settings_.save();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// A page is header (natural height) + body (scrolls inside itself) + footer (natural height). The footer's height is the
// one measured last frame, so the body takes what is left and the panel itself never scrolls.
void App::page_body_begin(const char* id) {
    const float fh = page_footer_h_[int(page_)];
    const float avail = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild(id, ImVec2(0, std::max(48.f * dpi_, avail - fh - ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_Borders);
}

void App::page_body_end() {
    ImGui::EndChild();
    page_footer_y0_ = ImGui::GetCursorPosY();
}

void App::page_footer_begin() {}

void App::page_footer_end() {
    page_footer_h_[int(page_)] = std::max(0.f, ImGui::GetCursorPosY() - page_footer_y0_);
}

}  // namespace ed
