// Force-included in every editor source file (build.cmd: /FI) after Dear ImGui: SetTooltip / SetItemTooltip wrap their text at a
// readable width, so a long tooltip is never cut at the window's right edge. ImGui's own versions draw one
// unwrapped line per "\n". Calls that build their own tooltip window (BeginTooltip) are not touched.
#pragma once
#include <cstdarg>
#include <algorithm>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"

namespace ImGui {

// The map's x range (left edge, right edge) in window pixels; the editor sets it each frame. A tooltip of an item in the tool strip,
// the left panel or the right panel opens beside that panel, over the map, so it never covers the next control of the panel.
struct TooltipZones { float map_left = 0.f, map_right = 1e9f; };
inline TooltipZones g_tooltip_zones;

// Call just before BeginTooltipEx(): `item` is the hovered item's rectangle. The window's own size is not known yet, so the vertical clamp uses
// the size of the last tooltip window.
inline void PlaceTooltipBesideItem(const ImRect& item) {
    ImGuiContext& g = *GImGui;
    if (g_tooltip_zones.map_right > 1e8f || !g.HoveredWindow || !g.HoveredWindow->RootWindow) return;
    if (g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasPos) return;   // the caller placed it (the palette's hover preview)
    const char* wn = g.HoveredWindow->RootWindow->Name;
    const bool left = strncmp(wn, "##strip", 7) == 0 || strncmp(wn, "##left", 6) == 0, right = strncmp(wn, "##right", 7) == 0;
    if (!left && !right) return;
    const float h = g.TooltipPreviousWindow ? g.TooltipPreviousWindow->Size.y : 0.f, vh = GetMainViewport()->Size.y;
    const float y = std::clamp(item.Min.y, 0.f, std::max(0.f, vh - h - 4.f));
    SetNextWindowPos(ImVec2(left ? g_tooltip_zones.map_left + 6.f : g_tooltip_zones.map_right - 6.f, y), ImGuiCond_Always, ImVec2(left ? 0.f : 1.f, 0.f));
}

inline bool BeginTooltipPlaced() {
    PlaceTooltipBesideItem(GImGui->LastItemData.Rect);
    return BeginTooltipEx(ImGuiTooltipFlags_None, ImGuiWindowFlags_None);
}

inline bool BeginItemTooltipPlaced() {
    if (!IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return false;
    return BeginTooltipPlaced();
}

inline void SetTooltipWrappedV(const char* fmt, va_list args) {
    PlaceTooltipBesideItem(GImGui->LastItemData.Rect);
    if (!BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None)) return;
    const float room = std::max(160.f, GetMainViewport()->Size.x - 24.f);  // never wider than the window
    PushTextWrapPos(std::min(GetFontSize() * 34.f, room));
    TextV(fmt, args);
    PopTextWrapPos();
    EndTooltip();
}

inline void SetTooltipWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    SetTooltipWrappedV(fmt, args);
    va_end(args);
}

inline void SetItemTooltipWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (IsItemHovered(ImGuiHoveredFlags_ForTooltip)) SetTooltipWrappedV(fmt, args);
    va_end(args);
}

}  // namespace ImGui

#define SetTooltip SetTooltipWrapped
#define SetTooltipV SetTooltipWrappedV
#define SetItemTooltip SetItemTooltipWrapped
#define BeginTooltip BeginTooltipPlaced
#define BeginItemTooltip BeginItemTooltipPlaced
