#!/bin/sh
#
# test integrity of few basic files
# pass gzip program you want to test as first argument ("$1")
#

# check if GZIP is exported
#if [ -z $GZIP ]; then
#    GZIP=gzip
#fi

if [ $# -eq 0 ]
  then
    echo "$0 <path/to/gzip>"
    echo "test integrity of (embedded) files with provided gzip client"
    exit 0
fi

# use passed 
if [ -n "$1" ]; then
    GZIP="$1"
fi

echo ""
echo "testing files integrity with '$GZIP'"

TEST_FILES_GOOD_NAMES=(
"ala1-1.gz"
"fox2-1.gz"
)
TEST_FILES_GOOD=(
"H4sIAAAAAAAAA3PMSVTITVTIzi9JVABTIJ5jzpGZelwAX+86ehsAAAA="
"H4sIAAAAAAAAAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIoAUrnJFZVKqTkp+txAQBqzFDrLQAAAA=="
)
TEST_COUNT_GOOD=0

TEST_FILES_BAD_NAMES=(
"ala1-bad-crc.gz"
"ala1-bad-flip.gz"
"ala1-bad-len.gz"
"fox2-bad-flip.gz"
"fox2-bad-truncated.gz"
)
TEST_FILES_BAD=(
"H4sIAO1YYmAAA3PMSVTITVTIzi9JVABTIJ5jzpGZelwAX+46ehsAAAA="
"H4sIAO1YYmAAA3PMSVTITVTIzi85VABTIJ5jzpGZelwAX+86ehsAAAA="
"H4sIAAAAAAAAA3PMSVTITVTIzi9JVABTIJ5jzpGZelwAX+86ehsBAAA="
"H4sIAAAAAAAAAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIogUrnJFZVKqTkp+txAQBqzFDrLQAAAA=="
"H4sIAAAAAAAAAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9L"
)
TEST_COUNT_BAD=5


test_files_b64()
{
    local _1=$1[@]
    local NAMES=(${!_1})
    local _2=$2[@]
    local FILES=(${!_2})

    i=0
    errors=0
    for f in ${NAMES[@]}; do
        echo -n ${NAMES[$i]}": "
        echo ${FILES[$i]} | base64 -d | $GZIP -t
        exit_status=$?

        if [ $exit_status -gt 0 ]; then
            echo "error!"
            ((errors=errors+1))
        else
            echo "ok."
        fi
        ((i=i+1))
    done

    return $errors
}

test_files_b64 TEST_FILES_GOOD_NAMES TEST_FILES_GOOD
errors_good=$?

test_files_b64 TEST_FILES_BAD_NAMES TEST_FILES_BAD
errors_bad=$?


echo -n "Test of file/s integrity: "

if [ $errors_good -eq  $TEST_COUNT_GOOD ] || [ $errors_bad -eq  $TEST_COUNT_BAD ]; then
    echo "passed."
    echo ""
    exit 0
else
    echo "failed."
    echo ""
    exit 1
fi

