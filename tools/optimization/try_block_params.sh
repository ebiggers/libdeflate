#!/bin/bash

set -e

topdir="$(dirname "$0")/../.."

git checkout -f "$topdir/src" > /dev/null

sed -i -e	\
"
    s/\<block_size >> 12\>/block_size >> $BLOCK_SIZE_SHIFT/
    s/\<200 \* stats->num_observations/$BLOCK_CUTOFF * stats->num_observations/
    s/#define MIN_BLOCK_LENGTH.*$/#define MIN_BLOCK_LENGTH $MIN_BLOCK_LENGTH/
    s/#define MAX_BLOCK_LENGTH.*$/#define MAX_BLOCK_LENGTH $MAX_BLOCK_LENGTH/
    s/num_new_observations < 512\>/num_new_observations < $OBSERVATIONS_PER_CHECK/
"	\
"$topdir/src/deflate_compress.c"

make -C "$topdir" -j BUILD_BENCHMARK_PROGRAM=yes > /dev/null
"$topdir/benchmark" -l 12 -s 100000000 $HOME/data/testdata | grep Compressed | cut -f 4 -d ' '
