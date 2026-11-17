// Where flat things sit: roads and thin foundation plates are "sheets". Each sheet has a layer (the order it
// was put down in; editor-only, kept in editor/project.json "layers"), and a height worked out from the ground under it
// and the sheets below it. The last sheet placed goes on top. Pure geometry: no GPU, no UI, linked into fmt_test.
//
//   a sheet rests kSheetLift above the highest ground under its footprint, or kSheetStep above the top of every lower
//   sheet it overlaps, whichever is higher.
//
// "Fixed" sheets keep the height they have (tilted, glued, hand-lifted, or from files made before layers existed): they
// are never moved by the stack, but they still hold up the layers above them.
#pragma once
#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "terrain.h"
#include "wdf.h"

namespace ma2::place {

inline constexpr float kSheetLift = 0.08f;     // underside of a sheet above the highest ground under it
inline constexpr float kSheetStep = 0.03f;     // underside of a sheet above the top of the sheet below it
inline constexpr float kOverlapMargin = 0.25f; // footprints that overlap by less than this do not count (joined plates, road ends)
inline constexpr float kSheetMaxThick = 0.5f;  // a plate thicker than this is a block, not a sheet
inline constexpr float kStandTol = 0.30f;      // an object "stands on" a surface when its foot is within this of it
inline constexpr const char* kLayerProp = "EditorLayer";
inline constexpr const char* kBasinProp = "EditorBasin";  // a water remembers the basin dug under it; editor-only, kept in project.json "basins"

struct Rect { float x0 = 0, x1 = 0, z0 = 0, z1 = 0; };  // model space: x across, z along (a road runs 0 .. -L)

struct Sheet {
    uint32_t uid = 0;
    Vec3 pos;                              // object origin
    float yaw = 0, pitch = 0, roll = 0;    // radians, the editor's orient_instance convention
    Rect fp{};                             // footprint, cropped
    float under = 0, top = 0;              // model-space y of the two faces
    int layer = 0;
    bool fixed = false;
    int run = -1;                          // plates joined edge to edge share a run id; -1 = alone
    int kind = 0;                          // 1 = road piece, anything else = plate. Where two sheets share a layer, the plate is under the road.
    bool level() const { return pitch > -1e-5f && pitch < 1e-5f && roll > -1e-5f && roll < 1e-5f; }
};

Vec3 up_of(float yaw, float pitch, float roll);                       // column 1 of the orient_instance matrix
// Inverse of up_of for a given yaw: the pitch and roll that give this up vector (|roll| < 90 deg).
void tilt_for_up(Vec3 up, float yaw, float& pitch, float& roll);
// Where the model-space point (x, y, z) lies relative to the origin once the object is turned by yaw, pitch, roll (the footprint offset).
Vec3 rotate_local(float yaw, float pitch, float roll, float x, float y, float z);
float face_y(const Sheet& s, float face, float x, float z);           // world y of that face at map point (x, z)
bool contains(const Sheet& s, float x, float z);                      // map point inside the turned footprint
// Two footprints overlap by more than `margin` (each is shrunk by margin / 2 first): touching edges do not count.
bool overlap(const Sheet& a, const Sheet& b, float margin = kOverlapMargin);
float ground_max(const Terrain& t, const Sheet& s);                   // highest height_at under the footprint (exact on the game's triangles)
float ground_min(const Terrain& t, const Sheet& s);
float top_over(const Sheet& lower, const Sheet& upper);               // highest point of lower's top face inside upper's footprint
int top_layer_under(const std::vector<Sheet>& all, const Sheet& s);   // highest layer among sheets overlapping s (s excluded), INT_MIN if none
int bottom_layer_under(const std::vector<Sheet>& all, const Sheet& s);  // lowest layer among sheets overlapping s, INT_MAX if none
// The origin height sheet s takes: on the ground (+ kSheetLift) or on the lower sheets it overlaps (+ kSheetStep).
// "Lower" = smaller (layer, uid). A sheet in a run is seated with its run (a what-if on a copy), so a joined plate's
// answer is the height the click will give it.
float rest_y(const Terrain& t, const std::vector<Sheet>& all, const Sheet& s);
// Recomputes pos.y of every sheet that is not fixed. Returns, per sheet index, how far it moved (0 = not).
std::vector<float> seat(const Terrain& t, std::vector<Sheet>& all);
// The layer a sheet takes when it is put down (placed, pasted, or dragged onto sheets it did not overlap before) among `all`
// (s itself, and its run mates, are skipped). Roads go on top of everything they overlap; a plate goes on top of the plates it
// overlaps but under the roads (retail: every road lies above the plates near it), whatever order they were put down in; only if
// a plate was ordered above a road by hand does it go on top. `s.layer` = INT_MIN for a new sheet, else its current layer (a drag
// never lowers a layer except to slip a plate under a road). `only` limits the sheets looked at (by uid). To make room under a
// road every layer from the road's up is moved one up in `all` (order unchanged): the caller writes back the layers that changed.
int drop_layer(std::vector<Sheet>& all, const Sheet& s, const std::vector<uint32_t>* only = nullptr);

// The top surface under map points: the highest top face of a sheet that contains the point and lies within [min_y, below_y]
// (a tilted sheet at its plane), but never below the ground; else the ground. `on` = that sheet's uid, 0 for the ground.
// SurfaceIndex keeps the sheets' boxes so many queries stay cheap.
class SurfaceIndex {
public:
    explicit SurfaceIndex(const std::vector<Sheet>& all);
    float at(const Terrain& t, float x, float z, float below_y = 1e30f, uint32_t* on = nullptr, float min_y = -1e30f) const;
    bool covers(float x, float z) const;   // some sheet's box contains the point
private:
    const std::vector<Sheet>* all_;
    std::vector<float> bx0_, bx1_, bz0_, bz1_;
};
float surface_at(const Terrain& t, const std::vector<Sheet>& all, float x, float z, float below_y = 1e30f, uint32_t* on_uid = nullptr);
// How far the lowest ground is below a level sheet resting on the highest: 0 on flat ground. A tilted sheet: how far its
// underside is above the ground at the worst point, less kSheetLift (what is left floating after the tilt).
float float_gap(const Terrain& t, const Sheet& s);
// What really hangs: a level sheet's low side over the lowest ground (its underside less kSheetLift above ground_min, 0 or more), at
// the height it is at. float_gap says how uneven the ground under it is, whatever its height: a base resting on a levelled plate beside
// higher ground that stays has no gap here. Same as float_gap for a sheet resting on the highest ground, and for a tilted one.
float hang_gap(const Terrain& t, const Sheet& s);
// Slopes: lays a sheet on the slope under it. Free (joint_fixed false): the least-squares plane through the ground samples
// (2 m grid over the footprint) gives pitch and roll (each at most kMaxTilt), then it is raised until its smallest clearance over the
// ground is kSheetLift. Joined (joint_fixed): the pivot (local pivot_x, 0, pivot_z; the origin by default) stays where it is (s.pos.y
// is its height), roll = joint_roll, and the pitch is the smallest at which no sample at least 4 m along from the pivot is closer than
// kSheetLift to the ground. Writes pitch, roll and pos.
void fit_tilt(const Terrain& t, Sheet& s, bool joint_fixed, float joint_roll, float pivot_x = 0.f, float pivot_z = 0.f);
// Smallest height of the underside over the ground, over the footprint samples (kSheetLift on a piece that fit_tilt just laid).
float min_clearance(const Terrain& t, const Sheet& s);
inline constexpr float kMaxTilt = 0.4363323f;  // 25 degrees
inline float float_tol(const Terrain& t) { return std::max(0.75f, 1.5f * t.step()); }

// The grid points (gx, gz) of a terrain with cells of `scale` metres that lie inside the footprint grown by `grow_cells` cells on
// every side (a ground tool leaves these alone). Not limited to the terrain: the caller clamps.
std::vector<std::pair<int, int>> grid_under(const Sheet& s, float scale, int grow_cells);

// The four corners of the footprint on the map (x[i], z[i]), in order around it.
void corners(const Sheet& s, float x[4], float z[4]);

// The layer property on an object ("<int>" or "<int> fixed").
bool parse_layer(std::string_view s, int& layer, bool& fixed);
std::string format_layer(int layer, bool fixed);
bool layer_of(const WObj& o, int& layer, bool& fixed);   // false when the object has none
void set_layer(WObj& o, int layer, bool fixed);

// The basin a water remembers: the rectangle of grid points it dug and the step (0..255) of each point before the dig and after it.
std::string basin_encode(int gx0, int gz0, int nx, int nz, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, const std::string& extra = {});  // extra: free text after the two hex runs (the editor keeps the water's footprint there)
bool basin_decode(const std::string& s, int& gx0, int& gz0, int& nx, int& nz, std::vector<uint8_t>& before, std::vector<uint8_t>& after, std::string* extra = nullptr);
// Pairs (indices into `all`) of overlapping level sheets whose tops are within `tol` m: they flicker where they overlap.
std::vector<std::pair<int, int>> same_height_pairs(const std::vector<Sheet>& all, float tol = 0.015f);

}  // namespace ma2::place
