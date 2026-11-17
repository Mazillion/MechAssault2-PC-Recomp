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

The editor has five tabs along the top (Map, Build, Objectives & Triggers, Cameras & Cutscenes,
Environment). The Build tab has a tool strip on the left (Select, Place, Scatter, Roads & walls,
Sculpt, Paint, Water, Starts); Properties and the Next steps list are on the right, and a small
map sits below them. Test saves the scenario and starts the game on it; for a multiplayer map the
arrow next to Test sets the game type, the bots and their skill.

The full description is the user guide, `docs\Scenario editor.txt` (the installer puts it next to
the game, and Help > User guide opens it). Inside the editor, Help (F1) has the same text as a topic
list with a search box, written with your own keys; Help > Keyboard shortcuts lists every key.
`docs\Scenario editor.txt` is generated from `src\app\help_text.h`: `out\fmt_test.exe` writes
`out\Scenario editor.txt` and fails when `docs\` differs (copy the new one over it).

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

- **Getting Started** opens on the first start (Help > Getting Started reopens it); **Help** (F1) is a
  topic list with a search box. Every tool has a tooltip, and a hint line at the bottom says what the
  mouse and the keys do.
- **Placing** shows the object under the mouse: green where it can go, red with the reason where it
  cannot. Q / E turn it first. In missions, "Place units as" picks Enemy / Player's side / Neutral
  (neutral 'Mechs stand empty for the player to climb into).
- **Check scenario** (Map tab, and before every Test) lists problems in plain words with how to fix them;
  "Must fix" items block Test. Test saves and starts the game straight into the scenario.
- **Undo** covers everything, triggers and messages included; deleting triggers, objectives and clips asks
  first.
- **Recovery**: an autosave every two minutes while there are unsaved changes, and a crash handler that
  saves too; the next start offers the work back. Crash reports go to `%LOCALAPPDATA%\MechAssault2\Logs`.
- **Sound clips**: any WAV is converted on import.
