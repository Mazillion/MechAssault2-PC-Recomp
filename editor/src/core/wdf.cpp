#include "wdf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace ma2 {

const char* prop_type_name(uint32_t t) {
    switch (t) {
        case PT_I8: return "i8"; case PT_I16: return "i16"; case PT_I32: return "i32";
        case PT_U8: return "u8"; case PT_U16: return "u16"; case PT_U32: return "u32";
        case PT_F32: return "f32"; case PT_F64: return "f64"; case PT_STRING: return "string";
        case PT_VEC2: return "vec2"; case PT_VEC3: return "vec3"; case PT_COLOR: return "colour";
        case PT_BLOB: return "blob"; case PT_U8B: return "u8b";
        default: return "?";
    }
}

static const uint32_t K_UID = key_of("UID"), K_PARENT = key_of("parent"), K_CHILD = key_of("child"),
                      K_SIBLING = key_of("sibling"), K_TEMPLATE = key_of("TemplateID");

// ---------------------------------------------------------------------------------------------
// WProp

bool WProp::numeric() const {
    switch (type) {
        case PT_I8: case PT_I16: case PT_I32: case PT_U8: case PT_U16: case PT_U32: case PT_COLOR: case PT_U8B:
            return true;
        case PT_F32: return data.size() == 4;
        case PT_F64: return data.size() == 8;
        default: return false;
    }
}

int64_t WProp::as_int() const {
    const uint8_t* p = data.data();
    switch (type) {
        case PT_I8: return data.size() >= 1 ? int8_t(p[0]) : 0;
        case PT_I16: return data.size() >= 2 ? rd<int16_t>(p) : 0;
        case PT_I32: return data.size() >= 4 ? rd<int32_t>(p) : 0;
        case PT_U8: case PT_U8B: return data.size() >= 1 ? p[0] : 0;
        case PT_U16: return data.size() >= 2 ? rd<uint16_t>(p) : 0;
        case PT_U32: case PT_COLOR: return data.size() >= 4 ? rd<uint32_t>(p) : 0;
        case PT_F32: return data.size() >= 4 ? int64_t(rd<float>(p)) : 0;
        case PT_F64: return data.size() >= 8 ? int64_t(rd<double>(p)) : 0;
        default: return 0;
    }
}

double WProp::as_float() const {
    if (type == PT_F32 && data.size() >= 4) return rd<float>(data.data());
    if (type == PT_F64 && data.size() >= 8) return rd<double>(data.data());
    return double(as_int());
}

std::string WProp::as_string() const {
    if (type != PT_STRING) return {};
    size_t n = 0;
    while (n < data.size() && data[n]) n++;
    return std::string(reinterpret_cast<const char*>(data.data()), n);
}

Vec3 WProp::as_vec3() const {
    Vec3 v;
    if (data.size() >= 12) { v.x = rd<float>(&data[0]); v.y = rd<float>(&data[4]); v.z = rd<float>(&data[8]); }
    else if (data.size() >= 8) { v.x = rd<float>(&data[0]); v.y = rd<float>(&data[4]); }
    return v;
}

void WProp::set_int(int64_t v) {
    switch (type) {
        case PT_I8: case PT_U8: case PT_U8B: data.assign(1, uint8_t(v)); break;
        case PT_I16: case PT_U16: data.resize(2); wr<uint16_t>(data.data(), uint16_t(v)); break;
        case PT_F32: data.resize(4); wr<float>(data.data(), float(v)); break;
        case PT_F64: data.resize(8); wr<double>(data.data(), double(v)); break;
        default: data.resize(4); wr<uint32_t>(data.data(), uint32_t(v)); break;
    }
}

void WProp::set_float(double v) {
    if (type == PT_F64) { data.resize(8); wr<double>(data.data(), v); }
    else if (type == PT_F32) { data.resize(4); wr<float>(data.data(), float(v)); }
    else set_int(int64_t(std::llround(v)));
}

void WProp::set_string(std::string_view s) {
    data.assign(s.begin(), s.end());
    data.push_back(0);
}

void WProp::set_vec3(Vec3 v) {
    data.resize(12);
    wr<float>(&data[0], v.x); wr<float>(&data[4], v.y); wr<float>(&data[8], v.z);
}

std::string WProp::display() const {
    char buf[160];
    switch (type) {
        case PT_I8: case PT_I16: case PT_I32: case PT_U8: case PT_U16: case PT_U32: case PT_U8B:
            snprintf(buf, sizeof buf, "%lld", (long long)as_int()); return buf;
        case PT_COLOR: snprintf(buf, sizeof buf, "#%08X", uint32_t(as_int())); return buf;
        case PT_F32: case PT_F64: snprintf(buf, sizeof buf, "%g", as_float()); return buf;
        case PT_STRING: return "\"" + as_string() + "\"";
        case PT_VEC2: { Vec3 v = as_vec3(); snprintf(buf, sizeof buf, "(%g, %g)", v.x, v.y); return buf; }
        case PT_VEC3: { Vec3 v = as_vec3(); snprintf(buf, sizeof buf, "(%g, %g, %g)", v.x, v.y, v.z); return buf; }
        default: {
            std::string s = std::to_string(data.size()) + " bytes:";
            for (size_t i = 0; i < data.size() && i < 16; i++) { snprintf(buf, sizeof buf, " %02X", data[i]); s += buf; }
            if (data.size() > 16) s += " ...";
            return s;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// WObj

WProp* WObj::find(uint32_t key) {
    auto it = std::lower_bound(props.begin(), props.end(), key, [](const WProp& p, uint32_t k) { return p.key < k; });
    if (it != props.end() && it->key == key) return &*it;
    for (auto& p : props) if (p.key == key) return &p;  // unsorted input
    return nullptr;
}
const WProp* WObj::find(uint32_t key) const { return const_cast<WObj*>(this)->find(key); }

WProp& WObj::set(uint32_t key, uint32_t type, Bytes data) {
    if (WProp* p = find(key)) { p->type = type; p->data = std::move(data); return *p; }
    auto it = std::lower_bound(props.begin(), props.end(), key, [](const WProp& p, uint32_t k) { return p.key < k; });
    WProp np;
    np.type = type; np.key = key; np.data = std::move(data);
    return *props.insert(it, std::move(np));
}

bool WObj::erase(uint32_t key) {
    for (auto it = props.begin(); it != props.end(); ++it) if (it->key == key) { props.erase(it); return true; }
    return false;
}

uint32_t WObj::u32(std::string_view n, uint32_t def) const { const WProp* p = find(n); return p ? uint32_t(p->as_int()) : def; }
int32_t WObj::i32(std::string_view n, int32_t def) const { const WProp* p = find(n); return p ? int32_t(p->as_int()) : def; }
float WObj::f32(std::string_view n, float def) const { const WProp* p = find(n); return p ? float(p->as_float()) : def; }
std::string WObj::str(std::string_view n) const { const WProp* p = find(n); return p ? p->as_string() : std::string(); }
Vec3 WObj::vec3(std::string_view n) const { const WProp* p = find(n); return p ? p->as_vec3() : Vec3{}; }

void WObj::set_u32(std::string_view n, uint32_t v, uint32_t type) {
    if (WProp* p = find(n)) { p->set_int(v); return; }
    WProp tmp; tmp.type = type; tmp.set_int(v);
    set(key_of(n), type, tmp.data);
}
void WObj::set_i32(std::string_view n, int32_t v, uint32_t type) { set_u32(n, uint32_t(v), type); }
void WObj::set_f32(std::string_view n, float v) {
    if (WProp* p = find(n)) { p->set_float(v); return; }
    Bytes b(4); wr<float>(b.data(), v); set(key_of(n), PT_F32, b);
}
void WObj::set_str(std::string_view n, std::string_view v) {
    Bytes b(v.begin(), v.end()); b.push_back(0);
    if (WProp* p = find(n)) { p->data = b; return; }
    set(key_of(n), PT_STRING, b);
}
void WObj::set_vec3(std::string_view n, Vec3 v) {
    Bytes b(12); wr<float>(&b[0], v.x); wr<float>(&b[4], v.y); wr<float>(&b[8], v.z);
    if (WProp* p = find(n)) { p->data = b; return; }
    set(key_of(n), PT_VEC3, b);
}

// ---------------------------------------------------------------------------------------------
// Wdf

bool Wdf::parse(const uint8_t* d, size_t n, std::string* err) {
    objs.clear();
    auto fail = [&](const char* why, size_t at) {
        if (err) { char b[128]; snprintf(b, sizeof b, "%s at 0x%zX", why, at); *err = b; }
        return false;
    };
    if (n < 20) return fail("short header", 0);
    for (int i = 0; i < 5; i++) header[i] = rd<uint32_t>(d + i * 4);
    if (header[0] != 0xFF002F00u) return fail("bad header marker", 0);
    size_t pos = 20;
    objs.reserve(header[2] < 100000 ? header[2] : 0);
    while (pos < n) {
        if (pos + 12 > n) return fail("short object header", pos);
        uint32_t marker = rd<uint32_t>(d + pos), count = rd<uint32_t>(d + pos + 4), cls = rd<uint32_t>(d + pos + 8);
        if (marker != 0xFF012F01u) return fail("bad object marker", pos);
        pos += 12;
        if (count > (n - pos) / 16) return fail("impossible property count", pos);
        WObj o;
        o.cls = cls;
        o.props.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            if (pos + 16 > n) return fail("short property header", pos);
            uint32_t m = rd<uint32_t>(d + pos), type = rd<uint32_t>(d + pos + 4), key = rd<uint32_t>(d + pos + 8),
                     size = rd<uint32_t>(d + pos + 12);
            if (m != 0xFF022F02u || size > n - pos - 16) return fail("bad property", pos);
            WProp& p = o.props[i];
            p.type = type; p.key = key;
            p.data.assign(d + pos + 16, d + pos + 16 + size);
            pos += 16 + size;
        }
        objs.push_back(std::move(o));
    }
    return true;
}

Bytes Wdf::serialize() const {
    size_t total = 20;
    for (auto& o : objs) { total += 12; for (auto& p : o.props) total += 16 + p.data.size(); }
    Bytes out;
    out.reserve(total);
    auto h = header;
    h[2] = uint32_t(objs.size());
    for (uint32_t v : h) put(out, v);
    for (auto& o : objs) {
        put(out, 0xFF012F01u); put(out, uint32_t(o.props.size())); put(out, o.cls);
        for (auto& p : o.props) {
            put(out, 0xFF022F02u); put(out, p.type); put(out, p.key); put(out, uint32_t(p.data.size()));
            put_bytes(out, p.data.data(), p.data.size());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// WTree

void WTree::build(const Wdf& w) {
    by_uid.clear();
    kids.assign(w.objs.size(), {});
    parent_of.assign(w.objs.size(), -1);
    root = -1;
    max_uid = 0;
    static const uint32_t K_ROOT = key_of("MARoot");
    for (size_t i = 0; i < w.objs.size(); i++) {
        uint32_t u = w.objs[i].uid();
        if (u) by_uid.emplace(u, int(i));
        if (u > max_uid) max_uid = u;
        if (root < 0 && w.objs[i].cls == K_ROOT) root = int(i);
    }
    if (root < 0 && !w.objs.empty()) root = 0;
    for (size_t i = 0; i < w.objs.size(); i++) {
        uint32_t c = w.objs[i].child();
        int guard = 0;
        while (c && guard++ < 100000) {
            int ci = find(c);
            if (ci < 0 || parent_of[ci] >= 0 || ci == int(i)) break;
            parent_of[ci] = int(i);
            kids[i].push_back(ci);
            c = w.objs[ci].sibling();
        }
    }
}

void WTree::subtree(int index, std::vector<int>& out) const {
    if (index < 0) return;
    std::vector<int> stack{index};
    while (!stack.empty()) {
        int n = stack.back();
        stack.pop_back();
        out.push_back(n);
        for (size_t k = kids[n].size(); k-- > 0;) stack.push_back(kids[n][k]);
    }
}

// ---------------------------------------------------------------------------------------------
// Structural edits

namespace wedit {

static int insert_after_subtree(Wdf& w, const WTree& t, int parent, WObj obj) {
    std::vector<int> sub;
    t.subtree(parent, sub);
    int at = parent;
    for (int i : sub) at = std::max(at, i);
    w.objs.insert(w.objs.begin() + at + 1, std::move(obj));
    return at + 1;
}

int append_child(Wdf& w, uint32_t parent_uid, WObj obj) {
    WTree t;
    t.build(w);
    int p = t.find(parent_uid);
    if (p < 0) return -1;
    uint32_t uid = obj.uid();
    obj.set_u32("parent", parent_uid);
    obj.set_u32("sibling", 0);
    if (!obj.find("child")) obj.set_u32("child", 0);
    if (t.kids[p].empty()) {
        w.objs[p].set_u32("child", uid);
    } else {
        w.objs[t.kids[p].back()].set_u32("sibling", uid);
    }
    return insert_after_subtree(w, t, p, std::move(obj));
}

int prepend_child(Wdf& w, uint32_t parent_uid, WObj obj) {
    WTree t;
    t.build(w);
    int p = t.find(parent_uid);
    if (p < 0) return -1;
    uint32_t uid = obj.uid();
    obj.set_u32("parent", parent_uid);
    obj.set_u32("sibling", w.objs[p].child());
    if (!obj.find("child")) obj.set_u32("child", 0);
    w.objs[p].set_u32("child", uid);
    w.objs.insert(w.objs.begin() + p + 1, std::move(obj));
    return p + 1;
}

void remove_subtree(Wdf& w, uint32_t uid) {
    WTree t;
    t.build(w);
    int i = t.find(uid);
    if (i < 0) return;
    int p = t.parent_of[i];
    if (p >= 0) {
        auto& sib = t.kids[p];
        auto it = std::find(sib.begin(), sib.end(), i);
        uint32_t next = w.objs[i].sibling();
        if (it == sib.begin()) w.objs[p].set_u32("child", next);
        else w.objs[*(it - 1)].set_u32("sibling", next);
    }
    std::vector<int> sub;
    t.subtree(i, sub);
    std::sort(sub.begin(), sub.end());
    for (size_t k = sub.size(); k-- > 0;) w.objs.erase(w.objs.begin() + sub[k]);
}

void reparent(Wdf& w, uint32_t uid, uint32_t new_parent_uid) {
    WTree t;
    t.build(w);
    int i = t.find(uid);
    if (i < 0 || t.find(new_parent_uid) < 0) return;
    // refuse to move into own subtree
    std::vector<int> sub;
    t.subtree(i, sub);
    for (int s : sub) if (w.objs[s].uid() == new_parent_uid) return;
    std::vector<WObj> moved;
    for (int s : sub) moved.push_back(w.objs[s]);
    remove_subtree(w, uid);
    insert_subtree(w, new_parent_uid, std::move(moved));
}

bool move_sibling(Wdf& w, uint32_t uid, int dir) {
    WTree t;
    t.build(w);
    int i = t.find(uid);
    if (i < 0 || !dir) return false;
    int p = t.parent_of[i];
    if (p < 0) return false;
    auto& sib = t.kids[p];
    size_t at = size_t(std::find(sib.begin(), sib.end(), i) - sib.begin());
    size_t other = dir < 0 ? at - 1 : at + 1;
    if ((dir < 0 && at == 0) || other >= sib.size()) return false;
    // The siblings from the earlier of the two on are re-appended in their new order (keeps the pre-order layout).
    size_t first = std::min(at, other), second = std::max(at, other);
    uint32_t parent = w.objs[size_t(p)].uid();
    uint32_t early = w.objs[size_t(sib[first])].uid();
    std::vector<uint32_t> seq;
    seq.push_back(w.objs[size_t(sib[second])].uid());
    seq.push_back(early);
    for (size_t k = second + 1; k < sib.size(); k++) seq.push_back(w.objs[size_t(sib[k])].uid());
    for (uint32_t u : seq) reparent(w, u, parent);
    return true;
}

std::vector<WObj> clone_subtree(const Wdf& w, uint32_t uid, uint32_t& next_uid,
                                const std::unordered_set<uint32_t>* keep_keys,
                                std::unordered_map<uint32_t, uint32_t>* map_out) {
    WTree t;
    t.build(w);
    std::vector<WObj> out;
    int i = t.find(uid);
    if (i < 0) return out;
    std::vector<int> sub;
    t.subtree(i, sub);
    std::unordered_map<uint32_t, uint32_t> map;
    for (int s : sub) map[w.objs[s].uid()] = next_uid++;
    for (int s : sub) {
        WObj o = w.objs[s];
        for (auto& p : o.props) {
            if (p.key == K_TEMPLATE) continue;
            if (p.key == K_UID || p.key == K_PARENT || p.key == K_CHILD || p.key == K_SIBLING) {
                uint32_t v = uint32_t(p.as_int());
                auto it = map.find(v);
                if (it != map.end()) p.set_int(it->second);
                continue;
            }
            if ((p.type == PT_U32 || p.type == PT_I32) && p.data.size() == 4 && !(keep_keys && keep_keys->count(p.key))) {
                uint32_t v = uint32_t(p.as_int());
                auto it = map.find(v);
                if (v && it != map.end()) p.set_int(it->second);
            }
        }
        out.push_back(std::move(o));
    }
    out.front().set_u32("parent", 0);
    out.front().set_u32("sibling", 0);
    if (map_out) *map_out = std::move(map);
    return out;
}

void insert_subtree(Wdf& w, uint32_t parent_uid, std::vector<WObj> objs) {
    if (objs.empty()) return;
    WTree t;
    t.build(w);
    int p = t.find(parent_uid);
    if (p < 0) return;
    uint32_t uid = objs.front().uid();
    objs.front().set_u32("parent", parent_uid);
    objs.front().set_u32("sibling", 0);
    if (t.kids[p].empty()) w.objs[p].set_u32("child", uid);
    else w.objs[t.kids[p].back()].set_u32("sibling", uid);
    std::vector<int> sub;
    t.subtree(p, sub);
    int at = p;
    for (int i : sub) at = std::max(at, i);
    w.objs.insert(w.objs.begin() + at + 1, std::make_move_iterator(objs.begin()), std::make_move_iterator(objs.end()));
}

}  // namespace wedit
}  // namespace ma2
