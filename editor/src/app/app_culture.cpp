// Culture scatter (gap 7): the grass, bushes, rocks and tree clusters the game scatters over the
// terrain from MATerrainInfo's culture map, drawn near the camera like the game (View > Ground scatter).
//
// Data (22 retail worlds): the culture map (key 89FB5118, 256 or 512 square, u16, row 0 = z 0) packs one
// level field per culture type: cultureBF<i>Name / Shift / NBits. MATerrainInfo > MACultureSystem >
// MACultureType (Name = the field name) > MACultureGenerator (GenerationTemplate, Density, GridScale,
// RandomScaleRange, RandomRotation, AlignWithSlope, Min/MaxSlope, SnapToGrid, StopFade).
// Count rule and RNG (MT19937 per cell, same draw order) follow sub_001801F6; Density is per km2 at the top level, scaled by level / top.
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "app.h"
#include "theme.h"
#include "imgui.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static const uint32_t K_CULTUREMAP = 0x89FB5118u;

static void hint_text(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void App::load_culture() {
    culture_ = {};
    overlay_dirty_ = true;  // the Plants overlay shows the reloaded map (undo / redo / another document)
    culture_.doc = doc_.get();
    culture_.objs = doc_ ? doc_->world.objs.size() : 0;
    if (!doc_) return;
    const Wdf& w = doc_->world;
    int ti = find_class(w, K_TINFO);
    if (ti < 0) return;
    const WObj& info = w.objs[size_t(ti)];
    const WProp* p = info.find(K_CULTUREMAP);
    Picture pic;
    if (!p || !read_picture(p->data, pic) || pic.dxt() || pic.swizzled() || pic.bits.size() < size_t(pic.width) * pic.height * 2) return;
    culture_.w = int(pic.width);
    culture_.h = int(pic.height);
    culture_.map.resize(size_t(pic.width) * pic.height);
    std::memcpy(culture_.map.data(), pic.bits.data(), culture_.map.size() * 2);
    static const uint32_t K_CTYPE = key_of("MACultureType"), K_CGEN = key_of("MACultureGenerator");
    WTree t;
    t.build(w);
    for (size_t i = 0; i < w.objs.size(); i++) {
        if (w.objs[i].cls != K_CTYPE) continue;
        std::string name = w.objs[i].str("Name");
        int field = -1;
        // The game loads culture layers from "%snBitfields" with no cap (sub_00189A1A); default to scanning 16 when the key is absent.
        int layers = info.find("culturenBitfields") ? std::clamp(int(info.u32("culturenBitfields")), 0, 64) : 16;
        for (int b = 0; b < layers && field < 0 && !name.empty(); b++)
            if (iequals(info.str("cultureBF" + std::to_string(b) + "Name"), name)) field = b;
        if (field < 0) continue;
        for (int k : t.kids[i]) {
            const WObj& g = w.objs[size_t(k)];
            if (g.cls != K_CGEN) continue;
            CultureType c;
            c.name = name;
            std::string bf = "cultureBF" + std::to_string(field);
            c.shift = int(info.u32(bf + "Shift"));
            c.bits = std::clamp(int(info.u32(bf + "NBits", 1)), 1, 15);
            if (uint32_t mask = info.u32(bf + "Mask") & 0xFFFFu; mask && !(mask >> std::countr_zero(mask) & ((mask >> std::countr_zero(mask)) + 1))) {  // contiguous mask: game level = (v & Mask) >> Shift
                c.shift = std::countr_zero(mask);
                c.bits = std::clamp(std::popcount(mask), 1, 15);
            }
            c.templ = g.u32("GenerationTemplate");
            c.density = g.f32("Density", 1000);  // ctor default sub_0017ED88
            const WProp* gs = g.find("GridScale");
            if (gs && gs->data.size() >= 8) { std::memcpy(&c.grid[0], gs->data.data(), 4); std::memcpy(&c.grid[1], gs->data.data() + 4, 4); }
            for (float& v : c.grid) if (!(v >= 1.f)) v = 10.f;
            c.scale_range = std::clamp(g.f32("RandomScaleRange"), 0.f, 0.95f);
            c.random_rot = g.i32("RandomRotation", 1) != 0;
            c.align = g.i32("AlignWithSlope", 1) != 0;
            c.snap = g.i32("SnapToGrid") != 0;
            c.min_slope = g.f32("MinSlope", 0);
            c.max_slope = g.f32("MaxSlope", 90);
            c.stop_fade = std::clamp(g.f32("StopFade", 300), 10.f, 1500.f);
            c.start_fade = g.f32("StartFade", 200);
            culture_.types.push_back(c);
        }
    }
}

// The game's generator (wiki culture-seed-and-placement-order, rng): MT19937 with old-style seeding (seed 0 -> 0x1105,
// mt[i] = 69069 * mt[i-1]), reseeded for every culture cell with (cx << 16) ^ cz (gen+0x2C is never written, so 0);
// rand(a, b) = a + u32 * 2^-32 * (b - a). Same draws in the same order as sub_001801F6, so the scatter lands where the game's does.
namespace {
struct GameRng {
    uint32_t mt[624];
    int idx = 624;
    explicit GameRng(uint32_t seed) {
        mt[0] = seed ? seed : 0x1105u;
        for (int i = 1; i < 624; i++) mt[i] = 69069u * mt[i - 1];
    }
    uint32_t next() {
        if (idx >= 624) {
            for (int k = 0; k < 624; k++) {
                uint32_t y = (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7FFFFFFFu);
                mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908B0DFu : 0u);
            }
            idx = 0;
        }
        uint32_t y = mt[idx++];
        y ^= y >> 11; y ^= (y << 7) & 0x9D2C5680u; y ^= (y << 15) & 0xEFC60000u; y ^= y >> 18;
        return y;
    }
    float range(float a, float b) { return float(double(a) + double(next()) * 2.3283064365386963e-10 * (double(b) - double(a))); }
};
}  // namespace

void App::add_culture() {
    if (!doc_) return;
    if (culture_.doc != doc_.get() || culture_.objs != doc_->world.objs.size()) load_culture();
    if (culture_.map.empty() || culture_.types.empty() || !cat_) return;
    const Terrain& tr = doc_->terrain;
    float sx = tr.size_x(), sz = tr.size_z();
    XMFLOAT3 eye = cam_.eye;
    float dx = eye.x - culture_.at[0], dz = eye.z - culture_.at[1];
    bool regen = dx * dx + dz * dz > 16.f;  // a type also regenerates while it has nothing (model loading)
    for (size_t ti = 0; ti < culture_.types.size(); ti++) {
        CultureType& c = culture_.types[ti];
        const TemplateInfo* tinfo = cat_->find(c.templ);
        GpuModel* gm = tinfo && !tinfo->model.empty() ? models_.get(tinfo->model, tinfo->mtb) : nullptr;
        if (!gm || gm->state != GpuModel::Ready) continue;
        if (regen || c.inst.empty()) {
            c.inst.clear();
            int top = (1 << c.bits) - 1;
            // Research (code-confirmed): unless SnapToGrid is set (no retail generator sets it) the placement
            // grid is one culture-map texel; GridScale only matters for snapped generators, which also clamp the count to 1.
            float cw = c.snap ? c.grid[0] : sx / float(culture_.w), ch = c.snap ? c.grid[1] : sz / float(culture_.h);
            float R = c.stop_fade, per_cell = c.density * cw * ch * 1e-6f;
            int x0 = int(std::floor((eye.x - R) / cw)), x1 = int(std::floor((eye.x + R) / cw));
            int z0 = int(std::floor((eye.z - R) / ch)), z1 = int(std::floor((eye.z + R) / ch));
            for (int gz = z0; gz <= z1; gz++)
                for (int gx = x0; gx <= x1; gx++) {
                    float cx = (gx + 0.5f) * cw, cz = (gz + 0.5f) * ch;
                    if (cx < 0 || cz < 0 || cx >= sx || cz >= sz) continue;
                    int mx = std::clamp(int(cx / sx * culture_.w), 0, culture_.w - 1), mz = std::clamp(int(cz / sz * culture_.h), 0, culture_.h - 1);
                    int level = (culture_.map[size_t(mz) * culture_.w + mx] >> c.shift) & top;
                    if (!level) continue;
                    GameRng rng((uint32_t(gx) << 16) ^ uint32_t(gz));
                    float n = per_cell * float(level) / float(top);
                    float r0 = rng.range(0.f, 1.f);
                    int count = int(n) + (n - float(int(n)) > r0 ? 1 : 0);
                    if (c.snap) count = std::min(count, 1);
                    for (int k = 0; k < count; k++) {
                        float px = (gx + 0.001f) * cw, pz = (gz + 0.001f) * ch;
                        if (!c.snap) { px = gx * cw + rng.range(0.001f, 0.999f) * cw; pz = gz * ch + rng.range(0.001f, 0.999f) * ch; }
                        float ex = px - eye.x, ez = pz - eye.z;
                        if (ex * ex + ez * ez > R * R) continue;
                        Vec3 nn = tr.normal_at(px, pz);
                        float slope = std::acos(std::clamp(nn.y, -1.f, 1.f)) * 57.29578f;
                        if (slope < c.min_slope || slope > c.max_slope) continue;
                        float yaw = c.random_rot ? rng.range(0.f, 6.2831853f) : 0.f;
                        float s = 1.f + rng.range(-c.scale_range, c.scale_range);
                        float cy = std::cos(yaw), sh = std::sin(yaw);
                        // Columns: the model's x, y (up) and z axes in the world.
                        float Y[3] = {0, 1, 0}, Z[3] = {sh, 0, cy};
                        if (c.align) {
                            Y[0] = nn.x; Y[1] = nn.y; Y[2] = nn.z;
                            float d = Z[0] * Y[0] + Z[2] * Y[2];
                            Z[0] -= Y[0] * d; Z[1] -= Y[1] * d; Z[2] -= Y[2] * d;
                            float l = std::sqrt(Z[0] * Z[0] + Z[1] * Z[1] + Z[2] * Z[2]);
                            for (float& v : Z) v /= std::max(l, 1e-4f);
                        }
                        float X[3] = {Y[1] * Z[2] - Y[2] * Z[1], Y[2] * Z[0] - Y[0] * Z[2], Y[0] * Z[1] - Y[1] * Z[0]};
                        BoxInstance mi;
                        mi.r0 = {X[0] * s, Y[0] * s, Z[0] * s, px};
                        mi.r1 = {X[1] * s, Y[1] * s, Z[1] * s, tr.height_at(px, pz)};
                        mi.r2 = {X[2] * s, Y[2] * s, Z[2] * s, pz};
                        mi.color = {1, 1, 1, 1};
                        c.inst.push_back(mi);
                    }
                }
        }
        if (!c.inst.empty()) r_.models.push_back({gm, c.inst});
    }
    if (regen) {
        culture_.at[0] = eye.x;
        culture_.at[1] = eye.z;
    }
}

// The Plants brush edits culture_.map; this writes it back into MATerrainInfo's culture map picture.
void App::store_culture() {
    if (!doc_ || culture_.map.empty()) return;
    int ti = find_class(doc_->world, K_TINFO);
    if (ti < 0) return;
    WProp* p = doc_->world.objs[size_t(ti)].find(K_CULTUREMAP);
    Mgic m;
    if (!p || !m.parse(p->data)) return;
    MgicSection* pic = m.section(fourcc("PIC "));
    MgicChunk* bits = pic ? pic->chunk(fourcc("bits")) : nullptr;
    if (!bits || bits->data.size() < culture_.map.size() * 2) return;
    std::memcpy(bits->data.data(), culture_.map.data(), culture_.map.size() * 2);
    p->data = m.serialize();
    doc_->dirty = true;
}

// The palette's plain name ("Friendly (Template)" -> "Friendly"); the template name goes in the tooltip.
static std::string plant_label_of(const App& app, const TemplateCatalog* cat, const std::string& type_name, uint32_t templ) {
    std::string label = pretty_name(type_name);
    if (const TemplateInfo* t = cat ? cat->find(templ) : nullptr) {
        std::string pn = app.palette_name(*t), suffix = " (" + t->name + ")";
        if (pn.size() > suffix.size() && pn.compare(pn.size() - suffix.size(), suffix.size(), suffix) == 0) label = pn.substr(0, pn.size() - suffix.size());
    }
    return label;
}

std::string App::plant_label(const CultureType& c) const { return plant_label_of(*this, cat_.get(), c.name, c.templ); }

// A plant's template into the scatter brush (trees and rocks as objects; also the way to get a plant on a map whose level
// has no culture system), then the Scatter page.
void App::plant_scatter(uint32_t tid) {
    if (!doc_ || !cat_ || !cat_->find(tid)) return;
    if (std::find(scatter_set_.begin(), scatter_set_.end(), tid) == scatter_set_.end()) scatter_toggle(tid, true);
    open_page(Page::Scatter);
    if (editable() && !scatter_set_.empty()) tool_ = Tool::ScatterTrees;
}

// Adds one plant of another level to this map: a culture layer (free bits, MATerrainInfo fields) and a copy of the type with
// its generators. The new layer starts empty; paint it with the Plants brush.
void App::plant_add(const ScenarioIO::PlantSource& src) {
    if (!doc_ || !io_ || !editable()) return;
    plant_msg_.clear();
    switch (io_->add_plant(*doc_, src, true)) {
        case ScenarioIO::PlantResult::NoCultureSystem:
            plant_msg_ = "This map's level has no ground plants system, so plants cannot be painted on it. \"Scatter it as objects\" puts the plant on the map as objects.";
            return;
        case ScenarioIO::PlantResult::NoFreeBits:
            plant_msg_ = "This map has no room for another plant layer. \"Scatter it as objects\" puts the plant on the map as objects.";
            return;
        case ScenarioIO::PlantResult::AlreadyHere:
            plant_msg_ = pretty_name(src.name) + " is already on this map: pick it in the Plants list.";
            return;
        default: break;
    }
    push_undo("add plant");
    bool fresh = false;
    io_->add_plant(*doc_, src, false, &fresh);
    culture_.doc = nullptr;
    load_culture();
    int pick = -1;
    for (int i = 0; i < int(culture_.types.size()); i++)
        if (iequals(culture_.types[size_t(i)].name, src.name)) { pick = i; break; }
    if (pick >= 0) {
        const CultureType& c = culture_.types[size_t(pick)];
        if (fresh && !culture_.map.empty()) {  // the layer starts empty whatever the map held in those bits
            const uint16_t keep = uint16_t(~(((1u << c.bits) - 1u) << c.shift));
            for (uint16_t& v : culture_.map) v &= keep;
            store_culture();
        }
        plant_pick_ = pick;
        plant_erase_ = false;
        tool_ = Tool::PaintCulture;
    }
    world_changed();
    status_ = pretty_name(src.name) + " added to this map's plants: drag on the map to grow it.";
}

// The Add a plant list: every plant of every level, a picture of what it grows, its cost in graphics memory, and the two
// ways to use it (a painted layer, or objects through the Scatter brush).
void App::plant_picker_ui() {
    static const void* opened_for = nullptr;   // The list belongs to the map it was opened on
    if (plant_picker_open_) {
        ImGui::OpenPopup("Add a plant##plantpick");
        plant_picker_open_ = false;
        plant_msg_.clear();
        opened_for = doc_.get();
    }
    ImGui::SetNextWindowSize(ImVec2(620 * dpi_, 560 * dpi_), ImGuiCond_Appearing);
    if (!theme::begin_modal("Add a plant##plantpick", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (!io_ || !doc_ || !cat_ || doc_.get() != opened_for) {
        ImGui::CloseCurrentPopup();
        theme::end_modal();
        return;
    }
    palette_thumb_budget_ = 2;
    palette_gfx_budget_ = 4;
    ImGui::TextWrapped("Grass, bushes and small rocks from every level of the game. An added plant can be painted with the Plants brush; its model files are copied into your scenario when you save, so each one adds to the graphics memory.");
    const auto& src = io_->plant_sources();
    const float tile = 48 * dpi_;
    const float foot = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2 + (plant_msg_.empty() ? 0.f : ImGui::GetTextLineHeight() * 3);
    if (ImGui::BeginChild("##plantlist", ImVec2(0, -foot), ImGuiChildFlags_Borders)) {
        std::string level;
        int shown = 0;
        for (int i = 0; i < int(src.size()); i++) {
            const ScenarioIO::PlantSource& ps = src[size_t(i)];
            if (ps.level != level) {
                level = ps.level;
                ImGui::SeparatorText(pretty_name(ps.map).c_str());
            }
            shown++;
            ImGui::PushID(i);
            const TemplateInfo* t = cat_->find(ps.templates.front());
            const ImVec2 top = ImGui::GetCursorScreenPos();
            ID3D11ShaderResourceView* th = nullptr;
            if (t && ImGui::IsRectVisible(top, ImVec2(top.x + tile, top.y + tile)) && (models_.thumbnail_cached(t->model, t->mtb) || palette_thumb_budget_-- > 0)) th = palette_picture(*t);
            if (th) ImGui::Image((ImTextureID)(intptr_t)th, ImVec2(tile, tile));
            else ImGui::Dummy(ImVec2(tile, tile));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(plant_label_of(*this, cat_.get(), ps.name, ps.templates.front()).c_str());
            float mb = gfx_cell_mb(ps.templates.front(), false);
            if (mb < 0 && palette_gfx_budget_ > 0) { palette_gfx_budget_--; mb = gfx_cell_mb(ps.templates.front(), true); }
            ImGui::SameLine();
            if (mb < 0) ImGui::TextDisabled("...");
            else if (mb < 0.05f) ImGui::TextDisabled("(no extra graphics memory)");
            else ImGui::TextDisabled("(adds about %.1f MB)", mb);
            bool already = false;
            for (auto& c : culture_.types) already = already || iequals(c.name, ps.name);
            if (kCrossLevelPlants) {
                ImGui::BeginDisabled(already || !editable());
                if (ImGui::Button("Add to this map##add")) {
                    plant_msg_.clear();
                    plant_add(ps);
                    if (plant_msg_.empty()) ImGui::CloseCurrentPopup();   // done: the map is usable again (a refusal stays on screen)
                }
                ImGui::EndDisabled();
                if (already) ImGui::SetItemTooltip("This map already has it.");
                ImGui::SameLine();
            }
            if (ImGui::Button("Scatter it as objects##sco")) {
                plant_scatter(ps.templates.front());
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("Puts it in the scatter brush: the Scatter tool places it as ordinary objects.");
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (!shown) ImGui::TextDisabled("No plants found on the disc.");
    }
    ImGui::EndChild();
    if (!plant_msg_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
        ImGui::TextWrapped("%s", plant_msg_.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Close", ImVec2(120 * dpi_, 0))) ImGui::CloseCurrentPopup();
    theme::end_modal();
}

// Paint > Plants: which of the level's ground plants to grow or clear, and plants from other levels.
void App::plants_ui() {
    if (culture_.doc != doc_.get() || culture_.objs != doc_->world.objs.size()) load_culture();
    const bool none = culture_.map.empty() || culture_.types.empty();
    if (none) {
        hint_text("This map has no ground plants (grass, bushes, small rocks) to paint yet. \"Add a plant\" brings one from any level of the game. Trees and rocks as objects: the Scatter tool.");
    } else {
        float near_m = 0;
        for (auto& c : culture_.types) near_m = std::max(near_m, c.stop_fade);
        char buf[448];
        snprintf(buf, sizeof buf, "The game grows these around the player wherever you paint them, like the grass and bushes of the real levels; painting again makes them thicker. Green on the map shows where the chosen plant grows; the plants themselves show only within %.0f m of the camera, as in the game. Trees and rocks as objects: the Scatter tool.", near_m);
        hint_text(buf);
        if (ImGui::RadioButton("Grow", !plant_erase_)) plant_erase_ = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Clear", plant_erase_)) plant_erase_ = true;
    }
    if (ImGui::Button("+ Add a plant##plants")) plant_picker_open_ = true;
    ImGui::SetItemTooltip("Grass, bushes and small rocks from any level of the game.");
    // Two homes for plants; each page names the other.
    hint_text("These grow by themselves around the player (the game's own ground plants, not objects). Single trees, rocks or grass clumps placed as objects: the Scatter tool (Build tab).");
    if (none) return;
    plant_pick_ = std::clamp(plant_pick_, 0, int(culture_.types.size()) - 1);
    std::set<std::pair<int, int>> shown;  // one row per culture-map field (a type can have several generators)
    const float sz = ImGui::GetFrameHeight() * 2.f;
    for (int i = 0; i < int(culture_.types.size()); i++) {
        const CultureType& c = culture_.types[size_t(i)];
        if (!shown.insert({c.shift, c.bits}).second) continue;
        ImGui::PushID(i);
        const TemplateInfo* t = cat_ ? cat_->find(c.templ) : nullptr;
        if (ID3D11ShaderResourceView* th = t ? palette_picture(*t) : nullptr) ImGui::Image((ImTextureID)(intptr_t)th, ImVec2(sz, sz));
        else ImGui::Dummy(ImVec2(sz, sz));
        bool click = ImGui::IsItemClicked();
        ImGui::SameLine();
        ImGui::BeginGroup();
        std::string label = plant_label(c);
        if (ImGui::RadioButton(label.c_str(), plant_pick_ == i) || click) plant_pick_ = i;
        if (t) ImGui::SetItemTooltip("Grows %s (%.0f per square km at full).", t->name.c_str(), c.density);
        if (t && ImGui::SmallButton("Scatter it as objects##sco")) plant_scatter(c.templ);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (!show_culture_) {
        hint_text("View > Ground scatter is off, so the plants are not drawn here.");
        if (ImGui::SmallButton("Show them")) show_culture_ = true;
    }
}

}  // namespace ed
