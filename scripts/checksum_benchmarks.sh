#!/bin/bash

set -eu -o pipefail

__have_cpu_feature() {
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

have_cpu_features() {
	local feature
	for feature; do
		__have_cpu_feature "$feature" || return 1
	done
}

make_and_test() {
	# Build the checksum program and tests.  Set the special test support
	# flag to get support for LIBDEFLATE_DISABLE_CPU_FEATURES.
	rm -rf build
	CFLAGS="$CFLAGS -DTEST_SUPPORT__DO_NOT_USE=1" \
		cmake -B build -G Ninja -DLIBDEFLATE_BUILD_TESTS=1 \
		"${EXTRA_CMAKE_FLAGS[@]}" > /dev/null
	cmake --build build > /dev/null

	# Run the checksum tests, for good measure.  (This isn't actually part
	# of the benchmarking.)
	./build/programs/test_checksums > /dev/null
}

__do_benchmark() {
	local impl="$1" speed
	shift
	local flags=("$@")

	speed=$(./build/programs/checksum "${CKSUM_FLAGS[@]}" \
		"${flags[@]}" -t "$FILE" | \
		grep -o '[0-9]\+ MB/s' | grep -o '[0-9]\+')
	printf "%-60s%-10s\n" "$CKSUM_NAME ($impl)" "$speed"
}

do_benchmark() {
	local impl="$1"

	CFLAGS="${EXTRA_CFLAGS[*]}" make_and_test
	if [ "$impl" = zlib ]; then
		__do_benchmark "$impl" "-Z"
	else
		__do_benchmark "libdeflate, $impl"
		if $ENABLE_32BIT; then
			CFLAGS="-m32 ${EXTRA_CFLAGS[*]}" make_and_test
			__do_benchmark "libdeflate, $impl, 32-bit"
		fi
	fi
}

sort_by_speed() {
	awk '{print $NF, $0}' | sort -nr | cut -f2- -d' '
}

disable_cpu_feature() {
	LIBDEFLATE_DISABLE_CPU_FEATURES+=",$1"
	shift
	if (( $# > 0 )); then
		EXTRA_CFLAGS+=("$@")
	fi
}

cleanup() {
	if $USING_TMPFILE; then
		rm "$FILE"
	fi
}

ARCH="$(uname -m)"
USING_TMPFILE=false
EXTRA_CMAKE_FLAGS=()
ENABLE_32BIT=false

trap cleanup EXIT

longopts="help"
longopts+=",cmake-flag:"
longopts+=",enable-32bit"

usage() {
	echo "Usage: $0 [--cmake-flag=FLAG]... [--enable-32bit] [FILE]"
}

if ! options=$(getopt -o "" -l "$longopts" -- "$@"); then
	usage 1>&2
	exit 1
fi
eval set -- "$options"
while (( $# >= 1 )); do
	case "$1" in
	--cmake-flag)
		EXTRA_CMAKE_FLAGS+=("$2")
		shift
		;;
	--enable-32bit)
		ENABLE_32BIT=true
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
		echo 1>&2 "Invalid option: '$1'"
		usage 1>&2
		exit 1
		;;
	esac
	shift
done

if (( $# == 0 )); then
	# Generate default test data file.
	FILE=$(mktemp -t checksum_testdata.XXXXXXXXXX)
	USING_TMPFILE=true
	echo "Generating 250 MB test file: $FILE"
	head -c 250000000 /dev/urandom > "$FILE"
elif (( $# == 1 )); then
	FILE="$1"
else
	usage 1>&2
	exit 1
fi

cat << EOF
Method                                                      Speed (MB/s)
------                                                      ------------
EOF

# CRC-32
CKSUM_NAME="CRC-32"
CKSUM_FLAGS=()
EXTRA_CFLAGS=()
export LIBDEFLATE_DISABLE_CPU_FEATURES=""
{
case $ARCH in
i386|x86_64)
	if have_cpu_features vpclmulqdq pclmulqdq avx512bw avx512vl; then
		do_benchmark "VPCLMULQDQ/AVX512/VL512"
		disable_cpu_feature zmm
		do_benchmark "VPCLMULQDQ/AVX512/VL256"
		disable_cpu_feature avx512vl "-mno-avx512vl"
		disable_cpu_feature avx512bw "-mno-avx512bw"
	fi
	if have_cpu_features vpclmulqdq pclmulqdq avx2; then
		do_benchmark "VPCLMULQDQ/AVX2"
		disable_cpu_feature vpclmulqdq "-mno-vpclmulqdq"
	fi
	if have_cpu_features pclmulqdq avx; then
		do_benchmark "PCLMULQDQ/AVX"
		disable_cpu_feature avx "-mno-avx"
	fi
	if have_cpu_features pclmulqdq; then
		do_benchmark "PCLMULQDQ"
		disable_cpu_feature pclmulqdq "-mno-pclmul"
	fi
	;;
aarch*)
	EXTRA_CFLAGS=("-march=armv8-a")
	if have_cpu_features pmull crc32 sha3; then
		do_benchmark "pmullx12_crc_eor3"
		disable_cpu_feature sha3
	fi
	if have_cpu_features pmull crc32; then
		do_benchmark "pmullx12_crc"
		disable_cpu_feature prefer_pmull
		do_benchmark "crc_pmullcombine"
	fi
	if have_cpu_features crc32; then
		do_benchmark "crc"
		disable_cpu_feature crc32
	fi
	if have_cpu_features pmull; then
		do_benchmark "pmull4x"
		disable_cpu_feature pmull
	fi
	;;
esac
do_benchmark "generic"
do_benchmark "zlib"
} | sort_by_speed

# Adler-32
CKSUM_NAME="Adler-32"
CKSUM_FLAGS=(-A)
EXTRA_CFLAGS=()
export LIBDEFLATE_DISABLE_CPU_FEATURES=""
echo
{
case $ARCH in
i386|x86_64)
	if have_cpu_features avx512bw avx512_vnni; then
		do_benchmark "AVX512VNNI/VL512"
		disable_cpu_feature zmm
		if have_cpu_features avx512vl; then
			do_benchmark "AVX512VNNI/VL256"
		fi
		disable_cpu_feature avx512_vnni "-mno-avx512vnni"
		disable_cpu_feature avx512bw "-mno-avx512bw"
	fi
	if have_cpu_features avx2 avx_vnni; then
		do_benchmark "AVX-VNNI"
		disable_cpu_feature avx_vnni "-mno-avxvnni"
	fi
	if have_cpu_features avx2; then
		do_benchmark "AVX2"
		disable_cpu_feature avx2 "-mno-avx2"
	fi
	if have_cpu_features sse2; then
		do_benchmark "SSE2"
		disable_cpu_feature sse2 "-mno-sse2"
	fi
	;;
arm*)
	if have_cpu_features neon; then
		do_benchmark "NEON"
		disable_cpu_feature neon "-mfpu=vfpv3"
	fi
	;;
aarch*)
	EXTRA_CFLAGS=("-march=armv8-a")
	if have_cpu_features asimd asimddp; then
		do_benchmark "DOTPROD"
		disable_cpu_feature dotprod
	fi
	if have_cpu_features asimd; then
		do_benchmark "NEON"
		disable_cpu_feature neon
		EXTRA_CFLAGS=("-march=armv8-a+nosimd")
	fi
	;;
esac
do_benchmark "generic"
do_benchmark "zlib"
} | sort_by_speed
