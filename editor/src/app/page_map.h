#pragma once
// The Build tab's pages: the tool strip's eight buttons and what each page must always show. No ImGui here, so the
// format tests can check the table (tests/fmt_test.cpp) and tools\pickers_check.py can read the same one (tools\pickers.json).
#include "tool_hints.h"

namespace ed {

enum class Page { Select, Place, Scatter, RoadsWalls, Sculpt, Paint, Water, Starts, Count };
inline constexpr int kPageCount = int(Page::Count);

inline const char* page_name(Page p) {
    static const char* n[kPageCount] = {"Select", "Place", "Scatter", "Roads & walls", "Sculpt", "Paint", "Water", "Starts"};
    return n[int(p)];
}

// The id the strip button carries ("Select##strip"): fixed for automation.
inline const char* page_strip_label(Page p) {
    static const char* n[kPageCount] = {"Select##strip", "Place##strip", "Scatter##strip", "Roads & walls##strip",
                                        "Sculpt##strip", "Paint##strip", "Water##strip", "Starts##strip"};
    return n[int(p)];
}

// The page a tool belongs to (the page whose panel holds its options). PlaceObject also runs on Roads & walls (the page the
// user came from stays); the tools the other tabs use (PickPoint, PlaceWaypoint, the paste ghost) belong to Select.
inline Page page_of(Tool t) {
    switch (t) {
        case Tool::Select: case Tool::PickPoint: case Tool::PlaceWaypoint: case Tool::PasteGhost: return Page::Select;
        case Tool::Raise: case Tool::Lower: case Tool::Flatten: case Tool::Smooth: case Tool::Roughen: case Tool::Plateau:
        case Tool::PlateauDown: case Tool::Ramp: return Page::Sculpt;
        case Tool::PaintColor: case Tool::PaintTexture: case Tool::PaintType: case Tool::PaintCulture: return Page::Paint;
        case Tool::PlaceObject: return Page::Place;
        case Tool::ScatterTrees: return Page::Scatter;
        case Tool::PlaceSpawn: case Tool::PlacePad: case Tool::PlaceStart: return Page::Starts;
        case Tool::MakePond: case Tool::PlaceWater: return Page::Water;
        case Tool::Count: break;
    }
    return Page::Select;
}

// Items every page must show while it is open (label or id part, as automation's `items` filter sees it). Conditional ones (a
// tool armed, something ticked, a mission or a multiplayer map) are in tools\pickers_check.py. Same list as tools\pickers.json.
inline const char* const* page_pickers(Page p) {
    static const char* const select[] = {"Deselect all", "Put on the ground", "Delete", "##placedfilt", nullptr};
    static const char* const place[] = {"##palfind", "Buildings##chip", nullptr};
    static const char* const scatter[] = {"##brushset", "Clear##brushset", "+ Trees of this theme##brushset", "##brushsc", "##scdensity", "Brush##scatter", nullptr};
    static const char* const roads[] = {"##palfind", "Roads##chip", "Walls & fences##chip", nullptr};
    static const char* const sculpt[] = {"Raise", "Lower", "Flatten", "##brush", "Objects follow the terrain", nullptr};
    static const char* const paint[] = {"Texture", "Colour tint", "Boundaries", "Plants", "##brush", nullptr};
    static const char* const water[] = {"##newtype", "##nw", "Place water on the map", nullptr};
    static const char* const starts[] = {"Place spawn point", "Place vehicle pad", "Show every game type", nullptr};  // a mission map: "Move player start"
    static const char* const* t[kPageCount] = {select, place, scatter, roads, sculpt, paint, water, starts};
    return t[int(p)];
}

}  // namespace ed
