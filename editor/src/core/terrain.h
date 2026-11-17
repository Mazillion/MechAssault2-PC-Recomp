// Terrain heightfield model.
//
// Verified against retail navigation nodes (fmt_test): the MATerrain heightfield is a W x H
// grid of 8-bit samples with TerrainWorldScale (8 m) spacing; world x runs along a stored row,
// world z runs against the stored rows (stored row r is z = (H-1-r) * scale), and
//     height = TerrainMinHeight + v / 255 * (TerrainMaxHeight - TerrainMinHeight).
// Retail grid nav nodes sit exactly 2 m above that surface.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "mgic.h"
#include "wdf.h"

namespace ma2 {

struct Terrain {
    // Sanity limits for a loaded file (retail worlds are at most 257 squared, as is what the editor makes).
    static constexpr int kMaxSize = 1025;
    static constexpr float kMinScale = 0.25f, kMaxScale = 1000.f;
    int w = 0, h = 0;          // samples along x and z
    float scale = 8.f;         // metres per cell
    float min_h = -20.f, max_h = 100.f;
    std::vector<float> hgt;    // metres, index gz * w + gx (gz along +z, NOT the stored row order)
    Mgic style;                // original heightfield container (kept for PAL/ver chunks)

    float size_x() const { return float(w - 1) * scale; }
    float size_z() const { return float(h - 1) * scale; }
    float at(int gx, int gz) const;
    float& ref(int gx, int gz) { return hgt[size_t(gz) * w + gx]; }
    // The ground the game has: the saved 8-bit steps, each cell split on the diagonal (gx,gz)-(gx+1,gz+1), clamped to the map.
    // at(), ref() and hgt are the brushes' private fine working layer; everything else reads height_at or step_at.
    float height_at(float x, float z) const;
    float fine_at(float x, float z) const;   // the fine layer, bilinear: the brushes' view, for debugging strokes only
    float step() const { return std::max(0.001f, max_h - min_h) / 255.f; }   // metres per stored step
    float lowest() const;                                                    // the lowest ground (the lowest saved step)
    float step_at(int gx, int gz) const { return stored(at(gx, gz)); }       // the ground at a grid point (what the game has)
    Vec3 normal_at(float x, float z) const;
    bool inside(float x, float z) const { return x >= 0 && z >= 0 && x <= size_x() && z <= size_z(); }

    bool load_from_world(const Wdf& w, std::string* err = nullptr);
    // Writes heights (quantized to 8 bits between min_h and max_h) and the range into MATerrain.
    bool store_to_world(Wdf& w, std::string* err = nullptr) const;
    // Quantize to the stored representation and back (what the game will see).
    std::vector<uint8_t> samples() const;  // stored row order
    void from_samples(const std::vector<uint8_t>& s);
    // A height as it will be stored (nearest 8-bit step), and the lowest stored step at or above h.
    float stored(float h) const;
    float stored_at_least(float h) const;

    void resize_flat(int w, int h, float height);
    // Ray from origin along dir (normalized); returns true and t on hit.
    bool raycast(const Vec3& o, const Vec3& d, float& t, float max_t = 20000.f) const;
};

// Bakes an 8-bit lightmap (A8L8 luminance, alpha = 255) of size lw x lh for the sun direction
// (MAEnvironment DirectionalLightDirection, pointing from the sun) with soft terrain shadows.
Image bake_lightmap(const Terrain& t, Vec3 sun_dir, int lw, int lh, float ambient = 0.35f);

}  // namespace ma2
