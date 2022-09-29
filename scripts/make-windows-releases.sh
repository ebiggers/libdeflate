#!/bin/bash

set -eu -o pipefail

for arch in 'i686' 'x86_64'; do
	dir=libdeflate-$(git describe --tags | tr -d v)-windows-${arch}-bin
	rm -rf build "$dir" "$dir.zip"
	CFLAGS="-Werror" ${arch}-w64-mingw32-cmake -B build -G Ninja \
		-DLIBDEFLATE_BUILD_TESTS=1
	cmake --build build
	mkdir "$dir"
	cp libdeflate.h build/libdeflate.{dll,dll.a,a} \
		build/programs/{benchmark,checksum}.exe "$dir"
	cp build/programs/libdeflate-gzip.exe "$dir"/gzip.exe
	cp build/programs/libdeflate-gzip.exe "$dir"/gunzip.exe
	${arch}-w64-mingw32-strip "$dir"/libdeflate.dll "$dir"/*.exe
	for file in COPYING NEWS.md README.md; do
		sed < $file > "$dir/${file}.txt" -e 's/$/\r/g'
	done
	(cd "$dir" && zip -r "../${dir}.zip" .)
done
