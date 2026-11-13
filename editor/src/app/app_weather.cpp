// Map tab "Time of day and weather" and the View > Weather preview over the 3D view.
// What each switch does in the game: core/weather.h.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "ui_tips.h"
#include "../core/weather.h"
#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "imgui_internal.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

const char* fog_preset_label(float start, float end);  // app_environment.cpp

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
static void tip(const char* text) {
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// "UnderCover" -> "Under Cover", "Mission_10" -> "Mission 10": the look's retail level as a player reads it.
static std::string level_words(const char* world) {
    std::string out;
    for (const char* c = world; *c; ++c) {
        if (*c == '_') { out += ' '; continue; }
        if (c != world && std::isupper((unsigned char)*c) && std::islower((unsigned char)c[-1])) out += ' ';
        out += *c;
    }
    return out;
}

// UX E3: a new map opens looking down, where sky, look and fog changes show nothing. Changing them from a steep view
// tilts the camera down to the horizon (same target and heading); the "Show the sky" button does it on demand.
void App::show_sky_view(bool only_if_top_down) {
    // Steeper than about 30 degrees the sky is out of the 0.9 rad field of view.
    if (only_if_top_down && cam_.pitch < 0.5f) return;
    cam_.pitch = 0.15f;
    cam_.dist = std::clamp(cam_.dist * 0.5f, 80.f, 250.f);
}

// Applies a look and says what it replaced (UX E14), so a hand-picked sky or fog colour is not lost silently.
void App::apply_look_ui(Scenario& s, const weather::Look* l, const char* what) {
    push_undo(what);
    std::string err;
    // A Light/Medium/Thick fog the user picked survives the look; the look's own distances apply otherwise.
    int ei = find_class(s.world, K_ENV);
    float fs = 0, fe = 0;
    const char* kept = nullptr;
    if (ei >= 0) {
        fs = s.world.objs[size_t(ei)].f32("FogStart");
        fe = s.world.objs[size_t(ei)].f32("FogEnd");
        kept = fog_preset_label(fs, fe);
    }
    if (l ? weather::apply_look(*io_, s, *l, l->night, &err) : weather::apply_own_light(*io_, s, &err)) {
        std::string fog;
        if (kept && ei >= 0) {
            WObj& env = s.world.objs[size_t(ei)];
            env.set_f32("FogStart", fs);
            env.set_f32("FogEnd", fe);
            fog = std::string(" Your ") + kept + " fog is kept.";
        } else if (ei >= 0) {
            char b[96];
            snprintf(b, sizeof b, l ? " Fog distance is now the look's own: %.0f-%.0f m." : " Fog distance is the level's own: %.0f-%.0f m.", s.world.objs[size_t(ei)].f32("FogStart"),
                     s.world.objs[size_t(ei)].f32("FogEnd"));
            fog = b;
        }
        status_ = l ? std::string(what) + ": the " + l->label + " look - sky, sun and fog colours from " + level_words(l->world) + "." + fog + " Undo to go back."
                    : std::string(what) + ": this map's own light - the sky, sun and fog colours its level has." + fog + " Undo to go back.";
        status_time_ = 8;
        objects_dirty_ = minimap_dirty_ = true;
        show_sky_view(true);
    } else {
        status_ = std::string(what) + ": " + err;
    }
}

// Environment tab: Day / Evening / Night buttons (UX-SPEC E6) and the look list of that time of day.
void App::time_of_day_panel(Scenario& s, int ei) {
    WObj& env = s.world.objs[size_t(ei)];
    const weather::Look* cur = weather::current_look(*io_, s);
    const int now = cur ? (cur->night ? 2 : cur->evening ? 1 : 0) : (env.i32("IsNightTime") != 0 ? 2 : -1);
    static const char* names[] = {"Day", "Evening", "Night"};
    static const char* tips[] = {"A bright, sunny look: sky, sun, ambient and fog colours from a real day level.",
                                 "Dusk: a low golden or cold blue evening light.",
                                 "Night: a night sky, dark sun, ambient and fog colours; building windows are lit."};
    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    for (int g = 0; g < 3; ++g) {
        if (g) ImGui::SameLine();
        const bool on = now == g;
        // Lit windows alone light the Night button on a map with its own light (KillingField); a click there still applies
        // the night look, since nothing named is chosen yet.
        if (theme::toggle_button(names[g], on, ImVec2(w, 0)) && (!on || !cur)) {
            const weather::Look* l = g == 0 ? weather::default_look(false) : g == 2 ? weather::default_look(true) : nullptr;
            if (g == 1) for (auto& x : weather::looks()) if (x.evening) { l = &x; break; }
            // The base level's own look for that time of day wins (playtest: Night on Ill Wind gave City night, the game
            // shows Stormy night).
            const std::string base = s.base_level();
            for (auto& x : weather::looks())
                if ((x.night ? 2 : x.evening ? 1 : 0) == g && _stricmp(x.world, base.c_str()) == 0) { l = &x; break; }
            if (l) apply_look_ui(s, l, names[g]);
        }
        tip(tips[g]);
    }
    form_label("Look", form_col());
    std::string shown = cur ? cur->label : "Custom (this map's own light)";
    if (ImGui::BeginCombo("##look", shown.c_str(), ImGuiComboFlags_HeightLargest)) {
        // Review E2: a level whose own light is none of the looks (Mission 16's gold dusk) had no way back but Undo.
        if (ImGui::Selectable("This map's own light", !cur)) apply_look_ui(s, nullptr, "Look");
        tip("The sky, light and fog colours this map started with (its level's own).");
        for (int grp = 0; grp < 3; ++grp) {
            ImGui::SeparatorText(names[grp]);
            for (auto& l : weather::looks()) {
                if ((l.night ? 2 : l.evening ? 1 : 0) != grp) continue;
                // UX-SPEC row 6: each look with the picture of its sky.
                if (thumb_row(sky_thumb(weather::look_sky(l)), l.label, l.label, cur == &l)) apply_look_ui(s, &l, "Look");
                tip(l.tip);
            }
        }
        ImGui::EndCombo();
    }
    tip("The light and sky, copied from a real level: sky, sun, ambient and fog colours, fog distance and reflections. "
        "Pick one of the looks for this time of day, or another one from the list.");
}

void App::lit_windows_control(Scenario& s, int ei) {
    WObj& env = s.world.objs[size_t(ei)];
    bool lit = env.i32("IsNightTime") != 0;
    if (ImGui::Checkbox("Lit building windows", &lit)) { push_undo("lit windows"); env.set_i32("IsNightTime", lit ? 1 : 0); s.dirty = true; }
    help("Building windows glow (street lamps are not switched by it). It does not darken the scene by itself (the time of day does). "
         "Each look sets it as the real level it comes from does (Go Commando, Ill Wind and Bogged Down: lit; New River City, Witch Cauldron and Cold dusk: not); "
         "change it here for a lit-up evening or a dark day without lights.");
}

void App::weather_controls(Scenario& s, int ei) {
    WObj& env = s.world.objs[size_t(ei)];
    // Weather: one precipitation effect at a time (the game runs one PrecipEffectID).
    weather::Precip p = weather::precip(env);
    // Two rain levels (design note); both are Precip::Rain, told apart by weather::rain_level.
    const bool rain = p == weather::Precip::Rain;
    const weather::RainLevel lvl = weather::rain_level(env);
    static const struct { weather::Precip p; weather::RainLevel level; const char* tip; } opts[] = {
        {weather::Precip::None, weather::RainLevel::Heavy, "No rain or snow."},
        {weather::Precip::Rain, weather::RainLevel::Heavy, "A downpour around the player: many thin, see-through streaks (made from the game's snow effect)."},
        {weather::Precip::Rain, weather::RainLevel::Light, "A light shower: less than half as many streaks as Heavy rain, and fainter."},
        {weather::Precip::Snow, weather::RainLevel::Heavy, "The game's blowing snow (Missions 10 and 11). Adds the effect's files to the scenario if the theme level does not have them."},
        {weather::Precip::Dust, weather::RainLevel::Heavy, "The game's desert dust storm (made for a cut wasteland level). Adds its files to the scenario."},
    };
    for (auto& o : opts) {
        if (&o != opts && &o != opts + 3) ImGui::SameLine();  // Snow and Dust storm on a second row: five did not fit the panel
        const bool is_rain = o.p == weather::Precip::Rain;
        const bool on = is_rain ? rain && lvl == o.level : p == o.p;
        if (ImGui::RadioButton(is_rain ? weather::rain_label(o.level) : weather::precip_label(o.p), on) && !on) {
            push_undo("weather");
            if (is_rain) weather::set_rain_level(env, o.level);
            else weather::set_precip(env, o.p);
            s.dirty = true;
            if (o.p != weather::Precip::None) show_weather_ = true;
        }
        tip(o.tip);
    }
    if (rain) ImGui::TextDisabled("In the game rain falls as thin, see-through streaks.");
    if (p == weather::Precip::Other) {
        ImGui::SameLine();
        ImGui::TextDisabled("(other: kept from the real level)");
    }
    bool flash = weather::lightning(env);
    // Playtest 10-24: the option appears once Rain (or Snow) is chosen, unticked (off by default on new maps). A map that
    // already flashes under other weather (a real level, or the weather changed afterwards) keeps the row so it can be turned off.
    if (p != weather::Precip::Rain && p != weather::Precip::Snow && !flash) return;
    if (ImGui::Checkbox("Lightning flashes", &flash)) {
        push_undo("lightning");
        weather::set_lightning(env, flash);
        s.dirty = true;
        if (flash) show_weather_ = true;
    }
    tip("Lightning flashes in short bursts, like Ill Wind and Bogged Down.\n"
        "In the game the flash is a light: it lights up 'Mechs, vehicles and people for a moment; the ground and "
        "sky stay as they are.\nThe game has no thunder sound (no real level uses one).");
    if (flash) {
        int lo = int(env.u32("FlashTimeMin", 3000) / 1000), hi = int(env.u32("FlashTimeMax", 15000) / 1000);
        // dogfood-mp P3 04: one labelled line ('Bursts every 3 s to 15 s'), not two bare boxes.
        form_label("Bursts every", form_col());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragIntRange2("##flashes", &lo, &hi, 0.2f, 1, 60, "%d s", "to %d s")) {
            push_undo_once("lightning timing");
            // The game picks the next burst with a remainder by (max - min): equal values crash it.
            lo = std::clamp(lo, 1, 59);
            hi = std::clamp(hi, lo + 1, 60);
            env.set_u32("FlashTimeMin", uint32_t(lo) * 1000);
            env.set_u32("FlashTimeMax", uint32_t(hi) * 1000);
            s.dirty = true;
        }
        tip("Seconds between bursts of flashes (real levels: 3 to 15).");
        // UX E16: the flash colour (the game's LightningColorRed/Green/Blue, 0-255 each).
        float lc[3] = {env.u32("LightningColorRed", 255) / 255.f, env.u32("LightningColorGreen", 255) / 255.f,
                       env.u32("LightningColorBlue", 255) / 255.f};
        form_label("Flash colour", form_col());
        if (ImGui::ColorEdit3("##flashcol", lc, ImGuiColorEditFlags_NoInputs)) {
            push_undo_once("lightning colour");
            env.set_u32("LightningColorRed", uint32_t(std::lround(lc[0] * 255)));
            env.set_u32("LightningColorGreen", uint32_t(std::lround(lc[1] * 255)));
            env.set_u32("LightningColorBlue", uint32_t(std::lround(lc[2] * 255)));
            s.dirty = true;
        }
        tip("The colour of the lightning's light on 'Mechs, vehicles and people. Click the swatch to pick a colour.");
    }
}

// View > Weather: the map's precipitation and lightning over the 3D view, tinted toward the fog
// colour. A preview of the look, not the game's particles.
void App::ui_weather() {
    if (!show_weather_ || !doc_) return;
    int ei = find_class(doc_->world, K_ENV);
    if (ei < 0) return;
    const WObj& env = doc_->world.objs[size_t(ei)];
    weather::Precip p = weather::precip(env);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    float t = float(ImGui::GetTime());
    uint32_t fc = env.u32("FogColor", 0xFFC0C0C0u);
    auto hash = [](uint32_t h) {  // murmur3 finalizer: no visible lattice
        h += 0x9E3779B9u;
        h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
        return h;
    };
    if (p == weather::Precip::Snow && precip_3d_) {
        // drawn as the game's particle effect (add_precip_effect)
    } else if (p == weather::Precip::Snow || p == weather::Precip::Other) {
        int r = 200 + int((fc >> 16) & 255) / 5, g = 200 + int((fc >> 8) & 255) / 5, b = 200 + int(fc & 255) / 5;
        for (int i = 0; i < 600; i++) {
            uint32_t h = hash(uint32_t(i));
            float u = float(h & 0xFFFF) / 65535.f, v = float((h >> 16) & 0xFFFF) / 65535.f;
            float depth = 0.35f + 0.65f * float((h >> 8) & 255) / 255.f;  // near flakes are bigger and faster
            float y = std::fmod(v * vp_h + t * 60.f * depth * dpi_, vp_h);
            float x = std::fmod(u * vp_w + std::sin(t * 0.8f + float(i)) * 6.f * dpi_ + vp_w, vp_w);
            dl->AddCircleFilled(ImVec2(vp_x + x, vp_y + y), (0.8f + 1.6f * depth) * dpi_,
                                IM_COL32(std::min(r, 255), std::min(g, 255), std::min(b, 255), int(110 + 120 * depth)), 6);
        }
    } else if (p == weather::Precip::Rain && precip_3d_) {
        // drawn as the game's rain particle (add_precip_effect)
    } else if (p == weather::Precip::Rain) {
        // Until the effect's files have loaded: thin slanted streaks, fast, faint blue-grey.
        for (int i = 0; i < 700; i++) {
            uint32_t h = hash(uint32_t(i) * 7u + 3u);
            float u = float(h & 0xFFFF) / 65535.f, v = float((h >> 16) & 0xFFFF) / 65535.f;
            float depth = 0.3f + 0.7f * float((h >> 8) & 255) / 255.f;
            float len = (10.f + 22.f * depth) * dpi_;
            float y = std::fmod(v * (vp_h + len) + t * 900.f * depth * dpi_, vp_h + len) - len;
            float x = std::fmod(u * vp_w + y * 0.08f + vp_w, vp_w);
            dl->AddLine(ImVec2(vp_x + x, vp_y + y), ImVec2(vp_x + x + len * 0.08f, vp_y + y + len),
                        IM_COL32(190, 200, 215, int(50 + 90 * depth)), (0.6f + 0.8f * depth) * dpi_);
        }
    } else if (p == weather::Precip::Dust) {
        // A brown haze with drifting streaks.
        dl->AddRectFilled(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), IM_COL32(150, 110, 70, 60));
        for (int i = 0; i < 250; i++) {
            uint32_t h = hash(uint32_t(i) * 13u + 5u);
            float u = float(h & 0xFFFF) / 65535.f, v = float((h >> 16) & 0xFFFF) / 65535.f;
            float depth = 0.3f + 0.7f * float((h >> 8) & 255) / 255.f;
            float x = std::fmod(u * vp_w + t * 400.f * depth * dpi_, vp_w);
            float y = v * vp_h + std::sin(t * 1.3f + float(i)) * 8.f * dpi_;
            dl->AddLine(ImVec2(vp_x + x, vp_y + y), ImVec2(vp_x + x + 30.f * depth * dpi_, vp_y + y + 2.f * dpi_),
                        IM_COL32(170, 130, 90, int(40 + 60 * depth)), 2.f * dpi_);
        }
    }
    // Lightning: bursts of 1-4 short flashes, the next burst FlashTimeMin..FlashTimeMax later
    // (the game's timing). The game's flash is a light on units only, so the preview stays faint.
    if (weather::lightning(env)) {
        static double next = 0, burst_end = 0;
        static int seed = 1;
        double now = ImGui::GetTime();
        float lo = std::max(0.5f, float(env.u32("FlashTimeMin", 3000)) / 1000.f), hi = std::max(lo, float(env.u32("FlashTimeMax", 15000)) / 1000.f);
        if (next == 0 || next > now + hi) next = now + 1.0;
        if (now >= next) {
            seed = int(hash(uint32_t(seed)) & 0x7FFFFFFF);
            burst_end = now + 0.25 + 0.15 * (seed % 4);
            next = now + lo + (hi - lo) * float(seed % 1000) / 1000.f;
        }
        if (now < burst_end) {
            double k = std::fmod((burst_end - now) * 7.0, 1.0);  // flicker
            int a = int(45 * k);
            uint32_t r = env.u32("LightningColorRed", 255), g = env.u32("LightningColorGreen", 255), b = env.u32("LightningColorBlue", 255);
            dl->AddRectFilled(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), IM_COL32(r & 255, g & 255, b & 255, a));
        }
    }
    dl->PopClipRect();
}

}  // namespace ed
