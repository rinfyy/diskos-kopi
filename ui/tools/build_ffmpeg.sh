#!/bin/sh
# Builds the small, static FFmpeg that Videos (video_disc.c) uses for MP4 / MKV / AVI / MOV / WebM:
#   tools/build_ffmpeg.sh <ffmpeg source dir> <install dir> [cross-prefix]
# FFmpeg 7.1 (git tag n7.1), LGPL-2.1-or-later, built from unmodified sources with only these parts:
#   video: H.264, H.265/HEVC, MPEG-4 Part 2 (Xvid/DivX), MPEG-1/2, VP8, VP9
#   audio: AAC, MP3, MP2, AC-3 / E-AC-3, Opus, Vorbis, FLAC
#   files: MP4 / MOV / M4V (mov), MKV / WebM (matroska), AVI, MPEG-PS / TS (mpegvideo: to recognise MPEG-PS pictures)
# plus swscale (picture size and colours) and swresample (sound to 44.1 kHz). No assembly (the player's MIPS build
# uses the plain C paths), no external libraries, no network, no programs.
set -e
SRC=$1; OUT=$2; CROSS=$3
[ -n "$SRC" ] && [ -n "$OUT" ] || { echo "usage: $0 <ffmpeg src> <install dir> [cross-prefix]"; exit 2; }
BUILD=$OUT/build; mkdir -p "$BUILD"; cd "$BUILD"
X=""
if [ -n "$CROSS" ]; then
  X="--enable-cross-compile --target-os=linux --arch=mips --cc=${CROSS}gcc --ar=${CROSS}ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip
     --disable-mipsdsp --disable-mipsdspr2 --disable-msa --disable-mmi --disable-mipsfpu --disable-mips32r6 --disable-mips64r6 --disable-mips64r2 --disable-loongson2 --disable-loongson3"
fi
"$SRC/configure" --prefix="$OUT" $X \
  --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug --disable-network \
  --disable-avdevice --disable-avfilter --disable-asm --disable-inline-asm --enable-static --disable-shared \
  --enable-avformat --enable-avcodec --enable-swscale --enable-swresample --enable-pthreads \
  --enable-protocol=file \
  --enable-demuxer=mov,matroska,avi,mpegps,mpegts,mpegvideo \
  --enable-decoder=h264,hevc,mpeg4,mpeg1video,mpeg2video,vp8,vp9,aac,aac_latm,mp3,mp3float,mp2,mp2float,ac3,eac3,opus,vorbis,flac \
  --enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9,aac,aac_latm,mpegaudio,ac3,opus,vorbis,flac \
  --enable-bsf=vp9_superframe_split \
  --optflags="-O2" --extra-cflags="-ffunction-sections -fdata-sections"
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "ffmpeg: installed to $OUT"
