# LGPL, decoder-only FFmpeg (planned replacement for the downloaded GPL build)

**Status: prepared, not yet used.** The installer downloads the gyan.dev "full" shared build of FFmpeg
9.0.2 on the player's PC (GPL-3.0, about 245 MB unpacked, about 100 external libraries); it is not bundled. This folder holds the reproducible
build that replaces it. It has not been built yet: it needs the FFmpeg source tarball and MSYS2, which
the maintainers have not fetched.

## What the game uses

From [`kit/source/game/src/bink_native.c`](../../kit/source/game/src/bink_native.c) and
[`kit/source/game/CMakeLists.txt`](../../kit/source/game/CMakeLists.txt):

| Part | Used for |
|:--|:--|
| `avcodec`: decoder `bink` | the game's Bink 1 cutscene video (packets are fed by the game; Bink audio is the game's own code) |
| `avcodec`: decoder `h264`, parser `h264` | enhanced cutscenes (H.264 in Matroska, made by the optional Enhancer) |
| `avformat`: demuxer `matroska`, protocol `file` | opening an enhanced cutscene |
| `swscale` | converting decoded frames to the game's surface formats |
| `avutil` | shared by all of the above |

Nothing else: no encoders, muxers, filters, devices, network or external libraries. `swresample` is not
used by the game (the full build only ships it as a dependency).

## Build

`build.sh` in this folder, in MSYS2 from a VS x64 Native Tools prompt, from the unmodified
`ffmpeg-9.0.2.tar.xz` from ffmpeg.org. Configure line (also written into the build's `README.txt`):

```
--toolchain=msvc --arch=x86_64 --target-os=win64 --enable-shared --disable-static
--disable-gpl --disable-version3 --disable-nonfree --disable-autodetect --disable-network --disable-doc
--disable-programs --disable-debug --disable-everything
--enable-decoder=bink,h264 --enable-parser=h264 --enable-demuxer=matroska --enable-protocol=file
--disable-avdevice --disable-avfilter --disable-swresample
```

The result is LGPL-2.1-or-later and a few MB. The DLL names stay `avcodec-63`, `avformat-63`,
`avutil-61`, `swscale-10` (same FFmpeg version).

## Switching over (checklist)

1. Fetch `ffmpeg-9.0.2.tar.xz` from https://ffmpeg.org/releases/, check its signature, record its SHA-256
   in `build.sh`; run `build.sh`; record every output file's SHA-256 and size in `kit/installer/data/ffmpeg.json`
   (replace the GPL pin, dest `kit/source/game/vendor/ffmpeg-9.0.2-lgpl-shared`), or add a second pin and point
   `tools/fetch_deps.py` (it reads `LOCK['ffmpeg']` from that file) and `kit/installer/ffmpeg_fetch.py` at it.
2. Point `FFMPEG` / `FFMPEG_DLLS` in `kit/installer/install.py` and `FFMPEG_DIR` / the DLL list and the
   licence file name in `kit/source/game/CMakeLists.txt` at the new folder (drop `swresample-7`).
3. The optional video Enhancer encodes with libx264 (GPL) through `ffmpeg.exe` / `ffprobe.exe`
   (`kit/enhancer/FMV/fmv_pipeline.py`), which the decoder-only build does not provide. Make the Enhancer's
   setup download a pinned ffmpeg.exe / ffprobe.exe on the player's PC when they ask for enhanced videos
   (like PyTorch and the models today), instead of bundling them.
4. Decide how the LGPL build reaches players: bundled in the installer (then attach the exact source
   tarball to each GitHub release, the LGPL source offer) or downloaded on the player's PC like the GPL
   build today (then publish the build and its source where `ffmpeg_fetch.py` downloads from). Update
   THIRD-PARTY-NOTICES.md, LEGAL.md (Licences), the installer size in the README, and
   `kit/defaults/Docs/Credits and licences.txt`.
5. Rebuild, run the installer tests and one end-to-end install, and play a cutscene with and without the
   enhanced videos.
