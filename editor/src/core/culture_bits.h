// Culture map bit allocation. The culture map is a u16 per texel; every culture layer owns `nbits` consecutive
// bits (cultureBF<i>Shift / NBits / Mask on MATerrainInfo). A plant from another level needs a free run of bits.
#pragma once
#include <cstdint>
#include <vector>

namespace ma2 {

// The lowest shift (0..16-nbits) whose `nbits` bits are free in every mask (masks are the layers' bit masks, 16 bits wide);
// 0 when no layer exists yet; -1 when there is no room or nbits is not 1..16.
inline int alloc_culture_bits(const std::vector<uint32_t>& masks, int nbits) {
    if (nbits < 1 || nbits > 16) return -1;
    uint32_t used = 0;
    for (uint32_t m : masks) used |= m & 0xFFFFu;
    const uint32_t run = (1u << nbits) - 1u;
    for (int shift = 0; shift + nbits <= 16; shift++)
        if (!(used & (run << shift))) return shift;
    return -1;
}

}  // namespace ma2
