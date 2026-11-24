# Linux and Steam Deck through Proton (1.2.1)

The game is a Windows program. On Linux it is installed and played through Proton or Wine with the **same installer exe**
as on Windows; native Linux builds are a 2.0 goal. This page says how the installer behaves there and how it is tested.
Everything here is tested headless under Wine 9.0 (WSL Ubuntu 24.04, Xvfb) and, for the game, GE-Proton 11; **nothing
here has been run on a real Linux PC or a Steam Deck yet.**

## What differs from Windows

| Step | Windows | Wine / Proton |
|:--|:--|:--|
| Stub unpack | LZMA, the stub's own decoder (`installer-exe/lzma_dec.h`) | the same (the LZMS kits up to 1.2.0 cannot unpack under Wine: the Compression API is missing) |
| Python | bundled 3.13 | the same; `python/Lib/sitecustomize.py` keeps `shutil` off `CopyFile2`, which Wine 9 does not implement |
| C++ compiler | Visual Studio Build Tools (MSVC) | LLVM 18.1.8 (clang-cl, lld-link, llvm-rc, llvm-mt), the Microsoft CRT and Windows SDK laid out by xwin 0.10.0, CMake 3.31.6, Ninja 1.12.1, pinned in `kit/installer/data/toolchain.json` and kept in `%LOCALAPPDATA%\MechAssault2\Toolchain` inside the prefix |
| Build Tools consent | "Install Microsoft's C++ tools" screen, `--install-build-tools` | "Download the build tools for Linux" screen, the same `--install-build-tools`; RecompLauncher shows the `linux_build_tools` tick |
| C runtime | the VS redistributable DLLs copied beside the game | linked into the exe (`/MT`, `kit/installer/data/wine-clang.cmake`) |
| d3dcompiler_47.dll | System32 | a probe at install time compiles the game's three base shaders with the prefix's own d3dcompiler (`wine_host.probe_d3dcompiler`): they compile (GE-Proton 11) = nothing needed; any fails (Wine 9.0) = the player is asked to put Microsoft's DLL in the game folder, which is checked. Never shipped. |
| Install stamp | `managed_by` | the same, plus `host: wine` |
| Paths | Windows paths | `/home/...` (RecompLauncher's `{install}`) becomes `Z:\home\...` (`precheck.clean_path`, `wine_host.dos_path`) |
| Reports | | system.txt, the support bundle and the report summary add `Running under: Proton (Wine x) on Linux y` |
| RecompLauncher "Update now" | `recomplauncher://` opens the launcher | the prefix gets a `recomplauncher` URL class pointing at `winebrowser` (hands the link to `xdg-open`) |

Detection: `wine_host.is_wine()` (ntdll exports `wine_get_version`); `MA2_TEST_WINE=<version>` pretends in tests.
`MA2_TOOLCHAIN=clang` builds with the LLVM toolchain on Windows too (maintainers). `MA2_WINE_TOOLCHAIN=<folder>` uses a
toolchain folder that is already laid out; `MA2_TOOLCHAIN_SOURCE=<folder>` takes the pinned archives (and a laid-out
`xwin\`) from a folder instead of the internet.

## A Linux player without RecompLauncher (UNTESTED on real hardware)

The simplest route is Steam: *Games > Add a Non-Steam Game to My Library*, choose `MechAssault 2 Expanded Installer.exe`,
then *Properties > Compatibility > Force the use of a specific Steam Play compatibility tool* (Proton 11; tested with GE-Proton 11) and
*Play*. The installer window opens inside that Proton prefix; the default install folder is `C:\Games\MechAssault 2 Expanded`
inside the prefix. When it says Ready to play, add `MechAssault 2.exe` from that folder as a second non-Steam game with the
same Proton. (umu-run or plain Wine 10+ work the same way: `umu-run "MechAssault 2 Expanded Installer.exe"`.)

## Testing under Wine (headless)

- Never on the desktop: Xvfb on a private display, `WAYLAND_DISPLAY` unset, `winemenubuilder.exe` disabled.
- A file as Wine's stdout makes Python stop with "invalid handle": pipe the output (`... 2>&1 | cat >> log`).
- The stub: `"MechAssault 2 Expanded Installer.exe" --unpack-to <empty folder>` unpacks and exits.
