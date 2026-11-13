// MGIc picture (.tif) decoding to RGBA8.
//
// Layout: 32-byte MGIc header, then 'PIC ' (16-byte chunk header) holding
// sub-chunks (tag, length incl. 8-byte header): 'ver ' u8, 'flgs', 'wdth',
// 'hgt ', 'mips', 'size', 'frms', 'dpth' (u32 each) and 'bits' = pixels.
// flgs: bits 0-3 pixel format (0 A8R8G8B8, 1 R5G6B5, 3 A4R4G4B4, 4 L8, 5 A8, 7 A8L8),
// bit 4 swizzled (Xbox Morton order), bits 8-11 compression (1 DXT1, 3 DXT3,
// 5 DXT5), bit 7 cube map, bit 12 volume, bit 31 always set, bit 14 unknown.
// Per frame the mip levels follow each other; frames (and cube faces) are
// rounded up to 128 bytes when that matches the payload size. Linear
// (unswizzled, uncompressed) rows may be padded to a 64-byte pitch.
#include "internal.h"

#include <mutex>
#include <unordered_map>

namespace ma2m {

const char* PixelFormatName(PixelFormat f) {
    switch (f) {
        case PixelFormat::DXT1: return "DXT1";
        case PixelFormat::DXT3: return "DXT3";
        case PixelFormat::DXT5: return "DXT5";
        case PixelFormat::A8R8G8B8: return "A8R8G8B8";
        case PixelFormat::R5G6B5: return "R5G6B5";
        case PixelFormat::A4R4G4B4: return "A4R4G4B4";
        case PixelFormat::A8: return "A8";
        case PixelFormat::A8L8: return "A8L8";
        case PixelFormat::L8: return "L8";
        default: return "unknown";
    }
}

namespace {

struct Header {
    TextureInfo info;
    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
    int row_align = 1;
};

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

int Bpp(PixelFormat f) {
    switch (f) {
        case PixelFormat::A8R8G8B8: return 4;
        case PixelFormat::R5G6B5:
        case PixelFormat::A4R4G4B4:
        case PixelFormat::A8L8: return 2;
        case PixelFormat::A8:
        case PixelFormat::L8: return 1;
        default: return 0;
    }
}

bool Compressed(PixelFormat f) { return f == PixelFormat::DXT1 || f == PixelFormat::DXT3 || f == PixelFormat::DXT5; }

size_t LevelSize(const Header& h, int level) {
    size_t w = std::max(1, h.info.width >> level), hh = std::max(1, h.info.height >> level);
    size_t d = std::max(1, h.info.depth >> level);
    if (Compressed(h.info.format))
        return ((w + 3) / 4) * ((hh + 3) / 4) * d * (h.info.format == PixelFormat::DXT1 ? 8 : 16);
    size_t bpp = Bpp(h.info.format);
    size_t pitch = ((w * bpp + h.row_align - 1) / h.row_align) * h.row_align;
    return pitch * hh * d;
}

size_t FrameBytes(const Header& h) {
    size_t s = 0;
    for (int l = 0; l < h.info.mip_count; l++) s += LevelSize(h, l);
    return s;
}

bool ParseHeader(const uint8_t* data, size_t size, Header& h, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = "picture: " + m;
        return false;
    };
    if (size < 56 || memcmp(data + 4, "MGIc", 4) != 0 || memcmp(data + 32, "PIC ", 4) != 0)
        return fail("not an MGIc picture");
    size_t cur = 48;
    bool have[8] = {false};
    uint32_t vals[8] = {0};
    static const char* tags[8] = {"flgs", "wdth", "hgt ", "mips", "size", "frms", "dpth", "ver "};
    for (;;) {
        if (cur + 8 > size) return fail("missing pixel payload");
        uint32_t len = U32(data + cur + 4);
        if (len < 8 || cur + len > size) return fail("bad chunk");
        if (!memcmp(data + cur, "bits", 4)) {
            h.payload = data + cur + 8;
            h.payload_len = len - 8;
            break;
        }
        for (int t = 0; t < 8; t++) {
            if (!memcmp(data + cur, tags[t], 4)) {
                have[t] = true;
                vals[t] = len >= 12 ? U32(data + cur + 8) : (len > 8 ? data[cur + 8] : 0);
            }
        }
        cur += len;
    }
    if (!have[0] || !have[1] || !have[2]) return fail("missing flgs/wdth/hgt");
    TextureInfo& i = h.info;
    i.flags = vals[0];
    i.width = (int)vals[1];
    i.height = (int)vals[2];
    i.mip_count = have[3] ? (int)vals[3] : 1;
    i.frames = have[5] ? (int)vals[5] : 1;
    i.depth = have[6] ? (int)vals[6] : 1;
    i.payload_bytes = h.payload_len;
    if (i.mip_count < 1) i.mip_count = 1;
    if (i.frames < 1) i.frames = 1;
    if (i.depth < 1) i.depth = 1;
    if (i.width < 1 || i.height < 1 || i.width > 8192 || i.height > 8192 || i.depth > 1024 || i.frames > 256 ||
        i.mip_count > 16)
        return fail("dimensions out of range");
    int comp = (i.flags >> 8) & 15;
    int fmt = i.flags & 15;
    if (comp) {
        i.format = comp == 1 ? PixelFormat::DXT1 : comp == 3 ? PixelFormat::DXT3 : comp == 5 ? PixelFormat::DXT5 : PixelFormat::Unknown;
    } else {
        i.format = fmt == 0 ? PixelFormat::A8R8G8B8 : fmt == 1 ? PixelFormat::R5G6B5 : fmt == 3 ? PixelFormat::A4R4G4B4
                 : fmt == 4 ? PixelFormat::L8 : fmt == 5 ? PixelFormat::A8 : fmt == 7 ? PixelFormat::A8L8
                 : PixelFormat::Unknown;
    }
    if (i.format == PixelFormat::Unknown) {
        char buf[64];
        snprintf(buf, sizeof(buf), "unknown pixel format (flags 0x%08X)", i.flags);
        return fail(buf);
    }
    i.swizzled = !comp && (i.flags & 0x10);
    i.cube = (i.flags & 0x80) != 0;
    i.volume = i.depth > 1;
    // Clamp mip count to what the dimensions allow.
    int maxdim = std::max(std::max(i.width, i.height), i.depth), levels = 1;
    while ((maxdim >> levels) >= 1 && levels < 16) levels++;
    if (i.mip_count > levels) i.mip_count = levels;
    if (!comp && !i.swizzled) {
        h.row_align = 1;
        size_t need = FrameBytes(h) * i.frames;
        if (need != h.payload_len) {
            h.row_align = 64;
            size_t need64 = FrameBytes(h) * i.frames;
            if (need64 > h.payload_len) h.row_align = need <= h.payload_len ? 1 : 64;
        }
    }
    return true;
}

// Xbox swizzle: interleave x, y, z bits (x lowest) while each axis still has bits.
void SwizzleTable(int w, int h, int d, std::vector<uint32_t>& table) {
    table.resize((size_t)w * h * d);
    std::vector<uint32_t> xs(w), ys(h), zs(d);
    int bit_pos = 0;
    for (int bit = 1; bit < std::max(std::max(w, h), d); bit <<= 1) {
        if (bit < w) {
            for (int x = 0; x < w; x++)
                if (x & bit) xs[x] |= 1u << bit_pos;
            bit_pos++;
        }
        if (bit < h) {
            for (int y = 0; y < h; y++)
                if (y & bit) ys[y] |= 1u << bit_pos;
            bit_pos++;
        }
        if (bit < d) {
            for (int z = 0; z < d; z++)
                if (z & bit) zs[z] |= 1u << bit_pos;
            bit_pos++;
        }
    }
    size_t k = 0;
    for (int z = 0; z < d; z++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) table[k++] = xs[x] | ys[y] | zs[z];
}

inline void Expand(PixelFormat f, const uint8_t* s, uint8_t* o) {
    switch (f) {
        case PixelFormat::A8R8G8B8:
            o[0] = s[2];
            o[1] = s[1];
            o[2] = s[0];
            o[3] = s[3];
            break;
        case PixelFormat::R5G6B5: {
            uint16_t v = (uint16_t)(s[0] | (s[1] << 8));
            uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            o[0] = (uint8_t)((r << 3) | (r >> 2));
            o[1] = (uint8_t)((g << 2) | (g >> 4));
            o[2] = (uint8_t)((b << 3) | (b >> 2));
            o[3] = 255;
            break;
        }
        case PixelFormat::A4R4G4B4: {
            uint16_t v = (uint16_t)(s[0] | (s[1] << 8));
            o[0] = (uint8_t)(((v >> 8) & 15) * 17);
            o[1] = (uint8_t)(((v >> 4) & 15) * 17);
            o[2] = (uint8_t)((v & 15) * 17);
            o[3] = (uint8_t)(((v >> 12) & 15) * 17);
            break;
        }
        case PixelFormat::A8:
            o[0] = o[1] = o[2] = 255;
            o[3] = s[0];
            break;
        case PixelFormat::A8L8:
            o[0] = o[1] = o[2] = s[0];
            o[3] = s[1];
            break;
        case PixelFormat::L8:
            o[0] = o[1] = o[2] = s[0];
            o[3] = 255;
            break;
        default:
            o[0] = o[1] = o[2] = o[3] = 0;
    }
}

void Rgb565(uint16_t v, uint8_t* o) {
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    o[0] = (uint8_t)((r << 3) | (r >> 2));
    o[1] = (uint8_t)((g << 2) | (g >> 4));
    o[2] = (uint8_t)((b << 3) | (b >> 2));
    o[3] = 255;
}

// Decodes one 4x4 block to 16 RGBA texels.
void DecodeBlock(PixelFormat f, const uint8_t* b, uint8_t out[64]) {
    const uint8_t* color = f == PixelFormat::DXT1 ? b : b + 8;
    uint16_t c0 = (uint16_t)(color[0] | (color[1] << 8)), c1 = (uint16_t)(color[2] | (color[3] << 8));
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    if (f != PixelFormat::DXT1 || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[3][3] = 0;
    }
    uint32_t bits = U32(color + 4);
    for (int t = 0; t < 16; t++) memcpy(out + t * 4, pal[(bits >> (2 * t)) & 3], 4);
    if (f == PixelFormat::DXT3) {
        for (int t = 0; t < 16; t++) {
            int a = (b[t / 2] >> ((t & 1) * 4)) & 15;
            out[t * 4 + 3] = (uint8_t)(a * 17);
        }
    } else if (f == PixelFormat::DXT5) {
        uint8_t a0 = b[0], a1 = b[1], ap[8];
        ap[0] = a0;
        ap[1] = a1;
        if (a0 > a1) {
            for (int k = 1; k < 7; k++) ap[k + 1] = (uint8_t)(((7 - k) * a0 + k * a1 + 3) / 7);
        } else {
            for (int k = 1; k < 5; k++) ap[k + 1] = (uint8_t)(((5 - k) * a0 + k * a1 + 2) / 5);
            ap[6] = 0;
            ap[7] = 255;
        }
        uint64_t abits = 0;
        for (int k = 0; k < 6; k++) abits |= (uint64_t)b[2 + k] << (8 * k);
        for (int t = 0; t < 16; t++) out[t * 4 + 3] = ap[(abits >> (3 * t)) & 7];
    }
}

// Decodes one level of one frame into `slices` images (depth slices).
bool DecodeLevel(const Header& h, const uint8_t* src, size_t src_len, int level, std::vector<Image>& slices) {
    const TextureInfo& i = h.info;
    int w = std::max(1, i.width >> level), hh = std::max(1, i.height >> level), d = std::max(1, i.depth >> level);
    slices.assign(d, Image());
    for (auto& im : slices) {
        im.width = w;
        im.height = hh;
        im.rgba.assign((size_t)w * hh * 4, 0);
    }
    if (Compressed(i.format)) {
        int bw = (w + 3) / 4, bh = (hh + 3) / 4;
        size_t bb = i.format == PixelFormat::DXT1 ? 8 : 16;
        if ((size_t)bw * bh * d * bb > src_len) return false;
        uint8_t texels[64];
        // Volume DXT: each block row/column position stores up to four Z slices together.
        size_t cursor = 0;
        for (int z0 = 0; z0 < d; z0 += (d > 1 ? 4 : 1)) {
            int group = d > 1 ? std::min(4, d - z0) : 1;
            for (int by = 0; by < bh; by++) {
                for (int bx = 0; bx < bw; bx++) {
                    for (int g = 0; g < group; g++) {
                        const uint8_t* blk = src + cursor + (((size_t)by * bw + bx) * group + g) * bb;
                        DecodeBlock(i.format, blk, texels);
                        Image& im = slices[z0 + g];
                        for (int ty = 0; ty < 4; ty++) {
                            int y = by * 4 + ty;
                            if (y >= hh) break;
                            for (int tx = 0; tx < 4; tx++) {
                                int x = bx * 4 + tx;
                                if (x >= w) break;
                                memcpy(&im.rgba[((size_t)y * w + x) * 4], texels + (ty * 4 + tx) * 4, 4);
                            }
                        }
                    }
                }
            }
            cursor += (size_t)bw * bh * group * bb;
        }
        return true;
    }
    int bpp = Bpp(i.format);
    if (i.swizzled) {
        if ((w & (w - 1)) || (hh & (hh - 1)) || (d & (d - 1))) return false;
        if ((size_t)w * hh * d * bpp > src_len) return false;
        std::vector<uint32_t> table;
        SwizzleTable(w, hh, d, table);
        size_t k = 0;
        for (int z = 0; z < d; z++)
            for (int y = 0; y < hh; y++)
                for (int x = 0; x < w; x++, k++)
                    Expand(i.format, src + (size_t)table[k] * bpp, &slices[z].rgba[((size_t)y * w + x) * 4]);
        return true;
    }
    size_t pitch = (((size_t)w * bpp + h.row_align - 1) / h.row_align) * h.row_align;
    if (pitch * hh * d > src_len) return false;
    for (int z = 0; z < d; z++)
        for (int y = 0; y < hh; y++) {
            const uint8_t* row = src + ((size_t)z * hh + y) * pitch;
            for (int x = 0; x < w; x++) Expand(i.format, row + (size_t)x * bpp, &slices[z].rgba[((size_t)y * w + x) * 4]);
        }
    return true;
}

}  // namespace

bool ReadTextureInfo(const uint8_t* data, size_t size, TextureInfo& out, std::string* error) {
    Header h;
    if (!ParseHeader(data, size, h, error)) return false;
    out = h.info;
    return true;
}

bool DecodeTexture(const uint8_t* data, size_t size, Texture& t, bool all_mips, std::string* error) {
    Header h;
    if (!ParseHeader(data, size, h, error)) return false;
    const TextureInfo& i = h.info;
    t = Texture();
    t.width = i.width;
    t.height = i.height;
    t.depth = i.depth;
    t.frames = i.frames;
    t.mip_count = i.mip_count;
    t.flags = i.flags;
    t.format = i.format;
    t.swizzled = i.swizzled;
    t.cube = i.cube;
    t.volume = i.volume;
    size_t frame_bytes = FrameBytes(h);
    size_t stride = frame_bytes;
    size_t rounded = (frame_bytes + 127) & ~(size_t)127;
    if (i.frames > 1 && rounded * i.frames == h.payload_len) stride = rounded;
    if (frame_bytes > h.payload_len || stride * (i.frames - 1) + LevelSize(h, 0) > h.payload_len) {
        // Tolerate a short mip chain: decode what level 0 allows.
        if (LevelSize(h, 0) * i.frames > h.payload_len) {
            if (error) *error = "picture: payload shorter than level 0";
            return false;
        }
        if (stride * i.frames > h.payload_len) stride = h.payload_len / i.frames;
    }
    t.layers.assign((size_t)i.frames * i.depth, std::vector<Image>());
    int levels = all_mips ? i.mip_count : 1;
    for (int f = 0; f < i.frames; f++) {
        const uint8_t* frame = h.payload + (size_t)f * stride;
        size_t avail = h.payload_len - (size_t)f * stride;
        size_t off = 0;
        for (int l = 0; l < levels; l++) {
            size_t ls = LevelSize(h, l);
            if (off + ls > avail) break;
            std::vector<Image> slices;
            if (!DecodeLevel(h, frame + off, ls, l, slices)) {
                if (l == 0) {
                    if (error) *error = "picture: cannot decode level 0";
                    return false;
                }
                break;
            }
            // Distribute depth slices: slice count shrinks with the level.
            for (int z = 0; z < (int)slices.size(); z++) t.layers[(size_t)f * i.depth + z].push_back(std::move(slices[z]));
            off += ls;
        }
    }
    // Volume slices that vanish at lower mips keep only the levels they have.
    if (const Image* top = t.Top()) {
        bool binary = true, alpha = false;
        for (size_t p = 3; p < top->rgba.size(); p += 4) {
            uint8_t a = top->rgba[p];
            if (a != 255) alpha = true;
            if (a != 0 && a != 255) binary = false;
        }
        t.has_alpha = alpha;
        t.alpha_is_binary = alpha && binary;
    }
    return true;
}

bool LoadTexture(const ArchiveSet& archives, const std::string& path, Texture& out, bool all_mips, std::string* error) {
    std::vector<uint8_t> data;
    if (!archives.Read(path, data, nullptr, error)) return false;
    return DecodeTexture(data.data(), data.size(), out, all_mips, error);
}

// ---------------------------------------------------------------------------

struct TextureCache::Impl {
    const ArchiveSet& archives;
    bool all_mips;
    std::mutex mu;
    struct Entry {
        std::shared_ptr<const Texture> tex;
        std::string error;
    };
    std::unordered_map<std::string, Entry> map;
    Impl(const ArchiveSet& a, bool m) : archives(a), all_mips(m) {}
};

TextureCache::TextureCache(const ArchiveSet& archives, bool all_mips) : impl_(new Impl(archives, all_mips)) {}
TextureCache::~TextureCache() = default;

std::shared_ptr<const Texture> TextureCache::Get(const std::string& path, std::string* error) {
    std::string key = detail::Lower(NormalizePath(path));
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        auto it = impl_->map.find(key);
        if (it != impl_->map.end()) {
            if (error) *error = it->second.error;
            return it->second.tex;
        }
    }
    auto tex = std::make_shared<Texture>();
    std::string err;
    std::shared_ptr<const Texture> result;
    if (LoadTexture(impl_->archives, path, *tex, impl_->all_mips, &err)) result = tex;
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto& e = impl_->map[key];
    e.tex = result;
    e.error = err;
    if (error) *error = err;
    return result;
}

void TextureCache::Clear() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->map.clear();
}

}  // namespace ma2m
