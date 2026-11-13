// Table-driven inflate (RFC 1950/1951), CRC-32 and Adler-32.
// Written for ma2models; the MGF archives store independent zlib blocks.
#include "internal.h"

namespace ma2m {
namespace detail {

namespace {

struct BitReader {
    const uint8_t* in;
    size_t len, pos;
    uint64_t buf;
    int count;

    void Refill() {
        while (count <= 56) {
            uint64_t byte = 0;
            if (pos < len) byte = in[pos];
            else overrun_bytes++;
            pos++;
            buf |= byte << count;
            count += 8;
        }
    }
    uint32_t Peek(int n) {
        if (count < n) Refill();
        return (uint32_t)(buf & ((1ull << n) - 1));
    }
    void Drop(int n) { buf >>= n; count -= n; }
    uint32_t Bits(int n) {
        if (n == 0) return 0;
        uint32_t v = Peek(n);
        Drop(n);
        return v;
    }
    int overrun_bytes = 0;
};

// A decoding table: entry = (symbol << 4) | code_length; 0 = invalid code.
struct Table {
    std::vector<uint16_t> entries;
    int bits = 0;
};

bool BuildTable(const uint8_t* lengths, int n, Table& t) {
    int count[16] = {0};
    int max_len = 0;
    for (int i = 0; i < n; i++) {
        count[lengths[i]]++;
        if (lengths[i] > max_len) max_len = lengths[i];
    }
    count[0] = 0;
    if (max_len == 0) {  // no codes (valid for an unused distance table)
        t.bits = 1;
        t.entries.assign(2, 0);
        return true;
    }
    int left = 1;
    for (int i = 1; i <= 15; i++) {
        left <<= 1;
        left -= count[i];
        if (left < 0) return false;  // over-subscribed
    }
    int next[16] = {0};
    int code = 0;
    for (int i = 1; i <= 15; i++) {
        code = (code + count[i - 1]) << 1;
        next[i] = code;
    }
    t.bits = max_len;
    t.entries.assign((size_t)1 << max_len, 0);
    for (int sym = 0; sym < n; sym++) {
        int len = lengths[sym];
        if (!len) continue;
        int c = next[len]++;
        // Reverse the code bits (deflate sends Huffman codes MSB first).
        int r = 0;
        for (int b = 0; b < len; b++) r |= ((c >> b) & 1) << (len - 1 - b);
        for (int fill = r; fill < (1 << max_len); fill += 1 << len)
            t.entries[fill] = (uint16_t)((sym << 4) | len);
    }
    return true;
}

inline int Decode(BitReader& br, const Table& t) {
    uint32_t e = t.entries[br.Peek(t.bits)];
    if (!e) return -1;
    br.Drop(e & 15);
    return (int)(e >> 4);
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                               3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool InflateCodes(BitReader& br, const Table& lit, const Table& dist, uint8_t* out, size_t out_len,
                  size_t& op) {
    for (;;) {
        int sym = Decode(br, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (op >= out_len) return false;
            out[op++] = (uint8_t)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + br.Bits(kLenExtra[sym]);
            int ds = Decode(br, dist);
            if (ds < 0 || ds >= 30) return false;
            size_t d = kDistBase[ds] + br.Bits(kDistExtra[ds]);
            if (d > op || op + len > out_len) return false;
            uint8_t* dst = out + op;
            const uint8_t* src = dst - d;
            if (d >= len) {
                memcpy(dst, src, len);
            } else {
                for (size_t i = 0; i < len; i++) dst[i] = src[i];
            }
            op += len;
        }
        if (br.overrun_bytes > 8) return false;
    }
}

}  // namespace

long RawInflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, size_t* consumed) {
    BitReader br{in, in_len, 0, 0, 0};
    size_t op = 0;
    struct Fixed {
        Table lit, dist;
        Fixed() {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            BuildTable(l, 288, lit);
            for (i = 0; i < 30; i++) l[i] = 5;
            BuildTable(l, 30, dist);
        }
    };
    static const Fixed fixed;  // thread-safe static init
    const Table& fixed_lit = fixed.lit;
    const Table& fixed_dist = fixed.dist;
    Table lit, dist;
    bool last = false;
    while (!last) {
        last = br.Bits(1) != 0;
        uint32_t type = br.Bits(2);
        if (type == 0) {
            // Stored block: align to a byte boundary.
            br.Drop(br.count & 7);
            uint32_t len = br.Bits(16), nlen = br.Bits(16);
            if ((len ^ 0xFFFF) != nlen) return -1;
            if (op + len > out_len) return -1;
            // Drain whole bytes still sitting in the bit buffer first.
            while (len && br.count >= 8) {
                out[op++] = (uint8_t)br.Bits(8);
                len--;
            }
            if (len) {
                // Buffer is empty here; read straight from the input.
                if (br.pos + len > br.len) return -1;
                memcpy(out + op, br.in + br.pos, len);
                br.pos += len;
                op += len;
            }
        } else if (type == 1) {
            if (!InflateCodes(br, fixed_lit, fixed_dist, out, out_len, op)) return -1;
        } else if (type == 2) {
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = (int)br.Bits(5) + 257, ndist = (int)br.Bits(5) + 1, ncode = (int)br.Bits(4) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            uint8_t cl[19] = {0};
            for (int i = 0; i < ncode; i++) cl[order[i]] = (uint8_t)br.Bits(3);
            Table clt;
            if (!BuildTable(cl, 19, clt)) return -1;
            uint8_t lengths[320] = {0};
            int i = 0;
            while (i < nlen + ndist) {
                int sym = Decode(br, clt);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[i++] = (uint8_t)sym;
                } else {
                    int rep, val = 0;
                    if (sym == 16) {
                        if (i == 0) return -1;
                        val = lengths[i - 1];
                        rep = 3 + (int)br.Bits(2);
                    } else if (sym == 17) {
                        rep = 3 + (int)br.Bits(3);
                    } else {
                        rep = 11 + (int)br.Bits(7);
                    }
                    if (i + rep > nlen + ndist) return -1;
                    while (rep--) lengths[i++] = (uint8_t)val;
                }
            }
            if (!lengths[256]) return -1;
            if (!BuildTable(lengths, nlen, lit) || !BuildTable(lengths + nlen, ndist, dist)) return -1;
            if (!InflateCodes(br, lit, dist, out, out_len, op)) return -1;
        } else {
            return -1;
        }
        if (br.overrun_bytes > 8) return -1;
    }
    if (consumed) *consumed = br.pos - (size_t)(br.count / 8);
    return (long)op;
}

long ZlibInflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
    if (in_len < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return -1;
    return RawInflate(in + 2, in_len - 2, out, out_len, nullptr);
}

uint32_t Crc32(const void* data, size_t len, uint32_t crc) {
    struct CrcTable {
        uint32_t v[256];
        CrcTable() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                v[i] = c;
            }
        }
    };
    static const CrcTable crc_table;
    const uint32_t* table = crc_table.v;
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t Adler32(const void* data, size_t len, uint32_t adler) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (len) {
        size_t n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

}  // namespace detail
}  // namespace ma2m
