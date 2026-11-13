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
    if (!(q.x1 - q.x0 >= kMinSide * 0.5f) || !(q.z1 - q.z0 >= kMinSide * 0.5f)) return false;
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
        std::map<std::string, std::vector<Vert>> kept;  // per meshxml name: what is left, mesh space
        for (size_t p = x.find("<meshxml"); p != std::string::npos; p = x.find("<meshxml", p + 1)) {
            size_t e = x.find('>', p);
            if (e == std::string::npos) break;
            std::string name = attr(x, p, e, "name"), vn = attr(x, p, e, "vertices"), in = attr(x, p, e, "indices");
            bool strip = lower(attr(x, p, e, "type")) == "indexedstrip";
            Bytes vb, ib, vo, io;
            if (!ar.Read(canon + "{" + vn + "}", vb) || !ar.Read(canon + "{" + in + "}", ib)) { if (err) *err = "missing mesh data of " + canon; return false; }
            if (!cut_mesh(vb, ib, strip, transform(name), r, vo, io, &kept[lower(name)], err)) return false;
            out.push_back({new_model + "{" + vn + "}", std::move(vo)});
            out.push_back({new_model + "{" + in + "}", std::move(io)});
        }
        // Each node's bounding box (bv_obb: half lengths along its own axes, in node space) shrinks to its meshes' kept part.
        for (size_t p = x.find("<bv_obb"); p != std::string::npos; p = x.find("<bv_obb", p + 1)) {
            size_t e = x.find('>', p);
            if (e == std::string::npos) break;
            size_t stop = std::min(x.find("<node_", e), x.find("</node_", e));
            std::vector<const std::vector<Vert>*> meshes;
            for (size_t q = x.find("<mesh ", e); q != std::string::npos && q < stop; q = x.find("<mesh ", q + 1)) {
                auto it = kept.find(lower(attr(x, q, x.find('>', q), "name")));
                if (it != kept.end()) meshes.push_back(&it->second);
            }
            if (meshes.empty()) continue;
            float ax[3][3];
            const char* names[3] = {"xaxis", "yaxis", "zaxis"};
            bool ok = true;
            for (int k = 0; k < 3; k++) ok &= sscanf(attr(x, p, e, names[k]).c_str(), "%f, %f, %f", &ax[k][0], &ax[k][1], &ax[k][2]) == 3;
            if (!ok) continue;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (auto* mv : meshes)
                for (auto& v : *mv)
                    for (int k = 0; k < 3; k++) {
                        float d = v.p[0] * ax[k][0] + v.p[1] * ax[k][1] + v.p[2] * ax[k][2];
                        lo[k] = std::min(lo[k], d);
                        hi[k] = std::max(hi[k], d);
                    }
            if (lo[0] > hi[0]) continue;
            float o[3] = {0, 0, 0}, len[3];
            for (int k = 0; k < 3; k++) {
                float mid = 0.5f * (lo[k] + hi[k]);
                len[k] = 0.5f * (hi[k] - lo[k]);
                for (int c = 0; c < 3; c++) o[c] += mid * ax[k][c];
            }
            char b[160];
            snprintf(b, sizeof b, "%.8f, %.8f, %.8f", o[0], o[1], o[2]);
            set_attr(x, p, e, "origin", b);
            e = x.find('>', p);
            snprintf(b, sizeof b, "%.8f, %.8f, %.8f", len[0], len[1], len[2]);
            set_attr(x, p, e, "lengths", b);
        }
        out.push_back({new_model, Bytes(x.begin(), x.end())});
        return true;
    }
    // Legacy text node (.NODE / _rw.node): every "mesh = ..." line names a .mesh (INI: vertices, faces, material, type).
    std::string nx;
    for (auto& line : split(x, '\n')) {
        std::string l = line;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        std::string code = trim(l.substr(0, l.find(';')));
        size_t eq = code.find('=');
        if (eq == std::string::npos || lower(trim(code.substr(0, eq))) != "mesh") { nx += line + "\n"; continue; }
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
        for (auto& ml : split(mt, '\n')) {
            std::string c = trim(ml.substr(0, ml.find(';')));
            size_t q = c.find('=');
            std::string k = q == std::string::npos ? std::string() : lower(trim(c.substr(0, q)));
            if (k == "vertices") mo += "vertices   = \"" + folder_of(vrel) + vnew + "\"\n";
            else if (k == "faces") mo += "faces      = \"" + folder_of(frel) + fnew + "\"\n";
            else mo += ml + "\n";
        }
        out.push_back({mnew, Bytes(mo.begin(), mo.end())});
        nx += "mesh = \"" + folder_of(val) + cut_name(val, r) + "\"\n";
    }
    out.push_back({new_model, Bytes(nx.begin(), nx.end())});
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
    for (auto& line : split(text, '\n')) {
        std::string l = line;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        std::string code = trim(l.substr(0, l.find(';')));
        if (!code.empty() && code.front() == '[') { section = lower(code); outt += l + "\n"; continue; }
        size_t eq = code.find('=');
        if (eq == std::string::npos) { outt += l + "\n"; continue; }
        std::string k = lower(trim(code.substr(0, eq))), v = trim(code.substr(eq + 1));
        float p[3];
        if (section == "[model]" && k == "model") {
            std::string model = ma2m::ResolveRelative(canon, v), cut;
            if (!make_model(ar, model, r, out, cut, err)) return false;
            std::string rel = v.substr(0, v.find_last_of("/\\") + 1) + file_of(cut);
            outt += "model\t  = " + rel + "\n";
        } else if (k == "point" && sscanf(v.c_str(), "%f, %f, %f", &p[0], &p[1], &p[2]) == 3 && (section == "[attachments]" || section == "[roads]")) {
            // Attachments sit on the cut edge; AI path points (sidewalks, lanes) stay a little inside it.
            clamp_point(r, p[0], p[2], section == "[roads]" ? 0.3f : 0.f);
            outt += "point\t  = " + fmt3(p[0], p[1], p[2]) + "\n";
        } else if ((k == "point1" || k == "point2") && sscanf(v.c_str(), "%f, %f, %f", &p[0], &p[1], &p[2]) == 3) {
            clamp_point(r, p[0], p[2]);
            outt += k + "\t  = " + fmt3(p[0], p[1], p[2]) + "\n";
        } else if (section == "[params]" && (k == "fwidth" || k == "flength")) {
            char s[32];
            snprintf(s, sizeof s, "%f", k == "fwidth" ? r.w() : r.l());
            outt += trim(code.substr(0, eq)) + " = " + s + "\n";
        } else {
            outt += l + "\n";
        }
    }
    new_road = folder_of(canon) + cut_name(canon, r);
    out.push_back({new_road, Bytes(outt.begin(), outt.end())});
    return true;
}

}  // namespace ma2::crop
