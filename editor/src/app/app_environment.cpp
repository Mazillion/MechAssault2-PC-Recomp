// Environment tab (§3 row 6): time of day, weather, fog, sun; the sky model, exact fog numbers,
// the height fog band and lit building windows under Advanced. Pieces of time of day and weather: app_weather.cpp.
#include <algorithm>
#include <cfloat>
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

// A combo's list opens downward within the window. The popup's height stops at the window bottom (never under four rows);
// with less than four rows of room, the panel scrolls the combo up once the list is open (the list follows the combo next frame).
void combo_opens_down(const char* id, float min_width, float row_h) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float room = vp->Pos.y + vp->Size.y - (p.y + ImGui::GetFrameHeight()) - 6.f;
    const float need = 4 * row_h;
    if (room < need) {
        if (ImGui::IsPopupOpen(ImGui::GetID(id), ImGuiPopupFlags_None)) ImGui::SetScrollHereY(0.f);  // only while the list is open: the panel never scrolls by itself
        room = need;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0), ImVec2(FLT_MAX, room));
}

bool begin_combo(const char* label, const char* preview, ImGuiComboFlags flags) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float row = ImGui::GetFontSize() + st.ItemSpacing.y;
    const int items = (flags & ImGuiComboFlags_HeightLargest) ? 1000 : (flags & ImGuiComboFlags_HeightLarge) ? 20 : (flags & ImGuiComboFlags_HeightSmall) ? 4 : 8;
    const float usual = row * float(items) - st.ItemSpacing.y + st.WindowPadding.y * 2.f;
    float room = vp->Pos.y + vp->Size.y - (p.y + ImGui::GetFrameHeight()) - 6.f;
    room = std::max(room, row * 4.f + st.WindowPadding.y * 2.f);   // under four rows would be unusable: the panel scrolls instead (as D12)
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::min(usual, room)));
    return ImGui::BeginCombo(label, preview, flags);
}

// Fog presets: start and end metres, within what real levels use (start 85-300 m, end 450-500 m).
static const struct { const char* label; float start, end; const char* tip; } kFog[] = {
    {"Light", 300, 700, "Far hills stay visible (fog from 300 m, everything gone past 700 m)."},
    {"Medium", 150, 475, "Like most real levels (fog from 150 m, everything gone past 475 m)."},
    {"Thick", 40, 250, "A misty map: things fade from 40 m and are gone past 250 m."},
};

// The fog preset these distances are, or nullptr (a look keeps a preset the user picked: 06).
const char* fog_preset_label(float start, float end) {
    for (auto& f : kFog)
        if (std::fabs(start - f.start) < 1 && std::fabs(end - f.end) < 1) return f.label;
    return nullptr;
}

void App::tab_environment() {
    Scenario& s = *doc_;

    const int ei = find_class(s.world, K_ENV);
    if (ei < 0) {
        ImGui::TextWrapped("This map has no environment settings (no sky, light or fog of its own).");
        return;
    }
    // The editor never moves the camera here. From a steep view the sky is out of sight: say so, and "Show the sky" tilts
    // the camera on request. It only moves the camera, so it works on a read-only real level too.
    // Both buttons always show, and the line under them keeps its row, so nothing below moves when the view changes.
    if (ImGui::Button("Show the sky")) {
        show_sky_view(false);
        env_status("Tilted the view to the horizon. Back to the map brings the map view back (so does %s).", kt(A_RESET_VIEW).c_str());
    }
    tip("Tilts the camera to the horizon so the sky, light and fog are in view.");
    ImGui::SameLine();
    if (ImGui::Button("Back to the map")) {
        reset_view();
        env_status("Back to the map view.");
    }
    tip("Returns the camera to the usual view of the map (the same as the Reset view key).");
    ImGui::TextDisabled("%s", cam_.pitch >= 0.5f ? "The sky is out of view." : " ");
    ImGui::BeginDisabled(s.read_only);

    ImGui::SeparatorText("Look");
    time_of_day_panel(s, ei);

    ImGui::SeparatorText("Weather");
    weather_controls(s, ei);

    ImGui::SeparatorText("Fog");
    {
        WObj& env = s.world.objs[size_t(ei)];
        const weather::Look* cur = weather::current_look_light(*io_, s);
        uint32_t fc = env.u32("FogColor");
        float col[3] = {float((fc >> 16) & 255) / 255, float((fc >> 8) & 255) / 255, float(fc & 255) / 255};
        const float fs = env.f32("FogStart"), fe = env.f32("FogEnd");
        // Default = the look's own distances, or the base level's own on "This map's own light".
        float ds = 0, de = 0;
        const bool have_default = cur ? weather::look_fog(*io_, *cur, &ds, &de) : weather::own_fog(*io_, s, &ds, &de);
        const bool default_on = have_default && std::fabs(fs - ds) < 1 && std::fabs(fe - de) < 1;
        auto stretch_note = [&](float a, float b, std::string& st) {
            if (std::fabs(fog_scale_ - 1) > 0.01f) {
                char t[160];
                snprintf(t, sizeof t, " Your game's draw distance stretches it to %.0f-%.0f m, as shown here.", a * fog_scale_, b * fog_scale_);
                st += t;
            }
        };
        ImGui::BeginDisabled(!have_default);
        if (theme::toggle_button("Default##fog", default_on) && !default_on) {
            push_undo("fog");
            env.set_f32("FogStart", ds);
            env.set_f32("FogEnd", de);
            s.dirty = true;
            char b[160];
            snprintf(b, sizeof b, "Fog: Default (%s), %.0f-%.0f m.", cur ? "the look's own" : "this map's own", ds, de);
            std::string st = b;
            stretch_note(ds, de, st);
            env_status("%s", st.c_str());
        }
        ImGui::EndDisabled();
        tip(cur ? "The fog distances of the look (the real level's own). Use it to go back after Light, Medium or Thick."
                : "The fog distances this map started with (its level's own). Use it to go back after Light, Medium or Thick.");
        for (auto& f : kFog) {
            ImGui::SameLine();
            const bool on = !default_on && std::fabs(fs - f.start) < 1 && std::fabs(fe - f.end) < 1;
            if (theme::toggle_button((std::string(f.label) + "##fog").c_str(), on) && !on) {
                push_undo("fog");
                env.set_f32("FogStart", f.start);
                env.set_f32("FogEnd", f.end);
                s.dirty = true;
                std::string st = std::string("Fog: ") + f.label + " - " + f.tip + " Undo to go back.";
                stretch_note(f.start, f.end, st);
                env_status("%s", st.c_str());
            }
            tip(f.tip);
        }
        // The current pair, then the colour swatch.
        const bool preset = fog_preset_label(fs, fe) != nullptr;
        if (default_on || preset) ImGui::Text("%.0f-%.0f m", fs, fe);
        else if (cur) ImGui::Text("The look's own: %.0f-%.0f m", fs, fe);  //
        else ImGui::Text("This map's own: %.0f-%.0f m", fs, fe);
        ImGui::SameLine();
        ImGui::TextDisabled("colour");
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##fogcol", col, ImGuiColorEditFlags_NoInputs)) {
            push_undo_once("fog colour");
            env.set_u32("FogColor", (fc & 0xFF000000u) | uint32_t(col[0] * 255) << 16 | uint32_t(col[1] * 255) << 8 | uint32_t(col[2] * 255));
            s.dirty = true;
            env_status("Fog colour set.");
        }
        tip("The colour far things fade into. Click the swatch to pick a colour.");
        // The PC game multiplies fog by the player's Draw distance setting; the view follows it, so say so.
        const bool stretched = std::fabs(fog_scale_ - 1) > 0.01f;
        if (stretched) ImGui::TextDisabled("On this PC (Draw distance) it shows at %.0f-%.0f m", fs * fog_scale_, fe * fog_scale_);
        tip(stretched ? "Where the fog begins and where it hides everything. Exact metres: Advanced below. "
                        "Your game's Draw distance setting stretches every map's fog, so on this PC it shows at the second "
                        "pair of numbers, in the game and in this view. Players with the default Draw distance see the first pair."
                      : "Where the fog begins and where it hides everything. Exact metres: Advanced below. "
                        "There is no 'no fog': the game stops drawing at about 475 m and the fog hides that edge.");
    }

    const bool moon = weather::light_is_night(*io_, s);
    ImGui::SeparatorText(moon ? "Moon" : "Sun");  // the same light lights a night map; the heading names the body
    {
        WObj& env = s.world.objs[size_t(ei)];
        Vec3 sun = env.vec3("DirectionalLightDirection");
        float yaw = std::atan2(sun.x, sun.z) * 57.2958f, elev = std::asin(std::clamp(-sun.y, -1.f, 1.f)) * 57.2958f;
        if (yaw < 0) yaw += 360.f;  // Headings are shown 0-359 deg
        if (yaw > 359.f) yaw = 359.f;
        const char* body = moon ? "Moon" : "Sun";
        bool released = false;
        form_label("Direction", form_col());
        bool ch = theme::slider_float("##sundir", &yaw, 0, 359, "%.0f deg");
        if (ch) push_undo_once("sun");
        released |= ImGui::IsItemDeactivatedAfterEdit();
        tip(moon ? "Where the moon is (compass direction). Shadows follow when you press Update shadows, or by themselves when you save."
                 : "Where the sun is (compass direction). Shadows follow when you press Update shadows, or by themselves when you save.");
        form_label("Height", form_col());
        if (theme::slider_float("##sunh", &elev, 5, 85, "%.0f deg")) { push_undo_once("sun"); ch = true; }
        released |= ImGui::IsItemDeactivatedAfterEdit();
        tip(moon ? "How high the moon is: low gives long shadows." : "How high the sun is: low gives long shadows.");
        if (ch) {
            float cy = std::cos(elev / 57.2958f);
            env.set_vec3("DirectionalLightDirection", {std::sin(yaw / 57.2958f) * cy, -std::sin(elev / 57.2958f), std::cos(yaw / 57.2958f) * cy});
            s.dirty = true;
            lightmap_stale_ = true;
        } else if (stale_sun_only_) {
            // Undo / redo of a sun change: stale exactly while the sun differs from the one the shadows were made for.
            lightmap_stale_ = std::abs(sun.x - lightmap_sun_.x) + std::abs(sun.y - lightmap_sun_.y) + std::abs(sun.z - lightmap_sun_.z) > 1e-4f;
        }
        if (released)
            env_status("%s moved: direction %.0f deg, height %.0f deg. Press Update shadows to move the hill shadows too.", body, yaw, elev);
        if (!lightmap_stale_) {
            lightmap_sun_ = env.vec3("DirectionalLightDirection");
            stale_sun_only_ = true;
        }
        ImGui::BeginDisabled(!lightmap_stale_);
        if (ImGui::Button(lightmap_stale_ ? (moon ? "Update shadows for the new moon" : "Update shadows for the new sun") : "Shadows match the light", ImVec2(-1, 0))) {
            io_->bake_lightmap(s);
            r_.set_lightmap(s.lightmap);
            lightmap_stale_ = false;
            stale_sun_only_ = false;
            s.dirty = true;
            minimap_dirty_ = true;
            env_status(moon ? "Shadows updated for the new moon." : "Shadows updated for the new sun.");
        }
        ImGui::EndDisabled();
        tip("Recalculates the hill shadows from the light's direction and the ground. Saving does this by itself, so the button is only for seeing them now.");
    }

    // A real level's Advanced values can be looked at (opened) even though they can't be changed.
    if (s.read_only) ImGui::EndDisabled();
    const bool adv_open = ImGui::TreeNode("Advanced");
    if (s.read_only) ImGui::BeginDisabled();
    if (adv_open) {
        WObj& env = s.world.objs[size_t(ei)];
        sky_picker(s);
        float fs = env.f32("FogStart"), fe = env.f32("FogEnd");
        form_label("Fog distance", form_col());
        if (ImGui::DragFloatRange2("##fogdist", &fs, &fe, 5, 0, 5000, "%.0f m")) {
            push_undo_once("fog");
            env.set_f32("FogStart", fs);
            env.set_f32("FogEnd", fe);
            s.dirty = true;
            env_status("Fog: %.0f-%.0f m.", fs, fe);
        }
        tip("Fog begins at the first distance and hides everything past the second. Drag the numbers, or double-click to type.");
        uint32_t fc = env.u32("FogColor");
        float col[3] = {float((fc >> 16) & 255) / 255, float((fc >> 8) & 255) / 255, float(fc & 255) / 255};
        form_label("Fog colour", form_col());
        if (ImGui::ColorEdit3("##fogrgb", col, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoPicker)) {
            push_undo_once("fog colour");
            env.set_u32("FogColor", (fc & 0xFF000000u) | uint32_t(col[0] * 255) << 16 | uint32_t(col[1] * 255) << 8 | uint32_t(col[2] * 255));
            s.dirty = true;
            env_status("Fog colour set.");
        }
        tip("Red, green and blue, 0 to 255. Drag a number or double-click to type.");
        if (env.find("FogTop") && env.find("FogBottom")) {
            float fb = env.f32("FogBottom"), ft = env.f32("FogTop");
            if (ft > fb) {
                // "Ground fog from [a] up to [b] high": two labelled boxes, not a bare range (on a row of their own: the panel is narrow).
                ImGui::TextUnformatted("Ground fog");
                help("A layer of fog that fills low ground between these heights (misty valleys). Retail missions use it.");
                ImGui::TextUnformatted("from");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70 * dpi_);
                bool gch = ImGui::DragFloat("##fogfrom", &fb, 0.5f, -500, 1000, "%.1f m");
                ImGui::SameLine();
                ImGui::TextUnformatted("up to");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70 * dpi_);
                gch |= ImGui::DragFloat("##fogupto", &ft, 0.5f, -500, 1000, "%.1f m");
                ImGui::SameLine();
                ImGui::TextUnformatted("high");
                if (gch) {
                    push_undo_once("ground fog");
                    ft = std::max(ft, fb + 0.5f);
                    env.set_f32("FogBottom", fb);
                    env.set_f32("FogTop", ft);
                    s.dirty = true;
                    env_status("Ground fog: from %.1f m up to %.1f m high.", fb, ft);
                }
                float lowest = s.terrain.lowest();
                if (ft > lowest && env.i32("FogMode", 1) == 2) {
                    ImGui::TextColored(theme::tok().warn, "Ground below %.1f m is covered in height fog (fog colour, thicker the further you look).", ft);
                    if (ImGui::SmallButton("Move the band under the lowest ground")) {
                        push_undo("ground fog");
                        env.set_f32("FogBottom", lowest - 2.f - (ft - fb));
                        env.set_f32("FogTop", lowest - 2.f);
                        s.dirty = true;
                        env_status("Ground fog moved under the lowest ground (%.1f m).", lowest - 2.f);
                    }
                }
            }
        }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
}

// A sun arrow at the view centre while the Environment tab is open: a ring where the sun is, a line down
// to the ground along the light, and an arrow head pointing the way the light (and the shadows) fall.
void App::draw_sun_arrow() {
    if (!doc_ || !show_helpers_) return;  // H hides every editor-only drawing, the sun arrow included
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

// Retail-reach #41 / row 6: a picture of a sky, its dome drawn with the sky shader as seen from the ground
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
        k.offset = {u.offset.x, u.offset.y, u.offset.z};  // no heading: the game does not turn the dome by it
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
