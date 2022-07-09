#!/bin/sh

# A minimal 'make check' target.  This only runs some quick tests;
# use scripts/run_tests.sh if you want to run the full tests.

set -e -u

if [ "$(uname)" = Darwin ]; then
	export DYLD_FALLBACK_LIBRARY_PATH=.
else
	export LD_LIBRARY_PATH=.
fi
cat lib/*.c | ./benchmark > /dev/null
cat lib/*.c | ./benchmark -C libz > /dev/null
cat lib/*.c | ./benchmark -D libz > /dev/null
for prog in ./test_*; do
	"$prog"
done
