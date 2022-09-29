#!/bin/bash

set -e -u -o pipefail

cd "$(dirname "$0")"

read -r -a AVAILABLE_TARGETS < <(echo */fuzz.c | sed 's@/fuzz.c@@g')

usage()
{
	cat << EOF
Usage: $0 [OPTION]... [TARGET]...

Fuzz libdeflate with afl-fuzz.

Options:
   --asan          Enable AddressSanitizer
   --no-resume     Don't resume existing afl-fuzz session; start a new one
   --ubsan         Enable UndefinedBehaviorSanitizer

Available targets: ${AVAILABLE_TARGETS[*]}
EOF
}

die()
{
	echo "$*" 1>&2
	exit 1
}

asan=false
ubsan=false
may_resume=true

longopts_array=(
asan
help
no-resume
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
		asan=true
		;;
	--help)
		usage
		exit 0
		;;
	--no-resume)
		may_resume=false
		;;
	--ubsan)
		ubsan=true
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

if $asan && $ubsan; then
	die "--asan and --ubsan are mutually exclusive"
fi

if ! type -P afl-fuzz > /dev/null; then
	die "afl-fuzz is not installed"
fi

if (( $# == 0 )); then
	targets=("${AVAILABLE_TARGETS[@]}")
else
	for target; do
		found=false
		for t in "${AVAILABLE_TARGETS[@]}"; do
			if [ "$target" = "$t" ]; then
				found=true
			fi
		done
		if ! $found; then
			echo 1>&2 "Unknown target '$target'"
			echo 1>&2 "Available targets: ${AVAILABLE_TARGETS[*]}"
			exit 1
		fi
	done
	targets=("$@")
fi
if (( ${#targets[@]} > 1 )) && ! type -P urxvt > /dev/null; then
	die "urxvt is not installed"
fi

afl_opts=""
if $asan; then
	export AFL_USE_ASAN=1
	export CFLAGS="-O2 -m32"
	export CC=afl-clang
	afl_opts+=" -m 800"
elif $ubsan; then
	export CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined"
	export CC=afl-gcc
else
	export AFL_HARDEN=1
	export CFLAGS="-O2"
	export CC=afl-gcc
fi
CFLAGS+=" -DLIBDEFLATE_ENABLE_ASSERTIONS"

sudo sh -c "echo core > /proc/sys/kernel/core_pattern"
if [ -e /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
	sudo sh -c "echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
fi

srcdir=../..
builddir=$srcdir/build
$srcdir/scripts/cmake-helper.sh -G Ninja
cmake --build $builddir

for dir in "${targets[@]}"; do
	cp -vaT "$dir" "/tmp/$dir"
	# shellcheck disable=SC2086 # Intended word splitting of $CFLAGS
	$CC $CFLAGS -Wall -I$srcdir "$dir"/fuzz.c $builddir/libdeflate.a \
		-o "/tmp/$dir/fuzz"
	indir=/tmp/$dir/inputs
	outdir=/tmp/$dir/outputs
	if [ -e "$outdir" ]; then
		if $may_resume; then
			indir="-"
		else
			rm -rf "${outdir:?}"/*
		fi
	else
		mkdir "$outdir"
	fi
	cmd="afl-fuzz -i $indir -o $outdir -T $dir $afl_opts -- /tmp/$dir/fuzz @@"
	if (( ${#targets[@]} > 1 )); then
		urxvt -e bash -c "$cmd" &
	else
		$cmd
	fi
done
wait
