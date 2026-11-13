// Property and class names for world-file keys. The game only stores key_of(name); the names
// come from the user's own ma2.xbe at run time (printable NUL-terminated strings, with numbered
// formats such as "ob%dvar%d" expanded), exactly like bringup/inspect_wdf.py.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "util.h"

namespace ma2 {

class NameTable {
public:
    bool load_xbe(const fs::path& xbe, std::string* err = nullptr);
    void add(std::string_view name);
    // First name (alphabetical) for the key, or nullptr.
    const std::string* name(uint32_t key) const;
    // Name, or 8 hex digits.
    std::string label(uint32_t key) const;
    const std::vector<std::string>* all(uint32_t key) const;
    size_t size() const { return map_.size(); }

private:
    std::unordered_map<uint32_t, std::vector<std::string>> map_;
};

}  // namespace ma2
