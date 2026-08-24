#!/usr/bin/env bash
# ============================================================================
# fetch-ffmpeg.sh - download, configure and build FFmpeg 9.0.1 as the static
# library set used by ffmpegkernel.
#
# Configure line is exactly the one this project was validated with (also
# recorded in build/ffmpeg-9.0.1/config.h after the run): h264 decode + mov
# demux + h264 parser + swscale + pthreads, everything else disabled.
#
# Licensing: no --enable-gpl / --enable-nonfree is used, so the produced
# libraries are LGPL-2.1-or-later. See COPYING.LGPLv2.1 and README.md.
#
# Verify the tarball yourself against the hashes published at
# https://ffmpeg.org/download.html before running sensitive workloads.
# ============================================================================
set -euxo pipefail

FFVER=9.0.1
FFTAR="ffmpeg-${FFVER}.tar.xz"
URL="https://ffmpeg.org/releases/${FFTAR}"

mkdir -p build
cd build

if [ ! -d "ffmpeg-${FFVER}" ]; then
    if [ ! -f "${FFTAR}" ]; then
        echo "downloading ${URL} (ffmpeg.org can be slow; be patient)"
        curl -fL --retry 3 -o "${FFTAR}.part" "${URL}"
        mv "${FFTAR}.part" "${FFTAR}"
    fi
    tar xf "${FFTAR}"
fi

cd ffmpeg-${FFVER}

./configure \
    --target-os=none \
    --disable-everything \
    --enable-decoder=h264 \
    --enable-demuxer=mov \
    --enable-parser=h264 \
    --enable-swscale \
    --enable-pthreads \
    --disable-network \
    --disable-iconv \
    --disable-zlib \
    --disable-bzlib \
    --prefix=/tmp/ffout

make -j"$(nproc)" libavformat/libavformat.a \
                libavcodec/libavcodec.a \
                libavutil/libavutil.a \
                libswscale/libswscale.a

echo "OK: FFmpeg ${FFVER} static libs built under build/ffmpeg-${FFVER}/"
