# Gameplay systems: vehicles, physics timing, QTE, aiming, missions, checkpoints

Pack t05_gameplay. Facts file: .

## Overview

The original game runs most gameplay once per host frame, with its own clocks. On the PC port the frame rate is uncapped, so every rate-dependent piece of the original (vehicle attitude springs, contact normals, weapon and QTE timers) changes with frame rate. Most work in this pack is about three things: making mouse aim independent of frame rate and routing the mouse to the right object, making tank physics match the 30 Hz original, and fixing the on-screen QTE (NeuroHack / jacking) prompts so their art and captions are right.

Vehicle aiming is split by class. A Tank (MATank 0x46AE48) uses a hull-control route for movement and a turret route (0x109FF4) for yaw. A Default POV (0x4772A0) or MATurret (0x46B130) uses 0x1729DE for yaw and 0x172989 for pitch. The VTOL (MAFlyer, 0x477570) uses 0x1745B9. Mouse input is added in `recomp_manual.c` at the authored argument boundary, and the division by the simulation step (double at 0x4E2428) is what keeps degrees per mouse count fixed across frame rates.

Tank physics is the hardest part. The original runs the whole MATank update (0x109536) once per frame, and inside it the physical root (0x170C08) runs wheel contact, a normal refresh and attitude springs (0x1718ED). The springs and contact are tuned for 30 Hz. Experiments in this pack showed that stepping only the springs (build 8DF1) does not fix the shoreline pitch, and stepping the whole body (build 2959) makes aim choppy. The current candidate (`ma2_tank_fixedstep.c`, opt-in with RECOMP_TANK_FIXEDSTEP=1) calls the body once per frame and steps only the spring at 30 Hz with a phase accumulator. It is not accepted. The shoreline symptom is still open.

QTE (NeuroHack / jacking) prompts are 56 image widgets (class 88D55AF4, vtable 0x48A828), not text. The original draw (0x1FE68D) and visibility (0x21E7D8) are hooked, and a keyboard/mouse caption replaces the bitmap when it fits. Several bugs were fixed (authored ID at +8, widescreen size guard, trigger-legend ordering). Native success of the LT/RT legend is not yet verified: a tree_rect guard rejects it and the offending component is not identified.

Controllers, name entry, missions and checkpoints complete the pack. Controller inversion was wired to the vibration flag and is fixed. The name-entry keyboard (MAKeyboardDlg, 0x48F7D0) gets a text queue. Save writes (0x167B25) and the checkpoint state are documented, and a save-latency stall is still open.

## Key code

| Address or function | What it does | Source |
| --- | --- | --- |
| 0046AE48 / 00109536 | MATank vtable and update slot (+1C) | `kit/source/game/src/ma2_tank_fixedstep.c` |
| 00477850 / 001753DC / 001753E6 | Tank component action 9 and hull-control route |  |
| 0017537D / 0017BCC1 | Tank action9 handler; calls the RET8 no-op (mouse X consumed here before the fix) |  |
| 00109FF4 / 002ADC10 / 002B28D0 | Turret setter and angular vector setter (inline actor+488, rate at actor+538) |  |
| 004772A0 / 00172989 / 000F065C | Default POV pitch (action 5), tail call bypasses manual dispatch |  |
| 001729DE / 000E9B3B | Default POV and MATurret yaw |  |
| 001745B9 | VTOL action 9 (MAFlyer 0x46BC38) |  |
| 00109536 -> 000E9A5F -> 000DBBCA | Full actor update chain |  |
| 00170C08 | Physical root update (root vtable 0x4770B8 +10); integer ms to seconds via 0x493AC4 |  |
| 00170D91 / 0017687C / 000F70FC | Fast normal branch; provider chain; epsilon 0x45AF34 |  |
| 00170FD8 / 00171010 / 00171369 / 001713E5 | Wheel gravity, contact resolution, mean height, contact normal |  |
| 001718ED | Attitude spring (30 Hz discrete map); defaults 300/300, secondary 100/20, fast 100/100 |  |
| 0010A3DA | Accel/brake tilt producer; uses the native dt (0x4E2428) |  |
| 0010B7F5 / 0010B8F7 | Secondary root spring; limit 5 deg (0x4BACBC) |  |
| 000CC083 | Transition notification; clears parent+3E0 first (one-shot) |  |
| 0001DE70 / 0001DD80 | Clock producers: double dt 0x4E2428, integer ms 0x4E2424 (truncated) |  |
| 00015AF0 / 00015D8F / 00015D4C7 | Manager frame loop and actor dispatch gates |  |
| 0018EC9D / 0021E7D8 | QTE state query (0-4) and its visibility caller |  |
| 0018F27B / 0018ED25 / 0018F296 | QTE cue select and countdown; response forgiveness after 0x18F296 |  |
| 0048A828 / 001FE68D / 0021E3A9 | QTE widget vtable, draw (hook at 001FE6B2) and factory |  |
| 001FF94C / 000C1640 / 000C0FD0 | Caption text path, font wrapper, rasterizer |  |
| 001CCC10 | QTE sprite layout (scales x by 4:3/aspect) |  |
| 00167B25 / 0015CED6 | Save write (signs 284 profile bytes, WDF graph) and progress callback |  |
| 0021F989 / 00223DBC / 0048BF20 | Controller inversion accept, in-mission camera update, derived accept type |  |
| 0048F7D0 / 00249E11 / 0024A2E6 | MAKeyboardDlg (name entry): vtable, constructor, edit handler |  |
| 000C9084 | Control-behaviour factory: MATank to 0x1753BF; player bit13 to Default POV 0x172967 |  |

## Added code and hooks

- `recomp_manual.c` (`kit/source/game/src/recomp_manual.c`):
  - `recomp_lookup_manual` (line 396) returns the save-progress probe for 0x15CED6 (line 398).
  - `ma2_input_read` (line 545) with the RECOMP_TEST_HUMAN_AUTOPILOT hook (line 570).
  - `ma2_mouse_look_value` (line 806): divides displacement by the sim step (line 826).
  - `ma2_mouse_commit` (line 950): reads raw mouse with a fixed 0.01 and clears used flags.
  - `ma2_mouse_pov_pitch` (line 863) and `ma2_mouse_tank_yaw` (line 849).
  - `ma2_object_primary` (line 878) and `ma2_input_command` (line 889).
  - Aim diagnostics `[AIM-TRANSITION]` (lines 656-705).
  - `ma2_campaign_probe_enabled` (line 982).
- `ma2_tank_fixedstep.c` (line 16 env gate; line 20 live guard; lines 26-30 wrapper; lines 34-51 phase accumulator). `ma2_spring_rate.h` holds the 30 Hz spring clock (2048 slots).
- `ma2_cinematic.c`: 20 cinema cache hashes (lines 25-47), path hook (line 71), install at startup (lines 85-91).
- `ma2_qte_response.h` (lines 8-20): 1.25x response budget for keyboard and mouse.
- `ma2_trace` / `ma2_tank_trace.c`: opt-in tank trace (see env vars).
- Generated units changed by the candidates: `recomp_0038.c` (spring, normal refresh), `recomp_0039.c` (VTOL and turret yaw), `recomp_0041.c` (QTE timer), `recomp_0053.c` (QTE draw hook at 1FE6B2, epilogue 1FE81F), `recomp_0026.c` (109536 wrapper).
- Bots: `patch_turret_slide.py` (gun emplacement TURRET kind), `patch_pilot_nothink.py` (ai_hold_think).
- Bridge and prompts (`pc_gui_bridge.c`, `pc_prompts.cpp`, `pc_native_settings.cpp`): QTE captions, controller invert mapping, name entry queue, bare-Press rewrite.

## Env vars, settings and test hooks

| Name | Effect | Source |
| --- | --- | --- |
| RECOMP_TANK_FIXEDSTEP | CORRECTION 2026-11-10: the 30 Hz tank step is ON by default; 0 turns it off (`t02.tank-physics-default`) | `ma2_tank_fixedstep.c:17` |
| RECOMP_CINEMATIC_MODE | 0 disables the cinematic pillarbox and 30 FPS cap | `ma2_cinematic.c:87` |
| RECOMP_INPUT_DIAG | [MOUSE-LOOK], [MOUSE-CAMERA], [CAMERA-COMMAND] diagnostics | `recomp_manual.c:828` |
| RECOMP_TANK_AIM_DIAG | 1 writes [AIM-TRANSITION] and [TANK-AIM-ROUTE] rows |  |
| RECOMP_TANK_TRACE_SCENE / _UIDS / _FILE / _ARM | Opt-in tank trace, Mission_01b only, 2-3 UIDs, 10 s, new file under logs |  |
| RECOMP_QTE_TRACE | 1 logs up to 16 unique offender components for QTE_TREE_BOX |  |
| RECOMP_BOT_DIAG | [BOT-FIXEDGUN] once per gun, [MOUSE-DIAG] | `patch_turret_slide.py:50` |
| RECOMP_BOT_PILOT_NOTHINK | 0 restores the old pilot gait (A/B) | `patch_pilot_nothink.py:45` |
| RECOMP_SAVE_DIR + RECOMP_WATCHDOG_SECS | Campaign probe only when the save folder is under bringup/logs | `recomp_manual.c:982` |
| RECOMP_VBLANK | Default 1: a 60 Hz vblank runs even when uncapped |  |

Test harnesses (CPU, no game run): `prove.py` (109536 wrapper, 11,921 checks), `input_verify.py` (61 cases), `verify_yaw.py` and `verify_vtol.py` (VTOL), `verify_shared.py` (Tank/Turret/POV contract), `verify.py` (POV, 3,517 cases), `mission_agent_qte_draw_verify.py` (2,820 checks, 786 Unicorn records), `mission_agent_qte_design_verify.py` (135 glyph cases), `mission_agent_controller_verify.py` (modes before, after, input, native, runtime-after), `mission_agent_name_verify.py` (4,415 checks), `prompt_text_test.cpp` (29 checks), `binding_caption_fixture.cpp` (41 checks), `replay_intro.py`, `Navigator.cs` (read-only player and Scout sampler), `decode_trace_v2.py` and `read_trace.py` (trace decoders).

## Bugs and fixes (history)

1. Tank yaw consumed at a no-op callback (0x17537D to 0x17BCC1). Fixed by `ma2_mouse_tank_yaw` (manual B10F7094, gen0039 423ABED1). Human-verified: horizontal Tank aiming works (build 1ABC, 2026-10-11).
2. A 144 Hz mouse pulse is lost when a whole actor tick is skipped. The current wrapper calls the full body every frame, so it does not skip input. Input buffers were superseded.
3. Spring-only fix (build 8DF1) falsified: pitch was excessive again. Whole-body 30 Hz (2959) fixed pitch but made aim choppy. Superseded by the current split.
4. Integer millisecond rounding (6 ms at 144 Hz) was tested; contact steps pass exactly 1/30, so it is not the cause. Superseded.
5. A CPU fixture reported owner-1 results on unowned actors. The W4 peer replaced it with a valid primary pointer and an ownership assert (2026-10-11). The `live` guard (0x46AE48, 0x109536, inline 0x4930B8, 0x4934B8) keeps the wrapper on the exact MATank.
6. Mouse POV pitch bypassed manual dispatch (0x172989 tail call). Adapter `ma2_mouse_pov_pitch` added; CPU-verified, not accepted.
7. Controller "Invert right stick Y" wrote profile +75 (vibration) instead of +76 (camera pitch). Fixed in `pc_gui_bridge.c`; confirmed in play.
8. Active camera inversion: native accept skipped the in-mission camera update (0x223DBC). Fixed; 8 pre-fix failures, 0 after.
9. QTE LT/RT: legend rejected because the bridge read widget +6C (tab_id) instead of authored +8. Fixed (v28). 114 original link cases.
10. QTE widescreen size guard used width x 640, which measured 17.78 px at 2.4 aspect and rejected the 32 px sprite. Corrected to the render width (1152) with 72 original cases.
11. QTE keyboard/mouse response was too short; now 1.25x (`ma2_qte_response.h`). CPU: 602 cases.
12. QTE trigger legend order: LT and RT legends were swapped relative to the buttons; now Mouse 1 is on LT and Mouse 2 on RT (W5, 1,696 checks). Native visual pending.
13. Bare "Press A/B/X/Y" in prompts were not rewritten; the unbounded Y rule also changed "Press Any button". Fixed in `pc_prompts.cpp` (29 checks).
14. Turret gun emplacements (MATank with zero drive limits) were driven by bots across the map. Fixed by the TURRET kind (2026-10-14).
15. Pilot stop-start gait: an asleep AI thinks every 0.2 s and stops the body. Fixed with ai_hold_think (documented; patch present).
16. Pulse laser class read from primary+15C (a heat float) instead of +17C. Diagnostic fixed in v5.
17. Name entry had no text queue for host keyboard. Added WM_CHAR queue and keyboard suppression during entry (CPU 4,415 checks).

## Open questions

- Shoreline tanks (UIDs 132500, 132555, 132557; cinema 41226) are pitched uphill when uncapped. The cause is open. The authored Mission_01b set has only two cinematic tanks (133496, 133503), and the third visible tank is not mapped.
- Which MATank the human controls in Mission03 (the stolen tank). The live selection capture (RECOMP_TANK_AIM_DIAG) is the next step.
- Native QTE LT/RT legend: the tree_rect guard rejects it and the offending component is not identified (RECOMP_QTE_TRACE).
- Save latency: 20.5 s menu stall and 3.7 s gameplay stall have no cause. Kernel file-call timing is proposed.
- Pulse laser invisibility: alpha test versus combiner alpha is unknown.
- Whether the dispatch bits (actor+1C bit11/13) are restored after the shoreline cinema.
- `DropPack` is in the brief but not in this pack's files or the bots source; not covered.
- Some pack files were read only in part (for example,).

