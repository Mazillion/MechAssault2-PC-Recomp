# kit/

Everything the installer carries, unpacked to a temporary folder while it runs. `installer-exe/pack_installer.py`
packs this folder plus a Python 3.13 runtime into the single installer exe.

| Folder | What | Lands in the install |
|:--|:--|:--|
| `installer/` | `install.py` (the engine and command line), `gui.py` (the window), `precheck.py`, `progress.py`, `gen_patch.py`, the PC menu recipes (`pc_assets.py`, `build_pc_assets*.py`, `scenario_assets.py`, `qte_icons.py`), `data/` (release identity, disc manifest of SHA-256 values, menu overlay hashes, icon, OFL fonts) | no (used while installing) |
| `toolkit/` | the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit: XISO, XBE parser, disassembler, function identification, ABI analysis, the recompiler. `vendor/capstone` is placed by `tools/fetch_deps.py` | no |
| `source/game` | the project's PC layer, bots (`src/bots`), custom scenarios (`src/scenario`), hooks (`src/recomp_manual.c`), the two bringup generators, Dear ImGui; FFmpeg in `vendor/` only on a developer's PC (`tools/fetch_deps.py`; the installer downloads it on the player's PC instead and never packs it). `src/recomp/gen` is created on the player's PC | the compiled exe; FFmpeg DLLs |
| `source/runtime` | the xboxrecomp runtime (kernel, D3D8 to D3D11, NV2A, APU/DSP audio, input, video) | compiled into the exe |
| `patches/` | the project's edits to the translated code, as line numbers + added lines, and FastCode ([docs/patches.md](../docs/patches.md)) | no |
| `assets/` | empty in public builds (no pre-compiled shaders are shipped) | `pc_assets/` |
| `defaults/` | `README.txt`, `Controls.txt`, `Docs/`, `pc_settings.ini` (defaults), `Licenses/` | install folder |
| `enhancer/` | scripts for the optional AI texture and video enhancement (the models and PyTorch are downloaded on request) | `Enhancer/` when used |
| `editor/` | the scenario editor exe, built from [`../editor`](../editor) by `tools/build_release.py` (not in git) | install folder |
| `report/` | `MA2 Report.exe`, the problem-report helper (crash and freeze boxes, Report a problem), built from [`../report-helper`](../report-helper) by `tools/build_release.py` (not in git) | install folder |

What is deliberately *not* here: anything from the game disc, the translated C code, pre-compiled shaders,
texture packs. See [LEGAL.md](../LEGAL.md#how-the-project-avoids-distributing-the-game).
