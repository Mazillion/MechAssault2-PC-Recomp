# Bots

Overview of the offline bot system for developers. Facts and measurements are in the wiki; ids such as
`t01a.bots-native-player-model` can be looked up with `python wq.py "<id>"` in `docs/wiki/` ([fact-index.md](fact-index.md)).
Packs: `t01a_bots_docs` (design and behaviour), `t01b_bots_code` (source map), `t13_push_perf_pathing_1027`,
`t14_push_surprise_1030`, `t14_start_vehicle`, `t18_ctf_vtol_1104`, `t19_bot_audit_1104`, `t20` to `t24` (balance,
objective reach, film review) and `t28` (release changes).

Bots are an addition of this project, not an Xbox feature. The multiplayer menus gain a Bots option; matches run offline
with computer-controlled players on both sides.

## 1. Model: native non-human players

A bot is an ordinary non-human `MAPlayer` that the local machine simulates (`t01a.bots-native-player-model`). The game
creates it at level spawn through its own player constructor `sub_1DAE30` after the lobby iterator, hooked at
`loc_001DB1E8` (`t01a.bots-native-spawn-hook`, `t01b.level-spawn-hook`, `t01b.bot-create-sub1dae30`). One `BotController`
per bot drives the body every simulation step from the player's own tick (`ma2_bot_player_tick`, hooked at
`loc_000C8130` in `sub_C80DB`; `t01a.bots-player-tick-hook`). With zero bots configured every hook returns without
touching guest memory (`t01b.bots-zero-footprint`).

Authority gates in the game test an `is_local_ai` registry instead of the port or human bit
(`t01a.bots-is-local-ai-authority`, `t01b.is-local-ai-registry`); non-human vehicles are flagged as network proxies, and the
native build clears those bits so vehicles actually move (`t01a.bots-player-flags-proxy`). Controller ids of 4 and above are
bots in the lobby tables (`t01b.session-field-offsets`). An earlier design that added bots as lobby records driven by a
virtual pad block was superseded (`t01a.bots-lobby-record-design-superseded`).

## 2. Layers and source files

Source is in `kit/source/game/src/bots/` (`t01b.bots-overview-layering`, `t01a.bots-five-layer-architecture`):

| File | Role |
|---|---|
| `ma2_bots_core.c` | Glue: snapshots the world into `Ma2World` every step, owns the roster and the `is_local_ai` registry, AI line of sight, ground probe, grid build, diagnostics. |
| `ma2_bot_brain*.c` | Strategy only: reads the snapshot, emits one `Ma2Order` per step (travel, get a vehicle, engage, fight-move, eject, hack, idle). Perception at 10 Hz, memory of enemies, utility-scored goals. |
| `ma2_bot_ctrl.c` | Executes the order through the game's own interfaces: campaign-AI MoveTo and attack state, the body's jet and claw calls, command frames to the player's control behaviour (boarding, jumps, clamp, fire). |
| `ma2_bot_nav.c`, `ma2_bot_path.c` | Planning on a 2 m traversability grid built from the game's own collision rays. |
| `ma2_bot_terrain.c` | Steers ground units around what the probes find. |
| `ma2_bot_entry.c` | Computes the "enter vehicle" zones. |
| `ma2_bot_defense.c` | Defence and weapon values (reflective, reactive, AMS, jammer, absorber). |
| `ma2_bot_surprise*.c` | Rare "surprise" tricks (see section 6). |
| `ma2_bot_c7.c` | Counter table for diagnostics. |
| `ma2_bots_ui.c` | Briefing Room, menu policy, roster handling. |

Hooks into generated code are inserted by patch scripts, anchored on `loc_` labels ([patches.md](patches.md)).

## 3. How bots drive bodies

- **Commands.** Bots send command records (0x1C bytes) to the player's control behaviour, so the game's own vehicle code
  runs (`t01a.bots-command-record-layout`, `t01a.bots-behaviour-dispatch`, `t01a.bots-mech-input-actions`). Vehicles also
  run the campaign AI: it ticks only when the AI is awake and the body flag `comp+0x144` bit 0 is set
  (`t01a.bots-ai-awake-gate`, `t01b.externally-driven-body-144`). The AI has no general A* of its own
  (`t01a.bots-ai-no-general-astar`), so path planning is done by the bot grid planner and fed to MoveTo as waypoints.
- **Boarding.** A bot walks to an entry zone and taps the enter command once the zone offers the vehicle. Vehicles
  without an active zone, or of the enemy nation in a team mode, are not used (`t01a` page, "Behaviours").
- **Jumpjets and BattleArmor.** `StartJump` (`sub_18ABB5`) starts a hop and MoveTo flies it. BattleArmor walks 11-15 m/s;
  a jet clears the walker bit `comp+0x145` bit 0, and nothing restored it for AI-driven bodies, so the bot ran in place
  until BaLand/BaWalk restored it (`t28.ba-walker-bit-cleared-by-jet`). Clamping onto a 'Mech uses the clamp path
  (`t01a.bots-clamp-path`) and the bot answers the NeuroHack QTE on its own actor (`t01a.bots-qte-answer-own-actor`).
  Wall climbing and roof landing are covered by `t01a.bots-ba-wall-climb`, `t19.audit-f2-baroofland`.
- **VTOL.** The yaw-hold bit (`comp+0x144` bit 15) blocks AI yaw and must be cleared after take-off; altitude is set
  through `ai+0x20C`; flight mixes MoveTo with the awake drive (`t01a.bots-vtol-yaw-hold`, `t01a.bots-vtol-altitude-ai`,
  `t01a.bots-vtol-hybrid-flight`). VTOLs follow the 'Mech ground route with a 60 m look-ahead and learn no-fly discs
  (`t01a.bots-vtol-ground-route`, `t01a.bots-vtol-no-fly-learning`). A craft carrying a BattleArmor on its claw has a lower
  ceiling and must route around tall structures (`t18.ctfvtol-tall-areas`). A later correction found that a ferry crawling
  at 4-9 m/s was caused by a stale forward-speed target on the flight component, fixed by `vtol_stick_take`
  (`t14.vtol-bot-rider-crawl-1101`, pack `t28_vtol_ferry_speed_1110`).
- **Defence and weapons.** Bots raise their own defence and pick weapons by `wpn_value_vs` (energy is avoided against
  reflective armour, ballistic against reactive, missiles against AMS; `t01a.bots-defense-decision-table`). Alpha Strike
  works because the bot's copy of the player trigger record is refreshed each frame (`t01a.bots-alpha-bot-copy-fix`).

## 4. Navigation and NavCache

- Shipped navigation graphs exist for four multiplayer maps; every other map gets a generated grid over the terrain
  bounds (`t01b.nav-load-order`, `t01b.nav-cache-path`).
- The planner uses a walkable-surface grid: a cell is blocked where the step to a neighbour is over 1.6 m, roofs are
  jet-only islands, bridges and ramps are walkable, and one-way cliff drops are allowed over natural terrain only
  (`t01a` page, "Navigation"; `t01b.obstacle-probe-rules`).
- **NavCache.** The static part of the obstacle grid is saved per map and game mode in
  `%LOCALAPPDATA%\MechAssault2\NavCache` and loaded in the next match, so bots are ready in the first frame
  (`t28.navcache-default-on`). The key holds the map archive, game mode, grid extent, terrain and prop fingerprints, a cache
  version, grid constants and the build stamp, so a mismatch rebuilds. Off with `RECOMP_BOT_NAV_CACHE=0` or `[Bots] NavCache=0`;
  `RECOMP_BOT_NAV_CACHE_VERIFY=1` builds anyway and compares.
- Path slicing, lane pinning, unreachable-goal handling and the ledge-trap fixes are listed in `t14_push_surprise_1030`
  ("Switches").

## 5. Skills, teams and fairness

- **Teams.** The counts in `[Bots]` are literal (`RECOMP_BOTS_RED/BLUE/FFA/MAX`); no side can exceed six players including
  the human, and the player is placed on the short side (`t01a` page, "Team rules and skill"; `t01b.total-cap-env`).
  Grinder allows one bot (`t01a.bots-grinder-one-bot`).
- **Skill levels.** The Bot Skill row offers Green, Regular, Veteran, Elite, Dragoon (experimental, past Elite) and Mixed
  (`ma2_bots_ui.c`, `populate`). In code the first four are `Recruit` to `Elite` (0 to 3), Mixed is config value 4 and
  Dragoon is 5; Mixed is the default and gives each side a spread of levels, with the side facing the player raised one notch (`t01b.mixed-skill-spread`, `t01b.mixed-skill-bump`,
  `t01a.bots-team-mixed-skill-opponents`). The brain's table sets reaction time, aim error and QTE reaction by level
  (`t01a.bots-team-skill-table`); each value gets up to plus or minus 15 percent personal variance from the seed.
- **Fair damage.** Skill never changes damage, armour, health or weapon stats; it changes reaction, aim error, decisions
  and vehicle tuning (accuracy radius and fire delay; `t01b.skill-tune-mover`, `RECOMP_BOT_SKILL_TUNE=0` leaves them alone).
  Differences in damage per shot come from hit rate and weapon mix.
- **Mixed-mode balance.** The side-balance sweeps and objective-reach audits (`t20_balance_sweep_1107`,
  `t21_balance_round2_1107`, `t22_objreach_1108`, `t23_objreach2_1108`) measure whether both sides reach contested
  objectives at the same pace; the film review (`t24_film_review_1108`) judges bot play against human play.

## 6. Surprise behaviours

A rare, skill-scaled set of human-like tricks (revenge and grudges, mood streaks, rookie mistakes, baiting, pincer, power-up
timing and denial, stomp, hack cover, peel-off) all start through one rarity gate, `sp_roll` in `ma2_bot_surprise.c`:
a master switch, a hold-off after start, a per-bot cooldown, a random roll and a per-victim budget. Tricks change decisions,
aim gate, timing and target only (fair damage). Switches: `RECOMP_BOT_SURPRISE` / `[Bots] Surprises`, per trick
`RECOMP_BOT_SP_<NAME>` / `Sp<Name>`. Pack `t14_push_surprise_1030` lists which tricks are proven in game and which are
shipped but event-gated.

## 7. Recorder and diagnostics

- **Recorder.** Each bot match writes one `.botrec` file at 30 Hz holding bot and human state, game objects, inputs and
  behaviours. The folder is pruned so that the newest four recordings exist, and `RECOMP_BOT_REC_MAX_MB` (default 150,
  about 25 minutes) caps one file. Switch: `RECOMP_BOT_REC` / `[Bots] BotRecorder` (`t28.botrec-limits`). The file layout is
  documented in the header of `kit/source/game/src/bots/ma2_bot_rec.c`; the reader and analysis scripts (`tools/botrec.py`,
  audit scripts such as `objreach.py`) are not included yet (`t21.objreach-audit` describes what the audit classifies). Note that `comp+0x90` in a recording always reads 0
  because it is sampled after the input pass (`t18` page).
- **Logging.** Bot diagnostics (`[BOT]`, `[C7]`, `[BOT-NOBOARD]`, `[BOT-TRACE]`, `[BOT-ROOFS]`) print only with
  `RECOMP_BOT_DIAG=1` or `[Bots] BotLog=1`; counters and the recorder stay live either way (`t28.bot-diag-switch`).
- **Test hooks.** `RECOMP_BOT_DIAG_NAV`, `RECOMP_BOT_NAV_REGION`, `RECOMP_BOT_PATHTEST`, `RECOMP_BOT_VTOL_TRACE`,
  `RECOMP_BOT_STALL` (stall monitor sampling all threads), `RECOMP_DEFENSE_PROBE`. The table is in the `t01a` page, section "Env vars,
  settings and test hooks"; every `RECOMP_*` name also appears in the wiki's environment-variable list.

## 8. Where things are documented

| Question | Pack or fact |
|---|---|
| How a bot is created and torn down | `t01a.bots-player-creation-chain`, `t01a.bots-player-teardown-hook` |
| Game structures bots read (session, roster, players vector) | `t01b` globals: `t01b.session-singleton-0x4ebbfc`, `t01b.players-vector-0x50e280` |
| Why a behaviour was changed | `t19_bot_audit_1104` (one fact per finding), `t18_ctf_vtol_1104` |
| What shipped when | `t28_release_polish_1110`, `t28.navcache-default-on` |
| Open items | facts with status `open-question` in the bot packs (they are marked in `wiki/WIKI.md`) |
