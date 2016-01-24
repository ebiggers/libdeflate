#!/bin/sh

set -e

make clean
make -j4 CC=i686-w64-mingw32-gcc BUILD_PROGRAMS=yes
cp *.exe /j/exe

make clean
make -j4 CC=x86_64-w64-mingw32-gcc BUILD_PROGRAMS=yes
cp *.exe /j/exe64

sudo systemctl restart smbd
