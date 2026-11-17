// Map tab "Time of day and weather" and the View > Weather preview over the 3D view.
// What each switch does in the game: core/weather.h.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
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

// The editor never moves the camera on its own on the Environment tab. A new map opens looking down, where sky, look
// and fog changes show nothing: the tab says so, and "Show the sky" tilts the camera to the horizon (same target and heading).
void App::show_sky_view(bool only_if_top_down) {
    // Steeper than about 30 degrees the sky is out of the 0.9 rad field of view.
    if (only_if_top_down && cam_.pitch < 0.5f) return;
    cam_.pitch = 0.15f;
    cam_.dist = std::clamp(cam_.dist * 0.5f, 80.f, 250.f);
}

// ---------------------------------------------------------------------------------------------
// Status lines: every Environment control says what it did.

void App::env_status(const char* fmt, ...) {
    char b[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    status_ = b;
    status_time_ = 8;
}

// The exact graphics figure (the same one Check and Save make). A sky or look change moves the sky's files, which the live
// per-template sum does not see; the exact figure replaces the live one afterwards (gfx_calibrate).
double App::env_gfx_before() { return io_ && doc_ ? io_->graphics_estimate(*doc_).total_mb : 0.0; }

std::string App::env_gfx_note(double before) {
    if (!io_ || !doc_) return {};
    const double now = io_->graphics_estimate(*doc_).total_mb;
    gfx_calibrate(now);
    // The tail is noise for a small change on a small map; it shows from a 1 MB step or when the map is over 3/4 of the limit.
    if (std::fabs(now - before) < 1.0 && now < 48.0) return {};
    char b[96];
    snprintf(b, sizeof b, " Graphics memory: about %.1f of 64 MB (%+.1f).", now, now - before);
    return b;
}

// ---------------------------------------------------------------------------------------------
// Look

// The look of a time of day (0 day, 1 evening, 2 night): the base level's own look of that time wins (playtest: Night on Ill Wind
// gave City night, the game shows Stormy night), else the default one.
const weather::Look* App::look_for_time(Scenario& s, int g) {
    const weather::Look* l = g == 0 ? weather::default_look(false) : g == 2 ? weather::default_look(true) : nullptr;
    if (g == 1) for (auto& x : weather::looks()) if (x.evening) { l = &x; break; }
    const std::string base = s.base_level();
    for (auto& x : weather::looks())
        if ((x.night ? 2 : x.evening ? 1 : 0) == g && _stricmp(x.world, base.c_str()) == 0) { l = &x; break; }
    return l;
}

// "Warm sun", "Warm sun, with the Starry space sky" (a hand-picked sky keeps the look's name), "This map's own light".
std::string App::look_label_now(Scenario& s) {
    const weather::Look* cur = weather::current_look_light(*io_, s);
    const std::string sky = io_->sky_name(s);
    std::string out = cur ? cur->label : "This map's own light";
    const bool own = cur ? iequals(sky, weather::look_sky(*cur)) : iequals(sky, io_->base_sky_name(s));
    if (!sky.empty() && !own) out += ", with the " + sky_words(sky) + " sky";
    return out;
}

// The sky's time disagrees with the light's (night sky on a day or evening look, day sky on a night look), unless "Keep it" was
// pressed for this sky. A look's own sky, and the base level's own sky on its own light, never disagree.
bool App::env_mismatch(Scenario& s, bool* sky_night) {
    const std::string sky = io_->sky_name(s);
    const bool sn = !sky.empty() && io_->sky_is_night(sky);
    if (sky_night) *sky_night = sn;
    if (sky.empty() || sky == mismatch_kept_sky_) return false;
    const weather::Look* cur = weather::current_look_light(*io_, s);
    const bool own = cur ? iequals(sky, weather::look_sky(*cur)) : iequals(sky, io_->base_sky_name(s));
    return !own && sn != weather::light_is_night(*io_, s);
}

// Applies a look and says what it replaced, so a hand-picked sky or fog colour is not lost silently.
// keep_sky: the sky the user picked, put back after the look ("Use a night look").
void App::apply_look_ui(Scenario& s, const weather::Look* l, const char* what, const std::string& keep_sky) {
    const double g0 = env_gfx_before();
    push_undo(what);
    std::string err;
    // A Light/Medium/Thick fog the user picked survives the look; the look's own distances apply otherwise.
    int ei = find_class(s.world, K_ENV);
    float fs = 0, fe = 0;
    const char* kept = nullptr;
    bool flash0 = false;
    if (ei >= 0) {
        fs = s.world.objs[size_t(ei)].f32("FogStart");
        fe = s.world.objs[size_t(ei)].f32("FogEnd");
        kept = fog_preset_label(fs, fe);
        flash0 = weather::lightning(s.world.objs[size_t(ei)]);
    }
    if (l ? weather::apply_look(*io_, s, *l, l->night, &err) : weather::apply_own_light(*io_, s, &err)) {
        std::string sky_note;
        if (!keep_sky.empty() && !iequals(keep_sky, io_->sky_name(s))) {
            std::string e2;
            if (io_->set_sky(s, keep_sky, false, &e2)) sky_note = " Your " + sky_words(keep_sky) + " sky is kept.";
        }
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
        std::string flash;
        if (ei >= 0) {
            const WObj& env = s.world.objs[size_t(ei)];
            if (weather::lightning(env)) {
                char b[96];
                snprintf(b, sizeof b, " With lightning: bursts every %u to %u s.", env.u32("FlashTimeMin", 3000) / 1000, env.u32("FlashTimeMax", 15000) / 1000);
                flash = b;
            } else if (flash0) {
                flash = " Lightning is now off.";
            }
        }
        const std::string head = l ? std::string(what) + ": the " + l->label + " look - sky, sun and fog colours from " + level_words(l->world) + "."
                                   : std::string(what) + ": this map's own light - the sky, sun and fog colours its level has.";
        env_status("%s", (head + sky_note + fog + flash + " Undo to go back." + env_gfx_note(g0)).c_str());
        objects_dirty_ = minimap_dirty_ = true;
    } else {
        env_status("%s: %s", what, err.c_str());
    }
}

// Environment tab, Look section: Day / Evening / Night buttons and the look list of that time of day, the line that
// names the source level, the amber sky-time line and the Building lights switch.
void App::time_of_day_panel(Scenario& s, int ei) {
    WObj& env = s.world.objs[size_t(ei)];
    (void)env;
    const weather::Look* cur = weather::current_look_light(*io_, s);
    const int now = cur ? (cur->night ? 2 : cur->evening ? 1 : 0) : (weather::light_is_night(*io_, s) ? 2 : -1);
    static const char* names[] = {"Day", "Evening", "Night"};
    static const char* tips[] = {"A bright, sunny look: sky, sun, ambient and fog colours from a real day level.",
                                 "Dusk: a low golden or cold blue evening light.",
                                 "Night: a night sky, dark sun, ambient and fog colours."};
    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    for (int g = 0; g < 3; ++g) {
        if (g) ImGui::SameLine();
        const bool on = now == g;
        // On a map with its own dark light the Night button is lit; a click there still applies the night look, since nothing
        // named is chosen yet.
        if (theme::toggle_button(names[g], on, ImVec2(w, 0)) && (!on || !cur)) {
            if (const weather::Look* l = look_for_time(s, g)) apply_look_ui(s, l, names[g]);
        }
        tip(tips[g]);
    }
    form_label("Look", form_col());
    const std::string shown = look_label_now(s);
    combo_opens_down("##look", 0, 40 * dpi_);
    if (ImGui::BeginCombo("##look", shown.c_str(), ImGuiComboFlags_HeightLargest)) {
        // A level whose own light is none of the looks (Mission 16's gold dusk) had no way back but Undo.
        if (ImGui::Selectable("This map's own light", !cur)) apply_look_ui(s, nullptr, "Look");
        tip("The sky, light and fog colours this map started with (its level's own).");
        for (int grp = 0; grp < 3; ++grp) {
            ImGui::SeparatorText(names[grp]);
            for (auto& l : weather::looks()) {
                if ((l.night ? 2 : l.evening ? 1 : 0) != grp) continue;
                // row 6: each look with the picture of its sky.
                if (thumb_row(sky_thumb(weather::look_sky(l)), l.label, l.label, cur == &l)) apply_look_ui(s, &l, "Look");
                tip(l.tip);
            }
        }
        ImGui::EndCombo();
    }
    tip("The light and sky, copied from a real level: sky, sun, ambient and fog colours, fog distance, lightning and reflections. "
        "Pick one of the looks for this time of day, or another one from the list.");
    // Where the look comes from.
    if (cur) ImGui::TextDisabled("Sky, sun and fog colours from %s.", level_words(cur->world).c_str());
    else ImGui::TextDisabled("The sky, sun and fog colours this map started with.");
    // A sky of the wrong time of day for the light.
    bool sky_night = false;
    if (env_mismatch(s, &sky_night)) {
        const bool light_night = weather::light_is_night(*io_, s);
        const char* kind = light_night ? "night" : cur && cur->evening ? "evening" : "day";
        ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
        ImGui::TextWrapped(sky_night ? "! A night sky with a %s look: the ground stays lit." : "! A day sky with a %s look: the sky is bright over a dark ground.", kind);
        ImGui::PopStyleColor();
        const std::string sky = io_->sky_name(s);
        if (ImGui::Button(sky_night ? "Use a night look" : "Use a day look")) {
            if (const weather::Look* l = look_for_time(s, sky_night ? 2 : 0)) apply_look_ui(s, l, "Look", sky);
        }
        tip(sky_night ? "Applies the night look of this map's level and keeps the sky you picked."
                      : "Applies the day look of this map's level and keeps the sky you picked.");
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) {
            mismatch_kept_sky_ = sky;
            env_status("Kept: the %s sky with this light. The line stays hidden until the sky changes.", sky_words(sky).c_str());
        }
        tip("Leaves the sky and the light as they are and hides this line for this sky.");
    }
    building_lights_control(s, ei);
    // The map view shows the look only roughly. Always shown (a line that appears with the Night look would move the rows below).
    if (ImGui::Checkbox("Brighten dark looks in this view", &r_.params.lift_dark)) { settings_.lift_dark = r_.params.lift_dark; settings_.save(); }
    tip("The map view lights the ground by the look, but lifts a very dark one (a night look) so you can still see what you place; the game itself is darker. "
        "Untick it to see the ground as dark as the game draws it. It changes this view only, never the map (also View > Brighten dark maps).");
    ImGui::TextWrapped("The map view lights the ground more than the game does at night: Test shows the real light.");
}

void App::building_lights_control(Scenario& s, int ei) {
    WObj& env = s.world.objs[size_t(ei)];
    bool lit = env.i32("IsNightTime") != 0;
    if (ImGui::Checkbox("Building lights on (windows and signs)", &lit)) {
        push_undo("building lights");
        env.set_i32("IsNightTime", lit ? 1 : 0);
        s.dirty = true;
        env_status(lit ? "Building lights on: windows and signs glow in the game." : "Building lights off: windows and signs stay dark in the game.");
    }
    help("Lights in building windows and signs glow. Evening and night looks turn this on; it does not make the map darker (the look does). "
         "Street lamps are not switched by it.");
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
        {weather::Precip::Rain, weather::RainLevel::Light, "A light shower: less than half as many streaks as Heavy rain, and fainter."},
        {weather::Precip::Rain, weather::RainLevel::Heavy, "A downpour around the player: many thin, see-through streaks (made from the game's snow effect)."},
        {weather::Precip::Snow, weather::RainLevel::Heavy, "The game's blowing snow (Missions 10 and 11). Adds the effect's files to the scenario if the theme level does not have them."},
        {weather::Precip::Dust, weather::RainLevel::Heavy, "A brown dust storm around the player: fine grit and drifting dust clouds. The game's own dust storm was never finished, so the scenario carries its effect (made from the snow effect)."},
    };
    for (auto& o : opts) {
        if (&o != opts && &o != opts + 3) ImGui::SameLine();  // Snow and Dust storm on a second row: five did not fit the panel
        const bool is_rain = o.p == weather::Precip::Rain;
        const bool on = is_rain ? rain && lvl == o.level : p == o.p;
        const char* label = is_rain ? weather::rain_label(o.level) : weather::precip_label(o.p);
        if (ImGui::RadioButton(label, on) && !on) {
            push_undo("weather");
            if (is_rain) weather::set_rain_level(env, o.level);
            else weather::set_precip(env, o.p);
            s.dirty = true;
            if (o.p != weather::Precip::None) show_weather_ = true;
            // Changing the weather never touches the lightning: say so.
            env_status("Weather: %s.%s", label, weather::lightning(env) ? " Lightning is still on." : "");
        }
        tip(o.tip);
    }
    // The line keeps its row whether it is rain or not, so the controls under it do not jump.
    if (rain) ImGui::TextDisabled("In the game rain falls as thin, see-through streaks.");
    else ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    if (p == weather::Precip::Other) {
        ImGui::SameLine();
        ImGui::TextDisabled("(other: kept from the real level)");
    }
    // Lightning: always shown. A look sets it from its retail level; changing the weather never touches it.
    bool flash = weather::lightning(env);
    if (ImGui::Checkbox("Lightning", &flash)) {
        push_undo("lightning");
        weather::set_lightning(env, flash);
        s.dirty = true;
        if (flash) {
            show_weather_ = true;
            env_status("Lightning on: bursts every %u to %u s. In the game it lights models and the sky for a moment; the ground stays as it is.",
                       env.u32("FlashTimeMin", 3000) / 1000, env.u32("FlashTimeMax", 15000) / 1000);
        } else {
            env_status("Lightning off.");
        }
    }
    tip("Lightning flashes in short bursts, like Ill Wind and Bogged Down.\n"
        "In the game the flash is a brief light: it lights up 'Mechs, vehicles, buildings and the sky for a moment; the ground "
        "stays as it is. This view flashes the same way (with View > Weather on).\nThe game has no thunder sound (no real level uses one). "
        "A look sets lightning as its level has it; changing the weather leaves it alone.");
    {   // The rows stay where they are, greyed while Lightning is off (ticking it used to push everything below down by 60 px)
        ImGui::BeginDisabled(!flash);
        int lo = int(env.u32("FlashTimeMin", 3000) / 1000), hi = int(env.u32("FlashTimeMax", 15000) / 1000);
        // 04: one labelled line ('Bursts every 3 s to 15 s'), not two bare boxes.
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
            env_status("Lightning: bursts every %d to %d s.", lo, hi);
        }
        tip("Seconds between bursts of flashes (real levels: 3 to 15).");
        // The flash colour (the game's LightningColorRed/Green/Blue, 0-255 each).
        float lc[3] = {env.u32("LightningColorRed", 255) / 255.f, env.u32("LightningColorGreen", 255) / 255.f,
                       env.u32("LightningColorBlue", 255) / 255.f};
        form_label("Flash colour", form_col());
        if (ImGui::ColorEdit3("##flashcol", lc, ImGuiColorEditFlags_NoInputs)) {
            push_undo_once("lightning colour");
            env.set_u32("LightningColorRed", uint32_t(std::lround(lc[0] * 255)));
            env.set_u32("LightningColorGreen", uint32_t(std::lround(lc[1] * 255)));
            env.set_u32("LightningColorBlue", uint32_t(std::lround(lc[2] * 255)));
            s.dirty = true;
            env_status("Lightning colour set.");
        }
        tip("The colour of the lightning's light on 'Mechs, vehicles and buildings. Click the swatch to pick a colour.");
        ImGui::EndDisabled();
    }
    // The rain, snow and lightning drawn over the map view can be switched off here (it is also View > Weather).
    ImGui::Checkbox("Show the weather in the map view", &show_weather_);
    tip("Draws the rain, snow, dust and lightning over the map view, so you can see them while you work. It changes the view only (View > Weather).");
}

// View > Weather: the map's precipitation and lightning over the 3D view, tinted toward the fog
// colour. A preview of the look, not the game's particles.
void App::ui_weather() {
    r_.params.flash = {0, 0, 0};
    r_.params.sky_flash = 1.f;
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
    } else if (p == weather::Precip::Dust && precip_3d_) {
        // drawn as the effect the save ships (add_precip_effect, weather::dust_files)
    } else if (p == weather::Precip::Dust) {
        // Until the effect's files have loaded: a brown haze with drifting streaks.
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
    // Lightning: bursts of 1..MaxFlashes short flashes, the next burst FlashTimeMin..FlashTimeMax later (the
    // game's timing). As in the PC game: lit models flash (their sun term + the game's boost x the flash colour),
    // the sky flashes on its own, the ground stays as it is. No whole-view tint.
    if (weather::lightning(env)) {
        static double next = 0, start = -1e9;
        static uint32_t seed = 1;
        const double now = ImGui::GetTime();
        const uint32_t maxf = std::max(1u, env.u32("MaxFlashes", 4));
        float lo = std::max(0.5f, float(env.u32("FlashTimeMin", 3000)) / 1000.f), hi = std::max(lo, float(env.u32("FlashTimeMax", 15000)) / 1000.f);
        if (next == 0 || next > now + hi) next = now + 1.0;
        if (now >= next) {
            seed = hash(seed);
            start = now;
            next = now + std::max<double>(lo, weather::burst_length(seed, maxf)) + (hi - lo) * float(seed % 1000) / 1000.f;
        }
        weather::FlashNow f = weather::burst_flash(now - start, seed, maxf);
        if (lightning_hold_ & 1) f.model = true;  // developer: --lightning-hold 1 holds the model flash, 2 the sky flash
        if (lightning_hold_ & 2) f.sky = true;
        if (f.model) {
            const float k = 2.f * weather::kFlashSunBoost / 255.f;  // the renderer's sun is the file colour x2
            r_.params.flash = {k * float(env.u32("LightningColorRed", 255) & 255), k * float(env.u32("LightningColorGreen", 255) & 255),
                               k * float(env.u32("LightningColorBlue", 255) & 255)};
        }
        if (f.sky) r_.params.sky_flash = weather::kSkyFlashGain;
    }
    dl->PopClipRect();
}

}  // namespace ed
