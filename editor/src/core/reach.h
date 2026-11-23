// "Can it get here?": which ground a unit can reach from a team's spawns (spec REACH-SPEC.md section 3).
// Pure core module (no ImGui, no D3D). The rules and the unit list live in reach_rules.h; this file is the flood.
//
// Grid: w x h cells of `cell` metres, cell (cx, cz) has its centre at ((cx + 0.5) * cell, (cz + 0.5) * cell); index cz * w + cx;
// +x is east, +z is north (the same axes as ma2::Terrain).
#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "reach_rules.h"

namespace ma2 {

struct Terrain;

enum : uint8_t {  // ReachInput::flags
    F_OOB = 1,        // out of bounds (type map): nothing may stand or fly here
    F_BREAKABLE = 2,  // `top` is only trees / breakable scenery: heavy bodies crush it
    F_LATCH = 4,      // an object wall face (MACorporeal / MABuilding): the BA claw can grip it
    F_WATER = 8,      // water over this cell (the bed is the walking surface)
    F_DECK = 16,      // a thin deck: a bridge or roof with open air under it (the raster helper sets it; Bots can walk under it)
};

struct ReachSpawn {
    float x = 0, z = 0;
    int team = 0;
};

struct ReachInput {  // filled on the main thread, read-only for the worker
    int w = 0, h = 0;
    float cell = 2.f;
    std::vector<float> th;        // terrain height per cell centre (Terrain::height_at, the saved steps)
    std::vector<float> top;       // highest solid object top per cell, -1e9 if none
    std::vector<uint8_t> flags;   // F_*
    std::vector<float> water_y;   // water surface or NaN (the readout shows the depth)
    std::vector<float> low;       // optional: lowest horizontal object surface per cell, +1e9 if none (the raster helper fills it)
    std::vector<ReachSpawn> spawns;  // player starts: MALevelStart, team 1 red, 2 blue, 0 campaign
    std::vector<ReachSpawn> flyers;  // placed VTOL objects (team 0 = anyone): start points of the VTOL flood and of a BA ride
    float vtol_cap = 55.f;        // MaxVTOLHeight of the placed `VTOL - Player` instance

    size_t count() const { return size_t(w) * size_t(h); }
    // Sizes every array: ground 0, no objects, no flags, no water.
    void init(int cells_x, int cells_z, float cell_m);
    bool valid() const;
};

// Fills w, h, cell and th from a terrain (height_at at the cell centres; cell = max(2, map size / 1024)); clears the rest.
void reach_input_from_terrain(ReachInput& in, const Terrain& t);
// Rasterises world-space triangles (9 floats each: x y z x y z x y z) into `top`: up-facing ones (|ny| >= 0.5) at the cell centres they
// cover; steep faces mark their xz cells with the face's highest vertex and, when `latch` is set, F_LATCH. Call reach_raster_finish once
// after the last model.
void reach_raster_tris(ReachInput& in, const float* tris, size_t tri_count, bool latch);
void reach_raster_finish(ReachInput& in);

enum : uint8_t { R_NONE, R_WALK, R_JET, R_LATCH, R_FLIGHT, R_WALL, R_OOB };
// R_NONE: standable but never reached; R_WALK: reached by walking / driving (and one-way step-offs; a VTOL: can land);
// R_JET: only with jets; R_LATCH: only by claw-climbing an object; R_FLIGHT: only by flight (VTOL: can fly over, can't land);
// R_WALL: nobody can stand here (cliff face, blocked); R_OOB: out of bounds.

enum : uint8_t { L_DROP, L_UNDER, L_UP, L_HOP, L_DIVE, L_LATCH, L_RIDE };
struct ReachLink {  // one non-walk link the flood used
    int32_t from = 0, to = 0;
    uint8_t kind = L_DROP;  // L_*
    uint8_t cls = R_WALK;   // R_* of the cells it opened up
    float rise = 0, run = 0;
};

struct ReachFace {  // a cliff / face seen from its top: one per 8 m block per direction
    int32_t top = 0, bot = 0;  // the free cell above the face, the free cell below it
    float rise = 0, run = 0, band_top = 0;
    uint8_t dir = 0;           // 0..7 from the top towards the bottom: E NE N NW W SW S SE (+x east, +z north)
    uint8_t up = R_NONE;       // can a unit at the bottom get to the top: R_NONE, R_JET (a jet leg), R_LATCH (the claw: a Battle Armor suit on an object wall)
    uint8_t down = R_NONE;     // can a unit at the top get to the bottom: R_NONE, R_WALK (step-off), R_JET (dive)
};

struct ReachResult {
    int w = 0, h = 0;
    float cell = 2.f;
    std::vector<uint8_t> cls;   // R_*
    std::vector<int32_t> comp;  // walk component of a standable cell (both-ways edges only), -1 elsewhere
    std::vector<ReachFace> faces;
    std::vector<ReachLink> links;  // non-walk links (capped), for tests and the readout
    int counts[7] = {};            // cells per R_*
    int spawn_total = 0, spawn_ok = 0;  // spawns of the team / spawns that snapped to usable ground
    bool cancelled = false;
    double ms = 0;
};

// Floods from the spawns of `team`. `cancel` is checked every 4096 pops; a cancelled result is partial (cancelled = true).
ReachResult reach_compute(const ReachInput& in, const ReachProfile& profile, int team, const std::atomic<bool>* cancel = nullptr);

// The readout text for the chosen unit at world (x, z): line 1 is the verdict for the cell ("Stiletto here: yes, walks"), then one
// line per nearby cliff ("Cliff to the east, 29 m: down only (jets lift about 23 m, est.)"), separated by newlines.
// "(est.)" ends a line (or its bracket) once when an estimated rule decided it.
std::string reach_explain(const ReachInput& in, const ReachResult& r, const ReachProfile& profile, float x, float z);

// The pieces of the hover readout and of the "Compare all" table row, for the cell at world (x, z).
struct ReachAnswer {
    std::string label;      // "Stiletto", "Stiletto (bots)"
    uint8_t cls = R_NONE;   // R_* of the cell
    bool outside = false;   // the point is off the map
    std::string verdict;    // "yes, walks" / "yes, jets only" / "can stand here, can't get here" / "can't stand here ..."
    std::string why;        // red cell only: "every way up is a wall above 23 m"
    std::string notes;      // water depth, start point problems
    std::string here, how;  // short cells of the table: "Yes", "jets only" / "No", "wall above 23 m"
    std::string cliff;      // short cell: "E 20 m: up" / "W 34 m: down only"
    std::vector<std::string> cliffs;  // up to 3 lines, nearest first: "Cliff to the east, 29 m: down only (jets lift about 23 m, est.)"
    bool est = false;       // an estimated rule decided the verdict line
};
ReachAnswer reach_answer(const ReachInput& in, const ReachResult& r, const ReachProfile& profile, float x, float z);

const char* reach_compass(int dir8);  // "east", "north-east", ...

}  // namespace ma2
