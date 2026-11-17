// Cutscene model: one reader for a MACinema's shots that the preview, the timeline and the Check all use, and the
// edits the panels make (a shot's length, where its camera stands and looks). Everything is the game's own objects: shot k =
// MAFreeCameraController k (play order = sibling order) with a camera, a start / end trigger, a type-2 step (eyeUID = the eye
// MAPath > MAWaypointGroup > MAWaypoints) and a type-3 step (focusUID = the marker the camera looks at). Nothing here pushes
// undo; the app wraps one call per gesture.
#pragma once
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "names.h"
#include "scenario.h"

namespace ma2 {

struct ShotInfo {
    uint32_t ctrl = 0, cam = 0, init = 0, exit = 0, eye_path = 0, focus = 0;  // objects (eye_path 0 = none left: the shot is still)
    float start = 0;      // seconds from the cutscene's start
    float len = 8;        // CameraDuration, else the eye path's duration, else the game's 8 s
    float move_len = 0;   // the eye path's own duration (0 without a path); shorter than len = "moves, then holds"
    float fov = 48;       // degrees (MAFreeCamera Field of view)
    std::vector<Vec3> eye;          // the eye path's points, else the camera's own spot
    std::vector<Vec3> focus_pts;    // what it looks at (usually one point)
    Vec3 look{};                    // focus_pts.front()
    bool has_look = false;
    bool still = true;              // no path, or a path that goes nowhere (under 10 cm)
};

constexpr float kShotMin = 0.5f, kShotMax = 120.f, kShotDefault = 5.f;
constexpr float kGameBandAspect = 2.35f;  // the game's picture between its letterbox bars (545 of 960 lines at 1280 x 960)
// The game's Field of view is the horizontal angle across the picture band (wiki t45: a letterboxed shot keeps the authored horizontal
// extent). The map view shows the band across its width `band_w` pixels and is `view_h` pixels tall, so the editor camera's vertical
// angle (radians) is tan(v/2) = tan(h/2) * view_h / band_w. One function for the preview, the framing view and the glyphs.
inline float game_fov_to_editor(float deg, float band_w, float view_h) {
    const float h = std::min(std::max(deg, 5.f), 120.f) * 0.0174532925f;
    return 2.f * std::atan(std::tan(h * 0.5f) * std::max(view_h, 1.f) / std::max(band_w, 1.f));
}
inline float editor_fov_to_game(float rad, float band_w, float view_h) {
    return 2.f * std::atan(std::tan(rad * 0.5f) * std::max(band_w, 1.f) / std::max(view_h, 1.f)) * 57.2957795f;
}
constexpr float kStillGap = 0.02f;  // a still shot's two eye points, metres apart

// The shots of a cinema in play order, with their start times. Empty when `cinema` is not a MACinema.
std::vector<ShotInfo> cutscene_shots(const Wdf& w, uint32_t cinema);
std::vector<ShotInfo> cutscene_shots(const Wdf& w, const WTree& tree, uint32_t cinema);  // with a tree the caller already built

float cutscene_total(const std::vector<ShotInfo>& shots);

// The shot's length, clamped to 0.5-120 s. The eye path's own duration follows it when the two were equal before
// ("linked"); otherwise the path keeps its own and the shot reads "moves, then holds". False when `ctrl` is not a shot.
bool set_shot_length(Scenario& s, uint32_t ctrl, float sec);

// Where a shot's camera stands (absolute heights). which 0 = still: the eye path becomes two points kStillGap apart at the
// eye, the camera object goes there too; 1 = start: the first eye point (and the camera); 2 = end: the last eye point (a
// one-point path gets an end point). set_look moves the focus marker to `look`. When the shot has no eye path and `io` is
// given, ensure_shot_eye_path rebuilds one first. False when nothing could be written.
bool set_shot_view(Scenario& s, uint32_t ctrl, int which, Vec3 eye, Vec3 look, bool set_look, ScenarioIO* io = nullptr);

bool set_shot_fov(Scenario& s, uint32_t ctrl, float deg);

// The shot's CameraDuration was edited in place (Properties) from old_len to its new value: the eye path's own duration follows when it
// was equal to the old length ("linked"), as the panel and the strip do through set_shot_length. False when nothing was linked.
bool carry_path_duration(Scenario& s, uint32_t ctrl, float old_len);

// The eye path's own duration ("the camera moves for X s, then holds"), clamped to 0.5 s .. the shot's length. "Move over the whole
// shot" = set_shot_move_len(len). False when the shot has no eye path object.
bool set_shot_move_len(Scenario& s, uint32_t ctrl, float sec);

// What the shot looks at: the focus marker (or every point of a focus path) moves to `look`. False when there is no focus left.
bool set_shot_look(Scenario& s, uint32_t ctrl, Vec3 look);

// How long a moving shot holds still after its camera move ends (0 for a still shot, or a move that spans the whole shot).
inline float shot_hold(const ShotInfo& sh) { return !sh.still && sh.move_len > 0.05f && sh.len - sh.move_len > 0.05f ? sh.len - sh.move_len : 0.f; }

// (game code, MAPath start 0x15705D): when a path's waypoint group hangs under an object with a place (an MAEntity with the
// spatial flag: the template's "flyby" MADummyObj, a MAFreeCamera), the game's path STARTS at that object, then runs through the
// waypoints in order (retail flybys rely on it: Mission_05 "IcarusDead flyby focus 1", Mission_02 cameras). A plain MAGroup parent
// adds nothing. True and `at` = the owner's spot when the group (index in `t`) has such an owner.
bool path_owner_point(const Wdf& w, const WTree& t, int group, Vec3& at);
// Moves a path's waypoint group from under such an owner into the cinema's "Paths" group, so its waypoints alone are the path; an
// owner MADummyObj left empty that nothing links to is removed. True when it moved. set_shot_view / set_shot_look /
// ensure_shot_eye_path / camera_path_point call it, so every authored shot plays its own points. (The focus step's Offset is
// added to the focus spot by the game, 0x1C9892; set_shot_view(set_look) and set_shot_look zero it, the reader adds it.)
bool detach_path_group(Wdf& w, uint32_t path);

// A shot made before still flies from the template's flyby marker (its eye path's waypoint group hangs under an object with a place),
// and looks 10 m above its focus (the focus step's Offset). `shot_hangs_under_marker` finds such a shot; `cutscene_play_from_own_view`
// is the one-click fix (detach_path_group on its eye / focus path, Offset zeroed): its own points are then the whole path. True when changed.
bool shot_hangs_under_marker(const Wdf& w, const WTree& t, const ShotInfo& sh);
bool cutscene_play_from_own_view(Scenario& s, uint32_t ctrl);

// The cinema (MACinema uid) a shot controller belongs to, 0 when none.
uint32_t shot_cinema(const Wdf& w, uint32_t ctrl);

// What a shot's start / end trigger carries: an action (MAAction*) or an order (MAEvent).
bool is_trigger_action(const NameTable& names, const WObj& o);
// A cutscene's end actions belong to its LAST shot (2.1): moves every action and order under trigger `from` to `to` (with what
// hangs under them). Returns how many moved. Call when the last shot changes (shot added, deleted or reordered).
int cutscene_move_end_actions(Scenario& s, const NameTable& names, uint32_t from_trigger, uint32_t to_trigger);

// The triggers a cutscene owns: every shot's start / end trigger and the cue timers (a disabled MATriggerTimer a
// shot trigger switches on with an MAActionAI). They live on the timeline, not in the Triggers list.
std::set<uint32_t> cutscene_owned_triggers(const Wdf& w);


// ---- Events on the timeline. An event is an action (or an AI order) the cutscene fires at a moment: shot k + o seconds. At o = 0
// it hangs under the shot's start trigger (triggerInit); later it hangs under a cue timer, a disabled MATriggerTimer ("<cutscene> - shot k +
// o s", group "Cutscene timers", Repeat on) that an MAActionAI under triggerInit switches on and whose last action switches it off again.
// Nothing new is saved: a cue timer is recognised by that structure alone (cutscene_owned_triggers).
struct CutEvent {
    uint32_t obj = 0;      // the action (MAAction*) or the order (MAEvent under its unit)
    bool order = false;
    int shot = 0;          // 0-based
    float offset = 0;      // seconds into that shot
    float t = 0;           // seconds from the cutscene's start (shot start + offset)
    uint32_t holder = 0;   // the trigger it hangs under: the shot's start trigger or a cue timer; for an end action the last shot's end trigger
    uint32_t unit = 0;     // order: the unit it is given to
};
// Every event of the cutscene in time order (end actions are cutscene_end_actions). Plumbing (the actions that start / stop cue timers and
// wake a unit for its order) is not listed.
std::vector<CutEvent> cutscene_events(const Wdf& w, const NameTable& names, uint32_t cinema);
// What runs when the last shot ends: actions and orders under its end trigger (t = the total).
std::vector<CutEvent> cutscene_end_actions(const Wdf& w, const NameTable& names, uint32_t cinema);
// Which shot a moment falls in and the offset into it (past the end: the last shot, offset beyond its length). False without shots.
bool cutscene_locate(const std::vector<ShotInfo>& shots, float t, int& shot, float& offset);
// The trigger an event at (shot, offset) hangs under: the start trigger when offset < 0.05, else the shot's cue timer for that offset
// (made when missing and `create`). 0 when it cannot be made.
uint32_t cutscene_event_holder(Scenario& s, uint32_t cinema, int shot, float offset, bool create = true);
// A new action of class `cls` ("MAActionBroadcast"...) at second t; returns its uid (0: no prototype). A message gets text "New message".
uint32_t cutscene_add_action(Scenario& s, uint32_t cinema, float t, const char* cls, const std::string& name);
// A new AI order for `unit` starting at second t (behaviour_cls MAPatrol / MADefend / MAFollow / MAAttack); returns the MAEvent (0 on failure).
// A MAPatrol order starts with an empty route (walk once, or loop it): the points are drawn on the map afterwards; Check flags a route under 2 points.
uint32_t cutscene_add_order(Scenario& s, uint32_t cinema, float t, uint32_t unit, const char* behaviour_cls, bool loop = false);
// The same under the last shot's end trigger (what runs when the cutscene ends).
uint32_t cutscene_add_end_action(Scenario& s, uint32_t cinema, const char* cls, const std::string& name);
// Retimes an event (an empty cue timer it leaves goes with its start action). False when `obj` is not an event of the cutscene.
bool cutscene_move_event(Scenario& s, const NameTable& names, uint32_t cinema, uint32_t obj, float t);
// Deletes an event (an order takes its path with it); an emptied cue timer and its start action go too. False when not an event.
bool cutscene_remove_event(Scenario& s, const NameTable& names, uint32_t cinema, uint32_t obj);
// The cue timers of one shot: {timer uid, offset seconds}, started from its start trigger.
std::vector<std::pair<uint32_t, float>> cutscene_cue_timers(const Wdf& w, uint32_t init_trigger);
// A shot is going away: its cue timers and the orders that wait for its triggers go with it (what hangs under the triggers goes with them).
void cutscene_drop_shot_triggers(Scenario& s, uint32_t init_trigger, uint32_t exit_trigger, const std::vector<uint32_t>& cue_timers);

}  // namespace ma2
