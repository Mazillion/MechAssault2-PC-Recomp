// The MA2 dictionary (dictionary/catalog.json, embedded in the editor): every world
// class (what it is, how to draw it) and every property (plain label, tooltip, type, units, enum,
// range, Common/Advanced/Internal tier), keyed by the CRC keys stored in .wdf files.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ma2 {

struct DictClass {
    std::string name, description, category, confidence;
    std::string draw_kind;                // model, box, sphere, plane, spline, marker, none
    std::vector<std::string> size_from;   // properties giving the size (box / sphere)
    uint32_t color = 0xFFAAAAAAu;         // ABGR (ImGui / editor convention)
    std::string draw_notes;
    bool default_visible = true;
    bool placeable = false;
};

struct DictProp {
    std::string name, owner, label, tooltip, type, units, tier, confidence, family;
    std::vector<std::pair<long long, std::string>> enums;  // value -> meaning, in value order
    bool has_range = false;
    double range_min = 0, range_max = 0;
    bool has_default = false;
    double default_num = 0;
    std::string default_text;
    bool known() const { return confidence == "verified" || confidence == "code-confirmed" || confidence == "documented" || confidence == "inferred"; }
};

class Dictionary {
public:
    // From the JSON text of catalog.json. Returns false (and stays empty) on a parse error.
    bool load(const std::string& json_text, std::string* err = nullptr);
    bool empty() const { return classes_.empty(); }
    const DictClass* cls(uint32_t class_key) const;
    const DictClass* cls(const std::string& name) const;
    const DictProp* prop(uint32_t key) const;
    size_t class_count() const { return classes_.size(); }
    size_t prop_count() const { return props_.size(); }

private:
    std::vector<DictClass> classes_;
    std::unordered_map<uint32_t, size_t> by_key_;
    std::unordered_map<std::string, size_t> by_name_;
    std::unordered_map<uint32_t, DictProp> props_;
};

// "#ff8c1a" -> ABGR with full alpha.
uint32_t parse_html_color(const std::string& s, uint32_t fallback);

}  // namespace ma2
