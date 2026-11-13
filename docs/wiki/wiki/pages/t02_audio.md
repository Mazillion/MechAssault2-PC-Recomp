# Audio: APU emulation, DSound, XACT, mixing

Pack t02_audio. Facts: `W\facts\t02_audio.json` . Short names in citations are relative to
`kit/source/game/src/apu/` (the VP, DSP and state sources) unless a full path is shown.
Line numbers are from the files as read on 2026-10-21.

## Overview

The title's DirectSound writes voice records and registers into the APU register range (0xFE800000+). The
voice processor (VP, `apu_vp.c`) runs once per audio frame of 32 samples (667 us at 48 kHz). It walks three
circular voice lists (2D, 3D and MP, `apu_vp.c:81`) and mixes each active voice into one of 32 mixbins. A
voice reads its sample through a libsamplerate sinc converter, runs an optional state-variable low-pass and an
optional HRTF filter, and adds `gain * sample` into its routed mixbins (`voice_process`, `voice_mix_block`).

After the VP, `apu_dsp_runtime.c` runs the title's own GP DSP program (DSP56300 interpreter) every frame and its
EP program every eighth frame. The GP and EP run on their own thread, one frame behind the voices, unless
`RECOMP_APU_DSP_THREAD=0` or the pipeline capture is set. The EP output is read at 24 bits, restored by the
headroom gain and passed through a soft limiter, then converted to int16 and sent to XAudio2 (4 x 1024 buffers,
). A host DirectSound layer in `kit/source/game/src/audio` is not on this path.

The 3D sounds are a two-stage path. A source voice writes bin 31 and a multipass voice on the MP list reads
it back out into bins 6 and 7 (the XTLK front submixes). Those bins carry most of the battle's energy and were
the main source of clipping. Four failure families were found and fixed in 2026-10 (details below): the
clipping (fixed by DSP headroom), the voice tail discarded at the end of each sound (fixed by an owned tail
drain), the idle voice notification that stalled DirectSound (fixed by sending one notification per frame), and
the XACT lock-order freezes (fixed by taking the DirectSound lock at the raised IRQL, with list guards as a
safety net).
for controlled tests.

## Key code

| Address or function | What it does | Source |
| --- | --- | --- |
| `mcpx_apu_vp_frame` | Per-frame VP: owned tails, then the 2D/3D/MP lists, idle notification, VP monitor | apu_vp.c:1847 |
| `voice_process` | Per-voice: envelope, sample, SVF, HRTF, gain per bin, mix, tail snapshot | apu_vp.c:1626 |
| `voice_mix_block` | Adds gain*sample into mixbins; HRTF slots 0-3 use ear `b>>1` | apu_vp.c:1505 |
| `voice_resample_callback` | Feeds libsamplerate; returns zero after padding to end input | apu_vp.c:1409 |
| `voice_resample` | Clamps pitch ratio (below 1/8 held at 1/8) and calls `src_callback_read` | apu_vp.c:1446 |
| `voice_off` | Clears the active bit and raises the DONE notifier | apu_vp.c:261 |
| `voice_lock` | VOICE_LOCK as an interlocked handshake on the one voice | apu_vp.c:317 |
| `set_hrir_coeff_tar` | Decodes HRIR taps as sign-magnitude (experimental) | apu_vp.c:373 |
| `voice_tail_budget` | Guard length for an owned tail from SRC support and SVF pole | apu_vp.c:1553 |
| `mcpx_apu_dsp_native_frame` | Hand-off to GP/EP; joins the DSP thread; runs EP every 8th frame | apu_dsp_runtime.c:664 |
| `dsp_run_frame` | Scales mixbins by 0.25 (headroom), `float_to_24b`, runs GP and EP | apu_dsp_runtime.c:581 |
| `headroom_ep_block` | Reads EP output at 24 bits via `mem_read`, checks against DMA 16-bit words | apu_dsp_runtime.c:145 |
| `headroom_output` | Restores x4 gain, 32-sample lookahead limiter, 0.99 ceiling | apu_dsp_runtime.c:103 |
| `bus_limit` | Opt-in pre-GP limiter (0.98 ceiling), off by default | apu_dsp_runtime.c:511 |
| `apu_time_frame` | `[APU-TIME]` line when RECOMP_PERF or RECOMP_AUDIO_STATS is set | apu_dsp_runtime.c:559 |
| `float_to_24b` | Saturates at plus or minus full scale (1.0) | fpconv.h:56 |
| `hrtf_filter_set_target_params` | Normalises each ear's taps to L1 = 1 (software policy) | apu_state.h:126 |
| `apu_hrtf_enabled` | Reads RECOMP_APU_HRTF once; default false | apu_shim.h:335 |
| `sub_003DEA53` (XACT per-frame update) | Walks three XACT LIST_ENTRY lists (+0x44, +0x64, +0x5C) | recomp_0097.c (see functions.json, cat game_audio) |
| `sub_003E708C` / `sub_003E716F` | XACT timer min-heap compare and pop | recomp_0098.c |
| `sub_003E4A20` | Walks a bank cue list (+0x14) under the XACT lock | recomp_0098.c |
| `sub_003DDE80` | XACT critical-section entry (lock 0x3E869C); skipped at raised IRQL | recomp_0097.c |
| `sub_003DDEAD` | XACT CIrql raise (35 callers) | recomp_0097.c |
| `sub_003DDEDC` | XACT DoWork; takes the DirectSound CS | recomp_0097.c |
| `sub_003DEE20` | XACT notification queue insert | recomp_0097.c |
| 0x3E8059 | XACT timer DPC | recomp_0098.c |
| 0x3C4598 | DirectSound API critical section (`ma2_xact_raise_lock`) | memory note, xact-freeze |
| 0x4259DC | KfLowerIrql import thunk (kernel ordinal 161) | patch_sp0 (SP0 file:56) |
| `loc_003AD48A` | DirectSound voice-stop wait; times out after 0.5 s | recomp_0096.c (REPORT:361) |
| `sub_003AAEE1` | APU interrupt voice release (try-lock; skipped while the lock is held, per the SP0 note) | recomp_0095.c |
| `sub_003A65D0`, `sub_003AC7E7` | DSound pool free; sbb/neg branch bug sites | recomp_0094 / 0096 |

Structures: `MCPXAPUVoiceFilter` (apu_state.h:286) holds the SRC converter, SVF pair, HRTF state, and the owned
tail fields (`drain_blocks`, `drain_gain`, `drain_bin`, `drain_svf`, `drain_hrtf_state`, `ended_by_data`,
apu_state.h:296-308). The three voice lists are in `voice_list_regs` (apu_vp.c:81). Register constants are in
apu_regs.h: 256 voices (line 330), 64 3D voices (331), 32 samples per frame (333), 32 mixbins (334), multipass
bin 31 (358), `SE2FE_IDLE_VOICE` 0x8000 (216).

## Added code and hooks

- **Owned tail drain** (apu_vp.c:1809-1831, 1859-1897): snapshots gains, bins, SVF and HRTF at data end, then
resamples 32 samples per frame until EOF and quiet. Cancelled by VOICE_ON, OFF, PAUSE and RELEASE.
- **Silent tail skip** (apu_vp.c:1811-1821): cancels a tail whose frozen gains are all zero.
- **Idle-voice notification** (apu_vp.c:1935-1952): one `SE2FE_IDLE_VOICE` per frame, for the longest waiter.
- **Multipass read** (apu_vp.c:1679-1692): copies `mixbins[mp_bin]` and optionally clears it.
- **HRTF stereo output** (apu_vp.c:1790-1791): HRTF sets channels to 2 after filtering.
- **DSP headroom** (apu_dsp_runtime.c:82-84 constants, 89 switch, 103 limiter, 145 EP read, 586 GP input).
- **Opt-in bus limiter** (apu_dsp_runtime.c:487-511). `apu_limiter.h` (apu_limit_block, 0.92 ceiling) is
included by apu_state.h:277 but never called in this tree.
- **Stall profiler** (apu_dsp_runtime.c:268-390): watcher thread and sampler, started by RECOMP_STALL_PROFILE.
- **Pipeline capture** (apu_dsp_runtime.c:437-498): writes mixbins and GP streams for offline analysis.
- **Voice lock handshake** (apu_vp.c:287-341) replaces the frame-long `d->lock` wait.
- **Telemetry**: `[APU-TAIL]` (apu_vp.c:36-63), `[APU-MP]` (apu_vp.c:1575-1618), `[APU-MIX]` and
`[APU-GAIN]` (apu_vp.c:402-563), `[APU-HEADROOM]` (apu_dsp_runtime.c:132), `[APU-TIME]` (apu_dsp_runtime.c:559).
- **XACT guards** (patch scripts, not in the source tree): `patch_xact_list_guard.py` (4096-step relink, logs
`[XACT-LIST]`), `patch_xact_heap_guard.py` (heap entries, `[XACT-HEAP]`, in the game source tree),
`patch_sp0_xact_update_lower_unlock.py` (audit patch, see open questions).

## Env vars, settings and test hooks

Read from `envvars.json`, the source and the notes. Launchers strip inherited `RECOMP_` variables, so set
each one in the final game process environment (`capture-launcher-env` fact).

| Variable | Effect | Default | Source |
| --- | --- | --- | --- |
| `RECOMP_APU_DSP_HEADROOM` | 0 restores the original GP/EP path (no input scaling, 16-bit EP read) | on | apu_dsp_runtime.c:89-92 |
| `RECOMP_APU_LIMITER` | Non-zero enables pre-GP bus limiter (0.98 ceiling) | off | apu_dsp_runtime.c:504 |
| `RECOMP_APU_HRTF` | Non-zero enables HRTF for 3D voices; 0 forces the bypass | off | apu_shim.h:339 |
| `RECOMP_APU_GAIN_TRACE` | At most 96 lines: HRTF headroom, submix headroom, HRIR taps | off | apu_vp.c:407 |
| `RECOMP_APU_MIX_TRACE` | Active post-filter route, HRIR and summary logs (32 routes, 48 HRIR, 120 summaries) | off | apu_vp.c:432 |
| `RECOMP_APU_MP_TRACE` | Writer counts per multipass read of bin 31; `[APU-MP]` | off | apu_vp.c:1588 |
| `RECOMP_AUDIO_TAIL_STATS` | `[APU-TAIL]` every 5 s with over667 count | off | apu_vp.c:39 |
| `RECOMP_AUDIO_STATS` | XA2 underruns, `[AUDIO]` peaks, `[APU-DSP]`, `[APU-HEADROOM]` | off | apu_dsp_runtime.c:132, 543, 697 |
| `RECOMP_PERF` | `[APU-TIME]` and voice-lock timing | off | apu_dsp_runtime.c:562; apu_vp.c:301 |
| `RECOMP_APU_STREAM_STATS` / `_TRACE=<voice>` | Stream gap stats and per-voice `[APU-SSL]` trace | off | apu_vp.c:196, 242 |
| `RECOMP_APU_STACK_BA=<hex>` | Print the game thread's stack when a voice starts on that buffer | unset | apu_vp.c:675 |
| `RECOMP_APU_VOICE_LOCK=frame` | Restore the old frame-long voice lock (A/B) | handshake | apu_vp.c:294 |
| `RECOMP_APU_DSP=0` | Disable GP and EP | on | apu_dsp_runtime.c:15 |
| `RECOMP_APU_DSP_THREAD=0` | DSP runs inline, not on its own thread | thread | apu_dsp_runtime.c:652 |
| `RECOMP_APU_PIPELINE_CAPTURE=<prefix>` (+ `_START` seconds, default 45; `_SECONDS`, default 30) | Writes mixbin, GP and state files; forces inline DSP | off | apu_dsp_runtime.c:443-446 |
| `RECOMP_STALL_PROFILE=1` (or N) | Sampling profiler for stalls (`[STALL]`) | off | apu_dsp_runtime.c:668, 273 |
| `RECOMP_AUDIO_CAPTURE=<wav path>` | Records the final PCM to a wav (the kit sets it per run) | off | capture READMEs |
| `RECOMP_AUDIO_ISR_STATS` | DirectSound ISR and DPC statistics | off | recomp_manual.c:104 |
| `RECOMP_AUDIO_STOP_ASSIST` | DirectSound stop assist; `0` disables | on | recomp_manual.c:133 |
| `RECOMP_AUDIO_SILENT=1` | Silences this process's device submissions | off | ma2_scenario_audio.cpp:84 |
| `RECOMP_AUDIO_VOLUME` (0-100) | Audio Volume (applied after the EP) | 100 | pc_native_settings.cpp:690; main.c:200 |

Test hooks and harnesses:

- `tests\apu_mixdown` (CTest `apu_mixdown_all`, `apu_mixdown_two_bins`): the GP/EP mixdown of bins 2 to 31.
`controls.c`, `tail_bypass_parity.py`) drive the real `apu_vp.c` through public PIO and frame entry points
with a real libsamplerate.
- `Start-Playthrough.cmd`, `Try-HRTF-Audio.cmd`, `Try-Audio-Limiter.cmd`, `Try-Audio-No-Limiter.cmd` and
`Try-Tank-Physics*.cmd` (capture READMEs) resume the same profile with one audio setting changed.
- `analyze_pipeline_capture.py` and the `analysis\*.py` scripts analyse captures (REPORT:118, 91).

## Bugs and fixes (history)

1. **One-shot tail discarded (click at the end of each sound).** When data ran out, `voice_off` was called
mid-block and `voice_process` returned before mixing the last 32 samples; the converter's buffered output was
lost. At 44.1 kHz the output stopped at sample 2335 with a 0.442 step (REPORT:12-25). Fixed by the owned
tail drain (apu_vp.c:1809-1897). The first patch (12 drain blocks) was superseded: 12 blocks are too short at
low pitch (0.80 residual at 4 kHz), it bypassed SVF and HRTF, and a guest OFF did not stop it (W3 review,
The full-tail candidate passed 1,784 focused assertions and 32
waveform comparisons (max error 1.32e-7), and the bounded native gate on Mission01b passed.
Status: current; the crackle is not proven fixed by this gate.
2. **Idle voice stall (0.5 s hitch per voice).** Every inactive listed voice raised `SE2FE_IDLE_VOICE` in one
walk, and each overwrote FEDECPARAM, so the DirectSound stop wait timed out at 0.5 s per voice (REPORT:361-375).
Fixed: one notification per frame for the longest waiter (apu_vp.c:1935-1952). Verified: no `[VOICE-TIMEOUT]`
in run 201811; `idle_max_wait_frames` 2 to 3 (REPORT:385).
3. **Clipping, layer 1: mixbins overflow before the GP.** Bins 6 and 7 peaked at 2.965 with 82,753 samples each
clamped by `float_to_24b` (REPORT:120-130). The 3D multipass voices sum to up to 52 visits per frame at unity
The clamp at the GP input is the audible crackle (REPORT:329).
4. **Clipping, layer 2: EP 24-bit sum saturates.** 14% of output samples were flat at full scale in run 193044
(0:47-0:50), with 44,674 full-scale samples in total (REPORT:315-318). The DMA reduces EP words to 16 bits with
`v >> 8` and does not clamp. PC volume is applied after the EP, so lowering it never helped (REPORT:326).
**Fix (default on):** GP input x 0.25 (apu_dsp_runtime.c:586), 24-bit EP read validated against the DMA
words (145-168), x4 restore and 0.99-ceiling soft limiter (103-143).
0 full-scale samples, no audible glitch (REPORT:342-346). Status: current.
heard muddy pickups and pole hits (REPORT:278-282). The muddiness was traced to the HRTF filter that was on
at the time. Status: historical; `RECOMP_APU_LIMITER=1` only.
6. **HRTF (rejected as default, corrected build for tests).** Four real defects were found: taps decoded as
two's complement instead of sign-magnitude (apu_vp.c:385); the left ear sent to both bins 6 and 7 by
`b % channels` (apu_vp.c:1517); mono channel count dropped the right ear (apu_vp.c:1791); the normalization loop
skipped the right ear on `break` (apu_state.h:137). The filter still normalises each ear to L1 = 1 (apu_state.h:126-140),
which is a software policy: the corrected tables average -14 dB at 4 to 12 kHz, so hardware HRIR gain is unknown.
A corrected three-file candidate was accepted for a controlled listening build;
default off (apu_shim.h:328).
7. **XACT list spin (freeze after a truck explosion, run 2026-10-11).** The main thread stayed in
`sub_003DEA53` taking and releasing locks; one of its three lists no longer led back to the head (memory note,
xact-freeze). Mitigation: `patch_xact_list_guard.py` relinks after 4096 steps and logs `[XACT-LIST]`. Root cause open.
8. **XACT timer heap crash (2026-10-13).** AV in `sub_003E708C` on a garbage node pointer during the pop in
`sub_003E716F`, about one run in hundreds. Guard `patch_xact_heap_guard.py` drops non-guest entries and logs `[XACT-HEAP]` (documented, not verified in a run).
9. **XACT cue-list spin (2026-10-14, run s11m4).** `sub_003E4A20` walked a bank cue list under the XACT lock.
The same guard was added to recomp_0098.c.
10. **XACT IRQL vs DirectSound DPC race (root cause of 7 and 9, 2026-10-14).** A raised IRQL does not stop the
DirectSound DPC, so the lock order was broken. Fixed by taking DirectSound's API lock 0x3C4598 in the XACT timer
DPC (0x3E8059) and in every raise below DISPATCH, and releasing it after each `KfLowerIrql` through
`ma2_xact_lower_unlock` (memory note, xact-freeze). A separate XACT lock deadlocked (run s15a). Result: 27
matches with no `XACT-LIST`, `XACT-HEAP` or IRQL events.
11. **Recompiler sbb/neg bug.** `je/jne` after `sbb reg,reg; neg reg` tested the wrong value at three sites
(sub_003A65D0, sub_003AC7E7, sub_003DDEAD). All fixed.
12. **Voice lock convoy.** `voice_lock` took `d->lock`, held by the APU thread for each 0.67 ms frame, so each
title lock waited for the frame (laptop runs at 20-25 fps). Fixed by the interlocked handshake (apu_vp.c:287-341).
13. **Pitch ratio freeze.** Ratios near 1/64 cost seconds per frame while holding `d->lock`. Held at 1/8 with
`[APU-RATE]` (apu_vp.c:1468-1471). Also the converter is reset once its input has ended (apu_vp.c:1478).
14. **Starvation ruled out.** Across 38 windows the XAudio2 underruns were 0 and VP work was at most 464 us per 667 us frame (REPORT:107-112).
The host DirectSound mixer was not used (0 "Play voice" lines, REPORT:284-286).

Cross-pack freezes found in the same audio notes (facts tagged cross-pack): the kernel critical-section table
filled at 4096 slots and left locks unprotected (kernel_rtl.c, REPORT:442-456); the multiplayer-exit hang came
from a closed-socket receive reaching a D3D flip spin (recomp_0051.c `sub_001ED780`, REPORT:514-528; the first
fix returned 0 and corrupted the stack, corrected to 0x80072736 with `esp += 8`); heap fragmentation was
relieved by best-fit and stack reclaim (REPORT:488-505).

**Added 2026-11 (pack t28, all in PC Release par24 unless stated).**

- **Missing voice lines.** One-packet XACT streams died because their sync wave-bank read completed at once; XwbDefer completes wave bank reads 4 ms later (commit c036eb4; `t28.xact-one-packet-stream-xwbdefer`). A full read queue now logs `[XWB-DEFER] queue full` once (r26 967efdc).
- **DSound crash and freeze guards.** `RECOMP_AUDIO_DEFER_GUARD` (0163bba, `t25.dsound-defer-double-insert`, `t28.audio-defer-guard-env`) and `RECOMP_AUDIO_ACTIVE_GUARD` (5a2b21c, `t26.dsound-active-reinsert-freeze`, `t28.audio-active-guard-env`) stop the active-list re-insert that froze the game during BattleArmor death blasts. `[CS-WAIT]` lines name the thread that holds a critical section during a hang (`t28.cs-wait-hang-sampler`).
- **Focus loss.** The output ramps to 0 while another window has focus (`t28.focus-loss-pause-mute`, r26, not yet published).

## Open questions

- **Cue IDs.** This pack does not map MAActionSound `CueEvent` to sound banks or cue IDs. The catalog
(class `MAActionSound`) says CueEvent works for imported clips (verified 2026-10-20). Needs its own pack.
- **Hardware HRIR gain and sign-magnitude.** The sign-magnitude decode is an inference from smoothness (89 of
96 responses). The absolute gain of the title's taps and the normalization policy are not verified against hardware.
- **Multipass semantics.** Whether the title's `CLEAR_MIX` and multipass pairing match hardware is open (REPORT:298-313).
- **High-overlap tail cost.** 128 audible tails with 128 active voices measured 555-812 us median (SSE2 vs
full-tail), above the 667 us frame. Whether real fights reach this load is not measured (
- **Tail guard expiry.** Any `[APU] Tail guard exceeded` line must block acceptance. None was seen in the bounded gate.
- **Clicks in quiet passages.** Isolated steps remain (click detector about 744 per run).
them, so they were not pursued (REPORT:346-350).
- **sp0 patch.** `patch_sp0_xact_update_lower_unlock.py` appends `ma2_xact_lower_unlock` after the two
KfLowerIrql icalls in `sub_003DEA53`. Whether it was applied to recomp_0097.c is not recorded here (SP0 file:1-60).
- **XACT list corruption.** The writer of the bad list node is unknown. Suspects are a concurrent DPC and game-thread
update, and XACT/DSound objects in reclaimed heap memory (memory note, xact-freeze).
- **Stale docs.** `kit/source/game/src/apu/README.md` (GP/EP stubbed, waveOut) and `kit/source/game/src/audio/README.md` (DirectSound over waveOut)
describe older code (REPORT:9-10). Do not use them for the live pipeline.

