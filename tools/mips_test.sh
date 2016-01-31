#!/bin/sh

set -e

TOOLCHAIN_DIR=$HOME/src/ddwrt-toolchains/toolchain-mips_34kc_gcc-5.1.0_musl-1.1.9

make clean
make -j4 BUILD_SHARED_LIBRARY=no BUILD_BENCHMARK_PROGRAM=yes \
	CC="$TOOLCHAIN_DIR/bin/mips-openwrt-linux-musl-gcc" \
	CFLAGS="-DNEED_PRINTF"

scp benchmark $HOME/data/test root@dd-wrt:
ssh root@dd-wrt ./benchmark "$@" test
