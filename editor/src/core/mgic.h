// MGIc picture container (".tif" files in MA2 archives and the terrain blobs in world files).
//
//   head    : 0x20 bytes: 00 00 01 00, 'MGIc', u32 total size, u32 section count, zeros
//   section : tag[4] ('PIC ', 'PAL '), u32 0, u32 length (incl. this 16-byte header), u32 chunk count
//   chunk   : tag[4], u32 length (incl. this 8-byte header), payload
// PIC chunks: 'ver ' (1 byte), 'flgs', 'wdth', 'hgt ', 'mips', 'size', 'frms', 'dpth', 'bits'.
// flgs: low nibble = pixel format (0 A8R8G8B8, 1 R5G6B5, 3 A4R4G4B4, 4 P8, 5 A8, 7 A8L8),
//       0x10 = swizzled (Xbox Morton order), bits 8-11 = DXT1/3/5, 0x80000000 = mipmapped set.
// Linear uncompressed pictures whose payload does not match a tight layout use a 64-byte pitch
// (the 8-bit terrain heightfield: 257 -> 320 bytes per row).
#pragma once
#include <string>
#include <vector>

#include "util.h"

namespace ma2 {

constexpr uint32_t fourcc(const char t[5]) {
    return uint32_t(uint8_t(t[0])) | uint32_t(uint8_t(t[1])) << 8 | uint32_t(uint8_t(t[2])) << 16 | uint32_t(uint8_t(t[3])) << 24;
}

struct MgicChunk { uint32_t tag = 0; Bytes data; };
struct MgicSection {
    uint32_t tag = 0;
    uint32_t zero = 0;
    std::vector<MgicChunk> chunks;
    Bytes trailing;  // bytes inside the section after its chunks
    MgicChunk* chunk(uint32_t tag);
    const MgicChunk* chunk(uint32_t tag) const;
    uint32_t u32(uint32_t tag, uint32_t def = 0) const;
    void set_u32(uint32_t tag, uint32_t v);
};

struct Mgic {
    uint8_t head[0x20] = {};
    std::vector<MgicSection> sections;
    Bytes tail;

    bool parse(const uint8_t* d, size_t n, std::string* err = nullptr);
    bool parse(const Bytes& b, std::string* err = nullptr) { return parse(b.data(), b.size(), err); }
    Bytes serialize() const;
    MgicSection* section(uint32_t tag);
    const MgicSection* section(uint32_t tag) const;
};

enum PixFmt : int { PF_ARGB8888 = 0, PF_RGB565 = 1, PF_ARGB4444 = 3, PF_P8 = 4, PF_A8 = 5, PF_A8L8 = 7 };

// Decoded view of a PIC section.
struct Picture {
    uint32_t flags = 0, width = 0, height = 0, mips = 1, frames = 1, depth = 1;
    int row_align = 1;
    Bytes bits;
    std::vector<uint32_t> palette;  // RGBA (R in the low byte) from 'PAL ', if any

    int format() const { return int(flags & 15); }
    bool swizzled() const { return (flags & 16) != 0; }
    int dxt() const { return int((flags >> 8) & 15); }
    std::string format_name() const;
    size_t level_size(int level) const;   // one frame, one level, including pitch
    size_t level_offset(int level) const;
    uint32_t pitch(int level) const;      // bytes per row (uncompressed)
};

bool read_picture(const Mgic& m, Picture& p, std::string* err = nullptr);
bool read_picture(const Bytes& file, Picture& p, std::string* err = nullptr);

// RGBA8 pixels, R in the low byte (D3D R8G8B8A8_UNORM order).
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    uint32_t& at(int x, int y) { return px[size_t(y) * w + x]; }
    uint32_t at(int x, int y) const { return px[size_t(y) * w + x]; }
};

// Decodes level `level` of frame 0. For P8 without a palette, the index becomes grey.
bool decode_picture(const Picture& p, int level, Image& out, std::string* err = nullptr);
// Raw 8-bit samples of a P8/A8 picture (level 0), pitch removed: width*height bytes.
bool picture_samples8(const Picture& p, std::vector<uint8_t>& out);

// Encodes a full mip chain in the picture's own format (flags, mips) from a top-level image of
// size width x height. Lower levels are box-filtered.
Bytes encode_levels(const Picture& layout, const Image& top);
// Builds a picture file from a style picture (copied head/ver/PAL) with new pixels.
// The new picture keeps style's flags and mip count unless overridden (mips=0 keeps style).
Bytes build_picture(const Mgic& style, uint32_t flags, uint32_t width, uint32_t height, uint32_t mips,
                    const Bytes& bits, int row_align);
// Heightfield helper: an 8-bit P8 picture with a 64-byte row pitch.
Bytes build_heightfield(const Mgic& style, uint32_t width, uint32_t height, const std::vector<uint8_t>& samples);

// Swizzle (Xbox Morton) index of (x, y) in a w x h power-of-two surface.
uint32_t swizzle_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

void dxt_decode_block(int dxt, const uint8_t* block, uint32_t out[16]);
void dxt_encode_block(int dxt, const uint32_t in[16], uint8_t* block);

}  // namespace ma2
