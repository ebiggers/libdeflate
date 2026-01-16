#!/bin/bash

set -eu -o pipefail

# This script generates source and binary archives that should be posted for
# each new release of libdeflate.

prefix="libdeflate-$(git describe HEAD | sed 's/^v//')"

# Generate source code archive libdeflate-*.tar.gz
tarball="${prefix}.tar.gz"
echo "Generating $tarball"
git archive --format=tar --prefix="${prefix}/" HEAD \
	| libdeflate-gzip -12 > "$tarball"

# Generate Windows binary release libdeflate-*-windows-x86_64-bin.zip
dir=${prefix}-windows-x86_64-bin
zipfile="${dir}.zip"
echo "Generating $zipfile"
rm -rf build "$dir" "$zipfile"
CFLAGS="-Werror" x86_64-w64-mingw32-cmake -B build -G Ninja \
	-DLIBDEFLATE_BUILD_TESTS=1 -DZLIB_USE_STATIC_LIBS=1 \
	-DCMAKE_EXE_LINKER_FLAGS="-static-libgcc" \
	-DCMAKE_SHARED_LINKER_FLAGS="-static-libgcc" > /dev/null
cmake --build build > /dev/null
mkdir "$dir"
cp libdeflate.h build/libdeflate.{dll,dll.a,a} \
	build/programs/{benchmark,checksum}.exe "$dir"
cp build/programs/libdeflate-gzip.exe "$dir"/gzip.exe
cp build/programs/libdeflate-gzip.exe "$dir"/gunzip.exe
x86_64-w64-mingw32-strip "$dir"/libdeflate.dll "$dir"/*.exe
for file in COPYING NEWS.md README.md; do
	sed < $file > "$dir/${file}.txt" -e 's/$/\r/g'
done
(cd "$dir" && zip -q -r "../${zipfile}" .)

echo "Successfully generated release archives"
