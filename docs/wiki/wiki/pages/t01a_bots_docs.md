# Bots: design, behaviours and tuning

This article covers the offline bot match for MechAssault 2 PC: how the bots hook into the game, how they behave, the team and skill rules, navigation, known bugs and their fixes, and the test hooks.

## Overview

A bot is a non-human `MAPlayer` that the local machine simulates. The native build (2026-10-13 onward) creates bots as ordinary players after the lobby iterator, gives each one a BotController that sends command frames each step, and drives vehicles through the game's own campaign AI. The earlier rev 2 guide (2026-10-11) instead made bots lobby records driven by a virtual 20-entry pad block. That design is superseded.

Each bot runs a brain with a perception layer (10 Hz), a memory of enemies, a utility-scored goal choice (GetVehicle, Attack, NeuroHack, BattleArmor aggression, Retreat, Defend vs hack, Eject, VTOL, Roam) and a controller that issues commands for the current goal. The native brain keeps these goal names and hysteresis; the pad mechanics are gone.

Navigation uses a grid planner over the walkable surface, with jumpjet hops, VTOL flight and one-way cliff drops.

## Key code

| Address or function | What it does |
| --- | --- |
| `sub_1DAE30` (`recomp_0050.c`) | Per-record player constructor: CreatePlayer, vehicle, SetNation. The non-human tail is at `loc_001DB093`. |
| `sub_1668D8` (`recomp_0037.c`) | CreatePlayer: clones the Player template and sets bit 2 for humans. |
| `sub_166C0C` (`recomp_0037.c`) | Vehicle creation plus MAInput for humans only. Bots get MAInput through the bit-0 path. |
| `loc_001DB1E8` (`recomp_0050.c`) | Native hook: `ma2_bots_level_spawn(ebx)` after the lobby iterator. |
| `loc_000C8130` (`recomp_0020.c`) | Native hook: `ma2_bot_player_tick(esi)`, the BotController step. |
| `sub_C8012`, `sub_C7F8F`, `sub_1AFE3B` | Player command receiver and dispatch of the type 0x67 frame, end-of-frame at id 60. |
| `sub_C9084`, `sub_C950E` | Control behaviour factory and per-tick rebind. |
| `sub_CA573` | Respawn: sets bit 6 and a respawn time 4 s ahead. |
| `sub_CA004`, `loc_000CA144` | New vehicle of a non-human gets the network-proxy flag. |
| `ai` vtable `+0x38` (MoveTo) | Thiscall MoveTo for MechAI `sub_18A38E`, TankAI `sub_18F4FE`, FlyerAI `sub_191175`. |
| `sub_183345` | Campaign AI tick. Runs only when the AI is awake and comp+0x144 bit 0 is set. |
| `sub_183B9D` | setmode: sets ai+0x12C. Mode 6 is go-to-point-and-stop. |
| `sub_2AC940` | Sets comp+0x144 bit 0 so the human control tick leaves the body alone. |
| `sub_2AD5C0`, `sub_2AE360` | Stop at a percentage of max speed; face a heading. |
| `sub_18ABB5` | StartJump: starts a jumpjet hop; MoveTo then flies it. |
| `sub_104D55` | Raw jet direction and on/off. |
| `sub_10239A`, `sub_1019EB`, `sub_102AC9`, `sub_10165B` | BattleArmor clamp: test, attach, candidate search (11 m), attach vtable call. |
| `sub_18AFDB`, `sub_18F874` | Torso aim for 'Mechs, turret and barrel aim for tanks. |
| `sub_18482E` | Native fire: fires every selected weapon of the group through `sub_12B234`. |
| `sub_113964`, `sub_10E40E`, `sub_2AE860` | VTOL yaw-hold bit set, cleared on take-off, and the yaw controller it blocks. |
| `ai+0x20C` (FlyerAI) | Wanted altitude for a VTOL. |
| `sub_17AD5C` | The AI's line-of-sight ray; clear when nothing lies between shooter and victim. |
| `sub_2AAA30`, `sub_F70A9` | Terrain height port (terrain object at `0x4EAF64`). |
| `sub_1760E1` | Object ground probe; call with apply = 0 so contact state is not reset. |
| `sub_2B1560`, `sub_2B1D60` | PathMap ctor and parser for the shipped `PATHFINDING/*_Mech.map`. |
| `sub_2B2B10`, `0x524B94` | The game's A* search, singleton. |
| `sub_E9CB6`, `sub_189462`, `sub_1895F9` | Defence activation, SetActive and expiry. |
| `sub_E9A10`, `sub_1872A0` | Absorber path: 25 to 29 percent of energy hits pass on a Reflective unit. |
| `sub_DF238`, `sub_12AE8E`, `sub_12B71A` | Alpha Strike class 7 selection and readiness. |
| `sub_CC06A`, `sub_CC083`, `sub_DE453`, `sub_D3806` | Player trigger path for Alpha Strike charging. |
| `sub_16B2F1`, `sub_16B73E`, `sub_16B7B8` | Camera manager set-current, activate and slot. Bot-owned cameras are refused. |
| `sub_16380`, `0x50C88C` | Audio listener rebuild and its four-slot singleton. |
| `sub_2B990` | Rumble with a four-entry port table; bot ports are guarded. |
| `sub_1D4710`, `sub_1D4670`, `sub_1D4550`, `sub_1D4F20` | Launch-check helpers: all ready, Red == Blue, team spread, count. |
| `sub_226587` (`recomp_0058.c`) | New Game per-frame update; the bot hook runs first. |
| `0x0208C45`, `0x0208BF9`, `0x0208BD0` | Barrel APIs: add item, select by value, find index. |

## Added code and hooks

The bot code lives in (the native build). The main modules are `ma2_bot_player.c` (BotPlayer creation and registration), `ma2_bot_ctrl.c` (controller, command frames, boarding, jumps, clamp, fire), `ma2_bot_brain.c` (goals and tactics), `ma2_bots_core.c` (world snapshot, wiring, diagnostics), `ma2_bot_path.c` (grid planner) and `ma2_bot_defense.c` (defence and weapon values).

The native build is applied by `patch_native.py`. It removes the lobby-era camera, view, listener and proxy hooks listed in its header, and adds the native spawn hook at `loc_001DB1E8`, the BotController tick at `loc_000C8130` and a diagnostic view counter. It keeps the rumble guard, the authority gates keyed on `is_local_ai` and the lobby listing hooks for the Briefing Room. Each patch script is idempotent and anchor-checked (`kit/source/game/bringup/staging/bots/core/patch_bots.py`).

The lobby-era build used `patch_bots.py` (`kit/source/game/bringup/staging/bots/core/patch_bots.py`). It appended bot lobby records at `loc_001D3F44`, routed session setters for ids 4 and up to a bot table, and applied `ma2_bot_local_flags` at nine human-only tests. It also added the Briefing Room count hook at `loc_001F8CF3` and the level-begin hook at `loc_001DB150`. The native build replaces the record and pad parts of this.

Round patches, applied to the bots tree in order:

- Round 2: `sub_F8DE3` accepts bot owners, and the mouse primary is decided per player (`patch_round2.py:1`).
- Round 3 and 4: bot POV camera gates (`patch_round3.py:1`, `patch_round4.py:1`). Superseded by the native removal.
- Round 5: bots can board, through `sub_C8576` and `sub_C86EC` (`patch_round5.py:1`).
- Round 6: rumble guard for bot ports (`patch_round6.py:3`).
- Round 7: audio listener filter and loop cap (`patch_round7.py:3`).
- Round 8: bot-owned cameras are refused at `sub_16B2F1` and `sub_16B73E`.
- Round 9: the hack cue flag is cleared for bot actors (`patch_round9.py:3`).
- Round 10: `sub_E6C8E` lets the campaign AI drive bot vehicles (`patch_round10.py:3`).
- Entry zones: exact zone centres and radius for each vehicle.
- XACT timer heap guard.
- Scoreboard widescreen column width.
- Asset overlay for the New Game rows in `common.mgf`.
- Defence probe hooks.

## Behaviours

**Boarding.** A bot on foot walks to a vehicle entry zone and taps command 39 (Y) once the zone offers the vehicle. Only frames with `+0x428` set can board, and the offer comes from the zone through `sub_C86EC`. Boarding takes about the path length over 9 m/s plus 2.5 s. A sweep of 236 boardings averaged 7.0 s. Where a vehicle has no active zone, or belongs to the enemy nation in a team mode, the bot does not use it.

**Jumpjets and BattleArmor.** StartJump (`sub_18ABB5`) sets the hop and MoveTo flies it; the hop covers 45 to 55 m every 4 s. CORRECTION 2026-11-10: BattleArmor walking works (a bot walks 11-15 m/s, a player's BattleArmor 15 m/s). The old 'walks 0 m/s, hops only' reading was wrong: a jet clears the walker bit comp+0x145 bit 0 and nothing set it back, so the bot ran the walk animation on the spot until BaLand/BaWalk (commit a8eab5c, in PC Release par24) restore it; see `t28.ba-walker-bit-cleared-by-jet`. BattleArmor still uses jets for long moves and climbs. The jet sound stutter came from the thrust update running twice per frame; the fix holds the jump action while the jump is live.

**NeuroHack.** A BattleArmor within about 40 m of an acquired enemy 'Mech paths to a point 4 m behind it, jumps inside 15 m and taps A inside 7 m. The clamp at 12 m succeeded on the first try in a test. The bot answers its own QTE with the skill's reaction time and never presses a wrong button. Clamps are rate limited to one per 20 s.

**Defence.** The RS-click defence is command 12, not 52; command 52 is NeuroHack answer bit 4. A bot raises its own defence when `def_own_ready` says it is present and idle. It picks weapons by `wpn_value_vs`, which avoids energy against Reflective, ballistic against Reactive, missiles against AMS and seekers against a jammer.

**Alpha Strike.** The game makes class 7 available when classes 0 to 2 are at their highest level, and the bot's copy of the trigger record must be refreshed each frame for the charge to hold. Alpha Strike has worked for bots since round 9.

**VTOL.** The yaw-hold bit (comp+0x144 bit 15) blocks AI yaw; clearing it after take-off lets MoveTo steer the craft. Altitude is set through `ai+0x20C`. The FlyerAI holds its altitude, so the bot uses MoveTo while climbing more than 5 m and the awake drive at cruise height. A VTOL cannot pick up power-ups; it hooks one with A (37) and drops it with B (38). Holding Y lowers the craft and gets the pilot out, and over a helipad an A press at 5.6 to 6.0 m repairs it fully.

**Tanks and fixed guns.** A tank cannot turn on the spot while the navigator keeps its turn radius; setting it to zero makes the in-place turn work. A MATank with zero drive limits is a fixed gun and is never ridden.

**Objectives.** CTF runs a per-nation flag run with a VTOL and a BattleArmor that carries the flag home. Grinder is capped at one bot, and the bot swaps to a Mech early.

**Targeting.** Targets are chosen by kill value, with weak enemies finished first, teammates' targets preferred and the scoreboard leader weighted up.

## Team rules and skill

The counts in `[Bots]` are literal. Red Bots and Blue Bots mean the number set, and no side can exceed six players including the human. The earlier AutoBalance split, which made 1 Red and 4 Blue into 3 v 3, is superseded. Setting the counts places the player on the short side. The game resets every side to Red while it builds the lobby, so `place_hold` re-applies the team for the first 30 frames.

The total cap defaults to 12 players. Earlier documents give 8; the later value wins.

Mixed skill (config value 4) gives each side a mix of Elite, Veteran, Regular and Recruit, and the side facing the player has its weakest place raised one level. The campaign-AI skill table for Recruit, Regular, Veteran and Elite sets reaction delays of 1.6, 0.8, 0.35 and 0.1 s and engage range multipliers of 0.6, 0.8, 1.0 and 1.2. The brain's own skill table sets aim error at 9, 6.5, 4 and 1.5 degrees.

## Navigation

The shipped nav graphs exist for four maps only: inyourface, newrivercity, rushhour and witchcauldron. They are text with nodes on a 20 unit grid. The other maps get a generated grid over the terrain bounds, cached under `%LOCALAPPDATA%\MechAssault2\BotNav\`.

The current planner uses a walkable-surface grid. A cell is blocked where the step to a neighbour is over 1.6 m, roofs are jet-only islands and bridges and ramps are walkable. A thin static wall that the 2 m probe misses is found by a 13-ray sweep at each stall. One-way cliff drops are allowed over natural terrain only.

The level identity comes from the cache file name, which is the CRC32 of the lower-case archive path. The module measured 0.03 to 0.08 ms per A* query on the four shipped graphs.

## Env vars, settings and test hooks

| Name | Effect | Source |
| --- | --- | --- |
| `[Bots] Red, Blue, FFA, Skill, AutoBalance` | Settings in `pc_settings.ini`; defaults 0, 0, 0, 1, 1. |  |
| `RECOMP_BOTS_RED/BLUE/FFA/SKILL/AUTOBALANCE/MAX` | Override the settings; MAX takes 2 to 12. |  |
| `RECOMP_BOT_DIAG=1` | One `[BOT]` line per bot per second. |  |
| `RECOMP_BOT_DIAG_NAV=1` | Dumps the nav graph. |  |
| `RECOMP_BOT_NAV_REGION="x0,z0,x1,z1"` | Prints heights and blocks of a region. |  |
| `RECOMP_BOT_PATHTEST="ax,az,bx,bz,r,crush,drop | "` | Path test; separator is ` |
| `RECOMP_BOT_NAV_FILE`, `RECOMP_BOT_NAV_MAP`, `RECOMP_BOT_NAV_STEP` | Explicit nav file, forced map archive, grid step. |  |
| `RECOMP_BOT_RADAR` | Radar range; default 300 m in core, 200 m in brain (see open questions). |  |
| `RECOMP_BOT_WORLD_PAUSE=0` | Restores the MP pause rule (world keeps running). |  |
| `RECOMP_BOT_TEST_JUMP=at:add` | Advances the game clock once to exercise match end. |  |
| `RECOMP_BOT_SCOREDUMP=1` | Dumps per-player dwords every 10 s. |  |
| `RECOMP_BOT_STALL=1` | Stall monitor sampling all threads. |  |
| `RECOMP_BOT_TRACE=<s>` | Per-step trace used to measure speeds. |  |
| `RECOMP_BOT_VTOL_TRACE=1` | `[BOT-VTRACE]` VTOL flight log. |  |
| `RECOMP_BOT_FLAGRUN_TEST=1`, `RECOMP_BOT_FLAGRUN=0` | Always roll the CTF flag run; turn it off. |  |
| `RECOMP_BOT_CLIMB_TEST` | Test-only BattleArmor wall climb. |  |
| `RECOMP_BOT_STUCK_MEMORY=1` | Enables persistent stuck memory. |  |
| `RECOMP_BOT_LEADER_FOCUS=0` | Turns off scoreboard-leader targeting. |  |
| `RECOMP_BOT_NO_SWAP` | Turns off the Grinder BattleArmor-to-Mech swap. |  |
| `RECOMP_BOT_LANGUAGE=0.5` | Forces a bot text language. |  |
| `RECOMP_TEST_TURRET_PICK=1`, `RECOMP_TEST_TURRET_DRIVE=1` | Reproduces the VTOL camera hijack on Bogged Down. |  |
| `RECOMP_TEST_HUMAN_KILLS=N` | Sets a human kill count for targeting tests. |  |
| `RECOMP_HUMAN_ACT` | Measures which actions a pad button lights. |  |
| `RECOMP_DEFENSE_PROBE=1`, `RECOMP_DEFENSE_TEST=<s>`, `RECOMP_DEFENSE_CATALOG=1` | Defence probe, forced tap every s seconds, catalogue dump. |  |
| `RECOMP_DEFENSE_TEST_ALPHA=<bot\ | 99>:<t>` | Forces class 7 fire for the Alpha Strike test. |
| `RECOMP_LOCO_TEST=1\ | 2\ | 3`, `RECOMP_LOCO_BOTS=bot:kind,` |
| `RECOMP_BAVT_TEST=1`, `RECOMP_BAVT_SCRIPT=<letters>`, `RECOMP_BAVT_PT=<s>` | BattleArmor, VTOL and tank phase scripts. |  |
| `RECOMP_BAVT_SPAWN=1`, `RECOMP_BAVT_SPAWN_UID=815F\ | 80BC` | Spawns a test player straight into a VTOL or BattleArmor. |
| `RECOMP_PLIFE` | Test hook for the player-life experiments (p2 to p8). |  |
| `RECOMP_AUDIO_ISR_STATS` | Prints XACT ISR tries and skips. |  |
| `REPLAY_EXTRA`, `REPLAY_FFA`, `REPLAY_USERENV`, `REPLAY_WIDTH`, `REPLAY_HEIGHT`, `REPLAY_SHOTS` | Replay harness settings; FFA types need `REPLAY_FFA > 0`. |  |

Scripts for the test runs: allows two game instances at once. `kit/source/game/bringup/staging/bots/tests/probe_bots.py` runs the pass criteria (no crash, every bot boards within 60 s, no bot stuck over 10 s, over half the bots score, the scoreboard lists all bots, every bot moves). Its navigation after the Multiplayer hub was not verified. The lobby fixture `kit/source/game/bringup/staging/bots/tests/bots_lobby_verify.py` checks that zero bots gives memory identical to the baseline.

## Bugs and fixes (history)

- **Lobby-era spawn and pads (2026-10-11).** Bot input went through a virtual pad and the lobby records were patched. Native BotPlayers replaced both on 2026-10-13.
- **Bots not moving.** A non-human player's vehicle was flagged as a network proxy; the native build clears the proxy bits.
- **Mech sluggish at a third of speed.** An asleep MechAI ran its tick with the not-simulated bit each frame. The fix keeps it awake in mode 6 and zeroes the engage radius.
- **Pilots wedged against parked Mech hulls.** MoveTo does not move a body touching a hull; `walk_direct` away from the hull fixed it.
- **Jetpack sound stutter.** The thrust update ran twice per frame; the cue restarted each frame. The jump action is now held while the jet is live.
- **Jet burn outlasting fuel.** The burn is capped to fuel minus 0.15, and the jump ends below 0.1.
- **Rumble crash for bot ports.** Ports of 4 and up overran the rumble table; the guard returns early.
- **Listener freeze.** Bot cameras overflowed the audio listener set; the count went to garbage and `sub_BFED0` spun forever. Fixed by filtering and capping in `patch_round7.py`, and superseded in native.
- **XACT freeze and crash in the APU interrupt.** The ISR ran concurrently with the DPC and could leave a self-linked node. The fix try-locks the API lock and skips when it is held. The XACT timer heap got a guard in `patch_xact_heap_guard.py`; its cause is still open.
- **Bots hacking audibly for everyone.** The minigame cue flag is cleared for bot actors.
- **VTOL camera hijack.** A bot in a fixed gun was given the human's screen camera. Bot cameras are now refused.
- **VTOL never steering.** The yaw-hold bit is cleared after take-off.
- **BattleArmor not moving / running on the spot.** First thought to be animation-driven walker speed (superseded). Real cause: a jet clears the walker bit comp+0x145 bit 0 and it was never restored; fixed by BaLand + BaWalk (a8eab5c, shipped in par24). See `t28.ba-walker-bit-cleared-by-jet`.
- **Tank turning on the spot.** The navigator's turn radius is zeroed before the turn.
- **Alpha Strike never charging.** The copy of the trigger record is refreshed each frame for bots.
- **Scoreboard columns out of the panel at widescreen.** The row drawer's caller gets the widened width.
- **Mouse never attaching to the human.** A bot's input component ran last and took the primary context; the primary is now decided per player.
- **Game-thread crash after a probe actor was freed.** `probe_live` checks vtable and physics back-links.
- **Parked BattleArmor refusing every pilot on In Your Face.** A shared refused list with timeouts stops the retries.
- **Cliff and drop wedges on Bogged Down.** Tank drops are limited to 6 m and hops are checked for structures.

## Open questions

- The HUD radar property at `+0x18C` (`sub_2057F5`) would settle it.
- What reads it in the Briefing Room is not checked.
- **PPC from the AI.** The AI fired only three PPC hits in 13 runs; a hold-charge controller is needed.
- **Tank turret error.** Residual turret error of 5 to 35 degrees after 8 s appeared in four cases.
- **BattleArmor wall grip.** The claw swing `sub_FE09B` that grips a wall was not traced.
- **Lift for BattleArmor.** The `g_lift` code is untested.
- **Audio freezes.** The deadlock recurs about once every 20 to 25 matches, and In Your Face shows a 3 s hitch about once every 30.
- **Game-thread crash in deferred deletion.** Seen once on Rush Hour FFA; not reproduced.
- **Defence probe audio crash.** One DSOUND read of APU MMIO in 14 runs.
- **On In Your Face, a pilot boxed in for 70 s** near a raised tactical spot.
- **Probe navigation.** The Multiplayer menu steps of `probe_bots.py` are unverified.

