// Cameras & Cutscenes tab: the level's own MACinema objects, their shots (MAFreeCameraController with
// its camera, start/end triggers, eye path and focus), which triggers play them, and Add shot / New
// cutscene / Delete. Same real objects as everywhere else (see ScenarioIO::add_cutscene).
#include <algorithm>
#include <cmath>
#include <map>

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
    // gets the taller list instead of the one-row list from a level with one camera (review BB2-S1).
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
        ImGui::SetItemTooltip(it != used.end() ? "Play the shot this camera flies, in the map view (Esc stops)."
                                               : "See the map from this camera: its position, Heading and zoom (Esc returns).");
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

    ImGui::TextWrapped("A cutscene is a camera sequence the game plays (the intro, a base reveal): one or more shots, each with a camera, "
                       "an eye path it flies and a focus point it looks at. A trigger action \"Play a camera sequence\" starts it.");
    {
        struct Dis { explicit Dis(bool d) { ImGui::BeginDisabled(d); } ~Dis() { ImGui::EndDisabled(); } } d(ro);
        if (ImGui::Button("New cutscene")) { if (uint32_t u = new_cutscene()) cine_uid_ = u; }
        ImGui::SetItemTooltip("Add a cutscene at the view centre: one shot whose camera flies a path over a focus marker.");
        if (!doc_->multiplayer) {
            ImGui::SameLine();
            if (ImGui::Button("New intro")) { if (uint32_t u = new_cutscene(true)) cine_uid_ = u; }
            ImGui::SetItemTooltip("Add a cutscene over the player start: the camera flies a path behind the player and looks at the start. "
                                  "Then \"Play at mission start\" makes it the intro.");
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
            q += "\n\nYou can undo this with Ctrl+Z.";
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
        ImGui::SetItemTooltip("Delete the selected cutscene (asks first; Ctrl+Z undoes it).");
        ImGui::EndDisabled();
        same_line_or_wrap("Show on map");
        if (ImGui::Button("Show on map")) {
            show_helpers_ = true;
            show_helper_group_[HG_CAMERAS] = true;
            objects_dirty_ = true;
            select_uid(cine_uid_);
            // Retail cutscene objects sit at 0,0 (the map corner): frame the cameras, path points and focus markers
            // of its shots instead (review BB2-S6).
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
        ImGui::SetItemTooltip("Play the selected cutscene in the map view (Esc stops).");
    }

    ImGui::BeginChild("##clist", ImVec2(0, 150 * dpi_), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    if (cinemas.empty()) ImGui::TextDisabled("This level has no cutscenes.");
    for (int ci : cinemas) {
        const WObj& o = w.objs[size_t(ci)];
        int n = int(shots_of(ci).size());
        std::string l = name_of(o.uid()) + "  - " + std::to_string(n) + " shot" + (n == 1 ? "" : "s");
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
        if (ImGui::InputText("##cutname", &nm)) { push_undo_once("rename cutscene"); s.dirty = true; objects_dirty_ = true; cin.set_str("Description", nm); }
    }
    ImGui::EndDisabled();

    // Played by: the actions that start it.
    auto uses = used_by(w, cin.uid(), [&](uint32_t c) { return names.label(c).rfind("MATrigger", 0) == 0; });
    ImGui::SeparatorText("Played by");
    if (uses.empty()) {
        hint_text("No trigger plays it yet. Make one here, or add the action \"Play a camera sequence\" to a trigger.");
        // One click: the trigger and its "Play a camera sequence" action, already pointing at this cutscene.
        ImGui::BeginDisabled(ro);
        const uint32_t cu = cin.uid();
        const std::string cn = cin.description();
        if (ImGui::Button("Play at mission start")) {
            push_undo("play cutscene at start");
            uint32_t u = add_trigger_object(s, "MATriggerTimer", "Play " + cn);
            // 1 s, not 0: in a custom mission a 0 s timer's cinema never showed in game (D13, g3 run).
            if (WObj* o = object_by_uid(w, u)) { o->set_f32("TrgTimerDurationSeconds", 1); o->set_f32("TrgTimerDuration", 1.f / 60.f); }
            if (WObj* a = object_by_uid(w, add_action_object(s, u, "MAActionActivateCinema", "Play " + cn))) a->set_u32("cinemaID", cu);
            world_changed();
            s.dirty = true;
            status_ = "Done: \"" + cn + "\" plays when the mission starts (trigger \"Play " + cn + "\" on the Triggers list).";
            status_time_ = 8;
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
    int n = 0;
    for (auto& u : uses) {
        if (u.kind != UsedBy::Action || !u.trigger) continue;
        ImGui::PushID(n++);
        if (ImGui::Selectable(("Trigger '" + name_of(u.trigger) + "'").c_str(), false)) open_trigger = u.trigger;
        ImGui::SetItemTooltip("Open the trigger on the Triggers list.");
        ImGui::PopID();
    }

    // Shots.
    std::vector<int> shots = shots_of(ci);
    ImGui::SeparatorText(("Shots (" + std::to_string(shots.size()) + ")").c_str());
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
        if (u) ImGui::SetItemTooltip(trigger ? "Open this trigger on the Triggers list." : "Select it on the map.");
        ImGui::PopID();
    };
    int shot_no = 0;
    for (int k : shots) {
        const WObj& c = w.objs[size_t(k)];
        shot_no++;
        std::vector<int> sub;
        t.subtree(k, sub);
        uint32_t eye = 0, focus = 0;
        int steps = 0;
        for (int m : sub) {
            const WObj& p = w.objs[size_t(m)];
            if (p.cls != K_PROP) continue;
            steps++;
            if (!eye) eye = p.u32("eyeUID");
            if (!focus) focus = p.u32("focusUID");
        }
        ImGui::PushID(int(c.uid()));
        std::string head = "Shot " + std::to_string(shot_no) + "  " + (c.description().empty() ? name_of(c.u32("Camera")) : c.description()) + "  (" + std::to_string(steps) + " steps)";
        if (ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            link("Camera", c.u32("Camera"), false);
            link("Eye path", eye, false);
            if (const WObj* eo = eye && !ro ? object_by_uid(w, eye) : nullptr; eo && eo->cls == key_of("MAPath")) {
                same_line_or_wrap("Draw flight path", form_col());
                if (theme::small_toggle_button("Draw flight path", picking(PickTarget::CamPath, eye) && pick_.redraw)) {
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::CamPath;
                    pick_.uid = eye;
                    pick_.restart = true;
                    pick_.redraw = true;
                    tool_ = Tool::PlaceWaypoint;
                    selection_ = {eye};
                    status_ = "Click the map once per point the camera flies through, in order; right-click when done.";
                    status_time_ = 10;
                }
                ImGui::SetItemTooltip("Replace the path: click the map once per point, in order (the camera keeps its height above the ground). Right-click when done.");
                same_line_or_wrap("Add points", form_col());
                if (theme::small_toggle_button("Add points", picking(PickTarget::CamPath, eye) && !pick_.redraw)) {
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::CamPath;
                    pick_.uid = eye;
                    tool_ = Tool::PlaceWaypoint;
                    selection_ = {eye};
                }
                ImGui::SetItemTooltip("Click the map to add points after the last one. Right-click when done.");
            }
            link("Looks at", focus, false);
            if (focus && !ro) {
                same_line_or_wrap("Move on map", form_col());
                if (theme::small_toggle_button("Move on map", picking(PickTarget::MoveShape, focus))) {
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::MoveShape;
                    pick_.uid = focus;
                    pick_.back = tab_;
                    tool_ = Tool::PickPoint;
                    selection_ = {focus};
                }
                ImGui::SetItemTooltip("Click the map where the camera should look.");
            }
            link("Starts with", c.u32("triggerInit"), true);
            link("Ends with", c.u32("triggerExit"), true);
            ImGui::BeginDisabled(ro);
            {
                WObj& cm = w.objs[size_t(k)];
                ui_field(cm, "CameraDuration");
                ui_field(cm, "bSkipTo");
                // Steps: what the camera does, when. The eye path and the focus marker are the objects above.
                int si = 0;
                for (int m : sub) {
                    WObj& st = w.objs[size_t(m)];
                    if (st.cls != K_PROP) continue;
                    si++;
                    int ty = st.i32("type");
                    const char* tn = ty == 1 ? "Screen size" : ty == 2 ? "Eye point" : ty == 3 ? "Focus point" : ty == 4 ? "Colour filter" : "Step";
                    ImGui::PushID(m);
                    if (ImGui::TreeNode("##step", "Step %d: %s, from %.1f s", si, tn, st.f32("start"))) {
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
                        WObj* po = st.find(pk) ? object_by_uid(w, st.u32(pk)) : nullptr;
                        if (!po || po->cls != key_of("MAPath")) continue;
                        ImGui::PushID(int(po->uid()));
                        if (ImGui::TreeNode("##path", "  Path '%s': %.1f s", po->description().c_str(), po->f32("duration"))) {
                            ui_field(*po, "duration"); ui_field(*po, "useSpline"); ui_field(*po, "smoothIn"); ui_field(*po, "smoothOut");
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndDisabled();  // previewing works on read-only levels too
            if (ImGui::SmallButton("Preview shot")) cutscene_preview_start(cin.uid(), shot_no - 1);
            ImGui::SetItemTooltip("Play only this shot in the map view: see where its camera looks (move the 'Looks at' marker to aim it). Esc stops.");
            ImGui::BeginDisabled(ro);
            ImGui::SameLine();
            if (ImGui::SmallButton("Select shot")) select_uid(c.uid());
            ImGui::SameLine();
            if (theme::danger_small_button("Delete shot")) del_shot = c.uid();
            ImGui::SetItemTooltip("Delete this shot with the camera, path, focus marker and triggers only it uses.");
            if (shots.size() > 1) {
                ImGui::SameLine();
                ImGui::BeginDisabled(shot_no == 1);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move_shot = shot_no - 1; move_dir = -1; }
                ImGui::SetItemTooltip("Play this shot earlier.");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(shot_no == int(shots.size()));
                if (ImGui::ArrowButton("##down", ImGuiDir_Down)) { move_shot = shot_no - 1; move_dir = 1; }
                ImGui::SetItemTooltip("Play this shot later.");
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(shots.empty());
    if (cpv_.on && cpv_.cin == cin.uid()) {
        if (theme::toggle_button("Stop preview", true)) cutscene_preview_stop();
    } else if (ImGui::Button("Preview")) cutscene_preview_start(cin.uid());
    ImGui::SetItemTooltip("Play the cutscene here in the map view: the camera flies each shot's path looking at its focus point. Esc stops.");
    why_disabled(shots.empty(), "This cutscene has no shots yet, so there is nothing to play.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ro || shots.empty());
    if (ImGui::Button("Add shot")) add_cutscene_shot(cin.uid());
    ImGui::SetItemTooltip("Add a shot after the last one (a copy of it) at the view centre; then Draw flight path and Move on map.");
    why_disabled(ro || shots.empty(), ro ? "This is a real level and cannot be changed. Press \"Make an editable copy\" on the Map tab first." : "Add shot copies the last shot, and this cutscene has none yet.");
    ImGui::EndDisabled();

    if (move_shot >= 0) {
        // The game plays a cinema's controllers in order: swap this shot's piece with the neighbour's
        // at the level where they are siblings (the controllers themselves, or the groups holding them).
        int a = shots[size_t(move_shot)], b = shots[size_t(move_shot + move_dir)];
        while (a >= 0 && b >= 0 && t.parent_of[size_t(a)] != t.parent_of[size_t(b)]) { a = t.parent_of[size_t(a)]; b = t.parent_of[size_t(b)]; }
        if (a >= 0 && b >= 0 && a != b) {
            const std::vector<int>& kids = t.kids[size_t(t.parent_of[size_t(a)])];
            int pa = int(std::find(kids.begin(), kids.end(), a) - kids.begin()), pb = int(std::find(kids.begin(), kids.end(), b) - kids.begin());
            uint32_t au = w.objs[size_t(a)].uid(), bu = w.objs[size_t(b)].uid();
            push_undo("reorder shots");
            // a goes to b's place and b to a's: step a past b, then b back past the ones in between.
            int steps = std::abs(pb - pa);
            for (int i = 0; i < steps; i++) wedit::move_sibling(w, au, move_dir);
            for (int i = 0; i + 1 < steps; i++) wedit::move_sibling(w, bu, -move_dir);
            world_changed();
            s.dirty = true;
        }
    }
    if (del_shot) {
        std::string q = "Delete this shot and the camera, path, focus marker and triggers only it uses?\n\nYou can undo this with Ctrl+Z.";
        confirm(q, [this, del_shot]() {
            if (!doc_ || !io_) return;
            push_undo("delete shot");
            io_->remove_cutscene_shot(*doc_, del_shot);
            selection_.clear();
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
std::vector<PvShot> preview_shots(const Wdf& w, uint32_t cinema) {
    static const uint32_t K_CTRL = key_of("MAFreeCameraController"), K_PROP = key_of("MACameraProperty"), K_PATH = key_of("MAPath"),
                          K_WPG = key_of("MAWaypointGroup");
    std::vector<PvShot> out;
    WTree t;
    t.build(w);
    int ci = t.find(cinema);
    if (ci < 0) return out;
    auto points = [&](uint32_t uid, float* dur) {
        std::vector<Vec3> v;
        int k = uid ? t.find(uid) : -1;
        if (k < 0) return v;
        if (w.objs[size_t(k)].cls == K_PATH) {
            if (dur) *dur = w.objs[size_t(k)].f32("duration");
            k = t.find(w.objs[size_t(k)].u32("m_wptGroupUID"));
        }
        if (k < 0) return v;
        if (w.objs[size_t(k)].cls == K_WPG) {
            for (int c : t.kids[size_t(k)]) if (Vec3 p; get_pos(w.objs[size_t(c)], p)) v.push_back(p);
        } else if (Vec3 p; get_pos(w.objs[size_t(k)], p)) v.push_back(p);
        return v;
    };
    std::vector<int> sub;
    t.subtree(ci, sub);
    for (int k : sub) {
        const WObj& c = w.objs[size_t(k)];
        if (c.cls != K_CTRL) continue;
        PvShot sh;
        float path_dur = 0;
        for (int m : t.kids[size_t(k)]) {
            const WObj& st = w.objs[size_t(m)];
            if (st.cls != K_PROP) continue;
            if (st.i32("type") == 2 && sh.eye.empty()) sh.eye = points(st.u32("eyeUID"), &path_dur);
            if (st.i32("type") == 3 && sh.focus.empty()) sh.focus = points(st.u32("focusUID"), nullptr);
        }
        const WObj* cam = object_by_uid(w, c.u32("Camera"));
        if (sh.eye.empty() && cam) if (Vec3 p; get_pos(*cam, p)) sh.eye.push_back(p);
        if (sh.eye.empty()) continue;
        if (cam) if (const WProp* f = cam->find(0xD186B2CAu)) sh.fov_deg = float(f->as_float());  // Field of view
        float d = c.f32("CameraDuration");
        sh.len = d > 0.05f ? d : path_dur > 0.05f ? path_dur : 8.f;  // the game's default shot length is 8 s
        out.push_back(std::move(sh));
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
    if (!cpv_.on) { cpv_.saved = cam_; cpv_.helpers = show_helpers_; }
    show_helpers_ = false;  // the game draws no helpers: preview shows what the player sees
    objects_dirty_ = true;
    cpv_.on = true;
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
    if (!cpv_.on) { cpv_.saved = cam_; cpv_.helpers = show_helpers_; }
    show_helpers_ = false;
    objects_dirty_ = true;
    cpv_.on = true;
    cpv_.cin = 0;
    cpv_.still = cam_uid;
}

void App::cutscene_preview_stop() {
    if (!cpv_.on) return;
    cpv_.on = false;
    cam_ = cpv_.saved;
    show_helpers_ = cpv_.helpers;
    objects_dirty_ = true;
}

void App::cutscene_preview_step(float dt) {
    if (!doc_ || ImGui::IsKeyPressed(ImGuiKey_Escape)) { cutscene_preview_stop(); return; }
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
        std::string txt = "Looking through " + nm + "   (Esc returns)";
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
        cam_.fov = std::clamp(sh.fov_deg, 5.f, 120.f) / 57.2958f;
    }
    // Letterbox, where it is, and a progress bar.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float bar = vp_h * 0.11f;
    dl->AddRectFilled(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + bar), IM_COL32(0, 0, 0, 255));
    dl->AddRectFilled(ImVec2(vp_x, vp_y + vp_h - bar), ImVec2(vp_x + vp_w, vp_y + vp_h), IM_COL32(0, 0, 0, 255));
    char txt[160];
    snprintf(txt, sizeof txt, "Preview: shot %zu of %zu   %.1f / %.1f s   (Esc stops)", i + 1, shots.size(), cpv_.t, total);
    dl->AddText(ImVec2(vp_x + 12 * dpi_, vp_y + vp_h - bar + 8 * dpi_), IM_COL32(230, 230, 230, 255), txt);
    float px = vp_x + vp_w * std::clamp(cpv_.t / total, 0.f, 1.f);
    dl->AddRectFilled(ImVec2(vp_x, vp_y + vp_h - 4 * dpi_), ImVec2(px, vp_y + vp_h), ImGui::ColorConvertFloat4ToU32(theme::tok().glow_halo1));
    cpv_.t += dt;
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
    // Review B17: an empty list was a blank box; say how an order is made.
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
