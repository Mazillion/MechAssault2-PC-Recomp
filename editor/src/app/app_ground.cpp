// More than two ground textures (PC only, opt-in per scenario; core/ground_layers.h). The Terrain
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

// (The texture brush on the weight map is part of App::apply_brush since: one stroke = one pass, see app_tools.cpp.)

}  // namespace ed
