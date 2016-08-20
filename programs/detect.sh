#!/bin/sh

if [ -z "$CC" ]; then
	CC=cc
fi

echo "/* THIS FILE WAS AUTOMATICALLY GENERATED.  DO NOT EDIT. */"
echo "#ifndef _CONFIG_H"
echo "#define _CONFIG_H"

tmpfile="$(mktemp -t libdeflate_config.XXXXXXXX)"
trap "rm -f \"$tmpfile\"" EXIT

check_function() {
	funcname="$1"
	macro="HAVE_$(echo $funcname | tr a-z A-Z)"
	echo "int main() { $funcname(); }" > "$tmpfile"
	echo
	echo "/* Is the $funcname() function available? */"
	if $CC -x c $tmpfile -o /dev/null > /dev/null 2>&1; then
		echo "#define $macro 1"
	else
		echo "/* $macro is not set */"
	fi
}

check_function clock_gettime
check_function futimens
check_function futimes
check_function posix_fadvise
check_function posix_madvise

echo
echo "#endif /* _CONFIG_H */"
