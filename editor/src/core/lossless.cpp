#include "lossless.h"

#include <cstring>

namespace ma2 {

namespace {

constexpr uint32_t kMagic = 0x4332414Du;  // 'MA2C'
constexpr uint32_t kVersion = 1;
constexpr int kMaxSide = 16384;

void put32(Bytes& o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back(uint8_t(v >> (8 * i))); }
void put64(Bytes& o, uint64_t v) { for (int i = 0; i < 8; i++) o.push_back(uint8_t(v >> (8 * i))); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

// Planes R, G, B, A with the row-delta prediction applied.
Bytes predict(const Image& img) {
    const size_t n = size_t(img.w) * size_t(img.h);
    Bytes s(n * 4);
    for (int ch = 0; ch < 4; ch++) {
        uint8_t* d = s.data() + size_t(ch) * n;
        auto at = [&](int x, int y) { return uint8_t(img.px[size_t(y) * img.w + x] >> (8 * ch)); };
        for (int y = 0; y < img.h; y++) {
            d[size_t(y) * img.w] = uint8_t(at(0, y) - (y ? at(0, y - 1) : 0));
            for (int x = 1; x < img.w; x++) d[size_t(y) * img.w + x] = uint8_t(at(x, y) - at(x - 1, y));
        }
    }
    return s;
}

void unpredict(const Bytes& s, Image& img) {
    const size_t n = size_t(img.w) * size_t(img.h);
    img.px.assign(n, 0);
    for (int ch = 0; ch < 4; ch++) {
        const uint8_t* d = s.data() + size_t(ch) * n;
        uint8_t first = 0;
        for (int y = 0; y < img.h; y++) {
            uint8_t v = first = uint8_t(first + d[size_t(y) * img.w]);
            img.px[size_t(y) * img.w] |= uint32_t(v) << (8 * ch);
            for (int x = 1; x < img.w; x++) {
                v = uint8_t(v + d[size_t(y) * img.w + x]);
                img.px[size_t(y) * img.w + x] |= uint32_t(v) << (8 * ch);
            }
        }
    }
}

// PackBits: 0..127 = n+1 literal bytes, 129..254 = the next byte repeated 257-n times (2..128), 128 unused,
// 255 = long run: LEB128 (count - 3), then the byte.
Bytes pack_bits(const Bytes& in) {
    Bytes o;
    size_t i = 0;
    std::vector<uint8_t> lit;
    auto flush = [&] {
        for (size_t k = 0; k < lit.size(); k += 128) {
            size_t c = std::min<size_t>(128, lit.size() - k);
            o.push_back(uint8_t(c - 1));
            o.insert(o.end(), lit.begin() + long(k), lit.begin() + long(k + c));
        }
        lit.clear();
    };
    while (i < in.size()) {
        size_t r = 1;
        while (i + r < in.size() && in[i + r] == in[i]) r++;
        if (r >= 3) {
            flush();
            if (r <= 128) { o.push_back(uint8_t(257 - r)); o.push_back(in[i]); }
            else {
                o.push_back(255);
                for (uint64_t v = r - 3; ; v >>= 7) { o.push_back(uint8_t((v & 0x7F) | (v > 0x7F ? 0x80 : 0))); if (v <= 0x7F) break; }
                o.push_back(in[i]);
            }
            i += r;
        } else {
            for (size_t k = 0; k < r; k++) lit.push_back(in[i]);
            i += r;
        }
    }
    flush();
    return o;
}

bool unpack_bits(const uint8_t* p, size_t n, size_t want, Bytes& out) {
    out.clear();
    out.reserve(want);
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i++];
        if (c < 128) {
            size_t k = size_t(c) + 1;
            if (i + k > n || out.size() + k > want) return false;
            out.insert(out.end(), p + i, p + i + k);
            i += k;
        } else if (c == 128) {
            return false;
        } else {
            size_t k;
            if (c == 255) {
                uint64_t v = 0;
                int sh = 0;
                for (;;) {
                    if (i >= n || sh > 35) return false;
                    uint8_t b = p[i++];
                    v |= uint64_t(b & 0x7F) << sh;
                    sh += 7;
                    if (!(b & 0x80)) break;
                }
                k = size_t(v) + 3;
            } else {
                k = 257 - size_t(c);
            }
            if (i >= n || out.size() + k > want) return false;
            out.insert(out.end(), k, p[i++]);
        }
    }
    return out.size() == want;
}

bool valid_size(int w, int h) { return w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide; }

}  // namespace

uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

Bytes pack_image(const Image& img) {
    Bytes o;
    put32(o, uint32_t(img.w));
    put32(o, uint32_t(img.h));
    if (!valid_size(img.w, img.h) || img.px.size() != size_t(img.w) * size_t(img.h)) { put32(o, 0); return o; }
    Bytes pk = pack_bits(predict(img));
    put32(o, uint32_t(pk.size()));
    o.insert(o.end(), pk.begin(), pk.end());
    return o;
}

static bool unpack_payload(const uint8_t* p, size_t n, int w, int h, Image& out) {
    if (!valid_size(w, h)) return false;
    Bytes s;
    if (!unpack_bits(p, n, size_t(w) * size_t(h) * 4, s)) return false;
    out.w = w;
    out.h = h;
    unpredict(s, out);
    return true;
}

bool unpack_image(const Bytes& b, Image& out) {
    if (b.size() < 12) return false;
    const uint32_t w = get32(b.data()), h = get32(b.data() + 4), sz = get32(b.data() + 8);
    if (w > uint32_t(kMaxSide) || h > uint32_t(kMaxSide) || size_t(sz) + 12 != b.size()) return false;
    Image t;
    if (!unpack_payload(b.data() + 12, sz, int(w), int(h), t)) return false;
    out = std::move(t);
    return true;
}

Bytes colormap_store(const Image& img, uint64_t tif_hash) {
    Bytes o;
    put32(o, kMagic);
    put32(o, kVersion);
    put32(o, uint32_t(img.w));
    put32(o, uint32_t(img.h));
    put64(o, tif_hash);
    Bytes pk;
    if (valid_size(img.w, img.h) && img.px.size() == size_t(img.w) * size_t(img.h)) pk = pack_bits(predict(img));
    put32(o, uint32_t(pk.size()));
    o.insert(o.end(), pk.begin(), pk.end());
    return o;
}

bool colormap_restore(const Bytes& b, uint64_t tif_hash, Image& out) {
    if (b.size() < 28 || get32(b.data()) != kMagic || get32(b.data() + 4) != kVersion) return false;
    const uint32_t w = get32(b.data() + 8), h = get32(b.data() + 12), sz = get32(b.data() + 24);
    if (get64(b.data() + 16) != tif_hash || size_t(sz) + 28 != b.size() || w > uint32_t(kMaxSide) || h > uint32_t(kMaxSide)) return false;
    Image t;
    if (!unpack_payload(b.data() + 28, sz, int(w), int(h), t)) return false;
    out = std::move(t);
    return true;
}

}  // namespace ma2
