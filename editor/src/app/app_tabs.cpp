// Tab panels (Map, Terrain, Players, Units, Messages, Options), the inspector and the New dialog.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "../core/cutscene.h"
#include "../core/mission_extras.h"
#include "../core/triggers.h"
#include "../core/water_custom.h"
#include "../core/weather.h"
#include "ui_tips.h"
#include "app.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "ui_util.h"
#include "palette_defs.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static bool g_first_objective = false;  // set when the first objective is added (Objectives section)

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

static void hint_wrapped(const char* text);

// sticky: the sculpt and paint buttons stay lit when clicked again (Esc or a right-click on the map puts the tool down).
static bool tool_button(const char* label, Tool t, Tool& cur, float w = 0, const char* tip = nullptr, bool sticky = false) {
    bool on = cur == t;
    bool pressed = theme::toggle_button(label, on, ImVec2(w, 0));
    if (tip) ImGui::SetItemTooltip("%s", tip);
    if (pressed) cur = on && !sticky ? Tool::Select : t;
    return pressed;
}

// A paired shaping tool (playtest 10-26): its two halves stacked in one faint box, one highlight that slides
// (~100 ms) to the half in use: the chosen half, or the other one while the reverse key is held (now). Clicking a
// half makes it the pair's tool; clicking the chosen half again keeps it (Esc or a right-click on the map puts the tool down).
static void tool_pair(Tool top, Tool bottom, Tool& cur, Tool now, float& slide, float w, const char* tip_top, const char* tip_bottom) {
    const theme::Tokens& tk = theme::tok();
    const bool active = cur == top || cur == bottom;
    const float hh = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (active) {
        const float target = now == bottom ? 1.f : 0.f, step = ImGui::GetIO().DeltaTime / 0.1f;
        slide = target > slide ? std::min(target, slide + step) : std::max(target, slide - step);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = ImGui::GetStyle().FrameRounding;
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    for (int half = 0; half < 2; half++) {
        const Tool t = half ? bottom : top;
        const ImVec2 a(p0.x, p0.y + hh * half), b(p0.x + w, p0.y + hh * (half + 1));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton(sculpt_name(t), ImVec2(w, hh))) cur = t;
        ImGui::SetItemTooltip("%s", half ? tip_bottom : tip_top);
        if (ImGui::IsItemHovered() && !(active && (now == t))) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ButtonHovered), rr);
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    if (active) {
        const ImVec2 a(p0.x, p0.y + hh * slide), b(p0.x + w, p0.y + hh * (slide + 1));
        dl->AddRectFilled(a, b, ImGui::GetColorU32(tk.select_fill), rr);
        dl->AddRect(a, b, ImGui::GetColorU32(tk.glow_halo1), rr);
    }
    ImVec4 faint = tk.text_hint;
    faint.w *= 0.45f;
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p0.x + w + 1, p0.y + 2 * hh + 1), ImGui::GetColorU32(faint), rr);
    for (int half = 0; half < 2; half++) {
        const Tool t = half ? bottom : top;
        const char* l = sculpt_name(t);
        const ImVec2 ts = ImGui::CalcTextSize(l);
        const bool lit = active && now == t;
        dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + hh * half + (hh - ts.y) * 0.5f), ImGui::GetColorU32(lit ? tk.text_bright : tk.text_item), l);
    }
}

// "BoggedDown" -> "Bogged Down", "Mission_05" -> "Mission 05".
std::string pretty_name(const std::string& n) {
    std::string o;
    for (size_t i = 0; i < n.size(); i++) {
        char c = n[i];
        if (c == '_') { o += ' '; continue; }
        if (i && isupper((unsigned char)c) && islower((unsigned char)n[i - 1])) o += ' ';
        o += c;
    }
    return o;
}

// Also while the item is greyed out: that is when a newcomer most needs to be told why.
// InputText callback (ImGuiInputTextFlags_CallbackEdit): cuts the text being typed to *(int*)user_data characters, so the box
// shows what is stored (clip_chars alone left the box showing the extra characters until it was left).
static int limit_chars_cb(ImGuiInputTextCallbackData* d) {
    const int max = *static_cast<const int*>(d->UserData);
    int n = 0, at = 0;
    while (at < d->BufTextLen) {
        if (n == max) { d->DeleteChars(at, d->BufTextLen - at); break; }
        const unsigned char c = (unsigned char)d->Buf[at];
        at += c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        n++;
    }
    return 0;
}

static void tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

// Water size as one labels-left row: "Size" in the label column, width and length side by side.
static bool water_size_row(const char* idw, const char* idl, float& w, float& l, float col) {
    form_label("Size (W x L)", col);
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::SetNextItemWidth(half);
    bool a = ImGui::DragFloat(idw, &w, 1.f, wcustom::kMinSide, wcustom::kMaxSide, "%.0f m");
    tip("Width (east-west), 8 to 1000 m: small ponds to big lakes.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool b = ImGui::DragFloat(idl, &l, 1.f, wcustom::kMinSide, wcustom::kMaxSide, "%.0f m");
    tip("Length (north-south), 8 to 1000 m.");
    return a || b;
}

// One line per game type for the New dialog and the Map tab: how a round is won and what the map needs.
static const char* game_type_tip(const char* ini) {
    static const std::pair<const char*, const char*> t[] = {
        {"TeamDestruction", "Two teams. Each kill scores for your team; the team with the most kills wins. Needs Red and Blue spawn points."},
        {"CTF", "Capture the Flag: two teams. Take the other team's flag back to your own base. Needs spawn points plus a flag and base for each team."},
        {"BaseWar", "Base War: two teams. Destroy the other team's base while defending yours. Needs spawn points plus a base for each team."},
        {"SnatchIt", "Snatch It: two teams. Grab the item and carry it to your base to score. Needs spawn points plus the item and a base for each team."},
        {"CheckIt", "Check It: two teams. Take and hold the checkpoints to score. Needs spawn points plus the checkpoints."},
        {"TeamLastManStanding", "Two teams with a limited number of lives. The last team with a player left wins. Needs Red and Blue spawn points."},
        {"Destruction", "Everyone for themselves. The player with the most kills wins. Needs free-for-all spawn points."},
        {"LastManStanding", "Everyone for themselves with a limited number of lives. The last player left wins. Needs free-for-all spawn points."},
        {"NotIt", "Everyone for themselves. One player is \"It\" and scores while staying alive; whoever destroys It takes over. Needs free-for-all spawn points."},
        {"Grinder", "One player against waves of enemies; last as long as you can. Needs free-for-all spawn points."},
    };
    for (auto& [k, v] : t) if (std::strcmp(k, ini) == 0) return v;
    return "";
}

std::string plain_logic_kind(const std::string& cls);  // app_triggers.cpp: "After a delay", "Swap a set in" ("" for other classes)

// Plain kind for the Units palette, from the template's engine class.
std::string kind_label(const std::string& cls) {
    static const std::pair<const char*, const char*> k[] = {
        {"MABuilding", "building"}, {"MAMech", "'Mech"}, {"MATank", "vehicle"}, {"MAVehicle", "vehicle"}, {"MAFlyer", "aircraft"},
        {"MATurret", "turret"}, {"MAPowerUp", "power-up"}, {"MAPerson", "person"}, {"MACorporeal", "scenery"}, {"MAWater", "water"},
        {"MAEffect", "effect"}, {"MAFlagCTF", "flag"}, {"MACrateGenerator", "crate"}, {"MALevelStart", "player start"},
        {"MATriggerRadius", "trigger circle"}, {"MATriggerCuboidalVolume", "trigger box"}, {"MAWaypoint", "AI path point"},
        {"MADummyObj", "marker"}, {"MAPointLight", "light"}, {"MAFreeCamera", "camera"}, {"MARoad", "road piece"},
        {"MASnatchItBase", "Snatch It base"}, {"MASnatchItDataCore", "data core"}};
    for (auto& [c, l] : k) if (cls == c) return l;
    std::string logic = plain_logic_kind(cls);  // triggers and actions: the Objectives & Triggers tab's names ("After a delay")
    if (!logic.empty()) return logic;
    static const std::pair<const char*, const char*> more[] = {
        {"MASwapSet", "swap set"}, {"MoeEntitySet", "set of units"}, {"MoeGroupSet", "sets folder"}, {"MACinema", "cutscene"},
        {"MAFreeCameraController", "camera controller"}, {"MACameraProperty", "camera step"}, {"MAPath", "camera path"}};
    for (auto& [c, l] : more) if (cls == c) return l;
    // Anything else: "MAFooBar" -> "foo bar".
    std::string s = cls.size() > 2 && cls[0] == 'M' && cls[1] == 'A' ? cls.substr(2) : cls.rfind("Moe", 0) == 0 ? cls.substr(3) : cls, out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && isupper((unsigned char)s[i]) && !isupper((unsigned char)s[i - 1])) out += ' ';
        out += char(tolower((unsigned char)s[i]));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Map

// A multiplayer map with no spawn points cannot be played: say so on the tabs a newcomer opens first.
static void no_spawns_hint(const Scenario& s, bool on_players_tab) {
    if (!s.multiplayer) return;
    for (auto& o : s.world.objs)
        if (o.cls == K_LEVELSTART) return;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(on_players_tab ? "This map has no spawn points yet, so nobody can start on it. Press \"Add 6 on a ring around the camera\" below for each team."
                                          : "This map has no spawn points yet, so nobody can start on it. Add them with the Starts tool.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void App::tab_map() {
    Scenario& s = *doc_;
    no_spawns_hint(s, false);
    ImGui::SeparatorText("Scenario");
    ImGui::BeginDisabled(s.read_only);
    form_label("Name", form_col());
    if (ImGui::InputText("##name", &s.name, ImGuiInputTextFlags_CallbackEdit, limit_chars_cb, const_cast<int*>(&kNameMax))) { clip_chars(s.name, kNameMax); push_undo_once("name"); }
    tip("The name the game shows in its map or mission list (up to 60 characters).");
    form_label("Author", form_col());
    if (ImGui::InputTextWithHint("##author", "Your name (optional)", &s.author)) push_undo_once("author");
    tip("Your name (shown with the description).");
    ImGui::TextUnformatted("Description");
    if (ImGui::InputTextMultiline("##desc", &s.description, ImVec2(-1, (ImGui::GetMainViewport()->Size.y / dpi_ < 800.f ? 44.f : 60.f) * dpi_), ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackEdit, limit_chars_cb, const_cast<int*>(&kDescMax))) { clip_chars(s.description, kDescMax); push_undo_once("description"); }
    tip(s.multiplayer ? "One or two lines the game shows under the name when the map is picked (up to 250 characters)."
                      : "One or two lines the game shows under the name when the mission is picked (up to 250 characters). Left empty, the mission's briefing is shown there instead.");
    {
        std::string d = s.description;
        ImGui::TextDisabled("%d / %d", clip_chars(d, 100000), kDescMax);
    }
    ImGui::EndDisabled();
    ImGui::Text("Kind: %s", s.multiplayer ? "Multiplayer map (play it against bots)" : "Mission (enemies use the campaign AI)");
    std::string base_name = s.base;
    for (auto& m : gd_.maps()) if (iequals(m.resource, s.base)) { base_name = pretty_name(m.name); break; }
    ImGui::Text("Built on: %s", base_name.c_str());
    ImGui::SetItemTooltip("The real level whose look (ground textures, lighting, buildings) this map uses.");
    ImGui::Text("Size: %.0f x %.0f m", s.terrain.size_x(), s.terrain.size_z());
    if (!s.file.empty()) ImGui::TextWrapped("File: %s", path_utf8(s.file).c_str());
    else ImGui::TextDisabled("Not saved yet.");
    // What the player starts in (MARoot's player configuration container).
    {
        uint32_t cur = io_->start_container(s);
        const auto& choices = io_->start_choices();
        std::string shown = "Kept from the real level";
        for (auto& c : choices) if (c.container == cur) shown = c.label;
        ImGui::BeginDisabled(s.read_only);
        form_label(s.multiplayer ? "Players start as" : "Player starts in", form_col());
        if (begin_combo("##startas", shown.c_str())) {
            int last_kind = -1;
            for (auto& c : choices) {
                if (s.multiplayer && c.kind > 1) continue;  // MP maps: on foot or Battle Armor (retail GoCommando, OnTheRocks)
                if (c.kind != last_kind && c.kind >= 2) ImGui::SeparatorText(c.kind == 2 ? "'Mechs" : "Vehicles");
                last_kind = c.kind;
                if (ImGui::Selectable(c.label.c_str(), c.container == cur)) {
                    push_undo("player start unit");
                    io_->set_start_container(s, c.container);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        tip(s.multiplayer ? "What every player starts as. Real maps start on foot, except Go Commando and On The Rocks (Battle Armor). On foot, Not It, Last Man Standing, Destruction and Grinder still start in Battle Armor, as on the real maps."
                          : "What the player starts in: on foot, in Battle Armor (like Mission 2), or already inside a 'Mech or tank (like Missions 5 and 13). Saving adds the model files that body needs."
                          "\n'Kept from the real level' = the level starts the player in something not in this list; it is left as it is.");
    }
    if (s.multiplayer) {
        ImGui::SeparatorText("Game types");
        ImGui::BeginDisabled(s.read_only);
        // The three most played first with one line each, the rest under More (as in the New dialog).
        static const std::pair<const char*, const char*> common[] = {
            {"TeamDestruction", "two teams, most kills wins"}, {"CTF", "take the other team's flag home"}, {"Destruction", "everyone for themselves"}};
        auto common_line = [](const char* ini) -> const char* {
            for (auto& [n, l] : common) if (iequals(n, ini)) return l;
            return nullptr;
        };
        auto ticked = [&](const GameTypeDef& g) {
            return std::find_if(s.types.begin(), s.types.end(), [&](const std::string& x) { return iequals(x, g.ini); }) != s.types.end();
        };
        auto box = [&](const GameTypeDef& g) {
            bool on = ticked(g);
            if (ImGui::Checkbox(g.label, &on)) {
                push_undo("game types");
                if (on) s.types.push_back(g.ini);
                else s.types.erase(std::remove_if(s.types.begin(), s.types.end(), [&](const std::string& x) { return iequals(x, g.ini); }), s.types.end());
                io_->sync_spawn_groups(s);
                s.dirty = true;
                // A type ticked later gets its spawn points and its objects at once, as on a new map (MP dogfood 10-26).
                if (on) {
                    int spawns = add_missing_spawns();
                    bool kit = add_type_kit(g.ini);
                    if (kit) io_->sync_spawn_groups(s);
                    if (spawns > 0 || kit) {
                        world_changed();
                        status_ = std::string("Added ") + (spawns > 0 && kit ? "spawn points and the objects" : spawns > 0 ? "spawn points" : "the objects") + " for " + g.label +
                                  (kit ? " (copied from a real level; drag them where you want them)." : " (the coloured markers).");
                        status_time_ = 8;
                    }
                }
            }
            tip(game_type_tip(g.ini));
            if (const char* l = common_line(g.ini)) {
                ImGui::SameLine();
                ImGui::TextDisabled("- %s", l);
            }
        };
        int more_on = 0;
        for (auto& g : game_types()) {
            if (common_line(g.ini)) box(g);
            else if (ticked(g)) more_on++;
        }
        if (more_on) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
        std::string more = "More game types" + (more_on ? " (" + std::to_string(more_on) + " ticked)" : std::string()) + "###map_more_types";
        if (ImGui::TreeNode(more.c_str())) {
            for (auto& g : game_types()) if (!common_line(g.ini)) box(g);
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
        help("Tick the game types this map supports. Each one needs its own objects: spawn points for every type, plus flags for Capture the Flag, bases for Snatch It and so on (Starts tool). Saved maps appear in the game under Multiplayer > Local Xbox Console > New Game.");
    }
    // Sky, time of day, weather, fog and sun live on their own Environment tab (E2/M10).
    ImGui::SeparatorText("Environment");
    if (ImGui::Button("Open the Environment tab", ImVec2(-1, 0))) tab_ = Tab::Environment;
    tip("Sky, time of day, light, fog, sun and weather are set there.");
    ImGui::SeparatorText("Check");
    if (ImGui::Button("Check scenario", ImVec2(-1, 0))) { validation_ = check_now(); show_validation_ = true; }
    tip("Lists anything that would stop the map from working in the game, and how to fix it. Test runs this check first.");
    if (editable()) {
        if (ImGui::Button("Put flat pieces in order", ImVec2(-1, 0))) reseat_command();
        tip("Stacks the selected road pieces and bases (or all of them) by the order they were placed in, so none lie at the same height. Tilted and glued pieces are left alone.");
    }
    if (last_report_.file_bytes) {   // The file holds more than "N placed" in the status bar counts
        ImGui::TextDisabled("Last saved: %zu objects in all, %.1f MB file", last_report_.objects, last_report_.file_bytes / 1048576.0);
        tip("Every object in the saved file. The status bar's \"placed\" counts only the things you put down yourself (no ground pieces or helpers), so it is a smaller number.");
    }
    gfx_footer_line();  // the live figure: it follows every object placed from another level, and equals the exact one after a save
}

// ---------------------------------------------------------------------------------------------
// Terrain

// Sky (MASky2): the Base level's own, or any sky model on the disc. A sky from another level
// adds its model's files to [Assets] when the scenario is saved.
// "Env_SkyAurora_01" -> "Aurora 01", "Env_Sky_CityNight" -> "City Night": the art name, shown only in the tooltip.
static std::string sky_art_name(const std::string& name) {
    std::string n = name;
    for (const char* pre : {"Env_Sky_", "Env_Sky", "En_Sky_", "Env_"})
        if (istarts_with(n, pre)) { n = n.substr(strlen(pre)); break; }
    return pretty_name(n);
}

// What each sky looks like, in the words of the time-of-day looks (core/weather.cpp) where a look uses
// it. Keyed by the art name, so "Env_Sky_NOR_01" and "Env_SkyNOR_01" both match.
const char* sky_plain_name(const std::string& name) {
    static const std::pair<const char*, const char*> t[] = {
        {"Warm Sun", "Clear blue day"},
        {"NOR 02", "White cloudy sky"},
        {"Dusk02", "Hazy golden dusk"},
        {"NOR 01", "Dark blue storm clouds"},
        {"Hesperus 01", "Red planet sky"},
        {"Hesperus 02", "Red planet sky, second"},
        {"City Night", "City night, orange glow"},
        {"Aurora 01", "Night with green aurora"},
        {"Stormy", "Black thunderstorm clouds"},
        {"Stormfront 01", "Storm front"},
        {"Storm Front 01", "Storm front"},  // the art name as sky_art_name spells it
        {"Asteroid 01", "Space with asteroids"},
        {"Space", "Starry space"},
        {"Space02", "Starry space, second"},
        {"Planet", "Planet seen from space"},
    };
    std::string art = sky_art_name(name);
    for (auto& [k, v] : t) if (iequals(art, k)) return v;
    return nullptr;
}

std::string sky_words(const std::string& name) {
    const char* p = sky_plain_name(name);
    return p ? std::string(p) : sky_art_name(name);
}

// "Used on: Bogged Down, Mission 05 and 3 cutscenes" from the worlds that use a sky.
std::string used_on_text(const std::vector<ScenarioIO::SkyUse>& uses) {
    std::set<std::string> seen;
    std::string levels;
    int cutscenes = 0;
    for (auto& u : uses) {
        std::string w = u.world.substr(u.world.find('/') + 1);
        w.erase(w.find_last_of('.'));
        if (!seen.insert(w).second) continue;
        if (u.kind == 2) { cutscenes++; continue; }
        levels += (levels.empty() ? "" : ", ") + pretty_name(w);
    }
    if (levels.empty() && !cutscenes) return "Not used by any real level.";
    std::string o = "Used on: " + (levels.empty() ? std::string("no playable level") : levels);
    if (cutscenes) o += " and " + std::to_string(cutscenes) + (cutscenes == 1 ? " cutscene" : " cutscenes");
    return o + ".";
}

void App::sky_picker(Scenario& s) {
    if (find_class(s.world, K_SKY) < 0) return;
    const auto& skies = io_->skies();
    std::string cur = io_->sky_name(s), base = io_->base_sky_name(s);
    auto label = [&](const ScenarioIO::SkyInfo& si) {
        const char* p = sky_plain_name(si.name);
        return p ? std::string(p) : sky_art_name(si.name);
    };
    std::string preview = "(unknown)";
    for (auto& si : skies) if (si.name == cur) preview = label(si) + (si.name == base ? "  (this map's own)" : "");
    std::string err;
    auto apply = [&](const std::string& name) {
        const double g0 = env_gfx_before();
        push_undo("sky");
        std::string msg;
        if (io_->set_sky(s, name, sky_use_fog_, &err)) {
            // The look keeps its name (its light did not change), only its sky differs.
            const weather::Look* l = weather::current_look_light(*io_, s);
            msg = "Sky: " + sky_words(name) + ". " + (l ? std::string("The ") + l->label + " light is kept." : std::string("This map's own light is kept.")) +
                  " Undo to go back." + env_gfx_note(g0);
        } else {
            msg = "Sky: " + err;
        }
        env_status("%s", msg.c_str());
        objects_dirty_ = true;
    };
    // Retail-reach #41: the current sky's picture, and a picture on every row of the list.
    if (ID3D11ShaderResourceView* th = sky_thumb(cur)) ImGui::Image((ImTextureID)(intptr_t)th, ImVec2(128 * dpi_, 64 * dpi_));
    form_label("Sky picture", form_col());
    combo_opens_down("##sky", 380 * dpi_, 40 * dpi_);  // wide enough for the long sky names; the list opens downward
    if (ImGui::BeginCombo("##sky", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (auto& si : skies) {
            std::string l = label(si) + (si.name == base ? "  (this map's own)" : "");
            if (thumb_row(sky_thumb(si.name), l, si.name, si.name == cur)) apply(si.name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nSky picture: %s", used_on_text(si.uses).c_str(), sky_art_name(si.name).c_str());
        }
        ImGui::EndCombo();
    }
    tip("The sky picture around the map. Hover an entry to see which real levels use it. To change the whole look (sky, light and fog together), use Look at the top of this tab.");
    if (ImGui::Checkbox("Use this sky's fog colour", &sky_use_fog_)) {
        // On: the fog colour of the level the sky comes from. Off: the Base level's own.
        uint32_t c;
        int ei = find_class(s.world, K_ENV);
        if (ei >= 0 && io_->sky_fog(s, sky_use_fog_ ? cur : base, c)) {
            push_undo("fog colour");
            s.world.objs[size_t(ei)].set_u32("FogColor", c);
            s.dirty = true;
            std::string from;
            if (sky_use_fog_)
                if (const ScenarioIO::SkyInfo* si = io_->find_sky(cur))
                    if (const ScenarioIO::SkyUse* u = io_->sky_source(s, *si)) {
                        std::string w = u->world.substr(u->world.find('/') + 1);
                        w.erase(w.find_last_of('.'));
                        from = " (" + pretty_name(w) + ")";
                    }
            if (sky_use_fog_) env_status("Fog colour now matches the sky%s. Untick to go back to this map's own.", from.c_str());
            else env_status("Fog colour is this map's own again.");
        } else {
            env_status("Fog colour: this sky has no fog colour of its own to copy.");
        }
    }
    help("The fog option copies the fog colour of the level the sky comes from, so the horizon matches; untick it to go back to this map's own fog colour. A sky from another level works too: the game loads it from your own copy of the game.");
}

std::string water_plain_name(const TemplateInfo& t);  // below

// Terrain tab > Water: the waters in this map (click selects; Properties edits them) and the Place water tool.
void App::water_list_ui() {
    Scenario& s = *doc_;
    int idx = 0;
    for (auto& o : s.world.objs) {
        if (o.cls != K_WATER) continue;
        ImGui::PushID(idx);
        Vec3 p;
        get_pos(o, p);
        bool picked = selection_.count(o.uid()) != 0;
        const TemplateInfo* wti = cat_->find(o.template_id());
        std::string wname = ScenarioIO::water_custom_name(o, wti ? wti->name : std::string());
        std::string label = !wname.empty() ? wname : "Water " + std::to_string(idx + 1);
        if (water_rename_uid_ == o.uid()) {
            // Double-click a name to rename it; Enter keeps it, Esc cancels, an empty name goes back to "Water N".
            static bool focus = true;
            if (focus) { ImGui::SetKeyboardFocusHere(); focus = false; }
            ImGui::SetNextItemWidth(-1);
            bool enter = ImGui::InputText("##rename", &water_rename_buf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (enter || esc || (ImGui::IsItemDeactivated() && !ImGui::IsItemActive())) {
                if (!esc) {
                    std::string nn = trim(water_rename_buf_);
                    if (nn.empty() && wti) nn = wti->name;
                    if (nn != wname && !(wname.empty() && nn == label)) {
                        push_undo("rename water");
                        float cw, cl;
                        o.set_str("Description", ScenarioIO::water_custom_size(o, cw, cl) ? ScenarioIO::water_description(nn, cw, cl, ScenarioIO::water_custom_shape(o)) : nn);
                        s.dirty = true;
                        objects_dirty_ = true;
                    }
                }
                water_rename_uid_ = 0;
                focus = true;
            }
        } else {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Selectable(label.c_str(), picked, ImGuiSelectableFlags_AllowDoubleClick)) {
                selection_ = {o.uid()};
                cam_.tx = p.x;
                cam_.tz = p.z;
            }
            // A Selectable reports the click on release, when IsMouseDoubleClicked is already false: test the double-click on its own.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { water_rename_uid_ = o.uid(); water_rename_buf_ = label; }
            tip("Click to select this water on the map and look at it (drag the red or blue arrow on the map to move it, the green one for its level; right-click on empty ground deselects). Double-click to rename it.");
        }
        float y = p.y;
        form_label("Water level", form_col());
        if (ImGui::DragFloat("##wlevel", &y, 0.1f, s.terrain.min_h - 50, s.terrain.max_h, "%.1f m")) {
            push_undo_once("water level");
            p.y = y;
            set_pos(o, p);
            s.dirty = true;
            objects_dirty_ = true;
        }
        tip("Height of the water surface. Ground higher than this hides the water.");
        {
            float cw, cl;
            water_size_of(o, nullptr, cw, cl);
            const TemplateInfo* lt = cat_->find(o.template_id());
            ImGui::Indent(form_col());
            ImGui::TextDisabled("%s, %.0f x %.0f m%s", lt ? water_type_name(*lt).c_str() : "?", cw, cl,
                                ScenarioIO::water_custom_shape(o) == wcustom::Shape::Oval ? ", oval" : "");
            tip("Select it (click its name) to change its type, size and shape in Properties on the right.");
            ImGui::Unindent(form_col());
        }
        ImGui::PopID();
        idx++;
    }
    if (!idx) ImGui::TextDisabled(s.read_only ? "This level has no water." : "No water in this map yet.");
}

void App::water_new_ui() {
    Scenario& s = *doc_;
    if (s.read_only) return;  // a real level: its water list only, no placing
    ImGui::TextUnformatted("New water");
    {
        // The type the next click places: a fixed-size water picked here or in the palette, else the resizable one.
        const TemplateInfo* pt = tool_ == Tool::PlaceObject && place_template_ ? cat_->find(place_template_) : nullptr;
        bool fixed = pt && pt->cls_name == "MAWater";
        uint32_t pick = water_type_combo("##newtype", fixed ? place_template_ : water_place_tpl(), 40.f);
        if (pick) { arm_water(pick); fixed = !water_can_resize(pick); }
        tip("The kind of water to place (colour, waves, reflection). Picking one arms Place water; the same list is in the palette's Water group.");
        ImGui::BeginDisabled(fixed);
        water_size_row("##nw", "##nl", water_new_w_, water_new_l_, form_col());
        ImGui::Checkbox("Oval##newoval", &water_new_oval_);
        tip("Place the water cropped to an ellipse inside the Width x Length box (a round lake or pond).");
        ImGui::EndDisabled();
        if (fixed) hint_wrapped("This water has a fixed size and shape: it is placed as it was made. Click the map to place it.");
        ImGui::Checkbox("Dig a basin##newdig", &water_new_dig_);
        tip(("Digs the ground under the new water so it has depth (on flat ground, shallow water is almost invisible). " + kt(A_UNDO) + " undoes the water and the digging together.").c_str());
        if (water_new_dig_) {
            ImGui::SameLine();
            ImGui::Checkbox("Deep lake (pilots go under)##newdigdeep", &water_new_deep_);
            tip("Off: the basin is about 1.2 m deep, like the game's own wading water: a pilot on foot wades through. "
                "On: up to 4 m deep (like the big multiplayer lakes); ground deeper than 1.6 m is marked deep water.");
        }
        if (water_new_dig_ && water_new_deep_) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
            hint_wrapped("Deep lake: a pilot on foot ends up below the surface. Pilots can still walk through it underwater.");
            ImGui::PopStyleColor();
        }
        water_new_w_ = std::clamp(std::round(water_new_w_), wcustom::kMinSide, wcustom::kMaxSide);
        water_new_l_ = std::clamp(std::round(water_new_l_), wcustom::kMinSide, wcustom::kMaxSide);
    }
    // Picking a type already arms Place water, so this button only arms it (a toggle switched it off on the
    // user's next click); Esc or right-click stops placing.
    const bool placing = tool_ == Tool::PlaceWater;
    if (tool_button("Place water on the map", Tool::PlaceWater, tool_, -1,
                    ("Click the map to put the water there: a preview of the water shows where it goes and how big it is. Click again for more, right-click or " + kt(A_CANCEL) + " to stop.").c_str()) &&
        placing) {
        tool_ = Tool::PlaceWater;
        status_ = "Placing water: click the map. Right-click or " + kt(A_CANCEL) + " stops.";
        status_time_ = 5;
    }
    if (tool_ == Tool::PlaceWater && water_new_dig_ && ground_preview_frame_ >= ImGui::GetFrameCount() - 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.86f, 0.3f, 1.f));
        ImGui::TextWrapped("%s", ground_preview_text_.c_str());  // the basin the click will dig (drawn on the map)
        ImGui::PopStyleColor();
    }
    hint_wrapped(water_new_dig_ ? "The surface is a little under the ground you click, with a basin dug under it. Use Level or the green arrow to change the height." : "The surface starts 1 m above the ground you click; use Level or the green arrow to set it, and sculpt a basin so the ground around it is higher than the surface.");
}

void App::water_section_ui() {
    water_list_ui();
    ImGui::Spacing();
    water_new_ui();
}


void App::page_sculpt() {
    Scenario& s = *doc_;
    if (s.read_only) {
        // A real level: the recipe of its ground instead of tools that cannot be used here.
        terrain_read_only_ui();
        return;
    }
    if (sculpt_page_tool(tool_)) sculpt_last_ = tool_;
    const Tool cur = sculpt_page_tool(tool_) ? tool_ : sculpt_last_;  // after Esc the options stay in view for the last tool
    // Pairs stacked in faint boxes, the reverse key flips to the other half while held; Flatten on its own (playtest).
    {
        const Tool now = sculpt_now();
        const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
        const std::string rk = mod_name(settings_.hold_reverse);
        const std::string t_raise = "Drag on the map to build hills. Hold " + rk + " to lower instead.";
        const std::string t_lower = "Drag on the map to dig valleys. Hold " + rk + " to raise instead.";
        const std::string t_pup = "Drag to build flat-topped ground at the Plateau height (below). Hold " + rk + " to cut down instead.";
        const std::string t_pdown = "Drag to cut the ground above the Plateau height (below) down to it: terraces, pits, riverbeds. Hold " + rk + " to build up instead.";
        const std::string t_smooth = "Drag to soften bumps and steep edges. Hold " + rk + " to roughen instead.";
        const std::string t_rough = "Drag to add small random bumps. Hold " + rk + " to smooth instead.";
        tool_pair(Tool::Raise, Tool::Lower, tool_, now, sculpt_slide_[0], w, t_raise.c_str(), t_lower.c_str()); ImGui::SameLine();
        tool_pair(Tool::Plateau, Tool::PlateauDown, tool_, now, sculpt_slide_[1], w, t_pup.c_str(), t_pdown.c_str()); ImGui::SameLine();
        tool_pair(Tool::Smooth, Tool::Roughen, tool_, now, sculpt_slide_[2], w, t_smooth.c_str(), t_rough.c_str());
        tip(("While you hold " + rk + ", each pair uses its other half (the brush ring turns orange for down / rough, cyan for up / smooth). Change the key in Settings > Keys > Hold keys.").c_str());
        const std::string t_flat = "Drag to level the ground to the height set below. Alt+click (or " + rk + "+click) picks the height of the ground there without changing anything.";
        tool_button("Flatten", Tool::Flatten, tool_, w, t_flat.c_str(), true);
        ImGui::SameLine();
        tool_button("Ramp", Tool::Ramp, tool_, w, "Click where a ramp starts and where it ends: the ground between is laid as a straight slope at the width set below, level across, with a soft shoulder. The label and the strip show who can get up it: green everyone, yellow bots' 'Mechs won't walk up it (jump-jet 'Mechs may jet up a short face), orange very steep (bots won't route here; some 'Mechs may still climb it).", true);
        ImGui::AlignTextToFramePadding();
        if (cur == Tool::Ramp) ImGui::TextDisabled("Two clicks: start, then end");
        else ImGui::TextDisabled("%s+click: pick the height (Flatten)", mod_name(settings_.hold_through));
        tip(t_flat.c_str());
        // Always in sight with the shaping tools, not among the chosen tool's options further down.
        // Playtest 10-26: one switch, no per-object rules. On: what stands on the ground (buildings, walls, roads, bridges) moves
        // with it, a snapped bridge as one piece; off: nothing moves, so the ground can be shaped around a bridge.
        if (ImGui::Checkbox("Objects follow the terrain", &settings_.objects_follow_terrain)) settings_.save();
        tip("Brushes, Fit ground, Level and water basins carry what stands on the ground (buildings, units, walls, roads, bridges; a joined bridge moves as one).\n"
            "Off: only the ground moves.");
        // Steep ground tinted on the map (it comes on by itself while Ramp is armed).
        if (bool st = show_steep_; ImGui::Checkbox("Show steep ground", &st)) { settings_.show_steep = st; show_steep_ = st; steep_auto_ = false; settings_.save(); overlay_dirty_ = true; }
        {
            // Tiers: who can get up it. Limits from the bot code (see core/brush.h); nothing here says what the player can climb.
            char sg[600];
            snprintf(sg, sizeof sg, "Tints steep ground by who can get up it. Yellow, over %.0f%%: bots' 'Mechs won't walk up it (jump-jet 'Mechs may jet up a short face) (bot 'Mechs and tanks stop at %.0f%%, Battle Armor and pilots at %.0f%%; hover: not measured). "
                                    "Orange, over %.0f%% (about %.0f degrees): very steep, bots won't route here; some 'Mechs may still climb it. "
                                    "It comes on by itself while the Ramp tool is armed. The Ramp tool cuts a way up.",
                     double(kSteepBot) * 100.0, double(kSteepBot) * 100.0, double(kSteepLight) * 100.0, double(kSteepLight) * 100.0, std::atan(double(kSteepLight)) * 57.2958);
            tip(sg);
        }
        if (steep_auto_) ImGui::TextDisabled("On while the Ramp tool is armed.");   // Not a setting the user changed
        if (show_steep_) hint_wrapped("Yellow: bots' 'Mechs won't walk up it (jump-jet 'Mechs may jet up a short face). Orange: very steep, bots won't route here; some 'Mechs may still climb it.");
        // The hill only shows when the view is tilted.
        hint_wrapped(("To see the shape of the ground, tilt the view: middle-drag or " + std::string(mod_name(settings_.hold_orbit)) + "+drag. " + kt(A_TOP_DOWN) + " goes back to top-down.").c_str());
        // "Can it get here?": which ground a unit gets to from the team's start points (app_reach.cpp); next to the steep switch when it fits.
        if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize("Can it get here?").x + 70.f * dpi_) ImGui::SameLine();
        if (bool rc = settings_.reach_show; ImGui::Checkbox("Can it get here?##reachtab", &rc)) { settings_.reach_show = rc; settings_.save(); overlay_dirty_ = true; }
        tip("Pick a unit and a team: the ground turns green where it gets to by walking or driving, amber where only jets, claws or a VTOL ride get it there, and red where it never gets. Arrows on cliffs show which way up or down works. Hover the ground for the details.");
    }
    if (cur == Tool::Ramp) {
        // Ramp: a strip from A to B; its heights come from the ground at both ends unless typed.
        const float lo = 2.f * s.terrain.scale;
        form_label("Ramp width", form_col());
        ImGui::SetNextItemWidth(-28 * dpi_);
        if (theme::slider_float("##rampw", &settings_.ramp_width, lo, 100.f, "%.0f m")) settings_.ramp_width = std::clamp(settings_.ramp_width, lo, 100.f);
        if (ImGui::IsItemDeactivatedAfterEdit()) settings_.save();
        help(("How wide the flat top of the ramp is, across. Keys: " + kt(A_BRUSH_SMALLER) + " narrower, " + kt(A_BRUSH_BIGGER) + " wider. The smallest is two ground cells (" + std::to_string(int(lo)) + " m); a soft shoulder of one and a half cells blends it into the old ground each side and past each end. Ctrl+click the slider to type a width.").c_str());
        ImGui::Checkbox("Typed heights##ramp", &ramp_typed_);
        tip("Off: the ramp runs between the drawn ground at the two clicks. On: it runs between the heights typed here, wherever you click.");
        if (ramp_typed_) {
            form_label("From", form_col());
            ImGui::SetNextItemWidth(-28 * dpi_);
            ImGui::DragFloat("##rampfrom", &ramp_h_from_, 0.5f, s.terrain.min_h, s.terrain.max_h, "%.2f m");
            form_label("To", form_col());
            ImGui::SetNextItemWidth(-28 * dpi_);
            ImGui::DragFloat("##rampto", &ramp_h_to_, 0.5f, s.terrain.min_h, s.terrain.max_h, "%.2f m");
        }
        ImGui::TextDisabled("Click where the ramp starts, then where it ends.");
        hint_wrapped("Green label: everyone can climb it. Yellow: bots' 'Mechs won't walk up it (jump-jet 'Mechs may jet up a short face). Orange: very steep, bots won't route here; some 'Mechs may still climb it. A yellow or orange edge along the sides is the cut where the ramp meets the old ground.");
    } else {
        form_label("Brush size", form_col());
        brush_size_slider(cur);
        tip(("How wide the brush is (the yellow circle on the map). All the tools on this page share it, and Scatter uses the same size. Ctrl+click the slider to type a size. Keys: " + kt(A_BRUSH_SMALLER) + " smaller, " + kt(A_BRUSH_BIGGER) + " bigger, or Ctrl+wheel over the map. " + brush_floor_note(cur)).c_str());
        {
            float lo, hi;
            brush_limits(cur, lo, hi);
            ImGui::TextDisabled("Smallest here %.0f m (a height point every %g m)", lo, double(s.terrain.scale));
        }
        if (cur == Tool::Raise || cur == Tool::Lower || cur == Tool::Roughen) {
            form_label(settings_.build_up ? "Height per second" : "Height per stroke", form_col());
            ImGui::SetNextItemWidth(-28 * dpi_);
            const float top = std::max(20.f, std::min(s.terrain.max_h - s.terrain.min_h, 1000.f));
            if (theme::slider_float("##amount", &settings_.raise_amount, 0.5f, 20.f, "%.1f m")) settings_.raise_amount = std::clamp(settings_.raise_amount, 0.1f, top);
            if (ImGui::IsItemDeactivatedAfterEdit()) settings_.save();
            help(("How far one stroke moves the ground, at the centre of the brush. Hold still or drag back and forth and it stays the same: let go and stroke again to add more. Keys: " + kt(A_BRUSH_WEAKER) + " less, " + kt(A_BRUSH_STRONGER) + " more (one ground step of " + [&] { char b[16]; snprintf(b, sizeof b, "%.2f m", s.terrain.step()); return std::string(b); }() + "), or Shift+wheel over the map. Ctrl+click the slider to type a height.").c_str());
            if (ImGui::Checkbox("Keep raising while held", &settings_.build_up)) settings_.save();
            tip("Off: one stroke adds the height above, however long you hold. On: the stroke keeps adding the same amount again every second it is held (it never depends on frame rate or drag speed).");
        } else if (cur == Tool::Smooth || cur == Tool::Flatten) {
            float& st = strength_ref(cur);
            float pct = st * 100.f;
            form_label("Speed", form_col());
            if (theme::slider_float("##speed", &pct, 2.f, 100.f, "%.0f%%")) { st = std::clamp(pct / 100.f, 0.02f, 1.f); strength_dirty_ = true; }
            if (ImGui::IsItemDeactivatedAfterEdit() && strength_dirty_) { settings_.tool_strength[strength_slot(cur)] = st; settings_.save(); strength_dirty_ = false; }
            tip(("How fast the brush works while you hold the mouse button: low for gentle changes, 100% for quick ones. Keys: " + kt(A_BRUSH_WEAKER) + " weaker, " + kt(A_BRUSH_STRONGER) + " stronger, or Shift+wheel over the map.").c_str());
        }
        {
            const char* edge = brush_hardness_ < 0.25f ? "Soft" : brush_hardness_ < 0.6f ? "Medium" : "Hard";
            form_label("Edge", form_col());
            theme::slider_float("##edge", &brush_hardness_, 0.f, 0.95f, edge);
            tip("Soft fades the brush out toward its rim; Hard works evenly right up to the yellow circle.");
        }
        const bool sculpt = true;
        (void)sculpt;
        if (sculpt) {
            // Square: straight edges for square pools, pads and terraces.
            form_label("Shape", form_col());
            if (ImGui::RadioButton("Round", !brush_square_)) brush_square_ = false;
            tip("A round brush: soft hills and hollows.");
            ImGui::SameLine();
            if (ImGui::RadioButton("Square", brush_square_)) brush_square_ = true;
            tip("A square brush lined up with the map: straight edges and corners for square pools, pads and terraces.");
        }
        if (cur == Tool::Flatten) { form_label("Flatten to", form_col()); ImGui::SetNextItemWidth(-28 * dpi_); ImGui::DragFloat("##flatten", &flatten_height_, 0.5f, s.terrain.min_h, s.terrain.max_h, "%.2f m"); if (ImGui::IsItemDeactivatedAfterEdit()) flatten_height_ = s.terrain.stored(flatten_height_); char sm[96]; snprintf(sm, sizeof sm, " The ground moves in steps of %.2f m on this map.", s.terrain.step()); help(("Dragging levels the ground to this height. Alt+click (or " + std::string(mod_name(settings_.hold_reverse)) + "+click) picks the height of the ground there without changing anything." + sm).c_str()); ImGui::TextDisabled("%s+click picks the height", mod_name(settings_.hold_through)); }
        if (cur == Tool::Plateau || cur == Tool::PlateauDown) { form_label("Plateau height", form_col()); ImGui::DragFloat("##plateau", &plateau_height_, 0.5f, s.terrain.min_h, s.terrain.max_h, "%.2f m"); if (ImGui::IsItemDeactivatedAfterEdit()) plateau_height_ = s.terrain.stored(plateau_height_); char sm[96]; snprintf(sm, sizeof sm, "The ground moves in steps of %.2f m on this map.", s.terrain.step()); tip(sm); }
    }
    ImGui::BeginDisabled(s.read_only);
    page_body_begin("##sculptbody");
    if (theme::section("Height range (advanced)")) {
        ImGui::SeparatorText("Height range");
        float mn = s.terrain.min_h, mx = s.terrain.max_h;
        form_label("Lowest / highest", form_col());
        ImGui::SetNextItemWidth(-28 * dpi_);  // room for the (?) after it
        ImGui::DragFloatRange2("##hrange", &mn, &mx, 1, -500, 1000, "%.1f m");
        if ((mn != s.terrain.min_h || mx != s.terrain.max_h) && mx > mn + 10) {
            push_undo_once("height range");
            if (!range_active_) {  // what stands on the ground now, before the steps move (the whole drag works from it)
                range_active_ = true;
                range_standing_ = settings_.objects_follow_terrain ? ground_standing() : std::vector<Standing>();
                range_steps0_.resize(s.terrain.hgt.size());
                for (size_t i = 0; i < range_steps0_.size(); i++) range_steps0_[i] = s.terrain.stored(s.terrain.hgt[i]);
            }
            s.terrain.min_h = mn;
            s.terrain.max_h = mx;
            for (auto& h : s.terrain.hgt) h = std::clamp(h, mn, mx);
            r_.set_terrain(s.terrain);
            s.dirty = true;
            if (show_steep_) overlay_dirty_ = true;
            if (settings_.objects_follow_terrain) {
                const int moved = ground_changed(range_standing_);
                float most = 0;
                for (size_t i = 0; i < range_steps0_.size() && i < s.terrain.hgt.size(); i++) most = std::max(most, std::fabs(s.terrain.stored(s.terrain.hgt[i]) - range_steps0_[i]));
                char rm[260];
                snprintf(rm, sizeof rm, "The ground now moves in steps of %.2f m and shifted by up to %.2f m. %d object%s moved with it.", s.terrain.step(), most, moved, moved == 1 ? "" : "s");
                status_ = rm;
                status_time_ = 6;
            }
        }
        if (range_active_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsItemActive()) { range_active_ = false; range_standing_.clear(); }
        {
            char hh[420];
            snprintf(hh, sizeof hh, "The lowest and highest ground the map can have. The game stores 256 steps between them, so a smaller range gives smoother slopes: on this map a step is %.2f m. Real levels use -20 to 100 m. Ground outside a new range is cut off.", s.terrain.step());
            help(hh);
        }
    }
    page_body_end();
    ImGui::TextDisabled("%s undoes a stroke.", kt(A_UNDO).c_str());
    page_footer_end();
    ImGui::EndDisabled();
}

void App::page_paint() {
    Scenario& s = *doc_;
    if (s.read_only) {
        terrain_read_only_ui();
        return;
    }
    const Tool before = tool_;
    if (tool_ == Tool::PaintTexture || tool_ == Tool::PaintColor || tool_ == Tool::PaintType || tool_ == Tool::PaintCulture) paint_last_ = tool_;
    const Tool cur = paint_last_;
    ImGui::BeginDisabled(s.read_only);
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    tool_button("Texture", Tool::PaintTexture, tool_, w, "Drag on the map to paint a ground texture: pebbles, sand, rock...", true); ImGui::SameLine();
    tool_button("Colour tint", Tool::PaintColor, tool_, w, "Drag on the map to tint the ground colour over the textures.", true);
    tool_button("Boundaries", Tool::PaintType, tool_, w, "Mark where players and bots may go (Playable) and where they may not (Border, Out of bounds).", true); ImGui::SameLine();
    tool_button("Plants", Tool::PaintCulture, tool_, w, "Drag on the map to grow grass, bushes and other ground plants of this level (or clear them).", true);
    // Boundaries paints an overlay that is otherwise hidden: show it as soon as the tool is chosen.
    if (tool_ == Tool::PaintType && before != Tool::PaintType && !show_typemap_) { show_typemap_ = typemap_auto_ = true; overlay_dirty_ = true; }
    form_label("Brush size", form_col());
    brush_size_slider(cur);
    tip(("How wide the brush is (the yellow circle on the map). All the tools on this page share it, and Scatter uses the same size. Ctrl+click the slider to type a size. Keys: " + kt(A_BRUSH_SMALLER) + " smaller, " + kt(A_BRUSH_BIGGER) + " bigger, or Ctrl+wheel over the map. " + brush_floor_note(cur)).c_str());
    if (cur != Tool::PaintType) {
        float& st = strength_ref(cur);   // each paint tool keeps its own: Texture 100%, Colour tint 50%, Plants 50%
        float pct = st * 100.f;
        form_label("Strength", form_col());
        if (theme::slider_float("##speed", &pct, 2.f, 100.f, "%.0f%%")) { st = std::clamp(pct / 100.f, 0.02f, 1.f); strength_dirty_ = true; }
        if (ImGui::IsItemDeactivatedAfterEdit() && strength_dirty_) { settings_.tool_strength[strength_slot(cur)] = st; settings_.save(); strength_dirty_ = false; }
        tip(("How much one stroke changes the ground: 100% paints it fully at the centre of the brush. Each tool keeps its own. Keys: " + kt(A_BRUSH_WEAKER) + " weaker, " + kt(A_BRUSH_STRONGER) + " stronger, or Shift+wheel over the map.").c_str());
    }
    if (cur == Tool::PaintTexture || cur == Tool::PaintColor) {
        const char* edge = brush_hardness_ < 0.25f ? "Soft" : brush_hardness_ < 0.6f ? "Medium" : "Hard";
        form_label("Edge", form_col());
        theme::slider_float("##edge", &brush_hardness_, 0.f, 0.95f, edge);
        tip("Soft fades the brush out toward its rim; Hard paints evenly right up to the yellow circle.");
    }
    page_body_begin("##paintbody");
    paint_section_ui();
    if (theme::section("Whole map")) terrain_whole_map_ui();
    if (theme::section("Advanced paint")) terrain_advanced_paint_ui();
    page_body_end();
    ImGui::EndDisabled();
    ground_picker_ui();
    plant_picker_ui();
}

void App::page_water() {
    Scenario& s = *doc_;
    if (s.read_only) {
        ImGui::BeginDisabled();
        water_list_ui();
        ImGui::EndDisabled();
        return;
    }
    water_new_ui();
    ImGui::SeparatorText("Waters in this map");
    page_body_begin("##waterbody");
    water_list_ui();
    page_body_end();
}


void name_item(const std::string& label) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    IMGUI_TEST_ENGINE_ITEM_INFO(g.LastItemData.ID, label.c_str(), g.LastItemData.StatusFlags);
}

// The palette picture of a template: its model's thumbnail; for water (a flat plane that renders white) the water
// material itself, drawn with the water shader.
ID3D11ShaderResourceView* App::palette_picture(const TemplateInfo& t) {
    if (t.cls_name == "MAEffect") {
        // An effect's model is mostly an empty holder for its particle nodes (the thumbnail came out a blank square:
        // BA Claw Hit, Battle armor explosion, Crush explosion...), so show the first sprite texture it emits instead.
        ID3D11ShaderResourceView* th = models_.thumbnail(t.model, t.mtb);  // rendered once and cached either way (the grid's budget)
        auto first_sprite = [&](const ma2::ParticleFx* fx) -> ID3D11ShaderResourceView* {
            if (fx)
                for (const ma2::PEmitter& em : fx->emitters)
                    for (const ma2::PSystem& sy : em.systems)
                        if (sy.has_sprite && !sy.sprite.texture.empty())
                            if (ID3D11ShaderResourceView* tex = models_.texture(sy.sprite.texture)) return tex;
            return nullptr;
        };
        // The template's own EffectFile first (most effect templates carry it themselves, not on their model).
        if (const WObj* o = cat_ ? cat_->object(t.uid) : nullptr; o && !o->str("EffectFile").empty())
            if (ID3D11ShaderResourceView* tex = first_sprite(models_.particle(o->str("EffectFile")))) return tex;
        if (const std::vector<FxRef>* refs = models_.effects(t.model, t.mtb))
            for (const FxRef& r : *refs)
                if (ID3D11ShaderResourceView* tex = first_sprite(models_.particle(r.file))) return tex;
        return th;
    }
    if (t.cls_name != "MAWater") return models_.thumbnail(t.model, t.mtb);
    const WaterMaterial& wm = models_.water_material(t.model);
    Renderer::WaterLook w;
    w.wave = wm.wave;
    w.refl = wm.refl;
    w.deep = wm.deep;
    w.shallow = wm.shallow;
    w.fresnel_min = wm.fresnel_min;
    w.fresnel_max = wm.fresnel_max;
    w.key = lower(t.model);
    return wm.ok ? r_.water_preview(w) : nullptr;
}

// Plain name of a water type (the template names carry tile sizes that are not the size you get).
std::string water_plain_name(const TemplateInfo& t) {
    std::string m = lower(t.model + " " + t.name);
    if (m.find("citynight") != std::string::npos) return "City canal (night)";
    if (m.find("oily") != std::string::npos) return "Oily pool (Hesperus)";
    if (m.find("hesperus") != std::string::npos) return "Hesperus water";
    if (m.find("waterfall") != std::string::npos) return "Waterfall pool";
    if (m.find("water_sw") != std::string::npos || m.find("swamp") != std::string::npos || m.find("_the_") != std::string::npos) return "Swamp water";
    if (m.find("250") != std::string::npos) return "Clear water (fine waves)";
    if (m.find("water1000x1000") != std::string::npos) return "Clear water";
    std::string n = t.name;  // fallback: the template name without the tile size and "NEW"
    for (const char* cut : {" NEW", " new", "_1000x1000", "1000x1000", "_250_250"}) {
        size_t k;
        while ((k = n.find(cut)) != std::string::npos) n.erase(k, std::strlen(cut));
    }
    std::replace(n.begin(), n.end(), '_', ' ');
    return n;
}

const std::vector<const TemplateInfo*>& App::water_types() {
    if (water_types_.empty()) {
        std::set<std::string> models;
        for (auto& ti : cat_->all())
            if (ti.cls_name == "MAWater" && ti.placeable && !ti.model.empty() && !ti.archives.empty() && models.insert(lower(ti.model)).second)
                water_types_.push_back(&ti);
        std::vector<std::pair<std::string, const TemplateInfo*>> keyed;
        for (const TemplateInfo* t : water_types_) keyed.push_back({std::string(water_can_resize(t->uid) ? "0" : "1") + lower(water_type_name(*t)), t});
        std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < keyed.size(); i++) water_types_[i] = keyed[i].second;
    }
    return water_types_;
}

void App::arm_water(uint32_t uid) {
    place_road_.clear();
    if (water_can_resize(uid)) {
        water_new_tpl_ = uid;
        tool_ = Tool::PlaceWater;
    } else {
        place_template_ = uid;
        tool_ = Tool::PlaceObject;
    }
}

// The water types as picture tiles (the same list, names and order as the palette's Water group):
// the uid picked this frame, else 0.
uint32_t App::water_type_combo(const char* id, uint32_t cur_uid, float tile_px) {
    const TemplateInfo* cur = cur_uid ? cat_->find(cur_uid) : nullptr;
    ImGui::PushID(id);
    ImGui::Text("Type: %s", cur ? water_type_name(*cur).c_str() : "(none)");
    tip("Click a picture to pick the kind of water (colour, waves, sky reflection).");
    uint32_t pick = 0;
    float tile = tile_px * dpi_, x_end = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    int n = 0;
    bool fixed_heading = false;
    for (const TemplateInfo* tp : water_types()) {
        const TemplateInfo& ti = *tp;
        bool rs = water_can_resize(ti.uid);
        if (!rs && !fixed_heading) {
            fixed_heading = true;
            n = 0;
            ImGui::TextDisabled("Fixed size (can't be resized):");
            tip("These waters are made of several layers, so they keep the size they were made at.");
        }
        if (n++ > 0) {
            ImGui::SameLine();
            if (ImGui::GetCursorScreenPos().x + tile > x_end) ImGui::NewLine();
        }
        ImGui::PushID(int(ti.uid));
        bool on = cur && lower(cur->model) == lower(ti.model);
        if (on) theme::push_toggle_on();
        std::string nm = water_type_name(ti);
        ID3D11ShaderResourceView* th = palette_picture(ti);
        ImGui::BeginGroup();
        bool hit = th ? ImGui::ImageButton("##t", (ImTextureID)(intptr_t)th, ImVec2(tile, tile)) : ImGui::Button("?", ImVec2(tile + 8, tile + 8));
        if (on) theme::pop_toggle_on();
        if (th) name_item(nm + id);
        // The previews of similar waters look alike: the plain name under each tile tells them apart.
        float tx = ImGui::GetCursorPosX();
        ImGui::PushTextWrapPos(tx + tile + 8);
        ImGui::TextWrapped("%s", nm.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (hit) pick = ti.uid;
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(nm.c_str());
            if (!rs) ImGui::TextDisabled("Fixed size (made of several layers)");
            ImGui::TextDisabled("%s", ti.name.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return pick;
}

// Water look: swaps a water plane for another water template (its model carries the water
// material: colours, waves, sky reflection). The game draws the template's model, so the
// instance is re-placed from the new template at the same spot, keeping its name and size.
bool App::water_look_ui(WObj& o) {
    Scenario& s = *doc_;
    const TemplateInfo* cur = cat_->find(o.template_id());
    uint32_t pick = water_type_combo("##look", cur ? cur->uid : 0);
    if (!pick || (cur && pick == cur->uid)) return false;
    push_undo("water type");
    Vec3 p;
    get_pos(o, p);
    float h = heading(o);
    std::string old_name = ScenarioIO::water_custom_name(o, cur ? cur->name : std::string());
    bool named = !old_name.empty();
    float cw = 0, cl = 0;
    bool sized = ScenarioIO::water_custom_size(o, cw, cl);
    int shape = int(ScenarioIO::water_custom_shape(o));
    uint32_t old = o.uid();  // o is invalid once objects are added
    uint32_t old_parent = o.parent();
    uint32_t root = io_->place_template(s, pick, p, h);
    if (!root) return false;
    // The new water takes the old one's place among its siblings, so the water list keeps its order (and an
    // unnamed "Water N" keeps its number) instead of jumping to the end.
    if (old_parent && old_parent != root) {
        wedit::reparent(s.world, root, old_parent);
        for (int guard = 0; guard < 100000; guard++) {
            const WObj* prev = nullptr;
            for (auto& n : s.world.objs)
                if (n.sibling() == root) { prev = &n; break; }
            if (!prev || prev->uid() == old || !wedit::move_sibling(s.world, root, -1)) break;
        }
    }
    for (auto& n : s.world.objs)
        if (n.uid() == root) {
            set_pos(n, p);  // keep the surface height (place_template may snap)
            if (named) n.set_str("Description", old_name);
            if (sized && water_can_resize(pick)) set_water_size(n, cw, cl, shape);
        }
    wedit::remove_subtree(s.world, old);
    selection_ = {root};
    world_changed();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Players

void App::tab_players() {
    Scenario& s = *doc_;
    ImGui::BeginDisabled(s.read_only);
    no_spawns_hint(s, true);
    if (!s.multiplayer) {
        ImGui::SeparatorText("Player start");
        ImGui::TextWrapped("Where the player starts the mission. Enemies are placed with the Place tool and get their orders on the Objectives & Triggers tab (missions have no bots).");
        if (tool_button("Move player start", Tool::PlaceStart, tool_, -1, "Then click the map where the player should start. Drag to face it, or Q / E turn it.") && tool_ == Tool::PlaceStart)
            if (const WObj* o = object_by_uid(s.world, player_start_uid(s.world)))  // the arrow starts at the start's own facing, not the last placed object's
                place_heading_ = o->f32("StartOrientation");
        if (uint32_t su = player_start_uid(s.world)) {
            ImGui::EndDisabled();  // looking is fine on a read-only level too
            if (ImGui::Button("Show me where", ImVec2(-1, 0))) { select_uid(su); zoom_to_selection(); }
            tip("Selects the player start and moves the camera to it.");
            ImGui::BeginDisabled(s.read_only);
        }
        ImGui::EndDisabled();
        return;
    }
    int counts[3] = {0, 0, 0};
    for (auto& o : s.world.objs)
        if (o.cls == K_LEVELSTART) { int t = o.i32("TeamIndex"); if (t >= 0 && t <= 2) counts[t]++; }
    ImGui::SeparatorText("Spawn points");
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Red %d", counts[1]); ImGui::SameLine(90 * dpi_);
    ImGui::TextColored(ImVec4(0.4f, 0.6f, 1, 1), "Blue %d", counts[2]); ImGui::SameLine(180 * dpi_);
    ImGui::Text("Free-for-all %d", counts[0]);
    help("Where players and bots appear. Team game types need at least 6 Red and 6 Blue spawn points, on opposite sides of the map. Free-for-all types (Destruction, Last Man Standing...) need at least 6 free-for-all ones.");
    ImGui::RadioButton("Red", &spawn_team_, 1); ImGui::SameLine();
    ImGui::RadioButton("Blue", &spawn_team_, 2); ImGui::SameLine();
    ImGui::RadioButton("Free-for-all", &spawn_team_, 0);
    tool_button("Place spawn point", Tool::PlaceSpawn, tool_, -1, "Click the map to place one spawn point for the team chosen above. Q / E turn it first.");
    if (ImGui::Button("Add 6 on a ring around the camera", ImVec2(-1, 0))) {
        push_undo("spawn ring");
        io_->add_spawn_ring(s, spawn_team_, {cam_.tx, 0, cam_.tz}, 30.f, 6);
        world_changed();
    }
    tip("Adds 6 spawn points for the chosen team in a circle where the camera looks. Move the camera to that team's side of the map first.");
    ImGui::SeparatorText("Vehicle pads");
    ImGui::TextWrapped("A pad where a 'Mech or vehicle waits for anyone to climb in. Pads have no team: either team's players and bots can take it.");
    auto& pads = io_->pad_kinds();
    if (!pads.empty()) {
        pad_kind_ = std::clamp(pad_kind_, 0, int(pads.size()) - 1);
        // Vehicles whose files the theme archive holds come first; the others add [Assets].
        const MapInfo* bm = nullptr;
        for (auto& m : gd_.maps()) if (iequals(m.resource, s.base)) bm = &m;
        auto in_base = [&](const ScenarioIO::PadKind& k) { return bm && iequals(k.map, bm->name); };
        if (begin_combo("Vehicle", pads[size_t(pad_kind_)].label.c_str())) {
            std::set<std::string> shown;
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 1) ImGui::SeparatorText("From other levels (adds files)");
                for (int i = 0; i < int(pads.size()); i++) {
                    if (in_base(pads[size_t(i)]) != (pass == 0)) continue;
                    if (!shown.insert(lower(pads[size_t(i)].label)).second) continue;  // first map per vehicle
                    ImGui::PushID(i);
                    if (ImGui::Selectable((pads[size_t(i)].label + "  (" + pads[size_t(i)].map + ")").c_str(), i == pad_kind_)) pad_kind_ = i;
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        tip("Vehicles listed first come with this map's theme; the others are loaded from other levels.");
        tool_button("Place vehicle pad", Tool::PlacePad, tool_, -1, "Click the map to place a pad with the vehicle chosen above. Q / E turn it first.");
    }
    ImGui::SeparatorText("Game-mode objects");
    ImGui::TextWrapped("Adds what a game type needs (flags, bases, generators, data cores, turrets) copied from a real level that has it. Pick the level on the left, press Add, then drag the pieces where you want them. Only the game types ticked on the Map tab are listed.");
    // Only the game types this map is ticked for (Map tab) get a row; the rest are one tick away.
    static bool kits_all = false;
    ImGui::Checkbox("Show every game type", &kits_all);
    tip("Off: only the game types ticked on the Map tab are listed. On: all ten, for example to borrow a type's objects before ticking it.");
    int gi = 0;
    for (auto& g : game_types()) {
        ImGui::PushID(g.ini);
        auto maps = io_->maps_with_mode(g.ini);
        bool wanted = kits_all || s.types.empty();
        for (auto& t : s.types) if (iequals(t, g.ini)) wanted = true;
        if (maps.empty() || !wanted) { ImGui::PopID(); gi++; continue; }
        int& src = kit_source_[gi % 16];
        src = std::clamp(src, 0, int(maps.size()) - 1);
        ImGui::SetNextItemWidth(150 * dpi_);
        if (begin_combo("##src", ("from " + pretty_name(maps[size_t(src)])).c_str())) {
            for (int i = 0; i < int(maps.size()); i++) if (ImGui::Selectable(pretty_name(maps[size_t(i)]).c_str(), i == src)) src = i;
            ImGui::EndCombo();
        }
        tip("Which real level to copy the objects from (each game type lists only the levels that have it).");
        ImGui::SameLine();
        if (ImGui::Button((std::string("Add ") + g.label).c_str(), ImVec2(-1, 0))) {
            push_undo("game-mode objects");
            std::string err;
            int before = int(placed_count());
            int n = io_->add_mode_kit(s, g.ini, maps[size_t(src)], &err);
            io_->sync_spawn_groups(s);
            world_changed();
            int placed = 0;
            if (n) {  // as the status bar counts them
                rebuild_objects();
                placed = int(placed_count()) - before;
            }
            if (!n && !undo_.empty()) drop_last_undo();  // nothing changed: no undo step for it
            status_ = n && placed <= 0 ? std::string(g.label) + " now also uses the matching pieces already on the map"
                    : n ? "Added " + std::to_string(placed) + " pieces for " + g.label + (g.team ? " by each side's spawn points (drag them where you want them)" : " (drag them where you want them)")
                        : "Nothing added: " + (err.empty() ? "this map already has the " + std::string(g.label) + " objects from " + pretty_name(maps[size_t(src)]) + "." : err);
            status_time_ = 5;
        }
        ImGui::PopID();
        gi++;
    }
    ImGui::EndDisabled();
}

// The test match settings (game type, bots, skill): under the Test button's arrow (Test settings sit next to Test).
void App::test_settings_ui() {
    Scenario& s = *doc_;
    ImGui::TextWrapped("Save, then press Test (%s). The game starts and goes straight into a match on this map, with the game type, "
                       "bots and skill chosen below.", kt(A_TEST).c_str());
    if (!s.types.empty()) {
        test_mode_ = std::clamp(test_mode_, 0, int(s.types.size()) - 1);
        const GameTypeDef* g = find_game_type(s.types[size_t(test_mode_)]);
        form_label("Game type", form_col());
        if (begin_combo("##testtype", g ? g->label : s.types[size_t(test_mode_)].c_str())) {
            for (int i = 0; i < int(s.types.size()); i++) {
                const GameTypeDef* gi2 = find_game_type(s.types[size_t(i)]);
                ImGui::PushID(i);
                if (ImGui::Selectable(gi2 ? gi2->label : s.types[size_t(i)].c_str(), i == test_mode_)) test_mode_ = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        tip("The game type the test match plays (one of the types ticked on the Map tab).");
        bool grinder = iequals(s.types[size_t(test_mode_)], "Grinder"), ffa = g && !g->team && !grinder;
        ImGui::BeginDisabled(grinder);
        if (ffa) {
            int n = test_red_ + test_blue_;
            form_label("Bots", form_col());
            if (theme::slider_int("##testbots", &n, 0, 11)) { test_red_ = n / 2 + n % 2; test_blue_ = n / 2; }
            tip("How many bots play in the free-for-all match.");
            why_disabled(grinder, "Grinder always plays with one bot, so the bot counts do not apply.");
        } else {
            // You join the team with fewer bots, so the uneven default (2 Red, 3 Blue) makes even teams with you.
            const char* you = test_red_ < test_blue_ ? "Red" : test_blue_ < test_red_ ? "Blue" : nullptr;
            const std::string why = you ? std::string("You play on the ") + you + " team: it has one bot fewer, so the teams are even with you."
                                        : std::string("Both teams have the same number of bots, so your team has one player more.");
            form_label(you && you[0] == 'R' ? "Red bots (plus you)" : "Red bots", form_col());
            theme::slider_int("##redbots", &test_red_, 0, 6);
            tip(("Bots on the Red team. " + why).c_str());
            why_disabled(grinder, "Grinder always plays with one bot, so the bot counts do not apply.");
            form_label(you && you[0] == 'B' ? "Blue bots (plus you)" : "Blue bots", form_col());
            theme::slider_int("##bluebots", &test_blue_, 0, 6);
            tip(("Bots on the Blue team. " + why).c_str());
            // Why the defaults are uneven (G-a): you make up the short side.
            bool red = test_red_ <= test_blue_;
            ImGui::TextDisabled("You join %s: %d against %d with you.", red ? "Red" : "Blue", (red ? test_red_ : test_blue_) + 1,
                                red ? test_blue_ : test_red_);
        }
        ImGui::EndDisabled();
        if (grinder) ImGui::TextDisabled("Grinder always plays with 1 bot.");
        form_label("Bot skill", form_col());
        if (begin_combo("##testskill", skill_name(test_skill_))) {
            for (int k = 0; k <= 4; k++) if (ImGui::Selectable(skill_name(k), k == test_skill_)) test_skill_ = k;
            ImGui::EndCombo();
        }
        tip("How well the bots play. Mixed gives each bot its own skill.");
        hint_wrapped("For the test only: your New Game settings in the game are not changed.");
    } else {
        ImGui::TextDisabled("Tick a game type on the Map tab first.");
    }
}

// ---------------------------------------------------------------------------------------------
// Units

// Special palette categories (dictionary audit, dictionary/audit.json): listed and
// placeable like everything else, but grouped and labelled so nobody mistakes them for game pieces.
static const char* kCatCutscene = "Cutscene props";
static const char* kCatWrecks = "Wrecks and ruins";
static const char* kCatDecor = "Decoration only (can't be used)";
static const char* kCatSkies = "Skies (pick one on the Environment tab)";
const char* special_category_tip(const std::string& c) {
    if (c == kCatCutscene) return "Pieces made for the game's cutscenes (stills, staged 'Mechs, set pieces). No real level places them in "
                                  "play; many cannot be shot or walked into.";
    if (c == kCatWrecks) return "Destroyed, collapsed and wrecked versions of buildings and vehicles. The game swaps a building to these when "
                                "it is destroyed; you can also place them as ruins.";
    if (c == kCatDecor) return "Look-alikes made only for show: the player cannot climb in or use them (for example the Ragnarok set "
                               "piece and the close-up Battle Armor).";
    if (c == kCatSkies) return "A map has one sky. Choose it on the Environment tab (Sky); placing one here would add a second sky.";
    return nullptr;
}


// Friendly palette names (playtest, 2026-10-21): a recognisable name with the official name in
// parentheses, for things whose identity is clear. Code-style template names ("Env_NOR_Conifer02_Snow
// _Single") are split into words; a name gets a friendly form only when EVERY word is in this list,
// so abbreviations nobody has decoded (BV, WS, Rstrctr...) keep the official name alone.
static const std::unordered_map<std::string, std::string>& name_words() {
    static const std::unordered_map<std::string, std::string> w = {
        // prefixes that only say "environment / building / people" (the category already does)
        {"env", ""}, {"bld", ""}, {"pep", ""}, {"com", ""}, {"bldcom", ""}, {"bldmil", "military"}, {"bldind", "industrial"},
        // suffixes decoded by the research (wiki t12 template-name-suffixes): _WS/_CS/_DS are the
        // destroyed-state husks, _BV an indestructible collidable copy, _QTR a quarter length
        {"ws", "destroyed"}, {"cs", "destroyed"}, {"ds", "destroyed"}, {"bv", "indestructible"}, {"qtr", "quarter"},
        {"ped", "pedestrian"}, {"rommel", "Rommel"}, {"icarus", "Icarus"}, {"cougar", "Cougar"}, {"talon", "Talon"},
        {"loki", "Loki"}, {"mad", "Mad"}, {"cat", "Cat"}, {"atlas", "Atlas"}, {"gator", "Gator"},
        {"mil", "military"}, {"ind", "industrial"}, {"civ", "civilian"}, {"vmil", "military"}, {"vciv", "civilian"},
        // planets and factions (BattleTech)
        {"dan", "Dante"}, {"dante", "Dante"}, {"hes", "Hesperus"}, {"hesperus", "Hesperus"}, {"nor", "Northwind"},
        {"the", "Thestria"}, {"ter", "Terra"}, {"wob", "Word of Blake"}, {"kurita", "Kurita"}, {"chiba", "Chiba"},
        // abbreviations whose meaning the categories or other names confirm
        {"mp", "multiplayer"}, {"lp", "low-poly"}, {"rd", "road"}, {"strght", "straight"}, {"twr", "tower"},
        {"mntn", "mountain"}, {"wndswpt", "windswept"}, {"skywlk", "skywalk"}, {"pwr", "power"}, {"lg", "large"},
        {"sm", "small"}, {"med", "medium"}, {"md", "medium"}, {"bb", "building base"}, {"bd", "building base"},
        {"clrshft", "colour-shift"}, {"hlf", "half"}, {"rmp", "ramp"}, {"elb", "elbow"}, {"apc", "APC"},
        {"vtol", "VTOL"}, {"ac", "AC"}, {"lcd", "LCD"}, {"mk", "Mk"}, {"benchand", "bench and"}, {"hanger", "hangar"},
        {"officef", "office F"}, {"officek", "office K"}, {"hextile", "hex tile"}, {"patternbrick", "pattern brick"},
        {"concretesquare", "concrete square"}, {"deadtree", "dead tree"}, {"deadbush", "dead bush"},
        {"blowingpapers", "blowing papers"}, {"workshoparea", "workshop area"}, {"emmisive", "glowing"}, {"nobtm", "no bottom"},
        {"indest", "indestructible"}, {"vert", "vertical"}, {"observ", "observation"}, {"vid", "video"}, {"clr", "colour"},
        {"shft", "shift"}, {"expl", "explosion"}, {"gen", "generic"}, {"conc", "concrete"}, {"dfa", "death-from-above"},
        {"missle", "missile"},
        // The level folders' own planet codes (category "Planet Hesperus - HES - (Wasteland)", "Northwind ... Arc")
        {"wst", "Wasteland"}, {"arc", "Arctic"}, {"drctn", "direction"},
        {"x", "x"},
    };
    return w;
}

static const std::unordered_set<std::string>& plain_words() {
    static const std::unordered_set<std::string> w = {
        "acid", "aero", "air", "airbase", "aircraft", "alley", "alone", "ammo", "annex", "apartment", "arch", "arctic", "arena",
        "armor", "arrival", "asphalt", "asteroid", "atlas", "aurora", "auto", "banner", "barge", "barracks", "barrel",
        "barricade", "barrier", "base", "bench", "bill", "block", "blower", "blue", "board", "boss", "boxes", "bridge", "bunker",
        "buoy", "bush", "canal", "cannon", "canopy", "canyon", "cap", "capital", "car", "card", "cargo", "carrier", "cat",
        "catwalk", "center", "chrome", "chunk", "city", "cliff", "climbing", "clump", "cluster", "column", "comm", "command",
        "concrete", "conifer", "console", "coolant", "copter", "core", "corner", "cougar", "cover", "crane", "crystal", "dais",
        "dam", "damaged", "dealer", "depot", "derelict", "digester", "dish", "dock", "door", "dropship", "drum", "dumpster",
        "dusk", "elbow", "end", "exit", "face", "factory", "female", "fence", "field", "fire", "foliage", "footing", "forest",
        "friendly", "furnace", "gantry", "garage", "gate", "gator", "generator", "generic", "glacier", "gothic", "grass", "grate",
        "green", "grey", "ground", "group", "guard", "guardrail", "gun", "hangar", "hanging", "head", "helipad", "highrise",
        "highway", "home", "hospital", "hub", "hull", "hut", "icarus", "information", "inner", "inside", "justice", "landing",
        "landside", "large", "laser", "launch", "light", "locked", "loki", "long", "lot", "mad", "main", "male", "mangrove",
        "manhole", "mech", "metal", "military", "missile", "mix", "moss", "mound", "new", "night", "nuclear", "observation",
        "off", "office", "oily", "on", "one", "open", "outer", "outside", "pad", "parking", "parts", "pedestrian", "perimeter",
        "pilot", "pipe", "pipeline", "pipes", "plane", "planet", "plant", "planters", "plaque", "platform", "player", "pontoon",
        "popup", "port", "portable", "power", "prison", "pump", "pylon", "quarters", "radar", "radio", "ramp", "red", "repair",
        "research", "retention", "rock", "rocks", "rommel", "rubble", "scaffold", "scanner", "scow", "screen", "section",
        "security", "shanty", "sign", "simple", "single", "siren", "sky", "skywalk", "small", "smoke", "snow", "soccer", "space",
        "spaceport", "span", "stack", "stacks", "stand", "station", "statue", "stone", "stop", "stoplight", "storage",
        "stormfront", "stormy", "street", "streetlight", "structure", "sun", "support", "swamp", "switch", "talon", "tall",
        "tank", "temp", "tent", "top", "tower", "trade", "tram", "transmitter", "transportation", "tree", "truck", "trunk",
        "tuft", "tunnel", "turret", "two", "uber", "unit", "utility", "valve", "vehicle", "vendor", "vent", "video", "walk",
        "wall", "warehouse", "warm", "warp", "warrior", "water", "waterfall", "weapon", "weapons", "weed", "wide", "windmill",
        "workshop", "wrecked", "yellow",
        "angled", "ballistic", "battle", "bird", "cloud", "crush", "debris", "dust", "energy", "for", "gas", "hacked", "hit",
        "neon", "nuke", "pickup", "plasma", "salvage", "scatter", "search", "tiny", "topple",
        // effect names (effect_plain_name)
        "active", "bomb", "building", "collapse", "crash", "destruction", "geyser", "hole", "infantry", "leaf", "mini",
        "map", "object", "oil", "person", "pop", "sparks", "spider", "spotlight", "terrain", "trees", "wood",
    };
    return w;
}

// Raw names the audit met that the word list cannot read. The canal embankment pieces say what
// they are (the 140 m straight is a wall of the canal, not a "Canal wall"), the footbridge says its length before it is placed.
static const std::unordered_map<std::string, std::string>& name_overrides() {
    static const std::unordered_map<std::string, std::string> m = {
        {"Canal_Wall_ST", "Canal embankment, straight"},
        {"Canal_Wall_CP", "Canal embankment, end cap"},
        {"Canal_Wall_QTR", "Canal embankment, quarter"},
        {"Canal_Wall_HLF", "Canal embankment, half"},
        {"Canal_Wall_RMP", "Canal embankment, ramp"},
        {"Canal_Wall_INT", "Canal embankment, intersection"},
        {"Env_HES_HesperusArena_Gate_Const", "Arena gate (Hesperus), under construction"},
        {"Env_HES_HesperusArena_Gate_Scaffold", "Arena gate (Hesperus), scaffold"},
        {"Env_HES_HesperusArena_Gate", "Arena gate (Hesperus)"},
        {"Env_HES_HesperusArena_Gate_Locked", "Arena gate (Hesperus), locked"},
        {"Bay_VTOL", "VTOL bay"},
        {"BldMil_VtolPad_ClrShft", "VTOL pad"},
        {"Air Deploy Laset Turret - Mission04", "Air-deployed laser turret"},
        {"Env_PedBridgeB", "Pedestrian bridge B (236 m arch)"},
        {"Env_PedBridgeB_BV", "Pedestrian bridge B, indestructible (236 m arch)"},
    };
    return m;
}

// A name that still holds "_" after the word list (an unknown abbreviation in it): its words anyway, never the raw code.
// "Canyon_Rd_Str" -> "Canyon road str", "Prometheus - Cinema_3_5" -> "Prometheus - Cinema 3.5", "Env_NoFly_Pylon" -> "No fly pylon".
// A closing _ST / _CP says what the piece is (wiki: name suffixes _CP, _ST, _QTR, _Vert; the word list has _QTR and _Vert).
// Capitals that are a whole word ("WTF", "SAC") stay; the game name stays on the row tooltip.
static std::string plain_fallback(const std::string& raw) {
    if (raw.find('_') == std::string::npos) return raw;
    std::string t;
    for (size_t i = 0; i < raw.size(); i++) {  // "5_5" -> "5.5": shot and variant numbers
        const bool d = raw[i] == '_' && i > 0 && i + 1 < raw.size() && std::isdigit(uint8_t(raw[i - 1])) && std::isdigit(uint8_t(raw[i + 1]));
        t += d ? '.' : raw[i];
    }
    std::string out, chunk;
    auto flush_chunk = [&] {
        if (chunk.empty()) return;
        if (chunk.find('_') == std::string::npos) { out += chunk; chunk.clear(); return; }
        std::vector<std::string> parts;
        std::string cur;
        for (char c : chunk) { if (c == '_') { parts.push_back(cur); cur.clear(); } else cur += c; }
        parts.push_back(cur);
        std::vector<std::string> words;
        for (size_t pi = 0; pi < parts.size(); pi++) {
            const std::string& part = parts[pi];
            if (part.empty()) continue;
            const std::string pl = lower(part);
            if (pi + 1 == parts.size() && pi > 0) {
                if (pl == "st") { words.push_back("straight"); continue; }
                if (pl == "cp") { words.push_back("end cap"); continue; }
            }
            // camelCase / letter-digit pieces of this part
            std::vector<std::string> toks;
            std::string tk;
            for (size_t i = 0; i < part.size(); i++) {
                const char c = part[i];
                if (!tk.empty()) {
                    const char p = tk.back();
                    const bool pdig = std::isdigit(uint8_t(p)) != 0, dig = std::isdigit(uint8_t(c)) != 0;
                    const bool up = std::isupper(uint8_t(c)) != 0, pup = std::isupper(uint8_t(p)) != 0;
                    const bool next_low = i + 1 < part.size() && std::islower(uint8_t(part[i + 1])) != 0;
                    if ((dig != pdig && c != '.' && p != '.') || (up && !pup) || (up && pup && next_low)) { toks.push_back(tk); tk.clear(); }
                }
                tk += c;
            }
            if (!tk.empty()) toks.push_back(tk);
            for (const std::string& k : toks) {
                const std::string kl = lower(k);
                auto w = name_words().find(kl);
                if (w != name_words().end()) { if (!w->second.empty()) words.push_back(w->second); continue; }
                const bool num = std::isdigit(uint8_t(k[0])) != 0;
                const bool shout = k.size() >= 2 && k.size() <= 4 && std::all_of(k.begin(), k.end(), [](char c) { return std::isupper(uint8_t(c)) != 0; });
                words.push_back(num || shout ? k : kl.size() == 1 ? std::string(1, char(std::toupper(kl[0]))) : kl);
            }
        }
        for (size_t i = 0; i < words.size(); i++) out += (i ? " " : "") + words[i];
        chunk.clear();
    };
    for (char c : t) {
        if (c == ' ') { flush_chunk(); out += ' '; }
        else chunk += c;
    }
    flush_chunk();
    if (!out.empty()) out[0] = char(std::toupper(uint8_t(out[0])));
    return out;
}

// "Env_NOR_Conifer02_Snow_Single" -> "Northwind conifer 02 snow single"; "" when not confident.
static std::string friendly_name(const std::string& raw) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(raw);
    if (it != cache.end()) return it->second;
    std::string& out = cache[raw];
    if (auto ov = name_overrides().find(raw); ov != name_overrides().end()) return out = ov->second;
    // Only code-style names: readable ones ("Atlas", "Armor (15T)", "Set - Icarus") stay as they are.
    if (raw.find('_') == std::string::npos) return out;
    // A code name with a designer's note ("Expl_Gas_LG - (Splash Damage)", "VMil_RommelMK2 - WOB"): the name in words,
    // the note in plain lower case ("Gas large explosion... - splash damage"), faction words kept ("Word of Blake").
    if (size_t dash = raw.find(" -"); dash != std::string::npos) {
        std::string head = raw.substr(0, dash);
        if (head.find('_') == std::string::npos) return out;
        std::string f = friendly_name(head);
        if (f.empty()) return out;
        std::string note, word;
        auto flush = [&] {
            if (word.empty()) return;
            std::string l = lower(word);
            auto w = name_words().find(l);
            if (l == "cs") l = "cutscene";
            else if (w != name_words().end() && !w->second.empty() && std::isupper(uint8_t(w->second[0]))) l = w->second;
            note += (note.empty() || note.back() == ' ' ? "" : " ") + l;
            word.clear();
        };
        for (char c : raw.substr(dash + 2)) {
            if (c == '-' && word.empty()) { if (!note.empty() && note.back() != ',') note += ","; continue; }  // "x - y" -> "x, y"
            if (c == ' ' || c == '(' || c == ')' || c == '!') { flush(); continue; }
            word += c;
        }
        flush();
        while (!note.empty() && (note.back() == ',' || note.back() == ' ')) note.pop_back();
        while (!note.empty() && (note.front() == ',' || note.front() == ' ')) note.erase(0, 1);
        return out = note.empty() ? f : f + " - " + note;
    }
    std::string base = raw.substr(0, raw.find(" ("));  // "Water_THE_500x500 (no splash)": the note stays in the official name shown beside it
    std::vector<std::string> toks;
    size_t us = base.find('_');
    std::string head = base.substr(0, us);
    if (us != std::string::npos && name_words().count(lower(head))) {
        toks.push_back(lower(head));
        base = base.substr(us + 1);
    }
    std::string cur;
    auto flush = [&] { if (!cur.empty()) toks.push_back(cur); cur.clear(); };
    for (size_t i = 0; i < base.size(); i++) {
        char c = base[i];
        if (c == '_' || c == ' ' || c == '-') { flush(); continue; }
        bool dig = c >= '0' && c <= '9', up = c >= 'A' && c <= 'Z';
        if (!cur.empty()) {
            char p = cur.back();
            bool pdig = p >= '0' && p <= '9', pup = p >= 'A' && p <= 'Z';
            bool next_low = i + 1 < base.size() && base[i + 1] >= 'a' && base[i + 1] <= 'z';
            if (dig != pdig) flush();                       // letters | digits
            else if (up && !pup) flush();                   // camelCase boundary
            else if (up && pup && next_low) flush();        // "APCLaser" -> APC | Laser
        }
        cur += c;
    }
    flush();
    std::vector<std::string> words;
    for (auto& t : toks) {
        std::string l = lower(t);
        bool num = std::all_of(l.begin(), l.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (num) { words.push_back(t); continue; }
        if (l == "x") { words.push_back("x"); continue; }  // "34x34"
        if (l.size() == 1) { words.push_back(std::string(1, char(std::toupper(l[0])))); continue; }  // variant letter
        auto w = name_words().find(l);
        if (w != name_words().end()) { if (!w->second.empty()) words.push_back(w->second); continue; }
        if (plain_words().count(l)) { words.push_back(l); continue; }
        return out;  // an unknown word: keep the official name only
    }
    // Sizes: "500 x 500" -> "500x500".
    std::string s;
    auto numeric = [](const std::string& w) { return !w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    for (size_t i = 0; i < words.size(); i++) {
        if (words[i] == "x" && i > 0 && i + 1 < words.size() && numeric(words[i - 1]) && numeric(words[i + 1])) { s += "x" + words[++i]; continue; }
        if (words[i] == "x") words[i] = i + 1 < words.size() && lower(words[i + 1]) == "large" ? "extra" : "X";  // "TreeXLarge", "XLg": extra large
        if (i > 0 && !numeric(words[i]) && lower(words[i]) == lower(words[i - 1])) continue;  // "HES_HesperusArena" (not "Water_250_250")
        if (!s.empty()) s += ' ';
        s += words[i];
    }
    if (s.empty()) return out;
    s[0] = char(std::toupper(s[0]));
    if (lower(s) != lower(raw)) out = s;
    return out;
}

// An effect's name for the user: "Expl_BD_LG_01 - (Splash Damage)" -> "Building collapse explosion
// large 01, hurts what is near". The designers' notes after the name ("- (Splash Damage)", "NOT FOR GENERAL USE",
// "-- TOPPLE ONLY") become a short plain note; "BD" in an effect is building destruction (category
// Effects/BuildingDestruction), not the "building base" it means in object names. "" when not confident.
static std::string effect_plain_name(const std::string& raw) {
    std::string lr = lower(raw);
    const bool splash = lr.find("(splash damage)") != std::string::npos && lr.find("no effects") == std::string::npos;
    const bool special = lr.find("not for") != std::string::npos || lr.find("only") != std::string::npos;
    // Cut only at the notes: readable names keep their own dashes ("Set - Arena Dais Shield", "Set Effect - Smoke 3_1").
    size_t cut = raw.size();
    for (const char* sep : {" - (", "- (", " -- ", " -  ", " (", " SET ONLY"}) cut = std::min(cut, raw.find(sep));
    if (special) cut = std::min(cut, raw.find(" - "));  // "BD_Debris_Hole_01 - NOT FOR GENERAL USE!"
    std::string base = raw.substr(0, cut);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    if (base.find('_') == std::string::npos && !splash && !special && !istarts_with(base, "BD ")) return "";  // already plain words
    if (istarts_with(base, "TEMP ")) base = base.substr(5);
    // Spaces as underscores so the code-style namer reads "BD HES OilFire 01" too; effect-only words first.
    std::string code;
    size_t i = 0;
    while (i <= base.size()) {
        size_t j = base.find_first_of("_ ", i);
        if (j == std::string::npos) j = base.size();
        std::string tok = base.substr(i, j - i);
        if (iequals(tok, "BD")) tok = "BuildingCollapse";
        else if (iequals(tok, "Pep")) tok = "Person";
        if (!tok.empty()) code += (code.empty() ? "" : "_") + tok;
        i = j + 1;
    }
    if (code.find('_') == std::string::npos) code += "_";  // one word: still let the namer read it
    std::string f = friendly_name(code);
    if (f.empty()) {
        if (base.find('_') != std::string::npos) return "";  // an unknown code word: keep the official name only
        f = base;  // already words ("Gauss L1 Fire")
    }
    // "Explosion gas large" -> "Gas explosion large": the kind after what explodes, before the size.
    if (istarts_with(f, "Explosion ")) {
        std::string rest = f.substr(10);
        size_t at = rest.size();
        for (const char* sz : {" large", " medium", " small", " tiny"}) at = std::min(at, rest.find(sz));
        for (size_t k = 0; k < rest.size(); k++)
            if (rest[k] == ' ' && k + 1 < rest.size() && rest[k + 1] >= '0' && rest[k + 1] <= '9') { at = std::min(at, k); break; }
        f = rest.substr(0, at) + " explosion" + rest.substr(at);
        f[0] = char(std::toupper((unsigned char)f[0]));
    }
    if (splash) f += ", hurts what is near";
    if (special) f += " (special use)";
    return lower(f) == lr ? "" : f;
}

// A water type's name for the user: the plain name, and where two waters share it ("Swamp water" for both
// Water_THE tiles), a short word for how each looks (shiny = reflects the sky, else its deep colour) so the tiles differ.
std::string App::water_type_name(const TemplateInfo& t) const {
    if (water_names_.empty()) {
        App& self = const_cast<App&>(*this);
        auto look = [&](const TemplateInfo& w) -> std::string {
            const WaterMaterial& m = self.models_.water_material(w.model);
            if (!m.ok) return "plain";
            if (m.refl) return "shiny";  // reflects the sky
            int r = m.deep & 255, g = (m.deep >> 8) & 255, b = (m.deep >> 16) & 255, hi = std::max({r, g, b});
            if (hi < 50) return "dark";
            if (b >= g && b >= r) return "blue";
            if (g >= r && g >= b) return r > b ? "olive" : "green";
            return "brown";
        };
        std::map<std::string, std::vector<const TemplateInfo*>> by;
        std::set<std::string> models;
        for (auto& w : cat_->all())
            if (w.cls_name == "MAWater" && !w.model.empty() && models.insert(lower(w.model)).second) by[water_plain_name(w)].push_back(&w);
        for (auto& [plain, list] : by) {
            std::map<std::string, int> used;
            for (const TemplateInfo* w : list) {
                // Short enough for the palette's one-line labels: "Swamp (shiny)", "Swamp (blue)".
                std::string base = plain;
                if (base.size() > 6 && base.compare(base.size() - 6, 6, " water") == 0) base.resize(base.size() - 6);
                std::string nm = plain;
                if (list.size() > 1)  // "Oily pool (Hesperus)" -> "Oily pool (Hesperus, olive)"
                    nm = !base.empty() && base.back() == ')' ? base.substr(0, base.size() - 1) + ", " + look(*w) + ")" : base + " (" + look(*w) + ")";
                if (int k = ++used[nm]; k > 1) nm += " " + std::to_string(k);
                water_names_[lower(w->model)] = nm;
            }
        }
    }
    auto it = water_names_.find(lower(t.model));
    return it != water_names_.end() ? it->second : water_plain_name(t);
}

// Object list rows and inspector titles: the friendly form of a code-style name ("Env_THE_MossTree_XLg"),
// else the name as it is. The official name stays in the tooltip and in search.
std::string strip_copy_counter(const std::string& n) {
    if (n.size() <= 6 || n[n.size() - 6] != '_') return n;
    for (size_t i = n.size() - 5; i < n.size(); i++) if (n[i] < '0' || n[i] > '9') return n;
    return n.substr(0, n.size() - 6);
}

std::string plain_object_name(const std::string& raw0, bool effect) {
    const std::string raw = strip_copy_counter(raw0);
    std::string f = effect ? effect_plain_name(raw) : friendly_name(raw);
    return f.empty() ? raw : f;
}

// Pickups (UX G-c): say what each one does. "Armor (15T)" restores armour (PowerupArmor 786 .. 2000 for
// 15T .. 100T, dictionary MAPowerUp); the three weapon power-ups set the weapon fire class.
static std::string pickup_plain_name(const std::string& n) {
    if (istarts_with(n, "Armor (")) return "Armor repair " + n.substr(6);
    std::string base = n, tail;
    if (size_t p = n.find(" - Small variant"); p != std::string::npos) { base = n.substr(0, p); tail = " (small)"; }
    for (const char* w : {"Energy Weapon", "Ballistic Weapon", "Missile Weapon"})
        if (base == w) return std::string(w, strchr(w, ' ')) + " weapon upgrade" + tail;
    if (istarts_with(n, "Data Core")) return "Data core (mission objective)";
    return "";
}

// Pickups group order: weapon upgrades first, then the bomb, armour repairs by size, the data core last
// (A-Z put fourteen "Armor (nT)" rows ahead of every weapon, G-c). All power-ups sort together under "pickup".
static std::string palette_sort_key(const TemplateInfo& t, const std::string& shown) {
    std::string n = lower(shown);
    if (t.cls_name != "MAPowerUp") return n;
    int rank = 3, tons = 0;
    if (n.find("weapon") != std::string::npos) rank = 0;
    else if (n.find("bomb") != std::string::npos) rank = 1;
    else if (n.rfind("armor", 0) == 0) { rank = 2; tons = atoi(t.name.c_str() + std::min<size_t>(7, t.name.size())); }
    char k[32];
    snprintf(k, sizeof k, "pickup %d %04d ", rank, tons);
    return k + n;
}

// Palette label: "Friendly (Official)" when the identity is clear, else the official name. A few retail
// templates share their name with another (two "Air Deployed Laser Turret CLOSED", two "Barricade security"):
// the later one (by uid) reads "... - variant 2" so the two rows differ; the uid is on the row tooltip.
std::string App::palette_name(const TemplateInfo& t) const {
    std::string n = palette_name_base(t);
    if (auto md = measured_dims_.find(t.uid); md != measured_dims_.end()) {  // The size in the friendly part follows the model
        const std::string off = " (" + t.name + ")";
        const bool has_off = n.size() > off.size() && n.compare(n.size() - off.size(), off.size(), off) == 0;
        const std::string head = has_off ? n.substr(0, n.size() - off.size()) : n;
        n = fix_size_token(head, md->second.first, md->second.second) + (has_off ? off : std::string());
    }
    if (dup_variant_.empty() && cat_) {
        std::map<std::string, int> seen;
        std::vector<const TemplateInfo*> all;
        for (auto& o : cat_->all()) if (o.placeable && o.cls_name != "MAWater") all.push_back(&o);
        std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->uid < b->uid; });
        for (auto* o : all) dup_variant_[o->uid] = ++seen[palette_name_base(*o)];
    }
    auto it = dup_variant_.find(t.uid);
    if (it == dup_variant_.end() || it->second < 2) return n;
    std::string v = " - variant " + std::to_string(it->second), off = " (" + t.name + ")";
    if (n.size() > off.size() && n.compare(n.size() - off.size(), off.size(), off) == 0) return n.insert(n.size() - off.size(), v);
    return n + v;
}

std::string App::palette_name_base(const TemplateInfo& t) const {
    if (t.cls_name == "MAWater") return water_type_name(t);
    if (t.cls_name == "MAPowerUp")
        if (std::string pk = pickup_plain_name(t.name); !pk.empty()) {
            // The four same-name copies (116281-116284) are what a Cargo Collection Area hands out: no pickup
            // or drop sound, mass 0. Said on the row so the two 'Energy weapon upgrade' rows are told apart.
            if (const WObj* o = cat_ ? cat_->object(t.uid) : nullptr)
                if (const WObj* p = cat_->object(o->u32("parent")); p && p->cls == key_of("MACollectionArea")) pk += " - cargo area copy, silent";
            return pk + " (" + t.name + ")";
        }
    // "PlayerTurret - Laser": a turret anyone can climb into, on any side ("Player" read as "the player's side").
    if (istarts_with(t.name, "PlayerTurret - ")) return t.name.substr(15) + " turret (climb in) (" + t.name + ")";
    if (t.name.size() > 9 && t.name.compare(t.name.size() - 9, 9, " - Player") == 0) {  // "VTOL - Player", "Kurita - Player"
        const std::string head = t.name.substr(0, t.name.size() - 9);
        std::string hf = friendly_name(head);
        if (hf.empty()) hf = plain_fallback(head);
        return hf + " (climb in) (" + t.name + ")";
    }
    std::string f = t.cls_name == "MAEffect" ? effect_plain_name(t.name) : friendly_name(t.name);
    if (!f.empty()) return f + " (" + t.name + ")";
    // An unreadable code name still shows words, not "Canyon_Rd_Str" (the game name stays in the tooltip).
    if (t.name.find('_') != std::string::npos) {
        std::string pf = plain_fallback(t.name);
        for (auto [from, to] : {std::pair<const char*, const char*>{"(Team 1)", "(Red team)"}, {"(Team 2)", "(Blue team)"}})
            if (size_t p = pf.find(from); p != std::string::npos) pf.replace(p, strlen(from), to);
        return pf + " (" + t.name + ")";
    }
    // One side glossary: the retail "(Team 1)" / "(Team 2)" are the Red and Blue teams.
    std::string n = t.name;
    for (auto [from, to] : {std::pair<const char*, const char*>{"(Team 1)", "(Red team)"}, {"(Team 2)", "(Blue team)"}})
        if (size_t p = n.find(from); p != std::string::npos) n.replace(p, strlen(from), to);
    return n;
}

// The palette name without its "(Official)" suffix: "Military Rommel Mk 2", not "... (VMil_RommelMK2 (Medium Tank))".
// For the main view (placing hints, inspector titles, new objects' names); the official name stays in tooltips.
std::string App::palette_plain_name(const TemplateInfo& t) const {
    std::string n = palette_name(t);
    if (std::string off = " (" + t.name + ")"; n.size() > off.size() && n.compare(n.size() - off.size(), off.size(), off) == 0) n.resize(n.size() - off.size());
    return n;
}

// Cross-cutting groups (playtest, 2026-10-21): one category for foliage and natural objects, one for
// city and man-made objects. Matched on the template's category path and name; an object keeps its
// own category too, so nothing moves out of sight.
static bool any_of_words(const std::string& s, std::initializer_list<const char*> ws) {
    for (const char* w : ws) if (s.find(w) != std::string::npos) return true;
    return false;
}
// "Streetlight" contains "tree": a lower-case name with "street" blanked, for the word tests.
static std::string no_street(std::string n) {
    for (size_t p; (p = n.find("street")) != std::string::npos;) n.replace(p, 6, "strt");
    return n;
}
bool in_group(const TemplateInfo& t, const std::string& g) {
    std::string c = lower(t.category), n = no_street(lower(t.name));
    bool building = c.rfind("buildings", 0) == 0 || c.find("/buildings") != std::string::npos;
    bool natural = any_of_words(c, {"trees", "bushes", "plants", "rocks", "/ice", "foliage", "fire/fade"}) ||
                   (!building && any_of_words(n, {"tree", "bush", "grass", "rock", "mangrove", "foliage", "conifer", "glacier", "crystal",
                                                  "swampmix", "weed", "tuft", "cliff"}));
    bool living = t.cls_name == "MAMech" || t.cls_name == "MATank" || t.cls_name == "MAVehicle" || t.cls_name == "MAFlyer" ||
                  t.cls_name == "MAPerson" || t.cls_name == "MATurret" || t.cls_name == "MAPowerUp" || t.cls_name == "MASky2" ||
                  t.cls_name == "MAWater";
    if (t.cls_name == "MAWater") return g == kGroupNatural;  // lakes and ponds sit with the natural things
    if (g == kGroupNatural) return natural && !living;
    if (g == kGroupCity)
        return !natural && !living &&
               any_of_words(c, {"buildings", "banners", "signs", "parkway", "industrial", "walls", "docks and bridges", "building base",
                                "debris", "statues", "military", "tunnel", "canyon run", "environmental"}) ;
    return false;
}

// Developer check (--dump-palette FILE): every placeable entry with its shown name, category,
// groups and cut verdict, tab-separated.
void App::dump_palette(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f || !doc_) { if (f) fclose(f); return; }
    fprintf(f, "uid\tofficial\tshown\tcategory\tgroup\tcut\n");
    for (auto& t : cat_->all()) {
        if (!t.placeable) continue;
        bool cut = !t.model.empty() && io_->model_absent(*doc_, t.uid);
        const char* g = in_group(t, kGroupNatural) ? kGroupNatural : in_group(t, kGroupCity) ? kGroupCity : "";
        fprintf(f, "%u\t%s\t%s\t%s\t%s\t%d\n", t.uid, t.name.c_str(), palette_plain_name(t).c_str(), palette_category(t).c_str(), g, cut ? 1 : 0);
    }
    fclose(f);
}

std::string App::palette_category(const TemplateInfo& t) const {
    std::string n = lower(t.name);
    if (n.find("set only") != std::string::npos || n.find("nearclip") != std::string::npos) return kCatDecor;
    auto it = palette_kind_.find(t.uid);
    const std::string v = it == palette_kind_.end() ? std::string() : it->second;
    if (v == "cinematic_prop" || t.category.find("Cinema Versions") != std::string::npos) return kCatCutscene;
    if (v == "destroyed_husk") return kCatWrecks;
    if (t.cls_name == "MASky2") return kCatSkies;
    return t.category.substr(0, t.category.find('/'));
}

const char* App::palette_note(const TemplateInfo& t) const {
    auto it = palette_kind_.find(t.uid);
    if (it != palette_kind_.end() && it->second == "ai_only_actor" && t.cls_name != "MAPerson") return "AI only";
    return nullptr;
}

static bool palette_civilian(const TemplateInfo& t) {
    std::string n = lower(t.name) + " " + lower(t.model);
    for (const char* w : {"pep_civ", "civmale", "civfemale", "civilian"})
        if (n.find(w) != std::string::npos) return true;
    return false;
}
int palette_group_of(const TemplateInfo& t, bool special) {
    const std::string& cls = t.cls_name;
    if (special) return PG_SPECIAL;
    if (cls == "MAMech") return PG_MECHS;
    if (istarts_with(t.name, "PlayerTurret")) return PG_TURRETS;  // MATank class (the player climbs in), but a gun fixed to the ground
    if (cls == "MATank" || cls == "MAVehicle" || cls == "MAFlyer") return PG_VEHICLES;
    if (cls == "MAPerson") return palette_civilian(t) ? PG_CIVILIANS : PG_SOLDIERS;
    if (cls == "MATurret") return PG_TURRETS;
    if (cls == "MABuilding") return PG_BUILDINGS;
    if (cls == "MAPowerUp" || cls == "MACrateGenerator") return PG_PICKUPS;
    if (cls == "MAEffect") return PG_EFFECTS;
    if (cls == "MAWater") return PG_WATER;
    return PG_SCENERY;
}
// Is t listed under button g: its home group, or one of the view groups it also fits.
bool App::palette_in_group(const TemplateInfo& t, bool special, int g) const {
    if (g == PG_ALL) return true;
    if (palette_group_of(t, special) == g) return true;
    if (special) return false;
    switch (g) {
        case PG_DEFENSES: return palette_group_has(t, "Defensive structures") || istarts_with(t.name, "PlayerTurret");
        case PG_LIGHTS: return palette_group_has(t, "Lights & lamps");
        case PG_SIGNS: return palette_group_has(t, "Signs & banners");
        case PG_BRIDGES: {
            const std::string& c = t.cls_name;
            if (c == "MAPerson" || c == "MATank" || c == "MAMech" || c == "MAFlyer" || c == "MAVehicle" || c == "MATurret" || c == "MAEffect") return false;
            if (lower(t.category).find("docks and bridges") != std::string::npos) return true;
            std::string n = lower(t.name) + " " + lower(t.model);
            return n.find("bridge") != std::string::npos;
        }
        default: return false;
    }
}

// Where the players start, on the tab where the sides are placed (AoE "players" idea): one button per
// side places a start for it, with the count per side and the game types the map is ticked for.
// Spawn rings, pads and game-mode objects stay on the Units and Props tab.
void App::player_starts_ui(Scenario& s) {
    if (!s.multiplayer) return;  // a mission's one start is in the 'Player start' fold at the top of the tab
    ImGui::SeparatorText("Player starts");
    int counts[3] = {0, 0, 0};
    for (auto& o : s.world.objs)
        if (o.cls == K_LEVELSTART) { int t = o.i32("TeamIndex"); if (t >= 0 && t <= 2) counts[t]++; }
    bool types = false;
    for (auto& g : game_types())
        for (auto& t : s.types) types |= iequals(t, g.ini);
    static const char* names[3] = {"Free-for-all", "Red", "Blue"};
    static const ImVec4 cols[3] = {ImVec4(0.85f, 0.85f, 0.85f, 1), ImVec4(1, 0.4f, 0.4f, 1), ImVec4(0.4f, 0.6f, 1, 1)};
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    for (int t : {1, 2, 0}) {
        ImGui::PushID(t);
        bool on = tool_ == Tool::PlaceSpawn && spawn_team_ == t;
        // Armed: the button wears its side's colour (side text on the accent blue could not be read).
        const ImVec4& c = cols[t];
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : c);
        // Armed also gets the toggle-on halo frame every other chosen button has; the fill stays the side's colour.
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x * 0.6f, c.y * 0.6f, c.z * 0.6f, 1));
            ImGui::PushStyleColor(ImGuiCol_Border, theme::tok().glow_halo1);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        }
        char label[48];
        std::snprintf(label, sizeof label, "%s  %d###start", names[t], counts[t]);  // stable id while the count changes
        if (ImGui::Button(label, ImVec2(bw, 0))) {
            spawn_team_ = t;
            tool_ = on ? Tool::Select : Tool::PlaceSpawn;
        }
        if (on) { ImGui::PopStyleVar(); ImGui::PopStyleColor(2); }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Click the map to place a %s start (a spawn point). Q / E turn it first. The number is how many this map has.", t ? names[t] : "free-for-all");
        if (t != 0) ImGui::SameLine();
        ImGui::PopID();
    }
    hint_wrapped("The game needs 6 per side; real maps have about 12 a side, spread over each team's half. "
                 "\"Add 6 on a ring\", vehicle pads and game-mode objects are under Starts at the top of this tab.");
    // Each ticked game type: ready, or what it still needs (the Check scenario errors for that type: spawns, flags,
    // bases, power plants). Checked twice a second, not every frame.
    static std::vector<ValidationItem> checks;
    static double checked_at = -1;
    // validate() is not cheap on a big map: run it when something changed (the undo stack, the object count or another
    // document), at most twice a second, plus a slow refresh for edits that push no undo step.
    static const void* checked_doc = nullptr;
    static size_t checked_undo = 0, checked_redo = 0, checked_objs = 0;
    const double now_t = ImGui::GetTime();
    const bool changed = checked_doc != doc_.get() || checked_undo != undo_.size() || checked_redo != redo_.size() ||
                         checked_objs != s.world.objs.size();
    if ((changed && now_t - checked_at > 0.5) || now_t - checked_at > 4.0 || now_t < checked_at) {
        checks = io_->validate(s);
        checked_at = now_t;
        checked_doc = doc_.get();
        checked_undo = undo_.size();
        checked_redo = redo_.size();
        checked_objs = s.world.objs.size();
    }
    if (ImGui::SmallButton("Change game types...")) tab_ = Tab::Map;
    tip("Tick the game types this map supports on the Map tab (one checkbox per type).");
    if (!types) { ImGui::SameLine(); ImGui::TextUnformatted("None ticked yet."); }
    for (auto& g : game_types()) {
        bool on = false;
        for (auto& t : s.types) on |= iequals(t, g.ini);
        if (!on) continue;
        std::string why;
        for (auto& c : checks)
            if (c.level == 2 && c.text.rfind(g.label, 0) == 0) why += (why.empty() ? "" : "\n") + c.text;
        ImGui::PushID(g.ini);
        ImGui::TextColored(why.empty() ? theme::tok().ok : theme::tok().warn, "%s  %s", why.empty() ? "Ready:" : "Needs work:", g.label);
        if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            ImGui::TextUnformatted(why.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        } else if (why.empty()) {
            tip(game_type_tip(g.ini));
        }
        ImGui::PopID();
    }
}


// The palette browser (header, body, brush set row) is in app_palette.cpp.
// Every placeable template A-Z by shown name ('Mechs by chassis); waters in the one water-type order (resizable first).
const std::vector<const TemplateInfo*>& App::palette_sorted() {
    static std::vector<const TemplateInfo*> sorted;
    static size_t sorted_for = 0;
    static const void* sorted_cat = nullptr;  // load_game replaces cat_ (same size after Choose.../Automatic): the list points into the old one
    if (sorted_for != cat_->all().size() || sorted_cat != cat_.get()) {
        sorted_for = cat_->all().size();
        sorted_cat = cat_.get();
        sorted.clear();
        for (auto& t : cat_->all()) sorted.push_back(&t);
        std::stable_sort(sorted.begin(), sorted.end(), [&](const TemplateInfo* a, const TemplateInfo* b) {
            return palette_sort_key(*a, palette_name(*a)) < palette_sort_key(*b, palette_name(*b));
        });
        const auto& wt = water_types();
        size_t k = 0;
        for (auto& tp : sorted)
            if (tp->cls_name == "MAWater" && k < wt.size()) tp = wt[k++];
            else if (tp->cls_name == "MAWater") tp = nullptr;  // a second template of a listed water model
        sorted.erase(std::remove(sorted.begin(), sorted.end(), nullptr), sorted.end());
    }
    return sorted;
}

void App::palette_footer(BrowserMode m) {
    Scenario& s = *doc_;
    if (m == BrowserMode::Scatter) return;
    if (m == BrowserMode::Place) {
    // AoE "Players" picker: the side every unit placed from here belongs to, in the side's map colour.
    auto side_radio = [](const char* label, int* v, int val, ImU32 col) {
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::RadioButton(label, v, val);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    // A wall or barricade picked from Defenses has no team (only the turrets and soldiers of that group do).
    const TemplateInfo* picked_t = tool_ == Tool::PlaceObject && place_road_.empty() && cat_ ? cat_->find(place_template_) : nullptr;
    const bool pick_no_team = picked_t && picked_t->cls_name != "MATurret" && picked_t->cls_name != "MAPerson";
    ImGui::TextUnformatted("Placing for:");
    ImGui::SameLine();
    if (!s.multiplayer && (unit_group_ == PG_BUILDINGS || unit_group_ == PG_BRIDGES || unit_group_ == PG_ROADS || unit_group_ == PG_WATER ||
                           unit_group_ == PG_LIGHTS || unit_group_ == PG_SIGNS || unit_group_ == PG_SCENERY || unit_group_ == PG_EFFECTS)) {
        // Review: the side row showed (Enemy ticked) for buildings and scenery, which are always placed neutral.
        ImGui::TextDisabled("neutral (change it in Properties)");
    } else if (!s.multiplayer) {
        side_radio("Enemy##side_place", &place_side_, 5, IM_COL32(255, 90, 90, 255));
        side_radio("Player's side##side_place", &place_side_, 6, IM_COL32(110, 160, 255, 255));
        side_radio("Neutral##side_place", &place_side_, 0, IM_COL32(200, 200, 200, 255));
        help("The side of the units you place next (the ring under each unit shows its side). Enemies attack the player (give them orders on the Objectives & Triggers tab). Units on the player's side fight with the player. Neutral 'Mechs and vehicles stand empty, and the player can climb in. Change a placed unit's side in its Properties.");
    } else if (pick_no_team || (unit_group_ != PG_TURRETS && unit_group_ != PG_DEFENSES && unit_group_ != PG_SOLDIERS && unit_group_ != PG_CIVILIANS &&
               unit_group_ != PG_SPECIAL && unit_group_ != PG_ALL)) {
        // Only turrets and soldiers take a team on a multiplayer map (G-b): power-ups, roads, scenery and empty 'Mechs have none.
        ImGui::TextDisabled("no team (only turrets and soldiers have one)");
    } else {
        side_radio("Red team##side_place", &place_team_, 1, IM_COL32(255, 90, 90, 255));
        side_radio("Blue team##side_place", &place_team_, 2, IM_COL32(110, 160, 255, 255));
        side_radio("Neutral##side_place", &place_team_, 0, IM_COL32(200, 200, 200, 255));
        help("The team of the turrets and soldiers you place next: they fight for that team (the ring under each one shows it); Neutral = no team. 'Mechs and vehicles always stand empty on multiplayer maps, for any player to climb in. Change a placed turret's team in its Properties.");
    }
    }
    if (m == BrowserMode::RoadsWalls) hint_text("Select a placed road, wall, fence, bridge or base and drag its orange handles to cut it to size.");
    if (tool_ != Tool::PlaceObject) ImGui::TextDisabled("Click an object above, or drag it onto the map.");
    if (tool_ == Tool::PlaceObject) {
        const TemplateInfo* t = place_road_.empty() ? cat_->find(place_template_) : nullptr;
        const RoadPiece* rp = place_road_.empty() ? nullptr : find_road_piece(place_road_);
        if (t && !settings_.pal_pictures) if (ID3D11ShaderResourceView* th = palette_picture(*t)) ImGui::Image((ImTextureID)(intptr_t)th, ImVec2(96 * dpi_, 96 * dpi_));
        if (rp && !settings_.pal_pictures) if (ID3D11ShaderResourceView* th = models_.thumbnail(rp->model, "")) ImGui::Image((ImTextureID)(intptr_t)th, ImVec2(96 * dpi_, 96 * dpi_));
        ImGui::TextWrapped("Placing: %s. Click the map; Q / E turn it (%.0f deg); right-click stops.",
                           t ? palette_plain_name(*t).c_str() : rp ? ("road piece " + rp->friendly).c_str() : "?",
                           heading_display(std::round(place_heading_ * 57.2958f)));
        {   // The typed / fine heading (degrees; the steps apply to keys, drags and picks, not to what is typed)
            float deg = heading_display(place_heading_ * 57.2957795f);
            if (deg >= 359.5f) deg = 0.f;
            form_label("Facing", form_col());
            if (ImGui::DragFloat("##placefacing", &deg, 0.25f, 0.f, 0.f, "%.0f deg")) place_heading_ = std::remainder(deg, 360.f) * 0.01745329252f;
            ImGui::SetItemTooltip("Drag, or double-click to type the heading in degrees. Q / E turn it in steps.");
        }
        // (Joints, grid and angle steps are in the bar over the map: one copy only,.)
        slope_placing_row();  // a road piece or base: what it does on a slope
        {
            const std::string t = std::string("Joints line things up the way the real bases are laid out; the grid and the angle steps are off or on in the bar above the map. Hold ") + mod_name(settings_.hold_free) +
                                  " to place one freely (no joints, no grid, no angle steps). Settings > Keys changes these keys.";
            tip(t.c_str());
        }
        if (t && !t->model.empty() && io_->model_absent(s, t->uid)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(theme::tok().warn, "Its 3D model was cut from the game: it shows as a pink placeholder there.");
            ImGui::PopTextWrapPos();
            if (const TemplateInfo* alt = cat_->find(io_->similar_template(s, t->uid))) {
                if (ImGui::Button(("Use \"" + alt->name + "\" instead").c_str())) place_template_ = alt->uid;
                tip("The same object, with a model that is on the disc.");
            }
        }
    }
}

// One toggle for the scatter brush set: a click on a cell, its tick, or a chip's x. Arms the brush while the set is not empty.
void App::scatter_toggle(uint32_t uid, bool arm) {
    auto it = std::find(scatter_set_.begin(), scatter_set_.end(), uid);
    const TemplateInfo* ti = cat_ ? cat_->find(uid) : nullptr;
    const std::string nm = ti ? palette_plain_name(*ti) : std::string("Object");
    if (it == scatter_set_.end()) {
        scatter_set_.push_back(uid);
        status_ = nm + " added to the scatter brush (" + std::to_string(scatter_set_.size()) + (scatter_set_.size() == 1 ? " kind)." : " kinds).");
    } else {
        scatter_set_.erase(it);
        status_ = nm + " removed from the scatter brush (" + std::to_string(scatter_set_.size()) + (scatter_set_.size() == 1 ? " kind)." : " kinds).");
    }
    status_time_ = 4;
    if (!scatter_set_.empty() && editable() && arm) tool_ = Tool::ScatterTrees;   // from the Place page's menu the brush is not armed (the page stays)
    else if (scatter_set_.empty() && tool_ == Tool::ScatterTrees) tool_ = Tool::Select;
}

void App::scatter_header_ui() {
    // The brush set: what a drag on the map drops. One thumbnail chip per kind; a click on a chip takes it out.
    const size_t n = scatter_set_.size();
    {
        std::string head = n == 0 ? "Brush set (nothing picked yet)###brushset" : "Brush set (" + std::to_string(n) + (n == 1 ? " kind)###brushset" : " kinds)###brushset");
        ImGui::SeparatorText(head.c_str());
    }
    ImGui::BeginDisabled(n == 0);
    if (ImGui::SmallButton("Clear##brushset")) { scatter_set_.clear(); if (tool_ == Tool::ScatterTrees) tool_ = Tool::Select; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Trees of this theme##brushset")) {
        Scenario& s = *doc_;
        int base_idx = -1;
        for (size_t i = 0; i < gd_.level_archives().size(); i++) if (iequals(gd_.level_archives()[i], s.base)) base_idx = int(i);
        const auto& base_set = cat_->culled_set(s.base_level());
        for (auto& t : cat_->all())
            if (t.placeable && (t.category.find("Trees") != std::string::npos || no_street(lower(t.name)).find("tree") != std::string::npos) &&
                !(!t.model.empty() && io_->model_absent(s, t.uid)) &&
                (base_set.count(t.uid) || std::find(t.archives.begin(), t.archives.end(), base_idx) != t.archives.end()) &&
                std::find(scatter_set_.begin(), scatter_set_.end(), t.uid) == scatter_set_.end())
                scatter_set_.push_back(t.uid);
        if (!scatter_set_.empty() && editable()) tool_ = Tool::ScatterTrees;
    }
    ImGui::SetItemTooltip("Adds every tree this theme comes with to the brush set (the ones already in it stay).");
    if (n == 0) {
        // The empty hint takes the height of the first row of picked-kind chips, so the first pick does not push the grid down.
        const float y0 = ImGui::GetCursorPosY();
        ImGui::TextDisabled("Pick objects below: a click adds or removes one.");
        ImGui::SetCursorPosY(y0 + 40 * dpi_ + ImGui::GetStyle().FramePadding.y * 2);
        ImGui::Dummy(ImVec2(0, 0));
    } else {
        // The picked kinds as thumbnails, each removed by a click (at most two rows here; the rest are in the "+N more" list).
        const float chip = 40 * dpi_, avail = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
        float x = 0;
        int rows = 1;
        size_t shown = 0;
        uint32_t drop = 0;
        for (uint32_t u : scatter_set_) {
            const TemplateInfo* ti = cat_ ? cat_->find(u) : nullptr;
            if (!ti) continue;
            const float w = chip + ImGui::GetStyle().FramePadding.x * 2;
            if (x > 0 && x + sp + w > avail) {
                if (rows == 2) break;
                rows++;
                x = 0;
            } else if (x > 0) ImGui::SameLine();
            x += (x > 0 ? sp : 0) + w;
            const std::string id = "##set_" + std::to_string(u);
            ID3D11ShaderResourceView* th = models_.thumbnail_cached(ti->model, ti->mtb) ? palette_picture(*ti) : nullptr;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            bool hit;
            if (th) hit = ImGui::ImageButton(id.c_str(), (ImTextureID)(intptr_t)th, ImVec2(chip, chip));
            else {
                std::string nm = palette_plain_name(*ti);
                if (nm.size() > 14) nm = nm.substr(0, 13) + "..";
                hit = ImGui::Button((nm + " x" + id).c_str(), ImVec2(w + 24 * dpi_, chip + ImGui::GetStyle().FramePadding.y * 2));
            }
            if (th) {
                name_item(id);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 q(p0.x + w - 10 * dpi_, p0.y + 2 * dpi_);
                dl->AddText(q, IM_COL32(255, 255, 255, 230), "x");
            }
            if (hit) drop = u;
            ImGui::SetItemTooltip("%s: click to take it out of the brush set.", palette_plain_name(*ti).c_str());
            shown++;
        }
        if (shown < n) {
            ImGui::SameLine();
            if (ImGui::SmallButton(("+" + std::to_string(n - shown) + " more##setmore").c_str())) ImGui::OpenPopup("##setmorepop");
            if (ImGui::BeginPopup("##setmorepop")) {
                size_t k = 0;
                for (uint32_t u : scatter_set_) {
                    if (k++ < shown) continue;
                    const TemplateInfo* ti = cat_ ? cat_->find(u) : nullptr;
                    if (ti && ImGui::Selectable((palette_plain_name(*ti) + "  (click to remove)##setmore_" + std::to_string(u)).c_str())) drop = u;
                }
                ImGui::EndPopup();
            }
        }
        if (drop) {
            scatter_set_.erase(std::remove(scatter_set_.begin(), scatter_set_.end(), drop), scatter_set_.end());
            if (scatter_set_.empty() && tool_ == Tool::ScatterTrees) tool_ = Tool::Select;
        }
    }
    // The rows of the scatter brush. Count by the area a stroke covers, spacing by the objects' own size.
    // The settings sit in a "Brush options" section so the pick list below keeps its room: shut at first on a window
    // under 800 px high at the current size (1280 x 720, or 1584 x 861 at 1.25), open otherwise; after that the section stays as the user leaves it.
    bool sc_dirty = false;
    static int s_opts_user = -1;   // -1: follows the window size; 0 / 1: the user shut / opened it
    const bool opts_want = s_opts_user >= 0 ? s_opts_user != 0 : ImGui::GetMainViewport()->Size.y >= 800.f * std::max(0.5f, dpi_);
    ImGui::SetNextItemOpen(opts_want, ImGuiCond_Always);
    const bool opts_open = theme::section("Brush options##scopts");
    if (ImGui::IsItemToggledOpen()) s_opts_user = opts_open ? 1 : 0;
    if (opts_open) {
    form_label("Brush size", form_col());
    ImGui::SetNextItemWidth(-28 * dpi_);
    theme::slider_float("##brushsc", &brush_radius_, 5, 200, "%.0f m");
    tip("The size of the ring on the map. Objects have no size setting in the game, so every object keeps its own size; for grass that varies, use Paint > Plants. Keys: [ / ] or Ctrl+wheel over the map.");
    form_label("Density", form_col());
    ImGui::SetNextItemWidth(-28 * dpi_);
    if (theme::slider_float("##scdensity", &settings_.sc_density, 1.f, 400.f, "%.0f per hectare", ImGuiSliderFlags_Logarithmic)) settings_.sc_density = std::clamp(settings_.sc_density, 1.f, 400.f);
    sc_dirty |= ImGui::IsItemDeactivatedAfterEdit();
    tip("How many objects a stroke drops on the ground it covers: this many for every 100 x 100 m (a hectare), however slowly or quickly you drag. Holding still adds nothing; going over the same ground again adds nothing.");
    {
        const float area = 3.14159265f * brush_radius_ * brush_radius_;
        char b[64];
        std::snprintf(b, sizeof b, "about %.0f in the brush", settings_.sc_density * area / 10000.f);
        ImGui::TextDisabled("%s", b);
    }
    form_label("Spacing", form_col());
    ImGui::SetNextItemWidth(-28 * dpi_);
    if (theme::slider_float("##scspacing", &settings_.sc_spacing, 0.3f, 3.f, "%.1f x size")) settings_.sc_spacing = std::clamp(settings_.sc_spacing, 0.3f, 3.f);
    sc_dirty |= ImGui::IsItemDeactivatedAfterEdit();
    tip("How far apart objects stand, as a multiple of their own size: 1.0 = crowns just touch, 0.5 = overlapping, 2 = a gap as wide as the object. A spot too close to another is skipped.");
    if (ImGui::Checkbox("Random facing##sc", &settings_.sc_facing)) sc_dirty = true;
    tip("Each object is turned a random way. Off: every one faces the way the object was made.");
    form_label("Lean up to", form_col());
    ImGui::SetNextItemWidth(-28 * dpi_);
    if (theme::slider_float("##sclean", &settings_.sc_lean, 0.f, 20.f, "%.0f deg")) settings_.sc_lean = std::clamp(settings_.sc_lean, 0.f, 20.f);
    sc_dirty |= ImGui::IsItemDeactivatedAfterEdit();
    tip("Each object leans a random amount up to this many degrees (only objects the game can tilt). 0 = straight up.");
    if (ImGui::Checkbox("Follow the slope##sc", &settings_.sc_follow)) sc_dirty = true;
    tip("Objects tilt to lie along the ground they stand on (a rock on a hillside), plus the lean above. Only objects the game can tilt.");
    if (ImGui::Checkbox("Keep off roads, bases and buildings##sc", &settings_.sc_keep_off)) sc_dirty = true;
    tip("Spots on a road piece, a base or flat plate or a building are skipped. Off: objects land on them, standing on their top surface. Water never blocks: trees go on its banks and in it.");
    if (ImGui::Checkbox("Skip slopes over##sc", &settings_.sc_skip_steep)) sc_dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-28 * dpi_);
    if (theme::slider_float("##scslope", &settings_.sc_slope, 10.f, 150.f, "%.0f%%")) settings_.sc_slope = std::clamp(settings_.sc_slope, 10.f, 150.f);
    sc_dirty |= ImGui::IsItemDeactivatedAfterEdit();
    tip("Spots on ground steeper than this are skipped (the same steepness the Sculpt page's steep-ground view shows: bots' 'Mechs won't route over ground steeper than 55% (Battle Armor and pilots 80%), so 60% keeps scattered objects off all but gentle hills).");
    }
    if (sc_dirty) settings_.save();
    hint_wrapped((std::string(mod_name(settings_.hold_reverse)) + ": remove these kinds under the brush. " + mod_name(settings_.hold_free) + "+click: a straight line.").c_str());   // Wraps, never cut
    ImGui::BeginDisabled(n == 0);
    tool_button("Brush##scatter", Tool::ScatterTrees, tool_, -1, "Drag on the map to drop the picked objects at random spots under the brush. Esc stops the brush; the page stays.");
    ImGui::EndDisabled();
    // The other home for plants: say which is which.
    hint_wrapped(n == 0 ? "Scatter places objects: pick some first. Grass and bushes that grow by themselves around the player: Paint > Plants (Scatter's \"Grass\" places each clump as an object instead)."
                        : "Scatter places objects. Grass and bushes that grow by themselves around the player: Paint > Plants (Scatter's \"Grass\" places each clump as an object instead).");
}

void App::page_select() {
    Scenario& s = *doc_;
    ImGui::BeginDisabled(s.read_only);
    ImGui::SeparatorText("Selection");
    ImGui::Text("%zu selected", selection_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(selection_.empty());
    const char* nothing_sel = "Nothing is selected: click an object on the map (or drag a box around several) first.";
    if (ImGui::SmallButton("Deselect all")) selection_.clear();
    tip(("Clear the selection (also: right-click an empty spot, or " + kt(A_CANCEL) + ").").c_str());
    why_disabled(selection_.empty(), nothing_sel);
    if (ImGui::Button(act_text(A_SNAP).c_str())) snap_selection_to_ground();
    tip(("Put the selected objects back on the ground (" + kt(A_SNAP) + ").").c_str());
    why_disabled(selection_.empty(), nothing_sel);
    ImGui::SameLine();
    if (theme::danger_button("Delete")) delete_selection();
    tip(("Delete the selected objects (" + kt(A_DELETE) + "). " + kt(A_UNDO) + " undoes.").c_str());
    why_disabled(selection_.empty(), nothing_sel);
    ImGui::SameLine();
    if (ImGui::Button("Copy")) copy_selection();
    tip(("Copy the selected objects (" + kt(A_COPY) + ").").c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(clipboard_.empty());
    if (ImGui::Button("Paste")) paste({cam_.tx, 0, cam_.tz});
    tip(("Paste where the camera looks (" + kt(A_PASTE) + " puts the copy on the mouse: click the map to place it).").c_str());
    why_disabled(clipboard_.empty(), ("Nothing copied yet: select objects and press Copy (" + kt(A_COPY) + ") first.").c_str());
    ImGui::EndDisabled();
    // Side only means something for units: not for the player start, triggers, lights and props.
    bool any_side = false;
    if (!selection_.empty() && !s.multiplayer)
        for (int i : selected_roots()) {
            const WObj& so = doc_->world.objs[size_t(i)];
            if (has_side(so)) any_side = true;
        }
    if (any_side) {
        auto set_nation = [&](int n) {
            push_undo("nation");
            std::vector<uint32_t> uids;
            for (int i : selected_roots())
                if (has_side(doc_->world.objs[size_t(i)])) uids.push_back(doc_->world.objs[size_t(i)].uid());
            for (uint32_t u : uids) io_->apply_unit_side(*doc_, u, n);
            world_changed();
            s.dirty = true;
        };
        ImGui::TextUnformatted("Side:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Player's side##side")) set_nation(6);
        ImGui::SameLine();
        if (ImGui::SmallButton("Enemy##side")) set_nation(5);
        ImGui::SameLine();
        if (ImGui::SmallButton("Neutral##side")) set_nation(0);
        help("In missions, enemies attack the player and the player's side fights with them; neutral units are left alone (empty 'Mechs the player can climb into). In multiplayer maps turrets and team buildings already belong to Red or Blue.");
        // One selected unit: a direct way to its orders (they live on the Objectives & Triggers tab).
        auto roots = selected_roots();
        static const uint32_t unit_classes[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"), key_of("MATurret"),
                                                key_of("MAPerson")};
        if (roots.size() == 1) {
            const WObj& u = doc_->world.objs[size_t(roots[0])];
            if (std::find(std::begin(unit_classes), std::end(unit_classes), u.cls) != std::end(unit_classes)) {
                order_button(0, u.uid(), "Give this unit orders...",
                             "Pick what the unit does: walk a path once, patrol, defend a point, follow or attack the player, or attack a unit. It makes a trigger on the Objectives & Triggers tab that starts as the mission starts; there you can make it wait for a condition instead.", true);
            }
        }
    }
    ImGui::EndDisabled();
    page_body_begin("##selbody");
    ui_placed_list();
    page_body_end();
}

void App::page_starts() {
    Scenario& s = *doc_;
    page_body_begin("##startsbody");
    tab_players();
    player_starts_ui(s);
    page_body_end();
}

void App::ui_build_tab() {
    switch (page_) {
        case Page::Select: page_select(); break;
        case Page::Place: page_browser(BrowserMode::Place); break;
        case Page::Scatter: page_browser(BrowserMode::Scatter); break;
        case Page::RoadsWalls: page_browser(BrowserMode::RoadsWalls); break;
        case Page::Sculpt: page_sculpt(); break;
        case Page::Paint: page_paint(); break;
        case Page::Water: page_water(); break;
        case Page::Starts: page_starts(); break;
        case Page::Count: break;
    }
}


// ---------------------------------------------------------------------------------------------
// Messages

// Any PCM or float WAV (8/16/24/32-bit, any rate, any channel count) to what the game plays:
// 16-bit PCM, mono or stereo, 22050/44100/48000 Hz (linear resampling to the nearest supported
// rate at or above the source, max 48000). note says what was converted, or why it failed.
static bool convert_wav(const Bytes& in, Bytes& out, std::string* note) {
    if (in.size() < 44 || std::memcmp(in.data(), "RIFF", 4) || std::memcmp(in.data() + 8, "WAVE", 4)) { *note = "it is not a WAV file"; return false; }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    for (size_t p = 12; p + 8 <= in.size();) {
        uint32_t id = rd<uint32_t>(&in[p]), len = rd<uint32_t>(&in[p + 4]);
        size_t body = p + 8, avail = std::min<size_t>(len, in.size() - body);
        if (id == 0x20746D66u && avail >= 16) {  // "fmt "
            fmt = rd<uint16_t>(&in[body]);
            ch = rd<uint16_t>(&in[body + 2]);
            rate = rd<uint32_t>(&in[body + 4]);
            bits = rd<uint16_t>(&in[body + 14]);
            if (fmt == 0xFFFE && avail >= 26) fmt = rd<uint16_t>(&in[body + 24]);  // WAVE_FORMAT_EXTENSIBLE subformat
        } else if (id == 0x61746164u) {  // "data"
            data = &in[body];
            data_len = avail;
        }
        p = body + size_t(len) + (len & 1);
    }
    bool pcm = fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32), flt = fmt == 3 && (bits == 32 || bits == 64);
    if (!pcm && !flt) { *note = "it is compressed (not plain PCM)"; return false; }
    if (!ch || !rate || !data) { *note = "the file is damaged"; return false; }
    size_t bps = bits / 8, frames = data_len / (bps * ch);
    if (!frames) { *note = "it has no sound in it"; return false; }
    auto sample = [&](size_t f, int c) -> float {
        const uint8_t* q = data + (f * ch + size_t(c)) * bps;
        if (flt) return bits == 32 ? rd<float>(q) : float(rd<double>(q));
        switch (bits) {
            case 8: return (float(q[0]) - 128.f) / 128.f;
            case 16: return float(rd<int16_t>(q)) / 32768.f;
            case 24: return float(int32_t(uint32_t(q[0]) << 8 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 24) >> 8) / 8388608.f;
            default: return float(rd<int32_t>(q)) / 2147483648.f;
        }
    };
    int out_ch = ch >= 2 ? 2 : 1;
    uint32_t out_rate = rate <= 22050 ? 22050 : rate <= 44100 ? 44100 : 48000;
    std::vector<std::string> did;
    if (!(fmt == 1 && bits == 16)) did.push_back("made 16-bit");
    if (ch > 2) did.push_back("mixed down to stereo");
    if (out_rate != rate) did.push_back("resampled to " + std::to_string(out_rate) + " Hz");
    size_t out_frames = size_t(double(frames) * out_rate / rate);
    if (out_frames > size_t(out_rate) * 600) { *note = "it is longer than 10 minutes"; return false; }
    Bytes pcm16;
    pcm16.reserve(out_frames * size_t(out_ch) * 2);
    for (size_t i = 0; i < out_frames; i++) {
        double t = double(i) * rate / out_rate;
        size_t f0 = std::min(frames - 1, size_t(t)), f1 = std::min(frames - 1, f0 + 1);
        float w = float(t - double(f0));
        for (int c = 0; c < out_ch; c++) {
            auto at = [&](size_t f) {
                if (ch <= 2) return sample(f, std::min(c, int(ch) - 1));
                float sum = 0;  // more than two channels: even ones left, odd ones right
                int n = 0;
                for (int k = c; k < ch; k += 2) { sum += sample(f, k); n++; }
                return sum / float(std::max(1, n));
            };
            float v = at(f0) * (1 - w) + at(f1) * w;
            put<int16_t>(pcm16, int16_t(std::clamp(v, -1.f, 1.f) * 32767.f));
        }
    }
    out.clear();
    put_bytes(out, "RIFF", 4);
    put<uint32_t>(out, uint32_t(36 + pcm16.size()));
    put_bytes(out, "WAVEfmt ", 8);
    put<uint32_t>(out, 16);
    put<uint16_t>(out, 1);
    put<uint16_t>(out, uint16_t(out_ch));
    put<uint32_t>(out, out_rate);
    put<uint32_t>(out, out_rate * uint32_t(out_ch) * 2);
    put<uint16_t>(out, uint16_t(out_ch * 2));
    put<uint16_t>(out, 16);
    put_bytes(out, "data", 4);
    put<uint32_t>(out, uint32_t(pcm16.size()));
    out.insert(out.end(), pcm16.begin(), pcm16.end());
    note->clear();
    for (size_t k = 0; k < did.size(); k++) *note += (k ? ", " : "") + did[k];
    return true;
}

// One 'destroy' target: watched by the objective's trigger and marked on the radar. An empty (no pilot) vehicle counts as lost at
// once, so it gets an enemy crew, the usual target. Returns 1 when it did.
int App::add_destroy_target(Scenario& s, uint32_t trig, uint32_t unit, int objective) {
    int crewed = 0;
    if (const WObj* o = object_by_uid(s.world, unit); o && o->find("StartAbandoned") && o->i32("StartAbandoned") && io_) {
        io_->set_boardable(s, unit, false);  // may remove a child: `o` is not used after this
        io_->apply_unit_side(s, unit, 5);
        crewed = 1;
    }
    add_reference(s, trig, unit);
    add_objective_target(s, unit, objective);
    return crewed;
}

// ObjTargets pick mode: each click on a unit or building adds it to the objective; the mode stays on until right-click / Esc.
void App::pick_objective_target(float mx, float my) {
    Scenario& s = *doc_;
    int h = pick_object(mx, my);
    const WObj* o = h >= 0 ? &s.world.objs[size_t(h)] : nullptr;
    if (!o || !o->find("CorporalPos")) {
        status_ = "Click a unit or building to add it. Right-click or " + kt(A_CANCEL) + " when done.";
        status_time_ = 6;
        return;
    }
    const uint32_t unit = o->uid();
    std::string name = o->description().empty() ? "unit " + std::to_string(unit) : o->description();
    auto have = objective_targets(s.world, pick_.objective);
    if (std::find(have.begin(), have.end(), unit) != have.end()) {
        status_ = name + " is already a target. Right-click or " + kt(A_CANCEL) + " when done.";
        status_time_ = 6;
        return;
    }
    push_undo("add objective target");
    int crewed = add_destroy_target(s, pick_.uid, unit, pick_.objective);
    // The text follows the picks ("Destroy Catapult and 1 more", as the template with a selection) until the user edits it.
    auto label_of = [&](uint32_t u) {
        const WObj* x = object_by_uid(s.world, u);
        return x && !x->description().empty() ? x->description() : "unit " + std::to_string(u);
    };
    auto auto_text = [&](const std::vector<uint32_t>& t) {
        return t.empty() ? std::string("Destroy the marked targets")
                         : "Destroy " + label_of(t[0]) + (t.size() > 1 ? " and " + std::to_string(t.size() - 1) + " more" : "");
    };
    if (pick_.objective >= 0 && pick_.objective < int(s.objectives.size()) && s.objectives[size_t(pick_.objective)] == auto_text(have)) {
        auto now = have;
        now.push_back(unit);
        s.objectives[size_t(pick_.objective)] = auto_text(now);
    }
    selection_.insert(unit);
    world_changed();
    s.dirty = true;
    status_ = "Added " + name + (crewed ? " (it now has an enemy crew)" : "") + ": " + std::to_string(have.size() + 1) +
              " target(s). Click more, right-click or " + kt(A_CANCEL) + " when done.";
    status_time_ = 8;
}

void App::tab_messages() {
    Scenario& s = *doc_;
    ImGui::BeginDisabled(s.read_only);
    if (!s.multiplayer) {
        if (theme::section("Briefing", ImGuiTreeNodeFlags_DefaultOpen)) {  // body kept at this indent (merges)
        form_label("Title", form_col());
        if (ImGui::InputTextWithHint("##btitle", "e.g. Operation Iron Gate", &s.briefing_title)) push_undo_once("briefing title");
        tip("The heading of the briefing, shown above its text. Leave empty for none.");
        form_label("Briefing text", form_col());
        if (ImGui::InputTextMultiline("##brief", &s.briefing, ImVec2(-FLT_MIN, 160 * dpi_), ImGuiInputTextFlags_WordWrap)) push_undo_once("briefing");
        tip("What the player is told when the mission starts. Leave empty for none. Long text is shown in several short pages, one after the other.");
        hint_wrapped("The player reads this as short subtitles when the mission starts (after the intro cutscene, if one plays at the start); a timed message that "
                     "shows while it is up replaces it. The Custom Scenarios list shows the Map tab's Description, or this briefing when that is empty.");
        }
        if (theme::section("Objectives", ImGuiTreeNodeFlags_DefaultOpen)) {  // body kept at this indent (merges)
        ImGui::TextWrapped("The goals for the player, in order. Each one is ticked off by a trigger with the action Complete objective. Targets get a marker on the player's radar.");
        // The game keeps one current objective (HUD line, pause menu, radar markers) and moves to the next open one when it is done.
        hint_wrapped("Like the campaign, the game shows one objective at a time: the first one not yet done, in a banner when the mission starts and in the pause menu. When it is done the next one appears; one finished early is ticked off quietly and skipped.");
        if (s.objectives.size() > 3) {  // a retail copy lists 10-15: the button at the end is screens away
            ImGui::BeginDisabled(s.objectives.size() >= 30);
            if (ImGui::Button("Add objective...##top", ImVec2(-1, 0))) ImGui::OpenPopup("addobj");
            ImGui::EndDisabled();
            tip("Adds a goal after the last one in this list, so the game shows it once the ones above are done.");
        }
        const auto& names = gd_.names();
        auto is_trig = [&](uint32_t cls) { return names.label(cls).rfind("MATrigger", 0) == 0; };
        auto unit_name = [&](uint32_t uid) {
            const WObj* o = object_by_uid(s.world, uid);
            std::string d = o ? o->description() : std::string();
            return d.empty() ? "unit " + std::to_string(uid) : d;
        };
        std::vector<uint32_t> picked;  // selected units/buildings (not trigger areas)
        for (uint32_t x : selection_) if (const WObj* o = object_by_uid(s.world, x)) if (!is_trigger_shape(o->cls) && !is_trig(o->cls)) picked.push_back(x);
        auto flow = [&](const std::string& label) {  // next small button on this line, or the next line when it would not fit
            float w = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < w) { ImGui::NewLine(); ImGui::SetCursorPosX(form_col()); }
        };
        auto row_label = [&](const char* label) {  // a dim label in the form's label column, the row's buttons after it
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine(form_col());
        };
        // "Reach a place" for objective i (the Add objective menu and the Completed by chooser): a circle that completes it, its radar
        // marker, then the click on the map that places the circle.
        auto make_reach = [&](int i) {
            const std::string num = std::to_string(i + 1);
            TriggerShape sh;
            sh.box = false;
            sh.x = cam_.tx;
            sh.z = cam_.tz;
            sh.name = "Objective " + num + ": reach";
            uint32_t u = add_trigger_shape(s, sh);
            if (!u) return;
            if (uint32_t a = add_action_object(s, u, "MAActionMissionVariableSet", "Complete objective " + num))
                if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("MissionVariableSet", claim_objective_var(s, i));
            // Its place on the player's radar / HUD: retail 'go here' objectives mark an invisible NULL marker at
            // the spot with the objective's MAObjectiveType (Mission_03 'Dummy - cp 1 objective'); a trigger alone shows nothing.
            uint32_t m = add_ai_marker(s, sh.x, sh.z, sh.name + kReachMarkerSuffix, true);
            if (WObj* mo = m ? object_by_uid(s.world, m) : nullptr)
                if (mo->find("IsCollidable")) mo->set_i32("IsCollidable", 0);  // as retail's markers: nothing bumps into it
            if (m) add_objective_target(s, m, i);
            pick_ = PickTarget();
            pick_.kind = PickTarget::MoveShape;
            pick_.uid = u;
            pick_.follow = m;
            pick_.back = Tab::Messages;
            tool_ = Tool::PickPoint;
        };
        int rm_obj = -1;
        uint32_t open_trig = 0;
        for (int i = 0; i < int(s.objectives.size()) && i < 30; i++) {
            ImGui::PushID(i + 200000);
            // The title row carries Del, so the text box gets the full panel width (it was ~140 px beside the label column).
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(("Objective " + std::to_string(i + 1)).c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Del").x - ImGui::GetStyle().FramePadding.x * 2);
            if (ImGui::SmallButton("Del")) rm_obj = i;
            tip("Delete this objective (asks first).");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##o", "What the player must do", &s.objectives[size_t(i)])) push_undo_once("objective text");
            if (ObjectiveSlot sl = objective_slot(s, i); sl.hidden)
                hint_wrapped("A hidden step: triggers complete it, the player never sees it.");
            else if (sl.game_text)
                hint_wrapped("The game shows the level's own words for this line; the box holds only the level's inside name for it. Type to replace them.");
            // How it is completed: the triggers whose "Complete objective" action names it.
            auto done_by = objective_completers(s.world, objective_var(s, i), is_trig);
            auto lost_by = objective_losers(s.world, i);
            if (done_by.empty() && !lost_by.empty()) {
                row_label("Lost if destroyed");
                for (size_t k = 0; k < lost_by.size(); k++) {
                    const WObj* t = object_by_uid(s.world, lost_by[k]);
                    std::string n = (t && !t->description().empty() ? t->description() : "trigger") + "##l" + std::to_string(k);
                    if (k) flow(n);
                    if (ImGui::SmallButton(n.c_str())) open_trig = lost_by[k];
                    tip("Open this trigger in the Triggers list below.");
                }
            } else if (!done_by.empty()) {
                row_label("Completed by");
                for (size_t k = 0; k < done_by.size(); k++) {
                    const WObj* t = object_by_uid(s.world, done_by[k]);
                    std::string n = (t && !t->description().empty() ? t->description() : "trigger") + "##t" + std::to_string(k);
                    if (k) flow(n);
                    if (ImGui::SmallButton(n.c_str())) open_trig = done_by[k];
                    tip("Open this trigger in the Triggers list below.");
                }
                // A delay the Completed by chooser made keeps its seconds here.
                for (uint32_t dt : done_by) {
                    WObj* to = object_by_uid(s.world, dt);
                    const std::string pre = "Objective " + std::to_string(i + 1) + ": after ";
                    if (!to || to->cls != key_of("MATriggerTimer") || to->description().rfind(pre, 0) != 0) continue;
                    float sec = to->f32("TrgTimerDurationSeconds");
                    row_label("After");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputFloat("##objdelay", &sec, 0, 0, "%.1f s")) {
                        sec = std::clamp(sec, 0.f, 36000.f);
                        push_undo_once("objective delay");
                        char b[32];
                        snprintf(b, sizeof b, "%g", double(sec));
                        to->set_f32("TrgTimerDurationSeconds", sec);
                        to->set_f32("TrgTimerDuration", sec / 60.f);
                        to->set_str("Description", pre + b + " s");
                        s.dirty = true;
                    }
                    ImGui::SetItemTooltip("Seconds after the mission starts: the objective is done then.");
                    break;
                }
            } else {
                row_label("Completed by");
                // A hidden step nothing completes is a spare slot (the retail levels' "JG Blank" lines): no warning colour.
                if (objective_slot(s, i).hidden) ImGui::TextDisabled("nothing (a spare hidden step)");
                else ImGui::TextColored(theme::tok().warn, "nothing yet");
                ImGui::SetCursorPosX(form_col());
                // One chooser for every way an objective is completed.
                int how = 0;
                uint32_t how_trig = 0;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (begin_combo("##how", "Choose how it is completed", ImGuiComboFlags_HeightLarge)) {
                    if (ImGui::Selectable("When the player reaches a place")) how = 1;
                    ImGui::SetItemTooltip("Makes a circle: click the map where the player must go. It is marked on the player's radar.");
                    if (ImGui::Selectable("After a delay")) how = 2;
                    ImGui::SetItemTooltip("Makes a timer: the objective is done that many seconds into the mission (30 s to start; change it in the row).");
                    ImGui::BeginDisabled(picked.empty());
                    if (ImGui::Selectable("When the selected units are destroyed")) how = 3;
                    ImGui::EndDisabled();
                    ImGui::SetItemTooltip(picked.empty() ? "Select the units or buildings on the map first." : "Done when every selected unit is destroyed; they are marked on the radar.");
                    const std::set<uint32_t> own = cutscene_owned_triggers(s.world);
                    bool header = false;
                    for (auto& x : s.world.objs) {
                        if (!is_trig(x.cls) || own.count(x.uid()) || x.description() == kWinAllTrigger || x.description().rfind(kGenPrefix, 0) == 0) continue;
                        if (!header) { ImGui::SeparatorText("Or when a trigger fires"); header = true; }
                        const std::string l = (x.description().empty() ? "Trigger #" + std::to_string(x.uid()) : x.description()) + "##hw" + std::to_string(x.uid());
                        if (ImGui::Selectable(l.c_str())) { how = 4; how_trig = x.uid(); }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Pick what completes this objective. Each choice makes the trigger for you.");
                const std::string onum = std::to_string(i + 1);
                if (how) g_first_objective = true;  // the win-when-all-done box below ticks itself once something completes an objective, as the Add objective kinds do
                if (how == 1) {
                    push_undo("objective trigger");
                    make_reach(i);
                    world_changed();
                    s.dirty = true;
                    status_ = "Click the map where the player must go (a 30 m circle, marked on the player's radar). Objective " + onum + " is done when the player enters it.";
                    status_time_ = 10;
                } else if (how == 2) {
                    push_undo("objective trigger");
                    if (uint32_t u = add_trigger_object(s, "MATriggerTimer", "Objective " + onum + ": after 30 s")) {
                        if (WObj* o = object_by_uid(s.world, u)) { o->set_f32("TrgTimerDurationSeconds", 30); o->set_f32("TrgTimerDuration", 0.5f); }
                        if (uint32_t a = add_action_object(s, u, "MAActionMissionVariableSet", "Complete objective " + onum))
                            if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("MissionVariableSet", claim_objective_var(s, i));
                        world_changed();
                        status_ = "Objective " + onum + " is done 30 seconds into the mission. Type the seconds in the After box.";
                        status_time_ = 8;
                    }
                    s.dirty = true;
                } else if (how == 3) {
                    push_undo("objective trigger");
                    uint32_t u = add_trigger_object(s, "MATriggerDestroyed", "Objective " + onum + ": destroy");
                    if (u) {
                        for (uint32_t x : picked) { add_reference(s, u, x); add_objective_target(s, x, i); }
                        if (uint32_t a = add_action_object(s, u, "MAActionMissionVariableSet", "Complete objective " + onum))
                            if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("MissionVariableSet", claim_objective_var(s, i));
                        world_changed();
                        status_ = "Objective " + onum + " is done when the " + std::to_string(picked.size()) +
                                  " selected unit(s) are destroyed; they are its radar targets. Edit the trigger on the Objectives & Triggers tab.";
                        status_time_ = 8;
                    }
                    s.dirty = true;
                } else if (how == 4 && how_trig) {
                    push_undo("objective trigger");
                    if (uint32_t a = add_action_object(s, how_trig, "MAActionMissionVariableSet", "Complete objective " + onum)) {
                        if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("MissionVariableSet", claim_objective_var(s, i));
                        world_changed();
                        const WObj* t = object_by_uid(s.world, how_trig);
                        status_ = "Objective " + onum + " is done when '" + (t ? t->description() : std::string("the trigger")) + "' fires.";
                        status_time_ = 8;
                    }
                    s.dirty = true;
                }
            }
            // Radar targets (MAObjectiveType under each target unit).
            auto targets = objective_targets(s.world, i);
            row_label("Radar targets");
            if (targets.empty()) ImGui::TextDisabled("none");
            for (size_t k = 0; k < targets.size(); k++) {
                std::string n = unit_name(targets[k]) + "##g" + std::to_string(k);
                if (k) flow(n);
                if (ImGui::SmallButton(n.c_str())) select_uid(targets[k]);
                tip("Select this target on the map. Right-click: stop marking it.");
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    push_undo("objective target");
                    remove_objective_targets(s, i, targets[k]);
                    world_changed();
                }
            }
            flow("Add selected");
            ImGui::BeginDisabled(picked.empty());
            if (ImGui::SmallButton("Add selected")) {
                push_undo("objective target");
                int n = 0;
                for (uint32_t x : picked) n += add_objective_target(s, x, i) ? 1 : 0;
                world_changed();
                status_ = std::to_string(n) + " unit(s) marked on the radar for objective " + std::to_string(i + 1) + ".";
                status_time_ = 6;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(picked.empty() ? "Select units or buildings on the map, then press this to mark them on the player's radar for this objective."
                                                 : "Mark the selected units on the player's radar for this objective.");
            // "Destroy these" left with no targets (Esc before any click) watches nothing, so the game counts it
            // as done the moment the mission starts. Say so here and offer the pick mode again.
            for (uint32_t t : done_by) {
                const WObj* to = object_by_uid(s.world, t);
                if (!to || to->cls != key_of("MATriggerDestroyed") || to->u32("DestroySet") || !reference_children(s.world, t).empty()) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
                ImGui::TextWrapped("Nothing to destroy yet, so the game ticks this objective off the moment the mission starts.");
                ImGui::PopStyleColor();
                ImGui::SetCursorPosX(form_col());
                if (ImGui::SmallButton("Pick targets on the map")) {
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::ObjTargets;
                    pick_.uid = t;
                    pick_.objective = i;
                    pick_.back = Tab::Messages;
                    tool_ = Tool::PickPoint;
                    selection_.clear();
                    status_ = "Click each unit or building to destroy on the map. Right-click or " + kt(A_CANCEL) + " when done.";
                    status_time_ = 10;
                }
                tip(("Then click each unit or building to destroy. Right-click or " + kt(A_CANCEL) + " when done.").c_str());
                break;
            }
            ImGui::PopID();
        }
        if (open_trig) { trig_uid_ = open_trig; trig_mode_ = 0; tab_ = Tab::Triggers; select_uid(open_trig); }
        if (rm_obj >= 0) {
            int refs = 0, rm_var = objective_var(s, rm_obj);
            for (auto& o : s.world.objs)
                if (o.cls == key_of("MAActionMissionVariableSet") && o.i32("MissionVariableSet") == rm_var) refs++;
            // A trigger an objective template made ("Objective 2: survive") whose only action completes this objective
            // goes with it; otherwise it would stay behind as a timer or circle that does nothing.
            auto objective_prefix = [](int n) { return "Objective " + std::to_string(n) + ": "; };
            std::vector<uint32_t> own;
            for (uint32_t t : objective_completers(s.world, rm_var, is_trig)) {
                const WObj* to = object_by_uid(s.world, t);
                if (!to || to->description().rfind(objective_prefix(rm_obj + 1), 0) != 0) continue;
                bool only = true;
                for (auto& o : s.world.objs) {
                    if (names.label(o.cls).rfind("MAAction", 0) != 0) continue;
                    if (o.cls == key_of("MAActionMissionVariableSet") && o.i32("MissionVariableSet") == rm_var) continue;
                    uint32_t p = o.parent();
                    for (int n = 0; p && p != t && n < 32; n++) { const WObj* x = object_by_uid(s.world, p); p = x ? x->parent() : 0; }
                    if (p == t) { only = false; break; }
                }
                if (only) own.push_back(t);
            }
            // The variable is also a trigger gate when some trigger waits on it (MATrigger MissionVariable, 1..29; the game
            // checks it in the trigger fire routine, research): then its setters keep it so the level's other logic still runs.
            bool gate = false;
            if (rm_var > 0 && rm_var < 30)
                for (auto& o : s.world.objs)
                    if (const WProp* p = o.find(key_of("MissionVariable")); p && p->numeric() && int(p->as_float()) == rm_var) { gate = true; break; }
            std::string q = "Delete objective " + std::to_string(rm_obj + 1) + " (\"" + s.objectives[size_t(rm_obj)] + "\")?";
            if (!own.empty()) q += "\n\nIts trigger \"" + object_by_uid(s.world, own[0])->description() + "\" is deleted with it.";
            if (refs > int(own.size())) q += "\n\n" + std::to_string(refs - int(own.size())) + " other trigger action(s) complete it" +
                     (gate ? std::string(". Other triggers wait on the same step, so those actions keep it and the level still plays as before.")
                           : std::string("; they will need a new objective picked."));
            q += "\n\nYou can undo this with " + kt(A_UNDO) + ".";
            confirm(q, [this, rm_obj, rm_var, own, objective_prefix, gate]() {
                if (!doc_ || rm_obj >= int(doc_->objectives.size())) return;
                push_undo("delete objective");
                for (uint32_t t : own) wedit::remove_subtree(doc_->world, t);
                // The template triggers of later objectives follow their new numbers ("Objective 3: survive" -> "Objective 2: survive").
                for (int k = rm_obj + 2; k <= int(doc_->objectives.size()); k++)
                    for (auto& o : doc_->world.objs) {
                        std::string d = o.description();
                        if (d.rfind(objective_prefix(k), 0) == 0) o.set_str("Description", objective_prefix(k - 1) + d.substr(objective_prefix(k).size()));
                    }
                doc_->objectives.erase(doc_->objectives.begin() + rm_obj);
                objective_removed(*doc_, rm_obj);
                world_changed();
                // Later objectives move up one place with their MARoot slots; only the deleted one's triggers lose their objective.
                for (auto& o : doc_->world.objs) {
                    if (o.cls != key_of("MAActionMissionVariableSet")) continue;
                    if (!gate && o.i32("MissionVariableSet") == rm_var) o.set_i32("MissionVariableSet", -1);  // the others keep theirs: variables move with their slots
                }
                doc_->dirty = true;
            });
        }
        if (s.objectives.empty()) hint_wrapped("No objectives yet. A mission needs at least one way to win: press Add objective and pick what the player must do.");
        // F1: a pilot on foot can hardly destroy a tank or a building: say so when a 'destroy' objective exists and the start is on foot.
        if (!s.multiplayer && io_) {
            static const uint32_t K_DESTROYED = key_of("MATriggerDestroyed");
            bool on_foot = false, destroy = false;
            const uint32_t cur = io_->start_container(s);
            for (auto& c : io_->start_choices()) if (c.container == cur) on_foot = c.kind == 0;
            for (int i = 0; on_foot && !destroy && i < int(s.objectives.size()); i++)
                for (uint32_t t : objective_completers(s.world, objective_var(s, i), is_trig))
                    if (const WObj* to = object_by_uid(s.world, t); to && to->cls == K_DESTROYED) destroy = true;
            if (destroy) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
                ImGui::TextWrapped("The player starts on foot, so destroying targets is very hard. Start them in a 'Mech (Map tab > Player starts in) or put a vehicle they can climb into near the start.");
                ImGui::PopStyleColor();
                if (ImGui::SmallButton("Open the Map tab")) tab_ = Tab::Map;
            }
        }
        ImGui::BeginDisabled(s.objectives.size() >= 30);
        if (ImGui::Button("Add objective...", ImVec2(-1, 0))) ImGui::OpenPopup("addobj");
        ImGui::EndDisabled();
        tip("Add a goal for the player. The ready-made kinds also make the trigger that completes it.");
        if (ImGui::BeginPopup("addobj")) {
            int i = int(s.objectives.size());
            std::string num = std::to_string(i + 1);
            std::string who = picked.empty() ? std::string() : unit_name(picked[0]) + (picked.size() > 1 ? " and " + std::to_string(picked.size() - 1) + " more" : "");
            auto complete = [&](uint32_t trig) {
                if (uint32_t a = add_action_object(s, trig, "MAActionMissionVariableSet", "Complete objective " + num))
                    if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("MissionVariableSet", claim_objective_var(s, i));
            };
            auto finish = [&](const std::string& msg) {
                if (s.objectives.size() == 1) g_first_objective = true;  // the win-when-all-done box below ticks itself
                world_changed();
                s.dirty = true;
                status_ = msg;
                status_time_ = 10;
            };
            if (ImGui::MenuItem(picked.empty() ? "Destroy these (click them on the map next)" : "Destroy these (the selected units)")) {
                push_undo("add objective");
                s.objectives.push_back(picked.empty() ? "Destroy the marked targets" : "Destroy " + who);
                int crewed = 0;
                if (uint32_t u = add_trigger_object(s, "MATriggerDestroyed", "Objective " + num + ": destroy")) {
                    for (uint32_t x : picked) crewed += add_destroy_target(s, u, x, i);
                    complete(u);
                    if (picked.empty()) {  // No selection -> pick the targets on the map, one click each
                        pick_ = PickTarget();
                        pick_.kind = PickTarget::ObjTargets;
                        pick_.uid = u;
                        pick_.objective = i;
                        pick_.back = Tab::Messages;
                        tool_ = Tool::PickPoint;
                        selection_.clear();
                    }
                }
                finish(picked.empty() ? "Click each unit or building to destroy on the map. Right-click or " + kt(A_CANCEL) + " when done."
                                      : "Objective " + num + " is done when the selected units are destroyed. They are marked on the player's radar." +
                                            (crewed ? " " + std::to_string(crewed) + " empty vehicle(s) now have an enemy crew." : std::string()));
            }
            ImGui::SetItemTooltip(picked.empty() ? "Then click the units or buildings to destroy on the map. (Or select them first.)"
                                                 : "Done when every selected unit is destroyed. They are marked on the player's radar.");
            ImGui::BeginDisabled(picked.empty());
            if (ImGui::MenuItem(picked.empty() ? "Keep these alive (select them on the map first)###keepalive" : "Keep these alive (lose if they are destroyed)###keepalive")) {
                push_undo("add objective");
                s.objectives.push_back("Keep " + who + " alive");
                if (uint32_t u = add_trigger_object(s, "MATriggerDestroyed", "Objective " + num + ": lose if destroyed")) {
                    if (WObj* o = object_by_uid(s.world, u)) o->set_i32("CombineMethod", 1);  // any one lost
                    for (uint32_t x : picked) { add_reference(s, u, x); add_objective_target(s, x, i); }
                    if (uint32_t a = add_action_object(s, u, "MAActionGameCondition", "Lose the mission"))
                        if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("GameCondition", 1);
                }
                finish("The mission is lost if any of the selected units is destroyed. They are marked on the player's radar.");
            }
            ImGui::SetItemTooltip(picked.empty() ? "Select the units or buildings to protect on the map first."
                                                 : "The mission is lost if any selected unit is destroyed. They are marked on the player's radar.");
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Reach a place")) {
                push_undo("add objective");
                s.objectives.push_back("Reach the marked area");
                make_reach(i);
                finish("Click the map where the player must go (a 30 m circle, marked on the player's radar). Objective " + num + " is done when the player enters it.");
            }
            ImGui::SetItemTooltip("Done when the player enters a circle you place on the map next. The place is marked on the player's radar.");
            if (ImGui::MenuItem("Survive for 5 minutes")) {
                push_undo("add objective");
                s.objectives.push_back("Survive for 5 minutes");
                if (uint32_t u = add_trigger_object(s, "MATriggerTimer", "Objective " + num + ": survive")) {
                    if (WObj* o = object_by_uid(s.world, u)) {
                        o->set_f32("TrgTimerDurationSeconds", 300);
                        o->set_f32("TrgTimerDuration", 5.f);
                        o->set_i32("TrgTimerDisplayOnHUD", 1);
                    }
                    complete(u);
                }
                finish("Objective " + num + " is done 5 minutes after the mission starts (clock on the HUD). Change the time in the Triggers list below.");
            }
            ImGui::SetItemTooltip("Done when a 5-minute clock on the HUD runs out. The time can be changed in the Triggers list below.");
            ImGui::Separator();
            if (ImGui::MenuItem("Other (complete it from a trigger)")) { push_undo("add objective"); s.objectives.push_back("New objective"); claim_objective_var(s, i); s.dirty = true; }
            ImGui::SetItemTooltip("Just the text. Then make a trigger with the action \"Complete objective\".");
            ImGui::EndPopup();
        }
        // Win when every objective is done: one "all of" trigger over the triggers that complete them (retail pattern).
        {
            static const char* kWinAll = kWinAllTrigger;
            uint32_t win = 0;
            for (auto& o : s.world.objs)
                if (o.cls == key_of("MATriggerLogic") && o.description() == kWinAll) { win = o.uid(); break; }
            std::vector<uint32_t> want;
            for (int i = 0; i < int(s.objectives.size()); i++)
                for (uint32_t t : objective_completers(s.world, objective_var(s, i), is_trig))
                    if (t != win && std::find(want.begin(), want.end(), t) == want.end()) want.push_back(t);
            bool on = win != 0;
            // The first objective switches it on (finishing the only objective did not win), unless
            // the mission already has another way to win. Same undo step as the objective.
            if (g_first_objective) {
                g_first_objective = false;
                bool wins = false;
                for (auto& o : s.world.objs)
                    if (o.cls == key_of("MAActionGameCondition") && o.i32("GameCondition") == 2) wins = true;
                if (!win && !wins && !s.objectives.empty()) {
                    win = add_trigger_object(s, "MATriggerLogic", kWinAll);
                    if (uint32_t a = add_action_object(s, win, "MAActionGameCondition", "Win the mission"))
                        if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("GameCondition", 2);
                    on = true;
                    world_changed();
                }
            }
            if (!s.objectives.empty()) {
                if (ImGui::Checkbox("Win the mission when every objective is done", &on)) {
                    push_undo("win when all done");
                    if (on) {
                        win = add_trigger_object(s, "MATriggerLogic", kWinAll);
                        if (uint32_t a = add_action_object(s, win, "MAActionGameCondition", "Win the mission"))
                            if (WObj* ao = object_by_uid(s.world, a)) ao->set_i32("GameCondition", 2);
                    } else {
                        wedit::remove_subtree(s.world, win);
                        win = 0;
                    }
                    world_changed();
                    s.dirty = true;
                }
                tip("One trigger that wins the mission once every objective a trigger completes is done. It follows the list as you add objectives.");
            }
            if (win) {  // its inputs = the completing triggers; with none it stays switched off so it cannot fire at once
                std::vector<uint32_t> have;
                for (uint32_t r : reference_children(s.world, win)) if (const WObj* ro = object_by_uid(s.world, r)) have.push_back(ro->u32("ReferenceUID"));
                if (have != want) {
                    for (uint32_t r : reference_children(s.world, win)) wedit::remove_subtree(s.world, r);
                    for (uint32_t t : want) add_reference(s, win, t);
                    if (WObj* o = object_by_uid(s.world, win)) {
                        o->set_i32("LogicType", 0);
                        if (o->find("TriggerEnabled")) o->set_i32("TriggerEnabled", want.empty() ? 0 : 1);
                    }
                    world_changed();
                    s.dirty = true;
                }
                // Kept in step on every pass, not only when the inputs change: a trigger with no inputs would win at load.
                if (WObj* o = object_by_uid(s.world, win))
                    if (o->find("TriggerEnabled") && o->i32("TriggerEnabled") != (want.empty() ? 0 : 1)) {
                        o->set_i32("TriggerEnabled", want.empty() ? 0 : 1);
                        world_changed();
                        s.dirty = true;
                    }
            }
        }
        // "Lose the mission if the time runs out": one HUD clock trigger named "Countdown" that this box owns.
        {
            static const uint32_t K_TIMER_ = key_of("MATriggerTimer");
            uint32_t cd = 0;
            for (auto& o : s.world.objs) if (o.cls == K_TIMER_ && o.description() == "Countdown") { cd = o.uid(); break; }
            bool on = cd != 0;
            if (ImGui::Checkbox("Lose the mission if the time runs out", &on)) {
                push_undo("lose when time runs out");
                if (on) {
                    uint32_t u = add_trigger_object(s, "MATriggerTimer", "Countdown");
                    if (WObj* o = object_by_uid(s.world, u)) {
                        o->set_f32("TrgTimerDurationSeconds", 120);
                        o->set_f32("TrgTimerDuration", 2.f);
                        o->set_i32("TrgTimerDisplayOnHUD", 1);
                    }
                    if (uint32_t a = add_action_object(s, u, "MAActionGameCondition", "Lose the mission"))
                        if (WObj* o = object_by_uid(s.world, a)) o->set_i32("GameCondition", 1);
                } else {
                    wedit::remove_subtree(s.world, cd);
                }
                world_changed();
                s.dirty = true;
            }
            tip("A clock on the player's screen counts down from the time you set; when it reaches zero the mission is lost. Unticking removes it.");
            if (WObj* o = cd ? object_by_uid(s.world, cd) : nullptr) {
                float sec = o->f32("TrgTimerDurationSeconds");
                form_label("Time limit (seconds)", form_col());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputFloat("##countdown", &sec, 0, 0, "%.1f s")) {
                    sec = std::clamp(sec, 1.f, 36000.f);
                    push_undo_once("time limit");
                    o->set_f32("TrgTimerDurationSeconds", sec);
                    o->set_f32("TrgTimerDuration", sec / 60.f);
                    s.dirty = true;
                }
                tip("Seconds from the start of the mission until the mission is lost. With an intro cutscene at the start, the clock starts when the intro is over. A cutscene played later in the mission shows the clock over its black bars: that is how the game draws it.");
            }
        }
        ImGui::SeparatorText("When the mission ends");
        form_label("Won", form_col());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##wintext", "Mission complete", &s.win_text)) push_undo_once("win text");
        tip("The line the game shows when the player wins. Leave empty for the game's own words.");
        form_label("Lost", form_col());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##losetext", "Mission failed", &s.lose_text)) push_undo_once("lose text");
        tip("The line the game shows when the player loses (a lose trigger, or the player's 'Mech is destroyed). Leave empty for the game's own words.");
        hint_wrapped("The game shows this with the choices (Continue / Replay, Retry / Quit).");
        }
    } else {
        if (theme::section("Messages", ImGuiTreeNodeFlags_DefaultOpen)) {
        hint_wrapped("A multiplayer map has no briefing or objectives: the game type sets the goal. To show text to players while they play, use the Objectives & Triggers tab and the action \"Show a message\". Nothing needs to be filled in here.");
        }
    }
    int remove = -1;
    bool adv_open = theme::section("Advanced: extra text lines");
    if (adv_open) {
    ImGui::TextWrapped("Not needed for normal use: trigger messages keep their own text. These lines are for modders who refer to text by number.");
    for (auto& [n, t] : s.strings) {
        ImGui::PushID(n);
        form_label(("Line " + std::to_string(n)).c_str(), form_col());
        ImGui::SetNextItemWidth(-60 * dpi_);  // room for Del
        // 60000 and up are the editor's own numbers: Save rewrites them from the briefing, objectives and
        // trigger messages, so an edit here would be lost (wiki t16-cutscenes-objectives-test-surprise-9).
        const bool compiled = n >= ma2::kStrBriefingTitle;
        if (ImGui::InputText("##t", &t, compiled ? ImGuiInputTextFlags_ReadOnly : 0)) push_undo_once("text");
        if (compiled)
            tip(n == ma2::kStrBriefingTitle ? "The briefing title. Change it on the Objectives & Triggers tab: Save writes this line from there."
                : n == ma2::kStrBriefing    ? "The briefing text. Change it on the Objectives & Triggers tab: Save writes this line from there."
                : n < ma2::kStrMessage      ? "An objective's text. Change it in the Objectives list on the Objectives & Triggers tab: Save writes this line from there."
                                            : "A trigger message's text. Change it in the trigger's \"Show a message\" effect: Save writes this line from there.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Del")) remove = n;
        ImGui::PopID();
    }
    if (remove >= 0) { push_undo("delete text"); s.strings.erase(remove); s.dirty = true; }
    if (ImGui::Button("Add text", ImVec2(-1, 0))) {
        push_undo("add text");
        int n = s.strings.empty() ? 1 : s.strings.rbegin()->first + 1;
        s.strings[n] = "";
        s.dirty = true;
    }
    }
    if (theme::section("Your own sound clips", ImGuiTreeNodeFlags_DefaultOpen)) {  // body kept at this indent (merges)
    ImGui::TextWrapped("Import WAV sound files to play from triggers with the action \"Play a sound\". Any WAV works: the editor converts it to the format the game plays. Other formats (MP3, OGG) must be saved as WAV first, for example in Audacity.");
    int rm = -1;
    for (auto& [n, c] : s.audio) {
        ImGui::PushID(n + 100000);
        audition_button(n, ("clip" + std::to_string(n)).c_str());
        ImGui::SameLine();
        ImGui::Text("%d  %s  (%.0f KB)", n, c.source.c_str(), c.wav.size() / 1024.0);
        ImGui::SameLine();
        if (ImGui::SmallButton("Del")) rm = n;
        tip("Remove this clip (asks first).");
        ImGui::PopID();
    }
    if (s.audio.empty()) ImGui::TextDisabled("No clips imported.");
    if (rm >= 0) {
        int refs = 0;
        for (auto& o : s.world.objs)
            if (o.cls == key_of("MAActionSound") && o.u32("CueEvent") == kAudioBase + uint32_t(rm)) refs++;
        std::string q = "Remove the sound clip \"" + s.audio[rm].source + "\"?";
        if (refs) q += "\n\n" + std::to_string(refs) + " trigger action(s) play it and will be silent.";
        q += "\n\nYou can undo this with " + kt(A_UNDO) + ".";
        confirm(q, [this, rm]() {
            if (!doc_) return;
            push_undo("remove clip");
            if (audition_clip_ == rm) audition_stop();
            doc_->audio.erase(rm);
            doc_->dirty = true;
        });
    }
    if (ImGui::Button("Import WAV...", ImVec2(-1, 0))) {
        std::wstring f = pick_wav(hwnd_);
        if (!f.empty()) {
            AudioClip c;
            Bytes raw;
            std::string err, note;
            if (!read_file(f, raw, &err)) {
                message("Import WAV", "The file could not be read: " + err);
            } else if (!convert_wav(raw, c.wav, &note)) {
                message("Import WAV", "This file cannot be used: " + note +
                                          "\n\nSave it as a WAV file first (in Audacity: File > Export > Export as WAV).");
            } else if (!s.audio.empty() && s.audio.rbegin()->first >= 63) {
                // The game reads only [Audio] entries 1-63; the next id is always max + 1, so deleting and importing reaches 64 sooner.
                message("Import WAV", "A scenario holds at most 63 sound clips (the game does not read clip 64 or higher). Delete a clip you do not use, then import again.");
            } else {
                c.source = path_utf8(fs::path(f).filename());
                push_undo("import clip");
                int n = s.audio.empty() ? 1 : s.audio.rbegin()->first + 1;
                s.audio[n] = std::move(c);
                status_ = "Imported " + s.audio[n].source + (note.empty() ? "" : " (" + note + ")") +
                          ". Use it with the trigger action \"Play a sound\".";
                status_time_ = 8;
                s.dirty = true;
            }
        }
    }
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------------------------
// Options

// Choose... and the typed path: make this the game folder, load it, and say what happened.
bool App::use_game_folder(const std::wstring& f) {
    settings_.install = f;
    settings_.save();
    std::string err;
    if (!load_game(&err)) {
        game_error_ = err;
        install_note_.clear();
        message("Install folder", err);
        return false;
    }
    install_note_.clear();
    doc_.reset(); rebuild_objects(); show_new_ = true;
    status_ = "Game folder found: " + path_utf8(fs::path(f)); status_amber_.clear(); status_time_ = 8;
    return true;
}

void App::settings_folders() {
    ImGui::SeparatorText("Scenario folder");
    ImGui::TextWrapped("%s", path_utf8(settings_.scenarios()).c_str());
    ImGui::TextDisabled(settings_.scenario_folder.empty() ? "(the standard place: Documents\\MechAssault 2\\Scenarios)" : "(your own choice; the game uses it too)");
    ImGui::TextWrapped("The game lists every scenario in this folder. Files already saved elsewhere stay where they are.");
    if (ImGui::Button("Change...")) {
        std::wstring f = pick_folder(hwnd_, settings_.scenarios());
        if (!f.empty()) {
            std::string err;
            if (Settings::write_scenario_folder(f, &err)) {
                settings_.scenario_folder = f;
                status_ = "Scenario folder changed. The game uses it from its next start.";
                status_time_ = 6;
            } else {
                message("Scenario folder", "The setting could not be saved: " + err);
            }
        }
    }
    tip("Choose another folder for scenarios (for example a shared or synced folder).");
    ImGui::SameLine();
    if (ImGui::Button("Use default")) {
        std::string err;
        if (Settings::write_scenario_folder({}, &err)) settings_.scenario_folder.clear();
        else message("Scenario folder", "The setting could not be saved: " + err);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) {
        std::error_code ec;
        fs::create_directories(settings_.scenarios(), ec);
        if (background) message("Open folder", "Scenario folder: " + path_utf8(settings_.scenarios()));  // hidden editor: no Explorer window
            else ShellExecuteW(hwnd_, L"open", settings_.scenarios().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SeparatorText("Game folder");
    ImGui::TextWrapped("%s", gd_.is_open() ? path_utf8(gd_.install()).c_str() : "(not found)");
    if (gd_.is_open()) ImGui::TextColored(theme::tok().ok, "Game found. The editor is ready.");
    ImGui::TextDisabled("The folder with MechAssault 2.exe and game_files.");
    if (ImGui::Button("Choose...##inst") && confirm_close()) {
        std::wstring f = pick_folder(hwnd_, gd_.install());
        if (!f.empty()) use_game_folder(f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Automatic") && confirm_close()) {
        settings_.install.clear();
        settings_.save();
        wchar_t mod[MAX_PATH];
        GetModuleFileNameW(nullptr, mod, MAX_PATH);
        const std::string where = path_utf8(fs::path(mod).parent_path());
        std::string err;
        if (!load_game(&err)) {
            game_error_ = err;
            install_note_ = "Automatic looked in " + where + " and the folders above it for MechAssault 2.exe and game_files, and found nothing. Use Choose... to pick the folder.";
            status_ = "Automatic: no game folder found next to the editor. Use Choose...";
            status_time_ = 8;
        } else {
            install_note_.clear();
            doc_.reset(); rebuild_objects(); show_new_ = true; status_ = "Game folder found: " + path_utf8(gd_.install()); status_amber_.clear(); status_time_ = 8;
        }
    }
    tip("Look for the game folder next to the editor (and the folders above it) again.");
    // The folder dialog is not the only way (it cannot open under automation, and a path is often at hand): type or paste it.
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool typed_go = ImGui::InputTextWithHint("##instpath", "or type / paste the folder here, then press Enter or Use", &install_typed_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (install_typed_.empty()) tip("The folder with MechAssault 2.exe and game_files, for example D:\\Games\\MechAssault 2. Pasting the path of MechAssault 2.exe itself works too.");  // not over the note below once something is typed
    ImGui::BeginDisabled(trim(install_typed_).empty());
    typed_go |= ImGui::Button("Use##instpath");
    ImGui::EndDisabled();
    tip("Use the folder typed above as the game folder.");
    if (typed_go && !trim(install_typed_).empty() && confirm_close()) {
        std::string t = trim(install_typed_);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = trim(t.substr(1, t.size() - 2));  // "Copy as path" adds quotes
        fs::path f = widen(t);
        std::error_code ec;
        if (fs::is_regular_file(f, ec)) f = f.parent_path();  // the path of MechAssault 2.exe (or any file in the folder)
        if (!fs::is_directory(f, ec)) {
            install_note_ = "There is no folder " + path_utf8(f) + ". Check the path and try again.";
            status_ = "Game folder: that folder does not exist.";
            status_time_ = 8;
        } else if (!fs::exists(f / L"game_files" / L"common.mgf", ec)) {
            install_note_ = path_utf8(f) + " has no game_files folder with the game's data in it. Pick the folder that holds MechAssault 2.exe and game_files.";
            status_ = "Game folder: that is not the game folder.";
            status_time_ = 8;
        } else if (use_game_folder(f.wstring())) {
            install_typed_.clear();
        }
    }
    if (!install_note_.empty()) ImGui::TextWrapped("%s", install_note_.c_str());
    if (!game_error_.empty() && (install_note_.empty() || gd_.is_open())) {  // The Automatic note already says it all
        // Wrapped, so a long message is not cut at the dialog's edge.
        ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().error);
        ImGui::TextWrapped("%s", game_error_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SeparatorText("You");
    form_label("Author name", form_col());
    if (ImGui::InputTextWithHint("##author", "Your name (optional)", &settings_.author)) settings_.save();
    tip("Shown as the author of new scenarios.");
}

// ---------------------------------------------------------------------------------------------
// Inspector

static bool edit_prop(WProp& p, const std::string& label) {
    bool ch = false;
    switch (p.type) {
        case PT_I8: case PT_I16: case PT_I32: case PT_U8: case PT_U16: case PT_U8B: {
            int v = int(p.as_int());
            if (ImGui::InputInt(label.c_str(), &v, 0)) { p.set_int(v); ch = true; }
            break;
        }
        case PT_U32: {
            long long v = p.as_int();
            if (ImGui::InputScalar(label.c_str(), ImGuiDataType_S64, &v)) { p.set_int(v); ch = true; }
            break;
        }
        case PT_COLOR: {
            uint32_t v = uint32_t(p.as_int());
            float c[4] = {float((v >> 16) & 255) / 255, float((v >> 8) & 255) / 255, float(v & 255) / 255, float(v >> 24) / 255};
            if (ImGui::ColorEdit4(label.c_str(), c, ImGuiColorEditFlags_NoInputs)) {
                p.set_int(int64_t(uint32_t(c[3] * 255) << 24 | uint32_t(c[0] * 255) << 16 | uint32_t(c[1] * 255) << 8 | uint32_t(c[2] * 255)));
                ch = true;
            }
            break;
        }
        case PT_F32: case PT_F64: {
            double v = p.as_float();
            if (ImGui::InputDouble(label.c_str(), &v, 0, 0, "%.4f") && std::isfinite(v)) { p.set_float(v); ch = true; }  // "inf" / "nan" / 1e999 would put NaN transforms in the game
            break;
        }
        case PT_STRING: {
            std::string s = p.as_string();
            if (ImGui::InputText(label.c_str(), &s)) { p.set_string(s); ch = true; }
            break;
        }
        case PT_VEC3: {
            Vec3 v = p.as_vec3();
            float f[3] = {v.x, v.y, v.z};
            if (ImGui::InputFloat3(label.c_str(), f) && std::isfinite(f[0] + f[1] + f[2])) { p.set_vec3({f[0], f[1], f[2]}); ch = true; }
            break;
        }
        default:
            ImGui::TextDisabled("%s  %s", label.c_str(), p.display().c_str());
    }
    return ch;
}

// A one-line help hint under a control: dimmed, wrapped to the panel.
static void hint_wrapped(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// One property through the dictionary: plain label, tooltip with units, a typed widget (checkbox,
// dropdown of meanings, slider within the known range...). Falls back to the raw editor.
// Object pickers for link settings (set by ui_inspector).
static ScenarioIO* g_link_io = nullptr;
static const NameTable* g_link_names = nullptr;
static const TemplateCatalog* g_link_cat = nullptr;
// Text id settings show words: the game's own (text.mgf) or the map's own text.
static GameData* g_link_gd = nullptr;
static Scenario* g_link_doc = nullptr;

// > 0: the task panels' form (App::ui_field): the label in that left column, the field filling the row, links
// shown by the linked object's name. 0: the Properties layout (label right of the field).
static float g_form_col = 0;

static bool edit_prop_dict(WProp& p, const DictProp* d, const std::string& raw, const Wdf& world, bool read_only, const char* note = nullptr) {
    std::string label = d && !d->label.empty() ? d->label : raw;
    // A trigger's fields read the same here as in the Triggers panel (one setting, one name).
    if (raw == "TriggerAutoReset") label = "Repeat";
    else if (raw == "TriggerEnabled") label = "Active from the start";
    else if (raw == "TrgTimerDurationSeconds") label = "After (seconds)";
    else if (raw == "CombineMethod") label = "Needs";  // (quick part): plain words for the circle's fields
    else if (raw == "TypeOfCheck") label = "What it checks";
    bool ch = false;
    ImGui::BeginDisabled(read_only);
    std::string id = label + "##" + raw;
    bool is_int = p.type == PT_I8 || p.type == PT_I16 || p.type == PT_I32 || p.type == PT_U8 || p.type == PT_U16 || p.type == PT_U32 || p.type == PT_U8B;
    const bool left = g_form_col > 0;
    if (left && d && is_int && d->type == "bool") {
        // A tick box keeps its words on its right, in the field column like the other fields.
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::SameLine(g_form_col);
    } else if (left) {
        form_label(label.c_str(), g_form_col);
        id = "##" + raw;
    }
    if (d && is_int && d->type == "bool") {
        bool v = p.as_int() != 0;
        if (ImGui::Checkbox(id.c_str(), &v)) { p.set_int(v ? 1 : 0); ch = true; }
    } else if (d && is_int && !d->enums.empty()) {
        long long cur = p.as_int();
        // The dictionary's research tags ("Any (code)", "Inside (verified)") are for us, not the designer.
        auto plain = [](std::string m) {
            for (const char* tag : {" (verified)", " (code)", " (inferred)", " (guess)", " (documented)"})
                for (size_t at; (at = m.find(tag)) != std::string::npos;) m.erase(at, strlen(tag));
            return m;
        };
        std::string shown = "Other (" + std::to_string(cur) + ")";
        for (auto& [v, m] : d->enums) if (v == cur) shown = plain(m);
        if (begin_combo(id.c_str(), shown.c_str())) {
            for (auto& [v, m] : d->enums)
                if (ImGui::Selectable((plain(m) + "##" + std::to_string(v)).c_str(), v == cur)) { p.set_int(v); ch = true; }
            ImGui::EndCombo();
        }
    } else if (d && (p.type == PT_F32 || p.type == PT_F64) && d->type == "angle_rad") {
        // Stored in radians; shown (and edited) in degrees like the Heading row.
        float v = float(p.as_float() * 57.29578);
        if (ImGui::DragFloat(id.c_str(), &v, 0.5f, 0, 0, "%.1f deg")) { p.set_float(double(v) / 57.29578); ch = true; }
    } else if (d && p.type == PT_U32 && d->type == "colour") {
        // RGB packed as 0xRRGGBB (any top byte is kept as stored).
        uint32_t v = uint32_t(p.as_int());
        float c[3] = {float((v >> 16) & 255) / 255, float((v >> 8) & 255) / 255, float(v & 255) / 255};
        if (ImGui::ColorEdit3(id.c_str(), c, ImGuiColorEditFlags_NoInputs)) {
            uint32_t n = (v & 0xFF000000u) | uint32_t(c[0] * 255 + 0.5f) << 16 | uint32_t(c[1] * 255 + 0.5f) << 8 | uint32_t(c[2] * 255 + 0.5f);
            p.set_int(int64_t(n));
            ch = true;
        }
    } else if (d && p.type == PT_U32 && d->type == "template_ref" && g_link_cat) {
        // The object template this setting names: show its name, pick another from the catalog.
        long long v = p.as_int();
        const TemplateInfo* cur = v ? g_link_cat->find(uint32_t(v)) : nullptr;
        std::string pop = "##tpick" + raw;
        std::string shown = v == 0 ? "(none)" : cur ? cur->name : "Unknown template " + std::to_string(v);
        if (ImGui::Button((shown + "##tb" + raw).c_str(), ImVec2(left ? -FLT_MIN : ImGui::GetContentRegionAvail().x * 0.6f, 0))) ImGui::OpenPopup(pop.c_str());
        if (!left) {
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
        }
        ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_Appearing);
        if (ImGui::BeginPopup(pop.c_str())) {
            static std::string pf;
            if (ImGui::IsWindowAppearing()) { pf.clear(); ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##tpf", "Find (name or group)", &pf);
            std::string lf = lower(pf);
            if (ImGui::Selectable("(none)")) { p.set_int(0); ch = true; ImGui::CloseCurrentPopup(); }
            ImGui::BeginChild("##tpl", ImVec2(0, 0), ImGuiChildFlags_Borders);
            int n = 0;
            for (const TemplateInfo& t : g_link_cat->all()) {
                if (!t.placeable && uint32_t(v) != t.uid) continue;
                if (!lf.empty() && lower(t.name + " " + t.category).find(lf) == std::string::npos) continue;
                if (++n > 400) { ImGui::TextDisabled("More match: type more."); break; }
                if (ImGui::Selectable((t.name + "   " + t.category + "##" + std::to_string(t.uid)).c_str(), t.uid == uint32_t(v))) {
                    p.set_int(t.uid); ch = true; ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
    } else if (d && is_int && d->type == "text_id") {
        // The words, never the number: the game's own text read-only, the map's own text editable.
        uint32_t v = uint32_t(p.as_int());
        std::string* own = v && g_link_doc ? own_text(*g_link_doc, v) : nullptr;
        std::string words = own ? *own : v && g_link_gd ? g_link_gd->game_text(v) : std::string();
        float w = left ? -FLT_MIN : ImGui::GetContentRegionAvail().x * 0.6f;
        ImGui::SetNextItemWidth(w);
        if (own) {
            if (ImGui::InputText(id.c_str(), own)) ch = true;
        } else {
            std::string shown = !v ? "(no text)" : words.empty() ? "(the game's text, not found on this disc)" : "\"" + words + "\"";
            ImGui::BeginDisabled();
            ImGui::InputText(id.c_str(), &shown, ImGuiInputTextFlags_ReadOnly);
            ImGui::EndDisabled();
        }
        if (v) ImGui::SetItemTooltip(own ? "This map's own text (text %u)." : "The game's own words (text %u); the game shows them as they are.", v);
    } else if (d && d->name == "CameraDuration" && (p.type == PT_F32 || p.type == PT_F64)) {
        // A shot's length is a typed number, never a slider that slips: 0.5 to 120 s.
        float v = float(p.as_float());
        if (ImGui::InputFloat(id.c_str(), &v, 0.5f, 0.5f, "%.1f s")) { p.set_float(std::clamp(v, 0.5f, 120.f)); ch = true; }
    } else if (d && (p.type == PT_F32 || p.type == PT_F64)) {
        float v = float(p.as_float());
        std::string fmt = "%.2f" + (d->units.empty() ? std::string() : " " + d->units);
        bool changed = d->has_range ? ImGui::SliderFloat(id.c_str(), &v, float(d->range_min), float(d->range_max), fmt.c_str())
                                    : ImGui::DragFloat(id.c_str(), &v, std::max(0.01f, std::fabs(v) * 0.01f), 0, 0, fmt.c_str());
        if (changed) { p.set_float(v); ch = true; }
    } else if (is_int && ((d && d->type == "uid_ref") || (g_link_io && p.data.size() == 4 && g_link_io->is_object_link(p.key)))) {
        long long v = p.as_int();
        std::string pop = "##pick" + raw;
        // One button naming the linked object (never its raw number); it opens the picker.
        std::string shown = "(none)";
        if (v) {
            // A shot made from the game's template points at its voice-over object, which a custom map does not have: it plays
            // nothing, so it reads as no voice line, not as a broken link with a number.
            shown = label == "Voice line" ? "(none)" : "(not in this map)";
            for (auto& o : world.objs)
                if (o.uid() == uint32_t(v)) { shown = o.description().empty() ? (g_link_names ? g_link_names->label(o.cls) : shown) : o.description(); break; }
        }
        if (left) {
            if (ImGui::Button((shown + "##lb" + raw).c_str(), ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup(pop.c_str());
        } else {
            if (ImGui::Button((shown + "##lb" + raw).c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0))) ImGui::OpenPopup(pop.c_str());
            ImGui::SetItemTooltip("Click to choose the object this setting points to from the level's objects.");
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
        }
        ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_Appearing);
        if (ImGui::BeginPopup(pop.c_str())) {
            static std::string pf;
            if (ImGui::IsWindowAppearing()) { pf.clear(); ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##pkf", "Find (name or kind)", &pf);
            std::string lf = lower(pf);
            if (ImGui::Selectable("(none)")) { p.set_int(0); ch = true; ImGui::CloseCurrentPopup(); }
            ImGui::BeginChild("##pkl", ImVec2(0, 0), ImGuiChildFlags_Borders);
            int shown = 0;
            for (auto& o : world.objs) {
                std::string cn = g_link_names ? g_link_names->label(o.cls) : std::string();
                if (o.description().empty() && lf.empty()) continue;
                if (!lf.empty() && lower(o.description() + " " + cn).find(lf) == std::string::npos) continue;
                if (++shown > 400) { ImGui::TextDisabled("More match: type more."); break; }
                std::string l = (o.description().empty() ? "(" + cn + ")" : o.description()) + "   " + cn + "##" + std::to_string(o.uid());
                if (ImGui::Selectable(l.c_str(), o.uid() == uint32_t(v))) { p.set_int(o.uid()); ch = true; ImGui::CloseCurrentPopup(); }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
    } else if (d && is_int && d->has_range && d->range_max - d->range_min <= 100000) {
        int v = int(p.as_int());
        std::string fmt = "%d" + (d->units.empty() ? std::string() : " " + d->units);
        if (ImGui::SliderInt(id.c_str(), &v, int(d->range_min), int(d->range_max), fmt.c_str())) { p.set_int(v); ch = true; }
    } else {
        ch = edit_prop(p, id);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        if (d && !d->tooltip.empty() && d->known()) ImGui::TextUnformatted(d->tooltip.c_str());
        else ImGui::TextUnformatted("What this setting does is not known yet.");
        if (d && !d->units.empty()) ImGui::TextDisabled("Units: %s", d->units.c_str());
        if (d && d->has_default) ImGui::TextDisabled("Usual value: %s", d->default_text.c_str());
        if (d && d->confidence == "inferred") ImGui::TextDisabled("(meaning worked out from the game data, not tested)");
        ImGui::TextDisabled("Game name: %s", raw.c_str());
        if (read_only) ImGui::TextDisabled("The game keeps this up to date; it cannot be changed here.");
        if (note) ImGui::TextUnformatted(note);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return ch;
}

// What step 2 asks for: 'Mechs, vehicles (a pad holds one) and power-ups; a building or a tree does not tick it.
size_t App::ns_things() const {
    static const uint32_t K[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"),
                                 key_of("MAPowerUp"), key_of("MACrateGenerator")};
    size_t n = 0;
    for (auto& o : doc_->world.objs)
        if (std::find(std::begin(K), std::end(K), o.cls) != std::end(K)) n++;
    return n;
}

void App::ns_start() {
    if (!doc_) return;
    ns_on_ = true;
    ns_shaped_ = ns_tested_ = ns_placed_said_ = ns_looked_ = false;
    ns_base_ = ns_things();
}

// The Next steps card: the 10-minute first scenario as a checklist.
void App::ui_next_steps() {
    Scenario& s = *doc_;
    bool enemies = false;
    if (!s.multiplayer)
        for (auto& o : s.world.objs)
            if (o.cls != K_LEVELSTART && helper_group(o.cls) < 0 && o.find("Nation") && o.i32("Nation") == 5) { enemies = true; break; }
    // An objective whose "Destroy these" watches nothing yet does not count as one (the game ticks it off at the start).
    size_t empty_destroy = 0;
    for (auto& o : s.world.objs)
        if (o.cls == key_of("MATriggerDestroyed") && o.description().compare(0, 10, "Objective ") == 0 && !o.u32("DestroySet") &&
            reference_children(s.world, o.uid()).empty())
            empty_destroy++;
    const bool objective = !s.objectives.empty() && empty_destroy < s.objectives.size();
    struct Step { const char* label; const char* tip; bool done; Tab tab; bool test; };
    std::vector<Step> steps;
    steps.push_back({"Shape the ground", "Sculpt and Paint tools: raise hills, dig valleys and paint the ground. Ticks itself after your first brush stroke.",
                     ns_shaped_, Tab::Terrain, false});
    if (s.multiplayer) {
        steps.push_back({"Place 'Mechs, vehicles and powerups", "Place tool: pick a thumbnail, click the map (drag to face it). "
                         "Ticks itself when you place something.", ns_things() > ns_base_, Tab::Units, false});
    } else {
        steps.push_back({"Place enemies to fight", "Place tool: set 'Placing for' to Enemy, pick a unit and click the map. "
                         "Ticks itself when an enemy is on the map.", enemies, Tab::Units, false});
        steps.push_back({"Add an objective", "Objectives & Triggers tab: a mission needs at least one way to win. Ticks itself when the mission has an objective.",
                         objective, Tab::Messages, false});
    }
    // The look and weather have a step too (optional; ticks itself once the Environment tab has been open).
    if (tab_ == Tab::Environment) ns_looked_ = true;
    steps.push_back({"Set the look and weather (optional)", "Environment tab: day, evening or night, the sky, fog, sun and the weather. Ticks itself once you have looked at it.",
                     ns_looked_, Tab::Environment, false});
    steps.push_back({"Test it in the game", nullptr, ns_tested_, Tab::Map, true});
    ImGui::SeparatorText("Next steps");
    int done = 0;
    for (size_t i = 0; i < steps.size(); i++) {
        const Step& st = steps[i];
        ImGui::PushID(int(i));
        bool v = st.done;
        bool go = ImGui::Checkbox("##done", &v);  // shows the tick; it sets itself, so a click just goes there
        ImGui::SameLine();
        char label[96];
        snprintf(label, sizeof label, "%zu. %s", i + 1, st.label);
        if (st.done) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        go |= ImGui::Selectable(label);
        if (st.done) ImGui::PopStyleColor();
        if (st.test) tip(("Saves, then starts the game straight into this scenario (" + key_text(settings_.keys[A_TEST][0]) + "). "
                          "Ticks itself once you have tested.").c_str());
        else tip(st.tip);
        if (go) {
            if (st.test) cmd_test();
            else {
                tab_ = st.tab;
                if (tool_ != Tool::Select && tool_ != Tool::PickPoint) tool_ = Tool::Select;
            }
        }
        done += st.done;
        ImGui::PopID();
    }
    if (s.multiplayer) {
        bool spawns = false;
        for (auto& o : s.world.objs) if (o.cls == K_LEVELSTART) { spawns = true; break; }
        if (spawns) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Spawn points for each team are already placed (Starts tool).");
            ImGui::PopStyleColor();
        }
    }
    if (done == int(steps.size())) ImGui::TextWrapped("All done. Keep building, or hide this list.");
    if (ImGui::SmallButton("Hide next steps")) ns_on_ = false;
    tip("Hides this list for this map. Help > Getting Started has the same steps.");
}

// Several objects selected: what they are, the actions that work on all of them, and the settings they share
// (a dash when they differ). Changing one sets it on every selected object, as one undo step.
void App::ui_inspector_multi(const std::vector<int>& roots) {
    Scenario& s = *doc_;
    static const uint32_t K_ROAD_MULTI = key_of("MARoad");
    const auto& names = gd_.names();
    ImGui::Text("%zu objects selected", roots.size());
    {
        std::map<std::string, int> kinds;
        for (int r : roots) {
            const WObj& o = s.world.objs[size_t(r)];
            std::string k = o.cls == K_LEVELSTART && s.multiplayer ? "spawn point" : kind_label(names.label(o.cls));
            kinds[k.empty() ? names.label(o.cls) : k]++;
        }
        std::string line;
        int shown = 0;
        for (auto& [k, n] : kinds) {
            if (++shown > 6) { line += ", ..."; break; }
            line += (line.empty() ? "" : ", ") + std::to_string(n) + " " + k + (n > 1 && k.back() != 's' ? "s" : "");
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::PopTextWrapPos();
        // Roads, bases, walls and bridges are sized one at a time.
        bool any_sizable = false;
        int looked = 0;
        for (size_t i = 0; i < roots.size() && looked < 40 && !any_sizable; i++) {
            const WObj& so = s.world.objs[size_t(roots[i])];
            if (so.cls != K_ROAD_MULTI && so.cls != key_of("MACorporeal") && so.cls != key_of("MABuilding")) continue;
            looked++;
            const SizeKind k = size_info(so).kind;
            any_sizable = k == SizeKind::Road || k == SizeKind::Plate || k == SizeKind::Run;
        }
        if (any_sizable) hint_item("Select one piece to size it.", "sizemultitext");
    }
    ImGui::BeginDisabled(s.read_only);
    if (ImGui::Button(act_text(A_SNAP).c_str())) snap_selection_to_ground();
    tip(("Put them all back on the ground (" + kt(A_SNAP) + ").").c_str());
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) duplicate_selected();
    tip(("A copy of each hangs on the mouse: click the map to put it down (" + kt(A_DUPLICATE) + ").").c_str());
    ImGui::SameLine();
    if (theme::danger_button("Delete")) { delete_selection(); ImGui::EndDisabled(); return; }
    tip(("Delete them all (" + kt(A_DELETE) + "). " + kt(A_UNDO) + " undoes.").c_str());
    ImGui::SameLine();
    ImGui::EndDisabled();
    if (ImGui::Button("Frame")) zoom_to_selection();
    tip(("Bring the camera to them (" + kt(A_ZOOM_SEL) + ").").c_str());
    ImGui::BeginDisabled(s.read_only);
    std::vector<uint32_t> uids;
    for (int r : roots) uids.push_back(s.world.objs[size_t(r)].uid());
    auto obj = [&](uint32_t u) { return object_by_uid(s.world, u); };
    // Team: every selected object is a multiplayer spawn point.
    bool all_spawns = s.multiplayer;
    for (int r : roots) all_spawns &= s.world.objs[size_t(r)].cls == K_LEVELSTART;
    if (all_spawns) {
        int team = s.world.objs[size_t(roots[0])].i32("TeamIndex");
        bool mixed = false;
        for (int r : roots) mixed |= s.world.objs[size_t(r)].i32("TeamIndex") != team;
        static const char* teams[] = {"Free-for-all", "Red", "Blue"};
        form_label("Team", form_col());
        if (begin_combo("##team", mixed ? "-" : teams[std::clamp(team, 0, 2)])) {
            for (int k = 0; k < 3; k++)
                if (ImGui::Selectable(teams[k], !mixed && team == k)) {
                    push_undo("team");
                    for (uint32_t u : uids) if (WObj* o = obj(u)) o->set_i32("TeamIndex", k);
                    s.dirty = true;
                    objects_dirty_ = true;
                }
            ImGui::EndCombo();
        }
        tip("Which team starts at these spawn points. A dash means they differ.");
    }
    // Side: mission units that have one (not helpers, not the player start).
    std::vector<uint32_t> sided;
    for (int r : roots) {
        const WObj& o = s.world.objs[size_t(r)];
        if (o.find("Nation") && !s.multiplayer && o.cls != K_LEVELSTART && helper_group(o.cls) < 0) sided.push_back(o.uid());
    }
    if (!sided.empty()) {
        auto side_of = [&](uint32_t u) { int n = obj(u)->i32("Nation"); return n == 6 ? 0 : n == 5 ? 1 : n == 0 ? 2 : 3; };
        int idx = side_of(sided[0]);
        bool mixed = false;
        for (uint32_t u : sided) mixed |= side_of(u) != idx;
        static const char* sides[] = {"Player's side", "Enemy", "Neutral", "Other"};
        static const int values[] = {6, 5, 0};
        std::string label = "Side" + (sided.size() < roots.size() ? " (" + std::to_string(sided.size()) + " of them)" : std::string());
        form_label(label.c_str(), form_col());
        if (begin_combo("##multiside", mixed ? "-" : sides[idx])) {
            for (int k = 0; k < 3; k++)
                if (ImGui::Selectable(sides[k], !mixed && idx == k)) {
                    push_undo("side");
                    for (uint32_t u : sided) io_->apply_unit_side(s, u, values[k]);  // may add children: only uids are kept
                    s.dirty = true;
                    objects_dirty_ = minimap_dirty_ = true;
                }
            ImGui::EndCombo();
        }
        tip("Enemies attack the player; neutral units are left alone. Only the selected units that have a side change. A dash means they differ.");
    }
    // Visible: shared by every placed object.
    bool all_vis = true;
    for (int r : roots) all_vis &= s.world.objs[size_t(r)].find("VisibleEntity") != nullptr;
    if (all_vis) {
        int on = 0;
        for (int r : roots) on += s.world.objs[size_t(r)].i32("VisibleEntity") != 0;
        bool v = on == int(roots.size());
        if (on > 0 && on < int(roots.size())) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("Visible", &v)) {
            push_undo("visible");
            for (uint32_t u : uids) if (WObj* o = obj(u)) o->set_i32("VisibleEntity", v ? 1 : 0);
            s.dirty = true;
            objects_dirty_ = true;
        }
        if (on > 0 && on < int(roots.size())) ImGui::PopItemFlag();
        tip("Shown in the game from the start (untick: hidden until a trigger shows it).");
    }
    // Heading (sets every object's; "mixed" when they differ) and Position (the group's centre: typing moves the group by the difference).
    {
        float h0 = world::heading(s.world.objs[size_t(roots[0])]), hs[1] = {0};
        bool mixed = false;
        for (int r : roots) {
            const float d = std::remainder((world::heading(s.world.objs[size_t(r)]) - h0) * 57.2958f, 360.f);
            mixed |= std::fabs(d) > 0.01f;
        }
        hs[0] = heading_display(h0 * 57.2958f);  // 0..359 like every other heading
        form_label("Heading", form_col());
        const bool turned = ImGui::DragFloat("##mheading", &hs[0], 0.5f, 0.f, 0.f, mixed ? "mixed" : "%.2f deg");
        ImGui::SetItemTooltip("Sets the heading of every selected object. Drag, or double-click to type.");
        static bool mhead_armed = false;
        if (ImGui::IsItemActivated()) mhead_armed = true;
        if (turned && mhead_armed) { push_undo("heading"); mhead_armed = false; }
        if (turned) {
            for (uint32_t u : uids)
                if (WObj* o = obj(u)) world::set_heading(*o, std::remainder(hs[0], 360.f) / 57.2958f);
            s.dirty = true;
            objects_dirty_ = true;
        }
        Vec3 gc;
        if (gizmo_pivot(gc)) {
            float f[3] = {gc.x, gc.y, gc.z};
            form_label("Position", form_col());
            const float fw = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemInnerSpacing.x) / 3.f;
            static const char* kAxis[3] = {"##mpx", "##mpy", "##mpz"};
            static const char* kFmt[3] = {"x %.2f", "h %.2f", "z %.2f"};
            bool moved = false, started = false;
            ImGui::BeginGroup();
            for (int k = 0; k < 3; k++) {
                if (k) ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::SetNextItemWidth(fw * (k == 1 ? 1.3f : 0.85f));
                moved |= ImGui::DragFloat(kAxis[k], &f[k], 0.25f, 0, 0, kFmt[k]);
                started |= ImGui::IsItemActivated();
                ImGui::SetItemTooltip("The centre of the group. Drag, or double-click to type: the whole group moves by the difference.");
            }
            ImGui::EndGroup();
            static bool mpos_armed = false;
            if (started) mpos_armed = true;
            if (moved && mpos_armed) { push_undo("position"); mpos_armed = false; }
            if (moved) {
                const Vec3 d{f[0] - gc.x, f[1] - gc.y, f[2] - gc.z};
                if (std::fabs(d.y) > 1e-6f) mark_selection_fixed();
                move_selection(d, false);
                s.dirty = true;
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Click one of them alone to see all its settings.");
}

static std::string fx5_stem(const std::string& path) {
    size_t s = path.find_last_of("/\\"), d = path.rfind('.');
    std::string n = path.substr(s == std::string::npos ? 0 : s + 1);
    d = n.rfind('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

void App::ui_inspector() {
    spawner_ui_done_ = false;  // 
    if (!doc_) return;
    ImGui::SeparatorText("Properties");
    auto roots = selected_roots();
    // The Next steps card is hidden while something is selected, and what was just placed stays selected:
    // say that step 2 is done.
    if (ns_on_ && editable() && doc_->multiplayer && !ns_placed_said_ && !roots.empty() && ns_things() > ns_base_) {
        ns_placed_said_ = true;
        status_ = "Next step done: 2. Place 'Mechs, vehicles and powerups. Press " + kt(A_CANCEL) + " (deselect) to see the list.";
        status_time_ = 8;
    }
    if (!roots.empty() && editable() && (tool_ == Tool::PlaceObject || tool_ == Tool::ScatterTrees)) {   // Say so while a new piece is armed
        std::string arm;
        if (tool_ == Tool::ScatterTrees) arm = "the scatter brush";
        else if (!place_road_.empty()) arm = "road piece " + fx5_stem(place_road_);
        else if (const TemplateInfo* pt = cat_ ? cat_->find(place_template_) : nullptr) arm = palette_plain_name(*pt);
        if (!arm.empty()) hint_text(("Placing " + arm + ". What is shown below is the piece already selected.").c_str());
    }
    if (roots.empty()) {
        if (ns_on_ && editable()) {
            ui_next_steps();
            ImGui::Spacing();
        }
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextWrapped("Click an object on the map to see and change it here. Place new ones with the Place tool.");
        ImGui::Spacing();
        glue_tool_ui();
        return;
    }
    if (roots.size() > 1) {
        ui_inspector_multi(roots);
        return;
    }
    Scenario& s = *doc_;
    g_link_io = io_.get();
    g_link_names = &gd_.names();
    g_link_cat = cat_.get();
    g_link_gd = &gd_;
    g_link_doc = doc_.get();
    static int inspect_child = -1;
    static uint32_t last_root = 0;
    WObj* root = &s.world.objs[size_t(roots[0])];
    if (root->uid() != last_root) { inspect_child = -1; last_root = root->uid(); }
    WTree t;
    t.build(s.world);
    std::vector<int> sub;
    t.subtree(roots[0], sub);
    int idx = inspect_child >= 0 && std::find(sub.begin(), sub.end(), inspect_child) != sub.end() ? inspect_child : roots[0];
    // A reinforcement holder (NULL corporeal > spawner > unit, "Make it reinforcements"): show the unit, which has the
    // Reinforcements settings; the holder itself is an invisible logic object.
    if (idx == roots[0] && s.world.objs[size_t(idx)].template_id() == 32093) {
        static const uint32_t K_SPAWNER = key_of("MASpawner");
        for (int k : t.kids[size_t(idx)])
            if (s.world.objs[size_t(k)].cls == K_SPAWNER && !t.kids[size_t(k)].empty()) { idx = t.kids[size_t(k)].front(); break; }
    }
    // A vehicle pad (stand > spawner > the 'Mech or vehicle waiting on it, players.cpp add_pad): shown as one thing,
    // "Vehicle pad", with the vehicle's settings; the stand and spawner stay under Parts.
    const uint32_t pad_v = pad_vehicle(s.world.objs[size_t(idx)].uid());
    const bool pad = pad_v != 0;
    if (pad && idx == roots[0]) idx = t.find(pad_v);
    WObj& o = s.world.objs[size_t(idx)];
    const auto& names = gd_.names();
    const TemplateInfo* ti = cat_->find(o.template_id());
    {
        // Plain kind (with what the dictionary knows about it on hover), then the object type.
        std::string kind = kind_label(names.label(o.cls));
        if (o.cls == K_LEVELSTART && s.multiplayer) kind = "spawn point";  // the Starts tool calls them that
        if (pad) kind = "vehicle pad";
        if (kind == "building" && ti) {   // Walls, bridges and bases are MABuilding in the game, but they are not "buildings" to the user
            const int fam = snap_family(*ti);
            if (fam == 1) kind = "wall";
            else if (fam == 2) kind = "bridge";
            else if (fam == 3) kind = "base";
        }
        if (kind == "AI path point")  // a cutscene camera path's point (drawn teal, rebuild_objects)
            for (auto& v : views_) if (v.uid == o.uid() && v.helper == HG_CAMERAS) { kind = "camera path point"; break; }
        if (!kind.empty()) kind[0] = char(toupper((unsigned char)kind[0]));
        ImGui::Text("%s", kind.c_str());
        const DictClass* dc = dict_.cls(o.cls);
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            if (dc && !dc->description.empty()) ImGui::TextUnformatted(dc->description.c_str());
            ImGui::TextDisabled("Game class: %s", names.label(o.cls).c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        // Only what the editor draws on the map (areas, lights, markers, paths, cameras); a trigger or action header carries none.
        if (const int hg = helper_group(o.cls); hg >= 0 && hg != HG_OTHER) { ImGui::SameLine(); ImGui::TextDisabled("(a marker: not drawn in the game)"); }
    }
    if (ti && o.cls != K_WATER) {
        // What it is in plain words; the game's own name on hover (no internal ids in the main view).
        std::string pn = palette_plain_name(*ti), suffix = " (" + ti->name + ")";
        if (pn.size() > suffix.size() && pn.compare(pn.size() - suffix.size(), suffix.size(), suffix) == 0) pn.resize(pn.size() - suffix.size());
        ImGui::TextDisabled("%s%s", pad ? "Vehicle waiting on it: " : "", pn.c_str());
        if (pn != ti->name) ImGui::SetItemTooltip("Game name: %s", ti->name.c_str());
    }
    ImGui::BeginDisabled(s.read_only);
    bool changed = false;
    std::string desc = o.description();
    float wcw = 0, wcl = 0;
    if (o.cls == K_WATER && ScenarioIO::water_custom_size(o, wcw, wcl)) {
        // Water keeps its size in its name ("Lake [200 x 120 m]"): edit only the name part.
        std::string wn = ScenarioIO::water_custom_name(o, ti ? ti->name : std::string());
        form_label("Name", form_col());
        if (ImGui::InputText("##name", &wn)) {
            push_undo_once("rename");
            o.set_str("Description", ScenarioIO::water_description(wn, wcw, wcl, ScenarioIO::water_custom_shape(o)));
            changed = true;
        }
    } else {
        form_label("Name", form_col());
        if (ImGui::InputText("##name", &desc)) { push_undo_once("rename"); o.set_str("Description", desc); changed = true; }
    }
    // A vehicle pad shows the vehicle's settings but moves and turns as a whole: Position and Heading belong to the pad (its stand), with the
    // vehicle and spawner riding along (typing a height used to move only the vehicle, burying it in its own pad).
    const int pidx = pad ? roots[0] : idx;
    WObj& po = s.world.objs[size_t(pidx)];
    if (WProp* pp = position(po)) {
        Vec3 p = pp->as_vec3();
        // Shown at the middle of the model's base (where the gizmo sits and where a click places it), not at the
        // model origin, which can be far off (Wide bridge 01: 42 m away, 35 m up). The origin is under Advanced settings.
        const Vec3 fo = footprint_offset(pidx);
        float f[3] = {p.x + fo.x, p.y + fo.y, p.z + fo.z};
        // Three labelled fields (three bare numbers): East / Height / North, like the status bar's x / z.
        bool moved = false, started = false;
        // Labels left like every other row; when the panel is too narrow for three readable boxes ("x 461.60" was cut), the label
        // column shrinks for this row so each box keeps its whole number.
        form_label("Position", ImGui::GetContentRegionAvail().x - form_col() < 3 * 74 * dpi_ ? 60 * dpi_ : form_col());
        // Water: x and z only; its height is the Level row below (it was shown twice).
        const bool no_h = o.cls == K_WATER;
        const float fw = (ImGui::GetContentRegionAvail().x - (no_h ? 1 : 2) * ImGui::GetStyle().ItemInnerSpacing.x) /
                         (no_h ? 1.7f : 3.f);
        static const float kShare[3] = {0.85f, 1.3f, 0.85f};  // "height 115.7" needs more room than "x 384.0"
        static const char* kAxis[3] = {"##px", "##py", "##pz"};
        static const char* kFmt[3] = {"x %.2f", "h %.2f", "z %.2f"};  // 'height' did not fit next to the label column; centimetres, so a layer 3 cm up shows
        static const char* kTip[3] = {"East-west position (m), as x in the status bar.", "Height (m) of the base of the model.",
                                      "North-south position (m), as z in the status bar."};
        ImGui::BeginGroup();
        for (int k = 0; k < 3; k++) {
            if (no_h && k == 1) continue;
            if (k) ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(fw * kShare[k]);
            const bool k_moved = ImGui::DragFloat(kAxis[k], &f[k], 0.25f, 0, 0, kFmt[k]);
            moved |= k_moved;
            started |= ImGui::IsItemActivated();
            if (k == 1) {
                // The height box has a floor: dragged with the mouse it stops where the object's foot meets the surface under it
                // (a piece already below it can move under where it began); the hold key (Shift) lets it through, and a typed number is
                // taken as it is.
                static float h_floor_y = -1e30f;
                if (ImGui::IsItemActivated()) h_floor_y = std::min(p.y, lift_floor_y(o));
                const bool typing = ImGui::TempInputIsActive(ImGui::GetItemID());
                if (k_moved && !typing && !holding(settings_.hold_free) && f[1] - fo.y < h_floor_y - 1e-4f) {
                    f[1] = h_floor_y + fo.y;
                    status_ = std::string("On the ground. Hold ") + mod_name(settings_.hold_free) + " to push it below the ground.";
                    status_time_ = 3;
                }
            }
            ImGui::SetItemTooltip("%s Drag, or double-click to type.", kTip[k]);
        }
        ImGui::EndGroup();
        where_it_sits_ui(po, pidx, f[1]);  // what it stands on: "On the ground", "0.15 m above Building base 3", or the layer of a flat piece
        // One undo step per real change: a double-click (to type a number) activates the field twice, and a
        // plain click that changes nothing should not leave an empty undo step either.
        static bool pos_undo_armed = false;
        if (started) pos_undo_armed = true;
        if (moved && pos_undo_armed) { push_undo("position"); pos_undo_armed = false; }
        if (moved) {
            Vec3 d{f[0] - fo.x - p.x, f[1] - fo.y - p.y, f[2] - fo.z - p.z};
            // Move the whole object (attached parts too).
            std::set<uint32_t> keep = selection_;
            selection_ = {s.world.objs[size_t(roots[0])].uid()};
            if (std::fabs(d.y) > 1e-6f) mark_selection_fixed();  // a flat piece lifted by typing keeps that height
            if (pidx == roots[0]) move_selection(d, false);
            else pp->set_vec3({p.x + d.x, p.y + d.y, p.z + d.z});
            selection_ = keep;
            changed = true;
        }
        // Shown as -180..180 (retail objects store headings such as 630 deg).
        // Shown as 0..360 like the status line and the placing panel (retail objects store headings such as 630 deg).
        float h = heading_display(world::heading(po) * 57.2958f);
        form_label("Heading", form_col());
        bool turned = ImGui::DragFloat("##heading", &h, 0.5f, 0.f, 0.f, "%.2f deg");  // 0.5 degree per pixel, wraps to -180..180, double-click types
        ImGui::SetItemTooltip("Drag, or double-click to type.");
        if (turned) h = heading_display(h);
        static bool head_undo_armed = false;  // as Position: one undo step per real change
        if (ImGui::IsItemActivated()) head_undo_armed = true;
        if (turned && head_undo_armed) { push_undo("heading"); head_undo_armed = false; }
        if (turned) {
            if (pad) {
                Vec3 pv;
                if (get_pos(po, pv)) turn_root_about(po.uid(), h / 57.2958f - world::heading(po), pv.x, pv.z);
            } else world::set_heading(o, h / 57.2958f);
            changed = true;
        }
        // The slope buttons come right after Heading: they were below the fold at 1280 x 720; the stacking and crop rows follow.
        slope_ui(o);        // when a piece floats over a slope: Level the ground under it / Tilt it (app_slope.cpp)
        road_ground_ui(o);  // a tilted or floating road piece: no collision, Fit ground to road (app_glue.cpp)
        sheet_layer_ui(o);  // road pieces and thin plates: where it sits in the stack, Bring forward / Send back (app_sheets.cpp)
        size_ui(o);         // Size: the cut of a base, wall or bridge, or why it has none (app_crop.cpp)
        if (o.cls == K_WATER) {
            // Water inspector: Level, Type (picture tiles), size + Oval; unit settings (side, boarding, low detail) are hidden.
            if (water_can_resize(o.template_id())) hint_text("Drag the orange handles on the map to size it, the blue knob to set its level.");   // 
            float y = p.y;
            form_label("Level", form_col());
            if (ImGui::DragFloat("##selevel", &y, 0.1f, s.terrain.min_h - 50, s.terrain.max_h, "%.1f m")) {
                push_undo_once("water level");
                set_pos(o, {p.x, y, p.z});
                changed = true;
            }
            tip("Height of the water surface. Ground higher than this hides the water.");
        }
        if (o.cls == K_WATER && water_can_resize(o.template_id())) {
            // Selected water: its size and shape here too, not only in the Terrain tab list (small ponds to big lakes).
            float cw, cl;
            water_size_of(o, nullptr, cw, cl);
            float w = cw, l = cl;
            bool sized = water_size_row("##selw", "##sell", w, l, form_col());
            if (sized) { push_undo_once("water size"); set_water_size(o, w, l); }
            bool oval = ScenarioIO::water_custom_shape(o) == wcustom::Shape::Oval;
            if (ImGui::Checkbox("Oval (a round pond)##selov", &oval)) {
                push_undo("water shape");
                set_water_size(o, cw, cl, int(oval ? wcustom::Shape::Oval : wcustom::Shape::Rect));
            }
            // Water moved onto flat or higher ground (or a retail water without a bed): dig its bowl at its level.
            const bool dig_clicked = ImGui::Button("Dig a basin under it");
            const bool dig_hover = ImGui::IsItemHovered();
            tip(("Lowers the ground under this water so it has depth (ground already lower stays) and marks it as water ground. Other buildings and flat pieces keep their ground. A basin dug before is filled in first. " + kt(A_UNDO) + " undoes it.").c_str());
            if (dig_hover) {  // what it will change, drawn on the map and said in one line
                BasinFoot bf;
                if (water_foot(o, bf)) {
                    ground_preview_ = basin_preview_edit(bf, &o);
                    ground_preview_frame_ = ImGui::GetFrameCount();
                    ground_preview_text_ = ground_edit_note(ground_preview_);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.86f, 0.3f, 1.f));
                    ImGui::TextWrapped("%s", ground_preview_text_.c_str());
                    ImGui::PopStyleColor();
                }
            }
            if (dig_clicked) {
                push_undo("dig basin");
                basin_kept_.clear();
                const bool again = o.find(place::kBasinProp) != nullptr;
                const int moved = make_basin(o, true, false, "dig basin");
                status_ = std::string(again ? "The basin was filled and dug again under this water." : "A basin was dug under this water.") +
                          (moved > 0 ? " " + std::to_string(moved) + " object" + (moved == 1 ? " rides" : "s ride") + " with the bed." : std::string()) +
                          (basin_kept_.empty() ? std::string() : " It stops short of " + basin_kept_ + ".") + " " + kt(A_UNDO) + " undoes it.";
                status_time_ = 8;
                s.dirty = true;
                minimap_dirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Mark as water ground")) {
                push_undo("water ground");
                int n = mark_water_ground(p, cw, cl, oval, world::heading(o));
                status_ = n ? "The ground under this water is now water ground (blue in Paint > Show where players may go)." : "Nothing to mark: no playable ground under the water lies below its surface.";
                status_time_ = 6;
            }
            tip("In the game, players and bots wade where the ground is marked as water (the map's ground types), not where the water model is. "
                "New water does this by itself; use this after moving or resizing it. Blue in Paint > Show where players may go.");
        }
        // The look (picture tiles) is the long part: it comes after the size and basin buttons, which were below the fold.
        if (o.cls == K_WATER && water_look_ui(o)) { ImGui::EndDisabled(); return; }  // replaced: `o` is gone
        if (o.cls != K_WATER) {
            if (ImGui::Button(act_text(A_SNAP).c_str())) snap_selection_to_ground();
            tip(("Put it back on the ground (" + kt(A_SNAP) + ").").c_str());
            ImGui::SameLine();
        }
        if (theme::danger_button("Delete")) { delete_selection(); ImGui::EndDisabled(); return; }
        tip(("Delete it (" + kt(A_DELETE) + "). " + kt(A_UNDO) + " undoes.").c_str());
    }
    // Left out of the map view (View > Objects the mission shows later is off): say so, else a selected unit showed only
    // the move arrows on an empty street (review M01b "tank cinema 1": Arrives said "At the start").
    if (!show_later_)
        if (auto it = start_state_.find(o.uid()); it != start_state_.end()) {
            const char* why = it->second == START_SPAWNED    ? "a spawner sends it in later"
                              : it->second == START_REMOVED  ? "the mission removes it when play starts"
                              : it->second == START_CUTSCENE ? "it is only used in a cutscene"
                                                             : "the mission hides it when play starts, until a trigger shows it";
            hint_wrapped((std::string("Not drawn on the map: ") + why + ".").c_str());
            ImGui::EndDisabled();  // a view setting: works on a real level too
            if (ImGui::Button("Show it on the map")) {
                show_later_ = true;
                objects_dirty_ = true;
            }
            tip("Turns on View > Objects the mission shows later, so this and the other later objects are drawn.");
            ImGui::BeginDisabled(s.read_only);
        }
    // A mission's player start has no team or side to pick: the player is always the player.
    if (o.cls == K_LEVELSTART && s.multiplayer) {
        int team = o.i32("TeamIndex");
        static const char* teams[] = {"Free-for-all", "Red", "Blue"};
        form_label("Team", form_col());
        if (ImGui::Combo("##team", &team, teams, 3)) { push_undo("team"); o.set_i32("TeamIndex", team); changed = true; }
        tip("Which team starts here. Free-for-all is for games without teams.");
    }
    // Multiplayer: turrets and soldiers belong to a team (Nation 1 Red, 2 Blue, 0 none); 'Mechs and vehicles always stand empty.
    static const uint32_t K_TURRET_ = key_of("MATurret"), K_PERSON_ = key_of("MAPerson");
    if (o.find("Nation") && s.multiplayer && (o.cls == K_TURRET_ || o.cls == K_PERSON_)) {
        int n = o.i32("Nation");
        std::string other = "Other (nation " + std::to_string(n) + ")";
        const char* teams[] = {"Red team", "Blue team", "Neutral", other.c_str()};
        int idx = n == 1 ? 0 : n == 2 ? 1 : n == 0 ? 2 : 3;
        // A value outside the list (a mission unit copied in, a developer flag) is shown, not drawn as a blank box.
        form_label("Side", form_col());
        if (ImGui::Combo("##side", &idx, teams, idx == 3 ? 4 : 3) && idx != 3) {
            static const int values[] = {1, 2, 0};
            push_undo("side");
            uint32_t u = o.uid();
            io_->apply_unit_side(s, u, values[idx]);
            s.dirty = true;
            objects_dirty_ = minimap_dirty_ = true;
            ImGui::EndDisabled();
            return;
        }
        tip("The team this turret or soldier fights for (the ring under it on the map shows the team colour).");
    }
    // Say why a multiplayer 'Mech or vehicle has no Side, instead of showing nothing.
    {
        static const uint32_t K_MECH_ = key_of("MAMech"), K_TANK_ = key_of("MATank"), K_VEH_ = key_of("MAVehicle"), K_FLY_ = key_of("MAFlyer");
        if (s.multiplayer && (o.cls == K_MECH_ || o.cls == K_TANK_ || o.cls == K_VEH_ || o.cls == K_FLY_)) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Team: none (it stands empty)");
            tip("On a multiplayer map 'Mechs and vehicles belong to no team: they wait empty for any player or bot to climb in. "
                "To give a team its own 'Mech, use a vehicle pad (Starts tool).");
        }
    }
    if (!s.multiplayer && has_side(o)) {
        int n = o.i32("Nation");
        std::string other = "Other (nation " + std::to_string(n) + ")";
        const char* sides[] = {"Player's side", "Enemy", "Neutral", other.c_str()};
        int idx = n == 6 ? 0 : n == 5 ? 1 : n == 0 ? 2 : 3;
        form_label("Side", form_col());
        if (ImGui::Combo("##side", &idx, sides, idx == 3 ? 4 : 3) && idx != 3) {
            static const int values[] = {6, 5, 0};
            push_undo("side");
            uint32_t u = o.uid();
            io_->apply_unit_side(s, u, values[idx]);  // may add a child: `o` is not used after this
            s.dirty = true;
            objects_dirty_ = minimap_dirty_ = true;
            ImGui::EndDisabled();
            return;
        }
        tip("Enemies attack the player; neutral units are left alone.");
        static const uint32_t vehicles[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer")};
        if (n == 0 && std::find(std::begin(vehicles), std::end(vehicles), o.cls) != std::end(vehicles)) {
            if (io_->can_board(o.template_id())) {
                bool on = io_->boardable(s, o.uid());
                if (ImGui::Checkbox("Player can climb in", &on)) {
                    push_undo("climb in");
                    io_->set_boardable(s, o.uid(), on);  // may add or remove a child: `o` is not used after this
                    s.dirty = true;
                    objects_dirty_ = true;
                    ImGui::EndDisabled();
                    return;
                }
                tip("Ticked: the 'Mech or vehicle stands empty and the player can walk up and climb in (press Q next to it). Unticked: it stands empty and locked.");
                if (on) hint_wrapped("In the game: walk up and tap Q (holding Q does nothing).");
            } else {
                hint_wrapped("The player cannot drive this one (no real level lets them).");
            }
        }
        // Starts hidden: the retail reinforcement pattern (Mission_05): a 0-second start-up timer hides the unit
        // (MAActionShow Enable 0); a later trigger's "Show or hide units" brings it in.
        static const uint32_t side_units[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"),
                                              key_of("MATurret"), key_of("MAPerson")};
        static const uint32_t K_SHOW = key_of("MAActionShow"), K_TIMER = key_of("MATriggerTimer");
        if (std::find(std::begin(side_units), std::end(side_units), o.cls) != std::end(side_units)) {
            Wdf& w = s.world;
            uint32_t uid = o.uid();
            std::vector<uint32_t> hiders;  // hide actions on 0-second timers that point at this unit
            for (auto& ub : used_by(w, uid, [&](uint32_t c) { return names.label(c).rfind("MATrigger", 0) == 0; })) {
                if (ub.kind != UsedBy::Action) continue;
                const WObj* a = object_by_uid(w, ub.holder);
                const WObj* t = object_by_uid(w, ub.trigger);
                if (a && t && a->cls == K_SHOW && a->i32("Enable") == 0 && t->cls == K_TIMER && t->f32("TrgTimerDurationSeconds") <= 0.01f)
                    hiders.push_back(ub.holder);
            }
            bool hidden = !hiders.empty();
            // Reinforcements, retail pattern B: the unit sits in a spawner that a trigger switches on (157 of the 167
            // campaign spawners). Such a unit does not exist until then, so "Starts hidden" does not apply to it.
            static const uint32_t K_SPAWNER = key_of("MASpawner");
            int oi = t.find(uid), pi = oi >= 0 ? t.parent_of[size_t(oi)] : -1;  // (idx here is the Side combo's)
            WObj* sp = pi >= 0 && w.objs[size_t(pi)].cls == K_SPAWNER ? &w.objs[size_t(pi)] : nullptr;
            // One "Arrives" row for both reinforcement patterns; a spawner unit gets it from ui_spawner.
            if (!sp && ui_unit_arrives(uid, hiders)) {
                ImGui::EndDisabled();
                return;
            }
            // A unit that is now a reinforcement keeps its Arrives row (and the Delay box under it) where the row was,
            // right under Side, not in a section far below.
            if (sp) { ui_spawner(uid); spawner_ui_done_ = true; }
            if (!sp && ImGui::Checkbox("Starts hidden", &hidden)) {
                push_undo("starts hidden");
                if (hidden) {
                    uint32_t timer = 0;
                    for (auto& t : w.objs)
                        if (t.cls == K_TIMER && t.description() == "Hidden at start") { timer = t.uid(); break; }
                    if (!timer) timer = add_trigger_object(s, "MATriggerTimer", "Hidden at start");
                    if (uint32_t a = timer ? add_action_object(s, timer, "MAActionShow", "Hide " + desc) : 0)
                        if (WObj* ao = object_by_uid(w, a)) { ao->set_u32("ActionEntity", uid); ao->set_i32("Enable", 0); }
                    // The map view leaves out what the mission hides at the start: keep the unit on screen to work on it.
                    if (!show_later_) {
                        show_later_ = true;
                        status_ = "Hidden at start. View > Objects the mission shows later is now on, so you still see it here.";
                        status_time_ = 8;
                    }
                } else {
                    for (uint32_t h : hiders) wedit::remove_subtree(w, h);
                }
                s.dirty = true;
                objects_dirty_ = true;
                ImGui::EndDisabled();
                return;
            }
            if (!sp) tip("Ticked: the unit waits on the map, hidden, when the mission starts, until a trigger's \"Show or hide units\" brings it in. "
                         "To make it come in when something happens, pick that under Arrives above.");
            if (!sp && hidden) hint_wrapped("Hidden at start. \"Used by\" below lists the trigger that hides it and any that show it.");
            // AI on or off at the start (playtest): retail copies are often saved asleep (woken by a trigger); a placed unit
            // for a side starts on. Shown here, not only under Common settings, because it decides whether the unit acts.
            if (!s.multiplayer && o.i32("Nation") != 0 && o.find("m_bAIEnabled")) {
                bool ai = o.i32("m_bAIEnabled") != 0;
                if (ImGui::Checkbox("AI starts on", &ai)) {
                    push_undo("AI starts on");
                    o.set_i32("m_bAIEnabled", ai ? 1 : 0);
                    s.dirty = true;
                }
                tip("Ticked: the unit moves and fights on its own from the start. Unticked: it stays asleep (still, not fighting, not even when shot) until a trigger "
                    "switches its AI on; giving it orders does that for you. Real levels keep many units asleep until the player gets close.");
            }
        }
    }
    {
        static const uint32_t order_units[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"), key_of("MATurret"), key_of("MAPerson")};
        if (!s.multiplayer && std::find(std::begin(order_units), std::end(order_units), o.cls) != std::end(order_units)) {
            uint32_t u = o.uid();
            size_t before = s.world.objs.size();
            ui_unit_orders(u);
            if (s.world.objs.size() != before) { ImGui::EndDisabled(); return; }  // a new trigger + orders: `o` may have moved
        }
    }
    {
        const std::string cn = names.label(o.cls);
        if (cn.rfind("MATrigger", 0) == 0)
            hint_wrapped("When it fires, the actions in its Parts run. Turn Enabled off to keep it asleep until another trigger switches it on.");
        else if (o.find("ForceUseLowDetail") && o.i32("ForceUseLowDetail") == 0 && o.find("Nation") && !s.multiplayer &&
                 (cn == "MAMech" || cn == "MATank" || cn == "MAVehicle" || cn == "MAFlyer")) {
            // Only when its model really is missing: the editor reads the same full-detail model the game does.
            const GpuModel* gm = models_.get(o.str("Corporal3DObj"), o.str("CorporalMTB"));
            if (gm && gm->state == GpuModel::Failed)
                hint_wrapped("Its 3D model is missing, so the game may show a pink pillar: turn on 'Use low-detail model' under Advanced settings.");
        }
    }
    if (!spawner_ui_done_) ui_spawner(o.uid());
    spawner_ui_done_ = false;
    ui_used_by(o.uid());
    if (sub.size() > 1 && theme::section("Parts")) {
        for (int k : sub) {
            const WObj& c = s.world.objs[size_t(k)];
            std::string l = std::string(k == idx ? "> " : "  ") + names.label(c.cls) + " \"" + c.description() + "\"";
            ImGui::PushID(k);
            if (ImGui::Selectable(l.c_str(), k == idx)) inspect_child = k;
            ImGui::PopID();
        }
    }
    // Every other setting, from the dictionary: Common (open), Advanced, and the ones the game keeps
    // up to date (read-only). Settings shown above, and the links between objects, are left out.
    {
        static std::string pf;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##pf", "Find a setting", &pf);
        std::string lf = lower(pf);
        static const std::set<std::string> shown_above = {"Description", "CorporalPos", "ObjPosition", "TriggerPoint", "RoadPosition", "Position",
                                                          "CameraPosition", "CorporalHeading", "StartOrientation", "VolumeHeading", "Nation", "TeamIndex",
                                                          "RoadRotation"};  // a road piece's turn is the Heading slider, not 12 raw bytes
        static const std::set<std::string> links = {"UID", "parent", "child", "sibling", "TemplateID"};
        static const std::set<std::string> unit_classes = {"MAMech", "MATank", "MAVehicle", "MAFlyer", "MAPerson", "MATurret"};
        static const std::set<std::string> unit_basic = {"m_bAIEnabled"};
        static const uint32_t K_EFFECT_ = key_of("MAEffect");  // Start empty is the "Player can climb in" box above
        const bool unit = unit_classes.count(names.label(o.cls)) > 0;
        // Cameras, paths, markers and cutscene parts: nothing the game keeps is useful to show.
        static const std::set<std::string> cut_parts = {"MAFreeCameraController", "MACameraProperty", "MAWaypointGroup"};
        const int hg = helper_group(o.cls);
        const bool helper = hg == HG_CAMERAS || hg == HG_PATHS || hg == HG_MARKERS || cut_parts.count(names.label(o.cls));
        std::vector<WProp*> tiers[3];  // Common, Advanced, Internal
        for (auto& p : o.props) {
            std::string raw = names.label(p.key);
            if (shown_above.count(raw)) continue;
            const DictProp* d = dict_.prop(p.key);
            // Undecoded bytes (any type WProp::display shows as "N bytes: ..", e.g. CameraRotation): nothing a user can read or change.
            const bool plain = (p.type >= PT_I8 && p.type <= PT_VEC3) || p.type == PT_COLOR || p.type == PT_U8B;
            if (!plain) continue;
            std::string lbl = d && !d->label.empty() ? d->label : raw;
            if (!lf.empty() && lower(lbl).find(lf) == std::string::npos && lower(raw).find(lf) == std::string::npos) continue;
            int tier = links.count(raw) ? 2 : !d ? 1 : d->tier == "Common" ? 0 : d->tier == "Internal" ? 2 : 1;
            if (tier == 0 && !d->known()) tier = 1;
            if (tier == 0 && o.cls == K_WATER) tier = 1;  // water: everything it needs is above
            // Typing over a file path or a name hash can break the object: keep those out of the everyday list.
            if (tier == 0 && (d->type == "cue_id" || d->label == "Model")) tier = 1;
            // Units ('Mechs, vehicles, infantry, turrets): the everyday list is what a designer changes; the
            // engine flags (Battle Armor, jacking, data core, low-detail model, set members...) go under Advanced.
            if (tier == 0 && unit && !unit_basic.count(raw)) tier = 1;
            if (tier == 0 && raw.rfind("SetMember", 0) == 0) tier = 1;  // raw set uids: "Used by" names the set ("Member of the set 'Red Stuff'")
            // Every corporeal inherits the unit flags: boarding means something only on a 'Mech or vehicle,
            // and an effect (fire, smoke, explosion) has no 3D model to swap for a low-detail one. They wait under Advanced.
            static const std::set<std::string> boardable_classes = {"MAMech", "MATank", "MAVehicle", "MAFlyer"};
            if (tier == 0 && raw == "StartAbandoned" && !boardable_classes.count(names.label(o.cls))) tier = 1;
            if (tier == 0 && raw == "ForceUseLowDetail" && o.cls == K_EFFECT_) tier = 1;
            if (tier == 2 && helper) continue;
            tiers[tier].push_back(&p);
        }
        auto sort_by_label = [&](std::vector<WProp*>& v) {
            std::sort(v.begin(), v.end(), [&](WProp* a, WProp* b) {
                const DictProp* da = dict_.prop(a->key);
                const DictProp* db = dict_.prop(b->key);
                std::string la = da && !da->label.empty() ? da->label : names.label(a->key);
                std::string lb = db && !db->label.empty() ? db->label : names.label(b->key);
                return lower(la) < lower(lb);
            });
        };
        static const char* titles[3] = {"Common settings", "Advanced settings", "Kept by the game (read-only)"};
        static const char* help_text[3] = {"The settings a level designer usually changes.",
                                           "Tuning: speeds, damage, AI timings, effects. Hover a setting to see what it does.",
                                           "Values the game keeps up to date or uses to link objects; shown for reference."};
        // A trigger picked on the Objectives & Triggers tab is edited there in plain words; its raw game fields stay folded here.
        const bool trig_left = tab_ == Tab::Triggers && names.label(o.cls).rfind("MATrigger", 0) == 0;
        if (trig_left) ImGui::TextWrapped("Edit this trigger in the Triggers panel on the left. The game's own fields are folded below for reference.");
        for (int tier = 0; tier < 3; tier++) {
            if (tiers[tier].empty()) continue;
            sort_by_label(tiers[tier]);
            std::string head = std::string(titles[tier]) + " (" + std::to_string(tiers[tier].size()) + ")###tier" + std::to_string(tier);
            if (!theme::section(head.c_str(), (tier == 0 && !trig_left) || !lf.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
            ImGui::TextDisabled("%s", help_text[tier]);
            if (tier == 1)
                if (const WProp* op = position(o)) {  // the raw position the game stores (Properties shows the footprint centre)
                    const Vec3 fo = footprint_offset(idx);
                    if (fo.x * fo.x + fo.y * fo.y + fo.z * fo.z > 0.25f) {
                        const Vec3 q = op->as_vec3();
                        ImGui::TextDisabled("Model origin (stored in the level): %.2f, %.2f, %.2f", q.x, q.y, q.z);
                        ImGui::SetItemTooltip("Position above is the middle of the model's base. This model's own origin is %.1f m away from it.",
                                              std::sqrt(fo.x * fo.x + fo.y * fo.y + fo.z * fo.z));
                    }
                }
            for (WProp* p : tiers[tier]) {
                ImGui::PushID(int(p->key));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                WProp before = *p;
                // A trigger's unit-type ticks (mechs, tanks...) do nothing unless "Filter by vehicle type" is on.
                static const std::set<std::string> vehicle_flags = {"Avatar", "BattleArmor", "Mech", "Tank", "Turret", "VTOL"};
                const WProp* cv = o.find("CheckVehicleType");
                bool idle_flag = cv && cv->as_int() == 0 && vehicle_flags.count(names.label(p->key));
                ImGui::BeginDisabled(idle_flag);
                bool edited = false;
                if (tier == 0 && names.label(p->key) == "RoadDefinition" && o.cls == key_of("MARoad") && !road_pieces().empty()) {
                    // A road piece swaps for another prefab from a list (the .road path is the piece's whole identity).
                    std::string cur = p->display();
                    if (cur.size() >= 2 && cur.front() == '"') cur = cur.substr(1, cur.size() - 2);
                    for (char& c : cur) if (c == '\\') c = '/';
                    const RoadPiece* rp = find_road_piece(cur);
                    // Plain names in the main view ("Long straight (60 m)"); the .road file is on the hover.
                    auto shown = [](const RoadPiece& q) { std::string n = q.friendly; if (!n.empty()) n[0] = char(std::toupper(uint8_t(n[0]))); return n; };
                    form_label("Road piece", form_col());
                    bool open = begin_combo("##roadpiece", rp ? shown(*rp).c_str() : cur.c_str());
                    if (rp && !open) ImGui::SetItemTooltip("Game file: %s", rp->official.c_str());
                    if (open) {
                        for (auto& q : road_pieces()) {
                            bool sel = rp && &q == rp;
                            bool picked = ImGui::Selectable(shown(q).c_str(), sel);
                            ImGui::SetItemTooltip("Game file: %s", q.official.c_str());
                            if (picked && !sel) {
                                std::string def = q.path;
                                for (char& c : def) if (c == '/') c = '\\';
                                // One step before both changes (the piece and its new resting height), so undo puts both back.
                                push_undo("road piece");
                                o.set_str("RoadDefinition", def);
                                changed = true;
                                if (io_) {  // the new piece has another footprint: rest it on the ground again
                                    Vec3 rp0;
                                    if (get_pos(o, rp0)) {
                                        float yaw = heading(o);
                                        rp0.y = io_->road_rest_height(*doc_, def, rp0, yaw);
                                        set_pos(o, rp0);
                                        reseat_sheets();
                                    }
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else
                    edited = edit_prop_dict(*p, dict_.prop(p->key), names.label(p->key), s.world, tier == 2,
                                            idle_flag ? "Has no effect while \"Filter by vehicle type\" is off; turn that on first." : nullptr);
                ImGui::EndDisabled();
                if (edited) {
                    // Undo keeps the value from before the edit (one step per drag or typing run).
                    WProp after = *p;
                    *p = before;
                    push_undo_once("property");
                    *p = after;
                    changed = true;
                    // A shot's length typed here carries its camera path's duration along, like the panel and the strip.
                    static const uint32_t K_SHOTCTRL = key_of("MAFreeCameraController");
                    if (o.cls == K_SHOTCTRL && names.label(p->key) == "CameraDuration") carry_path_duration(s, o.uid(), float(before.as_float()));
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndDisabled();
    if (changed) { s.dirty = true; objects_dirty_ = true; minimap_dirty_ = true; }
}

// The Common settings of an object with plain dictionary widgets (checkboxes, meanings, ranges, object
// pickers), for the Objectives & Triggers tab's kinds that have no hand-made editor: nothing is left Properties-only.
bool App::ui_common_fields(WObj& o) {
    if (!doc_) return false;
    g_link_io = io_.get();
    g_link_names = &gd_.names();
    g_link_gd = &gd_;
    g_link_doc = doc_.get();
    const auto& names = gd_.names();
    static const std::set<std::string> skip = {"Description", "UID", "parent", "child", "sibling", "TemplateID", "TriggerPoint", "TriggerEnabled",
                                               "TriggerAutoReset", "TriggerFired", "TriggerStarted", "NumberOfTimesActivated"};
    std::vector<WProp*> v;
    for (auto& p : o.props) {
        std::string raw = names.label(p.key);
        if (skip.count(raw)) continue;
        const DictProp* d = dict_.prop(p.key);
        if (!d || !d->known() || d->tier != "Common") continue;
        v.push_back(&p);
    }
    std::sort(v.begin(), v.end(), [&](WProp* a, WProp* b) {
        const DictProp* da = dict_.prop(a->key);
        const DictProp* db = dict_.prop(b->key);
        return lower(da->label) < lower(db->label);
    });
    bool changed = false;
    for (WProp* p : v) changed |= ui_field(o, names.label(p->key).c_str());
    if (v.empty()) ImGui::TextDisabled("Nothing else to set; the rest is in Properties.");
    return changed;
}

// One setting of an object by its game name, with its dictionary widget (any tier); false when it has none.
bool App::ui_field(WObj& o, const char* key) {
    if (!doc_) return false;
    g_link_io = io_.get();
    g_link_names = &gd_.names();
    g_link_gd = &gd_;
    g_link_doc = doc_.get();
    WProp* p = o.find(key);
    if (!p) return false;
    ImGui::PushID(int(p->key));
    WProp before = *p;
    g_form_col = form_col();
    bool ch = edit_prop_dict(*p, dict_.prop(p->key), key, doc_->world, doc_->read_only);
    g_form_col = 0;
    if (ch) {
        WProp after = *p;
        *p = before;
        const DictProp* d = dict_.prop(p->key);  // "Undo: Shot length", not "Undo: setting" or the game name
        push_undo_once(d && !d->label.empty() ? d->label : gd_.names().label(p->key));
        *p = after;
        doc_->dirty = true;
        objects_dirty_ = true;
    }
    ImGui::PopID();
    return ch;
}

// ---------------------------------------------------------------------------------------------
// New map

// Esc for a modal dialog: not while typing, and a drop-down list opened inside it takes the first Esc.
static bool esc_closes_modal() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    return g.OpenPopupStack.Size <= 1 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput;
}

// New-map Theme list: '<sky look> (<level>)' (ux\glossary-map-env-paint.md theme table) and a
// sky-coloured swatch (the disc has no level preview pictures). Keyed by the level name, lower
// case, letters and digits only, "mission0" -> "mission".
namespace {
struct ThemeLook { const char* look; ImU32 top, horizon, ground; };
const ThemeLook* theme_look(const std::string& level) {
    static const ThemeLook city{"City night, orange glow", IM_COL32(18, 16, 30, 255), IM_COL32(150, 80, 40, 255), IM_COL32(50, 48, 46, 255)},
        dusk{"Hazy golden dusk", IM_COL32(90, 80, 100, 255), IM_COL32(230, 170, 90, 255), IM_COL32(52, 66, 36, 255)},
        sun{"Clear blue day", IM_COL32(60, 120, 210, 255), IM_COL32(180, 210, 240, 255), IM_COL32(90, 110, 60, 255)},
        nor1{"Dark blue storm clouds", IM_COL32(25, 35, 60, 255), IM_COL32(80, 95, 125, 255), IM_COL32(200, 205, 215, 255)},
        nor2{"White cloudy sky", IM_COL32(150, 160, 175, 255), IM_COL32(225, 228, 235, 255), IM_COL32(205, 210, 220, 255)},
        stormy{"Black thunderstorm clouds", IM_COL32(15, 15, 20, 255), IM_COL32(70, 70, 80, 255), IM_COL32(60, 58, 52, 255)},
        hes1{"Red planet sky", IM_COL32(110, 40, 25, 255), IM_COL32(220, 130, 80, 255), IM_COL32(140, 95, 60, 255)},
        hes2{"Red planet sky, second", IM_COL32(80, 30, 40, 255), IM_COL32(200, 100, 90, 255), IM_COL32(120, 80, 60, 255)},
        front{"Storm front", IM_COL32(45, 50, 60, 255), IM_COL32(140, 140, 130, 255), IM_COL32(80, 85, 60, 255)},
        aurora{"Night with green aurora", IM_COL32(10, 20, 30, 255), IM_COL32(40, 160, 110, 255), IM_COL32(45, 50, 55, 255)},
        sunny{"Sunny day", IM_COL32(70, 130, 215, 255), IM_COL32(200, 220, 240, 255), IM_COL32(110, 105, 80, 255)},
        thunder{"Thunderstorm city", IM_COL32(20, 22, 30, 255), IM_COL32(90, 90, 110, 255), IM_COL32(55, 55, 58, 255)};
    static const std::map<std::string, const ThemeLook*> t = {
        {"mission1b", &city}, {"mission2", &city}, {"mission3", &city}, {"mission4", &city}, {"mission5", &dusk},
        {"mission6a", &sun}, {"mission6b", &sun}, {"mission7", &dusk}, {"mission8", &dusk}, {"mission9a", &nor1},
        {"mission9b", &nor1}, {"mission10", &nor1}, {"mission11a", &nor2}, {"mission11b", &nor2}, {"mission12", &stormy},
        {"mission13a", &stormy}, {"mission13b", &hes2}, {"mission14", &hes1}, {"mission15", &hes1}, {"mission16", &hes2},
        {"mission17a", &hes2}, {"mission17b", &hes2}, {"spdemomission", &sun}, {"boggeddown", &dusk}, {"deliverance", &dusk},
        {"gocommando", &city}, {"illwind", &nor1}, {"inyourface", &front}, {"killingfield", &hes1}, {"mountainhigh", &dusk},
        {"newrivercity", &aurora}, {"nomansland", &nor1}, {"ontherocks", &nor2}, {"overfalls", &dusk}, {"provingground", &hes1},
        {"rushhour", &hes2}, {"undercover", &sunny}, {"witchcauldron", &thunder}};
    std::string k;
    for (char c : level) if (isalnum((unsigned char)c)) k += char(tolower((unsigned char)c));
    if (k.rfind("mission0", 0) == 0) k.erase(7, 1);
    auto it = t.find(k);
    return it == t.end() ? nullptr : it->second;
}
std::string theme_counts_text(int buildings, int bases, int trees) {
    auto n = [](int v, const char* one, const char* many) { return std::to_string(v) + " " + (v == 1 ? one : many); };
    return " - " + n(buildings, "building", "buildings") + ", " + n(bases, "base", "bases") + ", " + n(trees, "tree", "trees");
}
std::string theme_label(const MapInfo& m, const std::string& counts = std::string()) {
    const ThemeLook* l = theme_look(m.name);
    return l ? std::string(l->look) + counts + " (" + pretty_name(m.name) + ")" : pretty_name(m.name) + counts;
}
// Sky gradient over a ground band; with km_w > 0 the map's footprint is drawn to scale on the ground (1 km squares).
// ground (when not 0): the colour the new map's ground really shows (App::theme_ground_rgb), over the hand-picked one.
void theme_swatch(const ThemeLook* l, ImVec2 size, int km_w = 0, int km_h = 0, ImU32 ground = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    ImU32 top = l ? l->top : IM_COL32(60, 60, 70, 255), hor = l ? l->horizon : IM_COL32(120, 120, 130, 255),
          gnd = ground ? ground : l ? l->ground : IM_COL32(70, 80, 60, 255);
    float hy = a.y + size.y * (km_w ? 0.3f : 0.6f);
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, hy), top, top, hor, hor);
    dl->AddRectFilled(ImVec2(a.x, hy), b, gnd);
    if (km_w) {
        float unit = (b.y - hy - 10) / 2.f, cx = (a.x + b.x) * 0.5f, cy = (hy + b.y) * 0.5f;  // 2 km fills the ground band
        ImVec2 p0(cx - unit * km_w * 0.5f, cy - unit * km_h * 0.5f), p1(cx + unit * km_w * 0.5f, cy + unit * km_h * 0.5f);
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 40));
        for (int i = 1; i < km_w; i++) dl->AddLine(ImVec2(p0.x + unit * i, p0.y), ImVec2(p0.x + unit * i, p1.y), IM_COL32(255, 255, 255, 90));
        for (int i = 1; i < km_h; i++) dl->AddLine(ImVec2(p0.x, p0.y + unit * i), ImVec2(p1.x, p0.y + unit * i), IM_COL32(255, 255, 255, 90));
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 220), 0, 0, 2);
    }
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 160));
    ImGui::Dummy(size);
}
}  // namespace

// The objects game type gt needs (flags, bases, data core), copied from a real level that has the type,
// the Base's own when it has it. False when the type needs none or nothing was added.
bool App::add_type_kit(const std::string& gt) {
    if (!doc_ || !io_ || !ScenarioIO::mode_needs_kit(gt)) return false;
    auto maps = io_->maps_with_mode(gt);
    if (maps.empty()) return false;
    std::string src = maps.front();
    for (auto& m : maps)
        if (const MapInfo* mi = gd_.find_map(m); mi && iequals(mi->resource, doc_->base)) { src = m; break; }
    std::string err;
    return io_->add_mode_kit(*doc_, gt, src, &err) > 0;
}

// What a theme level comes with, counted from its culled set (the objects its retail level carries): buildings, bases (foundations
// and paving) and trees; plus its most placed buildings and props as the preview row. Cached per level at first use.
const App::ThemeContent& App::theme_content(const MapInfo& m) {
    std::string key = lower(m.resource);
    if (iends_with(key, ".mgf")) key.resize(key.size() - 4);
    if (auto it = theme_content_.find(key); it != theme_content_.end()) return it->second;
    ThemeContent tc;
    if (cat_) {
        // What a level has = its own culled set plus the common set every level of its kind gets.
        std::set<uint32_t> have(cat_->culled_set(key).begin(), cat_->culled_set(key).end());
        for (uint32_t u : cat_->common_set(m.multiplayer)) have.insert(u);
        for (uint32_t u : have) {
            const TemplateInfo* t = cat_->find(u);
            if (!t || !t->placeable || t->model.empty() || special_category_tip(palette_category(*t))) continue;
            if (is_foundation(*t)) {   // Bases are plates of any class (Building base 34x34 is not an MABuilding), they read "0 bases" before
                tc.bases++;
            } else if (t->cls_name == "MABuilding") {
                if (snap_family(*t) == 3) tc.bases++;
                else if (snap_family(*t) == 0) tc.buildings++;
            } else if (t->cls_name != "MAWater" && in_group(*t, kGroupNatural)) {
                const std::string n = no_street(lower(t->name)), c = lower(t->category);
                if (n.find("tree") != std::string::npos || n.find("bush") != std::string::npos || c.find("trees") != std::string::npos || c.find("bushes") != std::string::npos ||
                    c.find("plants") != std::string::npos)
                    tc.trees++;
            }
        }
        // The preview: the buildings and props the retail world places most (units, helpers, skies and effects skipped).
        if (const Wdf* w = io_->retail_world(m.name)) {
            std::unordered_map<uint32_t, int> count;
            for (auto& o : w->objs)
                if (uint32_t tid = o.template_id()) count[tid]++;
            std::vector<std::pair<int, uint32_t>> order;
            for (auto& [tid, n] : count) order.push_back({n, tid});
            std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            // Buildings first (up to 4), then props (the rest, 6 in all); nameless placeholders ("NULL", colour shifts) are skipped.
            std::set<std::string> seen;
            std::vector<uint32_t> props;
            for (auto& [n, tid] : order) {
                const TemplateInfo* t = cat_->find(tid);
                if (!t || !t->placeable || t->model.empty() || special_category_tip(palette_category(*t))) continue;
                const std::string pn = palette_plain_name(*t), ln = lower(pn);
                if (ln == "null" || ln.find("colour shift") != std::string::npos || ln.find("color shift") != std::string::npos) continue;
                const int g = palette_group_of(*t, false);
                if (g != PG_BUILDINGS && g != PG_SCENERY) continue;
                if (!seen.insert(pn).second) continue;
                if (g == PG_BUILDINGS && tc.sample.size() < 4) tc.sample.push_back(tid);
                else props.push_back(tid);
            }
            for (uint32_t u : props) if (tc.sample.size() < 6) tc.sample.push_back(u);
            // fewer than 4 buildings: more props fill the row, and leftover buildings (beyond 4) were kept in props
        }
    }
    return theme_content_.emplace(key, std::move(tc)).first->second;
}

uint32_t App::theme_ground_rgb(const MapInfo& m, bool compute) {
    const std::string key = lower(m.name);
    if (auto it = theme_ground_.find(key); it != theme_ground_.end()) return it->second;
    if (!compute || !io_) return 0;
    std::string path;
    uint32_t rgb = 0;
    if (!io_->theme_ground(m, path, rgb)) rgb = 0;
    theme_ground_[key] = rgb;
    return rgb;
}

void App::ui_new_dialog() {
    // Closed from elsewhere (a dropped file, --no-dialog, opening a level): a modal left open
    // would block every click in the editor.
    if (!show_new_ || !io_) {
        show_new_ = false;
        if (ImGui::BeginPopupModal("New map or mission##newscen", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }
    ImGui::OpenPopup("New map or mission##newscen");
    // A fixed frame: placed once when it appears (top-centred at 8% of the work area), one size, and the body scrolls inside
    // it, so no option moves Cancel / Create. 600 x 690 at UI 1.0 (no empty band above the buttons), never taller than the work area.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // The top edge is 8 % down, but never so low that the bottom passes the window (it did on a 1584 x 861 window at 1.25).
    const float new_h = std::min(690 * dpi_, vp->WorkSize.y - 48 * dpi_);
    const float new_y = std::max(4 * dpi_, std::min(vp->WorkSize.y * 0.08f, vp->WorkSize.y - new_h - 8 * dpi_));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + new_y), ImGuiCond_Appearing, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(std::min(600 * dpi_, vp->WorkSize.x - 16 * dpi_), new_h), ImGuiCond_Always);
    if (!theme::begin_modal("New map or mission##newscen", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    NewMapParams& p = new_params_;
    // Esc = Cancel (not while a box is being typed in, and a drop-down list takes the first Esc).
    if (esc_closes_modal()) {
        show_new_ = false;
        ImGui::CloseCurrentPopup();
        theme::end_modal();
        return;
    }
    auto ends_with_copy = [](const std::string& n) { return n.size() > 10 && n.compare(n.size() - 10, 10, " (my copy)") == 0; };
    // The body takes all but the button row (a warning line and the buttons).
    // Exactly the separator, one warning line (always reserved, so nothing moves when it shows) and the button row.
    ImGui::BeginChild("##body", ImVec2(0, -(ImGui::GetFrameHeight() + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y * 3 + 2.f * dpi_)));
    int type = p.multiplayer ? 0 : 1;
    ImGui::RadioButton("Multiplayer map", &type, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Mission", &type, 1);
    // Said in a line under the choice, not in a tooltip that lands on the Name box the next click goes to.
    ImGui::TextDisabled("%s", type == 0 ? "A map to play against bots (Team Destruction, Capture the Flag...)."
                                         : "A single-player mission with a briefing, objectives, enemies and triggers.");
    if ((type == 0) != p.multiplayer) {
        p.multiplayer = type == 0;
        if (is_default_name(p.name) || ends_with_copy(p.name)) p.name = default_new_name(p.multiplayer);
    }
    form_label("Name", form_col());
    if (ImGui::InputText("##Name", &p.name, ImGuiInputTextFlags_CallbackEdit, limit_chars_cb, const_cast<int*>(&kNameMax))) clip_chars(p.name, kNameMax);
    tip("The name the game lists it under (up to 60 characters). You can change it later on the Map tab.");
    static const char* starts[] = {"Flat ground", "Random hills", "A copy of a real level"};
    int start_before = new_start_;
    form_label("Start from", form_col());
    ImGui::Combo("##Start from", &new_start_, starts, 3);
    // "Bogged Down (my copy)" was pre-filled for a copy; it makes no sense for a map that starts from scratch.
    if (new_start_ != start_before && new_start_ != 2 && ends_with_copy(p.name)) p.name = default_new_name(p.multiplayer);
    tip("What the new map starts as:\n"
        "- Flat ground: an empty level ground to shape yourself (Sculpt and Paint tools).\n"
        "- Random hills: rolling ground made for you, ready to paint and build on.\n"
        "- A copy of a real level: one of the game's own levels with all its buildings and objects, to change.");
    std::vector<const MapInfo*> maps;
    for (auto& m : gd_.maps()) if (m.resource_present && (new_start_ == 2 ? m.multiplayer == p.multiplayer : true)) maps.push_back(&m);
    if (new_start_ == 2) {
        new_copy_ = std::clamp(new_copy_, 0, int(maps.size()) - 1);
        if (!maps.empty()) form_label("Level to copy", form_col());
        if (!maps.empty() && begin_combo("##Level to copy", pretty_name(maps[size_t(new_copy_)]->name).c_str())) {
            for (int i = 0; i < int(maps.size()); i++) if (ImGui::Selectable(pretty_name(maps[size_t(i)]->name).c_str(), i == new_copy_)) new_copy_ = i;
            ImGui::EndCombo();
        }
        // A copy made here kept the name "New Mission"; name it after the level like Make a copy does
        // (a name the user typed stays).
        if (!maps.empty() && (is_default_name(p.name) || ends_with_copy(p.name)))
            p.name = pretty_name(maps[size_t(new_copy_)]->name) + " (my copy)";
        if (!maps.empty()) tip("The real level to start from. Missions and multiplayer maps list their own levels.");
        ImGui::TextWrapped("The copy keeps the level's ground, buildings and objects. Your changes are saved as a new scenario; the real level is not touched.");
    } else {
        static const char* sizes[] = {"Small (1 x 1 km)", "Long (1 x 2 km)", "Large (2 x 2 km)"};
        form_label("Size", form_col());
        ImGui::Combo("##Size", &new_size_, sizes, 3);
        if (p.multiplayer) tip("Small suits 2-6 players a side; Long gives a front line to push along; Large suits 6 against 6 with vehicles.");
        else tip("Small is a short mission; Long makes a path from the start to the goal; Large leaves room for bases and vehicles.");
        // Themes: retail level archives with a world.
        std::vector<const MapInfo*> themes;
        for (auto& m : gd_.maps()) if (m.resource_present) themes.push_back(&m);
        // This kind's own levels first: multiplayer looks for a map, campaign looks for a mission.
        std::stable_partition(themes.begin(), themes.end(), [&](const MapInfo* m) { return m->multiplayer == p.multiplayer; });
        if (new_base_kind_ != int(p.multiplayer)) {  // the order changed with the kind: keep the level that was picked
            new_base_kind_ = int(p.multiplayer);
            for (int i = 0; i < int(themes.size()); i++) if (themes[size_t(i)]->resource == p.base) new_base_ = i;
        }
        new_base_ = std::clamp(new_base_, 0, int(themes.size()) - 1);
        if (!themes.empty()) {
            form_label("Theme", form_col());
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("(?)").x - ImGui::GetStyle().ItemSpacing.x);  // room for the (?) after it
        }
        if (!themes.empty() && begin_combo("##Theme", theme_label(*themes[size_t(new_base_)], theme_counts_text(theme_content(*themes[size_t(new_base_)]).buildings, theme_content(*themes[size_t(new_base_)]).bases, theme_content(*themes[size_t(new_base_)]).trees)).c_str(), ImGuiComboFlags_HeightLarge)) {
            // Rows show the real ground colour of every theme already worked out (each takes a new map's build, so only
            // the picked theme's is made here, for the big preview below).
            int ground_budget = 0;
            for (int i = 0; i < int(themes.size()); i++) {
                const MapInfo* m = themes[size_t(i)];
                if (i == 0 || m->multiplayer != themes[size_t(i - 1)]->multiplayer)
                    ImGui::SeparatorText(m->multiplayer ? "Multiplayer levels" : "Campaign levels");
                ImGui::PushID(i);
                theme_swatch(theme_look(m->name), ImVec2(40 * dpi_, ImGui::GetTextLineHeight()), 0, 0, theme_ground_rgb(*m, ground_budget-- > 0));
                ImGui::SameLine();
                if (ImGui::Selectable(theme_label(*m, theme_counts_text(theme_content(*m).buildings, theme_content(*m).bases, theme_content(*m).trees)).c_str(), i == new_base_)) new_base_ = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        help("The real level whose look the map borrows: its sky, lighting, ground textures, buildings and trees. You can change the sky later on the Environment tab.");
        if (!themes.empty()) p.base = themes[size_t(new_base_)]->resource;
        // Preview: the theme's sky over its ground, and the map's size to scale.
        static const int KW[] = {1, 1, 2}, KH[] = {1, 2, 2};
        theme_swatch(themes.empty() ? nullptr : theme_look(themes[size_t(new_base_)]->name),
                     ImVec2(ImGui::GetContentRegionAvail().x, 110 * dpi_), KW[new_size_], KH[new_size_],
                     themes.empty() ? 0 : theme_ground_rgb(*themes[size_t(new_base_)], true));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Preview: the theme's sky and the colour its ground starts with. Each square is 1 x 1 km; a 'Mech crosses one in about a minute.");
        ImGui::PopStyleColor();
        if (!themes.empty()) {
            // Six real objects of the theme (the buildings and props its retail level places most) as pictures.
            const ThemeContent& tc = theme_content(*themes[size_t(new_base_)]);
            palette_thumb_budget_ = 2;
            if (!doc_) {  // no map open yet: the picture models come from this theme's archives (and load here, the map loop is not running)
                models_.set_archives(gd_.game_files(), themes[size_t(new_base_)]->resource, gd_.level_archives());
                models_.pump(6.0);
            }
            std::string names;
            ImGui::TextUnformatted("This theme comes with:");
            for (size_t i = 0; i < 6; i++) {
                if (i) ImGui::SameLine();
                const TemplateInfo* t = i < tc.sample.size() ? cat_->find(tc.sample[i]) : nullptr;
                const std::string id = "##themepic_" + std::to_string(i);
                const ImVec2 sz(64 * dpi_, 64 * dpi_);
                ID3D11ShaderResourceView* th = nullptr;
                if (t && (models_.thumbnail_cached(t->model, t->mtb) || palette_thumb_budget_-- > 0)) th = palette_picture(*t);
                // Not greyed out (a dark picture on a dim tile vanished): drawn at full strength, and it does nothing when clicked.
                const ImVec4 tile(0.30f, 0.38f, 0.46f, 1.f);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tile);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, tile);
                ImGui::PushStyleColor(ImGuiCol_Button, tile);
                if (th) { ImGui::ImageButton(id.c_str(), (ImTextureID)(intptr_t)th, sz); name_item(id); }
                else ImGui::Button(((t ? "..." : "") + id).c_str(), ImVec2(sz.x + ImGui::GetStyle().FramePadding.x * 2, sz.y + ImGui::GetStyle().FramePadding.y * 2));
                ImGui::PopStyleColor(3);
                if (t) {
                    ImGui::SetItemTooltip("%s", palette_plain_name(*t).c_str());
                    names += (names.empty() ? "" : ", ") + palette_plain_name(*t);
                }
            }
            if (!names.empty()) ImGui::TextWrapped("%s", names.c_str());
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Any object from any level can be placed; objects from other levels add to graphics memory.");
        ImGui::PopStyleColor();
        if (ImGui::TreeNode("More options")) {
        form_label("Height range", form_col());
        ImGui::DragFloatRange2("##Height range", &p.min_h, &p.max_h, 1, -300, 600, "%.1f m");
        { char hs[200]; snprintf(hs, sizeof hs, "The lowest and highest ground allowed. The game stores 256 steps between them: the default gives steps of 0.5 m (here %.2f m).", std::max(0.001f, p.max_h - p.min_h) / 255.f); tip(hs); }
        if (p.max_h < p.min_h + 10) p.max_h = p.min_h + 10;
        form_label("Start height", form_col());
        ImGui::DragFloat("##Start height", &p.flat_height, 0.5f, p.min_h, p.max_h, "%.2f m");
        tip("How high the flat ground starts.");
        p.flat_height = std::clamp(p.flat_height, p.min_h, p.max_h);
        if (new_start_ == 1) {
            {  // shown in metres: the hills rise and dip by up to this share of the height range
                char hill[48];
                snprintf(hill, sizeof hill, "up to %.0f m", p.noise_amp * (p.max_h - p.min_h));
                form_label("Hill height", form_col());
                theme::slider_float("##Hill height", &p.noise_amp, 0.05f, 0.6f, hill);
            }
            tip("How far the hills rise above and dip below the start height.");
            form_label("Hill spacing", form_col());
            ImGui::DragFloat("##Hill spacing", &p.noise_scale, 5, 60, 800, "%.0f m");
            tip("About how far apart the hills are: small numbers give many small bumps, large numbers a few broad hills.");
            int seed = int(p.seed);
            form_label("Seed", form_col());
            if (ImGui::InputInt("##Seed", &seed)) p.seed = uint64_t(seed);
            tip("A different number gives different hills.");
        }
        ImGui::TreePop();
        }
        if (p.multiplayer) {
            ImGui::TextUnformatted("Game types (you can change these later on the Map tab):");
            // The three most played first; the rest one click away, open when one of them is ticked.
            auto common = [](const GameTypeDef& g) {
                std::string_view n = g.ini;
                return n == "TeamDestruction" || n == "CTF" || n == "Destruction";
            };
            auto ticked = [&](const GameTypeDef& g) { return std::find(p.types.begin(), p.types.end(), g.ini) != p.types.end(); };
            auto boxes = [&](bool want_common) {
                int col = 0;
                for (auto& g : game_types()) {
                    if (common(g) != want_common) continue;
                    bool on = ticked(g);
                    if (col++ % 2) ImGui::SameLine(270 * dpi_);
                    if (ImGui::Checkbox(g.label, &on)) {
                        if (on) p.types.push_back(g.ini);
                        else p.types.erase(std::remove(p.types.begin(), p.types.end(), std::string(g.ini)), p.types.end());
                    }
                    tip(game_type_tip(g.ini));
                }
            };
            boxes(true);
            int more_on = 0;
            for (auto& g : game_types()) if (!common(g) && ticked(g)) more_on++;
            if (more_on) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
            std::string more = "More game types" + (more_on ? " (" + std::to_string(more_on) + " ticked)" : std::string()) + "###more_types";
            if (ImGui::TreeNode(more.c_str())) {
                boxes(false);
                ImGui::TreePop();
            }
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    bool no_types = p.multiplayer && new_start_ != 2 && p.types.empty();
    if (no_types) ImGui::TextColored(theme::tok().warn, "Tick at least one game type.");
    else ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    // Cancel left, Create right as the one primary button; Enter creates. Opening a saved map is File > Open.
    if (ImGui::Button("Cancel", ImVec2(140 * dpi_, 0))) { show_new_ = false; ImGui::CloseCurrentPopup(); }
    tip("Closes this without making anything (Esc).");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 160 * dpi_);
    ImGui::BeginDisabled(no_types);
    bool create = theme::primary_button("Create", ImVec2(160 * dpi_, 0));  // yellow like Test
    tip("Make the map (Enter). Next steps on the right then take you through it.");
    why_disabled(no_types, "Tick at least one game type above: a multiplayer map needs one to be played.");
    if (!no_types && !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        create = true;
    ImGui::EndDisabled();
    if (create) {
        static const int W[] = {129, 129, 257}, H[] = {129, 257, 257};
        p.w = W[new_size_];
        p.h = H[new_size_];
        p.start = new_start_ == 0 ? NewMapParams::Flat : new_start_ == 1 ? NewMapParams::Noise : NewMapParams::CopyRetail;
        if (p.start == NewMapParams::CopyRetail && !maps.empty()) p.copy_map = maps[size_t(new_copy_)]->name;
        auto s = std::make_unique<Scenario>();
        std::string err;
        if (io_->create(p, *s, &err)) {
            s->author = settings_.author;
            if (p.multiplayer && p.start != NewMapParams::CopyRetail) io_->sync_spawn_groups(*s);
            set_document(std::move(s));
            status_ = p.start == NewMapParams::CopyRetail ? "Made " + doc_->name + ", an editable copy of " + pretty_name(p.copy_map) + "."
                                                          : "Made " + doc_->name + ".";
            status_time_ = 6;
            // A new map is playable at once: spawn rings for every ticked game type, then the objects
            // each type needs (flags, bases, data core; copied from a real level, the Base's own when it
            // has the type). Rings first: the kit then goes next to them and brings no extra spawns.
            if (p.multiplayer && p.start != NewMapParams::CopyRetail) {
                int spawns = add_missing_spawns();
                int kits = 0;
                std::string kit_names;
                for (auto& gt : std::vector<std::string>(doc_->types))
                    if (add_type_kit(gt)) {
                        kits++;
                        for (auto& g : game_types()) if (iequals(g.ini, gt)) kit_names += std::string(kit_names.empty() ? "" : ", ") + g.label;
                    }
                if (kits) io_->sync_spawn_groups(*doc_);
                if (kits || spawns > 0) {
                    world_changed();
                    status_ = kits ? "Added the " + kit_names + " objects and spawn points for each team. Move them, or press Test (" + kt(A_TEST) + ") to play."
                                   : "Spawn points added for each team (the coloured markers). Move them, or press Test (" + kt(A_TEST) + ") to play.";
                    status_time_ = 10;
                }
            }
            // A map just made has nothing of the user's to lose (File > New asked 'Save first?'); its first edit sets dirty.
            // The spawn rings and kit above are part of making it (no undo step). The toolbar still says 'not saved' (no file).
            doc_->dirty = false;
            show_new_ = false;
            ImGui::CloseCurrentPopup();
            // Stay on Map: the Next steps card (Properties) says what to do and jumps to each tab.
            tab_ = Tab::Map;
            ns_start();
        } else {
            message("New map or mission", err);
        }
    }
    theme::end_modal();
}

}  // namespace ed
