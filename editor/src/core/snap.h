// Snapping solver: pure maths, no ImGui, no GPU, so fmt_test can run it. The app builds a Mover (the piece being placed, in
// its own frame) and Sockets (the join points of nearby placed pieces), asks for Candidates and picks one.
// Yaw convention (palette_search.cpp, the placing ghost): world = (c x + s z, -s x + c z), c = cos(yaw), s = sin(yaw); a direction
// (x, z) has angle atan2(x, z) and turning by yaw adds yaw to that angle.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace ma2::snap {

enum class Kind : uint8_t { End, Edge, EdgeCorner, Corner, Centre, Inside, Beside, EndOnEdge };

// One join point of a placed piece (world). fam: 1 wall, 2 bridge, 3 plate, 4 road. For walls and bridges `axis` is the heading of the
// piece's own +long axis; for plates the piece's heading. `half`: the length ends_meet() compares (a wall's half length, a plate's half
// size across the edge); `len`: a plate edge's half length. y = the joint's height; py = the piece's origin height.
struct Socket {
    float x = 0, y = 0, z = 0, ox = 0, oz = 0, axis = 0, half = 0, len = 0, py = 0, roll = 0;
    uint32_t uid = 0;
    int index = 0, fam = 0, obj = -1;
    bool tilted = false;
};

// The piece being placed, in its own frame (its origin at 0, 0). `own` holds its join points (x, z local; ox, oz local outward).
struct Mover {
    int fam = 0;                // 1 wall, 2 bridge, 3 plate, 4 road, 5 building, 0 none
    float cx = 0, cz = 0;       // footprint centre in the piece's own frame
    float hx = 0, hz = 0;       // footprint half sizes; the long side is 2 * max(hx, hz)
    float ratio = 1;            // run spacing / length (LargeWall 0.94 ...)
    float top = 0;              // plates: the model's top above its origin
    float road_w = 0, road_l = 0;
    bool straight = false;      // roads: a straight piece
    std::vector<Socket> own;    // the join points. Roads and walls: their ends. Buildings (fam 5): centre, 4 edge middles (+x -x +z -z), 4 corners (++ +- -+ --)
};
constexpr int kOwnCentre = 100;   // Cand::own: the footprint centre (not an entry of Mover::own)
inline Socket own_point(const Mover& m, int i) {
    if (i == kOwnCentre) { Socket s; s.x = m.cx; s.z = m.cz; return s; }
    return m.own[size_t(i)];
}

struct Cand {
    float ox = 0, oz = 0, yaw = 0;   // the piece's origin and heading when placed this way
    float jx = 0, jz = 0;            // the own join point (local)
    float tx = 0, tz = 0, ty = 0;    // the target point (world; ty = its height)
    float y = NAN;                   // height the join sets for the piece's origin (NaN: none)
    Kind kind = Kind::End;
    uint32_t uid = 0;                // target piece
    int socket = -1, own = -1, prio = 0;
    float d_m = 0, d_px = 0, dj_px = 0;   // joint distance on the free ghost: metres, pixels; dj_px = the pixels of the joint itself
    bool ok = false;                 // passed the reach, cap and heading tests
    bool anchor = false;             // a cross-family candidate (a building or plate: on_plate, road_edge, wall_corner)
    Socket sock;                     // the target socket
};

constexpr float kReachPx = 16, kCapFrac = 0.25f, kCapMin = 2, kCapMax = 12, kYawTol = 0.8727f, kHyst = 1.5f, kTiePx = 2;
constexpr float kPi = 3.14159265358979f;

float wrap_pi(float a);                  // (-pi, pi]
float cap_m(const Mover& m);             // clamp(0.25 x the long side, 2, 12)
void same_family(const Mover& m, const std::vector<Socket>& open, std::vector<Cand>& out);   // fam 1, 2, 3 (edge middles), 4

// A plate (kind 3) or building (kind 5) that other pieces can sit on or beside: its drawn (cropped) footprint in the world. Flat only.
struct Anchor {
    uint32_t uid = 0;
    int kind = 3;
    float cx = 0, cz = 0;   // footprint centre
    float yaw = 0;          // heading; the footprint is axis-aligned in this frame (+x = (cos, -sin), +z = (sin, cos))
    float hx = 0, hz = 0;   // half sizes along its own x / z
    float top = 0;          // height of its top
    float py = 0;           // origin height
    int obj = -1;
};
// Socket ids of a candidate on an anchor: 0 centre, 1..4 edge (+x -x +z -z), 5..8 corner (++ +- -+ --), plate frame.
void on_plate(const Mover& m, const Anchor& a, float fcx, float fcz, std::vector<Cand>& out);                       // fam 5 on a plate
void road_edge(const Mover& m, const Anchor& a, float fcx, float fcz, float fyaw, std::vector<Cand>& out);        // fam 4 beside / ending at a plate edge
void wall_corner(const Mover& m, const Anchor& a, std::vector<Cand>& out);                                         // fam 1 at a corner
// How far (m) two edges overlap along one line when they face each other (opposite normals, collinear within 0.1 m); 0 when they do not.
float edge_overlap(float ax, float az, float aox, float aoz, float alen, float bx, float bz, float box, float boz, float blen);

using ToPx = std::function<bool(float x, float y, float z, float& sx, float& sy)>;           // false = off screen
struct Prev {
    uint32_t uid = 0;
    int socket = -1, own = -1;
    Kind kind{};
    float yaw = 0;
    bool on = false;
    // Q / E while joined: the joint stays the target while the cursor stays within reach of where the key was pressed.
    bool pinned = false;
    uint32_t puid = 0;
    int psock = -1;
    float pfx = 0, pfz = 0, pfy = 0;
};
// Fills d_m / d_px / ok for each candidate (J on the free ghost: centre at (fcx, fcz), heading fyaw), applies the heading, reach and
// cap tests, hysteresis and the pick. Returns the winner's index or -1.
int choose(std::vector<Cand>& c, const Mover& m, float fcx, float fcz, float fyaw, float reach_px, const Prev& prev, const ToPx& to_px);
// Q / E while joined: the yaw of the next candidate at the winner's target joint in direction dir (+1 / -1), by yaw; NaN when it has none.
float next_yaw(const std::vector<Cand>& c, int winner, float cur_yaw, int dir);
float grid_round(float v, float step);
// A piece's kept length rounded to whole grid steps (never below min_side). Sizing by dragging (app_size_gizmo.cpp).
float size_step(float kept, float step, float min_side);
// Snapping the cut edge of a piece being sized. The dragged side: a point on it (ex, ez), its outward normal n and its direction t
// along it (world, unit), half_t = half its length; fam as Socket::fam. Targets: kind 0 = a point (an open end of another piece), 1 = the line
// through (x, z) along (dx, dz) (the edge of a plate, the kerb of a road).
struct EdgeDrag { float ex = 0, ez = 0, nx = 0, nz = 1, tx = 1, tz = 0, half_t = 0; int fam = 0; uint32_t uid = 0; };
struct EdgeTarget { float x = 0, z = 0, dx = 1, dz = 0; int kind = 0; uint32_t uid = 0; int index = 0; };
struct EdgeHit { float s = 0; int kind = -1; uint32_t uid = 0; int index = -1; float d_px = 0; };
constexpr float kEdgeParallelDeg = 2.f, kEdgeLateralMin = 1.f;
// s = how far to move the side along n. A point: s = (T - E).n, accepted when its offset along t is <= max(1 m, half_t). A line: only when it is
// within 2 degrees of t; s = (P - E).n. Accepted when |s| * px_per_m <= reach_px and |s| <= kCapMax; the smallest |s| wins; the previous hit (prev,
// matched by kind, uid and index) is kept while |s| <= kHyst x both limits. Returns whether `out` holds a hit.
bool edge_snap(const EdgeDrag& e, const std::vector<EdgeTarget>& t, float reach_px, float px_per_m, const EdgeHit* prev, EdgeHit& out);
float angle_round(float yaw, float step);   // radians, result wrapped to (-pi, pi]
void filter_translation(std::vector<Cand>& c, float yaw);   // Keep the candidates whose heading is within 1 degree of yaw (groups only slide)

// The piece's footprint centre when its origin is at (ox, oz) with heading yaw.
inline void centre_of(const Mover& m, float ox, float oz, float yaw, float& x, float& z) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    x = ox + c * m.cx + s * m.cz;
    z = oz - s * m.cx + c * m.cz;
}

}  // namespace ma2::snap
