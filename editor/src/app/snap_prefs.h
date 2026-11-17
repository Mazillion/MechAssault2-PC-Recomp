// What the snap bar remembers per user: joints, grid, angle steps and the reach. Header-only with no ImGui, so
// tests\fmt_test.cpp checks the file round trip. The ini section is [Snap]; a file without it (an older version's) gives the defaults.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../core/gamedata.h"

namespace ed {

inline constexpr float kGridSizes[] = {1, 2, 5, 8, 10, 20};   // metres
inline constexpr int kGridSizeCount = int(sizeof kGridSizes / sizeof kGridSizes[0]);
inline constexpr float kAngleSteps[] = {1, 5, 15, 45, 90};     // degrees
inline constexpr int kAngleStepCount = int(sizeof kAngleSteps / sizeof kAngleSteps[0]);

struct SnapPrefs {
    bool joints = true;       // road ends, walls, bridges, plates, buildings join what they come near
    bool grid = false;        // placing and moving round the footprint centre to a lattice (off until the user turns it on)
    float grid_m = 5.f;       // one of kGridSizes
    bool angle = true;        // headings from drags, picks, Q / E and the turn ring come in steps
    float angle_deg = 15.f;   // one of kAngleSteps
    float reach_px = 16.f;    // how close (screen pixels) the ghost's own joint must come to a placed piece's: 8 to 32
};

inline int snap_grid_index(float m) {
    for (int i = 0; i < kGridSizeCount; i++)
        if (std::fabs(kGridSizes[i] - m) < 1e-3f) return i;
    return -1;
}
inline int snap_angle_index(float d) {
    for (int i = 0; i < kAngleStepCount; i++)
        if (std::fabs(kAngleSteps[i] - d) < 1e-3f) return i;
    return -1;
}

// The grid size one step smaller / bigger (dir -1 / +1), kept inside the list.
inline float snap_grid_step(float m, int dir) {
    int i = snap_grid_index(m);
    if (i < 0) i = snap_grid_index(5.f);
    return kGridSizes[std::clamp(i + dir, 0, kGridSizeCount - 1)];
}

inline SnapPrefs snap_prefs_from_ini(const ma2::IniFile& ini) {
    SnapPrefs p;
    auto flag = [&](const char* key, bool def) { return ini.get("Snap", key, def ? "1" : "0") != "0"; };
    p.joints = flag("Joints", true);
    p.grid = flag("Grid", false);
    p.angle = flag("Angle", true);
    const std::string gs = ini.get("Snap", "GridSize"), as = ini.get("Snap", "AngleStep"), rp = ini.get("Snap", "ReachPx");
    if (!gs.empty() && snap_grid_index(float(atof(gs.c_str()))) >= 0) p.grid_m = float(atof(gs.c_str()));
    if (!as.empty() && snap_angle_index(float(atof(as.c_str()))) >= 0) p.angle_deg = float(atof(as.c_str()));
    if (!rp.empty()) p.reach_px = std::clamp(float(atof(rp.c_str())), 8.f, 32.f);
    return p;
}

inline std::string snap_prefs_text(const SnapPrefs& p) {
    char b[200];
    std::snprintf(b, sizeof b, "[Snap]\r\nJoints=%d\r\nGrid=%d\r\nGridSize=%g\r\nAngle=%d\r\nAngleStep=%g\r\nReachPx=%g\r\n", p.joints ? 1 : 0, p.grid ? 1 : 0,
                  double(p.grid_m), p.angle ? 1 : 0, double(p.angle_deg), double(p.reach_px));
    return b;
}

}  // namespace ed
