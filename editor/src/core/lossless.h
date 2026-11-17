// Lossless picture store for the editor's own copy of the ground colour map.
// The game's colour map is a DXT texture; every save would re-encode a decoded DXT image and drift. The editor keeps the
// image it painted in editor/colormap.bin (the game never reads editor/*) and uses it on open when the stored hash says
// the game's TIF in the same file is the one it was encoded into.
//
// Stream: the four byte planes R, G, B, A of the image; each row left-delta coded (d = p[x] - p[x-1] mod 256), the first byte
// of a row coded against the first byte of the row above (the first row's is raw); the whole stream PackBits-coded with one
// extension (control 0xFF = a long run: a LEB128 count - 3, then the byte), so a flat 256 KB plane packs to a few bytes.
#pragma once
#include <cstdint>
#include <vector>

#include "mgic.h"

namespace ma2 {

constexpr const char* kColormapStore = "editor/colormap.bin";

// u32 w, u32 h, u32 payload size, payload. All little-endian.
Bytes pack_image(const Image& img);
bool unpack_image(const Bytes& b, Image& out);

uint64_t fnv1a64(const uint8_t* p, size_t n);
inline uint64_t fnv1a64(const Bytes& b) { return fnv1a64(b.data(), b.size()); }

// editor/colormap.bin: u32 'MA2C', u32 version 1, u32 w, u32 h, u64 tif_hash, u32 payload size, payload.
Bytes colormap_store(const Image& img, uint64_t tif_hash);
// False (and `out` untouched) when the blob is damaged, from another version, or its hash is not `tif_hash`.
bool colormap_restore(const Bytes& b, uint64_t tif_hash, Image& out);

}  // namespace ma2
