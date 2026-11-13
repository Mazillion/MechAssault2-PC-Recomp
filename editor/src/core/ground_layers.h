// More than two ground textures (P4, PC only): a scenario that opts in carries up to 8 ground
// layers and its own weight map, finer than the colour map (256 x 256 on a square map; UnderCover,
// 1024 x 2048 m, has a non-square one) whose alpha picks between the
// retail pair. The PC runtime blends them on the game's lit terrain draw (maintainer tree,
// nv2a_gpu_terrain.hlsl); retail levels and scenarios without layers keep the original shader.
//
// Layer 0 = MATerrain TerrainDetailMap, layer 1 = TerrainDetailMap 1 (their own repeats,
// TerrainDetailMult/Mult1); layers 2.. = any ground texture, repeats across the map (colour-map UV).
// The weights of a texel are normalised by their sum (all 0 = the retail mix by colour-map alpha).
//
// File (in the .ma2map, scenario.ini [Terrain] Layers = terrain_layers.bin), little endian:
//   header  u32 'MA2L', u32 version 1, u32 layer count N (2..8), u32 weight w, u32 h,
//           u32 layer texture size S, u32 colour map width (0 = 256), u32 colour map height (0 = width)
//           (the runtime only blends the terrain draw whose T0 has exactly this size)
//   N x 128 bytes: float repeats, u32 kind (0 retail pair, 1 extra), char source[120] ("archive|path")
//   weights: h rows (row 0 = colour-map row 0, the north edge), each a run list of
//            (u16 count, 8 bytes w0..w7) covering w texels
//   (N - 2) x S x S RGBA8 layer textures, mip 0 (both sides build the mips with a 2x2 box filter)
#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "mgic.h"
#include "util.h"

namespace ma2 {

constexpr int kGroundMaxLayers = 8;
constexpr int kGroundWeightScale = 4;    // weight texels per colour-map texel, per axis
constexpr int kGroundWeightMax = 2048;
constexpr int kGroundTextureSize = 256;
constexpr const char* kGroundLayersFile = "terrain_layers.bin";

struct GroundLayer {
    std::string archive, path;  // texture source (layers 2..); layers 0/1 come from MATerrain
    float repeats = 64;
    Image image;                // decoded, S x S RGBA (layers 2..)
};

struct GroundLayers {
    bool enabled = false;
    std::vector<GroundLayer> layers;  // size 2..8 when enabled
    int w = 0, h = 0;
    int map_w = 256, map_h = 256;  // the colour map these weights cover
    std::shared_ptr<std::vector<uint8_t>> weights;  // w * h * 8; shared with undo snapshots (copy on write)

    uint8_t* mutable_weights();   // clones the weights when an undo snapshot still shares them
    const uint8_t* texel(int x, int y) const { return weights->data() + (size_t(y) * w + x) * 8; }
    int count() const { return int(layers.size()); }
    // Starts from the retail mix: alpha 255 = layer 0, 0 = layer 1 (bilinear from the colour map).
    // Weights are kGroundWeightScale x the colour map per axis (capped at kGroundWeightMax).
    void init_from_colormap(const Image& colormap);
    // The colour-map alpha an exe without layers should use: w0 / (w0 + w1); where only other layers are, the alpha stays.
    void bake_colormap_alpha(Image& colormap) const;
    // Fill the whole map with one layer.
    void fill(int layer);
    // Drops layer k (>= 2); the later layers and their weights move down one.
    void remove_layer(int k);
    // The texture brush: inside the disc (centre cx, cz in metres on a size_x * size_z map, z up) the layer rises toward 255 and the
    // others fall in proportion, so earlier painting stays wherever the brush did not go. k = strength 0..1 (smoothstep falloff,
    // hardness 0..1 = how much of the radius paints at full strength).
    void paint(int layer, float cx, float cz, float radius, float k, float size_x, float size_z, float hardness = 0);
};

Bytes encode_ground_layers(const GroundLayers& g);
bool decode_ground_layers(const Bytes& b, GroundLayers& g, std::string* err = nullptr);
// Square power-of-two RGBA resample (box when shrinking, bilinear when growing) for layer textures.
Image resample_square(const Image& src, int size);
// Brush weight at distance d of radius R: smoothstep of the remaining radius; hardness 0..1 widens the full-strength core.
inline float brush_falloff(float d, float R, float hardness) {
    float f = (1.f - d / R) / std::max(0.02f, 1.f - hardness);
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    return f * f * (3 - 2 * f);
}
class MgfReader;
// A retail ground texture (an MGIc .tif in an archive), decoded at kGroundTextureSize.
bool load_ground_texture(MgfReader& archive, const std::string& path, Image& out, std::string* err = nullptr);

}  // namespace ma2
