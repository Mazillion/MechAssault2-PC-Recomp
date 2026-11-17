#include "crop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

#include "ma2models.h"

namespace ma2::crop {

// ----------------------------------------------------------------------------- the property

bool parse(std::string_view s, Rect& r) {
    std::string t(s);
    Rect q;
    if (sscanf(t.c_str(), "%f %f %f %f", &q.x0, &q.x1, &q.z0, &q.z1) != 4) return false;
    if (!(q.x1 - q.x0 >= 0.05f) || !(q.z1 - q.z0 >= 0.05f)) return false;  // a wall is cut on its long axis only: its thin axis may be a few cm
    r = q;
    return true;
}

std::string format(const Rect& r) {
    char b[96];
    snprintf(b, sizeof b, "%.2f %.2f %.2f %.2f", r.x0, r.x1, r.z0, r.z1);
    return b;
}

bool of(const WObj& o, Rect& r) {
    const WProp* p = o.find(kProp);
    return p && parse(p->as_string(), r);
}

void set(WObj& o, const Rect* r) {
    if (!r) { o.erase(key_of(kProp)); return; }
    o.set_str(kProp, format(*r));
}

std::string tag(const Rect& r) {
    std::string f = format(r);
    char b[16];
    snprintf(b, sizeof b, "%08X", ~crc_acc(f.data(), f.size()));
    return b;
}

void clamp_point(const Rect& r, float& x, float& z, float inset) {
    float ix = std::min(inset, r.w() * 0.5f), iz = std::min(inset, r.l() * 0.5f);
    x = std::clamp(x, r.x0 + ix, r.x1 - ix);
    z = std::clamp(z, r.z0 + iz, r.z1 - iz);
}

// ----------------------------------------------------------------------------- clipping

std::vector<Plane> rect_planes(const Rect& r, const float* m) {
    static const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!m) m = I;
    // Model x = p . (m0, m4, m8) + m12, model z = p . (m2, m6, m10) + m14.
    return {{m[0], m[4], m[8], m[12] - r.x0},
            {-m[0], -m[4], -m[8], r.x1 - m[12]},
            {m[2], m[6], m[10], m[14] - r.z0},
            {-m[2], -m[6], -m[10], r.z1 - m[14]}};
}

namespace {

float side(const Plane& pl, const Vert& v) { return pl.a * v.p[0] + pl.b * v.p[1] + pl.c * v.p[2] + pl.d; }

bool before(const Vert& a, const Vert& b) {
    for (int k = 0; k < 3; k++)
        if (a.p[k] != b.p[k]) return a.p[k] < b.p[k];
    return std::memcmp(&a, &b, sizeof(Vert)) < 0;
}

// The point where edge a-b meets the plane. Always computed from the same end, so the two triangles that share an
// edge get bit-identical vertices (no cracks).
Vert cut(const Plane& pl, Vert a, Vert b) {
    if (before(b, a)) std::swap(a, b);
    float da = side(pl, a), db = side(pl, b);
    float t = std::clamp(da / (da - db), 0.f, 1.f);
    Vert r;
    for (int k = 0; k < 3; k++) r.p[k] = a.p[k] + (b.p[k] - a.p[k]) * t;
    float len = 0;
    for (int k = 0; k < 3; k++) { r.n[k] = a.n[k] + (b.n[k] - a.n[k]) * t; len += r.n[k] * r.n[k]; }
    len = std::sqrt(len);
    if (len > 1e-6f) for (float& c : r.n) c /= len;
    else std::memcpy(r.n, a.n, sizeof r.n);
    for (int k = 0; k < 2; k++) { r.uv0[k] = a.uv0[k] + (b.uv0[k] - a.uv0[k]) * t; r.uv1[k] = a.uv1[k] + (b.uv1[k] - a.uv1[k]) * t; }
    for (int k = 0; k < 4; k++) r.col[k] = a.col[k] + (b.col[k] - a.col[k]) * t;
    std::memcpy(r.tan, t < 0.5f ? a.tan : b.tan, sizeof r.tan);
    // Snap onto the plane exactly where it is axis aligned (the usual case), so the cut edge is straight.
    return r;
}

}  // namespace

void clip(std::vector<Vert>& v, std::vector<uint32_t>& tris, const std::vector<Plane>& planes) {
    const float eps = 1e-4f;
    std::vector<Vert> out_v;
    std::vector<uint32_t> out_t;
    std::vector<int32_t> remap(v.size(), -1);
    std::unordered_map<std::string, uint32_t> fresh;
    auto keep_orig = [&](uint32_t i) {
        if (remap[i] < 0) { remap[i] = int32_t(out_v.size()); out_v.push_back(v[i]); }
        return uint32_t(remap[i]);
    };
    auto add_new = [&](const Vert& x) {
        std::string k(reinterpret_cast<const char*>(&x), sizeof(Vert));
        auto it = fresh.find(k);
        if (it != fresh.end()) return it->second;
        uint32_t i = uint32_t(out_v.size());
        out_v.push_back(x);
        fresh.emplace(std::move(k), i);
        return i;
    };
    struct PV { Vert v; int64_t orig; };
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        uint32_t id[3] = {tris[t], tris[t + 1], tris[t + 2]};
        if (id[0] >= v.size() || id[1] >= v.size() || id[2] >= v.size()) continue;
        bool all_in = true, all_out_one = false;
        for (auto& pl : planes) {
            int out = 0;
            for (uint32_t i : id) out += side(pl, v[i]) < -eps;
            if (out == 3) all_out_one = true;
            if (out) all_in = false;
        }
        if (all_out_one) continue;
        if (all_in) {
            for (uint32_t i : id) out_t.push_back(keep_orig(i));
            continue;
        }
        std::vector<PV> poly = {{v[id[0]], id[0]}, {v[id[1]], id[1]}, {v[id[2]], id[2]}};
        for (auto& pl : planes) {
            std::vector<PV> np;
            for (size_t k = 0; k < poly.size(); k++) {
                const PV& a = poly[k];
                const PV& b = poly[(k + 1) % poly.size()];
                float da = side(pl, a.v), db = side(pl, b.v);
                bool ina = da >= -eps, inb = db >= -eps;
                if (ina) np.push_back(a);
                if (ina != inb) np.push_back({cut(pl, a.v, b.v), -1});
            }
            poly.swap(np);
            if (poly.size() < 3) break;
        }
        if (poly.size() < 3) continue;
        std::vector<uint32_t> ix;
        for (auto& p : poly) ix.push_back(p.orig >= 0 ? keep_orig(uint32_t(p.orig)) : add_new(p.v));
        for (size_t k = 1; k + 1 < ix.size(); k++) {
            if (ix[0] == ix[k] || ix[k] == ix[k + 1] || ix[0] == ix[k + 1]) continue;
            out_t.insert(out_t.end(), {ix[0], ix[k], ix[k + 1]});
        }
    }
    v.swap(out_v);
    tris.swap(out_t);
}

void strip_to_list(const std::vector<uint16_t>& idx, size_t nverts, std::vector<uint32_t>& out) {
    out.clear();
    size_t run = 0;
    for (size_t i = 0; i < idx.size(); i++) {
        if (idx[i] == 0xFFFF) { run = i + 1; continue; }
        if (i - run < 2) continue;
        uint32_t a = idx[i - 2], b = idx[i - 1], c = idx[i];
        if ((i - run - 2) & 1) std::swap(a, b);
        if (a == b || b == c || a == c || a >= nverts || b >= nverts || c >= nverts) continue;
        out.insert(out.end(), {a, b, c});
    }
}

std::vector<uint16_t> list_to_strip(const std::vector<uint32_t>& tris) {
    std::vector<uint16_t> s;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        uint16_t a = uint16_t(tris[t]), b = uint16_t(tris[t + 1]), c = uint16_t(tris[t + 2]);
        if (!s.empty()) {
            // Join with repeated indices (zero-area triangles); the real triangle starts at an even place, so it keeps
            // its winding (strip triangle k is (k, k+1, k+2), odd ones swapped).
            s.push_back(s.back());
            s.push_back(a);
            if (s.size() & 1) s.push_back(a);
        }
        s.insert(s.end(), {a, b, c});
    }
    return s;
}

// ----------------------------------------------------------------------------- vertex and index files

namespace {

bool tag_is(const Bytes& b, size_t off, const char* t) { return off + 4 <= b.size() && !std::memcmp(b.data() + off, t, 4); }

// Where the payload chunk (VERS / INDX) starts: after the 32-byte MGIc header and an optional XMLB chunk.
bool payload_at(const Bytes& b, const char* tag, size_t& at) {
    if (b.size() < 0x30 || !tag_is(b, 4, "MGIc")) return false;
    size_t off = 0x20;
    if (tag_is(b, off, "XMLB")) off += rd<uint32_t>(b.data() + off + 8);
    if (!tag_is(b, off, tag) || off + 16 > b.size()) return false;
    at = off;
    return true;
}

bool find_sub(const Bytes& b, size_t start, size_t end, const char* t, size_t& body, uint32_t& len) {
    size_t q = start;
    while (q + 8 <= end) {
        uint32_t l = rd<uint32_t>(b.data() + q + 4);
        if (l < 8 || q + l > end) return false;
        if (!std::memcmp(b.data() + q, t, 4)) { body = q + 8; len = l - 8; return true; }
        q += l;
    }
    return false;
}

uint32_t stride_of(uint32_t f) {
    uint32_t s = 0;
    if (f & 0x1) s += 12;
    if (f & 0x2) s += 12;
    if (f & 0x100) s += 4;
    if (f & 0x8) s += 4;
    if (f & 0x100000) s += 16;
    if (f & 0x200) s += 4;
    if (f & 0x400) s += 8;
    if (f & 0x1000) s += 8;
    if (f & 0x2000) s += 16;
    if (f & 0x80000) s += 20;
    return s;
}

float sext(uint32_t v, int bits, float scale) {
    int32_t s = int32_t(v << (32 - bits)) >> (32 - bits);
    return float(s) / scale;
}

int uv_sets(uint32_t f) { return (f & 0x200) ? 1 : (f & 0x400) ? 2 : (f & 0x1000) ? 1 : (f & 0x2000) ? 2 : 0; }

Vert decode(uint32_t f, const uint8_t* d) {
    Vert v;
    std::memcpy(v.p, d, 12);
    d += 12;
    if (f & 0x2) { std::memcpy(v.n, d, 12); d += 12; }
    if (f & 0x100) {
        uint32_t p = rd<uint32_t>(d);
        v.n[0] = sext(p & 0x7FF, 11, 1023.f);
        v.n[1] = sext((p >> 11) & 0x7FF, 11, 1023.f);
        v.n[2] = sext(p >> 22, 10, 511.f);
        d += 4;
    }
    if (f & 0x8) {
        uint32_t c = rd<uint32_t>(d);  // 0xAARRGGBB
        v.col[0] = float((c >> 16) & 255) / 255.f;
        v.col[1] = float((c >> 8) & 255) / 255.f;
        v.col[2] = float(c & 255) / 255.f;
        v.col[3] = float(c >> 24) / 255.f;
        d += 4;
    }
    if (f & 0x100000) { std::memcpy(v.tan, d, 16); d += 16; }
    const bool shorts = (f & (0x200 | 0x400)) != 0;
    for (int s = 0; s < uv_sets(f); s++) {
        float* uv = s ? v.uv1 : v.uv0;
        if (shorts) { uv[0] = float(rd<int16_t>(d)) / 1024.f; uv[1] = float(rd<int16_t>(d + 2)) / 1024.f; d += 4; }
        else { std::memcpy(uv, d, 8); d += 8; }
    }
    return v;
}

void encode(uint32_t f, const Vert& v, Bytes& out) {
    put_bytes(out, v.p, 12);
    if (f & 0x2) put_bytes(out, v.n, 12);
    if (f & 0x100) {
        auto q = [](float x, float s, uint32_t mask) { return uint32_t(int32_t(std::lround(std::clamp(x, -1.f, 1.f) * s))) & mask; };
        put<uint32_t>(out, q(v.n[0], 1023.f, 0x7FF) | (q(v.n[1], 1023.f, 0x7FF) << 11) | (q(v.n[2], 511.f, 0x3FF) << 22));
    }
    if (f & 0x8) {
        auto b = [](float x) { return uint32_t(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
        put<uint32_t>(out, (b(v.col[3]) << 24) | (b(v.col[0]) << 16) | (b(v.col[1]) << 8) | b(v.col[2]));
    }
    if (f & 0x100000) put_bytes(out, v.tan, 16);
    const bool shorts = (f & (0x200 | 0x400)) != 0;
    for (int s = 0; s < uv_sets(f); s++) {
        const float* uv = s ? v.uv1 : v.uv0;
        if (shorts)
            for (int k = 0; k < 2; k++) put<int16_t>(out, int16_t(std::lround(std::clamp(uv[k] * 1024.f, -32768.f, 32767.f))));
        else put_bytes(out, uv, 8);
    }
}

void chunk(Bytes& b, const char* t, const Bytes& body) {
    put_bytes(b, t, 4);
    put<uint32_t>(b, uint32_t(body.size() + 8));
    b.insert(b.end(), body.begin(), body.end());
}

// The source file's own header and XMLB chunk, then the new payload; the header's size field updated.
Bytes wrap(const Bytes& src, size_t head, const char* t, const Bytes& body) {
    Bytes out(src.begin(), src.begin() + std::ptrdiff_t(head));
    put_bytes(out, t, 4);
    put<uint32_t>(out, 0);
    put<uint32_t>(out, uint32_t(body.size() + 12));
    out.insert(out.end(), body.begin(), body.end());
    wr<uint32_t>(out.data() + 8, uint32_t(out.size()));
    return out;
}

struct Geo {
    uint32_t flags = 0;
    std::vector<Vert> v;
    std::vector<uint32_t> tris;
};

bool read_geo(const Bytes& vb, const Bytes& ib, bool strip, Geo& g, std::string* err) {
    size_t at;
    if (!payload_at(vb, "VERS", at)) { if (err) *err = "unknown vertex file"; return false; }
    size_t end = std::min<size_t>(vb.size(), at + rd<uint32_t>(vb.data() + at + 8)), info, data;
    uint32_t li, ld;
    if (!find_sub(vb, at + 16, end, "info", info, li) || li < 36 || !find_sub(vb, at + 16, end, "data", data, ld)) {
        if (err) *err = "vertex file without info / data";
        return false;
    }
    uint32_t n = rd<uint32_t>(vb.data() + info + 4);
    g.flags = rd<uint32_t>(vb.data() + info + 8);
    const uint32_t known = 0x1 | 0x2 | 0x8 | 0x100 | 0x200 | 0x400 | 0x1000 | 0x2000 | 0x100000;
    uint32_t st = stride_of(g.flags);
    if ((g.flags & ~known) || !(g.flags & 1) || !n || size_t(n) * st > ld) {
        if (err) { char b[96]; snprintf(b, sizeof b, "vertex layout 0x%X cannot be cut", g.flags); *err = b; }
        return false;
    }
    g.v.resize(n);
    for (uint32_t i = 0; i < n; i++) g.v[i] = decode(g.flags, vb.data() + data + size_t(i) * st);
    if (!payload_at(ib, "INDX", at)) { if (err) *err = "unknown index file"; return false; }
    end = std::min<size_t>(ib.size(), at + rd<uint32_t>(ib.data() + at + 8));
    size_t body, nb;
    uint32_t lb, ln;
    if (!find_sub(ib, at + 16, end, "data", body, lb)) { if (err) *err = "index file without data"; return false; }
    uint32_t count = lb / 2;
    if (find_sub(ib, at + 16, end, "nidx", nb, ln) && ln >= 4) count = std::min(count, rd<uint32_t>(ib.data() + nb));
    std::vector<uint16_t> idx(count);
    if (count) std::memcpy(idx.data(), ib.data() + body, size_t(count) * 2);
    if (strip) strip_to_list(idx, n, g.tris);
    else {
        g.tris.clear();
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
            if (idx[i] < n && idx[i + 1] < n && idx[i + 2] < n) g.tris.insert(g.tris.end(), {idx[i], idx[i + 1], idx[i + 2]});
    }
    return true;
}

void write_geo(const Bytes& vsrc, const Bytes& isrc, Geo g, bool strip, Bytes& vout, Bytes& iout) {
    if (g.v.empty() || g.tris.empty()) {  // nothing left: one point and a zero-area triangle keep the files loadable
        Vert keep = g.v.empty() ? Vert() : g.v.front();
        g.v = {keep};
        g.tris = {0, 0, 0};
    }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    Bytes data;
    for (auto& v : g.v) {
        for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], v.p[k]); mx[k] = std::max(mx[k], v.p[k]); }
        encode(g.flags, v, data);
    }
    Bytes info;
    put<uint32_t>(info, 3);
    put<uint32_t>(info, uint32_t(g.v.size()));
    put<uint32_t>(info, g.flags);
    for (float f : {mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]}) put<float>(info, f);
    Bytes body;
    put<uint32_t>(body, 2);
    chunk(body, "info", info);
    chunk(body, "data", data);
    size_t at = 0;
    payload_at(vsrc, "VERS", at);
    vout = wrap(vsrc, at, "VERS", body);
    std::vector<uint16_t> ix;
    if (strip) ix = list_to_strip(g.tris);
    else for (uint32_t i : g.tris) ix.push_back(uint16_t(i));
    Bytes ib, nidx, idata;
    put<uint32_t>(ib, 3);
    chunk(ib, "vidx", Bytes{1});
    put<uint32_t>(nidx, uint32_t(ix.size()));
    chunk(ib, "nidx", nidx);
    for (uint16_t i : ix) put<uint16_t>(idata, i);
    chunk(ib, "data", idata);
    payload_at(isrc, "INDX", at);
    iout = wrap(isrc, at, "INDX", ib);
}

std::string folder_of(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? std::string() : p.substr(0, s + 1); }
std::string file_of(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? p : p.substr(s + 1); }
std::string stem_of(const std::string& f) { std::string n = file_of(f); size_t d = n.rfind('.'); return d == std::string::npos ? n : n.substr(0, d); }
std::string ext_of(const std::string& f) { std::string n = file_of(f); size_t d = n.rfind('.'); return d == std::string::npos ? std::string() : n.substr(d); }
std::string cut_name(const std::string& file, const Rect& r) { return "MA2SE_" + stem_of(file) + "_c" + tag(r) + ext_of(file); }

// The game's INI reader (mgIO IniFile, 0x0025F000) ends a line ONLY at '\r' (it writes a NUL there and skips two
// bytes). A text file with bare '\n' line ends is one long line to it: every section and key after the first is lost. Retail
// .road / .node / .mesh files use "\r\n", and the cut copies must too. (A cut .road read as one line has no [attachments]
// nPoints; the game's loop (0x000D5508) then runs on an unset count that holds the CRC of the road's own path, so the map
// loaded or ran out of memory depending only on the file name.)
const char* const kEol = "\r\n";
std::vector<std::string> text_lines(const std::string& t) {   // the lines without their ends; no extra empty line after a final end
    std::vector<std::string> v = split(t, '\n');
    if (!v.empty() && v.back().empty() && !t.empty() && t.back() == '\n') v.pop_back();
    for (auto& l : v)
        if (!l.empty() && l.back() == '\r') l.pop_back();
    return v;
}

std::string attr(const std::string& x, size_t from, size_t to, const char* name) {
    std::string key = std::string(" ") + name + "=\"";
    size_t a = x.find(key, from);
    if (a == std::string::npos || a >= to) return {};
    a += key.size();
    size_t b = x.find('"', a);
    return b == std::string::npos || b > to ? std::string() : x.substr(a, b - a);
}

void set_attr(std::string& x, size_t from, size_t to, const char* name, const std::string& val) {
    std::string key = std::string(" ") + name + "=\"";
    size_t a = x.find(key, from);
    if (a == std::string::npos || a >= to) return;
    a += key.size();
    size_t b = x.find('"', a);
    if (b == std::string::npos || b > to) return;
    x.replace(a, b - a, val);
}

// Node -> model transforms of the model's meshes, by mesh name (lower case).
std::map<std::string, std::vector<float>> mesh_transforms(const ma2m::ArchiveSet& ar, const std::string& canon) {
    std::map<std::string, std::vector<float>> m;
    ma2m::ModelLoadOptions opt;
    opt.include_invisible = true;
    opt.apply_mtb = false;
    ma2m::Model model;
    if (ma2m::LoadModel(ar, canon, model, opt, nullptr))
        for (auto& sm : model.meshes) m.emplace(lower(sm.name), std::vector<float>(sm.transform, sm.transform + 16));
    return m;
}

bool cut_mesh(const Bytes& vb, const Bytes& ib, bool strip, const float* m, const Rect& r, Bytes& vo, Bytes& io, std::vector<Vert>* kept,
              std::string* err) {
    Geo g;
    if (!read_geo(vb, ib, strip, g, err)) return false;
    clip(g.v, g.tris, rect_planes(r, m));
    if (kept) *kept = g.v;
    write_geo(vb, ib, g, strip, vo, io);
    return true;
}

}  // namespace

// ----------------------------------------------------------------------------- collision volumes

namespace {

struct M4 { float m[16]; };  // row vectors: p' = p * M, translation in m[12..14] (the loader's convention)
M4 ident() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; }
M4 mul(const M4& a, const M4& b) {  // p * a * b
    M4 o{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a.m[i * 4 + k] * b.m[k * 4 + j];
            o.m[i * 4 + j] = v;
        }
    return o;
}
bool parse3(const std::string& s, float* v) { return sscanf(s.c_str(), "%f, %f, %f", &v[0], &v[1], &v[2]) == 3; }
// A node's own matrix from position / rot_axis / rot_angle / scale (scale, then rotation about the axis, then position).
M4 node_matrix(const std::string& x, size_t from, size_t to) {
    float pos[3] = {0, 0, 0}, ax[3] = {1, 0, 0}, sc[3] = {1, 1, 1};
    parse3(attr(x, from, to, "position"), pos);
    parse3(attr(x, from, to, "rot_axis"), ax);
    parse3(attr(x, from, to, "scale"), sc);
    const float ang = float(atof(attr(x, from, to, "rot_angle").c_str()));
    const float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    M4 r = ident();
    if (l > 1e-6f && std::fabs(ang) > 1e-7f) {
        const float ux = ax[0] / l, uy = ax[1] / l, uz = ax[2] / l, c = std::cos(ang), s = std::sin(ang), t = 1 - c;
        const float R[3][3] = {{t * ux * ux + c, t * ux * uy - s * uz, t * ux * uz + s * uy},
                               {t * ux * uy + s * uz, t * uy * uy + c, t * uy * uz - s * ux},
                               {t * ux * uz - s * uy, t * uy * uz + s * ux, t * uz * uz + c}};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[j * 4 + i] = R[i][j];  // column-vector R -> row-vector M
    }
    M4 s3 = ident();
    for (int k = 0; k < 3; k++) s3.m[k * 5] = sc[k];
    M4 o = mul(s3, r);
    for (int k = 0; k < 3; k++) o.m[12 + k] = pos[k];
    return o;
}
void xf_point(const M4& a, const float* p, float* o) {
    for (int j = 0; j < 3; j++) o[j] = p[0] * a.m[j] + p[1] * a.m[4 + j] + p[2] * a.m[8 + j] + a.m[12 + j];
}
void xf_dir(const M4& a, const float* p, float* o) {
    for (int j = 0; j < 3; j++) o[j] = p[0] * a.m[j] + p[1] * a.m[4 + j] + p[2] * a.m[8 + j];
}
bool inverse(const M4& a, M4& o) {  // affine, row vectors
    const float* m = a.m;
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    if (std::fabs(det) < 1e-12f) return false;
    const float id = 1.f / det;
    o = ident();
    o.m[0] = (m[5] * m[10] - m[6] * m[9]) * id;  o.m[1] = (m[2] * m[9] - m[1] * m[10]) * id; o.m[2] = (m[1] * m[6] - m[2] * m[5]) * id;
    o.m[4] = (m[6] * m[8] - m[4] * m[10]) * id;  o.m[5] = (m[0] * m[10] - m[2] * m[8]) * id; o.m[6] = (m[2] * m[4] - m[0] * m[6]) * id;
    o.m[8] = (m[4] * m[9] - m[5] * m[8]) * id;   o.m[9] = (m[1] * m[8] - m[0] * m[9]) * id;  o.m[10] = (m[0] * m[5] - m[1] * m[4]) * id;
    for (int j = 0; j < 3; j++) o.m[12 + j] = -(m[12] * o.m[j] + m[13] * o.m[4 + j] + m[14] * o.m[8 + j]);
    return true;
}

const float kBoxNothing = 0.005f;

// One box in model space: centre, three unit axes and half lengths along them.
struct Box { float c[3], ax[3][3], len[3]; };

// true when the box changed; gone = nothing of it is inside the rectangle (it is then a 5 mm box inside the rectangle).
bool clip_box(Box& b, const Rect& r, bool& gone) {
    gone = false;
    auto vanish = [&]() {
        gone = true;
        float x = b.c[0], z = b.c[2];
        clamp_point(r, x, z);
        b.c[0] = x;
        b.c[2] = z;
        for (int k = 0; k < 3; k++) b.len[k] = kBoxNothing;
        return true;
    };
    // Axis-aligned: every box axis lies along a model axis (any order or sign), so the box is its model AABB.
    int dom[3] = {0, 1, 2};
    bool aligned = true;
    int used = 0;
    for (int k = 0; k < 3 && aligned; k++) {
        int best = 0;
        for (int a = 1; a < 3; a++) if (std::fabs(b.ax[k][a]) > std::fabs(b.ax[k][best])) best = a;
        dom[k] = best;
        aligned = std::fabs(b.ax[k][best]) >= 0.999f && !(used & (1 << best));
        used |= 1 << best;
    }
    if (aligned) {
        float e[3];
        for (int k = 0; k < 3; k++) e[dom[k]] = b.len[k];
        const float lo_x = std::max(b.c[0] - e[0], r.x0), hi_x = std::min(b.c[0] + e[0], r.x1);
        const float lo_z = std::max(b.c[2] - e[2], r.z0), hi_z = std::min(b.c[2] + e[2], r.z1);
        if (hi_x <= lo_x || hi_z <= lo_z) return vanish();
        const bool same = std::fabs(lo_x - (b.c[0] - e[0])) < 1e-4f && std::fabs(hi_x - (b.c[0] + e[0])) < 1e-4f &&
                          std::fabs(lo_z - (b.c[2] - e[2])) < 1e-4f && std::fabs(hi_z - (b.c[2] + e[2])) < 1e-4f;
        if (same) return false;
        b.c[0] = 0.5f * (lo_x + hi_x);
        b.c[2] = 0.5f * (lo_z + hi_z);
        e[0] = 0.5f * (hi_x - lo_x);
        e[2] = 0.5f * (hi_z - lo_z);
        for (int k = 0; k < 3; k++) b.len[k] = e[dom[k]];
        return true;
    }
    // Turned box: 9 x 9 x 9 points through it; the ones inside the rectangle give the new extent along each box axis.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    int inside = 0, total = 0;
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++)
            for (int k = 0; k < 9; k++) {
                const float t[3] = {(float(i) - 4.f) / 4.f * b.len[0], (float(j) - 4.f) / 4.f * b.len[1], (float(k) - 4.f) / 4.f * b.len[2]};
                float p[3];
                for (int a = 0; a < 3; a++) p[a] = b.c[a] + t[0] * b.ax[0][a] + t[1] * b.ax[1][a] + t[2] * b.ax[2][a];
                total++;
                if (p[0] < r.x0 - 0.01f || p[0] > r.x1 + 0.01f || p[2] < r.z0 - 0.01f || p[2] > r.z1 + 0.01f) continue;
                inside++;
                for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], t[a]); hi[a] = std::max(hi[a], t[a]); }
            }
    if (inside == total) return false;
    if (!inside) return vanish();
    for (int a = 0; a < 3; a++) {
        const float mid = 0.5f * (lo[a] + hi[a]);
        for (int q = 0; q < 3; q++) b.c[q] += mid * b.ax[a][q];
        b.len[a] = std::max(0.5f * (hi[a] - lo[a]), kBoxNothing);
    }
    return true;
}

}  // namespace

std::string clip_volumes(const std::string& xml, const Rect& r) {
    std::string x = xml;
    // Walk the nodes in order, keeping each open node's node -> model matrix; a bounding volume belongs to the innermost one
    // (the root, identity, in every retail model seen; nested ones are handled).
    std::vector<M4> stack{ident()};
    size_t p = 0;
    while ((p = x.find('<', p)) != std::string::npos) {
        if (x.compare(p, 6, "<node_") == 0) {
            const size_t e = x.find('>', p);
            if (e == std::string::npos) break;
            if (!(e > 0 && x[e - 1] == '/')) stack.push_back(mul(node_matrix(x, p, e), stack.back()));
            p = e;
        } else if (x.compare(p, 7, "</node_") == 0) {
            if (stack.size() > 1) stack.pop_back();
        } else if (x.compare(p, 7, "<bv_obb") == 0) {
            const size_t e = x.find('>', p);
            if (e == std::string::npos) break;
            const M4& M = stack.back();
            M4 Minv;
            const char* names[3] = {"xaxis", "yaxis", "zaxis"};
            float o[3], len[3], axn[3][3];
            bool ok = parse3(attr(x, p, e, "origin"), o) && parse3(attr(x, p, e, "lengths"), len) && inverse(M, Minv);
            for (int k = 0; k < 3 && ok; k++) ok = ok && parse3(attr(x, p, e, names[k]), axn[k]);
            if (ok) {
                Box b{};
                float stretch[3];
                xf_point(M, o, b.c);
                for (int k = 0; k < 3; k++) {  // axes into model space; a node scale stretches the half length along its axis
                    xf_dir(M, axn[k], b.ax[k]);
                    const float l = std::sqrt(b.ax[k][0] * b.ax[k][0] + b.ax[k][1] * b.ax[k][1] + b.ax[k][2] * b.ax[k][2]);
                    stretch[k] = l > 1e-8f ? l : 1.f;
                    b.len[k] = len[k] * stretch[k];
                    for (int a = 0; a < 3; a++) b.ax[k][a] /= stretch[k];
                }
                bool gone = false;
                if (clip_box(b, r, gone)) {
                    float oo[3];
                    xf_point(Minv, b.c, oo);
                    char no[160], nl[160];
                    snprintf(no, sizeof no, "%.8f, %.8f, %.8f", oo[0], oo[1], oo[2]);
                    snprintf(nl, sizeof nl, "%.8f, %.8f, %.8f", b.len[0] / stretch[0], b.len[1] / stretch[1], b.len[2] / stretch[2]);
                    set_attr(x, p, e, "origin", no);
                    set_attr(x, p, x.find('>', p), "lengths", nl);
                }
            }
            p = x.find('>', p);
            if (p == std::string::npos) break;
        }
        p++;
    }
    return x;
}

// ----------------------------------------------------------------------------- models

bool make_model(const ma2m::ArchiveSet& ar, const std::string& model, const Rect& r, std::vector<OutFile>& out, std::string& new_model,
                std::string* err) {
    std::string canon = ma2m::ResolveModelPath(ar, ma2m::NormalizePath(model));
    if (canon.empty()) { if (err) *err = "model " + model + " is not on the disc"; return false; }
    Bytes src;
    if (!ar.Read(canon, src)) { if (err) *err = "cannot read " + canon; return false; }
    std::string x(src.begin(), src.end());
    const auto tf = mesh_transforms(ar, canon);
    auto transform = [&](const std::string& name) -> const float* {
        auto it = tf.find(lower(name));
        return it == tf.end() ? nullptr : it->second.data();
    };
    new_model = folder_of(canon) + cut_name(canon, r);
    if (iends_with(canon, ".mgmodel")) {
        if (x.find("<shadow_mesh") != std::string::npos) { if (err) *err = "this model has shadow volumes and cannot be cut"; return false; }
        for (size_t p = x.find("<meshxml"); p != std::string::npos; p = x.find("<meshxml", p + 1)) {
            size_t e = x.find('>', p);
            if (e == std::string::npos) break;
            std::string name = attr(x, p, e, "name"), vn = attr(x, p, e, "vertices"), in = attr(x, p, e, "indices");
            bool strip = lower(attr(x, p, e, "type")) == "indexedstrip";
            Bytes vb, ib, vo, io;
            if (!ar.Read(canon + "{" + vn + "}", vb) || !ar.Read(canon + "{" + in + "}", ib)) { if (err) *err = "missing mesh data of " + canon; return false; }
            if (!cut_mesh(vb, ib, strip, transform(name), r, vo, io, nullptr, err)) return false;
            out.push_back({new_model + "{" + vn + "}", std::move(vo)});
            out.push_back({new_model + "{" + in + "}", std::move(io)});
        }
        // Collision is the <bv_obb> boxes of the nodes' bounding volumes: each is clipped on its own (clip_volumes).
        x = clip_volumes(x, r);
        out.push_back({new_model, Bytes(x.begin(), x.end())});
        return true;
    }
    // Legacy text node (.NODE / _rw.node): every "mesh = ..." line names a .mesh (INI: vertices, faces, material, type).
    std::string nx;
    for (auto& l : text_lines(x)) {
        std::string code = trim(l.substr(0, l.find(';')));
        size_t eq = code.find('=');
        if (eq == std::string::npos || lower(trim(code.substr(0, eq))) != "mesh") { nx += l + kEol; continue; }
        std::string val = trim(code.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        std::string mesh = ar.CanonicalPath(ma2m::ResolveRelative(canon, val));
        Bytes mb;
        if (mesh.empty() || !ar.Read(mesh, mb)) { if (err) *err = "missing mesh " + val + " of " + canon; return false; }
        std::string mt(mb.begin(), mb.end()), vrel, frel, type, mname;
        for (auto& ml : split(mt, '\n')) {
            std::string c = trim(ml.substr(0, ml.find(';')));
            size_t q = c.find('=');
            if (q == std::string::npos) continue;
            std::string k = lower(trim(c.substr(0, q))), v = trim(c.substr(q + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            if (k == "vertices") vrel = v;
            else if (k == "faces") frel = v;
            else if (k == "type") type = lower(v);
            else if (k == "name") mname = v;
        }
        if (mname.empty()) mname = stem_of(mesh);
        Bytes vb, ib, vo, io;
        std::string vpath = ma2m::ResolveRelative(mesh, vrel), fpath = ma2m::ResolveRelative(mesh, frel);
        if (!ar.Read(vpath, vb) || !ar.Read(fpath, ib)) { if (err) *err = "missing geometry of " + mesh; return false; }
        if (!cut_mesh(vb, ib, type == "indexedstrip", transform(mname), r, vo, io, nullptr, err)) return false;
        std::string vnew = cut_name(vrel, r), fnew = cut_name(frel, r), mnew = folder_of(mesh) + cut_name(mesh, r);
        out.push_back({folder_of(ar.CanonicalPath(vpath).empty() ? vpath : ar.CanonicalPath(vpath)) + vnew, std::move(vo)});
        out.push_back({folder_of(ar.CanonicalPath(fpath).empty() ? fpath : ar.CanonicalPath(fpath)) + fnew, std::move(io)});
        // The .mesh copy names the cut files (same folders, so its material path still holds).
        std::string mo;
        for (auto& ml : text_lines(mt)) {
            std::string c = trim(ml.substr(0, ml.find(';')));
            size_t q = c.find('=');
            std::string k = q == std::string::npos ? std::string() : lower(trim(c.substr(0, q)));
            if (k == "vertices") mo += "vertices   = \"" + folder_of(vrel) + vnew + "\"" + kEol;
            else if (k == "faces") mo += "faces      = \"" + folder_of(frel) + fnew + "\"" + kEol;
            else mo += ml + kEol;
        }
        out.push_back({mnew, Bytes(mo.begin(), mo.end())});
        nx += "mesh = \"" + folder_of(val) + cut_name(val, r) + "\"" + kEol;
    }
    out.push_back({new_model, Bytes(nx.begin(), nx.end())});
    return true;
}

// ----------------------------------------------------------------------------- what can be cut

namespace {

// "" when the editor can rewrite this vertex file, else the reason (the tail of "its model ...").
std::string layout_refusal(const Bytes& vb) {
    size_t at, info, data;
    uint32_t li, ld;
    if (!payload_at(vb, "VERS", at)) return "has a vertex file the editor cannot read";
    const size_t end = std::min<size_t>(vb.size(), at + rd<uint32_t>(vb.data() + at + 8));
    if (!find_sub(vb, at + 16, end, "info", info, li) || li < 36 || !find_sub(vb, at + 16, end, "data", data, ld)) return "has a vertex file the editor cannot read";
    const uint32_t flags = rd<uint32_t>(vb.data() + info + 8);
    const uint32_t known = 0x1 | 0x2 | 0x8 | 0x100 | 0x200 | 0x400 | 0x1000 | 0x2000 | 0x100000;
    if (flags & 0x80000) return "has moving (skinned) parts";
    if ((flags & ~known) || !(flags & 1)) return "has a vertex layout the editor cannot rewrite";
    return {};
}

}  // namespace

bool cuttable(const ma2m::ArchiveSet& ar, const std::string& model, std::string* why) {
    auto no = [&](const std::string& w) { if (why) *why = w; return false; };
    const std::string canon = ma2m::ResolveModelPath(ar, ma2m::NormalizePath(model));
    Bytes src;
    if (canon.empty() || !ar.Read(canon, src)) return no("is not in the game's files");
    const std::string x(src.begin(), src.end());
    if (iends_with(canon, ".mgmodel")) {
        if (x.find("<shadow_mesh") != std::string::npos) return no("has shadow volumes");
        for (size_t p = x.find("<meshxml"); p != std::string::npos; p = x.find("<meshxml", p + 1)) {
            const size_t e = x.find('>', p);
            if (e == std::string::npos) break;
            Bytes vb, ib;
            if (!ar.Read(canon + "{" + attr(x, p, e, "vertices") + "}", vb) || !ar.Read(canon + "{" + attr(x, p, e, "indices") + "}", ib)) return no("is not in the game's files");
            if (std::string r = layout_refusal(vb); !r.empty()) return no(r);
        }
        return true;
    }
    for (auto& line : split(x, '\n')) {
        std::string code = trim(line.substr(0, line.find(';')));
        const size_t eq = code.find('=');
        if (eq == std::string::npos || lower(trim(code.substr(0, eq))) != "mesh") continue;
        std::string val = trim(code.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        const std::string mesh = ar.CanonicalPath(ma2m::ResolveRelative(canon, val));
        Bytes mb, vb;
        if (mesh.empty() || !ar.Read(mesh, mb)) return no("is not in the game's files");
        std::string vrel;
        for (auto& ml : split(std::string(mb.begin(), mb.end()), '\n')) {
            const std::string c = trim(ml.substr(0, ml.find(';')));
            const size_t q = c.find('=');
            if (q == std::string::npos || lower(trim(c.substr(0, q))) != "vertices") continue;
            vrel = trim(c.substr(q + 1));
            if (vrel.size() >= 2 && vrel.front() == '"' && vrel.back() == '"') vrel = vrel.substr(1, vrel.size() - 2);
        }
        if (vrel.empty() || !ar.Read(ma2m::ResolveRelative(mesh, vrel), vb)) return no("is not in the game's files");
        if (std::string r = layout_refusal(vb); !r.empty()) return no(r);
    }
    return true;
}

bool make_road(const ma2m::ArchiveSet& ar, const std::string& road, const Rect& r, std::vector<OutFile>& out, std::string& new_road,
               std::string* err) {
    std::string canon = ar.CanonicalPath(ma2m::NormalizePath(road));
    Bytes src;
    if (canon.empty() || !ar.Read(canon, src)) { if (err) *err = "road piece " + road + " is not on the disc"; return false; }
    std::string text(src.begin(), src.end()), outt, section;
    auto fmt3 = [](float a, float b, float c) {
        char s[96];
        snprintf(s, sizeof s, "%f, %f, %f", a, b, c);
        return std::string(s);
    };
    for (auto& l : text_lines(text)) {
        std::string code = trim(l.substr(0, l.find(';')));
        if (!code.empty() && code.front() == '[') { section = lower(code); outt += l + kEol; continue; }
        size_t eq = code.find('=');
        if (eq == std::string::npos) { outt += l + kEol; continue; }
        std::string k = lower(trim(code.substr(0, eq))), v = trim(code.substr(eq + 1));
        float p[3];
        if (section == "[model]" && k == "model") {
            std::string model = ma2m::ResolveRelative(canon, v), cut;
            if (!make_model(ar, model, r, out, cut, err)) return false;
            std::string rel = v.substr(0, v.find_last_of("/\\") + 1) + file_of(cut);
            outt += "model\t  = " + rel + kEol;
        } else if (k == "point" && sscanf(v.c_str(), "%f, %f, %f", &p[0], &p[1], &p[2]) == 3 && (section == "[attachments]" || section == "[roads]")) {
            // Attachments sit on the cut edge; AI path points (sidewalks, lanes) stay a little inside it.
            clamp_point(r, p[0], p[2], section == "[roads]" ? 0.3f : 0.f);
            outt += "point\t  = " + fmt3(p[0], p[1], p[2]) + kEol;
        } else if ((k == "point1" || k == "point2") && sscanf(v.c_str(), "%f, %f, %f", &p[0], &p[1], &p[2]) == 3) {
            clamp_point(r, p[0], p[2]);
            outt += k + "\t  = " + fmt3(p[0], p[1], p[2]) + kEol;
        } else if (section == "[params]" && (k == "fwidth" || k == "flength")) {
            char s[32];
            snprintf(s, sizeof s, "%f", k == "fwidth" ? r.w() : r.l());
            outt += trim(code.substr(0, eq)) + " = " + s + kEol;
        } else {
            outt += l + kEol;
        }
    }
    new_road = folder_of(canon) + cut_name(canon, r);
    out.push_back({new_road, Bytes(outt.begin(), outt.end())});
    return true;
}

}  // namespace ma2::crop
