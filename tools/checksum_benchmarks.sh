#!/bin/bash

set -eu

if [ $# -eq 0 ]; then
	FILE="$HOME/data/silesia"
	[ ! -e "$FILE" ] && FILE="$HOME/silesia"
	[ ! -e "$FILE" ] && FILE="$HOME/data/testdata"
	[ ! -e "$FILE" ] && FILE="$HOME/testdata"
	echo "Using default FILE: $FILE"
	echo
elif [ $# -eq 1 ]; then
	FILE="$1"
else
	echo "Usage: $0 [FILE]" 1>&2
	exit 1
fi

ARCH="$(uname -m)"

if [ -n "$(git status -s | egrep 'adler32_impl.h|crc32_impl.h')" ]; then
	echo "This script will overwrite adler32_impl.h and crc32_impl.h," \
		"which have uncommitted changes.  Refusing to run." 1>&2
	exit 1
fi

have_cpu_feature() {
	local feature="$1"
	local tag
	case $ARCH in
	arm*|aarch*)
		tag="Features"
		;;
	*)
		tag="flags"
		;;
	esac
	grep -q "^$tag"$'[ \t]'"*:.*\<$feature\>" /proc/cpuinfo
}

make_and_test() {
	make "$@" checksum test_checksums > /dev/null
	./test_checksums > /dev/null
}

__do_benchmark() {
	local impl="$1" speed
	shift
	local flags="$CKSUM_FLAGS $*"

	speed=$(./checksum $flags -t "$FILE" | \
		grep -o '[0-9]\+ MB/s' | grep -o '[0-9]\+')
	printf "%-45s%-10s\n" "$CKSUM_NAME ($impl)" "$speed"
}

do_benchmark() {
	local impl="$1"

	if [ "$impl" = zlib ]; then
		__do_benchmark "$impl" "-Z"
	else
		make_and_test CFLAGS="$EXTRA_CFLAGS"
		__do_benchmark "libdeflate, $impl"
		if [ $ARCH = x86_64 ]; then
			make_and_test CFLAGS="-m32 $EXTRA_CFLAGS"
			__do_benchmark "libdeflate, $impl, 32-bit"
		fi
	fi
}

sort_by_speed() {
	awk '{print $NF, $0}' | sort -nr | cut -f2- -d' '
}

disable_impl() {
	local name="$1"
	local extra_cflags="$2"

	sed -i '/^\#ifdef DISPATCH_'"$name"'$/aif (0)' lib/*/{adler,crc}32_impl.h
	EXTRA_CFLAGS+=" $extra_cflags"
}

restore_impls() {
	git checkout -f lib/*/{adler,crc}32_impl.h
}

trap restore_impls EXIT

cat << EOF
Method                                       Speed (MB/s)
------                                       ------------
EOF

# CRC-32
CKSUM_NAME="CRC-32"
CKSUM_FLAGS=""
EXTRA_CFLAGS=""
{
case $ARCH in
i386|x86_64)
	if have_cpu_feature pclmulqdq && have_cpu_feature avx; then
		do_benchmark "PCLMUL/AVX"
		disable_impl "PCLMUL_AVX" "-mno-avx"
	fi
	if have_cpu_feature pclmulqdq; then
		do_benchmark "PCLMUL"
		disable_impl "PCLMUL" "-mno-pclmul"
	fi
	;;
esac
do_benchmark "generic"
do_benchmark "zlib"
} | sort_by_speed

# Adler-32
CKSUM_NAME="Adler-32"
CKSUM_FLAGS="-A"
EXTRA_CFLAGS=""
echo
{
case $ARCH in
i386|x86_64)
	if have_cpu_feature avx2; then
		do_benchmark "AVX2"
		disable_impl "AVX2" "-mno-avx2"
	fi
	if have_cpu_feature sse2; then
		do_benchmark "SSE2"
		disable_impl "SSE2" "-mno-sse2"
	fi
	;;
arm*|aarch*)
	if have_cpu_feature neon; then
		do_benchmark "NEON"
		disable_impl "NEON" "-mfpu=vfpv3"
	fi
	;;
esac
do_benchmark "generic"
do_benchmark "zlib"
} | sort_by_speed
