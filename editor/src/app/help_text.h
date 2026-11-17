// The editor's Help text: one tokenised text that the Help window, Getting Started and docs\Scenario editor.txt all
// read, so the keys it names are always the user's own. Header-only, no ImGui: tests\fmt_test.cpp checks every token and writes
// the guide from it.
//
// Tokens (the raw text never holds a literal key name):
//   {k:Snap}        the first key bound to an action (its ini name, see input_map.h); {k2:ZoomIn} both ("= or Num +")
//   {n:Snap}        the action's name as menus show it ("Put on the ground")
//   {hold.free}     the hold keys of Settings > Keys: free, through, quarter, reverse, orbit
//   {tab.1}..{tab.5}  the tab names, in strip order; {page.select} .. {page.starts} the tool names
//   {pan}           "W A S D or the arrow keys";  {nudge}  the four nudge keys;  {turn}  "Q and E"
//   {steep.bot} {steep.light}   the steep-ground limits in percent, read from core\brush.h
#pragma once
#include <string>
#include <vector>

#include "input_map.h"
#include "page_map.h"
#include "../core/brush.h"

namespace ed {

// What a token needs: the key bindings and the hold keys in force (Settings, or the defaults).
struct KeyText {
    KeyBind keys[A_COUNT][2];
    Hold hold;

    static KeyText defaults() {
        KeyText k;
        for (int a = 0; a < A_COUNT; a++)
            for (int s = 0; s < 2; s++) k.keys[a][s] = kActTable[a].def[s];
        return k;
    }
    // First bound slot as the screen shows it, or "" when the action has no key.
    std::string first(int act) const {
        if (act < 0 || act >= A_COUNT) return {};
        for (const KeyBind& b : keys[act])
            if (b.key) return key_label(b);
        return {};
    }
    std::string both(int act) const {
        if (act < 0 || act >= A_COUNT) return {};
        const std::string a = key_label(keys[act][0]), b = key_label(keys[act][1]);
        if (a.empty()) return b;
        return b.empty() || b == a ? a : a + " or " + b;
    }
    // Four actions as one list: "W A S D or the arrow keys" (second slots all arrows), else the first slots, else the second.
    std::string four(int a0, int a1, int a2, int a3) const {
        const int acts[4] = {a0, a1, a2, a3};
        const int arrows[4] = {ik::Up, ik::Left, ik::Down, ik::Right};
        std::string f, s;
        bool first_arrows = true, arrow_keys = true;
        for (int i = 0; i < 4; i++) {
            const KeyBind& x = keys[acts[i]][0];
            const KeyBind& y = keys[acts[i]][1];
            if (x.key) f += (f.empty() ? "" : " ") + key_label(x);
            if (y.key) s += (s.empty() ? "" : " ") + key_label(y);
            if (!(y.key == arrows[i] && !y.mods)) arrow_keys = false;
            if (!(x.key == arrows[i] && !x.mods)) first_arrows = false;
        }
        if (first_arrows) return "the arrow keys";
        if (f.empty()) f = s, s.clear();
        if (s.empty()) return f.empty() ? "no key" : f;
        return f + " or " + (arrow_keys ? std::string("the arrow keys") : s);
    }
    std::string pan() const { return four(A_PAN_FWD, A_PAN_LEFT, A_PAN_BACK, A_PAN_RIGHT); }
    std::string nudge() const { return four(A_NUDGE_FWD, A_NUDGE_LEFT, A_NUDGE_BACK, A_NUDGE_RIGHT); }
    std::string turn() const {
        const std::string l = first(A_TURN_LEFT), r = first(A_TURN_RIGHT);
        return (l.empty() ? "no key" : l) + " and " + (r.empty() ? "no key" : r);
    }
};

inline const char* hold_name(int m) { return m == KM_SHIFT ? "Shift" : m == KM_CTRL ? "Ctrl" : m == KM_ALT ? "Alt" : "no key"; }

inline const char* const kHelpTabs[5] = {"Map", "Build", "Objectives & Triggers", "Cameras & Cutscenes", "Environment"};

inline int help_act_by_ini(const std::string& ini) {
    for (int a = 0; a < A_COUNT; a++)
        if (ini == kActTable[a].ini) return a;
    return -1;
}

inline std::string help_pct(float v) { return std::to_string(int(v * 100.f + 0.5f)); }

// Resolves one token body ("k:Snap", "hold.free", "tab.2"); false when it names nothing.
inline bool help_token(const std::string& t, const KeyText& k, std::string* out) {
    const size_t c = t.find(':');
    if (c != std::string::npos) {
        const std::string kind = t.substr(0, c);
        const int a = help_act_by_ini(t.substr(c + 1));
        if (a < 0) return false;
        if (kind == "k") { const std::string s = k.first(a); *out = s.empty() ? "(no key)" : s; return true; }
        if (kind == "k2") { const std::string s = k.both(a); *out = s.empty() ? "(no key)" : s; return true; }
        if (kind == "n") { *out = kActTable[a].label; return true; }
        return false;
    }
    if (t == "hold.free") { *out = hold_name(k.hold.free); return true; }
    if (t == "hold.through") { *out = hold_name(k.hold.through); return true; }
    if (t == "hold.quarter") { *out = hold_name(k.hold.quarter); return true; }
    if (t == "hold.reverse") { *out = hold_name(k.hold.reverse); return true; }
    if (t == "hold.orbit") { *out = hold_name(k.hold.orbit); return true; }
    if (t.size() == 5 && t.compare(0, 4, "tab.") == 0 && t[4] >= '1' && t[4] <= '5') { *out = kHelpTabs[t[4] - '1']; return true; }
    static const char* const pages[kPageCount] = {"select", "place", "scatter", "roads", "sculpt", "paint", "water", "starts"};
    if (t.compare(0, 5, "page.") == 0)
        for (int i = 0; i < kPageCount; i++)
            if (t.substr(5) == pages[i]) { *out = page_name(Page(i)); return true; }
    if (t == "pan") { *out = k.pan(); return true; }
    if (t == "nudge") { *out = k.nudge(); return true; }
    if (t == "turn") { *out = k.turn(); return true; }
    if (t == "steep.bot") { *out = help_pct(ma2::kSteepBot); return true; }
    if (t == "steep.light") { *out = help_pct(ma2::kSteepLight); return true; }
    return false;
}

// Replaces every {token}; a token that resolves to nothing is left in place and listed in *bad.
inline std::string help_expand(const std::string& text, const KeyText& k, std::vector<std::string>* bad = nullptr) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            const size_t e = text.find('}', i);
            if (e != std::string::npos) {
                std::string r;
                if (help_token(text.substr(i + 1, e - i - 1), k, &r)) { out += r; i = e + 1; continue; }
                if (bad) bad->push_back(text.substr(i + 1, e - i - 1));
            }
        }
        out += text[i++];
    }
    return out;
}

struct HelpTopic {
    const char* title;
    const char* body;  // paragraphs separated by \n; a line starting "- " is a list item, "1. " a numbered one
};

// The topics, in the order the Help list and the guide show them. Check each line against the code and the result notes.
inline const std::vector<HelpTopic>& help_raw() {
    static const std::vector<HelpTopic> t = {
        {"Start here",
         R"HELP({n:New}... makes a multiplayer map you can play against bots: pick a size, a theme, and a start from flat ground, random hills or a copy of a real level. {n:NewMission}... makes a single-player mission with a player start, enemies, a briefing and objectives.
{n:Open}... opens a saved .ma2map from your scenario folder. Look at a real level... opens one of the game's own levels, read-only, so you can see how it is made; "Make an editable copy" on the {tab.1} tab then lets you change it.
The Next steps list (right side, under Properties) takes you through a first scenario: shape the ground, place the starts and units, add an objective for a mission, set the look and weather if you like, then test it.
Hover over any button to see what it does. {k:Help} opens this window; Help > Getting Started opens the first-run window again.)HELP"},

        {"Moving around",
         R"HELP(Pan: {pan} (hold Shift to go faster), or hold the right mouse button and drag. To scroll by pushing the mouse against a window edge, tick the edge-scrolling box in File > Settings > Camera.
Zoom: the mouse wheel, or {k2:ZoomIn} to zoom in and {k2:ZoomOut} to zoom out.
Turn the view: hold the middle mouse button and drag, or hold {hold.orbit} and drag with the left button (a click without a drag stays a click), or press {k:ViewLeft} / {k:ViewRight} to turn it and {k:ViewUp} / {k:ViewDown} to tilt it.
Top-down view: {k:TopDown} switches to it and back. The hills only show when the view is tilted.
{n:ResetView}: {k:ResetView}. {n:ZoomSelection}: {k:ZoomSelection}.
Click the small map (bottom right) to jump there.)HELP"},

        {"The screen",
         R"HELP(Along the top, the menus and the toolbar: Undo, Redo, Save and Test. Next to Test, on a multiplayer map, an arrow opens the test settings: game type, bots and their skill. At the right end of the toolbar are the Helpers button (the things the game never draws: trigger areas, paths, markers, lights, cameras; {k:Helpers} toggles them) and, on a multiplayer map, the game-type box, which shows only the objects the game uses in that type. It changes the view only.
The tabs follow the order you make a scenario in ({k:Tab1} to {k:Tab5}; {k:TabNext} and {k:TabPrevious} go to the next and the previous one):
- {tab.1}: name, description, size and game types.
- {tab.2}: everything you put on the map or shape on it. The tool strip down the left edge picks the tool: {page.select}, {page.place}, {page.scatter}, {page.roads}, {page.sculpt}, {page.paint}, {page.water} and {page.starts} ({k:PageSelect} to {k:PageStarts}).
- {tab.3}: the briefing and objectives the player reads, and what happens when (triggers, messages, orders).
- {tab.4}: camera paths and the cutscenes that play them; the timeline sits under the map.
- {tab.5}: the look (sky, light, fog) and the weather.
On the right: Properties for what is selected, and the Next steps list. The snap bar at the top left of the map holds Snap to joints, Snap to grid and Angle steps. The hint line at the bottom says what the tool in your hand does with the mouse and the keys.
File > Settings: folders, camera, keys (every shortcut can be changed), recovery copies, window size and Quick test.)HELP"},

        {"Placing",
         R"HELP(Pick what to place in the {page.place} page: search ({k:Find}), look through the groups, or drag a picture onto the map. A green ghost shows where it will go; the joined colour means it snaps to what it is next to; red means it cannot go there.
{k:TurnLeft} and {k:TurnRight} turn it ({hold.quarter} with them turns 90 degrees). While it is joined to something, the same keys pick another way to join.
Hold {hold.free} to place freely: no joints, no grid, no angle steps. Hold {hold.through} and click to put it on top of the object under the mouse (a roof, a deck, a rock).
Grid: {k:GridToggle} turns it on and off, {k:GridSmaller} and {k:GridBigger} change its size.
On a slope a flat piece can lie Level, Tilt with the ground or Float; {k:SlopeMode} cycles through them.
{k:PlaceAgain} places the last thing again; {k:PickType} picks the selected object's type. Right-click or {k:Cancel} stops placing.)HELP"},

        {"Roads, bases and layers",
         R"HELP(Flat pieces (road, base, parking lot) stack: the last one placed is on top. {k:BringForward} brings the selected piece forward and {k:SendBack} sends it back. Things you place stand on the top surface under them.
Edit > Put flat pieces in order puts a whole stack in a sensible order again.
Check scenario warns about pieces that float, are buried, or sit at the same height as another one.
Walls, fences, gates and bridges are in the {page.roads} page; they join end to end like roads do, and the same keys turn them.)HELP"},

        {"Selecting and changing",
         R"HELP(Click an object to select it. Shift+click adds to the selection, Ctrl+click toggles one. Drag from empty ground, or from an object that is not selected, to select everything in a box (Shift adds, Ctrl takes out). Drag a selected object to move it; it snaps the way placing does.
Double-click an object to select all of its type on screen (Ctrl+double-click: the whole map). Hold {hold.through} and click to reach the object behind. {k:InvertSelection} inverts the selection, {k:SelectAll} selects everything, {k:Cancel} clears it. Right-click an object for its menu.
Handles: {k:HandlesMove} Move, {k:HandlesRotate} Rotate, {k:HandlesTilt} Tilt; {k:HandlesNext} goes to the next one; the World / Local buttons next to the handle modes choose the axes. The lift handle stops at the ground unless you hold {hold.free}. Hold {hold.quarter} to turn the ring in steps.
The arrow keys ({nudge}) nudge 1 m (hold Shift for 10 m); {k2:Raise} raises and {k2:Lower} lowers.
{k:Snap}: {n:Snap}. {k:Delete}: delete. {k:Copy}, {k:Cut} and {k:Paste} copy, cut and paste (the paste follows the mouse until you click). {k:Duplicate}: duplicate.
Glue ties two joined pieces together so they move as one; Properties shows what is glued.)HELP"},

        {"Sizing pieces",
         R"HELP(Select one road, base, wall, fence, gate, bridge, canal wall or water in Move mode and orange handles appear on its sides. Drag one to cut the piece to size; cutting only makes it smaller. Double-click a handle to make that side whole again. The Size section of Properties shows the numbers.
Buildings and units keep their size. Cut pieces that can be destroyed are set to "Cannot be destroyed" by default, so a cut wall does not leave a gap you did not mean.)HELP"},

        {"Shaping the ground",
         R"HELP(The {page.sculpt} page has Raise / Lower, Plateau up / down, Smooth / Roughen, Flatten and Ramp. One stroke raises or lowers by the Height per stroke and no more, however long you drag or hold. Hold {hold.reverse} to use the other half of a pair. Tick "Keep raising while held" if you want the ground to keep rising while you hold the button.
Brush size: {k:BrushSmaller} and {k:BrushBigger}, or Ctrl+wheel; strength: Shift+wheel, or {k:BrushWeaker} and {k:BrushStronger}. The page says the smallest brush this map allows (a height point every few metres). Shift+click draws a straight stroke from the end of the last one.
Flatten: {hold.through}+click picks the height from the ground. Ramp: click where it starts and where it ends; the strip and its label say who can get up it.
Steep ground shows in colours when you tick "Show steep ground" (it comes on by itself while Ramp is armed):
- yellow, over {steep.bot}%: bots' 'Mechs won't walk up it (jump-jet 'Mechs may jet up a short face; Battle Armor and pilots manage up to {steep.light}%);
- orange, over {steep.light}%: very steep, bots won't route here; some 'Mechs may still climb it.
A slope of 100% is 45 degrees. Tilt the view to see the shape of the ground.
Things standing on the ground follow it when you sculpt (switch this off with "Objects follow the terrain").)HELP"},

        {"Painting",
         R"HELP(The {page.paint} page has Texture, Colour tint, Boundaries and Plants. A stroke paints once, up to its Strength; holding the mouse still adds nothing. Hold {hold.reverse} to reverse it: Texture paints the other texture (with more than two it rubs the chosen one out), Boundaries paints Playable, Plants clears instead of grows.
The ground you see is a texture coloured by the map's colour tint, so green ground can be cracked earth with a green tint.
{hold.through}+click picks the colour of the ground under the mouse for Colour tint.
Boundaries marks where players and bots may go (Playable) and where they may not (Border, Out of bounds).
Plants are the game's own ground plants (grass and bushes), which grow by themselves around the player; single trees, rocks or clumps placed as objects are the {page.scatter} tool. Plants from other levels are listed too; "Scatter it as objects" places such a plant as objects instead. More than two ground textures work only in this PC version of the game.)HELP"},

        {"Scatter",
         R"HELP(Pick the kinds of objects in the {page.scatter} page (the brush set), then drag on the map. The page sets the brush size, the density per hectare, the spacing, and whether to keep off roads, bases and buildings. Objects can face at random, lean up to a limit, follow the slope, and skip slopes steeper than a limit you choose. Objects have no size to set: for a different size, pick a different object.
One stroke places each spot once: holding the mouse still places nothing more. Shift+click draws a line of them. Start the stroke holding {hold.reverse} to remove the kinds in the brush set under the ring instead. {k:Undo} takes back the whole stroke.
From straight above trees look like dark shapes; tilt the view to see them.)HELP"},

        {"Water",
         R"HELP(In the {page.water} page press "Place water on the map" (picking a kind of water from its list does the same), then click the map for a 60 x 40 m pond, or drag out a size: a long drag makes a lake. Until you press it, a click on the map only selects. Right-click or {k:Cancel} stops placing. A new pond gets a basin dug under it. Handles resize it; the blue knob sets the water level (so does the Level box in Properties).)HELP"},

        {"Starts",
         R"HELP(The {page.starts} page puts down where players start: spawn points (6 Red and 6 Blue for team games, or free-for-all ones), vehicle pads, the objects each game type needs, and the player start in a mission.
Tick the game types on the {tab.1} tab. Check scenario says what is missing, and the Starts page has an "Add" button for each game type's objects:
- Team game types: at least 6 spawn points for Red and 6 for Blue.
- Free-for-all game types: enough free-for-all spawn points.
- Capture the Flag: a flag for each team.
- Base War: a power plant for each team.
- Snatch It: the bases and cores.
- Check It: checkpoints and terminals.
- When players start on foot: vehicle pads.)HELP"},

        {"Missions",
         R"HELP(On {tab.3}: write the briefing (the player sees it at the start of the mission, and in the game's Custom Scenarios list), then add objectives. Each one is completed by something: "Destroy these" (click the targets), "Reach a place", "Survive for 5 minutes", or "Other", which a trigger completes. Tick "Win the mission when every objective is done" to win without writing a trigger, and "Lose the mission if the time runs out" for a time limit (the clock shows on the HUD).
"When the mission ends" holds the "Mission complete" and "Mission failed" texts the player reads; leave them empty for the game's own words.
Triggers wait for something (units destroyed, an area reached, time, other triggers) and then act: show a message (with a speaker), play a sound, give units orders, bring in units after a delay, win or lose. The list has "Your triggers" and, for a real level's copy, "From the level". A trigger area can fire when the player is inside or outside it.
Orders: walk a path, defend a point, and so on; units on the Neutral side ignore orders.)HELP"},

        {"Cutscenes",
         R"HELP(On {tab.4}: New intro plays at 0 s, the first frame of the mission; New cutscene plays when a trigger tells it to. The timeline sits under the map.
Add shot makes a 5 s still shot from the view you are looking at. To aim a shot, press Frame start or Frame end, move the view, then press {k:FrameUse} to use it for the shot. Set the Length of each shot; "+ Add at" puts a message, a sound or an order at a moment on the timeline. Preview plays it in the editor.
"During this cutscene" holds the fight (enemies stand still and hold fire, by default) so nothing happens off camera, and says whether the player can skip it with Space. The editor view may be brighter than the game: turn off View > Brighten dark maps to see the game's brightness.)HELP"},

        {"Look and weather",
         R"HELP(The {tab.5} tab sets how the map looks. The editor never moves the view here; "Show the sky" tilts it to the horizon when you ask.
Look: Day, Evening or Night, and the Look list, copy the sky, sun, ambient and fog colours, the fog distance and the lightning from one of the game's own levels. "This map's own light" is what the map started with. "Building lights on" lights the windows and signs. Street lamps and the other lights of the map light the ground around them in this view when View > Scene lights is on. If you pick a night sky with a day look (or the other way round) an amber line says so; "Use a night look" or "Use a day look" fixes it, "Keep it" leaves it.
Weather: Clear, Light rain, Heavy rain, Snow and Dust storm. Lightning is its own box: a look sets it from its level, and changing the weather never touches it. A flash lights the models and the sky for a moment; the ground stays as it is. Dust storm is made by the editor: the game's own was never finished.
Fog: Default (the look's), Light, Medium and Thick, with the distances and the fog colour.
Sun or Moon: Direction (0 to 359 degrees) and Height. Update shadows redraws the hill shadows for the new sun; saving does it too.
Advanced: Sky picture, whether to use this sky's fog colour, fog distance and fog colour; maps whose level has a band of fog low on the sky also show Ground fog from / up to.
The map view lights the ground a little by the look (dark looks are lifted so you can still work: View > Brighten dark maps turns that off); the game is darker. Test shows the real light.
Every change writes a line in the status bar, with the change in graphics memory when a sky or look brings a lot of new files or the map is near the limit.)HELP"},

        {"Check and Test",
         R"HELP({n:Check}... lists problems. Red items stop the game from using the scenario: Test refuses to start, and the game does not list a saved scenario that still has them. Yellow items are advice. Each Show button selects the object and moves the camera to it. Limits: 3,000 objects, 64 MB of graphics memory and 63 sound clips; the Place, Scatter and Roads & walls pages show the graphics memory figure.
Test ({k:Test}) saves, then starts the game straight into your scenario: a multiplayer map starts a match with the bots from the arrow next to Test (game type, bots and their skill), a mission starts selected in Custom Scenarios. Leaving the match closes the game again.
The game-type box in the toolbar shows only the objects one game type uses, so you can check every mode one by one.)HELP"},

        {"Undo",
         R"HELP({k:Undo} undoes and {k2:Redo} redoes, up to 200 steps. Repeated turns and nudges merge into one step. A whole brush stroke is one step. Edit > Undo shows what the next step is.)HELP"},

        {"Files and sharing",
         R"HELP(Scenarios are saved as .ma2map files in your scenario folder (File > Settings > General changes it); each save keeps the previous version as .bak. The game lists every scenario in that folder the next time it starts. Give each scenario its own name on the {tab.1} tab (up to 60 characters): two with the same name are listed with their file names.
Share a map by sending the .ma2map file. It holds no game files, so the other player needs their own copy of the game. Files saved with an editor older than 1.0.2 can show pink markers in the game: the editor offers to update them at start-up, or use File > Update old scenarios...
The editor keeps a recovery copy every two minutes while you have unsaved changes and offers it back if it closes unexpectedly. Crash reports and settings go to %LOCALAPPDATA%\MechAssault2; Help > About shows the editor version and the folders, with Open logs folder.
If the scenario is not in the game's list: open it in the editor, fix any red items in Check scenario and save. A copy made in Explorer is not listed until you open it in the editor and save it. "Damaged file" when opening: use the .bak beside it. If Test does nothing, the game may already be running, or Check scenario has red items.)HELP"},

        {"Keys and settings",
         R"HELP(Help > {n:Shortcuts} ({k:Shortcuts}) lists every key and mouse gesture. File > Settings > Keys changes any key, and the three hold keys: {hold.free} places freely, {hold.through} clicks through to what is behind, {hold.quarter} makes quarter turns; {hold.reverse} reverses a brush and {hold.orbit} turns the view with the left button. In the Help window every key shown is the one you set; this file shows the defaults.)HELP"},
    };
    return t;
}

struct HelpPage {
    std::string title, body;
};

// The topics with every token resolved for these keys.
inline std::vector<HelpPage> help_topics(const KeyText& k) {
    std::vector<HelpPage> out;
    for (const HelpTopic& h : help_raw()) out.push_back({h.title, help_expand(h.body, k)});
    return out;
}

// Words people type that the text says another way (the UI says "street lamps", the topic says "lights").
inline const char* help_alias(const std::string& w) {
    static const struct { const char* from; const char* to; } t[] = {
        {"lamp", "light"}, {"lamps", "light"}, {"streetlight", "light"}, {"streetlamp", "light"}, {"street", "light"}, {"lantern", "light"}, {"lighting", "light"},
        {"pond", "water"}, {"lake", "water"}, {"river", "water"}, {"sea", "water"}, {"color", "colour"}, {"colors", "colour"}, {"grey", "colour"},
        {"mountain", "raise"}, {"hill", "raise"}, {"valley", "lower"}, {"dig", "lower"}, {"grass", "plants"}, {"bush", "plants"}, {"forest", "scatter"},
        {"tree", "scatter"}, {"trees", "scatter"}, {"rocks", "scatter"}, {"cloud", "sky"}, {"clouds", "sky"}, {"storm", "weather"}, {"thunder", "lightning"},
        {"hotkey", "keys"}, {"hotkeys", "keys"}, {"shortcut", "keys"}, {"shortcuts", "keys"}, {"key", "keys"}, {"quit", "exit"}, {"crash", "recovery"},
        {"camera", "cutscene"}, {"cinematic", "cutscene"}, {"enemy", "enemies"}, {"remove", "delete"}, {"erase", "delete"}, {"move", "drag"},
        {"zoom", "wheel"}, {"rotate", "turn"}, {"spawn", "starts"}, {"flag", "capture"}, {"mission", "objective"}, {"win", "objective"},
    };
    for (auto& a : t)
        if (w == a.from) return a.to;
    return nullptr;
}

// Which topic a word finds (for "Find in Help"): case-insensitive, every word must occur in the title or the body, or its alias must.
inline bool help_matches(const HelpPage& p, const std::string& query) {
    auto low = [](std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    const std::string hay = low(p.title + "\n" + p.body), q = low(query);
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && q[i] == ' ') i++;
        size_t j = i;
        while (j < q.size() && q[j] != ' ') j++;
        if (j > i) {
            const std::string w = q.substr(i, j - i);
            bool hit = hay.find(w) != std::string::npos;
            if (!hit && w.size() > 3 && w.back() == 's') hit = hay.find(w.substr(0, w.size() - 1)) != std::string::npos;  // "lamps" -> "lamp"
            if (!hit)
                if (const char* a = help_alias(w)) hit = hay.find(a) != std::string::npos;
            if (!hit) return false;
        }
        i = j;
    }
    return true;
}

// The topic F1 opens for the Build page you are on; titles as help_raw lists them.
inline const char* help_topic_for_page(Page p) {
    static const char* const by_page[kPageCount] = {"Selecting and changing", "Placing", "Scatter", "Roads, bases and layers", "Shaping the ground", "Painting", "Water", "Starts"};
    const int i = int(p);
    return i >= 0 && i < kPageCount ? by_page[i] : "Start here";
}

// Getting Started's one steps line (it names tools, not tabs).
inline std::string help_getting_started_steps(const KeyText& k) {
    return help_expand("shape the ground ({page.sculpt}), place starts ({page.starts}) and objects ({page.place}), add an objective for a mission ({tab.3} tab), set the look and weather ({tab.5} tab), then Test ({k:Test}): it saves and plays your scenario in the game.", k);
}

// ------------------------------------------------------------------------------------------------ the guide
// Plain text, 76 columns, wrapped here. A line starting "- " or "N. " hangs under its text.
inline std::string help_wrap(const std::string& para, size_t width = 76) {
    size_t indent = 0;
    if (para.compare(0, 2, "- ") == 0) indent = 2;
    else if (para.size() > 3 && para[0] >= '0' && para[0] <= '9' && para[1] == '.' && para[2] == ' ') indent = 3;
    std::string out, line;
    size_t i = 0;
    while (i < para.size()) {
        while (i < para.size() && para[i] == ' ') i++;
        size_t j = i;
        while (j < para.size() && para[j] != ' ') j++;
        if (j == i) break;
        const std::string w = para.substr(i, j - i);
        if (!line.empty() && line.size() + 1 + w.size() > width) {
            out += line + "\n";
            line = std::string(indent, ' ') + w;
        } else {
            line += (line.empty() ? "" : " ") + w;
        }
        i = j;
    }
    return out + line;
}

inline std::string help_key_defaults() {
    const KeyText k = KeyText::defaults();
    auto cell = [](std::string s, size_t w) { while (s.size() < w) s += ' '; return s; };
    struct Row { const char* ini; const char* word; };
    static const Row rows[] = {
        {"New", "new"}, {"Open", "open"}, {"Save", "save"}, {"SaveAs", "save as"}, {"Test", "test"}, {"Help", "help"}, {"Preferences", "settings"},
        {"TopDown", "top-down view"}, {"ResetView", "reset the view"}, {"Helpers", "helpers"}, {"ZoomSelection", "zoom to the selection"},
        {"Undo", "undo"}, {"Redo", "redo"}, {"Copy", "copy"}, {"Cut", "cut"}, {"Paste", "paste"}, {"Duplicate", "duplicate"},
        {"SelectAll", "select all"}, {"Delete", "delete"}, {"Snap", "put on the ground"}, {"PlaceAgain", "place again"},
        {"PickType", "place more of this"}, {"Cancel", "stop the tool or deselect"},
        {"BrushSmaller", "smaller brush"}, {"BrushBigger", "bigger brush"}, {"Find", "search the object list"}, {"Shortcuts", "all keys"},
    };
    std::string out;
    size_t col = 0;
    std::string line;
    auto flush = [&]() { if (!line.empty()) { while (line.back() == ' ') line.pop_back(); out += "  " + line + "\n"; line.clear(); col = 0; } };
    for (const Row& r : rows) {
        const int a = help_act_by_ini(r.ini);
        const std::string key = k.first(a);
        if (key.empty()) continue;
        std::string c = key + " " + r.word;
        if (a == A_REDO) c = key + " or " + key_label(k.keys[a][1]) + " " + r.word;
        line += cell(c, 38);
        if (++col == 2) flush();
    }
    flush();
    out += "  " + std::string("Pan: ") + k.pan() + "; turn: " + k.turn() + "; handles: " + k.first(A_GIZMO_MOVE) + " " + k.first(A_GIZMO_ROTATE) + " " + k.first(A_GIZMO_TILT) + "\n";
    return out;
}

// docs\Scenario editor.txt (CRLF, like the kit's copy). {{VERSION}} and {{MA2_SUPPORT_URL}} are filled in when the release kit is made.
inline std::string help_guide(const KeyText& k) {
    std::string t;
    auto line = [&](const std::string& s) { for (char c : s) { if (c == '\n') t += "\r\n"; else t += c; } t += "\r\n"; };
    auto lines = [&](const std::string& s) { size_t i = 0; while (i <= s.size()) { size_t e = s.find('\n', i); if (e == std::string::npos) e = s.size(); line(s.substr(i, e - i)); i = e + 1; } };
    line("MA2 Scenario Editor - user guide (version {{VERSION}})");
    line("=====================================================");
    line("");
    line(help_wrap("Help > User guide in the editor opens this file. " + k.first(A_HELP) + " opens the editor's own help window, which has the same text with a topic list and a search box."));
    line("");
    line(help_wrap("\"MA2 Scenario Editor.exe\", next to the game, makes multiplayer maps (played with bots in Bot Match) and single-player missions (under Custom Scenarios). It uses the models, textures and sounds of your own installed game, so a scenario file is small: it refers to the disc's content instead of copying it."));
    int n = 1;
    for (const HelpPage& p : help_topics(k)) {
        line("");
        line("");
        std::string head = std::to_string(n++) + ". ";
        for (char c : p.title) head += char(std::toupper(static_cast<unsigned char>(c)));
        line(head);
        line(std::string(head.size(), '-'));
        size_t i = 0;
        while (i <= p.body.size()) {
            size_t e = p.body.find('\n', i);
            if (e == std::string::npos) e = p.body.size();
            lines(help_wrap(p.body.substr(i, e - i)));
            i = e + 1;
        }
    }
    line("");
    line("");
    const std::string head = std::to_string(n) + ". KEYS (defaults)";
    line(head);
    line(std::string(head.size(), '-'));
    lines(help_key_defaults());
    line("");
    line(help_wrap("Quote the editor version (Help > About) in reports at {{MA2_SUPPORT_URL}}."));
    return t;
}

}  // namespace ed
