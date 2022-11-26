#!/bin/bash

set -eu -o pipefail

SCRIPTDIR="$(dirname "$0")"
BUILDDIR="$SCRIPTDIR/../build"
API_LEVEL=28
ARCH=arm64
export CFLAGS=${CFLAGS:-}
ENABLE_CRC=false
ENABLE_CRYPTO=false
NDKDIR=$HOME/android-ndk-r23b

usage() {
	cat << EOF
Usage: $0 [OPTION]...
Build libdeflate for Android.

  --api-level=LEVEL    Android API level to target (default: $API_LEVEL)
  --arch=ARCH          Architecture: arm32|arm64|x86|x86_64 (default: $ARCH)
  --enable-crc         Enable crc instructions
  --enable-crypto      Enable crypto instructions
  --ndkdir=NDKDIR      Android NDK directory (default: $NDKDIR)
EOF
}
if ! options=$(getopt -o '' \
	-l 'api-level:,arch:,enable-crc,enable-crypto,help,ndkdir:' -- "$@"); then
	usage 1>&2
	exit 1
fi

eval set -- "$options"

while [ $# -gt 0 ]; do
	case "$1" in
	--api-level)
		API_LEVEL="$2"
		shift
		;;
	--arch)
		ARCH="$2"
		shift
		;;
	--enable-crc)
		ENABLE_CRC=true
		;;
	--enable-crypto)
		ENABLE_CRYPTO=true
		;;
	--help)
		usage
		exit 0
		;;
	--ndkdir)
		NDKDIR="$2"
		shift
		;;
	--)
		shift
		break
		;;
	*)
		echo 1>&2 "Unknown option \"$1\""
		usage 1>&2
		exit 1
	esac
	shift
done

case "$ARCH" in
arm|arm32|aarch32|armeabi-v7a)
	ANDROID_ABI=armeabi-v7a
	if $ENABLE_CRC || $ENABLE_CRYPTO; then
		CFLAGS+=" -march=armv8-a"
		if $ENABLE_CRC; then
			CFLAGS+=" -mcrc"
		else
			CFLAGS+=" -mnocrc"
		fi
		if $ENABLE_CRYPTO; then
			CFLAGS+=" -mfpu=crypto-neon-fp-armv8"
		else
			CFLAGS+=" -mfpu=neon"
		fi
	fi
	;;
arm64|aarch64|arm64-v8a)
	ANDROID_ABI=arm64-v8a
	features=""
	if $ENABLE_CRC; then
		features+="+crc"
	fi
	if $ENABLE_CRYPTO; then
		features+="+crypto"
	fi
	if [ -n "$features" ]; then
		CFLAGS+=" -march=armv8-a$features"
	fi
	;;
x86)
	ANDROID_ABI=x86
	;;
x86_64)
	ANDROID_ABI=x86_64
	;;
*)
	echo 1>&2 "Unknown architecture: \"$ARCH\""
	usage 1>&2
	exit 1
esac

"$SCRIPTDIR"/cmake-helper.sh -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$NDKDIR"/build/cmake/android.toolchain.cmake \
	-DANDROID_ABI="$ANDROID_ABI" \
	-DANDROID_PLATFORM="$API_LEVEL" \
	-DLIBDEFLATE_BUILD_TESTS=1
cmake --build "$BUILDDIR"
