# Push surprise, navigation and the push-merge (2026-10-30)

The bots push of 2026-10-30 added three groups of work on top of push-final (c44d849) and merged them into the branch `push-merge`:

- **Surprise behaviours** (push-s1, s2, s3): rare, skill-scaled human-like tricks that all start through one rarity gate, `sp_roll` in `ma2_bot_surprise.c` (switch, 20 s hold-off, per-bot cooldown, random roll, per-victim budget of 2 per 600 s, no repeat, 45 s spacing). Task A (s1): revenge, grudge, mood (hot/cold streaks), rookie mistakes A-F. Task B (s2, `ma2_bot_surprise_b.c`): bait, pincer, Elite power-up timing and denial. Task C (s3, `ma2_bot_surprise_c.c`): Level, Stomp, Hack cover, Peel-off. Each trick keeps its own state, steers through the existing tactical-point fields or a replacing order, and counts itself in `[C7] sp_*` pieces. Fair damage: tricks change decisions, aim gate, timing and target only.
- **Ground navigation and high ground** (push-x, x2, x3): NoPathHold and SnapWide remove the Nuclear Reactor C ledge trap, SwapUnstick bounds a stuck vehicle swap, the BA wall climb gets real roof candidates from a raster roof scan, and the R13 review fixes make the path planner NaN-safe and epoch-correct.
- **VTOL carrier fetch / ferry and threat altitude** (push-y): LiftClaimFix, LiftBoardFix, VtolThreatAlt, and the APU front-end trap fix (`apu_vp.c`, env only).

## Where things live

| Piece | File | Notes |
|---|---|---|
| Rarity gate, killer table, revenge, grudge, mood, rookie | `kit/source/game/src/bots/ma2_bot_surprise.c/.h` | included into `ma2_bot_brain.c` before `ma2_bot_brain_obj.c` |
| Bait, pincer, power-up timing and denial | `kit/source/game/src/bots/ma2_bot_surprise_b.c` | included before `brain_tick_body`; hooks `sp2_dead`, `sp2_tick`, `sp2_order` |
| Level, Stomp, Hack cover, Peel-off | `kit/source/game/src/bots/ma2_bot_surprise_c.c/.h` | hooks `spc_tick` and `spc_order`; new plumbing Ma2Senses.bld_ray/bld_get, Ma2Order.ff_skip/ff_rider |
| Roof scan | `kit/source/game/src/bots/ma2_bots_core.c` roofs_step / sn_roofs | feeds `tac_climb` in `ma2_bot_brain.c` |
| Wall climb on order | `kit/source/game/src/bots/ma2_bot_ctrl.c` climb_order | state reset on a new body (merge safety pass) |
| C7 diagnostics table | `kit/source/game/src/bots/ma2_bot_c7.c` | 256 pieces x 128 counters after the merge |

## Switches

Every approved behaviour ships ON (Design rule: 10-30). Master `RECOMP_BOT_SURPRISE` / `[Bots] Surprises`; per trick `RECOMP_BOT_SP_<NAME>` / `Sp<Name>`; navigation `NoPathHold`, `SnapWide`, `SwapUnstick`, `ReplanFix`, `NavFix`, `NavGuard`, `LanePin`, `UnreachableGoal`; lift `LiftClaimFix`, `LiftBoardFix`; `VtolThreatAlt`. Only `CarrierDanger` stays off. The shipped `pc_settings.ini` `[Bots] PathSlice` should be 1 (the code default is already ON).

## Status of the proofs

Proven in game: revenge and mood (forced), Level end to end once, bot-carrier VTOL fetch and ferry once, NRC ledge trap fixed (0 traps in 3 runs against 4 of 4 on the base), roof candidates on UC. Unproven (shipped ON, event-gated): Stomp, Hack cover action path, Peel-off, pincer arrival, power-up timing and denial, VtolThreatAlt effect, the BA claw climb to a roof, Grudge payback (8 %).

## Integration test + install (2026-10-31, exe C01EA259 = push-merge 65fcef1)
- 4 runs, 0 ICALL/FAULT/CRASH/R6025, 0 BOT-TRAPPED (NRC CTF PathSlice=1 included), no [C7] table full. UC TD 5v6 Mixed ON vs OFF: team-0 kill share 55 vs 57 %, frame median 9.4 vs 9.2 ms, p99 18.6 vs 17.3 ms, first-10 s HITCH counts 34 vs 37 (same causes). Surprise code cost [C7] sp_all tick_ms=0.013, tick_max=0.48.
- RECOMP_BOT_DIAG=1 is what prints [BOT] t= / [BOT-SUM] (stalls, kills); RECOMP_PERF=1 alone gives only [BOT-PERF] and [PERF-PACING]. A UC TD 5v6 match ends on its own at ~t=420 s.
- Roof scan (x2) is sliced (400 rays/step): the [BOT-ROOFS] line appears at game t~25 s (UC, 8 roofs) / ~49 s (NRC, 46 roofs); no load stall.
- NRC CTF pathslice jobs 2704 per 300 s (0.82 plans/bot/s) vs 4243 on c44d849; vtolpads always skips the enemy pad (picks = skip_enemy), vtpad heal runs <= 2.9 s.
- Pre-existing: bot ticks of 70-200 ms carrying ~6000 rays occur 9-20 times per 300 s, identical with surprises ON or OFF.

## Easy wins B11: recompiled-code hazards and the frame clock (branch ew-b11, 2026-10-30)
- Frame clock: 0x4E2428 = scaled frame dt (double s), 0x4E2440 = game clock (double s), 0x4E2430 = ftol(game clock*1000), 0x4E2424 = per-frame integer-ms step = ftol(dt*MEMD(0x493B58)). Producers sub_0001DD80/DDB4/DE70 (recomp_0001.c:11376/11464/11642); sub_0001DD00 (clock init) zeroes the step. sub_00160DF4/00160E76 (recomp_0036.c) snapshot/restore all of them.
- Truncating the step loses game time at every frame rate: measured 85-86 % of real time at ~250 fps (menus/attract) and 96 % at ~90 fps (UC TD 5v6). ma2_ms_step_carry (recomp_manual.c) carries the fraction: 100.0 % in both runs over 300 s ([MS-CARRY] lines, RECOMP_MS_CARRY_STATS=1). Switch: [Display] FrameClockCarry=1 (default), env RECOMP_MS_CARRY=0/1. No fps cap.
- Loading screens run above 1000 fps: in the first 10 s of a match 5,392 truncated steps were 0 ms (4,914 with the carry, which still gives 0/1 steps whose mean is exact).
- The prebuilt recomp_0001.c.obj is the copy (it has the rumble-pause code at ~19081); `generated recomp_0001.c (made at install time, not in the repository)` lacks it. 0065/0075/0086 are identical in both trees.
- CRT cos/sin/acos/asin (recomp_0077.c sub_0030C659/0030C709/0030A43D/0030C4D5) now take the NaN/Inf path from `eax & 0x7FF00000` (the ZF of _fload_withFB sub_00310AB5/00310AF8 was lost across the call). RECOMP_CRT_SPECIAL_TEST=1 self-test: +-Inf gives FFF8000000000000, NaN passes through, cos(1)/sin(1)/asin(1) are exact, stack and FPU balanced.
- The newly reachable special path: Inf loads the indefinite NaN (0x4A03A8) with eax=1 (_DOMAIN); NaN goes through sub_00310A9C (tests the quiet bit, eax & 0x80000), then, unless 0x4CDDF4 is set, sub_00310A33 (_startOneArgErrorHandling, op 0x12 for cos). Before B11.1 these handlers were never called.
- CPU probes: sub_00334C40 (MMX, render init) and sub_002E0F4C (HasSSE via sub_002E0FBA) are native in recomp_manual.c (lifts kept as *_unused_lift); results unchanged (MMX 0, HasSSE 0), no ebx/esp corruption. sub_0026D650 and sub_002E4180 pin edx/ebx/ecx to 0 before the no-op cpuid. sub_0026D650 runs 7 times per boot (movie path); the stale edx never had the MMX/SSE bits ([CPUID-PIN] never printed), so the pin changes nothing observed.
- Audio spins bounded: NEW_VOICE waits (recomp_0096.c 0x3AEA7F, 0x3AF1D4) and the XACT packet 0x103 wait (recomp_0098.c 0x3E51DB) give up after 250 ms with [VOICE-NEW-WAIT]/[XACT-WAIT] (RECOMP_AUDIO_WAIT_MS, 0 = unbounded). 0 such lines in a 5-minute 5v6 match.

- ma2_bot_nav.c: every grid epoch bump (tg_epoch_bump) logs the changed cell rectangle (64-entry ring); ma2_trav_changed_since(epoch, world rect) tells a bot or a queued A* whether a commit touched cells near its route. ctrl_route_stale (ctrl.c) replaces the blanket "epoch" replan; pq_step restarts a search only if the change is within 50 m of its from/to box.
- A destroyed building's reprobe (r=22 m, ~380 cells x 16 rays, ~6000 rays, 70-180 ms in one step) is now sliced (ma2_trav_reprobe_begin/step, 3 ms per step, RECOMP_BOT_WRECK_MS, switch WreckSlice) and committed once; UC 300 s: worst bot tick 18.4 ms, no tick above ~850 rays, 0 HITCH lines after 60 s.
- A grid commit (2 chamfers + 2 component floods, whole grid) costs 9 ms on UC and up to 24 ms on NRC; a stall used to do up to 3 (reprobe, wall_sweep, turrets_reapply), now one (StallBatch). NRC stall ticks of 20-54 ms remain (830-ray reprobe + commit).
- g_flagged[24] (body_drive'd vehicle actors) is cleared in runtime_free_level, and ai_drive_sweep frees slots whose actor is unmapped or not a MAMover ([C7] safety flag_stale = 14-20 per match).
- Safety: NaN axis/point guards in ctrl_send_frame, body_moveto, body_steer and the VTOL altitude write; nearest_in clamps its start cell (a 1e7 m query no longer runs millions of rings); ma2_pathq_reset at level free.

## Two riders and close-range hacks (2026-11-01)
- **VtolTwoRiders** (`RECOMP_BOT_VTOL_TWO_RIDERS` / `[Bots] VtolTwoRiders`, on): the VTOL holds (VtolMuster) while fewer than two ride, the player first, also on its own flag run at pickup/inbound 400 m+ from their flag; with one aboard `lift_hold` hovers for the second (bot 10.5 m, player 12 m ahead) or fetches the run's BattleArmor within 150 m; the second rider becomes `g_frun[n].ba2`, drops at their flag, goes for it, and whichever takes the flag is flown home. Proof: [BOT-TWORIDERS] carries two (player + bot) in tests wa2r3 and wa2r4, both captured.
- BattleArmor side: no muster boarding for a hop under 150 m (claw on/off loop every 2 s at our stand after a capture).
- **BaHackClose** (`RECOMP_BOT_BA_HACK_CLOSE` / `[Bots] BaHackClose`, on): an enemy 'Mech within 25 m is rolled for once per encounter, Recruit 45 / Regular 60 / Veteran 75 / Elite 85 %, +15 against the player (cap 95); a win = hack utility 1.4. [BOT-HACKROLL], [C7] bahackclose.
- Test hooks: `RECOMP_TEST_BOT_BA`, `RECOMP_TEST_BOT_VTOL` (bot index), `RECOMP_TEST_HFERRY_REACH=1`.
- Pre-existing, seen in the tests: VtolWaitPlayer restarts a player run every tick while the VTOL is below the give-up health (started_player 13 / ends_damaged 11 in wa2r4): its start test has no health check.

## VTOL air routes, map edges and BattleArmor flee (91392af, 2026-11-01)
- **VtolLaneRoute** (`RECOMP_BOT_VTOL_LANE_ROUTE` / `[Bots] VtolLaneRoute`, on): VTOL legs of 150 m+ are an A* on a 20 m air grid (walk-grid surface, highest per cell): length x lane factor x (1 + height over the start/goal height / K) + 2 per metre climbed, pulled tight where a cut stands no more than `slack` over the cells it replaces. Deliverance ferries now follow the valley (median 28-36 m, was 104-107 m over the middle hill); Bogged Down's plateau ferry stays near-straight (848 m vs 834 m). `[BOT-VROUTE]`, `[C7] vtlane`.
- **VtolRouteVary** (`RECOMP_BOT_VTOL_ROUTE_VARY`, on): per flight K 45-90, favoured lane, smooth +-35% cost field (160 m lattice), slack, cruise band; seeded per match/bot/flight. `[C7] vtvary`.
- **VtolEdgeKeep** (`RECOMP_BOT_VTOL_EDGE_KEEP`, on): the MATerrainInfo type map (key 1F1646EA) read at run time like sub_F7AC7 -> sub_189E62 -> sub_1899C6 -> sub_2674A0 (TI [0x50C8E0], layer [TI+0x84], picture [layer+0xC], base +0x4C, bpp +0x24, pitch +0x50, size +0x58/+0x5C). Routes never cross out-of-bounds (1) cells and keep 25 m off; band (2) cells cost x1.5 (Bogged Down marks inner cliff faces 2); goals on 1 cells are pulled ; fight hide/slide spots keep off. `RECOMP_BOT_VTOL_EDGE_DUMP=1` prints the map. `[BOT-VEDGE]` per-flight closest approach, `[C7] vtedge`.
- The invisible wall is at the **2/1 edge** and holds at any height (VTOL stopped at x 590-600 at 100-111 m on Deliverance's hill). Deliverance's middle hill is all type 1.
- **BaHackFlee** (`RECOMP_BOT_BA_HACK_FLEE`, on): an enemy BattleArmor inside 18 m of a bot 'Mech is a 26 m hazard in ctrl_bomb_escape with the death blast's reaction by skill; not once clamped on. `[C7] bahackflee`.
- Test scenarios (not tracked): `IllWind_Test.ma2map` (editor copies of the stock maps, CTF ticked).

