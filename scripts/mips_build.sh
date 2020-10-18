#!/bin/bash

set -eu

TOOLCHAIN_DIR=$HOME/src/ddwrt-toolchains/toolchain-mips_34kc_gcc-5.1.0_musl-1.1.9

make -j$(grep -c processor /proc/cpuinfo) all test_programs \
	CC="$TOOLCHAIN_DIR/bin/mips-openwrt-linux-musl-gcc" \
	CFLAGS="-DNEED_PRINTF -Werror"
