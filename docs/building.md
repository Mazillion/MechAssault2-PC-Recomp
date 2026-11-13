# Building

Two things can be built from this repository:

1. **The release installer** (`dist/MechAssault 2 Recomp Installer.exe`): what players download. Building
   it needs no game disc.
2. **The game itself**, from your own disc: what the installer does on a player's PC. You need this to
   work on the game code.

Everything runs on Windows 10/11 x64.

## Prerequisites

| Tool | Used for | Notes |
|:--|:--|:--|
| Visual Studio 2022 or newer (2026 tested), or its Build Tools, with **Desktop development with C++** (MSVC x64, Windows SDK, C++ CMake tools) | editor, installer stub, the game | found automatically with `vswhere` |
| **Python 3.13** from python.org (64-bit, with Tcl/Tk) | build scripts; it is also the runtime bundled into the installer | `py -3.13` must work |
| Git | version id in the editor's About box | optional |
| Your own **MechAssault 2: Lone Wolf** disc dump (US retail) | building / testing the game only | never commit anything from it |

## 1. Build the release installer

```bat
git clone https://github.com/Mazillion/MechAssault2-PC-Recomp
cd MechAssault2-PC-Recomp
py -3.13 tools\fetch_deps.py
py -3.13 tools\build_release.py
```

`fetch_deps.py` downloads the third-party binary packages and checks **every file**: Capstone 5.0 from PyPI
(pinned in [`tools/deps.lock.json`](../tools/deps.lock.json); it is packed into the installer) and the FFmpeg
9.0.2 shared build by gyan.dev (pinned in [`kit/installer/data/ffmpeg.json`](../kit/installer/data/ffmpeg.json)).
FFmpeg is **never packed**: the installer downloads the same build on the player's PC and checks it against the
same hashes. You need it here only to build the game yourself or to run the tests that use it; a release build
needs only `py -3.13 tools\fetch_deps.py --only capstone`. If a download link has moved, fetch the same archive
yourself and pass the extracted folder: `py -3.13 tools\fetch_deps.py --from ffmpeg=C:\path\to\ffmpeg-9.0.2-full_build-shared`.

`build_release.py` then:

1. checks that `VERSION`, `kit/installer/data/release.json`, the game's `ma2_release.h` and the editor's
   `version.h` and `editor.rc` agree, and that it runs on the exact Python version pinned in
   `tools/deps.lock.json`;
2. verifies Capstone (and refuses to pack FFmpeg);
3. builds the scenario editor (`editor/build.cmd`) into `kit/editor/`;
4. builds the native installer stub (`installer-exe/build_stub.cmd`);
5. packs `kit/` plus the running Python 3.13 into `dist/MechAssault 2 Recomp Installer.exe`
   (LZMS-compressed; the stub unpacks it with Windows' own Compression API);
6. scans the built binaries for local paths and user/machine names (`tools/privacy_scan.py`) and writes
   `dist/SHA256SUMS.txt`.

The compile steps run in a copy under `%SystemDrive%\MA2PubBuild` (change with `--build-dir`) so that no
path from your user profile can end up inside a binary. Expect 3-5 minutes. The result is about 22 MB (FFmpeg is not packed: the installer downloads it).

The installer's own tests. Run `tools\fetch_deps.py` first (tests that need the fetched FFmpeg are
skipped with a message otherwise), and keep the clone outside `%TEMP%` (the installer refuses install
targets there, and so do its tests):

```bat
py -3.13 tests\installer\test_engine.py
set MA2_TEST_XISO=<your disc image>
set MA2_TEST_GAME_FILES=<a folder with your disc's files, at least ma2.xbe>
py -3.13 tests\installer\ui_flow.py
```

`test_engine.py` runs without a disc (a few tests are skipped); set `MA2_TEST_XBE=<your ma2.xbe>` to run
those too. `ui_flow.py` always needs your own disc (the two variables above) and stops with a message
without it.

## 2. Build the game from your disc (development loop)

The installer's engine is a plain Python script and runs straight from the repository, so you do not
need to pack an installer to work on the game:

```bat
rem Once: a complete working tree, kept after the install (about 6 GB)
py -3.13 kit\installer\install.py --xiso D:\dumps\ma2.iso --dest D:\ma2dev --keep-work
```

Useful options: `--jobs N` (parallel compiles), `--low-priority`, `--until <step>` (stop after a step:
`disc`, `extract`, `analyze`, `recompile`, `generate`, `patch`, `build`, ...), `--vs <VS folder>`.

After it finishes:

| Path | What |
|:--|:--|
| `D:\ma2dev\MechAssault 2.exe` | the game, ready to run |
| `D:\ma2dev\_work\toolkit\MechAssault2\src\recomp\gen` | the translated, patched game code (+ FastCode copies) |
| `D:\ma2dev\_work\src\game`, `_work\src\runtime` | the sources the build compiled (a copy of `kit/source` + the generated code) |
| `D:\ma2dev\_work\build` | CMake/Ninja build folder; `_work\build.cmd` rebuilds the exe |

(If the install path is not plain ASCII, the build folder is an ASCII folder such as
`C:\MA2RecompBuild-xxxxxxxx`; the install log says which.)

### Changing the PC layer, runtime or bots

These are normal source files in [`kit/source`](../kit/source). Edit them in the repository, copy the
changed files over the matching ones in `_work\src`, run `_work\build.cmd`, and copy
`_work\build\mechassault2_recomp.exe` over `MechAssault 2.exe`. (Or simply run the installer with
`--update` on your dev install: it rebuilds from the repository without touching your settings.)

### Changing translated game code

Never commit the translated code. Keep a pristine copy of the toolkit output, edit a working copy, and
let the tool turn the difference into patch entries:

```bat
rem 1. pristine base: the toolkit output before any patch
py -3.13 kit\installer\install.py --xiso D:\dumps\ma2.iso --dest D:\ma2base --keep-work --until generate
rem 2. edit D:\ma2dev\_work\src\game\src\recomp\gen\recomp_00NN.c, rebuild, test
rem 3. record the edits as patches (line numbers + added lines only)
py -3.13 tools\make_patches.py --base "D:\ma2base\_work\toolkit\MechAssault2\src\recomp\gen" --edited D:\ma2dev\_work\src\game\src\recomp\gen
```

`make_patches.py` replays the whole install path (patch, FastCode) on the base and stops unless the result
equals your edited tree exactly. It refuses to write a patch set that would carry translated code in bulk
(for example if `--base` is the wrong tree). Keep the change in the translated file to a call into a named
`ma2_*` function in the PC layer whenever you can. How the format works: [patches.md](patches.md).

### Debugging

- The game log is `%LOCALAPPDATA%\MechAssault2\Logs\<time>.log`; crash and hang reports go next to it.
- `RECOMP_*` environment variables switch diagnostics and features; the [fact index](fact-index.md) lists
  them (`python docs\wiki\wq.py RECOMP_`).
- Bot matches write a `.botrec` recording; the bots doc explains the switches ([bots.md](bots.md)).

## 3. The scenario editor

```bat
editor\build.cmd          :: editor + format tests (out\fmt_test.exe)
editor\build.cmd tests    :: format tests only
```

It reads your installed game's files at run time. See [editor.md](editor.md).
