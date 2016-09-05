#!/bin/bash
#
# android_test.sh
#
# Build and run the libdeflate benchmark program on Android
#

set -e

FILE="$HOME/data/testdata"
ARCH="arm32"
COMPILER="gcc"
NDKDIR="/opt/android-ndk"
DATADIR="/data/local/tmp"

usage() {
	cat << EOF
Usage: $0 [OPTION]... -- [BENCHMARK_ARG]...
Build and run the libdeflate benchmark program on Android

  --file=FILE          Input data file (default: $FILE)
  --arch=ARCH          Architecture: arm32|arm64 (default: $ARCH)
  --compiler=COMPILER  Compiler: gcc|clang (default: $COMPILER)
  --ndkdir=NDKDIR      Android NDK directory (default: $NDKDIR)
  --datadir=DATADIR    Data directory on Android (default: $DATADIR)
EOF
}

if ! options=$(getopt -o '' \
	-l 'file:,arch:,compiler:,ndkdir:,datadir:,help' -- "$@"); then
	usage
	exit 1
fi

eval set -- "$options"

while [ $# -gt 0 ]; do
	case "$1" in
	--file)
		FILE="$2"
		shift
		;;
	--arch)
		ARCH="$2"
		shift
		;;
	--compiler)
		COMPILER="$2"
		shift
		;;
	--ndkdir)
		NDKDIR="$2"
		shift
		;;
	--datadir)
		DATADIR="$2"
		shift
		;;
	--help)
		usage
		exit 0
		;;
	--)
		shift
		break
		;;
	*)
		echo 1>&2 "Unknown option \"$1\""
		usage
		exit 1
	esac
	shift
done

CFLAGS="-fPIC -pie"

case "$ARCH" in
arm|arm32|aarch32)
	GCC_TOOLCHAIN="arm-linux-androideabi-4.9"
	CLANG_TARGET="armv7-none-linux-androideabi"
	CFLAGS+=" -march=armv7-a -mfpu=neon -mfloat-abi=softfp"
	CFLAGS+=" --sysroot=\"$NDKDIR/platforms/android-12/arch-arm\""
	;;
arm64|aarch64)
	GCC_TOOLCHAIN="aarch64-linux-android-4.9"
	CLANG_TARGET="aarch64-none-linux-android"
	CFLAGS+=" --sysroot=\"$NDKDIR/platforms/android-21/arch-arm64\""
	;;
*)
	echo 1>&2 "Unknown architecture: \"$ARCH\""
	usage
	exit 1
esac

case "$COMPILER" in
gcc)
	CC="\"$NDKDIR/toolchains/$GCC_TOOLCHAIN/prebuilt/linux-x86_64/bin/${GCC_TOOLCHAIN%-*}-gcc\""
	CFLAGS+=" -pie"
	;;
clang)
	CC="\"$NDKDIR/toolchains/llvm/prebuilt/linux-x86_64/bin/clang\""
	CFLAGS+=" -target \"$CLANG_TARGET\""
	CFLAGS+=" -gcc-toolchain \"$NDKDIR/toolchains/$GCC_TOOLCHAIN/prebuilt/linux-x86_64\""
	;;
*)
	echo 1>&2 "Unknown compiler: \"$COMPILER\""
	usage
	exit 1
esac

make -j$(grep -c processor /proc/cpuinfo) benchmark CC="$CC" CFLAGS="$CFLAGS"
adb push benchmark "$DATADIR"

FILENAME="$(basename "$FILE")"
if [ -z "$(adb shell "[ -e \"$DATADIR/$FILENAME\" ] && echo 1")" ]; then
	adb push "$FILE" "$DATADIR/$FILENAME"
fi
adb shell "$DATADIR/benchmark" "$@" "$DATADIR/$FILENAME"
