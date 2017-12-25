#!/bin/bash

set -eu

ARCH=`uname -m`
case $ARCH in
    x86 | x86_64)
        CPU_CRC_FEATURE=pclmulqdq
        CPU_ADLER_FEATURE=sse2
        EXTRA_CFLAGS=""
        ;;
    arm*)
        CPU_CRC_FEATURE=pmull
        CPU_ADLER_FEATURE=asimd
        EXTRA_CFLAGS="-mfpu=crypto-neon-fp-armv8"
        ;;
    aarch64)
        CPU_CRC_FEATURE=pmull
        CPU_ADLER_FEATURE=asimd
        EXTRA_CFLAGS="-march=armv8-a+crypto"
        ;;
    *)
        echo "Unsupported CPU arch"
        exit 1
esac

if [ $# -eq 0 ]; then
	FILE="$HOME/data/silesia"
	echo "Using default FILE: $FILE"
	echo
elif [ $# -eq 1 ]; then
	FILE="$1"
else
	echo "Usage: $0 [FILE]" 1>&2
	exit 1
fi


if ! grep -q "\<${CPU_ADLER_FEATURE}\>" /proc/cpuinfo; then
	echo "This script must be run on an $ARCH CPU with ${CPU_ADLER_FEATURE}" 1>&2
	exit 1
fi

if [ -n "$(git status -s | egrep 'adler32.c|crc32.c')" ]; then
	echo "This script will overwrite adler32.c and crc32.c, which" \
		"have uncommitted changes.  Refusing to run." 1>&2
	exit 1
fi

do_benchmark() {
	method="$1"
	shift
	speed=$(./checksum "$@" -t "$FILE" | \
		grep -o '[0-9]\+ MB/s' | grep -o '[0-9]\+')
	printf "%-36s%-10s\n" "$method" "$speed"
}

sort_by_speed() {
	awk '{print $NF, $0}' | sort -nr | cut -f2- -d' '
}

cat << EOF
Method                              Speed (MB/s)
------                              ------------
EOF

# CRC-32
(
if grep -q '\<pclmulqdq\>' /proc/cpuinfo; then
	make checksum > /dev/null
	do_benchmark "CRC-32 (libdeflate, PCLMUL/AVX)"
	sed -i '/^\#if NEED_PCLMUL_IMPL && !defined(__AVX__)/,/^\#endif$/d' lib/crc32.c
	make checksum > /dev/null
	do_benchmark "CRC-32 (libdeflate, PCLMUL)"
fi

if grep -q '\<pmull\>' /proc/cpuinfo; then
	make CFLAGS=${EXTRA_CFLAGS} checksum > /dev/null
	do_benchmark "CRC-32 (libdeflate, NEON-MULL)"
fi

sed -i '/^#if defined(__PCLMUL__)/,/^\#endif$/d' lib/crc32.c
sed -i '/^#if defined(__ARM_FEATURE_CRYPTO)/,/^\#endif$/d' lib/crc32.c
make checksum > /dev/null
do_benchmark "CRC-32 (libdeflate, generic)"
git checkout -f lib/crc32.c > /dev/null
do_benchmark "CRC-32 (zlib)" -Z
) | sort_by_speed

# Adler-32
echo
(
if grep -q '\<avx2\>' /proc/cpuinfo; then
	make checksum > /dev/null
	do_benchmark "Adler-32 (libdeflate, AVX2)" -A
fi
sed -i '/^#if defined(__AVX2__)/,/^\#endif$/d' lib/adler32.c
make CFLAGS=${EXTRA_CFLAGS} checksum > /dev/null
do_benchmark "Adler-32 (libdeflate, SIMD(${CPU_ADLER_FEATURE}))" -A
sed -i '/^#ifdef __SSE2__/,/^\#endif$/d' lib/adler32.c
sed -i '/^#ifdef __ARM_NEON/,/^\#endif$/d' lib/adler32.c
make checksum > /dev/null
do_benchmark "Adler-32 (libdeflate, generic)" -A
git checkout -f lib/adler32.c > /dev/null
do_benchmark "Adler-32 (zlib)" -A -Z
) | sort_by_speed
