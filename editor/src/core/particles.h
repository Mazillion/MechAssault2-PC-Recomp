// The game's particle effects (.mgparticle, XML) as the editor's optional effects view draws them.
//
// Only what the sprite look needs is read: emitters (looped or one-way, point / box / sphere /
// disk source, pitch cone), kinetic systems (rate, lifespan, initial velocity, gravity, drag,
// offset) and their mgSpriteType prototype (size, aspect, spin, colour curve over the life,
// texture, additive / alpha blend). Ribbons, lightning, debris models, sub-emitters and the
// collision controllers are skipped. The simulation is a closed form in time (no stored state):
// the same effect at the same time always draws the same sprites.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ma2 {

// A key curve over normalised time (0..1), 1 or 4 components, with the file's per-value variance.
struct PCurve {
    struct Key { float t = 0; float v[4] = {0, 0, 0, 0}; };
    std::vector<Key> keys;
    float variance = 0;
    bool empty() const { return keys.empty(); }
    float eval(float t, int comp = 0) const;  // linear between keys, clamped at the ends
    float first(float def = 0) const { return keys.empty() ? def : keys[0].v[0]; }
};

struct PSprite {
    std::string texture;              // archive path as written ("\Texture_Dump\SpecialEffects\fire4.tif")
    int blend = 0;                    // 0 alpha, 1 additive, 2 subtractive
    PCurve size, aspect, spin, color; // color: r g b a, 0-255
    float spin_offset = 0;            // initial angle is random in 0..spin_offset
    float pivot[2] = {0, 0};          // anchor inside the quad, in quad sizes (0,-0.5 = bottom centre)
    bool orient_to_velocity = false;
    float stretch = 1;
};

struct PEmitter;

struct PSystem {
    bool kinematic = false;           // mgKinematicSystem (velocity set per particle) vs mgKineticSystem
    float gravity = 0, drag = 0, inherit = 0;
    float offset[3] = {0, 0, 0};
    PCurve rate, lifespan, velocity;
    bool has_sprite = false;
    PSprite sprite;
    // mgEmitterType prototypes: each particle of this system carries these emitters (a spark shower at
    // every glow particle), running on the particle's own clock from its birth.
    std::vector<PEmitter> subs;
};

struct PEmitter {
    bool looped = false;
    float cycle_length = 1;
    bool worldspace = true;
    enum Source { Point, Box, Sphere, Disk } source = Point;
    PCurve length[3];                 // box extents (full lengths) x y z
    PCurve pitch_start, pitch_width;  // direction cone about +Y, degrees
    PCurve radius_start, radius_width;
    std::vector<PSystem> systems;
};

struct ParticleFx {
    float lod_start = 250, lod_range = 100;
    std::vector<PEmitter> emitters;
    float max_life = 0;               // longest particle life (s), for the simulation window
    bool loops() const;               // at least one looped emitter with a sprite system
    bool has_sprites() const;         // any emitter with a sprite system (one-way ones included)
};

bool parse_particle(const std::string& xml, ParticleFx& out);

// One drawn sprite (world space, before the camera's right / up are applied).
struct PSpriteOut {
    float pos[3];
    float w, h;                       // metres
    float angle;                      // radians about the view axis
    float vel[3];                     // world velocity (orient_to_velocity)
    float rgba[4];                    // 0..1, curve colour (already faded by the LOD)
    const PSprite* sprite;
};

// Appends the sprites of `fx` placed at `origin` at time t (seconds). `seed` decorrelates
// instances. Looped emitters run continuously; `on_time` / `off_time` (seconds, 0 = always on)
// gate their spawning like the .mtb's AliveTime / DeadTime. One-way emitters are drawn only when
// `retrigger` is set (the .mtb's Behavior 1: the effect is started again every on + off seconds).
// `rot` (optional, 3x3 row-major, world = rot * local) turns the emitter's frame: the box source, the system offset
// and the launch direction are in the node's frame (emit_worldspace false), gravity stays world down.
void simulate_particle(const ParticleFx& fx, const float origin[3], float t, uint32_t seed, float fade,
                       float on_time, float off_time, bool retrigger, std::vector<PSpriteOut>& out, const float* rot = nullptr);

}  // namespace ma2
