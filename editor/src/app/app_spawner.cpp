// Spawners (MASpawner): the retail way units arrive later. A spawner with Enabled 0 waits for the trigger in its
// TriggerEnableID and then makes its unit (wiki reinforcements-spawner-pattern: 157 of 167 campaign spawners; e.g.
// Mission_05 'TR - Return from 3' starts three tank spawners). Properties shows when it starts and how many it makes.
#include <algorithm>

#include "../core/triggers.h"
#include "app.h"
#include "ui_tips.h"
#include "imgui.h"
#include "theme.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static const char* const kArrivesTip =
    "When the unit comes into the mission: at the start, or later when a trigger fires (reinforcements). "
    "'A new circle' makes the trigger for you: click the map where the player must be.";

// A circle trigger "Reinforcements arrive" for the player to walk into; arms the click that puts it on the map.
uint32_t App::arrival_circle() {
    Scenario& s = *doc_;
    TriggerShape sh;
    sh.box = false;
    sh.x = cam_.tx;
    sh.z = cam_.tz;
    for (int k = 1;; k++) {
        sh.name = "Reinforcements arrive" + (k > 1 ? " " + std::to_string(k) : std::string());
        bool used = false;
        for (auto& x : s.world.objs) used |= x.description() == sh.name;
        if (!used) break;
    }
    uint32_t c = add_trigger_shape(s, sh);
    if (!c) {
        status_ = "Could not make the circle (the game's trigger pieces were not found).";
        status_time_ = 6;
        return 0;
    }
    pick_ = PickTarget();
    pick_.kind = PickTarget::MoveShape;
    pick_.uid = c;
    pick_.back = tab_;  // stay with the unit (review B7: the click left Units and Props for the briefing)
    tool_ = Tool::PickPoint;
    status_ = "Click the map where the player should be when the reinforcements arrive.";
    status_time_ = 10;
    return c;
}

// Dogfood D5: ONE way to say when a unit comes in, for both retail reinforcement patterns. A unit hidden by a
// 0-second timer and shown by a trigger (pattern A, the circle template) reads "When 'X' fires"; picking a trigger
// or a new circle makes the unit a spawner reinforcement (pattern B, make_reinforcement), whose Properties then
// show the same row (ui_spawner). hiders = the unit's hide-at-start actions. True when the world changed.
bool App::ui_unit_arrives(uint32_t uid, const std::vector<uint32_t>& hiders) {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const auto& names = gd_.names();
    static const uint32_t K_SHOW = key_of("MAActionShow");
    auto is_trig = [&](uint32_t c) { return names.label(c).rfind("MATrigger", 0) == 0; };
    auto trig_name = [&](const WObj& x) { return x.description().empty() ? "Trigger #" + std::to_string(x.uid()) : x.description(); };
    uint32_t shown_by = 0;
    if (!hiders.empty())
        for (auto& ub : used_by(w, uid, is_trig))
            if (const WObj* a = ub.kind == UsedBy::Action ? object_by_uid(w, ub.holder) : nullptr)
                if (a->cls == K_SHOW && a->i32("Enable") != 0) { shown_by = ub.trigger; break; }
    const WObj* st = shown_by ? object_by_uid(w, shown_by) : nullptr;
    std::string lab = hiders.empty() ? "At the start" : st ? "When '" + trig_name(*st) + "' fires" : "Hidden: no trigger yet";
    uint32_t to = 0;
    bool circle = false, at_start = false;
    form_label("Arrives", form_col());
    if (ImGui::BeginCombo("##arrives", lab.c_str())) {
        if (ImGui::Selectable("At the start", hiders.empty())) at_start = true;
        if (ImGui::Selectable("When the player enters a new circle...")) circle = true;
        for (auto& x : w.objs) {
            if (!is_trig(x.cls)) continue;
            std::string l = "When '" + trig_name(x) + "' fires##" + std::to_string(x.uid());
            if (ImGui::Selectable(l.c_str(), x.uid() == shown_by)) to = x.uid();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s\n\n%s", lab.c_str(), kArrivesTip);  // the combo cuts long trigger names off
    if (st && ImGui::SmallButton("Open the trigger")) { trig_uid_ = shown_by; trig_mode_ = 0; tab_ = Tab::Triggers; select_uid(shown_by); }
    if (at_start && !hiders.empty()) {
        push_undo("arrives at the start");
        for (uint32_t h : hiders) wedit::remove_subtree(w, h);
        world_changed();
        return true;
    }
    if ((to == 0 || to == shown_by) && !circle) return false;
    push_undo("reinforcements");
    if (circle && !(to = arrival_circle())) { world_changed(); return true; }
    for (uint32_t h : hiders) wedit::remove_subtree(w, h);  // the spawner brings it in now
    uint32_t sp = io_->make_reinforcement(s, uid);
    if (WObj* so = sp ? object_by_uid(w, sp) : nullptr) {
        so->set_u32("Enabled", 0);
        so->set_u32("TriggerEnableID", to);
        show_later_ = true;  // the map view leaves spawner units out otherwise: keep it on screen
        if (!circle) {
            const WObj* t = object_by_uid(w, to);
            status_ = "This unit now arrives when '" + (t ? trig_name(*t) : std::string("the trigger")) + "' fires.";
            status_time_ = 8;
        }
    } else {
        status_ = "Could not make reinforcements here (the game data's spawner pieces were not found).";
        status_time_ = 8;
    }
    world_changed();
    return true;
}

// A vehicle pad: a stand (MACorporeal) > MASpawner > 'Mech or vehicle
// (players.cpp add_pad, the retail MP pads). For the stand, its spawner or the vehicle: the vehicle's UID, else 0.
uint32_t App::pad_vehicle(uint32_t uid) {
    if (!doc_) return 0;
    const Wdf& w = doc_->world;
    static const uint32_t K_SPAWNER = key_of("MASpawner"), K_CORP = key_of("MACorporeal");
    static const uint32_t vehicles[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer")};
    auto is_vehicle = [&](int i) { return std::find(std::begin(vehicles), std::end(vehicles), w.objs[size_t(i)].cls) != std::end(vehicles); };
    WTree t;
    t.build(w);
    int i = t.find(uid);
    if (i < 0) return 0;
    // Retail pads stand on the same NULL corporeal (32093) as a mission's reinforcement holder: a multiplayer map has
    // no reinforcements, so there it is a pad; in a mission only a stand that is not the NULL holder counts.
    auto stand_ok = [&](int st) {
        return st >= 0 && w.objs[size_t(st)].cls == K_CORP && (doc_->multiplayer || w.objs[size_t(st)].template_id() != 32093);
    };
    auto vehicle_of = [&](int sp) -> uint32_t {  // sp: a spawner on a stand
        if (sp < 0 || w.objs[size_t(sp)].cls != K_SPAWNER || !stand_ok(t.parent_of[size_t(sp)])) return 0;
        for (int v : t.kids[size_t(sp)]) if (is_vehicle(v)) return w.objs[size_t(v)].uid();
        return 0;
    };
    if (is_vehicle(i)) return vehicle_of(t.parent_of[size_t(i)]) == uid ? uid : 0;
    if (w.objs[size_t(i)].cls == K_SPAWNER) return vehicle_of(i);
    if (!stand_ok(i)) return 0;
    for (int k : t.kids[size_t(i)]) if (uint32_t v = vehicle_of(k)) return v;
    return 0;
}

void App::ui_spawner(uint32_t uid) {
    if (!doc_) return;
    Scenario& s = *doc_;
    Wdf& w = s.world;
    static const uint32_t K_SPAWNER = key_of("MASpawner");
    WTree t;
    t.build(w);
    int i = t.find(uid);
    if (i < 0) return;
    // The spawner itself, the unit it makes (its child) or the stand that holds it (its parent).
    int sp = -1;
    for (int p = i, n = 0; p >= 0 && n < 3; p = t.parent_of[size_t(p)], n++)
        if (w.objs[size_t(p)].cls == K_SPAWNER) { sp = p; break; }
    if (sp < 0)
        for (int k : t.kids[size_t(i)])
            if (w.objs[size_t(k)].cls == K_SPAWNER) { sp = k; break; }
    if (sp < 0) return;
    WObj& o = w.objs[size_t(sp)];
    if (pad_vehicle(uid)) {
        // A pad: no arrival trigger and no team, just how often a new vehicle waits on it. A count of 0 is no limit
        // (the spawn tick skips a 0 cap; 1028 of 1202 retail spawners use 0).
        if (!theme::section("Vehicle pad", ImGuiTreeNodeFlags_DefaultOpen)) return;
        ImGui::BeginDisabled(s.read_only);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Pads have no team: any player or bot who reaches it can climb in. A new one comes when the last one is gone, within the limits below.");
        ImGui::PopTextWrapPos();
        auto padnum = [&](const char* key, const char* label, const char* fmt, float lo, float hi, const char* tipt) {
            WProp* p = o.find(key);
            if (!p || !p->numeric()) return;
            float v = float(p->as_float());
            form_label(label, form_col());
            if (ImGui::DragFloat((std::string("##pad") + key).c_str(), &v, 0.2f, lo, hi, v < 0.5f ? "no limit" : fmt)) {
                push_undo_once("pad count");
                p->set_float(std::clamp(v, lo, hi));
                s.dirty = true;
            }
            ImGui::SetItemTooltip("%s", tipt);
        };
        padnum("MaxSpawners", "Vehicles in total", "%.0f", 0, 99, "How many vehicles the pad gives over the whole match. 0 = no limit.");
        padnum("MaxActiveSpawners", "On the map at once", "%.0f", 1, 20, "How many of this pad's vehicles can be out at the same time (usually 1).");
        padnum("SpawnRate", "Seconds before a new one", "%.0f s", 0, 600, "Seconds from one vehicle appearing to the next one being allowed (it still waits until the pad has room). 0 = straight away.");
        ImGui::EndDisabled();
        return;
    }
    if (!theme::section("When it arrives", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::BeginDisabled(s.read_only);
    const auto& names = gd_.names();
    auto touch = [&](const char* what) { push_undo(what); s.dirty = true; objects_dirty_ = true; };
    auto trig_name = [&](const WObj& x) { return x.description().empty() ? "Trigger #" + std::to_string(x.uid()) : x.description(); };
    bool on = o.i32("Enabled") != 0;
    uint32_t cur = o.u32("TriggerEnableID");
    const WObj* ct = cur ? object_by_uid(w, cur) : nullptr;
    std::string lab = on ? "At the start" : ct ? "When '" + trig_name(*ct) + "' fires" : cur ? "Never: trigger deleted" : "Never: pick a trigger";
    bool new_circle = false;
    form_label("Arrives", form_col());
    if (ImGui::BeginCombo("##arrives", lab.c_str())) {
        if (ImGui::Selectable("At the start", on)) {
            touch("spawner start");
            o.set_i32("Enabled", 1);
            o.set_u32("TriggerEnableID", 0);
        }
        if (ImGui::Selectable("When the player enters a new circle...")) new_circle = true;
        for (auto& x : w.objs) {
            if (names.label(x.cls).rfind("MATrigger", 0) != 0) continue;
            std::string l = "When '" + trig_name(x) + "' fires##" + std::to_string(x.uid());
            if (ImGui::Selectable(l.c_str(), !on && x.uid() == cur)) {
                touch("spawner trigger");
                o.set_i32("Enabled", 0);
                o.set_u32("TriggerEnableID", x.uid());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s\n\n%s", lab.c_str(), kArrivesTip);  // the combo cuts long trigger names off
    if (new_circle) {
        push_undo("reinforcements circle");
        uint32_t sp_uid = o.uid();
        uint32_t c = arrival_circle();
        if (WObj* so = c ? object_by_uid(w, sp_uid) : nullptr) {
            so->set_i32("Enabled", 0);
            so->set_u32("TriggerEnableID", c);
        }
        world_changed();
        ImGui::EndDisabled();
        return;
    }
    if (!on && ct && ImGui::SmallButton("Open the trigger")) { trig_uid_ = cur; trig_mode_ = 0; tab_ = Tab::Triggers; select_uid(cur); }
    // Counts keep the stored type (retail files mix int and float widths).
    auto num = [&](const char* key, const char* label, const char* fmt, float lo, float hi, const char* tipt) {
        WProp* p = o.find(key);
        if (!p || !p->numeric()) return;
        float v = float(p->as_float());
        form_label(label, form_col());
        if (ImGui::DragFloat((std::string("##") + key).c_str(), &v, 0.2f, lo, hi, fmt)) {
            push_undo_once("spawner count");
            p->set_float(std::clamp(v, lo, hi));
            s.dirty = true;
        }
        ImGui::SetItemTooltip("%s", tipt);
    };
    num("MaxSpawners", "Units in total", "%.0f", 1, 99, "How many units it makes over the mission (one = a single reinforcement).");
    num("MaxActiveSpawners", "At once", "%.0f", 1, 20, "How many of them can be alive at the same time; a new one comes when one is destroyed.");
    num("SpawnRate", "Seconds between", "%.0f s", 0, 600, "Wait between one unit and the next (0 = straight away).");
    ImGui::EndDisabled();
}

}  // namespace ed
