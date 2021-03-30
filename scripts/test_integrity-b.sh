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

export TEST_FILES_GOOD_NAMES="\
ala1-1.gz \
fox2-1.gz"

export TEST_FILES_GOOD="\
H4sIAAAAAAAAA3PMSVTITVTIzi9JVABTIJ5jzpGZelwAX+86ehsAAAA= \
H4sIAAAAAAAAAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIoAUrnJFZVKqTkp+txAQBqzFDrLQAAAA=="

TEST_COUNT_GOOD=0

export TEST_FILES_BAD_NAMES="\
ala1-bad-crc.gz \
ala1-bad-flip.gz \
ala1-bad-len.gz \
fox2-bad-flip.gz \
fox2-bad-truncated.gz"

export TEST_FILES_BAD="\
H4sIAO1YYmAAA3PMSVTITVTIzi9JVABTIJ5jzpGZelwAX+46ehsAAAA= \
H4sIAO1YYmAAA3PMSVTITVTIzi85VABTIJ5jzpGZelwAX+86ehsAAAA= \
H4sIAAAAAAAAA3PMSVTITVTIzi9JVABTIJ5jzpGZelwAX+86ehsBAAA= \
H4sIAAAAAAAAAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIogUrnJFZVKqTkp+txAQBqzFDrLQAAAA== \
H4sIAAAAAAAAAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9L"

TEST_COUNT_BAD=5


## good files
i=0
k=0
errors_good=0

for N in ${TEST_FILES_GOOD_NAMES}; do
    #echo -n ${N}": "
    #echo "N: i=$i, k=$k"
    for M in ${TEST_FILES_GOOD}; do
        #echo "M: i=$i, k=$k"
        if [ $i -eq $k ]; then
            echo -n "$N"": "
            echo $M | base64 -d | $GZIP -t
            exit_status=$?

            if [ $exit_status -gt 0 ]; then
                echo "error!"
                errors_good=$((errors_good+1))
            else
                echo "ok."
            fi
        fi # i==k
        k=$((k+1))
    done # M
    i=$((i+1))
    k=0
done # N

## corruped files
i=0
k=0
errors_bad=0

for N in ${TEST_FILES_BAD_NAMES}; do
    #echo -n ${N}": "
    #echo "N: i=$i, k=$k"
    for M in ${TEST_FILES_BAD}; do
        #echo "M: i=$i, k=$k"
        if [ $i -eq $k ]; then
            echo -n "$N"": "
            echo $M | base64 -d | $GZIP -t
            exit_status=$?

            if [ $exit_status -gt 0 ]; then
                echo "error!"
                errors_bad=$((errors_good+1))
            else
                echo "ok."
            fi
        fi # i==k
        k=$((k+1))
    done # M
    i=$((i+1))
    k=0
done # N



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

