// MechAssault 2 world files (.wdf): a flat list of property-bag objects that form a tree through
// UID / parent / child (first child) / sibling (next sibling) u32 properties.
//
//   file header : 5 x u32, [0] = 0xFF002F00, [2] = object count
//   object      : u32 0xFF012F01, u32 property count, u32 class key
//   property    : u32 0xFF022F02, u32 type, u32 key, u32 size, payload
// Keys are crc_acc(name) (CRC-32 without the final complement). Retail objects keep their
// properties sorted by key. Reader and writer are lossless: unknown types/keys stay verbatim.
#pragma once
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "util.h"

namespace ma2 {

enum PropType : uint32_t {
    PT_I8 = 1, PT_I16 = 2, PT_I32 = 3, PT_U8 = 4, PT_U16 = 5, PT_U32 = 6, PT_F32 = 7, PT_F64 = 8,
    PT_STRING = 9, PT_VEC2 = 10, PT_VEC3 = 11, PT_COLOR = 12, PT_BLOB = 13, PT_U8B = 16,
};
const char* prop_type_name(uint32_t type);

struct Vec3 { float x = 0, y = 0, z = 0; };

struct WProp {
    uint32_t type = 0;
    uint32_t key = 0;
    Bytes data;

    int64_t as_int() const;          // integer types (and colour); 0 for others
    double as_float() const;         // numeric types
    std::string as_string() const;   // type 9 (up to the first NUL)
    Vec3 as_vec3() const;
    bool numeric() const;
    void set_int(int64_t v);         // keeps type and width
    void set_float(double v);
    void set_string(std::string_view s);
    void set_vec3(Vec3 v);
    std::string display() const;     // short human text for the inspector
};

struct WObj {
    uint32_t cls = 0;
    std::vector<WProp> props;

    WProp* find(uint32_t key);
    const WProp* find(uint32_t key) const;
    WProp* find(std::string_view name) { return find(key_of(name)); }
    const WProp* find(std::string_view name) const { return find(key_of(name)); }
    // Insert (sorted by key) or overwrite.
    WProp& set(uint32_t key, uint32_t type, Bytes data);
    bool erase(uint32_t key);

    uint32_t u32(std::string_view name, uint32_t def = 0) const;
    int32_t i32(std::string_view name, int32_t def = 0) const;
    float f32(std::string_view name, float def = 0) const;
    std::string str(std::string_view name) const;
    Vec3 vec3(std::string_view name) const;
    // Setters keep an existing property's type; a new one gets the given type.
    void set_u32(std::string_view name, uint32_t v, uint32_t type = PT_U32);
    void set_i32(std::string_view name, int32_t v, uint32_t type = PT_I32);
    void set_f32(std::string_view name, float v);
    void set_str(std::string_view name, std::string_view v);
    void set_vec3(std::string_view name, Vec3 v);

    uint32_t uid() const { return u32("UID"); }
    uint32_t parent() const { return u32("parent"); }
    uint32_t child() const { return u32("child"); }
    uint32_t sibling() const { return u32("sibling"); }
    uint32_t template_id() const { return u32("TemplateID"); }
    std::string description() const { return str("Description"); }
};

struct Wdf {
    std::array<uint32_t, 5> header{0xFF002F00u, 1475, 0, 1, 1};
    std::vector<WObj> objs;

    bool parse(const uint8_t* data, size_t size, std::string* err = nullptr);
    bool parse(const Bytes& b, std::string* err = nullptr) { return parse(b.data(), b.size(), err); }
    Bytes serialize() const;  // header[2] is written as the object count
};

// Tree view over a Wdf (indices into objs). Rebuild after structural edits.
struct WTree {
    std::unordered_map<uint32_t, int> by_uid;
    std::vector<std::vector<int>> kids;  // children in sibling order
    std::vector<int> parent_of;          // -1 for roots/orphans
    int root = -1;
    uint32_t max_uid = 0;

    void build(const Wdf& w);
    int find(uint32_t uid) const { auto it = by_uid.find(uid); return it == by_uid.end() ? -1 : it->second; }
    void subtree(int index, std::vector<int>& out) const;  // index and all descendants, pre-order
};

// Structural edits that keep parent/child/sibling links consistent.
namespace wedit {
// Appends obj (its UID must be set) as the last child of parent_uid. Returns its index.
int append_child(Wdf& w, uint32_t parent_uid, WObj obj);
// Inserts obj as the first child of parent_uid.
int prepend_child(Wdf& w, uint32_t parent_uid, WObj obj);
// Removes the object and its descendants; relinks the parent's child chain.
void remove_subtree(Wdf& w, uint32_t uid);
// Moves an object (with its subtree) to the end of new_parent's children.
void reparent(Wdf& w, uint32_t uid, uint32_t new_parent_uid);
// Swaps the object (with its subtree) with its previous (dir < 0) or next (dir > 0) sibling. Returns false at the end of the list.
bool move_sibling(Wdf& w, uint32_t uid, int dir);
// Deep copy of a subtree with fresh UIDs (next_uid is advanced); the root's parent/sibling
// links are cleared. Returns the copies in pre-order. Integer properties whose value is a UID
// inside the subtree are remapped too, except keys in keep_keys (template references: world
// and template UIDs share one number space, so a template id can equal a world UID).
std::vector<WObj> clone_subtree(const Wdf& w, uint32_t uid, uint32_t& next_uid,
                                const std::unordered_set<uint32_t>* keep_keys,
                                std::unordered_map<uint32_t, uint32_t>* map_out = nullptr);
// Inserts a pre-order list made by clone_subtree under parent_uid (as its last child).
void insert_subtree(Wdf& w, uint32_t parent_uid, std::vector<WObj> objs);
}  // namespace wedit

}  // namespace ma2
