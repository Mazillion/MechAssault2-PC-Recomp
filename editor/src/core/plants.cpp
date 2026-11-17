// Ground plants from every level: the culture layers (grass, bushes, small rocks) of the retail worlds, and
// adding one of them to a scenario. See app/app_culture.cpp for how the game reads them (culture map u16 per texel, one
// bit field per MACultureType; MATerrainInfo cultureBF<n>Name / Shift / NBits / Mask / Color0, culturenBitfields).
#include <algorithm>
#include <cstring>

#include "culture_bits.h"
#include "scenario.h"

namespace ma2 {

using namespace world;

static const uint32_t K_CINFO = key_of("MATerrainInfo"), K_CSYS = key_of("MACultureSystem"), K_CTYPE = key_of("MACultureType"),
                      K_CGEN = key_of("MACultureGenerator"), K_CMAP = 0x89FB5118u;

static std::string bf_key(int n, const char* field) { return "cultureBF" + std::to_string(n) + field; }

// Layer count: culturenBitfields, or the highest numbered cultureBF<i>Name when the count is missing.
static int layer_count(const WObj& info) {
    if (info.find("culturenBitfields")) return std::clamp(int(info.u32("culturenBitfields")), 0, 16);
    int n = 0;
    for (int b = 0; b < 16; b++) if (info.find(bf_key(b, "Name"))) n = b + 1;
    return n;
}

// The bits layer b owns: its Mask, else Shift / NBits.
static uint32_t layer_mask(const WObj& info, int b) {
    if (uint32_t m = info.u32(bf_key(b, "Mask")) & 0xFFFFu) return m;
    int nb = std::clamp(info.i32(bf_key(b, "NBits"), 0), 0, 16), sh = std::clamp(int(info.u32(bf_key(b, "Shift"))), 0, 15);
    return nb > 0 ? (((1u << nb) - 1u) << sh) & 0xFFFFu : 0u;
}

const std::vector<ScenarioIO::PlantSource>& ScenarioIO::plant_sources() {
    if (plants_ready_) return plants_;
    plants_ready_ = true;
    for (auto& m : gd_.maps()) {
        if (!m.resource_present) continue;
        Wdf w;
        if (!gd_.load_world(m, w)) continue;
        int ii = find_class(w, K_CINFO);
        if (ii < 0 || !w.objs[size_t(ii)].find(K_CMAP)) continue;
        const WObj& info = w.objs[size_t(ii)];
        const int layers = layer_count(info);
        WTree t;
        t.build(w);
        std::string level = lower(m.resource);
        if (iends_with(level, ".mgf")) level.resize(level.size() - 4);
        for (size_t i = 0; i < w.objs.size(); i++) {
            const WObj& ty = w.objs[i];
            if (ty.cls != K_CTYPE || ty.str("Name").empty()) continue;
            int field = -1;
            for (int b = 0; b < layers && field < 0; b++)
                if (iequals(info.str(bf_key(b, "Name")), ty.str("Name"))) field = b;
            if (field < 0) continue;
            PlantSource ps;
            ps.map = m.name;
            ps.level = level;
            ps.name = ty.str("Name");
            ps.nbits = std::clamp(info.i32(bf_key(field, "NBits"), 3), 1, 4);
            if (const WProp* c = info.find(bf_key(field, "Color0"))) ps.color0 = uint32_t(c->as_int());
            ps.tree.objs.push_back(ty);
            for (int k : t.kids[i]) {
                const WObj& g = w.objs[size_t(k)];
                if (g.cls != K_CGEN) continue;
                uint32_t tid = g.u32("GenerationTemplate");
                if (!tid || !cat_.find(tid)) continue;
                ps.templates.push_back(tid);
                ps.tree.objs.push_back(g);
            }
            if (ps.templates.empty()) continue;
            plants_.push_back(std::move(ps));
        }
    }
    return plants_;
}

ScenarioIO::PlantResult ScenarioIO::add_plant(Scenario& s, const PlantSource& src, bool dry_run, bool* new_layer) {
    if (new_layer) *new_layer = false;
    Wdf& w = s.world;
    int ii = find_class(w, K_CINFO), si = find_class(w, K_CSYS);
    if (ii < 0 || si < 0 || !w.objs[size_t(ii)].find(K_CMAP) || src.tree.objs.empty()) return PlantResult::NoCultureSystem;
    for (auto& o : w.objs)
        if (o.cls == K_CTYPE && iequals(o.str("Name"), src.name)) return PlantResult::AlreadyHere;
    WObj& info = w.objs[size_t(ii)];
    const int n = layer_count(info);
    int field = -1;  // a layer of that name with no type on this map (the Base level lists layers it culls): its bits are reused
    for (int b = 0; b < n && field < 0; b++)
        if (iequals(info.str(bf_key(b, "Name")), src.name)) field = b;
    if (field < 0) {
        std::vector<uint32_t> masks;
        for (int b = 0; b < n; b++) masks.push_back(layer_mask(info, b));
        const int shift = alloc_culture_bits(masks, src.nbits);
        if (shift < 0) return PlantResult::NoFreeBits;
        if (dry_run) return PlantResult::Added;
        if (new_layer) *new_layer = true;
        field = n;
        info.set_str(bf_key(field, "Name"), src.name);
        info.set_u32(bf_key(field, "Shift"), uint32_t(shift), PT_U32);
        info.set_i32(bf_key(field, "NBits"), src.nbits, PT_I32);
        info.set_u32(bf_key(field, "Mask"), ((1u << src.nbits) - 1u) << shift, PT_U32);
        Bytes col(4);
        std::memcpy(col.data(), &src.color0, 4);
        info.set(key_of(bf_key(field, "Color0")), PT_COLOR, col);
        info.set_i32("culturenBitfields", n + 1, PT_I32);
    }
    if (dry_run) return PlantResult::Added;
    // A copy of the type and its generators under the culture system; GenerationTemplate and the other template links keep
    // their template ids (world and template uids share one number space).
    std::unordered_set<uint32_t> keep = cat_.ref_keys();
    keep.insert(key_of("TemplateID"));
    keep.insert(key_of("GenerationTemplate"));
    uint32_t next = next_uid(w);
    auto clone = wedit::clone_subtree(src.tree, src.tree.objs.front().uid(), next, &keep);
    if (clone.empty()) return PlantResult::NoCultureSystem;
    wedit::insert_subtree(w, w.objs[size_t(si)].uid(), std::move(clone));
    s.dirty = true;
    return PlantResult::Added;
}

}  // namespace ma2
