#!/bin/bash
#
# Test script for libdeflate
#
# Usage:
#    Run all tests:
#	./run_tests.sh
#    Run only the given tests:
#	./run_tests.sh asan valgrind
#    Run all tests other than the given ones:
#	./run_tests.sh ^asan ^valgrind
#
# See TEST_FUNCS for the available tests.

set -eu -o pipefail
cd "$(dirname "$0")/.."

# Use CC if specified in environment, else default to "cc".
: "${CC:=cc}"

export CFLAGS="-Werror -DLIBDEFLATE_ENABLE_ASSERTIONS"

# No wrapper by default; overridden by valgrind tests
export WRAPPER=

TEST_FUNCS=()

CLEANUP_CMDS=()
cleanup() {
	for cmd in "${CLEANUP_CMDS[@]}"; do
		eval "$cmd"
	done
}
trap cleanup EXIT

CLEANUP_CMDS+=("rm -rf build")

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

MAKE="make -j$(getconf _NPROCESSORS_ONLN)"

UNAME=$(uname)
ARCH=$(uname -m)

SHLIB=build/libdeflate.so
if [ "$UNAME" = Darwin ]; then
	SHLIB=build/libdeflate.dylib
fi

###############################################################################

INDENT=0

log()
{
	echo -n "[$(date)] "
	if (( INDENT != 0 )); then
		head -c $(( INDENT * 4 )) /dev/zero | tr '\0' ' '
	fi
	echo "$@"
}

begin()
{
	log "$@"
	(( INDENT++ )) || true
}

end()
{
	(( INDENT-- )) || true
}

run_cmd()
{
	log "$@"
	"$@" > /dev/null
}

fail()
{
	echo 1>&2 "$@"
	exit 1
}

file_count()
{
	local dir=$1

	find "$dir" -type f -o -type l | wc -l
}

cflags_supported()
{
	# -Werror is needed here in order for old versions of clang to reject
	# invalid options.
	echo 'int main(void){ return 0; }' \
		| $CC "$@" -Werror -x c - -o /dev/null 2>/dev/null
}

# Build libdeflate, including the test programs.  Set the special test support
# flag to get support for LIBDEFLATE_DISABLE_CPU_FEATURES.
build()
{
	CFLAGS="$CFLAGS -DTEST_SUPPORT__DO_NOT_USE=1" scripts/cmake-helper.sh \
		-DLIBDEFLATE_BUILD_TESTS=1 "$@" > /dev/null
	$MAKE -C build > /dev/null
}

build_and_run_tests()
{
	local quick=false
	if [ "${1:-}" = "--quick" ]; then
		quick=true
		shift
	fi

	begin "CC=$CC CFLAGS=\"$CFLAGS\" WRAPPER=\"$WRAPPER\" $*"

	build "$@"

	# When not using -march=native, run the tests multiple times with
	# different combinations of CPU features disabled.  This is needed to
	# test all variants of dynamically-dispatched code.
	#
	# For now, we aren't super exhausive in which combinations of features
	# we test disabling.  We just disable the features roughly in order from
	# newest to oldest for each architecture, cumulatively.  In practice,
	# that's good enough to cover all the code.
	local features=('')
	if ! [[ "$CFLAGS" =~ "-march=native" ]] && ! $quick; then
		case "$ARCH" in
		i386|x86_64)
			features+=(zmm avx512_vnni avx512vl avx_vnni vpclmulqdq
				   avx2 avx bmi2 pclmulqdq sse2)
			;;
		arm*|aarch*)
			features+=(dotprod sha3 prefer_pmull crc32 pmull neon)
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
		fi
		log "Using LIBDEFLATE_DISABLE_CPU_FEATURES=$disable_str"
		LIBDEFLATE_DISABLE_CPU_FEATURES="$disable_str" \
		    sh ./scripts/exec_tests.sh build/programs/ > /dev/null
	done
	end
}

is_compatible_system_gzip()
{
	local prog=$1

	# Needs to exist.
	if ! [ -e "$prog" ]; then
		return 1
	fi
	# Needs to be GNU gzip.
	if ! "$prog" -V 2>&1 | grep -q 'Free Software Foundation'; then
		return 1
	fi
	# Needs to support the -k option, i.e. be v1.6 or later.
	if echo | { "$prog" -k 2>&1 >/dev/null || true; } \
				| grep -q 'invalid option'; then
		return 1
	fi
	return 0
}

gzip_tests()
{
	local gzips=("$PWD/build/programs/libdeflate-gzip")
	local gunzips=("$PWD/build/programs/libdeflate-gzip -d")
	if [ "${1:-}" != "--quick" ]; then
		if is_compatible_system_gzip /bin/gzip; then
			gzips+=(/bin/gzip)
			gunzips+=(/bin/gunzip)
		elif is_compatible_system_gzip /usr/bin/gzip; then
			gzips+=(/usr/bin/gzip)
			gunzips+=(/usr/bin/gunzip)
		else
			log "Unsupported system gzip; skipping comparison with system gzip"
		fi
	fi
	local gzip gunzip

	begin "Running gzip program tests with CC=\"$CC\" CFLAGS=\"$CFLAGS\""
	build
	for gzip in "${gzips[@]}"; do
		for gunzip in "${gunzips[@]}"; do
			log "GZIP=$gzip, GUNZIP=$gunzip"
			GZIP="$gzip" GUNZIP="$gunzip" TESTDATA="$TESTDATA" \
				./scripts/gzip_tests.sh
		done
	done
	end
}

do_run_tests()
{
	build_and_run_tests "$@"
	gzip_tests "$@"
}

################################################################################

regular_test()
{
	do_run_tests
}
TEST_FUNCS+=(regular_test)

O3_test()
{
	CFLAGS="$CFLAGS -O3" do_run_tests
}
TEST_FUNCS+=(O3_test)

march_native_test()
{
	if ! cflags_supported "-march=native"; then
		log "Compiler doesn't support -march=native; skipping test"
		return
	fi
	CFLAGS="$CFLAGS -march=native" do_run_tests
}
TEST_FUNCS+=(march_native_test)

valgrind_version_at_least()
{
	local want_vers=$1
	local vers

	if ! type -P valgrind &> /dev/null; then
		return 1
	fi

	vers=$(valgrind --version | grep -E -o '[0-9\.]+' | head -1)

	[ "$want_vers" = "$(echo -e "$vers\n$want_vers" | sort -V | head -1)" ]
}

valgrind_test()
{
	# Need valgrind 3.9.0 for '--errors-for-leak-kinds=all'
	# Need valgrind 3.12.0 for armv8 crypto and crc instructions
	if ! valgrind_version_at_least 3.12.0; then
		log "valgrind not found; skipping test"
		return
	fi
	WRAPPER="valgrind --quiet --error-exitcode=100 --leak-check=full --errors-for-leak-kinds=all" \
		do_run_tests --quick
}
TEST_FUNCS+=(valgrind_test)

ubsan_test()
{
	local cflags=("-fsanitize=undefined" "-fno-sanitize-recover=undefined")
	if ! cflags_supported "${cflags[@]}"; then
		log "Compiler doesn't support UBSAN; skipping test"
		return
	fi
	CFLAGS="$CFLAGS ${cflags[*]}" do_run_tests --quick
}
TEST_FUNCS+=(ubsan_test)

asan_test()
{
	local cflags=("-fsanitize=address" "-fno-sanitize-recover=address")
	if ! cflags_supported "${cflags[@]}"; then
		log "Compiler doesn't support ASAN; skipping test"
		return
	fi
	CFLAGS="$CFLAGS ${cflags[*]}" do_run_tests --quick
}
TEST_FUNCS+=(asan_test)

cfi_test()
{
	local cflags=("-fsanitize=cfi" "-fno-sanitize-recover=cfi" "-flto"
		      "-fvisibility=hidden")
	if ! cflags_supported "${cflags[@]}"; then
		log "Compiler doesn't support CFI; skipping test"
		return
	fi
	CFLAGS="$CFLAGS ${cflags[*]}" AR=llvm-ar do_run_tests --quick
}
TEST_FUNCS+=(cfi_test)

install_test()
{
	build
	$MAKE -C build install DESTDIR=inst > /dev/null
}
TEST_FUNCS+=(install_test)

symbol_prefix_test()
{
	build
	log "Checking that all global symbols are prefixed with \"libdeflate_\""
	if nm build/libdeflate.a | grep ' T ' | grep -E -v " _?libdeflate_"
	then
		fail "Some global symbols aren't prefixed with \"libdeflate_\""
	fi
	log "Checking that all exported symbols are prefixed with \"libdeflate\""
	if nm $SHLIB | grep ' T ' \
			| grep -E -v " _?(libdeflate_|_init\>|_fini\>)"; then
		fail "Some exported symbols aren't prefixed with \"libdeflate_\""
	fi
}
TEST_FUNCS+=(symbol_prefix_test)

is_dynamically_linked()
{
	local prog=$1

	if [ "$UNAME" = Darwin ]; then
		otool -L "$prog" | grep -q libdeflate
	else
		ldd "$prog" | grep -q libdeflate
	fi
}

use_shared_lib_test()
{
	log "Testing USE_SHARED_LIB=1"
	build
	if is_dynamically_linked build/programs/libdeflate-gzip; then
		fail "Binary should be statically linked by default"
	fi
	build -DLIBDEFLATE_USE_SHARED_LIB=1 > /dev/null
	if ! is_dynamically_linked build/programs/libdeflate-gzip; then
		fail "Binary isn't dynamically linked"
	fi
}
TEST_FUNCS+=(use_shared_lib_test)

freestanding_test()
{
	if [ "$UNAME" = Darwin ]; then
		log "Skipping freestanding build tests due to unsupported OS"
		return
	fi
	build_and_run_tests --quick -DLIBDEFLATE_FREESTANDING=1
	if nm $SHLIB | grep -v '\<__stack_chk_fail\>' | grep -q ' U '; then
		echo 1>&2 "Freestanding lib links to external functions!:"
		nm $SHLIB | grep ' U '
		return 1
	fi
	if ldd $SHLIB | grep -q -v '\<statically linked\>'; then
		echo 1>&2 "Freestanding lib links to external libraries!:"
		ldd $SHLIB
		return 1
	fi
}
TEST_FUNCS+=(freestanding_test)

###############################################################################

declare -A all_tests
for test_func in "${TEST_FUNCS[@]}"; do
	all_tests["${test_func%_test}"]=true
done
declare -A tests_to_run

# Determine the set of tests to run by applying any inclusions and exclusions
# given on the command line.  If no inclusions were given, then default to all
# tests (subject to exclusions).
all=true
for arg; do
	if [[ $arg != ^* ]]; then
		all=false
	fi
done
if $all; then
	for t in "${!all_tests[@]}"; do
		tests_to_run[$t]=true
	done
fi
for arg; do
	if [[ $arg == ^* ]]; then
		unset "tests_to_run[${arg#^}]"
	elif [[ -z ${all_tests["$arg"]:-} ]]; then
		fail "Unknown test '$arg'.  Options are: ${!all_tests[*]}"
	else
		tests_to_run["$arg"]=true
	fi
done

# Actually run the tests.
log "Running libdeflate tests: ${!tests_to_run[*]}"
for t in "${!tests_to_run[@]}"; do
	begin "Running ${t}_test"
	eval "${t}_test"
	end
done
log "All tests passed!"
