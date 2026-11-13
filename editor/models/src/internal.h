// Internal helpers shared by the ma2models sources. Not part of the API.
#pragma once

#include "../ma2models.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ma2m {
namespace detail {

// ---- inflate.cpp ----
// zlib stream (RFC 1950 header + deflate). Returns bytes written or -1.
long ZlibInflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);
// Raw deflate stream. Returns bytes written or -1.
long RawInflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, size_t* consumed = nullptr);
uint32_t Crc32(const void* data, size_t len, uint32_t crc = 0);
uint32_t Adler32(const void* data, size_t len, uint32_t adler = 1);

// ---- text.cpp ----
std::string Lower(std::string s);
bool IEquals(const std::string& a, const std::string& b);
bool EndsWithI(const std::string& s, const std::string& suffix);
bool StartsWithI(const std::string& s, const std::string& prefix);
std::string Trim(const std::string& s);
std::string Unquote(const std::string& s);
// "1.0, 2.0, 3.0" or "1 2 3" -> floats (returns count parsed)
int ParseFloats(const std::string& s, float* out, int max_count);
bool ParseBool(const std::string& s, bool fallback);
std::string FileStem(const std::string& path);    // name without folder and extension
std::string FolderOf(const std::string& path);    // "a/b/c.x" -> "a/b" ("" at root)
std::string FileName(const std::string& path);

// ---- xml.cpp ----  (enough for MGMODEL / mgparticle: elements + attributes)
struct XmlNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    const std::string* Attr(const char* name) const;
    std::string AttrOr(const char* name, const std::string& fallback = std::string()) const;
    const XmlNode* Child(const char* tag) const;
};
bool ParseXml(const char* text, size_t len, XmlNode& root, std::string* error);

// ---- text.cpp: MAXPort INI files (.NODE / .node / .mesh / .mat) ----
struct IniSection {
    std::string name;
    // Keys in file order; repeated keys (bone, child) keep every value.
    std::vector<std::pair<std::string, std::string>> values;
    const std::string* Get(const char* key) const;
    std::string GetOr(const char* key, const std::string& fallback = std::string()) const;
};
void ParseIni(const char* text, size_t len, std::vector<IniSection>& sections);

// ---- wdf.cpp ----
struct WdfProperty {
    uint32_t type = 0, key = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    std::string AsString() const;
    uint32_t AsU32() const;
    int32_t AsI32() const;
    float AsF32() const;
};
struct WdfObject {
    uint32_t class_key = 0;
    std::vector<WdfProperty> props;
    const WdfProperty* Find(uint32_t key) const;
};
// Objects point into `data`, which must outlive them.
bool ParseWdf(const uint8_t* data, size_t size, std::vector<WdfObject>& objects, std::string* error);
const char* KnownWdfName(uint32_t key);   // class/property names this library knows

// ---- geometry.cpp ----
struct VertexBuffer {
    uint32_t flags = 0, count = 0, stride = 0;
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
    std::vector<float> positions, normals, uv0, uv1;
    std::vector<uint32_t> colors;
    bool skinned = false;
};
bool DecodeVertexFile(const uint8_t* data, size_t size, VertexBuffer& out, std::string* error);
bool DecodeIndexFile(const uint8_t* data, size_t size, std::vector<uint16_t>& out, std::string* error);
// Strip -> list (0xFFFF restarts, degenerates dropped), list passthrough.
void BuildTriangles(const std::vector<uint16_t>& idx, bool strip, uint32_t vertex_count,
                    std::vector<uint32_t>& out);
uint32_t VertexStride(uint32_t flags, bool* known);

// ---- math ----
void MatIdentity(float m[16]);
void MatMul(const float a[16], const float b[16], float out[16]);  // out = a * b (row vectors: a first)
void MatFromSRT(const float scale[3], const float axis[3], float angle, const float pos[3], float out[16]);
void TransformPoint(const float m[16], const float p[3], float out[3]);
void TransformDir(const float m[16], const float d[3], float out[3]);  // linear part, no normalize
bool MatInvert3x3Transpose(const float m[16], float out[16]);  // normal matrix

// ---- model.cpp internals used by deps.cpp ----
struct MgmodelRefs {
    std::vector<std::string> entries;   // "{x verts}" / "{x indices}" sibling entries (full names)
    std::vector<std::string> textures;  // resolved texture paths
    std::vector<std::string> files;     // other referenced files (skeleton, anm, lbp)
};

}  // namespace detail
}  // namespace ma2m
