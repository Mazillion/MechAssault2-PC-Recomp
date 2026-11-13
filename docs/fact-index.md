# Fact index: how to search the wiki

The fact wiki (`docs/wiki/`) holds 3,305 facts in 37 topics about how the recompiled game works: what a function does,
which global holds what, which switch changes which behaviour, what a bug was and how it was fixed. This page explains how
to find things and lists the topics. How to add or correct a fact is in `docs/wiki/WIKI_BRIEF.md`.

## 1. Searching

```
cd docs/wiki
python wq.py "BaLand"          # facts mentioning BaLand: id, status, confidence, date, title, snippet
python wq.py "003AACE7"        # everything known about an address
python wq.py "frame ?limit" 0  # titles only (second argument = snippet length)
```

`wq.py` needs only Python 3 and reads `wiki/wiki.json`. It matches a regular expression, case-insensitively, against the
id, title, text, how-to-use, related names and addresses of every fact. You can also search the readable file with any
text tool, for example `grep -n "RECOMP_BOT_NAV_CACHE" wiki/WIKI.md | cut -c1-300`; it is large, so do not open it whole.

What the generated files hold:

| File | Content |
|---|---|
| `wiki/WIKI.md` | Per topic: the article (if any), then every fact; then an **address index** (every address with a fact, the owning function, fact titles) and a list of **environment variables** with the facts about each. |
| `wiki/wiki.json` | The same as data: `topics`, `facts`, `addresses`, `env`. |
| `wiki/facts/<pack>.json` | The source. Edit facts here, never in the generated files. |
| `wiki/overrides.json` | Corrections to older facts (a changed status, an appended CORRECTION or UPDATE sentence). |
| `wiki/raw/functions.json` | Function start addresses, sizes, categories and owner names of `ma2.xbe` (no strings). |

## 2. Reading a fact

Each fact has an id (`t28.ba-walker-bit-cleared-by-jet`: the pack prefix and the fact id), a subject, addresses, a text,
how to use or test it, a status, a confidence, sources, tags and a date.

- **status**: `current` (not printed in `WIKI.md`), `superseded`, `historical`, `open-question`. A superseded fact names its
  replacement; newer facts and corrections in `overrides.json` win.
- **confidence**: `verified-in-game` (observed in a run), `code-confirmed` (read in code or disassembly), `documented`
  (stated in a note, not re-proven), `inferred`.
- **sources**: repository paths such as `kit/source/game/src/bots/ma2_bot_ctrl.c` with a line number. Generated code is cited as
  `generated recomp_XXXX.c` (made at install time, not in the repository). Many facts have no source because the evidence was
  a run log or a measurement.
- **how to use**: the switch, environment variable, build or release in which something shipped, or how to test it.

## 3. Address conventions

- Addresses are virtual addresses in the Xbox executable `ma2.xbe` of the US/NTSC retail disc, image base `0x00010000`.
  Indexed addresses lie in `00010000`..`005FFFFF` and are written as **8 upper-case hex digits** in the `addresses` field
  (`003AE754`).
- `sub_0018ABB5` (or `sub_18ABB5`) is the function that starts at that address; `loc_001DB1E8` is a label inside a function,
  used as a patch anchor. Facts also write short forms such as `0x18ABB5` or `18ABB5`.
- Object offsets are written `ai+0x20C`, `comp+0x144`, `session+0x3A8`: a base name and a hex offset. A bit is written
  `comp+0x144 bit 15`.
- Vtables are written as the vtable address (`vt 0x469CD0`) and slot or offset (`slot 16 (+0x40)`).
- Generated files are named `recomp_NNNN.c`; a function's file is shown in the address index (from `raw/functions.json`).
- Addresses that could not be placed in the index are kept in `addresses_unverified` and do not appear in the address index.
- Kernel imports live at synthetic addresses `0xFE000000 + 4 x slot`; hardware apertures are NV2A `0xFD000000`, MCPX
  `0xFE800000`, flash `0xFF000000` (see [architecture.md](architecture.md)).
- `RECOMP_*` names are environment variables, `[Section] Key` names are `pc_settings.ini` entries (for example
  `RECOMP_BOT_NAV_CACHE` / `[Bots] NavCache`). Every environment variable with a fact is listed at the end of `WIKI.md`.

## 4. Topics

Pack prefix (`t01a`, `t02`, ...) = the part of an id before the dot. Counts are facts per pack.

| Pack | Facts | What it covers |
|---|---|---|
| `t01a_bots_docs` | 226 | Bot design and behaviour: native player model, controller, boarding, jumpjets, VTOL, defence, teams, skills, navigation, env switches. |
| `t01b_bots_code` | 153 | Bot source map: modules, globals and offsets the bot code reads, roster, nav loading, QTE and clamp offsets. |
| `t02_audio` | 86 | APU voice processor, GP/EP DSP, XAudio2 output, HRTF, clipping, voice tails, XACT freezes. |
| `t03_render` | 99 | NV2A push-buffer executor, D3D11 path, shader cache and seed, texture packs, HUD at widescreen, presentation, trace switches. |
| `t04_scenario` | 168 | Custom scenarios: `.ma2map` and MGF v4, scenario.ini, game-side loading, Custom Scenarios menu, autostart, editor settings. |
| `t05_gameplay` | 85 | Mouse aim per vehicle class, 30 Hz tank physics, QTE prompts, name entry, missions, checkpoints. |
| `t06_ui_pc` | 104 | PC front end: menus, input, settings file, saves, rumble, packaging, installer. |
| `t07_stability` | 130 | Memory map, kernel bridge, threads, freezes and crashes with causes and fixes. |
| `t08_toolchain` | 204 | The static recompiler: XBE parsing, lifting conventions, generated-code macros, debugging aids, lookup tools. |
| `t09_bringup_history` | 78 | Early bring-up findings: controller bridge, rumble, tank physics, HRTF option, movies, crash analyses. |
| `t10_status_misc` | 215 | Bring-up status and miscellaneous engine findings (USB, timers, pacing, paths, tools). |
| `t11_editor_verified` | 2 | Scenario editor findings verified in game: unit models, boarding. |
| `t12_code_research` | 1088 | Code research: object properties (MA* classes), triggers and actions, rendering details, audio, save/menu code, editor-to-game mapping. Largest pack; one finding per fact. |
| `t12_first_pass_research` | 71 | First-pass research on editor and render details (some entries are corrected by later facts). |
| `t13_movies_frontend` | 8 | Movies and front-end flow. |
| `t13_push_perf_pathing_1027` | 65 | Bot pathing and performance pass of 2026-10-27. |
| `t14_push_surprise_1030` | 104 | Bot surprise behaviours, ground navigation, VTOL ferry, frame clock. |
| `t14_start_vehicle` | 12 | Mission start vehicles: which body the player spawns in and which files it needs. |
| `t14b_start_vehicle_logs` | 7 | Run-log evidence for the start-vehicle facts. |
| `t15_editor_render_gaps` | 16 | Where the editor view differs from the game: terrain detail, water, lighting, fog, effects. |
| `t15_pipeline` | 73 | Native-pipeline performance: frame profile, pushbuffer cost, guest CPU, threading, FastCode. |
| `t16_editor_to_game` | 56 | What each editor control writes and how the game uses it; surprises and gaps. |
| `t18_ctf_vtol_1104` | 25 | Bot CTF VTOL flag runs: route, boarding, opening pair. |
| `t19_bot_audit_1104` | 31 | Bot behaviour audit, pass 2: one fact per finding. |
| `t20_balance_sweep_1107` | 7 | Side-balance sweep of the objective modes. |
| `t20_campaign_picker_centre_1105` | 4 | Mission picker barrel: Custom Scenarios shift, Campaign centring. |
| `t21_balance_round2_1107` | 14 | Side-balance sweep round 2: open items closed. |
| `t22_objreach_1108` | 6 | Objective-reach audit: tank slope, vehicle spawn pins, Check It walks. |
| `t23_objreach2_1108` | 7 | Objective-reach pass 2: exits, faces, jet-ups, tank embeds. |
| `t24_film_review_1108` | 19 | Film review and arrival parity: bot play judged against human play. |
| `t25_dsound_defer_guard_1109` | 1 | DirectSound deferred-list double insert (audio-thread crash). |
| `t26_dsound_active_guard_1109` | 1 | DirectSound active-list re-insert (freeze in BattleArmor death blasts). |
| `t27_pink_placeholder_mtb_refs_1109` | 3 | Pink placeholder in custom scenarios: templates named inside `.mtb` files. |
| `t28_release_polish_1110` | 62 | Release polish: fixes of 2026-10-31 to 2026-11-09, one fact per fix. |
| `t28_vtol_ferry_speed_1110` | 4 | VTOL ferry crawl: stale AI forward speed in the body integrator. |
| `t29_lens_flash_freeze_1110` | 8 | Explosion lens flash and the screen-haze fix. |
| `t3_first_pass_research` | 63 | First-pass research on bots and custom-map details. |

Topic articles (`wiki/pages/<pack>.md`) exist for the larger packs and give an overview, key code, added hooks, switches,
a bug-and-fix history and open questions before the facts.

## 5. Related documents

- [architecture.md](architecture.md): how the recompilation, runtime and PC layer fit together.
- [bots.md](bots.md): the bot system.
- [editor.md](editor.md): the scenario editor and custom scenarios.
- [building.md](building.md) and [patches.md](patches.md): building the project and the patch set.
- `wiki/README.md`: layout of the wiki directory and the contribution procedure.
