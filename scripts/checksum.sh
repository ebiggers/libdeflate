#!/bin/bash

set -e

SCRIPTDIR="$(dirname "$0")"
BUILDDIR="$SCRIPTDIR/../build"

"$SCRIPTDIR"/cmake-helper.sh -DLIBDEFLATE_BUILD_TESTS=1 -G Ninja
ninja -C "$BUILDDIR" checksum
"$BUILDDIR"/programs/checksum "$@"
