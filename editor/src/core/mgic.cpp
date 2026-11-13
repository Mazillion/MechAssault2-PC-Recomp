#include "mgic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ma2 {

static const uint32_t T_PIC = fourcc("PIC "), T_PAL = fourcc("PAL "), T_VER = fourcc("ver "), T_FLGS = fourcc("flgs"),
                      T_WDTH = fourcc("wdth"), T_HGT = fourcc("hgt "), T_MIPS = fourcc("mips"), T_SIZE = fourcc("size"),
                      T_FRMS = fourcc("frms"), T_DPTH = fourcc("dpth"), T_BITS = fourcc("bits"), T_RGB = fourcc("rgb "),
                      T_NCLR = fourcc("nclr");

MgicChunk* MgicSection::chunk(uint32_t tag) {
    for (auto& c : chunks) if (c.tag == tag) return &c;
    return nullptr;
}
const MgicChunk* MgicSection::chunk(uint32_t tag) const { return const_cast<MgicSection*>(this)->chunk(tag); }
uint32_t MgicSection::u32(uint32_t tag, uint32_t def) const {
    const MgicChunk* c = chunk(tag);
    if (!c || c->data.empty()) return def;
    uint32_t v = 0;
    std::memcpy(&v, c->data.data(), std::min<size_t>(4, c->data.size()));
    return v;
}
void MgicSection::set_u32(uint32_t tag, uint32_t v) {
    MgicChunk* c = chunk(tag);
    if (!c) { chunks.push_back({tag, {}}); c = &chunks.back(); }
    c->data.resize(4);
    wr<uint32_t>(c->data.data(), v);
}

MgicSection* Mgic::section(uint32_t tag) {
    for (auto& s : sections) if (s.tag == tag) return &s;
    return nullptr;
}
const MgicSection* Mgic::section(uint32_t tag) const { return const_cast<Mgic*>(this)->section(tag); }

bool Mgic::parse(const uint8_t* d, size_t n, std::string* err) {
    sections.clear();
    tail.clear();
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    if (n < 0x20 || std::memcmp(d + 4, "MGIc", 4)) return fail("not an MGIc picture");
    std::memcpy(head, d, 0x20);
    size_t pos = 0x20;
    uint32_t count = rd<uint32_t>(d + 12);
    for (uint32_t s = 0; s < count && pos + 16 <= n; s++) {
        MgicSection sec;
        sec.tag = rd<uint32_t>(d + pos);
        sec.zero = rd<uint32_t>(d + pos + 4);
        uint32_t len = rd<uint32_t>(d + pos + 8), nchunks = rd<uint32_t>(d + pos + 12);
        if (len < 16 || pos + len > n) return fail("bad MGIc section");
        size_t end = pos + len, c = pos + 16;
        for (uint32_t k = 0; k < nchunks; k++) {
            if (c + 8 > end) return fail("bad MGIc chunk header");
            uint32_t tag = rd<uint32_t>(d + c), clen = rd<uint32_t>(d + c + 4);
            if (clen < 8 || c + clen > end) return fail("bad MGIc chunk");
            sec.chunks.push_back({tag, Bytes(d + c + 8, d + c + clen)});
            c += clen;
        }
        sec.trailing.assign(d + c, d + end);
        sections.push_back(std::move(sec));
        pos = end;
    }
    tail.assign(d + pos, d + n);
    return true;
}

Bytes Mgic::serialize() const {
    Bytes out(head, head + 0x20);
    for (auto& s : sections) {
        size_t start = out.size();
        put(out, s.tag);
        put(out, s.zero);
        put(out, uint32_t(0));
        put(out, uint32_t(s.chunks.size()));
        for (auto& c : s.chunks) {
            put(out, c.tag);
            put(out, uint32_t(c.data.size() + 8));
            put_bytes(out, c.data.data(), c.data.size());
        }
        put_bytes(out, s.trailing.data(), s.trailing.size());
        wr<uint32_t>(&out[start + 8], uint32_t(out.size() - start));
    }
    put_bytes(out, tail.data(), tail.size());
    wr<uint32_t>(&out[8], uint32_t(out.size()));
    wr<uint32_t>(&out[12], uint32_t(sections.size()));
    return out;
}

// ---------------------------------------------------------------------------------------------

static int bytes_per_pixel(int fmt) {
    switch (fmt) {
        case PF_ARGB8888: return 4;
        case PF_RGB565: case PF_ARGB4444: case PF_A8L8: return 2;
        case PF_P8: case PF_A8: return 1;
        default: return 0;
    }
}

std::string Picture::format_name() const {
    std::string s;
    if (dxt()) s = dxt() == 1 ? "DXT1" : dxt() == 3 ? "DXT3" : dxt() == 5 ? "DXT5" : "DXT?";
    else switch (format()) {
        case PF_ARGB8888: s = "A8R8G8B8"; break; case PF_RGB565: s = "R5G6B5"; break;
        case PF_ARGB4444: s = "A4R4G4B4"; break; case PF_P8: s = "P8"; break;
        case PF_A8: s = "A8"; break; case PF_A8L8: s = "A8L8"; break; default: s = "fmt" + std::to_string(format());
    }
    if (swizzled()) s += " swizzled";
    if (row_align > 1) s += " pitch" + std::to_string(row_align);
    return s;
}

uint32_t Picture::pitch(int level) const {
    uint32_t w = std::max(1u, width >> level);
    uint32_t row = w * uint32_t(bytes_per_pixel(format()));
    return (row + uint32_t(row_align) - 1) / uint32_t(row_align) * uint32_t(row_align);
}

size_t Picture::level_size(int level) const {
    uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level), d = std::max(1u, depth >> level);
    if (dxt()) return size_t((w + 3) / 4) * ((h + 3) / 4) * d * (dxt() == 1 ? 8 : 16);
    return size_t(pitch(level)) * h * d;
}

size_t Picture::level_offset(int level) const {
    size_t off = 0;
    for (int l = 0; l < level; l++) off += level_size(l);
    return off;
}

bool read_picture(const Mgic& m, Picture& p, std::string* err) {
    const MgicSection* pic = m.section(T_PIC);
    if (!pic) { if (err) *err = "no PIC section"; return false; }
    p = Picture();
    p.flags = pic->u32(T_FLGS);
    p.width = pic->u32(T_WDTH);
    p.height = pic->u32(T_HGT);
    p.mips = std::max(1u, pic->u32(T_MIPS, 1));
    p.frames = std::max(1u, pic->u32(T_FRMS, 1));
    p.depth = std::max(1u, pic->u32(T_DPTH, 1));
    const MgicChunk* bits = pic->chunk(T_BITS);
    if (!bits || !p.width || !p.height || p.width > 16384 || p.height > 16384) { if (err) *err = "bad PIC"; return false; }
    p.bits = bits->data;
    if (!p.dxt() && !bytes_per_pixel(p.format())) { if (err) *err = "unsupported pixel format"; return false; }
    if (!p.dxt() && !p.swizzled()) {
        auto fits = [&](size_t expected) { return expected == p.bits.size() || ((expected + 15) & ~size_t(15)) == p.bits.size(); };
        if (!fits(p.level_offset(int(p.mips)) * p.frames)) {
            p.row_align = 64;
            if (!fits(p.level_offset(int(p.mips)) * p.frames)) { p.row_align = 1; if (err) *err = "unsupported row pitch layout"; return false; }
        }
    }
    if (const MgicSection* pal = m.section(T_PAL)) {
        if (const MgicChunk* rgb = pal->chunk(T_RGB)) {
            // 'rgb ' payload: u32 count/flags? then entries. Entries are B,G,R,A.
            size_t n = pal->u32(T_NCLR, 0);
            size_t start = rgb->data.size() >= n * 4 + 4 ? rgb->data.size() - n * 4 : 0;
            for (size_t i = 0; i < n && start + i * 4 + 3 < rgb->data.size(); i++) {
                const uint8_t* e = &rgb->data[start + i * 4];
                p.palette.push_back(uint32_t(e[2]) | uint32_t(e[1]) << 8 | uint32_t(e[0]) << 16 | uint32_t(e[3]) << 24);
            }
        }
    }
    if (p.level_offset(1) > p.bits.size()) { if (err) *err = "short pixel payload"; return false; }
    return true;
}

bool read_picture(const Bytes& file, Picture& p, std::string* err) {
    Mgic m;
    if (!m.parse(file, err)) return false;
    return read_picture(m, p, err);
}

uint32_t swizzle_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t result = 0, pos = 0;
    for (uint32_t bit = 1; bit < std::max(w, h); bit <<= 1) {
        if (bit < w) { if (x & bit) result |= 1u << pos; pos++; }
        if (bit < h) { if (y & bit) result |= 1u << pos; pos++; }
    }
    return result;
}

static inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

static uint32_t expand565(uint16_t v) {
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}

void dxt_decode_block(int dxt, const uint8_t* blk, uint32_t out[16]) {
    const uint8_t* cb = dxt == 1 ? blk : blk + 8;
    uint16_t c0 = rd<uint16_t>(cb), c1 = rd<uint16_t>(cb + 2);
    uint32_t pal[4];
    pal[0] = expand565(c0);
    pal[1] = expand565(c1);
    auto mix = [](uint32_t a, uint32_t b, int wa, int wb, int div) {
        uint32_t r = 0;
        for (int s = 0; s < 24; s += 8) r |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / div) << s;
        return r | 0xFF000000u;
    };
    if (dxt != 1 || c0 > c1) {
        pal[2] = mix(pal[0], pal[1], 2, 1, 3);
        pal[3] = mix(pal[0], pal[1], 1, 2, 3);
    } else {
        pal[2] = mix(pal[0], pal[1], 1, 1, 2);
        pal[3] = 0;
    }
    uint32_t idx = rd<uint32_t>(cb + 4);
    for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (2 * i)) & 3];
    if (dxt == 3) {
        for (int i = 0; i < 16; i++) {
            uint32_t a = (blk[i / 2] >> ((i & 1) * 4)) & 15;
            out[i] = (out[i] & 0xFFFFFFu) | ((a * 17) << 24);
        }
    } else if (dxt == 5) {
        uint32_t a0 = blk[0], a1 = blk[1], av[8];
        av[0] = a0; av[1] = a1;
        if (a0 > a1) for (int k = 1; k < 7; k++) av[k + 1] = ((7 - k) * a0 + k * a1) / 7;
        else { for (int k = 1; k < 5; k++) av[k + 1] = ((5 - k) * a0 + k * a1) / 5; av[6] = 0; av[7] = 255; }
        uint64_t bits = 0;
        for (int k = 0; k < 6; k++) bits |= uint64_t(blk[2 + k]) << (8 * k);
        for (int i = 0; i < 16; i++) out[i] = (out[i] & 0xFFFFFFu) | (av[(bits >> (3 * i)) & 7] << 24);
    }
}

bool decode_picture(const Picture& p, int level, Image& out, std::string* err) {
    if (level < 0 || uint32_t(level) >= p.mips) { if (err) *err = "no such level"; return false; }
    uint32_t w = std::max(1u, p.width >> level), h = std::max(1u, p.height >> level);
    size_t off = p.level_offset(level);
    if (off + p.level_size(level) > p.bits.size()) { if (err) *err = "short payload"; return false; }
    const uint8_t* src = p.bits.data() + off;
    out.w = int(w); out.h = int(h);
    out.px.assign(size_t(w) * h, 0);
    if (p.dxt()) {
        int bs = p.dxt() == 1 ? 8 : 16;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, blk[16];
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                dxt_decode_block(p.dxt(), src + (size_t(by) * bw + bx) * bs, blk);
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) out.px[size_t(y) * w + x] = blk[i];
                }
            }
        return true;
    }
    int bpp = bytes_per_pixel(p.format());
    uint32_t pitch = p.pitch(level);
    bool pow2 = !(w & (w - 1)) && !(h & (h - 1));
    if (p.swizzled() && !pow2) { if (err) *err = "swizzled non power of two"; return false; }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* s = p.swizzled() ? src + size_t(swizzle_index(x, y, w, h)) * bpp : src + size_t(y) * pitch + size_t(x) * bpp;
            uint32_t c = 0;
            switch (p.format()) {
                case PF_ARGB8888: c = rgba(s[2], s[1], s[0], s[3]); break;
                case PF_RGB565: c = expand565(rd<uint16_t>(s)); break;
                case PF_ARGB4444: {
                    uint16_t v = rd<uint16_t>(s);
                    c = rgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17);
                    break;
                }
                case PF_P8:
                    c = s[0] < p.palette.size() && p.palette[s[0]] ? p.palette[s[0]] : rgba(s[0], s[0], s[0], 255);
                    break;
                case PF_A8: c = rgba(255, 255, 255, s[0]); break;
                case PF_A8L8: c = rgba(s[0], s[0], s[0], s[1]); break;
            }
            out.px[size_t(y) * w + x] = c;
        }
    return true;
}

bool picture_samples8(const Picture& p, std::vector<uint8_t>& out) {
    if (p.dxt() || bytes_per_pixel(p.format()) != 1 || p.swizzled()) return false;
    uint32_t pitch = p.pitch(0);
    if (size_t(pitch) * p.height > p.bits.size()) return false;
    out.resize(size_t(p.width) * p.height);
    for (uint32_t y = 0; y < p.height; y++) std::memcpy(&out[size_t(y) * p.width], &p.bits[size_t(y) * pitch], p.width);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Encoding

static uint16_t to565(float r, float g, float b) {
    int R = std::clamp(int(std::lround(r * 31.f / 255.f)), 0, 31);
    int G = std::clamp(int(std::lround(g * 63.f / 255.f)), 0, 63);
    int B = std::clamp(int(std::lround(b * 31.f / 255.f)), 0, 31);
    return uint16_t(R << 11 | G << 5 | B);
}

static void encode_color_block(const uint32_t in[16], uint8_t* out, bool dxt1_alpha) {
    float px[16][3];
    bool transparent[16] = {};
    float mean[3] = {0, 0, 0};
    int used = 0;
    for (int i = 0; i < 16; i++) {
        px[i][0] = float(in[i] & 255); px[i][1] = float((in[i] >> 8) & 255); px[i][2] = float((in[i] >> 16) & 255);
        transparent[i] = dxt1_alpha && ((in[i] >> 24) < 128);
        if (transparent[i]) continue;
        for (int c = 0; c < 3; c++) mean[c] += px[i][c];
        used++;
    }
    if (!used) {  // fully transparent DXT1 block
        wr<uint16_t>(out, 0); wr<uint16_t>(out + 2, 0); wr<uint32_t>(out + 4, 0xFFFFFFFFu);
        return;
    }
    for (int c = 0; c < 3; c++) mean[c] /= float(used);
    float cov[6] = {};
    for (int i = 0; i < 16; i++) {
        if (transparent[i]) continue;
        float d0 = px[i][0] - mean[0], d1 = px[i][1] - mean[1], d2 = px[i][2] - mean[2];
        cov[0] += d0 * d0; cov[1] += d0 * d1; cov[2] += d0 * d2; cov[3] += d1 * d1; cov[4] += d1 * d2; cov[5] += d2 * d2;
    }
    float axis[3] = {0.577f, 0.577f, 0.577f};
    for (int it = 0; it < 8; it++) {
        float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        float len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-6f) break;
        axis[0] = x / len; axis[1] = y / len; axis[2] = z / len;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; i++) {
        if (transparent[i]) continue;
        float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t); hi = std::max(hi, t);
    }
    float inset = (hi - lo) / 32.f;
    lo += inset; hi -= inset;
    float e0[3], e1[3];
    for (int c = 0; c < 3; c++) { e0[c] = std::clamp(mean[c] + axis[c] * hi, 0.f, 255.f); e1[c] = std::clamp(mean[c] + axis[c] * lo, 0.f, 255.f); }
    uint16_t c0 = to565(e0[0], e0[1], e0[2]), c1 = to565(e1[0], e1[1], e1[2]);
    bool three = false;
    for (int i = 0; i < 16; i++) three |= transparent[i];
    if (three) { if (c0 > c1) std::swap(c0, c1); }  // c0 <= c1: 3-colour + transparent
    else {
        if (c0 < c1) std::swap(c0, c1);
        if (c0 == c1) {  // flat block
            wr<uint16_t>(out, c0); wr<uint16_t>(out + 2, c1); wr<uint32_t>(out + 4, 0);
            return;
        }
    }
    uint32_t pal[4];
    pal[0] = expand565(c0); pal[1] = expand565(c1);
    auto mix = [](uint32_t a, uint32_t b, int wa, int wb, int div) {
        uint32_t r = 0;
        for (int s = 0; s < 24; s += 8) r |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / div) << s;
        return r;
    };
    int ncol = three ? 3 : 4;
    if (three) pal[2] = mix(pal[0], pal[1], 1, 1, 2);
    else { pal[2] = mix(pal[0], pal[1], 2, 1, 3); pal[3] = mix(pal[0], pal[1], 1, 2, 3); }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        if (transparent[i]) best = 3;
        else {
            float bd = 1e30f;
            for (int k = 0; k < ncol; k++) {
                float dr = px[i][0] - float(pal[k] & 255), dg = px[i][1] - float((pal[k] >> 8) & 255), db = px[i][2] - float((pal[k] >> 16) & 255);
                float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
                if (d < bd) { bd = d; best = k; }
            }
        }
        idx |= uint32_t(best) << (2 * i);
    }
    wr<uint16_t>(out, c0); wr<uint16_t>(out + 2, c1); wr<uint32_t>(out + 4, idx);
}

void dxt_encode_block(int dxt, const uint32_t in[16], uint8_t* blk) {
    if (dxt == 1) { encode_color_block(in, blk, true); return; }
    encode_color_block(in, blk + 8, false);
    if (dxt == 3) {
        std::memset(blk, 0, 8);
        for (int i = 0; i < 16; i++) {
            uint32_t a = ((in[i] >> 24) * 15 + 127) / 255;
            blk[i / 2] |= uint8_t(a << ((i & 1) * 4));
        }
        return;
    }
    uint32_t amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) { uint32_t a = in[i] >> 24; amin = std::min(amin, a); amax = std::max(amax, a); }
    blk[0] = uint8_t(amax); blk[1] = uint8_t(amin);
    uint32_t av[8];
    av[0] = amax; av[1] = amin;
    for (int k = 1; k < 7; k++) av[k + 1] = ((7 - k) * amax + k * amin) / 7;
    uint64_t bits = 0;
    if (amax > amin) {
        for (int i = 0; i < 16; i++) {
            uint32_t a = in[i] >> 24;
            int best = 0, bd = 1 << 30;
            for (int k = 0; k < 8; k++) { int d = std::abs(int(a) - int(av[k])); if (d < bd) { bd = d; best = k; } }
            bits |= uint64_t(best) << (3 * i);
        }
    }
    for (int k = 0; k < 6; k++) blk[2 + k] = uint8_t(bits >> (8 * k));
}

static Image half(const Image& s) {
    Image d;
    d.w = std::max(1, s.w / 2); d.h = std::max(1, s.h / 2);
    d.px.resize(size_t(d.w) * d.h);
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++) {
            uint32_t acc[4] = {};
            int n = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int sx = std::min(s.w - 1, x * 2 + dx), sy = std::min(s.h - 1, y * 2 + dy);
                    uint32_t c = s.at(sx, sy);
                    for (int k = 0; k < 4; k++) acc[k] += (c >> (8 * k)) & 255;
                    n++;
                }
            uint32_t c = 0;
            for (int k = 0; k < 4; k++) c |= ((acc[k] + n / 2) / n) << (8 * k);
            d.at(x, y) = c;
        }
    return d;
}

static void encode_level(const Picture& p, int level, const Image& img, uint8_t* dst) {
    uint32_t w = uint32_t(img.w), h = uint32_t(img.h);
    if (p.dxt()) {
        int bs = p.dxt() == 1 ? 8 : 16;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, blk[16];
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                for (int i = 0; i < 16; i++) {
                    uint32_t x = std::min(w - 1, bx * 4 + (i & 3)), y = std::min(h - 1, by * 4 + (i >> 2));
                    blk[i] = img.px[size_t(y) * w + x];
                }
                dxt_encode_block(p.dxt(), blk, dst + (size_t(by) * bw + bx) * bs);
            }
        return;
    }
    int bpp = bytes_per_pixel(p.format());
    uint32_t pitch = p.pitch(level);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* d = p.swizzled() ? dst + size_t(swizzle_index(x, y, w, h)) * bpp : dst + size_t(y) * pitch + size_t(x) * bpp;
            uint32_t c = img.px[size_t(y) * w + x];
            uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
            switch (p.format()) {
                case PF_ARGB8888: d[0] = uint8_t(b); d[1] = uint8_t(g); d[2] = uint8_t(r); d[3] = uint8_t(a); break;
                case PF_RGB565: wr<uint16_t>(d, to565(float(r), float(g), float(b))); break;
                case PF_ARGB4444:
                    wr<uint16_t>(d, uint16_t(((a * 15 + 127) / 255) << 12 | ((r * 15 + 127) / 255) << 8 | ((g * 15 + 127) / 255) << 4 | ((b * 15 + 127) / 255)));
                    break;
                case PF_P8: d[0] = uint8_t(r); break;
                case PF_A8: d[0] = uint8_t(a); break;
                case PF_A8L8: d[0] = uint8_t(r); d[1] = uint8_t(a); break;
            }
        }
}

Bytes encode_levels(const Picture& p, const Image& top) {
    // Retail payloads are padded to a multiple of 16 bytes (a 9-level 256x256 A8L8 chain is
    // 174,762 bytes of levels in a 174,768-byte payload).
    Bytes out((p.level_offset(int(p.mips)) + 15) & ~size_t(15), 0);
    Image cur = top;
    for (uint32_t l = 0; l < p.mips; l++) {
        if (l) cur = half(cur);
        encode_level(p, int(l), cur, out.data() + p.level_offset(int(l)));
    }
    return out;
}

Bytes build_picture(const Mgic& style, uint32_t flags, uint32_t width, uint32_t height, uint32_t mips,
                    const Bytes& bits, int row_align) {
    (void)row_align;
    Mgic m = style;
    MgicSection* pic = m.section(T_PIC);
    if (!pic) {
        MgicSection s;
        s.tag = T_PIC;
        s.chunks.push_back({T_VER, Bytes{4}});
        m.sections.insert(m.sections.begin(), s);
        pic = &m.sections.front();
        static const uint8_t hd[16] = {0, 0, 1, 0, 'M', 'G', 'I', 'c', 0, 0, 0, 0, 1, 0, 0, 0};
        std::memcpy(m.head, hd, 16);
    }
    std::vector<MgicChunk> chunks;
    const MgicChunk* ver = pic->chunk(T_VER);
    chunks.push_back(ver ? *ver : MgicChunk{T_VER, Bytes{4}});
    auto u = [](uint32_t tag, uint32_t v) { MgicChunk c{tag, Bytes(4)}; wr<uint32_t>(c.data.data(), v); return c; };
    chunks.push_back(u(T_FLGS, flags));
    chunks.push_back(u(T_WDTH, width));
    chunks.push_back(u(T_HGT, height));
    chunks.push_back(u(T_MIPS, mips ? mips : pic->u32(T_MIPS, 1)));
    chunks.push_back(u(T_SIZE, uint32_t(bits.size())));
    chunks.push_back(u(T_FRMS, 1));
    chunks.push_back(u(T_DPTH, 1));
    chunks.push_back({T_BITS, bits});
    pic->chunks = std::move(chunks);
    pic->trailing.clear();
    return m.serialize();
}

Bytes build_heightfield(const Mgic& style, uint32_t width, uint32_t height, const std::vector<uint8_t>& samples) {
    uint32_t pitch = (width + 63) / 64 * 64;
    Bytes bits(size_t(pitch) * height, 0);
    for (uint32_t y = 0; y < height; y++) std::memcpy(&bits[size_t(y) * pitch], &samples[size_t(y) * width], width);
    const MgicSection* pic = style.section(T_PIC);
    uint32_t flags = pic ? pic->u32(T_FLGS, PF_P8) : PF_P8;
    return build_picture(style, flags, width, height, 1, bits, 64);
}

}  // namespace ma2
