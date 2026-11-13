// ma2models.h - CPU-side decoder for MechAssault 2 models and textures.
//
// Reads the retail MGF archives at run time and turns models (.MGMODEL XML,
// legacy .NODE/.mesh/.geomVert/.geomFace/.mat text sets) and MGIc pictures
// (.tif) into static, renderable meshes and RGBA8 images. No retail bytes are
// embedded; everything is decoded from the user's own game_files.
//
// Conventions (see API.md for the long form):
//  * Paths: archive paths such as "Models/Mechs/Assault/Atlas/atlas_low.NODE".
//    Lookups are case-insensitive, accept '\' or '/', and ignore a leading
//    slash, so template values like "\Models\Avatars\Pilot.MGMODEL" work as is.
//  * Space: game space, Y up, metres. Positions are kept in each sub-mesh's
//    node space; SubMesh::transform maps them to model space.
//  * Matrices: float[16], translation in [12],[13],[14]. A point transforms as
//    x' = x*m[0] + y*m[4] + z*m[8] + m[12] (row-vector / D3D layout, which is
//    the same memory layout as a column-major OpenGL matrix).
//  * Triangles: index lists, three per triangle. cross(p1-p0, p2-p0) points to
//    the front face (along the authored vertex normals).
//  * UVs: D3D convention, (0,0) = top-left texel row of the decoded image.
//  * Images: RGBA8, rows top to bottom, no padding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ma2m {

// ---------------------------------------------------------------------------
// Archives
// ---------------------------------------------------------------------------

// A prioritized set of MGF archives. Archives added first win when a path
// exists in several. Thread-safe for concurrent reads after all Add() calls.
class ArchiveSet {
public:
    ArchiveSet();
    ~ArchiveSet();
    ArchiveSet(const ArchiveSet&) = delete;
    ArchiveSet& operator=(const ArchiveSet&) = delete;

    // Opens an MGF v4 archive (compressed or plain). The file stays open.
    bool Add(const std::string& path_utf8, std::string* error = nullptr);
    size_t Count() const;
    std::string ArchivePath(size_t index) const;   // as passed to Add
    std::string ArchiveName(size_t index) const;   // file name, e.g. "boggeddown.mgf"
    size_t FileCount(size_t index) const;

    // -1 when no archive holds the path.
    int Find(const std::string& path) const;
    bool ExistsIn(size_t archive_index, const std::string& path) const;
    bool Exists(const std::string& path) const { return Find(path) >= 0; }
    // Path exactly as spelled inside the archive that holds it ("" if missing).
    std::string CanonicalPath(const std::string& path) const;
    uint32_t FileSize(const std::string& path) const;

    // Reads a whole file; archive_index (optional) receives the source archive.
    bool Read(const std::string& path, std::vector<uint8_t>& out, int* archive_index = nullptr,
              std::string* error = nullptr) const;
    // Reads from one specific archive only.
    bool ReadFrom(size_t archive_index, const std::string& path, std::vector<uint8_t>& out,
                  std::string* error = nullptr) const;

    // All file paths of one archive, as spelled in it.
    std::vector<std::string> List(size_t archive_index) const;
    // Union over all archives (first spelling wins), filtered by a
    // case-insensitive substring.
    std::vector<std::string> ListAll(const std::string& substring = std::string()) const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// Joins and normalizes archive paths: '/' separators, "." and ".." collapsed,
// no leading slash. Case is preserved.
std::string NormalizePath(const std::string& path);
// Resolves a path written relative to the folder of base_file
// (e.g. "..\..\Texture_Dump\x.tif" from a model). Paths starting with '\'
// or '/' are archive-absolute.
std::string ResolveRelative(const std::string& base_file, const std::string& relative);
// Lower-case extension without the dot ("mgmodel", "node", "tif").
std::string Extension(const std::string& path);

// ---------------------------------------------------------------------------
// Textures (.tif = MGIc picture container)
// ---------------------------------------------------------------------------

enum class PixelFormat {
    Unknown,
    DXT1, DXT3, DXT5,
    A8R8G8B8,   // format code 0 (X8R8G8B8 data is the same code; check Texture::has_alpha)
    R5G6B5,     // code 1
    A4R4G4B4,   // code 3
    A8,         // code 5: alpha only, decoded as white RGB + alpha
    A8L8,       // code 7: luminance + alpha
    L8,         // code 4: 8-bit luminance (MATerrain "TerrainHeight Field" heights), RGB = value, A = 255
};
const char* PixelFormatName(PixelFormat f);

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
};

struct Texture {
    int width = 0, height = 0;
    int depth = 1;               // volume slices
    int frames = 1;              // animation frames, or 6 cube faces
    int mip_count = 1;           // levels stored in the file
    uint32_t flags = 0;          // raw 'flgs' word
    PixelFormat format = PixelFormat::Unknown;
    bool swizzled = false, cube = false, volume = false;
    bool has_alpha = false;      // any decoded alpha < 255 (level 0, layer 0)
    bool alpha_is_binary = false;// every alpha is 0 or 255 (cutout-friendly)
    // layers[frame * depth + slice][mip]. Mips beyond the first are present
    // only when DecodeTexture was asked for them.
    std::vector<std::vector<Image>> layers;

    const Image* Top() const { return layers.empty() || layers[0].empty() ? nullptr : &layers[0][0]; }
};

struct TextureInfo {             // header only, no pixel decode
    int width = 0, height = 0, depth = 1, frames = 1, mip_count = 1;
    uint32_t flags = 0;
    PixelFormat format = PixelFormat::Unknown;
    bool swizzled = false, cube = false, volume = false;
    uint32_t payload_bytes = 0;
};

bool ReadTextureInfo(const uint8_t* data, size_t size, TextureInfo& out, std::string* error = nullptr);
// all_mips=false decodes only level 0 of every layer (faster; enough for previews).
bool DecodeTexture(const uint8_t* data, size_t size, Texture& out, bool all_mips = true,
                   std::string* error = nullptr);
bool LoadTexture(const ArchiveSet& archives, const std::string& path, Texture& out,
                 bool all_mips = true, std::string* error = nullptr);

// Optional thread-safe cache of decoded textures keyed by normalized path.
class TextureCache {
public:
    explicit TextureCache(const ArchiveSet& archives, bool all_mips = false);
    ~TextureCache();
    // nullptr when the file is missing or fails to decode (error kept per path).
    std::shared_ptr<const Texture> Get(const std::string& path, std::string* error = nullptr);
    void Clear();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

enum class AlphaMode {
    Opaque,      // blending="normal", no usable alpha
    Cutout,      // blending="normal" + diffuse alpha used as a mask (foliage, fences); test at 0.5
    Blend,       // blending="alpha": src*a + dst*(1-a), no depth write
    Additive,    // blending="additive": dst + src (glows, lights, cracks)
    Subtractive, // blending="subtractive": dst - src (shadow decals)
};
const char* AlphaModeName(AlphaMode m);

struct TextureSlot {
    std::string slot;            // "basetexture", "detailtexture", "multitexture", "cubetexture", ...
    std::string path;            // resolved archive path (as written, normalized)
    bool found = false;          // exists in the ArchiveSet
    std::string tiling;          // "U_WRAP | V_WRAP", "U_MIRROR | V_WRAP", ...
    // Animated textures: the material names "X.tif" but the archive holds
    // frames "X 01.tif", "X 02.tif", ...; path is then frame 01 and frames
    // lists every frame (empty for ordinary textures).
    std::vector<std::string> frames;
};

struct Material {
    std::string name;
    std::string type;            // "material_base", "material_complex", ... (legacy: "BASE", ...)
    std::string diffuse_texture; // resolved path of the base texture ("" = untextured)
    bool diffuse_found = false;
    std::vector<TextureSlot> textures;
    float diffuse[3] = {1, 1, 1};    // 0..1, multiplies the texture
    float selfillum[3] = {0, 0, 0};  // 0..1
    float opacity = 1.0f;
    bool two_sided = false;
    AlphaMode alpha = AlphaMode::Opaque;
    std::string blending;        // raw attribute: normal / alpha / additive / subtractive
    std::string shading;         // gouraud / material_only / translucent / flat
    bool mirror_u = false, mirror_v = false; // from tiling
    float colorshift[2][3] = {};     // COMPLEX: glow colours of the multi mask's G and B (0..1)
    float reflectivity = 0;          // material_env: env cube weight (0..1)
    bool colorshift1_diffuse = false; // COMPLEX: colorshift1 tints the light (mask.b) instead of glowing
    float colorshift_bloom[2] = {-1, -1};  // colorshift0/1_bloom: the bloom mask weight the game writes to the framebuffer alpha (-1 = attribute absent)
    float specular_bloom = -1;        // specular_bloom (same, for the specular term)
    float spec_color[3] = {0, 0, 0};  // spec_color (0..1)
    float shin_strength = 1;          // COMPLEX specular constant = spec_color x shin_strength
    // material_detail: blend = "modulate" | "alphablend" and blend_level; material_complex:
    // detail_blend = "modulate2x" and detail_blend_level (0..1, -1 = not given).
    std::string detail_blend;
    float detail_level = -1;
    // Legacy .mat "type = WATER" (.NODE water models): colours and fresnel like <material_water>.
    bool water = false;
    float deep[3] = {0, 0, 0}, shallow[3] = {0, 0, 0};
    float fresnel_min = 0.77f, fresnel_max = 1.0f;
    // material_distort: a screen-space refraction effect (ECM / stealth
    // shimmer) with no colour of its own. Skip it in a plain renderer.
    bool distortion = false;
    // material_env envmaptype="masked" (Hesperus oily water layers): rgb = lerp(base, cube, reflectivity),
    // alpha = opacity x base.a (Ill Wind trace, editor fidelity 2026-10-23).
    bool env_masked = false;
};

struct Node {
    std::string name;
    std::string type;            // node_generic, node_3dobject, node_skinned, node_switch_lod,
                                 // node_light, node_facing, node_anim (legacy: NODE, 3DOBJECT, ...)
    int parent = -1;
    float local[16];             // node -> parent
    float world[16];             // node -> model
    bool turned_on = true;
    bool hidden_by_mtb = false;  // a damage-state rule in the .mtb hides it at full health
    int lod = 0;                 // 0 = top LOD; >0 under node_switch_lod alternatives
    // node_light only (lamps): light_* attributes and the <lightmap> child (the ground light pool,
    // radius lerped from min to max by the light's height above the ground).
    struct Light {
        bool spot = false;
        float color[3] = {1, 1, 1};          // 0..1
        float inner_radius = 0, outer_radius = 0, inner_angle = 0, outer_angle = 0;  // metres, degrees
        int priority = 0;
        bool has_lightmap = false;
        float min_height = 0, max_height = 0, min_radius = 0, max_radius = 0;
        float min_color[3] = {1, 1, 1}, max_color[3] = {1, 1, 1};
    };
    bool is_light = false;
    Light light;
};

struct SubMesh {
    std::string name;            // meshxml name ("trunk mesh") or .mesh path
    std::string node;            // owning node name
    int node_index = -1;
    int material = -1;           // index into Model::materials (-1 = none)
    float transform[16];         // node space -> model space (== nodes[node_index].world)
    // Vertex streams (count = positions.size()/3). Missing streams are empty.
    std::vector<float> positions;   // xyz, node space
    std::vector<float> normals;     // xyz unit
    std::vector<float> uv0, uv1;    // uv pairs
    std::vector<uint32_t> colors;   // RGBA8 (r in low byte), from D3DCOLOR vertex data
    std::vector<uint32_t> indices;  // triangle list
    uint32_t vertex_flags = 0;   // raw layout flags (see NOTES.md)
    bool skinned = false;        // vertices carry bone weights; drawn here in bind pose
    bool shadow = false;         // shadow-volume mesh (<shadow_mesh>), excluded by default
    bool visible = true;         // false: node turned_on="false", hidden by an .mtb rule, LOD > 0, or shadow
    float tint[3] = {1, 1, 1};   // colour multiplier from the .mtb (heat-sink glow at rest), 0..1
    float bbox_min[3], bbox_max[3];  // model space
};

struct ModelLoadOptions {
    bool include_shadow_meshes = false;  // load <shadow_mesh> volumes
    bool include_invisible = false;      // keep sub-meshes with visible=false in the output
    bool all_lods = false;               // keep every node_switch_lod child (else only LOD 0)
    // .mtb to read for damage/defence-state visibility: pass the template's
    // CorporalMTB. "" = guess <folder>/<stem>.mtb for the model file and its
    // geometry file, also without a "_low" suffix and one folder up (for
    // model_low/x.mgmodel). Set apply_mtb=false to skip it.
    std::string mtb_path;
    bool apply_mtb = true;
    // Optional: decoded textures are needed to tell Opaque from Cutout
    // materials; pass the cache you render with to avoid decoding twice.
    TextureCache* texture_cache = nullptr;
};

struct Model {
    std::string requested_path;  // as passed to LoadModel
    std::string path;            // canonical path of the loaded file (after ResolveModelPath)
    std::string archive;         // archive name it came from
    std::string geometry_path;   // the .mgmodel / .node that holds geometry (after ANIMNODE redirect)
    std::string mtb_path;        // the .mtb actually applied ("" = none)
    std::string skeleton;        // .gr2 skeleton named by an ANIMNODE / node_anim ("" = none)
    std::vector<std::string> animations;  // .gr2 files named by the ANIMNODE
    std::vector<Node> nodes;
    std::vector<Material> materials;
    std::vector<SubMesh> meshes;
    float bbox_min[3], bbox_max[3];       // over visible meshes, model space
    bool has_bbox = false;
    std::vector<std::string> warnings;    // non-fatal problems (missing textures, unknown layouts...)
    int failed_meshes = 0;                // meshes skipped because their data was missing or undecodable

    size_t VisibleTriangleCount() const;
};

// The file the game loads for a template's model path, as a canonical archive
// path: the path itself if present, else "<stem>_low.<ext>" (MP levels ship
// only the low-detail 'Mechs and vehicles that templates name without _low),
// else the same stem with the other extension (.NODE <-> .MGMODEL), with and
// without _low. "" when nothing matches. LoadModel and CollectDependencies
// apply this automatically (Model::requested_path keeps the original).
std::string ResolveModelPath(const ArchiveSet& archives, const std::string& path);

bool LoadModel(const ArchiveSet& archives, const std::string& path, Model& out,
               const ModelLoadOptions& options = ModelLoadOptions(), std::string* error = nullptr);

// Positions/normals of one sub-mesh transformed into model space.
void BakeSubMesh(const SubMesh& mesh, std::vector<float>& positions, std::vector<float>& normals);

// Writes visible meshes as Wavefront OBJ (+ .mtl next to it). Texture file
// names in the .mtl are "<texture_dir_rel>/<slug>.png"; the caller writes the
// PNGs (modeldump does). OBJ is right-handed, so Z is negated on export.
bool WriteObj(const Model& model, const std::string& obj_path, const std::string& texture_dir_rel,
              std::string* error = nullptr);
// File-name-safe slug used by WriteObj for a texture path.
std::string TextureSlug(const std::string& texture_path);

// ---------------------------------------------------------------------------
// Dependency closure (SPEC [Assets])
// ---------------------------------------------------------------------------

struct Dependency {
    std::string path;            // canonical archive path
    std::string archive;         // archive name that holds it ("" when missing)
    std::string kind;            // model, node, mesh, verts, indices, geomvert, geomface,
                                 // material, mtb, texture, skeleton, animation, particle, other
    std::string required_by;     // the file that references it
    bool found = false;
};

// Every archive file the game reads to show this model: the model file, its
// "{... verts}"/"{... indices}" entries, legacy .mesh/.geomVert/.geomFace/.mat,
// textures, ANIMNODE skeleton/animations (.gr2), the .mtb (mtb_path, or the
// guessed one if empty) and the .mgparticle effects the .mtb names, recursively
// (particle models and textures too). Sorted, unique.
bool CollectDependencies(const ArchiveSet& archives, const std::string& model_path,
                         const std::string& mtb_path, std::vector<Dependency>& out,
                         std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Templates (world files: GlobalEntities.wdf, CulledTemplates/*.wdf, maps)
// ---------------------------------------------------------------------------

struct TemplateModel {
    uint32_t uid = 0;            // the template object's UID (what world objects' TemplateID use)
    uint32_t template_id = 0;    // TemplateID property of the object itself (0 for root templates)
    std::string class_name;      // e.g. "MABuilding" ("" = unknown class key)
    uint32_t class_key = 0;
    std::string name;            // Description / Name property
    std::string model;           // Corporal3DObj (normalized, no leading slash)
    std::string mtb;             // CorporalMTB
    // Other model/texture-valued properties: AbandonedModel, TransitionModel,
    // CamouflageTexture, HUDIconImage, MechFootprintDecal, ...
    std::vector<std::pair<std::string, std::string>> extra_files;
    // Other templates this one swaps to or spawns (by UID): DestroyedBuilding,
    // CorporalDestroyed, DestroyedEffect, HitEffect, HoleEdgeEffect, HoleTopEffect.
    std::vector<std::pair<std::string, uint32_t>> links;
};

// Scans a .wdf for objects that carry Corporal3DObj (and the other file
// properties above). Keys are CRC32-without-final-complement of the names.
bool ReadTemplateModels(const uint8_t* wdf, size_t size, std::vector<TemplateModel>& out,
                        std::string* error = nullptr);
bool ReadTemplateModels(const ArchiveSet& archives, const std::string& wdf_path,
                        std::vector<TemplateModel>& out, std::string* error = nullptr);
// Closure for one template: CollectDependencies(model, mtb) plus the
// AbandonedModel/AbandonedMTB and TransitionModel/TransitionMTB pairs and the
// other file properties (camouflage/HUD textures, decal .mat files and their
// textures). Run it over every template in a scenario's templates.wdf and
// union the results to fill SPEC [Assets]; linked templates (DestroyedBuilding,
// CorporalDestroyed, effects, weapons) are separate templates in that file.
bool CollectTemplateDependencies(const ArchiveSet& archives, const TemplateModel& t, std::vector<Dependency>& out,
                                 std::string* error = nullptr);

// crc32(name) ^ 0xFFFFFFFF, the WDF key of a class or property name.
uint32_t WdfKey(const char* name);

// ---------------------------------------------------------------------------
// Utilities: thumbnails and PNG
// ---------------------------------------------------------------------------

struct ThumbnailOptions {
    int width = 128, height = 128;
    float yaw_degrees = 35.0f;   // camera orbit around +Y; 0 = camera on +Z (the model's front) looking toward -Z
    float pitch_degrees = 25.0f; // camera elevation above the horizon
    uint32_t background = 0x00000000; // RGBA8 (r low byte); alpha 0 = transparent
    bool textured = true;
    int supersample = 2;         // render at N x size and box-filter down
};

// Small software rasterizer (perspective, z-buffer, textured, cutout/additive/
// blend approximations, simple headlight shading). Uses the cache if given.
bool RenderThumbnail(const Model& model, TextureCache* textures, Image& out,
                     const ThumbnailOptions& options = ThumbnailOptions(), std::string* error = nullptr);

bool WritePng(const std::string& path, const Image& image, std::string* error = nullptr);

}  // namespace ma2m
