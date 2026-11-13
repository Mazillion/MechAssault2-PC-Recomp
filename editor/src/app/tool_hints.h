// One status-bar line per tool, built from one template (UX-SPEC 5, audit G1-G3):
// "<Tool name>: <what a click does> - <keys> - <stop key> stops". No App dependency so fmt_test
// can check that every tool has a line. ASCII only (the UI font has no dashes or degree sign).
#pragma once
#include <cstdio>
#include <string>

namespace ed {

enum class Tool {
    Select,
    Raise, Lower, Flatten, Smooth, Roughen, Plateau, PlateauDown, PaintColor, PaintTexture, PaintType, PaintCulture,
    PlaceObject, ScatterTrees,
    PlaceSpawn, PlacePad, PlaceStart,
    PlaceWaypoint, PickPoint,
    MakePond, PlaceWater,
    Count
};

// The ground-shaping tools (Terrain tab, the pairs plus Flatten).
inline bool sculpt_tool(Tool t) { return t >= Tool::Raise && t <= Tool::PlateauDown; }

// Paired shaping tools (playtest 10-26): Raise / Lower, Plateau up / down, Smooth / Roughen. The 'Reverse the
// brush' hold key uses the other half; Flatten (and every other tool) has none.
inline Tool sculpt_partner(Tool t) {
    switch (t) {
        case Tool::Raise: return Tool::Lower;
        case Tool::Lower: return Tool::Raise;
        case Tool::Plateau: return Tool::PlateauDown;
        case Tool::PlateauDown: return Tool::Plateau;
        case Tool::Smooth: return Tool::Roughen;
        case Tool::Roughen: return Tool::Smooth;
        default: return t;
    }
}

// Button / status names of the shaping tools.
inline const char* sculpt_name(Tool t) {
    switch (t) {
        case Tool::Raise: return "Raise";
        case Tool::Lower: return "Lower";
        case Tool::Flatten: return "Flatten";
        case Tool::Smooth: return "Smooth";
        case Tool::Roughen: return "Roughen";
        case Tool::Plateau: return "Plateau up";
        case Tool::PlateauDown: return "Plateau down";
        default: return "";
    }
}

// What the line needs to know right now. Key texts come from the user's hotkey bindings.
struct ToolHintCtx {
    std::string thing;        // what is being placed ("Temporary Autocannon Turret", "road piece Long straight")
    std::string side;         // "Enemy", "Red team", "(no team)"; empty = not said
    float heading_deg = 0;    // facing of the ghost
    float brush_m = 0;        // brush radius in metres
    int selected = 0;
    bool read_only = false;
    bool road_snapped = false;  // PlaceObject with a road piece joined to an open end
    bool road = false;          // PlaceObject with a road piece
    bool move_shape = false;    // PickPoint putting a picked-up shape down
    bool obj_targets = false;   // PickPoint adding 'destroy' targets to an objective (D6)
    bool cam_path = false;      // PlaceWaypoint drawing a camera flight path
    bool no_place = false;      // Select: nothing selected has a position (a timer or 'units destroyed' trigger, a set...)
    std::string what;           // Select with no_place: "This trigger", "These 3 objects"
    bool on_top = false;        // PlaceObject standing on a roof / bridge deck (UX G10)
    std::string on_top_of;      // what it stands on ("Wide bridge 01"); empty: "the roof"
    bool tint_pick = false;     // PaintColor with the eyedropper armed
    bool plant_erase = false;   // PaintCulture clearing instead of growing
    bool objects_follow = true; // sculpt tools: 'Objects follow the terrain' (Terrain tab, saved in the settings)
    std::string stop = "Esc", turn_l = "Q", turn_r = "E", del = "Del", frame = "F", snap = "G";
    std::string hold_free = "Shift", hold_through = "Alt", hold_quarter = "Ctrl";  // the hold keys (Settings > Keys)
    std::string hold_reverse = "Ctrl";  // 'Reverse the brush' (shaping tools)
    bool reversed = false;              // a shaping tool's line while hold_reverse is down: t is the other half (Flatten: picking the height)
};

struct ToolHintParts {
    std::string name;   // "Select", "Raise ground (40 m)", "Placing X for Enemy, facing 120 deg"
    std::string click;  // what a click / drag does
    std::string keys;   // 2-3 key hints, without the stop key
    bool stops = true;  // "<Esc> or right-click stops" is appended
};

inline ToolHintParts tool_hint_parts(Tool t, const ToolHintCtx& c) {
    char size[48];
    std::snprintf(size, sizeof size, " (%.0f m)", c.brush_m);
    std::string qe = c.turn_l + " / " + c.turn_r + " turn";
    const std::string brush_keys = "[ / ] size - Ctrl+Z undoes a stroke";
    // Shown up front, where a narrow window does not cut it off.
    const std::string follow = c.objects_follow ? ", objects follow the terrain: ON" : ", objects follow the terrain: OFF";
    // A paired shaping tool names its other half and the key that reaches it ("Lower (40 m, release Ctrl for Raise)").
    auto sculpt = [&](const char* click) -> ToolHintParts {
        const char* other = sculpt_name(sculpt_partner(t));
        char sz[24];
        std::snprintf(sz, sizeof sz, "%.0f m", c.brush_m);
        if (c.reversed) return {std::string(sculpt_name(t)) + " (" + sz + ", release " + c.hold_reverse + " for " + other + ")" + follow, click, brush_keys};
        return {std::string(sculpt_name(t)) + " (" + sz + ")" + follow, click, "hold " + c.hold_reverse + ": " + other + " - " + brush_keys};
    };
    auto placing = [&](const std::string& what) {
        char h[32];
        std::snprintf(h, sizeof h, ", facing %.0f deg", c.heading_deg);
        return "Placing " + (what.empty() ? std::string("?") : what) + (c.side.empty() ? "" : c.side[0] == '(' ? " " + c.side : " for " + c.side) + h +
               (c.on_top ? ", on " + (c.on_top_of.empty() ? std::string("the roof") : "top of " + c.on_top_of) + " (" + c.hold_through + ": ground)" : "");
    };
    switch (t) {
        case Tool::Select:
            if (c.read_only) return {"Looking at a real level", "click selects to look at it", "Make an editable copy (left) to change it", false};
            if (c.selected > 0 && c.no_place)
                return {c.what.empty() ? "Selected" : c.what, "no place on the map, so edit it in the panel on the left", c.del + " deletes", false};
            if (c.selected > 0)
                return {"Select", "drag the arrows to move", qe + " - " + c.snap + " to ground - " + c.del + " deletes - " + c.frame + " frames", false};
            return {"Select", "click selects - drag on empty ground selects several", "Ctrl+A all - " + c.frame + " frames - ? shortcuts", false};
        case Tool::Raise: return sculpt("drag on the map");
        case Tool::Lower: return sculpt("drag on the map");
        case Tool::Flatten:
            if (c.reversed) return {"Flatten" + std::string(size) + follow, "click picks the height here, then drag to level to it", "release " + c.hold_reverse + " to use the height set on the left"};
            return {"Flatten" + std::string(size) + follow, "drag to level to the height set on the left", "hold " + c.hold_reverse + " and click to pick the height - " + brush_keys};
        case Tool::Smooth: return sculpt("drag to soften bumps and sharp edges");
        case Tool::Roughen: return sculpt("drag to add small bumps");
        case Tool::Plateau: return sculpt("drag to build flat-topped ground");
        case Tool::PlateauDown: return sculpt("drag to cut the ground down to a flat top");
        case Tool::PaintColor:
            if (c.tint_pick) return {"Colour tint", "click the ground to take its colour", ""};
            return {"Colour tint" + std::string(size), "drag to tint the ground", "Alt+click takes a colour - " + brush_keys};
        case Tool::PaintTexture: return {"Paint texture" + std::string(size), "drag to paint the chosen texture", brush_keys};
        case Tool::PaintType: return {"Boundaries" + std::string(size), "drag to mark where players may go", brush_keys};
        case Tool::PaintCulture: return {"Plants" + std::string(size), c.plant_erase ? "drag to clear the chosen plants" : "drag to grow the chosen plants", brush_keys};
        case Tool::PlaceObject:
            if (c.road && c.road_snapped)
                return {placing(c.thing), "click adds it where it joins the road", c.turn_l + " / " + c.turn_r + " join by another end - " + c.hold_free + " places it freely"};
            if (c.road) return {placing(c.thing), "click to place - it joins an open road end by itself", qe + " - hold " + c.hold_free + ": no snapping"};
            return {placing(c.thing), "click to place - drag to face it", qe + " - " + c.hold_quarter + "+" + c.turn_l + " / " + c.turn_r + " 90 deg - hold " + c.hold_free + ": no snapping"};
        case Tool::ScatterTrees: return {"Scatter" + std::string(size), "drag to scatter the ticked objects", "Ctrl+Z undoes a stroke"};
        case Tool::PlaceSpawn: return {placing(c.thing.empty() ? "spawn point" : c.thing), "click to place - drag to face it (the arrow is where the player looks)", qe};
        case Tool::PlacePad: return {placing(c.thing.empty() ? "vehicle pad" : c.thing), "click to place - drag to face it", qe};
        case Tool::PlaceStart: return {placing("the player start"), "click where the player starts - drag to face it", qe};
        case Tool::PlaceWaypoint:
            if (c.cam_path) return {"Camera flight path", "click once per point, in order", "right-click when done"};
            return {"Patrol path", "click to add points in order", "right-click when the path is done"};
        case Tool::PickPoint:
            if (c.move_shape) return {"Moving the shape", "click the map to put it down", "right-click leaves it where it is"};
            if (c.obj_targets) return {"Objective targets", "click each unit or building to destroy", "right-click when done"};
            return {"Pick a point", "click the map to set the trigger's point", ""};
        case Tool::MakePond: return {"Pond", "click the middle of the pond: digs a basin and adds water", "Ctrl+Z undoes both"};
        case Tool::PlaceWater: return {placing(c.thing.empty() ? "water" : c.thing), "click the map to place it (the preview shows its size)", "drag the green arrow later to change its level"};
        case Tool::Count: break;
    }
    return {};
}

inline std::string tool_hint_line(Tool t, const ToolHintCtx& c) {
    ToolHintParts p = tool_hint_parts(t, c);
    if (p.name.empty()) return "";
    std::string s = p.name + ": " + p.click;
    if (!p.keys.empty()) s += " - " + p.keys;
    if (p.stops) s += " - " + c.stop + " or right-click stops";
    return s;
}

}  // namespace ed
