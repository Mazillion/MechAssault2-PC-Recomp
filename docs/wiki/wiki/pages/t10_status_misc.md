# Bring-up status and miscellaneous engine findings

This article is the status record of the MechAssault 2 PC port. The facts behind it are in `facts/t10_status_misc.json` .

Source short names used below (full paths are in the fact records):
- `analysis.md`, `followup-qualifications.md`: Intel laptop v21 analysis,
- `S12`: `S3CAP`: `S3T`:
- `S3C`: `IPR`: `W3H`:
- `PKG26`, `V26B`, `V26P`, `CFGV26`: the v26 package, build and integration notes. `V28`: `PFP`:

The v22 configuration review has no date.

## Overview

The PC port of MechAssault 2 is a native executable built from the recompiled Xbox code. As of 2026-10-09 it boots, plays the intro, accepts keyboard, mouse and controller input, and reaches campaign gameplay on Direct3D 11. Bring-up work since then has covered four areas. Rendering was corrected at the level of Xbox NV2A semantics (indexed draws, texcoord clamps, compressed volumes, feedback copies) and tuned for 4K. The USB and XInput paths were fixed for controller reports, pacing and rumble policy. Save and campaign paths were made durable and observable. Finally, the scenario editor gained a loader for custom [Assets] and a test autostart.

The status file lists what is verified in a run and what is not. Most gameplay evidence is bounded: sampled scenes, staged stages, and interaction checks. Campaign completion, save and reload across a full mission, physical controller vibration, and AMD or Intel GPU testing were still open at the end of the record. The later v26, v27 and v28 candidates (2026-10-10) were CPU-checked and packaged; their gameplay was unobserved at the time of the notes.

## Key code

| Address or function | What it does | Source |
| --- | --- | --- |
| sub_000848E0 (recomp_0014.c) | NodeSwitchLod selector; draw-distance scale applied to its squared-distance input. |  |
| sub_0040F510 (recomp_0101.c) | D3D SDK resource-fence loop; the jb at 0040F640 now calls a bounded wait (nv2a_gpu_wait_word). | spin-orbit cpu-summary |
| sub_0040FA70 (recomp_0101.c) | Pulse weapon path; emits method 0x043C (point size) capped at 0x1FF. Point draws unsupported. | S12:36 |
| sub_000B06F0 (recomp_0018.c) | Display-paced movie update; branch at loc_000B0729 now uses the Bink timer. | patch_movie_pacing.py |
| 0012117F inside 00121108 (recomp_0028.c) | Camera-distance aspect query kept at authored aspect (owner MACameraThird slot 52). |  |
| sub_00013550 (recomp_0000.c) | Level mount block; scenario [Assets] archive mounted at loc_000138A0. | ASSETS:10 |
| sub_000171E0 (recomp_0000.c) | Level unload; scenario archive unmounted at loc_000178C9. | ASSETS:13 |
| sub_00025B70 / 00025C90 / 00025BF0 / 00025D10 (recomp_0002.c) | mount_archive, unmount by name, already-mounted check, lookup reset. | ASSETS:20-23 |
| sub_0002EA10 (recomp_0004.c), sub_00026890 (recomp_0003.c) | Archive object creation (mgIO\mgStream); fs+0x18C list push_back. | ASSETS:20, 22 |
| [0x524A60] | Level descriptor: +4 world path, +0x10 resource string, +0x68 no-cache flag. | ASSETS:24-25 |
| sub_000B9400, 0015EFF2, 00167B25, 00167DB6, 00167ED9 | Save writer, checkpoint, profile, savequit paths (gen0019/0035/0037). | S3C:15-41 |
| sub_002BD76E (recomp_0071.c) | Save cache transfer helper called before the profile call. | S3C:41 |
| 0015CED6 and 0016890C | Callback wrapper (16 bytes) and transfer callback (20 bytes) in the save path. | S3C:66 |
| sub_001661C1 (recomp_0037.c) | Game-type number mapping used by editor autostart. | AUTO:21 |
| sub_001CAD40 (recomp_0048.c), sub_00208BF9 (recomp_0054.c) | Autostart menu focus and barrel-value selection. | AUTO:14 |
| usb_gamepad_report:627, xbox_InputGetState:332 | One synchronous host controller read per guest interrupt input report. | analysis.md:90 |
| usb_gamepad_output:30, xbox_InputSetState:421 | Rumble output path to XInputSetState; runs on the OHCI worker. | analysis.md:94 |
| ImGui_ImplWin32 UpdateGamepads (vendored, lines 340-357) | Polls connected slot0 every rendered frame from the FPS HUD. | analysis.md:93, followup-qualifications.md:3 |
| nv2a_gpu_d3d11.c:2365-2378 | Present(VSync, tearing) followed by the explicit frame limiter. | analysis.md:24 |
| nv2a_pb_exec.c:90, 2673-2680 | PERF FPS (FLIP_STALL count) and CPU wall time around raster_batch. | S12:84, analysis.md:13 |
| xbox_memory_layout.c:877 | Single NV2A acknowledgement and command-consumption thread. | analysis.md:20 |
| kernel_hal.c:445, kernel_bridge.c:2294 | Guest tick source and 60 Hz vblank clock (neither is a frame cap). | analysis.md:22 |
| pc_gui_bridge.c (+0x75, +0x76) | Vibration and inversion preference offsets (swap fixed). | followup-qualifications.md:7 |
| xinput_feedback.h / xinput_device.c | Policy callback and allowed-port provider for output (pause and menus). | PFP:3-22 |
| MechAssault 2.exe --scenario (editor autostart) | Starts a custom scenario directly, with bots and skill options. | AUTO:7 |
| ma2models.h (namespace ma2m) | Template, model and texture decoder library for the editor. | MODELS:3 |

## Added code and hooks

- `bringup/patch_draw_distance.py`: reapplies the NodeSwitchLod hook in sub_000848E0 (idempotent).
- `bringup/patch_pc_frontend.py`: applies the ultrawide CameraDistance aspect fix.
- `bringup/patch_zero_flag_joins.py`: applies verified zero-flag branch repairs; changes are logged to. The fix itself lives in `tools/recomp/lifter.py`.
- `bringup/patch_x87_memory.py`: reapplies the ten-byte x87 memory operand fix after regeneration.
- `bringup/patch_movie_pacing.py`: PC Bink timer for display-paced movies in recomp_0018.c; bounded to 32 waits.
- `bringup/patch_gpu_fence_wait.py`: rewrites the SDK fence jb in recomp_0101.c to call nv2a_gpu_wait_word.
- `bringup/optimize_apu_io.py`: direct calls for lifted APU register operations (reapply after regeneration).
- `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`: the game source tree definition on the ImGui Win32 backend object, removing the FPS overlay's gamepad sampling.
- Scenario loader: `ma2_scn_level_load` and `ma2_scn_level_unload` hooks, writing `T:\scenarios\Scn<ID>_assets_<hash>.mgf` for [Assets] objects.
- Save-latency probes (opt-): spans in sub_000B9400, 0015EFF2, 00167B25, 00167DB6 and 00167ED9.
- Editor autostart in `ma2_scenario.c` and `ma2_bots_ui.c`, with `autotest.py`, `autostart/matrix.sh` and `autostart/mkvariant.py` as tests.
- Capture kit: Playthrough Capture v2 (v26), v3 (v27 QTE) and v4 (v28). v4 reads five native ranges every 500 ms; its state RVA is 03C11210.
- Tests: `tests/idex_channel` (IDE channel export) and `tests/wma_decoder/fixtures/sine.wma` (synthetic WMA fixture). Native regression suites are in `bringup/regression`.

## Env vars, settings and test hooks

| Name | Kind | Effect | Source |
| --- | --- | --- | --- |
| Display/TextureQuality (0/1/2) | setting | Original, 2x or 4x enhanced textures; applies live, saved for next launch. |  |
| Display/DrawDistance | setting | 0-100 maps to 0.5x-2x of authored distance; 50 is original. |  |
| RECOMP_HIGH_RES_PACING | env | Waitable-timer frame limiter; 0 selects the legacy Sleep path. |  |
| RECOMP_APU_DSP | env | 0 selects the older diagnostic audio fallback. |  |
| RECOMP_GPU=d3d11 | env | Supported renderer override. |  |
| RECOMP_VSH_VERIFY=1 | env | Vertex-program verification; stalls for readback. |  |
| RECOMP_CONSOLE=1 | env | Enables the development console. |  |
| RECOMP_AUDIO_CAPTURE, RECOMP_AUDIO_SILENT, RECOMP_AUDIO_STATS | env | Capture mixed PCM to WAV; silence device submission; log mix and queue health. | 1081 |
| RECOMP_FB_HIDDEN=1 | env | Keeps the diagnostic window hidden. |  |
| RECOMP_RUMBLE=0 | env | Set by owned probe launches to keep diagnostics quiet. |  |
| RECOMP_EXIT_ON_CLOSE=1 | env | Window close exits 0 in fb_present.c. | S12:16 |
| RECOMP_PERF=1 | env | PERF and PERF-PACING log lines. | S3T:10 |
| RECOMP_RUMBLE_TRACE, RECOMP_UPLOAD_TRACE, RECOMP_EFFECT_TRACE | env | Opt-in traces; capped at 256, 32 and 64 records; off by default. | S3T:9-61 |
| RECOMP_SAVE_LATENCY=1 | env | Save-stall probes; enabled only in the owned capture child. | S3C:84 |
| RECOMP_GPU_TIMING_FLAG | env | One-frame GPU timestamp capture (perturbs the window; not normal pacing). | analysis.md:72 |
| RECOMP_QTE_TRACE | env |  | S3T:15 |
| RECOMP_SCENARIO_AUTOSTART=0 / _STALL | env | Turns editor autostart off; stalls one menu to test give-up. | AUTO:23-24 |
| `--scenario`, `--mode`, `--bots`, `--skill` | command line | Editor autostart arguments. | AUTO:7 |
| `probe_texture_quality.py`, `probe_movie_skip.py`, `run_probe.py` | tools | Live menu and quality probes with private saves; owned, bounded diagnostic processes. | 1077 |
| `bringup/compare_process_audio.py` | tool | Process audio loopback comparison against submitted PCM. |  |
| `bringup/package_pc.py` | tool | Builds PC Release from the tested executable, libraries and assets. |  |

## Bugs and fixes (history)

Fixed and verified in game (2026-10-09 record):
- Mission 9a black sky wedge: incomplete indexed draws. Fixed by streaming indexed draws past entry 4,096 and by handling NV097 ARRAY_ELEMENT32. Fixed.
- Diagonal ground patches: texcoord was not clamped before the register combiner. Fixed.
- Battle-armor joints, road strips and shadow streaks: indexed-draw limit and the dropped last triangle. Fixed.
- Compressed volume fire scrambled into orange squares: volume slice layout. Fixed.
- Road collision: box-ray selector zero-flag merge (CDQ changed EDX). Fixed; the repair script applies the six verified branch changes to existing code.
- Inverse-trig orientation errors: x87 ten-byte memory operands. Fixed in the lifter and generated code.
- Distant lighting and billboard popping at extended draw distance: LOD selector, terrain-lightmap coverage and camera far planes. Fixed under the draw-distance setting.
- Ultrawide camera pushed into foliage: CameraDistance aspect query. Fixed.
- Controller-feedback stop: OHCI acknowledgement and dispatch serialization; USB clock 255.9 ms per second corrected to 999.8. Fixed.
- Pause and menu rumble latched after entering pause: policy callback added. Fixed in source (33/33 CPU checks); not yet verified on hardware.
- Wrong vibration toggle: +0x75 and +0x76 preference offsets swapped in pc_gui_bridge.c. Fixed.
- Early Alt+Enter breaking Present: DXGI default window handling. Fixed.
- Silent or stalled movies and incorrect intro skip: movie pacing and a distinct movie input context. Fixed.
- Save false success on rejected file operations and create dispositions: storage error propagation and FILE_CREATED handling. Fixed.
- Partial end-of-file reads in asset streams: buffered refill respects file length. Fixed.
- Scenario [Assets] missing objects, black T: mount, and the retail-map fallback: loader hooks and assets writes. Fixed; verified on Sky_Test and Small.
- Capture state JSON write latched on transient File.Replace errors (32, 33, 1175): retry plus DELETE-sharing readers. Fixed in the capture kit.

Open or superseded:
- Pulse weapon invisible in the 2026-10-10 report: point-size path is unsupported, but telemetry found no point draws. Installed build of 2026-10-11 shows pulse effects in one scene. Open.
- Open.
- Explosion frame drops: sustained slow window and one 3.7 s hitch. Causes unassigned. Open.
- Earlier internal render scale (1-4x, default 2x) and RenderScale saves: superseded by resolution-driven scene pixels.

## Open questions

- Full campaign objectives, new-campaign save and reload across a mission, and simulation speed through gameplay (1049-1064).
- A rare USB completion crash is only partly addressed (-109, 934-937).
- Physical AMD and Intel GPU validation. The Intel laptop slowdown (15-30 FPS at 720p) has no measured limiter. The planned comparisons are a fixed 720p view at 1280x720 and 1920x1080, then ShowFPS and controller on or off (analysis.md:54-70).
- FPS overlay gamepad polling: the the game source tree definition is in place, but its integrated cost is not measured (followup-qualifications.md:5).
- Rumble: the post-landing vibration (about one second) is unexplained; the pause fix does not establish its cause (PFP:55). Physical vibration is not measured in any run.
- Pulse weapon draw identity and the unexpected close origin: a bounded trace and EXIT markers are proposed but not implemented (S3CAP:90-92, S12:26-34).
- QTE acceptance: the native LT or RT offender is unknown, and the QTE caption guard is not a full resolution (W3H:21-23).
- Audio: the audible 128-tail plus 128-active CPU budget overrun is unresolved (W3H:33; IPR:103).
- Scenario: a single custom mission still plays the 70-second campaign intro first (AUTO:40).
- Object dictionary: 20 of 919 placeable-class keys are unknown; boardability needs a runtime check (DICT:13-14, 26).


