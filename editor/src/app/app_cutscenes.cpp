// Cameras & Cutscenes tab: the level's own MACinema objects, their shots (MAFreeCameraController with
// its camera, start/end triggers, eye path and focus), which triggers play them, and Add shot / New
// cutscene / Delete. Same real objects as everywhere else (see ScenarioIO::add_cutscene).
#include <algorithm>
#include <cmath>
#include <map>

#include "../core/cutscene.h"
#include "../core/mission_extras.h"
#include "../core/triggers.h"
#include "ui_tips.h"
#include "app.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "theme.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

// SameLine when a button labelled `label` still fits in the panel; else it starts the next row at x = col (the field
// column), so a narrow panel never cuts a button off at its edge.
static void same_line_or_wrap(const char* label, float col = 0) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;  // the row's right edge
    float need = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2 + st.ItemSpacing.x;
    if (ImGui::GetItemRectMax().x + need <= right) ImGui::SameLine();
    else if (col > 0) ImGui::SetCursorPosX(col);
}

// The Cameras & Cutscenes tab (AoE-style task tab): the cutscenes, then every camera with the shot that flies it.
// The trigger that plays a cutscene as the mission starts (an intro): a 0 s timer with a "Play" action. The caller holds the undo step.
void App::play_cutscene_at_start(uint32_t cu) {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const WObj* co = object_by_uid(w, cu);
    if (!co) return;
    const std::string cn = co->description();
    uint32_t u = add_trigger_object(s, "MATriggerTimer", "Play " + cn);
    // 0 s (a 1 s wait showed the mission for a second before the intro). The older note saw a 0 s
    // timer's cinema not show in a custom mission;'s game check repeats it, and 0.1 s is the fallback if it fails.
    if (WObj* o = object_by_uid(w, u)) { o->set_f32("TrgTimerDurationSeconds", 0); o->set_f32("TrgTimerDuration", 0); }
    if (WObj* a = object_by_uid(w, add_action_object(s, u, "MAActionActivateCinema", "Play " + cn))) a->set_u32("cinemaID", cu);
    world_changed();
    s.dirty = true;
    status_ = "Done: \"" + cn + "\" plays when the mission starts (trigger \"Play " + cn + "\" on the Triggers list).";
    status_time_ = 8;
}

void App::tab_cameras() {
    Scenario& s = *doc_;
    const Wdf& w = s.world;
    static const uint32_t K_CINEMA = key_of("MACinema"), K_CTRL = key_of("MAFreeCameraController"), K_CAM = key_of("MAFreeCamera");
    ImGui::SeparatorText("Cutscenes");
    tab_cutscenes();
    ImGui::Spacing();
    ImGui::SeparatorText("Cameras");
    // The shot that flies each camera: cutscene uid + shot number (0-based).
    WTree t;
    t.build(w);
    std::map<uint32_t, std::pair<uint32_t, int>> used;
    for (size_t i = 0; i < w.objs.size(); i++) {
        if (w.objs[i].cls != K_CINEMA) continue;
        std::vector<int> sub;
        t.subtree(int(i), sub);
        int n = 0;
        for (int k : sub)
            if (w.objs[size_t(k)].cls == K_CTRL) used.emplace(w.objs[size_t(k)].u32("Camera"), std::make_pair(w.objs[i].uid(), n++));
    }
    ImGui::TextWrapped("A shot of a cutscene flies its camera along the shot's path. A camera no shot uses shows nothing in the game.");
    ImGui::BeginDisabled(s.read_only);
    if (ImGui::Button("Place camera")) place_helper("MAFreeCamera", "Camera", "Cameras");
    ImGui::SetItemTooltip("Add a camera, then click the map to put it down. Turn it with Heading in Properties.");
    ImGui::EndDisabled();
    int ncam = 0;
    for (auto& o : w.objs) ncam += o.cls == K_CAM;
    if (!ncam) { ImGui::TextDisabled("No cameras yet. New cutscene (above) makes one for its shot."); return; }
    // A resizable child keeps the height it first had, so the id carries the row count: a level with more cameras
    // gets the taller list instead of the one-row list from a level with one camera.
    const int rows = std::min(ncam, 6);
    ImGui::BeginChild(ImGui::GetID(("##camlist" + std::to_string(rows)).c_str()), ImVec2(0, rows * ImGui::GetFrameHeightWithSpacing() + 8 * dpi_),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    for (auto& o : w.objs) {
        if (o.cls != K_CAM) continue;
        ImGui::PushID(int(o.uid()));
        auto it = used.find(o.uid());
        std::string where = "not in a cutscene";
        if (it != used.end()) {
            const WObj* c = object_by_uid(w, it->second.first);
            where = "shot " + std::to_string(it->second.second + 1) + " of " + (c && !c->description().empty() ? c->description() : "a cutscene");
        }
        std::string nm = o.description().empty() ? "Camera #" + std::to_string(o.uid()) : o.description();
        bool looking = cpv_.on && cpv_.still == o.uid();
        if (theme::small_toggle_button(looking ? "Stop" : "Look through", looking)) {
            if (looking) cutscene_preview_stop();
            else if (it != used.end()) cutscene_preview_start(it->second.first, it->second.second);
            else camera_look_through(o.uid());
        }
        ImGui::SetItemTooltip("%s", it != used.end() ? ("Play the shot this camera flies, in the map view (" + kt(A_CANCEL) + " stops).").c_str()
                                                  : ("See the map from this camera: its position, Heading and zoom (" + kt(A_CANCEL) + " returns).").c_str());
        ImGui::SameLine();
        if (ImGui::Selectable((nm + "  - " + where).c_str(), selection_.count(o.uid()) > 0)) { select_uid(o.uid()); zoom_to_selection(); }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void App::tab_cutscenes() {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const bool ro = s.read_only;
    static const uint32_t K_CINEMA = key_of("MACinema"), K_CTRL = key_of("MAFreeCameraController"), K_PROP = key_of("MACameraProperty");
    const auto& names = gd_.names();
    WTree t;
    t.build(w);
    std::vector<int> cinemas;
    for (size_t i = 0; i < w.objs.size(); i++) if (w.objs[i].cls == K_CINEMA) cinemas.push_back(int(i));

    // A level with one cutscene opens it at once (the shot panel and the timeline strip need a picked one).
    {
        const int cur = cine_uid_ ? t.find(cine_uid_) : -1;
        if (cinemas.size() == 1 && (cur < 0 || w.objs[size_t(cur)].cls != K_CINEMA)) cine_uid_ = w.objs[size_t(cinemas[0])].uid();
    }

    // The map selection leads: a cutscene, or something inside one, selected on the map opens here.
    static uint32_t last_sel = 0;
    if (selection_.size() == 1) {
        uint32_t su = *selection_.begin();
        if (su != last_sel) {
            last_sel = su;
            for (int p = t.find(su); p >= 0; p = t.parent_of[size_t(p)])
                if (w.objs[size_t(p)].cls == K_CINEMA) { cine_uid_ = w.objs[size_t(p)].uid(); break; }
        }
    } else last_sel = 0;

    auto name_of = [&](uint32_t u) -> std::string {
        const WObj* o = u ? object_by_uid(w, u) : nullptr;
        if (!o) return u ? "missing #" + std::to_string(u) : "(none)";
        return o->description().empty() ? names.label(o->cls) + " #" + std::to_string(u) : o->description();
    };
    auto shots_of = [&](int ci) {
        std::vector<int> sub, out;
        t.subtree(ci, sub);
        for (int k : sub) if (w.objs[size_t(k)].cls == K_CTRL) out.push_back(k);
        return out;
    };

    ImGui::TextWrapped("A cutscene is what the game plays for the intro or a base reveal: one or more shots, each with a camera, "
                       "an eye path it flies and a focus point it looks at. A trigger action \"Play a cutscene\" starts it.");
    {
        struct Dis { explicit Dis(bool d) { ImGui::BeginDisabled(d); } ~Dis() { ImGui::EndDisabled(); } } d(ro);
        if (ImGui::Button("New cutscene")) { if (uint32_t u = new_cutscene()) cine_uid_ = u; }
        ImGui::SetItemTooltip("Add a cutscene at the view centre: one shot whose camera flies a path over a focus marker.");
        if (!doc_->multiplayer) {
            ImGui::SameLine();
            if (ImGui::Button("New intro")) {
                // An intro is named "Intro" and already plays when the mission starts (one undo step).
                push_undo("new intro");
                if (uint32_t u = new_cutscene(true, false)) {
                    cine_uid_ = u;
                    if (WObj* co = object_by_uid(doc_->world, u)) {
                        int n = 1;
                        auto taken = [&](const std::string& nm) { for (auto& x : doc_->world.objs) if (x.cls == K_CINEMA && x.description() == nm) return true; return false; };
                        std::string nm = "Intro";
                        while (taken(nm)) nm = "Intro " + std::to_string(++n);
                        // The parts made with the cutscene carry its first name: they follow it.
                        const std::string old = co->description();
                        WTree t2;
                        t2.build(doc_->world);
                        std::vector<int> sub2;
                        if (int c2 = t2.find(u); c2 >= 0) t2.subtree(c2, sub2);
                        for (int k : sub2) {
                            WObj& x = doc_->world.objs[size_t(k)];
                            const std::string d = x.description();
                            if (x.uid() != u && d.size() > old.size() + 3 && d.compare(0, old.size() + 3, old + " - ") == 0) x.set_str("Description", nm + d.substr(old.size()));
                        }
                        co->set_str("Description", nm);
                        play_cutscene_at_start(u);
                    }
                }
            }
            ImGui::SetItemTooltip("Add a cutscene over the player start: the camera flies a path behind the player and looks at the start, "
                                  "and it plays when the mission starts.");
        }
    }
    ImGui::SameLine();
    {
        int ci = cine_uid_ ? t.find(cine_uid_) : -1;
        if (ci >= 0 && w.objs[size_t(ci)].cls != K_CINEMA) ci = -1;
        // Show on map and Preview only look, so they work on read-only levels too; Delete needs an editable level.
        struct Dis { explicit Dis(bool d) { ImGui::BeginDisabled(d); } ~Dis() { ImGui::EndDisabled(); } } d(ci < 0);
        ImGui::BeginDisabled(ro);
        if (theme::danger_button("Delete")) {
            uint32_t victim = cine_uid_;
            int users = int(used_by(w, victim, [&](uint32_t c) { return names.label(c).rfind("MATrigger", 0) == 0; }).size());
            std::string q = "Delete the cutscene \"" + name_of(victim) + "\" with all its shots?";
            if (users) q += "\n\n" + std::to_string(users) + " trigger action(s) play it and will have nothing to play.";
            q += "\n\nYou can undo this with " + kt(A_UNDO) + ".";
            confirm(q, [this, victim]() {
                if (!doc_) return;
                push_undo("delete cutscene");
                wedit::remove_subtree(doc_->world, victim);
                selection_.erase(victim);
                cine_uid_ = 0;
                world_changed();
                doc_->dirty = true;
            });
        }
        ImGui::SetItemTooltip("%s", ("Delete the selected cutscene (asks first; " + kt(A_UNDO) + " undoes it).").c_str());
        ImGui::EndDisabled();
        same_line_or_wrap("Show on map");
        if (ImGui::Button("Show on map")) {
            show_helpers_ = true;
            show_helper_group_[HG_CAMERAS] = true;
            objects_dirty_ = true;
            select_uid(cine_uid_);
            // Retail cutscene objects sit at 0,0 (the map corner): frame the cameras, path points and focus markers
            // of its shots instead.
            std::vector<int> sub;
            t.subtree(ci, sub);
            {  // plus what the shots point at (retail cameras, eye paths and focus markers live outside the cutscene)
                std::vector<int> more;
                for (int k : sub)
                    for (const char* key : {"Camera", "eyeUID", "focusUID"})
                        if (uint32_t u = w.objs[size_t(k)].u32(key); u && t.find(u) >= 0) t.subtree(t.find(u), more);
                sub.insert(sub.end(), more.begin(), more.end());
            }
            float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f, ysum = 0;
            int n = 0;
            for (int k : sub) {
                Vec3 p;
                if (k == ci || !get_pos(w.objs[size_t(k)], p) || (p.x == 0 && p.z == 0)) continue;
                x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); z0 = std::min(z0, p.z); z1 = std::max(z1, p.z);
                ysum += p.y;
                n++;
            }
            if (n) {
                cam_.tx = 0.5f * (x0 + x1);
                cam_.tz = 0.5f * (z0 + z1);
                cam_.ty = ysum / float(n);
                cam_.dist = std::clamp(1.3f * std::max(x1 - x0, z1 - z0), 80.f, 1500.f);
            } else {
                zoom_to_selection();
            }
        }
        ImGui::SetItemTooltip("Select the cutscene, switch the camera helpers on and move the view to it.");
        same_line_or_wrap("Preview");
        if (cpv_.on && cpv_.cin == cine_uid_) { if (theme::toggle_button("Stop", true)) cutscene_preview_stop(); }
        else if (ImGui::Button("Preview##top")) cutscene_preview_start(cine_uid_);
        ImGui::SetItemTooltip("%s", ("Play the selected cutscene in the map view (" + kt(A_CANCEL) + " stops).").c_str());
    }

    ImGui::BeginChild("##clist", ImVec2(0, 96 * dpi_), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    if (cinemas.empty()) ImGui::TextDisabled("This level has no cutscenes.");
    for (int ci : cinemas) {
        const WObj& o = w.objs[size_t(ci)];
        const std::vector<ShotInfo> li = cutscene_shots(w, t, o.uid());
        const int n = int(li.size());
        char tl[24];
        snprintf(tl, sizeof tl, "%.1f s", double(cutscene_total(li)));
        std::string l = name_of(o.uid()) + "  - " + std::to_string(n) + " shot" + (n == 1 ? "" : "s") + "  " + tl;
        ImGui::PushID(ci);
        if (ImGui::Selectable(l.c_str(), o.uid() == cine_uid_)) { cine_uid_ = o.uid(); select_uid(o.uid()); }
        ImGui::PopID();
    }
    ImGui::EndChild();

    int ci = cine_uid_ ? t.find(cine_uid_) : -1;
    if (ci < 0 || w.objs[size_t(ci)].cls != K_CINEMA) {
        if (!cinemas.empty()) ImGui::TextDisabled("Pick a cutscene above.");
        return;
    }
    WObj& cin = w.objs[size_t(ci)];
    ImGui::SeparatorText("Selected cutscene");
    ImGui::BeginDisabled(ro);
    {
        std::string nm = cin.description();
        form_label("Name", form_col());
        if (ImGui::InputText("##cutname", &nm)) {
            push_undo_once("rename cutscene");
            s.dirty = true;
            objects_dirty_ = true;
            // Its parts ("<name> - shot 2", "<name> - camera (shot 2)"...) and the triggers that play it ("Play <name>") follow the new name.
            const std::string old = cin.description();
            if (!old.empty() && old != nm) {
                std::vector<int> sub_idx;
                t.subtree(ci, sub_idx);
                auto follow = [&](WObj& x) {
                    const std::string d = x.description();
                    if (d.size() > old.size() + 3 && d.compare(0, old.size() + 3, old + " - ") == 0) x.set_str("Description", nm + d.substr(old.size()));
                    else if (d == "Play " + old) x.set_str("Description", "Play " + nm);
                };
                for (int k : sub_idx) if (k != ci) follow(w.objs[size_t(k)]);
                for (auto& x : w.objs) if (x.description() == "Play " + old) x.set_str("Description", "Play " + nm);
            }
            cin.set_str("Description", nm);
        }
    }
    ImGui::EndDisabled();

    // Played by: the actions that start it.
    auto uses = used_by(w, cin.uid(), [&](uint32_t c) { return names.label(c).rfind("MATrigger", 0) == 0; });
    ImGui::SeparatorText("Played by");
    if (uses.empty()) {
        hint_text("No trigger plays it yet. Make one here, or add the action \"Play a cutscene\" to a trigger.");
        // One click: the trigger and its "Play a cutscene" action, already pointing at this cutscene.
        ImGui::BeginDisabled(ro);
        const uint32_t cu = cin.uid();
        const std::string cn = cin.description();
        if (ImGui::Button("Play at mission start")) {
            push_undo("play cutscene at start");
            play_cutscene_at_start(cu);
        }
        ImGui::SetItemTooltip("Adds a trigger that plays this cutscene as the mission starts (an intro).");
        same_line_or_wrap("Play when the player enters a box");
        if (ImGui::Button("Play when the player enters a box")) {
            push_undo("play cutscene from a box");
            TriggerShape sh;
            sh.box = true;
            sh.x = cam_.tx;
            sh.z = cam_.tz;
            sh.name = "Play " + cn;
            if (uint32_t u = add_trigger_shape(s, sh)) {
                if (WObj* a = object_by_uid(w, add_action_object(s, u, "MAActionActivateCinema", "Play " + cn))) a->set_u32("cinemaID", cu);
                world_changed();
                s.dirty = true;
                pick_ = PickTarget();
                pick_.kind = PickTarget::MoveShape;
                pick_.uid = u;
                pick_.back = tab_;
                tool_ = Tool::PickPoint;
                status_ = "Click the map to put the box down, away from the player start: \"" + cn + "\" plays when the player walks into it.";
                status_time_ = 10;
            }
        }
        ImGui::SetItemTooltip("Adds a trigger box that plays this cutscene when the player walks into it; click the map to place the box. "
                              "Put it away from the player start: a box around the start fires as the mission starts, while the "
                              "level's intro may still be playing, and the cutscene is lost. For an intro use \"Play at mission start\".");
        ImGui::EndDisabled();
    }
    uint32_t open_trigger = 0;
    float play_at = -1;
    int n = 0;
    for (auto& u : uses) {
        if (u.kind != UsedBy::Action || !u.trigger) continue;
        ImGui::PushID(n++);
        const WObj* ptr = object_by_uid(w, u.trigger);
        std::string at;
        if (ptr && ptr->cls == key_of("MATriggerTimer")) {
            char b[40];
            snprintf(b, sizeof b, " at %.1f s", double(ptr->f32("TrgTimerDurationSeconds")));
            at = b;
            if (play_at < 0) play_at = ptr->f32("TrgTimerDurationSeconds");
        }
        if (ImGui::Selectable(("Played by '" + name_of(u.trigger) + "'" + at).c_str(), false)) open_trigger = u.trigger;
        ImGui::SetItemTooltip("Open the trigger on the Triggers list.");
        ImGui::PopID();
    }
    {
        const std::vector<ShotInfo> pi = cutscene_shots(w, t, cin.uid());
        char b[112];
        if (play_at >= 0) snprintf(b, sizeof b, "You get control back at %.1f s (the cutscene is %.1f s long).", double(play_at + cutscene_total(pi)), double(cutscene_total(pi)));
        else snprintf(b, sizeof b, "You get control back %.1f s after it starts.", double(cutscene_total(pi)));
        if (!pi.empty()) ImGui::TextDisabled("%s", b);
    }

    // During this cutscene: keep the fight from running on camera, and say whether the player may skip it.
    if (!s.multiplayer && !ro) {
        ImGui::SeparatorText("During this cutscene");
        static const int kOrder[] = {1, 2, 0};  // the list order: enemies (the default), everyone but the player, nothing
        static const char* kLabels[] = {"Enemies hold fire", "All but the player hold fire", "Nobody: the fight goes on"};  // short: they must fit a narrow panel
        const int cur = cutscene_hold(cin);
        int shown = 0;
        for (int i = 0; i < 3; i++) if (kOrder[i] == cur) shown = i;
        form_label("Hold the fight", form_col());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (begin_combo("##hold", kLabels[shown])) {
            for (int i = 0; i < 3; i++)
                if (ImGui::Selectable(kLabels[i], i == shown) && i != shown) {
                    push_undo("cutscene hold");
                    set_cutscene_hold(cin, kOrder[i]);
                    s.dirty = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Units that would shoot on camera get their AI switched off as the first shot starts, and on again when the "
                              "last shot ends (or at the cutscene's length if the player skips it). Units that start asleep are switched on "
                              "only if their own trigger has woken them.");
        bool skip = cutscene_skippable(cin);
        if (ImGui::Checkbox("The player can skip it (Space)", &skip)) {
            push_undo("cutscene skip");
            set_cutscene_skippable(cin, skip);
            s.dirty = true;
        }
        ImGui::SetItemTooltip("On: the player can skip it with Space in the game. Off: Space does nothing while this cutscene plays.");
        hint_text("When it ends or is skipped, held units carry on; the player's shots and the \"Ends with\" actions of the last shot run either way.");
    }

    // Shots: the strip under the map shows all of them; the panel holds the picked one (one typed length per shot, no slider).
    const std::vector<ShotInfo> infos = cutscene_shots(w, t, cin.uid());
    const int nshots = int(infos.size());
    tl_.shot = std::clamp(tl_.shot, 0, std::max(0, nshots - 1));
    uint32_t del_shot = 0;
    int move_shot = -1, move_dir = 0;  // shot index, -1 earlier / +1 later
    auto link = [&](const char* label, uint32_t u, bool trigger) {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);  // the form's label column (form_col), like the shot's settings below
        ImGui::SameLine(form_col());
        if (!u) ImGui::TextDisabled("(none)");
        else if (ImGui::SmallButton(name_of(u).c_str())) {
            if (trigger) open_trigger = u;
            else select_uid(u);
        }
        if (u) ImGui::SetItemTooltip("%s", trigger ? "Open this trigger on the Triggers list." : "Select it on the map.");
        ImGui::PopID();
    };
    {
        char hd[64];
        snprintf(hd, sizeof hd, "Shots (%d), %.1f s in all", nshots, double(cutscene_total(infos)));
        ImGui::SeparatorText(hd);
    }
    if (nshots == 0) ImGui::TextDisabled("No shots yet.");
    for (int i = 0; i < nshots; i++) {
        char lb[56];
        snprintf(lb, sizeof lb, "Shot %d  %.1f s##shotpick%d", i + 1, double(infos[size_t(i)].len), i);
        if (i) same_line_or_wrap(lb);
        if (theme::small_toggle_button(lb, i == tl_.shot)) {
            tl_.shot = i;
            tl_.playhead = infos[size_t(i)].start;
            if (infos[size_t(i)].ctrl) select_uid(infos[size_t(i)].ctrl);  // Properties follows the shot picked
        }
        ImGui::SetItemTooltip("Show this shot's settings here (the timeline under the map shows them all).");
    }
    // A camera move that ends before its shot does leaves the camera holding still: say so for every such shot.
    for (int i = 0; i < nshots; i++) {
        const ShotInfo& sh = infos[size_t(i)];
        const float hold = shot_hold(sh);
        if (hold < 0.5f) continue;
        ImGui::PushID(1000 + i);
        char wl[160];
        snprintf(wl, sizeof wl, "Shot %d holds still for %.1f s (the camera moves for %.1f s of %.1f s)", i + 1, double(hold), double(sh.move_len), double(sh.len));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
        if (ImGui::Selectable(wl, false)) tl_.shot = i;
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("The player waits through the hold with the camera standing still. Either shorten the shot or let the move take the whole shot.");
        ImGui::BeginDisabled(ro);
        ImGui::Indent(12 * dpi_);
        if (ImGui::SmallButton("Move over the whole shot")) {
            push_undo("camera move spans shot");
            set_shot_move_len(s, sh.ctrl, sh.len);
        }
        ImGui::Unindent(12 * dpi_);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (nshots > 0) {
        const ShotInfo& si = infos[size_t(tl_.shot)];
        const int shot_no = tl_.shot + 1;
        const int k = t.find(si.ctrl);
        ImGui::PushID(int(si.ctrl));
        ImGui::SeparatorText(("Shot " + std::to_string(shot_no)).c_str());
        ImGui::BeginDisabled(ro);
        if (theme::danger_small_button("Delete shot")) del_shot = si.ctrl;
        ImGui::SetItemTooltip("Delete this shot with the camera, path, focus marker, triggers and events only it uses.");
        if (nshots > 1) {
            ImGui::SameLine();
            ImGui::BeginDisabled(shot_no == 1);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move_shot = tl_.shot; move_dir = -1; }
            ImGui::SetItemTooltip("Play this shot earlier.");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(shot_no == nshots);
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) { move_shot = tl_.shot; move_dir = 1; }
            ImGui::SetItemTooltip("Play this shot later.");
            ImGui::EndDisabled();
        }
        // Length: one typed number, half-second steps with - and +.
        {
            float len = si.len;
            form_label("Length", form_col());
            ImGui::SetNextItemWidth(150 * dpi_);
            if (ImGui::InputFloat("##shotlen", &len, 0.5f, 0.5f, "%.1f s")) {
                push_undo_once("shot length");
                set_shot_length(s, si.ctrl, len);
            }
            ImGui::SetItemTooltip("How long this shot plays, 0.5 to 120 seconds. Type it, use - and +, or drag the shot's right edge on the timeline under the map.");
        }
        // Camera: still or moving.
        {
            form_label("Camera", form_col());
            const bool moving = !si.still;
            if (ImGui::RadioButton("Still##cam", !moving) && moving && !si.eye.empty()) {
                push_undo("camera still");
                set_shot_view(s, si.ctrl, 0, si.eye.front(), si.look, false, io_.get());
                world_changed();
            }
            ImGui::SetItemTooltip("The camera stands at the first point and does not move.");
            ImGui::SameLine();
            if (ImGui::RadioButton("Moving##cam", moving) && !moving && !si.eye.empty()) {
                push_undo("camera moving");
                const Vec3 e = si.eye.front();
                float dx = std::sin(cam_.yaw), dz = std::cos(cam_.yaw);
                if (si.has_look) {
                    const float ax = si.look.x - e.x, az = si.look.z - e.z, L = std::sqrt(ax * ax + az * az);
                    if (L > 0.5f) { dx = ax / L; dz = az / L; }
                }
                set_shot_view(s, si.ctrl, 2, {e.x + dx * 20.f, e.y, e.z + dz * 20.f}, si.look, false, io_.get());  // 20 m ahead; draw the real path next
                world_changed();
            }
            ImGui::SetItemTooltip("The camera flies along its path over the whole shot. A still shot gets an end point 20 m ahead; draw the path on the map to change it.");
            const WObj* eo = si.eye_path ? object_by_uid(w, si.eye_path) : nullptr;
            if (!si.eye_path) ImGui::TextDisabled("It has no camera path any more: it shows a still view at the camera's own spot.");
            if (!eo || eo->cls == key_of("MAPath")) {
                ImGui::SetCursorPosX(form_col());
                if (theme::small_toggle_button("Draw a path on the map", eo && picking(PickTarget::CamPath, si.eye_path) && pick_.redraw)) {
                    uint32_t ep = eo ? si.eye_path : 0;
                    if (!ep) {  // a deleted path: rebuild one from the Mission_05 prototype first
                        push_undo("shot path");
                        ep = io_->ensure_shot_eye_path(s, si.ctrl);
                        world_changed();
                    }
                    if (ep) {
                        pick_ = PickTarget();
                        pick_.kind = PickTarget::CamPath;
                        pick_.uid = ep;
                        pick_.restart = true;
                        pick_.redraw = true;
                        tool_ = Tool::PlaceWaypoint;
                        selection_ = {ep};
                        status_ = "Click the map once per point the camera flies through, in order; right-click when done.";
                        status_time_ = 10;
                    }
                }
                ImGui::SetItemTooltip("Replace the path: click the map once per point, in order (the camera keeps its height above the ground). Right-click when done.");
                if (eo) {
                    same_line_or_wrap("Add points", form_col());
                    if (theme::small_toggle_button("Add points", picking(PickTarget::CamPath, si.eye_path) && !pick_.redraw)) {
                        pick_ = PickTarget();
                        pick_.kind = PickTarget::CamPath;
                        pick_.uid = si.eye_path;
                        tool_ = Tool::PlaceWaypoint;
                        selection_ = {si.eye_path};
                    }
                    ImGui::SetItemTooltip("Click the map to add points after the last one. Right-click when done.");
                }
            }
        }
        // Frame the shot: the map view becomes the shot's camera (2.3), or the map view as it is now is written into the shot.
        {
            const bool moving = !si.still;
            auto frame_row = [&](const char* frame_label, const char* use_label, int which, const char* frame_tip, const char* use_tip) {
                ImGui::PushID(which);
                const bool on = frame_.on && frame_.ctrl == si.ctrl && frame_.which == which;
                ImGui::SetCursorPosX(form_col());
                if (theme::small_toggle_button(frame_label, on)) {
                    if (on) frame_leave();
                    else frame_begin(si.ctrl, which);
                }
                ImGui::SetItemTooltip("%s", frame_tip);
                same_line_or_wrap(use_label);
                if (ImGui::SmallButton(use_label)) {
                    if (on) frame_use();
                    else shot_use_view(si.ctrl, which);
                }
                ImGui::SetItemTooltip("%s", use_tip);
                ImGui::PopID();
            };
            const std::string key = kt(A_FRAME_USE);
            if (!moving)
                frame_row("Frame it", "Use this view", 0,
                          "Put the map view at this shot's camera, with the game's picture band. Move the view, then Enter writes it back (Esc leaves).",
                          "The shot's camera stands where the map view is now and looks at the middle of the map view. The camera marker and the look point move with it.");
            else {
                frame_row("Frame start", "Start = this view", 1,
                          "Put the map view at the camera's first point, with the game's picture band. Move the view, then Enter writes it back (Esc leaves).",
                          "The camera's first point and what it looks at become the map view as it is now.");
                frame_row("Frame end", "End = this view", 2,
                          "Put the map view at the camera's last point. Move the view, then Enter writes it back (Esc leaves). The camera keeps looking at its marker.",
                          "The camera's last point becomes the map view's eye. It keeps looking at the marker (Aim at view centre moves that).");
            }
        }
        // Looks at.
        {
            form_label("Looks at", form_col());
            if (si.has_look) ImGui::Text("%.0f, %.0f  (%.1f m up)", double(si.look.x), double(si.look.z), double(si.look.y - doc_->terrain.height_at(si.look.x, si.look.z)));
            else ImGui::TextDisabled("(nothing: it looks along its own heading)");
            if (si.focus) {
                ImGui::SetCursorPosX(form_col());
                ImGui::BeginDisabled(cpv_.on);
                if (ImGui::SmallButton("Aim at view centre")) {
                    push_undo("aim shot");
                    set_shot_look(s, si.ctrl, {cam_.tx, cam_.ty, cam_.tz});
                    world_changed();
                }
                ImGui::SetItemTooltip("The camera looks at the ground point in the middle of the map view.");
                ImGui::EndDisabled();
                same_line_or_wrap("Move on map");
                if (theme::small_toggle_button("Move on map", picking(PickTarget::MoveShape, si.focus))) {
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::MoveShape;
                    pick_.uid = si.focus;
                    pick_.back = tab_;
                    tool_ = Tool::PickPoint;
                    selection_ = {si.focus};
                }
                ImGui::SetItemTooltip("Click the map where the camera should look.");
            }
        }
        // Zoom.
        {
            float fov = si.fov;
            form_label("Zoom", form_col());
            ImGui::SetNextItemWidth(110 * dpi_);
            if (ImGui::InputFloat("##shotzoom", &fov, 0, 0, "%.0f deg")) {
                push_undo_once("shot zoom");
                set_shot_fov(s, si.ctrl, fov);
            }
            ImGui::SetItemTooltip("The camera's field of view in degrees: smaller zooms in. The game's default is 48.");
        }
        ImGui::EndDisabled();
        if (ImGui::SmallButton("Preview shot")) cutscene_preview_start(cin.uid(), tl_.shot);
        ImGui::SetItemTooltip("%s", ("Play only this shot in the map view (" + kt(A_CANCEL) + " stops).").c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select shot")) select_uid(si.ctrl);
        ImGui::SetItemTooltip("Select the shot's object (its All settings are in Properties).");
        // More: the hold, the links, the steps.
        if (ImGui::TreeNode("More##shotmore")) {
            ImGui::BeginDisabled(ro);
            const WObj* po = si.eye_path ? object_by_uid(w, si.eye_path) : nullptr;
            if (po && po->cls == key_of("MAPath")) {
                float ml = si.move_len;
                form_label("Camera moves for", form_col());
                ImGui::SetNextItemWidth(150 * dpi_);
                if (ImGui::InputFloat("##movelen", &ml, 0.5f, 0.5f, "%.1f s")) {
                    push_undo_once("camera move length");
                    set_shot_move_len(s, si.ctrl, ml);
                }
                ImGui::SetItemTooltip("How long the camera takes along its path. Shorter than the shot: it moves, then holds still for the rest.");
                if (shot_hold(si) > 0) {
                    ImGui::SetCursorPosX(form_col());
                    if (ImGui::SmallButton("Move over the whole shot##more")) {
                        push_undo("camera move spans shot");
                        set_shot_move_len(s, si.ctrl, si.len);
                    }
                }
            }
            ImGui::EndDisabled();
            const WObj& c = w.objs[size_t(k)];
            link("Camera", c.u32("Camera"), false);
            link("Eye path", si.eye_path, false);
            link("Looks at", si.focus, false);
            link("Starts with", c.u32("triggerInit"), true);
            link("Ends with", c.u32("triggerExit"), true);
            ImGui::BeginDisabled(ro);
            {
                WObj& cm = w.objs[size_t(k)];
                ui_field(cm, "bSkipTo");
                // Steps: what the camera does, when. The eye path and the focus marker are the objects above.
                std::vector<int> sub;
                t.subtree(k, sub);
                int step_no = 0;
                for (int m : sub) {
                    WObj& st = w.objs[size_t(m)];
                    if (st.cls != K_PROP) continue;
                    step_no++;
                    int ty = st.i32("type");
                    const char* tn = ty == 1 ? "Screen size" : ty == 2 ? "Eye point" : ty == 3 ? "Focus point" : ty == 4 ? "Colour filter" : "Step";
                    ImGui::PushID(m);
                    if (ImGui::TreeNode("##step", "Step %d: %s, from %.1f s", step_no, tn, double(st.f32("start")))) {
                        ui_field(st, "start");
                        if (ty == 2) { ui_field(st, "eyeUID"); ui_field(st, "Offset"); }
                        else if (ty == 3) { ui_field(st, "focusUID"); ui_field(st, "Offset"); }
                        else if (ty == 4) {
                            ui_field(st, "startRGB"); ui_field(st, "startAlpha"); ui_field(st, "endRGB"); ui_field(st, "endAlpha");
                        } else if (ty == 1) {
                            ui_field(st, "Endy"); ui_field(st, "EndWidth"); ui_field(st, "EndHeight");
                        }
                        ui_field(st, "flags");
                        if (ImGui::SmallButton("All settings")) select_uid(st.uid());
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                    // The eye or focus path's own flight settings.
                    for (const char* pk : {"eyeUID", "focusUID"}) {
                        WObj* po2 = st.find(pk) ? object_by_uid(w, st.u32(pk)) : nullptr;
                        if (!po2 || po2->cls != key_of("MAPath")) continue;
                        ImGui::PushID(int(po2->uid()));
                        if (ImGui::TreeNode("##path", "  Path '%s': %.1f s", po2->description().c_str(), double(po2->f32("duration")))) {
                            ui_field(*po2, "useSpline"); ui_field(*po2, "smoothIn"); ui_field(*po2, "smoothOut");
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    cut_event_panel(cin.uid());
    ImGui::BeginDisabled(nshots == 0);
    if (cpv_.on && cpv_.cin == cin.uid()) {
        if (theme::toggle_button("Stop preview", true)) cutscene_preview_stop();
    } else if (ImGui::Button("Preview")) cutscene_preview_at(cin.uid(), tl_.playhead >= cutscene_total(infos) - 0.05f ? 0.f : tl_.playhead, false);
    ImGui::SetItemTooltip("%s", ("Play the cutscene here in the map view, from the playhead on the timeline: the camera flies each shot's path looking at its focus point. " + kt(A_CANCEL) + " stops.").c_str());
    why_disabled(nshots == 0, "This cutscene has no shots yet, so there is nothing to play.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ro || nshots == 0);
    if (ImGui::Button("Add shot")) add_cutscene_shot(cin.uid());
    ImGui::SetItemTooltip("Add a still 5-second shot of the view you have now, after the last one.");
    why_disabled(ro || nshots == 0, ro ? "This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab first." : "Add shot builds on the cutscene's last shot, and this cutscene has none yet.");
    ImGui::EndDisabled();

    if (move_shot >= 0) {
        reorder_shot(cin.uid(), move_shot, move_dir);
        tl_.shot = std::clamp(move_shot + move_dir, 0, std::max(0, nshots - 1));
    }
    if (del_shot) {
        std::string q = "Delete this shot and the camera, path, focus marker and triggers only it uses? Its events (sounds, messages, unit orders) are deleted too.\n\nYou can undo this with " + kt(A_UNDO) + ".";
        confirm(q, [this, del_shot]() {
            if (!doc_ || !io_) return;
            push_undo("delete shot");
            io_->remove_cutscene_shot(*doc_, del_shot);
            selection_.clear();
            tl_.shot = std::max(0, tl_.shot - 1);
            world_changed();
            doc_->dirty = true;
        });
    }
    if (open_trigger) {
        trig_uid_ = open_trigger;
        trig_mode_ = 0;
        tab_ = Tab::Triggers;
        select_uid(open_trigger);
    }
}

// The game plays a cinema's controllers in order: swap shot `idx` (0-based) with its neighbour at the level where they are siblings (the
// controllers themselves, or the groups holding them). The cutscene's end actions belong to the last shot, so they follow it.
void App::reorder_shot(uint32_t cinema_uid, int idx, int dir, bool own_undo) {
    if (!doc_ || !editable()) return;
    Scenario& s = *doc_;
    Wdf& w = s.world;
    static const uint32_t K_CTRL = key_of("MAFreeCameraController");
    WTree t;
    t.build(w);
    const int ci = t.find(cinema_uid);
    if (ci < 0) return;
    std::vector<int> sub, shots;
    t.subtree(ci, sub);
    for (int k : sub) if (w.objs[size_t(k)].cls == K_CTRL) shots.push_back(k);
    if (idx < 0 || idx + dir < 0 || idx >= int(shots.size()) || idx + dir >= int(shots.size()) || !dir) return;
    int a = shots[size_t(idx)], b = shots[size_t(idx + dir)];
    while (a >= 0 && b >= 0 && t.parent_of[size_t(a)] != t.parent_of[size_t(b)]) { a = t.parent_of[size_t(a)]; b = t.parent_of[size_t(b)]; }
    if (a < 0 || b < 0 || a == b) return;
    const std::vector<int>& kids = t.kids[size_t(t.parent_of[size_t(a)])];
    int pa = int(std::find(kids.begin(), kids.end(), a) - kids.begin()), pb = int(std::find(kids.begin(), kids.end(), b) - kids.begin());
    uint32_t au = w.objs[size_t(a)].uid(), bu = w.objs[size_t(b)].uid();
    const uint32_t old_last_exit = w.objs[size_t(shots.back())].u32("triggerExit");
    if (own_undo) push_undo("reorder shots");
    // a goes to b's place and b to a's: step a past b, then b back past the ones in between.
    int steps = std::abs(pb - pa);
    for (int i = 0; i < steps; i++) wedit::move_sibling(w, au, dir);
    for (int i = 0; i + 1 < steps; i++) wedit::move_sibling(w, bu, -dir);
    std::vector<ShotInfo> now = cutscene_shots(w, cinema_uid);
    if (!now.empty()) cutscene_move_end_actions(s, gd_.names(), old_last_exit, now.back().exit);
    world_changed();
    s.dirty = true;
}

// ---- Framing a shot ----------------------------------------------------------------------------------------
// "Frame it / Frame start / Frame end" puts the map view at the shot's own camera: the eye is the path point, it looks at the focus
// marker, and the zoom is the shot's Field of view. The 2.35:1 band the game shows is drawn, the rest dimmed. Enter writes the view
// into the shot; Esc leaves. "Use this view" without framing writes the editor view as it is.
ma2::Vec3 App::cam_eye_now() const {
    const float cp = std::cos(cam_.pitch), sp = std::sin(cam_.pitch);
    return {cam_.tx - std::sin(cam_.yaw) * cp * cam_.dist, cam_.ty + sp * cam_.dist, cam_.tz - std::cos(cam_.yaw) * cp * cam_.dist};
}

// The picture the game shows: 2.35:1, as wide as the map view allows, in the middle of it.
void App::game_band(float& x, float& y, float& w, float& h) const {
    w = std::min(vp_w, vp_h * kGameBandAspect);
    h = w / kGameBandAspect;
    x = vp_x + (vp_w - w) * 0.5f;
    y = vp_y + (vp_h - h) * 0.5f;
}

static bool shot_by_ctrl(const Wdf& w, uint32_t ctrl, ShotInfo& out, int* idx = nullptr) {
    const std::vector<ShotInfo> v = cutscene_shots(w, shot_cinema(w, ctrl));
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].ctrl == ctrl) { out = v[i]; if (idx) *idx = int(i); return true; }
    return false;
}

void App::frame_begin(uint32_t ctrl, int which) {
    if (!doc_) return;
    ShotInfo si;
    int idx = 0;
    if (!shot_by_ctrl(doc_->world, ctrl, si, &idx)) return;
    if (cpv_.on) cutscene_preview_stop();
    if (!frame_.on) frame_.saved = cam_;
    frame_.on = true;
    frame_.cin = shot_cinema(doc_->world, ctrl);
    frame_.ctrl = ctrl;
    frame_.which = which;
    tl_.shot = idx;
    const ma2::Vec3 own = cam_eye_now();
    const ma2::Vec3 e = si.eye.empty() ? own : which == 2 ? si.eye.back() : si.eye.front();
    ma2::Vec3 l = si.look;
    float d = 0;
    if (si.has_look) d = std::sqrt((l.x - e.x) * (l.x - e.x) + (l.y - e.y) * (l.y - e.y) + (l.z - e.z) * (l.z - e.z));
    if (!si.has_look || d < 1.f) {  // nothing to look at: straight ahead the way the editor view was turned
        l = {e.x + std::sin(cam_.yaw) * 20.f, e.y, e.z + std::cos(cam_.yaw) * 20.f};
        d = 20.f;
    }
    cam_.tx = l.x; cam_.ty = l.y; cam_.tz = l.z;
    cam_.dist = d;
    cam_.yaw = std::atan2(l.x - e.x, l.z - e.z);
    cam_.pitch = std::clamp(std::asin(std::clamp((e.y - l.y) / d, -1.f, 1.f)), -1.4f, 1.5533f);
    cam_.fov = game_fov_to_editor(si.fov, std::min(vp_w, vp_h * kGameBandAspect), vp_h);
    char msg[200];
    snprintf(msg, sizeof msg, "Framing shot %d%s: move the view, then %s writes it into the shot (%s leaves).", idx + 1,
             which == 1 ? ", start" : which == 2 ? ", end" : "", kt(A_FRAME_USE).c_str(), kt(A_CANCEL).c_str());
    status_ = msg;
    status_time_ = 8;
}

void App::frame_leave() {
    if (!frame_.on) return;
    frame_.on = false;
    cam_ = frame_.saved;
}

// The editor view (framed or not) becomes the shot's camera: still = the eye and what it looks at; start = the first eye point and
// what it looks at; end = the last eye point (the camera keeps looking at the marker).
void App::shot_use_view(uint32_t ctrl, int which) {
    if (!doc_ || doc_->read_only) return;
    ShotInfo si;
    if (!shot_by_ctrl(doc_->world, ctrl, si)) return;
    const ma2::Vec3 e = cam_eye_now(), l{cam_.tx, cam_.ty, cam_.tz};
    push_undo(which == 0 ? "shot view" : which == 1 ? "shot start view" : "shot end view");
    set_shot_view(*doc_, ctrl, which, e, l, which != 2, io_.get());
    world_changed();
    status_ = which == 2 ? "The shot's camera now ends at the map view." : "The shot's camera is now at the map view.";
    status_time_ = 5;
}

void App::frame_use() {
    if (!frame_.on) return;
    const uint32_t ctrl = frame_.ctrl;
    const int which = frame_.which;
    shot_use_view(ctrl, which);
    frame_leave();
}

void App::frame_step() {
    ShotInfo si;
    int idx = 0;
    if (!doc_ || !shot_by_ctrl(doc_->world, frame_.ctrl, si, &idx)) { frame_.on = false; return; }  // the shot is gone (a new map, undo): nothing to restore
    if (tab_ != Tab::Cameras) { frame_leave(); return; }
    float bx, by, bw, bh;
    game_band(bx, by, bw, bh);
    cam_.fov = game_fov_to_editor(si.fov, bw, vp_h);  // the Zoom box writes the shot's Field of view; the view follows it
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    const ImU32 dim = IM_COL32(0, 0, 0, 153);  // 60 % outside the band
    dl->AddRectFilled(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, by), dim);
    dl->AddRectFilled(ImVec2(vp_x, by + bh), ImVec2(vp_x + vp_w, vp_y + vp_h), dim);
    dl->AddRectFilled(ImVec2(vp_x, by), ImVec2(bx, by + bh), dim);
    dl->AddRectFilled(ImVec2(bx + bw, by), ImVec2(vp_x + vp_w, by + bh), dim);
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), ImGui::ColorConvertFloat4ToU32(theme::tok().glow_halo1), 0, 0, 2.f * dpi_);
    char txt[256];
    snprintf(txt, sizeof txt, "Framing shot %d%s. Move the view, then %s = use this view, %s = leave.", idx + 1,
             frame_.which == 1 ? ", start" : frame_.which == 2 ? ", end" : "", kt(A_FRAME_USE).c_str(), kt(A_CANCEL).c_str());
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    const float px = vp_x + 10 * dpi_, py = vp_y + 8 * dpi_;
    dl->AddRectFilled(ImVec2(px - 6 * dpi_, py - 4 * dpi_), ImVec2(px + ts.x + 6 * dpi_, py + ts.y + 4 * dpi_), IM_COL32(0, 0, 0, 200), 3.f * dpi_);
    dl->AddText(ImVec2(px, py), IM_COL32(240, 240, 240, 255), txt);
    dl->PopClipRect();
}

// Labels of the shot glyphs (the frustum lines are in app.cpp shot_glyph_lines): "2a" at the start, "2b" at the end, "2" for a still shot.
void App::shot_glyph_labels() {
    if (cpv_.on) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    for (auto& g : shot_glyphs_) {
        const ImVec2 sz = ImGui::CalcTextSize(g.label.c_str());
        const ImVec2 p(vp_x + g.sx - sz.x * 0.5f, vp_y + g.sy - sz.y - 6 * dpi_);
        dl->AddRectFilled(ImVec2(p.x - 3 * dpi_, p.y - 1 * dpi_), ImVec2(p.x + sz.x + 3 * dpi_, p.y + sz.y + 1 * dpi_), IM_COL32(0, 0, 0, 170), 2.f * dpi_);
        dl->AddText(p, IM_COL32(200, 220, 60, 255), g.label.c_str());
    }
    dl->PopClipRect();
}

// Cutscene preview: shots in order; each lasts CameraDuration (else its eye path's duration, else 8 s). The eye
// moves along the eye-point step's path (MAPath -> its waypoints, or one object) by share of time; it looks at the
// focus step's point(s) matched by the same share; zoom = the shot camera's Field of view.
namespace {
struct PvShot { std::vector<Vec3> eye, focus; float len = 8, fov_deg = 48; };
Vec3 along(const std::vector<Vec3>& v, float u) {
    if (v.empty()) return {};
    if (v.size() == 1) return v[0];
    std::vector<float> acc{0};
    for (size_t i = 1; i < v.size(); i++) {
        float dx = v[i].x - v[i - 1].x, dy = v[i].y - v[i - 1].y, dz = v[i].z - v[i - 1].z;
        acc.push_back(acc.back() + std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    float d = std::clamp(u, 0.f, 1.f) * acc.back();
    for (size_t i = 1; i < v.size(); i++)
        if (d <= acc[i] || i + 1 == v.size()) {
            float seg = std::max(1e-4f, acc[i] - acc[i - 1]), f = std::clamp((d - acc[i - 1]) / seg, 0.f, 1.f);
            return {v[i - 1].x + (v[i].x - v[i - 1].x) * f, v[i - 1].y + (v[i].y - v[i - 1].y) * f, v[i - 1].z + (v[i].z - v[i - 1].z) * f};
        }
    return v.back();
}
// The preview and the timeline read shots the same way (core/cutscene: cutscene_shots).
std::vector<PvShot> preview_shots(const Wdf& w, uint32_t cinema) {
    std::vector<PvShot> out;
    for (auto& sh : cutscene_shots(w, cinema)) {
        if (sh.eye.empty()) continue;
        PvShot p;
        p.eye = sh.eye;
        p.focus = sh.focus_pts;
        p.len = sh.len;
        p.fov_deg = sh.fov;
        out.push_back(std::move(p));
    }
    return out;
}
}  // namespace

void App::cutscene_preview_start(uint32_t cinema_uid, int shot) {
    std::vector<PvShot> shots = doc_ ? preview_shots(doc_->world, cinema_uid) : std::vector<PvShot>{};
    if (shots.empty() || shot >= int(shots.size())) {
        status_ = "This cutscene has no shot with a camera path to preview.";
        status_time_ = 4;
        return;
    }
    frame_leave();
    if (!cpv_.on) { cpv_.saved = cam_; cpv_.helpers = show_helpers_; }
    show_helpers_ = false;  // the game draws no helpers: preview shows what the player sees
    objects_dirty_ = true;
    cpv_.on = true;
    cpv_.paused = false;
    cpv_.still = 0;
    cpv_.cin = cinema_uid;
    cpv_.t = 0;
    cpv_.stop_at = 0;
    for (int i = 0; shot >= 0 && i <= shot; i++) {
        if (i < shot) cpv_.t += shots[size_t(i)].len;
        else cpv_.stop_at = cpv_.t + shots[size_t(i)].len;
    }
}

void App::camera_look_through(uint32_t cam_uid) {
    if (!doc_ || !object_by_uid(doc_->world, cam_uid)) return;
    frame_leave();
    if (!cpv_.on) { cpv_.saved = cam_; cpv_.helpers = show_helpers_; }
    show_helpers_ = false;
    objects_dirty_ = true;
    cpv_.on = true;
    cpv_.cin = 0;
    cpv_.still = cam_uid;
}

// Timeline scrub and Preview from the playhead: show the camera at second t. Paused = the camera stands there (a scrub).
void App::cutscene_preview_at(uint32_t cinema_uid, float t, bool paused) {
    if (!doc_) return;
    if (!(cpv_.on && cpv_.cin == cinema_uid && !cpv_.still)) cutscene_preview_start(cinema_uid);
    if (!cpv_.on) return;
    cpv_.t = std::max(0.f, t);
    cpv_.stop_at = 0;
    cpv_.paused = paused;
}

void App::cutscene_preview_stop() {
    if (!cpv_.on) return;
    cpv_.on = false;
    cpv_.paused = false;
    cam_ = cpv_.saved;
    show_helpers_ = cpv_.helpers;
    objects_dirty_ = true;
}

void App::cutscene_preview_step(float dt) {
    if (!doc_) { cutscene_preview_stop(); return; }  // Esc is the dispatcher's (A_CANCEL)
    if (cpv_.still) {
        // Look through a camera no shot uses: its position, Heading, a slight downward tilt (the game's
        // CameraRotation is not decoded) and its Field of view. Follows edits made while looking.
        const WObj* o = object_by_uid(doc_->world, cpv_.still);
        Vec3 e;
        if (!o || !get_pos(*o, e)) { cutscene_preview_stop(); return; }
        float h = world::heading(*o), p = 0.15f, fov = 48;
        if (const WProp* f = o->find(0xD186B2CAu)) fov = float(f->as_float());  // Field of view
        float dx = std::sin(h) * std::cos(p), dy = -std::sin(p), dz = std::cos(h) * std::cos(p);
        cam_.dist = 15;
        cam_.tx = e.x + dx * 15; cam_.ty = e.y + dy * 15; cam_.tz = e.z + dz * 15;
        cam_.yaw = h;
        cam_.pitch = p;
        cam_.fov = std::clamp(fov, 5.f, 120.f) / 57.2958f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        std::string nm = o->description().empty() ? "the camera" : o->description();
        std::string txt = "Looking through " + nm + "   (" + kt(A_CANCEL) + " returns)";
        dl->AddRectFilled(ImVec2(vp_x, vp_y + vp_h - 28 * dpi_), ImVec2(vp_x + vp_w, vp_y + vp_h), IM_COL32(0, 0, 0, 170));
        dl->AddText(ImVec2(vp_x + 12 * dpi_, vp_y + vp_h - 22 * dpi_), IM_COL32(230, 230, 230, 255), txt.c_str());
        return;
    }
    std::vector<PvShot> shots = preview_shots(doc_->world, cpv_.cin);
    float total = 0;
    for (auto& s : shots) total += s.len;
    if (shots.empty() || cpv_.t >= total || (cpv_.stop_at > 0 && cpv_.t >= cpv_.stop_at)) { cutscene_preview_stop(); return; }
    float t = cpv_.t;
    size_t i = 0;
    while (i + 1 < shots.size() && t >= shots[i].len) { t -= shots[i].len; i++; }
    const PvShot& sh = shots[i];
    float u = std::clamp(t / sh.len, 0.f, 1.f);
    Vec3 e = along(sh.eye, u);
    Vec3 f = sh.focus.empty() ? Vec3{e.x, e.y, e.z + 10} : along(sh.focus, u);
    float dx = f.x - e.x, dy = f.y - e.y, dz = f.z - e.z, L = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (L > 1e-3f) {
        dx /= L; dy /= L; dz /= L;
        cam_.dist = 15;
        cam_.tx = e.x + dx * 15; cam_.ty = e.y + dy * 15; cam_.tz = e.z + dz * 15;
        cam_.yaw = std::atan2(dx, dz);
        cam_.pitch = -std::asin(std::clamp(dy, -1.f, 1.f));
        float qx, qy, qw, qh;
        game_band(qx, qy, qw, qh);
        cam_.fov = game_fov_to_editor(sh.fov_deg, qw, vp_h);
    }
    // Letterbox, where it is, and a progress bar.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // The bars leave the game's own band (2.35: 1 between the letterbox bars, kGameBandAspect).
    float qx, qy, qw, qh;
    game_band(qx, qy, qw, qh);
    const float bar = qy - vp_y;
    dl->AddRectFilled(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + bar), IM_COL32(0, 0, 0, 255));
    dl->AddRectFilled(ImVec2(vp_x, vp_y + vp_h - bar), ImVec2(vp_x + vp_w, vp_y + vp_h), IM_COL32(0, 0, 0, 255));
    dl->AddRectFilled(ImVec2(vp_x, qy), ImVec2(qx, qy + qh), IM_COL32(0, 0, 0, 255));
    dl->AddRectFilled(ImVec2(qx + qw, qy), ImVec2(vp_x + vp_w, qy + qh), IM_COL32(0, 0, 0, 255));
    char txt[160];
    snprintf(txt, sizeof txt, "%s: shot %zu of %zu   %.1f / %.1f s   (%s stops)", cpv_.paused ? "Paused" : "Preview", i + 1, shots.size(), double(cpv_.t), double(total),
             kt(A_CANCEL).c_str());
    dl->AddText(ImVec2(vp_x + 12 * dpi_, vp_y + vp_h - std::max(bar, 26 * dpi_) + 8 * dpi_), IM_COL32(230, 230, 230, 255), txt);
    float px = vp_x + vp_w * std::clamp(cpv_.t / total, 0.f, 1.f);
    dl->AddRectFilled(ImVec2(vp_x, vp_y + vp_h - 4 * dpi_), ImVec2(px, vp_y + vp_h), ImGui::ColorConvertFloat4ToU32(theme::tok().glow_halo1));
    if (!cpv_.paused) cpv_.t += dt;
}

// Objectives & Triggers tab > AI orders: every order (MAEvent) grouped by the unit it is given to, with the
// trigger that starts it. The same objects as "AI orders it starts" on a trigger, seen from the unit.
void App::tab_orders_by_unit() {
    Scenario& s = *doc_;
    const Wdf& w = s.world;
    const auto& names = gd_.names();
    static const uint32_t K_EVENT = key_of("MAEvent");
    struct Ord { uint32_t ev, trigger; std::string kind; };
    std::map<uint32_t, std::vector<Ord>> by_unit;
    for (auto& o : w.objs) {
        if (o.cls != K_EVENT) continue;
        const WObj* b = object_by_uid(w, order_behaviour(w, o.uid()));
        std::string c = b ? names.label(b->cls) : "";
        std::string kind = c == "MAPatrol" ? "patrol" : c == "MADefend" ? "defend" : c == "MAFollow" ? "follow" : c == "MAAttack" ? "attack" : "order";
        by_unit[order_unit(w, o.uid())].push_back({o.uid(), o.u32("TriggerID"), kind});
    }
    auto name_of = [&](uint32_t u) -> std::string {
        const WObj* o = u ? object_by_uid(w, u) : nullptr;
        if (!o) return u ? "missing #" + std::to_string(u) : "(no unit)";
        return o->description().empty() ? names.label(o->cls) + " #" + std::to_string(u) : o->description();
    };
    ImGui::TextWrapped("The AI orders in this level, by the unit that gets them. Each waits for a trigger to start it; open that trigger to change when it happens or what it asks for.");
    static std::string filter;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##of", "Filter (unit, trigger, patrol, attack...)", &filter);
    std::string lf = lower(filter);
    ImGui::TextDisabled("%zu units, %d orders", by_unit.size(), [&] { int n = 0; for (auto& [u, v] : by_unit) n += int(v.size()); return n; }());
    uint32_t open_trigger = 0, pick_unit = 0;
    ImGui::BeginChild("##olist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (auto& [unit, ords] : by_unit) {
        std::string un = name_of(unit);
        bool any = lf.empty() || lower(un).find(lf) != std::string::npos;
        for (auto& o : ords) any = any || lower(o.kind + " " + name_of(o.trigger)).find(lf) != std::string::npos;
        if (!any) continue;
        ImGui::PushID(int(unit));
        std::string head = un + "  - " + std::to_string(ords.size()) + " order" + (ords.size() == 1 ? "" : "s");
        bool open = ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | (!lf.empty() || by_unit.size() < 8 ? ImGuiTreeNodeFlags_DefaultOpen : 0));
        if (ImGui::IsItemClicked()) pick_unit = unit;
        if (open) {
            for (auto& o : ords) {
                ImGui::PushID(int(o.ev));
                if (ImGui::Selectable((o.kind + "  started by '" + name_of(o.trigger) + "'").c_str(), false) && o.trigger) open_trigger = o.trigger;
                ImGui::SetItemTooltip("Open the trigger that starts this order.");
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    // An empty list was a blank box; say how an order is made.
    if (by_unit.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("No AI orders yet. To give one: select a unit on the map, then press \"Give this unit orders...\" in its Properties "
                           "(patrol, attack, go to a point). Each order waits for a trigger to start it.");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    if (pick_unit) select_uid(pick_unit);
    if (open_trigger) { trig_uid_ = open_trigger; trig_mode_ = 0; select_uid(open_trigger); }
}

}  // namespace ed
