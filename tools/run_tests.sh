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

NDKDIR="${NDKDIR:=/opt/android-ndk}"

FILES=("$SMOKEDATA" ./tools/exec_tests.sh benchmark 'test_*')
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

have_python() {
	if ! type -P python3 > /dev/null; then
		log_skip "Python not found"
		return 1
	fi
}

###############################################################################

native_build_and_test() {
	make "$@" -j$NPROC all test_programs > /dev/null
	WRAPPER="$WRAPPER" SMOKEDATA="$SMOKEDATA" sh ./tools/exec_tests.sh \
			> /dev/null
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

###############################################################################

checksum_benchmarks() {
	test_group_included checksum_benchmarks || return 0
	./tools/checksum_benchmarks.sh
}

###############################################################################

android_build_and_test() {
	run_cmd ./tools/android_build.sh --ndkdir="$NDKDIR" "$@"
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

	for compiler in gcc clang; do
		for flags in "" "--enable-neon" "--enable-crypto"; do
			for arch in arm32 arm64; do
				android_build_and_test --arch=$arch \
					--compiler=$compiler $flags
			done
		done
	done
}

###############################################################################

mips_tests() {
	test_group_included mips || return 0
	if [ "$(hostname)" != "zzz" ] && [ "$(hostname)" != "sol" ]; then
		log_skip "MIPS tests are not supported on this host"
		return 0
	fi
	if ! ping -c 1 dd-wrt > /dev/null; then
		log_skip "Can't run MIPS tests: dd-wrt system not available"
		return 0
	fi
	run_cmd ./tools/mips_build.sh
	run_cmd scp ${FILES[@]} root@dd-wrt:
	run_cmd ssh root@dd-wrt "$EXEC_TESTS_CMD"

	log "Checking that compression on big endian CPU produces same output"
	run_cmd scp gzip root@dd-wrt:
	run_cmd ssh root@dd-wrt \
		"rm -f big*.gz;
		 ./gzip -c -6 $(basename $SMOKEDATA) > big6.gz;
		 ./gzip -c -10 $(basename $SMOKEDATA) > big10.gz"
	run_cmd scp root@dd-wrt:big*.gz .
	make -j$NPROC gzip > /dev/null
	./gzip -c -6 "$SMOKEDATA" > little6.gz
	./gzip -c -10 "$SMOKEDATA" > little10.gz
	if ! cmp big6.gz little6.gz || ! cmp big10.gz little10.gz; then
		echo 1>&2 "Compressed data differed on big endian vs. little endian!"
		return 1
	fi
	rm big*.gz little*.gz
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
				./tools/gzip_tests.sh
		done
	done

	if have_valgrind; then
		log "Running gzip program tests with Valgrind"
		GZIP="$VALGRIND $PWD/gzip" GUNZIP="$VALGRIND $PWD/gunzip" \
			SMOKEDATA="$SMOKEDATA" ./tools/gzip_tests.sh
	fi

	if have_ubsan; then
		log "Running gzip program tests with undefined behavior sanitizer"
		run_cmd make -j$NPROC CC=clang CFLAGS="$SANITIZE_CFLAGS" gzip gunzip
		GZIP="$PWD/gzip" GUNZIP="$PWD/gunzip" \
			SMOKEDATA="$SMOKEDATA" ./tools/gzip_tests.sh
	fi
}

###############################################################################

edge_case_tests() {
	test_group_included edge_case || return 0

	# Regression test for "deflate_compress: fix corruption with long
	# literal run".  Try to compress a file longer than 65535 bytes where no
	# 2-byte sequence (3 would be sufficient) is repeated <= 32768 bytes
	# apart, and the distribution of bytes remains constant throughout, and
	# yet not all bytes are used so the data is still slightly compressible.
	# There will be no matches in this data, but the compressor should still
	# output a compressed block, and this block should contain more than
	# 65535 consecutive literals, which triggered the bug.
	#
	# Note: on random data, this situation is extremely unlikely if the
	# compressor uses all matches it finds, since random data will on
	# average have a 3-byte match every (256**3)/32768 = 512 bytes.
	if have_python; then
		python3 > "$TMPFILE" << EOF
import sys
for i in range(2):
    for stride in range(1,251):
        b = bytes(stride*multiple % 251 for multiple in range(251))
        sys.stdout.buffer.write(b)
EOF
		run_cmd make -j$NPROC benchmark
		run_cmd ./benchmark -3 "$TMPFILE"
		run_cmd ./benchmark -6 "$TMPFILE"
		run_cmd ./benchmark -12 "$TMPFILE"
	fi
}

###############################################################################

log "Starting libdeflate tests"
log "	TESTGROUPS=(${TESTGROUPS[@]})"
log "	SMOKEDATA=$SMOKEDATA"
log "	NDKDIR=$NDKDIR"

native_tests
checksum_benchmarks
android_tests
mips_tests
windows_tests
static_analysis_tests
gzip_tests
edge_case_tests

if (( TESTS_SKIPPED )); then
	log "No tests failed, but some tests were skipped.  See above."
else
	log "All tests passed!"
fi
