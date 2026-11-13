# How patches work

The game's program is translated to C on the player's PC. The project still has to change some of that
translated code (bug fixes, hooks for the PC layer, bots and custom scenarios). Those changes live in
[`kit/patches`](../kit/patches) in a form that carries **none of the translated code**.

## The pieces

```
kit/patches/
  manifest.json            format "edits-2"; SHA-256 of every base file and every final file
  diff/recomp_NNNN.c.json  edits for one translated file
  add/*.h                  whole new headers written by the project (e.g. ma2_spring_rate.h)
  fastcode/
    fastcode.py            generates fast copies of hot functions on the player's PC
    fastcode_hot.txt       which functions are hot (names like sub_0018ABB5 = addresses)
    fastcode.json          inputs, forced functions, SHA-256 of every file FastCode writes
    post/                  strict edits applied after FastCode (currently deletions only)
```

### `diff/*.json`: line number + the lines we wrote

Each file is a list of edits `[base_line, delete_count, [new lines]]`, replayed from the end of the file
backwards (see [`kit/installer/gen_patch.py`](../kit/installer/gen_patch.py)). It records *where* to delete and
*what the project writes*, never the text of what is deleted and never surrounding context: the base file's
SHA-256 (in `manifest.json`) already guarantees the line numbers mean the same thing on every PC.

A new line is either text the project wrote, or a **reference** to a line of the generated file: an
integer `N` (generated line `N` as it is) or `[N, "indent"]` (the same line re-indented). When an edit
moves a generated line into a new block, or repeats one on a second path, only its line number is stored
and the installer copies the line from the code generated on your PC. `gen_patch.py make` does this
automatically for every line that already exists in the base file, so no stored line equals a generated
one (the release tooling checks this, ignoring whitespace).

Applying is strict: every base file must match its SHA-256 before, and every result must match after. A
different disc version, a modified `ma2.xbe` or a different toolkit stops the install with a clear
message instead of building something untested.

### What the stored lines are

The patch set writes 2,109 lines (36 of them blank) into 59 translated files (plus 3 new headers), and refers
to 38 generated lines by number. Measured on the 1.0.8 set, unchanged in 1.0.9 (the FastCode post set writes none):

| Kind | Lines | Example |
|:--|--:|:--|
| Calls into the project's own code (`ma2_*`, `pc_*`), declarations, includes, comments, braces, project helpers | 1,628 (77%) | `if (ma2_bot_is_local_ai(edi)) { ebx = 0; goto loc_000C93D1; }` |
| One-line stubs for function addresses the translator did not detect (`recomp_stubs_unresolved.c`) | 249 (12%) | `void sub_00012E30(void) { recomp_unresolved_stub(0x00012E30u); g_esp += 4; /* ... */ }` |
| Statements in the translator's notation with no project function name | 128 (6%) | a corrected comparison (`if (edi == 0) goto ...` where the translator tested the wrong operand), a guarded memory read, a jump to an existing label, an FPU stack fix |
| Register-only statements | 68 (3%) | `esp += 8;`, `edx = 0; ebx = 0; ecx = 0;` |

The last three groups are edits the project wrote, not copies of the original: the line they replace is not
stored, and none of them equals a generated line. A few debug-only trace blocks (for example
`RECOMP_SND_TRACE`, `RECOMP_JET_TRACE`; off unless their variable is set) also sit inside translated
functions; they are the project's own code too. We keep all of this as small as possible and move logic
into named `ma2_*` functions in the PC layer whenever a fix grows beyond a line or two.

### FastCode

For speed, the 713 hottest functions get a second, register-promoted copy (`recomp_fast_NNNN.c`).
Those copies are pure machine output, so they are **generated on the player's PC** by `fastcode.py`
from the already patched code; the kit carries only the tool, the hot list and SHA-256 values of what it
must produce. Functions whose body calls one of our hooks are normally left out of FastCode; the
`force` list in `fastcode.json` names the few that are still worth a fast copy, and `fastcode.py --force`
generates them like any other. Earlier builds shipped such a copy as a post-patch (about 1,200 lines of
translated code); the `force` mechanism removed that.

## Making and checking patches

Use [`tools/make_patches.py`](../tools/make_patches.py) (walkthrough in [building.md](building.md#changing-translated-game-code)).
It:

1. strips FastCode output from your edited tree and matches line endings to the base;
2. writes `diff/`, `add/`, `manifest.json`;
3. replays the installer's patch + FastCode steps on the pristine base and requires a byte-identical result;
4. lists every stored line that looks like translated code without a project hook, and refuses to write
   at all if there are more than 250 of them (the usual cause is a wrong `--base`).

Before a release, review that list. If a fix needs more than a couple of such lines, move it into the PC
layer and leave one call in the translated file.

## Why not ship a binary diff or the patched files?

A binary or context diff would contain the original code around every change. Shipping patched files
would ship the game. Line numbers plus the lines we wrote, pinned by hashes, is the smallest form that
still makes the build fully reproducible on every PC.
