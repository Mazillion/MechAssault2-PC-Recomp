# The static recompiler: how ma2.xbe became C

This article explains how the xboxrecomp toolchain turns `ma2.xbe` into the generated C under `generated (made at install time, not in the repository)`, and what the generated code means to someone reading it. The toolkit docs use Burnout 3 for their examples, so most addresses in them are Burnout 3 values. The MA2 source tree is the authority for anything specific to this title. Facts are in `facts/t08_toolchain.json` (ids `tc-*`).

## Overview

The pipeline runs in fixed stages. `tools.xbe_parser` reads the headers: image base 0x00010000, the XOR-encoded entry point, the section table, the kernel thunk table and library versions. `tools.disasm` finds functions with a recursive descent from the entry point plus a linear sweep, and writes `functions.json`, `labels.json`, `xrefs.json` and `strings.json`. `tools.func_id` and `tools.abi_analysis` classify functions and recover signatures. `tools.recomp` then lifts every x86 instruction to a C statement. Each function becomes `void sub_XXXXXXXX(void)`; arguments travel on a simulated guest stack and return values in `g_eax` `README.md:53-92`, `tools/README.md:13-53`).

Every generated function reads and writes shared guest registers. In the current MA2 runtime these are thread-local (`recomp_types.h:160-168`), so guest threads each have their own registers. Memory accesses go through `MEM8/16/32` macros over a 64 MB mapping, and `XBOX_PTR` truncates to 32 bits first so overflow wraps as it does on the Xbox (`memory-layout.md:108`). Indirect calls cannot be resolved at build time, so each goes through `RECOMP_ICALL_SAFE_AT` or `RECOMP_ITAIL`, which look up the target in three tiers: manual overrides, the generated dispatch table, then the kernel bridge at 0xFE000000 and up (`recomp_types.h:1060-1077`). Most of the hard bugs in a port come from these indirect calls, from wrong stack balance, or from a missing or wrong lift.

The toolkit also contains a conformance harness (`tools/conformance`). It runs the original x86 bytes on the host CPU and compares the result with the lifted C, so a lifted instruction is checked by execution rather than by comparing text. Several real lifter bugs were found this way that text tests had missed (fxch, fnstsw C2, js/jns width, bt family, carry-flag instructions).

## Key code

| Address or function | What it does | Source |
|---|---|---|
| XBE header 0x128 | Entry point, XOR key 0xA8FC57AB (retail) or 0x94859D4B (debug) |  |
| XBE kernel thunk field | Offset and key disagree between docs; read the header before trusting either | `01-xbe-parsing.md:63`, `docs/formats/xbe.md:41` |
| `RECOMP_ICALL` | Records the target in a 16-entry ring, checks code range, then manual, generated and kernel lookups; a failed lookup pops the dummy return | `recomp_types.h:1060-1077` |
| `RECOMP_ICALL_IS_CODE` | Range test against `g_xbox_code_lo/hi` from the mapped sections; kernel thunks always allowed | `recomp_types.h:126-128` |
| `RECOMP_ICALL_SAFE_AT` | Indirect call that restores the guest esp on failure and names the call site (about 21,600 uses in gen) | `recomp_types.h:1133` |
| `RECOMP_ITAIL` | Indirect jump: looks up and calls, pushes no return address (about 526 uses in gen) | `recomp_types.h:1180` |
| `RECOMP_ABI_CALL` | Direct and indirect call hook; with `RECOMP_ABI_CHECK` it verifies ebx, esi, edi and esp | `recomp_types.h:1011-1024` |
| `RECOMP_FCMP` / `g_fp_cmp` | x87 compare result: 2 unordered, -1 less, 0 equal, 1 greater | `recomp_types.h:376`, `recomp_0037.c:5605-5607` |
| `RECOMP_FP_PC` | Rounds x87 results to float under precision control 24 | `recomp_types.h:282` |
| `XBOX_FS_BASE` / `g_fs_base` | Base for every fs-relative access; page zero stays unmapped | `recomp_types.h:186-194`, `recomp_0000.c:905` |
| `g_seh_ebp` | SEH frame-pointer bridge between prolog helper and caller | `recomp_types.h:194`, `docs/technical/seh-handling.md:28` |
| `g_ebp` | Thread-local ebp published for frameless callees | `recomp_types.h:236`, `recomp_0000.c:322` |
| `g_df` / `RECOMP_DF_STEP` | Direction flag for string ops (cld/std) | `recomp_types.h:250-251` |
| `RECOMP_ATOMIC_ADD32` / `_CAS32` | lock xadd and lock cmpxchg as host atomics | `recomp_types.h:226` |
| `recomp_debug_service` | INT 2D service 1 prints the ANSI string at ecx | `recomp_types.h:79` |
| `lookup.py` (re_index) | Class, vtable, function and string lookups over `index.pkl` |  |
| `q.py` (re_index) | Regex over strings in ma2.xbe with their xrefs; reads `idx.pkl`; paths hard-coded |  |

### Calling and stack form in MA2 gen code

A direct call is emitted as a push of the real return VA followed by a checked call. For example, `recomp_0000.c:93` pushes `0x000120A1` and calls `sub_0001CF40` through `RECOMP_ABI_CALL`. Guest code can read that return value, so it must be correct even though control flow uses C call and return. A SEH function builds its registration frame by pushing try level `0xFFFFFFFF`, a handler and a scope table, then linking the frame at `fs:[0]` (`recomp_0000.c:900-907`).

### Flags

The gen code has no flags register. Each arithmetic or compare instruction saves its operands in `_fa`/`_fb` (unsigned) and `_fas`/`_fbs` (sign-width), and a condition macro such as `TEST_Z(_fa, _fb)` reads them (`recomp_0000.c:85-88`). Flag state is propagated only along predecessor edges where every predecessor agrees (`docs/technical/conformance-testing.md:214`).

## Added code and hooks

- `generated recomp_types.h (made at install time, not in the repository)`: the runtime header that generated code includes. It is written by the lifter and is not overwritten on regeneration. Deleting it refreshes the copy (`GETTING_STARTED.md:256`).
- `generated recomp_0000.c (made at install time, not in the repository)` through `recomp_0037.c`: generated code. The gen tree has about 93,600 `RECOMP_ABI_CALL` lines, about 5,300 `sub_` calls and 2,500 `g_fp_cmp` uses.
- `recomp_manual.c`: manual overrides. It must include `recomp_types.h`, or an override writes a plain variable that the caller never reads).
- Post-generation patches (`#if 0` around crashing functions, vtable guards, jump-table replacements) are lost on regeneration and must be re-applied from a checklist).

## Env vars, settings and test hooks

These names appear in the MA2 env var catalog: `RECOMP_ABI_CHECK`, `RECOMP_WATCHDOG_SECS`, `RECOMP_UNIMPL_TRAP`, `RECOMP_NV2A_TRACE`, `RECOMP_PB_EXEC`, `RECOMP_AC97_READY`, `RECOMP_HEAP_RECLAIM`. Their meanings come from the toolkit docs:

- `RECOMP_ABI_CHECK` (build flag): checks callee register and stack balance on every call. Six CRT helpers are always reported (`06-debugging.md:357`).
- `RECOMP_WATCHDOG_SECS=N`: dumps registers, PUT/GET and the guest stack when progress stops (`06-debugging.md:317`).
- `RECOMP_UNIMPL_TRAP=1`: stops at the first untranslated instruction that is reached (`06-debugging.md:320`).
- `RECOMP_NV2A_TRACE=1` shows whether DMA_PUT moves. `RECOMP_PB_EXEC=1` consumes the pushbuffer (`06-debugging.md:313`).
- `RECOMP_AC97_READY=1`: reports the audio codec ready, which can unblock a title that skips its whole engine init when DirectSound fails (`06-debugging.md:305`).
- `RECOMP_HEAP_RECLAIM`: opt-in real frees (`README.md:581`).
- `RECOMP_ICALL_FEEDBACK` and `RECOMP_CALL_PROFILE` are compile-time switches in the header (`recomp_types.h:450`, `:1017`).
- Also in the toolkit: `RECOMP_WATCH=<va>`, `RECOMP_TITLE_KEVENTS`, `RECOMP_GUEST_LOCK`, `RECOMP_EXT_VMA`.

Conformance: `py -3 -m tools.conformance` (all), `--only snippets`, `-k NAME`, `--keep`, `--xbe PATH` (`conformance-testing.md:78`). The unit suite `py -3 -m pytest tools/` should show 630 passed and 0 skipped (`README.md:680`).

The re_index tools answer the common questions:
- `python lookup.py class MAMech -n 8` lists a registered class, its vtables and slots. Verified in this pass: MAMech resolves to vtable 00469CD0 with 162 slots.
- `python lookup.py vt 0x469CD0` names the class that owns a vtable, or the vtable that holds an address.
- `python lookup.py fn 0x1087C8` lists the vtable slots holding a function, its strings, callers and pointer refs.
- `python lookup.py str Waypoint` lists functions whose strings match, with owning vtable slots. Verified: four functions, including 0012F52F in slot 4 of 0046FEC8 (MAWaypoint).

Finding a class: MA2 has no RTTI (only 8 `std:exception` descriptors), so names come from the runtime class registry. Descriptor fields are documented .

## Bugs and fixes (history)

Toolkit fixes that affect MA2, most recent first:

- v0.13.0 (October 2026): MechAssault's CRT memcpy ran a misaligned `inc esp` lifted from inside a `mov`, so every 28 to 31 byte copy returned with ESP off by one. Fixed in PR #135 (`README.md:611`).
- v0.13.0: jump tables named by their last slot lost every later arm (PR #134, `README.md:614`).
- v0.13.0: switch tables whose displacement is not slot 0 silently skipped the CRT memcpy's unaligned-byte copy (PR #140, `README.md:637`).
- v0.13.0: the save directory was relative, so `SHCreateDirectoryExW` created nothing (PR #133, `README.md:616`).
- v0.13.0: pushad and popad were TODOs (#167); a failed ICALL to a caller-cleans function popped its arguments twice (#169); lahf and unordered compares (#150); x87 precision control (#149).
- v0.13.0: NT pseudo-handles were not sign-extended on x64, so `NtDuplicateObject` retried forever (`kernel-replacement.md:309`). STATUS_CONFLICTING_ADDRESSES now maps to 487 so the CRT heap grows elsewhere (`kernel-replacement.md:325`).
- Before v0.13.0, conformance found fxch no-op, fnstsw without C2 or TOP, js/jns width, flag propagation order, stc/clc/cmc and the bt family (`conformance-testing.md:135-242`).
- Code-confirmed superseded design points: fs: segment is now translated, not dropped (`recomp_types.h:186`); the ICALL range is from mapped sections, not 0x00400000 (`recomp_types.h:126`); calls push the real return VA, not 0xDEAD0000 (`recomp_0000.c:93`); guest threads run on host threads (`recomp_types.h:143`).

## Open questions

- The kernel thunk offset and XOR key are inconsistent in the docs (`01-xbe-parsing.md:63`, `xbe.md:41`). Read the MA2 header to settle them.
- The heap base is 0x00880000 in `05-runtime.md:131` but the stack range is 0x00780000 to 0x00F7FFFF in `memory-layout.md:37`, which puts 0x00880000 inside the stack. Check the MA2 constant.
- Docs disagree on whether .text is copied into mapped memory (`05-runtime.md:94`, `memory-layout.md:155`).
- The kernel export count differs by document: 147 imports, 170 routed of 371, 366 possible (`README.md:100`, `kit/source/game/src/README.md`).
- ebp is local per the docs, but the MA2 gen code publishes it to g_ebp about 32,500 times (`recomp_0000.c:322`). The effect on FPO functions is not measured here.
- Whether SEH registration frames are ever walked for `__try` dispatch is not established. The docs say no; the MA2 scope table comment says the frames matter (`recomp_types.h:41`).
- Dispatch is still a binary search over the generated table; the flat-array replacement is listed as in progress (`docs/technical/ms-fusion-adoption-plan.md:64`).
- Crimson Skies still has two unexplained conformance divergences (`conformance-testing.md:386`). Not MA2, but they are the next conformance work item.

