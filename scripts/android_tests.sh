#!/bin/bash
#
# Test libdeflate on a connected arm64 Android device.
# Requires the Android NDK (release 19 or later) and adb.

set -eu -o pipefail
cd "$(dirname "$0")/.."

if [ $# -ne 0 ]; then
	echo 1>&2 "Usage: $0"
	exit 2
fi

# Use NDKDIR if specified in environment, else use default value.
: "${NDKDIR:=$HOME/android-ndk-r23b}"
if [ ! -e "$NDKDIR" ]; then
	cat 1>&2 << EOF
Android NDK was not found in NDKDIR=$NDKDIR!  Set the
environmental variable NDKDIR to the location of your Android NDK installation.
EOF
	exit 1
fi

CLEANUP_CMDS=()
cleanup() {
	for cmd in "${CLEANUP_CMDS[@]}"; do
		eval "$cmd"
	done
}
trap cleanup EXIT

# Use TESTDATA if specified in environment, else generate it.
if [ -z "${TESTDATA:-}" ]; then
	# Generate default TESTDATA file.
	TESTDATA=$(mktemp -t libdeflate_testdata.XXXXXXXXXX)
	export TESTDATA
	CLEANUP_CMDS+=("rm -f '$TESTDATA'")
	find . '(' -name '*.c' -o -name '*.h' -o -name '*.sh' ')' \
		-exec cat '{}' ';' | head -c 1000000 > "$TESTDATA"
fi

TMPDIR=$(mktemp -d -t libdeflate_test.XXXXXXXXX)
CLEANUP_CMDS+=("rm -r '$TMPDIR'")

android_build_and_test() {
	echo "Running Android tests with $*"

	./scripts/android_build.sh --ndkdir="$NDKDIR" "$@" > /dev/null
	adb push "$TESTDATA" ./scripts/exec_tests.sh \
		./build/programs/{benchmark,test_*} /data/local/tmp/ > /dev/null

	# Note: adb shell always returns 0, even if the shell command fails...
	adb shell "cd /data/local/tmp && WRAPPER= TESTDATA=$(basename "$TESTDATA") sh exec_tests.sh" \
		> "$TMPDIR/adb.out"
	if ! grep -q "exec_tests finished successfully" "$TMPDIR/adb.out"; then
		echo 1>&2 "Android test failure!  adb shell output:"
		cat "$TMPDIR/adb.out"
		exit 1
	fi
}

android_build_and_test --arch=arm32
android_build_and_test --arch=arm32 --enable-crc
android_build_and_test --arch=arm64
android_build_and_test --arch=arm64 --enable-crc
android_build_and_test --arch=arm64 --enable-crypto
android_build_and_test --arch=arm64 --enable-crc --enable-crypto

echo "Android tests passed"
