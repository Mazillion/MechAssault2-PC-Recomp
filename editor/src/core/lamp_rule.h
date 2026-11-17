// Lamp parts and light cones: which model parts the game draws. Pure: fmt_test checks it on game data.
//
// "the bridge-foot lamps draw cones in the editor, none in the game" is not a lamp-part rule. The lamps at the bridge
// foot are the near bank's lamp column of the same Canal_Wall_ST (one model holds both banks of the canal, 139 x 140 m);
// the "canal-wall lamps" are its far bank. The cone mesh (vcone, Light_016_spot, additive) is a plain visible mesh with no
// node_light and no .mtb rule, and its material is single-sided (two_sided="false"). Seen from behind (the near bank's
// cones point away from a player on the near side), the game culls it like any single-sided face; the editor drew every
// transparent mesh without culling. The rule: a single-sided mesh is back-face culled whatever its blend mode.
//
// Lamp parts (the existing rule, kept as the D9 fallback): a part under a node that the .mtb hides (or that is turned off)
// whose subtree holds a node_light (Env_Highway_Light: lightbulb > spotlight + cone) is drawn, unlit. The game draws the
// highway-lamp bulb at night on Mission_01b (wiki, runtime-observed); the code-confirmed fact that nothing switches a plain
// MACorporeal lamp at run time leaves the day case open (no pair in G0: no highway lamp in view).
#pragma once
#include <vector>

namespace ma2 {

// Back-face culling for one model mesh. Ground decals keep both faces (they lie on the ground, seen from above).
inline bool cull_back_faces(bool two_sided, bool decal) { return !two_sided && !decal; }

// One node on the path from a mesh to the model root (mesh node first).
struct LampNode {
    bool hidden_by_mtb = false, turned_on = true, holds_light = false;  // holds_light: a node_light in its subtree
};
// The lamp-part rule: is a mesh that the .mtb hides drawn anyway (as a lit lamp part)?
inline bool lamp_part_drawn(const std::vector<LampNode>& path_to_root) {
    for (const LampNode& n : path_to_root)
        if ((n.hidden_by_mtb || !n.turned_on) && n.holds_light) return true;
    return false;
}

}  // namespace ma2
