#!/bin/bash

set -eu -o pipefail
topdir="$(dirname "$0")/.."
tmpfile=$(mktemp)
trap 'rm -f $tmpfile' EXIT

run_benchmark()
{
	local best_ctime=1000000000
	local i

	for i in $(seq "$NUM_ITERATIONS"); do
		"$@" > "$tmpfile"
		csize=$(awk '/Compressed/{print $4}' "$tmpfile")
		ctime=$(awk '/Compression time/{print $3}' "$tmpfile")
		if (( ctime <  best_ctime )); then
			best_ctime=$ctime
		fi
		: "$i" # make shellcheck happy
	done
	CSIZE=$csize
	CTIME=$best_ctime
}

multifile()
{
	local file results cmd best em

	NUM_ITERATIONS=1

	echo "File | zlib -6 | zlib -9 | libdeflate -6 | libdeflate -9 | libdeflate -12"
	echo "-----|---------|---------|---------------|---------------|---------------"

	for file in "$@"; do
		echo -n "$(basename "$file")"
		results=()
		cmd=("$topdir/build/programs/benchmark"
		     -s"$(stat -c "%s" "$file")" "$file")
		run_benchmark "${cmd[@]}" -Y -6
		results+=("$CSIZE")
		run_benchmark "${cmd[@]}" -Y -6
		results+=("$CSIZE")
		run_benchmark "${cmd[@]}" -6
		results+=("$CSIZE")
		run_benchmark "${cmd[@]}" -9
		results+=("$CSIZE")
		run_benchmark "${cmd[@]}" -12
		results+=("$CSIZE")
		best=2000000000
		for result in "${results[@]}"; do
			if (( result < best)); then
				best=$result
			fi
		done
		for result in "${results[@]}"; do
			if (( result == best )); then
				em="**"
			else
				em=""
			fi
			echo -n " | ${em}${result}${em}"
		done
		echo
	done
}

single_file()
{
	local file=$1
	local usize args
	local include_old=false

	usize=$(stat -c "%s" "$file")
	: ${NUM_ITERATIONS:=3}

	if [ -e "$topdir/benchmark-old" ]; then
		include_old=true
	fi
	echo -n "Level | libdeflate (new) "
	if $include_old; then
		echo -n "| libdeflate (old) "
	fi
	echo "| zlib"
	echo -n "------|------------------"
	if $include_old; then
		echo -n "|------------------"
	fi
	echo "|-----"
	for level in {1..12}; do
		echo -n "$level"
		args=("$file" -s "$usize" "-$level")

		run_benchmark "$topdir/build/programs/benchmark" "${args[@]}"
		echo -n " | $CSIZE / $CTIME"

		if $include_old; then
			run_benchmark "$topdir/benchmark-old" "${args[@]}"
			echo -n " | $CSIZE / $CTIME"
		fi

		if (( level > 9 )); then
			echo -n " | N/A"
		else
			run_benchmark "$topdir/build/programs/benchmark" \
				      "${args[@]}" -Y
			echo -n " | $CSIZE / $CTIME"
		fi
		echo
	done
}

if (( $# > 1 )); then
	multifile "$@"
elif (( $# == 1 )); then
	single_file "$@"
else
	echo 1>&2 "Usage: $0 FILE..."
fi
