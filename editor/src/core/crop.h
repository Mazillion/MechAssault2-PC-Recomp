// Cropped road pieces and foundations (playtest 11-01: road pieces and foundations should be croppable to
// fit more places). The world format has no scale or size property for either: a road piece is its .road file's prefab
// model, a foundation (building base, parking lot...) is its model's flat plate. A crop is therefore real geometry: the
// model's triangles are clipped to the kept rectangle (texture coordinates interpolated, so the pattern is cut, not
// squeezed) and the scenario's own archive ships the cut model beside the source (MA2SE_<stem>_c<tag>.*), the way
// custom-size water ships its plane (scenario.cpp add_custom_water).
//
// In the editor the crop is one editor-only string property on the object (kProp, "x0 x1 z0 z1": the kept rectangle in
// the model's own x / z, metres). Saving strips it from the world the game reads and keeps it in editor/project.json
// ("crops"), with the object's original model / .road path, so reopening restores both.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "util.h"
#include "wdf.h"

namespace ma2m { class ArchiveSet; }

namespace ma2::crop {

inline constexpr const char* kProp = "EditorCrop";
inline constexpr float kMinSide = 1.f;  // the smallest piece left, metres

struct Rect {
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0;  // kept part, model space
    float w() const { return x1 - x0; }
    float l() const { return z1 - z0; }
};

bool parse(std::string_view s, Rect& r);
std::string format(const Rect& r);
bool of(const WObj& o, Rect& r);           // the object's crop, if it has one
void set(WObj& o, const Rect* r);          // nullptr removes it
std::string tag(const Rect& r);            // 8 hex digits naming the cut files
void clamp_point(const Rect& r, float& x, float& z, float inset = 0.f);

// One vertex with every attribute the game's vertex layouts carry (skinned layouts are not cut).
struct Vert {
    float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, uv0[2] = {0, 0}, uv1[2] = {0, 0}, col[4] = {1, 1, 1, 1};
    uint8_t tan[16] = {};
};
struct Plane { float a = 0, b = 0, c = 0, d = 0; };  // keeps a x + b y + c z + d >= 0

// The four planes of `r` for vertices in a mesh's own space; m = mesh -> model (row vectors, translation in m[12..14]),
// nullptr = the vertices are in model space already.
std::vector<Plane> rect_planes(const Rect& r, const float* m = nullptr);
// Cuts a triangle list to the planes. Vertices fully inside keep their attributes; cut edges get interpolated ones.
// Unused vertices are dropped and indices renumbered.
void clip(std::vector<Vert>& v, std::vector<uint32_t>& tris, const std::vector<Plane>& planes);

// Index helpers: strip <-> list (strips keep their winding; an output strip joins triangles with repeated indices).
void strip_to_list(const std::vector<uint16_t>& strip, size_t nverts, std::vector<uint32_t>& tris);
std::vector<uint16_t> list_to_strip(const std::vector<uint32_t>& tris);

// Game files for a cut model. `model` is the path the game loads (Corporal3DObj, or the .road's model); out gets every
// new file (archive path, bytes) and new_model the cut model's archive path. MGMODEL and legacy .NODE models.
struct OutFile { std::string path; Bytes data; };
bool make_model(const ma2m::ArchiveSet& ar, const std::string& model, const Rect& r, std::vector<OutFile>& out,
                std::string& new_model, std::string* err = nullptr);
// The refusals of make_model without writing anything: false with the tail of "This piece cannot be cut: its model ..." in why
// ("has shadow volumes", "has moving (skinned) parts", "has a vertex layout the editor cannot rewrite", "is not in the game's files").
bool cuttable(const ma2m::ArchiveSet& ar, const std::string& model, std::string* why = nullptr);
// Clips every <bv_obb> collision box of an MGMODEL's XML to the kept rectangle (model x / z), each box on its own, in the box
// owner's node space. Axis-aligned boxes (any order or sign of axes) become their intersection with the rectangle; turned
// boxes keep the part of a 9 x 9 x 9 sample grid inside it; a box with nothing inside becomes a 5 mm box inside the rectangle
// (the element stays, so the XML keeps its shape). Boxes that lie inside are not touched.
std::string clip_volumes(const std::string& xml, const Rect& r);
// A cut road piece: a .road beside the source whose [model] is the cut model, attachments / boundary / AI path points
// moved inside the kept part. new_road is its archive path (forward slashes).
bool make_road(const ma2m::ArchiveSet& ar, const std::string& road, const Rect& r, std::vector<OutFile>& out,
               std::string& new_road, std::string* err = nullptr);

}  // namespace ma2::crop
