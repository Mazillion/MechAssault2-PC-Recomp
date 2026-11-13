# MA2 Scenario Editor

An Age of Empires II–style map and scenario editor for the MechAssault 2 PC recompilation.
It saves `.ma2map` scenarios (file formats: [docs/editor.md](../docs/editor.md)). Multiplayer maps appear in
Multiplayer → Local → New Game, where the Bots rows fill them; missions appear under
Custom Scenarios.

The editor reads your own `game_files` at run time. It never contains or writes retail model
or texture files: a scenario references the files it needs by archive and path, and the game
rebuilds the level from your own disc.

## Build

```
build.cmd           editor + format tests
build.cmd tests     format tests only
build.cmd clean
```

Needs Visual Studio 2019 or newer, or the Build Tools, with the C++ x64 workload (vcvars64 is
called automatically), and the vendored Dear ImGui at `kit/source/game/vendor/imgui-1.92.5`. Output:

- `out\MA2 Scenario Editor.exe`
- `out\fmt_test.exe`

## Install folder

The editor needs the folder that holds `MechAssault 2.exe` and `game_files`. By default it
uses the folder the editor exe sits in, then its parent, then any ancestor folder with
`game_files\common.mgf`. Change it in File > Settings > General (saved in
`%LOCALAPPDATA%\MechAssault2\ScenarioEditor.ini`).

## Scenario folder

Default: `Documents\MechAssault 2\Scenarios` (OneDrive redirection respected). File > Settings >
General > Scenario folder writes `%LOCALAPPDATA%\MechAssault2\Scenarios.ini` (`[Scenarios] Folder=`),
which the game reads too.

## Using it

Layout: tabs along the top (Map, Terrain, Units and Props, Objectives & Triggers, Cameras &
Cutscenes, Environment), the tool panel on the left, the 3D view in the middle, the properties
panel and the minimap on the right. The sections below were written for the first tab layout:
"Players" and "Units" are now one tab, Units and Props; "Triggers" and "Messages" are now
Objectives & Triggers; "Options" moved to File > Settings. Help > How to use the editor... has
the current description.

**Camera.** WASD, the arrow keys or the window edges pan; the mouse wheel zooms;
middle-drag rotates; `T` toggles top-down. Click or drag on the minimap to jump.

**Mouse.** Left-click places (with a placing tool) or selects. Drag a selected object to
move it; drag on empty ground to box-select; Shift/Ctrl-click adds to the selection.
Right-click cancels the tool, or clears the selection.

**Keys.** `Q`/`E` turn the selection (or the object about to be placed) by 15°, `G` snaps
to the ground, `Del` deletes, `Ctrl+C`/`Ctrl+V` copy and paste at the cursor,
`Ctrl+Z`/`Ctrl+Y` undo and redo, `Ctrl+S` saves, `F5` tests.

**File → New.**
- Multiplayer map or Mission.
- Size: Small 129×129 (1 km), Medium 129×257, Large 257×257.
- Theme: the retail level the map is built on. Its sky, lighting, ground textures and every
  model in its archive come for free.
- Height range, start height; start flat, from random hills, or as a copy of a retail map.
- The new world is built from the theme level's own world file: environment, sky, terrain
  info, culture, entity sets and (for MP) the setup triggers are kept; everything else is
  removed and the terrain is replaced.

**File → Open retail map** shows any of the 16 multiplayer maps and 22 missions read-only.

**Map tab.** Name, author, description, game types (MP), sky, sun direction, fog, night/rain/
lightning, validation.

**Sky.** The Map tab's Sky list has the Base level's own sky (marked) and every sky model on
the disc: Env_SkyDusk02, Env_Sky_CityNight, Env_SkyNOR_01/02, Env_SkyHesperus_01/02,
Env_SkyAurora_01, Env_Sky_StormFront_01, Env_Sky_Stormy, En_Sky_WarmSun, Env_SkySpace /
Env_SkySpace02 and Env_Planet (no level uses that one as a sky). Hover an entry to see the
levels and archives that use it.
- Picking a sky sets the world's MASky2. MP maps get the `_MP` model when there is one.
- The `.mtb`, scale, offset, position and heading come from a retail level that uses the
  sky: the Base level itself when it does, else a level of the same kind.
- The view draws the sky behind the terrain (View → Sky toggles it).
- "Use this sky's fog colour" copies that level's fog colour; unticking it restores the
  Base level's.
- A sky from another level's archive goes into the scenario's `[Assets]` list. The game
  loads its model, `.mtb` and textures from your own disc. For example, Sky_Test (Bogged
  Down with New River City's aurora) lists 8 files.

**Terrain tab.** Raise, Lower, Flatten, Smooth, Roughen and Plateau brushes; colour-map
paint (with a picker on the current colour map); height range; water planes; Bake lightmap
(the MAEnvironment sun, with terrain shadows; strokes re-bake on release).
Terrain-type brush (the MATerrainInfo 512×512 map) with an overlay:
- Playable (0): players and bots move here. Spawns, vehicle pads, CTF flags and SnatchIt
  objects must stand on Playable or Water/special ground; validation reports anything else
  as an error.
- Water/special (8, blue): water or waterway inside the arena.
- Border (2, orange): the band around the arena.
- Out of bounds (1, red): nothing moves here.
- Other values from retail maps (16, 32, ...) show purple.

New maps start with the whole terrain Playable inside 4 cells of Out of bounds and a 6-cell
Border band. Copies of retail maps keep their own map. "Make the whole map playable" resets
it. Validation warns when spawns, pads and objectives fall in separate playable areas.

**Players tab** (MP). Red/Blue/free-for-all spawn points (MALevelStart, TeamIndex 1/2/0),
6 on a ring at once; vehicle pads copied from retail pads (pad → spawner → vehicle with its
owner controller and salvage drops); game-mode kits copied from a retail map's
MAMissionTypeGroup subtrees and scaled to this map; test settings (mode, bots per side,
skill). Missions: move the player start.

**Units tab.** The object palette from `Templates/GlobalEntities.wdf`, by category, with a
search box and a "theme archive only" filter. Objects are placed as copies of a retail
instance of the template when one exists (so mechs keep their owner controller and drops),
otherwise from the template itself. Tick objects for the scatter brush (trees). Selection
tools: snap to ground, delete, copy/paste, nation.

**Properties panel.** Name, position, heading, team or nation, the object's attached parts,
and an Advanced grid with every raw property (names from your `ma2.xbe`).

**Triggers tab.** AoE2 model: a list of triggers, each with a name, Enabled at start, Loop,
All/Any conditions, and effects that run in order. Areas, units and AI paths of the selected
trigger are drawn in the map.
- Conditions: mission start, timer, player in a circle or box (inside/outside), units
  destroyed (all/any, jacking counts), unit damaged (%), another trigger has / has not fired.
- Effects: display message (inline text, optional caption, seconds, pause), play a retail
  voice/sound cue (picked from the cues the shipped missions use), play an imported WAV, music,
  complete objective, win, lose, show/hide/remove unit, AI order (patrol a path drawn on the
  map, defend a point, follow or attack the player or a unit), enable/disable a unit or
  spawner, teleport the player, change nation, checkpoint save, set mission variable,
  enable/disable another trigger.
- "Pick on map" sets a point; "Draw on map" adds path points until right-click; "Use
  selection" takes the selected unit.
- A trigger that starts disabled and has a timer counts down from the moment another trigger
  enables it (the retail "delay" pattern).
- On save the triggers compile into native MATrigger*/MAAction* trees (group "Editor
  Triggers") and AI orders into the units (`[ed] AI Orders`); inline text gets text ids from
  60000 up.

**Messages tab.** Mission briefing (also written to MARoot TitleText/BriefingText) and
objectives (objective n is completed by mission variable n); custom text lines (text id
`0x00F00000 + n`); imported WAV clips (16-bit PCM, cue id `0x5C000000 + n`).

**Options tab.** Scenario folder, install folder, author name, edge scrolling.

**Save** writes a `.ma2map`: `scenario.ini`, `world.wdf`, `templates.wdf` (the culled
template set), the terrain colour map/lightmap when they changed, the campaign-AI path graph
(`Pathfinding/Scn<ID>_Mech.map`, 20 m grid), imported audio, `preview.tif` and
`editor/project.json`. Files outside the Base archive that the objects need are listed in
`[Assets]`.

**Test** (`F5`) validates, saves and runs
`"<install>\MechAssault 2.exe" --scenario "<file>" --mode <type> --bots <r>v<b> --skill <n>`.

## Format tests

`out\fmt_test.exe [--game <install>] [--samples <dir>]` checks, against your game_files:

- all 61 world files in common.mgf and the 60 culled template files in Templates.mgf
  round-trip byte-identically;
- every heightfield and terrain-info picture round-trips, and heightfields re-encode losslessly;
- all 212 `Terrain/*.tif` pictures decode; lossless formats re-encode identically, DXT
  re-encodes stay above 30 dB;
- the MGF writer's directory keys match retail archives;
- every retail culled template file is rebuilt byte-identically from its UID list;
- the height formula matches the retail navigation nodes;
- two sample scenarios are built, saved, reloaded and re-saved.

With `--samples <dir>` the samples are written there (see `samples\`). `--all-maps` also
copies, saves and reloads every retail map with a level archive (36).

Samples (`samples\`):
- `BoggedDown_Plateau.ma2map`: retail Bogged Down copy with a raised plateau and a re-baked lightmap.
- `Small_TeamDestruction.ma2map`: Small map from scratch, 6 Red + 6 Blue spawns, Team
  Destruction, vehicle pads, snow trees from another level (exercises `[Assets]`).
- `Mission_Ambush.ma2map`: mission with briefing, objective, two enemy mechs (patrol, defend,
  ambush/attack), messages, an imported WAV, music change, checkpoint, delayed win.

The editor renders retail models with the models helper's library (`../models`, namespace
`ma2m`), compiled straight into the editor; the same library collects `[Assets]`.

## Developer screenshots

`"MA2 Scenario Editor.exe" --shot out.bmp [--retail BoggedDown] [file.ma2map]
[--tab N] [--topdown] [--cam x,z,dist,yaw,pitch] [--select UID] [--overlay bit]` renders ten
frames in a hidden 1600×900 window, writes the BMP and exits.

## Files

| path | |
|---|---|
| `src/core/` | format libraries: MGF, WDF, MGIc pictures, names, templates, terrain, pathfinding, scenario model, triggers |
| `src/app/` | the Win32 + Direct3D 11 + Dear ImGui application |
| `tests/fmt_test.cpp` | round-trip tests and sample generation |

File formats and the scenario contract with the game: [docs/editor.md](../docs/editor.md).

## Help, checks and safety

- **Getting Started** opens on the first start (Help > Getting Started reopens it); **Help** (F1) has
  step-by-step recipes for a multiplayer map and a mission. Every tool has a tooltip, and a hint line at
  the top of the map says what the mouse does.
- **Placing** shows the object under the mouse: green where it can go, red with the reason where it
  cannot. Q / E turn it first. In missions, "Place units as" picks Enemy / Player's side / Neutral
  (neutral 'Mechs stand empty for the player to climb into).
- **Check scenario** (Map tab, and before every Test) lists problems in plain words with how to fix them;
  "Must fix" items block Test. Test saves and starts the game with `--mode/--bots/--skill`; the game
  then walks its own menus to the map.
- **Undo** covers everything, triggers and messages included; deleting triggers, objectives and clips asks
  first.
- **Recovery**: an autosave every two minutes while there are unsaved changes, and a crash handler that
  saves too; the next start offers the work back. Crash reports go to `%LOCALAPPDATA%\MechAssault2\Logs`.
- **Sound clips**: any WAV is converted on import.
- Findings and fixes: `USABILITY.md`.
