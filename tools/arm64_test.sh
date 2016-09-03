#!/bin/sh

set -e

NDKDIR=/opt/android-ndk

make -j benchmark \
	CC="$NDKDIR/toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64/bin/aarch64-linux-android-gcc" \
	CFLAGS="--sysroot=$NDKDIR/platforms/android-21/arch-arm64 -fPIC -pie"

adb push benchmark /data/local/tmp
if [ -z "$(adb shell '[ -e /data/local/tmp/testdata ] && echo 1')" ]; then
	adb push $HOME/data/testdata  /data/local/tmp
fi
adb shell /data/local/tmp/benchmark "$@" /data/local/tmp/testdata
