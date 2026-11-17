// The cutscene timeline strip under the map view: the shots as blocks on a time ruler, a playhead the preview follows, the
// shot length dragged by its right edge (one number per shot, never a slider), shots reordered by dragging a block sideways.
// Same objects as the Cameras & Cutscenes panel: every edit goes through core/cutscene (set_shot_length ...), one undo step per
// gesture. Items carry fixed ids for the automation: ##tl_shot<n>, ##tl_edge<n>, ##tl_ruler, ##tl_add, ##tl_play, ##tl_end.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "../core/cutscene.h"
#include "../core/triggers.h"
#include "app.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "theme.h"
#include "trigger_ui.h"
#include "ui_tips.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

namespace {
constexpr float kHeadH = 22.f, kRulerH = 16.f, kShotsH = 30.f, kLaneH = 20.f;
constexpr float kStripH = kHeadH + kRulerH + kShotsH + 4 * kLaneH;  // 148
constexpr float kMapMin = 300.f;                                  // the strip folds itself when the map would be under this many pixels
constexpr float kRightPad = 12.f;
constexpr const char* kLaneNames[4] = {"Sound", "Text", "Units", "Other"};

ImU32 col(const ImVec4& c, float a = -1) {
    ImVec4 v = c;
    if (a >= 0) v.w = a;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// The lane an event sits in: 0 Sound, 1 Text, 2 Units, 3 Other.
int event_lane(const NameTable& names, const WObj& o, bool order) {
    if (order) return 2;
    const std::string c = names.label(o.cls);
    if (c == "MAActionSound" || c == "MAActionPlayVoice" || c == "MAActionMusic") return 0;
    if (c == "MAActionBroadcast") return 1;
    if (c == "MAActionShow" || c == "MAActionRemoveEntity" || c == "MAActionAI" || c == "MAActionNation" || c == "MAActionTeleport") return 2;
    return 3;
}

// Seconds of a 16-bit PCM WAV (0 when it cannot be read).
float wav_seconds(const std::vector<uint8_t>& b) {
    size_t p = 12;
    uint32_t rate = 0, data = 0;
    while (p + 8 <= b.size()) {
        uint32_t sz;
        std::memcpy(&sz, &b[p + 4], 4);
        if (!std::memcmp(&b[p], "fmt ", 4) && p + 24 <= b.size()) std::memcpy(&rate, &b[p + 16], 4);
        if (!std::memcmp(&b[p], "data", 4)) { data = sz; break; }
        p += 8 + size_t(sz) + (sz & 1);
    }
    return rate ? float(data) / float(rate) : 0.f;
}

std::string fmt_t(float t) { char b[24]; snprintf(b, sizeof b, "%.1f", double(t)); return b; }
}  // namespace

// Runs a menu's structural edit one frame later (the strip and the panel hold pointers into the world's object list while they draw).

bool App::timeline_wanted() const {
    static const uint32_t K_CINEMA = key_of("MACinema");
    if (!doc_ || tab_ != Tab::Cameras || !cine_uid_ || cpv_.still) return false;
    const WObj* c = object_by_uid(doc_->world, cine_uid_);
    return c && c->cls == K_CINEMA;
}

float App::timeline_height() {
    if (tl_later_) { auto f = std::move(tl_later_); tl_later_ = nullptr; f(); }
    if (!timeline_wanted()) return 0;
    const float full = kStripH * dpi_, head = kHeadH * dpi_;
    tl_.auto_folded = vp_h - full < kMapMin * dpi_;
    return settings_.timeline == 1 || tl_.auto_folded ? head : full;
}

void App::timeline_draw() {
    if (!doc_) return;
    Scenario& s = *doc_;
    const bool ro = s.read_only;
    const float d = dpi_;
    ImGuiIO& io = ImGui::GetIO();
    const uint32_t cin = cine_uid_;
    const WObj* cino = object_by_uid(s.world, cin);
    if (!cino) return;
    const std::vector<ShotInfo> shots = cutscene_shots(s.world, cin);
    const int n = int(shots.size());
    const float total = cutscene_total(shots);
    if (cin != tl_.last_cin) {
        tl_.last_cin = cin;
        tl_.shot = 0;
        tl_.playhead = 0;
        tl_.drag = 0;
    }
    tl_.shot = std::clamp(tl_.shot, 0, std::max(0, n - 1));
    const bool folded = tl_h_ < 100 * d;
    const bool this_preview = cpv_.on && cpv_.cin == cin && !cpv_.still;
    if (this_preview && tl_.drag != 3) tl_.playhead = std::clamp(cpv_.t, 0.f, total);
    tl_.playhead = std::clamp(tl_.playhead, 0.f, total);

    const float wx = vp_x, wy = vp_y + vp_h, ww = vp_w, wh = tl_h_;
    tl_.rect[0] = wx; tl_.rect[1] = wy; tl_.rect[2] = ww; tl_.rect[3] = wh;
    const theme::Tokens& tk = theme::tok();
    ImGui::SetNextWindowPos(ImVec2(wx, wy));
    ImGui::SetNextWindowSize(ImVec2(ww, wh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    const bool open = ImGui::Begin("##timeline", nullptr, fl);
    ImGui::PopStyleVar(3);
    if (!open) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = col(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), 1.f), dim = col(tk.text_hint, 0.55f), line = col(tk.text_hint, 0.25f),
                txt = col(tk.text_item), accent = col(tk.accent);
    dl->AddRectFilled(ImVec2(wx, wy), ImVec2(wx + ww, wy + wh), bg);
    dl->AddLine(ImVec2(wx, wy), ImVec2(wx + ww, wy), col(tk.accent, 0.6f), 1.f);

    // ---- header: name, Preview / Stop, Add shot, total, the player's hand-back, fold
    ImGui::SetCursorScreenPos(ImVec2(wx + 8 * d, wy + 3 * d));
    {
        std::string nm = cino->description().empty() ? "Cutscene" : cino->description();
        ImGui::TextColored(tk.text_bright, "%s", nm.c_str());
        ImGui::SameLine(0, 12 * d);
        const bool playing = this_preview && !cpv_.paused;
        ImGui::BeginDisabled(n == 0);
        if (ImGui::SmallButton(playing ? "Stop##tl_play" : "Preview##tl_play")) {
            if (playing) cutscene_preview_stop();
            else cutscene_preview_at(cin, tl_.playhead >= total - 0.05f ? 0.f : tl_.playhead, false);
        }
        ImGui::SetItemTooltip("%s", playing ? ("Stop the preview (" + kt(A_CANCEL) + " also stops it).").c_str()
                                            : ("Play the cutscene in the map view from the playhead (" + kt(A_CANCEL) + " stops).").c_str());
        if (this_preview && cpv_.paused) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop##tl_stop")) cutscene_preview_stop();
            ImGui::SetItemTooltip("%s", ("Back to the map (" + kt(A_CANCEL) + ").").c_str());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ro || n == 0);
        if (ImGui::SmallButton("Add shot##tl_add")) {
            add_cutscene_shot(cin);
            tl_.shot = std::max(0, int(cutscene_shots(s.world, cin).size()) - 1);
        }
        ImGui::SetItemTooltip("Add a still 5-second shot of the view you have now, after the last one.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ro || n == 0);
        {
            const std::string al = "+ Add at " + fmt_t(tl_.playhead) + " s##tl_addev";
            if (ImGui::SmallButton(al.c_str())) { tl_.add_t = std::min(std::round(tl_.playhead * 10.f) / 10.f, total); tl_.add_end = false; ImGui::OpenPopup("##tl_addmenu"); }
            ImGui::SetItemTooltip("Add something that happens at the playhead: a sound, a message, a unit walking. Click the ruler first to set the moment.");
            if (ImGui::BeginPopup("##tl_addmenu")) { cut_add_menu(cin, tl_.add_t, false); ImGui::EndPopup(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 14 * d);
        ImGui::TextDisabled("Total %.1f s", total);
        {
            // The hand-back note takes the room that is left (long, short, or none) and never runs under "Hide timeline".
            const float fold_room = ImGui::CalcTextSize("Hide timeline").x + ImGui::GetStyle().FramePadding.x * 2 + 16 * d;
            char longt[80], shortt[48];
            snprintf(longt, sizeof longt, "Control returns to the player at %.1f s", total);
            snprintf(shortt, sizeof shortt, "You play at %.1f s", total);
            const float x_after = ImGui::GetItemRectMax().x + 14 * d;
            const float limit = wx + ww - fold_room;
            const char* use = x_after + ImGui::CalcTextSize(longt).x < limit ? longt : x_after + ImGui::CalcTextSize(shortt).x < limit ? shortt : nullptr;
            if (use) {
                ImGui::SameLine(0, 14 * d);
                ImGui::TextDisabled("%s", use);
            }
        }
        {
            const int ne = int(cutscene_end_actions(s.world, gd_.names(), cin).size());
            const std::string el = "When it ends: " + std::to_string(ne) + (ne == 1 ? " action" : " actions");
            const float btn_w = ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
            const float fold_w = ImGui::CalcTextSize("Hide timeline").x + ImGui::GetStyle().FramePadding.x * 2 + 8 * d;
            const float end_x = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + 14 * d + ImGui::CalcTextSize(el.c_str()).x + 4 * d + btn_w;
            if (end_x + 16 * d < wx + ww - fold_w - 8 * d) {
                ImGui::SameLine(0, 14 * d);
                ImGui::TextDisabled("%s", el.c_str());
                ImGui::SameLine(0, 4 * d);
                ImGui::BeginDisabled(ro || n == 0);
                if (ImGui::SmallButton("+##tl_endadd")) { tl_.add_end = true; ImGui::OpenPopup("##tl_endmenu"); }
                ImGui::SetItemTooltip("Add something that runs when the last shot ends (the hand-back: a message, a unit given orders).");
                if (ImGui::BeginPopup("##tl_endmenu")) { cut_add_menu(cin, total, true); ImGui::EndPopup(); }
                ImGui::EndDisabled();
            }
        }
        const char* fold_label = folded ? "Show timeline##tl_fold" : "Hide timeline##tl_fold";
        const float fw = ImGui::CalcTextSize(fold_label, nullptr, true).x + 8 * d;
        ImGui::SetCursorScreenPos(ImVec2(wx + ww - fw - 8 * d, wy + 3 * d));
        ImGui::BeginDisabled(tl_.auto_folded);
        if (ImGui::SmallButton(fold_label)) {
            settings_.timeline = settings_.timeline == 1 ? 0 : 1;
            settings_.save();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", tl_.auto_folded ? "The map would be too small with the strip open: make the window taller." : "Fold the strip down to this line, or open it again.");
    }
    if (folded) { ImGui::End(); return; }

    // ---- scale: the whole cutscene fits the width (8 s at least), with room after it for "You play"
    const float lw = 52 * d, x0 = wx + lw + 4 * d, x1 = wx + ww - kRightPad * d;
    const float span = std::max(8.f, total);
    float px = std::max(4.f, (x1 - x0) / (span * 1.12f));
    if (tl_.drag == 1 && tl_.drag_scale > 0) px = tl_.drag_scale;  // frozen while an edge is dragged, so the block does not run from the mouse
    const float yr = wy + kHeadH * d, ys = yr + kRulerH * d, yl = ys + kShotsH * d, yend = wy + wh;

    // lanes: labels and rows (the events are drawn into them below)
    dl->AddText(ImVec2(wx + 8 * d, ys + (kShotsH * d - ImGui::GetFontSize()) * 0.5f), txt, "Shots");
    for (int i = 0; i < 4; i++) {
        const float y = yl + float(i) * kLaneH * d;
        if (i % 2 == 0) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + kLaneH * d), col(tk.text_hint, 0.07f));
        dl->AddText(ImVec2(wx + 8 * d, y + (kLaneH * d - ImGui::GetFontSize()) * 0.5f), dim, kLaneNames[i]);
    }

    // ---- ruler
    {
        static const float kSteps[] = {0.5f, 1, 2, 5, 10, 15, 30, 60, 120};
        float step = kSteps[8];
        for (float st : kSteps) if (st * px >= 44 * d) { step = st; break; }
        for (int k = 0; k * step <= span * 1.12f + 0.001f; k++) {
            const float x = x0 + float(k) * step * px;
            if (x > x1) break;
            dl->AddLine(ImVec2(x, yr + 8 * d), ImVec2(x, yr + kRulerH * d), line);
            char tb[16];
            if (step < 1) snprintf(tb, sizeof tb, "%.1f s", double(k) * double(step));
            else snprintf(tb, sizeof tb, "%g s", double(k) * double(step));
            dl->AddText(ImVec2(x + 3 * d, yr + 1 * d), dim, tb);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, yr));
        ImGui::InvisibleButton("##tl_ruler", ImVec2(std::max(1.f, x1 - x0), kRulerH * d));
        if (ImGui::IsItemActivated()) tl_.drag = 3;
        if (ImGui::IsItemActive() && tl_.drag == 3 && n > 0) {
            // Whole tenths, so the "+ Add at 9.0 s" label and the event's time are the same number (a click used to land on 8.99).
            const float t = std::clamp(std::round((io.MousePos.x - x0) / px * 10.f) / 10.f, 0.f, total);
            tl_.playhead = t;
            cutscene_preview_at(cin, std::min(t, std::max(0.f, total - 0.02f)), true);
        }
        ImGui::SetItemTooltip("Click or drag: the map view shows the camera at that moment.");
    }

    // ---- shot blocks, with the length edge on each block's right side
    const float edge = 7 * d;
    const ImU32 colA = col(tk.select_fill, 0.55f), colB = col(tk.info_card, 0.85f);
    int edge_hot = -1;
    for (int k = 0; k < n; k++) {
        const ShotInfo& sh = shots[size_t(k)];
        const float xs = x0 + sh.start * px, xe = x0 + (sh.start + sh.len) * px;
        const bool sel = k == tl_.shot;
        const ImVec2 a(xs + 1, ys + 2 * d), b(xe - 1, yl - 2 * d);
        dl->AddRectFilled(a, b, k % 2 ? colB : colA, 3 * d);
        if (shot_hold(sh) > 0) {  // a camera move that ends early: the hold shows as a warning-coloured bar under the block
            const float xh = x0 + (sh.start + sh.move_len) * px;
            dl->AddRectFilled(ImVec2(std::min(xh, b.x), b.y - 4 * d), b, col(tk.warn, 0.9f), 0);
        }
        dl->AddRect(a, b, sel ? accent : line, 3 * d, 0, sel ? 2.f : 1.f);
        char lb[64];
        snprintf(lb, sizeof lb, "%d  %.1f s%s", k + 1, double(sh.len), sh.still ? "" : "  moving");
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(xs + 6 * d, ys + (kShotsH * d - ImGui::GetFontSize()) * 0.5f), txt, lb);
        dl->PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(xs, ys));
        const std::string sid = "##tl_shot" + std::to_string(k + 1);
        ImGui::InvisibleButton(sid.c_str(), ImVec2(std::max(4.f, xe - xs - edge), kShotsH * d));
        if (ImGui::IsItemActivated()) {
            tl_.shot = k;
            tl_.playhead = sh.start;
            if (sh.ctrl) select_uid(sh.ctrl);  // Properties follows the shot picked
            if (this_preview) cutscene_preview_at(cin, std::min(sh.start, std::max(0.f, total - 0.02f)), cpv_.paused);
            tl_.drag = 2;
            tl_.drag_shot = k;
            tl_.drag_x0 = io.MousePos.x;
            tl_.drag_moved = tl_.drag_pushed = false;
        }
        if (ImGui::IsItemHovered() && tl_.drag == 0)
            ImGui::SetTooltip("Shot %d: %.1f s%s\nClick to select it. Drag sideways to play it earlier or later.", k + 1, double(sh.len),
                              shot_hold(sh) > 0 ? " (its camera move ends early and the shot then holds still)" : "");

        ImGui::SetCursorScreenPos(ImVec2(std::max(xs, xe - edge), ys));
        const std::string eid = "##tl_edge" + std::to_string(k + 1);
        ImGui::InvisibleButton(eid.c_str(), ImVec2(std::min(edge, xe - xs), kShotsH * d));
        const bool hot = ImGui::IsItemHovered() || (tl_.drag == 1 && tl_.drag_shot == k);
        if (hot) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); edge_hot = k; }
        dl->AddLine(ImVec2(xe - 3 * d, ys + 6 * d), ImVec2(xe - 3 * d, yl - 6 * d), hot ? accent : line, hot ? 3.f : 2.f);
        if (ImGui::IsItemActivated() && !ro) {
            tl_.shot = k;
            tl_.drag = 1;
            tl_.drag_shot = k;
            tl_.drag_x0 = io.MousePos.x;
            tl_.drag_len0 = sh.len;
            tl_.drag_scale = px;
            tl_.drag_pushed = false;
        }
        if (ImGui::IsItemActive() && tl_.drag == 1 && tl_.drag_shot == k && !ro) {
            float nl = tl_.drag_len0 + (io.MousePos.x - tl_.drag_x0) / tl_.drag_scale;
            if (!io.KeyShift) nl = std::round(nl * 2.f) / 2.f;  // Shift = free
            nl = std::clamp(nl, kShotMin, kShotMax);
            if (std::fabs(nl - sh.len) > 1e-4f) {
                if (!tl_.drag_pushed) { push_undo("shot length"); tl_.drag_pushed = true; }
                set_shot_length(s, sh.ctrl, nl);
            }
            ImGui::SetTooltip("%.1f s (total %.1f s)", double(nl), double(total - sh.len + nl));
        } else if (ImGui::IsItemHovered() && tl_.drag == 0) {
            ImGui::SetTooltip("Drag to change this shot's length (half seconds; hold %s to set any value).", "Shift");
        }
    }
    (void)edge_hot;

    // ---- the end of the cutscene: the player has the controls back
    {
        const float xe = x0 + total * px;
        dl->AddLine(ImVec2(xe, yr + 2 * d), ImVec2(xe, yend - 2 * d), accent, 1.f);
        const float xt = xe + 6 * d;
        if (x1 - xt > 20 * d) {
            dl->AddText(ImVec2(xt, ys + (kShotsH * d - ImGui::GetFontSize()) * 0.5f), accent, "You play");
            ImGui::SetCursorScreenPos(ImVec2(xt, ys));
            ImGui::InvisibleButton("##tl_end", ImVec2(x1 - xt, kShotsH * d));
            ImGui::SetItemTooltip("Control returns to the player at %.1f s, when the last shot ends.", double(total));
        }
    }

    // ---- events: diamonds in their lane at their moment (own clips draw a bar of their real length); drag = retime (one undo step)
    {
        const NameTable& names = gd_.names();
        const std::vector<CutEvent> evs = cutscene_events(s.world, names, cin);
        const std::vector<CutEvent> ends = cutscene_end_actions(s.world, names, cin);
        const ImU32 lane_col[4] = {col(tk.accent, 0.95f), col(tk.text_bright, 0.9f), col(tk.side_ally, 0.95f), col(tk.text_hint, 0.9f)};
        // each label stops where the next event of its lane begins
        auto lane_of_ev = [&](const CutEvent& e) { const WObj* o = object_by_uid(s.world, e.obj); return o ? event_lane(names, *o, e.order) : 3; };
        auto next_x = [&](const CutEvent& e, float cx) {
            float best = 1e9f;
            const int ln = lane_of_ev(e);
            for (const CutEvent& f : evs) if (f.obj != e.obj && lane_of_ev(f) == ln) { const float fx = x0 + f.t * px; if (fx > cx + 1.f && fx < best) best = fx; }
            return best;
        };
        auto draw_event = [&](const CutEvent& e, float cx, bool at_end, int end_count) {
            const WObj* o = object_by_uid(s.world, e.obj);
            if (!o) return;
            const int lane = event_lane(names, *o, e.order);
            const float y = yl + float(lane) * kLaneH * d, yc = y + kLaneH * d * 0.5f;
            const bool sel = tl_.ev == e.obj;
            const bool dragging = tl_.drag == 4 && tl_.drag_ev == e.obj;
            if (dragging) cx = x0 + tl_.drag_ev_t * px;
            cx = std::min(cx, x1 - 6 * d);
            const ImU32 c = lane_col[lane];
            if (!e.order && lane == 0 && o->cls == key_of("MAActionSound")) {  // an own clip: a bar as long as the clip
                const uint32_t cue = o->u32("CueEvent");
                if (cue >= kAudioBase && cue < kAudioBase + 100000u) {
                    auto it = s.audio.find(int(cue - kAudioBase));
                    const float len = it != s.audio.end() ? wav_seconds(it->second.wav) : 0.f;
                    if (len > 0.05f) dl->AddRectFilled(ImVec2(cx, yc - 3 * d), ImVec2(std::min(cx + len * px, x1), yc + 3 * d), col(tk.accent, 0.35f), 2 * d);
                }
            }
            const float r = 5 * d;
            dl->AddQuadFilled(ImVec2(cx, yc - r), ImVec2(cx + r, yc), ImVec2(cx, yc + r), ImVec2(cx - r, yc), c);
            if (sel) dl->AddQuad(ImVec2(cx, yc - r - 2), ImVec2(cx + r + 2, yc), ImVec2(cx, yc + r + 2), ImVec2(cx - r - 2, yc), col(tk.glow_halo1, 1.f), 2.f);
            std::string lb = at_end && end_count > 1 ? std::to_string(end_count) + " when it ends" : cut_event_label(e);
            const float tw = ImGui::CalcTextSize(lb.c_str()).x;
            const bool left = cx + r + 4 * d + tw > x1;  // near the right edge: the label goes to the left of the diamond
            const float stop = at_end ? wx + ww : std::min(wx + ww, next_x(e, cx) - 3 * d);
            dl->PushClipRect(ImVec2(x0, y), ImVec2(std::max(x0, stop), y + kLaneH * d), true);
            dl->AddText(ImVec2(left ? cx - r - 4 * d - tw : cx + r + 4 * d, yc - ImGui::GetFontSize() * 0.5f), txt, lb.c_str());
            dl->PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(cx - r - 2, y));
            const std::string id = "##tl_ev" + std::to_string(e.obj);
            ImGui::InvisibleButton(id.c_str(), ImVec2(2 * r + 4, kLaneH * d));
            if (ImGui::IsItemActivated()) {
                tl_.ev = e.obj;
                tl_.shot = std::clamp(e.shot, 0, std::max(0, n - 1));
                if (!at_end && !ro) { tl_.drag = 4; tl_.drag_ev = e.obj; tl_.drag_ev_t = e.t; tl_.drag_x0 = io.MousePos.x; tl_.drag_moved = false; }
            }
            if (ImGui::IsItemActive() && tl_.drag == 4 && tl_.drag_ev == e.obj) {
                if (std::fabs(io.MousePos.x - tl_.drag_x0) > 3 * d) tl_.drag_moved = true;
                if (tl_.drag_moved) {
                    float nt = std::max(0.f, (io.MousePos.x - x0) / px);
                    if (!io.KeyShift) nt = std::round(nt * 2.f) / 2.f;  // 0.5 s steps; Shift = free
                    tl_.drag_ev_t = std::min(nt, std::max(span, total));
                    ImGui::SetTooltip("%s s", fmt_t(tl_.drag_ev_t).c_str());
                }
            } else if (ImGui::IsItemHovered() && tl_.drag == 0) {
                ImGui::SetTooltip("%s\n%s%s s%s", lb.c_str(), at_end ? "When it ends, at " : "At ", fmt_t(e.t).c_str(),
                                  at_end ? "" : (std::string("  (shot ") + std::to_string(e.shot + 1) + " + " + fmt_t(e.offset) + " s)\nClick to select it, drag to move it.").c_str());
            }
            if (ImGui::IsItemDeactivated() && tl_.drag == 4 && tl_.drag_ev == e.obj) {
                const float nt = tl_.drag_ev_t;
                if (tl_.drag_moved && std::fabs(nt - e.t) > 0.004f && !at_end) {
                    const uint32_t eu = e.obj;
                    tl_later_ = [this, cin, eu, nt]() {
                        push_undo("move event");
                        cutscene_move_event(*doc_, gd_.names(), cin, eu, nt);
                        world_changed();
                    };
                }
                tl_.drag = 0;
                tl_.drag_ev = 0;
            }
        };
        for (const CutEvent& e : evs) draw_event(e, x0 + e.t * px, false, 0);
        if (!ends.empty()) draw_event(ends.front(), x0 + total * px, true, int(ends.size()));
    }

    // ---- shot reorder by dragging a block past half its neighbour; the gesture is one undo step
    if (tl_.drag == 2 && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ro && n > 1) {
        const float mx = io.MousePos.x;
        if (std::fabs(mx - tl_.drag_x0) > 6 * d) tl_.drag_moved = true;
        const int k = tl_.drag_shot;
        if (tl_.drag_moved && k >= 0 && k < n) {
            if (k + 1 < n && mx > x0 + (shots[size_t(k + 1)].start + shots[size_t(k + 1)].len * 0.5f) * px) {
                reorder_shot(cin, k, +1, !tl_.drag_pushed);
                tl_.drag_pushed = true;
                tl_.drag_shot = tl_.shot = k + 1;
            } else if (k > 0 && mx < x0 + (shots[size_t(k - 1)].start + shots[size_t(k - 1)].len * 0.5f) * px) {
                reorder_shot(cin, k, -1, !tl_.drag_pushed);
                tl_.drag_pushed = true;
                tl_.drag_shot = tl_.shot = k - 1;
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        tl_.drag = 0;
        tl_.drag_shot = -1;
        tl_.drag_ev = 0;
    }

    // ---- playhead
    {
        const float x = x0 + tl_.playhead * px;
        dl->AddLine(ImVec2(x, yr), ImVec2(x, yend), col(tk.glow_halo1, 0.95f), 1.5f);
        dl->AddTriangleFilled(ImVec2(x - 4 * d, yr), ImVec2(x + 4 * d, yr), ImVec2(x, yr + 7 * d), col(tk.glow_halo1, 0.95f));
    }
    ImGui::End();
}


// ---- Events ---------------------------------------------------------------------------------------------------------------

std::string App::cut_event_label(const CutEvent& e) {
    if (!doc_) return "";
    const Wdf& w = doc_->world;
    const WObj* o = object_by_uid(w, e.obj);
    if (!o) return "?";
    if (e.order) {
        const std::string u = unit_label(w, e.unit);
        const WObj* b = object_by_uid(w, order_behaviour(w, e.obj));
        const std::string c = b ? gd_.names().label(b->cls) : std::string();
        if (c == "MAPatrol") {
            const WObj* g = object_by_uid(w, order_path(w, e.obj));
            return u + (g && g->i32("TraverseType") == 1 ? " walks" : " patrols");
        }
        if (c == "MAAttack") return u + " attacks";
        if (c == "MADefend") return u + " defends";
        if (c == "MAFollow") return u + " follows";
        return u + " (order)";
    }
    const std::string cls = gd_.names().label(o->cls);
    if (cls == "MAActionBroadcast") {
        const uint32_t id = o->u32("BodyTextID");
        std::string words;
        if (const std::string* tx = own_text(const_cast<Scenario&>(*doc_), id)) words = *tx;
        else if (id) words = gd_.game_text(id);
        if (words.size() > 28) words = words.substr(0, 27) + "...";
        return words.empty() ? "message" : "\"" + words + "\"";
    }
    if (cls == "MAActionSound" || cls == "MAActionPlayVoice") {
        const uint32_t cue = o->u32(cls == "MAActionSound" ? "CueEvent" : "CueID");
        if (cue >= kAudioBase && cue < kAudioBase + 100000u) return "clip " + std::to_string(cue - kAudioBase);
    }
    return trig_display_name(*o, cls);
}

// The items of the Add menu: "+ Add at t" on the strip and "When it ends [+]". The edit runs one frame later (tl_later_).
void App::cut_add_menu(uint32_t cin, float t, bool at_end) {
    struct Kind { const char* label; const char* cls; const char* name; const char* tip; };
    static const Kind kMain[] = {
        {"Play a sound", "MAActionSound", "Play a sound", "A sound effect or one of your own clips."},
        {"Play a voice line", "MAActionPlayVoice", "Play a voice line", "One of the game's voice lines, or your own clip."},
        {"Change the music", "MAActionMusic", "Change the music", "Switch to another music cue."},
        {"Show a message", "MAActionBroadcast", "Show a message", "A line of text on the screen."},
    };
    static const Kind kUnits[] = {
        {"A unit walks a path", "MAPatrol", "walk", "Pick the unit on the map, then click the points it walks through (once)."},
        {"A unit patrols", "MAPatrol", "patrol", "Pick the unit on the map, then click the points it patrols (round and round)."},
        {"A unit attacks the player", "MAAttack", "attack", "Pick the unit on the map: it goes for the player from this moment."},
    };
    static const Kind kOther[] = {
        {"Show or hide units", "MAActionShow", "Show or hide units", "Make units appear or disappear."},
        {"Switch units on or off", "MAActionAI", "Switch units on or off", "Wake a sleeping unit, or put one to sleep."},
        {"Remove units", "MAActionRemoveEntity", "Remove units", "Take units out of the level."},
        {"Change which side units are on", "MAActionNation", "Change side", "Make a unit friendly, enemy or neutral."},
        {"Complete an objective", "MAActionMissionVariableSet", "Complete an objective", "Tick an objective off."},
        {"Win or lose the mission", "MAActionGameCondition", "Win or lose", "End the mission."},
        {"Play another cutscene", "MAActionActivateCinema", "Play a cutscene", "Start another cutscene."},
        {"Move the player", "MAActionTeleport", "Move the player", "Put the player at a marker."},
    };
    auto make = [this, cin, t, at_end](const Kind& k) {
        const std::string cls = k.cls, name = k.name;
        tl_later_ = [this, cin, t, at_end, cls, name]() {
            if (!doc_) return;
            push_undo("add event");
            const uint32_t a = at_end ? cutscene_add_end_action(*doc_, cin, cls.c_str(), name) : cutscene_add_action(*doc_, cin, t, cls.c_str(), name);
            if (a) {
                tl_.ev = a;
                open_action_ = a;
                if (cls == "MAActionSound" || cls == "MAActionPlayVoice" || cls == "MAActionMusic") open_cue_for_ = a;  // its picker opens in the Event panel
                if (!at_end) tl_.playhead = t;
            } else {
                status_ = "Could not add that: the trigger prototypes did not load.";
                status_time_ = 6;
            }
            world_changed();
        };
    };
    for (const Kind& k : kMain) {
        if (ImGui::MenuItem(k.label)) make(k);
        ImGui::SetItemTooltip("%s", k.tip);
    }
    ImGui::Separator();
    for (const Kind& k : kUnits) {
        if (at_end && std::string(k.name) != "attack") continue;  // a walk or patrol needs the time to be useful; at the end only an attack makes sense
        if (!ImGui::MenuItem(k.label)) { ImGui::SetItemTooltip("%s", k.tip); continue; }
        ImGui::SetItemTooltip("%s", k.tip);
        pick_ = PickTarget();
        pick_.kind = PickTarget::CutUnit;
        pick_.uid = cin;
        pick_.t = at_end ? -1.f : t;
        pick_.key = k.cls;
        pick_.once = std::string(k.name) == "walk";
        pick_.back = tab_;
        tool_ = Tool::PickPoint;
        status_ = "Click the unit on the map. Right-click or " + kt(A_CANCEL) + " cancels.";
        status_time_ = 8;
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Other action...")) {
        for (const Kind& k : kOther) {
            if (ImGui::MenuItem(k.label)) make(k);
            ImGui::SetItemTooltip("%s", k.tip);
        }
        ImGui::EndMenu();
    }
}

// The click on the unit that gets the order from the Add menu. Stays armed on a miss; a path order then arms the path tool.
void App::pick_cut_unit(float mx, float my) {
    Scenario& s = *doc_;
    const int h = pick_object(mx, my);
    const WObj* o = h >= 0 ? &s.world.objs[size_t(h)] : nullptr;
    if (!o || is_trigger_shape(o->cls) || !o->find("CorporalPos")) {
        status_ = "Click a unit on the map. Right-click or " + kt(A_CANCEL) + " cancels.";
        status_time_ = 6;
        return;
    }
    const uint32_t unit = o->uid(), cin = pick_.uid;
    const float t = pick_.t;
    const std::string cls = pick_.key;
    const bool once = pick_.once, at_end = t < 0;
    const Tab back = pick_.back;
    push_undo("add event");
    uint32_t ev = 0;
    if (at_end) {
        const std::vector<ShotInfo> shots = cutscene_shots(s.world, cin);
        const uint32_t ex = shots.empty() ? 0 : shots.back().exit;
        ev = ex ? add_order(s, ex, unit, cls.c_str()) : 0;
    } else {
        ev = cutscene_add_order(s, cin, t, unit, cls.c_str(), !once);
    }
    pick_ = PickTarget();
    tool_ = Tool::Select;
    tab_ = back;
    if (!ev) { status_ = "Could not give that unit an order."; status_time_ = 6; return; }
    world_changed();
    tl_.ev = ev;
    if (cls == "MAPatrol") {  // now its route: one click per point
        pick_.kind = PickTarget::OrderPath;
        pick_.uid = ev;
        pick_.back = back;
        tool_ = Tool::PlaceWaypoint;
        status_ = std::string("Click the map once per point the unit ") + (once ? "walks through" : "patrols") + ", in order. Right-click when done.";
        status_time_ = 10;
    } else {
        status_ = "Done: " + (o->description().empty() ? std::string("the unit") : o->description()) + " attacks the player.";
        status_time_ = 6;
    }
}

// Left panel of the Cameras & Cutscenes tab: what runs when the cutscene ends, and the picked event with the same editor a trigger's
// action has (ui_action_body), or the order editor.
void App::cut_event_panel(uint32_t cin) {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const bool ro = s.read_only;
    const NameTable& names = gd_.names();
    const std::vector<CutEvent> evs = cutscene_events(w, names, cin);
    const std::vector<CutEvent> ends = cutscene_end_actions(w, names, cin);
    // When it ends
    ImGui::SeparatorText("When it ends");
    if (ends.empty()) ImGui::TextDisabled("Nothing runs when it ends. Control goes back to the player.");
    for (const CutEvent& e : ends) {
        ImGui::PushID(int(e.obj));
        if (ImGui::Selectable(cut_event_label(e).c_str(), tl_.ev == e.obj)) tl_.ev = e.obj;
        ImGui::PopID();
    }
    ImGui::BeginDisabled(ro);
    if (ImGui::SmallButton("+ Add when it ends##endadd")) ImGui::OpenPopup("##endaddmenu");
    ImGui::SetItemTooltip("Add something that runs as the last shot ends: a hand-back message, a unit given orders.");
    if (ImGui::BeginPopup("##endaddmenu")) {
        const std::vector<ShotInfo> sh = cutscene_shots(w, cin);
        cut_add_menu(cin, cutscene_total(sh), true);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();

    // The picked event
    const CutEvent* pick = nullptr;
    bool is_end = false;
    for (const CutEvent& e : evs) if (e.obj == tl_.ev) pick = &e;
    for (const CutEvent& e : ends) if (e.obj == tl_.ev) { pick = &e; is_end = true; }
    if (!pick) {
        if (evs.empty()) hint_text("Put things on the timeline: set the playhead, then + Add at ... (a sound, a message, a unit walking).");
        return;
    }
    const CutEvent e = *pick;
    WObj* a = object_by_uid(w, e.obj);
    if (!a) return;
    ImGui::SeparatorText("Event");
    {   // a newly picked event scrolls the panel to its editor
        static uint32_t scrolled = 0;
        if (scrolled != e.obj) { scrolled = e.obj; ImGui::SetScrollHereY(0.1f); }
    }
    {
        const std::string head = cut_event_label(e);
        if (is_end) ImGui::TextWrapped("%s: when the cutscene ends (%s s)", head.c_str(), fmt_t(e.t).c_str());
        else ImGui::TextWrapped("%s at %s s (shot %d + %s s)", head.c_str(), fmt_t(e.t).c_str(), e.shot + 1, fmt_t(e.offset).c_str());
    }
    std::function<void()> later;
    ImGui::BeginDisabled(ro);
    if (theme::danger_small_button("Delete##evdel")) {
        const uint32_t eu = e.obj;
        later = [this, cin, eu]() {
            push_undo("delete event");
            cutscene_remove_event(*doc_, gd_.names(), cin, eu);
            tl_.ev = 0;
            world_changed();
        };
    }
    ImGui::SetItemTooltip("Take this event off the timeline.");
    if (!is_end) {
        form_label("At", form_col());
        ImGui::SetNextItemWidth(150 * dpi_);
        float tt = e.t;
        const bool done = ImGui::InputFloat("##evtime", &tt, 0.5f, 0.5f, "%.1f s", ImGuiInputTextFlags_EnterReturnsTrue);
        if (done && std::fabs(tt - e.t) > 0.004f) {
            const uint32_t eu = e.obj;
            const float nt = std::max(0.f, tt);
            later = [this, cin, eu, nt]() {
                push_undo("move event");
                cutscene_move_event(*doc_, gd_.names(), cin, eu, nt);
                world_changed();
            };
        }
        ImGui::SetItemTooltip("Seconds from the start of the cutscene. Type it and press Enter, or drag the marker on the timeline.");
    }
    if (e.order) {
        // The order: which unit, what it does, its route.
        const uint32_t ev = e.obj;
        const WObj* b = object_by_uid(w, order_behaviour(w, ev));
        const std::string bc = b ? names.label(b->cls) : std::string();
        form_label("Unit", form_col());
        ImGui::TextUnformatted(unit_label(w, e.unit).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select##evunit")) { select_uid(e.unit); go_to_object(e.unit); }
        static const char* ocls[] = {"MAPatrol", "MADefend", "MAFollow", "MAAttack"};
        static const char* olab[] = {"Walk or patrol a path", "Defend a point", "Follow the player", "Attack the player"};
        int cur = -1;
        for (int k = 0; k < 4; k++) if (bc == ocls[k]) cur = k;
        form_label("Order", form_col());
        ImGui::SetNextItemWidth(-1);
        if (begin_combo("##evorder", cur >= 0 ? olab[cur] : "(none)")) {
            for (int k = 0; k < 4; k++)
                if (ImGui::Selectable(olab[k], k == cur)) later = [this, ev, beh = e.unit ? order_behaviour(w, ev) : 0u, k]() {
                    push_undo("change order");
                    set_order_kind(*doc_, ev, ocls[k]);
                    if ((tool_ == Tool::PlaceWaypoint || tool_ == Tool::PickPoint) && (pick_.uid == ev || pick_.uid == beh)) { tool_ = Tool::Select; pick_ = PickTarget(); }  // The old order's click is dropped
                    world_changed();
                };
            ImGui::EndCombo();
        }
        if (bc == "MAPatrol") {
            const uint32_t g = order_path(w, ev);
            const int n = g ? ai_path_length(s, g) : 0;
            ImGui::Text("Route: %d point%s%s", n, n == 1 ? "" : "s", n < 2 ? " (needs 2 or more)" : "");
            if (g) {
                if (const WObj* go = object_by_uid(w, g)) {
                    bool loop = go->i32("TraverseType") == 2;
                    if (ImGui::Checkbox("Go round and round (untick: walk it once, then stand)", &loop))
                        later = [this, g, loop]() {
                            push_undo("patrol loop");
                            if (WObj* o = object_by_uid(doc_->world, g)) o->set_i32("TraverseType", loop ? 2 : 1);
                            doc_->dirty = true;
                        };
                }
            }
            auto draw = [&](bool clear) {
                if (clear) later = [this, ev]() { push_undo("redraw path"); order_clear_path(*doc_, ev); world_changed(); };
                pick_ = PickTarget();
                pick_.kind = PickTarget::OrderPath;
                pick_.uid = ev;
                pick_.back = tab_;
                tool_ = Tool::PlaceWaypoint;
            };
            if (theme::small_toggle_button(n ? "Add points##evpath" : "Draw on map##evpath", picking(PickTarget::OrderPath, ev))) draw(false);
            ImGui::SetItemTooltip("Then click the map once per point, in order. Right-click when done.");
            if (n) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Redraw##evpath")) draw(true);
            }
        } else {
            if (ImGui::SmallButton("More settings (Properties)##evprops")) select_uid(ev);
            ImGui::SetItemTooltip("Opens the order itself in Properties: the point it defends, who it follows or attacks.");
        }
    } else if (WObj* tr = object_by_uid(w, e.holder)) {
        ui_action_body(*tr, *a, later);
    }
    ImGui::EndDisabled();
    if (later) tl_later_ = later;  // after the panel is done: the objects it points at may move
}

}  // namespace ed
