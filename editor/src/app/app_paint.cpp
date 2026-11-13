// The Paint section of the Terrain tab (P1). Three modes side by side: Texture, Colour tint, Boundaries.
//
// Texture mode shows ONE palette of the ground textures of the map (up to 8). Two textures (the MATerrain
// pair, First / Second) keep the retail path: the colour map's alpha mixes them, which every version of the
// game understands. Adding a third switches the scenario to the P4 layers (core/ground_layers.h, PC only);
// removing back to two bakes the mix back into the colour map's alpha.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "theme.h"
#include "app.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "ui_util.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
// Also while the item is greyed out: that is when a newcomer most needs to be told why.
static void tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}
static void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
static bool mode_button(const char* label, Tool t, Tool& cur, float w, const char* tp) {
    bool on = cur == t;
    bool pressed = theme::toggle_button(label, on, ImVec2(w, 0));
    tip(tp);
    if (pressed) cur = on ? Tool::Select : t;
    return pressed;
}
static std::string short_name(const std::string& p) {
    std::string n = p.substr(p.find_last_of("/\\") + 1);
    return n.substr(0, n.find_last_of('.'));
}
// Plain names for the ground textures on the disc (by look; the file name stays in the tooltip).
static std::string friendly_name(const std::string& p) {
    static const std::pair<const char*, const char*> names[] = {
        {"dante_detail_03", "Pebbles"},          {"dante_detail_06", "Lava rock"},
        {"m05_detail02", "Cracked earth"},       {"missionxx_detail01", "Smooth stone"},
        {"details_010_cracked", "Cracked ground"},
        {"missionxx_detail02", "Boulders"},      {"northwind_detail_02", "Dark rock"},
        {"northwind_detail_03", "Mossy gravel"}, {"northwind_detail_04", "Pale stone"},
        {"northwind_detail_05", "Light gravel"}, {"northwind_detail_06", "Rubble"},
        {"prototype_detail_01", "Dry crust"},    {"rushhourdetail_01", "Broken paving"},
        {"rushhourdetail_02", "Concrete"},       {"space_null_terrain", "Plain (no pattern)"},
        {"spot", "Soft spot"},                   {"testmission_detailmap1", "Grass"},
        {"testmission_detailmap2", "Speckled stone"}, {"uraskicked_park", "Park picture"},
        {"uraskicked_park_detail", "Sand"},      {"uraskicked_park_mdetail01", "Gravel"},
    };
    std::string n = lower(short_name(p));
    for (auto& [file, name] : names)
        if (n == file) return name;
    return short_name(p);
}
static std::string stored_path(const std::string& p) {
    std::string s = "\\" + p;
    for (auto& ch : s) if (ch == '/') ch = '\\';
    return s;
}
static const char* kDetailKeys[2] = {"TerrainDetailMap", "TerrainDetailMap 1"};

// The engine always blends a pair of textures. A map that uses only one keeps the same file in the second
// slot at 0% (a new map does): the palette hides that slot, and the next added texture takes it.
static bool second_slot_hidden(const Scenario& s) {
    if (s.ground.enabled) return false;
    int ti = find_class(s.world, K_TERRAIN);
    if (ti < 0) return false;
    const WObj& t = s.world.objs[size_t(ti)];
    std::string a = t.str(kDetailKeys[0]), b = t.str(kDetailKeys[1]);
    return b.empty() || iequals(norm_path(a), norm_path(b));
}

void App::ensure_detail_textures() {
    if (!detail_textures_.empty()) return;
    std::set<std::string> seen;
    std::vector<std::string> archives = gd_.level_archives();
    archives.insert(archives.begin(), doc_->base);
    archives.push_back("common.mgf");
    for (auto& an : archives) {
        MgfReader* a = gd_.archive(an);
        if (!a) continue;
        for (auto& f : a->files()) {
            std::string l = lower(f.path);
            if (!istarts_with(l, "terrain/") || !iends_with(l, ".tif")) continue;
            if (l.find("colormap") != std::string::npos || l.find("clormap") != std::string::npos || l.find("lightmap") != std::string::npos ||
                istarts_with(l, "terrain/scn"))
                continue;
            if (seen.insert(l).second) detail_textures_.push_back({f.path, lower(an)});
        }
    }
    std::sort(detail_textures_.begin(), detail_textures_.end(), [](auto& a, auto& b) { return lower(a.first) < lower(b.first); });
}

// Retail pair -> P4 layers (the weights start as the colour map's alpha mix, so nothing changes visibly).
void App::enable_ground_layers() {
    Scenario& s = *doc_;
    GroundLayers& g = s.ground;
    g = GroundLayers();
    g.enabled = true;
    g.layers.resize(2);
    int ti = find_class(s.world, K_TERRAIN);
    if (ti >= 0) {
        g.layers[0].repeats = s.world.objs[size_t(ti)].f32("TerrainDetailMult", 64);
        g.layers[1].repeats = s.world.objs[size_t(ti)].f32("TerrainDetailMult1", 64);
    }
    g.init_from_colormap(s.colormap);
    s.colormap_custom = true;
    s.dirty = true;
    r_.set_ground_layers(&g);
}

// P4 layers -> retail pair: the colour map's alpha takes the pair's share of the weights.
void App::disable_ground_layers() {
    Scenario& s = *doc_;
    GroundLayers& g = s.ground;
    g.bake_colormap_alpha(s.colormap);
    g = GroundLayers();
    r_.set_colormap(s.colormap);
    r_.set_ground_layers(&g);
    if (paint_texture_ > 1) paint_texture_ = 0;
    s.colormap_custom = true;
    s.dirty = true;
    minimap_dirty_ = true;
}

bool App::add_ground_texture(const std::string& path, const std::string& archive) {
    Scenario& s = *doc_;
    if (s.colormap.px.empty()) return false;
    // A texture that is already on the map is selected instead of added twice.
    {
        int ti = find_class(s.world, K_TERRAIN);
        const int n = s.ground.enabled ? s.ground.count() : 2;
        for (int k = 0; k < n; k++) {
            std::string have = k < 2 ? (ti >= 0 ? s.world.objs[size_t(ti)].str(kDetailKeys[k]) : std::string()) : s.ground.layers[size_t(k)].path;
            if (!have.empty() && iequals(norm_path(have), norm_path(path))) {
                paint_texture_ = k;
                tool_ = Tool::PaintTexture;
                return true;
            }
        }
    }
    // One texture showing: the new one takes the hidden second slot (still the retail pair, works everywhere).
    if (second_slot_hidden(s)) {
        if (!replace_ground_texture(1, path, archive)) return false;
        undo_.back().label = "add ground texture";
        paint_texture_ = 1;
        tool_ = Tool::PaintTexture;
        return true;
    }
    if (s.ground.enabled && s.ground.count() >= kGroundMaxLayers) return false;
    GroundLayer L;
    L.archive = archive;
    L.path = path;
    MgfReader* a = gd_.archive(archive);
    std::string err;
    if (!a || !load_ground_texture(*a, path, L.image, &err)) {
        message("Ground texture", "Could not read " + path + ": " + err);
        return false;
    }
    push_undo("add ground texture");
    if (!s.ground.enabled) enable_ground_layers();
    GroundLayers& g = s.ground;
    L.repeats = g.layers[0].repeats;
    g.layers.push_back(std::move(L));
    paint_texture_ = g.count() - 1;
    tool_ = Tool::PaintTexture;
    r_.set_ground_layers(&g);
    s.dirty = true;
    return true;
}

bool App::replace_ground_texture(int k, const std::string& path, const std::string& archive) {
    Scenario& s = *doc_;
    if (k < 2) {
        int ti = find_class(s.world, K_TERRAIN);
        if (ti < 0) return false;
        push_undo("ground texture");
        s.world.objs[size_t(ti)].set_str(kDetailKeys[k], stored_path(path));
        s.dirty = true;
        return true;
    }
    if (!s.ground.enabled || k >= s.ground.count()) return false;
    GroundLayer L;
    L.archive = archive;
    L.path = path;
    MgfReader* a = gd_.archive(archive);
    std::string err;
    if (!a || !load_ground_texture(*a, path, L.image, &err)) {
        message("Ground texture", "Could not read " + path + ": " + err);
        return false;
    }
    push_undo("ground texture");
    L.repeats = s.ground.layers[size_t(k)].repeats;
    s.ground.layers[size_t(k)] = std::move(L);
    r_.set_ground_layers(&s.ground);
    s.dirty = true;
    return true;
}

// Removes an extra layer (k >= 2); two left = back to the retail pair. The pair's second texture (k = 1, two
// textures) goes back to being the hidden copy of the first, its painting turned into the first.
void App::remove_ground_layer(int k) {
    Scenario& s = *doc_;
    GroundLayers& g = s.ground;
    if (k == 1 && !g.enabled && !second_slot_hidden(s)) {
        int ti = find_class(s.world, K_TERRAIN);
        if (ti < 0 || s.colormap.px.empty()) return;
        WObj& t = s.world.objs[size_t(ti)];
        push_undo("remove ground texture");
        status_ = "Removed " + friendly_name(t.str(kDetailKeys[1])) + ": where it was painted, " + friendly_name(t.str(kDetailKeys[0])) +
                  " shows. Ctrl+Z puts it back.";
        status_time_ = 5;
        t.set_str(kDetailKeys[1], t.str(kDetailKeys[0]));
        for (auto& px : s.colormap.px) px |= 0xFF000000u;
        s.colormap_custom = true;
        r_.set_colormap(s.colormap);
        paint_texture_ = 0;
        s.dirty = true;
        minimap_dirty_ = true;
        return;
    }
    if (!g.enabled || k < 2 || k >= g.count()) return;
    push_undo("remove ground texture");
    // No confirm (it undoes), so say what went and how to get it back.
    status_ = "Removed " + friendly_name(g.layers[size_t(k)].path) + ": where it was painted, the other textures show. Ctrl+Z puts it back.";
    status_time_ = 5;
    g.remove_layer(k);
    if (paint_texture_ == k) paint_texture_ = 0;
    else if (paint_texture_ > k) paint_texture_--;
    if (g.count() <= 2) disable_ground_layers();
    else r_.set_ground_layers(&g);
    s.dirty = true;
    minimap_dirty_ = true;
}

void App::fill_ground(int layer) {
    Scenario& s = *doc_;
    push_undo("ground texture fill");
    if (s.ground.enabled) {
        s.ground.fill(layer);
        r_.set_ground_weights(s.ground);
    } else {
        for (auto& p : s.colormap.px) p = (p & 0x00FFFFFFu) | (layer ? 0u : 0xFF000000u);
        s.colormap_custom = true;
    }
    // Like the brush: with "Show it in its own colours" on, the tint goes to neutral too, so the texture
    // looks like itself instead of tinted by the old ground colour.
    if (paint_neutral_colour_ && !s.colormap.px.empty()) {
        uint32_t n = uint32_t(std::clamp(1.f / (4.f * std::max(0.05f, r_.detail.blend)), 0.f, 1.f) * 255.f + 0.5f);
        for (auto& p : s.colormap.px) p = (p & 0xFF000000u) | n << 16 | n << 8 | n;
        s.colormap_custom = true;
    }
    if (!s.colormap.px.empty()) r_.set_colormap(s.colormap);
    s.dirty = true;
    minimap_dirty_ = true;
}

// The thumbnail picker for adding / changing a texture: every ground texture on the disc.
void App::ground_picker_ui() {
    if (open_pick_) {
        ImGui::OpenPopup("Ground texture##pick");
        open_pick_ = false;
        pick_filter_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(600 * dpi_, 520 * dpi_), ImGuiCond_Appearing);
    if (!theme::begin_modal("Ground texture##pick", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ensure_detail_textures();
    if (pick_target_ < 0) {
        ImGui::TextUnformatted("Pick a texture to add to this map");
    } else {
        // Name the slot being changed and what it is now (retail rebuild H7: the picker looked the same for both).
        const Scenario& s = *doc_;
        int ti = find_class(s.world, K_TERRAIN);
        std::string now = pick_target_ < 2 ? (ti >= 0 ? s.world.objs[size_t(ti)].str(kDetailKeys[pick_target_]) : std::string())
                                           : pick_target_ < int(s.ground.layers.size()) ? s.ground.layers[size_t(pick_target_)].path : std::string();
        ImGui::Text("Replace %s with...", now.empty() ? "this texture" : friendly_name(now).c_str());
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pf", "Filter by name (grass, rock, sand...)", &pick_filter_);
    std::string f = lower(pick_filter_);
    std::vector<int> idx;
    for (int i = 0; i < int(detail_textures_.size()); i++)
        if (f.empty() || lower(short_name(detail_textures_[size_t(i)].first)).find(f) != std::string::npos ||
            lower(friendly_name(detail_textures_[size_t(i)].first)).find(f) != std::string::npos)
            idx.push_back(i);
    int chosen = -1;
    float tile = 84 * dpi_, gap = ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::BeginChild("##grid", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders)) {
        // An image button is the picture plus its frame padding on both sides: without it the last column was cut off.
        const float cell = tile + 2 * ImGui::GetStyle().FramePadding.x;
        int cols = std::max(1, int((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize + gap) / (cell + gap)));
        int rows = (int(idx.size()) + cols - 1) / cols;
        float rowh = tile + ImGui::GetTextLineHeight() * 2 + ImGui::GetStyle().ItemSpacing.y * 3 + ImGui::GetStyle().FramePadding.y * 2;
        MgfReader* base = gd_.archive(doc_->base);
        ImGuiListClipper clip;
        clip.Begin(rows, rowh);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++)
                for (int c = 0; c < cols; c++) {
                    int n = r * cols + c;
                    if (n >= int(idx.size())) break;
                    auto& [path, ar] = detail_textures_[size_t(idx[size_t(n)])];
                    if (c) ImGui::SameLine();
                    ImGui::PushID(idx[size_t(n)]);
                    ImGui::BeginGroup();
                    ID3D11ShaderResourceView* srv = models_.texture(path);
                    // Opaque grey behind the image: a texture with no alpha ('Plain (no pattern)') showed as an empty tile.
                    bool hit = srv ? ImGui::ImageButton("##t", (ImTextureID)(intptr_t)srv, ImVec2(tile, tile), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.5f, 0.5f, 0.5f, 1))
                                   : ImGui::Button("?", ImVec2(cell, cell));
                    if (hit) chosen = idx[size_t(n)];
                    std::string nm = friendly_name(path);
                    for (auto& ch : nm) if (ch == '_') ch = ' ';  // so the name can wrap under the thumbnail
                    float y0 = ImGui::GetCursorPosY();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
                    ImGui::TextWrapped("%s", nm.c_str());
                    ImGui::PopTextWrapPos();
                    float used = ImGui::GetCursorPosY() - y0, want = ImGui::GetTextLineHeight() * 2 + ImGui::GetStyle().ItemSpacing.y;
                    if (used < want) ImGui::Dummy(ImVec2(cell, want - used - ImGui::GetStyle().ItemSpacing.y));
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered()) {
                        bool own = base && base->find(path) >= 0;
                        ImGui::SetTooltip("%s (file %s)\n%s", friendly_name(path).c_str(), short_name(path).c_str(),
                                          own ? "Ships with this map's Base level." : ("From " + ar.substr(0, ar.find('.')) + ": copied into your scenario when you save.").c_str());
                    }
                    ImGui::PopID();
                }
        if (idx.empty()) ImGui::TextDisabled("No texture matches.");
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel", ImVec2(120 * dpi_, 0))) ImGui::CloseCurrentPopup();
    if (chosen >= 0) {
        auto [path, ar] = detail_textures_[size_t(chosen)];
        if (pick_target_ < 0) add_ground_texture(path, ar);
        else replace_ground_texture(pick_target_, path, ar);
        ImGui::CloseCurrentPopup();
    }
    theme::end_modal();
}

// Texture mode: the palette.
void App::ground_textures_ui() {
    Scenario& s = *doc_;
    int ti = find_class(s.world, K_TERRAIN);
    if (ti < 0) { ImGui::TextDisabled("This map has no terrain object."); return; }
    ensure_detail_textures();
    GroundLayers& g = s.ground;
    WObj& t = s.world.objs[size_t(ti)];
    const int n = g.enabled ? g.count() : 2;
    paint_texture_ = std::clamp(paint_texture_, 0, n - 1);
    if (g.enabled) {
        g.layers[0].repeats = t.f32("TerrainDetailMult", 64);
        g.layers[1].repeats = t.f32("TerrainDetailMult1", 64);
    }
    // Slots the palette shows: the hidden copy in the pair's second slot is left out (see second_slot_hidden).
    std::vector<int> shown;
    for (int k = 0; k < n; k++)
        if (k != 1 || !second_slot_hidden(s)) shown.push_back(k);
    if (std::find(shown.begin(), shown.end(), paint_texture_) == shown.end()) paint_texture_ = 0;
    auto slot_path = [&](int k) { return k < 2 ? t.str(kDetailKeys[k]) : g.layers[size_t(k)].path; };
    hint("Click a texture, then drag on the map to paint it. The brush only changes the ground it touches, so you can build the ground up one texture at a time.");
    if (tool_ == Tool::PaintTexture)
        ImGui::TextColored(theme::tok().text_value, "Painting with: %s", friendly_name(slot_path(paint_texture_)).c_str());
    else
        ImGui::TextDisabled("Click a texture to start painting.");
    ImGui::Checkbox("Show it in its own colours", &paint_neutral_colour_);
    tip("Ground = texture x the map's colour tint, so a texture painted over green ground looks green. On: the brush also clears the tint under it. Off: only the texture changes.");

    // Share of the map per texture (sampled; refreshed a few times a second).
    static float share[kGroundMaxLayers];
    if (ImGui::GetFrameCount() % 12 == 0) {
        for (float& v : share) v = 0;
        if (g.enabled && g.weights) {
            double sum[kGroundMaxLayers] = {}, tot = 0;
            for (int y = 0; y < g.h; y += 8)
                for (int x = 0; x < g.w; x += 8) {
                    const uint8_t* w = g.texel(x, y);
                    for (int k = 0; k < kGroundMaxLayers; k++) { sum[k] += w[k]; tot += w[k]; }
                }
            for (int k = 0; k < kGroundMaxLayers; k++) share[k] = tot > 0 ? float(sum[k] / tot) : 0.f;
        } else if (!s.colormap.px.empty()) {
            size_t cnt = s.colormap.px.size(), step = std::max<size_t>(1, cnt / 40000), m = 0;
            double sum = 0;
            for (size_t i = 0; i < cnt; i += step, m++) sum += double(s.colormap.px[i] >> 24) / 255.0;
            share[0] = m ? float(sum / double(m)) : 0.f;
            share[1] = 1.f - share[0];
        }
        if (second_slot_hidden(s)) { share[0] = 1.f; share[1] = 0.f; }
    }

    // The palette: one row of picture swatches with plain names (wraps when the panel is narrow), '+' last.
    ImGui::SeparatorText("Ground textures on this map");
    const float sz = ImGui::GetFrameHeight() * 2.6f, gap = ImGui::GetStyle().ItemSpacing.x;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    int remove = -1;
    bool first = true;
    auto next_cell = [&] {
        if (!first && ImGui::GetItemRectMax().x + gap + sz <= right) ImGui::SameLine();
        first = false;
    };
    for (int k : shown) {
        ImGui::PushID(k);
        std::string pth = slot_path(k);
        bool chosen = paint_texture_ == k && tool_ == Tool::PaintTexture;
        next_cell();
        ImGui::BeginGroup();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ID3D11ShaderResourceView* srv = pth.empty() ? nullptr : models_.texture(pth);
        bool clicked = ImGui::InvisibleButton("##sw", ImVec2(sz, sz));
        bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + sz, p0.y + sz), IM_COL32(128, 128, 128, 255));
        if (srv) dl->AddImage((ImTextureID)(intptr_t)srv, p0, ImVec2(p0.x + sz, p0.y + sz));
        dl->AddRect(p0, ImVec2(p0.x + sz, p0.y + sz),
                    chosen ? ImGui::ColorConvertFloat4ToU32(theme::tok().accent) : ImGui::GetColorU32(hov ? ImGuiCol_ButtonHovered : ImGuiCol_Border), 0, 0,
                    chosen ? 3.f * dpi_ : 1.f);
        if (hov) {
            const float sh = share[k];
            std::string cov = sh > 0 && sh < 0.005f ? "under 1%" : std::to_string(int(sh * 100.f + 0.5f)) + "%";
            ImGui::SetTooltip("%s - covers %s of the map\nClick to paint with it; right-click to change or remove it.\nFile: %s",
                              friendly_name(pth).c_str(), cov.c_str(), short_name(pth).c_str());
        }
        if (ImGui::BeginPopupContextItem("##swmenu")) {
            ImGui::TextDisabled("%s", friendly_name(pth).c_str());
            if (ImGui::MenuItem("Change...")) { pick_target_ = k; open_pick_ = true; }
            tip("Swaps this texture for another one (anything painted with it changes too).");
            bool can_remove = shown.size() > 1 && k != 0 && (k >= 2 || !g.enabled);
            if (ImGui::MenuItem("Remove", nullptr, false, can_remove)) remove = k;
            tip(can_remove ? "Takes this texture out of the map; where it was painted, the others show."
                : k == 0   ? (friendly_name(pth) + " is the map's base ground: change it instead.").c_str()
                           : "Remove the textures after it first (or use Back to two textures), or change it instead.");
            ImGui::EndPopup();
        }
        std::string nm = friendly_name(pth);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + sz);
        if (chosen) ImGui::TextColored(theme::tok().text_value, "%s", nm.c_str());
        else ImGui::TextWrapped("%s", nm.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (clicked) { paint_texture_ = k; tool_ = Tool::PaintTexture; }
        ImGui::PopID();
    }
    if (remove >= 0) remove_ground_layer(remove);

    const bool full = n >= kGroundMaxLayers && !second_slot_hidden(s);
    next_cell();
    ImGui::BeginGroup();
    ImGui::BeginDisabled(full || s.colormap.px.empty());
    if (ImGui::Button("+##addtex", ImVec2(sz, sz))) { pick_target_ = -1; open_pick_ = true; }
    ImGui::EndDisabled();
    tip(s.colormap.px.empty() ? "This map has no ground colour map, so its ground cannot be painted."
        : full               ? "8 textures is the most a map can have."
                             : "Add a texture: any ground texture of any level (up to 8). It is picked for painting right away.");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + sz);
    ImGui::TextDisabled("Add");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (s.colormap.px.empty()) hint("This map has no ground colour map, so its ground cannot be painted or covered with extra textures.");
    if (s.ground.enabled) {
        const std::string a = friendly_name(slot_path(0)), b = friendly_name(slot_path(1));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
        ImGui::TextWrapped("More than two textures: only this PC version shows them all. The original Xbox game and older PC builds show only %s and %s.", a.c_str(), b.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Back to two textures"))
            confirm("Keep only " + a + " and " + b + "? The others leave the map; where they were painted, " + a + " and " + b + " show instead (Ctrl+Z undoes).",
                    [this] { push_undo("two ground textures"); disable_ground_layers(); });
        tip(("Drops the other textures; " + a + " and " + b + " keep their painted share, so it works in every version of the game.").c_str());
        help(("The original Xbox game, and older PC builds, blend only two textures (" + a + " and " + b + "): where you painted the others they show those two, mixed as you painted them. This PC version blends all of them, on a texture map four times finer than the colour map.").c_str());
    }
}

// Colour tint mode: the colour map's RGB multiplies the textures (4 x detail x colour in the game's combiner).
void App::tint_ui() {
    Scenario& s = *doc_;
    hint("The ground colour tints the textures: grey leaves them as they are, a colour shifts them toward it, so one texture can look like grass, dirt or scorched earth.");
    // Ready-made tints (the swatch shows the colour the brush lays down; "No tint" = the texture's own colours).
    {
        const float neutral = std::clamp(1.f / (4.f * std::max(0.05f, r_.detail.blend)), 0.f, 1.f);
        struct Preset { const char* name; float r, g, b; };
        const Preset presets[] = {
            {"No tint (texture's own colours)", neutral, neutral, neutral},
            {"Grass", 0.35f, 0.43f, 0.27f}, {"Dry grass", 0.49f, 0.47f, 0.31f}, {"Dirt", 0.45f, 0.37f, 0.27f},
            {"Sand", 0.65f, 0.59f, 0.45f},  {"Snow", 0.84f, 0.86f, 0.88f},      {"Rock", 0.47f, 0.47f, 0.46f},
            {"Scorched", 0.24f, 0.22f, 0.20f},
        };
        ImGui::TextUnformatted("Pick a colour:");
        const float sw = ImGui::GetFrameHeight() * 1.4f;
        for (int i = 0; i < int(sizeof presets / sizeof presets[0]); i++) {
            const Preset& pr = presets[size_t(i)];
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            if (ImGui::ColorButton(pr.name, ImVec4(pr.r, pr.g, pr.b, 1.f), ImGuiColorEditFlags_NoAlpha, ImVec2(sw, sw))) {
                paint_color_[0] = pr.r; paint_color_[1] = pr.g; paint_color_[2] = pr.b;
            }
            ImGui::PopID();
        }
        if (level_tints_n_) {
            // The tints taken from a real level's Terrain tab (dark / average / light), to mottle the ground like it.
            ImGui::Text("From %s:", level_tints_name_.c_str());
            static const char* what[] = {"darker", "average", "lighter"};
            for (int i = 0; i < level_tints_n_; i++) {
                ImGui::SameLine();
                ImGui::PushID(100 + i);
                const ImVec4 c(level_tints_[i][0], level_tints_[i][1], level_tints_[i][2], 1.f);
                if (ImGui::ColorButton("##leveltint", c, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(sw, sw))) {
                    paint_color_[0] = c.x; paint_color_[1] = c.y; paint_color_[2] = c.z;
                }
                char t[96];
                snprintf(t, sizeof t, "%s's %s tint.", level_tints_name_.c_str(), what[std::min(i, 2)]);
                tip(t);
                ImGui::PopID();
            }
        }
    }
    ImGui::ColorEdit3("Exact colour", paint_color_);
    tip("Any colour: click the square for a colour picker, or type red, green and blue values.");
    if (!s.colormap.px.empty()) {
        const bool picking = tint_pick_;
        if (theme::toggle_button(picking ? "Click the ground to take its colour...###eyedrop" : "Pick a colour from the map###eyedrop", picking, ImVec2(-1, 0))) tint_pick_ = !picking;
        tip("Eyedropper: click this, then click the ground to tint with the colour that is there. Alt+click on the map does the same while tinting.");
    }
}

// Boundaries mode: where players and bots may go (the ground-type layer).
void App::boundaries_ui() {
    Scenario& s = *doc_;
    if (s.typemap.empty()) { ImGui::TextDisabled("This map has no ground-type layer."); return; }
    static const uint16_t values[] = {tmtype::Playable, tmtype::Special, tmtype::Border, tmtype::OutOfBounds};
    static const char* hints[] = {"players and bots go here (shown clear)", "water or waterway inside the arena", "band around the arena", "nothing moves here"};
    for (int i = 0; i < 4; i++) {
        // Playable is drawn clear on the map, so its swatch is the see-through checkerboard (a green one matched nothing).
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(type_overlay_color(values[i]) | (values[i] == tmtype::Playable ? 0u : 0xFF000000u));
        ImGui::ColorButton(("##tc" + std::to_string(i)).c_str(), col,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_AlphaPreview,
                           ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        char lab[96];
        snprintf(lab, sizeof lab, "%s: %s", tmtype::label(values[i]), hints[i]);
        if (ImGui::RadioButton(lab, paint_type_value_ == values[i])) {
            paint_type_value_ = values[i];
            tool_ = Tool::PaintType;
            if (!show_typemap_) show_typemap_ = typemap_auto_ = true;
            overlay_dirty_ = true;
        }
    }
    {
        // Deep water (16) is set by Deep lakes, not painted: shown so the darker blue is explained.
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(type_overlay_color(tmtype::DeepWater) | 0xFF000000u);
        ImGui::ColorButton("##tcdeep", col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted("Deep water: too deep to wade on foot (Deep lakes)");
    }
    // Other retail values show purple: explain them only on maps that have some (a new map has none).
    bool other = false;
    for (uint16_t v : s.typemap)
        if (v != tmtype::Playable && v != tmtype::Special && v != tmtype::DeepWater && v != tmtype::Border && v != tmtype::OutOfBounds) { other = true; break; }
    if (other) {
        ImGui::ColorButton("##tcother", ImGui::ColorConvertU32ToFloat4(type_overlay_color(0xFFFF) | 0xFF000000u), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                           ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted("Other: special ground of the real level; leave it as it is");
    }
    help("Spawn points, vehicle pads, flags and other objectives must stand on Playable (or Water) ground, and bots never walk onto Out of bounds ground. New maps start playable everywhere except a thin border at the edge. Copies of real levels keep their own layout. Whole map > \"Make the whole map playable\" resets it.");
}

// The options of the chosen paint tool (the tool row is on the Terrain tab).
void App::paint_section_ui() {
    if (tool_ == Tool::PaintColor) tint_ui();
    else if (tool_ == Tool::PaintCulture) plants_ui();
    else if (tool_ == Tool::PaintType) {
        if (ImGui::Checkbox("Show where players may go", &show_typemap_)) overlay_dirty_ = true, typemap_auto_ = false;
        tip("Colours the map: clear = playable, orange = border, red = out of bounds, blue = water.");
        boundaries_ui();
    } else ground_textures_ui();
}

// Terrain tab on a real level (read-only): what its ground is made of, so a user can copy the recipe.
void App::terrain_read_only_ui() {
    Scenario& s = *doc_;
    ImGui::SeparatorText("How this level's ground is made");
    int ti = find_class(s.world, K_TERRAIN);
    const Image& cm = s.colormap;
    if (ti >= 0) {
        // Colour-map alpha is the texture mix: 255 = all Texture 1.
        double a = 0;
        for (uint32_t c : cm.px) a += c >> 24;
        const double share1 = cm.px.empty() ? 1 : a / (255.0 * double(cm.px.size()));
        const int n = s.ground.enabled ? s.ground.count() : 2;
        for (int k = 0; k < n; k++) {
            std::string pth = k < 2 ? s.world.objs[size_t(ti)].str(kDetailKeys[k]) : s.ground.layers[size_t(k)].path;
            if (pth.empty() || (k == 1 && second_slot_hidden(s))) continue;
            // Pattern size as Advanced > Pattern size shows it: one tile in metres.
            const float rep = k < 2 ? s.world.objs[size_t(ti)].f32(k ? "TerrainDetailMult1" : "TerrainDetailMult", 64) : s.ground.layers[size_t(k)].repeats;
            const float tile_m = std::max(1.f, s.terrain.size_x()) / std::max(1.f, rep);
            if (n == 2 && !second_slot_hidden(s)) ImGui::TextWrapped("%s: about %.0f%% of the ground", friendly_name(pth).c_str(), 100.0 * (k ? 1 - share1 : share1));
            else if (n == 2) ImGui::TextWrapped("%s: all of the ground", friendly_name(pth).c_str());
            else ImGui::TextUnformatted(friendly_name(pth).c_str());
            ImGui::SetItemTooltip("%s\nPattern size %.0f m (Advanced > Pattern size).", pth.c_str(), tile_m);
        }
    }
    if (!cm.px.empty()) {
        // Tint: the average and the darkest / lightest tenth, as swatches.
        std::vector<std::pair<int, uint32_t>> lum;
        lum.reserve(cm.px.size());
        double sum[3] = {};
        for (uint32_t c : cm.px) {
            int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
            sum[0] += r, sum[1] += g, sum[2] += b;
            lum.push_back({r * 3 + g * 6 + b, c});
        }
        std::sort(lum.begin(), lum.end(), [](auto& x, auto& y) { return x.first < y.first; });
        auto col = [](uint32_t c) { return ImVec4((c & 255) / 255.f, ((c >> 8) & 255) / 255.f, ((c >> 16) & 255) / 255.f, 1.f); };
        const double n = double(cm.px.size());
        ImGui::TextUnformatted("Colour tint:");
        // Clicking a swatch keeps it as the Colour tint tool's colour, to paint the same on your own map.
        auto swatch = [&](const char* id, ImVec4 c, const char* what) {
            ImGui::SameLine();
            if (ImGui::ColorButton(id, c, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip)) {
                paint_color_[0] = c.x, paint_color_[1] = c.y, paint_color_[2] = c.z;
                level_tints_name_ = s.name;
                const ImVec4 keep[3] = {col(lum[lum.size() / 10].second), ImVec4(float(sum[0] / n / 255), float(sum[1] / n / 255), float(sum[2] / n / 255), 1.f),
                                        col(lum[lum.size() * 9 / 10].second)};
                for (int i = 0; i < 3; i++) level_tints_[i][0] = keep[i].x, level_tints_[i][1] = keep[i].y, level_tints_[i][2] = keep[i].z;
                level_tints_n_ = 3;
                status_ = ("Colour tint set to this level's " + std::string(what) + " colour: the Colour tint tool paints it on your own map.");
            }
            char t[160];
            snprintf(t, sizeof t, "The %s tint. Click to use it with the Colour tint tool on your own map.", what);
            tip(t);
        };
        swatch("##rodark", col(lum[lum.size() / 10].second), "darker");
        swatch("##roavg", ImVec4(float(sum[0] / n / 255), float(sum[1] / n / 255), float(sum[2] / n / 255), 1.f), "average");
        swatch("##rolight", col(lum[lum.size() * 9 / 10].second), "lighter");
        ImGui::SameLine();
        ImGui::TextDisabled("dark / average / light");
    }
    if (culture_.doc != doc_.get() || culture_.objs != s.world.objs.size()) load_culture();
    std::set<std::pair<int, int>> shown;
    for (const CultureType& c : culture_.types) {
        if (!shown.insert({c.shift, c.bits}).second || culture_.map.empty()) continue;
        const int top = std::max(1, (1 << c.bits) - 1);
        size_t on = 0;
        for (uint16_t v : culture_.map) on += ((v >> c.shift) & top) != 0;
        ImGui::TextWrapped("Plants: %s on about %.0f%% of the ground", plant_label(c).c_str(), 100.0 * double(on) / double(culture_.map.size()));
    }
    if (shown.empty()) ImGui::TextUnformatted("Plants: none");
    ImGui::Text("Ground height: %.0f to %.0f m", s.terrain.min_h, s.terrain.max_h);
    ImGui::Spacing();
    hint("To change it, press \"Make an editable copy\" on the Map tab. On a new map the same tools are under Shape the ground and Paint the ground.");
}

// Whole map > Copy the tint of a real level: its colour map at the same metres per texel, centred (tiled when this
// map is bigger). The texture mix (alpha) stays.
void App::copy_level_tint(const std::string& level) {
    const MapInfo* m = gd_.find_map(level);
    Scenario src;
    std::string err;
    if (!m || !doc_ || !io_ || !io_->open_retail(*m, src, &err) || src.colormap.px.empty() || doc_->colormap.px.empty()) {
        message("Copy the tint", "Could not read " + pretty_name(level) + "'s colour tint" + (err.empty() ? std::string(".") : ": " + err));
        return;
    }
    push_undo("copy ground colours");
    Image& dst = doc_->colormap;
    const Image& from = src.colormap;
    const float ms = doc_->terrain.size_x(), mz = doc_->terrain.size_z(), rs = src.terrain.size_x(), rz = src.terrain.size_z();
    const float ox = (rs - ms) * 0.5f, oz = (rz - mz) * 0.5f;
    auto texel = [&](float u, float v) {  // bilinear, wrapping
        u = u * from.w - 0.5f, v = v * from.h - 0.5f;
        int x0 = int(std::floor(u)), y0 = int(std::floor(v));
        float fx = u - float(x0), fy = v - float(y0), out[3] = {};
        for (int k = 0; k < 4; k++) {
            int x = ((x0 + (k & 1)) % from.w + from.w) % from.w, y = ((y0 + (k >> 1)) % from.h + from.h) % from.h;
            float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            uint32_t c = from.at(x, y);
            for (int ch = 0; ch < 3; ch++) out[ch] += wgt * float((c >> (8 * ch)) & 255);
        }
        uint32_t r = 0;
        for (int ch = 0; ch < 3; ch++) r |= uint32_t(std::clamp(int(out[ch] + 0.5f), 0, 255)) << (8 * ch);
        return r;
    };
    for (int y = 0; y < dst.h; y++)
        for (int x = 0; x < dst.w; x++) {
            float u = ((x + 0.5f) / dst.w * ms + ox) / std::max(1.f, rs), v = ((y + 0.5f) / dst.h * mz + oz) / std::max(1.f, rz);
            uint32_t& p = dst.px[size_t(y) * dst.w + x];
            p = (p & 0xFF000000u) | texel(u, v);
        }
    doc_->colormap_custom = true;
    r_.set_colormap(dst);
    doc_->dirty = true;
    minimap_dirty_ = true;
    status_ = "Copied " + pretty_name(level) + "'s colour tint (Ctrl+Z undoes).";
}

// Terrain tab > Whole map: the buttons that change every part of the map at once.
void App::terrain_whole_map_ui() {
    Scenario& s = *doc_;
    if (s.read_only) return;
    int ti = find_class(s.world, K_TERRAIN);
    if (!s.colormap.px.empty() && ti >= 0) {
        ImGui::SeparatorText("Texture");
        const int n = s.ground.enabled ? s.ground.count() : 2;
        paint_texture_ = std::clamp(paint_texture_, 0, n - 1);
        // The texture picked in the palette, by its swatch name (never the hidden copy in the pair's second slot).
        if (paint_texture_ == 1 && second_slot_hidden(s)) paint_texture_ = 0;
        std::string pth = paint_texture_ < 2 ? s.world.objs[size_t(ti)].str(kDetailKeys[paint_texture_]) : s.ground.layers[size_t(paint_texture_)].path;
        std::string name = pth.empty() ? std::string("this texture") : friendly_name(pth);
        const float pic = ImGui::GetFrameHeight();
        if (ID3D11ShaderResourceView* srv = pth.empty() ? nullptr : models_.texture(pth)) {
            ImGui::Image((ImTextureID)(intptr_t)srv, ImVec2(pic, pic));
            tip(("The texture picked in the palette (Texture tool): " + name + ".").c_str());
            ImGui::SameLine();
        }
        if (ImGui::Button(("Cover the whole map with " + name).c_str(), ImVec2(-1, 0))) {
            int chosen = paint_texture_;
            confirm("Cover the whole map with " + name + "? This REPLACES all the texture painting you have done: the other textures disappear from the ground" +
                        std::string(paint_neutral_colour_ ? ", and the colour tint is reset so it shows in its own colours" : "") + " (Ctrl+Z undoes).",
                    [this, chosen] { fill_ground(chosen); });
        }
        tip("Replaces everything: the whole map becomes the texture picked in the palette (Texture tool), and your painting is wiped. To add a patch instead, drag on the map with the brush. With \"Show it in its own colours\" on (Texture tool), the colour tint is reset too; otherwise it stays.");
    }
    if (!s.colormap.px.empty()) {
        ImGui::SeparatorText("Colour tint");
        ImGui::ColorButton("##wholetint", ImVec4(paint_color_[0], paint_color_[1], paint_color_[2], 1.f), ImGuiColorEditFlags_NoAlpha);
        tip("The colour chosen with the Colour tint tool.");
        ImGui::SameLine();
        if (ImGui::Button("Tint the whole map with this colour", ImVec2(-1, 0))) {
            confirm("Tint the whole map with this colour? This replaces all the colour painting you have done (Ctrl+Z undoes). The textures stay.", [this] {
                push_undo("ground colour fill");
                uint32_t c = uint32_t(std::clamp(int(paint_color_[0] * 255 + 0.5f), 0, 255)) | uint32_t(std::clamp(int(paint_color_[1] * 255 + 0.5f), 0, 255)) << 8 |
                             uint32_t(std::clamp(int(paint_color_[2] * 255 + 0.5f), 0, 255)) << 16;
                for (auto& p : doc_->colormap.px) p = (p & 0xFF000000u) | c;
                doc_->colormap_custom = true;
                r_.set_colormap(doc_->colormap);
                doc_->dirty = true;
                minimap_dirty_ = true;
            });
        }
        tip("Sets the tint everywhere. The texture painting is not touched.");
        // A real level's tint carries its hand-painted look (Bogged Down's pale mud cracks, dark patches): copy it.
        const MapInfo* from = nullptr;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##tintfrom", "Copy the tint of a real level...", ImGuiComboFlags_HeightLarge)) {
            for (auto& m : gd_.maps())
                if (m.resource_present && ImGui::Selectable(pretty_name(m.name).c_str())) from = &m;
            ImGui::EndCombo();
        }
        tip("Gives your map the colour painting of a level from the game (its dark and light patches, paths and cracks) at the same size on the ground. Replaces your colour painting; the textures stay (Ctrl+Z undoes).");
        if (from) {
            const std::string name = from->name;
            confirm("Copy " + pretty_name(name) + "'s colour tint onto this map? This replaces all the colour painting you have done (Ctrl+Z undoes). The textures stay.",
                    [this, name] { copy_level_tint(name); });
        }
        if (ImGui::Button("Repaint ground colours from height", ImVec2(-1, 0))) {
            // Asks like the other whole-map buttons: on a copy of a real level it replaces the level's own colour work.
            confirm("Repaint the whole map's colours from its height? This replaces all the colour painting (Ctrl+Z undoes). The textures stay.", [this] {
                Scenario& d = *doc_;
                push_undo("ground colours");
                int sz = d.colormap_layout.width ? int(d.colormap_layout.width) : 256;
                // The colour-map alpha is the retail texture mix: keep what the layers or the brush made.
                Image old = d.colormap;
                d.colormap = io_->procedural_colormap(d, sz);
                if (old.w == d.colormap.w && old.h == d.colormap.h)
                    for (size_t i = 0; i < old.px.size(); i++) d.colormap.px[i] = (d.colormap.px[i] & 0x00FFFFFFu) | (old.px[i] & 0xFF000000u);
                d.colormap_custom = true;
                r_.set_colormap(d.colormap);
                d.dirty = true;
                minimap_dirty_ = true;
            });
        }
        tip("Paints the whole map automatically: valleys, slopes and hilltops get the theme's colours. Replaces your colour painting; the texture painting stays (Ctrl+Z undoes).");
    }
    if (!s.typemap.empty()) {
        ImGui::SeparatorText("Boundaries");
        if (ImGui::Button("Make the whole map playable", ImVec2(-1, 0))) {
            confirm("Make the whole map playable? This replaces where players may go (on a real level: its borders and out-of-bounds areas) with a thin border at the edge (Ctrl+Z undoes).", [this] {
                push_undo("terrain types");
                fill_playable_typemap(*doc_);
                doc_->dirty = true;
                // Shown while the Boundaries tool is on; it goes again with the tool, as when the tool turned it on.
                tool_ = Tool::PaintType;
                if (!show_typemap_) show_typemap_ = typemap_auto_ = true;
                overlay_dirty_ = true;
            });
        }
        tip("Resets where players may go: playable everywhere except a thin border at the edge, like a new map (Ctrl+Z undoes).");
    }
}

// Terrain tab > Advanced: pattern size of each texture and the close-up detail.
void App::terrain_advanced_paint_ui() {
    Scenario& s = *doc_;
    int ti = find_class(s.world, K_TERRAIN);
    if (ti < 0) return;
    GroundLayers& g = s.ground;
    WObj& t = s.world.objs[size_t(ti)];
    const int n = g.enabled ? g.count() : 2;
    ImGui::SeparatorText("Pattern size");
    // One row per texture in the palette (picture + name); the hidden copy in the pair's second slot is left out.
    const bool hide1 = second_slot_hidden(s);
    for (int k = 0; k < n; k++) {
        if (k == 1 && hide1) continue;
        ImGui::PushID(k);
        std::string pth = k < 2 ? t.str(kDetailKeys[k]) : g.layers[size_t(k)].path;
        const float pic = ImGui::GetFrameHeight();
        if (ID3D11ShaderResourceView* srv = pth.empty() ? nullptr : models_.texture(pth)) ImGui::Image((ImTextureID)(intptr_t)srv, ImVec2(pic, pic));
        else ImGui::Dummy(ImVec2(pic, pic));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * dpi_);
        float rep = k < 2 ? t.f32(k ? "TerrainDetailMult1" : "TerrainDetailMult", 64) : g.layers[size_t(k)].repeats;
        // Shown as the size of one pattern tile in metres (bigger = bigger pattern); stored as repeats across the map.
        const float map_m = std::max(1.f, s.terrain.size_x());
        float tile_m = map_m / std::max(1.f, rep);
        if (ImGui::DragFloat((pth.empty() ? std::string("(no texture)") : friendly_name(pth)).c_str(), &tile_m, 0.1f, map_m / 512.f, map_m, "%.0f m")) {
            push_undo_once("pattern size");
            rep = std::clamp(map_m / std::max(0.5f, tile_m), 1.f, 512.f);
            if (k < 2) t.set_f32(k ? "TerrainDetailMult1" : "TerrainDetailMult", rep);
            else g.layers[size_t(k)].repeats = rep;
            if (g.enabled) { if (k < 2) g.layers[size_t(k)].repeats = rep; r_.set_ground_layers(&g); }
            s.dirty = true;
        }
        tip("How big one tile of this texture's pattern is on the ground. Bigger = bigger stones, cracks and grain.");
        ImGui::PopID();
    }
    // Not paint: a separate fine grain over all the ground near the camera, so it is never named like a palette texture.
    ImGui::SeparatorText("Close-up grain (seen within ~128 m, not painted)");
    {
        // The close-up detail pair (TerrainMicroDetailMap0/1): two fine textures averaged and laid over
        // everything within about 128 m. The game draws it only when both are set; it is not painted.
        MgfReader* base = gd_.archive(s.base);
        static const char* mkeys[2] = {"TerrainMicroDetailMap0", "TerrainMicroDetailMap1"};
        for (int k = 0; k < 2; k++) {
            ImGui::PushID(100 + k);
            std::string cur = t.str(mkeys[k]);
            // One row per grain texture: its name, then its tile size in metres (as Pattern size above).
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            if (ImGui::BeginCombo("##micro", cur.empty() ? "(none)" : friendly_name(cur).c_str())) {
                if (ImGui::Selectable("(none)", cur.empty())) { push_undo("close-up detail"); t.set_str(mkeys[k], ""); s.dirty = true; }
                for (auto& [path, ar] : detail_textures_) {
                    bool own = base && base->find(path) >= 0;
                    std::string l = friendly_name(path) + (own ? "  (this level)" : "  (from " + ar.substr(0, ar.find('.')) + ")");
                    if (ImGui::Selectable(l.c_str(), iequals(norm_path(cur), norm_path(path)))) {
                        push_undo("close-up detail");
                        t.set_str(mkeys[k], stored_path(path));
                        s.dirty = true;
                    }
                    if (ImGui::IsItemHovered())
                        if (ID3D11ShaderResourceView* pv = models_.texture(path)) {
                            ImGui::BeginTooltip();
                            ImGui::Image((ImTextureID)(intptr_t)pv, ImVec2(128 * dpi_, 128 * dpi_));
                            ImGui::EndTooltip();
                        }
                }
                ImGui::EndCombo();
            }
            tip("Two fine textures, averaged and laid over the whole ground when the player is within about 128 m (they are not painted). The game shows them only when both are set.");
            static const char* tkeys[2] = {"TerrainMicroDetailTiling0", "TerrainMicroDetailTiling1"};
            const float map_m = std::max(1.f, s.terrain.size_x());
            float tile_m = map_m / std::max(1.f, t.f32(tkeys[k], 240));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70 * dpi_);
            if (ImGui::DragFloat(k ? "Grain 2 size" : "Grain 1 size", &tile_m, 0.05f, map_m / 1000.f, map_m, "%.1f m")) {
                push_undo_once("close-up detail size");
                t.set_f32(tkeys[k], std::clamp(map_m / std::max(0.05f, tile_m), 1.f, 1000.f));
                s.dirty = true;
            }
            tip("How big one tile of this close-up texture is on the ground. Bigger = coarser grain.");
            ImGui::PopID();
        }
    }
}

}  // namespace ed
