#include "model_cache.h"

#include "../core/crop.h"
#include "../core/weather.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ed {

using namespace ma2;

ModelCache::~ModelCache() {
    clear();
    end_frame();
}

// GPU objects are released after the frame is presented: the current frame's draw lists (palette
// thumbnails, model batches) may still point at them when a new document clears the cache.
void ModelCache::clear() {
    auto retire = [&](IUnknown* p) { if (p) retired_.push_back(p); };
    for (auto& [k, m] : models_)
        for (auto& g : m->meshes) {
            retire(g.vb);
            retire(g.ib);
        }
    // Models stay alive until end_frame() for the same reason.
    for (auto& [k, m] : models_) retired_models_.push_back(std::move(m));
    models_.clear();
    queue_.clear();
    for (auto& [k, s] : srvs_) retire(s);
    srvs_.clear();
    for (auto& [k, s] : thumbs_) retire(s);
    thumbs_.clear();
    water_.clear();
    fx_.clear();
    particles_.clear();
}

void ModelCache::end_frame() {
    for (IUnknown* p : retired_) p->Release();
    retired_.clear();
    retired_models_.clear();
}

void ModelCache::set_archives(const fs::path& game_files, const std::string& base, const std::vector<std::string>& levels) {
    if (archives_ && iequals(base, base_)) return;
    clear();
    textures_.reset();
    archives_ = std::make_unique<ma2m::ArchiveSet>();
    base_ = lower(base);
    archives_->Add(path_utf8(game_files / widen(base_)));
    archives_->Add(path_utf8(game_files / L"common.mgf"));
    for (auto& l : levels) if (!iequals(l, base_)) archives_->Add(path_utf8(game_files / widen(l)));
    textures_ = std::make_unique<ma2m::TextureCache>(*archives_, true);
}

GpuModel* ModelCache::get(const std::string& model, const std::string& mtb, const std::string& crop) {
    if (model.empty() || !archives_) return nullptr;
    std::string key = lower(model) + "|" + lower(mtb);
    if (!crop.empty()) key += "|" + crop;
    auto it = models_.find(key);
    if (it != models_.end()) return it->second.get();
    auto m = std::make_unique<GpuModel>();
    GpuModel* raw = m.get();
    models_[key] = std::move(m);
    queue_.push_back(key);
    return raw;
}

bool ModelCache::pump(double budget_ms) {
    auto t0 = std::chrono::steady_clock::now();
    bool any = false;
    while (!queue_.empty()) {
        std::string key = queue_.front();
        queue_.pop_front();
        auto it = models_.find(key);
        if (it == models_.end()) continue;
        load(key, *it->second);
        any = true;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > budget_ms) break;
    }
    return any;
}

ID3D11ShaderResourceView* ModelCache::texture_srv(const std::string& path, int kind) {
    if (path.empty() || !textures_) return nullptr;
    std::string key = (kind ? std::string(1, char('0' + kind)) + "|" : "") + lower(path);
    auto it = srvs_.find(key);
    if (it != srvs_.end()) return it->second;
    ID3D11ShaderResourceView* srv = nullptr;
    auto tex = textures_->Get(path);
    if (kind == 1 && tex && tex->depth > 1 && int(tex->layers.size()) >= tex->depth && !tex->layers[0].empty()) {
        // Volume: layers[slice][mip]; level l keeps depth >> l slices, packed slice after slice.
        const auto& top = tex->layers[0][0];
        D3D11_TEXTURE3D_DESC td{};
        td.Width = UINT(top.width); td.Height = UINT(top.height); td.Depth = UINT(tex->depth);
        UINT levels = 0;
        for (;; levels++) {
            int d = std::max(1, tex->depth >> levels);
            bool ok = levels < 16;
            for (int z = 0; ok && z < d; z++) ok = tex->layers[size_t(z)].size() > levels;
            if (!ok) break;
            const auto& im = tex->layers[0][levels];
            if (im.width != std::max(1, top.width >> levels) || im.height != std::max(1, top.height >> levels)) break;
        }
        std::vector<std::vector<uint8_t>> data(levels);
        std::vector<D3D11_SUBRESOURCE_DATA> sd(levels);
        for (UINT l = 0; l < levels; l++) {
            int d = std::max(1, tex->depth >> l);
            const auto& im0 = tex->layers[0][l];
            for (int z = 0; z < d; z++) {
                const auto& px = tex->layers[size_t(z)][l].rgba;
                data[l].insert(data[l].end(), px.begin(), px.end());
            }
            sd[l] = {data[l].data(), UINT(im0.width * 4), UINT(im0.width * im0.height * 4)};
        }
        td.MipLevels = levels;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture3D* t = nullptr;
        if (levels && SUCCEEDED(dev_->CreateTexture3D(&td, sd.data(), &t))) {
            dev_->CreateShaderResourceView(t, nullptr, &srv);
            t->Release();
        }
    } else if (kind == 2 && tex && tex->frames == 6 && tex->depth == 1 && tex->layers.size() >= 6) {
        // Cube: frames are the faces in D3D order (+X, -X, +Y, -Y, +Z, -Z).
        const auto& top = tex->layers[0][0];
        UINT levels = UINT(tex->layers[0].size());
        for (int f = 0; f < 6; f++) levels = std::min(levels, UINT(tex->layers[size_t(f)].size()));
        for (UINT i = 0; i < levels; i++)
            if (tex->layers[0][i].width != std::max(1, top.width >> i) || tex->layers[0][i].height != std::max(1, top.height >> i)) { levels = i; break; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(top.width); td.Height = UINT(top.height);
        td.MipLevels = levels; td.ArraySize = 6;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        std::vector<D3D11_SUBRESOURCE_DATA> sd;
        for (int f = 0; f < 6; f++)
            for (UINT i = 0; i < levels; i++) sd.push_back({tex->layers[size_t(f)][i].rgba.data(), UINT(tex->layers[size_t(f)][i].width * 4), 0});
        ID3D11Texture2D* t = nullptr;
        if (levels && SUCCEEDED(dev_->CreateTexture2D(&td, sd.data(), &t))) {
            dev_->CreateShaderResourceView(t, nullptr, &srv);
            t->Release();
        }
    } else if (kind == 0 && tex && !tex->layers.empty() && !tex->layers[0].empty()) {
        const auto& mips = tex->layers[0];
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(mips[0].width);
        td.Height = UINT(mips[0].height);
        // Only a full chain down to the last level the file stores.
        UINT levels = 0;
        for (size_t i = 0; i < mips.size(); i++) {
            if (mips[i].width != std::max(1, mips[0].width >> i) || mips[i].height != std::max(1, mips[0].height >> i)) break;
            levels++;
        }
        td.MipLevels = levels;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> sd(levels);
        for (UINT i = 0; i < levels; i++) sd[i] = {mips[i].rgba.data(), UINT(mips[i].width * 4), 0};
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(dev_->CreateTexture2D(&td, sd.data(), &t))) {
            dev_->CreateShaderResourceView(t, nullptr, &srv);
            t->Release();
        }
    }
    srvs_[key] = srv;
    return srv;
}

// "a, b, c" (0-255) -> ABGR
static bool parse_rgb(const std::string& v, uint32_t& out) {
    int r, g, b;
    if (sscanf(v.c_str(), "%d , %d , %d", &r, &g, &b) != 3) return false;
    out = 0xFF000000u | uint32_t(std::clamp(b, 0, 255)) << 16 | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(r, 0, 255));
    return true;
}

static std::string xml_attr(const std::string& x, size_t from, size_t to, const char* name) {
    std::string key = std::string(" ") + name + "=\"";
    size_t a = x.find(key, from);
    if (a == std::string::npos || a > to) return "";
    a += key.size();
    size_t b = x.find('"', a);
    return b == std::string::npos ? "" : x.substr(a, b - a);
}

// "..\..\Texture_Dump\X.tif" relative to the model's folder -> archive path.
static std::string resolve_rel(const std::string& model, const std::string& rel) {
    std::string dir = norm_path(model);
    dir.erase(dir.find_last_of('/') == std::string::npos ? 0 : dir.find_last_of('/'));
    std::vector<std::string> parts;
    for (size_t a = 0; a <= dir.size();) {
        size_t b = dir.find('/', a);
        if (b == std::string::npos) b = dir.size();
        if (b > a) parts.push_back(dir.substr(a, b - a));
        a = b + 1;
    }
    std::string r = norm_path(rel);
    for (size_t a = 0; a <= r.size();) {
        size_t b = r.find('/', a);
        if (b == std::string::npos) b = r.size();
        std::string seg = r.substr(a, b - a);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        a = b + 1;
    }
    std::string out;
    for (auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

const WaterMaterial& ModelCache::water_material(const std::string& model) {
    std::string key = lower(model);
    auto it = water_.find(key);
    if (it != water_.end()) return it->second;
    WaterMaterial& w = water_[key];
    std::vector<uint8_t> b;
    if (model.empty() || !archives_ || !archives_->Read(norm_path(model), b)) return w;
    std::string x(b.begin(), b.end());
    size_t a = x.find("<material_water");
    if (a == std::string::npos) {
        // .NODE water models: a legacy .mat with "type = WATER" (waves=, deep/shallow colour, fresnel).
        ma2m::ModelLoadOptions opt;
        opt.texture_cache = textures_.get();
        ma2m::Model m;
        if (!ma2m::LoadModel(*archives_, norm_path(model), m, opt, nullptr)) return w;
        auto abgr = [](const float c[3]) {
            auto u = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
            return 0xFF000000u | u(c[2]) << 16 | u(c[1]) << 8 | u(c[0]);
        };
        for (const auto& mat : m.materials) {
            if (!mat.water) continue;
            w.deep = abgr(mat.deep);
            w.shallow = abgr(mat.shallow);
            w.fresnel_min = mat.fresnel_min;
            w.fresnel_max = mat.fresnel_max;
            for (const auto& ts : mat.textures)
                if (ts.slot == "wavetexture" && ts.found) w.wave_path = ts.path;
            w.wave = w.wave_path.empty() ? nullptr : texture_srv(w.wave_path, 1);
            w.ok = true;
            break;
        }
        return w;
    }
    size_t e = x.find("</material_water>", a);
    if (e == std::string::npos) e = x.size();
    size_t tag_end = x.find('>', a);
    parse_rgb(xml_attr(x, a, tag_end, "deep_color"), w.deep);
    parse_rgb(xml_attr(x, a, tag_end, "shallow_color"), w.shallow);
    std::string f0 = xml_attr(x, a, tag_end, "fresnel_min"), f1 = xml_attr(x, a, tag_end, "fresnel_max");
    if (!f0.empty()) w.fresnel_min = float(atof(f0.c_str()));
    if (!f1.empty()) w.fresnel_max = float(atof(f1.c_str()));
    size_t wt = x.find("<wavetexture", a), rt = x.find("<reflectiontexture", a);
    if (wt < e) w.wave_path = resolve_rel(model, xml_attr(x, wt, x.find('>', wt), "filename"));
    if (rt < e) w.refl_path = resolve_rel(model, xml_attr(x, rt, x.find('>', rt), "filename"));
    // The wave texture is a volume normal map, the reflection a cube map (frame trace, R2-3).
    w.wave = w.wave_path.empty() ? nullptr : texture_srv(w.wave_path, 1);
    w.refl = w.refl_path.empty() ? nullptr : texture_srv(w.refl_path, 2);
    w.ok = true;
    return w;
}

// The .mtb's MAModSubObjectParticleEffect objects (each hangs off the node named by SubObjectName,
// at the Offset property): the effects this model starts in the game.
float AlphaRamp::at(float t) const {
    float range = mx - mn, step = delta * sign;
    if (range <= 1e-6f || std::fabs(step) < 1e-9f) return std::clamp(cur, mn, mx);
    float x = cur + step * t;  // unbounded travel along the ramp
    if (pingpong) {  // triangle wave in [mn, mx]
        float u = std::fmod(x - mn, 2 * range);
        if (u < 0) u += 2 * range;
        return mn + (u <= range ? u : 2 * range - u);
    }
    if (loop) {  // restart at the saved value on a clamp
        float u = std::fmod(x - cur, range);
        if (u < 0) u += range;
        float v = cur + u;
        return v > mx ? mn + (v - mx) : v;
    }
    return std::clamp(x, mn, mx);
}

std::map<std::string, AlphaRamp> ModelCache::collect_blends(const ma2m::Model& model) {
    std::map<std::string, AlphaRamp> out;
    std::vector<uint8_t> b;
    if (model.mtb_path.empty() || !archives_->Read(model.mtb_path, b)) return out;
    Wdf w;
    if (!w.parse(b.data(), b.size())) return out;
    static const uint32_t K_BLEND = key_of("MAModSubObjectBlend");
    for (const WObj& o : w.objs) {
        if (o.cls != K_BLEND) continue;
        std::string node = lower(o.str("SubObjectName"));
        if (node.empty()) continue;
        AlphaRamp r;
        r.on = true;
        r.mn = o.f32("MinAlphaLevel");
        r.mx = o.f32("MaxAlphaLevel", 1.f);
        r.delta = o.f32("AlphaLevelDelta");
        r.cur = o.f32("CurAlphaLevel", r.mx);
        r.sign = o.f32("CurSign", 1.f) < 0 ? -1.f : 1.f;
        r.pingpong = o.i32("PingPong") != 0;
        r.loop = o.i32("Loop") != 0;
        r.alpha_blend = o.i32("GetOrigAlpha") == 0;
        out[node] = r;
    }
    return out;
}

void ModelCache::collect_effects(const std::string& key, const ma2m::Model& model) {
    std::vector<FxRef>& out = fx_[key];
    out.clear();
    std::vector<uint8_t> b;
    if (model.mtb_path.empty() || !archives_->Read(model.mtb_path, b)) return;
    Wdf w;
    if (!w.parse(b.data(), b.size())) return;
    static const uint32_t K_FX = key_of("MAModSubObjectParticleEffect");
    // developer probe: every object of the model's .mtb (class, property keys and values), appended to the file named by
    // MA2SE_DUMP_MTB or, when the editor is started by the automation tools (no env), by the first line of %TEMP%\ma2se_dump_mtb.txt
    std::string dm_path;
    if (const char* dm = getenv("MA2SE_DUMP_MTB")) dm_path = dm;
    else if (const char* tmp = getenv("TEMP")) {
        char line[1024] = {};
        if (FILE* c = fopen((std::string(tmp) + "\\ma2se_dump_mtb.txt").c_str(), "r")) {
            if (fgets(line, sizeof line, c)) dm_path = line;
            fclose(c);
            while (!dm_path.empty() && (dm_path.back() == '\n' || dm_path.back() == '\r')) dm_path.pop_back();
        }
    }
    if (!dm_path.empty()) {
        if (FILE* f = fopen(dm_path.c_str(), "a")) {
            fprintf(f, "== %s\n", key.c_str());
            for (const auto& mt : model.materials)
                fprintf(f, "  MAT %s type %s alpha %d cs0 %.2f,%.2f,%.2f cs1 %.2f,%.2f,%.2f cs1_diffuse %d bloom %.3f %.3f spec %.3f spec_color %.2f,%.2f,%.2f shin %.2f\n", mt.name.c_str(), mt.type.c_str(), int(mt.alpha),
                        mt.colorshift[0][0], mt.colorshift[0][1], mt.colorshift[0][2], mt.colorshift[1][0], mt.colorshift[1][1], mt.colorshift[1][2], int(mt.colorshift1_diffuse), mt.colorshift_bloom[0], mt.colorshift_bloom[1], mt.specular_bloom, mt.spec_color[0], mt.spec_color[1], mt.spec_color[2], mt.shin_strength);
            for (size_t ni = 0; ni < model.nodes.size(); ni++)
                if (lower(model.nodes[ni].type).find("facing") != std::string::npos) {
                    fprintf(f, "  FACING %s at %.2f,%.2f,%.2f\n", model.nodes[ni].name.c_str(), model.nodes[ni].world[12], model.nodes[ni].world[13], model.nodes[ni].world[14]);
                    for (const auto& sm : model.meshes)
                        if (sm.node_index == int(ni)) fprintf(f, "    mesh %s verts %zu mat %d %s\n", sm.name.c_str(), sm.positions.size() / 3, sm.material, sm.material >= 0 ? model.materials[size_t(sm.material)].name.c_str() : "");
                }
            for (const WObj& o : w.objs) {
                fprintf(f, "  class %08X\n", o.cls);
                for (const WProp& pr : o.props) fprintf(f, "    %08X t%u %s\n", pr.key, pr.type, pr.display().c_str());
            }
            fclose(f);
        }
    }
    for (const WObj& o : w.objs) {
        if (o.cls != K_FX) continue;
        FxRef r;
        r.file = norm_path(o.str("EffectFile"));
        if (r.file.empty()) continue;
        Vec3 off = o.vec3("Offset");
        float p[3] = {off.x, off.y, off.z};
        std::string node = lower(o.str("SubObjectName"));
        for (const auto& n : model.nodes)
            if (!node.empty() && lower(n.name) == node) {
                // node -> model, row-vector layout (translation in [12..14])
                for (int k = 0; k < 3; k++) r.pos[k] = off.x * n.world[k] + off.y * n.world[4 + k] + off.z * n.world[8 + k] + n.world[12 + k];
                for (int j = 0; j < 3; j++) {  // columns of rot = the node's axes (row-vector layout rows), scale removed
                    float len = std::sqrt(n.world[4 * j] * n.world[4 * j] + n.world[4 * j + 1] * n.world[4 * j + 1] + n.world[4 * j + 2] * n.world[4 * j + 2]);
                    if (len < 1e-4f) { r.rot[0] = r.rot[4] = r.rot[8] = 1; r.rot[1] = r.rot[2] = r.rot[3] = r.rot[5] = r.rot[6] = r.rot[7] = 0; break; }
                    for (int i = 0; i < 3; i++) r.rot[i * 3 + j] = n.world[4 * j + i] / len;
                }
                node.clear();
                break;
            }
        if (!node.empty()) for (int k = 0; k < 3; k++) r.pos[k] = p[k];  // node not found: model origin
        r.retrigger = o.i32("Behavior") == 1;
        r.intermittent = o.f32("AliveTimeMax") > 0 && o.f32("DeadTimeMax") > 0;
        r.on_s = (o.f32("AliveTimeMin") + o.f32("AliveTimeMax")) * 0.0005f;
        r.off_s = (o.f32("DeadTimeMin") + o.f32("DeadTimeMax")) * 0.0005f;
        r.damage_state = o.find("HitEffectStateFlags") && o.find("HitEffectStateFlags")->as_int() != 0;
        r.terrain_only = o.i32("TerrainCondition") != 0;
        r.rain_only = o.i32("RainOnly") != 0;
        out.push_back(std::move(r));
    }
}

const std::vector<FxRef>* ModelCache::effects(const std::string& model, const std::string& mtb) {
    auto it = fx_.find(lower(model) + "|" + lower(mtb));
    return it == fx_.end() ? nullptr : &it->second;
}

const ma2::ParticleFx* ModelCache::particle(const std::string& path) {
    std::string key = lower(norm_path(path));
    auto it = particles_.find(key);
    if (it != particles_.end()) return it->second.get();
    std::unique_ptr<ma2::ParticleFx> fx;
    std::vector<uint8_t> b;
    if (archives_ && archives_->Read(norm_path(path), b)) {
        fx = std::make_unique<ma2::ParticleFx>();
        if (!parse_particle(std::string(b.begin(), b.end()), *fx)) fx.reset();
    }
    return (particles_[key] = std::move(fx)).get();
}

const ma2::ParticleFx* ModelCache::rain_particle(const std::string& path, int level) {
    std::string key = "rain" + std::to_string(level) + ":" + lower(norm_path(path));
    auto it = particles_.find(key);
    if (it != particles_.end()) return it->second.get();
    std::unique_ptr<ma2::ParticleFx> fx;
    std::vector<uint8_t> b;
    if (archives_ && archives_->Read(norm_path(path), b)) {
        std::string s(b.begin(), b.end());
        fx = std::make_unique<ma2::ParticleFx>();
        if (!ma2::weather::rain_particle_xml(s, ma2::weather::RainLevel(level)) || !parse_particle(s, *fx)) fx.reset();
        // The game's rain streaks reach the ground: same-pose pair ux\shots\rain-levels-pair-heavy-game.png (eye 7 m above the
        // ground). Read as m/s^2 its gravity="10" never brought them below 2.8 m above the eye (no rain in view); 2.8x makes the
        // 1.1 s fall ~30 m (22 m above the eye to 8 m below it), which matches the pair's streak spread. Snow is left as it is.
        else for (auto& em : fx->emitters) for (auto& sy : em.systems) sy.gravity *= 2.8f;
    }
    return (particles_[key] = std::move(fx)).get();
}

struct TexSampling { bool point = false, nomip = false; float bias = 0; };

// Each <material_*> element's first <basetexture>: mipmapping, mipmap_bias and filter (the loader keeps none of them).
static std::map<std::string, TexSampling> texture_sampling(const std::string& x) {
    std::map<std::string, TexSampling> out;
    auto attr = [](const std::string& tag, const char* name) {
        std::string k = std::string(" ") + name + "=\"";
        size_t a = tag.find(k);
        if (a == std::string::npos) return std::string();
        a += k.size();
        size_t b = tag.find('"', a);
        return b == std::string::npos ? std::string() : tag.substr(a, b - a);
    };
    for (size_t a = x.find("<material_"); a != std::string::npos;) {
        size_t next = x.find("<material_", a + 1), e = x.find('>', a);
        if (e == std::string::npos) break;
        std::string name = lower(attr(x.substr(a, e - a), "name"));
        size_t t = x.find("<basetexture", e);
        if (!name.empty() && t != std::string::npos && t < next) {
            size_t te = x.find('>', t);
            std::string tag = x.substr(t, te == std::string::npos ? std::string::npos : te - t);
            TexSampling ts;
            ts.point = lower(attr(tag, "filter")) == "filter_point";
            ts.nomip = lower(attr(tag, "mipmapping")) == "false";
            std::string bias = attr(tag, "mipmap_bias");
            if (!bias.empty()) ts.bias = float(atof(bias.c_str()));
            out.emplace(name, ts);
        }
        a = next;
    }
    return out;
}

void ModelCache::load(const std::string& key, GpuModel& m) {
    size_t bar = key.find('|'), bar2 = key.find('|', bar + 1);
    std::string path = key.substr(0, bar), mtb = key.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1);
    ma2::crop::Rect crop_rect;
    const bool cropped = bar2 != std::string::npos && ma2::crop::parse(key.substr(bar2 + 1), crop_rect);
    ma2m::ModelLoadOptions opt;
    opt.mtb_path = mtb;
    opt.texture_cache = textures_.get();
    opt.include_invisible = true;  // for lamp parts (see GpuMesh::lamp_part); other hidden meshes are skipped below
    ma2m::Model model;
    bool loaded = ma2m::LoadModel(*archives_, path, model, opt, &m.error);
    std::map<std::string, AlphaRamp> blends;
    if (loaded) {
        collect_effects(key, model);
        blends = collect_blends(model);
    }
    std::map<std::string, TexSampling> sampling;
    if (std::vector<uint8_t> raw; loaded && archives_->Read(norm_path(path), raw)) sampling = texture_sampling(std::string(raw.begin(), raw.end()));
    if (!loaded || model.meshes.empty()) {
        m.state = GpuModel::Failed;
        return;
    }
    struct V { float p[3], n[3], uv[2], uv1[2]; uint32_t col; };  // 44 bytes (render.cpp kModelStride)
    // Lamp parts: a node hidden by the .mtb (or turned off) whose subtree holds a node_light, and
    // everything under it (Env_Highway_Light: lightbulb > spotlight + cone). The game shows them lit.
    std::vector<char> lamp_root(model.nodes.size(), 0);
    for (size_t i = 0; i < model.nodes.size(); i++) {
        if (!model.nodes[i].is_light) continue;
        for (int p = int(i); p >= 0; p = model.nodes[size_t(p)].parent)
            if (model.nodes[size_t(p)].hidden_by_mtb || !model.nodes[size_t(p)].turned_on) lamp_root[size_t(p)] = 1;
    }
    auto under_lamp_root = [&](int n) {
        for (; n >= 0; n = model.nodes[size_t(n)].parent) if (lamp_root[size_t(n)]) return true;
        return false;
    };
    bool water_only = false;  // a water mesh was skipped: the model stays Ready (its box sizes the water plane)
    float vmin[3] = {1e30f, 1e30f, 1e30f}, vmax[3] = {-1e30f, -1e30f, -1e30f};  // drawn solid parts (selection brackets)
    float smin[3] = {1e30f, 1e30f, 1e30f}, smax[3] = {-1e30f, -1e30f, -1e30f};  // opaque / cut-out parts (roofs to place on)
    for (auto& sm : model.meshes) {
        const ma2m::Node* node = sm.node_index >= 0 && sm.node_index < int(model.nodes.size()) ? &model.nodes[size_t(sm.node_index)] : nullptr;
        // Lamp parts: lightbulb, cone and spotlight start hidden in the .mtb, but the game draws them when the lights are on (M01b trace,
        // game px 816,236: one colour_shift lightbulb draw per Env_Highway_Light, n 43 vertices, white bar; IsNightTime 1). They show at night.
        bool lamp_part = sm.node_index >= 0 && under_lamp_root(sm.node_index);
        if (const char* dq = getenv("MA2SE_DUMP_SKY"); dq && lower(path).find(dq) != std::string::npos) {  // developer: every submesh of the models whose path has this text
            const ma2m::Material* dm = sm.material >= 0 && sm.material < int(model.materials.size()) ? &model.materials[size_t(sm.material)] : nullptr;
            fprintf(stderr, "[SKYMESH] %s sub %s node %s visible %d shadow %d idx %zu tex %s alpha %d opacity %.3f diffuse %.3f %.3f %.3f selfillum %.3f %.3f %.3f type %s\n", path.c_str(), sm.name.c_str(),
                    node ? node->name.c_str() : "-", int(sm.visible), int(sm.shadow), sm.indices.size(), dm ? dm->diffuse_texture.c_str() : "-", dm ? int(dm->alpha) : -1, dm ? dm->opacity : 0.f,
                    dm ? dm->diffuse[0] : 0.f, dm ? dm->diffuse[1] : 0.f, dm ? dm->diffuse[2] : 0.f, dm ? dm->selfillum[0] : 0.f, dm ? dm->selfillum[1] : 0.f, dm ? dm->selfillum[2] : 0.f, dm ? dm->type.c_str() : "-");
            if (dm) for (const auto& ts : dm->textures) fprintf(stderr, "[SKYMESH]   tex slot %s found %d path %s\n", ts.slot.c_str(), int(ts.found), ts.path.c_str());
        }
        if ((!sm.visible && !lamp_part) || sm.shadow || sm.indices.empty()) continue;
        if (const char* sk = getenv("MA2SE_SKIP_SUB"); sk && ("," + std::string(sk) + ",").find("," + std::string(sm.name.c_str()) + ",") != std::string::npos) continue;  // developer: skip submeshes by name (comma list) to find what draws a pixel
        // Damage cracks (cracks_upper_torso, *crack* materials) only show on a damaged unit (R2-6b).
        if (lower(sm.name).rfind("cracks", 0) == 0) continue;
        const ma2m::Material* mat = sm.material >= 0 && sm.material < int(model.materials.size()) ? &model.materials[size_t(sm.material)] : nullptr;
        if (mat && mat->distortion) continue;
        // The water surface itself is the renderer's water plane (Renderer::Quad); a water model's other layers
        // (the Hesperus oily sheets) draw as meshes over it.
        if (mat && (mat->water || lower(mat->type) == "material_water")) { water_only = true; continue; }
        if (mat && lower(mat->name).find("crack") != std::string::npos) continue;
        // Heat-sink glows (additive, tinted by the .mtb heat colour) are dark on a cool unit at rest.
        bool heat_tint = sm.tint[0] < 0.999f || sm.tint[1] < 0.999f || sm.tint[2] < 0.999f;
        if (mat && mat->alpha == ma2m::AlphaMode::Additive &&
            (heat_tint || lower(sm.name).rfind("heat_", 0) == 0 || lower(mat->name).rfind("mech_heatsink", 0) == 0))
            continue;
        // Shield bubbles show only while shields are up (KillingField 'Mechs).
        if (mat && (lower(mat->name).rfind("shield", 0) == 0 || lower(mat->diffuse_texture).find("wpn_shields_anim") != std::string::npos)) continue;
        std::vector<float> pos, nrm;
        ma2m::BakeSubMesh(sm, pos, nrm);
        if (cropped) {
            // Cropped road piece / foundation: the same cut the saved model gets (crop.cpp), here on the baked model-space mesh.
            const size_t n0 = pos.size() / 3;
            std::vector<ma2::crop::Vert> cv(n0);
            for (size_t i = 0; i < n0; i++) {
                for (int k = 0; k < 3; k++) { cv[i].p[k] = pos[i * 3 + k]; if (nrm.size() >= (i + 1) * 3) cv[i].n[k] = nrm[i * 3 + k]; }
                if (sm.uv0.size() >= (i + 1) * 2) { cv[i].uv0[0] = sm.uv0[i * 2]; cv[i].uv0[1] = sm.uv0[i * 2 + 1]; }
                if (sm.uv1.size() >= (i + 1) * 2) { cv[i].uv1[0] = sm.uv1[i * 2]; cv[i].uv1[1] = sm.uv1[i * 2 + 1]; }
                if (sm.colors.size() == n0) {
                    uint32_t c = sm.colors[i];
                    for (int k = 0; k < 4; k++) cv[i].col[k] = float((c >> (8 * k)) & 255) / 255.f;
                }
            }
            std::vector<uint32_t> tris = sm.indices;
            ma2::crop::clip(cv, tris, ma2::crop::rect_planes(crop_rect));
            if (tris.empty()) continue;
            const size_t n1 = cv.size();
            pos.assign(n1 * 3, 0.f);
            nrm.assign(n1 * 3, 0.f);
            sm.uv0.assign(sm.uv0.empty() ? 0 : n1 * 2, 0.f);
            sm.uv1.assign(sm.uv1.empty() ? 0 : n1 * 2, 0.f);
            const bool cols = sm.colors.size() == n0;
            sm.colors.assign(cols ? n1 : 0, 0u);
            for (size_t i = 0; i < n1; i++) {
                for (int k = 0; k < 3; k++) { pos[i * 3 + k] = cv[i].p[k]; nrm[i * 3 + k] = cv[i].n[k]; }
                if (!sm.uv0.empty()) { sm.uv0[i * 2] = cv[i].uv0[0]; sm.uv0[i * 2 + 1] = cv[i].uv0[1]; }
                if (!sm.uv1.empty()) { sm.uv1[i * 2] = cv[i].uv1[0]; sm.uv1[i * 2 + 1] = cv[i].uv1[1]; }
                if (cols) {
                    uint32_t c = 0;
                    for (int k = 0; k < 4; k++) c |= uint32_t(std::lround(std::clamp(cv[i].col[k], 0.f, 1.f) * 255.f)) << (8 * k);
                    sm.colors[i] = c;
                }
            }
            sm.indices = std::move(tris);
        }
        size_t nv = pos.size() / 3;
        std::vector<V> verts(nv);
        float lmin[3] = {1e30f, 1e30f, 1e30f}, lmax[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < nv; i++)
            for (int k = 0; k < 3; k++) { lmin[k] = std::min(lmin[k], pos[i * 3 + k]); lmax[k] = std::max(lmax[k], pos[i * 3 + k]); }
        for (size_t i = 0; i < nv; i++) {
            V& v = verts[i];
            for (int k = 0; k < 3; k++) { v.p[k] = pos[i * 3 + k]; v.n[k] = nrm.size() >= (i + 1) * 3 ? nrm[i * 3 + k] : (k == 1 ? 1.f : 0.f); }
            v.uv[0] = sm.uv0.size() >= (i + 1) * 2 ? sm.uv0[i * 2] : 0;
            v.uv[1] = sm.uv0.size() >= (i + 1) * 2 ? sm.uv0[i * 2 + 1] : 0;
            bool two = sm.uv1.size() >= (i + 1) * 2;
            v.uv1[0] = two ? sm.uv1[i * 2] : v.uv[0];
            v.uv1[1] = two ? sm.uv1[i * 2 + 1] : v.uv[1];
            v.col = sm.colors.size() == nv ? sm.colors[i] : 0xFFFFFFFFu;
        }
        GpuMesh g;
        if (!blends.empty())  // the blend object sits on the mesh's node or one above it
            for (int n = sm.node_index; n >= 0 && n < int(model.nodes.size()); n = model.nodes[size_t(n)].parent) {
                auto it = blends.find(lower(model.nodes[size_t(n)].name));
                if (it != blends.end()) { g.ramp = it->second; break; }
            }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(verts.size() * sizeof(V));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{verts.data(), 0, 0};
        if (FAILED(dev_->CreateBuffer(&bd, &sd, &g.vb))) continue;
        bd.ByteWidth = UINT(sm.indices.size() * 4);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = sm.indices.data();
        if (FAILED(dev_->CreateBuffer(&bd, &sd, &g.ib))) { g.vb->Release(); continue; }
        g.count = UINT(sm.indices.size());
        if (mat) {
            g.srv = texture_srv(mat->diffuse_texture);
            g.alpha = int(mat->alpha);
            g.two_sided = mat->two_sided;
            g.unlit = mat->shading == "material_only";
            g.translucent = mat->shading == "translucent";
            for (int k = 0; k < 3; k++) g.color[k] = std::min(1.f, mat->diffuse[k] * sm.tint[k] + mat->selfillum[k]);
            for (int k = 0; k < 3; k++) g.skycol[k] = std::min(2.f, mat->diffuse[k] * sm.tint[k] + mat->selfillum[k]);
            g.color[3] = mat->opacity;
            g.address = (mat->mirror_u ? 1 : 0) | (mat->mirror_v ? 2 : 0);
            if (auto ts = sampling.find(lower(mat->name)); ts != sampling.end()) {
                g.tex_point = ts->second.point;
                g.tex_nomip = ts->second.nomip;
                g.tex_bias = ts->second.bias;
            }
            g.lamp_part = lamp_part;
            if (lower(path).find("buildings") != std::string::npos)  // MABuilding only
                for (int n = sm.node_index; n >= 0 && n < int(model.nodes.size()); n = model.nodes[size_t(n)].parent)
                if (lower(model.nodes[size_t(n)].name) == "lights") { g.night_light = true; break; }
            // M09A trace (IsNightTime 0): the game still draws the building's additive spot cone, so only windows and point lights stay gated.
            if (g.night_light && mat->alpha == ma2m::AlphaMode::Additive) {
                std::string cn = lower(sm.name) + " " + (node ? lower(node->name) : std::string());
                if (cn.find("cone") != std::string::npos || cn.find("beam") != std::string::npos) g.night_light = false;  // NOT plain "spot": M16 Tower_03 'spotlights' sprites stay gated (game frame at IsNightTime 0 shows no cones; M09A's are floodlight_cones / flood_cones)
            }
            std::string mt = lower(mat->type);
            if (mt == "detail" || mt == "material_detail")
                for (const auto& ts : mat->textures)
                    if (ts.slot == "detailtexture" && ts.found && (g.detail = texture_srv(ts.path)))
                        g.detail_mode = mat->alpha == ma2m::AlphaMode::Blend ? 2 : mat->alpha == ma2m::AlphaMode::Additive ? 3 : 1;
            // detail_blend (research mat-detail-blend): modulate2x lerp(base, 2 x base x detail, level),
            // alphablend lerp(base, lerp(base, detail, detail.a), level), additive2x lerp(base, base + 2 x detail - 1,
            // level); blend_level is the level for every mode (KF pad 0.5, M13b 0.8, M05 0.44).
            if (mat->detail_level >= 0) g.detail_level = std::min(1.f, mat->detail_level);
            g.detail_kind = mat->detail_blend.find("add") != std::string::npos ? 2 : mat->detail_blend.find("alpha") != std::string::npos ? 1 : 0;
            for (const auto& ts : mat->textures)
                if (ts.slot == "normal_map" && ts.found) g.normal = texture_srv(ts.path);
                else if (ts.slot == "cubetexture" && ts.found) g.cube = texture_srv(ts.path, 2);
            g.vcol_glow = !g.srv && mat->alpha == ma2m::AlphaMode::Additive;
            if (lower(mat->type) == "material_color_shift") std::memcpy(g.cshift, mat->colorshift[0], sizeof g.cshift);
            if (lower(mat->type).find("env") != std::string::npos) g.reflectivity = mat->reflectivity > 0 ? mat->reflectivity : 0.5f;
            if (g.reflectivity > 0 && mat->env_masked) g.reflectivity = -g.reflectivity;  // masked: lerp(base, cube, reflectivity)
            else if (g.reflectivity > 0 && g.alpha == 0 && mat->opacity >= 0.99f) g.reflectivity += 10.f;  // opaque env (building windows / doors): +10 selects the lerp(base, cube, k) path in the model shader
            // Fully self-lit opaque parts (eyes, heat sinks) add light in the game, never cover (R2-6b).
            if (g.alpha == 0 && !g.reflectivity && std::max({mat->selfillum[0], mat->selfillum[1], mat->selfillum[2]}) > 0.9f) g.alpha = 3;
            // Sky light rays (Hesperus env_sky light_rayNN: blended, selfillum 1, 'Light_043_light'): the game draws them SRC_ALPHA / ONE (M16 trace,
            // draws 80B98000 n14 with d0 alpha per vertex), i.e. they add light to the sky instead of lerping toward the ray texture.
            if (g.alpha == int(ma2m::AlphaMode::Blend) && lower(sm.name).rfind("light_ray", 0) == 0 && std::min({mat->selfillum[0], mat->selfillum[1], mat->selfillum[2]}) > 0.9f) g.alpha = 3;
            if (lamp_part) g.unlit = true;
            // Additive self-lit glow meshes (lit building windows 'windows_glow', light strips, spot cones: diffuse = selfillum, M01b): the game
            // draws them at full strength at night, not scaled by the (near zero) night light.
            if (g.alpha == int(ma2m::AlphaMode::Additive) && g.srv && std::max({mat->selfillum[0], mat->selfillum[1], mat->selfillum[2]}) > 0.5f) {
                g.unlit = true;
                for (int k = 0; k < 3; k++) g.color[k] = std::min(2.f, 2 * mat->selfillum[k] * sm.tint[k]);  // not diffuse + selfillum (clamps to white): keeps the amber; x2 = the combiner's 2 x T0 x V0 (M01b trace: colour_shift glow draws, blend CONST_ALPHA/ONE bc 1, d0 0.8)
            }
            // Searchlight beam (Light_026_MechSpotlight, M04 trace): the game's vertex diffuse is Kd x a per-vertex lit scalar
            // (+0.212 on 'lightinside', negative on the first 'light' vertex; two draws sum to 104,134,160 pre-bloom), not
            // selfillum: use 2 x Kd x 0.2 so the cone is not saturated cyan (editor was 242,255,255).
            if (g.alpha == int(ma2m::AlphaMode::Additive) && g.srv && lower(mat->name).find("mechspotlight") != std::string::npos) {
                g.unlit = true;
                for (int k = 0; k < 3; k++) g.color[k] = std::min(2.f, 2 * 0.2f * mat->diffuse[k] * sm.tint[k]);
            }
            // Point-light markers (M10 crane, point_light03_blue, g3 trace): 3 stacked quads per marker, vertex diffuse constant per quad
            // = 0.6 x selfillum + a grey step (core .247, halo .417, mid .542; mean .40), not 2 x selfillum (which saturated the blue).
            if (g.alpha == int(ma2m::AlphaMode::Additive) && g.srv && (lower(mat->name).find("point_light") != std::string::npos || lower(sm.name).find("point_light") != std::string::npos)) {
                g.unlit = true;
                for (int k = 0; k < 3; k++) g.color[k] = std::min(2.f, 2 * (0.6f * mat->selfillum[k] + 0.4f) * sm.tint[k]);
            }
            std::string type = lower(mat->type);
            bool glows =mat->colorshift[0][0] + mat->colorshift[0][1] + mat->colorshift[0][2] + mat->colorshift[1][0] + mat->colorshift[1][1] + mat->colorshift[1][2] > 0;
            bool complex = type == "complex" || type == "material_complex";
            if (complex && glows)
                for (const auto& ts : mat->textures)
                    if (ts.slot == "multitexture" && ts.found) {
                        g.multi = texture_srv(ts.path);
                        std::memcpy(g.glow, mat->colorshift, sizeof g.glow);
                        g.tint1 = mat->colorshift1_diffuse;
                    }
            // COMPLEX + detail: the multi mask's alpha gates the detail (R2-9 #3).
            if (complex && !g.detail)
                for (const auto& ts : mat->textures)
                    if (ts.slot == "detailtexture" && ts.found && (g.detail = texture_srv(ts.path))) {
                        g.detail_mode = 4;
                        g.detail_level = mat->detail_level >= 0 ? std::min(1.f, mat->detail_level) : 1.f;
                        for (const auto& ms : mat->textures)
                            if (ms.slot == "multitexture" && ms.found && !g.multi) g.multi = texture_srv(ms.path);
                    }
            // COMPLEX with a cube and no detail (Hesperus spires, R2-15 #10): base' = lerp(cube, base, base.a), ahead of the x2 light.
            if (complex && !g.detail && g.cube) g.complex_env = true;
            // SPECULAR_MASK constant = spec_color x shin_strength (research: ma2.xbe 0x4DE70, PS c0), e.g. the
            // Env_Highway_Light post (0.1) is nearly flat in game.
            // The mask is the material's detailtexture (batch 05, parser sub_0004DB80: loaded into mat+0x6C, stage 1). Without one the
            // game draws plain program 00C01 with no stage 1 (Env_Highway_Light post, Ill Wind trace draw 2358141): no specular.
            if (type.find("specular_mask") != std::string::npos)
                for (const auto& ts : mat->textures)
                    if (ts.slot == "detailtexture" && ts.found && !g.detail && (g.detail = texture_srv(ts.path))) {
                        for (int k = 0; k < 3; k++) g.spec[k] = mat->spec_color[k] * mat->shin_strength;
                        g.spec_detail = true;  // mask = detail.r, bound on t14 (detail_mode stays 0: not a detail layer)
                    }
            // Bloom mask (framebuffer alpha in the game): color_shift = colorshift0_bloom x base.a, COMPLEX = colorshift0/1_bloom x multi.g/.b
            // + specular_bloom x multi.r x cube.a, specular_mask = specular_bloom x mask x cube.a (traces: Bone Dragon / KF / Ill Wind).
            {
                float b0 = std::max(0.f, mat->colorshift_bloom[0]), b1 = std::max(0.f, mat->colorshift_bloom[1]), bs = std::max(0.f, mat->specular_bloom);
                if (type == "material_color_shift" || type == "color_shift") { g.bloom_kind = 1; g.bloom[0] = b0; }
                else if (complex) { g.bloom_kind = 2; g.bloom[0] = b0; g.bloom[1] = b1; g.bloom[2] = bs; }
                else if (g.spec_detail) { g.bloom_kind = 3; g.bloom[2] = bs; }
                // M11a floodlight cone (g3 trace, px 470,135): colorshift bloom 0 yet the bloom composite saturates the cone to FFFFFF; the cone
                // draw (blend CONST_ALPHA .86 / ONE) writes its alpha into the bloom mask: mask = opacity x T0.a (bloom[0] = 2 x opacity x the shader's 0.5).
                if (b0 <= 0 && g.alpha == int(ma2m::AlphaMode::Additive) && g.srv && (type == "material_color_shift" || type == "color_shift")) {
                    std::string cn = lower(sm.name) + " " + (node ? lower(node->name) : std::string());
                    if (cn.find("cone") != std::string::npos || cn.find("beam") != std::string::npos) { g.bloom_kind = 1; g.bloom[0] = 2 * mat->opacity; }
                }
            }
            // COMPLEX specular (KF trace line 10124, R2-15 #1): + multi.r x cube.a x spec_color after the x2.
            if (complex && mat->spec_color[0] + mat->spec_color[1] + mat->spec_color[2] > 0) {
                for (const auto& ts : mat->textures)
                    if (ts.slot == "multitexture" && ts.found && !g.multi) g.multi = texture_srv(ts.path);
                if (g.multi) { for (int k = 0; k < 3; k++) g.spec[k] = mat->spec_color[k] * mat->shin_strength; g.spec_multi = true; }
                // M12 crystal trace (program 08C21 stage 6/7): the specular mask is the detail ALPHA and the added term is the coloured cube RGB,
                // not multi.r x cube.a.
                // Gated on the crystal base texture (Rock_017_Hesperus_crystal): the other COMPLEX + detail + cube materials (Killing Field) are not traced for this program.
                if (g.multi && g.detail && g.cube)
                    for (const auto& ts : mat->textures)
                        if (lower(ts.path).find("crystal") != std::string::npos) g.spec_crystal = true;
            }
        }
        // Glows, cones and beams (additive) reach well past the body: left out of the selection bounds.
        if (g.alpha != int(ma2m::AlphaMode::Additive) && g.alpha != 3)
            for (int k = 0; k < 3; k++) { vmin[k] = std::min(vmin[k], lmin[k]); vmax[k] = std::max(vmax[k], lmax[k]); }
        if (g.alpha == int(ma2m::AlphaMode::Opaque) || g.alpha == int(ma2m::AlphaMode::Cutout)) {
            for (int k = 0; k < 3; k++) { smin[k] = std::min(smin[k], lmin[k]); smax[k] = std::max(smax[k], lmax[k]); }
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3)
                if (sm.indices[i] < nv && sm.indices[i + 1] < nv && sm.indices[i + 2] < nv)
                    for (int c = 0; c < 3; c++) m.solid_tris.insert(m.solid_tris.end(), &pos[sm.indices[i + c] * 3], &pos[sm.indices[i + c] * 3] + 3);
        }
        m.meshes.push_back(g);
    }
    if (model.has_bbox) for (int k = 0; k < 3; k++) { m.bmin[k] = model.bbox_min[k]; m.bmax[k] = model.bbox_max[k]; }
    if (cropped) {  // the box shrinks to the kept part (selection outline, footprint, snapping ends)
        m.bmin[0] = std::clamp(m.bmin[0], crop_rect.x0, crop_rect.x1); m.bmax[0] = std::clamp(m.bmax[0], crop_rect.x0, crop_rect.x1);
        m.bmin[2] = std::clamp(m.bmin[2], crop_rect.z0, crop_rect.z1); m.bmax[2] = std::clamp(m.bmax[2], crop_rect.z0, crop_rect.z1);
    }
    m.has_vbox = vmin[0] <= vmax[0];
    if (m.has_vbox) for (int k = 0; k < 3; k++) { m.vmin[k] = vmin[k]; m.vmax[k] = vmax[k]; }
    m.has_sbox = smin[0] <= smax[0];
    if (m.has_sbox) for (int k = 0; k < 3; k++) { m.smin[k] = smin[k]; m.smax[k] = smax[k]; }
    for (const auto& n : model.nodes) {
        if (!n.is_light || n.lod != 0 || !n.turned_on) continue;
        GpuModel::Light L;
        // Spot lights point along -Y of the light node: the beam meshes say so (Env_Searchlight_Blue_Lg_Angled's 'light' beam
        // runs along -row1 = (0,.17,.99); Env_Highway_Light's node is identity and its 'cone' hangs straight down). +Z (research
        // spot-light-axis) aimed every street lamp sideways into the buildings: user scene pair 10-26, Office20 annex 208 vs game 81.
        for (int k = 0; k < 3; k++) { L.pos[k] = n.world[12 + k]; L.dir[k] = -n.world[4 + k]; }
        float len = std::sqrt(L.dir[0] * L.dir[0] + L.dir[1] * L.dir[1] + L.dir[2] * L.dir[2]);
        for (float& d : L.dir) d = len > 1e-4f ? d / len : 0.f;
        if (len <= 1e-4f) L.dir[1] = -1;
        L.l = n.light;
        m.lights.push_back(L);
    }
    m.state = m.meshes.empty() && !water_only ? GpuModel::Failed : GpuModel::Ready;
}

ID3D11ShaderResourceView* ModelCache::thumbnail(const std::string& path, const std::string& mtb) {
    if (path.empty() || !archives_) return nullptr;
    std::string key = lower(path) + "|" + lower(mtb);
    auto it = thumbs_.find(key);
    if (it != thumbs_.end()) return it->second;
    ID3D11ShaderResourceView* srv = nullptr;
    ma2m::ModelLoadOptions opt;
    opt.mtb_path = mtb;
    opt.texture_cache = textures_.get();
    ma2m::Model model;
    ma2m::Image img;
    ma2m::ThumbnailOptions to;
    to.background = 0xFF302A26u;
    const bool loaded = ma2m::LoadModel(*archives_, path, model, opt);
    if (loaded && !fx_.count(key)) collect_effects(key, model);  // effect pictures use the model's particle list (palette_picture)
    if (loaded && ma2m::RenderThumbnail(model, textures_.get(), img, to)) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(img.width);
        td.Height = UINT(img.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{img.rgba.data(), UINT(img.width * 4), 0};
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(dev_->CreateTexture2D(&td, &sd, &t))) {
            dev_->CreateShaderResourceView(t, nullptr, &srv);
            t->Release();
        }
    }
    thumbs_[key] = srv;
    return srv;
}

}  // namespace ed
