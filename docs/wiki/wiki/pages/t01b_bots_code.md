# Bots: source code map

## Overview

The offline bots are a set of native host-C modules in `kit/source/game/src/bots/`. A bot is an ordinary non-human MAPlayer that this machine simulates. The game creates it at level spawn through its own player constructor (`sub_1DAE30`), gives it a control behaviour and an actor, and then one BotController per bot drives the body each simulation step from the player's own tick (`ma2_bot_player_tick`, hooked at `loc_000C8130` in `sub_C80DB`).

The code is layered. `ma2_bots_core.c` is the glue: it snapshots the world into `Ma2World` every step, owns the roster and the is_local_ai registry, the AI line of sight, the ground probe, the grid build and the diagnostics. `ma2_bot_brain*.c` is strategy only. It reads the snapshot and emits one `Ma2Order` per step (travel, get a vehicle, engage, fight-move, eject, hack, idle). `ma2_bot_ctrl.c` executes the order through the game's own interfaces: campaign-AI MoveTo and attack state, the body's jet and claw calls, and command frames sent to the player's control behaviour. `ma2_bot_nav.c` and `ma2_bot_path.c` plan on a 2 m traversability grid built from the game's own rays. `ma2_bot_terrain.c` steers ground units around what the probes . `ma2_bot_entry.c` computes the "Press Y to enter" zones.

Every guest-facing entry point saves and restores the complete guest register state, and with zero bots configured every hook returns without touching guest memory.

## Key code

| Address or function | What it does | Source |
| --- | --- | --- |
| `VA_SESSION` 0x4EBBFC |  | `ma2_bots_core.c:50` |
| `VA_LOCAL_MASK` 0x50C9AC | Mask of local controllers holding humans | `ma2_bots_core.c:51` |
| `VA_PLAYERS_BEGIN/END` 0x50E280 / 0x50E284 | Active player vector bounds | `ma2_bots_core.c:52` |
| `VA_GAME` 0x50C958 | Game object (`game+0x48` = Not-It "It" player) | `ma2_bots_core.c:54` |
| `VA_TERRAIN` 0x4EAF64 | Terrain object; size at +4/+8, cell at +0x814, rows at +0x834 | `ma2_bots_core.c:55` |
| `VA_STEP_TIME` 0x4E2430, `VA_STEP_DT` 0x4E2428, `VA_GAME_TIME` 0x4E2440 | Step stamp, step dt, game clock | `ma2_bots_core.c:56-58` |
| `VA_PAUSED` 0x4EAED4 | Pause flag byte | `ma2_bots_core.c:59` |
| `compute_roster` | Bot counts per side from settings, humans and the cap | `ma2_bots_core.c:365` |
| `mixed_skills` | Skill spread per side for setting 4 | `ma2_bots_core.c:429` |
| `ma2_bot_lobby_record` | Writes the 0x4C-byte lobby record for a bot | `ma2_bots_core.c:519` |
| `ma2_bot_session_set/get` | Briefing Room overrides for vehicle, option, team, ready (session +0x388/+0x398/+0x3A8/+0x3B8) | `ma2_bots_core.c:548,563` |
| `ma2_bots_level_begin` (sub_1DB150 entry) | Computes the roster for the match | `ma2_bots_core.c:739` |
| `ma2_bots_level_spawn` (loc_001DB1E8) | Creates the BotPlayers and brains | `ma2_bot_player.c:43` |
| `bot_create` (sub_1DAE30, sub_1668D8) | Creates one BotPlayer through the game constructor | `ma2_bot_player.c:22` |
| `ma2_bot_map_path`, `ma2_bot_map_install` | Identifies the MP map from its level cache file hash | `ma2_bot_mapid.c:68,97` |
| `ensure_nav` | Waits for terrain, builds the traversability grid, loads or generates the graph | `ma2_bots_core.c:1172` |
| `ray_cast_as` (sub_A10A0, sub_8DF50) | Ray through collision space with a self filter (vtable 0x477CA8) | `ma2_bots_core.c:301` |
| `terrain_height` (sub_2AAA30 via sub_F70A9) | Native terrain height port | `ma2_bots_core.c:214` |
| ground probe (sub_1760E1, apply=0) | Physics ground follow used as a probe | `ma2_bots_core.c:259,277` |
| `ai_los_impl` (sub_17AD5C) | Game AI collision ray, used as bot line of sight | `ma2_bots_core.c:2977` |
| `build_player` | Fills `Ma2PlayerView` (HP, nation, kills, weapons, QTE, riders, VTOL cargo) | `ma2_bots_core.c:1955` |
| `actor_kind` | Body kind from property bits (+0x268, +0x26C, +0x260) | `ma2_bots_core.c:1466` |
| `actor_pos`, `actor_yaw` | Position from scene node +0x94 to node+0xF0; physics +0x4C0 for walkers | `ma2_bots_core.c:1486,1500` |
| `scan_entities` | Vehicles, pickups, pads, turrets, hulls, from the entity tree | `ma2_bots_core.c:2339` |
| `lanes_update` | Main route and flank lanes for team modes, once per match | `ma2_bots_core.c:2638` |
| `world_update` | Fills `Ma2World` each step | `ma2_bots_core.c:2747` |
| `damage_track` | Damage events for the brain's senses | `ma2_bots_core.c:3112` |
| `ma2_bot_player_tick` | Per-step entry for each bot | `ma2_bots_core.c:3513` |
| `ctrl_tick` | Per-step controller: executes the brain's order | `ma2_bot_ctrl.c:2197` |
| `body_moveto`, `body_stop`, `body_walk_direct`, `body_steer` | Locomotion: AI mode 6 MoveTo, mode 7 stop, direct walk, steering | `ma2_bot_ctrl.c:202,215,250,282` |
| `ai_startjump` (sub_18ABB5), `body_jump_*` | Jump-jet use; fuel at actor+0x49C | `ma2_bot_ctrl.c:301-336` |
| `ai_fire` (sub_18482E), `ctrl_fire` | Native fire with discipline checks | `ma2_bot_ctrl.c:365,488` |
| `ba_clamp`, `ba_release` | BattleArmor claw attach and release | `ma2_bot_ctrl.c:383,396` |
| `ctrl_travel` | Walk or drive along the planned path; watchdogs; jet hops | `ma2_bot_ctrl.c:1034` |
| `vtol_prepare`, `vtol_stick`, `vtol_manual` | VTOL cruise height, nap of the earth, stick control | `ma2_bot_ctrl.c:885,1023,991` |
| `ctrl_board` | Boarding walk to the entry zone and enter (command 39) | `ma2_bot_ctrl.c:1593` |
| `ctrl_do_engage`, `ctrl_do_hack`, `ctrl_fight` | Engage, BA hack, fight movement | `ma2_bot_ctrl.c:1838,1993,1770` |
| `ma2_brain_tick` | One decision per step | `ma2_bot_brain.c:2465` |
| `evaluate` | Scores each goal; hysteresis and dwell | `ma2_bot_brain.c:1623` |
| `choose_target` | Target by threat removed per second | `ma2_bot_brain.c:657` |
| `k_kind`, `k_skill` | Kind and skill tables | `ma2_bot_brain.c:106,132` |
| `pick_vehicle`, `vehicle_value` | Vehicle choice; refusal memory | `ma2_bot_brain.c:1187,1222` |
| `alpha_logic`, `qte_control`, `shield_logic` | Alpha Strike, NeuroHack answers, defence use | `ma2_bot_brain.c:1316,1271,1391` |
| `objective_eval`, `obj_order` (brain_obj) | Game-mode objective roles and orders | `ma2_bot_brain_obj.c:269,321` |
| `objective_update` | Objective state from the game (flags, cores, checkpoints, targets) | `ma2_bot_objective.c:267` |
| `ma2_path_plan` | A* on the grid with string pulling | `ma2_bot_path.c:273` |
| `ma2_trav_build`, `ma2_trav_steer` | Grid build and steering | `ma2_bot_nav.c:240,506` |
| `ma2_nav_generate`, `ma2_nav_load_text` | Graph generation and shipped-map load | `ma2_bot_nav.c:810,648` |
| `ma2_terrain_steer` | Ground unit probe fan and steer | `ma2_bot_terrain.c:133` |
| `entry_zones` | Reads MACrateGenerator and MATriggerPoint for entry zones | `ma2_bot_entry.c:69` |
| `ma2_def_*` hooks | Defence objects, absorb, AMS, activation | `ma2_bot_defense.c:218-295` |
| `ma2_bots_newgame_update` (hook at sub_226587) | New Game barrel rows (Red, Blue, Skill) | `ma2_bots_ui.c:867` |
| `ma2_bots_config` / `ma2_bots_config_set` | Settings from ini `[Bots]` plus env overrides | `ma2_bots_ui.c:99,118` |
| `ma2_menu_policy_pulse` | Hides online, split screen and demo entries natively | `ma2_bots_ui.c:849` |

## Added code and hooks

- Core hooks: `ma2_bots_level_begin` at the entry of sub_1DB150; `ma2_bots_level_spawn` at loc_001DB1E8; `ma2_bot_player_tick` at loc_000C8130 in sub_C80DB; `ma2_bot_is_local_ai` and `ma2_bot_local_flags` in the authority gates (player+0x244 bit 2); `ma2_bot_vehicle_ai` at sub_E6C55 (loc_000E6C8E) for actor+0x1E8; `ma2_bot_ai_select` at sub_1837B3; `ma2_bot_world_pause_ok` at sub_17ED0; `ma2_bot_cam_set` in the vehicle-camera block (recomp_0021.c); `ma2_bot_map_install` through the CRT .CRT$XCU initializer, chaining `g_xbox_path_hook`.
- Controller patches: `ma2_bot_ctrl.c` drives the body through the game's own calls (sub_183B9D setmode, MoveTo vtable+0x38, sub_18ABB5 StartJump, sub_18482E fire, sub_18AFDB and sub_18F874 aim, sub_10239A and sub_1019EB clamp, sub_10165B and sub_102102 release, sub_2AE360 and sub_2AD5C0 walk, sub_1760E1 probe) and the command-frame delivery (sub_11AD8B style frames, receiver sub_C8012).
- Defence patches: the TakeDamage entry sub_100A52, MAMover Absorb sub_E9A10, the activation path and the defence SetActive (sub_189462). Command 12 reaches the defence through behaviour vtable+0xA4 (sub_175952).
- UI patches: barrel rows added at sub_226587 with sub_208C45, sub_208BF9 and related helpers; policy widgets keyed by vtable.

## Env vars, settings and test hooks

Settings (ini section `[Bots]`, env overrides in parentheses): `Red` (RECOMP_BOTS_RED), `Blue` (RECOMP_BOTS_BLUE), `FFA` (RECOMP_BOTS_FFA), `Skill` (RECOMP_BOTS_SKILL, default 1 = Regular, 4 = Mixed), `AutoBalance` (RECOMP_BOTS_AUTOBALANCE, default 1). Cap: `RECOMP_BOTS_MAX` (default 12, 6 a side).

Diagnostics and switches that matter most:
- `RECOMP_BOT_DIAG` (1 for logs, 2 adds per-player state): [BOT-ROSTER], [BOT-LEVEL], [BOT-PLAYER], [BOT-NAV], [BOT-PAUSE], [HUMAN-HIT], [DEATH].
- `RECOMP_BOT_GAME_AI=0`: bot bodies do not use the campaign AI.
- `RECOMP_BOT_WORLD_PAUSE=0`: keeps the multiplayer pause rule.
- `RECOMP_BOT_STUCK_MEMORY=1`: replays stuck spots from `<profile>/Bot Nav/<map>.stuck` (off by default).
- `RECOMP_BOT_NAV_MAP=<archive>`: forces a shipped nav archive.
- `RECOMP_BOT_TRAV=0`: skips the grid build. `RECOMP_BOT_PATHTEST`: plans given paths and prints them.
- `RECOMP_BOT_DROPS=0`: no ledge drops. `RECOMP_BOT_MECH_JUMP=0`: no long 'Mech jumps.
- `RECOMP_BOT_UPJET_DUR10` (default 30 = 3 s): up-jump burn length.
- `RECOMP_BOT_OBJECTIVES` (default 1), `RECOMP_BOT_OBJDIAG`, `RECOMP_BOT_CHECKIT_CLAIM` (default 1).
- `RECOMP_BOT_RADAR` (default 300 m).
- `RECOMP_BOT_SKILL_TUNE=0`: keep authored AccuracyRadius and FireDelay.
- `RECOMP_BOT_ATTACK_PILOT=0`: AI flag bits 3 instead of 0x83.
- `RECOMP_BOT_PU_OFF`: power-up armour offset (default 0x20).
- `RECOMP_BOT_PROBE_TERRAIN_ONLY=1`: probe ignores objects.
- `RECOMP_BOT_LANGUAGE`: menu text language index.
- `RECOMP_BOT_PERF=1`, `RECOMP_PROFILE=start:seconds`: timing and sampling profiler.
- `RECOMP_BOT_STALL=1`: samples the game thread when bot ticks stop for 3 s.
- `RECOMP_BOT_WATCH=1`: hardware write watch on the first human's camera controller.
- `RECOMP_BOT_CAMWATCH`: camera field logging.
- `RECOMP_BOT_TEST_JUMP=at:add`: advances the game clock once.
- `RECOMP_BOT_TRACE` (which drive call moved the body), `RECOMP_BOT_JET_SHOW`, `RECOMP_BOT_HULL_TEST`, `RECOMP_BOT_CLIMB_TEST`, `RECOMP_BOT_LIFT_TEST`, `RECOMP_BOT_FLAGRUN_TEST`, `RECOMP_BOT_VTOL_MANUAL`, `RECOMP_BOT_VTOL_SIGNS`, `RECOMP_BOT_VTOL_HEAL_AT`, `RECOMP_BOT_VTOL_SWAP_P`, `RECOMP_BOT_QTE_MISS_MECH`, `RECOMP_BOT_QTE_MISS_BA`: developer tests (see the files for each).
- `RECOMP_DEFENSE_PROBE=1`, `RECOMP_DEFENSE_TEST=<s>` (with `RECOMP_DEFENSE_TEST_CMD`, default 12), `RECOMP_DEFENSE_CATALOG`, `RECOMP_DEFENSE_TEST_ALPHA`.
- `RECOMP_ALPHA_TRACE`: Alpha Strike trace.
- `RECOMP_HUMAN_ACT`: logs which action the human's input lights up.
- `RECOMP_MENU_TRACE`, `RECOMP_MENU_DUMP`, `RECOMP_FOOTER_TRACE`, `RECOMP_ONLINE_MENUS`, `RECOMP_SPLITSCREEN_MENUS`, `RECOMP_DEMO_MENUS`: menu policy and dump switches.
- `RECOMP_PC_SETTINGS_INI`: the settings ini path used by the bots profile.

## Bugs and fixes (history)

- 2026-10-13/14: crash in the ray filter when the probe actor was freed (a bot pilot boarding a VTOL). `probe_live` revalidates the probe actor each frame. Shipped.
- 2026-10-14: a bot in a fixed gun took over the human's screen with its own gun camera. The vehicle-camera block skips bots (recomp_0021.c) and `ma2_bot_cam_set` refuses them. Shipped.
- 2026-10-14: `getenv` was a large share of the game thread in a 6v6 match. `ma2_bot_env.h` caches each lookup by string address. Shipped.
- 2026-10-14: Alpha Strike needed the player's trigger record copied to player+0x3C4 and +0x3E0 (bots never do the copy). `ctrl_player_trigger` writes it.
- 2026-10-14: a 'Mech walking under its AI sleep moved a third of its speed; the AI is kept awake in mode 6.
- 2026-10-14: VTOL pressed against a hillside; cruise height now 20 m over the highest surface ahead. A later fix stopped climbing counting as stuck.
- 2026-10-19: a VTOL's entry zone sits under its nose, and the pilot stood at the rim for 20 s (Overfalls). Done once within the rim plus 4 m.
- 2026-10-20: AutoBalance split 1 Red and 4 Blue into 3v3 with the human. Bots are now placed by the counts set, with the players on the short side.
- 2026-10-20: a spawn-camping human was the ace of a match. Mixed now raises the weakest bot facing humans one notch.
- Off by default (`RECOMP_BOT_STUCK_MEMORY`).
- Undated: a BattleArmor on a VTOL claw was knocked off; the VTOL carries it 12 m higher.
- Undated: the jet sputtered when the tank ran dry; the controller now ends the burn cleanly. Shipped in ma2_bot_ctrl.c.
- Undated: bots fought on while a human sat in the pause menu; bots now stand down while paused.
- Undated: a parked BattleArmor refused every pilot (0 of 17). Refusal memory now skips it for the roster.

## Open questions

- The api header (`ma2_bot_api.h:332`) says the defend order taps command 52. `ma2_bot_defense.c:14` says the defence command is 12 and that 49.55 are NeuroHack answers. The defence file is treated as current here. Status: superseded (api comment).
- Those files are not in this pack.
- The power-up armour offset is a guess (`RECOMP_BOT_PU_OFF`, default 0x20 with 0 as the alternative). Its confirmation is not in these files.
- Some Ma2PlayerView fields are approximations: `ma2_bots_core.c:1979` notes the MAMech health ints for 'Mechs, and the radar range is a TODO (`ma2_bots_core.c:2758`, HUD radar at +0x18C not yet read).
- The `.orig` and `.pre_airblk` copies of `ma2_bots_core.c` and `ma2_bot_ctrl.c` were not in this pack and were not read.

