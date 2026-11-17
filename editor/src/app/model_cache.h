// GPU cache of retail models decoded by the models helper's library (models,
// namespace ma2m). Models load on demand within a per-frame time budget.
#pragma once
#include <d3d11.h>

#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "../core/particles.h"
#include "../core/util.h"
#include "../core/wdf.h"
#include "ma2models.h"

namespace ed {

// MAModSubObjectBlend on the mesh's node (Mission_13b sky light rays): the draw's alpha runs cur += dt x delta x sign inside [mn, mx];
// PingPong turns the sign at an end (triangle wave), Loop restarts at the saved value. alpha_blend: GetOrigAlpha false = alpha blend.
struct AlphaRamp {
    bool on = false, pingpong = false, loop = false, alpha_blend = false;
    float mn = 0, mx = 1, delta = 0, cur = 0, sign = 1;
    float at(float t) const;  // alpha t seconds after the level starts
};

struct GpuMesh {
    AlphaRamp ramp;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    UINT count = 0;
    ID3D11ShaderResourceView* srv = nullptr;  // owned by the texture cache
    int alpha = 0;            // ma2m::AlphaMode
    bool two_sided = false, unlit = false;
    float color[4] = {1, 1, 1, 1};  // diffuse * tint, opacity
    int address = 0;          // texture addressing: bit 0 mirror U, bit 1 mirror V (else wrap)
    // The material's <basetexture> sampling, which the game honours (M03 sky trace: mipmap_bias 0.5 -> TEXFILTER
    // 02062080, mipmapping false + FILTER_POINT -> 01012000). Used for the sky domes.
    bool tex_point = false, tex_nomip = false;
    float tex_bias = 0;
    // COMPLEX materials: the multi mask (owned by the texture cache); G x glow[0] + B x glow[1]
    // are added unlit after lighting (the road lane lights).
    ID3D11ShaderResourceView* multi = nullptr;
    float glow[2][3] = {};
    // A part the model ships turned off in a model that carries lights (lamp bulbs, beam cones):
    // the game switches it on; drawn unlit only with View > Scene lights.
    bool lamp_part = false;
    // A mesh under the building's 'lights' group node: the game's MABuilding switches that group by IsNightTime.
    bool night_light = false;
    // material_env (cockpit / canopy glass): base x diffuse x light + reflectivity x the level's env cube.
    float reflectivity = 0;   // negative: envmaptype "masked" (lerp toward the cube instead of adding it)
    // DETAIL materials (second UV set): 1 = decal lerp(base, 2 x base x detail, 0.5) (KillingField
    // paving); 2 = alpha overlay rgb lerp(base, detail, 0.5), a = base.a x detail.a (swamp moss);
    // 3 = additive glow card lerp(base, detail, 0.835); 4 = COMPLEX + detail (KillingField
    // trace line 10124): base' = lerp(env cube, base, base.a), lerp(base', 2 x base' x detail,
    // (1 - multi.a) x detail_level).
    ID3D11ShaderResourceView* detail = nullptr;
    int detail_mode = 0;
    // Mode 1 mix: the .mat's blend_level for blend="modulate" (Mission_13b cypress trunks, modulate
    // 0.8 = the trace's lerp 0.8); 0.5 otherwise. Mode 4: detail_blend_level (default 1).
    float detail_level = 0.5f;
    int detail_kind = 0;       // detail_blend: 0 modulate2x, 1 alphablend, 2 additive2x (modes 1 and 4)
    // A ground decal drawn as dst x src x 2 over the lit terrain (pass 2). Nothing sets it yet: the
    // KillingField "paving" decal turned out to be the terrain micro-detail pass.
    bool decal = false;
    bool tint1 = false;        // COMPLEX colorshift1_diffuse: light x lerp(1, colorshift1, mask.b)
    float spec[3] = {};        // material_specular_mask: + base.a x cube.a x spec_color (mask inferred)
    bool complex_env = false;  // COMPLEX + own cube, no detail: base' = lerp(cube, base, base.a)
    // material_color_shift (the sky domes): + colorshift0_color x base.a, unlit (Ill Wind sky trace: out = T.rgb + C0 x T.a).
    float cshift[3] = {};
    float ymax = 0;  // highest vertex (model space): a sky mesh reaching above the horizon is a dome
    float skycol[3] = {1, 1, 1};  // diffuse + selfillum unclamped (<= 2): the sky's V0 = (Kd + Ke) / 2 goes through the x2 (Dusk02 sky_main Ke 0.898 -> 1.9x, BD trace d0 0.949)
    float bloom[3] = {0, 0, 0};  // bloom mask weights: colorshift0_bloom, colorshift1_bloom, specular_bloom (the game writes them to the framebuffer alpha)
    bool translucent = false;    // material shading "translucent" (trees, bushes): lit with N.L = 1
    int bloom_kind = 0;          // 1 material_color_shift, 2 COMPLEX, 3 specular_mask; 0 none
    bool spec_detail = false;  // specular_mask: the mask is the material's detailtexture .r
    bool spec_multi = false;   // COMPLEX: the specular mask is multi.r, not base.a
    bool spec_crystal = false; // COMPLEX + detail + cube + glow (M12 crystals, program 08C21): spec = detail.a x cube.rgb (research)
    // normal_map slot: tangent-space normals for per-pixel sun N.L (owned by the texture cache).
    ID3D11ShaderResourceView* normal = nullptr;
    // The material's own cubetexture (Hesperus crystals), used instead of the level env cube.
    ID3D11ShaderResourceView* cube = nullptr;
    // Untextured additive mesh (turret blink lights, Mission_13b): out = 2 x V0, fog to black.
    // V0 = material colour x the vertex colour when the mesh has one.
    bool vcol_glow = false;
};

// material_water of a water model (MAWater tiles): colours and its textures.
struct WaterMaterial {
    bool ok = false;
    uint32_t deep = 0xFF657A64u, shallow = 0xFF618989u;  // ABGR
    float fresnel_min = 0.77f, fresnel_max = 1.f;
    ID3D11ShaderResourceView *wave = nullptr, *refl = nullptr;  // Texture3D, TextureCube
    std::string wave_path, refl_path;
};

struct GpuModel {
    enum State { Pending, Ready, Failed } state = Pending;
    std::vector<GpuMesh> meshes;
    float bmin[3] = {-1, 0, -1}, bmax[3] = {1, 2, 1};
    // Bounds of the drawn solid meshes (no glows/cones): the selection brackets. The file's bbox can be much wider
    // (M01b Manticore 'tank cinema 1': brackets well past the model on +x).
    bool has_vbox = false;
    float vmin[3] = {}, vmax[3] = {};
    // The opaque and cut-out triangles (no see-through sheets, glass or glows), model space, 9 floats each, and their
    // bounds: what can hold a prop up when placing on a roof. A box is not enough: the Over Falls lake rock's box
    // reaches 24 m over the water around the small tip that shows.
    bool has_sbox = false;
    float smin[3] = {}, smax[3] = {};
    std::vector<float> solid_tris;
    std::string error;
    // node_lights (lamps), model space; spot lights point along the node's +z.
    struct Light { float pos[3], dir[3]; ma2m::Node::Light l; };
    std::vector<Light> lights;
};

// A particle effect the model's .mtb attaches to one of its nodes (MAModSubObjectParticleEffect):
// the .mgparticle, where it sits in model space (node origin + the Offset property) and how it runs.
struct FxRef {
    std::string file;       // archive path of the .mgparticle
    float pos[3] = {0, 0, 0};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // node frame (model space, row-major, model = rot * node): the emitter's axes
    bool intermittent = false;  // AliveTime / DeadTime given: on for AliveTime ms, off for DeadTime ms (the means)
    bool retrigger = false;     // Behavior 1: one-way emitters start again every on + off
    float on_s = 0, off_s = 0;
    bool damage_state = false;  // HitEffectStateFlags set: shown only in a damage state, not at full health
    bool terrain_only = false;  // TerrainCondition set (water splashes): needs the unit on that terrain
    bool rain_only = false;
};

class ModelCache {
public:
    ~ModelCache();
    void init(ID3D11Device* dev, ID3D11DeviceContext* ctx) { dev_ = dev; ctx_ = ctx; }
    // (Re)opens the archive set: Base first, then common.mgf, then every other level archive.
    void set_archives(const fs::path& game_files, const std::string& base, const std::vector<std::string>& levels);
    const std::string& base() const { return base_; }
    ma2m::ArchiveSet* archives() { return archives_.get(); }
    ma2m::TextureCache* textures() { return textures_.get(); }

    // Model for a (Corporal3DObj, CorporalMTB) pair; queued when not loaded yet. crop: a cropped road piece or foundation
    // (crop.h format, "" = whole): its triangles are cut to that rectangle in model space, and its bounds with them.
    // standin (the view while a crop is dragged): a cut variant still queued shows the newest Ready cut of the same model instead of
    // an empty frame.
    GpuModel* get(const std::string& model, const std::string& mtb, const std::string& crop = std::string(), bool standin = false);
    // Cut variants of one model (every crop key it has been asked for). Frees those whose crop key is not in `keep` (at the end of a size drag).
    void drop_crop_variants(const std::string& model, const std::string& mtb, const std::set<std::string>& keep);
    int crop_variants(const std::string& model, const std::string& mtb) const;
    // Loads queued models for up to budget_ms. Returns true if anything finished loading.
    bool pump(double budget_ms);
    size_t pending() const { return queue_.size(); }
    // CPU thumbnail (RGBA8) as a texture for the palette; cached.
    ID3D11ShaderResourceView* thumbnail(const std::string& model, const std::string& mtb);
    // Already rendered (or tried): thumbnail() returns at once. The palette grid renders a few new ones per frame.
    bool thumbnail_cached(const std::string& model, const std::string& mtb) const {
        return model.empty() || thumbs_.count(ma2::lower(model) + "|" + ma2::lower(mtb)) != 0;
    }
    // Texture by archive path ("\Terrain\X.tif" or "Terrain/X.tif"); nullptr if missing. Cached.
    ID3D11ShaderResourceView* texture(const std::string& path) { return texture_srv(ma2::norm_path(path)); }
    ID3D11ShaderResourceView* cube(const std::string& path) { return texture_srv(ma2::norm_path(path), 2); }
    // Reads the model's material_water (cached; ok = false for other models).
    const WaterMaterial& water_material(const std::string& model);
    // Particle effects the model's .mtb carries; nullptr until the model has been loaded (get() queues it).
    const std::vector<FxRef>* effects(const std::string& model, const std::string& mtb);
    // A parsed .mgparticle by archive path (cached; nullptr when missing or unparsable).
    const ma2::ParticleFx* particle(const std::string& path);
    // The game's rain particle for a rain level: the snow particle at `path` turned into rain by the same
    // weather::rain_particle_xml the save uses, so the view and the game draw the same file (cached per level).
    const ma2::ParticleFx* rain_particle(const std::string& path, int level);
    const ma2::ParticleFx* dust_particle(const std::string& path, bool haze);  // Weather::dust_particle_xml
    void clear();
    // Releases what clear() retired (call after Present).
    void end_frame();
    // Drops every GPU object and the archives (device lost); the next set_archives reloads.
    void reset_device(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
        clear();
        end_frame();
        archives_.reset();
        textures_.reset();
        base_.clear();
        init(dev, ctx);
    }

private:
    std::vector<IUnknown*> retired_;
    std::vector<std::unique_ptr<GpuModel>> retired_models_;
    // kind 0 = 2D (frame 0), 1 = volume (Texture3D), 2 = cube map (6 frames).
    ID3D11ShaderResourceView* texture_srv(const std::string& path, int kind = 0);
    void load(const std::string& key, GpuModel& m);
    void collect_effects(const std::string& key, const ma2m::Model& model);
    std::map<std::string, AlphaRamp> collect_blends(const ma2m::Model& model);  // lower-case SubObjectName -> ramp
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    std::unique_ptr<ma2m::ArchiveSet> archives_;
    std::unique_ptr<ma2m::TextureCache> textures_;
    std::string base_;
    std::map<std::string, std::unique_ptr<GpuModel>> models_;
    std::deque<std::string> queue_;
    std::map<std::string, std::string> last_ready_;  // model|mtb -> key of its newest Ready cut variant (the drag's stand-in)
    std::map<std::string, ID3D11ShaderResourceView*> srvs_;
    std::map<std::string, ID3D11ShaderResourceView*> thumbs_;
    std::map<std::string, WaterMaterial> water_;
    std::map<std::string, std::vector<FxRef>> fx_;
    std::map<std::string, std::unique_ptr<ma2::ParticleFx>> particles_;
};

}  // namespace ed
