#!/bin/bash

set -e

SCRIPTDIR="$(dirname "$(realpath "$0")")"
BUILDDIR="$SCRIPTDIR/../build"

"$SCRIPTDIR"/cmake-helper.sh -DLIBDEFLATE_BUILD_TESTS=1 -G Ninja > /dev/null
ninja -C "$BUILDDIR" --quiet checksum
"$BUILDDIR"/programs/checksum "$@"
