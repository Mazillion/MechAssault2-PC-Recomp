// Compiles the editor's AoE2-style triggers (Scenario::triggers) into native MechAssault 2
// trigger trees, learned from the 22 retail missions:
//
//   trigger      MATrigger* object; TriggerEnabled, TriggerAutoReset (loop); MAAction* children
//   conditions   one native trigger per condition; with several, an MATriggerLogic
//                (LogicType 0 = all, 1 = any; Inverse = not) lists them through
//                MoeEntityReference children and carries the actions
//   AI orders    unit -> MAAIGroup -> MAEvent (TriggerID = the trigger that starts it, Priority)
//                -> MAPatrol + MAWaypointGroup/MAWaypoint | MADefend (DefendUID marker, Radius) |
//                MAFollow (FollowUID, FollowRadius) | MAAttack (AttackUID + AttackPlayer 2 = player)
//   objectives   MARoot ObjectiveItemDesc%d (text id) + ob%dvar0 (mission variable); an
//                MAActionMissionVariableSet with VariableState 1 completes it
//   win / lose   MAActionGameCondition GameCondition 2 / 1
//
// Native objects are copies of retail instances ("prototypes") with the fields set. Everything
// compiled goes under the root group "Editor Triggers" or carries the "[ed] " description prefix
// inside units, and is rebuilt on every save.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "wdf.h"

namespace ma2 {

struct Scenario;
class GameData;

extern const char* kEditorTriggerGroup;  // "Editor Triggers"
extern const char* kEditorPrefix;        // "[ed] "
extern const char* kReachMarkerSuffix;   // " (radar marker)": a 'Reach a place' objective's marker, named after its circle
extern const char* kAIPathGroup;        // "AI Paths": patrol waypoint groups and defend/teleport markers the Triggers tab uses
extern const char* kTriggerShapeGroup;   // "Trigger Volumes": the trigger boxes and circles the Triggers tab uses

// Text ids the compiler allocates for inline text (n in [Strings]).
constexpr int kStrBriefingTitle = 60000, kStrBriefing = 60001, kStrObjective = 60100, kStrMessage = 61000;
constexpr uint32_t kTextBase = 0x00F00000u, kAudioBase = 0x5C000000u;

bool load_trigger_prototypes(GameData& gd, std::string* err = nullptr);
bool trigger_prototypes_ready();

// Compiles into w. extra_strings receives the inline text (n -> UTF-8) to add to [Strings].
void compile_triggers(const Scenario& s, Wdf& w, std::vector<std::string>* warnings, std::map<int, std::string>* extra_strings);

// Condition/effect kinds the editor offers.
struct ArgDef { const char* key; const char* label; const char* kind; const char* def; };
// kind: "float", "int", "text", "bool", "unit", "units", "point", "path", "trigger", "string", "clip",
//       "cue", "music", "objective", "order", "who"
struct TriggerKind { const char* id; const char* label; std::vector<ArgDef> args; };
const std::vector<TriggerKind>& condition_kinds();
const std::vector<TriggerKind>& effect_kinds();
const TriggerKind* find_condition_kind(const std::string& id);
// Height of a "box" condition's bottom edge above the ground at its centre. Flat squares and older
// files (no "base") are centred on the ground, so this is -height/2 for them.
float trigger_box_base(const std::map<std::string, std::string>& args);
const TriggerKind* find_effect_kind(const std::string& id);

// Trigger boxes and circles are real map objects, as in the retail missions: a box condition ("box")
// or circle condition ("area") names a MATriggerCuboidalVolume / MATriggerRadius by its UID in the
// argument "vol". The object (a copy of a retail one, same class and fields) holds the shape, is
// drawn, picked and edited like the retail ones, and on save becomes the trigger itself (the
// actions hang under it) or one input of the trigger's MATriggerLogic.
bool is_trigger_shape(uint32_t cls);
struct TriggerShape {
    bool box = true;
    float x = 0, z = 0;
    float base = 0;                    // box: bottom edge above the ground at the centre
    float w = 40, l = 40, h = 60, heading = 0;  // box size (m) and turn (degrees)
    float radius = 30;                 // circle
    bool inside = true;                // fire when inside (circle: InRadiusCheck)
    std::string name = "Trigger box";
};
// Adds the object under the "Trigger Volumes" group; returns its UID (0 when the prototypes are not loaded).
uint32_t add_trigger_shape(Scenario& s, const TriggerShape& sh);
// The object a box/circle condition uses (nullptr if it has none or it was deleted).
WObj* condition_shape(Scenario& s, const std::map<std::string, std::string>& args);
// Older files kept the shape in the condition's own arguments (point, width, base, flat, ...):
// gives each such condition its object (same position and size as the old compiled one) and drops
// those arguments. Returns how many were converted.
int adopt_trigger_shapes(Scenario& s);

// Patrol paths, defend points and teleport markers are real objects too (MAWaypointGroup with its
// MAWaypoints, a marker) under "AI Paths"; the effect's "wpt" / "marker" argument names them and
// the compile moves the path under the unit's MAEvent exactly like retail. Drawing or picking in
// the Triggers tab leaves a "path" / "point" argument that this turns into (or adds to / moves) the
// object. Older files' arguments are converted the same way. Returns how many were handled.
int adopt_ai_objects(Scenario& s);
uint32_t add_ai_path(Scenario& s, const std::vector<std::pair<float, float>>& pts, const std::string& name);
uint32_t add_ai_marker(Scenario& s, float x, float z, const std::string& name, bool null_marker);
void park_ai_objects(Wdf& w, const Scenario& s);
int ai_path_length(Scenario& s, uint32_t group);

// ----------------------------------------------------------------------------- native triggers
// The Triggers tab edits the world's own MATrigger* / MAAction* / MAEvent objects. These build the
// pieces from retail prototypes the way the retail levels have them.
extern const char* kTriggerGroup;  // "Triggers": where new triggers go
WObj* object_by_uid(Wdf& w, uint32_t uid);
const WObj* object_by_uid(const Wdf& w, uint32_t uid);
// A new trigger of a prototype class (MATriggerTimer, MATriggerDestroyed, MATriggerLogic; shapes: add_trigger_shape) under
// parent (0 = the "Triggers" group), enabled, no actions. Returns its UID (0: prototypes not loaded).
uint32_t add_trigger_object(Scenario& s, const char* cls, const std::string& name, uint32_t parent = 0);
// A new action (a prototype MAAction* class) as the last child of the trigger, with usable defaults.
uint32_t add_action_object(Scenario& s, uint32_t trigger_uid, const char* cls, const std::string& name);
// MoeEntityReference child of `parent` pointing at `target`: a MATriggerLogic input or a MATriggerDestroyed unit.
uint32_t add_reference(Scenario& s, uint32_t parent, uint32_t target);
std::vector<uint32_t> reference_children(const Wdf& w, uint32_t uid);
// Objective radar targets: an MAObjectiveType child (ObjectiveNumber = the objective's slot) under each
// target unit, as in the retail missions; the save lists them on MARoot (NumObjectiveObjects%d, ob%dobj%d).
std::vector<uint32_t> objective_targets(const Wdf& w, int objective);  // the target units, in world order
uint32_t add_objective_target(Scenario& s, uint32_t unit_uid, int objective);  // no-op if already a target; 0: no prototype
// Puts each '<circle name> (radar marker)' back at the centre of the trigger circle/box of that name (it was moved since).
void sync_reach_markers(Scenario& s);
int remove_objective_targets(Scenario& s, int objective, uint32_t unit_uid = 0);  // unit 0 = all; returns how many
void objective_removed(Scenario& s, int objective);  // its markers and MARoot slot go, later slots move up one
// The triggers whose actions complete an objective (MAActionMissionVariableSet with its variable, set; see objective_var).
std::vector<uint32_t> objective_completers(const Wdf& w, int var,const std::function<bool(uint32_t)>& is_trigger_cls);
// Objective slot i on MARoot. Its mission variable (ob%dvar0) belongs to the slot: retail levels use
// other numbers (Mission_03 slot 0 = variable 27), new maps use the slot index. A retail slot keeps the
// game's own text id until the user retypes the line; slots with no text are hidden steps.
struct ObjectiveSlot { int var = -1; uint32_t game_text = 0; bool hidden = false; };  // game_text: the level's own text id while the line is not retyped
ObjectiveSlot objective_slot(const Scenario& s, int i);
int objective_var(const Scenario& s, int i);          // the variable that completes objective i
int claim_objective_var(Scenario& s, int i);          // same, writing the slot index on MARoot when the slot has none
void load_retail_objectives(Scenario& s);             // lists a retail level's used slots (names = ObjectiveEditorName%d)
// "Keep these alive" objectives: the destroyed-triggers that lose the mission when one of the objective's radar targets dies.
std::vector<uint32_t> objective_losers(const Wdf& w, int objective);
// The map's own message texts: ids kTextBase + n with n in Scenario::strings.
uint32_t add_text(Scenario& s, const std::string& text);
std::string* own_text(Scenario& s, uint32_t text_id);  // nullptr for the game's own text ids

// AI orders: a MAEvent (TriggerID = the trigger that starts it) under the unit's MAAIGroup, with one
// behaviour child (MAPatrol / MADefend / MAFollow / MAAttack), as in the retail missions.
bool is_order_behaviour(uint32_t cls);
// The mission's player start: the MALevelStart named "Player Spawner" (the one the game uses), else the first one.
uint32_t player_start_uid(const Wdf& w);
std::vector<uint32_t> orders_of_trigger(const Wdf& w, uint32_t trigger_uid);
uint32_t order_unit(const Wdf& w, uint32_t event_uid);
uint32_t order_behaviour(const Wdf& w, uint32_t event_uid);
uint32_t order_path(const Wdf& w, uint32_t event_uid);   // the MAWaypointGroup under the event (0 = none)
uint32_t add_order(Scenario& s, uint32_t trigger_uid, uint32_t unit_uid, const char* behaviour_cls);
bool set_order_kind(Scenario& s, uint32_t event_uid, const char* behaviour_cls);
void order_add_point(Scenario& s, uint32_t event_uid, float x, float z);
void order_clear_path(Scenario& s, uint32_t event_uid);
// A cutscene eye path (MAPath -> its MAWaypointGroup): restart = the click replaces the whole path (its first point moves
// there, the others go); else a copy of the last point is appended. Keeps the points' height above the ground.
bool camera_path_point(Scenario& s, uint32_t path_uid, float x, float z, bool restart);
// Moves (or creates under "AI Paths") the marker that owner's link `key` names (MADefend DefendUID, MAActionTeleport MarkerUID).
uint32_t set_marker_link(Scenario& s, uint32_t owner_uid, const char* key, float x, float z, bool null_marker, const std::string& name);

// "Used by": what refers to an object, found by scanning the link keys (ActionEntity, MarkerUID,
// cinemaID, ReferenceUID, TriggerID, DefendUID, AttackUID, FollowUID). `trigger` = the MATrigger* the
// holder belongs to (0 = none); `unit` = the unit an order belongs to.
struct UsedBy {
    // Ref: another object points at it (m_wptGroupUID: an order's waypoint group). Enables: a spawner waits for this trigger (TriggerEnableID).
    enum Kind { Action, Input, Starts, OrderOf, OrderTarget, Member, SwapOf, Ref, Enables } kind;
    uint32_t holder = 0;   // the action / reference / MAEvent / behaviour that holds the link
    uint32_t trigger = 0;  // the trigger it is part of (Starts: the trigger the order waits for)
    uint32_t unit = 0;     // OrderOf / OrderTarget / Starts: the unit the order is given to
    std::string key;       // link key (Action, OrderTarget)
};
std::vector<UsedBy> used_by(const Wdf& w, uint32_t uid, const std::function<bool(uint32_t)>& is_trigger_cls);

// Files from before the Triggers tab worked on the world (editor/project.json "triggers", the
// hidden "Editor Triggers" group and "[ed] " objects): compiled once into ordinary objects, the
// list is cleared. Returns how many triggers were converted (-1: prototypes not loaded yet).
int convert_legacy_triggers(Scenario& s);

// Retail sound and music cues seen in the shipped missions (CueEvent / MusicCue values), with
// a label from the trigger that plays them.
struct CueInfo { uint32_t id; std::string label; };
const std::vector<CueInfo>& retail_sound_cues();
const std::vector<CueInfo>& retail_music_cues();
const std::vector<CueInfo>& retail_voice_cues();  // MAActionPlayVoice CueID values

}  // namespace ma2
