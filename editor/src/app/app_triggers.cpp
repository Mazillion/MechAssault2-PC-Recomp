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
#include "ui_tips.h"
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
    {"MAActionActivateCinema", "Play a camera sequence (cutscene)"},
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
    {"MAActionActivateCinema", "Play a camera sequence (cutscene)"},
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

std::string unit_label(const Wdf& w, uint32_t uid) {
    if (!uid) return "(none)";
    if (const WObj* o = object_by_uid(w, uid)) return o->description().empty() ? "unnamed object #" + std::to_string(uid) : o->description();  // ids only when there is no name
    return "missing #" + std::to_string(uid);
}

}  // namespace

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
// (units destroyed, timer, logic, mission start...) have no place on the map, so they get no gizmo and no "drag the arrows" hint (O5).
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

uint32_t App::give_unit_orders(uint32_t uid) {
    const WObj* u = doc_ ? object_by_uid(doc_->world, uid) : nullptr;
    if (!u || !io_) return 0;
    push_undo("unit orders");
    std::string who = u->description().empty() ? std::string("unit") : u->description();
    const bool turret = gd_.names().label(u->cls) == "MATurret";  // read before the new trigger moves the objects
    uint32_t trig = add_trigger_object(*doc_, "MATriggerTimer", "Orders: " + who);  // fires as the mission starts
    // Units patrol (draw the path next); a turret cannot walk, so it attacks the player.
    uint32_t ev = trig ? add_order(*doc_, trig, uid, turret ? "MAAttack" : "MAPatrol") : 0;
    if (!ev) return 0;
    // Open the new trigger on the Triggers view (not the last-used Sets / AI orders view).
    trig_uid_ = trig;
    trig_mode_ = 0;
    tab_ = Tab::Triggers;
    selection_ = {trig};
    if (turret) {
        status_ = who + " attacks the player when the mission starts. Change the order or add a condition here.";
    } else {
        pick_ = PickTarget();
        pick_.kind = PickTarget::OrderPath;
        pick_.uid = ev;
        tool_ = Tool::PlaceWaypoint;
        status_ = who + " patrols when the mission starts: click the map once per patrol point, right-click when done. Or change the order to defend, follow or attack.";
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
                what = "Patrol along a path (" + std::to_string(k) + (k == 1 ? " point" : " points") + (k < 2 ? ", needs 2 or more)" : ")");
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
            if (tr && names.label(tr->cls) == "MATriggerTimer" && tr->f32("TrgTimerDurationSeconds") == 0 && tr->f32("TrgTimerDuration") == 0)
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
    if (!n) {
        // An AI-off unit stays asleep: it neither moves nor fights until a trigger wakes it (retail M01b tanks; game-tested).
        const WObj* u = object_by_uid(w, uid);
        if (u && u->find("m_bAIEnabled") && !u->i32("m_bAIEnabled"))
            ImGui::TextWrapped("None, and its AI is off: it stays still and does not fight. Tick 'AI starts on' above, or give it orders (they switch its AI on).");
        else
            ImGui::TextDisabled("None: it stands and fights whatever comes close.");
    }
    ImGui::BeginDisabled(doc_->read_only);
    bool give = ImGui::Button(n ? "Give it more orders..." : "Give this unit orders...", ImVec2(-1, 0));
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Makes a trigger that starts as the mission starts and orders this unit to patrol: click the map for each patrol point (a turret attacks the player). "
                          "On the Objectives & Triggers tab you can change it to defend a spot, follow or attack, or make it wait for a condition.");
    if (give) give_unit_orders(uid);
    else if (open_trigger) { trig_uid_ = open_trigger; trig_mode_ = 0; tab_ = Tab::Triggers; select_uid(open_trigger); }
}

// ----------------------------------------------------------------------------- the tab

// A 'Complete objective' action just pointed at an objective with no radar target (run next frame: it adds objects).
static uint32_t g_mark_trigger = 0;
static int g_mark_objective = -1;

void App::tab_triggers() {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const bool ro = s.read_only;
    // The player gets a radar / HUD target as retail does: a Destroyed trigger's units, or an invisible NULL marker at a
    // circle or box (Mission_03 'Dummy - cp 1 objective', as 'Reach a place' makes). Other triggers have no place to show.
    if (g_mark_trigger && g_mark_objective >= 0 && g_mark_objective < int(s.objectives.size()) && objective_targets(w, g_mark_objective).empty()) {
        static const uint32_t K_DESTR = key_of("MATriggerDestroyed"), K_RAD = key_of("MATriggerRadius"), K_VOL = key_of("MATriggerCuboidalVolume");
        int n = 0;
        if (const WObj* tg = object_by_uid(w, g_mark_trigger)) {
            const uint32_t cls = tg->cls;
            const std::string name = tg->description();
            Vec3 p;
            const bool at = get_pos(*tg, p);
            if (cls == K_DESTR) {
                std::vector<uint32_t> units;
                for (uint32_t r : reference_children(w, g_mark_trigger))
                    if (const WObj* ro_ = object_by_uid(w, r)) if (uint32_t u = ro_->u32("ReferenceUID")) units.push_back(u);
                for (uint32_t u : units) n += add_objective_target(s, u, g_mark_objective) ? 1 : 0;
            } else if ((cls == K_RAD || cls == K_VOL) && at) {
                uint32_t m = add_ai_marker(s, p.x, p.z, name + kReachMarkerSuffix, true);
                if (WObj* mo = m ? object_by_uid(w, m) : nullptr)
                    if (mo->find("IsCollidable")) mo->set_i32("IsCollidable", 0);
                if (m && add_objective_target(s, m, g_mark_objective)) n++;
            }
        }
        if (n) {
            world_changed();
            s.dirty = true;
            status_ = "Objective " + std::to_string(g_mark_objective + 1) + ": " + std::to_string(n) + " radar target(s) added from this trigger (Objectives list: Radar targets).";
            status_time_ = 6;
        }
    }
    g_mark_trigger = 0;
    g_mark_objective = -1;
    const auto& names = gd_.names();
    WTree t;
    t.build(w);

    // 1 = trigger, 2 = action, per object.
    std::vector<char> kind(w.objs.size(), 0);
    for (size_t i = 0; i < w.objs.size(); i++) {
        std::string c = names.label(w.objs[i].cls);
        kind[i] = c.rfind("MATrigger", 0) == 0 ? 1 : c.rfind("MAAction", 0) == 0 ? 2 : 0;
    }
    auto cls_name = [&](const WObj& o) { return names.label(o.cls); };
    auto kind_label = [&](const WObj& o) -> std::string {
        std::string c = cls_name(o);
        if (c.rfind("MATrigger", 0) == 0) {
            const char* n = lookup(kTriggerNames, std::size(kTriggerNames), c);
            return n ? n : words_of(c, "MATrigger");
        }
        const char* n = lookup(kActionNames, std::size(kActionNames), c);
        return n ? n : words_of(c, "MAAction");
    };
    // Rows: every trigger, nested ones indented under the trigger that holds them.
    struct Row { int idx, depth; };
    std::vector<Row> rows;
    // Triggers can sit under an action or a plain group inside another trigger: look through those too.
    std::function<void(int, int)> walk_kids;
    std::function<void(int, int)> walk = [&](int idx, int depth) {
        rows.push_back({idx, depth});
        walk_kids(idx, depth + 1);
    };
    walk_kids = [&](int idx, int depth) {
        for (int k : t.kids[size_t(idx)]) {
            if (kind[size_t(k)] == 1) walk(k, depth);
            else walk_kids(k, depth);
        }
    };
    for (size_t i = 0; i < w.objs.size(); i++) {
        if (kind[i] != 1) continue;
        bool inner = false;
        for (int p = t.parent_of[i]; p >= 0 && !inner; p = t.parent_of[size_t(p)]) inner = kind[size_t(p)] == 1;
        if (!inner) walk(int(i), 0);
    }
    // A cutscene's own start/end triggers with nothing in them belong to the cutscene (Cameras & Cutscenes tab),
    // not to the user's trigger list; ones with actions (retail cast shows/hides) stay listed.
    int cut_hidden = 0;
    {
        static const uint32_t K_CTRL = key_of("MAFreeCameraController");
        std::set<uint32_t> cut_trig;
        for (auto& o : w.objs)
            if (o.cls == K_CTRL) { cut_trig.insert(o.u32("triggerInit")); cut_trig.insert(o.u32("triggerExit")); }
        cut_trig.erase(0);
        std::vector<Row> kept;
        for (auto& r : rows) {
            uint32_t u = w.objs[size_t(r.idx)].uid();
            std::vector<int> sub;
            t.subtree(r.idx, sub);
            bool empty = true;
            for (int k : sub) empty = empty && (k == r.idx || kind[size_t(k)] == 0);
            if (r.depth == 0 && empty && cut_trig.count(u) && u != trig_uid_) { cut_hidden++; continue; }
            kept.push_back(r);
        }
        rows.swap(kept);
    }

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

    // ---- field widgets (all write straight into the object)
    auto f_float = [&](WObj& o, const char* key, const char* label, const char* tip = nullptr) {
        float v = o.f32(key);
        form_label(label, form_col());
        if (ImGui::InputFloat((std::string("##") + key).c_str(), &v, 0, 0, "%.2f")) { touch("trigger setting"); o.set_f32(key, v); }
        if (tip) ImGui::SetItemTooltip("%s", tip);
    };
    auto f_int = [&](WObj& o, const char* key, const char* label, const char* tip = nullptr) {
        int v = o.i32(key);
        form_label(label, form_col());
        if (ImGui::InputInt((std::string("##") + key).c_str(), &v, 0, 0)) { touch(label); o.set_i32(key, v); }
        if (tip) ImGui::SetItemTooltip("%s", tip);
    };
    auto f_bool = [&](WObj& o, const char* key, const char* label, const char* tip = nullptr) {
        bool b = o.i32(key) != 0;
        if (ImGui::Checkbox(label, &b)) { touch(label); o.set_i32(key, b ? 1 : 0); }
        if (tip) ImGui::SetItemTooltip("%s", tip);
    };
    auto f_str = [&](WObj& o, const char* key, const char* label) {
        std::string v = o.str(key);
        form_label(label, form_col());
        if (ImGui::InputText((std::string("##") + key).c_str(), &v)) { touch(label); o.set_str(key, v); }
    };
    auto f_enum = [&](WObj& o, const char* key, const char* label, std::vector<std::pair<int, const char*>> opts) {
        int cur = o.i32(key);
        std::string shown = "Other (" + std::to_string(cur) + ")";
        for (auto& [v, n] : opts) if (v == cur) shown = n;
        form_label(label, form_col());
        if (ImGui::BeginCombo((std::string("##") + key).c_str(), shown.c_str())) {
            for (auto& [v, n] : opts) if (ImGui::Selectable(n, v == cur)) { touch(label); o.set_i32(key, v); }
            ImGui::EndCombo();
        }
    };
    auto go_to = [&](uint32_t uid) {
        const WObj* o = object_by_uid(w, uid);
        Vec3 p;
        if (o && get_pos(*o, p)) { cam_.tx = p.x; cam_.tz = p.z; }
    };
    // A link to another object: unit, trigger or marker.
    // set_key: the action can also act on a whole set (AISet / ActionSet): retail reinforcements show, wake and
    // change the side of a hidden set in one action each (Mission_05 "2nd Wave Catch").
    auto f_link = [&](WObj& o, const char* key, const char* label, bool triggers = false, const char* set_key = nullptr) {
        uint32_t u = o.u32(key);
        const uint32_t su = set_key ? o.u32(set_key) : 0;
        const WObj* tgt = u ? object_by_uid(w, u) : nullptr;
        ImGui::PushID(key);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(form_col());
        if (su && !u) {
            const WObj* so = object_by_uid(w, su);
            int n = so ? so->i32("MoeEntitySetCount") : 0;
            if (!so) ImGui::TextColored(theme::tok().warn, "missing set #%u", su);
            else ImGui::TextDisabled("set '%s' (%d unit%s)", so->description().c_str(), n, n == 1 ? "" : "s");
        } else if (u && !tgt) ImGui::TextColored(theme::tok().warn, "%s", unit_label(w, u).c_str());
        else ImGui::TextDisabled("%s", unit_label(w, u).c_str());
        std::vector<uint32_t> picked;
        for (uint32_t x : selection_) if (const WObj* po = object_by_uid(w, x)) if (!is_trigger_shape(po->cls)) picked.push_back(x);
        if (ImGui::SmallButton("Use selection") && !selection_.empty()) {
            if (set_key && picked.size() > 1) {
                // Several units: they become one new set the action acts on.
                uint32_t me = o.uid();
                std::string nm;
                for (int k = 1; nm.empty(); k++) {  // "Units 1", "Units 2"...: the first name no set has yet
                    std::string c = "Units " + std::to_string(k);
                    bool used = false;
                    for (auto& x : w.objs) used |= x.cls == key_of("MoeEntitySet") && x.description() == c;
                    if (!used) nm = c;
                }
                std::string sk = set_key, ek = key;
                later = [this, me, nm, picked, sk, ek]() {
                    push_undo("trigger setting");
                    uint32_t ns = entity_set_from(nm, picked);
                    if (WObj* a = object_by_uid(doc_->world, me)) { a->set_u32(sk, ns); a->set_u32(ek, 0); }
                    world_changed();
                    doc_->dirty = true;
                };
            } else {
                touch("trigger setting");
                o.set_u32(key, *selection_.begin());
                if (set_key) o.set_u32(set_key, 0);
            }
        }
        ImGui::SetItemTooltip(set_key ? "Click something on the map first, then press this to use it here. With several units selected, they become one set this acts on."
                                      : "Click something on the map first (a unit, a marker...), then press this to use it here.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Go") && u) { go_to(u); select_uid(u); }
        ImGui::SetItemTooltip("Select it and move the camera to it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) { touch("trigger setting"); o.set_u32(key, 0); if (set_key) o.set_u32(set_key, 0); }
        if (set_key) {
            static const uint32_t K_SET = key_of("MoeEntitySet");
            ImGui::SetNextItemWidth(150 * dpi_);
            if (ImGui::BeginCombo("##set", "or a set...")) {
                for (auto& x : w.objs)
                    if (x.cls == K_SET) {
                        std::string l = (x.description().empty() ? "Set #" + std::to_string(x.uid()) : x.description()) + " (" +
                                        std::to_string(x.i32("MoeEntitySetCount")) + ")##" + std::to_string(x.uid());
                        if (ImGui::Selectable(l.c_str(), x.uid() == su)) { touch("trigger setting"); o.set_u32(set_key, x.uid()); o.set_u32(key, 0); }
                    }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Act on every unit of a named set (Triggers > Sets). Select several units and press Use selection to make one.");
        }
        if (triggers) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150 * dpi_);
            if (ImGui::BeginCombo("##trg", "or a trigger...")) {
                for (auto& r : rows) {
                    const WObj& x = w.objs[size_t(r.idx)];
                    std::string l = (x.description().empty() ? kind_label(x) : x.description()) + "##" + std::to_string(x.uid());
                    if (ImGui::Selectable(l.c_str(), x.uid() == u)) { touch("trigger setting"); o.set_u32(key, x.uid()); }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
    };
    auto cue_combo = [&](WObj& o, const char* key, bool music, bool voice = false) {
        const auto& cues = music ? retail_music_cues() : voice ? retail_voice_cues() : retail_sound_cues();
        uint32_t cur = o.u32(key);
        std::string lab = cur ? std::to_string(cur) : "(choose)";
        for (auto& c : cues) if (c.id == cur) lab = c.label;
        if (!music && cur >= kAudioBase && cur < kAudioBase + 100000u) {
            auto it = s.audio.find(int(cur - kAudioBase));
            lab = "Your clip " + std::to_string(cur - kAudioBase) + (it != s.audio.end() ? "  " + it->second.source : "  (not imported)");
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##cue", lab.c_str())) {
            static std::string filter;
            ImGui::InputTextWithHint("##cf", "Filter (mission, trigger name)", &filter);
            std::string lf = lower(filter);
            if (!music)
                for (auto& [n, c] : s.audio) {
                    std::string l = "Your clip " + std::to_string(n) + "  " + c.source;
                    if (!lf.empty() && lower(l).find(lf) == std::string::npos) continue;
                    if (ImGui::Selectable(l.c_str(), cur == kAudioBase + uint32_t(n))) { touch("trigger setting"); o.set_u32(key, kAudioBase + uint32_t(n)); }
                }
            int shown = 0;
            for (auto& c : cues) {
                if (!lf.empty() && lower(c.label).find(lf) == std::string::npos) continue;
                ImGui::PushID(int(c.id));
                if (ImGui::Selectable(c.label.c_str(), c.id == cur)) { touch("trigger setting"); o.set_u32(key, c.id); }
                ImGui::PopID();
                if (++shown > 400) break;
            }
            ImGui::EndCombo();
        }
        if (!music && s.audio.empty()) hint_text("Import your own WAV clips on the Objectives & Triggers tab to play them here.");
    };
    auto text_field = [&](WObj& o, const char* key, const char* label, bool optional) {
        uint32_t id = o.u32(key);
        ImGui::TextUnformatted(label);
        if (std::string* tx = own_text(s, id)) {
            std::string v = *tx;
            if (ImGui::InputTextMultiline((std::string("##") + key).c_str(), &v, ImVec2(-1, 52 * dpi_), ImGuiInputTextFlags_WordWrap)) { touch("message text"); *tx = v; }
            if (optional && ImGui::SmallButton("No caption")) { touch("message text"); o.set_u32(key, 0); }
        } else {
            std::string words = id ? gd_.game_text(id) : std::string();
            if (!id) ImGui::TextDisabled("(none)");
            else if (words.empty()) ImGui::TextDisabled("The game's own text (not found on this disc)");
            else ImGui::TextWrapped("\"%s\"", words.c_str());
            if (id) ImGui::SetItemTooltip("The game's own words (text %u). Write my own text replaces them in this map.", id);
            if (ImGui::SmallButton(((id ? "Write my own text##" : "Add text##") + std::string(key)).c_str())) {
                touch("message text");
                o.set_u32(key, add_text(s, ""));
            }
        }
    };
    auto trigger_name = [&](const WObj& x) { return x.description().empty() ? kind_label(x) : x.description(); };

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
    if (!ro && ImGui::TreeNode("How do I make something happen when the player arrives?")) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("1. New trigger... > 'Player enters a box' (or 'Player enters a circle'), then click the map to put it down; drag its handles to size it.");
        ImGui::TextUnformatted("2. Under 'Then do', use 'Add action...' to choose what should happen: show a message, play a sound, play a camera sequence (cutscene), complete an objective...");
        ImGui::TextUnformatted(s.multiplayer ? "3. On a multiplayer map, start from a condition (a circle, a delay, the match clock) and add what the game's own maps use: play a sound, move the player to a marker, change sides. Campaign jobs are under Advanced."
                                             : "3. Common jobs are ready-made: New trigger... > Templates (win when units are destroyed, reinforcements arrive, play a cutscene). They do steps 1-2 for you.");
        ImGui::TextUnformatted("4. Press Test (F5). The trigger fires once when the player first enters the box; tick Repeat to make it fire every time.");
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
            // under Advanced there (review #12; research census of the 16 retail MP worlds).
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
                if (ImGui::MenuItem("Win when these units are destroyed")) {
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
                if (ImGui::MenuItem("Reinforcements arrive when the player enters a circle")) {
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
                if (ImGui::MenuItem("Enemies attack when the player comes close")) {
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
                // fires at load (Lose at once -> the mission restarts forever; dogfood D11). The game ends a mission on player death itself.
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
                    // 1 s, not 0: in a custom mission a 0 s timer's cinema never showed in game (D13, g3 run).
                    if (WObj* o = object_by_uid(w, u)) { o->set_f32("TrgTimerDurationSeconds", 1); o->set_f32("TrgTimerDuration", 1.f / 60.f); }
                    uint32_t act = add_action_object(s, u, "MAActionActivateCinema", "Play the intro");
                    made(u);
                    later = [this, act]() {
                        if (uint32_t c = new_cutscene(true, false)) if (WObj* o = object_by_uid(doc_->world, act)) o->set_u32("cinemaID", c);
                    };
                }
                ImGui::SetItemTooltip("A trigger that plays a camera sequence as soon as the mission starts; a new cutscene is built over the player start for you to shape.");
                if (ImGui::MenuItem("Play a cutscene when the player enters a box")) {
                    // One step for the common case: a box on the map that starts a camera sequence.
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
                ImGui::SetItemTooltip("Adds a trigger box and a 'Play a camera sequence' action in one go. Place the box away from the player start (a box around the start fires as the mission starts and the cutscene is lost), then pick or build the cutscene in the action below.");
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
                for (uint32_t x : selection_) if (x != u) add_reference(s, u, x);
                made(u);
            }
            if (mission) logic();
            // Other kinds: on a multiplayer map only the kinds the retail MP worlds place; the rest go under Advanced.
            auto other_kinds = [&](int want) {  // 0 = all, 1 = used by retail MP, 2 = campaign-only
                // Review B14: the kinds offered above (After a delay, Units are destroyed, Several conditions together) are not
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
                            // B15: named like the menu entry, not "New TriggerPlayerVehicleType".
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
        if (ImGui::Button("Duplicate") && io_) {
            uint32_t src = trig_uid_;
            later = [this, src]() {
                push_undo("duplicate trigger");
                if (uint32_t u = io_->duplicate_subtree(*doc_, src, 12.f)) { trig_uid_ = u; select_uid(u); }
                world_changed();
            };
        }
        ImGui::SetItemTooltip("Copy the selected trigger with its actions (12 m to the side if it has a place on the map).");
        ImGui::SameLine();
        if (theme::danger_button("Delete")) {
            uint32_t victim = trig_uid_;
            int orders = int(orders_of_trigger(w, victim).size()), users = 0;
            for (auto& o : w.objs)
                for (const char* k : {"ReferenceUID", "ActionEntity"}) users += o.find(k) && o.u32(k) == victim;
            std::string q = "Delete the trigger \"" + (ti >= 0 ? trigger_name(w.objs[size_t(ti)]) : std::string()) + "\" with its actions?";
            if (orders) q += "\n\nThe " + std::to_string(orders) + " AI order(s) it starts are deleted too.";
            if (users) q += "\n\n" + std::to_string(users) + " other trigger(s) or action(s) point at it and will stop working.";
            q += "\n\nYou can undo this with Ctrl+Z.";
            confirm(q, [this, victim]() {
                if (!doc_) return;
                push_undo("delete trigger");
                for (uint32_t ev : orders_of_trigger(doc_->world, victim)) wedit::remove_subtree(doc_->world, ev);
                wedit::remove_subtree(doc_->world, victim);
                selection_.erase(victim);
                trig_uid_ = 0;
                world_changed();
                doc_->dirty = true;
            });
        }
        ImGui::SetItemTooltip("Delete the selected trigger (asks first; Ctrl+Z undoes it).");
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
    { int all = 0; for (char kd : kind) all += kd == 1; ImGui::TextDisabled("%d triggers listed (%d in the level file)", int(rows.size()), all); }
    if (cut_hidden) {
        ImGui::TextDisabled("%d cutscene start/end trigger%s not listed:", cut_hidden, cut_hidden == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cameras & Cutscenes")) tab_ = Tab::Cameras;
        ImGui::SetItemTooltip("Empty triggers that only mark where a cutscene shot starts and ends; they are shown with their cutscene.");
    }
    // With a trigger picked the list stays short so its When / Then do (and a message's text box) are in view.
    // As tall as its rows (at least 3, at most 12; 6 while a trigger is picked so its When / Then do stay in view).
    int vis_rows = 0;
    for (auto& r : rows) vis_rows += matches(r.idx);
    float list_h = (float(std::clamp(vis_rows, 3, ti >= 0 ? 6 : 12)) + 0.6f) * ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##tlist", ImVec2(0, list_h), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    // Retail names carry the designers' kind prefix ("TT - Setup", "TGT - GameOver", "TR Player in Uziel"): the list
    // shows the rest ("Setup") next to the plain kind; the full name is on hover and in the Name box below. Only the
    // codes the retail levels use (ux	rigger-prefix-glossary.md), any case, then " - ", "_" or a space; action names
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
    if (rows.empty()) ImGui::TextDisabled(ro ? "This level has no triggers of its own." : "No triggers yet. Press New trigger.");
    int shown = 0;
    for (auto& r : rows) {
        if (!matches(r.idx)) continue;
        if (++shown > 400) { ImGui::TextDisabled("More triggers than shown: use the filter."); break; }
        const WObj& o = w.objs[size_t(r.idx)];
        int nact = 0;
        for (int k : t.kids[size_t(r.idx)]) nact += kind[size_t(k)] == 2;
        std::string nm = o.description().empty() ? trigger_name(o) : friendly(o.description());
        std::string l = std::string(r.depth * 2, ' ') + (o.find("TriggerEnabled") && !o.i32("TriggerEnabled") ? "(asleep) " : "") + nm;
        if (!o.description().empty()) l += "   [" + kind_label(o) + "]";
        if (o.i32("TriggerAutoReset")) l += "  [repeats]";
        int nord = order_count.count(o.uid()) ? order_count[o.uid()] : 0;
        l += "  - " + std::to_string(nact) + " action" + (nact == 1 ? "" : "s") + (nord ? ", " + std::to_string(nord) + " order" + (nord == 1 ? "" : "s") : "");
        // Cut at the panel edge with "..." (the whole line and the level's own name on hover).
        std::string full = l;
        const float room = ImGui::GetContentRegionAvail().x;
        const bool cut = ImGui::CalcTextSize(l.c_str()).x > room;
        if (cut) {
            while (l.size() > 4 && ImGui::CalcTextSize((l + "...").c_str()).x > room) l.pop_back();
            l += "...";
        }
        ImGui::PushID(r.idx);
        // A trigger just made (or picked on the map) scrolls into view: the list starts with the level's own retail ones,
        // so a new one used to be below the fold and the newcomer saw nothing happen.
        static uint32_t scrolled_to = 0;
        if (o.uid() == trig_uid_ && scrolled_to != trig_uid_) { ImGui::SetScrollHereY(0.5f); scrolled_to = trig_uid_; }
        if (ImGui::Selectable(l.c_str(), o.uid() == trig_uid_)) {
            trig_scroll_to_ = true;  // bring its editor into view below
            trig_uid_ = o.uid();
            select_uid(o.uid());
            tool_ = tool_ == Tool::PickPoint || tool_ == Tool::PlaceWaypoint ? Tool::Select : tool_;
            pick_ = PickTarget();
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
    // A trigger just made from New trigger... (review B9) or picked in the list (BB6): bring its When / Then do editor into view.
    if (g_show_trig_editor || trig_scroll_to_) { ImGui::SetScrollHereY(0.0f); g_show_trig_editor = false; trig_scroll_to_ = false; }
    {
        Dis d(ro);
        std::string nm = tr.description();
        form_label("Name", form_col());
        if (ImGui::InputText("##tname", &nm)) { touch("rename trigger"); tr.set_str("Description", nm); }
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
            if (ImGui::InputFloat("##tsec", &sec, 0, 0, "%.1f")) { touch("trigger timer"); tr.set_f32("TrgTimerDurationSeconds", sec); tr.set_f32("TrgTimerDuration", sec / 60.f); }
            ImGui::SetItemTooltip("Counted from when this trigger is switched on (0 = as the mission starts).");
        } else if (tcls == "MATriggerRadius" || tcls == "MATriggerCuboidalVolume") {
            bool box = tcls == "MATriggerCuboidalVolume";
            if (box) {
                f_float(tr, "VolumeWidth", "Width (m)");
                f_float(tr, "VolumeLength", "Length (m)");
                f_float(tr, "VolumeHeight", "Height (m)");
                f_float(tr, "VolumeHeading", "Turn (degrees)");
                f_enum(tr, "VolumeStateTest", "Fires when", {{1, "Something is inside"}, {2, "Something is outside"}});
            } else {
                f_float(tr, "EncounterRadius", "Radius (m)");
                f_bool(tr, "InRadiusCheck", "Fires when something is inside (untick: outside)");
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
                if (ImGui::BeginCombo("##units", lab.c_str())) {
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
            // B19: the old line ("Who triggers it ... is under Properties") contradicted the picker above.
            hint_text("More filters (which side, players only) are under Properties.");
        } else if (tcls == "MATriggerDestroyed") {
            f_enum(tr, "CombineMethod", "Fires when", {{2, "All of the units are lost"}, {1, "Any one of the units is lost"}});
            f_bool(tr, "AllowJacking", "Stealing a unit counts too");
            f_int(tr, "DamageLevel", "Damage % (0 = destroyed)", "Fire when a unit is damaged this much instead of destroyed.");
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
            if (refs.empty() && !tr.u32("DestroySet")) {  // "Include player units" alone also fires at load (dogfood D11)
                ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().error);
                ImGui::TextWrapped("No units yet, so this fires the moment the mission starts. Click the units on the map, then press Add selected.");
                ImGui::PopStyleColor();
            }
            if (ImGui::SmallButton("Add selected")) {
                std::vector<uint32_t> add;
                for (uint32_t x : selection_) {
                    bool have = false;
                    for (uint32_t r : refs) { const WObj* q = object_by_uid(w, r); have |= q && q->u32("ReferenceUID") == x; }
                    if (!have && x != tr.uid()) add.push_back(x);
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
            if (ImGui::BeginCombo("##addin", "Add input...", ImGuiComboFlags_HeightLarge)) {
                for (auto& r : rows) {
                    const WObj& x = w.objs[size_t(r.idx)];
                    if (x.uid() == tr.uid()) continue;
                    std::string l = trigger_name(x) + "##" + std::to_string(x.uid());
                    if (ImGui::Selectable(l.c_str())) {
                        uint32_t me = tr.uid(), tg = x.uid();
                        later = [this, me, tg]() { push_undo("add input"); add_reference(*doc_, me, tg); world_changed(); };
                    }
                }
                ImGui::EndCombo();
            }
            if (rm) later = [this, rm]() { push_undo("remove input"); wedit::remove_subtree(doc_->world, rm); world_changed(); };
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
        std::string head = std::to_string(ai + 1) + ". " + kind_label(a);
        bool open = ImGui::TreeNodeEx("##a", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "%s", head.c_str());
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
            if (ac == "MAActionBroadcast") {
                text_field(a, "BodyTextID", "Message", false);
                // The game never draws the caption (t16-triggers-sets-surprise-4): only kept for a message that already has one.
                if (a.u32("CaptionTextID")) text_field(a, "CaptionTextID", "Title (the game does not show it)", true);
                f_float(a, "TimeDisplayedFor", "Seconds shown", "How long the message stays on screen; 0 = until the player closes it.");  // the long label was cut off by the label column
                f_bool(a, "PauseGame", "Pause the game while it shows");
            } else if (ac == "MAActionSound") {
                cue_combo(a, "CueEvent", false);
            } else if (ac == "MAActionPlayVoice") {
                cue_combo(a, "CueID", false, true);
            } else if (ac == "MAActionMusic") {
                cue_combo(a, "MusicCue", true);
                f_int(a, "IntensityLevel", "Intensity");
            } else if (ac == "MAActionMissionVariableSet") {
                int var = a.i32("MissionVariableSet");
                int cur = -1;  // the objective whose MARoot slot uses this variable (retail slots use other numbers)
                for (int i = 0; i < int(s.objectives.size()) && cur < 0; i++) if (objective_var(s, i) == var) cur = i;
                std::string lab = cur >= 0 ? std::to_string(cur + 1) + ". " + s.objectives[size_t(cur)] : "variable " + std::to_string(var);
                form_label("Objective", form_col());
                if (ImGui::BeginCombo("##objective", lab.c_str())) {
                    for (int i = 0; i < int(s.objectives.size()); i++)
                        if (ImGui::Selectable((std::to_string(i + 1) + ". " + s.objectives[size_t(i)]).c_str(), i == cur)) {
                            touch("trigger setting");
                            a.set_i32("MissionVariableSet", claim_objective_var(s, i));
                            g_mark_trigger = tr.uid();  // its radar target, next frame (see the top of tab_triggers)
                            g_mark_objective = i;
                        }
                    ImGui::EndCombo();
                }
                if (s.objectives.empty()) ImGui::TextDisabled("Add objectives on the Objectives & Triggers tab.");
                f_int(a, "MissionVariableSet", "Variable (advanced)");
                f_bool(a, "VariableState", "Set (untick: clear)");
            } else if (ac == "MAActionGameCondition") {
                f_enum(a, "GameCondition", "Result", {{2, "Win the mission"}, {1, "Lose the mission"}});
            } else if (ac == "MAActionShow") {
                f_link(a, "ActionEntity", "Unit:", false, "AISet");
                f_bool(a, "Enable", "Show it (untick: hide it)");
            } else if (ac == "MAActionRemoveEntity") {
                f_link(a, "ActionEntity", "Unit:", false, "ActionSet");
                f_bool(a, "ShowKillEffect", "With an explosion");
            } else if (ac == "MAActionAI") {
                f_link(a, "ActionEntity", "Unit or trigger:", true, "AISet");
                f_bool(a, "EnableAI", "Switch on (untick: off)");
            } else if (ac == "MAActionActivateCinema") {
                uint32_t cur = a.u32("cinemaID");
                static const uint32_t K_CINEMA = key_of("MACinema");
                std::string lab = cur ? unit_label(w, cur) : "(choose)";
                form_label("Cutscene", form_col());
                if (ImGui::BeginCombo("##cutscene", lab.c_str())) {
                    for (auto& o : w.objs)
                        if (o.cls == K_CINEMA) {
                            std::string l = (o.description().empty() ? "Cutscene #" + std::to_string(o.uid()) : o.description()) + "##" + std::to_string(o.uid());
                            if (ImGui::Selectable(l.c_str(), o.uid() == cur)) { touch("trigger setting"); a.set_u32("cinemaID", o.uid()); }
                        }
                    ImGui::EndCombo();
                }
                uint32_t me = a.uid();
                if (ImGui::SmallButton("New cutscene")) {
                    later = [this, me]() {
                        push_undo("new cutscene");
                        if (uint32_t u = new_cutscene()) if (WObj* o = object_by_uid(doc_->world, me)) o->set_u32("cinemaID", u);
                    };
                }
                ImGui::SetItemTooltip("Builds a real cutscene (camera, shot, flight path, focus marker) at the view centre and picks it.");
            } else if (ac == "MAActionTeleport") {
                uint32_t mk = a.u32("MarkerUID");
                ImGui::Text("Destination: %s", mk ? "a marker on the map" : "(none yet)");
                ImGui::SameLine();
                if (mk && ImGui::SmallButton("Select")) { select_uid(mk); go_to(mk); }
                ImGui::SameLine();
                if (theme::small_toggle_button("Pick on map", picking(PickTarget::LinkMarker, a.uid()))) {
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::LinkMarker;
                    pick_.uid = a.uid();
                    pick_.key = "MarkerUID";
                    pick_.null_marker = false;
                    pick_.name = trigger_name(tr) + " - teleport marker";
                    tool_ = Tool::PickPoint;
                }
                ImGui::SetItemTooltip("Then click the map where the player should appear.");
                f_float(a, "Heading", "Facing (degrees)");
            } else if (ac == "MAActionNation") {
                f_link(a, "ActionEntity", "Unit:", false, "AISet");
                f_enum(a, "Nation", "Side", {{6, "Player's side"}, {5, "Enemy"}, {0, "Neutral"}});
            } else if (ac == "MAActionCheckPointSave") {
                f_str(a, "suffix", "Name");
            } else if (ac == "MAActionSwapSetIn" || ac == "MAActionSwapSetOut") {
                static const uint32_t K_SWAPSET = key_of("MASwapSet");
                uint32_t cur = a.u32("dwSwapSet");
                std::string lab = cur ? unit_label(w, cur) : "(choose)";
                form_label("Swap set", form_col());
                if (ImGui::BeginCombo("##swapset", lab.c_str())) {
                    for (auto& o : w.objs)
                        if (o.cls == K_SWAPSET) {
                            std::string l = (o.description().empty() ? "Swap set #" + std::to_string(o.uid()) : o.description()) + "##" + std::to_string(o.uid());
                            if (ImGui::Selectable(l.c_str(), o.uid() == cur)) { touch("trigger setting"); a.set_u32("dwSwapSet", o.uid()); }
                        }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled(ac == "MAActionSwapSetIn" ? "Its units appear in the level." : "Its units leave the level.");
                uint32_t me = a.uid(), cu = cur;
                if (cu && ImGui::SmallButton("Edit its units")) {
                    // Opens the Sets view on the set the swap set swaps (members list + Add selected).
                    const WObj* so = object_by_uid(w, cu);
                    uint32_t es = so ? so->u32("dwEntitySwapSet") : 0;
                    if (es) { set_uid_ = es; trig_mode_ = 3; }
                    select_uid(cu);
                }
                ImGui::SetItemTooltip("Opens the Sets view on this swap set's units: select units on the map, then press Add selected.");
                if (cu) ImGui::SameLine();
                if (ImGui::SmallButton("New swap set")) {
                    later = [this, me]() {
                        push_undo("new swap set");
                        Scenario& sc = *doc_;
                        uint32_t next = next_uid(sc.world);
                        uint32_t grp = ensure_group(sc.world, root_uid(sc.world), "Swap Sets", next);
                        uint32_t sw = io_->add_part(sc, key_of("MASwapSet"), grp, nullptr);
                        uint32_t set = add_entity_set("New swap set");
                        if (sw && set) {
                            if (WObj* o = object_by_uid(sc.world, sw)) { o->set_str("Description", "New swap set"); o->set_u32("dwEntitySwapSet", set); o->set_i32("bSwappedIn", 0); }
                            if (WObj* o = object_by_uid(sc.world, me)) o->set_u32("dwSwapSet", sw);
                            selection_ = {sw};
                        }
                        world_changed();
                    };
                }
                ImGui::SetItemTooltip("Creates an empty swap set (a MASwapSet and its entity set) and picks it; then add its units in Properties.");
            } else {
                ui_common_fields(a);
            }
            ImGui::SameLine();
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
        if (ImGui::BeginCombo("##adda", "Add action...", ImGuiComboFlags_HeightLarge)) {
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
                            world_changed();
                            // A new 'Complete objective' names objective 1 at once: its radar target follows next frame.
                            if (const WObj* ao = a && cls == "MAActionMissionVariableSet" ? object_by_uid(doc_->world, a) : nullptr)
                                for (int i = 0; i < int(doc_->objectives.size()); i++)
                                    if (objective_var(*doc_, i) == ao->i32("MissionVariableSet")) { g_mark_trigger = me; g_mark_objective = i; break; }
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
                        later = [this, me, cls]() { push_undo("add action"); io_->add_part(*doc_, cls, me, nullptr); world_changed(); };
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
        std::string head = unit_label(w, unit) + ": " + (bc == "MAPatrol" ? "patrol" : bc == "MADefend" ? "defend a point" : bc == "MAFollow" ? "follow" : bc == "MAAttack" ? "attack" : "(no order)");
        bool open = ImGui::TreeNodeEx("##o", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "%s", head.c_str());
        ImGui::SameLine(std::max(40.f * dpi_, ImGui::GetContentRegionAvail().x - 20 * dpi_));
        {
            Dis d(ro);
            if (ImGui::SmallButton("x")) rm_order = ev;
            ImGui::SetItemTooltip("Remove these orders (and their patrol path).");
        }
        if (open) {
            if (ImGui::SmallButton("Select unit") && unit) { select_uid(unit); go_to(unit); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Properties")) select_uid(ev);
            Dis d(ro);
            static const char* ocls[] = {"MAPatrol", "MADefend", "MAFollow", "MAAttack"};
            static const char* olab[] = {"Patrol along a path", "Defend a point", "Follow (the player or a unit)", "Attack (the player or a unit)"};
            int cur = -1;
            for (int k = 0; k < 4; k++) if (bc == ocls[k]) cur = k;
            form_label("Order", form_col());
            if (ImGui::BeginCombo("##order", cur >= 0 ? olab[cur] : "(none)")) {
                for (int k = 0; k < 4; k++)
                    if (ImGui::Selectable(olab[k], k == cur)) {
                        later = [this, ev, k]() { push_undo("change order"); set_order_kind(*doc_, ev, ocls[k]); world_changed(); };
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
                ImGui::SetItemTooltip("Then click the map where the unit should hold.");
                if (ImGui::TreeNode("Advanced##defend")) {
                    f_float(*b, "Radius", "Radius (m)");
                    ImGui::SetItemTooltip("Kept as the game's missions store it, but the game does not read it: changing it does nothing in the game.");
                    ImGui::TreePop();
                }
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
        Dis d(!unit_ok);
        if (ImGui::Button("Give the selected unit orders")) {
            uint32_t me = tr.uid(), unit = *selection_.begin();
            later = [this, me, unit]() {
                push_undo("unit orders");
                if (uint32_t ev = add_order(*doc_, me, unit, "MAPatrol")) {  // patrol, as the other order buttons: draw its path next
                    pick_ = PickTarget();
                    pick_.kind = PickTarget::OrderPath;
                    pick_.uid = ev;
                    tool_ = Tool::PlaceWaypoint;
                }
                world_changed();
            };
        }
        ImGui::SetItemTooltip("Select one unit on the map first. It patrols: click the map once per patrol point, right-click when done. Change the order below.");
    }

    if (tool_ == Tool::PickPoint) ImGui::TextColored(theme::tok().accent, "Click the map to set the point (right-click cancels).");
    if (tool_ == Tool::PlaceWaypoint) ImGui::TextColored(theme::tok().accent, "Click the map to add path points; right-click when done.");
    finish();
}

}  // namespace ed
