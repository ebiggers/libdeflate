#!/bin/bash

set -eu

ARCH="arm32"
COMPILER="gcc"
NDKDIR="/opt/android-ndk"
ENABLE_NEON=false
ENABLE_CRYPTO=false

usage() {
	cat << EOF
Usage: $0 [OPTION]... -- [BENCHMARK_PROGRAM_ARG]...
Build the libdeflate test programs for Android

  --arch=ARCH          Architecture: arm32|arm64 (default: $ARCH)
  --compiler=COMPILER  Compiler: gcc|clang (default: $COMPILER)
  --ndkdir=NDKDIR      Android NDK directory (default: $NDKDIR)
  --enable-neon        Enable NEON instructions
  --enable-crypto      Enable crypto extensions (implies NEON too)
EOF
}

if ! options=$(getopt -o '' \
	-l 'arch:,compiler:,ndkdir:,enable-neon,enable-crypto,help' -- "$@"); then
	usage
	exit 1
fi

eval set -- "$options"

while [ $# -gt 0 ]; do
	case "$1" in
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
	--enable-neon)
		ENABLE_NEON=true
		;;
	--enable-crypto)
		ENABLE_CRYPTO=true
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

CFLAGS="-fPIC"

case "$ARCH" in
arm|arm32|aarch32)
	GCC_TOOLCHAIN="arm-linux-androideabi-4.9"
	CLANG_TARGET="armv7-none-linux-androideabi"
	if $ENABLE_CRYPTO; then
		CFLAGS+=" -march=armv7-a -mfloat-abi=softfp -mfpu=crypto-neon-fp-armv8"
	elif $ENABLE_NEON; then
		CFLAGS+=" -march=armv7-a -mfloat-abi=softfp -mfpu=neon"
	else
		CFLAGS+=" -march=armv6"
	fi
	CFLAGS+=" --sysroot=\"$NDKDIR/platforms/android-12/arch-arm\""
	;;
arm64|aarch64)
	GCC_TOOLCHAIN="aarch64-linux-android-4.9"
	CLANG_TARGET="aarch64-none-linux-android"
	if $ENABLE_CRYPTO; then
		CFLAGS+=" -march=armv8-a+crypto"
	else
		CFLAGS+=" -march=armv8-a"
	fi
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

make -j$(grep -c processor /proc/cpuinfo) test_programs \
	CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="-pie"
