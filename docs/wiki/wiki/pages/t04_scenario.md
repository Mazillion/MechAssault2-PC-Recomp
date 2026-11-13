# Custom scenarios: editor, file formats, game-side hooks

Pack `t04_scenario`. Facts: `facts/t04_scenario.json` . Property and class meanings are in
`W\.\`; this article links to them and does not repeat them.

Short source names used below (full paths in each fact):
`ma2_scenario.c` and `.h` and `ma2_scenario_audio.cpp` are in `kit/source/game/src/scenario/`;
`patch_scenario.py` and `patch_scenario_assets.py` are ;
`STS` is `editor-status.md`.

## Overview

A custom scenario is a `.ma2map` file: one plain MGF v4 archive holding `scenario.ini`, `world.wdf` and the
scenario's own files (terrain pictures, a campaign-AI path graph, imported WAV clips). The MechAssault 2
Scenario Editor (a separate exe, `MA2 Scenario Editor.exe`) writes these files. The game reads them from a
scenario folder (default `Documents\MechAssault 2\Scenarios`). Each valid file is turned into data the retail
loaders already understand. Multiplayer scenarios appear in Multiplayer, Local, New Game, through the retail
map-set loader. Mission scenarios appear in the Custom Scenarios hub item, which replaces the old Game Demos
button, with a generated one-campaign file.

The game does not copy retail bytes into the scenario. The `[Base]` archive is always the retail level archive,
read from the disc and cached. A small per-scenario `[Assets]` archive holds only extra retail files, and it is
mounted beside the level while that level is loaded. Text and audio ids that the scenario authored locally are
remapped to global ranges when the world is copied, so several scenarios can coexist. Custom text is served
through `ma2_scn_text`; imported clips are played through XAudio2 by `ma2_scn_audio_play`.

Everything the game does for a scenario lives in `ma2_scenario.c` (about 1,780 lines), with two small audio
and text hooks and a set of patch scripts that insert the hooks into the generated recomp C. The editor reads
and writes the same files through its own format libraries and uses the `ma2models` decoder for previews and
for the `[Assets]` closure. A "Test" button in the editor saves the scenario and starts the game straight into
it, walking the game's own menus (the autostart walk).

## Key code

| Address or function | What it does | Source |
|---|---|---|
| `ma2_scn_mount` | Boot: scans the folder, builds `T:\scenarios\index_<n>.mgf`, mounts it after Common.mgf | `ma2_scenario.c:1183` |
| `scan` / `add_scenario` | Loads every `*.ma2map`, skips bad and duplicate ids, caps at 200 | `ma2_scenario.c:1046`, `:921` |
| `load_scenario` | Reads `scenario.ini` and `world.wdf` from the archive; requires FormatVersion 1 | `ma2_scenario.c:726` |
| `build_resource` | Base stays the ResourceFile; writes `Scn<ID>_assets_<hash>.mgf` for `[Assets]` | `ma2_scenario.c:765` |
| `build_index` | Writes Multiplay and Missions files, `MissionSet63.ini`, the generated campaign | `ma2_scenario.c:818` |
| `remap_world` | Local text 0x00F00000+n and cue 0x5C000000+n become global ids in the copied world | `ma2_scenario.c:649` |
| `rescan` | Hub and Multiplayer menu visits: picks up new files as a delta index (not changed files) | `ma2_scenario.c:1224` |
| `ma2_scn_resource_path` | Sends a level archive named `Scn*` to its real resource | `ma2_scenario.c:1258` |
| `ma2_scn_level_load` / `_unload` | Mounts and drops the level's `[Assets]` archive | `ma2_scenario.c:1169`, `:1162` |
| `level_scenario` | Finds the scenario from the level descriptor world path at `[0x00524A60]+4` | `ma2_scenario.c:1147` |
| `mount_archive` / `unmount_archive` | Guest calls `sub_0001A060`, `sub_00025B70`, `sub_00025C90`, `sub_00025D10`, `sub_00019D40` | `ma2_scenario.c:1089`, `:1113` |
| `ma2_scn_custom_enter` / `_leave` | Custom Scenarios mode: campaign pointer `[0x004B2910]`, save names, profile snapshot | `ma2_scenario.c:1333`, `:1367` |
| `ma2_scn_hub_action` | Ends the custom mode when any other hub item is activated | `ma2_scenario.c:1710` |
| `ma2_scn_menu_frame` / `auto_frame` | Test autostart: one step per menu frame | `ma2_scenario.c:1677`, `:1601` |
| `ma2_scn_mission_picker` | Preselects the Test mission in the Campaign Mission Picker | `ma2_scenario.c:1384` |
| `ma2_scn_text` | Text for ids 0x00F00000.0x00FFFFFF (name ids, UI strings, [Strings]) | `ma2_scenario.c:1726` |
| `ma2_scn_clip` | Cue to WAV bytes of `[Audio] n` inside the .ma2map | `ma2_scenario.c:1759` |
| `ma2_scn_action_cue` | Cue hook: routes scenario cues to the clip player | `ma2_scenario.c:1776` |
| `ma2_scn_audio_play` | XAudio2 source voice per clip, volume from `[Audio] Volume` | `ma2_scenario_audio.cpp:79` |
| `sub_00165187` | Level mount gate: returns 0 if the resource archive is not on D: (retail function) | functions.json |
| `sub_001630FA` | Boot map-set loader: tries `MissionSet00.63.ini` | functions.json (`recomp_0036.c`) |
| `sub_0016321D` | Map-ini loader used by `add_map_ini` for each new multiplayer map | `ma2_scenario.c:1211` |
| `sub_0014CB91` / `sub_0019B89F` | `MAActionSound` / `MAActionPlayVoice` Execute (vtable slot 22) | `patch_scenario.py:197` |
| `sub_001F3BCC` | Hub item activation; action 8 is the old demo slot | `patch_scenario.py:114` |
| `sub_00208BF9` | Selects a barrel item by value (mission picker) | `ma2_scenario.c:1448` |

Retail addresses used as globals: `0x00524A60` (level descriptor), `0x0050BAC8` (level file system),
`0x0050C958` (game object), `0x004B2910` (campaign path format), `0x0050E838` and `0x0050E844` (save names),
`0x00524A6C` (frontend). Menu vtables used by the autostart: hub `0x00481B70`, multiplayer `0x0048C228`,
Press Start `0x00482170`, New Game `0x0048C668`, Briefing Room `0x00482240`, in game `0x00481EF8`, mission
picker `0x0048F3E0`, barrel `0x00485778`, carousel `0x004874A8`.

## File formats

- **`.ma2map` container.** An uncompressed MGF v4 archive. The writer (`w_write`) lays out a 64-byte header,
8-word file records, 6-word dir rows, a `MGI` string table and 16-byte aligned data
(`ma2_scenario.c:306`, `:440`). Keys are CRC32 without the final complement (`path_key`, `:98`), the
engine's form. The editor's `fmt_test` checks these keys against retail archives.
- **Retail archives.** Compressed MGF uses word 1 = `0xC5735A0F`, block count, logical size and a 131,072-byte
block size, with zlib blocks (`models\`). `mgf_load` inflates them whole (`ma2_scenario.c:324`).
- **`scenario.ini`.** `[Scenario]` (FormatVersion 1, Id, Name, Description, Author, Type, GameType, Base, Size),
`[Strings]` (n = 1.65535), `[Audio]` (n = 1.63), `[Assets]` (`File = archive|path`), and `[Mission]` for
missions (`ma2_scenario.c:576`). Free-text keys keep semicolons.
- **World file (`.wdf`).** Header `0xFF002F00`, then objects (`FF012F01`) and properties (`FF022F02`). Keys are
the CRC rule above. Types: 1 i8, 2 i16, 3 i32, 4 u8, 5 u16, 6 u32, 7 f32, 8 f64, 9 string, 10 vec2, 11 vec3,
12 colour, 13 blob, 16 u8. Retail worlds round-trip byte-identically (61 of 61).
- **MGIc pictures (`.tif`).** A 32-byte head, then `PIC ` and `PAL ` sections with tagged chunks. The `flgs`
word gives format, swizzle, cube, compression and volume bits (`models\` to `:231`).
- **Terrain.** `MATerrain."TerrainHeight Field"` is an L8 MGIc picture with a 64-byte row pitch. Retail sizes
are 129x129, 129x257 and 257x257. Height is `Min + v/255 * (Max-Min)`, and stored row r is `z = (H-1-r) * 8 m`
(`editor\`).
- **Type map (`MATerrainInfo` key `1F1646EA`).** A 512x512 16-bit map with row 0 at z = 0. Values: 0 playable,
8 playable special (water inside the arena), 2 border, 1 out of bounds. Every retail multiplayer spawn, pad,
flag and SnatchIt object stands on 0 or 8 (`editor\`). Validated by bisect runs in game.
- **Pathfinding (`Pathfinding/Scn<ID>_Mech.map`).** A text path graph on a 20 m grid. The
editor keeps only nodes reachable from starts, spawns and AI units, snapped within 120 m, and widens the lattice
until the set is about 2,500 nodes (`editor\`).
- **Audio.** `Audio/<n>.wav` is PCM or float, 1 or 2 channels, 8,000 to 96,000 Hz (`ma2_scenario_audio.cpp:75`).
The editor converts any imported WAV to 16-bit PCM first.
- **Model formats** (for `[Assets]` and previews): `.MGMODEL` XML-like resources with a node tree, vertex flags
(`models\` to `:123`), `.mtb` visibility rules (`:176` to `:201`), and `_low` substitution
(`:268`). Those are documented in `models\` and `models\API.md`.

Object and property meanings: see `W\.\` (175 classes). The mission trigger, action and AI classes
(`MATriggerRadius`, `MAActionBroadcast`, `MAAIGroup` and so on) are summarised in `editor\` to `:119`.

## Added code and hooks

Each hook is inserted by an idempotent patch script (marker `ma2_scn_`).

| Patch site | Insertion | Script |
|---|---|---|
| `recomp_0000.c` `loc_00012FA0` (3 copies) | `ma2_scn_mount(edi)` after Common.mgf | `patch_scenario.py:27` |
| `recomp_0000.c` `loc_000138A0` | `ma2_scn_level_load` at the end of the level mount block | `patch_scenario.py:40` |
| `recomp_0000.c` `loc_000178C9` (4 copies) | `ma2_scn_level_unload` after the level unmount | `patch_scenario.py:40` |
| `recomp_0037.c` `loc_001652AA`, `loc_0016533E` | `ma2_scn_resource_path(ebp-532)` after the sprintf | `patch_scenario.py:57` |
| `pc_prompts.cpp` `ma2_pc_localized` | ids 0x00F00000.0x00FFFFFF go to `ma2_scn_text` | `patch_scenario.py:75` |
| `recomp_0051.c` `loc_001F3BFA` | action 8: `ma2_scn_custom_enter`, jump past the demo launch | `patch_scenario.py:106` |
| `recomp_0051.c` `loc_001F3BCC` | `ma2_scn_hub_action(action)` for every hub item | `patch_scenario.py:106` |
| `ma2_bots_ui.c` (policy) | Custom Scenarios row dimmed when no missions | `patch_scenario.py:126` |
| `ma2_bots_ui.c` `ma2_menu_policy_pulse` | `ma2_scn_menu_frame(object, vtable)` each frame | `patch_scenario.py:142` |
| `recomp_0033.c` `loc_0014CB91`, `recomp_0042.c` `loc_0019B89F` | `ma2_scn_action_cue(MEM32(ecx+0x68 / +0x78))` | `patch_scenario.py:194` |
| `recomp_manual.c` `ma2_probe_missions` | `ma2_scn_mission_picker(object)` | `patch_scenario.py:210` |
| `build/combined.ninja` | compile rule and link input for `ma2_scenario.c` and `ma2_scenario_audio.cpp` | `patch_scenario.py:86`, `:176` |
| `interface/menus/hub/hub_main_demos.ggf` (in `common.mgf` overlay) | button 8 command and label | `patch_scenario_assets.py:36` |

The `patch_scenario_assets.py` script edits `pc_assets/common.mgf` and rewrites the directory record of the
hub file; it also re-derives `cache.version`.

## Env vars, settings and test hooks

| Name | Effect | Source |
|---|---|---|
| `RECOMP_SCENARIOS=0` | Disables the scan and the rescan (retail behaviour) | `ma2_scenario.c:1048` |
| `RECOMP_SCENARIO_DIR` | Overrides the scenario folder (tests) | `ma2_scenario.c:686` |
| `RECOMP_SCENARIO_FILE` | Same as `--scenario <file>` | `ma2_scenario.c:943` |
| `RECOMP_SCENARIO_TRACE` | Logs each action cue id | `ma2_scenario.c:1779` |
| `RECOMP_SCENARIO_AUTOSTART=0` | Turns the Test autostart walk off | `ma2_scenario.c:1019` |
| `RECOMP_SCENARIO_AUTOSTART_STALL=<hex vtable>` | Makes the walk do nothing on that menu (tests the give-up) | `ma2_scenario.c:1638` |
| `RECOMP_SCENARIO_AUTOSTART_DROP=1` | Drops the first press on each screen (tests the retry) | `ma2_scenario.c:1527` |
| `RECOMP_AUDIO_SILENT=1` | Mutes imported clips (logic tests) | `ma2_scenario_audio.cpp:84` |
| `[Audio] Enabled=0` / `[Audio] Volume` | Mutes clips / sets clip volume (0 to 100) | `ma2_scenario_audio.cpp:85`, `:102` |
| `[Scenarios] Folder=` in `%LOCALAPPDATA%\MechAssault2\Scenarios.ini` | Scenario folder (shared by game and editor) | `ma2_scenario.c:689` |
| `--scenario`, `--mode`, `--bots RvB`, `--skill 0-4` | Test launch arguments; `--bots` and `--skill` apply to one launch only | `ma2_scenario.c:1000` |

Editor settings: `%LOCALAPPDATA%\MechAssault2\ScenarioEditor.ini` (editor options). Editor dev flags:
`--shot`, `--play script.txt`, `--model-sweep`, `--place-before-start`, `--unit-side`, `--trigger-units`,
`--drop-trigger`, `--save-to`, `--confirm-test` (`README.md:180`, `HND:45`).

Test and sweep harnesses: `fmt_test` (`README.md:153`); the 18-map sweep (`sweep32`, `sweep33`, `sweep34`,
`MEM:66`, `:84`, `:95`); `run_v4.sh` and `quit_scn.py` for samples and remount (`MEM:82`).

## Bugs and fixes

- **Black screen with `[Assets]` scenarios** (fixed in 049ccc70). The map-ini loader wrote
`D:\Scn<ID>_<hash>.mgf` into the level descriptor, so the level gate `sub_00165187` found nothing and returned 0
(`MEM:100` to `:103`). Fix: keep ResourceFile as the Base archive and mount the assets archive separately
(`MEM:77` to `:86`). Two earlier attempts (v5, v6) hung or still failed and were reverted (`MEM:88` to `:109`).
- **Custom Scenarios hub item launched the demo.** Fixed by the hub patch and `ma2_scn_custom_enter`.
- **Stale comment on hub leave** (superseded). `ma2_scenario.c:1299` says `sub_001F3263` ends custom mode. The code
only does so through `loc_001F3BCC`; line 1708 says a rebuild does not end the mode.
- **`templates.wdf` ignored** (SPEC mismatch). SPEC describes a `CulledTemplates` mapping that the code does not
have (`ma2_scenario.c:856`). The game uses the full GlobalEntities set.
- **Audio volume** (SPEC mismatch). SPEC says voice volume; the code uses `[Audio] Volume` (superseded SPEC).
- **Editor saved missions into the wrong folder** (W8). Modern save dialog pinned to the scenario folder.
- **Mission restarted forever** (W9). Empty multiplayer pickups copied into missions counted as lost, so
"units destroyed" won at once. Fixed by crewing units per side (`USABILITY.md:35`).
- **Base War match ended at once** (W7). A missing power plant (objective 0 or 1) ends the match. Check
scenario now requires both (`USABILITY.md:33`).
- **Round never started on from-scratch maps.** The type map was all 1 or copied from the Base. Fixed by a generated
type map with the 0/8 playable rule (`editor\` to `:178`).
- **Washed-out missions.** Colour-map alpha was written as 255 and the fog band was too high. Now the Base
mean alpha is used and FogTop sits 2 m under the lowest ground (`editor\` to `:138`).
- **Crash after a Raise click** (editor, C1). A GPU texture was released while ImGui still drew it; now released after
Present (`USABILITY.md:14`).
- **Empty description showed NULL** (W1) and **map name stayed New Map** (W2). Both fixed in the editor.
- **Test ignored `--mode/--bots/--skill`** (OB1, open in USABILITY). Superseded: the autostart walk runs in game
(`HND:110`, `:139`).

## Open questions

- **Caption not shown.** A custom `CaptionTextID` does not show in the HUD message box (`USABILITY.md:109`).
- **Briefing screen skipped.** With one custom mission the campaign intro plays and the briefing screen is never
shown, so custom `TitleText` and `BriefingText` are untested (`USABILITY.md:108`, to `:220`).
- **Pink cube.** A user mission showed a pink cube on a pole. Rewriting the Ymir to `ymir_low.NODE` did not fix it. RESOLVED 11-09: the pink pole-and-cube marks any object whose build failed, usually a file named in its `.mtb` (debris, rubble, explosion) or the building damage effects not shipped; fixed in editor `dcdcad4` (see the t27 article).
The object is not identified (`HND:6` to `:19`).
- **Boarding.** Neutral units now get `StartAbandoned=1` and a crew controller in the editor source (unpublished).
A scripted Q did not change the player-context actor, so boarding is not proven (`HND:21` to `:29`).
- **Text id range.** Global text ids from `0x00F10000` could reach the name ids at `0x00FF0000` with more than about
917,000 total strings. Not reached in practice; inferred (`ma2_scenario.c:62`, `:63`).
- **`MAPathDB` in MP worlds.** The editor adds one to every saved world, copied from NewRiverCity. Whether this is
harmless is open (to `:206`).
- **Water size.** Water planes have no size property the editor can find (500 m default). Open.
- **Heading zero.** Whether `StartOrientation` uses the same zero as the placement rotation is unverified.
- **Copied mission units** lose their AI orders and kill references.
- **Placed object budget.** The 16 MB warning in SPEC is outdated; the plan is to warn only on the assets archive
(`HND:103` to `:106`).
- **Audio slot limit.** The game only uses `[Audio] n` for n = 1 to 63, and ignores clips numbered 64 and above
(`ma2_scenario.c:629`, `:1763`). The editor's own clip cap was not checked against this.

