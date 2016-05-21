#!/bin/sh

set -e

NDKDIR=/opt/android-ndk

make -j benchmark \
	CC="$NDKDIR/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin/arm-linux-androideabi-gcc" \
	CFLAGS="--sysroot=$NDKDIR/platforms/android-12/arch-arm -march=armv7-a -fPIC -pie -mfpu=neon -mfloat-abi=softfp"

adb push benchmark /data/local/tmp
adb push $HOME/data/testdata  /data/local/tmp
adb shell /data/local/tmp/benchmark "$@" /data/local/tmp/testdata
