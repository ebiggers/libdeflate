#!/bin/bash

set -e -u -o pipefail

cd "$(dirname "$0")"

read -r -a AVAILABLE_TARGETS < <(echo */fuzz.c | sed 's@/fuzz.c@@g')

usage()
{
	cat << EOF
Usage: $0 [OPTION]... FUZZ_TARGET

Fuzz libdeflate with LLVM's libFuzzer.

Options:
   --asan          Enable AddressSanitizer
   --msan          Enable MemorySanitizer
   --time=SECONDS  Stop after the given time has passed
   --ubsan         Enable UndefinedBehaviorSanitizer

Available fuzz targets: ${AVAILABLE_TARGETS[*]}
EOF
}

die()
{
	echo "$*" 1>&2
	exit 1
}

run_cmd()
{
	echo "$*"
	"$@"
}

EXTRA_SANITIZERS=
EXTRA_FUZZER_ARGS=()

longopts_array=(
asan
help
msan
time:
ubsan
)
longopts=$(echo "${longopts_array[@]}" | tr ' ' ',')

if ! options=$(getopt -o "" -l "$longopts" -- "$@"); then
	usage 1>&2
	exit 1
fi
eval set -- "$options"
while (( $# >= 0 )); do
	case "$1" in
	--asan)
		EXTRA_SANITIZERS+=",address"
		;;
	--help)
		usage
		exit 0
		;;
	--time)
		EXTRA_FUZZER_ARGS+=("-max_total_time=$2")
		shift
		;;
	--msan)
		EXTRA_SANITIZERS+=",memory"
		;;
	--ubsan)
		EXTRA_SANITIZERS+=",undefined"
		;;
	--)
		shift
		break
		;;
	*)
		echo 1>&2 "Invalid option: \"$1\""
		usage 1>&2
		exit 1
	esac
	shift
done

if (( $# != 1 )); then
	echo 1>&2 "No fuzz target specified!"
	usage 1>&2
	exit 1
fi
TARGET=$1
if [ ! -e "$TARGET/fuzz.c" ]; then
	echo 1>&2 "'$TARGET' is not a valid fuzz target!"
	usage 1>&2
	exit 1
fi
run_cmd clang -g -O1 -fsanitize=fuzzer$EXTRA_SANITIZERS \
	-Wall -Werror -DLIBDEFLATE_ENABLE_ASSERTIONS=1 \
	../../lib/*{,/*}.c "$TARGET/fuzz.c" -o "$TARGET/fuzz"
run_cmd "$TARGET/fuzz" "${EXTRA_FUZZER_ARGS[@]}" "$TARGET/corpus"
