// Mission extras: objects and texts the export adds to the copy of the world the game gets, never to the
// document (add to them). Everything generated is recognisable and removed when a file is
// opened, so a reopened map is exactly the authored one:
//   objects   Description starts with "[gen] " (the "[ed] " prefix would be adopted as the user's own by
//             convert_legacy_triggers); the generated triggers sit in the root group "[gen] Mission extras";
//             generated actions under a trigger of the user's carry the prefix too
//   texts     ids kStrGenerated .. kStrGeneratedEnd-1 (triggers.h), never handed out by add_text
//   ini       scenario.ini [Generated]: "Body<generated id> = <original id>" for message bodies swapped at export
//            the game ignores sections it does not know
//   cutscenes "During this cutscene": the hold (units' AI off while it plays) is compiled into [gen] sets, timers,
//             logic triggers and actions; the choices live on the MACinema as EditorHold / EditorSkip (editor-only: erased
//             from the copy at export, kept in editor/project.json)
#pragma once
#include <map>
#include <string>
#include <vector>

#include "gamedata.h"
#include "wdf.h"

namespace ma2 {

struct Scenario;

extern const char* kGenPrefix;  // "[gen] "
extern const char* kGenGroup;   // "[gen] Mission extras"

// Export only: adds the generated objects to w (the copy being saved). `strings` receives the generated texts (n ->
// UTF-8, ids from kStrGenerated); `generated_ini` receives the lines of scenario.ini [Generated].
void add_mission_extras(const Scenario& s, Wdf& w, std::map<int, std::string>& strings, std::vector<std::string>& generated_ini,
                        std::vector<std::string>* warnings);

// Open: restores swapped message bodies from [Generated], removes every "[gen] " object (with its subtree), drops the
// texts kStrGenerated... Returns how many objects were removed. Does not touch s.dirty.
int strip_mission_extras(Scenario& s, const IniFile& ini);

// The briefing as the player reads it in the mission: pages of the title and the text, each short enough for the
// game's message box (about six lines of 17-20 characters, measured by D0 Q4).
struct BriefingPage { std::string text; float seconds = 4; };
std::vector<BriefingPage> briefing_pages(const std::string& title, const std::string& text);

// What the player reads for a message with a speaker: "Speaker: text"; the text alone when the speaker is empty.
std::string speaker_line(const std::string& speaker, const std::string& text);

// When the player reads the briefing, in seconds from the mission start: it starts when the intro cutscene that plays at the
// start is over (0 without one) and runs for its pages. False when the mission has no briefing. `intro` = how long the intro takes.
bool briefing_span(const Scenario& s, float& from, float& to, float* intro = nullptr);
// How long the intro cutscene that plays when the mission starts takes (a timer of one second or less plays it); 0 without one.
float intro_seconds(const Wdf& w);
// The cinema that intro is (the one a timer of one second or less plays at the start); 0 without one.
uint32_t intro_cinema(const Wdf& w);

// The Custom Scenarios list text derived from the briefing when the map has no description of its own:
// "Title: text" on one line, cut at 240 characters with "...". Empty when there is no briefing.
std::string briefing_description(const std::string& title, const std::string& text);

// ----------------------------------------------------------------------------- cutscene safety
constexpr const char* kHoldProp = "EditorHold";  // MACinema: 0 nothing, 1 enemies (default, absent), 2 everyone but the player
constexpr const char* kSkipProp = "EditorSkip";  // MACinema: 0 = the player cannot skip it; absent = can
int cutscene_hold(const WObj& cinema);
bool cutscene_skippable(const WObj& cinema);
void set_cutscene_hold(WObj& cinema, int hold);
void set_cutscene_skippable(WObj& cinema, bool skippable);
// The MACinema uids that cannot be skipped (scenario.ini [Cutscenes] NoSkip), in file order.
std::vector<uint32_t> cutscene_noskip_uids(const Wdf& w);
// Check scenario lines for the holds: {text, uid}.
std::vector<std::pair<std::string, uint32_t>> cutscene_hold_notes(const Wdf& w);
// The units a cutscene's hold switches off (hold 1: Nation 5, hold 2: every side but neutral), AI-capable units only.
std::vector<uint32_t> cutscene_hold_units(const Wdf& w, int hold);

}  // namespace ma2
