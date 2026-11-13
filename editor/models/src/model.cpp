// Model assembly: .MGMODEL (XML), legacy .NODE/.mesh/.mat sets, ANIMNODE
// redirects, and .mtb damage-state visibility.
#include "model_loader.h"

#include <cmath>
#include <set>

namespace ma2m {

const char* AlphaModeName(AlphaMode m) {
    switch (m) {
        case AlphaMode::Opaque: return "opaque";
        case AlphaMode::Cutout: return "cutout";
        case AlphaMode::Blend: return "blend";
        case AlphaMode::Additive: return "additive";
        case AlphaMode::Subtractive: return "subtractive";
    }
    return "?";
}

size_t Model::VisibleTriangleCount() const {
    size_t n = 0;
    for (const auto& m : meshes)
        if (m.visible) n += m.indices.size() / 3;
    return n;
}

namespace detail {

namespace {

void ParseColor(const std::string& s, float out[3]) {
    float v[3];
    if (ParseFloats(s, v, 3) == 3)
        for (int i = 0; i < 3; i++) out[i] = std::min(1.0f, std::max(0.0f, v[i] / 255.0f));
}

bool IsCutoutCapable(const Material& m) {
    std::string t = Lower(m.type);
    return t == "material_base" || t == "material_detail" || t == "material_animated" || t == "base" ||
           t == "detail" || t == "animated" || m.shading == "translucent";
}

const char* kTextureSlots[] = {"basetexture", "detailtexture", "multitexture", "cubetexture", "wavetexture",
                               "distortiontexture", "normal_map", "reflectiontexture"};

}  // namespace

Loader::Loader(const ArchiveSet& a, const ModelLoadOptions& o, Model& m) : ar(a), opt(o), model(m) {}

void Loader::Ref(const std::string& path, const char* kind, const std::string& by) {
    std::string key = Lower(NormalizePath(path));
    if (key.empty() || !seen.insert(key + "|" + kind).second) return;
    Dependency d;
    std::string canon = ar.CanonicalPath(path);
    d.found = !canon.empty();
    d.path = d.found ? canon : NormalizePath(path);
    int ai = ar.Find(path);
    d.archive = ai >= 0 ? ar.ArchiveName((size_t)ai) : std::string();
    d.kind = kind;
    d.required_by = by;
    deps.push_back(d);
}

std::string Loader::FindTexture(const std::string& base, const std::string& rel) {
    std::string p = ResolveRelative(base, rel);
    if (ar.Exists(p)) return ar.CanonicalPath(p);
    // Some files climb the wrong number of folders; Texture_Dump is at the root.
    std::string norm = NormalizePath(rel);
    std::string low = Lower(norm);
    size_t at = low.find("texture_dump/");
    if (at != std::string::npos && ar.Exists(norm.substr(at))) return ar.CanonicalPath(norm.substr(at));
    at = low.find("envmaps/");
    if (at != std::string::npos && ar.Exists(norm.substr(at))) return ar.CanonicalPath(norm.substr(at));
    return p;
}

// Fills path/found/frames of a texture slot and records the dependencies.
void Loader::ResolveTexture(const std::string& base, const std::string& rel, TextureSlot& ts) {
    ts.path = FindTexture(base, rel);
    ts.found = ar.Exists(ts.path);
    ts.frames.clear();
    if (!ts.found) {
        // Animated texture: "X.tif" is stored as frames "X 01.tif", "X 02.tif", ...
        std::string ext = Extension(ts.path);
        std::string stem = ts.path.substr(0, ts.path.size() - (ext.empty() ? 0 : ext.size() + 1));
        for (int k = 1; k < 100; k++) {
            char num[8];
            snprintf(num, sizeof(num), " %02d", k);
            std::string f = stem + num + (ext.empty() ? std::string() : "." + ext);
            if (!ar.Exists(f)) break;
            ts.frames.push_back(ar.CanonicalPath(f));
        }
        if (!ts.frames.empty()) {
            ts.path = ts.frames[0];
            ts.found = true;
        }
    }
    if (ts.frames.empty()) Ref(ts.path, "texture", base);
    for (const auto& f : ts.frames) Ref(f, "texture", base);
}

bool Loader::TextureAlpha(const std::string& path, bool& has_alpha) {
    has_alpha = false;
    if (path.empty() || !ar.Exists(path)) return false;
    std::string key = Lower(NormalizePath(path));
    auto it = alpha_cache.find(key);
    if (it != alpha_cache.end()) {
        has_alpha = it->second;
        return true;
    }
    bool a = false;
    if (opt_cache) {
        auto t = opt_cache->Get(path);
        if (!t) return false;
        a = t->has_alpha;
    } else {
        std::vector<uint8_t> data;
        if (!ar.Read(path, data)) return false;
        TextureInfo info;
        if (!ReadTextureInfo(data.data(), data.size(), info)) return false;
        if (info.format == PixelFormat::R5G6B5) {
            a = false;
        } else {
            Texture t;
            if (!DecodeTexture(data.data(), data.size(), t, false)) return false;
            a = t.has_alpha;
        }
    }
    alpha_cache[key] = a;
    has_alpha = a;
    return true;
}

void Loader::FinishMaterial(Material& mat, int alpha_kill) {
    std::string lt = Lower(mat.type);
    mat.distortion = lt == "material_distort" || lt == "distort";
    for (const auto& slot : mat.textures) {
        if (slot.slot == "basetexture" || slot.slot == "base") {
            mat.diffuse_texture = slot.path;
            mat.diffuse_found = slot.found;
            std::string t = Lower(slot.tiling);
            mat.mirror_u = t.find("u_mirror") != std::string::npos;
            mat.mirror_v = t.find("v_mirror") != std::string::npos;
            break;
        }
    }
    std::string b = Lower(mat.blending);
    if (b == "alpha") mat.alpha = AlphaMode::Blend;
    else if (b == "additive") mat.alpha = AlphaMode::Additive;
    else if (b == "subtractive") mat.alpha = AlphaMode::Subtractive;
    else {
        mat.alpha = AlphaMode::Opaque;
        bool has_alpha = false;
        if ((IsCutoutCapable(mat) || alpha_kill > 0) && mat.diffuse_found && TextureAlpha(mat.diffuse_texture, has_alpha) &&
            has_alpha)
            mat.alpha = AlphaMode::Cutout;
        if (mat.opacity < 0.999f) mat.alpha = AlphaMode::Blend;
    }
    if (!mat.diffuse_texture.empty() && !mat.diffuse_found)
        model.warnings.push_back("missing texture " + mat.diffuse_texture + " (material " + mat.name + ")");
}

int Loader::NewNode(const std::string& name, const std::string& type, int parent, const float local[16], bool on,
                    int lod) {
    Node n;
    n.name = name;
    n.type = type;
    n.parent = parent;
    memcpy(n.local, local, sizeof(n.local));
    if (parent >= 0) MatMul(local, model.nodes[parent].world, n.world);
    else memcpy(n.world, local, sizeof(n.world));
    n.turned_on = on;
    n.lod = lod;
    model.nodes.push_back(n);
    return (int)model.nodes.size() - 1;
}

void Loader::AddSubMesh(const std::string& name, int node, int material, const std::vector<uint8_t>& vdata,
                        const std::vector<uint8_t>& idata, bool strip, bool shadow, const std::string& where) {
    if (skip_geometry) return;
    VertexBuffer vb;
    std::string err;
    if (!DecodeVertexFile(vdata.data(), vdata.size(), vb, &err)) {
        model.warnings.push_back(where + ": " + err);
        failed_meshes++;
        return;
    }
    std::vector<uint16_t> idx;
    if (!DecodeIndexFile(idata.data(), idata.size(), idx, &err)) {
        model.warnings.push_back(where + ": " + err);
        failed_meshes++;
        return;
    }
    SubMesh sm;
    sm.name = name;
    sm.node_index = node;
    sm.node = node >= 0 ? model.nodes[node].name : std::string();
    sm.material = material;
    if (node >= 0) memcpy(sm.transform, model.nodes[node].world, sizeof(sm.transform));
    else MatIdentity(sm.transform);
    sm.positions = std::move(vb.positions);
    sm.normals = std::move(vb.normals);
    sm.uv0 = std::move(vb.uv0);
    sm.uv1 = std::move(vb.uv1);
    sm.colors = std::move(vb.colors);
    sm.vertex_flags = vb.flags;
    sm.skinned = vb.skinned;
    sm.shadow = shadow;
    BuildTriangles(idx, strip, vb.count, sm.indices);
    if (kFlipWinding)
        for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) std::swap(sm.indices[i + 1], sm.indices[i + 2]);
    model.meshes.push_back(std::move(sm));
}

// ---------------------------------------------------------------------------
// MGMODEL (XML)
// ---------------------------------------------------------------------------

bool Loader::LoadMgmodel(const std::string& path, int parent, int lod) {
    std::vector<uint8_t> data;
    std::string err;
    if (!ar.Read(path, data, nullptr, &err)) {
        error = err;
        return false;
    }
    std::string canon = ar.CanonicalPath(path);
    XmlNode root;
    if (!ParseXml((const char*)data.data(), data.size(), root, &err)) {
        error = path + ": " + err;
        return false;
    }
    if (model.geometry_path.empty()) model.geometry_path = canon;
    // Materials and meshes are scoped to this file.
    std::map<std::string, int> materials;
    std::map<std::string, const XmlNode*> meshes;
    for (const auto& el : root.children) {
        if (StartsWithI(el.tag, "material")) {
            Material mat;
            mat.name = el.AttrOr("name");
            mat.type = el.tag;
            ParseColor(el.AttrOr("diffuse", "255,255,255"), mat.diffuse);
            ParseColor(el.AttrOr("selfillum", "0,0,0"), mat.selfillum);
            float op = 1;
            if (ParseFloats(el.AttrOr("opacity", "1"), &op, 1) == 1) mat.opacity = op;
            mat.two_sided = ParseBool(el.AttrOr("two_sided"), false);
            mat.shading = Lower(el.AttrOr("shading"));
            mat.blending = Lower(el.AttrOr("blending", "normal"));
            ParseFloats(el.AttrOr("reflectivity", "0"), &mat.reflectivity, 1);
            mat.env_masked = IEquals(el.AttrOr("envmaptype"), "masked");
            ParseColor(el.AttrOr("colorshift0_color", "0,0,0"), mat.colorshift[0]);
            ParseColor(el.AttrOr("colorshift1_color", "0,0,0"), mat.colorshift[1]);
            mat.colorshift1_diffuse = ParseBool(el.AttrOr("colorshift1_diffuse"), false);
            ParseFloats(el.AttrOr("colorshift0_bloom", "-1"), &mat.colorshift_bloom[0], 1);
            ParseFloats(el.AttrOr("colorshift1_bloom", "-1"), &mat.colorshift_bloom[1], 1);
            ParseFloats(el.AttrOr("specular_bloom", "-1"), &mat.specular_bloom, 1);
            ParseColor(el.AttrOr("spec_color", "0,0,0"), mat.spec_color);
            ParseFloats(el.AttrOr("shin_strength", "1"), &mat.shin_strength, 1);
            mat.detail_blend = Lower(el.AttrOr("blend", el.AttrOr("detail_blend")));
            ParseFloats(el.AttrOr("blend_level", el.AttrOr("detail_blend_level", "-1")), &mat.detail_level, 1);
            for (const auto& c : el.children) {
                const std::string* fn = c.Attr("filename");
                if (!fn || fn->empty()) continue;
                TextureSlot ts;
                ts.slot = Lower(c.tag);
                ResolveTexture(canon, *fn, ts);
                ts.tiling = c.AttrOr("tiling");
                mat.textures.push_back(ts);
            }
            float ak = -1;
            ParseFloats(el.AttrOr("alpha_kill", "-1"), &ak, 1);
            FinishMaterial(mat, (int)ak);
            model.materials.push_back(mat);
            materials[Lower(mat.name)] = (int)model.materials.size() - 1;
        } else if (IEquals(el.tag, "meshxml")) {
            meshes[Lower(el.AttrOr("name"))] = &el;
        }
    }
    for (const auto& el : root.children) {
        if (StartsWithI(el.tag, "node_")) AddXmlNode(el, canon, parent, lod, materials, meshes);
    }
    return true;
}

void Loader::AddXmlNode(const XmlNode& el, const std::string& file, int parent, int lod,
                        const std::map<std::string, int>& materials,
                        const std::map<std::string, const XmlNode*>& meshes) {
    if (++node_budget > 20000) return;
    float pos[3] = {0, 0, 0}, axis[3] = {1, 0, 0}, scale[3] = {1, 1, 1}, angle = 0;
    ParseFloats(el.AttrOr("position"), pos, 3);
    ParseFloats(el.AttrOr("rot_axis"), axis, 3);
    ParseFloats(el.AttrOr("rot_angle"), &angle, 1);
    if (ParseFloats(el.AttrOr("scale"), scale, 3) != 3) scale[0] = scale[1] = scale[2] = 1;
    float local[16];
    MatFromSRT(scale, axis, angle, pos, local);
    bool on = ParseBool(el.AttrOr("turned_on", "true"), true);
    int ni = NewNode(el.AttrOr("name"), Lower(el.tag), parent, local, on, lod);
    if (IEquals(el.tag, "node_light")) {
        Node::Light& L = model.nodes[ni].light;
        model.nodes[ni].is_light = true;
        L.spot = IEquals(el.AttrOr("light_type"), "spot");
        ParseColor(el.AttrOr("light_color", "255,255,255"), L.color);
        ParseFloats(el.AttrOr("light_inner_radius"), &L.inner_radius, 1);
        ParseFloats(el.AttrOr("light_outer_radius"), &L.outer_radius, 1);
        ParseFloats(el.AttrOr("light_inner_angle"), &L.inner_angle, 1);
        ParseFloats(el.AttrOr("light_outer_angle"), &L.outer_angle, 1);
        float pr = 0;
        if (ParseFloats(el.AttrOr("light_priority"), &pr, 1) == 1) L.priority = (int)pr;
        if (const XmlNode* lm = el.Child("lightmap")) {
            L.has_lightmap = true;
            ParseFloats(lm->AttrOr("min_height"), &L.min_height, 1);
            ParseFloats(lm->AttrOr("max_height"), &L.max_height, 1);
            ParseFloats(lm->AttrOr("min_radius"), &L.min_radius, 1);
            ParseFloats(lm->AttrOr("max_radius"), &L.max_radius, 1);
            ParseColor(lm->AttrOr("min_color", "255,255,255"), L.min_color);
            ParseColor(lm->AttrOr("max_color", "255,255,255"), L.max_color);
        }
    }
    if (IEquals(el.tag, "node_anim")) {
        std::string sk = el.AttrOr("skeleton");
        if (!sk.empty()) {
            std::string p = ResolveRelative(file, sk);
            if (model.skeleton.empty()) model.skeleton = ar.Exists(p) ? ar.CanonicalPath(p) : p;
            Ref(p, "skeleton", file);
        }
        std::string anm = el.AttrOr("animation_set");
        if (!anm.empty()) {
            std::string p = ResolveRelative(file, anm);
            model.animations.push_back(ar.Exists(p) ? ar.CanonicalPath(p) : p);
            Ref(p, "animation", file);
            ReadAnimationSet(p);
        }
    }
    for (const auto& c : el.children) {
        bool shadow = IEquals(c.tag, "shadow_mesh");
        if (IEquals(c.tag, "mesh") || shadow) {
            if (shadow && !opt.include_shadow_meshes && !collect_all) continue;
            auto it = meshes.find(Lower(c.AttrOr("name")));
            if (it == meshes.end()) {
                model.warnings.push_back(file + ": node " + model.nodes[ni].name + " uses unknown mesh " + c.AttrOr("name"));
                continue;
            }
            const XmlNode& mx = *it->second;
            std::string vname = file + "{" + mx.AttrOr("vertices") + "}";
            std::string iname = file + "{" + mx.AttrOr("indices") + "}";
            Ref(vname, "verts", file);
            Ref(iname, "indices", file);
            int mat = -1;
            auto mit = materials.find(Lower(mx.AttrOr("material")));
            if (mit != materials.end()) mat = mit->second;
            if (skip_geometry) continue;
            std::vector<uint8_t> vd, id;
            std::string err;
            if (!ar.Read(vname, vd, nullptr, &err) || !ar.Read(iname, id, nullptr, &err)) {
                model.warnings.push_back("missing mesh data: " + err);
                failed_meshes++;
                continue;
            }
            std::string type = Lower(mx.AttrOr("type"));
            AddSubMesh(mx.AttrOr("name"), ni, mat, vd, id, type == "indexedstrip", shadow, vname);
        } else if (IEquals(c.tag, "expression_list")) {
            for (const auto& e : c.children) {
                std::string f = e.AttrOr("file");
                if (!f.empty()) {
                    std::string p = ResolveRelative(file, f);
                    model_files.push_back(p);
                    Ref(p, "other", file);
                }
            }
        }
    }
    if (const XmlNode* kids = el.Child("children")) {
        bool lod_switch = IEquals(el.tag, "node_switch_lod");
        int k = 0;
        for (const auto& c : kids->children) {
            if (!StartsWithI(c.tag, "node_")) continue;
            AddXmlNode(c, file, ni, lod_switch ? std::max(lod, k) : lod, materials, meshes);
            k++;
        }
    }
}

// An .anm animation set lists .gr2 files (text). Record any it names.
void Loader::ReadAnimationSet(const std::string& path) {
    std::vector<uint8_t> data;
    if (!ar.Read(path, data)) return;
    std::string text((const char*)data.data(), data.size());
    std::string low = Lower(text);
    size_t at = 0;
    while ((at = low.find(".gr2", at)) != std::string::npos) {
        size_t end = at + 4, start = at;
        while (start > 0 && text[start - 1] != '"' && text[start - 1] != '=' && text[start - 1] != '\n' &&
               text[start - 1] != '\r' && text[start - 1] != '\'' && text[start - 1] != '>')
            start--;
        std::string rel = Trim(text.substr(start, end - start));
        if (!rel.empty()) {
            std::string p = ResolveRelative(path, rel);
            Ref(p, "animation", path);
        }
        at = end;
    }
}

// ---------------------------------------------------------------------------
// Legacy MAXPort text set
// ---------------------------------------------------------------------------

int Loader::LegacyMaterial(const std::string& path) {
    std::string key = Lower(NormalizePath(path));
    auto it = legacy_materials.find(key);
    if (it != legacy_materials.end()) return it->second;
    int index = -1;
    std::vector<uint8_t> data;
    Ref(path, "material", current_file);
    if (ar.Read(path, data)) {
        std::vector<IniSection> secs;
        ParseIni((const char*)data.data(), data.size(), secs);
        for (const auto& s : secs) {
            if (!IEquals(s.name, "material")) continue;
            Material mat;
            mat.name = s.GetOr("name", FileStem(path));
            mat.type = s.GetOr("type", "BASE");
            ParseColor(s.GetOr("diff_color", "255,255,255"), mat.diffuse);
            ParseColor(s.GetOr("selfillum_color", "0,0,0"), mat.selfillum);
            ParseColor(s.GetOr("colorshift0_color", "0,0,0"), mat.colorshift[0]);
            ParseColor(s.GetOr("colorshift1_color", "0,0,0"), mat.colorshift[1]);
            mat.colorshift1_diffuse = ParseBool(s.GetOr("colorshift1_diffuse"), false);
            ParseFloats(s.GetOr("colorshift0_bloom", "-1"), &mat.colorshift_bloom[0], 1);
            ParseFloats(s.GetOr("colorshift1_bloom", "-1"), &mat.colorshift_bloom[1], 1);
            ParseFloats(s.GetOr("specular_bloom", "-1"), &mat.specular_bloom, 1);
            ParseColor(s.GetOr("spec_color", "0,0,0"), mat.spec_color);
            ParseFloats(s.GetOr("shin_strength", "1"), &mat.shin_strength, 1);
            if (IEquals(mat.type, "WATER")) {
                mat.water = true;
                ParseColor(s.GetOr("deep_color", "0,0,0"), mat.deep);
                ParseColor(s.GetOr("shallow_color", "0,0,0"), mat.shallow);
                ParseFloats(s.GetOr("fresnel_min", "0.77"), &mat.fresnel_min, 1);
                ParseFloats(s.GetOr("fresnel_max", "1"), &mat.fresnel_max, 1);
            }
            float op = 1;
            if (ParseFloats(s.GetOr("opacity", "1"), &op, 1) == 1) mat.opacity = op;
            mat.two_sided = ParseBool(s.GetOr("two_sided"), false);
            mat.shading = Lower(s.GetOr("shading"));
            mat.blending = Lower(s.GetOr("blending", "normal"));
            static const char* keys[][2] = {{"base", "basetexture"}, {"detail", "detailtexture"},
                                            {"multi", "multitexture"}, {"waves", "wavetexture"},
                                            {"unique", "uniquetexture"}};
            std::string canon = ar.CanonicalPath(path);
            for (const auto& k : keys) {
                const std::string* v = s.Get(k[0]);
                if (!v || v->empty()) continue;
                TextureSlot ts;
                ts.slot = k[1];
                ResolveTexture(canon.empty() ? path : canon, *v, ts);
                ts.tiling = s.GetOr((std::string(k[0]) + "_tiling").c_str());
                mat.textures.push_back(ts);
            }
            float ak = -1;
            ParseFloats(s.GetOr("alpha_kill", "-1"), &ak, 1);
            FinishMaterial(mat, (int)ak);
            model.materials.push_back(mat);
            index = (int)model.materials.size() - 1;
            break;
        }
    } else {
        model.warnings.push_back("missing material " + path);
    }
    legacy_materials[key] = index;
    return index;
}

void Loader::LegacyMesh(const std::string& mesh_path, int node) {
    Ref(mesh_path, "mesh", current_file);
    std::vector<uint8_t> data;
    if (!ar.Read(mesh_path, data)) {
        model.warnings.push_back("missing mesh " + mesh_path);
        failed_meshes++;
        return;
    }
    std::vector<IniSection> secs;
    ParseIni((const char*)data.data(), data.size(), secs);
    const IniSection* s = nullptr;
    for (const auto& x : secs)
        if (IEquals(x.name, "mesh")) s = &x;
    if (!s) {
        model.warnings.push_back("no [mesh] in " + mesh_path);
        failed_meshes++;
        return;
    }
    std::string canon = ar.CanonicalPath(mesh_path);
    std::string vpath = ResolveRelative(canon, s->GetOr("vertices"));
    std::string fpath = ResolveRelative(canon, s->GetOr("faces"));
    std::string mpath = s->GetOr("material");
    Ref(vpath, "geomvert", canon);
    Ref(fpath, "geomface", canon);
    std::string saved = current_file;
    current_file = canon;
    int mat = mpath.empty() ? -1 : LegacyMaterial(ResolveRelative(canon, mpath));
    current_file = saved;
    if (skip_geometry) return;
    std::vector<uint8_t> vd, id;
    if (!ar.Read(vpath, vd) || !ar.Read(fpath, id)) {
        model.warnings.push_back("missing geometry for " + mesh_path);
        failed_meshes++;
        return;
    }
    std::string type = Lower(s->GetOr("type"));
    // Every retail .mesh without a type is an indexed list (see NOTES.md).
    AddSubMesh(s->GetOr("name", FileStem(mesh_path)), node, mat, vd, id, type == "indexedstrip", false, vpath);
}

// Consumes sections[cursor] and its num_children followers.
void Loader::LegacySection(const std::vector<IniSection>& secs, size_t& cursor, const std::string& file, int parent,
                           int lod) {
    if (cursor >= secs.size() || ++node_budget > 20000) return;
    const IniSection& s = secs[cursor++];
    float pos[3] = {0, 0, 0}, axis[3] = {1, 0, 0}, scale[3] = {1, 1, 1}, angle = 0;
    ParseFloats(s.GetOr("position"), pos, 3);
    ParseFloats(s.GetOr("rot_axis"), axis, 3);
    ParseFloats(s.GetOr("rot_angle"), &angle, 1);
    if (ParseFloats(s.GetOr("scale"), scale, 3) != 3) scale[0] = scale[1] = scale[2] = 1;
    float local[16];
    MatFromSRT(scale, axis, angle, pos, local);
    std::string type = s.GetOr("type", "NODE");
    bool on = ParseBool(s.GetOr("turned_on", "true"), true);
    int ni = NewNode(s.name, type, parent, local, on, lod);
    if (IEquals(type, "ANIMNODE")) {
        AnimNode(s, file, ni);
        return;
    }
    for (const auto& kv : s.values) {
        if (IEquals(kv.first, "mesh") && !kv.second.empty()) {
            std::string saved = current_file;
            current_file = file;
            LegacyMesh(ResolveRelative(file, kv.second), ni);
            current_file = saved;
        }
    }
    int kids = 0;
    float k = 0;
    if (ParseFloats(s.GetOr("num_children", "0"), &k, 1) == 1) kids = (int)k;
    for (int i = 0; i < kids && cursor < secs.size(); i++) LegacySection(secs, cursor, file, ni, lod);
}

void Loader::AnimNode(const IniSection& s, const std::string& file, int node) {
    std::string sk = s.GetOr("skeleton");
    if (!sk.empty()) {
        std::string p = ResolveRelative(file, sk);
        if (model.skeleton.empty()) model.skeleton = ar.Exists(p) ? ar.CanonicalPath(p) : p;
        Ref(p, "skeleton", file);
    }
    pending_child.clear();
    for (const auto& kv : s.values)
        if (IEquals(kv.first, "child") && !kv.second.empty()) pending_child.push_back(ResolveRelative(file, kv.second));
    pending_anim_node = node;
}

bool Loader::LoadNodeFile(const std::string& path, int parent, int lod) {
    struct DepthGuard {
        int& depth;
        ~DepthGuard() { depth--; }
    } guard{file_depth};
    if (++file_depth > 8) {
        error = "ANIMNODE nesting too deep at " + path;
        return false;
    }
    std::vector<uint8_t> data;
    std::string err;
    if (!ar.Read(path, data, nullptr, &err)) {
        error = err;
        return false;
    }
    std::string canon = ar.CanonicalPath(path);
    std::vector<IniSection> all, secs;
    ParseIni((const char*)data.data(), data.size(), all);
    for (auto& s : all) {
        if (IEquals(s.name, "fileinfo")) continue;
        if (StartsWithI(s.name, "animation") && s.Get("animation")) {
            std::string a = s.GetOr("animation");
            if (!a.empty()) {
                std::string p = ResolveRelative(canon, a);
                model.animations.push_back(ar.Exists(p) ? ar.CanonicalPath(p) : p);
                Ref(p, "animation", canon);
            }
            continue;
        }
        secs.push_back(s);
    }
    if (secs.empty()) {
        error = path + ": no node sections";
        return false;
    }
    bool has_geometry = false;
    for (const auto& s : secs)
        if (s.Get("mesh")) has_geometry = true;
    if (has_geometry && model.geometry_path.empty()) model.geometry_path = canon;
    size_t cursor = 0;
    while (cursor < secs.size()) {
        pending_child.clear();
        pending_anim_node = -1;
        LegacySection(secs, cursor, canon, parent, lod);
        if (pending_anim_node >= 0) {
            int an = pending_anim_node;
            std::vector<std::string> kids = pending_child;
            for (const auto& c : kids) {
                Ref(c, Extension(c) == "mgmodel" ? "model" : "node", canon);
                if (!LoadAny(c, an, lod)) return false;
            }
        }
    }
    return true;
}

bool Loader::LoadAny(const std::string& path, int parent, int lod) {
    std::string ext = Extension(path);
    if (ext == "mgmodel") return LoadMgmodel(path, parent, lod);
    if (ext == "node") return LoadNodeFile(path, parent, lod);
    error = "unsupported model type: " + path;
    return false;
}

// ---------------------------------------------------------------------------
// .mtb visibility
// ---------------------------------------------------------------------------

std::string Loader::GuessMtb() const {
    std::vector<std::string> c;
    auto add = [&](const std::string& folder, const std::string& stem) {
        if (stem.empty()) return;
        c.push_back((folder.empty() ? std::string() : folder + "/") + stem + ".mtb");
        std::string low = Lower(stem);
        if (low.size() > 4 && low.compare(low.size() - 4, 4, "_low") == 0)
            c.push_back((folder.empty() ? std::string() : folder + "/") + stem.substr(0, stem.size() - 4) + ".mtb");
    };
    std::string top = model.path, geo = model.geometry_path.empty() ? model.path : model.geometry_path;
    add(FolderOf(top), FileStem(top));
    add(FolderOf(geo), FileStem(geo));
    add(FolderOf(top), FileStem(geo));
    add(FolderOf(FolderOf(geo)), FileStem(geo));
    for (const auto& p : c)
        if (ar.Exists(p)) return ar.CanonicalPath(p);
    return std::string();
}

void Loader::ApplyMtb(const std::string& path) {
    std::vector<uint8_t> data;
    if (!ar.Read(path, data)) {
        model.warnings.push_back("missing mtb " + path);
        return;
    }
    model.mtb_path = ar.CanonicalPath(path);
    std::vector<WdfObject> objs;
    std::string err;
    if (!ParseWdf(data.data(), data.size(), objs, &err)) {
        model.warnings.push_back(path + ": " + err);
        return;
    }
    static const uint32_t kShow = WdfKey("MAModSubObjectShow"), kName = WdfKey("SubObjectName"),
                          kEnable = WdfKey("Enable"), kMin = WdfKey("MinDamagePercent"),
                          kMax = WdfKey("MaxDamagePercent"), kAband = WdfKey("Abandoned"),
                          kTree = WdfKey("EnableTree");
    struct Rule {
        bool any = false, visible = false, tree = false;
    };
    std::map<std::string, Rule> rules;
    // Heat-sink glows are tinted by the 'Mech's heat; at rest they use the minimum colour.
    static const uint32_t kHeat = WdfKey("MAModMechHeatSink"), kR = WdfKey("MinimumHeatColorRed"),
                          kG = WdfKey("MinimumHeatColorGreen"), kB = WdfKey("MinimumHeatColorBlue");
    for (const auto& o : objs) {
        if (o.class_key != kHeat) continue;
        const WdfProperty *n = o.Find(kName), *r = o.Find(kR), *g = o.Find(kG), *b = o.Find(kB);
        if (!n || !r || !g || !b) continue;
        std::string node = Lower(n->AsString());
        for (auto& sm : model.meshes)
            if (Lower(sm.node) == node) {
                sm.tint[0] = r->AsU32() / 255.0f;
                sm.tint[1] = g->AsU32() / 255.0f;
                sm.tint[2] = b->AsU32() / 255.0f;
            }
    }
    // SubObjStateFlags bit 0x10 marks defence states (shield bubbles, AMS). A node in
    // that state whose meshes are all effect materials only shows while the shield is up.
    static const uint32_t kState = WdfKey("MAModSubObjectState"), kFlags = WdfKey("SubObjStateFlags");
    for (const auto& o : objs) {
        if (o.class_key != kState) continue;
        const WdfProperty *n = o.Find(kName), *fl = o.Find(kFlags);
        if (!n || !fl || !(fl->AsU32() & 0x10)) continue;
        std::string node = Lower(n->AsString());
        bool any = false, effect_only = true;
        for (const auto& sm : model.meshes) {
            if (Lower(sm.node) != node) continue;
            any = true;
            const Material* mt = sm.material >= 0 ? &model.materials[sm.material] : nullptr;
            bool effect = mt && (mt->distortion || mt->alpha == AlphaMode::Additive || mt->alpha == AlphaMode::Blend);
            if (!effect) effect_only = false;
        }
        if (!any || !effect_only) continue;
        for (auto& nd : model.nodes)
            if (Lower(nd.name) == node) nd.hidden_by_mtb = true;
    }
    for (const auto& o : objs) {
        if (o.class_key != kShow) continue;
        const WdfProperty* n = o.Find(kName);
        if (!n) continue;
        Rule& r = rules[Lower(n->AsString())];
        r.any = true;
        const WdfProperty *en = o.Find(kEnable), *mn = o.Find(kMin), *mx = o.Find(kMax), *ab = o.Find(kAband),
                          *tr = o.Find(kTree);
        int enable = en ? en->AsI32() : 1, lo = mn ? mn->AsI32() : 0, hi = mx ? mx->AsI32() : 100,
            aband = ab ? ab->AsI32() : 0;
        if (tr && tr->AsI32()) r.tree = true;
        // Undamaged (100%), piloted (Abandoned 0): shown when an enabled rule covers it.
        if (enable && aband == 0 && lo <= 100 && hi >= 100) r.visible = true;
    }
    for (size_t i = 0; i < model.nodes.size(); i++) {
        auto it = rules.find(Lower(model.nodes[i].name));
        if (it == rules.end() || !it->second.any || it->second.visible) continue;
        model.nodes[i].hidden_by_mtb = true;
        if (it->second.tree) {
            for (size_t j = i + 1; j < model.nodes.size(); j++) {
                int p = model.nodes[j].parent;
                while (p >= 0 && p != (int)i) p = model.nodes[p].parent;
                if (p == (int)i) model.nodes[j].hidden_by_mtb = true;
            }
        }
    }
}

void Loader::Finish() {
    float inf = 1e30f;
    for (int k = 0; k < 3; k++) {
        model.bbox_min[k] = inf;
        model.bbox_max[k] = -inf;
    }
    model.has_bbox = false;
    std::vector<SubMesh> kept;
    for (auto& m : model.meshes) {
        const Node* n = m.node_index >= 0 ? &model.nodes[m.node_index] : nullptr;
        bool vis = !m.shadow;
        if (n) {
            if (!n->turned_on || n->hidden_by_mtb) vis = false;
            if (n->lod > 0 && !opt.all_lods) vis = false;
        }
        m.visible = vis;
        for (int k = 0; k < 3; k++) {
            m.bbox_min[k] = inf;
            m.bbox_max[k] = -inf;
        }
        for (size_t v = 0; v + 2 < m.positions.size(); v += 3) {
            float p[3];
            TransformPoint(m.transform, &m.positions[v], p);
            for (int k = 0; k < 3; k++) {
                m.bbox_min[k] = std::min(m.bbox_min[k], p[k]);
                m.bbox_max[k] = std::max(m.bbox_max[k], p[k]);
            }
        }
        if (vis && !m.positions.empty() && !m.indices.empty()) {
            model.has_bbox = true;
            for (int k = 0; k < 3; k++) {
                model.bbox_min[k] = std::min(model.bbox_min[k], m.bbox_min[k]);
                model.bbox_max[k] = std::max(model.bbox_max[k], m.bbox_max[k]);
            }
        }
        if (vis || opt.include_invisible) kept.push_back(std::move(m));
    }
    model.meshes = std::move(kept);
    model.failed_meshes = failed_meshes;
    if (!model.has_bbox)
        for (int k = 0; k < 3; k++) model.bbox_min[k] = model.bbox_max[k] = 0;
}

bool Loader::Run(const std::string& path) {
    std::string canon = ResolveModelPath(ar, path);
    if (canon.empty()) {
        error = "not found: " + path;
        return false;
    }
    model.requested_path = path;
    model.path = canon;
    if (Lower(canon) != Lower(NormalizePath(path))) model.warnings.push_back("resolved " + path + " as " + canon);
    int ai = ar.Find(canon);
    model.archive = ai >= 0 ? ar.ArchiveName((size_t)ai) : std::string();
    std::string ext = Extension(canon);
    Ref(canon, ext == "mgmodel" ? "model" : "node", std::string());
    if (!LoadAny(canon, -1, 0)) return false;
    if (opt.apply_mtb) {
        std::string mtb = opt.mtb_path.empty() ? GuessMtb() : NormalizePath(opt.mtb_path);
        if (!mtb.empty()) {
            Ref(mtb, "mtb", canon);
            ApplyMtb(mtb);
        }
    }
    Finish();
    return true;
}

}  // namespace detail

std::string ResolveModelPath(const ArchiveSet& ar, const std::string& path) {
    std::string p = NormalizePath(path);
    std::string folder = detail::FolderOf(p), stem = detail::FileStem(p), ext = Extension(p);
    std::string pre = folder.empty() ? std::string() : folder + "/";
    std::string other = ext == "node" ? "MGMODEL" : ext == "mgmodel" ? "NODE" : std::string();
    std::string dot_ext = p.substr(p.size() - ext.size());  // keep the original spelling
    std::vector<std::string> tries = {p, pre + stem + "_low." + dot_ext};
    if (!other.empty()) {
        tries.push_back(pre + stem + "." + other);
        tries.push_back(pre + stem + "_low." + other);
    }
    // Archive order first, name variant second: a level's own _low model beats
    // the full-detail file of a later archive, as when the game mounts that level.
    for (size_t a = 0; a < ar.Count(); a++)
        for (const auto& t : tries)
            if (ar.ExistsIn(a, t)) return ar.CanonicalPath(t);
    return std::string();
}

bool LoadModel(const ArchiveSet& archives, const std::string& path, Model& out, const ModelLoadOptions& options,
               std::string* error) {
    out = Model();
    detail::Loader l(archives, options, out);
    l.opt_cache = options.texture_cache;
    if (!l.Run(path)) {
        if (error) *error = l.error;
        return false;
    }
    return true;
}

void BakeSubMesh(const SubMesh& m, std::vector<float>& positions, std::vector<float>& normals) {
    positions.resize(m.positions.size());
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3) detail::TransformPoint(m.transform, &m.positions[i], &positions[i]);
    normals.resize(m.normals.size());
    float nm[16];
    detail::MatInvert3x3Transpose(m.transform, nm);
    for (size_t i = 0; i + 2 < m.normals.size(); i += 3) {
        float* o = &normals[i];
        detail::TransformDir(nm, &m.normals[i], o);
        float l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (l > 1e-8f) {
            o[0] /= l;
            o[1] /= l;
            o[2] /= l;
        }
    }
}

}  // namespace ma2m
