// Direct3D 11 renderer for the editor viewport: terrain, object boxes, lines, water planes.
#pragma once
#include <d3d11.h>
#include <DirectXMath.h>

#include <map>
#include <string>
#include <vector>

#include "../core/ground_layers.h"
#include "../core/mgic.h"
#include "../core/terrain.h"

namespace ed {

using namespace DirectX;

struct Camera {
    float tx = 512, tz = 512;     // target on the ground
    float ty = 0;
    float yaw = 0.0f;             // radians, 0 = looking toward +z
    float pitch = 0.95f;          // radians above the horizon
    float dist = 600.f;
    float fov = 0.9f;
    XMFLOAT4X4 view{}, proj{}, viewproj{};
    XMFLOAT3 eye{};
    void update(float aspect);
    // Ray through a viewport pixel (0..w, 0..h).
    void ray(float px, float py, float w, float h, XMFLOAT3& o, XMFLOAT3& d) const;
    bool project(const XMFLOAT3& p, float w, float h, float& sx, float& sy) const;
};

struct BoxInstance {
    XMFLOAT4 r0, r1, r2;  // 3x4 world matrix rows (row-vector convention, translation in .w)
    XMFLOAT4 color;
};

struct LineVertex { XMFLOAT3 p; uint32_t c; };
struct GpuModel;
struct GpuMesh;

class Renderer {
public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string* err);
    void shutdown();

    // Terrain geometry and textures.
    void set_terrain(const ma2::Terrain& t);
    void update_terrain_rect(const ma2::Terrain& t, int x0, int z0, int x1, int z1);
    void set_colormap(const ma2::Image& img);
    // P4 ground layers: null or disabled = the retail pair; set_ground_weights after painting.
    void set_ground_layers(const ma2::GroundLayers* g);
    void set_ground_weights(const ma2::GroundLayers& g);
    void set_lightmap(const ma2::Image& img);
    void set_overlay(const ma2::Image& img);  // RGBA overlay drawn over the terrain (type map view)
    void clear_overlay();
    ID3D11ShaderResourceView* colormap_srv() const { return colormap_srv_; }
    ID3D11ShaderResourceView* minimap_srv() const { return minimap_srv_; }
    void set_minimap(const ma2::Image& img);

    // Per-frame lists.
    struct ModelBatch { const GpuModel* model; std::vector<BoxInstance> inst; bool receiver = false; uint32_t tint = 0; /* entity colour set (ARGB, 0 = none): replaces the COMPLEX colorshift1 tint constant */ };
    std::vector<ModelBatch> models;
    std::vector<BoxInstance> boxes;
    std::vector<LineVertex> lines;
    // MAEffect (fires, smoke): a camera-facing additive flame card at each position (gap 6).
    std::vector<XMFLOAT3> flames;
    // Particle sprites (View > Effects): world-space camera-facing quads built by the app from the
    // game's .mgparticle files; the renderer faces them at the camera, sorts and draws them.
    struct Sprite {
        XMFLOAT3 p, vel{0, 0, 0};
        float w = 1, h = 1, angle = 0, pivot_x = 0, pivot_y = 0;
        bool orient = false;
        float rgba[4] = {1, 1, 1, 1};
        ID3D11ShaderResourceView* tex = nullptr;  // owned by the model cache
        int blend = 0;                            // 0 alpha, 1 additive, 2 subtractive
    };
    std::vector<Sprite> sprites;
    // Water: deep/shallow colour + the model's material_water textures when known: wave = volume
    // normal map (Texture3D), refl = sky cube map (TextureCube).
    struct Quad {
        XMFLOAT3 c; float hx, hz; uint32_t color, shallow = 0xFF618989u;
        ID3D11ShaderResourceView *wave = nullptr, *refl = nullptr;  // owned by the model cache
        float fresnel_min = 0.77f, fresnel_max = 1.f;
        bool selected = false;
        float heading = 0;   // CorporalHeading in radians: turns the plane about its centre (outline code in app.cpp: x' = cos x + sin z)
        bool oval = false;   // an ellipse inscribed in the plane (a cropped water)
    };
    std::vector<Quad> quads;
    // MATerrain detail layers (owned by the model cache): the colour map's alpha is the weight of
    // detail 0 (255) against detail 1 (0); repeats are across the whole map (TerrainDetailMult/Mult1, MicroDetailTiling0/1).
    struct Detail {
        ID3D11ShaderResourceView* tex[4] = {};  // detail 0, detail 1, micro 0, micro 1
        float rep[4] = {64, 64, 240, 240};
        float blend = 0.3f;                     // TerrainDetailBlend: detail strength (see tps)
        bool modulate = true;                   // TerrainDetailModulate: colour x detail x 2
    } detail;
    // The world's sky (MASky2), drawn first around the camera as the background.
    // Scene lights (View > Scene lights): lamp node_lights and MAPointLights, world space. Each
    // lights the ground through a light map (a pool whose radius follows its height above the
    // ground, like the game's per-frame 512x512 map) and nearby models directly.
    struct SceneLight {
        XMFLOAT3 pos{0, 0, 0}, color{1, 1, 1}, dir{0, -1, 0};
        float inner = 0, outer = 0;               // reach on models, metres
        bool spot = false;
        float cos_in = 1, cos_out = 0;            // spot cone (angles are half angles, inferred)
        float min_h = 0, max_h = 0, min_r = 0, max_r = 0;  // ground pool (<lightmap>); max_r 0 = none
        XMFLOAT3 pool_min{0.5f, 0.5f, 0.5f}, pool_max{0.5f, 0.5f, 0.5f};  // ground pool colour (<lightmap> min/max_color), lerped like the radius
    };
    std::vector<SceneLight> lights;
    // The level's env cube (MAEnvironment EnvironmentMapName; owned by the model cache): glass and
    // water without their own reflection texture.
    ID3D11ShaderResourceView* env_cube = nullptr;
    struct Sky { const GpuModel* model = nullptr; XMFLOAT3 scale{1, 1, 1}, offset{0, 0, 0}; float heading = 0; bool fog_on = false, hide_additive = false; XMFLOAT3 fog_color{0, 0, 0}; float fog_top = 0, fog_bottom = 0; } sky;

    struct Params {
        XMFLOAT3 brush{0, 0, 0};
        float brush_radius = 0;  // < 0: square brush of that half-size
        float brush_contour = 0;  // > 0: height contour lines this many metres apart inside the brush (shaping tools)
        float brush_tint = 0;     // ring colour: 0 yellow, 1 cyan (Raise / Plateau up / Smooth), 2 orange (Lower / Plateau down / Roughen)
        bool grid = true;
        bool overlay = false;
        bool lighting = true;
        bool detail = true;   // View > Ground textures
        float time = 0;       // seconds, animates water
        // MAEnvironment (View > Game lighting / Fog): ambient and sun colours (rgb 0-1, as stored;
        // the game's lighting doubles them), fog colour and linear start/end in metres.
        bool env_light = true, fog = true;
        bool scene_lights = false;  // View > Scene lights
        bool night_time = true;     // MAEnvironment IsNightTime: building 'lights' groups (lit windows, point lights, cones) show only then
        bool bloom = true;          // View > Bloom: the game's glow post-process (framebuffer-alpha mask, blurred at 1/8 size, added back)
        bool fast_terrain_detail = true;  // Preferences: ground detail fades like the game (128 m block switch)
        int wave_up = 0;            // water normal map: 0 blue up, 1 green up, 2 swizzled (developer flag --wave-up)
        XMFLOAT3 ambient{0.5f, 0.5f, 0.5f}, sun{0.5f, 0.5f, 0.5f}, fog_color{0.5f, 0.5f, 0.5f};
        float fog_start = 0, fog_end = 0;
        bool fog_height = false;  // FogMode 2: distance fog plus the FogBottom..FogTop height band
        float fog_top = 0, fog_bottom = 0;
        XMFLOAT3 sun_dir{0, 0, 0};  // DirectionalLightDirection (from the sun); 0 = editor light
        float terrain_size_x = 1024, terrain_size_z = 1024;
    } params;

    void render(const Camera& cam, D3D11_VIEWPORT vp);
    // A small picture of a water material (palette hover): the water shader on a patch seen from above at an angle, deep
    // colour, wave normals and sky reflection. Drawn at the start of the next render(); null until then.
    struct WaterLook {
        ID3D11ShaderResourceView *wave = nullptr, *refl = nullptr;
        uint32_t deep = 0, shallow = 0;
        float fresnel_min = 0.77f, fresnel_max = 1.f;
        std::string key;  // the water model's path: one picture per model
    };
    ID3D11ShaderResourceView* water_preview(const WaterLook& w);
    // A small picture of a sky (Environment tab lists, retail-reach #41): the dome drawn with the sky shader from the
    // ground, looking a little above the horizon (128 x 64, no fog). Drawn at the start of the next render(); null until then.
    struct SkyLook {
        const GpuModel* model = nullptr;
        XMFLOAT3 scale{400, 400, 400}, offset{0, 0, 0};
        float heading = 0;
        std::string key;  // the sky's name: one picture per sky
    };
    ID3D11ShaderResourceView* sky_preview(const SkyLook& s);
    // Releases textures replaced during the frame (call after Present).
    void end_frame();

private:
    bool make_texture(const ma2::Image& img, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv);
    void retire(IUnknown* p);
    std::vector<IUnknown*> retired_;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    void draw_models(int pass);  // 0 opaque, 1 transparent, 2 ground decals (modulate 2x)
    void draw_sky(const Camera& cam);
    ID3D11Buffer* sky_inst_vb_ = nullptr;
    ID3D11VertexShader* sprite_vs_ = nullptr;
    ID3D11PixelShader* sprite_ps_ = nullptr;
    ID3D11InputLayout* sprite_il_ = nullptr;
    ID3D11Buffer *sprite_vb_ = nullptr, *sprite_cb_ = nullptr;
    UINT sprite_cap_ = 0;
    void draw_sprites(const Camera& cam);
    ID3D11VertexShader* water_vs_ = nullptr;
    ID3D11PixelShader* water_ps_ = nullptr;
    ID3D11VertexShader *terrain_vs_ = nullptr, *box_vs_ = nullptr, *line_vs_ = nullptr, *model_vs_ = nullptr;
    ID3D11PixelShader *terrain_ps_ = nullptr, *box_ps_ = nullptr, *line_ps_ = nullptr, *model_ps_ = nullptr;
    ID3D11InputLayout *terrain_il_ = nullptr, *box_il_ = nullptr, *line_il_ = nullptr, *model_il_ = nullptr;
    ID3D11Buffer *cb_ = nullptr, *mesh_cb_ = nullptr, *model_inst_vb_ = nullptr;
    UINT model_inst_cap_ = 0;
    std::vector<UINT> model_inst_start_;
    ID3D11SamplerState* wrap_sampler_ = nullptr;
    std::map<int, ID3D11SamplerState*> tex_samplers_;  // material <basetexture> sampling (tex_sampler)
    ID3D11SamplerState* mirror_samplers_[3] = {};  // mirror U, mirror V, both
    ID3D11BlendState *bs_add_ = nullptr, *bs_sub_ = nullptr;
    ID3D11Buffer *terrain_vb_ = nullptr, *terrain_ib_ = nullptr;
    UINT terrain_indices_ = 0;
    int tw_ = 0, th_ = 0;
    float outside_y_ = 0;  // ground outside the map: a plane 2 m under the lowest terrain point
    ID3D11VertexShader* outside_vs_ = nullptr;
    ID3D11PixelShader* outside_ps_ = nullptr;
    ID3D11Buffer* outside_vb_ = nullptr;
    ID3D11Buffer *cube_vb_ = nullptr, *cube_ib_ = nullptr, *inst_vb_ = nullptr;
    UINT inst_cap_ = 0;
    ID3D11Buffer *line_vb_ = nullptr;
    UINT line_cap_ = 0;
    ID3D11Texture2D *colormap_ = nullptr, *lightmap_ = nullptr, *overlay_ = nullptr, *minimap_ = nullptr;
    ID3D11ShaderResourceView *colormap_srv_ = nullptr, *lightmap_srv_ = nullptr, *overlay_srv_ = nullptr, *minimap_srv_ = nullptr;
    ID3D11Texture2D *gw_tex_[2] = {}, *gl_tex_ = nullptr;   // P4 ground layers: weights 0-3 / 4-7, layers 2.. (array)
    ID3D11ShaderResourceView *gw_srv_[2] = {}, *gl_srv_ = nullptr;
    float ground_rep_[8] = {};
    int ground_count_ = 0;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11RasterizerState *rs_solid_ = nullptr, *rs_nocull_ = nullptr;
    ID3D11DepthStencilState *ds_on_ = nullptr, *ds_nowrite_ = nullptr, *ds_off_ = nullptr;
    ID3D11BlendState *bs_alpha_ = nullptr, *bs_water_ = nullptr, *bs_mod2x_ = nullptr;
    // Terrain heights (R32F, one texel per grid point) for the water's depth.
    ID3D11Texture2D* height_ = nullptr;
    ID3D11ShaderResourceView* height_srv_ = nullptr;
    // Water refraction: a copy of the frame taken just before the water planes draw.
    ID3D11Texture2D* scene_copy_ = nullptr;
    ID3D11ShaderResourceView* scene_copy_srv_ = nullptr;
    bool copy_scene();
    ID3D11SamplerState* tex_sampler(const GpuMesh& m);
    // Bloom (game: draws write a mask to the framebuffer alpha; the frame is boxed down twice (1/2, 1/4), blurred 4 passes, max-merged, added).
    // Here the mask is its own R8 target written by the model pass (MRT) and the post chain runs at the end of render().
    struct Rt { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr; UINT w = 0, h = 0; };
    Rt mask_, bl0_, bl1_, bl2_;
    ID3D11VertexShader* bloom_vs_ = nullptr;
    ID3D11PixelShader *bloom_ps_down_ = nullptr, *bloom_ps_box_ = nullptr, *bloom_ps_blur_ = nullptr, *bloom_ps_copy_ = nullptr;
    ID3D11Buffer* bloom_cb_ = nullptr;
    ID3D11BlendState *bs_bmax_ = nullptr, *bs_badd_ = nullptr;
    ID3D11SamplerState* bloom_samp_ = nullptr;
    bool bloom_drawn_ = false;  // a model with a mask drew this frame
    bool make_rt(Rt& r, UINT w, UINT h, DXGI_FORMAT f);
    void free_rt(Rt& r);
    bool prepare_mask();  // size + clear the mask to the bound render target; false when bloom is off or unavailable
    void run_bloom(D3D11_VIEWPORT vp);
    struct WaterPic { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; };
    std::map<std::string, WaterPic> water_pics_;
    std::vector<WaterLook> water_pending_;
    std::map<std::string, WaterPic> sky_pics_;
    std::vector<SkyLook> sky_pending_;
    void build_sky_previews();
    ID3D11VertexShader* wprev_vs_ = nullptr;
    ID3D11PixelShader* wprev_ps_ = nullptr;
    ID3D11Buffer* wprev_cb_ = nullptr;
    void build_water_previews();
    void upload_heights();
    float height_at(float x, float z) const;
    void build_light_map();
    ID3D11Texture2D* lightpool_ = nullptr;
    ID3D11ShaderResourceView* lightpool_srv_ = nullptr;
    ID3D11Buffer* lights_cb_ = nullptr;
    uint64_t lights_hash_ = 0;
    struct TerrainVertex { XMFLOAT3 p; XMFLOAT3 n; XMFLOAT2 uv; };
    std::vector<TerrainVertex> tverts_;
    void build_vertex(const ma2::Terrain& t, int gx, int gz, TerrainVertex& v);
};

}  // namespace ed
