#!/bin/bash

set -e

SCRIPTDIR="$(dirname "$0")"
BUILDDIR="$SCRIPTDIR/../build"

"$SCRIPTDIR"/cmake-helper.sh -DLIBDEFLATE_BUILD_TESTS=1 -G Ninja > /dev/null
ninja -C "$BUILDDIR" --quiet benchmark
"$BUILDDIR"/programs/benchmark "$@"
