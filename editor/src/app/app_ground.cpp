// P4: more than two ground textures (PC only, opt-in per scenario; core/ground_layers.h). The Terrain
// tab's two-texture list grows into up to 8 layers; the texture brush then paints the weight map
// (layers persist: painting one pushes the others down only under the brush) and "cover all" fills it.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "imgui.h"
#include "ui_util.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

// The texture brush on the weight map (GroundLayers::paint): the chosen layer rises toward 255 under the
// brush and the others fall in proportion, so earlier painting stays wherever the brush did not go.
void App::paint_ground_layers(float cx, float cz, float R, float k) {
    GroundLayers& g = doc_->ground;
    g.paint(std::clamp(paint_texture_, 0, g.count() - 1), cx, cz, R, k, doc_->terrain.size_x(), doc_->terrain.size_z(), brush_hardness_);
    r_.set_ground_weights(g);
    doc_->dirty = true;
}

}  // namespace ed
