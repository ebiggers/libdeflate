#!/bin/bash
#
# Test script for libdeflate
#
#	Usage: ./tools/run_tests.sh [TESTGROUP]... [-TESTGROUP]...
#
# By default all tests are run, but it is possible to explicitly include or
# exclude specific test groups.
#

set -eu -o pipefail
cd "$(dirname "$0")/.."

TESTGROUPS=(all)

set_test_groups() {
	TESTGROUPS=("$@")
	local have_exclusion=0
	local have_all=0
	for group in "${TESTGROUPS[@]}"; do
		if [[ $group == -* ]]; then
			have_exclusion=1
		elif [[ $group == all ]]; then
			have_all=1
		fi
	done
	if (( have_exclusion && !have_all )); then
		TESTGROUPS=(all "${TESTGROUPS[@]}")
	fi
}

if [ $# -gt 0 ]; then
	set_test_groups "$@"
fi

SMOKEDATA="${SMOKEDATA:=$HOME/data/smokedata}"
if [ ! -e "$SMOKEDATA" ]; then
	echo "SMOKEDATA (value: $SMOKEDATA) does not exist.  Set the" \
	      "environmental variable SMOKEDATA to a file to use in" \
	      "compression/decompression tests." 1>&2
	exit 1
fi

NDKDIR="${NDKDIR:=/opt/android-ndk}"

FILES=("$SMOKEDATA" ./tools/exec_tests.sh benchmark test_checksums)
EXEC_TESTS_CMD="WRAPPER= SMOKEDATA=\"$(basename $SMOKEDATA)\" sh exec_tests.sh"
NPROC=$(grep -c processor /proc/cpuinfo)
VALGRIND="valgrind --quiet --error-exitcode=100 --leak-check=full --errors-for-leak-kinds=all"
SANITIZE_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined,integer"

TMPFILE="$(mktemp)"
trap "rm -f \"$TMPFILE\"" EXIT

###############################################################################

rm -f run_tests.log
exec >  >(tee -ia run_tests.log)
exec 2> >(tee -ia run_tests.log >&2)

TESTS_SKIPPED=0
log_skip() {
	log "[WARNING, TEST SKIPPED]: $@"
	TESTS_SKIPPED=1
}

log() {
	echo "[$(date)] $@"
}

run_cmd() {
	log "$@"
	"$@" > /dev/null
}

test_group_included() {
	local included=0 group
	for group in "${TESTGROUPS[@]}"; do
		if [ "$group" = "$1" ]; then
			included=1 # explicitly included
			break
		fi
		if [ "$group" = "-$1" ]; then
			included=0 # explicitly excluded
			break
		fi
		if [ "$group" = "all" ]; then # implicitly included
			included=1
		fi
	done
	if (( included )); then
		log "Starting test group: $1"
	fi
	(( included ))
}

###############################################################################

native_build_and_test() {
	make "$@" -j$NPROC all test_programs > /dev/null
	WRAPPER="$WRAPPER" SMOKEDATA="$SMOKEDATA" sh ./tools/exec_tests.sh \
			> /dev/null
}

native_tests() {
	test_group_included native || return 0
	local compiler cflags compilers=(gcc clang)
	shopt -s nullglob
	compilers+=(/usr/bin/gcc-[0-9]*)
	compilers+=(/opt/gcc*/bin/gcc)
	shopt -u nullglob
	for compiler in ${compilers[@]}; do
		for cflags in "" "-march=native" "-m32"; do
			if [ "$compiler" = "/usr/bin/gcc-4.8" -a \
			     "$cflags" = "-m32" ]; then
				continue
			fi
			log "Running tests with CC=$compiler," \
				"CFLAGS=$cflags"
			WRAPPER= native_build_and_test \
				CC=$compiler CFLAGS="$cflags -Werror"
		done
	done

	log "Running tests with Valgrind"
	WRAPPER="$VALGRIND" native_build_and_test

	log "Running tests with undefined behavior sanitizer"
	WRAPPER= native_build_and_test CC=clang CFLAGS="$SANITIZE_CFLAGS"
}

###############################################################################

android_build() {
	run_cmd ./tools/android_build.sh --ndkdir="$NDKDIR" "$@"
}

android_build_and_test() {
	android_build "$@"
	run_cmd adb push "${FILES[@]}" /data/local/tmp/

	# Note: adb shell always returns 0, even if the shell command fails...
	log "adb shell \"cd /data/local/tmp && $EXEC_TESTS_CMD\""
	adb shell "cd /data/local/tmp && $EXEC_TESTS_CMD" > "$TMPFILE"
	if ! grep -q "exec_tests finished successfully" "$TMPFILE"; then
		log "Android test failure!  adb shell output:"
		cat "$TMPFILE"
		return 1
	fi
}

android_tests() {
	local compiler

	test_group_included android || return 0
	if [ ! -e $NDKDIR ]; then
		log_skip "Android NDK was not found in NDKDIR=$NDKDIR!" \
		         "If you want to run the Android tests, set the" \
			 "environmental variable NDKDIR to the location of" \
			 "your Android NDK installation"
		return 0
	fi

	if ! type -P adb > /dev/null; then
		log_skip "adb (android-tools) is not installed"
		return 0
	fi

	if ! adb devices | grep -q 'device$'; then
		log_skip "No Android device is currently attached"
		return 0
	fi

	for compiler in gcc clang; do
		android_build_and_test --arch=arm --compiler=$compiler

		android_build_and_test --arch=arm --compiler=$compiler \
				       --disable-neon

		# arm64: currently compiled but not run
		android_build --arch=arm64 --compiler=$compiler
	done
}

###############################################################################

mips_tests() {
	test_group_included mips || return 0
	if ! ping -c 1 dd-wrt > /dev/null; then
		log_skip "Can't run MIPS tests: dd-wrt system not available"
		return 0
	fi
	run_cmd ./tools/mips_build.sh
	run_cmd scp "${FILES[@]}" root@dd-wrt:
	run_cmd ssh root@dd-wrt "$EXEC_TESTS_CMD"
}

###############################################################################

windows_tests() {
	local arch

	test_group_included windows || return 0

	# Windows: currently compiled but not run
	for arch in i686 x86_64; do
		local compiler=${arch}-w64-mingw32-gcc
		if ! type -P $compiler > /dev/null; then
			log_skip "$compiler not found"
			continue
		fi
		run_cmd make CC=$compiler CFLAGS=-Werror -j$NPROC \
			all test_programs
	done
}

###############################################################################

static_analysis_tests() {
	test_group_included static_analysis || return 0
	if ! type -P scan-build > /dev/null; then
		log_skip "clang static analyzer (scan-build) not found"
		return 0
	fi
	run_cmd scan-build --status-bugs make -j$NPROC all test_programs
}

###############################################################################

gzip_tests() {
	test_group_included gzip || return 0

	local gzip gunzip
	run_cmd make -j$NPROC gzip gunzip
	for gzip in "$PWD/gzip" /usr/bin/gzip; do
		for gunzip in "$PWD/gunzip" /usr/bin/gunzip; do
			log "Running gzip program tests with GZIP=$gzip," \
				"GUNZIP=$gunzip"
			GZIP="$gzip" GUNZIP="$gunzip" SMOKEDATA="$SMOKEDATA" \
				./tools/gzip_tests.sh
		done
	done

	log "Running gzip program tests with Valgrind"
	GZIP="$VALGRIND $PWD/gzip" GUNZIP="$VALGRIND $PWD/gunzip" \
		SMOKEDATA="$SMOKEDATA" ./tools/gzip_tests.sh

	log "Running gzip program tests with undefined behavior sanitizer"
	run_cmd make -j$NPROC CC=clang CFLAGS="$SANITIZE_CFLAGS" gzip gunzip
	GZIP="$PWD/gzip" GUNZIP="$PWD/gunzip" \
		SMOKEDATA="$SMOKEDATA" ./tools/gzip_tests.sh
}

###############################################################################

log "Starting libdeflate tests"
log "	TESTGROUPS=(${TESTGROUPS[@]})"
log "	SMOKEDATA=$SMOKEDATA"
log "	NDKDIR=$NDKDIR"

native_tests
android_tests
mips_tests
windows_tests
static_analysis_tests
gzip_tests

if (( TESTS_SKIPPED )); then
	log "No tests failed, but some tests were skipped.  See above."
else
	log "All tests passed!"
fi
