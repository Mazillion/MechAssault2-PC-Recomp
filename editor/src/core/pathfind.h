// Campaign-AI pathfinding graphs (PATHFINDING/*_Mech.map, *_Traffic.map), retail text format:
//   [Params]
//   Nodes:	  = <n>	...	; Number of nodes
//   Quadrants: = 0	...	; Number of quadrants
//   [Nodes]
//    = <id> <type> <flags> <x> <y> <z> <link count> <owner uid>
//    = <linked id>            (link count lines)
//   [Quadrants]
// Retail terrain grid nodes are type 4 on a 20 m lattice, 2 m above the ground, with
// id = gx * stride + gz + 1; nodes of other types (1-3) follow roads and buildings.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "terrain.h"

namespace ma2 {

struct NavNode {
    int id = 0, type = 4, flags = 0, owner = 0;
    Vec3 pos;
    std::vector<int> links;
};

struct NavGraph {
    std::vector<NavNode> nodes;
    void parse(std::string_view text);
    std::string serialize() const;
};

struct NavGenParams {
    float cell = 20.f;          // lattice spacing (retail: 20 m); widened to fit max_nodes
    float lift = 2.f;           // node height above ground (retail: 2 m)
    float max_step = 9.f;       // max height difference between linked neighbours at 20 m
    float max_slope_deg = 40.f; // per-node ground slope limit
    float edge_margin = 8.f;    // keep nodes away from the map border
    std::vector<Vec3> seeds;    // player starts / spawns: only nodes reachable from them are kept
    int max_nodes = 2500;       // campaign-AI memory budget (retail graphs: 535-2,000 nodes)
};

// blocked(x, z) returns true where a 'Mech cannot stand (deep water, impassable terrain types,
// building footprints). The callback may be empty. Nodes not reachable from a seed are dropped;
// if more than max_nodes remain, the lattice spacing grows until they fit.
NavGraph generate_nav(const Terrain& t, const NavGenParams& p, const std::function<bool(float, float)>& blocked);
NavGraph empty_nav();

}  // namespace ma2
