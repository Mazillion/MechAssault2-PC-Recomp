// Environment tab (UX-SPEC §3 row 6): time of day, weather, fog, sun; the sky model, exact fog numbers,
// the height fog band and lit building windows under Advanced. Pieces of time of day and weather: app_weather.cpp.
#include <algorithm>
#include <cmath>

#include "ui_tips.h"
#include "../core/weather.h"
#include "theme.h"
#include "app.h"
#include "imgui.h"

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
static void tip(const char* text) {
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Fog presets: start and end metres, within what real levels use (start 85-300 m, end 450-500 m).
static const struct { const char* label; float start, end; const char* tip; } kFog[] = {
    {"Light", 300, 700, "Far hills stay visible (fog from 300 m, everything gone past 700 m)."},
    {"Medium", 150, 475, "Like most real levels (fog from 150 m, everything gone past 475 m)."},
    {"Thick", 40, 250, "A misty map: things fade from 40 m and are gone past 250 m."},
};

// The fog preset these distances are, or nullptr (a look keeps a preset the user picked: dogfood-mp P2 06).
const char* fog_preset_label(float start, float end) {
    for (auto& f : kFog)
        if (std::fabs(start - f.start) < 1 && std::fabs(end - f.end) < 1) return f.label;
    return nullptr;
}

void App::tab_environment() {
    Scenario& s = *doc_;
    // Entering the tab from a top-down view shows the horizon, where sky, light and fog changes are visible (UX E3).
    static int last_frame = -10;
    if (ImGui::GetFrameCount() - last_frame > 2) show_sky_view(true);
    last_frame = ImGui::GetFrameCount();

    const int ei = find_class(s.world, K_ENV);
    if (ei < 0) {
        ImGui::TextWrapped("This map has no environment settings (no sky, light or fog of its own).");
        return;
    }
    // Review E4: only moves the camera, so it works on a read-only real level too.
    if (ImGui::Button("Show the sky", ImVec2(-1, 0))) show_sky_view(false);
    tip("Tilts the camera to the horizon so the sky, light and fog are in view.");
    ImGui::BeginDisabled(s.read_only);

    ImGui::SeparatorText("Time of day");
    time_of_day_panel(s, ei);

    ImGui::SeparatorText("Weather");
    weather_controls(s, ei);

    ImGui::SeparatorText("Fog");
    {
        WObj& env = s.world.objs[size_t(ei)];
        uint32_t fc = env.u32("FogColor");
        float col[3] = {float((fc >> 16) & 255) / 255, float((fc >> 8) & 255) / 255, float(fc & 255) / 255};
        auto set_fog_colour = [&] {
            push_undo_once("fog colour");
            uint32_t v = (fc & 0xFF000000u) | uint32_t(col[0] * 255) << 16 | uint32_t(col[1] * 255) << 8 | uint32_t(col[2] * 255);
            env.set_u32("FogColor", v);
            s.dirty = true;
        };
        if (ImGui::ColorEdit3("##fogcol", col, ImGuiColorEditFlags_NoInputs)) { set_fog_colour(); show_sky_view(true); }
        tip("The colour far things fade into. Click the swatch to pick a colour.");
        const float fs = env.f32("FogStart"), fe = env.f32("FogEnd");
        bool preset = false;
        for (auto& f : kFog) {
            ImGui::SameLine();
            const bool on = std::fabs(fs - f.start) < 1 && std::fabs(fe - f.end) < 1;
            preset |= on;
            if (theme::toggle_button(f.label, on) && !on) {
                push_undo("fog");
                env.set_f32("FogStart", f.start);
                env.set_f32("FogEnd", f.end);
                s.dirty = true;
                status_ = std::string("Fog: ") + f.label + " - " + f.tip + " Undo to go back.";
                if (std::fabs(fog_scale_ - 1) > 0.01f) {
                    char b[160];
                    snprintf(b, sizeof b, " Your game's draw distance stretches it to %.0f-%.0f m, as shown here.", f.start * fog_scale_, f.end * fog_scale_);
                    status_ += b;
                }
                show_sky_view(true);
            }
            tip(f.tip);
        }
        ImGui::SameLine();
        // A fresh map (or a look) brings fog that is none of the presets: say so, so the row never reads as unset.
        if (preset) ImGui::TextDisabled("%.0f-%.0f m", fs, fe);
        else if (weather::current_look(*io_, s)) ImGui::Text("The look's own: %.0f-%.0f m", fs, fe);  // review E1
        else ImGui::Text("This map's own: %.0f-%.0f m", fs, fe);
        // The PC game multiplies fog by the player's Draw distance setting; the view follows it, so say so (BB2A-5).
        const bool stretched = std::fabs(fog_scale_ - 1) > 0.01f;
        if (stretched) ImGui::TextDisabled("On this PC (Draw distance) it shows at %.0f-%.0f m", fs * fog_scale_, fe * fog_scale_);
        tip(stretched ? "Where the fog begins and where it hides everything. Exact metres: Advanced below. "
                        "Your game's Draw distance setting stretches every map's fog, so on this PC it shows at the second "
                        "pair of numbers, in the game and in this view. Players with the default Draw distance see the first pair."
                      : "Where the fog begins and where it hides everything. Exact metres: Advanced below. "
                        "There is no 'no fog': the game stops drawing at about 475 m and the fog hides that edge.");
    }

    ImGui::SeparatorText("Sun / moon light");  // the same light lights a night map (dogfood-mp P3 04)
    {
        WObj& env = s.world.objs[size_t(ei)];
        Vec3 sun = env.vec3("DirectionalLightDirection");
        float yaw = std::atan2(sun.x, sun.z) * 57.2958f, elev = std::asin(std::clamp(-sun.y, -1.f, 1.f)) * 57.2958f;
        form_label("Sun direction", form_col());
        bool ch = ImGui::SliderFloat("##sundir", &yaw, -180, 180, "%.0f deg");
        if (ch) push_undo_once("sun");
        tip("Where the sun is (compass direction). Shadows follow when you press Update shadows.");
        form_label("Sun height", form_col());
        if (ImGui::SliderFloat("##sunh", &elev, 5, 85, "%.0f deg")) { push_undo_once("sun"); ch = true; }
        tip("How high the sun is: low gives long shadows.");
        if (ch) {
            float cy = std::cos(elev / 57.2958f);
            env.set_vec3("DirectionalLightDirection", {std::sin(yaw / 57.2958f) * cy, -std::sin(elev / 57.2958f), std::cos(yaw / 57.2958f) * cy});
            s.dirty = true;
            lightmap_stale_ = true;
        } else if (stale_sun_only_) {
            // Undo / redo of a sun change: stale exactly while the sun differs from the one the shadows were made for.
            lightmap_stale_ = std::abs(sun.x - lightmap_sun_.x) + std::abs(sun.y - lightmap_sun_.y) + std::abs(sun.z - lightmap_sun_.z) > 1e-4f;
        }
        if (!lightmap_stale_) {
            lightmap_sun_ = env.vec3("DirectionalLightDirection");
            stale_sun_only_ = true;
        }
        ImGui::BeginDisabled(!lightmap_stale_);
        if (ImGui::Button(lightmap_stale_ ? "Update shadows for the new sun" : "Shadows match the sun", ImVec2(-1, 0))) {
            io_->bake_lightmap(s);
            r_.set_lightmap(s.lightmap);
            lightmap_stale_ = false;
            stale_sun_only_ = false;
            s.dirty = true;
            minimap_dirty_ = true;
        }
        ImGui::EndDisabled();
        tip("Recalculates the hill shadows from the sun.");
    }

    // E4: a real level's Advanced values can be looked at (opened) even though they can't be changed.
    if (s.read_only) ImGui::EndDisabled();
    const bool adv_open = ImGui::TreeNode("Advanced");
    if (s.read_only) ImGui::BeginDisabled();
    if (adv_open) {
        WObj& env = s.world.objs[size_t(ei)];
        sky_picker(s);
        float fs = env.f32("FogStart"), fe = env.f32("FogEnd");
        form_label("Fog distance", form_col());
        if (ImGui::DragFloatRange2("##fogdist", &fs, &fe, 5, 0, 5000, "%.0f m")) { push_undo_once("fog"); env.set_f32("FogStart", fs); env.set_f32("FogEnd", fe); s.dirty = true; }
        tip("Fog begins at the first distance and hides everything past the second. Drag the numbers, or double-click to type.");
        uint32_t fc = env.u32("FogColor");
        float col[3] = {float((fc >> 16) & 255) / 255, float((fc >> 8) & 255) / 255, float(fc & 255) / 255};
        form_label("Fog colour", form_col());
        if (ImGui::ColorEdit3("##fogrgb", col, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoPicker)) {
            push_undo_once("fog colour");
            env.set_u32("FogColor", (fc & 0xFF000000u) | uint32_t(col[0] * 255) << 16 | uint32_t(col[1] * 255) << 8 | uint32_t(col[2] * 255));
            s.dirty = true;
        }
        tip("Red, green and blue, 0 to 255. Drag a number or double-click to type.");
        if (env.find("FogTop") && env.find("FogBottom")) {
            float fb = env.f32("FogBottom"), ft = env.f32("FogTop");
            if (ft > fb) {
                form_label("Low fog band", form_col());
                ImGui::SetNextItemWidth(-28 * dpi_);  // room for the (?) after it
                if (ImGui::DragFloatRange2("##fogband", &fb, &ft, 0.5f, -500, 1000, "%.1f m")) { push_undo_once("fog band"); env.set_f32("FogBottom", fb); env.set_f32("FogTop", ft); s.dirty = true; }
                float lowest = *std::min_element(s.terrain.hgt.begin(), s.terrain.hgt.end());
                if (ft > lowest && env.i32("FogMode", 1) == 2) {
                    ImGui::TextColored(theme::tok().warn, "Ground below %.1f m is covered in height fog (fog colour, thicker the further you look).", ft);
                    if (ImGui::SmallButton("Move the band under the lowest ground")) {
                        push_undo("fog band");
                        env.set_f32("FogBottom", lowest - 2.f - (ft - fb));
                        env.set_f32("FogTop", lowest - 2.f);
                        s.dirty = true;
                    }
                }
                help("A layer of thick fog between these two heights, for misty valleys (real missions use it). Ground inside it looks washed out.");
            }
        }
        lit_windows_control(s, ei);
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
}

// UX E10: a sun arrow at the view centre while the Environment tab is open: a ring where the sun is, a line down
// to the ground along the light, and an arrow head pointing the way the light (and the shadows) fall.
void App::draw_sun_arrow() {
    if (!doc_) return;
    const int ei = find_class(doc_->world, K_ENV);
    if (ei < 0) return;
    Vec3 d = doc_->world.objs[size_t(ei)].vec3("DirectionalLightDirection");
    const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (n < 1e-4f) return;
    d = {d.x / n, d.y / n, d.z / n};
    const float L = std::clamp(cam_.dist * 0.25f, 15.f, 80.f);
    const float gy = doc_->terrain.height_at(cam_.tx, cam_.tz) + 0.5f;
    const Vec3 t{cam_.tx, gy, cam_.tz}, s{t.x - d.x * L, t.y - d.y * L, t.z - d.z * L};
    const uint32_t c = 0xFF30D8FFu;
    auto line = [&](const Vec3& a, const Vec3& b) {
        r_.lines.push_back({{a.x, a.y, a.z}, c});
        r_.lines.push_back({{b.x, b.y, b.z}, c});
    };
    line(s, t);
    // Arrow head: two short lines back from the ground point, in the vertical plane of the light.
    const float h = L * 0.12f, hx = -d.z, hz = d.x;
    line(t, {t.x - d.x * h + hx * h * 0.5f, t.y - d.y * h, t.z - d.z * h + hz * h * 0.5f});
    line(t, {t.x - d.x * h - hx * h * 0.5f, t.y - d.y * h, t.z - d.z * h - hz * h * 0.5f});
    // The sun: a small ring facing the ground point.
    const float r = L * 0.08f;
    for (int k = 0; k < 24; ++k) {
        const float a0 = k * 6.2831853f / 24, a1 = (k + 1) * 6.2831853f / 24;
        line({s.x + std::cos(a0) * r, s.y + std::sin(a0) * r, s.z}, {s.x + std::cos(a1) * r, s.y + std::sin(a1) * r, s.z});
        line({s.x, s.y + std::cos(a0) * r, s.z + std::sin(a0) * r}, {s.x, s.y + std::cos(a1) * r, s.z + std::sin(a1) * r});
    }
    // Its shadow line on the ground: the compass direction the shadows point.
    line(t, {t.x + d.x * L * 0.5f, t.y, t.z + d.z * L * 0.5f});
}

// Retail-reach #41 / UX-SPEC row 6: a picture of a sky, its dome drawn with the sky shader as seen from the ground
// (Renderer::sky_preview). The dome loads through the model cache like the scene's sky; nullptr until it has and the
// picture is drawn (the rows show a grey box meanwhile).
ID3D11ShaderResourceView* App::sky_thumb(const std::string& name) {
    const ScenarioIO::SkyInfo* si = io_->find_sky(name);
    if (!si) return nullptr;
    Renderer::SkyLook k;
    k.key = si->name;
    std::string model = si->models.empty() ? std::string() : si->models[0], mtb = si->mtbs.empty() ? std::string() : si->mtbs[0];
    if (!si->uses.empty()) {
        const ScenarioIO::SkyUse& u = si->uses[0];
        model = u.model;
        mtb = u.mtb;
        if (u.scale.y > 0) k.scale = {u.scale.x, u.scale.y, u.scale.z};
        k.offset = {u.offset.x, u.offset.y, u.offset.z};
        k.heading = u.heading_deg / 57.2958f;
    }
    k.model = model.empty() ? nullptr : models_.get(model, mtb);
    return r_.sky_preview(k);
}

// A list row: the sky picture on the left, the name beside it. Returns true when clicked.
bool App::thumb_row(ID3D11ShaderResourceView* th, const std::string& label, const std::string& id, bool selected) {
    const float h = 36 * dpi_, w = 72 * dpi_;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool hit = ImGui::Selectable(("##" + id).c_str(), selected, 0, ImVec2(0, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (th) dl->AddImage((ImTextureID)(intptr_t)th, p, ImVec2(p.x + w, p.y + h));
    else dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(60, 60, 60, 255));
    dl->AddText(ImVec2(p.x + w + 8 * dpi_, p.y + (h - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    return hit;
}

}  // namespace ed
