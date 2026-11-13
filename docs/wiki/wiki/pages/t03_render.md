# Rendering: NV2A emulation, D3D11, HUD, shaders, textures, performance

Source tree for the MA2 PC port: Paths below are relative to that folder unless shown in full. Fact records are in `facts/t03_render.json`.

## Overview

The original game writes NV2A commands into guest RAM and advances the push-buffer put pointer. Nothing in the PC port executes those commands unless `nv2a_pb_exec.c` walks them. The executor (`kernel/nv2a_pb_exec.c`) runs on the FIFO ack thread (`nv2a_ack_thread`, called from `kernel/xbox_memory_layout.c`), which is the only caller of `nv2a_pb_consume`. It decodes methods, keeps a mirror of the NV2A register state (`hardware_state`, line 784), and decides per draw whether the GPU or the CPU does the work. Method dispatch is `nv2a_pb_exec_method` (line 3357). Clears and draws become native D3D11 work in `kernel/nv2a_gpu_d3d11.c` when `RECOMP_GPU=d3d11`. `main.c` sets that default, together with `RECOMP_PB_EXEC=1`.

Fixed-function draws are only drawn when vertex 0 is already in screen space (`batch_is_screen_space`, line 1603). Programmable draws (`xf_mode&3 == 2`, not disabled by `RECOMP_NO_VSH`) go through `raster_batch_program` (line 2457). Vertex data is read by the GPU when vertex pulling is on (`nv2a_gpu_draw_buffers`); otherwise the CPU gathers it. If a vertex shader is still compiling, the CPU interpreter (`transform_vertex`) feeds the GPU draw so the frame keeps moving.

Pixel shaders are keyed on combiner instructions, not constants (`pixel_program`, line 423). Compiled DXBC is cached on disk under `%LOCALAPPDATA%/xboxrecomp/shader-cache` (v2 names, FNV-1a key), can be seeded from `pc_assets/shader-seed.pack`, and can be pre-created per level from `pc_assets/shader-levels.txt` when the "Load level shaders when loading" setting is on. A missing shader draws with the fallback program that frame.

Textures are looked up in an optional replacement pack (`kernel/nv2a_texture_pack.cpp`). A texture is hashed by SHA-256 over its decoded RGBA, and a match in `index.json` swaps in a DDS for the 2x or 4x pack. The pack is chosen by Display/TextureQuality at a frame boundary. Replacement is skipped during a movie. HUD mask stages are never replaced, because the game writes those thresholds itself.

Presentation: `nv2a_gpu_present` (line 2555) draws the GPU target for the presented guest address into a flip-model DXGI swap chain. If that is not possible, the surface is read back and shown in the GDI window in `video/fb_present.c`. FMV playback has its own native decoder (`bink_native.c`) and an optional enhanced MKV reader.

## Key code

| Address or function | What it does | Source |
|---|---|---|
| `nv2a_pb_exec_method` | Dispatches one NV2A method (subchannel object class, semaphores, software methods) | `kernel/nv2a_pb_exec.c:3357` |
| `draw_primitive` | Counts a draw and calls `raster_batch` | `kernel/nv2a_pb_exec.c:2812` |
| `raster_batch` | Chooses programmable path or fixed-function path; GPU or CPU rasteriser | `kernel/nv2a_pb_exec.c:2632` |
| `raster_batch_program` | Vertex-program path: pulls vertices, or gathers them on the CPU; falls back to `transform_vertex` while a shader compiles | `kernel/nv2a_pb_exec.c:2457` |
| `batch_is_screen_space` | Gate for fixed-function drawing | `kernel/nv2a_pb_exec.c:1603` |
| `hardware_state` | Copies executor state into `Nv2aGpuState` | `kernel/nv2a_pb_exec.c:784` |
| `note_drawn` | Records the largest drawn clip rectangle; this is what flip presents | `kernel/nv2a_pb_exec.c:821` |
| `dma_resolve` | DMA offset to guest address | `kernel/nv2a_pb_exec.c:185` |
| `nv2a_gpu_enabled` | Initialises D3D11 when `RECOMP_GPU=d3d11`; compiles the vertex shader | `kernel/nv2a_gpu_d3d11.c:667` |
| `nv2a_gpu_clear` | Native clears; masked or partial clears as small draws | `kernel/nv2a_gpu_d3d11.c:2345` |
| `pixel_program` | Pixel shader key, cache lookup, pending fallback, shader depth variant | `kernel/nv2a_gpu_d3d11.c:423` |
| `texture_view` / `sampled_view` | Texture SRV; calls the pack first, then the decoded original | `kernel/nv2a_gpu_d3d11.c:1386` |
| `present_init` | DXGI flip-discard swap chain, two buffers, B8G8R8A8 | `kernel/nv2a_gpu_d3d11.c:2410` |
| `nv2a_gpu_present` | Presents the target for the flipped address; pacer | `kernel/nv2a_gpu_d3d11.c:2555` |
| `open_seed` | Maps `shader-seed.pack` (magic NV2ASED1) | `kernel/nv2a_shader_cache.c:49` |
| `shader_path` | Disk-cache file name | `kernel/nv2a_shader_cache.c:123` |
| `read_blob` / `valid_blob` | Cached DXBC size check and D3DReflect validation | `kernel/nv2a_shader_cache.c:135` |
| `seed_blob` | Takes a shader from the seed and writes it into the user cache | `kernel/nv2a_shader_cache.c:164` |
| `nv2a_shader_level_begin` | Level load: bumps epoch, reads level list, queues pre-creation | `kernel/nv2a_shader_cache.c:489` |
| `nv2a_shader_note_level` | Records a shader used in the current level | `kernel/nv2a_shader_cache.c:549` |
| `load_index` | Reads a pack's `index.json` | `kernel/nv2a_texture_pack.cpp:122` |
| `pack_fingerprint` / `nv2a_texture_pack_fingerprint` | Pack key (SHA-256 over header and RGBA rows) | `kernel/nv2a_texture_pack.cpp:277` |
| `nv2a_texture_pack_frame` | Applies TextureQuality at a frame boundary | `kernel/nv2a_texture_pack.cpp:316` |
| `nv2a_texture_pack_view` | Lookup and SRV for a replacement | `kernel/nv2a_texture_pack.cpp:335` |
| `nv2a_texture_decode_row_canonical` | Row decoder used for fingerprinting | `kernel/nv2a_texture.c:301` |
| `linear_twin` | Swizzled format to the linear format with the same texels | `kernel/nv2a_texture.c:28` |
| `xbox_FramebufferWindowPresent` | GDI fallback present | `video/fb_present.c:73` |
| `sub_001CCC10` (recomp_0048.c) | HUD rect layout; patched to extrapolate between 4:3 and 16:9 |  |
| `sub_0007E5D0`, `sub_0007DC28` (recomp_0013.c) | HUD image edges; patched to snap to the output grid | same patch |
| `sub_00044BE0` (recomp_0007.c) | Camera far plane; patched with draw distance | `kit/source/game/bringup/patch_draw_distance.py` |
| `ma2_bink_decode` / `ma2_bink_copy` | Native Bink packet decode and copy into guest memory | `bink_native.h` |
| BinkOpen `0x00390FC0` (recomp_0094.c) | Movie open; success association at `0x00391EBB` |  |

## Added code and hooks

- **Executor and D3D11 renderer** (`kernel/nv2a_pb_exec.c`, `kernel/nv2a_gpu_d3d11.c`). These replaced the early CPU-only executor. The header of `nv2a_pb_exec.c` still describes the CPU path only; the code is authoritative.
- **Shader cache and seed** (`kernel/nv2a_shader_cache.c`, `.h`). Adds the disk cache, the seed pack, and level pre-creation.
- **Texture pack** (`kernel/nv2a_texture_pack.cpp`, uses DirectXTex and jsmn under `vendor/`). Replacement lookup, async reads and caches.
- **Row decoder** (`kernel/nv2a_texture.c`). Shared texel decoder; `nv2a_texture.h` declares it.
- **Versioned guest memory** (`kernel/nv2a_gpu_memory.h`). Version, watch and epoch counters for guest ranges.
- **Presentation** (`video/fb_present.c`). GDI window, copies each finished frame so it never reads memory being drawn.
- **Patch scripts** (all under):
- **Draw distance** (`patch_draw_distance.py`, `pc_draw_distance.h`). Hooks in recomp_0007, 0008, 0014 and 0028.
- **Native Bink** (`bink_native.h`, `bink_native.c`). Native packet decode, and enhanced MKV reader with fallback. Fixture: `kit/source/game/bringup/mission_agent_fmv_verify.py`.
- **Enhancer** (`Enhancer/`, `Enhancer/FMV/`). AI texture pack generator and FMV upscaler. Writes `index.json`, DDS files and enhanced MKVs.

## Env vars, settings and test hooks

**Settings** (INI key, default, range; the env var overrides it):

| Setting (INI) | Menu label | Default | Env var | Notes |
|---|---|---|---|---|
| Display/TextureQuality | Textures | 0 | RECOMP_TEXTURE_QUALITY | 0 Original, 1 2x Enhanced, 2 4x Enhanced |
| Display/PreloadShaders | Load level shaders when loading | 0 | RECOMP_SHADER_PRELOAD | Pre-creation thread |
| Display/DrawDistance | Draw distance | 50 | RECOMP_DRAW_DISTANCE | 0 to 100; 50 keeps the authored scene |
| Display/FMVQuality | Cutscene videos | 0 | RECOMP_FMV_QUALITY | 0 Original, 1 Enhanced |
| Display/VSync | VSync | 0 | RECOMP_VSYNC |  |
| Display/PreserveAspect | Preserve aspect | 1 | RECOMP_PRESERVE_ASPECT |  |
| Display/SmoothScaling | Smooth scaling | 1 | RECOMP_SCALE_FILTER | 0 is point sampling |
| Display/FrameLimit | Frame limit | 0 | RECOMP_FRAME_LIMIT | 0 is off |
| Display/ShowFPS | Show FPS | 0 | RECOMP_SHOW_FPS |  |
| Display/WindowWidth, WindowHeight | Resolution | 1280x960 | RECOMP_WINDOW_WIDTH, RECOMP_WINDOW_HEIGHT | Includes 640x480 (Xbox original) |
| Input/MouseAim | Mouse aim | 1 | RECOMP_MOUSE_AIM |  |
| Input/InvertFlight | Invert flight controls (VTOL) | 0 | RECOMP_INVERT_FLIGHT |  |

Settings are read from `main.c` lines 175 to 189 and from `pc_native_settings.cpp`.

**Renderer switches:**

- `RECOMP_PB_EXEC`, `RECOMP_GPU` (must be `d3d11`), `RECOMP_GPU_WARP`, `RECOMP_GPU_LEGACY_ENUMERATION`, `RECOMP_GPU_DEBUG`.
- `RECOMP_PIXEL_SPECIALIZE` (default 1); `RECOMP_NO_VSH`; `RECOMP_NO_COMBINERS` (CPU path); `RECOMP_VERTEX_PULL` (default on); `RECOMP_RASTER_THREADS`.
- `RECOMP_GPU_CONSTANT_STREAMING=discard`; `RECOMP_GPU_READBACK_PRESENT`; `RECOMP_HIGH_RES_PACING` (default 1).
- `RECOMP_GPU_RECOVERY_TEST`, `RECOMP_GPU_RECOVERY_FAIL_ATTEMPTS`.

**Shader cache:**

- `RECOMP_SHADER_CACHE=0` disables the disk cache.
- `RECOMP_SHADER_SEED=0` ignores the seed pack.
- `RECOMP_SHADER_CACHE_LIST=<file>` logs every cache name used (to build a seed).
- `RECOMP_GAME_OVERLAY` changes the `pc_assets` folder.

**Texture pack:**

- `RECOMP_TEXTURE_PACK_DIR` changes the root (default `game_files/texture_packs`).
- `RECOMP_TEXTURE_PACK_ASYNC=0` reads inline.
- `RECOMP_TEXTURE_PACK_TRACE=1` logs pack activity.
- `RECOMP_PACK_VERIFY=1` compares the fast and per-texel fingerprints.

**Diagnostics and traces:**

- `RECOMP_PERF`: `[PERF]` CPU phase lines in about 5 second windows.
- `RECOMP_UPLOAD_TRACE`: per-upload trace; not turned on by `RECOMP_PERF`.
- `RECOMP_RENDER_TRACE`, `RECOMP_TARGET_TRACE`.
- `RECOMP_VSH_VERIFY`, `RECOMP_VSH_VERIFY_DUMP`, `RECOMP_VSH_VERIFY_AFTER=<s>`, `RECOMP_VSH_TRACE=<n>`.
- `RECOMP_FRAME_TRACE=<flag file>` with `_SURFACES`, `_VERTICES`, `_TEXTURE`, `_PIXEL=<x,y>`, `_DEPTH`, `_METHODS`.
- `RECOMP_FB_DUMP=<prefix>` (one BMP per flip, or `RECOMP_FB_DUMP_FLIPS` for every flip); `RECOMP_FB_DUMP_INTERVAL`.
- `RECOMP_TEX_DUMP=<prefix>` with `RECOMP_TEX_DUMP_EVERY=<n>`; `RECOMP_TEX_STATE`.
- `RECOMP_RASTER_TEST` draws a known triangle after each clear.
- `RECOMP_PEEK=<addr,>`, `RECOMP_PEEK_CHAIN`, `RECOMP_FIND_NAN`, `RECOMP_FIND_QUAD`.
- `RECOMP_PB_UNHANDLED_ALL=1` lists every unhandled method.
- `RECOMP_PB_EXEC_VERBOSE` logs batches.
- `RECOMP_EFFECT_TRACE=1` (effect draws).
- `RECOMP_BOT_JET_SHOW=1` (bots tree): a jet-capable bot jets 30 m ahead, for smoke checks.

**Window:**

- `RECOMP_FB_WINDOW` (main.c sets it), `RECOMP_FB_VA` (pin to one guest framebuffer), `RECOMP_FB_HIDDEN`, `RECOMP_FULLSCREEN`, `RECOMP_DISPLAY_TRACE`.
- `RECOMP_EXIT_ON_CLOSE` (main.c sets it).

**Test commands (CPU only, safe to run without the game):**

- `python "kit/source/game/bringup/mission_agent_fmv_verify.py" decode` (108 checks at the last run), `settings`, `evidence`.
- `patch_draw_distance.py` is idempotent and can be re-run after regenerating the recomp sources.
- Measuring laptop-class CPU phases on the desktop: `mission1perf.py` with `REPLAY_USERENV=1` and `REPLAY_AFFINITY=FF0000`, then `m1sum.py`.

## Bugs and fixes

| Date | Problem | Cause and fix | Status |
|---|---|---|---|
| 2026-10-15 | Laptop texture fingerprinting cost about 140 ns per texel (render-thread hitches) | Row decoder `nv2a_texture_decode_row_canonical`; digest 3.5x faster; checked bit-exact over more than 1000 textures (`RECOMP_PACK_VERIFY`). Shipped in 22ddd9d4. | Verified on the laptop |
| 2026-10-15 | 250 to 450 ms per first-use shader compile on the laptop; stalls when an effect first appears | `shader-seed.pack` precompiled shaders written into the user cache; `shader-levels.txt` and per-level pre-creation. Mission 1 from an empty cache: 439 compiles, then 0 with the 3071-shader seed. | Verified on the laptop; not all shaders covered |
| 2026-10-15 | Texture pack reads on the render thread |  | Shipped |
| 2026-10-11 | HUD misaligned at ultrawide (extra weapon bar at 21:9) | `sub_001CCC10` evaluated at 4:3 and 16:9 and extrapolated. | Installed; playtest |
| 2026-10-11 | HUD image edges off by up to 5 output pixels at 3440x1440 | Floor on the output grid; unchanged at 640x480. | Installed |
| 2026-10-11 | Faint vertical lines on upscaled HUD sprites | WRAP bled the opposite edge; CLAMP for small in-bounds sprites. | Installed |
| 2026-10-11 | Dashed weapon-bar outline with TextureQuality=1 | The pack matched game-written threshold masks and replaced them with upscaled copies. Mask stages now never use pack replacements, and are point-sampled. | Installed (EXE 28798d5d) |
| 2026-10-14 | Intro movie squeezed to 0.84:1 on a 2.4:1 display | The 4:3 movie override was forced before the movie player's layout; override removed. | Fixed |
| 2026-10-09 | Enhanced FMV never selected at boot | The Xbox wrapper ORs flag `0x00800000`, so the first BinkOpen argument is a file handle (tag 0x48000000), not a C string. Resolved with `GetFinalPathNameByHandleW`. | Code-confirmed; live check pending when written |
| 2026-10-09 | Enhanced FMV rejected at frame 1 | Read frame rate from +0x10 (LastFrameNum) instead of +0x14/+0x18. Fixed in `bink_native.c`. | Verified live up to the fallback |
| 2026-10-17 | Jet-jump smoke looked like dark boxes and hard seams | 178 SpecialEffects images had been AI-upscaled; now resampled per channel. Packs rebuilt. | Fixed |
| 2026-10-10 | Explosion hitch attributed to shaders and textures in a summary | Not supported by the evidence: nested interval totals cannot be summed, and the one 8.8 ms shader split has no explosion marker. | Open |

## Open questions

- **Explosion hitch.** Not tied to a specific call.
- **Shader telemetry gap.** In the G7 renderer copy used for the 2026-10-10 review, `miss_split` reports `create_ms` as 0 when a shader job finishes on a later poll (lines 121 to 131 of that copy). The current source may differ. Fix not yet made.
- **Cached shader load cost on Iris Xe.** About 5 ms each, against 0.6 ms on the desktop. Driver cost, not fixed.
- **Radar blips.** Drawn unsqueezed at 16 guest pixels at widescreen.
- **Enhanced FMV in-game.** Live activation, skip, A/V sync and menu behaviour still need a live smoke test. The enhanced picture is shown at guest size (720x480 for MAIntro), not as a true 2x movie texture.
- **FPS overlay gamepad.** A controller connected after an absent start may never be seen by the overlay: the ImGui Win32 backend refreshes availability only on WM_DEVICECHANGE, which the frontend does not forward.
- **Native graphics regression.** The 19 hardware, WARP and texture CTests in the v22 plan have not been run.
- **Movie SAR.** The enhanced MKV has SAR 8:9 tagged; the notes say this tag is wrong and the true NTSC 720x480 aspect is 3:2 square-pixel.
- **Documentation drift.** `kit/source/game/src/nv2a/README.md` says PGRAPH does no rendering, and `docs/technical/d3d-translation.md` describes a generic D3D8 layer. Neither describes the MA2 renderer. `docs/technical/d3d8ltcg-device-context.md` describes Burnout 3. `docs/technical/nv2a-shaders.md` names files that are not in the MA2 kernel.

