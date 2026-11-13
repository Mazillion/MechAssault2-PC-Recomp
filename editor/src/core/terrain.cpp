#include "terrain.h"

#include <algorithm>
#include <cmath>

namespace ma2 {

static const uint32_t K_TERRAIN = key_of("MATerrain");

float Terrain::at(int gx, int gz) const {
    gx = std::clamp(gx, 0, w - 1);
    gz = std::clamp(gz, 0, h - 1);
    return hgt[size_t(gz) * w + gx];
}

float Terrain::height_at(float x, float z) const {
    if (w < 2 || h < 2) return 0.f;
    float fx = std::clamp(x / scale, 0.f, float(w - 1)), fz = std::clamp(z / scale, 0.f, float(h - 1));
    int x0 = std::min(int(fx), w - 2), z0 = std::min(int(fz), h - 2);
    float tx = fx - float(x0), tz = fz - float(z0);
    float a = at(x0, z0), b = at(x0 + 1, z0), c = at(x0, z0 + 1), d = at(x0 + 1, z0 + 1);
    return (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz;
}

Vec3 Terrain::normal_at(float x, float z) const {
    float e = scale;
    float hx = height_at(x + e, z) - height_at(x - e, z);
    float hz = height_at(x, z + e) - height_at(x, z - e);
    Vec3 n{-hx, 2 * e, -hz};
    float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return {n.x / l, n.y / l, n.z / l};
}

std::vector<uint8_t> Terrain::samples() const {
    std::vector<uint8_t> s(size_t(w) * h);
    float range = std::max(0.001f, max_h - min_h);
    for (int gz = 0; gz < h; gz++)
        for (int gx = 0; gx < w; gx++) {
            float v = (at(gx, gz) - min_h) / range * 255.f;
            s[size_t(h - 1 - gz) * w + gx] = uint8_t(std::clamp(int(std::lround(v)), 0, 255));
        }
    return s;
}

float Terrain::stored(float h) const {
    float range = std::max(0.001f, max_h - min_h);
    return min_h + float(std::clamp(int(std::lround((h - min_h) / range * 255.f)), 0, 255)) / 255.f * range;
}

float Terrain::stored_at_least(float h) const {
    float range = std::max(0.001f, max_h - min_h);
    return min_h + float(std::clamp(int(std::ceil((h - min_h) / range * 255.f - 1e-4f)), 0, 255)) / 255.f * range;
}

void Terrain::from_samples(const std::vector<uint8_t>& s) {
    hgt.assign(size_t(w) * h, 0.f);
    for (int gz = 0; gz < h; gz++)
        for (int gx = 0; gx < w; gx++)
            ref(gx, gz) = min_h + float(s[size_t(h - 1 - gz) * w + gx]) / 255.f * (max_h - min_h);
}

bool Terrain::load_from_world(const Wdf& wd, std::string* err) {
    const WObj* t = nullptr;
    for (auto& o : wd.objs) if (o.cls == K_TERRAIN) { t = &o; break; }
    if (!t) { if (err) *err = "world has no MATerrain"; return false; }
    const WProp* hf = t->find("TerrainHeight Field");
    if (!hf) { if (err) *err = "MATerrain has no heightfield"; return false; }
    if (!style.parse(hf->data, err)) return false;
    Picture p;
    if (!read_picture(style, p, err)) return false;
    std::vector<uint8_t> s;
    if (!picture_samples8(p, s)) { if (err) *err = "heightfield is not 8-bit"; return false; }
    w = int(p.width);
    h = int(p.height);
    min_h = t->f32("TerrainMinHeight", -20.f);
    max_h = t->f32("TerrainMaxHeight", 100.f);
    scale = t->f32("TerrainWorldScale", 8.f);
    // A scale of 0 or NaN from a bad file makes Terrain::raycast's step 0 (an endless loop at the first hover); a huge
    // heightfield (the picture format allows 16384 squared) is a gigabyte of heights plus 40 undo copies.
    if (!(scale > kMinScale && scale < kMaxScale)) scale = 8.f;
    if (w < 2 || h < 2 || w > kMaxSize || h > kMaxSize) {
        if (err) *err = "unsupported terrain size " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    from_samples(s);
    return true;
}

bool Terrain::store_to_world(Wdf& wd, std::string* err) const {
    WObj* t = nullptr;
    for (auto& o : wd.objs) if (o.cls == K_TERRAIN) { t = &o; break; }
    if (!t) { if (err) *err = "world has no MATerrain"; return false; }
    Bytes blob = build_heightfield(style, uint32_t(w), uint32_t(h), samples());
    t->set(key_of("TerrainHeight Field"), PT_BLOB, blob);
    t->set_f32("TerrainMinHeight", min_h);
    t->set_f32("TerrainMaxHeight", max_h);
    t->set_f32("TerrainWorldScale", scale);
    return true;
}

void Terrain::resize_flat(int nw, int nh, float height) {
    w = nw;
    h = nh;
    hgt.assign(size_t(w) * h, height);
}

bool Terrain::raycast(const Vec3& o, const Vec3& d, float& t_out, float max_t) const {
    if (w < 2) return false;
    float step = scale * 0.25f;
    float prev_t = 0, prev_diff = o.y - height_at(o.x, o.z);
    bool prev_in = inside(o.x, o.z);
    // Skip ahead to the map's bounding box quickly.
    for (float t = step; t < max_t; t += step) {
        float x = o.x + d.x * t, y = o.y + d.y * t, z = o.z + d.z * t;
        bool in = inside(x, z);
        float diff = y - height_at(x, z);
        if (in && prev_in && diff <= 0 && prev_diff > 0) {
            float a = prev_t, b = t;
            for (int k = 0; k < 20; k++) {
                float m = (a + b) * 0.5f;
                float dm = o.y + d.y * m - height_at(o.x + d.x * m, o.z + d.z * m);
                if (dm > 0) a = m; else b = m;
            }
            t_out = (a + b) * 0.5f;
            return true;
        }
        if (y < min_h - 100 && d.y < 0) break;
        prev_t = t; prev_diff = diff; prev_in = in;
        if (t > 200) step = scale * 0.5f;
    }
    // A ray that never hits terrain: intersect the map's mean plane so tools still work.
    if (std::fabs(d.y) > 1e-4f) {
        float plane = 0;
        float tt = (plane - o.y) / d.y;
        if (tt > 0) { t_out = tt; return inside(o.x + d.x * tt, o.z + d.z * tt); }
    }
    return false;
}

Image bake_lightmap(const Terrain& t, Vec3 sun, int lw, int lh, float ambient) {
    Image img;
    img.w = lw; img.h = lh;
    img.px.resize(size_t(lw) * lh);
    float l = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (l < 1e-6f) sun = {0, -1, 0}; else sun = {sun.x / l, sun.y / l, sun.z / l};
    Vec3 to_sun{-sun.x, -sun.y, -sun.z};
    if (to_sun.y < 0.05f) to_sun.y = 0.05f;
    for (int py = 0; py < lh; py++)
        for (int px = 0; px < lw; px++) {
            // Same orientation as the heightfield: row 0 = far z.
            float x = (float(px) + 0.5f) / float(lw) * t.size_x();
            float z = (1.f - (float(py) + 0.5f) / float(lh)) * t.size_z();
            Vec3 n = t.normal_at(x, z);
            float diffuse = std::max(0.f, n.x * to_sun.x + n.y * to_sun.y + n.z * to_sun.z);
            // Shadow march toward the sun. Retail lightmaps keep the shadow mask in alpha
            // (0 = shadowed) and the diffuse term in luminance.
            float h0 = t.height_at(x, z) + 0.5f, lit = 1.f;
            for (float s = t.scale; s < 600.f; s += t.scale * 0.75f) {
                float sx = x + to_sun.x * s, sz = z + to_sun.z * s, sy = h0 + to_sun.y * s;
                if (!t.inside(sx, sz)) break;
                float d = t.height_at(sx, sz) - sy;
                if (d > 0) { lit = std::max(0.f, 1.f - d / 3.f); break; }
                if (sy > t.max_h + 10) break;
            }
            float v = ambient + (1.f - ambient) * diffuse;
            uint32_t L = uint32_t(std::clamp(int(v * 255.f), 0, 255));
            uint32_t A = uint32_t(std::clamp(int(lit * 255.f), 0, 255));
            img.px[size_t(py) * lw + px] = L | L << 8 | L << 16 | A << 24;
        }
    return img;
}

}  // namespace ma2
