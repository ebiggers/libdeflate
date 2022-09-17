#!/bin/bash

set -eu -o pipefail

for arch in 'i686' 'x86_64'; do
	make clean
	make -j CC=${arch}-w64-mingw32-gcc CFLAGS="-Werror" all \
		benchmark.exe checksum.exe
	dir=libdeflate-$(git describe --tags | tr -d v)-windows-${arch}-bin
	rm -rf "$dir" "$dir.zip"
	mkdir "$dir"
	cp libdeflate.{dll,dll.a,def,a} libdeflate.h ./*.exe "$dir"
	${arch}-w64-mingw32-strip "$dir/libdeflate.dll" "$dir"/*.exe
	for file in COPYING NEWS.md README.md; do
		sed < $file > "$dir/${file}.txt" -e 's/$/\r/g'
	done
	(cd "$dir" && zip -r "../${dir}.zip" .)
done
