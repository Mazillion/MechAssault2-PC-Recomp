// Keys: the one dispatcher. Every key press in the editor goes through resolve (input_map.h) and runs exactly one
// action in run_action(); the names the screen shows for keys (kt / kt2) come from the same table.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "imgui.h"
#include "theme.h"

namespace ed {

using namespace ma2;

// input_map.h keeps ImGui's key values as plain ints (fmt_test does not link ImGui): they must be ImGui's.
static_assert(ik::kBegin == ImGuiKey_NamedKey_BEGIN && ik::Tab == ImGuiKey_Tab && ik::Left == ImGuiKey_LeftArrow &&
                  ik::Right == ImGuiKey_RightArrow && ik::Up == ImGuiKey_UpArrow && ik::Down == ImGuiKey_DownArrow &&
                  ik::PgUp == ImGuiKey_PageUp && ik::PgDn == ImGuiKey_PageDown && ik::Home == ImGuiKey_Home &&
                  ik::Delete == ImGuiKey_Delete && ik::Space == ImGuiKey_Space && ik::Escape == ImGuiKey_Escape,
              "input_map.h key values must equal ImGuiKey");
static_assert(ik::kModFirst == ImGuiKey_LeftCtrl && ik::kModLast == ImGuiKey_Menu, "modifier key range");
static_assert(ik::A == ImGuiKey_A && ik::C == ImGuiKey_C && ik::D == ImGuiKey_D && ik::E == ImGuiKey_E && ik::F == ImGuiKey_F &&
                  ik::G == ImGuiKey_G && ik::H == ImGuiKey_H && ik::I == ImGuiKey_I && ik::N == ImGuiKey_N && ik::O == ImGuiKey_O &&
                  ik::Q == ImGuiKey_Q && ik::R == ImGuiKey_R && ik::S == ImGuiKey_S && ik::T == ImGuiKey_T && ik::V == ImGuiKey_V &&
                  ik::W == ImGuiKey_W && ik::X == ImGuiKey_X && ik::Y == ImGuiKey_Y && ik::Z == ImGuiKey_Z,
              "letter keys");
static_assert(ik::N1 == ImGuiKey_1 && ik::N2 == ImGuiKey_2 && ik::N3 == ImGuiKey_3 && ik::N4 == ImGuiKey_4 && ik::N5 == ImGuiKey_5 &&
                  ik::N6 == ImGuiKey_6 && ik::F1 == ImGuiKey_F1 && ik::F5 == ImGuiKey_F5 && ik::Comma == ImGuiKey_Comma &&
                  ik::Minus == ImGuiKey_Minus && ik::Slash == ImGuiKey_Slash && ik::Equal == ImGuiKey_Equal &&
                  ik::LBracket == ImGuiKey_LeftBracket && ik::RBracket == ImGuiKey_RightBracket &&
                  ik::KpAdd == ImGuiKey_KeypadAdd && ik::KpSub == ImGuiKey_KeypadSubtract,
              "other keys");
static_assert(ik::kEnd - 1 == ImGuiKey_Oem102, "the name table covers the keyboard keys up to Oem102");

static int current_mods() {
    const ImGuiIO& io = ImGui::GetIO();
    return (io.KeyCtrl ? KM_CTRL : 0) | (io.KeyShift ? KM_SHIFT : 0) | (io.KeyAlt ? KM_ALT : 0);
}

std::string App::kt(int act) const {
    if (act < 0 || act >= A_COUNT) return {};
    for (const KeyBind& k : settings_.keys[act])
        if (k.key) return key_label(k);
    return {};
}

std::string App::kt2(int act) const {
    if (act < 0 || act >= A_COUNT) return {};
    std::string a = key_label(settings_.keys[act][0]), b = key_label(settings_.keys[act][1]);
    if (a.empty()) return b;
    return b.empty() || b == a ? a : a + " or " + b;
}

std::string App::turn_keys_text() const {
    std::string l = kt(A_TURN_LEFT), r = kt(A_TURN_RIGHT);
    return (l.empty() ? "-" : l) + " / " + (r.empty() ? "-" : r);
}

// "W A S D or the arrow keys": the first slots of the four Pan actions, then the second slots (the arrows by default).
std::string App::pan_text() const {
    static const int acts[4] = {A_PAN_FWD, A_PAN_LEFT, A_PAN_BACK, A_PAN_RIGHT};
    static const int arrows[4] = {ik::Up, ik::Left, ik::Down, ik::Right};
    std::string first, second;
    bool arrow_keys = true;
    for (int i = 0; i < 4; i++) {
        const KeyBind& a = settings_.keys[acts[i]][0];
        const KeyBind& b = settings_.keys[acts[i]][1];
        if (a.key) first += (first.empty() ? "" : " ") + key_label(a);
        if (b.key) second += (second.empty() ? "" : " ") + key_label(b);
        if (!(b.key == arrows[i] && !b.mods)) arrow_keys = false;
    }
    if (first.empty()) first = second, second.clear();
    if (second.empty()) return first.empty() ? "-" : first;
    return first + " or " + (arrow_keys ? std::string("the arrow keys") : second);
}

static bool placing_tool_(Tool t) {
    return t == Tool::PlaceObject || t == Tool::PlaceSpawn || t == Tool::PlacePad || t == Tool::PlaceStart || t == Tool::PlaceWater;
}
static bool brush_tool_(Tool t) { return (t >= Tool::Raise && t <= Tool::PaintCulture) || t == Tool::ScatterTrees || t == Tool::Ramp; }

int App::active_ctx() const {
    int m = C_GLOBAL;
    if (!doc_) return m;
    m |= C_MAP;
    if (editable()) {
        m |= C_EDIT;
        if (placing_tool_(tool_) || tool_ == Tool::PasteGhost) m |= C_PLACING;
        else if (brush_tool_(tool_)) m |= C_BRUSH;
        else if (tool_ == Tool::Select && !selection_.empty()) m |= C_SELECTION;
        if (glue_mode_ && tool_ == Tool::Select) m |= C_GLUE;
    }
    if (cpv_.on) m |= C_PREVIEW;
    if (frame_.on) m |= C_FRAME;
    return m;
}

// The tab row's order (ui_tabs): Terrain, Players and Units are pages of Build, Messages is inside Objectives & Triggers.
static const Tab kStrip[5] = {Tab::Map, Tab::Build, Tab::Triggers, Tab::Cameras, Tab::Environment};

int App::tab_strip_index() const {
    const Tab shown = tab_ == Tab::Players || tab_ == Tab::Units || tab_ == Tab::Terrain ? Tab::Build : tab_ == Tab::Messages ? Tab::Triggers : tab_;
    for (int i = 0; i < 5; i++)
        if (kStrip[i] == shown) return i;
    return -1;
}

void App::goto_tab(Tab t) {
    if (!doc_ && t != Tab::Map) return;  // with no map open only Map leads anywhere (ui_tabs)
    if (t == Tab::Build) { open_page(page_); return; }  // back to the page it was on (Sculpt and Paint arm their tool again)
    tab_ = t;
    if (tool_ != Tool::Select && tool_ != Tool::PickPoint) tool_ = Tool::Select;
}

// The size range of a brush tool on this map: the smallest is 1.5 cells of what the tool edits, so a click always reaches a point.
void App::brush_limits(Tool t, float& lo, float& hi) const {
    lo = 4.f;
    hi = 300.f;
    if (t == Tool::ScatterTrees) { lo = 5.f; hi = 200.f; return; }  // the Scatter slider's range (app_tabs.cpp)
    if (!doc_) return;
    const Terrain& tr = doc_->terrain;
    if (sculpt_tool(t)) lo = ma2::min_brush_radius(tr.scale);
    else if ((t == Tool::PaintColor || t == Tool::PaintTexture) && doc_->colormap.w > 0) lo = ma2::min_brush_radius(tr.size_x() / float(doc_->colormap.w));
    else if (t == Tool::PaintType && doc_->typemap_w > 0) lo = ma2::min_brush_radius(tr.size_x() / float(doc_->typemap_w));
    else if (t == Tool::PaintCulture && culture_.w > 0) lo = ma2::min_brush_radius(tr.size_x() / float(culture_.w));
    lo = std::min(lo, hi);
}

std::string App::brush_floor_note(Tool t) const {
    float lo, hi;
    brush_limits(t, lo, hi);
    if (!doc_) return "";
    const Terrain& tr = doc_->terrain;
    char b[200];
    if (sculpt_tool(t)) std::snprintf(b, sizeof b, "Smallest brush on this map: %.0f m (the ground has a height point every %g m).", lo, double(tr.scale));
    else if (t == Tool::PaintColor || t == Tool::PaintTexture) std::snprintf(b, sizeof b, "Smallest brush on this map: %.0f m (the colour map has a pixel every %.1f m).", lo, double(doc_->colormap.w > 0 ? tr.size_x() / float(doc_->colormap.w) : 0.f));
    else if (t == Tool::PaintType) std::snprintf(b, sizeof b, "Smallest brush on this map: %.0f m (the boundary map has a cell every %.1f m).", lo, double(doc_->typemap_w > 0 ? tr.size_x() / float(doc_->typemap_w) : 0.f));
    else if (t == Tool::PaintCulture) std::snprintf(b, sizeof b, "Smallest brush on this map: %.0f m (the plant map has a cell every %.1f m).", lo, double(culture_.w > 0 ? tr.size_x() / float(culture_.w) : 0.f));
    else std::snprintf(b, sizeof b, "Smallest brush: %.0f m.", lo);
    return b;
}

// brush_radius_ into the tool's range. When it had to grow, `say` puts the reason on the status line (once per change).
float App::brush_size_clamped(Tool t, bool say) {
    float lo, hi;
    brush_limits(t, lo, hi);
    const float was = brush_radius_;
    brush_radius_ = std::clamp(brush_radius_, lo, hi);
    if (say && brush_radius_ > was + 1e-3f) { status_ = brush_floor_note(t); status_time_ = 5; }
    return brush_radius_;
}

// The "Brush size" slider all sculpt / paint pages share: typed values below the smallest brush grow to it and the status says why.
bool App::brush_size_slider(Tool t) {
    float lo, hi;
    brush_limits(t, lo, hi);
    const bool ch = theme::slider_float("##brush", &brush_radius_, lo, hi, "%.0f m", ImGuiSliderFlags_Logarithmic);
    if (brush_radius_ < lo - 1e-3f || brush_radius_ > hi + 1e-3f) brush_size_clamped(t, true);
    return ch;
}

void App::brush_size_step(int dir) {
    if (tool_ == Tool::Ramp) { ramp_width_step(dir); return; }   // [ ] size the ramp's width
    float lo, hi;
    brush_limits(tool_, lo, hi);
    brush_radius_ = std::clamp(brush_step(brush_radius_, dir), lo, hi);
}

int App::strength_slot(Tool t) {
    switch (t) {
        case Tool::PaintTexture: return 0;
        case Tool::PaintColor: return 1;
        case Tool::Smooth: return 2;
        case Tool::Flatten: return 3;
        case Tool::PaintCulture: return 4;
        default: return -1;
    }
}

void App::brush_strength_step(int dir) {
    if (tool_ == Tool::Ramp) return;   // a ramp has no strength
    // Raise / Lower / Roughen have no speed: the keys change the height one stroke moves the ground, by one ground step.
    if (tool_ == Tool::Raise || tool_ == Tool::Lower || tool_ == Tool::Roughen) {
        const float step = doc_ ? std::max(0.01f, doc_->terrain.step()) : 0.5f;
        const float hi = doc_ ? std::max(20.f, std::min(doc_->terrain.max_h - doc_->terrain.min_h, 1000.f)) : 20.f;
        settings_.raise_amount = std::clamp(std::round((settings_.raise_amount + step * float(dir)) * 100.f) / 100.f, std::min(0.5f, step), hi);
        settings_.save();
        return;
    }
    brush_strength_ = std::clamp(std::round((brush_strength_ + 0.05f * float(dir)) * 100.f) / 100.f, 0.02f, 1.f);
}

// The camera turn of a middle-drag (and of Alt + left-drag): dx, dy are pixels of mouse travel. The view turns about a pivot that stays
// where it is on the screen: the selection if there is one, else the ground under the mouse where the drag began, else the
// view centre as before. The pivot is chosen once per drag (a gap of frames starts a new one).
void App::turn_view(float dx, float dy) {
    const float sx = (settings_.invert_rotate ? -1.f : 1.f) * settings_.rotate_speed;
    const float sy = (settings_.invert_pitch ? -1.f : 1.f) * settings_.rotate_speed;
    const int frame = ImGui::GetFrameCount();
    if (!orbit_pivot_on_ || frame - orbit_last_frame_ > 3) {
        orbit_pivot_on_ = false;
        Vec3 pv;
        if (doc_ && !selection_.empty() && gizmo_pivot(pv)) { orbit_pivot_ = pv; orbit_pivot_on_ = true; }
        else if (doc_) {  // the ground under the press (the mouse has moved on by the time the first turn arrives)
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 pr = ImGui::IsMouseDown(ImGuiMouseButton_Middle) ? io.MouseClickedPos[ImGuiMouseButton_Middle] : ImVec2(orb_x_, orb_y_);
            XMFLOAT3 o, d;
            cam_.ray(pr.x - vp_x, pr.y - vp_y, vp_w, vp_h, o, d);
            float t;
            if (doc_->terrain.raycast({o.x, o.y, o.z}, {d.x, d.y, d.z}, t)) { orbit_pivot_ = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t}; orbit_pivot_on_ = true; }
        }
        orbit_last_frame_ = frame;
        if (!orbit_pivot_on_) orbit_last_frame_ = -10;  // no pivot found: try again next frame (the view centre meanwhile)
    } else {
        orbit_last_frame_ = frame;
    }
    // The camera basis for a yaw / pitch (as Camera::update builds it): forward f from the eye to the target, right r, up u.
    struct Basis { float f[3], r[3], u[3]; };
    auto basis = [](float yaw, float pitch) {
        Basis b;
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        b.f[0] = std::sin(yaw) * cp; b.f[1] = -sp; b.f[2] = std::cos(yaw) * cp;
        const float up[3] = {pitch > 1.5f ? std::sin(yaw) : 0.f, pitch > 1.5f ? 0.f : 1.f, pitch > 1.5f ? std::cos(yaw) : 0.f};
        b.r[0] = up[1] * b.f[2] - up[2] * b.f[1]; b.r[1] = up[2] * b.f[0] - up[0] * b.f[2]; b.r[2] = up[0] * b.f[1] - up[1] * b.f[0];
        const float n = std::max(1e-6f, std::sqrt(b.r[0] * b.r[0] + b.r[1] * b.r[1] + b.r[2] * b.r[2]));
        for (float& c : b.r) c /= n;
        b.u[0] = b.f[1] * b.r[2] - b.f[2] * b.r[1]; b.u[1] = b.f[2] * b.r[0] - b.f[0] * b.r[2]; b.u[2] = b.f[0] * b.r[1] - b.f[1] * b.r[0];
        return b;
    };
    const Basis b0 = basis(cam_.yaw, cam_.pitch);
    float rel[3] = {0, 0, 0}, view[3] = {0, 0, 0};
    if (orbit_pivot_on_) {
        const float eye[3] = {cam_.tx - b0.f[0] * cam_.dist, cam_.ty - b0.f[1] * cam_.dist, cam_.tz - b0.f[2] * cam_.dist};
        rel[0] = orbit_pivot_.x - eye[0]; rel[1] = orbit_pivot_.y - eye[1]; rel[2] = orbit_pivot_.z - eye[2];
        for (int k = 0; k < 3; k++) view[0] += rel[k] * b0.r[k], view[1] += rel[k] * b0.u[k], view[2] += rel[k] * b0.f[k];
    }
    cam_.yaw += dx * 0.006f * sx;
    cam_.pitch = std::clamp(cam_.pitch + dy * 0.005f * sy, frame_.on ? -1.4f : 0.12f, 1.5533f);  // framing a shot may look up
    if (orbit_pivot_on_) {
        const Basis b1 = basis(cam_.yaw, cam_.pitch);
        float eye[3];
        for (int k = 0; k < 3; k++) eye[k] = (k == 0 ? orbit_pivot_.x : k == 1 ? orbit_pivot_.y : orbit_pivot_.z) - (view[0] * b1.r[k] + view[1] * b1.u[k] + view[2] * b1.f[k]);
        // The view centre stays on the ground (the camera eases its target's height to the terrain every frame): slide it along the view
        // axis to where the axis meets the ground. The eye, and so the pivot's place on the screen, does not change.
        const Terrain& tr = doc_->terrain;
        float t = cam_.dist;
        for (int k = 0; k < 8; k++) {
            const float x = std::clamp(eye[0] + b1.f[0] * t, 0.f, tr.size_x()), z = std::clamp(eye[2] + b1.f[2] * t, 0.f, tr.size_z());
            t = std::clamp((eye[1] - tr.height_at(x, z)) / std::max(0.1f, -b1.f[1]), 15.f, 5000.f);
        }
        cam_.dist = t;
        cam_.tx = eye[0] + b1.f[0] * t;
        cam_.ty = eye[1] + b1.f[1] * t;
        cam_.tz = eye[2] + b1.f[2] * t;
    }
}

// The brush's second ring: where the stroke is still half strength (the outer ring is where it ends; for Plateau, where its flat top ends),
// and what the next click does, two lines: "40 m  +2.0 m per stroke" / "ground 12.50 m  slope 18%". Drawn on the ground under the
// mouse with the foreground list so no panel hides it.
void App::draw_brush_rings(const ma2::Vec3& g) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const bool sculpt = sculpt_tool(tool_), soft = tool_ == Tool::PaintColor || tool_ == Tool::PaintTexture;
    if (sculpt || soft) {
        const float R = brush_radius_;
        // Sculpt: half strength at R * (1 - 0.5 (1 - Edge)); Plateau: where the flat top ends. Paint: the same curve over the soft part of the radius.
        const bool plateau = tool_ == Tool::Plateau || tool_ == Tool::PlateauDown;
        const float r = sculpt ? (plateau ? ma2::plateau_flat_radius(R, brush_hardness_) : ma2::half_strength_radius(R, brush_hardness_)) : R * (1.f - 0.5f * (1.f - brush_hardness_));
        const bool square = brush_square_ && sculpt;
        const int n = 48;
        ImVec2 pts[n + 1];
        bool ok[n + 1];
        for (int i = 0; i <= n; i++) {
            float x, z;
            if (square) {  // the corners of the square, 12 samples a side
                const int side = (i % n) / 12;
                const float u = float((i % n) % 12) / 12.f * 2.f - 1.f;
                const float sxs[4] = {u, 1.f, -u, -1.f}, szs[4] = {-1.f, u, 1.f, -u};
                x = g.x + sxs[side] * r;
                z = g.z + szs[side] * r;
            } else {
                const float a = float(i) / float(n) * 6.2831853f;
                x = g.x + std::cos(a) * r;
                z = g.z + std::sin(a) * r;
            }
            float sx, sy;
            ok[i] = cam_.project({x, doc_->terrain.height_at(x, z) + 0.15f, z}, vp_w, vp_h, sx, sy);
            pts[i] = ImVec2(sx + vp_x, sy + vp_y);
        }
        for (int i = 0; i < n; i++)
            if (ok[i] && ok[i + 1]) {
                dl->AddLine(pts[i], pts[i + 1], IM_COL32(0, 0, 0, 150), 3.f);
                dl->AddLine(pts[i], pts[i + 1], IM_COL32(255, 235, 120, 235), 1.5f);
            }
        if (sculpt) draw_height_points(g);
    }
    const Terrain& tr = doc_->terrain;
    const Tool shown = stroke_active_ ? stroke_tool_ : sculpt_now();
    char l1[96], l2[120] = "";
    if (tool_ == Tool::ScatterTrees) {   // How many a stroke would drop here (the ring's area x density), or that Ctrl erases
        if (stroke_active_ && sc_run_) std::snprintf(l1, sizeof l1, "%.0f m  %s %d", brush_radius_, sc_run_->erase ? "removed" : "placed", scatter_so_far());
        else if (holding(settings_.hold_reverse)) std::snprintf(l1, sizeof l1, "%.0f m  remove the set's kinds", brush_radius_);
        else std::snprintf(l1, sizeof l1, "%.0f m  about %.0f", brush_radius_, settings_.sc_density * 3.14159265f * brush_radius_ * brush_radius_ / 10000.f);
    } else if (tool_ == Tool::PaintType) std::snprintf(l1, sizeof l1, "%.0f m", brush_radius_);
    else if (shown == Tool::Raise || shown == Tool::Lower || shown == Tool::Roughen) {
        const char* sign = shown == Tool::Raise ? "+" : shown == Tool::Lower ? "-" : "+/-";
        if (stroke_active_ && shown != Tool::Roughen) {
            // During the stroke: the ground here now and what it moved since the stroke began.
            auto at0 = [&](float x, float z) {  // the ground at the stroke's start, on the saved steps and the game's diagonal
                const float fx = std::clamp(x / tr.scale, 0.f, float(tr.w - 1)), fz = std::clamp(z / tr.scale, 0.f, float(tr.h - 1));
                const int x0 = std::min(int(fx), tr.w - 2), z0 = std::min(int(fz), tr.h - 2);
                const float tx = fx - float(x0), tz = fz - float(z0);
                auto hs = [&](int gx, int gz) { return tr.stored(stroke_h0_.size() == tr.hgt.size() ? stroke_h0_[size_t(gz) * tr.w + gx] : tr.at(gx, gz)); };
                const float a = hs(x0, z0), b = hs(x0 + 1, z0), c = hs(x0, z0 + 1), d = hs(x0 + 1, z0 + 1);
                return tx >= tz ? a + (b - a) * tx + (d - b) * tz : a + (c - a) * tz + (d - c) * tx;
            };
            const float now = tr.height_at(g.x, g.z);
            std::snprintf(l1, sizeof l1, "ground %.2f m (%+.2f)", now, now - at0(g.x, g.z));
        } else std::snprintf(l1, sizeof l1, "%.0f m  %s%.1f m per stroke", brush_radius_, sign, settings_.raise_amount);
    } else if (shown == Tool::Flatten) std::snprintf(l1, sizeof l1, "%.0f m  to %.2f m", brush_radius_, flatten_height_);
    else if (shown == Tool::Plateau || shown == Tool::PlateauDown) std::snprintf(l1, sizeof l1, "%.0f m  to %.2f m", brush_radius_, plateau_height_);
    else std::snprintf(l1, sizeof l1, "%.0f m  %.0f%%", brush_radius_, brush_strength_ * 100.f);
    if (sculpt) {
        const float sl = ma2::slope_at(tr, g.x, g.z);
        if (steep_tier(sl) > 0) std::snprintf(l2, sizeof l2, "ground %.2f m  slope %.0f%%: %s", tr.height_at(g.x, g.z), sl * 100.f, steep_tag(sl));
        else std::snprintf(l2, sizeof l2, "ground %.2f m  slope %.0f%%", tr.height_at(g.x, g.z), sl * 100.f);
    }
    brush_readout_ = l1;
    if (l2[0]) brush_readout_ += std::string("\n") + l2;
    const ImVec2 at(io.MousePos.x + 18.f * dpi_, io.MousePos.y + 14.f * dpi_);
    const float lh = ImGui::GetTextLineHeight();
    float y = at.y;
    size_t from = 0;
    while (from <= brush_readout_.size()) {
        size_t e = brush_readout_.find('\n', from);
        const std::string line = brush_readout_.substr(from, e == std::string::npos ? std::string::npos : e - from);
        dl->AddText(ImVec2(at.x + 1, y + 1), IM_COL32(0, 0, 0, 220), line.c_str());
        dl->AddText(ImVec2(at.x, y), IM_COL32(255, 255, 255, 255), line.c_str());
        y += lh + 1.f;
        if (e == std::string::npos) break;
        from = e + 1;
    }
}

// Height points shown: dots on the drawn ground at every grid point within R + one cell of the cursor, so the user sees what a
// brush can move. 4 px white inside the ring (those move), 2 px grey outside. Skipped when neighbouring dots are under 6 px apart.
void App::draw_height_points(const ma2::Vec3& g, float radius) {
    const Terrain& t = doc_->terrain;
    const float rin = radius >= 0.f ? radius : brush_radius_;   // the Ramp passes half its width
    const float R = rin + t.scale;
    float ax, ay, bx, by;
    const float cx = std::clamp(g.x, 0.f, t.size_x()), cz = std::clamp(g.z, 0.f, t.size_z());
    if (!cam_.project({cx, t.height_at(cx, cz), cz}, vp_w, vp_h, ax, ay)) return;
    const float cx2 = std::min(cx + t.scale, t.size_x()) == cx ? cx - t.scale : cx + t.scale;
    if (!cam_.project({cx2, t.height_at(cx2, cz), cz}, vp_w, vp_h, bx, by)) return;
    const float cz2 = std::min(cz + t.scale, t.size_z()) == cz ? cz - t.scale : cz + t.scale;
    float qx, qy;
    if (!cam_.project({cx, t.height_at(cx, cz2), cz2}, vp_w, vp_h, qx, qy)) return;
    const float gap = std::min(std::hypot(bx - ax, by - ay), std::hypot(qx - ax, qy - ay));
    if (gap < 6.f * dpi_) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const bool square = radius < 0.f && brush_square_;
    const int gx0 = std::max(0, int((g.x - R) / t.scale)), gx1 = std::min(t.w - 1, int((g.x + R) / t.scale) + 1);
    const int gz0 = std::max(0, int((g.z - R) / t.scale)), gz1 = std::min(t.h - 1, int((g.z + R) / t.scale) + 1);
    for (int gz = gz0; gz <= gz1; gz++)
        for (int gx = gx0; gx <= gx1; gx++) {
            const float dx = gx * t.scale - g.x, dz = gz * t.scale - g.z;
            const float d = square ? std::max(std::abs(dx), std::abs(dz)) : std::sqrt(dx * dx + dz * dz);
            if (d > R) continue;
            float sx, sy;
            if (!cam_.project({gx * t.scale, t.step_at(gx, gz), gz * t.scale}, vp_w, vp_h, sx, sy)) continue;
            const ImVec2 p(sx + vp_x, sy + vp_y);
            const bool inside = d < rin;
            const float rad = (inside ? 2.f : 1.f) * dpi_;
            dl->AddCircleFilled(p, rad + 0.8f, IM_COL32(0, 0, 0, 120));
            dl->AddCircleFilled(p, rad, inside ? IM_COL32(255, 255, 255, 235) : IM_COL32(150, 150, 150, 200));
        }
}

void App::dispatch_keys(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (!keymap_checked_) {  // the name table of input_map.h must be ImGui's, or saved keys would not load
        keymap_checked_ = true;
        for (int i = 0; i < ik::kCount; i++)
            if (strcmp(ik::names[i], ImGui::GetKeyName(ImGuiKey(ik::kBegin + i))) != 0) keymap_ok_ = false;
    }
    {  // PgUp / PgDn lift while placing: the lift belongs to one tool and template and starts over when either changes
        const int pt = placing_tool_(tool_) || tool_ == Tool::PasteGhost ? int(tool_) : -1;
        if (pt != place_lift_tool_ || place_template_ != place_lift_tmpl_) { place_lift_ = 0; place_lift_tool_ = pt; place_lift_tmpl_ = place_template_; }
    }
    const bool latch = esc_closed_popup_;  // ui_menu's Esc closed a pop-up this frame: this Esc is used up
    esc_closed_popup_ = false;
    // Keys work unless the player is typing, a dialog or menu is open, or Settings is waiting for a key to bind.
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || capture_act_ >= 0 || latch) return;
    const int mods = current_mods(), active = active_ctx();
    const Hold hold = settings_.hold();
    // Held actions (pan, turn the view) every frame the key is down and still means that action.
    for (int a = 0; a < A_COUNT; a++) {
        const ActRow& r = kActTable[a];
        if (r.kind != Kind::Held || r.pending || row_rank(r, active) < 0) continue;
        bool on = false;
        for (const KeyBind& b : settings_.keys[a])
            if (b.key && ImGui::IsKeyDown(ImGuiKey(b.key)) && resolve(settings_.keys, b.key, mods, active, hold) == a) on = true;
        if (on) run_action(a, dt);
    }
    // Presses: each key resolves to one action (the most specific active one) and that action runs once.
    for (int key = ik::kBegin; key < ik::kEnd; key++) {
        if (!key_bindable(key) || !ImGui::IsKeyPressed(ImGuiKey(key), true)) continue;
        const int a = resolve(settings_.keys, key, mods, active, hold);
        if (a == A_NONE) continue;
        if (a < A_COUNT) {
            const Kind k = kActTable[a].kind;
            if (k == Kind::Held) continue;
            if (k == Kind::Press && !ImGui::IsKeyPressed(ImGuiKey(key), false)) continue;
        } else if (!ImGui::IsKeyPressed(ImGuiKey(key), false)) {
            continue;  // the quarter turns step once per press
        }
        run_action(a, dt);
    }
}

void App::run_action(int act, float dt) {
    if (act >= 0 && act < A_COUNT + 2) { key_counts_[act]++; last_act_ = act; }
    ImGuiIO& io = ImGui::GetIO();
    const bool placing_tool = placing_tool_(tool_);
    const bool ghost = tool_ == Tool::PasteGhost;  // the paste ghost: Q / E turn the whole group about its centre
    switch (act) {
        // ---- camera
        case A_PAN_FWD: case A_PAN_BACK: case A_PAN_LEFT: case A_PAN_RIGHT: {
            const float fx = std::sin(cam_.yaw), fz = std::cos(cam_.yaw), rx = std::cos(cam_.yaw), rz = -std::sin(cam_.yaw);
            const float speed = cam_.dist * 1.2f * dt * settings_.pan_speed * (io.KeyShift ? 2.5f : 1.f);
            if (act == A_PAN_FWD) { cam_.tx += fx * speed; cam_.tz += fz * speed; }
            if (act == A_PAN_BACK) { cam_.tx -= fx * speed; cam_.tz -= fz * speed; }
            if (act == A_PAN_RIGHT) { cam_.tx += rx * speed; cam_.tz += rz * speed; }
            if (act == A_PAN_LEFT) { cam_.tx -= rx * speed; cam_.tz -= rz * speed; }
            break;
        }
        case A_VIEW_LEFT: case A_VIEW_RIGHT: {  // the middle-drag rate for 5 px a frame at 60 fps
            const float k = (settings_.invert_rotate ? -1.f : 1.f) * settings_.rotate_speed;
            cam_.yaw += (act == A_VIEW_RIGHT ? 1.f : -1.f) * 5.f * dt * 60.f * 0.006f * k;
            break;
        }
        case A_VIEW_UP: case A_VIEW_DOWN: {
            const float k = (settings_.invert_pitch ? -1.f : 1.f) * settings_.rotate_speed;
            cam_.pitch = std::clamp(cam_.pitch + (act == A_VIEW_DOWN ? 1.f : -1.f) * 5.f * dt * 60.f * 0.005f * k, frame_.on ? -1.4f : 0.12f, 1.5533f);
            break;
        }
        case A_ZOOM_IN: case A_ZOOM_OUT: {  // one wheel notch about the view centre
            const float w = (act == A_ZOOM_IN ? 1.f : -1.f) * settings_.zoom_speed * (settings_.invert_zoom ? -1.f : 1.f);
            cam_.dist = std::clamp(cam_.dist * std::pow(0.87f, w), 15.f, 5000.f);
            break;
        }
        case A_TOP_DOWN: {
            static float saved = 0.95f;
            if (cam_.pitch < 1.5f) { saved = cam_.pitch; cam_.pitch = 1.5533f; cam_.yaw = 0; }
            else cam_.pitch = saved;
            break;
        }
        case A_RESET_VIEW: reset_view(); break;
        case A_ZOOM_SEL: zoom_to_selection(); break;
        case A_HELPERS: show_helpers_ = !show_helpers_; objects_dirty_ = true; break;
        // ---- editing
        case A_CANCEL:
            // Framing a shot, glue and the cutscene preview take Esc first; otherwise it steps back one level like a right-click: out of a
            // placing / painting tool first (what was just placed stays selected), then the selection.
            if (frame_.on) { frame_leave(); break; }
            if (cpv_.on) { cutscene_preview_stop(); break; }
            if (glue_mode_ && tool_ == Tool::Select && editable()) {
                glue_mode_ = false;
                glue_first_ = 0;
                status_ = "Glue tool off.";
                status_time_ = 3;
                break;
            }
            place_drag_ = false;
            if (tool_ == Tool::Ramp && ramp_stage_ == 1) ramp_cancel(true);   // Esc after click 1 starts the ramp again; the tool stays armed
            else if (tool_ != Tool::Select) tool_ = Tool::Select;
            else selection_.clear();
            break;
        case A_FRAME_USE: frame_use(); break;
        case A_DELETE: delete_selection(); break;
        case A_TURN_LEFT:
            if (ghost) paste_heading_ -= 0.2618f;
            else if (tool_ == Tool::PlaceObject && snap_turn(-1)) {}  // joined: the next way to join at this joint
            else if (placing_tool) turn_place_heading(-1);  // 15 degrees (or the angle step), onto the step
            else rotate_selection(-0.2618f);
            break;
        case A_TURN_RIGHT:
            if (ghost) paste_heading_ += 0.2618f;
            else if (tool_ == Tool::PlaceObject && snap_turn(1)) {}
            else if (placing_tool) turn_place_heading(1);
            else rotate_selection(0.2618f);
            break;
        // Quarter-turn key (Ctrl) + Q / E: a quarter turn, joined or not.
        case A_TURN_QUARTER_L:
            if (ghost) paste_heading_ -= 1.5708f;
            else if (placing_tool) place_heading_ -= 1.5708f;
            else rotate_selection(-1.5708f);
            break;
        case A_TURN_QUARTER_R:
            if (ghost) paste_heading_ += 1.5708f;
            else if (placing_tool) place_heading_ += 1.5708f;
            else rotate_selection(1.5708f);
            break;
        // Arrow keys with a selection: 1 m (Shift 10 m) along the world axis nearest the camera's forward / right; PgUp / PgDn 0.5 m
        // (Shift 5 m). Shift here is the step size, not the place-freely key: the floor stays (type a height or drag the lift knob
        // with the place-freely key to go under).
        case A_NUDGE_FWD: case A_NUDGE_BACK: case A_NUDGE_LEFT: case A_NUDGE_RIGHT:
            if (selection_positionless()) {  // nothing here can move: the arrows keep panning
                run_action(act == A_NUDGE_FWD ? A_PAN_FWD : act == A_NUDGE_BACK ? A_PAN_BACK : act == A_NUDGE_LEFT ? A_PAN_LEFT : A_PAN_RIGHT, dt);
                break;
            }
            nudge_selection(act == A_NUDGE_FWD ? 1 : act == A_NUDGE_BACK ? -1 : 0, act == A_NUDGE_RIGHT ? 1 : act == A_NUDGE_LEFT ? -1 : 0);
            break;
        case A_RAISE: case A_LOWER: {
            const float step = (io.KeyShift ? 5.f : 0.5f) * (act == A_RAISE ? 1.f : -1.f);
            if (tool_ == Tool::Select) raise_selection(step);
            else {  // placing: the next piece lands this far above the click
                place_lift_ = std::clamp(place_lift_ + step, 0.f, 200.f);
                char b[160];
                snprintf(b, sizeof b, "Placing %.1f m above the ground. %s / %s lift and lower it; it starts over when you change tool.", place_lift_, kt(A_RAISE).c_str(), kt(A_LOWER).c_str());
                status_ = b;
                status_time_ = 3;
            }
            break;
        }
        case A_SNAP: snap_selection_to_ground(); break;
        case A_UNDO: undo(); break;
        case A_REDO: redo(); break;
        case A_COPY: copy_selection(); break;
        case A_CUT: cut_selection(); break;
        case A_PASTE:  // the copies hang on the mouse until a click puts them down (handle_viewport_input)
            if (clipboard_.empty()) { status_ = "Nothing copied yet: select objects and press " + kt(A_COPY) + " first."; status_time_ = 4; }
            else arm_paste(clipboard_, false);
            break;
        case A_DUPLICATE: duplicate_selected(); break;
        case A_SELECT_ALL: select_all(); break;
        case A_INVERT_SEL: invert_selection(); break;
        case A_SELECT_SAME:
            if (!selection_.empty()) select_same_type(*selection_.begin(), true);
            break;
        case A_PLACE_AGAIN: place_again(); break;
        case A_PICK_TYPE: pick_selected_type(); break;
        // ---- brush: one handler for [ and ]
        case A_BRUSH_SMALLER: case A_BRUSH_BIGGER: {
            brush_size_step(act == A_BRUSH_BIGGER ? 1 : -1);
            break;
        }
        case A_BRUSH_WEAKER: case A_BRUSH_STRONGER:
            if (tool_ == Tool::Ramp) {}
            else if (tool_ == Tool::PaintType || tool_ == Tool::ScatterTrees) {
                status_ = std::string(tool_ == Tool::PaintType ? "Boundaries" : "Scatter") + " has no strength: use the Density slider on the left instead.";
                status_time_ = 4;
            } else brush_strength_step(act == A_BRUSH_STRONGER ? 1 : -1);
            break;
        // ---- handles
        case A_GIZMO_MOVE: gizmo_mode_ = 0; break;
        case A_GIZMO_ROTATE: gizmo_mode_ = 1; break;
        case A_GIZMO_TILT:
            if (gizmo_can_tilt()) gizmo_mode_ = 2;
            else { status_ = "Tilt is greyed out: nothing selected can lean."; status_time_ = 3; }
            break;
        case A_GIZMO_CYCLE:
            gizmo_mode_ = (gizmo_mode_ + 1) % 3;
            if (gizmo_mode_ == 2 && !gizmo_can_tilt()) gizmo_mode_ = 0;
            break;
        case A_SLOPE_MODE: cycle_slope_mode(); break;
        // ---- snapping
        case A_GRID_TOGGLE: grid_toggle(); break;
        case A_GRID_SMALLER: grid_step(-1); break;
        case A_GRID_BIGGER: grid_step(1); break;
        case A_SNAP_JOINTS:
            settings_.snap.joints = !settings_.snap.joints;
            snap_prefs_save();
            status_ = settings_.snap.joints ? "Snapping to joints is on." : "Snapping to joints is off: pieces land where you put them.";
            status_time_ = 3;
            break;
        case A_ANGLE_SNAP:
            settings_.snap.angle = !settings_.snap.angle;
            snap_prefs_save();
            status_ = settings_.snap.angle ? "Angle steps are on (" + std::to_string(int(settings_.snap.angle_deg)) + " degrees)." : "Angle steps are off: headings are free.";
            status_time_ = 3;
            break;
        case A_BRING_FORWARD: layer_forward(); break;
        case A_SEND_BACK: layer_back(); break;
        // ---- windows and tabs
        case A_FIND:  // the search box of the browser page that is open (Place when the open page has none)
            if (!doc_) break;
            if (tab_ != Tab::Build || (page_ != Page::Place && page_ != Page::Scatter && page_ != Page::RoadsWalls)) open_page(Page::Place);
            focus_filter_ = true;
            break;
        case A_TAB_NEXT: case A_TAB_PREV: {
            const int i = tab_strip_index(), n = doc_ ? 5 : 1;
            goto_tab(kStrip[((i < 0 ? 0 : i) + (act == A_TAB_NEXT ? 1 : n - 1)) % n]);
            break;
        }
        case A_TAB_1: case A_TAB_2: case A_TAB_3: case A_TAB_4: case A_TAB_5:
            goto_tab(kStrip[act - A_TAB_1]);
            break;
        case A_PAGE_SELECT: case A_PAGE_PLACE: case A_PAGE_SCATTER: case A_PAGE_ROADS: case A_PAGE_SCULPT: case A_PAGE_PAINT: case A_PAGE_WATER: case A_PAGE_STARTS:
            if (!doc_) break;
            if (!editable() && act != A_PAGE_SELECT) {
                status_ = "This is a real level: only Select works until you make an editable copy (Map tab).";
                status_time_ = 4;
            } else {
                open_page(Page(act - A_PAGE_SELECT));
            }
            break;
        // ---- files and windows
        case A_NEW: cmd_new(); break;
        case A_OPEN: cmd_open(); break;
        case A_NEW_MISSION: cmd_new_kind(false); break;
        case A_CHECK:
            if (doc_) { validation_ = check_now(); show_validation_ = true; }
            break;
        case A_SAVE: case A_SAVE_AS: case A_TEST:
            if (editable()) {
                if (act == A_SAVE) cmd_save(false);
                else if (act == A_SAVE_AS) cmd_save(true);
                else cmd_test();
            } else if (doc_ && doc_->read_only) {
                // On a real level the keys say why nothing happened (the toolbar buttons say it in their tooltips).
                status_ = std::string("This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab, then ") +
                          (act == A_TEST ? "test" : "save") + " your copy.";
                status_time_ = 8;
            }
            break;
        case A_HELP: show_help_ = true; help_ctx_ = true; break;
        case A_PREFS: open_prefs_ = true; break;
        case A_SHORTCUTS: show_keys_ = !show_keys_; break;
        default: break;  // a pending action (the resolver never returns one)
    }
}

}  // namespace ed
