# Runtime, kernel and stability: memory map, threads, freezes and crashes

Pack `t07_stability`. Facts are in `.\facts\t07_stability.json` . Sources are cited by file name and line. Full paths are in the facts file.

## Overview

The recompiled game runs against a memory map that copies the Xbox layout at its original addresses. Guest RAM is one 64 MB page-file mapping, mapped at a host base and mirrored 28 times at 64 MB steps, so guest addresses wrap as they do on the 26-bit Xbox bus. The XBE sections are copied to the addresses they were linked . The fake TIB sits at 0x00004000, the main stack at 0x00780000 (8 MB, growing down), the kernel data exports at 0x00740000, and the heap from 0x00F80000 to the top of the mapping. Unlike the older docs, the TIB is no longer at address 0, so a null dereference faults where it happens.

Kernel imports are not resolved by the XBE loader. Each thunk slot holds `0x80000000 | ordinal`, and init rewrites it to a synthetic VA at 0xFE000000 + 4 x slot (data exports get the VA of their data). A recompiled indirect call to that VA reaches `recomp_lookup_kernel`, which picks the bridge; the bridge reads stdcall arguments from the simulated stack at `g_esp`, does the Win32 work, and returns in `g_eax`. Hardware apertures (NV2A at 0xFD000000, MCPX at 0xFE800000, flash at 0xFF000000) are plain zeroed RAM, and a background thread clears the NV2A busy bits that D3D8 spins on.

Threads are real. `PsCreateSystemThreadEx` calls `CreateThread`, and critical sections are shadowed onto host locks keyed by the guest pointer. Timing is Windows timing: `KeDelayExecutionThread` becomes `Sleep`, and one kernel timer thread drains guest DPCs, the 60 Hz vblank and guest timers. Windows does not give that thread 1 ms resolution unless the process asks for it. A bare-process measurement of `Sleep(1)` averaged about 15.5 ms, so a guest 1 ms sleep may take that long; the in-game figure is not confirmed. Most of the freezes in this pack were a spin or a lock that waited on something that never changed. The fixes were loop guards, locks taken in the right order, and early-outs for dead sockets.

Stability work came in three waves: the 2026-10-10 hitch and crash investigations (measurements and one crash, no fixes); the 2026-10-14 single-player audit, where the XACT guards were in PC Release 57702bc3 but the CRT trig fix and the XACT DPC lock patch were not yet; and the heap and lock work after that. The table under "Bugs and fixes" says which build each fix is .

## Key code

| Address or function | What it does | Source |
|---|---|---|
| `xbox_MemoryLayoutInit` | Creates the 64 MB file mapping, maps the base and 28 mirrors, copies XBE sections, sets up apertures and heap | `xbox_memory_layout.c:1737`, `:2553`, `:2588` |
| base try order | 0x00010000, 0x00800000, 0x01000000, 0x02000000, 0x10000000, then host-chosen | `xbox_memory_layout.c:1793` |
| `XBOX_TIB_MAIN` 0x00004000 | fake TIB for the main thread; `fs:` based at `g_fs_base` | `xbox_memory_layout.h:369`, `xbox_memory_layout.c:1136` |
| `XBOX_STACK_BASE` 0x00780000 | main stack, 8 MB, top = base + size - 16 | `xbox_memory_layout.h:373-376` |
| `XBOX_HEAP_BASE` 0x00F80000 | heap base; `XBOX_HEAP_TOP` is mapped size or RAM | `xbox_memory_layout.h:413`, `:432` |
| `XBOX_CONTIG_BASE` 0x80000000 | contiguous physical window, own storage, page P at 0x80000000 + P | `xbox_memory_layout.c:2290-2329` |
| `XBOX_TILED_BASE` 0xF0000000 | view of the contiguous window (not RAM) | `xbox_memory_layout.c:2630-2690` |
| 0x80010000 | fake kernel PE page, e_lfanew 0x80, no sections | `xbox_memory_layout.c:2527-2551` |
| 0xFD000000 / 0xFE800000 / 0xFF000000 | NV2A (16 MB), MCPX (8 MB), flash (1 MB), all zeroed RAM | `xbox_memory_layout.c:91-117`, `:2350`, `:2394`, `:2496` |
| `nv2a_ack_thread` / `NV2A_ACK` table | clears busy bits at 0x100410 and holds interrupt registers at zero | `xbox_memory_layout.c:159`, `:915` |
| `xbox_HeapAlloc` / `heap_alloc_locked` | block table under `g_heap_lock`; bump-only unless reclaim is on | `xbox_memory_layout.c:3351`, `:3407` |
| `heap_carve_free` | best-fit allocation in reclaim mode | `xbox_memory_layout.c:2986` |
| `xbox_AllocThreadStack` | 512 KB thread stacks from the heap, max 8 live | `xbox_memory_layout.c:3041`, `:3094` |
| `xbox_FreeThreadStack` | returns a worker stack when the thread ends | `xbox_memory_layout.c:3139` |
| `xbox_AllocThreadTib` | per-thread TIB and TLS block | `xbox_memory_layout.c:3063` |
| `xbox_RtlNtStatusToDosError` | maps status to DOS error; 0xC0000018 becomes 487 | `kernel_rtl.c:637` |
| shadow critical sections | guest pointer to host CRITICAL_SECTION, index doubles at half full | `kernel_rtl.c:164-206`, `:340` |
| `xbox_RtlTryEnterCriticalSectionShadow` | non-blocking enter for interrupt-level code | `kernel_rtl.c:605` |
| `xbox_PsCreateSystemThreadEx` | `CreateThread` wrapper; 64 KB default stack | `kernel_thread.c:69`, `:105`, `:108` |
| `xbox_KeDelayExecutionThread` | relative 100 ns to `Sleep` milliseconds, minimum 1 ms | `kernel_thread.c:158-198` |
| `sub_0030C659`, `sub_0030C709`, `sub_0031DF5C` | CRT cos, sin and fmod that spun on `g_fp_cmp` | `recomp_0077.c`, `recomp_0079.c` |
| `sub_003DEA53` | per-frame XACT update; walks lists at +0x44, +0x64, +0x5C | `recomp_0097.c` |
| `sub_003E7249` (+ `loc_003E725F`) | XACT timer-heap remove; guard now runs under the lock | `recomp_0098.c`, `patch_sp1_heap_sanitize_locked.py:21` |
| `ma2_xact_list_repair` | list guard, 4096 steps, relinks the circular list | `recomp_0097.c` (via `patch_sp2_list_repair_selflink_locked.py:7`) |
| `sub_001ED390` | network update; early-out when the receive socket is -1 | `recomp_0051.c`, `patch_nal2.py:1` |
| `sub_0042532E` / `sub_00425264` | XUSB TD pop; head at 0x00534D0C | `recomp_0103.c`, `crash-2026-10-10.md:22` |
| `sub_000B9400`, `sub_0015EFF2` | checkpoint save writer and checkpoint routine | `recomp_0019.c`, `recomp_0035.c`, |

## Added code and hooks

- `xbox_memory_layout.c` and `.h`: the mapping, apertures, heap and reclaim code, thread stacks and TIBs, the NV2A ack thread, the watchdog and the peek and watch tools.
- `kernel_rtl.c`: shadow critical sections, the contention report, CRT lock tracing, the NTSTATUS table.
- `kernel_thread.c`: thread creation, delay and priority.
- Generated-code patches: `recomp_0077.c` and `recomp_0079.c` (the five `if (0)` sites; bots tree, build 786d78b8), `recomp_0097.c` and `recomp_0098.c` (XACT guards and the DPC lock), `recomp_0051.c` (NAL early-out).
- `patch_x87_memory.py`: rewrites generated 80-bit `fld` and `fstp` sites to `recomp_fp_load80` and `recomp_fp_store80`.
- `patch_sp0`, `patch_sp1`, `patch_sp2`, `patch_nal`, `patch_nal2`: the stability patches with their status below.
- `[EXIT]` markers in `fb_present.c` (WM_CLOSE), `d3d8_device.c` (WM_QUIT) and `pc_native_settings.cpp` (quit posts). Each writes pid, tid, QPC and phase before the existing exit. The change is in a candidate; the v25 EXE did not have it.

## Env vars, settings and test hooks

| Variable | Effect | Default |
|---|---|---|
| `RECOMP_HEAP_RECLAIM` | heap reuse: split, merge, free returns memory | off |
| `RECOMP_STACK_RECLAIM` | stack middle to heap; `=0` keeps it reserved | on with heap reclaim |
| `RECOMP_EXT_VMA` | tracks guest addresses above the mirrors | off |
| `RECOMP_TRAP_NULL` | page zero faults at init | off |
| `RECOMP_CS_MODE=single` | one shared lock for all guest critical sections (diagnostic) | off |
| `RECOMP_CS_TRACE_CRT` | logs CRT lock acquire and release order | off |
| `RECOMP_NV2A_SPIN=1` | ack thread always spins, old behaviour | off |
| `RECOMP_NV2A_TRACE`, `RECOMP_PB_SCAN` | per-pushbuffer diagnostics | off |
| `RECOMP_AC97_READY` | forces the codec-ready bit for DirectSound | off |
| `RECOMP_TITLE_KEVENTS=1` | title-built KEVENTs can be waited on | off |
| `RECOMP_WATCHDOG_SECS` | dumps the guest stack and aborts after N seconds | off |
| `RECOMP_PEEK`, `RECOMP_WATCH`, `RECOMP_WATCH_RAW` | print globals at a hang; name the writer of a dword | off |
| `RECOMP_ALLOC_TRACE` | logs contiguous allocations | off |
| `RECOMP_PERF=1`, `RECOMP_FIRSTUSE_TRACE=1` | timing for shader and texture first use (candidate) | off |
| `RECOMP_BOT_STALL=1` | bot stall detector (`[BOT-STALL]`) | set in replay runs |

Harnesses, all under `tests\`. Each runs the real layout or dispatcher with a synthetic guest buffer. None needs game files, except the posix layout test, which takes a real XBE for the thunk count.

| Harness | Checks |
|---|---|
| `memory_regressions` | default, heap reclaim, ext VMA, 128 MB map (ctest, `-A x64`) |
| `memory_layout_posix` | base mapping, host page alignment, mirrors, null trap; prints `Kernel thunks: N` on a real XBE |
| `kernel_memory_posix` | 15 checks on Mm* and Nt* memory calls |
| `kernel_bridge` | ordinal 173 through the dispatcher; the stdcall pop |
| `kernel_regressions` | 487 mapping, pseudo-handle duplication, 2^31 wrap (opt-in `-DXBOX_SLOW_TESTS=ON`) |
| `kernel_events` | title-built KEVENT waits with and without the switch |
| `kernel_directory`, `kernel_object_paths`, `kernel_dispatch` | directory query ABI, counted paths, per-thread dispatch slot |

For a hang: set `RECOMP_WATCHDOG_SECS` and `RECOMP_PEEK`. For a deadlock: set `RECOMP_CS_TRACE_CRT`, and try `RECOMP_CS_MODE=single` to see whether a lock cycle is the cause. Launch every game through (at most two at a time; four to six blue-screened the PC).

## Bugs and fixes (history)

| Symptom | Root cause | Fix | Shipped build |
|---|---|---|---|
| Game thread spins for about 1 s in cos, sin or fmod during a match | `fnstsw; sahf; jp` after fcos, fsin or fprem1 tests `g_fp_cmp == 2` (last compare), not C2; `sub_0030C659`, `sub_0030C709`, `sub_0031DF5C` | Five sites `if (0)` (`recomp_0077.c`, `recomp_0079.c`) | Bots tree, build 786d78b8. Not in PC Release 57702bc3 as of 2026-10-14; needs publish |
| Freeze in a truck explosion (campaign, 2026-10-11) | XACT list walk never returns to the head: `sub_003DEA53` and `sub_003E4A20` | `ma2_xact_list_repair` 4096-step guard, `[XACT-LIST]` log | 57702bc3 (PC Release) |
| XACT update leaves the DirectSound lock held; DPC stalls | Two `KfLowerIrql` calls through `esi` skip the unlock in `sub_003DEA53` | `patch_sp0` (recomp_0097.c) | Applied 2026-10-14; not in 57702bc3 |
| Heap guard races the timer DPC on remove | Guard ran in `sub_003E7249` after `sub_003E133C` had lowered IRQL | `patch_sp1` moves it below the raise at `loc_003E725F` (recomp_0098.c) | Applied 2026-10-14 |
| Heap stops growing; CRT retries | 0xC0000018 mapped to 317, not 487 | Map to 487 (`kernel_rtl.c:637`) | Source; shipped status not in pack |
| Network freeze after leaving multiplayer | Receive on closed socket (+0x94 = -1) in `sub_001ED390`; Verify (`sub_0040ACB0`) waits in D3D for a flip | Early-out returns 0x80072736 and pops 8 bytes (`patch_nal2.py`); first version (`patch_nal.py`) wrong, superseded |  |
| Heap runs out during the front end after multiplayer | First-fit split the lowest hole; 48.5 MB heap | Best fit plus stack middle to heap (reclaim mode) | Source (`xbox_memory_layout.c`) |
| Saves take 5 s, menus up to 19 s, after about 7 minutes | 4096-slot critical-section shadow index filled up | Index doubles at half full | EXE 531f2575 |
| Null dereference silently corrupts the SEH head | TIB at page zero | TIB at 0x4000; `RECOMP_TRAP_NULL` | Source (current header) |
| Halo: GPU spin, then PGRAPH_INTR recursion | Busy bits and interrupt registers never clear | NV2A ack table | Source |
| Kernel log floods after 2^31 calls (about 1 FPS) | `int` counter wraps | `long long` counter | Source |
| CRT duplicates thread handle forever | `NtCurrentThread` zero-extended | Sign-extend 0xFFFFFFF0 and above | Source |
| USB TD free-list corruption, access violation 0xC0000005 | Pop from `sub_00425264` with no empty check, free-list head 0x00534D0C | None yet (cross-thread race suspected) |  |
| First explosion hitch, about 668 ms | Shader and texture first use; per-miss cost about 4 ms | Instrumentation only | Not fixed |
| Checkpoint pause, about 146 ms | Guest CPU inside `sub_000B9400`, not disk or audio | None yet | Open, low priority |
| Controller rumble stays on after walking | A one-shot off was lost; timer granularity suspected | Proposed: `timeBeginPeriod(1)`, and a 500 ms staleness stop | Open |
| Audio pump blamed for save hitch | Measured 1.55 ms | Refuted | n/a |
| BattleArmor death blast froze the game | DirectSound active-list re-insert (DPC spinning on a self-linked entry) | `RECOMP_AUDIO_ACTIVE_GUARD` (5a2b21c), `RECOMP_AUDIO_DEFER_GUARD` (0163bba) | par24 (2026-11-09), `t28.audio-active-guard-env` |
| A hang left no clue | No frame for a long time | `[CS-WAIT]` sampler in the stall monitor; r26 adds the default-on hang monitor with `hang-<pid>.dmp` | `t28.cs-wait-hang-sampler` (par24); `t28.hang-monitor-default-on` (r26, not yet published) |
| Two threads faulting at once lost the crash report | Second fault ended the process | Second thread waits, rumble stops, newest 5 dumps kept | r26 7eba2c6, `t28.crash-dump-retention` (not yet published) |
| FPU C2 loops spun for ever (Rush Hour freeze) | NaN compare left in `g_fp_cmp` | Five `if (0)` hand edits (cos, sin, fmod); the installer build now fails if one is missing | par24; `t07.fpu-c2-fix-state`, `t28.build-fails-without-fpu-edits` |

## Open questions

1. What is the root cause of the original campaign XACT list corruption? The guard stops the spin, but the bad link is not explained (`REPORT`).
2. Is patch_sp2 (self-linked node drop under the lock) needed? It is not applied because the guard has not fired in a real run.
3. What is the cause of the USB TD free-list corruption?
4. Does the explosion hitch come from cold cache-file opens? A reboot followed by the same explosion test would decide it. The `RECOMP_FIRSTUSE_TRACE` run has not been done.
5. Does the checkpoint's 140 ms of guest CPU in `sub_000B9400` come from one hot helper? A sampling profile of a checkpoint would show it.
6. Is the in-game timer resolution 1 ms or 15.6 ms? The measurement was in a bare process; `NtQueryTimerResolution` at runtime settles it.
7. Where is the inline-or-spawn branch for `PsCreateSystemThreadEx`? `kernel_thread.c` creates threads unconditionally; the branch is probably in `kernel_bridge.c`, which is not in this pack.
8. Which kernel ordinal is DbgPrint: 8 or unknown? `kernel-exports.md` lists both.
9. Do the stuck-rumble and walking-rumble symptoms go away once the timer fix is in? Repeat the walk-and-stop test about ten times.
10. Was `RECOMP_FORCE_RETURN` a guest-side fault switch? The layout code reads it but does not say what it forces.

