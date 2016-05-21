#!/bin/bash

set -e

if [ -z "$LEVEL" -o -z "$INPUTFILE" ]; then
	echo "Must specify LEVEL and INPUTFILE" 1>&2
	exit 1
fi

topdir="$(dirname "$0")/../.."

git checkout -f "$topdir/src" > /dev/null

sed -i -e	\
"
    s/\<block_size >> 12\>/block_size >> $BLOCK_SIZE_SHIFT/
    s/\<200 \* stats->num_observations/$BLOCK_CUTOFF * stats->num_observations/
    s/num_new_observations < 512\>/num_new_observations < $OBSERVATIONS_PER_CHECK/
"	\
"$topdir/src/deflate_compress.c"

make -C "$topdir" -j BUILD_BENCHMARK_PROGRAM=yes > /dev/null
"$topdir/benchmark" -l $LEVEL -s 100000000 $INPUTFILE | grep Compressed | cut -f 4 -d ' '
