// One status-bar line per tool, built from one template:
// "<Tool name>: <what a click does> - <keys> - <stop key> stops". No App dependency so fmt_test
// can check that every tool has a line. ASCII only (the UI font has no dashes or degree sign).
#pragma once
#include <algorithm>
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
    PasteGhost,  // Ctrl+V / Ctrl+D: the copies hang on the mouse until a click puts them down
    Ramp,        // Two clicks lay a sloping strip of ground from A to B (appended: never renumber)
    Count
};

// The ground-shaping tools (Terrain tab, the pairs plus Flatten).
inline bool sculpt_tool(Tool t) { return t >= Tool::Raise && t <= Tool::PlateauDown; }
// The Sculpt page's tools: the shaping brushes and the Ramp.
inline bool sculpt_page_tool(Tool t) { return sculpt_tool(t) || t == Tool::Ramp; }

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
    bool snapped = false;       // PlaceObject joined to a placed piece (any family)
    std::string snap_what;      // ...what it joins ("Joins Barricade security 3")
    bool move_shape = false;    // PickPoint putting a picked-up shape down
    bool link_unit = false;     // PickPoint choosing the unit an action acts on (Pick on map)
    bool obj_targets = false;   // PickPoint adding 'destroy' targets to an objective
    bool cam_path = false;      // PlaceWaypoint drawing a camera flight path
    bool no_place = false;      // Select: nothing selected has a position (a timer or 'units destroyed' trigger, a set...)
    std::string what;           // Select with no_place: "This trigger", "These 3 objects"
    bool on_top = false;        // PlaceObject standing on a roof / bridge deck
    std::string on_top_of;      // what it stands on ("Wide bridge 01"); empty: "the roof"
    std::string page_pick;      // Select with an object page open and nothing armed: what to do on that page
    bool scatter_empty = false; // ScatterTrees with nothing in the brush set
    int scatter_count = -1;     // ScatterTrees mid-stroke: objects placed (or removed, scatter_erase) so far; -1 = no stroke
    bool scatter_erase = false;
    bool tint_pick = false;     // PaintColor with the eyedropper armed
    bool paint_reversed = false;   // Texture / Boundaries / Plants while the reverse key is held (or the stroke began with it)
    bool paint_layers = false;     // ... and the map has more than two ground textures (reverse rubs the chosen one out)
    bool plant_erase = false;   // PaintCulture clearing instead of growing
    bool objects_follow = true; // sculpt tools: 'Objects follow the ground' (Terrain tab, saved in the settings)
    // A flat piece being placed on sloping ground (slope_mode -1 = not on a slope; 0 level the ground, 1 tilt, 2 float).
    int slope_mode = -1;
    bool slope_fell_back = false;  // asked to tilt, but the piece cannot: it floats
    float slope_gap = 0;           // Float: how far the low side floats (m)
    float slope_up = 0, slope_down = 0;  // Level: the ground rises / falls by up to this (m)
    float slope_deg = 0;           // Tilt: the angle it is tilted to (degrees)
    float slope_left = 0;          // Tilt: what still floats afterwards (m), 0 = nothing worth saying
    bool slope_fits = false;       // Tilt, road piece: the click shapes the ground under it
    bool slope_joint = false;      // Float, joined to a road: kept level with the joint
    float slope_buried = 0;        // ... and how far the ground rises over it (m)
    std::string slope_key = "L";
    std::string stop = "Esc", turn_l = "Q", turn_r = "E", del = "Del", frame = "F", snap = "G";
    std::string brush = "[ / ]";      // Smaller / Bigger brush keys ("[ / ]")
    std::string brush_weak = "Shift+[ / Shift+]";  // Weaker / Stronger brush keys
    std::string select_all = "Ctrl+A", shortcuts = "?", undo = "Ctrl+Z";  // from the key table (App::tool_hint_text fills them)
    std::string hold_free = "Shift", hold_through = "Alt", hold_quarter = "Ctrl";  // the hold keys (Settings > Keys)
    std::string hold_reverse = "Ctrl";  // 'Reverse the brush' (shaping tools)
    int paste_count = 0;                // PasteGhost: how many objects hang on the mouse
    bool paste_dup = false;             // PasteGhost from Duplicate (else Paste)
    bool reversed = false;              // a shaping tool's line while hold_reverse is down: t is the other half (Flatten: picking the height)
    bool moving = false;                // A move drag (body or plane square) is in progress
    float grid_m = 0;                   // The snap grid size while it is on (0 = off)
    float angle_step = 0;               // ...and the angle step (0 = off)
    std::string grid_keys = "[ / ]";    // Smaller / Bigger grid keys
    bool sizable = false;               // The selection is one piece with orange size handles showing
    bool size_small = false;            // ... one that could be sized but is too small on screen
    std::string size_hint;              // ... and the line for the handle under the mouse (empty: none)
    bool size_water = false;            // ... and the piece is a resizable water (handles size it, the blue knob sets its level)
    int ramp_stage = 0;                 // Ramp: 0 = waiting for the start, 1 = start set, waiting for the end
    float ramp_width = 16.f;            // ... the width in metres
    bool build_up = false;              // "Keep raising while held": the amount is added every second
    std::string page_name;              // The page whose pick is named in page_pick (the line then starts with it, not "Select")
    float raise_amount = 2.f;           // Metres one Raise / Lower / Roughen stroke moves the ground
    float water_w = 60, water_l = 40;   // the Place water boxes (the default a plain click places)
    bool water_dragging = false;        // Place water: a size is being dragged out
};

struct ToolHintParts {
    std::string name;   // "Select", "Raise ground (40 m)", "Placing X for Enemy, facing 120 deg"
    std::string click;  // what a click / drag does
    std::string keys;   // 2-3 key hints, without the stop key
    bool stops = true;  // "<Esc> or right-click stops" is appended
};

// The sloping-ground sentence of the placing line.
inline std::string slope_hint(const ToolHintCtx& c) {
    char b[320];
    if (c.slope_mode < 0) return "";
    const std::string k = c.slope_key + ": ";
    if (c.slope_mode == 0) {
        if (c.slope_left > 0.3f)
            std::snprintf(b, sizeof b, "Sloping ground: it will level the ground here (up to +%.1f / -%.1f m), but the ground next to another piece stays: %.1f m still floats. %stilt or float instead.", c.slope_up, c.slope_down, c.slope_left, k.c_str());
        else if (c.slope_buried > 0.3f)
            std::snprintf(b, sizeof b, "Sloping ground: it will level the ground here (up to +%.1f / -%.1f m); part of it ends under the ground next to it that stays (up to %.1f m). %stilt or float instead.", c.slope_up, c.slope_down, c.slope_buried, k.c_str());
        else
            std::snprintf(b, sizeof b, "Sloping ground: it will level the ground here (up to +%.1f / -%.1f m). %stilt or float instead.", c.slope_up, c.slope_down, k.c_str());
        return b;
    }
    if (c.slope_mode == 1) {
        if (c.slope_fits)
            std::snprintf(b, sizeof b, "Sloping ground: tilted %.0f deg, and the ground under it is shaped to meet it when you click. %slevel the ground or float instead.", c.slope_deg, k.c_str());
        else if (c.slope_left > 0.05f)
            std::snprintf(b, sizeof b, "Sloping ground: tilted %.0f deg, %.1f m still floats. %slevel the ground or float instead.", c.slope_deg, c.slope_left, k.c_str());
        else
            std::snprintf(b, sizeof b, "Sloping ground: tilted %.0f deg to follow it. %slevel the ground or float instead.", c.slope_deg, k.c_str());
        return b;
    }
    if (c.slope_joint) {
        char fl[48] = "";
        if (c.slope_gap > 0.3f) std::snprintf(fl, sizeof fl, " The low side floats %.1f m.", c.slope_gap);
        std::snprintf(b, sizeof b, "Sloping ground: kept level with the road it joins.%s%s %stilt or level the ground to follow the slope.", fl,
                      c.slope_buried > 0.3f ? " Part of it is under the ground." : "", k.c_str());
        return b;
    }
    if (c.slope_fell_back) std::snprintf(b, sizeof b, "This piece cannot tilt: it floats %.1f m on one side. %slevel the ground instead.", c.slope_gap, k.c_str());
    else std::snprintf(b, sizeof b, "Sloping ground: one side floats %.1f m. In the game a floating piece cannot be seen from below. %slevel the ground or tilt instead.", c.slope_gap, k.c_str());
    return b;
}

inline ToolHintParts tool_hint_parts(Tool t, const ToolHintCtx& c) {
    char size[48];
    std::snprintf(size, sizeof size, " (%.0f m)", c.brush_m);
    std::string qe = c.turn_l + " / " + c.turn_r + " turn";
    const std::string size_keys = c.brush + " (or Ctrl+wheel) size";
    const std::string brush_keys = size_keys + " - " + c.brush_weak + " (or Shift+wheel) strength - " + c.undo + " undoes a stroke";
    // Shown up front, where a narrow window does not cut it off.
    const std::string follow = c.objects_follow ? ", objects follow the ground: ON" : ", objects follow the ground: OFF";
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
        if (c.angle_step > 0) std::snprintf(h, sizeof h, ", facing %.0f deg (steps of %g)", c.heading_deg, double(c.angle_step));
        else std::snprintf(h, sizeof h, ", facing %.0f deg", c.heading_deg);
        return "Placing " + (what.empty() ? std::string("?") : what) + (c.side.empty() ? "" : c.side[0] == '(' ? " " + c.side : " for " + c.side) + h +
               (c.on_top ? ", on " + (c.on_top_of.empty() ? std::string("the roof") : "top of " + c.on_top_of)
                         : c.road ? std::string() : " (" + c.hold_through + ": on top of the object under the cursor)");
    };
    switch (t) {
        case Tool::Select:
            if (c.read_only) return {"Looking at a real level", "click selects to look at it", "Make an editable copy (left) to change it", false};
            if (c.selected > 0 && c.no_place)
                return {c.what.empty() ? "Selected" : c.what, "no place on the map, so edit it in the panel on the left", c.del + " deletes", false};
            if (c.selected > 0 && c.snapped)   // A move drag that is joined to something
                return {"Moving", (c.snap_what.empty() ? std::string("Joined") : c.snap_what) + " - release puts it where it joins", "hold " + c.hold_free + ": place freely", false};
            if (c.selected > 0 && c.grid_m > 0 && c.moving)
                return {"Moving", "release puts it on the grid (" + std::to_string(int(c.grid_m)) + " m)", "hold " + c.hold_free + ": place freely", false};
            if (c.selected > 0 && !c.size_hint.empty())   // The mouse is on an orange handle
                return {c.size_water ? "Size" : "Cut", c.size_hint, "", false};
            if (c.selected > 0 && c.sizable && c.size_water)
                return {"Select", "Drag the orange handles to size this water, the blue knob for its level, the arrows to move it", qe + " - " + c.del + " deletes - " + c.frame + " zooms to the selection", false};
            if (c.selected > 0 && c.sizable)
                return {"Select", "Drag the orange handles to cut this piece, or the arrows to move it", qe + " - " + c.del + " deletes - " + c.hold_through + "+click: the one behind - " + c.frame + " zooms to the selection", false};
            if (c.selected > 0 && c.size_small)
                return {"Select", "Zoom in to size it (drag the arrows to move)", qe + " - " + c.del + " deletes - " + c.frame + " zooms to the selection", false};
            if (c.selected > 0)
                return {"Select", c.page_pick.empty() ? std::string("drag the arrows to move") : "drag the arrows to move, or " + c.page_pick, qe + " - " + c.snap + " puts it on the ground - " + c.del + " deletes - " + c.hold_through + "+click: the one behind - " + c.frame + " zooms to the selection", false};
            if (!c.page_pick.empty())   // An object page is open but nothing is armed: say what the page does
                return {c.page_name.empty() ? std::string("Select") : c.page_name, (c.page_name.empty() ? "click selects - " : "") + c.page_pick, c.hold_through + "+click: the one behind - " + c.select_all + " all - " + c.shortcuts + " shortcuts", false};
            return {"Select", "click selects - drag on empty ground selects several", c.hold_through + "+click: the one behind - " + c.select_all + " all - " + c.frame + " frames - " + c.shortcuts + " shortcuts", false};
        case Tool::Raise: case Tool::Lower: {
            char cl[200];
            if (c.build_up) std::snprintf(cl, sizeof cl, "drag on the map: holding %s the ground by %.1f m a second (%s+click: a straight line)", t == Tool::Raise ? "raises" : "lowers", double(c.raise_amount), c.hold_free.c_str());
            else std::snprintf(cl, sizeof cl, "drag on the map: one stroke %s the ground by %.1f m (%s+click: a straight line)", t == Tool::Raise ? "raises" : "lowers", double(c.raise_amount), c.hold_free.c_str());
            return sculpt(cl);
        }
        case Tool::Flatten:
            if (c.reversed) return {"Flatten" + std::string(size) + follow, "click picks the height here (nothing is changed)", "release " + c.hold_reverse + " to level to the height set on the left"};
            return {"Flatten" + std::string(size) + follow, "drag to level to the height set on the left", c.hold_through + "+click (or " + c.hold_reverse + "+click) picks the height - " + brush_keys};
        case Tool::Smooth: return sculpt("drag to soften bumps and sharp edges");
        case Tool::Roughen: return sculpt(("drag to add small bumps (" + c.hold_free + "+click: a straight line)").c_str());
        case Tool::Plateau: return sculpt("drag to build flat-topped ground");
        case Tool::PlateauDown: return sculpt("drag to cut the ground down to a flat top");
        case Tool::PaintColor:
            if (c.tint_pick) return {"Colour tint", "click the ground to take its colour", ""};
            return {"Colour tint" + std::string(size), "drag to tint the ground", c.hold_through + "+click picks a colour - " + brush_keys};
        case Tool::PaintTexture:
            if (c.paint_reversed)
                return {"Paint texture" + std::string(size), c.paint_layers ? "drag to rub the chosen texture out" : "drag to paint the other texture", "release " + c.hold_reverse + " to paint the chosen one - " + brush_keys};
            return {"Paint texture" + std::string(size), "drag to paint the chosen texture", "hold " + c.hold_reverse + ": " + (c.paint_layers ? "rub it out" : "the other texture") + " - " + brush_keys};
        case Tool::PaintType:
            if (c.paint_reversed) return {"Boundaries" + std::string(size), "drag to mark ground as Playable", "release " + c.hold_reverse + " to mark the chosen kind - " + size_keys + " - " + c.undo + " undoes a stroke"};
            return {"Boundaries" + std::string(size), "drag to mark where players may go", "hold " + c.hold_reverse + ": Playable - " + size_keys + " - " + c.undo + " undoes a stroke"};
        case Tool::PaintCulture:
            if (c.paint_reversed) return {"Plants" + std::string(size), c.plant_erase ? "drag to clear the chosen plants" : "drag to grow the chosen plants", "release " + c.hold_reverse + " for the other way - " + brush_keys};
            return {"Plants" + std::string(size), c.plant_erase ? "drag to clear the chosen plants" : "drag to grow the chosen plants", "hold " + c.hold_reverse + ": " + (c.plant_erase ? "grow" : "clear") + " - " + brush_keys};
        case Tool::PlaceObject: {
            const std::string sl = slope_hint(c);
            const std::string sp = sl.empty() ? std::string() : sl + " - ";
            char gb[48] = "";
            if (c.grid_m > 0) std::snprintf(gb, sizeof gb, " - Grid %g m (%s)", double(c.grid_m), c.grid_keys.c_str());
            const std::string grid = gb;
            if (c.snapped)
                return {placing(c.thing), (c.snap_what.empty() ? std::string("Joined") : c.snap_what) + " - click places it where it joins",
                        sp + c.turn_l + " / " + c.turn_r + ": another way to join - hold " + c.hold_free + ": place freely"};
            if (c.road && c.road_snapped)
                return {placing(c.thing), "click adds it where it joins the road", sp + c.turn_l + " / " + c.turn_r + " join by another end - " + c.hold_free + " places it freely"};
            if (c.road) return {placing(c.thing), "click to place - it joins an open road end by itself", sp + qe + " - hold " + c.hold_free + ": no snapping" + grid};
            return {placing(c.thing), "click to place - drag to face it", sp + qe + " - " + c.hold_quarter + "+" + c.turn_l + " / " + c.turn_r + " 90 deg - hold " + c.hold_free + ": no snapping" + grid};
        }
        case Tool::ScatterTrees:
            if (c.scatter_empty) return {"Scatter" + std::string(size), "Scatter: pick objects below first", "the map ignores clicks until the brush set has something"};
            if (c.scatter_count >= 0)
                return {"Scatter" + std::string(size), std::string(c.scatter_erase ? "Removing: " : "Scattering: ") + std::to_string(c.scatter_count) + " so far", "release to finish - " + c.undo + " undoes the stroke"};
            return {"Scatter" + std::string(size), "drag to scatter the ticked objects", c.hold_reverse + ": remove them - " + c.hold_free + "+click: a line - " + size_keys + " - " + c.undo + " undoes a stroke"};
        case Tool::PlaceSpawn: return {placing(c.thing.empty() ? "spawn point" : c.thing), "click to place - drag to face it (the arrow is where the player looks)", qe};
        case Tool::PlacePad: return {placing(c.thing.empty() ? "vehicle pad" : c.thing), "click to place - drag to face it", qe};
        case Tool::PlaceStart: return {placing("the player start"), "click where the player starts - drag to face it", qe};
        case Tool::PlaceWaypoint:
            if (c.cam_path) return {"Camera flight path", "click once per point, in order", "right-click when done"};
            return {"Patrol path", "click to add points in order", "right-click when the path is done"};
        case Tool::PickPoint:
            if (c.move_shape) return {"Moving the shape", "click the map to put it down", "right-click leaves it where it is"};
            if (c.link_unit) return {"Pick a unit", "click the unit or marker on the map", "right-click cancels"};
            if (c.obj_targets) return {"Objective targets", "click each unit or building to destroy", "right-click when done"};
            return {"Pick a point", "click the map to set the trigger's point", ""};
        case Tool::MakePond: return {"Pond", "click the middle of the pond: digs a basin and adds water", c.undo + " undoes both"};
        case Tool::PlaceWater: {
            char sz[96];
            std::snprintf(sz, sizeof sz, "%.0f x %.0f m", c.water_w, c.water_l);
            if (c.water_dragging) return {placing(c.thing.empty() ? "water" : c.thing), "release to place it at the size shown", "right-click cancels"};
            return {placing(c.thing.empty() ? "water" : c.thing), std::string("click for a ") + sz + (std::min(c.water_w, c.water_l) < 60.f ? " pond" : " lake") + ", or drag out its size", "later: drag its orange handles to resize it, the blue knob for its level"};
        }
        case Tool::PasteGhost: {
            const std::string what = c.paste_count == 1 ? "1 object" : c.paste_count > 1 ? std::to_string(c.paste_count) + " objects" : std::string("objects");
            if (c.snapped) return {std::string(c.paste_dup ? "Duplicate " : "Paste ") + what, (c.snap_what.empty() ? std::string("Joined") : c.snap_what) + " - click puts it where it joins", qe + " - hold " + c.hold_free + ": place freely"};
            return {std::string(c.paste_dup ? "Duplicate " : "Paste ") + what, "click to put it down", qe};
        }
        case Tool::Ramp:
            if (c.ramp_stage == 1) return {"Ramp", "click where it ends", c.brush + " width - " + c.stop + " starts again", false};
            return {"Ramp", "click where the ramp starts, then where it ends", c.brush + " width - " + c.undo + " undoes it"};
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
