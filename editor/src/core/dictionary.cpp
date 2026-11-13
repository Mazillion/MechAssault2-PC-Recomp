#include "dictionary.h"

#include <algorithm>
#include <cstdlib>

#include "json.h"
#include "util.h"

namespace ma2 {

uint32_t parse_html_color(const std::string& s, uint32_t fallback) {
    if (s.size() != 7 || s[0] != '#') return fallback;
    char* e = nullptr;
    unsigned long v = strtoul(s.c_str() + 1, &e, 16);
    if (!e || *e) return fallback;
    uint32_t r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
    return 0xFF000000u | b << 16 | g << 8 | r;
}

static uint32_t hex_key(const std::string& s) { return uint32_t(strtoul(s.c_str(), nullptr, 16)); }

static std::string text_of(const Json* j) {
    if (!j) return {};
    if (j->type == Json::String) return j->s;
    if (j->type == Json::Number) {
        char b[64];
        snprintf(b, sizeof b, "%g", j->n);
        return b;
    }
    if (j->type == Json::Bool) return j->b ? "1" : "0";
    return {};
}

bool Dictionary::load(const std::string& text, std::string* err) {
    Json root;
    if (!Json::parse(text, root, err)) return false;
    classes_.clear();
    by_key_.clear();
    by_name_.clear();
    props_.clear();
    if (const Json* cs = root.get("classes"))
        for (auto& [name, c] : cs->o) {
            DictClass d;
            d.name = name;
            d.description = c.str("description");
            d.category = c.str("category");
            d.confidence = c.str("confidence");
            d.default_visible = c.flag("editor_default_visible", true);
            d.placeable = c.flag("placeable", false);
            if (const Json* dr = c.get("draw")) {
                d.draw_kind = dr->str("kind", "none");
                d.color = parse_html_color(dr->str("color"), d.color);
                d.draw_notes = dr->str("notes");
                if (const Json* sf = dr->get("size_from"))
                    for (auto& x : sf->a) if (x.type == Json::String) d.size_from.push_back(x.s);
            }
            size_t idx = classes_.size();
            by_name_[name] = idx;
            std::string key = c.str("class_key");
            by_key_[key.empty() ? key_of(name) : hex_key(key)] = idx;
            classes_.push_back(std::move(d));
        }
    if (const Json* ps = root.get("properties"))
        for (auto& [key, p] : ps->o) {
            DictProp d;
            d.name = p.str("name");
            d.owner = p.str("owner");
            d.label = p.str("label");
            // Family members are labelled "Gives weapon class %d (2)" in the catalog: show "Gives weapon class 2".
            if (size_t pc = d.label.find("%d"), op = d.label.rfind(" ("); pc != std::string::npos && op != std::string::npos && op > pc &&
                d.label.back() == ')') {
                std::string n = d.label.substr(op + 2, d.label.size() - op - 3);
                d.label = d.label.substr(0, op).replace(pc, 2, n);
                static const char* weapon[] = {"energy", "ballistic", "missile"};
                if (p.str("family") == "PowerupWeaponFireClass%d" && n.size() == 1 && n[0] >= '0' && n[0] <= '2')
                    d.label = std::string("Upgrades ") + weapon[n[0] - '0'] + " weapons";
            }
            d.tooltip = p.str("tooltip");
            d.type = p.str("type");
            d.units = p.str("units");
            d.family = p.str("family");
            d.tier = p.str("tier", "Advanced");
            // Power-up weapon classes 3-5 do not exist in the game (0 energy, 1 ballistic, 2 missile): out of Common
            // settings, where they read as three more meaningless 0 rows (MP dogfood, Crossfire Yard).
            if (d.family == "PowerupWeaponFireClass%d" && d.label.rfind("Gives weapon class ", 0) == 0) {
                d.label = "Unused weapon class " + d.label.substr(19);
                d.tier = "Advanced";
            }
            d.confidence = p.str("confidence", "unknown");
            if (const Json* en = p.get("enum"); en && en->type == Json::Object)
                for (auto& [v, meaning] : en->o) d.enums.push_back({atoll(v.c_str()), text_of(&meaning)});
            std::sort(d.enums.begin(), d.enums.end(), [](auto& a, auto& b) { return a.first < b.first; });
            if (const Json* r = p.get("range"); r && r->type == Json::Array && r->a.size() == 2 && r->a[0].type == Json::Number &&
                                               r->a[1].type == Json::Number && r->a[1].n > r->a[0].n) {
                d.has_range = true;
                d.range_min = r->a[0].n;
                d.range_max = r->a[1].n;
            }
            if (const Json* df = p.get("default"); df && df->type != Json::Null) {
                d.has_default = true;
                d.default_num = df->type == Json::Number ? df->n : 0;
                d.default_text = text_of(df);
            }
            props_[hex_key(key)] = std::move(d);
        }
    return !classes_.empty();
}

const DictClass* Dictionary::cls(uint32_t k) const {
    auto it = by_key_.find(k);
    return it == by_key_.end() ? nullptr : &classes_[it->second];
}
const DictClass* Dictionary::cls(const std::string& n) const {
    auto it = by_name_.find(n);
    return it == by_name_.end() ? nullptr : &classes_[it->second];
}
const DictProp* Dictionary::prop(uint32_t k) const {
    auto it = props_.find(k);
    return it == props_.end() ? nullptr : &it->second;
}

}  // namespace ma2
