#!/bin/sh

set -e

if [ -z "$1" ] ; then
    echo "usage: $0 path/to/from"
    exit 1
fi
from="$1"

r="$(echo -e "\x1b[31m")"
n="$(echo -e "\x1b[m")"

failed="0"

pass_case () {
    if ! text="$(echo -n -e "$1" | "$from")" ; then
        echo "${r}nonzero exit code on input: $n$1"
        failed=$((failed + 1))
        return 0
    fi
    if [ "$text" != "$2" ] ; then
        echo "${r}for input: $n$1$r got: $n$text$r but expected: $n$2"
        failed=$((failed + 1))
        return 0
    fi
}

fail_case () {
    if text="$(echo -n -e "$1" | "$from" 2>/dev/null)" ; then
        echo "${r}expected to reject input: $n$1$r but got: $n$text"
        failed=$((failed + 1))
        return 0
    fi
}

pass_case "test@test.com" "test@test.com"
pass_case "<test@test.com>" "test@test.com"
pass_case "\"Last, First\" <test@test.com>" "test@test.com"
pass_case "\"Last, First\" <test@test.com>, asdf@asdf.com" "test@test.com"
pass_case "test @ test.com" "test@test.com"
pass_case "test\r\n @\r\n test.com" "test@test.com"
pass_case "test \r\n @ \r\n test.com" "test@test.com"
pass_case ",,test@test.com,," "test@test.com"
pass_case ",,test@test.com,," "test@test.com"
pass_case "test@\r\n (this, is, a, comment\\\\\n!!@#$%)test.com" "test@test.com"
pass_case "test@\r\n (more comment\\\\))test.com" "test@test.com"
pass_case "<test@test.com>, <asdf@asdf.com>" "test@test.com"
pass_case "test\r\n \r\n @\n \n \n test.com" "test@test.com"
pass_case "te|st@test.com" "te|st@test.com"
pass_case "test@(comment)test.com" "test@test.com"
pass_case "test@(comment\\\\\x00)test.com" "test@test.com"
pass_case "1234@test.com" "1234@test.com"

fail_case "\r\ntest@test.com"
fail_case "test@\r\ntest.com"
fail_case "test@(comment))test.com"
fail_case "test@test.com>"
fail_case "A <test@test.com"

exit "$failed"
