// Vertex ("{x verts}" / .geomVert) and index ("{x indices}" / .geomFace) files.
//
// Both start with a 32-byte MGIc header (u32 0x00010000, 'MGIc', u32 file
// size, u32 kind: 2 = MGMODEL sub-entry, 1 = legacy file). MGMODEL sub-entries
// then carry an 'XMLB' chunk (16-byte header + "vertbin"/"idxbin" name) before
// the payload chunk; legacy files start the payload chunk at 0x20.
//   VERS chunk: 'VERS', 0, length, version 2, then sub-chunks (tag, length incl. header):
//     'info': u32 3, u32 vertex count, u32 layout flags, float min[3], float max[3]
//     'data': vertex count * stride bytes
//   INDX chunk: 'INDX', 0, length, version 3, then 'vidx' (u8 1), 'nidx' (u32 count),
//     'data' (count * u16)
#include "internal.h"

#include <cmath>

namespace ma2m {
namespace detail {

namespace {

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// Finds the payload chunk (VERS / INDX) after the MGIc header.
bool PayloadChunk(const uint8_t* data, size_t size, const char* tag, size_t& start, size_t& end,
                  std::string* error) {
    if (size < 0x30 || memcmp(data + 4, "MGIc", 4) != 0) {
        if (error) *error = "not an MGIc geometry file";
        return false;
    }
    size_t off = 0x20;
    if (!memcmp(data + off, "XMLB", 4)) {
        uint32_t len = U32(data + off + 8);
        if (len < 16 || off + len + 16 > size) {
            if (error) *error = "bad XMLB chunk";
            return false;
        }
        off += len;
    }
    if (off + 16 > size || memcmp(data + off, tag, 4) != 0) {
        if (error) *error = std::string("missing ") + tag + " chunk";
        return false;
    }
    uint32_t len = U32(data + off + 8);
    if (len < 16 || off + len > size) len = (uint32_t)(size - off);
    start = off + 16;
    end = off + len;
    return true;
}

bool FindSub(const uint8_t* data, size_t start, size_t end, const char* tag, const uint8_t*& payload,
             uint32_t& payload_len) {
    size_t p = start;
    while (p + 8 <= end) {
        uint32_t len = U32(data + p + 4);
        if (len < 8 || p + len > end) return false;
        if (!memcmp(data + p, tag, 4)) {
            payload = data + p + 8;
            payload_len = len - 8;
            return true;
        }
        p += len;
    }
    return false;
}

inline float SignExtend(uint32_t v, int bits, float scale) {
    int32_t s = (int32_t)(v << (32 - bits)) >> (32 - bits);
    return (float)s / scale;
}

}  // namespace

// Layout flags (see NOTES.md):
//   0x000001 position float3            0x000002 normal float3
//   0x000100 normal NORMPACKED3 (11:11:10) 0x000008 diffuse D3DCOLOR
//   0x100000 tangent float4             0x000200 one short2 UV (value/1024)
//   0x000400 two short2 UVs             0x001000 one float2 UV
//   0x002000 two float2 UVs             0x080000 skin: u8 index[4] + float weight[4]
// Stored in that order.
uint32_t VertexStride(uint32_t f, bool* known) {
    const uint32_t kKnown = 0x1 | 0x2 | 0x8 | 0x100 | 0x200 | 0x400 | 0x1000 | 0x2000 | 0x80000 | 0x100000;
    if (known) *known = (f & ~kKnown) == 0;
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

bool DecodeVertexFile(const uint8_t* data, size_t size, VertexBuffer& vb, std::string* error) {
    size_t start, end;
    if (!PayloadChunk(data, size, "VERS", start, end, error)) return false;
    const uint8_t* info;
    const uint8_t* body;
    uint32_t info_len, body_len;
    if (!FindSub(data, start, end, "info", info, info_len) || info_len < 36) {
        if (error) *error = "vertex file: missing info chunk";
        return false;
    }
    if (!FindSub(data, start, end, "data", body, body_len)) {
        if (error) *error = "vertex file: missing data chunk";
        return false;
    }
    vb.count = U32(info + 4);
    vb.flags = U32(info + 8);
    memcpy(vb.bmin, info + 12, 12);
    memcpy(vb.bmax, info + 24, 12);
    if (!vb.count) {
        if (error) *error = "vertex file: zero vertices";
        return false;
    }
    bool known;
    uint32_t stride = VertexStride(vb.flags, &known);
    uint32_t actual = body_len / vb.count;
    if (!known || stride != actual || !(vb.flags & 1)) {
        if (error) {
            char buf[128];
            snprintf(buf, sizeof(buf), "unsupported vertex layout 0x%X (stride %u, expected %u)", vb.flags, actual, stride);
            *error = buf;
        }
        return false;
    }
    vb.stride = stride;
    uint32_t n = vb.count;
    vb.positions.resize((size_t)n * 3);
    bool has_normal = (vb.flags & (0x2 | 0x100)) != 0;
    if (has_normal) vb.normals.resize((size_t)n * 3);
    int uv_sets = (vb.flags & 0x200) ? 1 : (vb.flags & 0x400) ? 2 : (vb.flags & 0x1000) ? 1 : (vb.flags & 0x2000) ? 2 : 0;
    bool uv_short = (vb.flags & (0x200 | 0x400)) != 0;
    if (uv_sets >= 1) vb.uv0.resize((size_t)n * 2);
    if (uv_sets >= 2) vb.uv1.resize((size_t)n * 2);
    if (vb.flags & 0x8) vb.colors.resize(n);
    vb.skinned = (vb.flags & 0x80000) != 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* v = body + (size_t)i * stride;
        memcpy(&vb.positions[(size_t)i * 3], v, 12);
        v += 12;
        if (vb.flags & 0x2) {
            memcpy(&vb.normals[(size_t)i * 3], v, 12);
            v += 12;
        }
        if (vb.flags & 0x100) {
            uint32_t p = U32(v);
            float* nn = &vb.normals[(size_t)i * 3];
            nn[0] = SignExtend(p & 0x7FF, 11, 1023.0f);
            nn[1] = SignExtend((p >> 11) & 0x7FF, 11, 1023.0f);
            nn[2] = SignExtend(p >> 22, 10, 511.0f);
            float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            if (l > 1e-6f) {
                nn[0] /= l;
                nn[1] /= l;
                nn[2] /= l;
            }
            v += 4;
        }
        if (vb.flags & 0x8) {
            uint32_t c = U32(v);  // 0xAARRGGBB
            vb.colors[i] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u);
            v += 4;
        }
        if (vb.flags & 0x100000) v += 16;  // tangent + handedness, unused here
        for (int s = 0; s < uv_sets; s++) {
            float* dst = s == 0 ? &vb.uv0[(size_t)i * 2] : &vb.uv1[(size_t)i * 2];
            if (uv_short) {
                int16_t uv[2];
                memcpy(uv, v, 4);
                dst[0] = uv[0] / 1024.0f;
                dst[1] = uv[1] / 1024.0f;
                v += 4;
            } else {
                memcpy(dst, v, 8);
                v += 8;
            }
        }
        // Skin data (0x80000) follows; bind pose needs none of it.
    }
    return true;
}

bool DecodeIndexFile(const uint8_t* data, size_t size, std::vector<uint16_t>& out, std::string* error) {
    size_t start, end;
    if (!PayloadChunk(data, size, "INDX", start, end, error)) return false;
    const uint8_t* nidx;
    const uint8_t* body;
    uint32_t nidx_len, body_len;
    if (!FindSub(data, start, end, "data", body, body_len)) {
        if (error) *error = "index file: missing data chunk";
        return false;
    }
    uint32_t count = body_len / 2;
    if (FindSub(data, start, end, "nidx", nidx, nidx_len) && nidx_len >= 4) {
        uint32_t n = U32(nidx);
        if (n <= count) count = n;
    }
    out.resize(count);
    if (count) memcpy(out.data(), body, (size_t)count * 2);
    return true;
}

void BuildTriangles(const std::vector<uint16_t>& idx, bool strip, uint32_t vertex_count, std::vector<uint32_t>& out) {
    out.clear();
    auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || a == c) return;
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) return;
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    };
    if (!strip) {
        for (size_t i = 0; i + 2 < idx.size(); i += 3) emit(idx[i], idx[i + 1], idx[i + 2]);
        return;
    }
    size_t run_start = 0;
    for (size_t i = 0; i < idx.size(); i++) {
        if (idx[i] == 0xFFFF) {
            run_start = i + 1;
            continue;
        }
        if (i - run_start < 2) continue;
        size_t k = i - run_start - 2;  // triangle number in this run
        if (k & 1) emit(idx[i - 1], idx[i - 2], idx[i]);
        else emit(idx[i - 2], idx[i - 1], idx[i]);
    }
}

// ---------------------------------------------------------------------------
// Math (row vectors: p' = p * M; translation in m[12..14])
// ---------------------------------------------------------------------------

void MatIdentity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void MatMul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof(r));
}

// Node transform: scale, then rotate by rot_angle (radians) about rot_axis,
// then translate. Angles follow the right-hand rule about rot_axis
// (checked on the Rommel tank treads, 240 deg about (1,1,-1); NOTES.md).
static const float kRotationSign = 1.0f;

void MatFromSRT(const float scale[3], const float axis_in[3], float angle, const float pos[3], float out[16]) {
    float x = axis_in[0], y = axis_in[1], z = axis_in[2];
    float l = std::sqrt(x * x + y * y + z * z);
    if (l < 1e-8f) {
        x = 1;
        y = z = 0;
        angle = 0;
    } else {
        x /= l;
        y /= l;
        z /= l;
    }
    angle *= kRotationSign;
    float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    // Column-vector rotation R; the row-vector matrix is its transpose.
    float R[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                     {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                     {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i * 4 + j] = scale[i] * R[j][i];
        out[i * 4 + 3] = 0;
    }
    out[12] = pos[0];
    out[13] = pos[1];
    out[14] = pos[2];
    out[15] = 1;
}

void TransformPoint(const float m[16], const float p[3], float o[3]) {
    float x = p[0], y = p[1], z = p[2];
    o[0] = x * m[0] + y * m[4] + z * m[8] + m[12];
    o[1] = x * m[1] + y * m[5] + z * m[9] + m[13];
    o[2] = x * m[2] + y * m[6] + z * m[10] + m[14];
}

void TransformDir(const float m[16], const float d[3], float o[3]) {
    float x = d[0], y = d[1], z = d[2];
    o[0] = x * m[0] + y * m[4] + z * m[8];
    o[1] = x * m[1] + y * m[5] + z * m[9];
    o[2] = x * m[2] + y * m[6] + z * m[10];
}

bool MatInvert3x3Transpose(const float m[16], float out[16]) {
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    MatIdentity(out);
    if (std::fabs(det) < 1e-20f) return false;
    float inv = 1.0f / det;
    // inverse = adj / det; we want its transpose (cofactor matrix / det).
    out[0] = A * inv;
    out[1] = B * inv;
    out[2] = C * inv;
    out[4] = -(b * i - c * h) * inv;
    out[5] = (a * i - c * g) * inv;
    out[6] = -(a * h - b * g) * inv;
    out[8] = (b * f - c * e) * inv;
    out[9] = -(a * f - c * d) * inv;
    out[10] = (a * e - b * d) * inv;
    return true;
}

}  // namespace detail
}  // namespace ma2m
