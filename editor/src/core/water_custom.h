// Water of any size. MAWater has no size property: the plane is the bounding box of its model, so a
// different size needs a different model. The vertex and index files of a plain water model (one
// flat grid, one material_water) are rebuilt here for a width x length rectangle, centred on the
// origin with the source's texture density, and the scenario's own archive ships them.
#pragma once
#include <string>

#include "util.h"

namespace ma2::wcustom {

struct Source {
    Bytes verts, idx;  // the model's "{... verts}" and "{... indices}" files as stored
};

// True when the model's vertex file is one flat, up-facing grid this library can resample (the
// retail plain water tiles: Env_Water_250_250, Water_Sw_500x500, Water_Sw_1000x1000, ...).
bool plain(const Source& src);

// What the water covers inside its w x l bounding box: the whole rectangle, or an oval (ellipse) inscribed in it.
// The game draws the model's triangles, so the outline is real; the bounding box still sizes the plane.
enum class Shape { Rect, Oval };

// The vertex and index files for a w x l metre water (each side 8 .. 1000 m). Rect keeps the source's triangle
// strips; Oval is a triangle list (*as_list = true: the model's meshxml type must read "indexedlist").
bool generate(const Source& src, float w, float l, Shape shape, Bytes& verts, Bytes& idx, bool* as_list, std::string* err = nullptr);

// The width / length of a generated vertex file (its bounding box); false when it is not one.
bool extent(const Bytes& verts, float& w, float& l);

constexpr float kMinSide = 8.f, kMaxSide = 1000.f;

}  // namespace ma2::wcustom
