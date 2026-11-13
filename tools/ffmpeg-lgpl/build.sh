#!/usr/bin/env bash
# Reproducible LGPL, decoder-only FFmpeg for the game (see README.md in this folder).
#
# Run in an MSYS2 shell started from an "x64 Native Tools Command Prompt for VS" (so cl.exe and link.exe
# are on PATH), with: pacman -S --needed make diffutils nasm pkgconf
#
#   tools/ffmpeg-lgpl/build.sh <ffmpeg-9.0.2.tar.xz> <output dir>
#
# The tarball's SHA-256 must match FFMPEG_SHA256 below (ffmpeg.org also publishes a .asc signature).
set -euo pipefail
FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=${FFMPEG_SHA256:-UNSET}          # recorded in tools/deps.lock.json once the tarball is fetched
tarball=$1; out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")

got=$(sha256sum "$tarball" | cut -d' ' -f1)
if [ "$FFMPEG_SHA256" != UNSET ] && [ "$got" != "$FFMPEG_SHA256" ]; then
    echo "tarball sha256 $got does not match $FFMPEG_SHA256" >&2; exit 1
fi
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
tar -xJf "$tarball" -C "$work"
cd "$work/ffmpeg-$FFMPEG_VERSION"

# Exactly what MechAssault 2 PC Recomp uses (kit/source/game/src/bink_native.c):
#   decoders  bink (the game's Bink 1 cutscene video; audio is decoded by the game itself), h264 (enhanced
#             cutscenes, which the optional Enhancer writes as H.264 in Matroska)
#   parser    h264        demuxer  matroska        protocol  file
#   libraries avcodec, avformat, avutil, swscale (colour conversion to the game's surface formats)
# No encoders, muxers, filters, devices, network, external libraries or GPL/version3 code.
./configure \
    --toolchain=msvc --arch=x86_64 --target-os=win64 \
    --prefix="$out" --enable-shared --disable-static \
    --disable-gpl --disable-version3 --disable-nonfree \
    --disable-autodetect --disable-network --disable-doc --disable-programs --disable-debug \
    --disable-everything \
    --enable-decoder=bink,h264 --enable-parser=h264 --enable-demuxer=matroska --enable-protocol=file \
    --disable-avdevice --disable-avfilter --disable-swresample \
    --extra-cflags=-Brepro --extra-ldflags=-Brepro
make -j"$(nproc)"
make install
cp COPYING.LGPLv2.1 "$out/LICENSE.txt"
{
    echo "FFmpeg $FFMPEG_VERSION, LGPL-2.1-or-later, decoder-only shared build for MechAssault 2 PC Recomp."
    echo "Source: https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz (sha256 $got), unmodified."
    echo "Built with tools/ffmpeg-lgpl/build.sh from https://github.com/Mazillion/MechAssault2-PC-Recomp"
    echo "Configuration:"
    ./ffbuild/version.sh . /dev/stdout 2>/dev/null || true
    grep -m1 'FFMPEG_CONFIGURATION' config.h
} > "$out/README.txt"
echo "done: $out"
