#!/bin/bash
#
# Test script for libdeflate
#
#	Usage: ./scripts/run_tests.sh [TESTGROUP]... [-TESTGROUP]...
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


TMPFILE="$(mktemp)"
USING_TMP_SMOKEDATA=false

cleanup() {
	rm "$TMPFILE"
	if $USING_TMP_SMOKEDATA; then
		rm "$SMOKEDATA"
	fi
}

trap cleanup EXIT

if [ -z "${SMOKEDATA:-}" ]; then
	# Generate default SMOKEDATA file.
	SMOKEDATA=$(mktemp -t smokedata.XXXXXXXXXX)
	USING_TMP_SMOKEDATA=true
	cat $(find . -name '*.c' -o -name '*.h' -o -name '*.sh') \
		| head -c 1000000 > "$SMOKEDATA"
fi

NDKDIR="${NDKDIR:=$HOME/android-ndk-r21d}"

FILES=("$SMOKEDATA" ./scripts/exec_tests.sh benchmark 'test_*')
EXEC_TESTS_CMD="WRAPPER= SMOKEDATA=\"$(basename $SMOKEDATA)\" sh exec_tests.sh"
NPROC=$(grep -c processor /proc/cpuinfo)
VALGRIND="valgrind --quiet --error-exitcode=100 --leak-check=full --errors-for-leak-kinds=all"
SANITIZE_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined,integer"

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

have_valgrind() {
	if ! type -P valgrind > /dev/null; then
		log_skip "valgrind not found; can't run tests with valgrind"
		return 1
	fi
}

have_ubsan() {
	if ! type -P clang > /dev/null; then
		log_skip "clang not found; can't run tests with UBSAN"
		return 1
	fi
}

###############################################################################

native_build_and_test() {

	# Build libdeflate, including the test programs.  Set the special test
	# support flag to get support for LIBDEFLATE_DISABLE_CPU_FEATURES.
	make "$@" TEST_SUPPORT__DO_NOT_USE=1 \
		-j$NPROC all test_programs > /dev/null

	# When not using -march=native, run the tests multiple times with
	# different combinations of CPU features disabled.  This is needed to
	# test all variants of dynamically-dispatched code.
	#
	# For now, we aren't super exhausive in which combinations of features
	# we test disabling.  We just disable the features roughly in order from
	# newest to oldest for each architecture, cumulatively.  In practice,
	# that's good enough to cover all the code.
	local features=('')
	if ! [[ "$*" =~ "-march=native" ]]; then
		case "$(uname -m)" in
		i386|x86_64)
			features+=(avx512bw avx2 avx bmi2 pclmul sse2)
			;;
		arm*|aarch*)
			features+=(crc32 pmull neon)
			;;
		esac
	fi
	local disable_str=""
	local feature
	for feature in "${features[@]}"; do
		if [ -n "$feature" ]; then
			if [ -n "$disable_str" ]; then
				disable_str+=","
			fi
			disable_str+="$feature"
			log "Retrying with CPU features disabled: $disable_str"
		fi
		WRAPPER="$WRAPPER" SMOKEDATA="$SMOKEDATA" \
			LIBDEFLATE_DISABLE_CPU_FEATURES="$disable_str" \
			sh ./scripts/exec_tests.sh > /dev/null
	done
}

native_tests() {
	test_group_included native || return 0
	local compiler compilers_to_try=(gcc)
	local cflags cflags_to_try=("")
	shopt -s nullglob
	compilers_to_try+=(/usr/bin/gcc-[0-9]*)
	compilers_to_try+=(/usr/bin/clang-[0-9]*)
	compilers_to_try+=(/opt/gcc*/bin/gcc)
	compilers_to_try+=(/opt/clang*/bin/clang)
	shopt -u nullglob

	if [ "$(uname -m)" = "x86_64" ]; then
		cflags_to_try+=("-march=native")
		cflags_to_try+=("-m32")
	fi
	for compiler in ${compilers_to_try[@]}; do
		for cflags in "${cflags_to_try[@]}"; do
			if [ "$cflags" = "-m32" ] && \
			   $compiler -v |& grep -q -- '--disable-multilib'
			then
				continue
			fi
			log "Running tests with CC=$compiler," \
				"CFLAGS=$cflags"
			WRAPPER= native_build_and_test \
				CC=$compiler CFLAGS="$cflags -Werror"
		done
	done

	if have_valgrind; then
		log "Running tests with Valgrind"
		WRAPPER="$VALGRIND" native_build_and_test
	fi

	if have_ubsan; then
		log "Running tests with undefined behavior sanitizer"
		WRAPPER= native_build_and_test CC=clang CFLAGS="$SANITIZE_CFLAGS"
	fi
}

# Test the library built with FREESTANDING=1.
freestanding_tests() {
	test_group_included freestanding || return 0

	WRAPPER= native_build_and_test FREESTANDING=1
	if nm libdeflate.so | grep -q ' U '; then
		echo 1>&2 "Freestanding lib links to external functions!:"
		nm libdeflate.so | grep ' U '
		return 1
	fi
	if ldd libdeflate.so | grep -q -v '\<statically linked\>'; then
		echo 1>&2 "Freestanding lib links to external libraries!:"
		ldd libdeflate.so
		return 1
	fi

	if have_valgrind; then
		WRAPPER="$VALGRIND" native_build_and_test FREESTANDING=1
	fi

	if have_ubsan; then
		WRAPPER= native_build_and_test FREESTANDING=1 \
			CC=clang CFLAGS="$SANITIZE_CFLAGS"
	fi
}

###############################################################################

checksum_benchmarks() {
	test_group_included checksum_benchmarks || return 0
	./scripts/checksum_benchmarks.sh
}

###############################################################################

android_build_and_test() {
	run_cmd ./scripts/android_build.sh --ndkdir="$NDKDIR" "$@" \
		all test_programs
	run_cmd adb push ${FILES[@]} /data/local/tmp/

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

	for arch in arm32 arm64; do
		for flags in "" "--enable-crc" "--enable-crypto" \
			     "--enable-crc --enable-crypto"; do
			android_build_and_test --arch=$arch $flags
		done
	done
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
	for gzip in "$PWD/gzip" /bin/gzip; do
		for gunzip in "$PWD/gunzip" /bin/gunzip; do
			log "Running gzip program tests with GZIP=$gzip," \
				"GUNZIP=$gunzip"
			GZIP="$gzip" GUNZIP="$gunzip" SMOKEDATA="$SMOKEDATA" \
				./scripts/gzip_tests.sh
		done
	done

	if have_valgrind; then
		log "Running gzip program tests with Valgrind"
		GZIP="$VALGRIND $PWD/gzip" GUNZIP="$VALGRIND $PWD/gunzip" \
			SMOKEDATA="$SMOKEDATA" ./scripts/gzip_tests.sh
	fi

	if have_ubsan; then
		log "Running gzip program tests with undefined behavior sanitizer"
		run_cmd make -j$NPROC CC=clang CFLAGS="$SANITIZE_CFLAGS" gzip gunzip
		GZIP="$PWD/gzip" GUNZIP="$PWD/gunzip" \
			SMOKEDATA="$SMOKEDATA" ./scripts/gzip_tests.sh
	fi
}

###############################################################################

log "Starting libdeflate tests"
log "	TESTGROUPS=(${TESTGROUPS[@]})"
log "	SMOKEDATA=$SMOKEDATA"
log "	NDKDIR=$NDKDIR"

native_tests
freestanding_tests
checksum_benchmarks
android_tests
windows_tests
static_analysis_tests
gzip_tests

if (( TESTS_SKIPPED )); then
	log "No tests failed, but some tests were skipped.  See above."
else
	log "All tests passed!"
fi
