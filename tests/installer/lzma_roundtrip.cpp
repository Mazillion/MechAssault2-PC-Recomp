// lzma_roundtrip <list.txt> (built by test_stub_lzma.py): each line "packed<TAB>original"; decodes each packed file and compares with the original.
#include "lzma_dec.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
static std::vector<uint8_t> rd(const std::string &p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}); }
int main(int argc, char **argv) {
    std::ifstream list(argv[1]); std::string line; int ok = 0, bad = 0; unsigned long long bytes = 0;
    while (std::getline(list, line)) {
        auto t = line.find('\t'); auto packed = rd(line.substr(0, t)), orig = rd(line.substr(t + 1));
        std::vector<uint8_t> out(orig.size() ? orig.size() : 1);
        bool good = lzma_dec::lzma_decode(packed.data(), packed.size(), out.data(), orig.size()) && std::equal(orig.begin(), orig.end(), out.begin());
        if (good) { ok++; bytes += orig.size(); } else { bad++; std::printf("BAD %s\n", line.c_str() + t + 1); }
        // a damaged stream must be refused, never crash: flip a byte in the middle
        if (packed.size() > 40) { packed[packed.size() / 2] ^= 0x5A; lzma_dec::lzma_decode(packed.data(), packed.size(), out.data(), orig.size()); }
    }
    std::printf("ok=%d bad=%d bytes=%llu\n", ok, bad, bytes); return bad ? 1 : 0;
}
