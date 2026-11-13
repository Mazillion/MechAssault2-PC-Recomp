// Small software rasterizer for palette thumbnails and visual checks.
// Left-handed view (game/D3D space), perspective-correct UVs, z-buffer,
// bilinear wrap/mirror sampling, cutout / blend / additive approximations.
#include "internal.h"

#include <cmath>

namespace ma2m {

namespace {

struct V3 {
    float x, y, z;
};
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Norm(V3 a) {
    float l = std::sqrt(Dot(a, a));
    return l > 1e-12f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 1, 0};
}

struct Target {
    int w, h;
    std::vector<float> color;  // rgba
    std::vector<float> depth;
};

struct Sampler {
    const Image* img = nullptr;
    bool mirror_u = false, mirror_v = false;
    void Sample(float u, float v, float out[4]) const {
        if (!img || img->width <= 0) {
            out[0] = out[1] = out[2] = out[3] = 1;
            return;
        }
        int w = img->width, h = img->height;
        float x = u * w - 0.5f, y = v * h - 0.5f;
        int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        float fx = x - x0, fy = y - y0;
        float acc[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
            int xi = x0 + (k & 1), yi = y0 + (k >> 1);
            xi = Wrap(xi, w, mirror_u);
            yi = Wrap(yi, h, mirror_v);
            float wt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            const uint8_t* p = &img->rgba[((size_t)yi * w + xi) * 4];
            for (int c = 0; c < 4; c++) acc[c] += wt * p[c];
        }
        for (int c = 0; c < 4; c++) out[c] = acc[c] / 255.0f;
    }
    static int Wrap(int i, int n, bool mirror) {
        if (mirror) {
            int period = 2 * n;
            i %= period;
            if (i < 0) i += period;
            return i < n ? i : period - 1 - i;
        }
        i %= n;
        return i < 0 ? i + n : i;
    }
};

struct DrawVert {
    float sx, sy, iz;   // screen position, 1/viewz
    float u, v;         // texcoords (pre-divided by z later)
    V3 n;               // world normal
    float col[4];       // vertex color
};

struct MeshJob {
    const SubMesh* mesh;
    const Material* mat;
    std::vector<float> pos, nrm;
    float depth;  // view depth of centre, for sorting transparent meshes
};

}  // namespace

bool RenderThumbnail(const Model& model, TextureCache* textures, Image& out, const ThumbnailOptions& o,
                     std::string* error) {
    int ss = std::max(1, std::min(4, o.supersample));
    Target t{o.width * ss, o.height * ss, {}, {}};
    if (o.width <= 0 || o.height <= 0 || o.width > 4096 || o.height > 4096) {
        if (error) *error = "bad thumbnail size";
        return false;
    }
    t.color.assign((size_t)t.w * t.h * 4, 0.0f);
    float bg[4] = {(o.background & 0xFF) / 255.0f, ((o.background >> 8) & 0xFF) / 255.0f,
                   ((o.background >> 16) & 0xFF) / 255.0f, ((o.background >> 24) & 0xFF) / 255.0f};
    for (size_t i = 0; i < t.color.size(); i += 4)
        for (int c = 0; c < 4; c++) t.color[i + c] = bg[c];
    t.depth.assign((size_t)t.w * t.h, 1e30f);

    if (!model.has_bbox) {
        // Nothing visible: transparent/background image.
        out.width = o.width;
        out.height = o.height;
        out.rgba.assign((size_t)o.width * o.height * 4, 0);
        for (size_t i = 0; i < out.rgba.size(); i += 4)
            for (int c = 0; c < 4; c++) out.rgba[i + c] = (uint8_t)(bg[c] * 255 + 0.5f);
        return true;
    }
    V3 bmin{model.bbox_min[0], model.bbox_min[1], model.bbox_min[2]};
    V3 bmax{model.bbox_max[0], model.bbox_max[1], model.bbox_max[2]};
    V3 center{(bmin.x + bmax.x) / 2, (bmin.y + bmax.y) / 2, (bmin.z + bmax.z) / 2};
    float radius = 0.5f * std::sqrt(Dot(Sub(bmax, bmin), Sub(bmax, bmin)));
    if (radius < 1e-4f) radius = 1;
    const float kPi = 3.14159265f;
    float fov = 30.0f * kPi / 180.0f, f = 1.0f / std::tan(fov / 2);
    float yaw = o.yaw_degrees * kPi / 180.0f, pitch = o.pitch_degrees * kPi / 180.0f;
    float dist = radius / std::sin(fov / 2) * 1.02f;
    V3 dir{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    V3 eye{center.x + dir.x * dist, center.y + dir.y * dist, center.z + dir.z * dist};
    V3 zf = Norm(Sub(center, eye));
    V3 xr = Norm(Cross(V3{0, 1, 0}, zf));
    V3 yu = Cross(zf, xr);
    V3 light = Norm(V3{-zf.x * 0.6f + yu.x * 0.7f - xr.x * 0.4f, -zf.y * 0.6f + yu.y * 0.7f - xr.y * 0.4f,
                       -zf.z * 0.6f + yu.z * 0.7f - xr.z * 0.4f});
    float half = 0.5f * (float)std::min(t.w, t.h);

    std::vector<MeshJob> jobs;
    for (const auto& m : model.meshes) {
        if (!m.visible || m.indices.empty()) continue;
        if (m.material >= 0 && m.material < (int)model.materials.size() && model.materials[m.material].distortion) continue;
        MeshJob j;
        j.mesh = &m;
        j.mat = m.material >= 0 && m.material < (int)model.materials.size() ? &model.materials[m.material] : nullptr;
        BakeSubMesh(m, j.pos, j.nrm);
        V3 c{(m.bbox_min[0] + m.bbox_max[0]) / 2, (m.bbox_min[1] + m.bbox_max[1]) / 2, (m.bbox_min[2] + m.bbox_max[2]) / 2};
        j.depth = Dot(Sub(c, eye), zf);
        jobs.push_back(std::move(j));
    }
    auto is_transparent = [](const Material* mat) {
        return mat && (mat->alpha == AlphaMode::Blend || mat->alpha == AlphaMode::Additive ||
                       mat->alpha == AlphaMode::Subtractive);
    };
    std::stable_sort(jobs.begin(), jobs.end(), [&](const MeshJob& a, const MeshJob& b) {
        bool ta = is_transparent(a.mat), tb = is_transparent(b.mat);
        if (ta != tb) return !ta;
        if (ta) return a.depth > b.depth;
        return false;
    });

    for (const auto& j : jobs) {
        const SubMesh& m = *j.mesh;
        const Material* mat = j.mat;
        std::shared_ptr<const Texture> tex;
        Sampler smp;
        if (o.textured && mat && mat->diffuse_found && textures) {
            tex = textures->Get(mat->diffuse_texture);
            if (tex) smp.img = tex->Top();
            smp.mirror_u = mat->mirror_u;
            smp.mirror_v = mat->mirror_v;
        }
        AlphaMode mode = mat ? mat->alpha : AlphaMode::Opaque;
        bool two_sided = mat ? mat->two_sided : false;
        bool unlit = mat && (mat->shading == "material_only");
        float kd[3] = {1, 1, 1}, ke[3] = {0, 0, 0}, opacity = 1;
        if (mat) {
            for (int c = 0; c < 3; c++) {
                kd[c] = mat->diffuse[c];
                ke[c] = mat->selfillum[c];
            }
            opacity = mat->opacity;
        }
        if (!smp.img && mat && mat->diffuse_texture.empty() && kd[0] + kd[1] + kd[2] < 0.05f) kd[0] = kd[1] = kd[2] = 0.3f;
        size_t nv = j.pos.size() / 3;
        std::vector<DrawVert> dv(nv);
        for (size_t i = 0; i < nv; i++) {
            V3 p{j.pos[i * 3], j.pos[i * 3 + 1], j.pos[i * 3 + 2]};
            V3 d = Sub(p, eye);
            float vx = Dot(d, xr), vy = Dot(d, yu), vz = std::max(1e-4f, Dot(d, zf));
            DrawVert& v = dv[i];
            v.iz = 1.0f / vz;
            v.sx = t.w * 0.5f + vx * v.iz * f * half;
            v.sy = t.h * 0.5f - vy * v.iz * f * half;
            v.u = m.uv0.size() == nv * 2 ? m.uv0[i * 2] : 0;
            v.v = m.uv0.size() == nv * 2 ? m.uv0[i * 2 + 1] : 0;
            v.n = j.nrm.size() == nv * 3 ? V3{j.nrm[i * 3], j.nrm[i * 3 + 1], j.nrm[i * 3 + 2]} : V3{0, 1, 0};
            if (m.colors.size() == nv) {
                uint32_t c = m.colors[i];
                for (int k = 0; k < 4; k++) v.col[k] = ((c >> (8 * k)) & 0xFF) / 255.0f;
            } else {
                v.col[0] = v.col[1] = v.col[2] = v.col[3] = 1;
            }
        }
        for (size_t ti = 0; ti + 2 < m.indices.size(); ti += 3) {
            const DrawVert* a = &dv[m.indices[ti]];
            const DrawVert* b = &dv[m.indices[ti + 1]];
            const DrawVert* c = &dv[m.indices[ti + 2]];
            float area = (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy);
            if (std::fabs(area) < 1e-8f) continue;
            bool front = area > 0;  // pixel space (y down): front faces have positive area
            if (!front && !two_sided) continue;
            float flip = front ? 1.0f : -1.0f;
            int x0 = std::max(0, (int)std::floor(std::min(a->sx, std::min(b->sx, c->sx))));
            int x1 = std::min(t.w - 1, (int)std::ceil(std::max(a->sx, std::max(b->sx, c->sx))));
            int y0 = std::max(0, (int)std::floor(std::min(a->sy, std::min(b->sy, c->sy))));
            int y1 = std::min(t.h - 1, (int)std::ceil(std::max(a->sy, std::max(b->sy, c->sy))));
            if (x0 > x1 || y0 > y1) continue;
            float inv_area = 1.0f / area;
            for (int y = y0; y <= y1; y++) {
                float py = y + 0.5f;
                for (int x = x0; x <= x1; x++) {
                    float px = x + 0.5f;
                    float w0 = ((b->sx - px) * (c->sy - py) - (c->sx - px) * (b->sy - py)) * inv_area;
                    float w1 = ((c->sx - px) * (a->sy - py) - (a->sx - px) * (c->sy - py)) * inv_area;
                    float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float iz = w0 * a->iz + w1 * b->iz + w2 * c->iz;
                    float z = 1.0f / iz;
                    size_t pi = (size_t)y * t.w + x;
                    if (z >= t.depth[pi]) continue;
                    float pa = w0 * a->iz * z, pb = w1 * b->iz * z, pc = w2 * c->iz * z;  // perspective weights
                    float u = pa * a->u + pb * b->u + pc * c->u;
                    float v = pa * a->v + pb * b->v + pc * c->v;
                    float texel[4];
                    smp.Sample(u, v, texel);
                    float vc[4];
                    for (int k = 0; k < 4; k++) vc[k] = pa * a->col[k] + pb * b->col[k] + pc * c->col[k];
                    float alpha = texel[3] * vc[3] * opacity;
                    if (mode == AlphaMode::Cutout && texel[3] < 0.5f) continue;
                    V3 n = Norm(V3{pa * a->n.x + pb * b->n.x + pc * c->n.x, pa * a->n.y + pb * b->n.y + pc * c->n.y,
                                   pa * a->n.z + pb * b->n.z + pc * c->n.z});
                    n = V3{n.x * flip, n.y * flip, n.z * flip};
                    // Preview lighting: ambient + headlight, mildly over-bright like the game's lightmaps.
                    float shade = unlit ? 1.0f : 0.55f + 0.75f * std::max(0.0f, Dot(n, light));
                    float rgb[3];
                    for (int k = 0; k < 3; k++)
                        rgb[k] = std::min(1.0f, texel[k] * vc[k] * m.tint[k] * (kd[k] * shade + ke[k]));
                    float* dst = &t.color[pi * 4];
                    switch (mode) {
                        case AlphaMode::Opaque:
                        case AlphaMode::Cutout:
                            for (int k = 0; k < 3; k++) dst[k] = rgb[k];
                            dst[3] = 1;
                            t.depth[pi] = z;
                            break;
                        case AlphaMode::Blend:
                            for (int k = 0; k < 3; k++) dst[k] = rgb[k] * alpha + dst[k] * (1 - alpha);
                            dst[3] = std::min(1.0f, dst[3] + alpha * (1 - dst[3]));
                            break;
                        case AlphaMode::Additive:
                            for (int k = 0; k < 3; k++) dst[k] = std::min(1.0f, dst[k] + rgb[k] * alpha);
                            dst[3] = std::min(1.0f, dst[3] + alpha * 0.5f * (rgb[0] + rgb[1] + rgb[2]) / 3);
                            break;
                        case AlphaMode::Subtractive:
                            for (int k = 0; k < 3; k++) dst[k] = std::max(0.0f, dst[k] - rgb[k] * alpha);
                            break;
                    }
                }
            }
        }
    }
    out.width = o.width;
    out.height = o.height;
    out.rgba.assign((size_t)o.width * o.height * 4, 0);
    for (int y = 0; y < o.height; y++)
        for (int x = 0; x < o.width; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    const float* p = &t.color[(((size_t)y * ss + sy) * t.w + (size_t)x * ss + sx) * 4];
                    for (int k = 0; k < 4; k++) acc[k] += p[k];
                }
            for (int k = 0; k < 4; k++)
                out.rgba[((size_t)y * o.width + x) * 4 + k] = (uint8_t)std::min(255.0f, acc[k] / (ss * ss) * 255.0f + 0.5f);
        }
    return true;
}

}  // namespace ma2m
