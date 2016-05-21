#!/bin/sh

set -e

TOOLCHAIN_DIR=$HOME/src/ddwrt-toolchains/toolchain-mips_34kc_gcc-5.1.0_musl-1.1.9

make -j benchmark \
	CC="$TOOLCHAIN_DIR/bin/mips-openwrt-linux-musl-gcc" \
	CFLAGS="-DNEED_PRINTF"

scp benchmark $HOME/data/test root@dd-wrt:
ssh root@dd-wrt ./benchmark "$@" test
