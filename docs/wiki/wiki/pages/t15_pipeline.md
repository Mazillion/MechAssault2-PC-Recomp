# Native pipeline perf: guest CPU, kernel and threading (2026-10-30, P-B)

One codegen measurement was made (recomp_0048.c compiled to asm); no game was run.

## Overview

**Game logic is single-threaded by design.** MA2 creates two guest threads:
- the game itself, which the first `PsCreateSystemThreadEx` runs inline on the main thread;
- one event worker, `sub_00394790`.

**Guest code also runs on two host threads besides those two:**
- the kernel timer thread, for vblank, DPCs and timers;
- the NV2A executor, for software methods (`ma2_gpu_software_method` → `sub_004152E0`).

`RECOMP_GUEST_LOCK` is off, so all of these run in parallel. The game thread is the critical path.

**Lifted code pays two big per-instruction costs:**
1. **Guest registers are TLS memory.** `g_eax.g_edi` and the x87 stack are `__declspec(thread)`. Guest memory pointers are made from integers, so MSVC must assume a guest store can alias a register, and every register access becomes a load or a store. A `/volatile:iso` build changes nothing, so aliasing is the cause.
2. **In the bots overlay, every register-addressed access goes through `ma2_tank_scoped_pointer`.** This is the tank fixed-step clock scope. It is a range check plus a TLS flag test, and it reloads `g_xbox_mem_offset` each time.
   - Measured on recomp_0048.c: 90,736 instructions with the scoped accessor, 41,634 with the plain pointer.
   - The 59 prebuilt gen objects already use the plain pointer.

The kernel layer handles about 125k calls per second in a 6v6 match. That is not a throughput problem, but locks and the single timer thread couple audio stalls into timing.

## Key code

| Address or function | What it does | Source |
|---|---|---|
| `XBOX_PTR` / `ma2_tank_scoped_pointer` | Overlay memory accessor with the clock-scope check | `generated recomp_types.h (made at install time, not in the repository)`, `ma2_tank_step_scope.h:11` |
| `RECOMP_ICALL*` | Indirect call: 3 shared trace stores, manual lookup, flat table, kernel |  |
| `kernel_thunk_dispatch` | Every kernel call: optional guest lock, GetTickCount, counters, 2 s summary |  |
| `xbox_cs_create` / `xbox_RtlEnterCriticalSection` | Shadow guest locks, spin count 0 | `kit/source/game/src/kernel/kernel_rtl.c`, `:562` |
| `kernel_timer_thread` | 1 ms high-resolution timer → vblank (60 Hz), DPC drain, 32 timers |  |
| `nv2a_idle_wait` | Executor yield policy: `Sleep(0)` while active and for 2 ms after, then 250 µs | `kit/source/game/src/kernel/xbox_memory_layout.c` |
| `nv2a_gpu_wait_word` | GPU fence wait through WaitOnAddress, 2 ms bound |  |
| `ma2_ms_step_carry` | Frame clock fractional-ms carry (ew-b11) | `kit/source/game/src/recomp_manual.c` |
| `optimize_leafs.py` | Precedent: registers as locals for 2 Bink leaves | `kit/source/game/bringup/optimize_leafs.py` |

## Measurements

- **Codegen, recomp_0048.c** (build flags `/O2 /Ob2`, no `/GL`):
  - scoped accessor: 90,736 instructions, 636 KB obj;
  - plain accessor: 41,634 instructions, 416 KB obj;
  - the `/volatile:iso` asm is the same size.
- **Lifted functions:** 25,841, of which 5,245 are leaves (no call, ICALL, ITAIL or MMIO).
- **Kernel calls per second** (NRC 6v6):

| Call | Rate |
  |---|---|
| RtlLeaveCS | 51k |
| RtlEnterCS | 39k |
| KfLowerIrql | 16k |
| KfRaiseIrql | 14k |
| KeRaiseIrqlToDpcLevel | 2.4k |

- **Bots, median ms per second of play:**

| Item | Median | p90 |
  |---|---|---|
| Total | 119-144 | 170-187 |
| Brain | 37-41 |  |
| Ctrl | 44-55 |  |
| Rays | 21-24 |  |
| Plans | 4-20 | up to 61 |
| World view | 16-31 |  |

That is about 2 ms of a 14 ms frame.

## Planned switches (see PIPELINE-B.md; none implemented yet)

| Switch | Purpose |
|---|---|
| `[Cpu] FastCode` / `RECOMP_FASTCODE` | Fast bodies (plain pointer, local memory offset, registers as locals) |
| `[Cpu] KernelThreads=split\ | single` |
| `[Cpu] CsSpin=auto` | Critical-section spin count |
| `[Cpu] ThreadPlacement=auto\ | off` |
| `[Cpu] Nv2aWake` | WaitOnAddress executor wake |
| `[Bots] PathWorker` |  |
| `RECOMP_KERNEL_SUMMARY` | Kernel summary on demand |

Proposed test hooks:
- `RECOMP_PERF_CPU`: per-thread CPU time.
- `RECOMP_FIXED_DT` + `RECOMP_STATE_HASH`: a bit-exact A/B state hash, using the 0x50BA70 fixed-dt override (unverified).
- `[CPU-TOPO]`: the detected CPU topology.

## Open questions

- The split of game-thread time: guest code, bots, waits on the executor. This needs P-A's PROFILE.md.
- Which producer (DD80, DDB4 or DE70) is live in each mode, and whether 0x50BA70 gives a usable fixed dt for hashing.
- Whether any function called inside a tank contact scope, in a prebuilt gen file, reads the clock block. This decides whether a swap-based scope is exactly equivalent.

# Audio: APU, XAudio2 and DirectSound/XACT sync (2026-10-31, P-C)

Numbers are from `[APU-TIME]` lines in earlier runs (ewm_a/ewm_b, ew-b8 m1, wperf armA/B). No game was run.

- **Audio CPU on the the main test desktop CPU** [measured, logs]
  - GP DSP interpreter: a constant 1,527-1,640 ms per 5 s, which is 30-33% of one core (up to 36%), in menus too.
  - EP: 125-145 ms per 5 s (2.5-2.9%).
  - VP: 393-560 ms per 5 s on average (8-11%), and 986-1,904 ms at peak (20-38%) at 65-129 voices per frame (max 151).
  - That is about 2.0 µs per voice-frame. The laptop memory gives about 3 µs on an E-core, and the GP at about 54% of an E-core.
- **APU frames are GP-bound.** The DSP thread runs frame n's GP while the APU thread mixes frame n+1. The APU thread joins before each frame, so a frame takes about max(VP, GP), about 0.21 ms, until a fight passes about 100 voices. Sources: `apu_dsp_runtime.c:678` (join) and `:691` (SetEvent every frame).
- **The throttle waits only on every 8th frame** [code]. `apu_core.c:539` returns unless `ep_frame_div % 8 == 0`. So the APU runs 8 frames in a burst (about 1.7-2 ms on the desktop), then idles about 3.3 ms per 5.33 ms EP period.
- **The libsamplerate sinc is about 80% of VP time** [code + measured]. `SRC_SINC_FASTEST` (half length 2463, increment 128) uses about 38 taps per channel per output sample at ratio 1 or above. Each tap is a double coefficient interpolation plus 2 MACs. That is 1,232 taps per stereo 32-sample voice-frame, which matches the measured 2 µs.
- **`mcpx_apu_phys` is not inline** (`apu_core.c:80-89`, another translation unit). Every `voice_get_mask`/`set_mask` (about 40 per voice per frame) and every sample read in `voice_get_samples` (`apu_vp.c:1197-1416`, with a per-sample SGE page lookup) is a cross-TU call.
- **Unbounded hot spin at 0x3AA7DF** [code, new]. `recomp_0095.c:13597-13600`: after copying a DSP command block and writing the doorbell word (`ma2_register_dsp_doorbell`), the title spins with no yield until the GP program clears the word. That needs the GP's next frame, which can be a whole burst gap (about 3.3 ms). It is the last unbounded audio spin. Its call rate is unknown.
- **Every sound start waits for the VP** [code]. 0x3AEA7F and 0x3AF1D4 (`recomp_0096.c:9517`, `:11044`) wait for NEW_VOICE (PAR_STATE bit 0x100000), which `voice_get_samples` clears (`apu_vp.c:1240`) on the VP's next pass. With the burst cadence the estimated mean is about 1.2 ms and the maximum about 3.6 ms per start (unmeasured; B11.3 logs only waits of 20 ms or more). A sweep log shows 45,500-77,500 ISR voice-release attempts per In Your Face 6v6 run.
- **The XAudio2 queue sat at 3 of 4 x 1024 buffers** [measured, ewm_a 290 s]: `queued=3`, `backpressure=0`, underruns 0. So the APU loop is not blocked on the device today. Latency is about 64 ms.
- **The default VP voice_lock does not take `d->lock`.** Only `RECOMP_APU_VOICE_LOCK=frame` and the host mixer do, so `d->lock` is effectively uncontended. Voice locks run at 2,600-3,200 per second, and the title waits 0.4-0.5 ms per 5 s in them.
- **The DirectSound ISR runs on the APU thread** (`apu_core.c:143-195`), at the end of each loop iteration, with `d->lock` released. It is held off only while a thread is at IRQL 22 or above (`kernel_hal.c:145-149`).
- **The VEH APU trap calls `getenv("RECOMP_APU_TRACE")` on every trapped access**. Only recomp_0095/0096/0098 use the `ma2_store32`/`ma2_load32` fast path.
- **The exe imports `xaudio2_9.dll` statically**, so it runs on Windows 10/11 only.
- **Prior art.**
  - xemu: VP worker threads; `audio.use_dsp_jit` (default false) uses mborgerson's MIT `dsp56300` Rust/Cranelift JIT, which is hardware-differential tested.
  - Cxbx-Reloaded: DirectSound HLE on host `IDirectSound8`, with `SetEffectData`, `SetI3DL2Listener` and `SetMixBinHeadroom` stubbed. That means no DSP effects.

## Planned switches (PIPELINE-C.md; none implemented)

| Switch | Purpose |
|---|---|
| `RECOMP_AUDIO_WAIT_STATS` | `[AUDIO-WAITS]` / `[APU-WAIT]` / `[APU-VEH]` counters |
| `[Audio] ApuPacing=kick\ | smooth\ |
| `RECOMP_APU_DEVICE_CLOCK` | Throttle trimmed to the XAudio2 queue depth (±200 ppm) |
| `RECOMP_APU_VP_FAST`, `RECOMP_APU_SRC=ref\ | simd` |
| `RECOMP_APU_DSP_ENGINE=interp\ | fast\ |
| `RECOMP_APU_VP_WORKERS`, `[Audio] Threads=auto` | Order-preserving parallel VP render; one audio thread at 4 or fewer logical CPUs |

# Frame profile and render path (2026-10-31, P-A)

Profiler: branch (eace004, 820bc2b), `kit/source/game/src/pl_prof.c`; summariser. All runs were solo, through MCP `game_*`, at 3840x1600, TQ1, DrawDistance 100, uncapped, on the the main test desktop.

## Overview
- **The game thread is the frame-rate limit.** Its CPU time per frame equals the frame time:
  - UC 5v6: 15.9 ms in a 16.4 ms frame (60 fps).
  - Mission 01b copy: 9.5 of 10.2 ms (101 fps).
  - It is blocked only 0.2-1.4 % of the time.
- **The NV2A executor (`nv2a_ack_thread`) is second.**
  - Pushbuffer work: 6.8-7.7 ms per frame, plus 1.3-1.7 ms of `Sleep(0)` spin.
  - In M01b it is idle only 1.75 ms per frame. It would cap the game at about 120-130 fps even with a free game thread.
- **The executor's time goes to the per-word method loop**: 330-460 k words per frame, mostly ARRAY_ELEMENT16 index pairs. D3D11 itself is cheap: about 3 us per draw for 1,100 draws, with d3d11 plus the driver at 4 % of executor samples.
- **The GPU is not the limit on the 3090** (37 % utilisation).
- **High-level D3D8 is not where the time is.** Guest D3D is 6.7-7.3 % of the game thread, and the title is D3D8LTCG.

## Measurements

|---|---|---|---|
| fps (gameplay windows) | 60.1 | 57.8 | 101 |
| frame p50 / p99 (ms) | 16.35 / 23.9 | 16.63 / 27.0 | 10.17 / 18.3 |
| game-thread CPU per frame (ms) | 15.86 | 15.90 | 9.54 |
| executor pushbuffer work / spin / idle (ms per frame) | 7.68 / 1.64 / 6.61 | 8.06 / 1.58 / 7.02 | 6.83 / 1.39 / 1.75 |
| draws per frame, ms | 1,108, 3.37 | 1,052, 3.66 | 1,119, 3.86 |
| pushbuffer words per frame | 461,730 | 428,623 | 353,033 |

**Threads (CPU % of one core, UC 5v6):**

| Thread | CPU % |
|---|---|
| game | 95 |
| executor | 60 |
| `dsp_thread_main` | 35 |
| `mcpx_apu_frame_thread` | 14 |
| host driver worker | 10 |
| `kernel_timer_thread` | 2 |
| `ohci_thread` | 1.7 |
| `fb_thread` | 1.1 |

**Game thread by category (samples):**

| Category | % |
|---|---|
| guest .text | 75-80 |
| guest D3D | 6.7-7.3 |
| ntdll | 2.3-6.8 |
| recomp helpers (`recomp_lookup`) | 4 |
| native bots | 3 |
| DSOUND/XACT | 1.4-2.1 |

## Planned switches (PIPELINE-A.md; none implemented)
- `[Render] PbBulk` / `RECOMP_PB_BULK`: bulk 0x1800/0x1808/0x1818 payloads in `nv2a_pb_consume` (E1).
- `[Render] ExecWake` / `RECOMP_NV2A_WAKE` (spin, poll, event, auto): no `Sleep(0)`; KickOff event wake (E2).
- `[Render] DrawFast` / `RECOMP_DRAW_FAST`: constant dirtiness, CPU vertex shader only when needed, one constant Map, dirty-page vertex uploads (E3).
- `[Render] ExecSplit` / `RECOMP_EXEC_SPLIT`: decode plus submit threads; reserve, gated on 6+ P-cores (E4).
- `[Render] FrameLatency` / `RECOMP_FRAME_LATENCY`: waitable swap chain (E5).
- Dev only: `RECOMP_PROF`, `RECOMP_PROF_HZ`, `RECOMP_PROF_AFFINITY`.

## Open questions
- Which objects the game thread waits on through `KeWaitForSingleObject` (3.2 % in UC), and which lock `bridge_RtlLeaveCriticalSection` releases with waiters (2.7 % in M01b)?
- What the recomp_0066.c float cluster does (`sub_0027B8C0`, `sub_0027C930`, `sub_0027E740`, 9-11 % of the game thread).
- The executor's D3D11 share on AMD and Intel drivers (NVIDIA does part of the work on its own driver thread).
- The guest KickOff store site for DMA_PUT (0xFD800040), needed by E2b.

## M0 shared plumbing (0517065, 2026-10-31)

M0 is the commit both pipeline tracks branch . It changes no behaviour. It moves the guest audio helpers to a file the audio track owns, and adds the switch reader and the CPU-topology API that both tracks call.

| File | What it holds | Owner |
|---|---|---|
| `kit/source/game/src/apu/ma2_audio_guest.inc` | Verbatim move of the API lock 0x3C4598, the ISR try-enter, the stop assist and the B11 bounded waits. recomp_manual.c #includes it at the two old places (`MA2_AUDIO_GUEST_PART` 1 and 2). The object is byte-identical apart from its timestamp. | T2 |
| `kit/source/game/src/platform/pc_switch.h` | Header-only `pc_switch` / `pc_switch_choice` / `pc_switch_text`, read as env > ini > default. The ini step is a selectany pointer that main.c `configure_pc` sets. | T1 |
| `kit/source/game/src/platform/pc_cpu_topo.h/.c` | The `[CPU-TOPO]` detection (CPU Sets under the mask, P/E, LLC, battery, tier), `pc_cpu_logical`, and `pc_thread_role_enter/leave`. Library code reaches it through the selectany `pc_cpu_topo_api`. The .c is #included by main.c. | T1 |
| `kit/source/game/src/main.c` | `RECOMP_AFFINITY=<hex>` (test only) and the `[CPU-TOPO]` line at the top of WinMain, plus the ini hook for pc_switch. | T1 |

- **Include spelling (kit hazard):** use `"platform/pc_switch.h"` from `kit/source/game/src/*.c`, and `"./platform/pc_switch.h"` from `kit/source/game/src/kernel` and `kit/source/game/src/apu`. The kit moves those folders to `runtime\src`, and the kernel target cannot see `game\src`.
- **Test masks on the main test desktop:** 55 = 4 P-cores without SMT, FF = 4c/8t, 0F = 2c/4t, 5555 = 8c/8t, FF0000 = the 8 E-cores (logged as `pclass=0/1`), 3 = below the floor.
- **Baseline (UC 5v6, 3840x1600, TQ1, DrawDistance 100, uncapped, after 60 s):**
  - FULL: 71.2 fps, p50 13.9 ms, p99 20.0 ms.
  - Mask 55: 75.6 fps, p50 13.1 ms, p99 19.3 ms.
  - Both runs passed the crash, audio, clock and bot gates.

## Track 1: bring-ins (T1-0.0, cf4af24, 2026-10-31)
- **The prebuilt gen objects use the scoped accessor (corrects spec §0.2).**
  - The shipped recomp_00xx.c.obj files for the non-overlay gen files were built ; the obj path is embedded in each object.
  - They use the same scoped `XBOX_PTR` (`ma2_tank_scoped_pointer`, ma2_tank_step_scope.h) as the bots overlay's recomp_types.h.
  - Compiled with the plain accessor, they would shrink by 36-55 %: 0066 1,080 → 673 KB, 0101 52.3 → 23.8 MB.
  - So the tank-scope code bloat covers all guest code, and O1 (plain pointer) applies to every hot file.
- **Bring-in lineage.**
  - W3K `runtime\src\kernel` kernel_bridge.c and kernel_file.c, and the overlay's kernel_thread.c, match the xbox_kernel.lib members.
  - The compile rule is. The objcmp tool is.
- **Hot file shares** (sums of PROFILE's listed guest functions, uc5v6 / m01b sample %): 0066 10.6 / 7.8, 0068 4.7 / 3.0, 0014 4.3 / -, 0103 3.7 / 3.9, 0101 1.8 / 2.1, 0008 1.6 / 1.5, 0006 1.6 / 0.9, 0035 1.0 / 0.9, 0064 0.7 / 1.3, 0015 0.5 / 1.2, 0017 - / 0.8.
  - 0103 is the guest D3D8 pushbuffer writer range.

## Track 2 (T2-0.T2-2)

T2-0 (measurement and bring-ins) passed its gates. T2-1 (bulk pushbuffer words) and T2-2 (executor wake) are in the branch with their switches default OFF; neither flip is done yet.

- **Bulk words.** RECOMP_PB_BULK / [Render] PbBulk (nv2a_pb_scan.c bulk branch + nv2a_pb_exec.c nv2a_pb_exec_bulk) decodes whole non-incrementing 0x1800/0x1808/0x1818 payloads in one call; 2.48 M verified packets (UC + M01b), 0 mismatches; executor loop 8.95 -> 3.79 ns/word.
- **Words per frame vary.** UC 5v6 matches range 140-450 k pushbuffer words per frame between 5 s windows and runs (t2r0 median 363 k, t2b1 179 k); compare executor cost per word, not per frame.
- **Heavy-frame word mix.** UC 5v6 heavy frame: idx16 178 k, const/program 104 k (already bulk), inline 39 k, state 24 k, immvtx 6 k, tex 5 k words; 872 draws; 4.3 kicks.
- **Executor wake.** RECOMP_NV2A_WAKE=sleep0|poll|auto ([Render] ExecWake, RECOMP_NV2A_ACTIVE_US) in xbox_memory_layout.c nv2a_idle_wait; poll+timer cuts executor spin from 1.67 to about 0.1 ms/frame, but at 4 logical CPUs the 250 us timer raised UC p99 +9 % and >33 ms frames 18 vs 4 (one run); default stays sleep0.
- **No game-thread audio waits.** with RECOMP_AUDIO_WAIT_STATS, a 5 min UC 5v6 fight showed 0 NEW_VOICE, 0 DSP-CMD (0x3AA7DF), 0 XACT packet waits on the game thread; the APU thread sleeps in its throttle 3.3 s per 5 s and waits on the DSP join 1.2 s per 5 s.
- **Resampler ratio.** [APU-SRC] in UC 5v6: about 67 % of voice-frames resample with ratio >1, 33 % at ratio 1, <1 % ramping, none <1.
- **GP program static.** [APU-DSP] GP PRAM sampled every 8th APU frame showed 0 changed words during a fight (GP program static after load), which favours the T2-7 AOT hash gate.
- **Capture under silent.** RECOMP_AUDIO_CAPTURE writes the post-gain PCM in mcpx_apu_monitor_frame before RECOMP_AUDIO_SILENT zeroes the device buffer, so captures work in muted MCP runs.
- **Guest CS contention is ~0 in UC 5v6 too.** [PERF-RATE] at FULL: 3 contended RtlEnterCriticalSection/s, 0.0 ms/s blocked; at 4T (mask 55) with spin 200: 2/s, 0.0 ms/s; all on 0x3C4598. The CS thunk share is uncontended Enter/Leave (ord 294/277 at 41-48 k / 30-35 k per s), so a CS spin (RECOMP_CS_SPIN, default 0) gains nothing. Evidence: t1p2_stderr.txt.
- **ICALL lookup path (T1-2 input).** RECOMP_ICALL (recomp_types.h:1061) calls recomp_lookup_manual BEFORE recomp_lookup. g_flat_table is `static` in recomp_dispatch.c, and recomp_dispatch.c / recomp_bringup.c are prebuilt, so an inline flat probe (O2b) needs recomp_dispatch.c brought in to export the table.
- **Tank-step scope and prebuilt code (T1-2 input).** 54 of 104 gen files are still prebuilt objects that inline ma2_tank_scoped_pointer (reads TLS ma2_tank_step_scope.active, offset 0). A plain-pointer + 48-byte swap design must therefore keep `active`=0 while swapped (so prebuilt scoped code reads guest memory) and move the "inside a tank step" logic flag (ma2_tank_contact_begin re-entry guard, ma2_spring_rate.h:45 in the game source tree recomp_0038.c) to a NEW TLS symbol; never retype ma2_tank_step_scope.
- **B4.3 volume query.** MA2 issues FileFsSizeInformation in normal play; with B4.3 the root is the handle's volume (logged once as `[FILE] volume size query: root=C:\ .`).

## Track 2
- **t2-tex-row-decode**: [Render] TexRowDecode default 1 (e128b35): texture uploads decode a row at a time (nv2a_texture_decode_row, the pack's row decoder with canonical 0); UC 4T worst texture_prepare 21.16 -> 7.70 ms, total -70 %; mode 2 verified 4.67 M texels, 0 mismatches.
- **t2-recovery-hidden**: [GPU-RECOVERY] healthy is printed only after presentations with IsWindowVisible true, so hidden MCP runs stay in VERIFYING after a RECOMP_GPU_RECOVERY_TEST recreation (rendering continues; the line needs a visible window).
- **t2-tq-schedule**: RECOMP_PACK_TQ_SCHEDULE="sec:q," (nv2a_texture_pack.cpp) switches texture quality at those seconds, a TQ-switch test without the settings menu.
- **t2-cache-dir**: RECOMP_SHADER_CACHE_DIR=<dir> (nv2a_shader_cache.c) replaces %LOCALAPPDATA%\xboxrecomp\shader-cache for a cold-cache run; the MCP launcher passes only RECOMP_*.
- **t2-draw-cost-split**: UC FULL heavy scene: all D3D11 Maps 0.59 ms/f (2,834 Maps), guest vertex SRV uploads 0.034 ms/f, draw 2.9 us per draw; E3 items (c)-(f) together are worth under 0.4 ms of executor CPU per frame.
- **t2-bulk-on**: [Render] PbBulk default 1 since 4f41a53; same-exe 4T FL144 A/B (t2w3/t2w4) executor loop 4.6 vs 11.4 ns/word.
- **t2-wake-auto**: [Render] ExecWake default auto since 388ce5f; auto = sleep0 below 12 logical CPUs; at 24 logical spin 1.67 -> 0.19 ms/f with a 100 us in-window timer tail (RECOMP_NV2A_WINDOW_US); out of the 2 ms traffic window both modes always used the 250 us timer.
- **t2-trap-noise**: UC 5v6 kind-2 BOT-TRAPPED "bailing out" repeats at one spot also occur in feature-OFF runs (s1-runs/off1, s2runs/uc-off2, s1-runs/on2); executor changes cannot cause them.
- **t2-seed-first**: nv2a_shader_cache.c checks the mapped shader-seed.pack before the user cache and no longer copies seed hits into it (B2.4); M01b program_miss worst frame 7.35 -> 0.36 ms.

## Track 2 (T2-6, T2-8, T2-7 step 1)
- **t2-src-fast**: RECOMP_APU_SRC default auto since 9cd0f04: bit-exact fast stereo sinc (SSE2) in the libsamplerate overlay; harness and 662 k in-game calls 0 mismatches; VP -35 % per voice-frame at 4T (2.19 -> 1.43 us).
- **t2-4t-audio-contention**: At 4 logical CPUs (RECOMP_AFFINITY=55) with both audio threads at MMCSS Pro Audio HIGH (priority 25), the game thread is runnable without a CPU about 3 % of the time, and the DSP and VP threads were running in about 70 % of those samples (t2p1).
- **t2-thsamp**: (pwsh Process.Threads snapshots at ~32 Hz, run on E-cores outside the game mask) plus thsum.py give per-thread running/READY shares and priorities without ETW.
- **t2-thread-plan**: [Audio] Threads / RECOMP_APU_THREADS=auto|high|low|inline (apu_dsp_runtime.c, T2-8 e8e857a, default auto): auto = low at <=4 logical CPUs (pc_cpu_logical), high above. low runs the GP/EP DSP thread at NORMAL priority with boosts off instead of MMCSS HIGH; a lag guard promotes it back while the APU is >2 EP blocks behind ([APU-THREADS] promotions=). The APU/VP thread stays MMCSS HIGH. Logged as [APU] threads: cpus= plan=.
- **t2-thread-plan-result**: UC 5v6 4T FL144: plan low cut game-thread READY share from 2.4-3.0 % to 1.1-1.25 % and load-binned p99 by about 1.9 ms (150-250 k words/f 19.92 -> 18.02 ms; 250-360 k 23.52 -> 21.58 ms); 0 XA2 underruns, APU-TIME 100 %, 0 clipped runs, 0 guard promotions in 137 windows; DSP join max rose to 5-10 ms (buffer 64 ms).
- **t2-dsp-programs**: MA2 GP runs two program images: a 371-word boot image then the main 2,633-word image (FNV hash 76c42efa9fbcaacd), static during play; the EP PRAM changes every second (44 distinct hashes in 330 s), so it holds data and an EP AOT needs a code-region hash, not a whole-PRAM hash.
- **t2-dsp-dump**: RECOMP_APU_DSP_DUMP=<dir> (apu_dsp_runtime.c dsp_program_dump, 08a9326) writes each distinct GP/EP PRAM image (DSP_PRAM_SIZE u32 words) on the first idle frame after a reset and every 1500th frame, logging [APU-DSP-DUMP].
- **t2-dsp-trace-free**: dsp_shim.h defines trace_event_get_state as false, so the per-instruction trace/disassembly check in dsp_cpu.c is already compiled out; the GP costs about 42.5 k cycles (about 21 k instructions) per frame, about 10 ns per instruction, so interpreter tuning (A3a) has little left and the real gain needs AOT with constant-folded decode (A3b).

## Pipeline merge (364d737, 2026-10-31, installed)
- **-shas**: = 0517065 + `--no-ff` d7faff2 (5e22148) + 08a9326 (364d737), no conflicts. Build dir build- (copy of build- + -rules.py + -rules.py). Exe sha256 7E43425225DF7ECD (130,263,040 bytes), map 7F89992820F8C80D. Installed to PC Release over 51A945F9.
- **-topo-api-abi**: T1 added `cpu_note` to the selectany PcCpuTopoApi (pc_cpu_topo.h), so every T2 object that includes the header (apu_core.c, apu_dsp_runtime.c, nv2a_*.c, xbox_memory_layout.c) gets .bss +8 in the merge build; never link track-built kernel/apu objects with the other track's main.c. All gen and fast objects are section-identical to 's (154), T2's own 6 to 's.
- **-hash**: M01b fixed dt 0.016666667: [STATE-RAW] minus 0x4E2438 equal to t16v1 in every segment at FULL and at 4T (RECOMP_AFFINITY=55); 4T sthcmp vs t16v1 frames 0-8000: RAM 161/161,data only the A==A noise block 0x4B2000. Track 2's executor/audio/texture changes do not touch game state.
- **-m01b-gain**: M01b FULL draw-normalised frame time at 1,100 draws/frame: base t1v1 9.96 ms, t16v1 6.10, 6.12 (-39 %); window p99 median 19.45 -> 15.62 ms; 4T 6.29 ms vs t14a4 8.41.
- **-uc-gain**: UC 5v6 skill 4: FULL uncapped 71.2 -> 141.1 fps mean, p50 13.94 -> 6.86 ms, p99 19.97 -> 11.99 ms (plm0b -> plme); 4T uncapped 75.6 -> 127.0 fps, p99 19.31 -> 13.32 ms (plm0a -> plmd); FL144 FULL 132.4 fps mean (112-144), 10 % of 5 s windows at >= 142 fps, p99 12.80 ms (ew-merge run a: 67.9 fps, p99 21.6).
- **-recovery-test**: bringup/regression/gpu_recovery_policy_test.c is now the macro-relative copy; 44/44 PASS against the merged nv2a_gpu_recovery.h. Old file kept as

