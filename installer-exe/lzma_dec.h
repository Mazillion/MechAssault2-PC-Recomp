// LZMA ("LZMA alone" / .lzma) decoder for the installer stub: lets the kit unpack without the Windows Compression API,
// which Wine and Proton do not implement (the stub stopped at "unimplemented function Cabinet.dll.40" under Wine).
// Written from the LZMA specification (Igor Pavlov's lzma-specification.txt, public domain); the whole output is the
// dictionary, so the caller must know the unpacked size (the kit index records it). Header: 1 byte lc/lp/pb, 4 bytes
// dictionary size, 8 bytes unpacked size (ignored: the index size wins); an end marker, if present, ends the stream.
// lzma_decode() returns true only when exactly `out_size` bytes came out of a well-formed stream.
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace lzma_dec {

struct Range {
    const uint8_t *p, *end; uint32_t range = 0xFFFFFFFFu, code = 0; bool bad = false;
    uint8_t byte() { if (p < end) return *p++; bad = true; return 0; }
    bool init() { if (byte() != 0) return false; for (int i = 0; i < 4; i++) code = (code << 8) | byte(); return !bad && code != range; }
    void norm() { if (range < (1u << 24)) { range <<= 8; code = (code << 8) | byte(); } }
    unsigned bit(uint16_t &prob) {
        uint32_t bound = (range >> 11) * prob; unsigned b;
        if (code < bound) { prob = (uint16_t)(prob + ((2048 - prob) >> 5)); range = bound; b = 0; }
        else { prob = (uint16_t)(prob - (prob >> 5)); code -= bound; range -= bound; b = 1; }
        norm(); return b;
    }
    uint32_t direct(unsigned n) {
        uint32_t res = 0;
        do {
            range >>= 1; code -= range; uint32_t t = 0u - (code >> 31); code += range & t;
            if (code == range) bad = true;
            norm(); res = (res << 1) + (t + 1);
        } while (--n);
        return res;
    }
    unsigned tree(uint16_t *probs, unsigned bits) {
        unsigned m = 1; for (unsigned i = 0; i < bits; i++) m = (m << 1) + bit(probs[m]); return m - (1u << bits);
    }
    unsigned rtree(uint16_t *probs, unsigned bits) {
        unsigned m = 1, sym = 0;
        for (unsigned i = 0; i < bits; i++) { unsigned b = bit(probs[m]); m = (m << 1) + b; sym |= b << i; }
        return sym;
    }
};

struct Len {
    uint16_t choice = 1024, choice2 = 1024, low[16][8], mid[16][8], high[256];
    Len() { for (auto &a : low) for (auto &b : a) b = 1024; for (auto &a : mid) for (auto &b : a) b = 1024; for (auto &b : high) b = 1024; }
    unsigned get(Range &rc, unsigned ps) {
        if (!rc.bit(choice)) return rc.tree(low[ps], 3);
        if (!rc.bit(choice2)) return 8 + rc.tree(mid[ps], 3);
        return 16 + rc.tree(high, 8);
    }
};

inline bool lzma_decode(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    if (in_size < 13) return false;
    unsigned d = in[0];
    if (d >= 9 * 5 * 5) return false;
    unsigned lc = d % 9, lp = (d / 9) % 5, pb = d / 45;
    Range rc{in + 13, in + in_size};
    if (!rc.init()) return false;
    std::vector<uint16_t> lit((size_t)0x300 << (lc + lp), 1024);
    uint16_t isMatch[12 << 4], isRep[12], isRepG0[12], isRepG1[12], isRepG2[12], isRep0Long[12 << 4];
    uint16_t posSlot[4][64], posDec[115], align[16];
    for (auto *a : {isMatch, isRep0Long}) for (int i = 0; i < (12 << 4); i++) a[i] = 1024;
    for (auto *a : {isRep, isRepG0, isRepG1, isRepG2}) for (int i = 0; i < 12; i++) a[i] = 1024;
    for (auto &a : posSlot) for (auto &b : a) b = 1024;
    for (auto &b : posDec) b = 1024;
    for (auto &b : align) b = 1024;
    Len lenDec, repLenDec;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0; unsigned state = 0;
    size_t pos = 0;
    const unsigned pbMask = (1u << pb) - 1, lpMask = (1u << lp) - 1;
    while (pos < out_size) {
        unsigned ps = (unsigned)pos & pbMask;
        if (!rc.bit(isMatch[(state << 4) + ps])) {
            unsigned prev = pos ? out[pos - 1] : 0;
            uint16_t *probs = &lit[(size_t)0x300 * ((((unsigned)pos & lpMask) << lc) + (prev >> (8 - lc)))];
            unsigned sym = 1;
            if (state >= 7) {
                if (rep0 >= pos) return false;
                unsigned mb = out[pos - rep0 - 1];
                do {
                    unsigned m = (mb >> 7) & 1; mb <<= 1;
                    unsigned b = rc.bit(probs[((1 + m) << 8) + sym]);
                    sym = (sym << 1) | b;
                    if (m != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | rc.bit(probs[sym]);
            out[pos++] = (uint8_t)sym;
            state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
            if (rc.bad) return false;
            continue;
        }
        unsigned len;
        if (rc.bit(isRep[state])) {
            if (pos == 0) return false;
            if (!rc.bit(isRepG0[state])) {
                if (!rc.bit(isRep0Long[(state << 4) + ps])) {
                    state = state < 7 ? 9 : 11;
                    if (rep0 >= pos) return false;
                    out[pos] = out[pos - rep0 - 1]; pos++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc.bit(isRepG1[state])) dist = rep1;
                else { if (!rc.bit(isRepG2[state])) dist = rep2; else { dist = rep3; rep3 = rep2; } rep2 = rep1; }
                rep1 = rep0; rep0 = dist;
            }
            len = repLenDec.get(rc, ps);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2; rep2 = rep1; rep1 = rep0;
            len = lenDec.get(rc, ps);
            state = state < 7 ? 7 : 10;
            unsigned ls = len < 3 ? len : 3;
            unsigned slot = rc.tree(posSlot[ls], 6);
            uint32_t dist;
            if (slot < 4) dist = slot;
            else {
                unsigned nd = (slot >> 1) - 1;
                dist = (2 | (slot & 1)) << nd;
                if (slot < 14) dist += rc.rtree(posDec + dist - slot, nd);
                else dist += (rc.direct(nd - 4) << 4) + rc.rtree(align, 4);
            }
            rep0 = dist;
            if (rep0 == 0xFFFFFFFFu) break;            // end marker
        }
        len += 2;
        if (rep0 >= pos || rc.bad) return false;
        if (len > out_size - pos) return false;          // a match running past the recorded size: not this file
        for (unsigned i = 0; i < len; i++, pos++) out[pos] = out[pos - rep0 - 1];
    }
    return pos == out_size && !rc.bad;
}

}  // namespace lzma_dec
