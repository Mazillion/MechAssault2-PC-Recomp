// zlib (RFC 1950/1951) inflate, adapted from game tree ma2_bots_core.c.
#include "inflate.h"

#include <cstring>

namespace ma2 {
namespace {

struct Inf {
    const uint8_t* in; size_t inlen, inpos;
    uint32_t bb; int bc;
    uint8_t* out; size_t outlen, outpos;
    int err;
};
struct Huff { short count[16]; short symbol[320]; };

int bits(Inf* s, int n) {
    uint32_t v = s->bb;
    while (s->bc < n) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        v |= uint32_t(s->in[s->inpos++]) << s->bc;
        s->bc += 8;
    }
    s->bb = n < 32 ? v >> n : 0;
    s->bc -= n;
    return int(v & ((1u << n) - 1u));
}

int build(Huff* h, const short* len, int n) {
    short offs[16];
    std::memset(h->count, 0, sizeof(h->count));
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = short(offs[i] + h->count[i]);
    for (int s = 0; s < n; s++) if (len[s]) h->symbol[offs[len[s]]++] = short(s);
    return left;
}

int decode(Inf* s, const Huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

const short kLBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const short kLExt[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const short kDBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const short kDExt[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

int codes(Inf* s, const Huff* lc, const Huff* dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (s->err || sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -1;
            s->out[s->outpos++] = uint8_t(sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = kLBase[sym] + bits(s, kLExt[sym]);
            int ds = decode(s, dc);
            if (s->err || ds < 0 || ds >= 30) return -1;
            size_t dist = size_t(kDBase[ds] + bits(s, kDExt[ds]));
            if (s->err || dist > s->outpos || s->outpos + size_t(len) > s->outlen) return -1;
            while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
        }
    }
}

int stored(Inf* s) {
    s->bb = 0; s->bc = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    unsigned nlen = s->in[s->inpos + 2] | (s->in[s->inpos + 3] << 8);
    s->inpos += 4;
    if (len != (~nlen & 0xFFFFu) || s->inpos + len > s->inlen || s->outpos + len > s->outlen) return -1;
    std::memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len; s->outpos += len;
    return 0;
}

int fixed(Inf* s) {
    Huff lc, dc;
    short l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build(&lc, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(&dc, l, 30);
    return codes(s, &lc, &dc);
}

int dynamic(Inf* s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    Huff lc, dc;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    int i = 0;
    for (; i < ncode; i++) lengths[order[i]] = short(bits(s, 3));
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (s->err || build(&lc, lengths, 19) != 0) return -1;
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lc);
        if (s->err || sym < 0) return -1;
        if (sym < 16) { lengths[i++] = short(sym); continue; }
        int len = 0, rep;
        if (sym == 16) { if (i == 0) return -1; len = lengths[i - 1]; rep = 3 + bits(s, 2); }
        else if (sym == 17) rep = 3 + bits(s, 3);
        else rep = 11 + bits(s, 7);
        if (s->err || i + rep > nlen + ndist) return -1;
        while (rep--) lengths[i++] = short(len);
    }
    if (lengths[256] == 0) return -1;
    int e = build(&lc, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return -1;
    e = build(&dc, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return -1;
    return codes(s, &lc, &dc);
}

}  // namespace

long zlib_inflate(const uint8_t* in, size_t inlen, uint8_t* out, size_t outlen) {
    if (inlen < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return -1;
    Inf s;
    std::memset(&s, 0, sizeof(s));
    s.in = in; s.inlen = inlen; s.inpos = 2; s.out = out; s.outlen = outlen;
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.err) return -1;
        int rc = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (rc != 0) return -1;
    } while (!last);
    return long(s.outpos);
}

}  // namespace ma2
