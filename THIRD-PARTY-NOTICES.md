# Third-party notices

The project's own code is under the [MIT licence](LICENSE). The components below keep their own licences.
Their licence texts are in [`kit/defaults/Licenses`](kit/defaults/Licenses) and are installed into every
game folder (`Licenses\`); files copied from these projects keep their original copyright headers.

"Where" says whether a component is **in this repository** (source), **fetched** at release-build time by
[`tools/fetch_deps.py`](tools/fetch_deps.py) (pinned file by file in [`tools/deps.lock.json`](tools/deps.lock.json)),
**bundled** in the released installer, or **downloaded on the user's PC** by the installer (FFmpeg on every
install; the rest on request).

## Compiled into the game (built on the user's PC, never distributed as a binary)

| Component | Licence | Where | Source |
|:--|:--|:--|:--|
| xboxrecomp runtime (kernel, D3D8 to D3D11, input, platform) | MIT | repo: `kit/source/runtime` | https://github.com/sp00nznet/xboxrecomp |
| xemu MCPX APU and NV2A files (espes, Jannik Vogel, Matt Borgerson) | LGPL-2.1-or-later | repo: `kit/source/runtime/src/apu`, `src/nv2a` | https://github.com/xemu-project/xemu |
| xemu DSP56300 emulator (espes, Matt Borgerson; adapted from Hatari / ARAnyM, Thomas Huth) | GPL-2.0-or-later | repo: `kit/source/runtime/src/apu/dsp` | https://github.com/xemu-project/xemu |
| libsamplerate (Erik de Castro Lopo) | BSD-2-Clause | repo: `kit/source/runtime/src/apu/libsamplerate` | https://github.com/libsndfile/libsamplerate |
| xxHash (Yann Collet) | BSD-2-Clause | repo: `kit/source/runtime/src/kernel/vendor/xxhash` | https://github.com/Cyan4973/xxHash |
| jsmn (Serge Zaitsev) | MIT | repo | https://github.com/zserge/jsmn |
| DirectXTex DDSTextureLoader (Microsoft) | MIT | repo | https://github.com/microsoft/DirectXTex |
| Dear ImGui 1.92.5 (Omar Cornut; with stb_truetype, stb_rect_pack, stb_textedit by Sean Barrett) | MIT | repo: `kit/source/game/vendor/imgui-1.92.5` | https://github.com/ocornut/imgui |

Because of the GPL parts above, the game executable built on a user's PC is, as a whole, under GPL terms.
It is never distributed by this project (it also contains the game's own program, which belongs to its
rights holders).

## Downloaded on the user's PC by the installer, then installed beside the game

### FFmpeg

| Component | Licence | Where |
|:--|:--|:--|
| FFmpeg 9.0.2, "full" shared build by gyan.dev: `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll`, `swresample-7.dll` and `swscale-10.dll` (copied next to the game); `ffmpeg.exe` and `ffprobe.exe` (used only by the optional video Enhancer); the build's headers and import libraries (to compile the game). | GPL-3.0 | **Not in this repository and not in the installer.** The installer downloads `ffmpeg-9.0.2-full_build-shared.zip` on the player's PC from where gyan.dev publishes it (its GitHub release mirror first, then gyan.dev), and checks each of its 177 files against the SHA-256 recorded in [`kit/installer/data/ffmpeg.json`](kit/installer/data/ffmpeg.json) before anything uses it ([`kit/installer/ffmpeg_fetch.py`](kit/installer/ffmpeg_fetch.py)). |

The project does not convey FFmpeg: the player's PC fetches it from its publisher. Its source: FFmpeg itself at
https://github.com/FFmpeg/FFmpeg/commit/946fcce07b; the build configuration and the versions and sources of
every external library in it are listed in the build's own `README.txt` (installed as
`Licenses\FFmpeg-README.txt`) and at https://www.gyan.dev/ffmpeg/builds/. Developers who build the game
themselves fetch the same files with `tools/fetch_deps.py` (same hashes). (Planned: a few-MB LGPL,
decoder-only build limited to what the game uses (Bink and H.264 video, Matroska), prepared in
[`tools/ffmpeg-lgpl`](tools/ffmpeg-lgpl/README.md) but not yet built.)

## Bundled in the installer

### Installer runtime (unpacked to a temporary folder while the installer runs)

| Component | Licence | Where | Source |
|:--|:--|:--|:--|
| Python 3.13 (exact version pinned in `tools/deps.lock.json`; with Tcl/Tk, OpenSSL, SQLite, zlib, bzip2, xz, libffi, expat, mpdecimal) | PSF-2.0 and the licences listed in its `LICENSE.txt` | bundled (from the python.org Windows installer) | https://www.python.org |
| Capstone 5.0 (Nguyen Anh Quynh and contributors) | BSD-3-Clause | fetched: `kit/toolkit/vendor/capstone`; bundled | https://github.com/capstone-engine/capstone |
| xboxrecomp toolkit (translator, XBE/XISO tools) | MIT, see `kit/toolkit/NOTICE` | repo: `kit/toolkit` | https://github.com/sp00nznet/xboxrecomp |
| RealBasicVSR (Kelvin C.K. Chan) and MMEditing (OpenMMLab) reference modules, and the inference model adapted from them | Apache-2.0 (licences and `NOTICE.txt` in the folder) | repo: `kit/enhancer/FMV/vendor/reference`, `kit/enhancer/FMV/realbasicvsr_model.py`; bundled | https://github.com/ckkelvinchan/RealBasicVSR, https://github.com/open-mmlab/mmagic |
| BasicSR RRDBNet (Xintao Wang and contributors), adapted | Apache-2.0 | repo: `kit/enhancer/rrdbnet.py`; bundled | https://github.com/XPixelGroup/BasicSR |
| Rajdhani (Indian Type Foundry) | SIL OFL 1.1 | repo: `kit/installer/data/fonts` | https://fonts.google.com/specimen/Rajdhani |
| Share Tech Mono (Carrois Type Design) | SIL OFL 1.1 | repo: `kit/installer/data/fonts` | https://fonts.google.com/specimen/Share+Tech+Mono |

## Downloaded on the user's PC, only on request

| Component | Licence | When |
|:--|:--|:--|
| Microsoft C++ Build Tools (compiler, linker, Windows SDK, MSVC runtime DLLs copied beside the game) | Microsoft's own licence terms (linked on the installer's Systems page; accepting them there starts Microsoft's bootstrapper) | when the installer finds no compiler and the user presses "Install for me" |
| Real-ESRGAN models, PyTorch, the RealBasicVSR checkpoint, DirectXTex texconv | BSD-3-Clause, BSD-style, Apache-2.0, MIT | only when the user chooses enhanced textures / videos |

## Not third-party, but worth stating

- Project icon and installer artwork: original work of the project, MIT.
- The screenshots in `docs/img` show the game and are not covered by the MIT licence (see [LEGAL.md](LEGAL.md)).
- The research wiki (`docs/wiki`): the project's own text, under the licence in `docs/wiki/LICENSE`.
