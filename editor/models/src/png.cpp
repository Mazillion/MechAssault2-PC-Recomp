// PNG writer: per-row filter choice + LZ77 with fixed-Huffman deflate.
#include "internal.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace ma2m {

namespace {

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t buf = 0;
    int count = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void Put(uint32_t bits, int n) {  // LSB first
        buf |= bits << count;
        count += n;
        while (count >= 8) {
            out.push_back((uint8_t)buf);
            buf >>= 8;
            count -= 8;
        }
    }
    void PutRev(uint32_t code, int n) {  // Huffman code, MSB first
        uint32_t r = 0;
        for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
        Put(r, n);
    }
    void Flush() {
        if (count) out.push_back((uint8_t)buf);
        buf = 0;
        count = 0;
    }
};

void PutLiteral(BitWriter& bw, int sym) {
    if (sym < 144) bw.PutRev(0x30 + sym, 8);
    else if (sym < 256) bw.PutRev(0x190 + sym - 144, 9);
    else if (sym < 280) bw.PutRev(sym - 256, 7);
    else bw.PutRev(0xC0 + sym - 280, 8);
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void PutMatch(BitWriter& bw, int len, int dist) {
    int li = 28;
    while (kLenBase[li] > len) li--;
    PutLiteral(bw, 257 + li);
    if (kLenExtra[li]) bw.Put(len - kLenBase[li], kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > dist) di--;
    bw.PutRev(di, 5);
    if (kDistExtra[di]) bw.Put(dist - kDistBase[di], kDistExtra[di]);
}

void ZlibCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    out.push_back(0x78);
    out.push_back(0x01);
    BitWriter bw(out);
    bw.Put(1, 1);  // final
    bw.Put(1, 2);  // fixed Huffman
    const int kWindow = 32768, kHashBits = 15, kChain = 48;
    std::vector<int> head(1 << kHashBits, -1), prev(in.size(), -1);
    auto hash = [&](size_t i) {
        return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1 << kHashBits) - 1);
    };
    size_t i = 0, n = in.size();
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            int h = hash(i);
            int cand = head[h], chain = 0;
            while (cand >= 0 && (int)i - cand <= kWindow && chain++ < kChain) {
                int maxl = (int)std::min<size_t>(258, n - i), l = 0;
                while (l < maxl && in[cand + l] == in[i + l]) l++;
                if (l > best_len) {
                    best_len = l;
                    best_dist = (int)i - cand;
                    if (l == maxl) break;
                }
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = (int)i;
        }
        if (best_len >= 3) {
            PutMatch(bw, best_len, best_dist);
            for (int k = 1; k < best_len; k++) {
                size_t j = i + k;
                if (j + 3 <= n) {
                    int h = hash(j);
                    prev[j] = head[h];
                    head[h] = (int)j;
                }
            }
            i += best_len;
        } else {
            PutLiteral(bw, in[i]);
            i++;
        }
    }
    PutLiteral(bw, 256);
    bw.Flush();
    uint32_t a = detail::Adler32(in.data(), in.size());
    out.push_back((uint8_t)(a >> 24));
    out.push_back((uint8_t)(a >> 16));
    out.push_back((uint8_t)(a >> 8));
    out.push_back((uint8_t)a);
}

void PutU32BE(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    PutU32BE(png, (uint32_t)data.size());
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    PutU32BE(png, detail::Crc32(png.data() + start, png.size() - start));
}

int Paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}

}  // namespace

bool WritePng(const std::string& path, const Image& img, std::string* error) {
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() < (size_t)img.width * img.height * 4) {
        if (error) *error = "WritePng: empty image";
        return false;
    }
    size_t stride = (size_t)img.width * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * img.height);
    std::vector<uint8_t> line(stride), best(stride);
    for (int y = 0; y < img.height; y++) {
        const uint8_t* cur = &img.rgba[y * stride];
        const uint8_t* up = y ? &img.rgba[(y - 1) * stride] : nullptr;
        long best_score = -1;
        int best_f = 0;
        for (int f = 0; f < 5; f++) {
            long score = 0;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= 4 ? cur[x - 4] : 0, b = up ? up[x] : 0, c = (up && x >= 4) ? up[x - 4] : 0;
                int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : Paeth(a, b, c);
                uint8_t v = (uint8_t)(cur[x] - pred);
                line[x] = v;
                score += v < 128 ? v : 256 - v;
            }
            if (best_score < 0 || score < best_score) {
                best_score = score;
                best_f = f;
                best = line;
            }
        }
        raw.push_back((uint8_t)best_f);
        raw.insert(raw.end(), best.begin(), best.end());
    }
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    PutU32BE(ihdr, (uint32_t)img.width);
    PutU32BE(ihdr, (uint32_t)img.height);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    Chunk(png, "IHDR", ihdr);
    std::vector<uint8_t> z;
    ZlibCompress(raw, z);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    FILE* f = _wfopen(w.c_str(), L"wb");
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    if (!ok && error) *error = "short write " + path;
    return ok;
}

}  // namespace ma2m
