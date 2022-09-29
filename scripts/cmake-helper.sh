#!/bin/sh

# This script ensures that the 'build' directory has been created and configured
# with the given CMake options and environment.

TOPDIR="$(dirname "$0")"/..
BUILDDIR="$TOPDIR"/build

flags=$(env; echo "@CMAKEOPTS@=$*")
if [ "$flags" != "$(cat "$BUILDDIR"/.flags 2>/dev/null)" ]; then
	rm -rf "$BUILDDIR"/CMakeCache.txt "$BUILDDIR"/CMakeFiles
	mkdir -p "$BUILDDIR"
	cmake -S "$TOPDIR" -B "$BUILDDIR" "$@"
	echo "$flags" > "$BUILDDIR"/.flags
fi
