// Level tree > Add part / Duplicate: any class the retail levels use can be added to a scenario as a
// copy of a retail object (links empty), and any object can be duplicated with all its parts.
#include <algorithm>
#include <unordered_map>

#include "../core/triggers.h"
#include "../core/util.h"
#include "../core/cutscene.h"
#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "imgui_stdlib.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

// Ctrl+D, the Duplicate buttons and the right-click menu: copies of the selection hang on the mouse (the paste ghost) and a click
// puts them down. A selection with a part that has no place on the map (a timer, a set...) cannot hang on the mouse: it is copied at once.
void App::duplicate_selected() {
    if (!doc_ || !editable() || selection_.empty()) return;
    for (uint32_t u : selection_) {
        const WObj* so = object_by_uid(doc_->world, u);
        Vec3 p;
        if (!so || !get_pos(*so, p)) { duplicate_selected_now(); return; }
    }
    std::vector<Clip> clips = clips_of_selection();
    if (clips.empty()) { duplicate_selected_now(); return; }
    arm_paste(std::move(clips), true);
}

void App::duplicate_selected_now() {
    if (!doc_ || !editable() || selection_.empty()) return;
    push_undo("duplicate");
    std::vector<uint32_t> made;
    std::vector<float> above;  // each original's height over the surface it stands on: the copy keeps it over the surface it lands on
    for (uint32_t u : std::vector<uint32_t>(selection_.begin(), selection_.end())) {
        const WObj* so = object_by_uid(doc_->world, u);
        const float ab = so && !sheet_kind(*so) ? above_surface(*so) : 0.f;
        uint32_t r = io_->duplicate_subtree(*doc_, u, 12.f);
        if (r) { made.push_back(r); above.push_back(ab); }
    }
    if (made.empty()) {
        status_ = "Nothing to duplicate (the root cannot be copied).";
        status_time_ = 4;
        return;
    }
    selection_.clear();
    for (size_t k = 0; k < made.size(); k++) {
        const uint32_t r = made[k];
        selection_.insert(r);
        if (WObj* po = object_by_uid(doc_->world, r); po && sheet_kind(*po)) {  // a copied flat piece goes on top of what it overlaps
            po->erase(key_of(place::kLayerProp));
            sheet_dropped(r);
        } else {
            if (WObj* wo = object_by_uid(doc_->world, r); wo && wo->cls == K_WATER) wo->erase(key_of(place::kBasinProp));  // a copy of a lake digs nothing
            put_on_surface(r, above[k]);
        }
    }
    world_changed();
    status_ = std::to_string(made.size()) + " duplicated (the copy sits next to the original). " + kt(A_UNDO) + " undoes.";
    status_time_ = 4;
}

void App::add_part_to(uint32_t parent_uid, uint32_t cls) {
    if (!doc_ || !editable()) return;
    push_undo("add part");
    Vec3 at{cam_.tx, 0, cam_.tz};
    uint32_t u = io_->add_part(*doc_, cls, parent_uid, &at);
    if (!u) {
        status_ = "Could not add it there.";
        status_time_ = 4;
        return;
    }
    selection_.clear();
    selection_.insert(u);
    world_changed();
    status_ = "Added. Its settings are in Properties; " + kt(A_UNDO) + " undoes.";
    status_time_ = 4;
}

uint32_t App::new_cutscene(bool intro, bool own_undo) {
    if (!doc_ || !editable() || !io_) return 0;
    if (own_undo) push_undo("new cutscene");
    int n = 1;
    for (auto& o : doc_->world.objs) n += o.cls == key_of("MACinema");
    Vec3 at{cam_.tx, 0, cam_.tz};
    // An intro looks at the player start with its flight path behind the start's facing.
    float face = 0;
    const WObj* st = intro && !doc_->multiplayer ? object_by_uid(doc_->world, player_start_uid(doc_->world)) : nullptr;
    if (st && get_pos(*st, at)) face = st->f32("StartOrientation");
    else st = nullptr;
    uint32_t u = io_->add_cutscene(*doc_, at, "New cutscene " + std::to_string(n), st ? &face : nullptr);
    if (!u) {
        status_ = "Could not build a cutscene (the game's Mission 05 is needed as the model).";
        status_time_ = 5;
        return 0;
    }
    if (!st) {  // A cutscene made over the view starts as a still shot of what the view shows (an intro keeps its flight behind the start)
        auto shots = cutscene_shots(doc_->world, u);
        if (!shots.empty()) set_shot_view(*doc_, shots[0].ctrl, 0, {cam_.eye.x, cam_.eye.y, cam_.eye.z}, {cam_.tx, cam_.ty, cam_.tz}, true, io_.get());
    }
    selection_.clear();
    selection_.insert(u);
    world_changed();
    status_ = st ? "Intro cutscene added over the player start: the camera flies the teal path behind the player and looks at the start. Move the points, change the shot in Properties."
                 : "Cutscene added at the view centre: the camera flies the teal path over the focus marker. Move the points, change the shot in Properties.";
    status_time_ = 8;
    return u;
}

void App::add_cutscene_shot(uint32_t cinema_uid) {
    if (!doc_ || !editable() || !io_) return;
    push_undo("add shot");
    // A still 5 s shot of what the view shows now (the eye where the camera is, looking at the view centre); nothing copied from the last shot.
    Vec3 eye{cam_.eye.x, cam_.eye.y, cam_.eye.z}, look{cam_.tx, cam_.ty, cam_.tz};
    uint32_t c = io_->add_cutscene_shot(*doc_, cinema_uid, &eye, &look, kShotDefault);
    if (!c) {
        status_ = "That cutscene has no shot to copy.";
        status_time_ = 4;
        return;
    }
    tl_.shot = std::max(0, int(cutscene_shots(doc_->world, cinema_uid).size()) - 1);
    selection_.clear();
    selection_.insert(c);
    world_changed();
    status_ = "Shot added after the last one: a still 5-second shot of the view you have now. The yellow line on the map shows which way its camera looks.";
    status_time_ = 6;
}

// The picker: classes the retail levels put under this kind of parent first, then all others.
void App::add_part_popup() {
    if (add_part_open_) {
        ImGui::OpenPopup("Add part##ap");
        add_part_open_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440 * dpi_, 480 * dpi_), ImGuiCond_Appearing);
    if (!theme::begin_modal("Add part##ap", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (!doc_ || !io_ || !add_part_parent_) {
        ImGui::CloseCurrentPopup();
        theme::end_modal();
        return;
    }
    const Wdf& w = doc_->world;
    WTree t;
    t.build(w);
    int pi = t.find(add_part_parent_);
    if (pi < 0) {
        ImGui::CloseCurrentPopup();
        theme::end_modal();
        return;
    }
    const auto& names = gd_.names();
    const WObj& po = w.objs[size_t(pi)];
    std::string pname = po.description().empty() ? names.label(po.cls) : po.description();
    ImGui::TextWrapped("Add under: %s (%s)", pname.c_str(), names.label(po.cls).c_str());
    ImGui::TextDisabled("Every class here exists in the game's own levels; the new part is a copy of one of them with its links empty.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##apf", "Find (e.g. timer, effect, camera, light, checkpoint)", &add_part_filter_);
    std::string lf = lower(add_part_filter_);
    const auto& kinds = io_->part_kinds();
    std::unordered_map<uint32_t, int> under;
    for (auto& k : kinds)
        for (auto& [pc, n] : k.parent_classes)
            if (pc == po.cls) under[k.cls] = n;
    std::vector<const ScenarioIO::PartKind*> here, other;
    for (auto& k : kinds) {
        if (!lf.empty()) {
            const DictClass* dc = dict_.cls(k.cls);
            if (lower(k.name + " " + (dc ? dc->description : std::string())).find(lf) == std::string::npos) continue;
        }
        (under.count(k.cls) ? here : other).push_back(&k);
    }
    std::sort(here.begin(), here.end(), [&](auto* a, auto* b) { return under[a->cls] > under[b->cls]; });
    bool close = false;
    ImGui::BeginChild("##aplist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4), ImGuiChildFlags_Borders);
    auto row = [&](const ScenarioIO::PartKind* k, int under_n) {
        const DictClass* dc = dict_.cls(k->cls);
        std::string l = k->name + "##" + std::to_string(k->cls);
        if (ImGui::Selectable(l.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups)) {
            add_part_to(add_part_parent_, k->cls);
            close = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
            if (dc && !dc->description.empty()) ImGui::TextUnformatted(dc->description.c_str());
            ImGui::TextDisabled("%d in %d of the game's levels%s. Copy of the one in %s.", k->count, k->worlds,
                                k->positioned ? ", has a place on the map" : "", k->sample_map.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        if (under_n) ImGui::TextDisabled("%d under %s in the game's levels", under_n, names.label(po.cls).c_str());
        else ImGui::TextDisabled("%d in the game's levels", k->count);
    };
    if (!here.empty()) {
        ImGui::SeparatorText(("The game's levels put these under " + names.label(po.cls)).c_str());
        for (auto* k : here) row(k, under[k->cls]);
    }
    if (!other.empty()) {
        ImGui::SeparatorText(here.empty() ? "All classes" : "Other classes");
        for (auto* k : other) row(k, 0);
    }
    if (here.empty() && other.empty()) ImGui::TextDisabled("Nothing matches.");
    ImGui::EndChild();
    if (ImGui::Button("Close") || close || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        add_part_parent_ = 0;
        ImGui::CloseCurrentPopup();
    }
    theme::end_modal();
}

// Units tab: things with a place on the map that the game never draws (lights, cameras, checkpoints,
// markers, menu and point triggers), added as copies of the retail objects and put down with a click.
void App::place_helper(const char* cls_name, const char* label, const char* group) {
    if (!doc_ || !editable() || !io_) return;
    Wdf& w = doc_->world;
    const uint32_t cls = key_of(cls_name);
    uint32_t parent = 0;
    for (auto& o : w.objs) if (o.cls == cls && o.parent()) { parent = o.parent(); break; }  // where the level keeps its own
    push_undo("add helper");
    if (!parent) {
        uint32_t next = next_uid(w);
        parent = ensure_group(w, root_uid(w), group, next);
    }
    Vec3 at{cam_.tx, 0, cam_.tz};
    uint32_t u = io_->add_part(*doc_, cls, parent, &at);
    if (!u) { status_ = std::string("Could not add the ") + label + "."; status_time_ = 4; return; }
    // Plain default name ("Camera 3", not "New FreeCamera"): the label plus the next free number among this class.
    {
        int n = 0;
        for (const auto& o : w.objs) if (o.cls == cls) n++;
        for (auto& o : w.objs) if (o.uid() == u) { if (o.find("Description")) o.set_str("Description", std::string(label) + " " + std::to_string(n)); break; }
    }
    show_helpers_ = true;
    objects_dirty_ = true;
    for (int g = 0; g < HG_COUNT; g++) show_helper_group_[g] = show_helper_group_[g] || helper_group(cls) == g;
    selection_ = {u};
    pick_ = PickTarget();
    pick_.kind = PickTarget::MoveShape;
    pick_.uid = u;
    pick_.back = tab_;
    tool_ = Tool::PickPoint;
    world_changed();
    status_ = std::string(label) + " added at the view centre: click the map to put it down (right-click leaves it here).";
    status_time_ = 8;
}

void App::new_trigger_area(bool box) {
    if (!doc_ || !editable()) return;
    Scenario& s = *doc_;
    const char* base = box ? "Trigger box" : "Trigger circle";
    std::string name = base;
    for (int k = 2;; k++) {  // "Trigger box 2" when the map has one: the Triggers list tells them apart
        bool used = false;
        for (auto& o : s.world.objs) used |= o.description() == name;
        if (!used) break;
        name = std::string(base) + " " + std::to_string(k);
    }
    push_undo("new trigger");
    TriggerShape sh;
    sh.box = box;
    sh.x = cam_.tx;
    sh.z = cam_.tz;
    sh.name = name;
    uint32_t u = add_trigger_shape(s, sh);
    if (!u) { status_ = "Could not add the trigger area (the level's trigger objects are not loaded)."; status_time_ = 5; return; }
    trig_uid_ = u;
    select_uid(u);
    s.dirty = true;
    world_changed();
    pick_ = PickTarget();
    pick_.kind = PickTarget::MoveShape;
    pick_.uid = u;
    pick_.back = tab_;
    tool_ = Tool::PickPoint;
    status_ = name + " added: click the map to put it down, drag its handles to size it. What it does: Objectives & Triggers.";
    status_time_ = 10;
}

void App::trigger_area_buttons(const char* id) {
    ImGui::PushID(id);
    ImGui::BeginDisabled(!editable());
    if (ImGui::Button("Trigger box")) new_trigger_area(true);
    ImGui::SetItemTooltip("A box on the map that fires when the player enters it (the game's own trigger volume). Click the map to put it down; "
                          "choose what happens on Objectives & Triggers, where it is listed.");
    ImGui::SameLine();
    if (ImGui::Button("Trigger circle")) new_trigger_area(false);
    ImGui::SetItemTooltip("A circle on the map that fires when the player comes within its radius. Click the map to put it down; "
                          "choose what happens on Objectives & Triggers, where it is listed.");
    ImGui::SameLine();
    if (ImGui::Button("Open Objectives & Triggers")) tab_ = Tab::Triggers;
    ImGui::SetItemTooltip("Timers, 'units destroyed' and the actions of every trigger are made there.");
    ImGui::EndDisabled();
    ImGui::PopID();
}

void App::ui_helper_buttons() {
    if (!doc_ || !io_) return;
    if (!theme::section("Helpers and logic (not drawn in the game)")) return;
    ImGui::BeginDisabled(doc_->read_only);
    struct H { const char* cls; const char* label; const char* group; const char* tip; };
    static const H hs[] = {
        {"MAPointLight", "Light", "Lights", "A point light that lights the ground and buildings around it."},
        {"MACheckpoint", "Checkpoint", "Checkpoints", "A Check It control plate (multiplayer): a team takes it by standing on it. On its own it has no terminal; \"Add Check It\" under Game-mode objects brings plates with their terminals."},
        {"MADummyObj", "Marker", "Markers", "An empty point other objects can refer to (a defend point, a teleport target)."},
        {"MATriggerMenuActivate", "Menu trigger", "Triggers", "Fires when the player chooses a menu entry at this place."},
        {"MATriggerPoint", "Trigger point", "Triggers", "A point that triggers can test against."},
    };
    for (size_t i = 0; i < std::size(hs); i++) {
        if (i % 3) ImGui::SameLine();
        if (ImGui::Button(hs[i].label)) place_helper(hs[i].cls, hs[i].label, hs[i].group);
        ImGui::SetItemTooltip("%s Add it, then click the map. The Objectives & Triggers tab adds timers and the actions; effects are in the object list above (Effects).", hs[i].tip);
    }
    ImGui::EndDisabled();
    trigger_area_buttons("helpers");
}

}  // namespace ed
