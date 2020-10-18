#!/bin/bash
#
# Test script for libdeflate's gzip and gunzip programs.
#
# To run, you must set GZIP and GUNZIP in the environment to the absolute paths
# to the gzip and gunzip programs to test.  All tests should pass regardless of
# whether the GNU versions or the libdeflate versions, or a combination, of
# these programs are used.
#
# The environmental variable SMOKEDATA must also be set to a file containing
# test data.
#

set -eu -o pipefail

export -n GZIP GUNZIP SMOKEDATA

TMPDIR="$(mktemp -d)"
CURRENT_TEST=

cleanup() {
	if [ -n "$CURRENT_TEST" ]; then
		echo "TEST FAILED: \"$CURRENT_TEST\""
	fi
	rm -rf -- "$TMPDIR"
}

trap cleanup EXIT

SMOKEDATA="$(realpath "$SMOKEDATA")"
cd "$TMPDIR"

begin_test() {
	CURRENT_TEST="$1"
	rm -rf -- "${TMPDIR:?}"/*
	cp "$SMOKEDATA" file
}

gzip() {
	$GZIP "$@"
}

gunzip() {
	$GUNZIP "$@"
}

assert_status() {
	local expected_status="$1"
	local expected_msg="$2"
	shift 2
	(
		set +e
		{ eval "$*" > /dev/null; } 2>&1
		local actual_status=$?
		if [ "$actual_status" != "$expected_status" ]; then
			echo 1>&2 "Command '$*' exited with status" \
				"$actual_status but expected status" \
				"$expected_status"
			exit 1
		fi
		exit 0
	) > command_output
	if ! grep -E -q "$expected_msg" command_output; then
		echo 1>&2 "Expected output of command '$*' to match regex" \
			"'$expected_msg'"
		echo 1>&2 "Actual output was:"
		echo 1>&2 "---------------------------------------------------"
		cat 1>&2 command_output
		echo 1>&2 "---------------------------------------------------"
		return 1
	fi
}

assert_error() {
	assert_status 1 "$@"
}

assert_warning() {
	assert_status 2 "$@"
}

assert_skipped() {
	assert_warning '\<(ignored|skipping|unchanged)\>' "$@"
}

assert_equals() {
	local expected="$1"
	local actual="$2"

	if [ "$expected" != "$actual" ]; then
		echo 1>&2 "Expected '$expected', but got '$actual'"
		return 1
	fi
}

begin_test 'Basic compression and decompression works'
cp file orig
gzip file
[ ! -e file ] && [ -e file.gz ]
gunzip file.gz
[ -e file ] && [ ! -e file.gz ]
cmp file orig


begin_test 'gzip -d is gunzip'
cp file orig
gzip file
gzip -d file.gz
cmp file orig


begin_test '-k (keep original file) works'
cp file orig
gzip -k file
cmp file orig
rm file
cp file.gz orig.gz
gunzip -k file.gz
cmp file.gz orig.gz


begin_test '-c (write to stdout) works'
cp file orig
gzip -k file
gzip -c file > 2.gz
cmp file orig
cmp file.gz 2.gz
gunzip -c 2.gz > file
cmp file.gz 2.gz
cmp file orig


begin_test 'Reading from stdin works'
gzip < file > 1.gz
gzip - < file > 2.gz
cat file | gzip > 3.gz
cat file | gzip - > 4.gz
cmp file <(gunzip < 1.gz)
cmp file <(gunzip - < 2.gz)
cmp file <(cat 3.gz | gunzip)
cmp file <(cat 4.gz | gunzip -)


begin_test '-n option is accepted'
gzip -n file
gunzip -n file.gz


begin_test 'can specify multiple options'
gzip -fk1 file
cmp <(gzip -c -1 file) file.gz
gunzip -kfd file.gz


begin_test 'Compression levels'
if [ "$GZIP" = /bin/gzip ]; then
	assert_error '\<invalid option\>' gzip -10
	max_level=9
else
	for level in 13 99999 1a; do
		assert_error '\<Invalid compression level\>' gzip -$level
	done
	max_level=12
fi
for level in $(seq 1 $max_level); do
	gzip -c "-$level" file > "file$level"
	cmp file <(gunzip -c "file$level")
done
rm file command_output
cmp <(ls -S) <(ls -v) # file,file{1..max_level} have decreasing size


begin_test 'Overwriting output file requires -f'
cp file orig
echo -n > file.gz
gzip -c file > 2.gz
assert_warning 'already exists' gzip file </dev/null
cmp file.gz /dev/null
gzip -f file
cmp 2.gz file.gz
echo -n > file
assert_warning 'already exists' gunzip file.gz </dev/null
gunzip -f file.gz
cmp file orig


begin_test 'Nonexistent input file fails, even with -f'
for prog in 'gzip' 'gzip -f' 'gunzip' 'gunzip -f'; do
	assert_error 'No such file or directory' "$prog" NONEXISTENT
done


begin_test 'Compressing already-suffixed file requires -f or -c'
gzip file
gzip -c file.gz > c.gz
gzip file.gz 2>&1 >/dev/null | grep -q 'already has .gz suffix'
[ -e file.gz ] && [ ! -e file.gz.gz ]
gzip -f file.gz
[ ! -e file.gz ] && [ -e file.gz.gz ]
cmp file.gz.gz c.gz


begin_test 'Decompressing unsuffixed file only works with -c'
gzip file && mv file.gz file
assert_skipped gunzip file
assert_skipped gunzip -f file
gunzip -c file > orig
mv file file.gz && gunzip file.gz && cmp file orig


begin_test '... unless there is a corresponding suffixed file'
cp file orig
gzip file
[ ! -e file ] && [ -e file.gz ]
gunzip -c file > tmp
cmp tmp orig
rm tmp
ln -s NONEXISTENT file
gunzip -c file > tmp
cmp tmp orig
rm tmp file
gunzip file
[ -e file ] && [ ! -e file.gz ]
cmp file orig


begin_test 'Directory is skipped, even with -f'
mkdir dir
mkdir dir.gz
for opt in '' '-f' '-c'; do
	assert_skipped gzip $opt dir
done
#assert_skipped gzip dir.gz  # XXX: GNU gzip warns, libdeflate gzip no-ops
for opt in '' '-f' '-c'; do
	for name in dir dir.gz; do
		assert_skipped gunzip $opt $name
	done
done


begin_test '(gzip) symlink is rejected without -f or -c'
ln -s file symlink1
ln -s file symlink2
assert_error 'Too many levels of symbolic links' gzip symlink1
[ -e file ] && [ -e symlink1 ] && [ ! -e symlink1.gz ]
gzip -f symlink1
[ -e file ] && [ ! -e symlink1 ] && [ -e symlink1.gz ]
gzip -c symlink2 > /dev/null


begin_test '(gunzip) symlink is rejected without -f or -c'
gzip file
ln -s file.gz symlink1.gz
ln -s file.gz symlink2.gz
assert_error 'Too many levels of symbolic links' gunzip symlink1
[ -e file.gz ] && [ -e symlink1.gz ] && [ ! -e symlink1 ]
gunzip -f symlink1.gz
[ -e file.gz ] && [ ! -e symlink1.gz ] && [ -e symlink1 ]
gunzip -c symlink2.gz > /dev/null


begin_test 'FIFO is skipped, even with -f'
mkfifo foo
mkfifo foo.gz
assert_skipped gzip foo
assert_skipped gzip -f foo
#assert_skipped gzip -c foo # XXX: works with GNU gzip, not libdeflate's
assert_skipped gunzip foo.gz
assert_skipped gunzip -f foo.gz
#assert_skipped gunzip -c foo.gz # XXX: works with GNU gzip, not libdeflate's


begin_test '(gzip) overwriting symlink does not follow symlink'
echo 1 > 1
echo 2 > 2
gzip 1
ln -s 1.gz 2.gz
gzip -f 2
gunzip 1.gz
cmp <(echo 1) 1


begin_test '(gunzip) overwriting symlink does not follow symlink'
echo 1 > 1
echo 2 > 2
gzip 2
ln -s 1 2
gunzip -f 2.gz
cmp <(echo 1) 1
cmp <(echo 2) 2


begin_test '(gzip) hard linked file skipped without -f or -c'
cp file orig
ln file link
assert_equals 2 "$(stat -c %h file)"
assert_skipped gzip file
gzip -c file > /dev/null
assert_equals 2 "$(stat -c %h file)"
gzip -f file
assert_equals 1 "$(stat -c %h link)"
assert_equals 1 "$(stat -c %h file.gz)"
cmp link orig
# XXX: GNU gzip skips hard linked files with -k, libdeflate's doesn't


begin_test '(gunzip) hard linked file skipped without -f or -c'
gzip file
ln file.gz link.gz
cp file.gz orig.gz
assert_equals 2 "$(stat -c %h file.gz)"
assert_skipped gunzip file.gz
gunzip -c file.gz > /dev/null
assert_equals 2 "$(stat -c %h file.gz)"
gunzip -f file
assert_equals 1 "$(stat -c %h link.gz)"
assert_equals 1 "$(stat -c %h file)"
cmp link.gz orig.gz


begin_test 'Multiple files'
cp file file2
gzip file file2
[ ! -e file ] && [ ! -e file2 ] && [ -e file.gz ] && [ -e file2.gz ]
gunzip file.gz file2.gz
[ -e file ] && [ -e file2 ] && [ ! -e file.gz ] && [ ! -e file2.gz ]


begin_test 'Multiple files, continue on warning'
mkdir 1
cp file 2
assert_skipped gzip 1 2
[ ! -e 1.gz ]
cmp file <(gunzip -c 2.gz)
rmdir 1
mkdir 1.gz
assert_skipped gunzip 1.gz 2.gz
[ ! -e 1 ]
cmp 2 file


begin_test 'Multiple files, continue on error'
cp file 1
cp file 2
chmod a-r 1
assert_error 'Permission denied' gzip 1 2
[ ! -e 1.gz ]
cmp file <(gunzip -c 2.gz)
rm -f 1
cp 2.gz 1.gz
chmod a-r 1.gz
assert_error 'Permission denied' gunzip 1.gz 2.gz
[ ! -e 1 ]
cmp 2 file


begin_test 'Compressing empty file'
echo -n > empty
gzip empty
gunzip empty.gz
cmp /dev/null empty


begin_test 'Decompressing malformed file'
echo -n > foo.gz
assert_error '\<(not in gzip format|unexpected end of file)\>' \
	gunzip foo.gz
echo 1 > foo.gz
assert_error '\<not in gzip format\>' gunzip foo.gz
echo abcdefgh > foo.gz
assert_error '\<not in gzip format\>' gunzip foo.gz
xxd -r > foo.gz <<-EOF
00000000: 1f8b 0800 0000 0000 00ff 4b4c 4a4e 4924  ..........KLJNI$
00000010: 1673 0100 6c5b a262 2e00 0000            .s..l[.b....
EOF
assert_error '\<(not in gzip format|crc error)\>' gunzip foo.gz


for suf in .foo foo .blaaaaaaaaaaaaaaaargh; do
	begin_test "Custom suffix: $suf"
	gzip -S $suf file
	[ ! -e file ] && [ ! -e file.gz ] && [ -e file$suf ]
	assert_skipped gunzip file$suf
	gunzip -S $suf file$suf
	[ -e file ] && [ ! -e file.gz ] && [ ! -e file$suf ]
done
# DIFFERENCE: GNU gzip lower cases suffix, we don't


begin_test 'Empty suffix is rejected'
assert_error '\<invalid suffix\>' gzip -S '""' file
assert_error '\<invalid suffix\>' gunzip -S '""' file


begin_test 'Timestamps and mode are preserved'
chmod 777 file
orig_stat="$(stat -c '%a;%x;%y' file)"
gzip file
sleep 1
gunzip file.gz
assert_equals "$orig_stat" "$(stat -c '%a;%x;%y' file)"


begin_test 'Decompressing multi-member gzip file'
cat file file > orig
gzip -c file > file.gz
gzip -c file >> file.gz
gunzip -f file.gz
cmp file orig


begin_test 'Decompressing multi-member gzip file (final member smaller)'
echo 'hello world' > 2
cat file 2 > orig
gzip -c file > file.gz
gzip -c 2 >> file.gz
gunzip -f file.gz
cmp file orig


begin_test 'Help option'
gzip -h 2>&1 | grep -q 'Usage'
gunzip -h 2>&1 | grep -q 'Usage'


begin_test 'Incorrect usage'
for prog in gzip gunzip; do
	for opt in '--invalid-option' '-0'; do
		assert_error '\<(unrecognized|invalid) option\>' $prog $opt
	done
done


begin_test 'Version information'
gzip -V | grep -q Copyright
gunzip -V | grep -q Copyright

CURRENT_TEST=
