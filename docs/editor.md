# Scenario editor and custom scenarios

Developer overview of the MechAssault 2 Scenario Editor and of how the game loads what it writes. The editor is a
separate Windows program (`MA2 Scenario Editor.exe`, source in `editor/`); the game side lives in
`kit/source/game/src/scenario/` (`ma2_scenario.c`, its header, and `ma2_scenario_audio.cpp` for imported clips). Facts: `t04_scenario` (formats and
game-side hooks), `t16_editor_to_game` (what every editor control writes and how the game uses it),
`t15_editor_render_gaps`, `t11_editor_verified`, `t27_pink_placeholder_mtb_refs_1109` and the editor sections of
`t12_code_research`. Ids such as `t04.ma2map-container` are looked up with `python wq.py "<id>"` in `docs/wiki/`
([fact-index.md](fact-index.md)).

## 1. What the editor produces

A custom scenario is one `.ma2map` file: a plain, uncompressed **MGF v4 archive** (`t04.ma2map-container`,
`t04.mgf-v4-header-and-records`) holding:

| Entry | Content |
|---|---|
| `scenario.ini` | `[Scenario]` (needs `FormatVersion = 1`, an 8-character `Id`, `Base`, `Name`; limits: Name 95, Description 255, Author 63 characters, up to 16 `GameType` lines), `[Strings]` (n = 1..65535), `[Audio]` (n = 1..63), `[Assets]` (`File = archive|path` lines). See `t04.scenario-ini-sections`. |
| `world.wdf` | The level's world file: the `MAEnvironment`, `MATerrain`, `MASky2`, `MATerrainInfo`, `MARoot` objects and everything placed (`t04.world-file-structure`, `t16_editor_to_game`). Property keys use CRC32 without the final complement (`t04.path-key`, `t04.world-file-keys`). |
| terrain pictures, path graph, imported WAV clips | The scenario's own files (`Terrain/Scn<id>_*.tif`, campaign-AI path graph, audio clips). |

The editor reads and writes the same files through its own format libraries and uses the `ma2models` decoder for
previews and for the `[Assets]` closure (`t04.ma2models-resolver`). Format round-trips are covered by `fmt_test`, which
checks all retail world files and culled template sets byte for byte, the MGF writer keys against retail archives, and
a build-save-reload of sample scenarios (`t04.editor-tests`). Source locations: `editor/src/` (`app/`, `core/`),
`editor/tests/fmt_test.cpp`, `editor/models/src/` (the model library).

## 2. How the game loads a scenario (t04)

- **Scan.** The game scans a scenario folder (default `Documents\MechAssault 2\Scenarios`; the editor (File > Settings > General) and the game share the `[Scenarios] Folder=` key in `%LOCALAPPDATA%\MechAssault2\Scenarios.ini`; `t04.scenario-folder-resolution`,
  `t04.editor-scenario-folder`). Every `*.ma2map` is parsed; a file that fails is logged and skipped; a duplicate `Id`
  loses to the first; at most 200 scenarios load (`t04.scenario-scan-and-duplicates`).
- **Retail loaders do the work.** Each valid file is converted into data the retail loaders already understand: a
  generated map-ini for multiplayer maps and a generated one-campaign file for missions (`t04.mp-map-ini`,
  `t04.campaign-ini-generated`). Multiplayer scenarios appear under Multiplayer, Local, New Game; missions appear under the
  **Custom Scenarios** hub item, which replaces the original Game Demos button (`t04.hub-demo-button-patch`,
  `t04.hub-action-8-hook`). Custom mode swaps the campaign and save names so progress is kept separately
  (`t04.custom-mode-enter`, `t04.save-name-swap`) and restores them on leaving (`t04.custom-mode-leave`).
- **No retail bytes in the scenario.** The `[Base]` archive is always the retail level archive, read from the user's disc
  copy (`t04.base-archive-read`, `t04.resource-archive-is-base`); a scenario whose Base is missing is skipped
  (`t04.base-missing-skip`). A small per-scenario `[Assets]` archive holds only extra retail files (for example model
  files the chosen start vehicle needs) and is mounted beside the level while it is loaded (`t04.assets-archive`,
  `t04.level-load-hook`, `t04.level-unload-hook`).
- **Boot index.** A generated index archive is mounted after `Common.mgf` so new files are visible to the engine
  (`t04.index-archive-mount`); rescans add deltas and a changed scenario needs a game restart (`t04.rescan-delta`,
  `t04.rescan-new-files-only`).
- **Text and audio.** Locally authored text and cue ids are remapped to global ranges when the world is copied, so several
  scenarios coexist (`t04.world-remap-text-cue`); custom text is served by `ma2_scn_text` and imported clips play through
  XAudio2 (`t04.ma2-scn-text`, `t04.scenario-audio-play`).
- **Placeholders.** A pink pole-and-cube in a custom scenario means an object build failed, often because a template names
  a model absent from the disc or from the loaded archives (`t27_pink_placeholder_mtb_refs_1109`, `t11.unit-model-selection`).

## 3. What each editor control writes (t16)

`t16_editor_to_game` has a table per editor tab (the wiki may use older tab names; the current tabs are Map, Terrain, Units and Props, Objectives & Triggers, Cameras & Cutscenes and Environment): control label, the class property or file written, how the game uses it, the
related wiki facts and the confidence. Each section ends with "surprises" (places where editor wording and game behaviour
differ) and "gaps" (controls whose game effect is not explained by any fact). Examples of things developers trip over:

- Terrain colour-map alpha 255 selects `TerrainDetailMap`, alpha 0 selects `TerrainDetailMap 1` (`t15.terrain-alpha-selects-detail-verified`).
- Road textures need MIRROR addressing (`t15.road-mirror-addressing`); water is a five-pass screen-space effect in the game
  (`t15.water-render-pipeline`) that the editor approximates; the rendering gaps are listed in `t15_editor_render_gaps`.
- "Test" starts a separate game process that reads the scenario at startup, on its own scratch save folder
  (`%LOCALAPPDATA%\MechAssault2\TestSaves`), so the player's saves and settings are not touched
  (`t16.t16-cutscenes-objectives-test-surprise-10`).
- Mission units draw the model chosen by their detail flags; most levels ship only `_low` models, so units placed with
  `ForceUseLowDetail=1` avoid pink markers (`t11.unit-model-selection`). Boardable units need `StartAbandoned=1`,
  `AttackPilot=0`, `OverrideEnable=0` and an `MAOwnerController` child (`t11.boarding-rule`).
- A weather setting with `FlashTimeMax == FlashTimeMin` crashed the game with a divide by zero; the editor forces max > min
  (`t12.weather-flash-divzero-crash`).

## 4. Test button, quick test and autostart

The editor's **Test** button saves the scenario and starts the game straight into it. The game walks its own menus
("autostart"): Press Start, hub Multiplayer or Custom Scenarios, Local, New Game with the chosen mode and map, then Enter
in the Briefing Room (`t04.ma2-test-autostart-walk`, `t04.autostart-new-game-steps`). It repeats presses on screens that
have not moved and gives up after 15 s (`t04.autostart-press-retry`, `t04.autostart-give-up`); `RECOMP_SCENARIO_AUTOSTART=0`
turns it off (`t04.autostart-arm-conditions`).

Test starts `MechAssault 2.exe --scenario <file>`, plus `--mode`, `--bots <red>v<blue>` and `--skill` for a multiplayer map. The game also accepts `--quick-test` (skip movies and the intro, exit on leaving the level; an editor preference). `--place` is an editor developer flag (sent with `script`), not a game flag (`t12.editor-place-is-relative-to-the-first-malevelstart-not-the-p`). Under quick test a multiplayer
scenario loads one game type: objects under `MAMissionTypeGroup` ancestors whose flag for that mode is 0 are absent
(`t12.quick-test-loads-one-mp-game-type-objects-under-mamissiontyp`).

## 5. Automation pipe

The editor has an `--automation` mode that exposes a named pipe so an external client can drive it without a visible
window; `editor/tools/edctl.py` is the command-line client (any test harness can speak the same pipe protocol, see
`editor/tools/AUTOMATION.md`). Rules found in practice (`t12` facts):

- Under `--automation` every question box and file picker is answered by the automation client from launch, Windows crash
  and disk boxes are off, fatal errors go to stderr, "Open folder" never starts Explorer, and a hidden **Test** starts the
  game hidden and silent (`t12.editor-automation-mode-answers-every-dialog-from-launch-test`,
  `t12.editor-hidden-automation-rules-dialogs-answered-from-launch`).
- Save on an untitled map with no queued path saves under a new name in the scenario folder; a cancelled save reports
  "Not saved" (`t12.editor-automation-save-without-a-queued-path-auto-names-in-t`).
- The mouse command has no modifier argument: hold a modifier with a key-down command; dismiss the "Getting Started" dialog
  before scripting; `script --new-copy <level>` makes an editable copy of a retail level
  (`t12.editor-automation-tips-modifier-hold-getting-started-dismiss`).

## 6. Robustness and settings

- Autosave every two minutes while there are unsaved changes, into `%LOCALAPPDATA%\MechAssault2\Autosave`; a crash handler
  writes a minidump and log under `%LOCALAPPDATA%\MechAssault2\Logs` and saves a recovery copy; the next start offers it back
  (`t04.editor-recovery`). Saving outside the scenario folder asks first (`t04.editor-save-folder-guard`).
- The editor's options file is `%LOCALAPPDATA%\MechAssault2\ScenarioEditor.ini`.
- The installer's diagnostics export redacts the Windows user name in its text files (`kit/installer/precheck.py`); an optional crash dump is included as is (`t28` pack, "diagnostics").

## 7. Open points

Search the wiki for `status: open-question` facts tagged `editor`; the `t15_editor_render_gaps` article and the "gaps"
rows of `t16_editor_to_game` are the most useful lists of known differences between the editor view and the game.
