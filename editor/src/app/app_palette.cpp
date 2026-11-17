// The palette browser: one list of entries (templates, road pieces, water types), drawn as pictures or rows through
// ImGuiListClipper, in three modes (Place, Roads & walls, Scatter). The chips, the filter popup, the cells, the right-click menu
// and drag-and-drop onto the map are here; the placing options under the list stay in app_tabs.cpp (palette_footer).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "../core/json.h"
#include "../core/triggers.h"
#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "palette_defs.h"
#include "theme.h"
#include "ui_tips.h"
#include "ui_util.h"

namespace ed {
using namespace ma2;
using namespace ma2::world;

namespace {

enum ChipKind { C_GROUP, C_HELPERS, C_CUT, C_NATURAL, C_WALLS, C_BRIDGES, C_BASES, C_ROADS, C_ALL };
struct Chip { const char* label; const char* tip; ChipKind kind; int group; };

const std::vector<Chip>& chips_for(BrowserMode m) {
    static std::vector<Chip> place, roads, scatter;
    if (place.empty()) {
        for (int g : {PG_MECHS, PG_VEHICLES, PG_SOLDIERS, PG_CIVILIANS, PG_TURRETS, PG_DEFENSES, PG_BUILDINGS, PG_LIGHTS, PG_SIGNS, PG_SCENERY, PG_PICKUPS, PG_EFFECTS})
            place.push_back({kPaletteGroups[g][0], kPaletteGroups[g][1], C_GROUP, g});
        place.push_back({"Helpers", "Lights, checkpoints, markers, menu triggers and trigger areas: things that do something but are not drawn in the game.", C_HELPERS, PG_SCENERY});
        place.push_back({"Special", kPaletteGroups[PG_SPECIAL][1], C_GROUP, PG_SPECIAL});
        place.push_back({"All", kPaletteGroups[PG_ALL][1], C_ALL, PG_ALL});
        place.push_back({"Not in the game", "Objects cut before the game was released: their 3D model is on none of the disc's archives, and no real level uses them. You can place one, but the game shows a pink placeholder.", C_CUT, PG_ALL});
        roads.push_back({"Roads", kPaletteGroups[PG_ROADS][1], C_ROADS, PG_ROADS});
        roads.push_back({"Walls & fences", "Walls, gates, fences and barriers that join end to end.", C_WALLS, PG_DEFENSES});
        roads.push_back({"Bridges & docks", "Bridges, docks and piers that join end to end.", C_BRIDGES, PG_BRIDGES});
        roads.push_back({"Bases & paving", "Foundations, parking lots, pavement and the concrete plates buildings stand on (they join edge to edge).", C_BASES, PG_ROADS});
        roads.push_back({"All of these", "Every road piece, wall, bridge and base plate.", C_ALL, PG_ROADS});
        scatter.push_back({"Natural", "Trees, bushes, plants, rocks, ice and crystals from every planet.", C_NATURAL, PG_ALL});
        scatter.push_back({"Scenery", kPaletteGroups[PG_SCENERY][1], C_GROUP, PG_SCENERY});
        scatter.push_back({"Buildings", "Buildings and towers.", C_GROUP, PG_BUILDINGS});
        scatter.push_back({"All", "Everything that can be scattered.", C_ALL, PG_ALL});
    }
    return m == BrowserMode::Place ? place : m == BrowserMode::RoadsWalls ? roads : scatter;
}

const char* mode_name(BrowserMode m) { return m == BrowserMode::Place ? "Place" : m == BrowserMode::RoadsWalls ? "Roads & walls" : "Scatter"; }

int search_rank(const std::string& c) {
    if (c == "MAMech" || c == "MATank" || c == "MAVehicle" || c == "MAFlyer" || c == "MATurret" || c == "MAPerson") return 0;
    return c == "MAEffect" ? 2 : 1;
}

}  // namespace

bool App::browser_mode_of_page(BrowserMode& m) const {
    if (page_ == Page::Place) m = BrowserMode::Place;
    else if (page_ == Page::RoadsWalls) m = BrowserMode::RoadsWalls;
    else if (page_ == Page::Scatter) m = BrowserMode::Scatter;
    else return false;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Entries

void App::palette_build(BrowserMode m, PalCache& pc, int chip, const std::string& filt) {
    Scenario& s = *doc_;
    pc.entries.clear();
    pc.headings.clear();
    pc.heading_at.clear();
    const auto& chips = chips_for(m);
    const Chip& ch = chips[size_t(std::clamp(chip, 0, int(chips.size()) - 1))];
    const bool searching = !filt.empty();
    ChipKind ck = ch.kind;
    if (searching) ck = C_ALL;  // a search spans every chip of the page
    if (ck == C_HELPERS) { pc.chip_label = ch.label; return; }
    pc.chip_label = ch.label;
    const std::string& narrow = pal_state_[int(m)].category;
    int base_idx = -1;
    for (size_t i = 0; i < gd_.level_archives().size(); i++) if (iequals(gd_.level_archives()[i], s.base)) base_idx = int(i);
    const auto& base_set = cat_->culled_set(s.base_level());
    const bool group_filter = narrow == kGroupNatural || narrow == kGroupCity;
    const bool extra_group = !group_filter && std::any_of(palette_extra_groups().begin(), palette_extra_groups().end(), [&](auto& g) { return narrow == g.first; });
    std::set<uint32_t> fx_here;  // templates of the effect objects the level has (the "Effects in this level" view)
    if (narrow == kFxHere)
        for (auto& o : s.world.objs) if (o.cls == key_of("MAEffect")) fx_here.insert(o.template_id());

    struct Key { int pass, nrank, rank, other; long order; };
    std::vector<std::pair<Key, PalEntry>> out;

    // Road pieces: no template; every theme can use them.
    const bool want_roads = (m == BrowserMode::Place && ck == C_ALL && (narrow.empty() || narrow == "Roads & pavement")) ||
                            (m == BrowserMode::RoadsWalls && (ck == C_ROADS || ck == C_ALL));
    if (want_roads) {
        const auto& rps = road_pieces();
        for (size_t i = 0; i < rps.size(); i++) {
            const RoadPiece& p = rps[i];
            // up10deg / down10deg are the 60 m straight under another name (same model, flat ends): they only confused (playtest 11-04).
            if (lower(p.path).find("10deg.road") != std::string::npos) continue;
            RankText rt;
            rt.visible = p.friendly + " road piece";
            rt.official = p.official + "\n" + fs::path(p.path).stem().string();
            rt.aliases = p.search;
            const int tier = palette_rank(filt, rt);
            if (tier < 0) continue;
            PalEntry e;
            e.kind = PalEntry::Road;
            e.road = int(i);
            e.name = p.friendly;
            if (!e.name.empty()) e.name[0] = char(std::toupper(uint8_t(e.name[0])));
            char sz[32];
            if (p.l <= 1.01f) std::snprintf(sz, sizeof sz, "bridge section");   // Its .road header has no length ("road 1 m"); the real size shows on the ghost
            else std::snprintf(sz, sizeof sz, "%.0f m", double(p.l));
            e.size = sz;
            e.tier = tier;
            // Ranked like any object (tier, class, A-Z); on a tie while searching they follow the objects.
            out.push_back({{0, tier, 1, 0, searching ? 1000000 + long(i) : -100000 + long(i)}, std::move(e)});
        }
    }

    long order = 0;
    if (!(m == BrowserMode::RoadsWalls && ck == C_ROADS)) {
        for (const TemplateInfo* tp : palette_sorted()) {
            order++;
            const TemplateInfo& t = *tp;
            if (!t.placeable || t.name == "NULL") continue;
            if (t.model.empty() && t.cls_name == "MACorporeal") continue;  // units without their own model (spawners) are not useful here
            // A mission has no game types: the Capture the Flag flags and bases and the Snatch It bases and core do nothing there.
            if (!s.multiplayer && (t.cls_name == "MAFlagCTF" || t.cls_name == "MASnatchItBase" || t.cls_name == "MASnatchItDataCore" || t.name.rfind("CTF Flagbase", 0) == 0))
                continue;
            const std::string pcat = palette_category(t);
            const bool special = special_category_tip(pcat) != nullptr;
            const bool cut = !t.model.empty() && io_->model_absent(s, t.uid);
            if (cut != (m == BrowserMode::Place && ck == C_CUT)) continue;  // cut content only in its own chip
            if (m == BrowserMode::Place) {
                if (ck == C_GROUP && !palette_in_group(t, special, ch.group)) continue;
            } else if (m == BrowserMode::Scatter) {
                // Scatter lists only what is worth dropping many of: no water, skies, cutscene props or helpers.
                if (t.cls_name == "MAWater" || t.cls_name == "MASky2" || special) continue;
                if (snap_family(t) == 2) continue;   // Bridges and docks are 100 m and more: not for dropping many at random ("L rock bridge long")
                if (ck == C_NATURAL && !in_group(t, kGroupNatural)) continue;
                if (ck == C_GROUP && !palette_in_group(t, false, ch.group)) continue;
            } else {
                if (t.cls_name == "MAWater" || t.cls_name == "MASky2" || special) continue;
                const int fam = snap_family(t);
                const bool walls = fam == 1, bridges = fam == 2 || palette_in_group(t, false, PG_BRIDGES), bases = fam == 3 || palette_group_has(t, "Roads & pavement");
                const bool ok = ck == C_WALLS ? walls : ck == C_BRIDGES ? bridges : ck == C_BASES ? bases : (walls || bridges || bases);
                if (!ok) continue;
            }
            if (m != BrowserMode::RoadsWalls) {
                if (narrow == kFxHere) {
                    if (!fx_here.count(t.uid)) continue;
                } else if (!narrow.empty() && (group_filter ? !in_group(t, narrow) : extra_group ? !palette_group_has(t, narrow) : pcat != narrow))
                    continue;
            }
            const std::string shown = palette_plain_name(t);
            const int nrank = palette_rank_of(filt, t, shown);
            if (nrank < 0) continue;
            const bool other = !(base_set.count(t.uid) || std::find(t.archives.begin(), t.archives.end(), base_idx) != t.archives.end());
            PalEntry e;
            e.kind = t.cls_name == "MAWater" ? PalEntry::Water : PalEntry::Template;
            e.t = &t;
            e.name = shown;
            e.tier = nrank;
            e.theme = !other;
            e.cut = cut;
            e.special = special;
            out.push_back({{special ? 1 : 0, nrank, searching ? search_rank(t.cls_name) : 0, searching ? 0 : (other ? 1 : 0), order}, std::move(e)});
            // While searching, this theme's objects still come before the others of the same rank.
            if (searching) out.back().first.other = other ? 1 : 0;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) {
        const Key &x = a.first, &y = b.first;
        if (x.pass != y.pass) return x.pass < y.pass;
        if (x.nrank != y.nrank) return x.nrank < y.nrank;
        if (x.rank != y.rank) return x.rank < y.rank;
        if (x.other != y.other) return x.other < y.other;
        return x.order < y.order;
    });
    pc.entries.reserve(out.size());
    int last_pass = 0;
    bool other_heading = false;
    for (size_t i = 0; i < out.size(); i++) {
        const Key& k = out[i].first;
        if (k.pass == 1 && last_pass == 0 && m == BrowserMode::Place && ck == C_ALL && narrow.empty()) {
            pc.headings.push_back("Special: cutscene props, wrecks, decoration, skies");
            pc.heading_at.push_back({int(i), int(pc.headings.size()) - 1});
        } else if (!searching && k.pass == 0 && k.other == 1 && !other_heading) {
            other_heading = true;
            pc.headings.push_back("From other levels (each adds graphics memory)");
            pc.heading_at.push_back({int(i), int(pc.headings.size()) - 1});
        }
        last_pass = k.pass;
        pc.entries.push_back(std::move(out[i].second));
    }
    // Two different objects never read the same: the later one gets its game name (or a number) after it.
    {
        std::map<std::string, int> seen;
        for (auto& e : pc.entries) {
            if (e.kind != PalEntry::Template || !e.t) continue;
            const int n = ++seen[lower(e.name)];
            if (n == 1) continue;
            e.name += " (" + (e.t->name.empty() || lower(e.t->name) == lower(e.name) ? std::to_string(n) : e.t->name) + ")";
        }
    }
    pc.cols = 0;  // rows are packed again by the body
}

// ---------------------------------------------------------------------------------------------
// Size of a piece (say how long a footbridge or canal wall is before it is placed)

bool App::palette_model_dims(const TemplateInfo& t, float& dx, float& dy, float& dz) {
    if (t.model.empty()) return false;
    GpuModel* gm = models_.get(t.model, t.mtb);  // queues the load; the answer comes a few frames later
    if (!gm || gm->state != GpuModel::Ready) return false;
    const float* lo = gm->has_vbox ? gm->vmin : gm->bmin;
    const float* hi = gm->has_vbox ? gm->vmax : gm->bmax;
    dx = hi[0] - lo[0];
    dy = hi[1] - lo[1];
    dz = hi[2] - lo[2];
    if (dx > 0.01f || dz > 0.01f) measured_dims_[t.uid] = {dx, dz};
    return dx > 0.01f || dz > 0.01f;
}

// "140 m" for a long thin piece, "38 x 38 m" for a plate: from the model box, for walls, bridges and bases (snap families 1-3).
void App::palette_entry_size(const PalEntry& e, bool force) {
    if (!e.size.empty() || e.kind != PalEntry::Template || !e.t || snap_family(*e.t) == 0) return;
    if (!force && !models_.thumbnail_cached(e.t->model, e.t->mtb) && palette_thumb_budget_-- <= 0) return;  // a model loads on the thumbnail budget, 2 a frame
    float dx, dy, dz;
    if (!palette_model_dims(*e.t, dx, dy, dz)) return;
    e.name = fix_size_token(e.name, dx, dz);  // "34x34" in a base's name follows the model's measured size
    const float a = std::max(dx, dz), b = std::min(dx, dz);
    char sz[48];
    if (b < 0.3f * a) std::snprintf(sz, sizeof sz, "%.0f m", double(a));
    else std::snprintf(sz, sizeof sz, "%.0f x %.0f m", double(a), double(b));
    e.size = sz;
}

// ---------------------------------------------------------------------------------------------
// Arming and the right-click menu

void App::palette_arm(const PalEntry& e) {
    if (e.kind == PalEntry::Road) {
        const RoadPiece& p = road_pieces()[size_t(e.road)];
        place_road_ = p.path;
        place_template_ = 0;
        tool_ = Tool::PlaceObject;
        return;
    }
    const TemplateInfo& t = *e.t;
    Scenario& s = *doc_;
    if (t.cls_name == "MASky2") {
        tab_ = Tab::Map;
        status_ = "Skies are chosen on the Environment tab (Sky).";
        status_time_ = 6;
    } else if (t.cls_name == "MAWater" && water_can_resize(t.uid)) {
        // Water is placed like a unit (pick it here, click the map) with the Place water tool: any size, the water look as the preview.
        arm_water(t.uid);
    } else {
        if (place_template_ != t.uid || !place_road_.empty() || tool_ != Tool::PlaceObject) {
            // A new pick faces the way the camera looks (the last object's drag-aimed heading no longer carries over silently).
            XMFLOAT3 ro, rd;
            cam_.ray(vp_w * 0.5f, vp_h * 0.5f, vp_w, vp_h, ro, rd);
            if (place_template_ != t.uid && rd.x * rd.x + rd.z * rd.z > 1e-6f) place_heading_ = angle_snapped(std::atan2(rd.x, rd.z));
        }
        place_template_ = t.uid;
        place_road_.clear();
        tool_ = Tool::PlaceObject;
        // A Red / Blue team turret picked on a multiplayer map: the "Placing for" picker follows its colour.
        if (s.multiplayer && (t.cls_name == "MATurret" || t.cls_name == "MAPerson")) {
            std::string n = lower(t.name);
            if (n.find(" - red") != std::string::npos || n.find("(team 1)") != std::string::npos) place_team_ = 1;
            else if (n.find(" - blue") != std::string::npos || n.find("(team 2)") != std::string::npos) place_team_ = 2;
        }
    }
}

void App::palette_context_menu(const PalEntry& e, BrowserMode m) {
    const bool road = e.kind == PalEntry::Road;
    if (ImGui::MenuItem("Place it")) palette_arm(e);
    if (!road && e.kind == PalEntry::Template && e.t->cls_name != "MASky2" && !e.special) {
        const bool in = std::find(scatter_set_.begin(), scatter_set_.end(), e.t->uid) != scatter_set_.end();
        if (ImGui::MenuItem(in ? "Remove from the scatter brush" : "Add to the scatter brush")) scatter_toggle(e.t->uid, m == BrowserMode::Scatter);
    }
    if (!road) {
        int n = 0;
        for (auto& o : doc_->world.objs) if (o.template_id() == e.t->uid) n++;
        char lab[96];
        std::snprintf(lab, sizeof lab, "Select all of these on the map (%d)###selall", n);
        if (ImGui::MenuItem(lab, nullptr, false, n > 0)) {
            selection_.clear();
            for (auto& o : doc_->world.objs) if (o.template_id() == e.t->uid) selection_.insert(o.uid());
            if (tool_ == Tool::PlaceObject || tool_ == Tool::ScatterTrees || tool_ == Tool::PlaceWater) tool_ = Tool::Select;
            status_ = std::to_string(n) + (n == 1 ? " object selected." : " objects selected.");
            status_time_ = 4;
        }
    }
    if (ImGui::MenuItem("Copy the game name")) {
        const std::string nm = road ? road_pieces()[size_t(e.road)].path : e.t->name;
        ImGui::SetClipboardText(nm.c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// One cell (picture or row)

static std::string gfx_list_note(float mb, bool on_map) {
    if (on_map) return "  - from another level (already on the map)";
    if (mb < 0.05f) return "  - from another level";
    char b[48];
    std::snprintf(b, sizeof b, "  - from another level (+%.1f MB)", double(mb));
    return b;
}

void App::palette_cell(const PalEntry& e, BrowserMode m, float cell, float cw) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const bool road = e.kind == PalEntry::Road;
    const bool pictures = settings_.pal_pictures;
    const RoadPiece* rp = road ? &road_pieces()[size_t(e.road)] : nullptr;
    const TemplateInfo* t = e.t;
    const uint32_t uid = t ? t->uid : 0;
    const std::string cell_id = road ? "##road_" + fs::path(rp->path).stem().string()
                                     : (e.kind == PalEntry::Water ? "##water_" : "##cell_") + std::to_string(uid);
    const std::string tick_id = "##tick_" + std::to_string(uid);
    const bool in_scatter = !road && std::find(scatter_set_.begin(), scatter_set_.end(), uid) != scatter_set_.end();
    bool sel = road ? tool_ == Tool::PlaceObject && iequals(place_road_, rp->path)
                    : (place_template_ == uid && place_road_.empty() && tool_ == Tool::PlaceObject) || (t->cls_name == "MAWater" && water_new_tpl_ == uid && tool_ == Tool::PlaceWater);
    if (m == BrowserMode::Scatter) sel = in_scatter;  // a ticked cell keeps the lit look
    const bool other_level = !e.theme && !e.cut;
    const char* special_tip = t ? special_category_tip(palette_category(*t)) : nullptr;
    const char* note = t ? palette_note(*t) : nullptr;
    palette_entry_size(e);
    palette_entry_gfx(e, false);
    ImGui::PushID(road ? rp->path.c_str() : cell_id.c_str());
    if (e.cut) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    bool clicked = false;
    if (pictures) {
        ID3D11ShaderResourceView* th = nullptr;
        if (road) {
            if (models_.thumbnail_cached(rp->model, "") || palette_thumb_budget_-- > 0) th = models_.thumbnail(rp->model, "");
        } else if (t->cls_name == "MAWater" || models_.thumbnail_cached(t->model, t->mtb) || palette_thumb_budget_-- > 0) th = palette_picture(*t);
        ImGui::BeginGroup();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (sel) theme::push_toggle_on();
        clicked = th ? ImGui::ImageButton(cell_id.c_str(), (ImTextureID)(intptr_t)th, ImVec2(cell, cell))
                     : ImGui::Button(((road || models_.thumbnail_cached(t->model, t->mtb) ? (road ? std::string("road") : kind_label(t->cls_name)) : std::string("...")) + cell_id).c_str(),
                                     ImVec2(cw, cell + st.FramePadding.y * 2));
        if (sel) theme::pop_toggle_on();
        if (th) name_item(cell_id);
        // Drag onto the map to place one there; right-click for the menu.
        if (m != BrowserMode::Scatter && !(t && t->cls_name == "MASky2") && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("PALCELL", &uid, sizeof uid);
            pal_drag_live_ = true;
            pal_drag_frame_ = ImGui::GetFrameCount();
            if (!sel) palette_arm(e);
            ImGui::EndDragDropSource();
        }
        const bool ctx = ImGui::BeginPopupContextItem("##palctx");
        if (ctx) { palette_context_menu(e, m); ImGui::EndPopup(); }
        const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay);
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        if (m == BrowserMode::Scatter && !road) {
            // The tick sits on the cell's top-left corner (its own item: click it or the picture, both toggle).
            const ImVec2 after = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 3 * dpi_, p0.y + 3 * dpi_));
            bool v = in_scatter;
            if (ImGui::Checkbox(tick_id.c_str(), &v)) scatter_toggle(uid);
            ImGui::SetCursorScreenPos(after);
        }
        // What an object from another level adds to the graphics memory sits on the picture's top right corner.
        if (other_level && !road && e.kind == PalEntry::Template) {
            char gb[24];
            if (e.gfx_mb < 0) std::snprintf(gb, sizeof gb, "...");
            else if (e.gfx_mb < 0.05f) gb[0] = 0;
            else std::snprintf(gb, sizeof gb, "+%.1f MB", double(e.gfx_mb));
            if (gb[0]) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 ts = ImGui::CalcTextSize(gb);
                const ImVec2 q0(mx.x - ts.x - 9 * dpi_, mn.y + 3 * dpi_), q1(mx.x - 3 * dpi_, mn.y + ts.y + 5 * dpi_);
                dl->AddRectFilled(q0, q1, IM_COL32(0, 0, 0, 170), 3 * dpi_);
                dl->AddText(ImVec2(q0.x + 3 * dpi_, q0.y + dpi_), IM_COL32(255, 200, 90, 255), gb);
            }
        }
        // The size of a modular piece sits on the picture's lower right corner (no extra row: the clipper needs equal rows).
        if (!e.size.empty()) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 ts = ImGui::CalcTextSize(e.size.c_str());
            const ImVec2 q1(mx.x - 3 * dpi_, mx.y - 3 * dpi_), q0(q1.x - ts.x - 6 * dpi_, q1.y - ts.y - 2 * dpi_);
            dl->AddRectFilled(q0, q1, IM_COL32(0, 0, 0, 170), 3 * dpi_);
            dl->AddText(ImVec2(q0.x + 3 * dpi_, q0.y + dpi_), IM_COL32(235, 235, 235, 255), e.size.c_str());
        }
        const std::string nm = palette_cell_name(e.name, cw);  // always three lines: equal rows for the clipper
        if (other_level) ImGui::TextColored(theme::tok().warn, "%s", nm.c_str());  // from another level (tooltip says so)
        else ImGui::TextUnformatted(nm.c_str());
        ImGui::EndGroup();
        if (e.cut) ImGui::PopStyleColor();
        if (hov && !ctx) {
            const bool low = mn.y > ImGui::GetMainViewport()->Size.y * 0.5f;
            ImGui::SetNextWindowPos(ImVec2(panels_.left[0] + panels_.left[2] + 8 * dpi_, low ? mx.y : mn.y), ImGuiCond_Always, ImVec2(0, low ? 1.f : 0.f));
            if (ImGui::BeginTooltip()) {
                if (road) {
                    ImGui::Text("Road: %s", rp->friendly.c_str());
                    ImGui::Text("About %.0f m long, %.0f m wide. The next piece joins the open end of the last one; Q / E turn it.", double(rp->l), double(rp->w) * 2);
                    ImGui::TextDisabled("Game name: %s", rp->path.c_str());
                } else if (e.cut) {
                    ImGui::TextUnformatted("This object was cut before the game was released: its 3D model is not on the disc,");
                    ImGui::TextUnformatted("under any name, and no real level uses it. You can place it, but the game shows a pink placeholder.");
                    if (const TemplateInfo* alt = cat_->find(io_->similar_template(*doc_, uid))) ImGui::Text("The same object that is on the disc: %s", alt->name.c_str());
                } else {
                    ImGui::TextUnformatted(e.name.c_str());  // the whole name (the cell may cut it)
                    if (ID3D11ShaderResourceView* big = palette_picture(*t)) ImGui::Image((ImTextureID)(intptr_t)big, ImVec2(128 * dpi_, 128 * dpi_));
                    float dx, dy, dz;
                    if (palette_model_dims(*t, dx, dy, dz)) ImGui::Text("About %.0f x %.0f x %.0f m (wide x long x high)", double(std::min(dx, dz)), double(std::max(dx, dz)), double(dy));
                    ImGui::Text("%s", t->category.c_str());
                    ImGui::TextDisabled("Game name: %s (template %u)", t->name.c_str(), t->uid);
                    if (other_level) {
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
                        ImGui::TextColored(theme::tok().warn, "From another level: it works, but the game loads its files too, so the map takes longer to load.");
                        if (gfx_on_map_.count(uid)) ImGui::TextColored(theme::tok().warn, "Already on the map: adds nothing.");
                        else if (e.gfx_mb >= 0) ImGui::TextColored(theme::tok().warn, "Adds about %.1f MB of graphics memory (less if it shares files with objects already on the map).", double(e.gfx_mb));
                        ImGui::PopTextWrapPos();
                    }
                    if (special_tip || note) {
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
                        ImGui::TextColored(theme::tok().warn, "%s", note ? "Driven by the computer: the player cannot climb into it, even when it is Neutral." : special_tip);
                        ImGui::PopTextWrapPos();
                    }
                }
                ImGui::EndTooltip();
            }
        }
    } else {
        // List row: [tick] name (kind) - notes.
        if (m == BrowserMode::Scatter && !road) {
            bool v = in_scatter;
            if (ImGui::Checkbox(tick_id.c_str(), &v)) scatter_toggle(uid);
            ImGui::SameLine();
        }
        std::string label = e.name + "  (" + (special_tip ? lower(palette_category(*t).substr(0, palette_category(*t).find(' ') == std::string::npos ? palette_category(*t).size() : palette_category(*t).find(" ("))) + ", " : std::string()) +
                            (road ? std::string("road piece") : kind_label(t->cls_name)) + ")" + (note ? std::string("  - ") + note : std::string()) +
                            (e.size.empty() || road ? std::string() : "  - " + e.size) + (e.cut ? "  - no model on the disc" : "") + (other_level ? gfx_list_note(e.gfx_mb, gfx_on_map_.count(uid) != 0) : std::string());
        clicked = ImGui::Selectable((fit_text(label, ImGui::GetContentRegionAvail().x - 10 * dpi_) + cell_id).c_str(), sel);   // Cut with "...", the tooltip has the whole name
        if (m != BrowserMode::Scatter && !(t && t->cls_name == "MASky2") && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("PALCELL", &uid, sizeof uid);
            pal_drag_live_ = true;
            pal_drag_frame_ = ImGui::GetFrameCount();
            if (!sel) palette_arm(e);
            ImGui::EndDragDropSource();
        }
        const bool ctx = ImGui::BeginPopupContextItem("##palctx");
        if (ctx) { palette_context_menu(e, m); ImGui::EndPopup(); }
        if (e.cut) ImGui::PopStyleColor();
        if (!ctx && ImGui::BeginItemTooltip()) {
            if (road) {
                if (ID3D11ShaderResourceView* big = models_.thumbnail(rp->model, "")) ImGui::Image((ImTextureID)(intptr_t)big, ImVec2(128 * dpi_, 128 * dpi_));
                ImGui::Text("Road: %s", rp->friendly.c_str());
                ImGui::Text("About %.0f m long, %.0f m wide. The next piece joins the open end of the last one; Q / E turn it.", double(rp->l), double(rp->w) * 2);
                ImGui::TextDisabled("Game name: %s", rp->path.c_str());
            } else {
                ImGui::TextUnformatted(e.name.c_str());
                if (!e.cut) if (ID3D11ShaderResourceView* big = palette_picture(*t)) ImGui::Image((ImTextureID)(intptr_t)big, ImVec2(128 * dpi_, 128 * dpi_));
                float dx, dy, dz;
                if (!e.cut && palette_model_dims(*t, dx, dy, dz)) ImGui::Text("About %.0f x %.0f x %.0f m (wide x long x high)", double(std::min(dx, dz)), double(std::max(dx, dz)), double(dy));
                ImGui::Text("%s", t->category.c_str());
                ImGui::TextDisabled("Game name: %s (template %u)", t->name.c_str(), t->uid);
            }
            ImGui::EndTooltip();
        }
    }
    if (clicked) {
        if (m == BrowserMode::Scatter && !road) scatter_toggle(uid);
        else palette_arm(e);
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------
// Header: search, the filter popup and the chips

void App::palette_header(BrowserMode m) {
    PalState& ps = pal_state_[int(m)];
    const auto& chips = chips_for(m);
    ImGuiStyle& st = ImGui::GetStyle();
    static std::vector<std::string> cats, special;
    if (cats.empty()) {
        std::set<std::string> set, sp;
        for (auto& t : cat_->all()) {
            if (!t.placeable) continue;
            std::string c = palette_category(t);
            (special_category_tip(c) ? sp : set).insert(c);
        }
        cats.assign(set.begin(), set.end());
        special.assign(sp.begin(), sp.end());
    }
    // Search first (Ctrl+F lands here); it spans every chip of the page. The Filter button holds the view and the narrowing.
    const float fw = ImGui::CalcTextSize("Filter").x + st.FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fw - st.ItemSpacing.x);
    if (focus_filter_) { ImGui::SetKeyboardFocusHere(); focus_filter_ = false; }
    ImGui::InputTextWithHint("##palfind", m == BrowserMode::Scatter ? "Search objects to scatter" : "Search every object (tree, tower, atlas)", &unit_filter_);
    ImGui::SameLine();
    const bool narrowed = !ps.category.empty();
    if (narrowed) theme::push_toggle_on();
    if (ImGui::Button("Filter##palfilter")) ImGui::OpenPopup("##palfilterpop");
    if (narrowed) theme::pop_toggle_on();
    ImGui::SetItemTooltip("%s", narrowed ? ("Narrowed to: " + ps.category + ". Click to change how the objects are shown or narrowed.").c_str()
                                          : "How the objects are shown (pictures or a list, their size) and, for the object pages, a narrower view (game folders, Natural or City).");
    if (ImGui::BeginPopup("##palfilterpop")) {
        ImGui::SeparatorText("Show as");
        bool changed = false;
        if (ImGui::RadioButton("Pictures##view", settings_.pal_pictures)) { settings_.pal_pictures = true; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("List##view", !settings_.pal_pictures)) { settings_.pal_pictures = false; changed = true; }
        ImGui::BeginDisabled(!settings_.pal_pictures);
        ImGui::TextUnformatted("Picture size");
        for (int i = 0; i < 3; i++) {
            ImGui::SameLine();
            if (ImGui::RadioButton((std::string(i == 0 ? "Small" : i == 1 ? "Medium" : "Large") + "##picsize").c_str(), settings_.pal_size == i)) { settings_.pal_size = i; changed = true; }
        }
        ImGui::EndDisabled();
        if (changed) settings_.save();
        if (m != BrowserMode::RoadsWalls) {
            ImGui::SeparatorText("Narrow to");
            if (begin_combo("##narrowto", ps.category.empty() ? "(no filter)" : ps.category.c_str())) {
                if (ImGui::Selectable("(no filter)", ps.category.empty())) ps.category.clear();
                for (auto& c : cats) if (ImGui::Selectable(c.c_str(), c == ps.category)) ps.category = c;
                ImGui::SeparatorText("Groups (an object can be in several)");
                for (const char* g : {kGroupNatural, kGroupCity}) {
                    if (ImGui::Selectable(g, ps.category == g)) ps.category = g;
                    ImGui::SetItemTooltip("%s", g == kGroupNatural ? "Trees, bushes, grass, plants, rocks, ice and crystals from every planet."
                                                                   : "Buildings, walls, bridges, signs, street furniture and other city and industrial pieces.");
                }
                if (ImGui::Selectable(kFxHere, ps.category == kFxHere)) ps.category = kFxHere;
                ImGui::SetItemTooltip("Only the effect objects (fires, smoke...) this level already uses, so you can place more of the same.");
                for (auto& [g, gtip] : palette_extra_groups()) {
                    if (ImGui::Selectable(g, ps.category == g)) ps.category = g;
                    ImGui::SetItemTooltip("%s", gtip);
                }
                if (!special.empty()) ImGui::SeparatorText("Special");
                for (auto& c : special) {
                    if (ImGui::Selectable(c.c_str(), c == ps.category)) ps.category = c;
                    ImGui::SetItemTooltip("%s", special_category_tip(c));
                }
                ImGui::EndCombo();
            }
            if (const char* stip = special_category_tip(ps.category)) ImGui::TextWrapped("%s", stip);
            ImGui::TextDisabled("Every object of every level is listed; ones that come from another level say so.");
        }
        ImGui::EndPopup();
    }
    // Chips: a wrapped row of buttons.
    static int cut_count = -1;
    if (cut_count < 0) {
        cut_count = 0;
        for (auto& t : cat_->all()) if (t.placeable && !t.model.empty() && io_->model_absent(*doc_, t.uid)) cut_count++;
    }
    float avail = ImGui::GetContentRegionAvail().x, x = 0;
    for (size_t i = 0; i < chips.size(); i++) {
        std::string label = chips[i].label;
        if (chips[i].kind == C_CUT) label += " (" + std::to_string(cut_count) + ")";
        const float w = ImGui::CalcTextSize(label.c_str()).x + st.FramePadding.x * 2;
        if (i && x + st.ItemSpacing.x + w <= avail) ImGui::SameLine();
        else if (i) x = 0;
        x += (x > 0 ? st.ItemSpacing.x : 0) + w;
        const bool on = ps.chip == int(i) && unit_filter_.empty();
        if (theme::toggle_button((label + (chips[i].kind == C_CUT ? "###cutchip" : "##chip")).c_str(), on)) {
            ps.chip = int(i);
            unit_filter_.clear();
        }
        ImGui::SetItemTooltip("%s", chips[i].tip);
    }
}

// ---------------------------------------------------------------------------------------------
// Body: the list, packed into rows and clipped

void App::palette_body(BrowserMode m) {
    Scenario& s = *doc_;
    PalState& ps = pal_state_[int(m)];
    const auto& chips = chips_for(m);
    const Chip& ch = chips[size_t(std::clamp(ps.chip, 0, int(chips.size()) - 1))];
    const std::string filt = lower(unit_filter_);
    // One cell row per wheel notch (the default scrolled under one row).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().MouseWheel != 0 && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
        ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - ImGui::GetIO().MouseWheel * 96.f * dpi_));
        ImGui::GetIO().MouseWheel = 0;
    }
    if (ch.kind == C_HELPERS && filt.empty() && m == BrowserMode::Place) {
        ImGui::TextWrapped("Helpers do something in the game but are not drawn in it. Pick one, then click the map.");
        ui_helper_buttons();
        return;
    }
    // Rebuild the entries when what they depend on changes (not every frame).
    PalCache& pc = pal_cache_[int(m)];
    int base_idx = -1;
    for (size_t i = 0; i < gd_.level_archives().size(); i++) if (iequals(gd_.level_archives()[i], s.base)) base_idx = int(i);
    char kb[128];
    std::snprintf(kb, sizeof kb, "%d|%d|%d|%zu|%p|%d|%d", int(m), ps.chip, base_idx, cat_->all().size(), static_cast<const void*>(cat_.get()), s.multiplayer ? 1 : 0, road_ready_ ? 1 : 0);
    const std::string key = std::string(kb) + "|" + ps.category + "|" + filt + "|" + s.base;
    static std::string last_key[3];
    if (key != pc.key) {
        pc.key = key;
        palette_build(m, pc, ps.chip, filt);
    }
    if (last_key[int(m)] != key) { ImGui::SetScrollY(0); last_key[int(m)] = key; }  // another chip or search opens at the top of its list
    palette_gfx_budget_ = 4;    // other-level figures measured this frame
    palette_thumb_budget_ = 2;  // new thumbnails rendered this frame (roads and objects share it)
    if (ch.kind == C_CUT && filt.empty())
        ImGui::TextWrapped("Cut before release: the game shows a pink placeholder for these. Kept here so nothing is lost; the list says when the same object exists under another name.");

    const bool pictures = settings_.pal_pictures;
    const float cell = (pictures ? (settings_.pal_size == 0 ? 56.f : settings_.pal_size == 2 ? 104.f : 76.f) * dpi_ : 0.f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float cw = cell + st.FramePadding.x * 2;
    const float avail = ImGui::GetContentRegionAvail().x - st.ScrollbarSize;
    const int cols = pictures ? std::max(1, int((avail + st.ItemSpacing.x) / (cw + st.ItemSpacing.x))) : 1;
    if (pc.cols != cols) {
        pc.cols = cols;
        pc.rows = pack_rows(int(pc.entries.size()), pc.heading_at, cols);
    }
    const auto& rows = pc.rows;
    int i = 0;
    const int nrows = int(rows.size());
    while (i < nrows) {
        if (rows[size_t(i)].heading >= 0) {
            ImGui::SeparatorText(pc.headings[size_t(rows[size_t(i)].heading)].c_str());
            i++;
            continue;
        }
        int j = i;
        while (j < nrows && rows[size_t(j)].heading < 0) j++;
        ImGuiListClipper clip;
        clip.Begin(j - i);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const PackRow& row = rows[size_t(i + r)];
                for (int c = 0; c < row.count; c++) {
                    if (c) ImGui::SameLine();
                    palette_cell(pc.entries[size_t(row.first + c)], m, cell, cw);
                }
            }
        i = j;
    }
    bool trig_hit = false;
    if (m == BrowserMode::Place && !filt.empty()) {
        // Trigger areas are not templates, so a search for them found nothing: offer them here.
        static const char* const kTriggerWords[] = {"trigger", "zone", "volume", "region", "area"};
        bool trig_word = false;
        for (const char* k : kTriggerWords)
            for (size_t p = 0; !trig_word && p < filt.size();) {
                size_t e = filt.find(' ', p);
                std::string w = filt.substr(p, e == std::string::npos ? std::string::npos : e - p);
                trig_word = w.size() >= 3 && std::string(k).compare(0, w.size(), w) == 0;
                if (e == std::string::npos) break;
                p = e + 1;
            }
        if (trig_word) {
            ImGui::Separator();
            ImGui::TextWrapped("Trigger areas: a box or circle the player walks into. Put one down here; what it does is set on Objectives & Triggers.");
            trigger_area_buttons("palette");
            trig_hit = true;
        }
    }
    if (pc.entries.empty() && !trig_hit) {
        ImGui::TextDisabled(filt.empty() ? "Nothing here." : "Nothing matches the search.");
        if (!filt.empty() && m == BrowserMode::Place) {
            bool any_cut = false;
            for (auto& t : cat_->all())
                if (t.placeable && t.name != "NULL" && !t.model.empty() && io_->model_absent(s, t.uid) && palette_rank_of(filt, t, palette_plain_name(t)) >= 0) { any_cut = true; break; }
            if (any_cut) ImGui::TextWrapped("The \"Not in the game\" chip has objects for this word (cut before release: the game shows a pink placeholder for them).");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Graphics memory

// A new Base (or a new document) drops the per-template figures: they are relative to what that Base already carries.
void App::gfx_reset() {
    gfx_tpl_mb_.clear();
    gfx_on_map_.clear();
    gfx_tids_.clear();
    gfx_pos_ = 0;
    gfx_sum_ = 0;
    gfx_raw_ = gfx_offset_ = 0;
    gfx_scan_ = true;
    gfx_counting_ = false;
    gfx_calibrated_ = false;
    gfx_cache_base_.clear();
}

float App::gfx_cell_mb(uint32_t uid, bool compute) {
    if (!doc_ || !io_) return -1;
    if (gfx_cache_base_ != doc_->base) { gfx_reset(); gfx_cache_base_ = doc_->base; }
    if (auto it = gfx_tpl_mb_.find(uid); it != gfx_tpl_mb_.end()) return it->second;
    if (!compute) return -1;
    const float mb = float(io_->template_graphics_mb(*doc_, uid));
    gfx_tpl_mb_[uid] = mb;
    return mb;
}

// Fills e.gfx_mb for an object from another level: 4 new measurements a frame (the first one reads the model's files).
void App::palette_entry_gfx(const PalEntry& e, bool force) {
    if (e.gfx_mb >= 0 || e.kind != PalEntry::Template || !e.t || e.theme || e.cut) return;
    float mb = gfx_cell_mb(e.t->uid, false);
    if (mb < 0 && (force || palette_gfx_budget_ > 0)) {
        if (!force) palette_gfx_budget_--;
        mb = gfx_cell_mb(e.t->uid, true);
    }
    if (mb >= 0) e.gfx_mb = mb;
}

// The live total: 8 + Base + every distinct other-level template on the map (each counted once), plus the offset the last exact
// figure (Check, save) left. The template set is collected when the world changed and the mouse is up (not during a drag or a
// stroke); uncached templates are measured at most 4 a frame, so a big pasted map settles over a few frames.
void App::gfx_live_update(bool force) {
    static const uint32_t K_CGEN_ = key_of("MACultureGenerator");
    if (!doc_ || !io_ || !cat_) return;
    if (gfx_cache_base_ != doc_->base) { gfx_reset(); gfx_cache_base_ = doc_->base; }
    if (gfx_scan_ && (force || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        std::set<uint32_t> ids;
        for (auto& o : doc_->world.objs)
            if (uint32_t t = o.template_id()) ids.insert(t);
            else if (o.cls == K_CGEN_) ids.insert(o.u32("GenerationTemplate"));  // a plant added from another level
        ids.erase(0u);
        gfx_tids_.assign(ids.begin(), ids.end());
        gfx_pos_ = 0;
        gfx_sum_ = 0;
        gfx_on_map_.clear();
        gfx_scan_ = false;
        gfx_counting_ = true;
    }
    if (!gfx_counting_) return;
    int heavy = 0;
    while (gfx_pos_ < gfx_tids_.size()) {
        const uint32_t uid = gfx_tids_[gfx_pos_];
        const bool cached = gfx_tpl_mb_.count(uid) != 0;
        const double t0 = ImGui::GetTime();
        const float mb = gfx_cell_mb(uid, true);
        if (!cached && ImGui::GetTime() - t0 > 0.0005) heavy++;
        gfx_sum_ += mb;
        if (mb > 0) gfx_on_map_.insert(uid);
        gfx_pos_++;
        if (!force && heavy >= 4) break;
    }
    gfx_raw_ = 8.0 + io_->base_graphics_mb(*doc_) + gfx_sum_;  // while counting this is partial: the footer shows it with "..."
    if (gfx_pos_ < gfx_tids_.size()) return;
    gfx_counting_ = false;
    if (!gfx_calibrated_) {  // once per document: the exact figure carries what the sum misses (shared files, the sky, roads, start units)
        gfx_offset_ = io_->graphics_estimate(*doc_).total_mb - gfx_raw_;
        gfx_calibrated_ = true;
    }
}

void App::gfx_calibrate(double exact) {
    if (!doc_) return;
    gfx_scan_ = true;
    gfx_calibrated_ = true;  // (keeps the update from computing an estimate of its own)
    gfx_live_update(true);   // the raw sum must describe the world as it is now
    gfx_offset_ = exact - gfx_raw_;
    gfx_calibrated_ = true;
}

void App::gfx_footer_line() {
    if (!doc_) return;
    const double v = gfx_live_total();
    const ImVec4 c = v >= ScenarioIO::kGraphicsErrorMb ? theme::tok().error : v >= ScenarioIO::kGraphicsWarnMb ? theme::tok().warn : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    char b[96];
    std::snprintf(b, sizeof b, "Graphics memory: about %.1f of 64 MB%s###gfxmem", v, gfx_counting_ ? " ..." : "");
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    ImGui::Selectable(b, false, ImGuiSelectableFlags_None, ImVec2(0, 0));
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip("Textures and 3D models this map's theme comes with, plus every object placed from another level (each kind counted once). "
                          "Real levels use about 25 MB; past about 48 the game may not load the map, past 58 it crashes while loading. "
                          "Check scenario lists the biggest.");
}

// ---------------------------------------------------------------------------------------------
// The page: header, body, footer

void App::page_browser(BrowserMode m) {
    Scenario& s = *doc_;
    PalState& ps = pal_state_[int(m)];
    const auto& chips = chips_for(m);
    ps.init = true;
    ps.chip = std::clamp(ps.chip, 0, int(chips.size()) - 1);
    unit_category_ = ps.category;
    unit_filter_ = ps.filter;
    ImGui::BeginDisabled(s.read_only);
    if (m == BrowserMode::Scatter) scatter_header_ui();
    palette_header(m);
    ps.chip = std::clamp(ps.chip, 0, int(chips.size()) - 1);
    unit_group_ = chips[size_t(ps.chip)].group;  // the footer's side row follows the chip
    page_body_begin("##palbody");
    palette_body(m);
    page_body_end();
    page_footer_begin();
    palette_footer(m);
    gfx_footer_line();
    page_footer_end();
    ImGui::EndDisabled();
    ps.filter = unit_filter_;
}

// ---------------------------------------------------------------------------------------------
// Automation: the lean `palette` read

std::string App::automation_palette(int limit) const {
    Json out = Json::object();
    BrowserMode m;
    if (!doc_ || !browser_mode_of_page(m)) return out.with("error", Json::string("the open page has no object browser (Place, Roads & walls or Scatter)")).dump();
    const PalCache& pc = pal_cache_[int(m)];
    out.with("mode", Json::string(mode_name(m))).with("chip", Json::string(pc.chip_label)).with("view", Json::string(settings_.pal_pictures ? "Pictures" : "List"));
    out.with("cols", Json::number(pc.cols)).with("count", Json::number(double(pc.entries.size())));
    Json rows = Json::array();
    int shown = 0;
    for (auto& e : pc.entries) {
        if (shown++ >= std::max(1, limit)) break;
        Json r = Json::object();
        r.with("name", Json::string(e.name));
        if (e.kind == PalEntry::Road) r.with("road", Json::string(road_pieces_[size_t(e.road)].path));
        else r.with("uid", Json::number(e.t->uid));
        r.with("tier", Json::number(e.tier)).with("theme", Json::boolean(e.theme)).with("cut", Json::boolean(e.cut));
        const_cast<App*>(this)->palette_entry_gfx(e, true);
        if (e.gfx_mb >= 0) r.with("gfx_mb", Json::number(double(e.gfx_mb)));
        if (e.kind != PalEntry::Road) r.with("in_scatter", Json::boolean(std::find(scatter_set_.begin(), scatter_set_.end(), e.t->uid) != scatter_set_.end()));
        const_cast<App*>(this)->palette_entry_size(e, true);  // queues the model: the figure shows a few frames after the first read
        if (!e.size.empty()) r.with("size", Json::string(e.size));
        rows.push(std::move(r));
    }
    out.with("entries", rows);
    return out.dump();
}

}  // namespace ed
