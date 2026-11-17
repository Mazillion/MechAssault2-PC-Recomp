// Objectives & Triggers tab: the level's own triggers (MATrigger*), what they do (MAAction* children) and the AI
// orders they start (MAEvent with TriggerID), edited in place, for your maps and the read-only retail
// levels alike. Nothing here is a separate model: a saved map holds exactly these objects (core/triggers.h).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <set>

#include "theme.h"
#include "../core/triggers.h"
#include "../core/mission_extras.h"
#include "../core/cutscene.h"
#include "imgui_internal.h"
#include "ui_tips.h"
#include "trigger_ui.h"
#include "ui_util.h"
#include "app.h"
#include "imgui.h"
#include "imgui_stdlib.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

static bool g_show_trig_editor = false;  // scroll to the selected trigger's editor next frame (New trigger...)

namespace {

struct Plain { const char* cls; const char* name; };

const Plain kTriggerNames[] = {
    {"MATriggerTimer", "After a delay"},
    {"MATriggerRadius", "When units enter a circle"},
    {"MATriggerCuboidalVolume", "When units enter a box"},
    {"MATriggerDestroyed", "When units are destroyed"},
    {"MATriggerLogic", "When other triggers have fired (all or any)"},
    {"MATriggerNationRadius", "When a side's units enter a circle"},
    {"MATriggerPoint", "Point that other triggers test"},
    {"MATriggerMenuActivate", "When the player picks a menu prompt"},
    {"MATriggerCounter", "When something has happened N times"},
    {"MATriggerGameTime", "When the match clock reaches a time"},
    {"MATriggerAttach", "When a unit is attached or detached"},
    {"MATriggerPlayerVehicleType", "When the player is in a vehicle type"},
    {"MATriggerUber", "Mission 17b boss trigger (advanced)"},
    {"MATriggerSpider", "Mission 08 spider trigger (advanced)"},
    {"MATrigger", "When another trigger starts it"},
};

const Plain kActionNames[] = {
    {"MAActionBroadcast", "Show a message"},
    {"MAActionSound", "Play a sound"},
    {"MAActionPlayVoice", "Play a voice line"},
    {"MAActionMusic", "Change the music"},
    {"MAActionMissionVariableSet", "Complete an objective / set a mission flag"},
    {"MAActionGameCondition", "Win or lose the mission"},
    {"MAActionShow", "Show or hide units"},
    {"MAActionRemoveEntity", "Remove units"},
    {"MAActionAI", "Switch units or triggers on or off"},
    {"MAActionAICommand", "Give a unit an order"},
    {"MAActionAIFire", "Make a unit fire at a target"},
    {"MAActionAIScan", "Make a 'Mech sweep its sensors"},
    {"MAActionTeleport", "Move the player to a marker"},
    {"MAActionNation", "Change which side units are on"},
    {"MAActionCheckPointSave", "Save a checkpoint"},
    {"MAActionActivateCinema", "Play a cutscene"},
    {"MAActionCamera", "Change the camera"},
    {"MAActionLights", "Turn lights on or off"},
    {"MAActionPlayAnim", "Play a unit animation"},
    {"MAActionPopupMenu", "Show a menu"},
    {"MAActionChangeHUD", "Switch the HUD layout"},
    {"MAActionChangePlayerVehicle", "Put the player in another vehicle"},
    {"MAActionAddCargo", "Add cargo"},
    {"MAActionAttachDetach", "Attach a rider to a vehicle"},
    {"MAActionAttribute", "Change a unit's setting (invulnerable, accuracy...)"},
    {"MAActionDropShip", "Make a drop ship land, take off or crash"},
    {"MAActionMoeEFX", "Play a visual effect"},
    {"MAActionResponse", "Turn an object's response on or off"},
    {"MAActionSwapSetIn", "Bring a group of units in (swap set)"},
    {"MAActionSwapSetOut", "Take a group of units out (swap set)"},
    {"MAActionCheckItGoal", "Multiplayer checkpoint goal"},
    {"MAActionUber", "Mission 17b boss change (advanced)"},
};

const char* lookup(const Plain* table, size_t n, const std::string& cls) {
    for (size_t i = 0; i < n; i++) if (cls == table[i].cls) return table[i].name;
    return nullptr;
}

// "MAActionFooBar" -> "Foo bar" for classes without a plain name.
std::string words_of(std::string cls, const char* prefix) {
    if (cls.rfind(prefix, 0) == 0) cls = cls.substr(strlen(prefix));
    std::string out;
    for (size_t i = 0; i < cls.size(); i++) {
        if (i && isupper((unsigned char)cls[i]) && !isupper((unsigned char)cls[i - 1])) out += ' ';
        out += out.empty() ? char(toupper((unsigned char)cls[i])) : char(tolower((unsigned char)cls[i]));
    }
    return out.empty() ? "?" : out;
}

struct AddAction { const char* cls; const char* label; };
const AddAction kAddActions[] = {
    {"MAActionBroadcast", "Show a message"},
    {"MAActionSound", "Play a sound"},
    {"MAActionMusic", "Change the music"},
    {"MAActionMissionVariableSet", "Complete an objective"},
    {"MAActionGameCondition", "Win or lose the mission"},
    {"MAActionShow", "Show or hide units"},
    {"MAActionRemoveEntity", "Remove units"},
    {"MAActionAI", "Switch units or triggers on or off"},
    {"MAActionActivateCinema", "Play a cutscene"},
    {"MAActionTeleport", "Move the player to a marker"},
    {"MAActionNation", "Change which side units are on"},
    {"MAActionCheckPointSave", "Save a checkpoint"},
    {"MAActionPlayVoice", "Play a voice line"},
};

// The 15 trigger/action classes the 16 retail multiplayer worlds place (research census of the dictionary, 10-26).
// Campaign-only kinds stay reachable on an MP map under Advanced: the census is what retail places, not what the MP game can run.
bool mp_retail_class(const std::string& cls) {
    static const char* const kMp[] = {
        "MATriggerTimer", "MATriggerDestroyed", "MATriggerRadius", "MATriggerMenuActivate", "MATriggerGameTime", "MATriggerNationRadius",
        "MAActionSound", "MAActionAI", "MAActionAttribute", "MAActionCheckItGoal", "MAActionMusic", "MAActionNation",
        "MAActionRemoveEntity", "MAActionResponse", "MAActionTeleport",
    };
    for (const char* c : kMp) if (cls == c) return true;
    return false;
}

}  // namespace

std::string unit_label(const Wdf& w, uint32_t uid) {
    if (!uid) return "(none)";
    if (const WObj* o = object_by_uid(w, uid)) return o->description().empty() ? "unnamed object #" + std::to_string(uid) : o->description();  // ids only when there is no name
    return "missing #" + std::to_string(uid);
}

std::string trig_kind_label(const std::string& c) {
    if (c.rfind("MATrigger", 0) == 0) {
        const char* n = lookup(kTriggerNames, std::size(kTriggerNames), c);
        return n ? n : words_of(c, "MATrigger");
    }
    const char* n = lookup(kActionNames, std::size(kActionNames), c);
    return n ? n : words_of(c, "MAAction");
}

std::string trig_display_name(const WObj& o, const std::string& cls) { return o.description().empty() ? trig_kind_label(cls) : o.description(); }

std::vector<char> trigger_kinds(const Wdf& w, const NameTable& names) {
    std::vector<char> kind(w.objs.size(), 0);
    for (size_t i = 0; i < w.objs.size(); i++) {
        std::string c = names.label(w.objs[i].cls);
        kind[i] = c.rfind("MATrigger", 0) == 0 ? 1 : c.rfind("MAAction", 0) == 0 ? 2 : 0;
    }
    return kind;
}

// The trigger list: roots in file order, each followed by the triggers nested under it. Left out: a cutscene's own
// start / end triggers and cue timers (a note links to the timeline), except the one picked (keep_uid). level = the trigger is
// one of the Base level's own (same uid and class in its retail world), as the validator's `retail` map reads it.
App::TrigList App::trigger_list(const WTree& t, const std::vector<char>& kind, uint32_t keep_uid) const {
    TrigList out;
    if (!doc_) return out;
    const Wdf& w = doc_->world;
    const auto& names = gd_.names();
    const Wdf* bw = io_ ? io_->base_world(*doc_) : nullptr;
    if (bw != level_cache_world_) {
        level_cache_.clear();
        level_cache_world_ = bw;
        if (bw) for (auto& o : bw->objs) if (names.label(o.cls).rfind("MATrigger", 0) == 0) level_cache_[o.uid()] = o.cls;
    }
    const std::set<uint32_t> owned = cutscene_owned_triggers(w);
    std::function<void(int, int, bool)> walk_kids;
    std::function<void(int, int, bool)> walk = [&](int idx, int depth, bool level) {
        out.rows.push_back({idx, depth, level});
        walk_kids(idx, depth + 1, level);
    };
    // Triggers can sit under an action or a plain group inside another trigger: look through those too.
    walk_kids = [&](int idx, int depth, bool level) {
        for (int k : t.kids[size_t(idx)]) {
            if (kind[size_t(k)] == 1) walk(k, depth, level);
            else walk_kids(k, depth, level);
        }
    };
    for (size_t i = 0; i < w.objs.size(); i++) {
        if (kind[i] != 1) continue;
        bool inner = false;
        for (int p = t.parent_of[i]; p >= 0 && !inner; p = t.parent_of[size_t(p)]) inner = kind[size_t(p)] == 1;
        if (inner) continue;
        const uint32_t u = w.objs[i].uid();
        if (owned.count(u) && u != keep_uid) { out.cutscene_hidden++; continue; }
        auto it = level_cache_.find(u);
        walk(int(i), 0, it != level_cache_.end() && it->second == w.objs[i].cls);
    }
    return out;
}

std::string plain_logic_kind(const std::string& cls) {
    if (cls.rfind("MATrigger", 0) == 0) {
        const char* n = lookup(kTriggerNames, std::size(kTriggerNames), cls);
        return n ? n : words_of(cls, "MA");
    }
    if (cls.rfind("MAAction", 0) == 0) {
        const char* n = lookup(kActionNames, std::size(kActionNames), cls);
        return n ? n : words_of(cls, "MAAction");
    }
    return "";
}

// ----------------------------------------------------------------------------- map overlay

bool App::single_trigger_shape() const {
    if (!doc_ || selection_.size() != 1) return false;
    static const uint32_t K_NRAD = key_of("MATriggerNationRadius");
    for (auto& o : doc_->world.objs) if (o.uid() == *selection_.begin()) return is_trigger_shape(o.cls) || o.cls == K_NRAD;
    return false;
}

// Every trigger class carries a TriggerPoint, but only boxes, circles, nation circles and trigger points use it: the others
// (units destroyed, timer, logic, mission start...) have no place on the map, so they get no gizmo and no "drag the arrows" hint.
bool App::positionless_trigger(const WObj& o) const {
    static const uint32_t K_NRAD = key_of("MATriggerNationRadius"), K_TPOINT = key_of("MATriggerPoint");
    if (is_trigger_shape(o.cls) || o.cls == K_NRAD || o.cls == K_TPOINT) return false;
    return gd_.names().label(o.cls).rfind("MATrigger", 0) == 0;
}

bool App::selection_positionless() const {
    if (!doc_ || selection_.empty()) return false;
    for (auto& o : doc_->world.objs)
        if (selection_.count(o.uid()) && !positionless_trigger(o)) return false;
    return true;
}

// What the selected trigger acts on: its units (green rings), the units a destroyed-trigger watches (red).
void App::draw_trigger_overlay() {
    if (!doc_ || !trig_uid_) return;
    const Wdf& w = doc_->world;
    const WObj* tr = object_by_uid(w, trig_uid_);
    if (!tr) return;
    auto circle = [&](const Vec3& p, float r, uint32_t col) {
        const int N = 48;
        for (int k = 0; k < N; k++) {
            float a0 = k * 6.2831853f / N, a1 = (k + 1) * 6.2831853f / N;
            float x0 = p.x + std::cos(a0) * r, z0 = p.z + std::sin(a0) * r, x1 = p.x + std::cos(a1) * r, z1 = p.z + std::sin(a1) * r;
            r_.lines.push_back({{x0, doc_->terrain.height_at(x0, z0) + 1.5f, z0}, col});
            r_.lines.push_back({{x1, doc_->terrain.height_at(x1, z1) + 1.5f, z1}, col});
        }
    };
    auto ring = [&](uint32_t uid, uint32_t col) {
        const WObj* o = uid ? object_by_uid(w, uid) : nullptr;
        Vec3 p;
        if (o && get_pos(*o, p)) circle(p, 8, col);
    };
    for (auto& o : w.objs) {
        if (o.parent() != trig_uid_) continue;
        for (const char* k : {"ActionEntity", "MarkerUID"}) if (o.find(k)) ring(o.u32(k), 0xFF40FF40u);
        if (o.find("ReferenceUID")) ring(o.u32("ReferenceUID"), 0xFF3030FFu);
    }
    for (uint32_t ev : orders_of_trigger(w, trig_uid_)) ring(order_unit(w, ev), 0xFF40FF40u);
}

// ----------------------------------------------------------------------------- Used by

void App::ui_used_by(uint32_t uid) {
    if (!doc_) return;
    const Wdf& w = doc_->world;
    const auto& names = gd_.names();
    auto is_trig = [&](uint32_t cls) { return names.label(cls).rfind("MATrigger", 0) == 0; };
    auto uses = used_by(w, uid, is_trig);
    if (uses.empty()) return;
    auto kind_label = [&](uint32_t cls) -> std::string {
        std::string c = names.label(cls);
        const char* n = lookup(kActionNames, std::size(kActionNames), c);
        if (n) return n;
        n = lookup(kTriggerNames, std::size(kTriggerNames), c);
        return n ? n : words_of(c, c.rfind("MAAction", 0) == 0 ? "MAAction" : "MA");
    };
    auto name_of = [&](uint32_t u) -> std::string {
        const WObj* o = u ? object_by_uid(w, u) : nullptr;
        if (!o) return u ? "missing #" + std::to_string(u) : "(none)";
        return o->description().empty() ? kind_label(o->cls) : o->description();
    };
    auto behaviour = [&](uint32_t ev) -> std::string {
        const WObj* b = object_by_uid(w, order_behaviour(w, ev));
        if (!b) return "order";
        std::string c = names.label(b->cls);
        return c == "MAPatrol" ? "patrol" : c == "MADefend" ? "defend" : c == "MAFollow" ? "follow" : c == "MAAttack" ? "attack" : "order";
    };
    if (!theme::section("Used by", ImGuiTreeNodeFlags_DefaultOpen)) return;
    uint32_t jump = 0, jump_trigger = 0;
    int n = 0;
    for (auto& u : uses) {
        std::string label;
        uint32_t target = u.trigger;
        bool to_unit = false;
        switch (u.kind) {
            case UsedBy::Action: {
                const WObj* h = object_by_uid(w, u.holder);
                if (u.holder == u.trigger) label = "Watched by the trigger '" + name_of(u.trigger) + "'";  // the trigger's own unit set
                else label = "Trigger '" + name_of(u.trigger) + "' > " + (h ? kind_label(h->cls) : "action");
                break;
            }
            case UsedBy::Ref: {
                const WObj* h = object_by_uid(w, u.holder);
                label = "Path of '" + name_of(u.holder) + "'" + (h ? " (" + kind_label(h->cls) + ")" : "");
                target = u.holder;
                to_unit = true;
                break;
            }
            case UsedBy::Enables:
                label = "Switches on '" + name_of(u.holder) + "'";
                target = u.holder;
                to_unit = true;
                break;
            case UsedBy::Member: {
                label = "Member of the set '" + name_of(u.holder) + "'";
                target = u.holder;
                to_unit = true;
                break;
            }
            case UsedBy::SwapOf:
                label = "Swapped by '" + name_of(u.holder) + "'";
                target = u.holder;
                to_unit = true;
                break;
            case UsedBy::Input:
                label = "Condition of '" + name_of(u.trigger) + "'";
                break;
            case UsedBy::Starts:
                label = "Starts the " + behaviour(u.holder) + " order of " + name_of(u.unit);
                target = u.unit;
                to_unit = true;
                break;
            case UsedBy::OrderOf:
                label = "AI order '" + behaviour(u.holder) + "' started by '" + name_of(u.trigger) + "'";
                break;
            case UsedBy::OrderTarget: {
                const WObj* b = object_by_uid(w, u.holder);
                std::string c = b ? names.label(b->cls) : "";
                label = std::string(c == "MADefend" ? "Defended" : c == "MAFollow" ? "Followed" : "Attacked") + " by " + name_of(u.unit) + (u.trigger ? ", started by '" + name_of(u.trigger) + "'" : "");
                break;
            }
        }
        ImGui::PushID(n++);
        if (ImGui::Selectable(label.c_str(), false) && target) { jump = target; jump_trigger = to_unit ? 0 : target; }
        ImGui::SetItemTooltip(to_unit ? "Select that unit." : "Open the trigger on the Objectives & Triggers tab.");
        ImGui::PopID();
    }
    if (jump) {
        if (jump_trigger) { trig_uid_ = jump_trigger; trig_mode_ = 0; tab_ = Tab::Triggers; }
        else if (const WObj* jo = object_by_uid(w, jump); jo && (names.label(jo->cls) == "MoeEntitySet" || names.label(jo->cls) == "MASwapSet")) {
            set_uid_ = jump;
            trig_mode_ = 3;
            tab_ = Tab::Triggers;
        }
        select_uid(jump);
    }
}

// ----------------------------------------------------------------------------- the orders chooser

namespace {
struct OrderKind { const char* label; const char* cls; const char* tip; bool path; };
const OrderKind kOrderKinds[] = {
    {"Walk a path once", "MAPatrol", "The unit walks the route you click once, then stands at its last point.", true},
    {"Patrol a path (loops)", "MAPatrol", "The unit walks the route you click again and again.", true},
    {"Defend a point", "MADefend", "The unit walks to the spot you click, then guards it: it fights what comes near and does not chase further than the radius from the spot. A round trigger is made at the spot to tell the unit it has arrived.", false},
    {"Follow the player", "MAFollow", "The unit stays close to the player.", false},
    {"Attack the player", "MAAttack", "The unit goes for the player and keeps attacking.", false},
    {"Attack a unit...", "MAAttack", "The unit goes for one unit you click on the map next.", false},
};
constexpr int kOrderKindCount = int(sizeof kOrderKinds / sizeof kOrderKinds[0]);
}  // namespace

void App::order_menu_items(uint32_t trig, uint32_t unit, bool turret) {
    for (int k = 0; k < kOrderKindCount; k++) {
        const bool off = turret && kOrderKinds[k].path;  // a turret cannot walk
        ImGui::BeginDisabled(off);
        if (ImGui::MenuItem(kOrderKinds[k].label)) give_order(trig, unit, k);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", off ? "A turret cannot walk." : kOrderKinds[k].tip);
    }
}

// A button that opens the chooser. trig = the trigger the orders start with (0: a new trigger that starts as the mission starts).
void App::order_button(uint32_t trig, uint32_t unit, const char* label, const char* tip, bool full_width) {
    const WObj* u = doc_ ? object_by_uid(doc_->world, unit) : nullptr;
    const bool turret = u && gd_.names().label(u->cls) == "MATurret";
    ImGui::PushID("##ordbtn");
    if (ImGui::Button(label, ImVec2(full_width ? -1 : 0, 0)) || std::exchange(order_menu_open_, false)) ImGui::OpenPopup("##givekind");
    ImGui::SetItemTooltip("%s", tip);
    if (ImGui::BeginPopup("##givekind")) {
        order_menu_items(trig, unit, turret);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// The request is kept and handled after the panels (the world changes), by ui_neutral_orders().
void App::give_order(uint32_t trig, uint32_t unit, int kind) {
    order_req_ = {unit, trig, kind, true};
}

void App::set_unit_side_now(uint32_t unit, int nation) {
    if (!doc_ || !io_ || !object_by_uid(doc_->world, unit)) return;
    push_undo("side");
    io_->apply_unit_side(*doc_, unit, nation);
    doc_->dirty = true;
    objects_dirty_ = minimap_dirty_ = true;
    world_changed();
}

// Red line under an order that belongs to a Neutral unit (neutral units have no AI, so it never moves): returns the side chosen
// (6 player's side, 5 enemy) or -1. The caller applies it (set_unit_side_now) when its loops are done.
int App::ui_neutral_fix(uint32_t unit) {
    const WObj* u = doc_ && !doc_->multiplayer ? object_by_uid(doc_->world, unit) : nullptr;
    if (!u || !u->find("Nation") || u->i32("Nation") != 0) return -1;
    int pick = -1;
    const std::string nm = u->description().empty() ? std::string("This unit") : "\"" + u->description() + "\"";
    ImGui::PushID(int(unit) + 77000);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().error);
    ImGui::TextWrapped("%s is Neutral, so it ignores these orders (neutral units have no AI). Put it on:", nm.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Player's side##nfix")) pick = 6;
    ImGui::SameLine();
    if (ImGui::SmallButton("Enemy##nfix")) pick = 5;
    ImGui::SetItemTooltip("Enemies attack the player; units on the player's side fight with the player.");
    ImGui::PopID();
    return pick;
}

// After the panels: a pending order request (asks about the side first when the unit is Neutral), and its popup.
void App::ui_neutral_orders() {
    if (order_req_.valid) {
        const OrderReq rq = std::exchange(order_req_, OrderReq());
        const WObj* u = doc_ ? object_by_uid(doc_->world, rq.unit) : nullptr;
        if (u && u->find("Nation") && u->i32("Nation") == 0 && !doc_->multiplayer) neutral_ask_ = {rq.unit, rq.trig, rq.kind, true};
        else if (u) make_order(rq.trig, rq.unit, rq.kind);
    }
    if (neutral_ask_.open) {
        ImGui::OpenPopup("Neutral unit");
        neutral_ask_.open = false;
        neutral_ask_active_ = true;
    }
    if (!neutral_ask_active_) return;
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(440 * dpi_, 0), ImVec2(440 * dpi_, vp->WorkSize.y - 24 * dpi_));
    }
    if (theme::begin_modal("Neutral unit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Neutral units ignore orders: they have no AI.");
        ImGui::TextWrapped("Put it on:");
        ImGui::Spacing();
        int side = -1;
        if (ImGui::Button("Player's side##neutral", ImVec2(130 * dpi_, 0))) side = 6;
        ImGui::SameLine();
        if (ImGui::Button("Enemy##neutral", ImVec2(130 * dpi_, 0))) side = 5;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel##neutral", ImVec2(130 * dpi_, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (side >= 0 || cancel) {
            ImGui::CloseCurrentPopup();
            neutral_ask_active_ = false;
            if (side >= 0) make_order(neutral_ask_.trig, neutral_ask_.unit, neutral_ask_.kind, side);
            neutral_ask_ = NeutralAsk();
        }
        theme::end_modal();
    } else {
        neutral_ask_active_ = false;
    }
}

// The click on the unit that gets orders ("Pick a unit on the map" on a trigger): the chooser opens for it.
void App::pick_order_unit(float mx, float my) {
    Scenario& s = *doc_;
    const int h = pick_object(mx, my);
    const WObj* o = h >= 0 ? &s.world.objs[size_t(h)] : nullptr;
    static const uint32_t unit_classes[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"), key_of("MATurret"), key_of("MAPerson")};
    if (!o || std::find(std::begin(unit_classes), std::end(unit_classes), o->cls) == std::end(unit_classes)) {
        status_ = "Click a unit on the map. Right-click or " + kt(A_CANCEL) + " cancels.";
        status_time_ = 6;
        return;
    }
    const uint32_t unit = o->uid();
    const Tab back = pick_.back;
    const std::string nm = o->description().empty() ? std::string("the unit") : o->description();
    pick_ = PickTarget();
    tool_ = Tool::Select;
    tab_ = back;
    selection_ = {unit};
    order_menu_open_ = true;
    status_ = "Choose what " + nm + " does.";
    status_time_ = 6;
}

uint32_t App::make_defend_walk(uint32_t beh, float x, float z, const std::string& name) {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const WObj* bo = object_by_uid(w, beh);
    const uint32_t ev = bo ? bo->parent() : 0;
    const uint32_t unit = order_unit(w, ev);
    const WObj* uo = object_by_uid(w, unit);
    Vec3 up;
    if (!uo || !get_pos(*uo, up)) return set_marker_link(s, beh, "DefendUID", x, z, true, name);
    const float radius = bo->find("Radius") ? bo->f32("Radius", 60) : 60.f;
    const std::string who = uo->description().empty() ? std::string("unit") : uo->description();
    if ((up.x - x) * (up.x - x) + (up.z - z) * (up.z - z) < 14.f * 14.f) return set_marker_link(s, beh, "DefendUID", x, z, true, name);  // already there
    // The circle that notices the unit arriving (fires when a member of its set is inside, as the retail wave triggers do), then the guard order.
    TriggerShape sh;
    sh.box = false;
    sh.x = x;
    sh.z = z;
    sh.radius = 12;
    sh.name = who + " reaches its defend point";
    const uint32_t circ = add_trigger_shape(s, sh);
    const uint32_t set = circ ? entity_set_from(who + " walking to its defend point", {unit}) : 0;
    if (!circ || !set) return set_marker_link(s, beh, "DefendUID", x, z, true, name);
    if (WObj* c = object_by_uid(w, circ)) {
        c->set_u32("RadiusSet", set);
        c->set_i32("TypeOfCheck", 2);
        c->set_i32("PlayerEntitesOnly", 0);
        c->set_i32("NationInverse", 1);
        c->set_i32("NationCompare", 11);
    }
    const uint32_t ev2 = add_order(s, circ, unit, "MADefend");
    const uint32_t beh2 = ev2 ? order_behaviour(w, ev2) : 0;
    uint32_t mk = beh2 ? set_marker_link(s, beh2, "DefendUID", x, z, true, name) : 0;
    if (!mk) return set_marker_link(s, beh, "DefendUID", x, z, true, name);
    if (WObj* b2 = object_by_uid(w, beh2)) b2->set_f32("Radius", radius);
    // The first order walks there once: from where the unit stands to the spot.
    set_order_kind(s, ev, "MAPatrol");
    if (uint32_t g = add_ai_path(s, {{up.x, up.z}, {x, z}}, who + " - walk to its defend point")) {
        if (WObj* go = object_by_uid(w, g)) go->set_i32("TraverseType", 1);
        wedit::reparent(w, g, ev);
    }
    return mk;
}

// Gives `unit` an order of chooser kind `kind`, started by `trig` (0 = a new "Orders: <unit>" trigger that fires as the mission starts,
// opened). side >= 0 first puts a Neutral unit on that side (one undo step). Path kinds arm the path tool; the others do not.
uint32_t App::make_order(uint32_t trig, uint32_t unit, int kind, int side) {
    if (!doc_ || !io_ || kind < 0 || kind >= kOrderKindCount) return 0;
    const WObj* u0 = object_by_uid(doc_->world, unit);
    if (!u0) return 0;
    const std::string who = u0->description().empty() ? std::string("unit") : u0->description();  // read before the new trigger moves the objects
    const OrderKind& k = kOrderKinds[kind];
    push_undo("unit orders");
    if (side >= 0) io_->apply_unit_side(*doc_, unit, side);
    // A reinforcement (a unit in a spawner a trigger switches on) starts its orders when it arrives, not at the mission start.
    const uint32_t arrival = trig == 0 ? unit_arrival_trigger(unit) : 0;
    const bool fresh = trig == 0;
    if (arrival) trig = arrival;
    else if (fresh) trig = add_trigger_object(*doc_, "MATriggerTimer", "Orders: " + who);  // fires as the mission starts
    const uint32_t ev = trig ? add_order(*doc_, trig, unit, k.cls) : 0;
    if (!ev) return 0;
    const uint32_t beh = order_behaviour(doc_->world, ev);
    if (kind == 0) {  // walk once: the route exists (empty) with TraverseType 1 so the points clicked next are walked once
        if (uint32_t g = add_ai_path(*doc_, {}, who + " - walk path")) {
            if (WObj* go = object_by_uid(doc_->world, g)) go->set_i32("TraverseType", 1);
            wedit::reparent(doc_->world, g, ev);
        }
    }
    if (fresh) {
        trig_uid_ = trig;  // open the new trigger on the Triggers view (not the last-used Sets / AI orders view)
        trig_mode_ = 0;
        tab_ = Tab::Triggers;
        selection_ = {trig};
    }
    pick_ = PickTarget();
    tool_ = Tool::Select;
    const std::string when = arrival ? " when it arrives" : fresh ? " when the mission starts" : "";
    if (k.path) {
        pick_.kind = PickTarget::OrderPath;
        pick_.uid = ev;
        tool_ = Tool::PlaceWaypoint;
        status_ = who + (kind == 0 ? " walks the route once" : " patrols") + when + ": click the map once per point, right-click when done.";
    } else if (kind == 2) {
        pick_.kind = PickTarget::LinkMarker;
        pick_.uid = beh;
        pick_.key = "DefendUID";
        pick_.null_marker = true;
        pick_.chain = true;  // the unit walks to the spot first, then guards it
        pick_.name = (fresh ? "Orders: " + who : std::string("Orders")) + " - defend point";
        tool_ = Tool::PickPoint;
        status_ = who + " defends a point" + when + ": click the map where it should go and guard.";
    } else if (kind == 5) {
        pick_.kind = PickTarget::LinkUnit;
        pick_.uid = beh;
        pick_.key = "AttackUID";
        tool_ = Tool::PickPoint;
        status_ = who + " attacks a unit" + when + ": click that unit on the map.";
    } else {
        status_ = who + (kind == 3 ? " follows the player" : " attacks the player") + when + ". Change the order or add a condition in the trigger.";
    }
    status_time_ = 10;
    world_changed();
    doc_->dirty = true;
    return trig;
}

void App::ui_unit_orders(uint32_t uid) {
    if (!doc_ || doc_->multiplayer) return;
    const Wdf& w = doc_->world;
    const auto& names = gd_.names();
    ImGui::SeparatorText("Starting orders");
    uint32_t open_trigger = 0;
    int n = 0;
    for (auto& g : w.objs) {
        if (g.parent() != uid || names.label(g.cls) != "MAAIGroup") continue;
        for (auto& ev : w.objs) {
            if (ev.parent() != g.uid()) continue;
            const WObj* b = object_by_uid(w, order_behaviour(w, ev.uid()));
            if (!b) continue;
            std::string c = names.label(b->cls), what;
            if (c == "MAPatrol") {
                uint32_t p = order_path(w, ev.uid());
                int k = p ? ai_path_length(*doc_, p) : 0;
                const WObj* po = p ? object_by_uid(w, p) : nullptr;
                what = std::string(po && po->i32("TraverseType") == 1 ? "Walk a path once" : "Patrol along a path") + " (" + std::to_string(k) + (k == 1 ? " point" : " points") + (k < 2 ? ", needs 2 or more)" : ")");
            } else if (c == "MADefend") {
                what = b->u32("DefendUID") ? "Defend a spot" : "Defend a spot (no spot picked yet)";
            } else if (c == "MAFollow") {
                uint32_t f = b->u32("FollowUID");
                what = "Follow " + (!f ? std::string("(no one picked)") : f == player_start_uid(w) ? std::string("the player") : unit_label(w, f));
            } else if (c == "MAAttack") {
                what = "Attack " + (b->u32("AttackPlayer") == 2 ? std::string("the player") : unit_label(w, b->u32("AttackUID")));
            } else {
                continue;
            }
            uint32_t t = ev.u32("TriggerID");
            const WObj* tr = object_by_uid(w, t);
            std::string when;
            if (t && t == unit_arrival_trigger(uid))
                when = "when it arrives";
            else if (tr && names.label(tr->cls) == "MATriggerTimer" && tr->f32("TrgTimerDurationSeconds") == 0 && tr->f32("TrgTimerDuration") == 0)
                when = "when the mission starts";
            else
                when = "when '" + (tr ? unit_label(w, t) : std::string("(no trigger)")) + "' fires";
            ImGui::PushID(int(ev.uid()));
            if (ImGui::SmallButton("Edit") && tr) open_trigger = t;
            ImGui::SetItemTooltip("Open these orders on the Objectives & Triggers tab (change the order, its path or spot, or when it starts).");
            ImGui::SameLine();
            ImGui::TextWrapped("%s, %s", what.c_str(), when.c_str());
            ImGui::PopID();
            n++;
        }
    }
    // Orders on a Neutral unit do nothing (no AI): a red line with the two fixes.
    if (n) if (int side = ui_neutral_fix(uid); side >= 0) { set_unit_side_now(uid, side); return; }
    if (!n) {
        // An AI-off unit stays asleep: it neither moves nor fights until a trigger wakes it (retail M01b tanks; game-tested).
        const WObj* u = object_by_uid(w, uid);
        if (u && u->find("Nation") && u->i32("Nation") == 0)
            ImGui::TextWrapped("None. A Neutral unit has no AI and stands still: giving it orders offers to put it on a side first.");
        else if (u && u->find("m_bAIEnabled") && !u->i32("m_bAIEnabled"))
            ImGui::TextWrapped("None, and its AI is off: it stays still and does not fight. Tick 'AI starts on' above, or give it orders (they switch its AI on).");
        else
            ImGui::TextDisabled("None: it stands and fights whatever comes close.");
    }
    ImGui::BeginDisabled(doc_->read_only);
    order_button(0, uid, n ? "Give it more orders..." : "Give this unit orders...",
                 "Pick what the unit does: walk a path once, patrol, defend a point, follow or attack the player, or attack a unit. "
                 "It makes a trigger that starts as the mission starts (for a reinforcement: when it arrives); on the Objectives & Triggers tab you can make it wait for a condition instead.", true);
    ImGui::EndDisabled();
    if (open_trigger) { trig_uid_ = open_trigger; trig_mode_ = 0; tab_ = Tab::Triggers; select_uid(open_trigger); }
}

// ----------------------------------------------------------------------------- the tab

// A 'Complete objective' action just pointed at an objective with no radar target (run next frame: it adds objects).

void App::tab_triggers() {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const bool ro = s.read_only;
    // The player gets a radar / HUD target as retail does: a Destroyed trigger's units, or an invisible NULL marker at a
    // circle or box (Mission_03 'Dummy - cp 1 objective', as 'Reach a place' makes). Other triggers have no place to show.
    if (mark_trigger_ && mark_objective_ >= 0 && mark_objective_ < int(s.objectives.size()) && objective_targets(w, mark_objective_).empty()) {
        static const uint32_t K_DESTR = key_of("MATriggerDestroyed"), K_RAD = key_of("MATriggerRadius"), K_VOL = key_of("MATriggerCuboidalVolume");
        int n = 0;
        if (const WObj* tg = object_by_uid(w, mark_trigger_)) {
            const uint32_t cls = tg->cls;
            const std::string name = tg->description();
            Vec3 p;
            const bool at = get_pos(*tg, p);
            if (cls == K_DESTR) {
                std::vector<uint32_t> units;
                for (uint32_t r : reference_children(w, mark_trigger_))
                    if (const WObj* ro_ = object_by_uid(w, r)) if (uint32_t u = ro_->u32("ReferenceUID")) units.push_back(u);
                for (uint32_t u : units) n += add_objective_target(s, u, mark_objective_) ? 1 : 0;
            } else if ((cls == K_RAD || cls == K_VOL) && at) {
                uint32_t m = add_ai_marker(s, p.x, p.z, name + kReachMarkerSuffix, true);
                if (WObj* mo = m ? object_by_uid(w, m) : nullptr)
                    if (mo->find("IsCollidable")) mo->set_i32("IsCollidable", 0);
                if (m && add_objective_target(s, m, mark_objective_)) n++;
            }
        }
        if (n) {
            world_changed();
            s.dirty = true;
            status_ = "Objective " + std::to_string(mark_objective_ + 1) + ": " + std::to_string(n) + " radar target(s) added from this trigger (Objectives list: Radar targets).";
            status_time_ = 6;
        }
    }
    mark_trigger_ = 0;
    mark_objective_ = -1;
    const auto& names = gd_.names();
    WTree t;
    t.build(w);

    // 1 = trigger, 2 = action, per object.
    std::vector<char> kind = trigger_kinds(w, names);
    auto cls_name = [&](const WObj& o) { return names.label(o.cls); };
    auto kind_label = [&](const WObj& o) -> std::string { return trig_kind_label(cls_name(o)); };
    // Rows: every trigger, nested ones indented under the trigger that holds them; a cutscene's own start / end triggers and cue
    // timers are left out (they are edited on the timeline) and the level's own ones are marked.
    const TrigList tl = trigger_list(t, kind, trig_uid_);
    const std::vector<TrigRow>& rows = tl.rows;
    const int cut_hidden = tl.cutscene_hidden;

    // The map selection leads: a trigger, or something inside one, selected on the map opens here.
    static uint32_t last_sel = 0;
    if (selection_.size() == 1) {
        uint32_t su = *selection_.begin();
        if (su != last_sel) {
            last_sel = su;
            for (int p = t.find(su); p >= 0; p = t.parent_of[size_t(p)])
                if (kind[size_t(p)] == 1) { trig_uid_ = w.objs[size_t(p)].uid(); break; }
        }
    } else {
        last_sel = 0;
    }

    std::function<void()> later;  // structural edits wait until nothing refers to the object list any more
    auto touch = [&](const char* label) {
        push_undo_once(label);
        s.dirty = true;
        objects_dirty_ = true;
    };
    struct Dis { explicit Dis(bool d) { ImGui::BeginDisabled(d); } ~Dis() { ImGui::EndDisabled(); } };

    // ---- field widgets: the shared editors of app_actions.cpp
    auto f_float = [&](WObj& o, const char* key, const char* label, const char* tip = nullptr) { ui_float(o, key, label, tip); };
    auto f_int = [&](WObj& o, const char* key, const char* label, const char* tip = nullptr) { ui_int(o, key, label, tip); };
    auto f_bool = [&](WObj& o, const char* key, const char* label, const char* tip = nullptr) { ui_bool(o, key, label, tip); };
    auto f_enum = [&](WObj& o, const char* key, const char* label, std::vector<std::pair<int, const char*>> opts) { ui_enum(o, key, label, std::move(opts)); };
    auto f_link = [&](WObj& o, const char* key, const char* label, bool triggers = false, const char* set_key = nullptr) { ui_link(o, key, label, triggers, set_key, later); };
    auto go_to = [&](uint32_t uid) { go_to_object(uid); };
    auto trigger_name = [&](const WObj& x) { return x.description().empty() ? kind_label(x) : x.description(); };
    // A unit or building a Destroyed trigger can watch: not the player's start point, a trigger shape or another trigger.
    auto watchable = [&](uint32_t uid) {
        const WObj* o = object_by_uid(w, uid);
        return o && o->find("CorporalPos") && !is_trigger_shape(o->cls) && uid != player_start_uid(w) && cls_name(*o).rfind("MATrigger", 0) != 0;
    };
    // Duplicate / Delete, for the buttons and the row menu: the work runs after the lists are drawn (later / confirm).
    auto do_duplicate = [&](uint32_t src) {
        if (!io_ || !src) return;
        later = [this, src]() {
            push_undo("duplicate trigger");
            if (uint32_t u = io_->duplicate_subtree(*doc_, src, 12.f)) {
                if (WObj* o = object_by_uid(doc_->world, u)) {
                    const WObj* so = object_by_uid(doc_->world, src);
                    std::string nm = so && !so->description().empty() ? so->description() : trig_kind_label(gd_.names().label(o->cls));
                    o->set_str("Description", nm + " copy");  // the copy is named, and its Name box takes the keyboard
                }
                trig_uid_ = u;
                trig_name_focus_ = true;
                g_show_trig_editor = true;
                select_uid(u);
            }
            world_changed();
        };
    };
    auto do_delete = [&](uint32_t victim) {
        const WObj* vo = object_by_uid(w, victim);
        int orders = int(orders_of_trigger(w, victim).size()), users = 0;
        for (auto& o : w.objs)
            for (const char* k : {"ReferenceUID", "ActionEntity"}) users += o.find(k) && o.u32(k) == victim;
        std::string q = "Delete the trigger \"" + (vo ? trigger_name(*vo) : std::string()) + "\" with its actions?";
        if (orders) q += "\n\nThe " + std::to_string(orders) + " AI order(s) it starts are deleted too.";
        if (users) q += "\n\n" + std::to_string(users) + " other trigger(s) or action(s) point at it and will stop working.";
        q += "\n\nYou can undo this with " + kt(A_UNDO) + ".";
        confirm(q, [this, victim]() {
            if (!doc_) return;
            push_undo("delete trigger");
            for (uint32_t ev : orders_of_trigger(doc_->world, victim)) wedit::remove_subtree(doc_->world, ev);
            wedit::remove_subtree(doc_->world, victim);
            selection_.erase(victim);
            if (trig_uid_ == victim) trig_uid_ = 0;
            world_changed();
            doc_->dirty = true;
        });
    };

    // ---- header
    if (trig_mode_ == 1) trig_mode_ = 0;  // Cutscenes have their own tab now
    if (ImGui::RadioButton("Triggers##mode", trig_mode_ == 0)) trig_mode_ = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("AI orders", trig_mode_ == 2)) trig_mode_ = 2;
    ImGui::SetItemTooltip("Every AI order of the level, listed under the unit it is given to.");
    ImGui::SameLine();
    if (ImGui::RadioButton("Sets", trig_mode_ == 3)) trig_mode_ = 3;
    ImGui::SetItemTooltip("Named lists of units that triggers act on together (teams, a cutscene's cast), and swap sets.");
    if (trig_mode_ == 3) { tab_sets(); return; }
    if (trig_mode_ == 2) { tab_orders_by_unit(); return; }
    if (ro) ImGui::TextWrapped("This is a real level, so it is read-only here: look at its triggers, or make your own copy (the button above) to change them.");
    else ImGui::TextWrapped("A trigger waits for its condition (\"when the player reaches this circle\"), then runs its actions in order "
                            "(\"show a message, play a sound, complete an objective\") and any AI orders it starts. These are the level's own objects.");
    if (!ro && s.multiplayer) {
        ImGui::PushTextWrapPos(0);
        hint_text("Most triggers of a multiplayer map run the game's own rules (setup, game-over timer, scoring). Changing them can stop a match from ending. Add your own with New trigger.");
        ImGui::PopTextWrapPos();
    }
    if (!ro && ImGui::TreeNode("How do I make something happen on arrival?")) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("1. New trigger... > 'Player enters a box' (or 'Player enters a circle'), then click the map to put it down; drag its handles to size it.");
        ImGui::TextUnformatted("2. Under 'Then do', use 'Add action...' to choose what should happen: show a message, play a sound, play a cutscene, complete an objective...");
        ImGui::TextUnformatted(s.multiplayer ? "3. On a multiplayer map, start from a condition (a circle, a delay, the match clock) and add what the game's own maps use: play a sound, move the player to a marker, change sides. Campaign jobs are under Advanced."
                                             : "3. Common jobs are ready-made: New trigger... > Templates (win when units are destroyed, reinforcements arrive, play a cutscene). They do steps 1-2 for you.");
        ImGui::TextUnformatted(("4. Press Test (" + kt(A_TEST) + "). The trigger fires once when the player first enters the box; tick Repeat to make it fire every time.").c_str());
        ImGui::PopTextWrapPos();
        ImGui::TreePop();
    }
    {
        Dis d(ro);
        if (ImGui::Button("New trigger...")) ImGui::OpenPopup("newtrig");
        ImGui::SetItemTooltip("Add a trigger: pick what it waits for.");
        if (ImGui::BeginPopup("newtrig")) {
            auto name_for = [&](const char* base) {
                for (int k = 1;; k++) {
                    std::string n = std::string(base) + (k > 1 ? " " + std::to_string(k) : "");
                    bool used = false;
                    for (auto& r : rows) used |= w.objs[size_t(r.idx)].description() == n;
                    if (!used) return n;
                }
            };
            auto made = [&](uint32_t u) {
                if (!u) return;
                g_show_trig_editor = true;
                trig_name_focus_ = true;  // the Name box takes the keyboard, text selected
                trig_uid_ = u;
                select_uid(u);
                world_changed();
                s.dirty = true;
            };
            // ---- Templates: the common jobs in one step, built from the game's own trigger objects.
            ImGui::SeparatorText("Templates");
            std::vector<uint32_t> picked;
            for (uint32_t x : selection_) if (const WObj* o = object_by_uid(w, x)) if (!is_trigger_shape(o->cls)) picked.push_back(x);
            auto place_shape = [&](uint32_t u) {  // click the map to put the new area down
                pick_ = PickTarget();
                pick_.kind = PickTarget::MoveShape;
                pick_.uid = u;
                tool_ = Tool::PickPoint;
            };
            auto set_link = [&](uint32_t act, const char* key, uint32_t target, const char* flag, int on) {
                if (WObj* a = object_by_uid(w, act)) { a->set_u32(key, target); if (flag) a->set_i32(flag, on); }
            };
            // Multiplayer maps: the game type sets the goal, and the retail MP worlds place none of the classes these
            // templates build (win/lose, messages, AI ambushes and reinforcements, checkpoints, cutscenes), so they sit
            // under Advanced there (research census of the 16 retail MP worlds).
            const bool mission = !s.multiplayer;
            // The one template MP maps keep at the top level (freeze fix M: it was hidden under Advanced since daa80ea).
            auto message_template = [&]() {
                if (ImGui::MenuItem(mission ? "Show a message 30 seconds into the mission" : "Show a message 30 seconds into the match")) {
                    push_undo("new trigger");
                    uint32_t u = add_trigger_object(s, "MATriggerTimer", name_for("Message after 30 s"));
                    if (WObj* o = object_by_uid(w, u)) { o->set_f32("TrgTimerDurationSeconds", 30); o->set_f32("TrgTimerDuration", 30.f / 60.f); }
                    add_action_object(s, u, "MAActionBroadcast", "Show a message");
                    made(u);
                    status_ = "Type the message under 'Then do', and change the seconds under 'When'.";
                    status_time_ = 8;
                }
                ImGui::SetItemTooltip("A timer that shows an on-screen message after 30 seconds; change the time and the text below.");
            };
            auto campaign_templates = [&]() {
                ImGui::BeginDisabled(picked.empty());
                if (ImGui::MenuItem(picked.empty() ? "Win when these units are destroyed (select units first)###tmpl_win" : "Win when these units are destroyed###tmpl_win")) {
                    push_undo("new trigger");
                    uint32_t u = add_trigger_object(s, "MATriggerDestroyed", name_for("Win when destroyed"));
                    for (uint32_t x : picked) add_reference(s, u, x);
                    add_action_object(s, u, "MAActionGameCondition", "Win the mission");
                    made(u);
                    status_ = "Done: the mission is won when the " + std::to_string(picked.size()) + " selected unit(s) are destroyed.";
                    status_time_ = 8;
                }
                tip_always(picked.empty() ? "Select the units or buildings on the map first (they are what must be destroyed)."
                                                     : "Watches the selected units; when they are all destroyed the player wins.");
                if (ImGui::MenuItem(picked.empty() ? "Reinforcements arrive when the player enters a circle (select units first)###tmpl_reinf" : "Reinforcements arrive when the player enters a circle###tmpl_reinf")) {
                    push_undo("new trigger");
                    // Retail pattern (Mission_05): the units wait hidden with their AI asleep, the trigger shows them and wakes them up.
                    uint32_t wait = 0;  // made only for units not already hidden at start (Properties > Starts hidden)
                    TriggerShape sh;
                    sh.box = false;
                    sh.x = cam_.tx;
                    sh.z = cam_.tz;
                    sh.name = name_for("Reinforcements arrive");
                    uint32_t u = add_trigger_shape(s, sh);
                    for (uint32_t x : picked) {
                        if (WObj* o = object_by_uid(w, x)) o->set_i32("m_bAIEnabled", 0);
                        bool hidden = false;
                        static const uint32_t K_SHOW = key_of("MAActionShow");
                        for (auto& o : w.objs) hidden |= o.cls == K_SHOW && o.u32("ActionEntity") == x && !o.i32("Enable");
                        if (!hidden) {
                            if (!wait) wait = add_trigger_object(s, "MATriggerTimer", name_for("Reinforcements wait hidden"));
                            set_link(add_action_object(s, wait, "MAActionShow", "Hide until called"), "ActionEntity", x, "Enable", 0);
                        }
                        if (u) {
                            uint32_t show = add_action_object(s, u, "MAActionShow", "Show the reinforcements");
                            set_link(show, "ActionEntity", x, "Enable", 1);
                            if (WObj* a = object_by_uid(w, show)) a->set_i32("EnableTree", 1);
                            set_link(add_action_object(s, u, "MAActionAI", "Wake them up"), "ActionEntity", x, "EnableAI", 1);
                        }
                    }
                    made(u);
                    if (u) place_shape(u);
                    status_ = "Click the map where the player should be when the reinforcements appear.";
                    status_time_ = 4;
                }
                tip_always(picked.empty() ? "Place the reinforcement units first and select them; they stay hidden until the player enters the circle."
                                                     : "The selected units stay hidden and asleep until the player enters the circle, then appear and fight.");
                if (ImGui::MenuItem(picked.empty() ? "Enemies attack when the player comes close (select units first)###tmpl_attack" : "Enemies attack when the player comes close###tmpl_attack")) {
                    push_undo("new trigger");
                    // Retail pattern (area reveal / ambush): the units stand asleep until the player enters the circle, then their AI wakes.
                    TriggerShape sh;
                    sh.box = false;
                    sh.x = cam_.tx;
                    sh.z = cam_.tz;
                    sh.radius = 100;
                    sh.name = name_for("Ambush");
                    uint32_t u = add_trigger_shape(s, sh);
                    // Asleep units do not react to being shot, so they also wake as soon as any of them is hit.
                    uint32_t hit = add_trigger_object(s, "MATriggerDestroyed", name_for("Ambush: wake when hit"));
                    if (WObj* o = object_by_uid(w, hit)) { o->set_i32("CombineMethod", 1); o->set_u32("DamageLevel", 99); }  // 99: the first hit (retail uses 99 in 60 of 451 Destroyed triggers)
                    for (uint32_t x : picked) {
                        if (WObj* o = object_by_uid(w, x)) o->set_i32("m_bAIEnabled", 0);
                        if (u) set_link(add_action_object(s, u, "MAActionAI", "Wake them up"), "ActionEntity", x, "EnableAI", 1);
                        if (hit) {
                            add_reference(s, hit, x);
                            set_link(add_action_object(s, hit, "MAActionAI", "Wake them up"), "ActionEntity", x, "EnableAI", 1);
                        }
                    }
                    made(u);
                    if (u) place_shape(u);
                    status_ = "Click the map where the player sets off the ambush (a 100 m circle). They also wake when one of them is hit.";
                    status_time_ = 10;
                }
                ImGui::SetItemTooltip(picked.empty() ? "Select the enemy units first; they wait without moving until the player comes within the circle."
                                                     : "The selected units wait asleep until the player enters the circle (or one of them is hit), then they attack.");
                ImGui::EndDisabled();
                if (mission) message_template();
                if (ImGui::MenuItem("Save a checkpoint when the player enters a circle")) {
                    push_undo("new trigger");
                    TriggerShape sh;
                    sh.box = false;
                    sh.x = cam_.tx;
                    sh.z = cam_.tz;
                    sh.name = name_for("Checkpoint");
                    uint32_t u = add_trigger_shape(s, sh);
                    if (u) add_action_object(s, u, "MAActionCheckPointSave", "Save a checkpoint");
                    made(u);
                    if (u) place_shape(u);
                    status_ = "Click the map where the checkpoint should be.";
                    status_time_ = 8;
                }
                ImGui::SetItemTooltip("Saves a checkpoint when the player reaches this circle (as the game's missions do). Restarting from it after a death is not yet confirmed in this game build.");
                // No "lose if the player is destroyed" template: a Destroyed trigger with IncludePlayers and no watched units
                // fires at load (Lose at once -> the mission restarts forever). The game ends a mission on player death itself.
                if (ImGui::MenuItem("Countdown: lose when the time runs out")) {
                    push_undo("new trigger");
                    uint32_t u = add_trigger_object(s, "MATriggerTimer", name_for("Countdown"));
                    if (WObj* o = object_by_uid(w, u)) {
                        o->set_f32("TrgTimerDurationSeconds", 300);
                        o->set_f32("TrgTimerDuration", 5.f);
                        o->set_i32("TrgTimerDisplayOnHUD", 1);
                    }
                    if (uint32_t a = add_action_object(s, u, "MAActionGameCondition", "Lose the mission")) if (WObj* o = object_by_uid(w, a)) o->set_i32("GameCondition", 1);
                    made(u);
                    status_ = "5 minutes on the HUD clock. To stop it when the player succeeds, add 'Switch units or triggers on or off' (off) to that trigger and pick this one.";
                    status_time_ = 12;
                }
                ImGui::SetItemTooltip("A 5-minute clock shown on the HUD; when it reaches zero the mission is lost. Switch it off from another trigger to stop it.");
                if (ImGui::MenuItem("Play the intro cutscene when the mission starts")) {
                    push_undo("new trigger");
                    uint32_t u = add_trigger_object(s, "MATriggerTimer", name_for("Intro cutscene"));
                    // 0 s (1 s showed the mission first; the old note is rechecked by's game run, 0.1 s if it fails).
                    if (WObj* o = object_by_uid(w, u)) { o->set_f32("TrgTimerDurationSeconds", 0); o->set_f32("TrgTimerDuration", 0); }
                    uint32_t act = add_action_object(s, u, "MAActionActivateCinema", "Play the intro");
                    made(u);
                    later = [this, act]() {
                        if (uint32_t c = new_cutscene(true, false)) if (WObj* o = object_by_uid(doc_->world, act)) o->set_u32("cinemaID", c);
                    };
                }
                ImGui::SetItemTooltip("A trigger that plays a cutscene as soon as the mission starts; a new cutscene is built over the player start for you to shape.");
                if (ImGui::MenuItem("Play a cutscene when the player enters a box")) {
                    // One step for the common case: a box on the map that starts a cutscene.
                    push_undo("new trigger");
                    TriggerShape sh;
                    sh.box = true;
                    sh.x = cam_.tx;
                    sh.z = cam_.tz;
                    sh.name = name_for("Cutscene box");
                    uint32_t u = add_trigger_shape(s, sh);
                    made(u);
                    if (u) {
                        add_action_object(s, u, "MAActionActivateCinema", "Play a cutscene");
                        world_changed();
                        pick_ = PickTarget();
                        pick_.kind = PickTarget::MoveShape;
                        pick_.uid = u;
                        tool_ = Tool::PickPoint;
                        status_ = "Click the map to put the box down (away from the player start), then choose the cutscene below (or press New cutscene).";
                        status_time_ = 10;
                    }
                }
                ImGui::SetItemTooltip("Adds a trigger box and a 'Play a cutscene' action in one go. Place the box away from the player start (a box around the start fires as the mission starts and the cutscene is lost), then pick or build the cutscene in the action below.");
            };
            if (mission) campaign_templates();
            else {
                ImGui::TextDisabled("Multiplayer: the game type decides who wins.");
                message_template();
                if (ImGui::BeginMenu("Advanced: campaign templates")) {
                    ImGui::TextDisabled("The game's multiplayer maps use none of these.");
                    campaign_templates();
                    ImGui::EndMenu();
                }
            }
            ImGui::SeparatorText("Start from a condition");
            if (ImGui::MenuItem(mission ? "When the mission starts" : "When the match starts")) {
                push_undo("new trigger");
                made(add_trigger_object(s, "MATriggerTimer", name_for(mission ? "Mission start" : "Match start")));
            }
            if (ImGui::MenuItem("After a delay")) {
                push_undo("new trigger");
                uint32_t u = add_trigger_object(s, "MATriggerTimer", name_for("Timer"));
                if (WObj* o = object_by_uid(w, u)) { o->set_f32("TrgTimerDurationSeconds", 10); o->set_f32("TrgTimerDuration", 10.f / 60.f); }
                made(u);
            }
            auto enters = [&](bool box) {
                if (!ImGui::MenuItem(box ? "Player enters a box (volume)" : "Player enters a circle")) return;
                push_undo("new trigger");
                TriggerShape sh;
                sh.box = box;
                sh.x = cam_.tx;
                sh.z = cam_.tz;
                sh.name = name_for(box ? "Trigger box" : "Trigger circle");
                uint32_t u = add_trigger_shape(s, sh);
                made(u);
                if (u) {  // click the map to put it down (right-click leaves it at the view centre)
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::MoveShape;
                    pick_.uid = u;
                    tool_ = Tool::PickPoint;
                }
            };
            auto logic = [&]() {
                if (ImGui::MenuItem("Several conditions together")) { push_undo("new trigger"); made(add_trigger_object(s, "MATriggerLogic", name_for("Combined trigger"))); }
            };
            enters(false);
            if (mission) enters(true);
            if (ImGui::MenuItem("Units are destroyed")) {
                push_undo("new trigger");
                uint32_t u = add_trigger_object(s, "MATriggerDestroyed", name_for("Units destroyed"));
                for (uint32_t x : selection_) if (x != u && watchable(x)) add_reference(s, u, x);  // units only, not the start point or trigger shapes
                made(u);
            }
            if (mission) logic();
            // Other kinds: on a multiplayer map only the kinds the retail MP worlds place; the rest go under Advanced.
            auto other_kinds = [&](int want) {  // 0 = all, 1 = used by retail MP, 2 = campaign-only
                // The kinds offered above (After a delay, Units are destroyed, Several conditions together) are not
                // repeated here, and the mission-specific "(advanced)" ones come last.
                for (int pass = 0; pass < 2; pass++)
                    for (auto& k : io_->part_kinds()) {
                        if (k.name.rfind("MATrigger", 0) != 0 || k.positioned) continue;
                        if (k.name == "MATriggerTimer" || k.name == "MATriggerDestroyed" || k.name == "MATriggerLogic") continue;
                        if (want && (want == 1) != mp_retail_class(k.name)) continue;
                        const char* n = lookup(kTriggerNames, std::size(kTriggerNames), k.name);
                        std::string label = n ? n : words_of(k.name, "MATrigger");
                        if ((label.find("(advanced)") != std::string::npos) != (pass == 1)) continue;
                        if (ImGui::MenuItem(label.c_str())) {
                            push_undo("new trigger");
                            uint32_t next = next_uid(w);
                            uint32_t grp = ensure_group(w, root_uid(w), kTriggerGroup, next);
                            uint32_t u = io_->add_part(s, k.cls, grp, nullptr);
                            // Named like the menu entry, not "New TriggerPlayerVehicleType".
                            if (WObj* o = object_by_uid(w, u)) o->set_str("Description", name_for(label.c_str()));
                            made(u);
                        }
                    }
            };
            if (io_ && ImGui::BeginMenu("Other kinds")) {
                other_kinds(mission ? 0 : 1);
                ImGui::EndMenu();
            }
            if (!mission && ImGui::BeginMenu("Advanced: campaign conditions")) {
                ImGui::TextDisabled("The game's multiplayer maps use none of these.");
                enters(true);
                logic();
                if (io_) other_kinds(2);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
    int ti = trig_uid_ ? t.find(trig_uid_) : -1;
    if (ti >= 0 && kind[size_t(ti)] != 1) ti = -1;
    {
        Dis d(ro || ti < 0);
        if (ImGui::Button("Duplicate")) do_duplicate(trig_uid_);
        ImGui::SetItemTooltip("Copy the selected trigger with its actions (12 m to the side if it has a place on the map).");
        ImGui::SameLine();
        if (theme::danger_button("Delete")) do_delete(trig_uid_);
        ImGui::SetItemTooltip("%s", ("Delete the selected trigger (asks first; " + kt(A_UNDO) + " undoes it).").c_str());
    }
    ImGui::SameLine();
    {
        Dis d(ti < 0);
        if (ImGui::Button("Show on map")) {
            show_helpers_ = true;
            show_helper_group_[HG_TRIGGERS] = true;
            objects_dirty_ = true;
            select_uid(trig_uid_);
            zoom_to_selection();
        }
        ImGui::SetItemTooltip("Select this trigger, switch the trigger areas on and move the camera to it (if it has a place on the map).");
    }

    // ---- list
    static std::string filter;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##tf", "Filter (name, kind, message...)", &filter);
    std::string lf = lower(filter);
    auto matches = [&](int idx) {
        if (lf.empty()) return true;
        std::vector<int> sub;
        t.subtree(idx, sub);
        for (int k : sub) {
            const WObj& o = w.objs[size_t(k)];
            if (lower(o.description() + " " + kind_label(o) + " " + cls_name(o)).find(lf) != std::string::npos) return true;
            if (kind[size_t(k)] == 2) {
                uint32_t id = o.find("BodyTextID") ? o.u32("BodyTextID") : 0;
                if (std::string* tx = id ? own_text(s, id) : nullptr) if (lower(*tx).find(lf) != std::string::npos) return true;
            }
        }
        return false;
    };
    std::unordered_map<uint32_t, int> order_count;
    {
        static const uint32_t K_EV = key_of("MAEvent");
        for (auto& o : w.objs) if (o.cls == K_EV && o.u32("TriggerID")) order_count[o.u32("TriggerID")]++;
    }
    // A timer that brings reinforcements in has no actions of its own; the row says what it does instead of "0 actions".
    std::unordered_map<uint32_t, int> brings_in;
    for (auto& o : w.objs)
        if (o.cls == key_of("MASpawner") && o.u32("TriggerEnableID")) brings_in[o.u32("TriggerEnableID")]++;
    // Two sections: the triggers you made, open; the level's own, folded. A row's nested triggers stay with it.
    std::vector<const TrigRow*> yours, lvl;
    for (auto& r : rows) {
        if (!matches(r.idx)) continue;
        (r.level ? lvl : yours).push_back(&r);
    }
    int n_yours = 0, n_level = 0;
    for (auto& r : rows) (r.level ? n_level : n_yours) += r.depth == 0;
    ImGui::TextDisabled("%d trigger%s of your own, %d from the level", n_yours, n_yours == 1 ? "" : "s", n_level);
    if (cut_hidden) {
        ImGui::TextDisabled("%d cutscene trigger%s: edit %s on the timeline.", cut_hidden, cut_hidden == 1 ? "" : "s", cut_hidden == 1 ? "it" : "them");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cameras & Cutscenes")) tab_ = Tab::Cameras;
        ImGui::SetItemTooltip("The triggers that mark where a cutscene shot starts and ends, and the timers of its events, are shown with their cutscene.");
    }
    // As tall as its rows (3 at least; 10 at most, so about 8 of your own triggers show without scrolling), with room for the two headings.
    const int want_rows = int(yours.size()) + 2 + (trig_level_open_ ? int(lvl.size()) : 0);
    float list_h = (float(std::clamp(want_rows, 3, ti >= 0 ? 10 : 14)) + 0.6f) * ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##tlist", ImVec2(0, list_h), ImGuiChildFlags_Borders);  // no ResizeY: a stored size kept the first (3-row) height
    // Retail names carry the designers' kind prefix ("TT - Setup", "TGT - GameOver", "TR Player in Uziel"): the list
    // shows the rest ("Setup") next to the plain kind; the full name is on hover and in the Name box below. Only the
    // codes the retail levels use (see the prefix glossary), any case, then " - ", "_" or a space; action names
    // are kept verbatim (they quote trigger names).
    auto friendly = [](const std::string& d) {
        static const char* const kCodes[] = {"TPVT", "TGT", "TMA", "TUB", "TNR", "TSB", "TT", "TD", "TR", "TG", "TL", "TB", "TA", "TC"};
        for (const char* c : kCodes) {
            size_t n = strlen(c), j = n;
            if (d.size() <= n || _strnicmp(d.c_str(), c, n) != 0) continue;
            // the whole separator run: retail has doubled ones ("TG - - VO" -> "VO")
            while (j < d.size() && (std::isspace(uint8_t(d[j])) || d[j] == '-' || d[j] == '_')) j++;
            if (j > n && j < d.size()) return d.substr(j);
        }
        return d;
    };
    int shown = 0;
    auto draw_rows = [&](const std::vector<const TrigRow*>& list) {
        for (const TrigRow* rp : list) {
            const TrigRow& r = *rp;
            if (++shown > 400) { ImGui::TextDisabled("More triggers than shown: use the filter."); break; }
            const WObj& o = w.objs[size_t(r.idx)];
            int nact = 0;
            for (int k : t.kids[size_t(r.idx)]) nact += kind[size_t(k)] == 2;
            std::string nm = o.description().empty() ? trigger_name(o) : friendly(o.description());
            const std::string lead = std::string(r.depth * 2, ' ') + (o.find("TriggerEnabled") && !o.i32("TriggerEnabled") ? "(asleep) " : "");
            const std::string kind = o.description().empty() ? std::string() : "   [" + kind_label(o) + "]";
            int nord = order_count.count(o.uid()) ? order_count[o.uid()] : 0;
            const int nbring = brings_in.count(o.uid()) ? brings_in[o.uid()] : 0;
            std::string tail = std::string(o.i32("TriggerAutoReset") ? "  [repeats]" : "") + "  - ";
            if (nact == 0 && nbring) tail += "brings in " + std::to_string(nbring) + " unit" + (nbring == 1 ? "" : "s");
            else tail += std::to_string(nact) + " action" + (nact == 1 ? "" : "s") + (nord ? ", " + std::to_string(nord) + " order" + (nord == 1 ? "" : "s") : "");
            // When the row is too long the kind goes first, then the end of the name; what it does stays readable.
            std::string l = lead + nm + kind + tail;
            std::string full = l;
            const float room = ImGui::GetContentRegionAvail().x;
            if (ImGui::CalcTextSize(l.c_str()).x > room) l = lead + nm + tail;
            if (ImGui::CalcTextSize(l.c_str()).x > room) {
                std::string n2 = nm;
                while (n2.size() > 4 && ImGui::CalcTextSize((lead + n2 + "..." + tail).c_str()).x > room) n2.pop_back();
                l = lead + n2 + "..." + tail;
            }
            const bool cut = l != full;
            ImGui::PushID(r.idx);
            // A trigger just made (or picked on the map) scrolls into view.
            static uint32_t scrolled_to = 0;
            if (o.uid() == trig_uid_ && scrolled_to != trig_uid_) { ImGui::SetScrollHereY(0.5f); scrolled_to = trig_uid_; }
            if (ImGui::Selectable(l.c_str(), o.uid() == trig_uid_)) {
                trig_scroll_to_ = true;  // bring its editor into view below
                trig_uid_ = o.uid();
                select_uid(o.uid());
                tool_ = tool_ == Tool::PickPoint || tool_ == Tool::PlaceWaypoint ? Tool::Select : tool_;
                pick_ = PickTarget();
            }
            // Right-click: the same row picks itself, then its menu.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && trig_uid_ != o.uid()) { trig_uid_ = o.uid(); select_uid(o.uid()); }
            if (ImGui::BeginPopupContextItem("##trow")) {
                const uint32_t ru = o.uid();
                ImGui::BeginDisabled(ro);
                if (ImGui::MenuItem("Rename")) { trig_uid_ = ru; trig_name_focus_ = true; g_show_trig_editor = true; }
                if (ImGui::MenuItem("Duplicate")) do_duplicate(ru);
                if (o.find("TriggerEnabled")) {
                    bool on = o.i32("TriggerEnabled") != 0;
                    if (ImGui::MenuItem("Active from the start", nullptr, on)) {
                        touch("trigger setting");
                        if (WObj* mo = object_by_uid(w, ru)) mo->set_i32("TriggerEnabled", on ? 0 : 1);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) do_delete(ru);
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            if ((cut || nm != o.description()) && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
                ImGui::TextUnformatted(full.c_str() + std::min(full.size(), size_t(r.depth * 2)));
                if (!o.description().empty() && nm != o.description()) ImGui::TextDisabled("Name in the level file: %s", o.description().c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    };
    if (rows.empty()) ImGui::TextDisabled(ro ? "This level has no triggers of its own." : "No triggers yet. Press New trigger.");
    {
        const std::string head = "Your triggers (" + std::to_string(n_yours) + ")##yours";
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            if (yours.empty()) ImGui::TextDisabled(ro ? "A real level: its own triggers are below." : "None yet: New trigger... adds one.");
            draw_rows(yours);
            ImGui::TreePop();
        }
    }
    if (n_level) {
        const std::string head = "From the level (" + std::to_string(n_level) + ")##level";
        // Folded, unless the map is a read-only level, a filter is typed, or the picked trigger is one of them.
        static uint32_t forced_for = 0;
        bool force = !lf.empty();
        for (auto* rp : lvl) force |= w.objs[size_t(rp->idx)].uid() == trig_uid_ && forced_for != trig_uid_;
        // The fold state is stored per heading id, so a newly opened map starts again from its own default (open for a real level).
        static const Scenario* seen_doc = nullptr;
        static bool seen_ro = false;
        if (force) { ImGui::SetNextItemOpen(true, ImGuiCond_Always); forced_for = trig_uid_; }
        else if (seen_doc != doc_.get() || seen_ro != ro) ImGui::SetNextItemOpen(ro, ImGuiCond_Always);
        seen_doc = doc_.get();
        seen_ro = ro;
        trig_level_open_ = ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        if (trig_level_open_) {
            draw_rows(lvl);
            ImGui::TreePop();
        }
    } else {
        trig_level_open_ = false;
    }
    ImGui::EndChild();

    auto finish = [&]() {
        if (later) { auto f = std::move(later); later = nullptr; f(); }
    };
    if (ti < 0) {
        if (!rows.empty()) ImGui::TextDisabled("Pick a trigger above.");
        finish();
        return;
    }
    WObj& tr = w.objs[size_t(ti)];
    const std::string tcls = cls_name(tr);
    ImGui::SeparatorText(("Selected: " + kind_label(tr)).c_str());
    // A trigger just made from New trigger... or picked in the list: bring its When / Then do editor into view.
    if (g_show_trig_editor || trig_scroll_to_) { ImGui::SetScrollHereY(0.0f); g_show_trig_editor = false; trig_scroll_to_ = false; }
    {
        Dis d(ro);
        std::string nm = tr.description();
        form_label("Name", form_col());
        if (trig_name_focus_) { ImGui::SetKeyboardFocusHere(); trig_name_focus_ = false; }  // a new / copied / renamed trigger: type its name at once
        if (ImGui::InputText("##tname", &nm, ImGuiInputTextFlags_AutoSelectAll)) { touch("rename trigger"); tr.set_str("Description", nm); }
        if (tr.find("TriggerEnabled")) f_bool(tr, "TriggerEnabled", "Active from the start", "Untick to keep the trigger asleep until an action of another trigger switches it on.");
        if (tr.find("TriggerAutoReset")) {
            ImGui::SameLine();
            f_bool(tr, "TriggerAutoReset", "Repeat", "Fire every time the condition is met, not just the first time. A condition that stays true (someone standing in a circle) fires again on every check, not once per entry: switch the trigger off in its own actions if it should fire once per visit.");
        }
    }
    ImGui::SeparatorText("When");
    {
        Dis d(ro);
        if (tcls == "MATriggerTimer") {
            float sec = tr.f32("TrgTimerDurationSeconds");
            form_label("After (seconds)", form_col());
            if (ImGui::InputFloat("##tsec", &sec, 0, 0, "%.1f")) {
                touch("trigger timer");
                tr.set_f32("TrgTimerDurationSeconds", sec);
                tr.set_f32("TrgTimerDuration", sec / 60.f);
                // A name that says "... after 30 s" follows the number (until the user renames it to something else).
                const std::string d = tr.description();
                const size_t at = d.rfind("after ");
                if (at != std::string::npos && d.size() > 8 && d.compare(d.size() - 2, 2, " s") == 0) {
                    const std::string num = d.substr(at + 6, d.size() - 2 - (at + 6));
                    bool digits = !num.empty();
                    for (char ch : num) digits &= (ch >= '0' && ch <= '9') || ch == '.';
                    if (digits) {
                        char b[32];
                        snprintf(b, sizeof b, "%g", double(std::max(sec, 0.f)));
                        tr.set_str("Description", d.substr(0, at + 6) + b + " s");
                    }
                }
            }
            {
                // The clock runs from the mission start, intro included; the briefing starts when the intro ends.
                const float in = intro_seconds(w);
                if (in > 0.f && tr.i32("TriggerEnabled", 1))
                    ImGui::SetItemTooltip("Counted from when this trigger is switched on (0 = as the mission starts). The intro takes the first %.0f s of that, and the briefing starts when it ends.", double(in));
                else
                    ImGui::SetItemTooltip("Counted from when this trigger is switched on (0 = as the mission starts).");
            }
            // Every timer can show its countdown on the player's HUD (off unless ticked; the Countdown template ticks it).
            f_bool(tr, "TrgTimerDisplayOnHUD", "Show the clock on the HUD", "The player sees the time left counting down on the HUD while this timer runs.");
        } else if (tcls == "MATriggerRadius" || tcls == "MATriggerCuboidalVolume") {
            bool box = tcls == "MATriggerCuboidalVolume";
            if (box) {
                f_float(tr, "VolumeWidth", "Width (m)");
                f_float(tr, "VolumeLength", "Length (m)");
                f_float(tr, "VolumeHeight", "Height (m)");
                f_float(tr, "VolumeHeading", "Turn (degrees)");
                f_enum(tr, "VolumeStateTest", "Fires when", {{1, "The player is inside the box"}, {2, "The player is outside the box"}});
            } else {
                f_float(tr, "EncounterRadius", "Radius (m)");
                // The game tests inside = 1, outside = 2 (never 0); a 0 left by an old file shows as outside and is repaired on open.
                f_enum(tr, "InRadiusCheck", "Fires when", {{1, "The player is inside the circle"}, {2, "The player is outside the circle"}});
                if (tr.i32("InRadiusCheck") == 2 && tr.i32("TriggerEnabled", 1)) {
                    Vec3 st{}, c{};
                    bool have = false;
                    for (auto& o : w.objs) if (o.cls == key_of("MALevelStart") && get_pos(o, st)) { have = true; break; }
                    const float r = tr.f32("EncounterRadius", 30);
                    if (have && get_pos(tr, c) && (st.x - c.x) * (st.x - c.x) + (st.z - c.z) * (st.z - c.z) >= r * r)
                        ImGui::TextDisabled("The player starts outside: it fires as soon as the mission starts.");
                }
            }
            {
                // Retail wave/MP triggers test a unit SET (RadiusSet for circles, EntitySetUID for boxes): the shape fires when a
                // member of that set is inside. None = the player (the editor default; proven to fire in missions with a mech
                // player, but never in UnderCover MP with the on-foot avatar; proven there with a set and "players only" off).
                static const uint32_t K_ESET = key_of("MoeEntitySet");
                const char* key = box ? "EntitySetUID" : "RadiusSet";
                uint32_t cur = tr.u32(key);
                std::string lab = cur ? unit_label(w, cur) : "The player";
                form_label("Units that set it off", form_col());
                if (begin_combo("##units", lab.c_str())) {
                    // Retail wave triggers with a set: TypeOfCheck 2 (nation test), PlayerEntitesOnly 0, nation 11 inverted = a member
                    // of any side. Proven alone in UnderCover MP (set + PlayerEntitesOnly 0 with TypeOfCheck 3 did not fire).
                    if (ImGui::Selectable("The player", cur == 0)) {
                        touch("trigger units");
                        tr.set_u32(key, 0);
                        tr.set_i32("TypeOfCheck", 3); tr.set_i32("PlayerEntitesOnly", 1); tr.set_i32("NationInverse", 0); tr.set_i32("NationCompare", 6);
                    }
                    for (auto& o : w.objs)
                        if (o.cls == K_ESET) {
                            std::string l = (o.description().empty() ? "Set #" + std::to_string(o.uid()) : o.description()) + "##" + std::to_string(o.uid());
                            if (ImGui::Selectable(l.c_str(), o.uid() == cur)) {
                                touch("trigger units");
                                tr.set_u32(key, o.uid());
                                tr.set_i32("TypeOfCheck", 2); tr.set_i32("PlayerEntitesOnly", 0); tr.set_i32("NationInverse", 1); tr.set_i32("NationCompare", 11);
                            }
                        }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("A list of units from the Sets view. The shape fires when one of them is inside it. 'The player' = whoever you play.");
                if (!cur && doc_->multiplayer) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(theme::tok().warn, "On a multiplayer map 'The player' never sets it off: pick a set of units (Sets view) instead.");
                    ImGui::PopTextWrapPos();
                }
                if (cur && ImGui::SmallButton("Edit its units")) { set_uid_ = cur; trig_mode_ = 3; select_uid(cur); }
            }
            if (theme::small_toggle_button("Move to a point", picking(PickTarget::MoveShape, tr.uid()))) {
                pick_ = PickTarget();
                pick_.kind = PickTarget::MoveShape;
                pick_.uid = tr.uid();
                tool_ = Tool::PickPoint;
            }
            ImGui::SetItemTooltip("Then click the map: it moves there and keeps its height above the ground. Drag its handles on the map to resize it.");
            // The old line ("Who triggers it ... is under Properties") contradicted the picker above.
            hint_text("More filters (which side, players only) are under Properties.");
        } else if (tcls == "MATriggerDestroyed") {
            f_enum(tr, "CombineMethod", "Fires when", {{2, "All of the units are lost"}, {1, "Any one of the units is lost"}});
            f_bool(tr, "AllowJacking", "Stealing a unit counts too");
            f_int(tr, "DamageLevel", "Fires when its health is down to (%)", "Health LEFT, not damage dealt: it fires once a unit is down to this much health or less. 0 = only when it is destroyed; 80 = after a little damage; 20 = when it is nearly dead.");
            auto refs = reference_children(w, tr.uid());
            ImGui::TextUnformatted("Units it watches:");
            if (tr.i32("IncludePlayers")) ImGui::BulletText("The player's own units");
            uint32_t rm = 0;
            for (uint32_t r : refs) {
                const WObj* ro_ = object_by_uid(w, r);
                uint32_t target = ro_ ? ro_->u32("ReferenceUID") : 0;
                ImGui::PushID(int(r));
                ImGui::BulletText("%s", unit_label(w, target).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Go") && target) { go_to(target); select_uid(target); }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) rm = r;
                ImGui::PopID();
            }
            if (refs.empty() && !tr.u32("DestroySet")) {  // "Include player units" alone also fires at load
                ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().error);
                ImGui::TextWrapped("No units yet, so this fires the moment the mission starts. Click the units on the map, then press Add selected.");
                ImGui::PopStyleColor();
            }
            if (ImGui::SmallButton("Add selected")) {
                std::vector<uint32_t> add;
                for (uint32_t x : selection_) {
                    bool have = false;
                    for (uint32_t r : refs) { const WObj* q = object_by_uid(w, r); have |= q && q->u32("ReferenceUID") == x; }
                    if (!have && x != tr.uid() && watchable(x)) add.push_back(x);
                }
                if (!add.empty()) {
                    uint32_t me = tr.uid();
                    later = [this, me, add]() { push_undo("watch units"); for (uint32_t x : add) add_reference(*doc_, me, x); world_changed(); };
                }
            }
            ImGui::SetItemTooltip("Select units on the map (Shift-click or drag a box for several), then press this.");
            if (rm) later = [this, rm]() { push_undo("stop watching"); wedit::remove_subtree(doc_->world, rm); world_changed(); };
        } else if (tcls == "MATriggerLogic") {
            f_enum(tr, "LogicType", "Fires when", {{0, "All of its inputs are true"}, {1, "Any one input is true"}});
            f_bool(tr, "Inverse", "Fires when this is NOT true (swap)");
            auto refs = reference_children(w, tr.uid());
            ImGui::TextUnformatted("Inputs (other triggers):");
            uint32_t rm = 0;
            for (uint32_t r : refs) {
                const WObj* ro_ = object_by_uid(w, r);
                uint32_t target = ro_ ? ro_->u32("ReferenceUID") : 0;
                const WObj* to = target ? object_by_uid(w, target) : nullptr;
                ImGui::PushID(int(r));
                ImGui::BulletText("%s", to ? trigger_name(*to).c_str() : unit_label(w, target).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Open") && to) { trig_uid_ = target; select_uid(target); }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) rm = r;
                ImGui::PopID();
            }
            if (refs.empty()) ImGui::TextDisabled("No inputs yet.");
            ImGui::SetNextItemWidth(220 * dpi_);
            if (begin_combo("##addin", "Add input...", ImGuiComboFlags_HeightLarge)) {
                static std::string inf;  // a level has hundreds of triggers: a filter on top
                if (ImGui::IsWindowAppearing()) { inf.clear(); ImGui::SetKeyboardFocusHere(); }
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##infilter", "Filter", &inf);
                const std::string il = lower(inf);
                for (auto& r : rows) {
                    const WObj& x = w.objs[size_t(r.idx)];
                    if (x.uid() == tr.uid()) continue;
                    if (!il.empty() && lower(trigger_name(x) + " " + kind_label(x)).find(il) == std::string::npos) continue;
                    std::string l = trigger_name(x) + "##" + std::to_string(x.uid());
                    if (ImGui::Selectable(l.c_str())) {
                        uint32_t me = tr.uid(), tg = x.uid();
                        later = [this, me, tg]() { push_undo("add input"); add_reference(*doc_, me, tg); world_changed(); };
                    }
                }
                ImGui::EndCombo();
            }
            if (rm) later = [this, rm]() { push_undo("remove input"); wedit::remove_subtree(doc_->world, rm); world_changed(); };
        } else if (tcls == "MATriggerCounter") {
            // The game adds up how many times the listed triggers have fired (sub_14FC90: the sum of each referenced
            // trigger's fire count) and compares it with Count needed. Nothing was offered to say what to count.
            f_enum(tr, "CheckType", "Fires when the total is", {{0, "exactly the number"}, {1, "more than the number"}, {2, "less than the number"}, {3, "anything but the number"}});
            f_int(tr, "CounterValue", "The number", "How many times in total the triggers below must have fired.");
            auto refs = reference_children(w, tr.uid());
            ImGui::TextUnformatted("Triggers it counts (every time one fires):");
            uint32_t rm = 0;
            for (uint32_t r : refs) {
                const WObj* ro_ = object_by_uid(w, r);
                uint32_t target = ro_ ? ro_->u32("ReferenceUID") : 0;
                const WObj* to = target ? object_by_uid(w, target) : nullptr;
                ImGui::PushID(int(r));
                ImGui::BulletText("%s", to ? trigger_name(*to).c_str() : unit_label(w, target).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Open") && to) { trig_uid_ = target; select_uid(target); }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) rm = r;
                ImGui::PopID();
            }
            if (refs.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().error);
                ImGui::TextWrapped("Nothing to count yet, so the total stays 0 and this never fires (unless it asks for 'exactly 0'). Add the triggers whose firings it should count.");
                ImGui::PopStyleColor();
            }
            ImGui::SetNextItemWidth(220 * dpi_);
            if (begin_combo("##addcount", "Add a trigger to count...", ImGuiComboFlags_HeightLarge)) {
                static std::string cf;
                if (ImGui::IsWindowAppearing()) { cf.clear(); ImGui::SetKeyboardFocusHere(); }
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##cfilter", "Filter", &cf);
                const std::string cl = lower(cf);
                for (auto& r : rows) {
                    const WObj& x = w.objs[size_t(r.idx)];
                    if (x.uid() == tr.uid()) continue;
                    if (!cl.empty() && lower(trigger_name(x) + " " + kind_label(x)).find(cl) == std::string::npos) continue;
                    std::string l = trigger_name(x) + "##" + std::to_string(x.uid());
                    if (ImGui::Selectable(l.c_str())) {
                        uint32_t me = tr.uid(), tg = x.uid();
                        later = [this, me, tg]() { push_undo("count a trigger"); add_reference(*doc_, me, tg); world_changed(); };
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("A trigger that fires once adds 1; one set to repeat adds 1 each time it fires.");
            if (rm) later = [this, rm]() { push_undo("stop counting"); wedit::remove_subtree(doc_->world, rm); world_changed(); };
        } else if (tcls == "MATriggerGameTime") {
            // The game compares the match's time left with this percentage (sub_19F40A). A mission has no match clock (game
            // run: with 100 it never fired), so this only works on a multiplayer map with a time limit.
            f_int(tr, "m_nPercentLeft", "Fires when this much of the match time is left (%)", "100 = at the start, 50 = half the match left, 0 = when time runs out.");
            if (!doc_->multiplayer) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
                ImGui::TextWrapped("Missions have no match clock, so this never fires here. For a time in a mission use 'After a delay'. This kind is for multiplayer maps with a time limit.");
                ImGui::PopStyleColor();
            }
        } else {
            ui_common_fields(tr);
        }
    }
    if (ImGui::SmallButton("Open in Properties")) select_uid(tr.uid());
    ImGui::SetItemTooltip("Every setting of this trigger (who sets it off, unit-type filters...).");

    // ---- actions
    ImGui::SeparatorText("Then do (in order)");
    std::vector<int> acts;
    for (int k : t.kids[size_t(ti)]) if (kind[size_t(k)] == 2) acts.push_back(k);
    uint32_t rm_action = 0, up = 0, down = 0;
    for (size_t ai = 0; ai < acts.size(); ai++) {
        WObj& a = w.objs[size_t(acts[ai])];
        const std::string ac = cls_name(a);
        ImGui::PushID(int(a.uid()));
        const std::string head_full = std::to_string(ai + 1) + ". " + kind_label(a);
        // A long title stops before the move / remove buttons and shows in full as a tooltip.
        const std::string head = fit_text(head_full, ImGui::GetContentRegionAvail().x - 64 * dpi_ - ImGui::GetFrameHeight() * 1.5f);
        if (a.uid() == open_action_) { ImGui::SetNextItemOpen(true, ImGuiCond_Always); open_action_ = 0; }  // a new action starts open
        bool open = ImGui::TreeNodeEx("##a", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "%s", head.c_str());
        if (head != head_full) ImGui::SetItemTooltip("%s", head_full.c_str());
        ImGui::SameLine(std::max(40.f * dpi_, ImGui::GetContentRegionAvail().x - 64 * dpi_));
        {
            Dis d(ro);
            if (ImGui::SmallButton("^")) up = a.uid();
            ImGui::SetItemTooltip("Run this action earlier.");
            ImGui::SameLine();
            if (ImGui::SmallButton("v")) down = a.uid();
            ImGui::SetItemTooltip("Run this action later.");
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) rm_action = a.uid();
            ImGui::SetItemTooltip("Remove this action.");
        }
        if (open) {
            Dis d(ro);
            ui_action_body(tr, a, later);
            same_line_if_fits("Properties");   // On its own line when the row above leaves no room
            if (ImGui::SmallButton("Properties")) select_uid(a.uid());
            ImGui::SetItemTooltip("Select this action: Properties shows every setting.");
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (acts.empty()) {
        // AI orders it starts (below) are what it does when it fires, so a trigger with orders only is not idle.
        const size_t n_orders = orders_of_trigger(w, tr.uid()).size();
        ImGui::PushTextWrapPos(0);
        if (n_orders)
            ImGui::TextDisabled("No actions: when it fires it starts the AI order%s below. Add an action to also do something else.",
                                n_orders == 1 ? "" : "s");
        else
            ImGui::TextDisabled("No actions yet: until you add one (for example \"%s\"), this trigger does nothing when it fires.",
                                s.multiplayer ? "Play a sound" : "Show a message");
        ImGui::PopTextWrapPos();
    }
    if (rm_action) later = [this, rm_action]() { push_undo("remove action"); wedit::remove_subtree(doc_->world, rm_action); selection_.erase(rm_action); world_changed(); };
    auto shift = [&](uint32_t uid, int dir) {
        // Steps over the siblings (references, nested triggers) between this action and the neighbouring action.
        std::vector<uint32_t> sibs;
        for (int k : t.kids[size_t(ti)]) sibs.push_back(w.objs[size_t(k)].uid());
        int pos = int(std::find(sibs.begin(), sibs.end(), uid) - sibs.begin()), steps = 0;
        for (int p = pos + dir; p >= 0 && p < int(sibs.size()); p += dir) {
            steps++;
            const WObj* o = object_by_uid(w, sibs[size_t(p)]);
            if (o && names.label(o->cls).rfind("MAAction", 0) == 0) break;
            if (p + dir < 0 || p + dir >= int(sibs.size())) { steps = 0; break; }
        }
        if (!steps) return;
        later = [this, uid, dir, steps]() { push_undo("move action"); for (int i = 0; i < steps; i++) wedit::move_sibling(doc_->world, uid, dir); world_changed(); };
    };
    if (up) shift(up, -1);
    if (down) shift(down, 1);
    if (!ro) {
        ImGui::SetNextItemWidth(260 * dpi_);
        if (begin_combo("##adda", "Add action...", ImGuiComboFlags_HeightLarge)) {
            uint32_t me = tr.uid();
            // Multiplayer map: the main list and Other actions hold the kinds the retail MP worlds use; the rest go under Advanced.
            const bool mp = s.multiplayer;
            auto shown = [&](const std::string& cls, bool advanced) { return !mp || mp_retail_class(cls) != advanced; };
            auto friendly_items = [&](bool advanced) {
                for (auto& k : kAddActions) {
                    if (!shown(k.cls, advanced)) continue;
                    if (advanced ? ImGui::MenuItem(k.label) : ImGui::Selectable(k.label)) {
                        std::string cls = k.cls, label = k.label;
                        later = [this, me, cls, label]() {
                            push_undo("add action");
                            uint32_t a = add_action_object(*doc_, me, cls.c_str(), label);
                            open_action_ = a;
                            world_changed();
                            // A new 'Complete objective' names objective 1 at once: its radar target follows next frame.
                            if (const WObj* ao = a && cls == "MAActionMissionVariableSet" ? object_by_uid(doc_->world, a) : nullptr)
                                for (int i = 0; i < int(doc_->objectives.size()); i++)
                                    if (objective_var(*doc_, i) == ao->i32("MissionVariableSet")) { mark_trigger_ = me; mark_objective_ = i; break; }
                        };
                    }
                }
            };
            auto other_items = [&](bool advanced) {
                for (auto& k : io_->part_kinds()) {
                    if (k.name.rfind("MAAction", 0) != 0 || !shown(k.name, advanced)) continue;
                    bool friendly = false;
                    for (auto& f : kAddActions) friendly |= k.name == f.cls;
                    if (friendly) continue;
                    const char* n = lookup(kActionNames, std::size(kActionNames), k.name);
                    if (ImGui::MenuItem((n ? n : words_of(k.name, "MAAction")).c_str())) {
                        uint32_t cls = k.cls;
                        later = [this, me, cls]() { push_undo("add action"); open_action_ = io_->add_part(*doc_, cls, me, nullptr); world_changed(); };
                    }
                }
            };
            friendly_items(false);
            if (io_ && ImGui::BeginMenu("Other actions")) {
                other_items(false);
                ImGui::EndMenu();
            }
            if (mp && ImGui::BeginMenu("Advanced: campaign actions")) {
                ImGui::TextDisabled("The game's multiplayer maps use none of these.");
                friendly_items(true);
                if (io_) other_items(true);
                ImGui::EndMenu();
            }
            ImGui::EndCombo();
        }
    }

    // ---- AI orders
    ImGui::SeparatorText("AI orders it starts");
    if (s.multiplayer) ImGui::TextDisabled("Unit orders only make sense in missions.");
    auto orders = orders_of_trigger(w, tr.uid());
    uint32_t rm_order = 0;
    for (uint32_t ev : orders) {
        WObj* e = object_by_uid(w, ev);
        if (!e) continue;
        uint32_t unit = order_unit(w, ev), beh = order_behaviour(w, ev);
        WObj* b = beh ? object_by_uid(w, beh) : nullptr;
        std::string bc = b ? cls_name(*b) : std::string();
        ImGui::PushID(int(ev));
        const std::string head_full = unit_label(w, unit) + ": " + (bc == "MAPatrol" ? "patrol" : bc == "MADefend" ? "defend a point" : bc == "MAFollow" ? "follow" : bc == "MAAttack" ? "attack" : "(no order)");
        const std::string head = fit_text(head_full, ImGui::GetContentRegionAvail().x - 20 * dpi_ - ImGui::GetFrameHeight() * 1.5f);
        bool open = ImGui::TreeNodeEx("##o", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "%s", head.c_str());
        if (head != head_full) ImGui::SetItemTooltip("%s", head_full.c_str());
        ImGui::SameLine(std::max(40.f * dpi_, ImGui::GetContentRegionAvail().x - 20 * dpi_));
        {
            Dis d(ro);
            if (ImGui::SmallButton("x")) rm_order = ev;
            ImGui::SetItemTooltip("Remove these orders (and their patrol path).");
        }
        if (open) {
            if (ImGui::SmallButton("Select unit") && unit) { select_uid(unit); go_to(unit); }
            same_line_if_fits("Properties");
            if (ImGui::SmallButton("Properties")) select_uid(ev);
            Dis d(ro);
            if (int side = ro ? -1 : ui_neutral_fix(unit); side >= 0) later = [this, unit, side]() { set_unit_side_now(unit, side); };
            static const char* ocls[] = {"MAPatrol", "MADefend", "MAFollow", "MAAttack"};
            static const char* olab[] = {"Patrol along a path", "Defend a point (stand and guard)", "Follow (the player or a unit)", "Attack (the player or a unit)"};
            int cur = -1;
            for (int k = 0; k < 4; k++) if (bc == ocls[k]) cur = k;
            form_label("Order", form_col());
            if (begin_combo("##order", cur >= 0 ? olab[cur] : "(none)")) {
                for (int k = 0; k < 4; k++)
                    if (ImGui::Selectable(olab[k], k == cur)) {
                        later = [this, ev, beh, k]() {
                            push_undo("change order");
                            set_order_kind(*doc_, ev, ocls[k]);
                            // A path / point / unit click waiting for the old order is dropped with it
                            if ((tool_ == Tool::PlaceWaypoint || tool_ == Tool::PickPoint) && (pick_.uid == ev || pick_.uid == beh)) { tool_ = Tool::Select; pick_ = PickTarget(); }
                            world_changed();
                        };
                    }
                ImGui::EndCombo();
            }
            if (b && bc == "MAPatrol") {
                uint32_t g = order_path(w, ev);
                int n = g ? ai_path_length(s, g) : 0;
                ImGui::Text("Patrol path: %d point%s on the map%s", n, n == 1 ? "" : "s", n < 2 ? " (needs 2 or more)" : "");
                if (g) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Select")) { select_uid(g); }
                    const WObj* go = nullptr;
                    for (auto& o : w.objs) if (o.uid() == g) go = &o;
                    if (go) {
                        bool loop = go->i32("TraverseType") == 2;
                        if (ImGui::Checkbox("Loop the route", &loop))
                            later = [this, g, loop]() {
                                push_undo("patrol loop");
                                for (auto& o : doc_->world.objs) if (o.uid() == g) o.set_i32("TraverseType", loop ? 2 : 1);
                                doc_->dirty = true;
                            };
                        ImGui::SetItemTooltip("On: the unit goes round the route again and again. Off: it walks the route once, then stands at the last point.");
                    }
                }
                auto draw = [&](bool clear) {
                    if (clear) later = [this, ev]() { push_undo("redraw patrol path"); order_clear_path(*doc_, ev); world_changed(); };
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::OrderPath;
                    pick_.uid = ev;
                    tool_ = Tool::PlaceWaypoint;
                };
                if (theme::small_toggle_button(n ? "Add points" : "Draw on map", picking(PickTarget::OrderPath, ev))) draw(false);
                ImGui::SetItemTooltip("Then click the map once per point, in order. Right-click when done.");
                if (n) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Redraw")) draw(true);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) later = [this, ev]() { push_undo("clear patrol path"); order_clear_path(*doc_, ev); world_changed(); };
                }
            } else if (b && bc == "MADefend") {
                uint32_t mk = b->u32("DefendUID");
                ImGui::Text("Defend point: %s", mk ? "a marker on the map" : "(none yet)");
                ImGui::SameLine();
                if (mk && ImGui::SmallButton("Select")) { select_uid(mk); go_to(mk); }
                ImGui::SameLine();
                if (theme::small_toggle_button("Pick on map", picking(PickTarget::LinkMarker, beh))) {
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::LinkMarker;
                    pick_.uid = beh;
                    pick_.key = "DefendUID";
                    pick_.null_marker = true;
                    pick_.name = trigger_name(tr) + " - defend point";
                    tool_ = Tool::PickPoint;
                }
                ImGui::SetItemTooltip("Then click the map: the unit guards that spot.");
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("The unit guards this spot: it fights what comes near and does not chase further than the radius from it. It does not walk there by itself (\"Defend a point\" in the orders chooser makes the walk and a round trigger that starts this guard order when the unit arrives).");
                ImGui::PopTextWrapPos();
                f_float(*b, "Radius", "Radius (m)");
                ImGui::SetItemTooltip("How far from the spot the unit goes after an enemy (the game's missions use 100 to 190 m).");
            } else if (b && bc == "MAFollow") {
                f_link(*b, "FollowUID", "Follow (the player start = the player):");
                f_float(*b, "FollowRadius", "Radius (m)");
            } else if (b && bc == "MAAttack") {
                bool player = b->u32("AttackPlayer") == 2;
                ImGui::Text("Attacks: %s", player ? "the player" : unit_label(w, b->u32("AttackUID")).c_str());
                if (ImGui::SmallButton("Attack the selected unit") && !selection_.empty()) {
                    touch("order target");
                    b->set_u32("AttackUID", *selection_.begin());
                    b->set_u32("AttackPlayer", 1);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Attack the player")) {
                    touch("order target");
                    b->set_u32("AttackUID", player_start_uid(w));
                    b->set_u32("AttackPlayer", 2);
                }
                bool keep = b->f32("AttackTimeout") >= 9999;
                if (ImGui::Checkbox("Keep attacking until the target is gone", &keep)) {
                    touch("attack time");
                    b->set_f32("AttackTimeout", keep ? 9999.f : 3.f);
                }
                ImGui::SetItemTooltip("Off: the unit attacks for a few seconds (one volley), then goes back to its other orders.");
                if (!keep) f_float(*b, "AttackTimeout", "Stop attacking after (s)");
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (orders.empty()) {  // wrapped: one line ran past the panel's edge
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("None. Select a unit on the map, then press the button below to make it act when this trigger fires.");
        ImGui::PopStyleColor();
    }
    if (rm_order) later = [this, rm_order]() { push_undo("remove orders"); wedit::remove_subtree(doc_->world, rm_order); world_changed(); };
    if (!ro && !s.multiplayer) {
        static const uint32_t unit_classes[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"), key_of("MATurret"), key_of("MAPerson")};
        const WObj* su = selection_.size() == 1 ? object_by_uid(w, *selection_.begin()) : nullptr;
        bool unit_ok = su && std::find(std::begin(unit_classes), std::end(unit_classes), su->cls) != std::end(unit_classes);
        if (unit_ok) {
            order_button(tr.uid(), *selection_.begin(), "Give the selected unit orders...",
                         "Pick what the unit does when this trigger fires: walk a path once, patrol, defend a point, follow or attack the player, or attack a unit.", false);
        } else {
            if (theme::small_toggle_button("Pick a unit on the map", tool_ == Tool::PickPoint && pick_.kind == PickTarget::OrderUnit)) {
                pick_ = PickTarget();
                pick_.kind = PickTarget::OrderUnit;
                pick_.uid = tr.uid();
                pick_.back = tab_;
                tool_ = Tool::PickPoint;
                status_ = "Click the unit on the map. Right-click or " + kt(A_CANCEL) + " cancels.";
                status_time_ = 8;
            }
            ImGui::SetItemTooltip("Then click the unit on the map: you choose what it does when this trigger fires.");
        }
    }

    if (tool_ == Tool::PickPoint) ImGui::TextColored(theme::tok().accent, "Click the map to set the point (right-click cancels).");
    if (tool_ == Tool::PlaceWaypoint) ImGui::TextColored(theme::tok().accent, "Click the map to add path points; right-click when done.");
    finish();
}

}  // namespace ed
