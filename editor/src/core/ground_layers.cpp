#include "ground_layers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "mgf.h"

namespace ma2 {

static constexpr uint32_t kMagic = 0x4C32414Du;  // 'MA2L'

uint8_t* GroundLayers::mutable_weights() {
    if (!weights) weights = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 8, 0);
    else if (weights.use_count() > 1) weights = std::make_shared<std::vector<uint8_t>>(*weights);
    return weights->data();
}

void GroundLayers::init_from_colormap(const Image& cm) {
    map_w = cm.px.empty() ? 256 : cm.w;
    map_h = cm.px.empty() ? 256 : cm.h;
    w = std::min(map_w * kGroundWeightScale, kGroundWeightMax);
    h = std::min(map_h * kGroundWeightScale, kGroundWeightMax);
    weights = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 8, 0);
    uint8_t* d = weights->data();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float a = 1;
            if (!cm.px.empty()) {
                // Texel centres line up like the GPU's bilinear sampling of the colour map.
                float fx = (x + 0.5f) / w * cm.w - 0.5f, fy = (y + 0.5f) / h * cm.h - 0.5f;
                int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                float tx = fx - x0, ty = fy - y0;
                auto al = [&](int px, int py) {
                    px = std::clamp(px, 0, cm.w - 1); py = std::clamp(py, 0, cm.h - 1);
                    return float(cm.at(px, py) >> 24) / 255.f;
                };
                a = (al(x0, y0) * (1 - tx) + al(x0 + 1, y0) * tx) * (1 - ty) + (al(x0, y0 + 1) * (1 - tx) + al(x0 + 1, y0 + 1) * tx) * ty;
            }
            uint8_t* t = d + (size_t(y) * w + x) * 8;
            t[0] = uint8_t(std::lround(a * 255));
            t[1] = uint8_t(255 - t[0]);
        }
}

void GroundLayers::bake_colormap_alpha(Image& cm) const {
    if (cm.px.empty() || !weights || !w || !h) return;
    for (int y = 0; y < cm.h; y++)
        for (int x = 0; x < cm.w; x++) {
            // Box over the weight texels this colour-map texel covers.
            int x0 = x * w / cm.w, x1 = std::max(x0 + 1, (x + 1) * w / cm.w), y0 = y * h / cm.h, y1 = std::max(y0 + 1, (y + 1) * h / cm.h);
            double a = 0, b = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) { const uint8_t* t = texel(xx, yy); a += t[0]; b += t[1]; }
            if (a + b <= 0) continue;  // only other layers here: the alpha already in the map stays (what the runtime does with all-zero weights)
            uint32_t alpha = uint32_t(std::lround(255.0 * a / (a + b)));
            uint32_t& p = cm.at(x, y);
            p = (p & 0x00FFFFFFu) | alpha << 24;
        }
}

void GroundLayers::fill(int layer) {
    uint8_t* d = mutable_weights();
    for (size_t i = 0; i < size_t(w) * h; i++) {
        std::memset(d + i * 8, 0, 8);
        d[i * 8 + layer] = 255;
    }
}

void GroundLayers::remove_layer(int k) {
    if (k < 2 || k >= count()) return;
    uint8_t* d = mutable_weights();
    for (size_t i = 0; i < size_t(w) * h; i++) {
        uint8_t* t = d + i * 8;
        for (int j = k; j < kGroundMaxLayers - 1; j++) t[j] = t[j + 1];
        t[kGroundMaxLayers - 1] = 0;
    }
    layers.erase(layers.begin() + k);
}

void GroundLayers::paint(int layer, float cx, float cz, float R, float k, float size_x, float size_z, float hardness) {
    if (!weights || layer < 0 || layer >= count()) return;
    uint8_t* d = mutable_weights();
    int x0 = std::max(0, int((cx - R) / size_x * w)), x1 = std::min(w - 1, int((cx + R) / size_x * w) + 1);
    int y0 = std::max(0, int((1 - (cz + R) / size_z) * h)), y1 = std::min(h - 1, int((1 - (cz - R) / size_z) * h) + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float wx = (x + 0.5f) / w * size_x, wz = (1 - (y + 0.5f) / h) * size_z;
            float dist = std::sqrt((wx - cx) * (wx - cx) + (wz - cz) * (wz - cz));
            if (dist >= R) continue;
            float a = brush_falloff(dist, R, hardness) * k;
            uint8_t* t = d + (size_t(y) * w + x) * 8;
            float sum = 0;
            for (int j = 0; j < kGroundMaxLayers; j++) sum += t[j];
            float scale = sum > 0 ? 255.f / sum : 0.f;  // normalise to 255 first
            int total = 0;
            for (int j = 0; j < kGroundMaxLayers; j++) {
                float v = t[j] * scale, target = j == layer ? 255.f : 0.f;
                v += (target - v) * a;
                // Round toward the target so strokes reach 0 / 255.
                int vi = target > v ? int(std::ceil(v)) : int(std::floor(v));
                t[j] = uint8_t(std::clamp(vi, 0, 255));
                total += t[j];
            }
            if (!total) t[layer] = 255;
        }
}

static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (i * 8))); }
static uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

Bytes encode_ground_layers(const GroundLayers& g) {
    Bytes b;
    const int n = g.count(), S = kGroundTextureSize;
    put32(b, kMagic); put32(b, 1); put32(b, uint32_t(n)); put32(b, uint32_t(g.w)); put32(b, uint32_t(g.h));
    put32(b, n > 2 ? uint32_t(S) : 0u); put32(b, uint32_t(g.map_w)); put32(b, uint32_t(g.map_h));
    for (int i = 0; i < n; i++) {
        uint8_t rec[128] = {};
        float r = g.layers[size_t(i)].repeats;
        std::memcpy(rec, &r, 4);
        uint32_t kind = i >= 2;
        std::memcpy(rec + 4, &kind, 4);
        std::string src = g.layers[size_t(i)].archive + "|" + g.layers[size_t(i)].path;
        std::memcpy(rec + 8, src.data(), std::min<size_t>(src.size(), 119));
        b.insert(b.end(), rec, rec + 128);
    }
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w;) {
            const uint8_t* t = g.texel(x, y);
            int run = 1;
            while (x + run < g.w && run < 65535 && !std::memcmp(g.texel(x + run, y), t, 8)) run++;
            b.push_back(uint8_t(run)); b.push_back(uint8_t(run >> 8));
            b.insert(b.end(), t, t + 8);
            x += run;
        }
    for (int i = 2; i < n; i++) {
        Image im = g.layers[size_t(i)].image.w == S && g.layers[size_t(i)].image.h == S ? g.layers[size_t(i)].image
                                                                                        : resample_square(g.layers[size_t(i)].image, S);
        if (im.px.size() != size_t(S) * S) im.px.assign(size_t(S) * S, 0xFF808080u);
        for (uint32_t p : im.px) put32(b, p);  // R in the low byte = RGBA8 in memory
    }
    return b;
}

bool decode_ground_layers(const Bytes& b, GroundLayers& g, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (b.size() < 32 || get32(b.data()) != kMagic || get32(b.data() + 4) != 1) return fail("not a terrain layers file");
    uint32_t n = get32(b.data() + 8), w = get32(b.data() + 12), h = get32(b.data() + 16), S = get32(b.data() + 20);
    if (n < 2 || n > kGroundMaxLayers || !w || !h || w > 4096 || h > 4096 || (n > 2 && (!S || S > 2048))) return fail("bad terrain layers header");
    size_t pos = 32;
    if (pos + size_t(n) * 128 > b.size()) return fail("terrain layers truncated");
    GroundLayers out;
    out.enabled = true;
    out.w = int(w); out.h = int(h);
    out.map_w = get32(b.data() + 24) ? int(get32(b.data() + 24)) : 256;
    out.map_h = get32(b.data() + 28) ? int(get32(b.data() + 28)) : out.map_w;
    for (uint32_t i = 0; i < n; i++, pos += 128) {
        GroundLayer L;
        std::memcpy(&L.repeats, b.data() + pos, 4);
        const char* src = reinterpret_cast<const char*>(b.data() + pos + 8);
        std::string s(src, strnlen(src, 120));
        size_t bar = s.find('|');
        if (bar != std::string::npos) { L.archive = s.substr(0, bar); L.path = s.substr(bar + 1); }
        out.layers.push_back(std::move(L));
    }
    out.weights = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 8, 0);
    uint8_t* d = out.weights->data();
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w;) {
            if (pos + 10 > b.size()) return fail("terrain layers weights truncated");
            uint32_t run = uint32_t(b[pos]) | uint32_t(b[pos + 1]) << 8;
            if (!run || x + run > w) return fail("bad weight run");
            for (uint32_t k = 0; k < run; k++) std::memcpy(d + (size_t(y) * w + x + k) * 8, b.data() + pos + 2, 8);
            x += run; pos += 10;
        }
    for (uint32_t i = 2; i < n; i++) {
        if (pos + size_t(S) * S * 4 > b.size()) return fail("terrain layer textures truncated");
        Image& im = out.layers[i].image;
        im.w = im.h = int(S);
        im.px.resize(size_t(S) * S);
        std::memcpy(im.px.data(), b.data() + pos, size_t(S) * S * 4);
        pos += size_t(S) * S * 4;
    }
    g = std::move(out);
    return true;
}

Image resample_square(const Image& src, int size) {
    Image out;
    out.w = out.h = size;
    out.px.assign(size_t(size) * size, 0xFF808080u);
    if (src.px.empty()) return out;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            double acc[4] = {};
            if (src.w >= size && src.h >= size) {
                int x0 = x * src.w / size, x1 = (x + 1) * src.w / size, y0 = y * src.h / size, y1 = (y + 1) * src.h / size;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++)
                        for (int c = 0; c < 4; c++) acc[c] += (src.at(xx, yy) >> (c * 8)) & 255;
                double k = 1.0 / double((x1 - x0) * (y1 - y0));
                for (double& v : acc) v *= k;
            } else {
                // Wrapping bilinear (ground textures tile).
                float fx = (x + 0.5f) * src.w / size - 0.5f, fy = (y + 0.5f) * src.h / size - 0.5f;
                int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                float tx = fx - x0, ty = fy - y0;
                auto px = [&](int a, int b2) { return src.at(((a % src.w) + src.w) % src.w, ((b2 % src.h) + src.h) % src.h); };
                uint32_t q[4] = {px(x0, y0), px(x0 + 1, y0), px(x0, y0 + 1), px(x0 + 1, y0 + 1)};
                float wq[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                for (int c = 0; c < 4; c++)
                    for (int k = 0; k < 4; k++) acc[c] += wq[k] * float((q[k] >> (c * 8)) & 255);
            }
            uint32_t p = 0;
            for (int c = 0; c < 4; c++) p |= uint32_t(std::clamp(int(std::lround(acc[c])), 0, 255)) << (c * 8);
            out.at(x, y) = p;
        }
    return out;
}

bool load_ground_texture(MgfReader& a, const std::string& path, Image& out, std::string* err) {
    Bytes b;
    Mgic m;
    Picture p;
    Image img;
    if (!a.read_file(path, b, err) || !m.parse(b, err) || !read_picture(m, p, err) || !decode_picture(p, 0, img, err)) return false;
    out = resample_square(img, kGroundTextureSize);
    return true;
}

}  // namespace ma2
