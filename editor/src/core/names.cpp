#include "names.h"

#include <algorithm>
#include <cstdio>

namespace ma2 {

void NameTable::add(std::string_view name) {
    auto& v = map_[key_of(name)];
    std::string s(name);
    auto it = std::lower_bound(v.begin(), v.end(), s);
    if (it == v.end() || *it != s) v.insert(it, std::move(s));
}

static bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_';
}

// inspect_wdf.py: [A-Za-z][A-Za-z0-9 _]*(?:%d[A-Za-z0-9 _]*){1,2}
static int numbered_format(const std::string& s) {
    if (s.empty() || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return 0;
    int count = 0;
    for (size_t i = 1; i < s.size(); i++) {
        if (s[i] == '%') {
            if (i + 1 >= s.size() || s[i + 1] != 'd') return 0;
            count++;
            i++;
            continue;
        }
        if (!name_char(s[i])) return 0;
    }
    return (count == 1 || count == 2) ? count : 0;
}

bool NameTable::load_xbe(const fs::path& xbe, std::string* err) {
    Bytes data;
    if (!read_file(xbe, data, err)) return false;
    size_t start = 0;
    for (size_t i = 0; i < data.size(); i++) {
        uint8_t c = data[i];
        if (c >= 0x20 && c <= 0x7E) continue;
        if (c == 0 && i - start >= 3) {
            std::string s(reinterpret_cast<const char*>(&data[start]), i - start);
            add(s);
            int n = numbered_format(s);
            if (n) {
                char buf[512];
                if (n == 1) {
                    for (int a = 0; a < 32; a++) { snprintf(buf, sizeof buf, s.c_str(), a); add(buf); }
                } else {
                    for (int a = 0; a < 32; a++)
                        for (int b = 0; b < 32; b++) { snprintf(buf, sizeof buf, s.c_str(), a, b); add(buf); }
                }
            }
        }
        start = i + 1;
    }
    // Names the XBE builds at run time (not present as literal strings). Found by matching keys
    // in the retail worlds; see NOTES.md.
    static const char* extra[] = {"TerrainHeight Field", "Mech Map", "Road Map"};
    for (const char* e : extra) add(e);
    return true;
}

const std::string* NameTable::name(uint32_t key) const {
    auto it = map_.find(key);
    return it == map_.end() || it->second.empty() ? nullptr : &it->second.front();
}

const std::vector<std::string>* NameTable::all(uint32_t key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

std::string NameTable::label(uint32_t key) const {
    if (const std::string* n = name(key)) return *n;
    char b[16];
    snprintf(b, sizeof b, "%08X", key);
    return b;
}

}  // namespace ma2
