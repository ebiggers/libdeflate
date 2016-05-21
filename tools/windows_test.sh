#!/bin/sh

set -e

make -j CC=i686-w64-mingw32-gcc
cp -vf *.exe /j/exe/
make -j CC=x86_64-w64-mingw32-gcc
cp -vf *.exe /j/exe64/

sudo $HOME/bin/sudo/restart-smbd
