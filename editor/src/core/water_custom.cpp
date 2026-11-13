#include "water_custom.h"

#include <algorithm>
#include <cmath>

namespace ma2::wcustom {

namespace {

// File layout (models/src/geometry.cpp): 32-byte MGIc header, an 'XMLB' chunk naming the entry,
// then VERS (sub-chunks 'info' and 'data') or INDX (sub-chunks 'vidx', 'nidx', 'data').
constexpr uint32_t kFlags = 0x301;  // position float3, normal packed 11:11:10, one short2 UV (value / 1024)
constexpr size_t kStride = 20;
constexpr uint32_t kUp = 0x001FF800u;  // the packed (0, 1, 0) normal of the retail water tiles

struct Parsed {
    size_t head = 0;       // end of the header and XMLB chunk
    uint32_t count = 0, flags = 0;
    const uint8_t* data = nullptr;
};

bool tag_is(const Bytes& b, size_t off, const char* t) { return off + 4 <= b.size() && !memcmp(b.data() + off, t, 4); }

bool parse_verts(const Bytes& b, Parsed& p) {
    if (b.size() < 0x60 || !tag_is(b, 4, "MGIc")) return false;
    size_t off = 0x20;
    if (tag_is(b, off, "XMLB")) off += rd<uint32_t>(b.data() + off + 8);
    if (!tag_is(b, off, "VERS") || off + 16 > b.size()) return false;
    p.head = off;
    uint32_t len = rd<uint32_t>(b.data() + off + 8);
    size_t end = std::min<size_t>(b.size(), off + len), q = off + 16;
    while (q + 8 <= end) {
        uint32_t l = rd<uint32_t>(b.data() + q + 4);
        if (l < 8 || q + l > end) return false;
        if (tag_is(b, q, "info") && l >= 8 + 36) {
            p.count = rd<uint32_t>(b.data() + q + 12);
            p.flags = rd<uint32_t>(b.data() + q + 16);
        } else if (tag_is(b, q, "data")) {
            p.data = b.data() + q + 8;
            if (p.count == 0 || size_t(l - 8) < size_t(p.count) * kStride) return false;
        }
        q += l;
    }
    return p.data && p.flags == kFlags && p.count >= 4;
}

struct Vtx { float x, y, z; int16_t u, v; };
Vtx vtx(const uint8_t* d) {
    Vtx r;
    r.x = rd<float>(d); r.y = rd<float>(d + 4); r.z = rd<float>(d + 8);
    r.u = rd<int16_t>(d + 16); r.v = rd<int16_t>(d + 18);
    return r;
}

// The chunk framing around `payload`: the source's own header and XMLB chunk, then the new chunk.
Bytes wrap(const Bytes& src, size_t head, const Bytes& payload) {
    Bytes out(src.begin(), src.begin() + std::ptrdiff_t(head));
    out.insert(out.end(), payload.begin(), payload.end());
    wr<uint32_t>(out.data() + 8, uint32_t(out.size()));
    return out;
}

void chunk(Bytes& b, const char* tag, const Bytes& body) {
    put_bytes(b, tag, 4);
    put<uint32_t>(b, uint32_t(body.size() + 8));
    b.insert(b.end(), body.begin(), body.end());
}

}  // namespace

bool plain(const Source& src) {
    Parsed p;
    if (!parse_verts(src.verts, p) || !tag_is(src.idx, 4, "MGIc")) return false;
    float ymin = 1e30f, ymax = -1e30f;
    for (uint32_t i = 0; i < p.count; i++) {
        Vtx v = vtx(p.data + size_t(i) * kStride);
        ymin = std::min(ymin, v.y);
        ymax = std::max(ymax, v.y);
        if (rd<uint32_t>(p.data + size_t(i) * kStride + 12) != kUp) return false;
    }
    return ymax - ymin < 0.01f;
}

bool generate(const Source& src, float w, float l, Shape shape, Bytes& verts, Bytes& idx, bool* as_list, std::string* err) {
    Parsed p;
    if (!parse_verts(src.verts, p) || !tag_is(src.idx, 4, "MGIc")) {
        if (err) *err = "this water model's geometry is not a plain grid";
        return false;
    }
    w = std::clamp(w, kMinSide, kMaxSide);
    l = std::clamp(l, kMinSide, kMaxSide);
    // The source's texture mapping: u = ku * x + cu, v = kv * z + cv (in 1/1024 steps).
    Vtx a = vtx(p.data);
    const Vtx *bx = nullptr, *bz = nullptr;
    Vtx tmp_x, tmp_z;
    for (uint32_t i = 1; i < p.count && !(bx && bz); i++) {
        Vtx v = vtx(p.data + size_t(i) * kStride);
        if (!bx && std::fabs(v.x - a.x) > 1.f) { tmp_x = v; bx = &tmp_x; }
        if (!bz && std::fabs(v.z - a.z) > 1.f) { tmp_z = v; bz = &tmp_z; }
    }
    if (!bx || !bz) {
        if (err) *err = "this water model's geometry is not a plain grid";
        return false;
    }
    float ku = float(a.u - bx->u) / (a.x - bx->x), kv = float(a.v - bz->v) / (a.z - bz->z);
    float cu = float(a.u) - ku * a.x, cv = float(a.v) - kv * a.z;
    Bytes data;
    uint32_t count = 0;
    auto put_vertex = [&](float x, float z) {
        put<float>(data, x); put<float>(data, 0.f); put<float>(data, z);
        put<uint32_t>(data, kUp);
        put<int16_t>(data, int16_t(std::lround(std::clamp(ku * x + cu, -32768.f, 32767.f))));
        put<int16_t>(data, int16_t(std::lround(std::clamp(kv * z + cv, -32768.f, 32767.f))));
        count++;
    };
    std::vector<uint16_t> ix;
    if (as_list) *as_list = shape != Shape::Rect;
    if (shape == Shape::Oval) {
        // Rings of at most 50 m around the centre: a fan, then quads between rings (a triangle list). Wound like the
        // rectangle's strips: up-facing.
        int rings = std::max(1, int(std::ceil(std::max(w, l) * 0.5f / 50.f)));
        float perim = 3.14159265f * (w + l) * 0.5f;
        int seg = (std::clamp(int(std::ceil(perim / 50.f)), 16, 360) + 3) / 4 * 4;  // a multiple of 4: the outline touches the box on all four sides
        put_vertex(0.f, 0.f);
        for (int k = 1; k <= rings; k++)
            for (int s = 0; s < seg; s++) {
                float t = 6.2831853f * float(s) / float(seg), f = float(k) / float(rings);
                put_vertex(std::cos(t) * w * 0.5f * f, std::sin(t) * l * 0.5f * f);
            }
        auto ring = [&](int k, int s) { return uint16_t(1 + (k - 1) * seg + (s % seg)); };
        for (int s = 0; s < seg; s++) {
            ix.insert(ix.end(), {uint16_t(0), ring(1, s + 1), ring(1, s)});
            for (int k = 1; k < rings; k++) {
                ix.insert(ix.end(), {ring(k, s), ring(k, s + 1), ring(k + 1, s)});
                ix.insert(ix.end(), {ring(k, s + 1), ring(k + 1, s + 1), ring(k + 1, s)});
            }
        }
    } else {
        // The grid: at most 50 m between rows and columns, like the retail 21 x 21 tiles.
        int nx = std::max(1, int(std::ceil(w / 50.f))), nz = std::max(1, int(std::ceil(l / 50.f)));
        int cols = nx + 1, rows = nz + 1;
        data.reserve(size_t(cols) * rows * kStride);
        for (int r = 0; r < rows; r++)       // row 0 at +z, like the source strip
            for (int c = 0; c < cols; c++)
                put_vertex(-w * 0.5f + w * float(c) / float(nx), l * 0.5f - l * float(r) / float(nz));
        // Indices: one strip per column pair, rows running down (-z), joined by two repeated indices
        // (an even length strip keeps the winding; same order as the retail tiles, up-facing).
        for (int c = 0; c < nx; c++) {
            if (c) { ix.push_back(ix.back()); ix.push_back(uint16_t(c)); }
            for (int r = 0; r < rows; r++) { ix.push_back(uint16_t(r * cols + c)); ix.push_back(uint16_t(r * cols + c + 1)); }
        }
    }
    Bytes info;
    put<uint32_t>(info, 3);
    put<uint32_t>(info, count);
    put<uint32_t>(info, kFlags);
    for (float f : {-w * 0.5f, 0.f, -l * 0.5f, w * 0.5f, 0.f, l * 0.5f}) put<float>(info, f);
    // VERS chunk: 'VERS', 0, length, version 2, sub-chunks.
    Bytes body;
    put<uint32_t>(body, 2);
    chunk(body, "info", info);
    chunk(body, "data", data);
    Bytes vc;
    put_bytes(vc, "VERS", 4);
    put<uint32_t>(vc, 0);
    put<uint32_t>(vc, uint32_t(body.size() + 12));
    vc.insert(vc.end(), body.begin(), body.end());
    verts = wrap(src.verts, p.head, vc);
    Bytes vidx{1}, nidx, ibody;
    put<uint32_t>(nidx, uint32_t(ix.size()));
    Bytes idata;
    for (uint16_t v : ix) put<uint16_t>(idata, v);
    put<uint32_t>(ibody, 3);
    chunk(ibody, "vidx", vidx);
    chunk(ibody, "nidx", nidx);
    chunk(ibody, "data", idata);
    Bytes ic;
    put_bytes(ic, "INDX", 4);
    put<uint32_t>(ic, 0);
    put<uint32_t>(ic, uint32_t(ibody.size() + 12));
    ic.insert(ic.end(), ibody.begin(), ibody.end());
    // The index file's header and XMLB chunk.
    size_t ihead = 0x20;
    if (tag_is(src.idx, ihead, "XMLB")) ihead += rd<uint32_t>(src.idx.data() + ihead + 8);
    idx = wrap(src.idx, ihead, ic);
    return true;
}

bool extent(const Bytes& verts, float& w, float& l) {
    Parsed p;
    if (!parse_verts(verts, p)) return false;
    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    for (uint32_t i = 0; i < p.count; i++) {
        Vtx v = vtx(p.data + size_t(i) * kStride);
        x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
        z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
    }
    w = x1 - x0;
    l = z1 - z0;
    return true;
}

}  // namespace ma2::wcustom
