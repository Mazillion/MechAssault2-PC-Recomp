# Architecture

How the PC build of MechAssault 2 is put together, from the Xbox executable to the window on screen. This page is an
overview for people who want to read, fork or debug the project. The details live in the fact wiki (`docs/wiki/`); ids
such as `t03.pb-exec-role` are fact ids you can look up with `python wq.py "<id>"` (see [fact-index.md](fact-index.md)).

The repository contains no game data. You need your own copy of the retail disc; the installer turns it into the PC
build on your machine ([building.md](building.md), [patches.md](patches.md)).

## 1. The pipeline at a glance

```
ma2.xbe (US/NTSC retail, image base 0x00010000)
   |  xboxrecomp tools: XBE parser, disassembler, function id / ABI analysis, lifter
   v
generated C: recomp_0000.c ... recomp_NNNN.c   (one void sub_XXXXXXXX(void) per function)
   |  + patch set (hooks, guards, fixes) applied to the generated files
   |  + optional fast copies (FastCode)
   |  + hand-written C/C++ of this project (recomp_manual.c, bots, scenarios, PC layer)
   v
MSVC build  ->  MechAssault 2.exe  (launcher + recompiled game + runtime)
   |
   v
runtime: memory map, kernel HLE, NV2A -> D3D11, APU/DSP -> XAudio2, input -> XInput, movies
```

Generated code is not stored in the repository. It is produced at install time from the user's own `ma2.xbe` and is
cited in the wiki as `generated recomp_XXXX.c` (t08 overview). Hand-written code lives under `kit/source/game/src/`
(game side, including `bots/` and `scenario/`) and `kit/source/runtime/src/` (the xboxrecomp runtime); see
[building.md](building.md) for the layout.

## 2. Static recompilation (t08)

The toolchain runs in fixed stages (`t08` overview): the XBE parser reads the headers (image base `0x00010000`, the
XOR-encoded entry point, section table, kernel thunk table); the disassembler finds functions by recursive descent from
the entry point plus a linear sweep; function identification and ABI analysis classify functions and recover
signatures; the lifter turns every x86 instruction into a C statement. Each function becomes `void sub_XXXXXXXX(void)`:
arguments travel on a simulated guest stack, return values in `g_eax`.

Conventions every reader of the generated code needs:

- **Registers are thread-local globals** (`g_eax` ... `g_edi`, the x87 stack, `g_fp_cmp`), so each guest thread has its
  own register file. The cost is that MSVC cannot keep them in host registers (`t15.guest-registers-tls-not-register-allocated`).
- **Memory** is accessed through `MEM8/16/32` macros over a 64 MB mapping; `XBOX_PTR` truncates to 32 bits first so
  overflow wraps as on the Xbox.
- **Flags** are not materialised. Each arithmetic or compare saves its operands (`_fa/_fb`) and a condition macro such
  as `TEST_Z(_fa, _fb)` evaluates the condition when a branch needs it.
- **Direct calls** push the real return address and call through `RECOMP_ABI_CALL`; guest code can read that return
  address, so it must be right even though control flow uses C call and return.
- **Indirect calls** (`RECOMP_ICALL_SAFE_AT`, `RECOMP_ITAIL`) look the target up in three tiers: manual overrides,
  the generated dispatch table, then the kernel bridge at `0xFE000000` and up. Most hard bugs in a port come from
  indirect calls, wrong stack balance, or a wrong lift (`t08` overview).
- **x87 compares** go through `RECOMP_FCMP` / `g_fp_cmp` (2 unordered, -1 less, 0 equal, 1 greater). A stale
  `g_fp_cmp` made CRT `cos/sin/fmod` spin; the five affected sites were fixed by hand edits
  (`t07.fpu-c2-fix-state`).

Debug switches that exist in the toolkit and are used here: `RECOMP_ABI_CHECK` (build flag, verifies callee register
and stack balance), `RECOMP_WATCHDOG_SECS=N` (dumps registers and the guest stack when progress stops),
`RECOMP_UNIMPL_TRAP=1`, `RECOMP_NV2A_TRACE=1`, `RECOMP_PB_EXEC=1`, `RECOMP_AC97_READY=1`.

## 3. Runtime: memory and kernel (t07)

- **Memory map.** Guest RAM is one 64 MB page-file mapping mirrored 28 times at 64 MB steps, so addresses wrap as on
  the 26-bit Xbox bus. XBE sections sit at their linked addresses. The fake TIB is at `0x00004000`, the main stack at
  `0x00780000` (8 MB, growing down), kernel data exports at `0x00740000`, the heap from `0x00F80000` to the top. A null
  dereference therefore faults where it happens.
- **Kernel HLE.** Import thunk slots are rewritten to synthetic addresses at `0xFE000000 + 4 x slot`; an indirect call
  to such an address reaches `recomp_lookup_kernel`, which picks a bridge function. The bridge reads stdcall arguments
  from the simulated stack at `g_esp`, does the Win32 work and returns in `g_eax`. Hardware apertures (NV2A at
  `0xFD000000`, MCPX at `0xFE800000`, flash at `0xFF000000`) are plain zeroed RAM; a background thread clears the NV2A
  busy bits that Direct3D 8 spins on.
- **Threads and timing.** Threads are real host threads (`PsCreateSystemThreadEx` calls `CreateThread`), critical
  sections are shadowed onto host locks keyed by the guest pointer, `KeDelayExecutionThread` becomes `Sleep`. One
  kernel timer thread drains guest DPCs, the 60 Hz vblank and guest timers. Game logic is single-threaded by design;
  the guest also runs code on the timer thread and on the NV2A executor (`t15` overview). Most freezes found were a
  spin or a lock that waited for something that never changed; the fixes are loop guards, lock ordering and early-outs
  for dead sockets (`t07` overview; for example `t26.dsound-active-reinsert-freeze`).
- **Frame clock.** The original advances its clock in whole milliseconds per frame, which loses time at high frame
  rates; `ma2_ms_step_carry` in `recomp_manual.c` carries the fraction (`[Display] FrameClockCarry`, env
  `RECOMP_MS_CARRY`; `t14` pack, "Easy wins B11").

## 4. Graphics: NV2A to D3D11 (t03)

The game writes NV2A commands into guest RAM and advances the push-buffer put pointer. Nothing executes them unless
`nv2a_pb_exec.c` walks them (`t03.pb-exec-role`):

- The executor runs on the FIFO ack thread (`t03.nv2a-ack-thread-owner`), decodes methods, mirrors the NV2A register
  state in `hardware_state`, and decides per draw whether the GPU or the CPU does the work. `main.c` turns the renderer
  chain on by default (`RECOMP_GPU=d3d11`, `RECOMP_PB_EXEC=1`; `t03.gpu-d3d11-enable-env`, `t03.pb-exec-default-env`).
- Clears and draws become native D3D11 work in `nv2a_gpu_d3d11.c`. Fixed-function draws are drawn only when vertex 0 is
  already in screen space (`t03.screen-space-rule`); programmable draws go through the vertex-program path, with
  vertex pulling sending guest buffers straight to the GPU (`t03.vertex-pull-path`). While a vertex shader compiles, the
  CPU interpreter keeps the frame moving (`t03.pending-shader-cpu-fallback`).
- Pixel shaders are keyed by their combiner instructions, not constants (`t03.pixel-shader-key`). Compiled DXBC is cached
  on disk under `%LOCALAPPDATA%\xboxrecomp\shader-cache` (older builds) or `%LOCALAPPDATA%\MechAssault2\ShaderCache`
  (`t28` pack), and pre-created per level from the levels the player has visited (`t03.shader-what-loads-when`).
  The runtime can also read a pre-compiled seed (`pc_assets\shader-seed.pack`); public releases do not ship one,
  because its entries are compiled from the game's own shader programs.
- Optional texture packs replace textures by a SHA-256 over the decoded RGBA; replacement is skipped for movies and for
  HUD mask stages (`t03.texture-pack-lookup-rules`, `t03.texture-mask-stages-original`).
- Presentation uses a DXGI flip-model swap chain, with a GDI framebuffer window as fallback
  (`t03.present-flip-model`, `t03.fb-present-gdi-fallback`). HUD layout is evaluated at 4:3 and 16:9 and extrapolated
  for other aspects (`t03.hud-layout-interpolation`).
- Movies use a native Bink decoder (`bink_native.c`) and an optional enhanced MKV reader (`t03` overview, `t13` pack).

Performance numbers (frame profile, pushbuffer words per frame, executor and game-thread costs) are in `t15`
(`t15.pa-game-thread-critical-path`, `t15.pa-executor-second-bottleneck`).

## 5. Audio: APU and DSP to XAudio2 (t02)

The title's DirectSound writes voice records and registers into the APU range at `0xFE800000`. The voice processor
(`apu_vp.c`) runs once per 32-sample frame (667 us at 48 kHz): it walks three voice lists (2D, 3D, MP), mixes active
voices into 32 mixbins through a libsamplerate sinc resampler, an optional state-variable filter and an optional HRTF
(`t02.audio-pipeline-overview`, `t02.vp-frame-order`, `t02.voice-process-chain`). `apu_dsp_runtime.c` then runs the
title's own GP DSP program (DSP56300 interpreter) every frame and its EP program every eighth frame, on its own thread
(`t02.dsp-frame`); the EP output is read at 24 bits, restored by a headroom gain, soft-limited and sent to XAudio2
(`t02.xa2-output`). HRTF is off by default (`t02.hrtf-default-off`). The fixes that shaped it: DSP headroom for
clipping (`t02.clip-fix-confirmed`), an owned voice tail drain (`t02.owned-tail-design`), and XACT lock-order guards
(`t02.xact-list-spin`, `t26.dsound-active-reinsert-freeze`, `t25.dsound-defer-double-insert`).

## 6. The PC layer (t06, t05)

`main.c` is the launcher: it loads `game_files\ma2.xbe`, sets up storage, reads `pc_settings.ini` and turns each key
into a `RECOMP_*` environment variable (precedence: env var, then INI, then built-in default). `platform/pc_frontend.c`
holds the shared state (settings, bindings, input context, mouse capture, display sizes, rumble policy);
`pc_native_settings.cpp` draws the PC-only settings pages on top of the original Xbox menu objects; `pc_prompts.cpp`
rewrites guest prompt text to name the keys that are actually bound. The original menus are kept; PC pages take over
only when the matching menu vtable is active (`t06` overview). Saves, profiles and logs live per Windows user under
`%LOCALAPPDATA%\MechAssault2`; the installer ships none (`t06.save-install-storage-policy`). Mouse aim is added in
`recomp_manual.c` at the authored argument boundary and divided by the simulation step so it stays constant across
frame rates (`t05` overview).

## 7. Hooks and the patch set

Three mechanisms connect hand-written code to generated code:

1. **Manual overrides** in `recomp_manual.c`. A function listed there replaces the generated one. The file must include
   `recomp_types.h` so it shares the thread-local registers; without it an override writes a plain global the caller
   never reads (`t08.tc-manual-include-types`).
2. **Hooks inserted into generated files** by patch scripts. Each edit anchors on a unique `loc_XXXXXXXX:` label and is
   idempotent; a missing anchor raises "hook changed; inspect". Examples: the menu, text, input, audio and display hooks
   (`t06.installer-patch-hook-regen`); the bot hooks `ma2_bots_level_spawn` at
   `loc_001DB1E8` and `ma2_bot_player_tick` at `loc_000C8130` (`t01a.bots-native-spawn-hook`,
   `t01a.bots-player-tick-hook`); the scenario hooks (`t04.level-load-hook`, `t04.hub-action-8-hook`).
3. **Hand edits in generated code** for crashes and hangs: `#if 0` around functions that cannot run, vtable guards,
   list-walk guards. They are lost when the code is regenerated, so they must exist as patches
   (`t08` overview, "Added code and hooks").

The patch set and how it is stored and applied is described in [patches.md](patches.md).

**FastCode.** The kit can also produce `recomp_fast_NNNN.c` copies of generated files in which memory accesses use a
plain pointer and a local memory offset and guest registers become C locals (`[Cpu] FastCode` / `RECOMP_FASTCODE`;
`t15` pack, `t15.prior-art-register-locals`). Because a fast copy is derived from the generated file, a hand edit in a
generated file that has a fast copy needs a *post-FastCode* patch applied after the fast copy is made; the kit stores it
under `patches/fastcode/post/` with a `post` entry in `fastcode.json` (`t28.installer-kit-1109-post-fastcode`). Bodies
that call native `ma2_` functions are denied a fast copy.

## 8. How the installer builds it on the user's PC (t06)

One installer exe holds the whole kit. A native stub unpacks an LZMS-compressed kit to
`%TEMP%\MA2RecompInstaller-<id>`, runs the bundled Python on `installer\gui.py` (or `install.py` with arguments) and
deletes the unpacked copy on exit (`t06.installer-single-file`). The install has 11 required steps (check, disc, extract,
analyze, recompile, generate, patch, build, assets, install, finish) plus optional texture and video steps: it unpacks the
XISO, runs xboxrecomp on `ma2.xbe`, applies the patch set, builds with MSVC, and copies out a clean install folder
(`kit/installer/install.py`, `t06.installer-pipeline`). If no compiler is found it fetches Microsoft's VS Build Tools and checks the Authenticode
signature (`t06.installer-msvc-autoinstall`). A non-ASCII Windows user name moves the build to an ASCII folder because
`link.exe` reads its response file in the ANSI code page (`t06.installer-build-root-ascii`). Enhanced textures and
videos can be added later by running the same exe on the install folder. Step-by-step instructions for building without
the installer are in [building.md](building.md).

## 9. Where to read more

| Topic | Wiki pack |
|---|---|
| Recompiler, generated code conventions | `t08_toolchain` |
| Memory map, kernel, threads, freezes | `t07_stability`, `t10_status_misc`, `t15_pipeline` |
| Renderer, shaders, textures, HUD | `t03_render`, `t15_editor_render_gaps` |
| Audio | `t02_audio` |
| Menus, input, settings, saves, installer | `t06_ui_pc`, `t05_gameplay`, `t28_release_polish_1110` |
| Bots | see [bots.md](bots.md) |
| Scenario editor and custom scenarios | see [editor.md](editor.md) |
