# PC port: menus, input, settings, saves, packaging

## Overview

The PC port wraps the recompiled Xbox game in a native Windows front end. `main.c` starts the process, reads `pc_settings.ini` and turns each key into a `RECOMP_*` environment variable. `platform/pc_frontend.c` holds the shared, atomic state (settings, bindings, input context, mouse capture, display sizes, rumble policy). `pc_native_settings.cpp` draws and runs the PC-only settings pages (Display, Audio, Controls, Keyboard and mouse bindings, Controller) on top of the original Xbox menu objects. `pc_prompts.cpp` rewrites guest prompt text so it names the keys that are actually bound.

Guest menus call into the PC layer at a few hook sites. `patch_pc_frontend.py` re-applies those hooks after the generated code is regenerated, and each hook is anchored on a unique `loc_XXXXXXXX` label. The original menus are kept. The PC pages take over only when the matching menu vtable is active (0048B360 Video, 0048BE60 Volume, 0048BF20 Controls). Unavailable menu items (Xbox Live, System Link, Conquest, Game Demos, Add Guest) are shown disabled by the policy in `ma2_bots_ui.c`, which is not in this pack.

Settings flow: a key is changed on a page, the value is stored in `pc_frontend.c` at once (and applies immediately where the code allows), and the INI is written on Back (no window change) or Apply (window change). Env vars override the INI for a launch. Saves and profiles live per Windows user under `%LOCALAPPDATA%\MechAssault2`, and the installer ships none. Packaging is a single installer exe that unpacks a kit and runs the build; the release is PC Release only.

## Key code

| Address or function | What it does | Source |
| --- | --- | --- |
| `ma2_pc_submenu_update` (page type from vtable) | Runs settings pages 0-4. Maps 0048B360 to Display, 0048BE60 to Audio, 0048BF20 to Controls. On result 2 it saves (`save_settings(false)`), releases capture and returns 2 to the guest. | `pc_native_settings.cpp:514`, `:569`, `:572` |
| `activate` | Changes one row. Display rows 0-12, Audio rows 0-4, Controls rows 0-8, Controller rows 0-3. Returns 2 for Back rows. | `pc_native_settings.cpp:467` onward |
| `page_key` | Escape or Backspace closes (from bindings, goes back to Controls). Up and down skip invalid rows. | `pc_native_settings.cpp:504` |
| `row_text` | Builds every row label, including the live values. | `pc_native_settings.cpp:278` |
| `save_settings(bool apply_window)` | Writes all `[Display]`, `[Audio]`, `[Input]` and `[Bindings]` keys. Deletes `RenderScale`. Posts the window message only when `apply_window` is true. | `pc_native_settings.cpp:227` |
| `check_optional_content` | Checks the 2x and 4x texture packs and the 2x movie folder, and resets unavailable choices to Original. | `pc_native_settings.cpp:259` |
| `ma2_pc_settings_initialize` | Sets the env defaults, clamps values, loads bindings, builds the resolution list and registers the render and window callbacks. | `pc_native_settings.cpp:687` |
| `ma2_pc_menu_pulse(object, vtable)` | Called from each menu's update. Runs barrel arrows, menu policy, input context, footer glow, and the click path. Hooked at `loc_001F34BE`, `loc_001FC712`, `loc_001FD243`. | `pc_native_settings.cpp:363`; `patch_pc_frontend.py:28`, `:91`, `:97` |
| `ma2_pc_modal_pulse` | Re-runs the menu pulse for an active modal. Hooked at `loc_001FBE6A`. | `pc_native_settings.cpp:450`; `patch_pc_frontend.py:101` |
| `ma2_pc_form_loading` | Detects the keyboard name-entry form (vtable 0048F7D0) and turns on text capture. | `pc_native_settings.cpp:346` |
| `ma2_pc_input_context_update` | Chooses MOVIE, MENU, TITLE or GAMEPLAY from the current menu state. | `pc_native_settings.cpp:336` |
| `window_message` | Handles F8 and F11, Escape to capture release, the Esc exit prompt on the main menu and HUB, and queues mouse and key events for pages. | `pc_native_settings.cpp:579`, `:609`, `:617`, `:621` |
| `ma2_pc_prompt_text` | Rewrites a guest string so its button words name the bound keys. Cached per input revision and keyboard layout. Hooked at `loc_000C0FD0`, `loc_001FFA04`, `loc_00200F56`. | `pc_prompts.cpp:130`; `patch_pc_frontend.py:68`, `:134`, `:135` |
| `ma2_pc_localized` | Routes text ids: scenario (0x00F0xxxx), bots (0x7002xxxx), settings page (0x7000xxxx), footer legends (0x7001xxxx). Hooked at `loc_002B4979`. | `pc_prompts.cpp:210`; `patch_pc_frontend.py:131` |
| `ma2_pc_qte_caption(mask)` | Gives the QTE caption for the bound key (mask 4 A, 1 X, 2 Y, 0x40 B, 0x10 and 0x20 the triggers). | `pc_prompts.cpp:263` |
| `pc_input_note_gamepad` / `pc_input_note_keyboard` | Switch the active device. A device change bumps the revision and runs the rumble output policy. | `platform/pc_frontend.c:184`, `:187`, `:202` |
| `pc_display_aspect` | Returns the 3D aspect (widened when PC_ASPECT is on, unless a cinematic is active). | `platform/pc_frontend.c:98` |
| `pc_display_render_extent` | Maps the 640x480 guest layout onto the selected render pixels. | `platform/pc_frontend.c:108` |
| `pc_cinematic_pace` | Holds cinematics to 30 Hz using a waitable timer and a short spin. | `platform/pc_frontend.c:135` |
| `main.c WinMain` | Startup order: logging, power throttling opt-out, asset overlay, cache namespace, `ma2.xbe` load, kernel, storage, bridge, watchdog, entry point. | `main.c:451`, `:483`, `:485`, `:619`, `:675` |
| `configure_pc` | Reads INI keys, sets RECOMP_* env vars (only if unset), and sets the process defaults. | `main.c:148`, `:164` |
| `sub_0042532E` (USB request builder) | Builds the USB SET_REPORT packet (rumble). The crash candidate is here. |  |
| `sub_0001FE60` (rumble manager) | Runs rumble expiry. The expiry candidate fixes its 1FEC2-1FEF2 block. |  |
| `sub_0015CED6` (save progress callback) | Save progress callback. Calls BBAF0 and sometimes BCAD0. Runs at 100 ms. |  |
| `sub_0015EFF2` / `sub_00167B25` / `sub_000B53E0` / `sub_000B9400` | Checkpoint chain: checkpoint to cache, profile envelope, the writer's single caller, and the common writer. | `:73` |
| `ma2_mouse_look` | Mouse look hook for the camera (guest sites 0038, 0039). | `patch_pc_frontend.py:148` |

## Added code and hooks

Hook sites are in `bringup/patch_pc_frontend.py`. The file reapplies each change after regeneration and raises `hook changed; inspect` if an anchor is missing. Groups:

- Menus: `recomp_0051` (main menu pulse, movie skip at `loc_001F40D3`), `recomp_0052` (pause menu `loc_001FC712`, selection `loc_001FCEB6`, result `loc_001FD26E`, dispatch `loc_001FD243`, form load `loc_001FD869`, modal `loc_001FBE6A`), `recomp_0057` (settings submenus: video `loc_002206BD`, controls `loc_0021F8DF`, volume `loc_002202E8`), `recomp_0058` (destroy `loc_00223D8E`, `loc_00223D2D`).
- Text: `recomp_0020` (prompt text `loc_000C0FD0`, font width counter `loc_000C1640` and `loc_000C1755`), `recomp_0053` (`loc_001FFA04`, `loc_00200F56`), `recomp_0070` (localized text `loc_002B4979`).
- Input: `recomp_0039` and `recomp_0038` (mouse look), `recomp_0027` (input context `loc_0011ADCE`), `recomp_0041` (input mapping), `recomp_0003` (input converted, every occurrence), `recomp_0001` (input committed).
- Display: `recomp_0005` (aspect at `loc_00038309`, widescreen render width at `loc_000382C0`), `recomp_0007` (aspect scale), `recomp_0048` (loading form rects, `sub_001CCC10`).
- Audio: `recomp_0094`, `recomp_0095`, `recomp_0096` (trace points and reference counts).
- Movies: `recomp_0094` (Bink decode, release, copy).

Platform code: `platform/pc_frontend.c` and `.h` (settings, bindings, input context, capture, display, cinematic pacing, rumble policy). `pc_storage.h` declares the copy-missing helper. `pc_settings.h` declares the PC settings helpers used by the hooks. The `xbox_input` layer (`kit/source/game/src/input/README.md`) maps the Xbox XInput API to Windows XInput.

## Env vars, settings and test hooks

### Settings file

`pc_settings.ini` lives beside the exe (packaged defaults) and in `%LOCALAPPDATA%\MechAssault2` (per user, written by the game). Precedence: env var, then INI, then built-in default. Out-of-range values fall back to the default (`main.c:135`).

| Section | Keys | What each changes |
| --- | --- | --- |
| `[Display]` | WindowWidth, WindowHeight (default 1280x960) | Client size and scene render pixels |
| `[Display]` | Fullscreen | Borderless fullscreen |
| `[Display]` | VSync, FrameLimit (0-1000) | Present policy and frame cap |
| `[Display]` | PreserveAspect | 1 = Automatic (widened 3D), 0 = Original 4:3 |
| `[Display]` | SmoothScaling | Scale filter |
| `[Display]` | ShowFPS | ImGui frame-rate overlay |
| `[Display]` | DrawDistance (0-100, 50 = Original) | Fog, far clip and object visibility |
| `[Display]` | TextureQuality (0, 1, 2), FMVQuality (0, 1) | Enhanced packs (`game_files\texture_packs`, `D:\Movie\Enhanced\2x`) |
| `[Display]` | PreloadShaders | Pre-create shaders while loading a level |
| `[Display]` | RenderScale | Retired; deleted on save |
| `[Audio]` | Enabled | 0 clears `RECOMP_AC97_READY` so no APU is brought up (never saved) |
| `[Audio]` | Volume (0-100) | Master volume |
| `[Input]` | Keyboard, MouseAim | Keyboard bindings and mouse aim on or off |
| `[Input]` | MouseSensitivity | Mouse sensitivity (see gap below) |
| `[Input]` | InvertY | Invert mouse Y |
| `[Input]` | InvertFlight | Invert VTOL flight controls (consumer not in this pack) |
| `[Input]` | KeyboardLayout | Passed to `RECOMP_KEYBOARD_LAYOUT` (default `pc`) |
| `[Bindings]` | Start, Back, A, B, X, Y, White, Black, LeftTrigger, RightTrigger, MoveLeft, MoveRight, MoveBack, MoveForward, LeftThumb, RightThumb, MouseFire, MouseAltFire | Virtual-key code per action (`pc_native_settings.cpp:35-37`) |

Controller invert and vibration are not INI keys. They are options in the original player profile (`pc_native_settings.cpp:498`).

Sources: `main.c:175-201` for loading, `pc_native_settings.cpp:227-245` for saving.

### Env vars

| Variable | Effect |
| --- | --- |
| `RECOMP_WINDOW_WIDTH`, `RECOMP_WINDOW_HEIGHT`, `RECOMP_FULLSCREEN` | Starting window and fullscreen |
| `RECOMP_VSYNC`, `RECOMP_FRAME_LIMIT`, `RECOMP_PRESERVE_ASPECT`, `RECOMP_SCALE_FILTER`, `RECOMP_SHOW_FPS` | Display policy |
| `RECOMP_DRAW_DISTANCE`, `RECOMP_TEXTURE_QUALITY`, `RECOMP_FMV_QUALITY`, `RECOMP_SHADER_PRELOAD` | Display content |
| `RECOMP_TEXTURE_PACK_DIR` | Where the 2x and 4x texture packs are looked for |
| `RECOMP_KEYBOARD`, `RECOMP_MOUSE_AIM`, `RECOMP_MOUSE_SENSITIVITY`, `RECOMP_INVERT_Y`, `RECOMP_INVERT_FLIGHT`, `RECOMP_KEYBOARD_LAYOUT` | Input |
| `RECOMP_AC97_READY`, `RECOMP_AUDIO_VOLUME` | Audio |
| `RECOMP_RENDER_SCALE` | Diagnostic 1-4 scale of the render height; default 0 |
| `RECOMP_CONSOLE` | Attach or allocate a console; otherwise stderr goes to `%LOCALAPPDATA%\MechAssault2\Logs` |
| `RECOMP_GAME_OVERLAY`, `RECOMP_CACHE_NAMESPACE` | Asset overlay folder and cache namespace (from `cache.version`) |
| `RECOMP_PB_EXEC`, `RECOMP_GPU` (d3d11), `RECOMP_TITLE_KEVENTS`, `RECOMP_VBLANK`, `RECOMP_FB_WINDOW`, `RECOMP_HEAP_RECLAIM`, `RECOMP_ASYNC_IO`, `RECOMP_USB`, `RECOMP_INPUT_LATCH`, `RECOMP_MOUSE_GUEST_POLL`, `RECOMP_EXIT_ON_CLOSE` | Launcher defaults (set to 1 when absent, `main.c:164-174`) |
| `RECOMP_USB_PADS` | Controller count; split-screen Add Guest needs 2 or more |
| `RECOMP_ONLINE_MENUS`, `RECOMP_SPLITSCREEN_MENUS`, `RECOMP_DEMO_MENUS` | Restore the disabled menu items (fork switches) |
| `RECOMP_NO_BARREL_ARROWS` | Turns off the barrel arrows and value-row clicks |
| `RECOMP_WATCHDOG_SECS` | Arms the hang watchdog (`main.c:619`) |
| `RECOMP_SAVE_LATENCY=1` | Writes one `[SAVE-LATENCY]` line per save exit, and `[SAVE-LATENCY-UNCOVERED]` for callbacks outside a save |
| `RECOMP_SCENARIO_TRACE` | Removed from the scenario loader in the v5 fix |

### Trace and test hooks

- `RECOMP_MENU_TRACE` prints `[PC-MENU]`, `[PC-MOUSE]`, `[PC-ARROWS]`, `[PC-POINTER]` and `[PC-GLOW]`.
- `RECOMP_MENU_DUMP=1` prints each menu's widget tree as `[MENU-DUMP]` (vtable, flags, state, action, text id, rect). Use it before changing a menu policy.
- `RECOMP_DISPLAY_TRACE` prints `[PC-DISPLAY]` for size, aspect and movie changes.
- `RECOMP_FOOTER_TRACE` prints `[PC-FOOTER]`.
- `RECOMP_UI_LIVE` uses the injected pointer for hover tests (`hovershot.py` takes `hover=WxH`).
- `RECOMP_HUD_TRACE` prints `[HUD-LAYOUT]` (widescreen HUD layout; see the HUD note).
- `RECOMP_SAVE_LATENCY=1` for save attribution. Require a full-span record and no coverage error before optimizing.
- `RECOMP_GPU_RECOVERY_TEST` suppresses the renderer-error message box in tests.

Log lines: `[PC-MENU] saved settings WxH fullscreen=N` (or `failed to save`) is written on each Back or Apply (`pc_native_settings.cpp:244`). `[EXIT]` lines show the confirmed quit path.

## Menus, pause and input

- The main menu and HUB start screens use Enter to select. Escape there opens the EXIT GAME prompt, and a confirm posts `WM_CLOSE`. In gameplay, Escape opens the pause menu.
- Escape on a settings page closes the page (back, saving the settings). Settings pages never quit the game.
- Pause menu items are stored in action order 100, 200, 500, 600, 400, 11, 8, 9, 800. Action 600 is Add Guest in a networked session, otherwise Quit to Main Hub. Match on the action number and the text, not the slot.
- Mouse capture: a settings page captures the mouse on open and releases it on Back. Escape with the camera captured releases the camera and does not pause. Focus loss releases camera capture. F8 toggles capture; F11 or Alt+Enter toggles fullscreen.
- Mouse aim turns on when play resumes from a menu on keyboard and mouse (PC_WINDOW_AIM_MESSAGE).
- Input device: the last device used decides the prompts and whether rumble runs. Keyboard and mouse input stops controller rumble at once.
- Controls page rows: keyboard controls, mouse aiming, sensitivity (steps of 10), Invert mouse Y, Invert flight (VTOL), bindings, defaults, controller settings, back.
- Controller page rows: Invert right stick Y and Vibration (profile options), keyboard and mouse settings, back.
- Bindings: three pages of eight. Pick a row, press a key or mouse button; Escape cancels. Enter, Escape, F8 and F11 are reserved on menus.

## Prompts and text

`ma2_pc_prompt_text` rewrites the guest's authored strings before they are drawn. On keyboard it:

- replaces `Press A` / `the B button` / `White button` and similar with the bound key, in order, and keeps the authored case;
- maps `Pull the Right trigger` to a press of the mouse fire key, but only as a bounded trigger phrase;
- replaces `D-pad` with `arrow keys`, `left thumbstick` with the forward, left, back and right keys, and `right thumbstick` with `mouse`;
- maps `Click the Left thumbstick` and similar to the stick-click binding.

On a gamepad it keeps the controller words (A, B, X, Y, RB, LB, Menu). A/B menu commands use Enter and Esc only for the explicit command list. Everything else uses the current gameplay binding.

Footer legends (text ids 0x70010000-0x70010005) show Enter, Esc and the bound keys, with short names for mouse and special keys. QTE captions come from `ma2_pc_qte_caption` and use a font10 width table.

## Rumble

- Vibration reaches the native XInput motors through the XInput layer. It is controller option 1 in the profile.
- A device switch to keyboard and mouse stops the primary controller's feedback at the next report. Closing or switching away stops active motors at once.
- Candidate (not verified in play): a four-bit port mask for pause (`xbox_InputFeedbackPaused`), so physical output stops at pause and each port stays suppressed until its zero is retired.
- Candidate: the expiry block in `sub_0001FE60` (1FEC2-1FEF2) keeps a slot reserved until its zero is accepted. Without it, a skipped zero is never retried.
- The rumble scheduler (`sub_0001FD30`) takes the first duration-zero slot. There are ten slots; a pending zero uses one.

## Saves

- Location: `%LOCALAPPDATA%\MechAssault2\Saved Games` for saves and caches; `%LOCALAPPDATA%\MechAssault2\Logs` for logs. Profiles are shared per Windows user. The installer ships no save or profile data.
- Files: `MASave.sav` (308 bytes in a checked sample) and `Missions\MASaveGame.wdf` (3145728 bytes, preallocated checkpoint generation). The checkpoint writes the WDF to the cache path (Z: maps to the cache, from the original string `Z:\Missions\MASaveGame.wdf` at `00475FA8`). A profile-only span writes 380 bytes: 308 profile and 72 global settings.
- Storage helper: `ma2_pc_copy_missing` copies only missing files, so existing saves are never replaced (`pc_storage.h:9`).
- Chain: `sub_0015D4C7` -> `sub_0015EFF2` (checkpoint to cache, installs the progress callback) -> `sub_000B53E0` (the only direct caller of the common writer `sub_000B9400`). Profile envelope: `sub_00167B25`.
- Timing: the progress callback `sub_0015CED6` runs at 100 ms. Long callbacks can dominate the save. The graph phase is B53E0-inclusive.
- Save and Quit: the transfer before `sub_00167B25` (`2BD76E`) is not measured by the current probe.

## Packaging and launcher

- Launcher: `MechAssault 2.exe` is `main.c`. It loads `game_files\ma2.xbe` (error box if missing), sets up storage, and runs the entry point. It picks `pc_assets` for the packaged overlay.
- Installer: one exe (`release\MechAssault 2 Recomp Installer.exe`, about 108 MB). Its stub unpacks an LZMS kit to `%TEMP%\MA2RecompInstaller-<id>` and runs `installer\gui.py` (or `install.py` with arguments). The kit is built by `assemble.py`, `cmake_patch.py`, `gen_patch.py make`, `make_payload.py` and `installer-exe\pack_installer.py`.
- Install steps: unpack the XISO, run xboxrecomp, apply the patch set, build with MSVC (VS Build Tools are fetched if no compiler is found; the fetch path was not exercised end to end), and copy out a clean PC Release. A CLI end-to-end run passes 11 steps; the objcompare check gives 201 of 202 objects identical (only the path-hashed `.data$rs` differs).
- Accented Windows user names: the build moves to an ASCII folder `<drive>:\MA2RecompBuild-<hash>`, because link.exe reads the response file in the ANSI code page.
- Publish: PC Release only. `publish.py --freeze/--stage/--install`. PC Release Bots is abandoned.
- Release notes: rerun `make_payload.py` after each publish and before packing. `release.json`'s exe hash is provenance only.

## Bugs and fixes (history)

| Date | Item | Status |
| --- | --- | --- |
| 2026-10-14 | Movie 4:3 forcing squeezed widened intros. Removed; movies follow the screen size. | fixed |
| 2026-10-15 | Pause and Add Guest slot checks disabled bot-match exits. Fixed 2026-10-19 by matching the text too. | fixed, verified in game |
| 2026-10-15 | Shader pre-creation: miss-path CPU time 107 ms to 9 ms on Bogged Down. | in the published builds (default off) |
| 2026-10-10 | Save pause attribution: profile spans missed the checkpoint graph. Latency probe added (`RECOMP_SAVE_LATENCY`). | implemented in the approved scope; integration not confirmed in this pack |
| 2026-10-10 | Rumble expiry: skipped zero never retried. Candidate `sub_0001FE60` block. | candidate |
| 2026-10-10 | USB dispatch crash: stack matcher skipped the dispatch lock. Candidate in `kernel_bridge.c` and `kernel_hal.c`. | candidate, not confirmed |
| 2026-10-20 | Scenario loading hang (Sky_Test, Small): desc hook reverted in v6. | open |
| 2026-10-21 | Installer: accented user name build fix (ASCII build folder). | shipped |
| 2026-11-04 | Display: Unlimited frame limit removed; 0 / old ini = 144. | shipped (par24), `t28.frame-limit-no-unlimited-default-144` |
| 2026-11-10 | Release polish batch (Esc on dialogs, Sticky Keys restore, first-run defaults, BOT MATCH label, binding names, focus pause/mute, shader cache folder, footer hints, start-up boxes, scenario list notes). One fact each in pack t28. | in branch r26, not yet published; see the t28 page |

Superseded notes: the earlier "pre-create shaders" label was renamed to "Load level shaders when loading" (2026-10-15). The first save-latency plan with `recomp_0019` and `recomp_0035` hooks was replaced by the approved scope in `kernel_bridge.c`, `recomp_manual.c` and `recomp_0037.c`. Older notes saying to publish to both PC Release and PC Release Bots are superseded.

## Open questions

- Frame-limit row (CORRECTION 2026-11-10): there is no Unlimited preset any more. The presets are 30, 60, 90, 120, 144, 165, 180, 240, 360; a saved 0 or an old ini reads as 144 and a value below 30 as 30 (commit c3bee9c, 2026-11-04, in PC Release par24; `t28.frame-limit-no-unlimited-default-144`).
- Controller capture guard: a private candidate (F3) suppresses stale controller edges after capture. Its status against the current source is not confirmed.
- Invert flight: the INI key and UI row exist; the guest reader is not in this pack.
- Mouse look: the Invert Y and sensitivity handling inside `ma2_mouse_look` is inferred, not read from its body.
- Prompt candidate P1-P5: changes 31 of 181 authored rows. Whether the current source equals the candidate was not checked.
- Hub button activatecommand CRCs and the menu policy tables live in `ma2_bots_ui.c`, which is outside this pack.
- Pause trigger path (what raises the pause in guest code) is outside this pack.
- Rumble pause and expiry candidates are not verified in play; motor sensation was not tested.
- USB dispatch crash: the candidate is not confirmed by a runtime gate.
- Restart heap D27: native repair unproven; needs a repeat restart capture.
- Save and Quit transfer (`2BD76E`) is not timed.

