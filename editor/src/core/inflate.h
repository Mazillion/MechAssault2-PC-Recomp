#pragma once
#include <cstddef>
#include <cstdint>

namespace ma2 {
// zlib stream -> out; returns the number of bytes written, or -1 on a malformed stream.
long zlib_inflate(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen);
}
