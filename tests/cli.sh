#!/bin/sh
# End-to-end tests for the calci command.
#
# Usage: sh tests/cli.sh [PATH-TO-CALCI]
#
# Runs calci with various arguments and inputs and checks what it prints and
# how it exits. Prints every failed check, and exits with status 1 if any.

calci=${1:-./calci}
tmp=$(mktemp -d 2>/dev/null) || tmp=${TMPDIR:-/tmp}/calci-test.$$
mkdir -p "$tmp" || exit 1
trap 'rm -rf "$tmp"' EXIT

checks=0
failures=0

# run INPUT [ARGUMENT...]
#   Runs calci with the arguments, feeding it INPUT (after expanding escapes
#   such as \n) on standard input. Sets $status, and $out and $err to what it
#   printed on stdout and stderr, without carriage returns.
run() {
    input=$1
    shift
    printf '%b' "$input" | "$calci" "$@" >"$tmp/out" 2>"$tmp/err"
    status=$?
    out=$(tr -d '\r' <"$tmp/out")
    err=$(tr -d '\r' <"$tmp/err")
}

fail() {
    failures=$((failures + 1))
    printf 'FAIL: %s\n' "$1"
    printf '  status: %s\n' "$status"
    printf '  stdout: %s\n' "$out"
    printf '  stderr: %s\n' "$err"
}

# check DESCRIPTION COMMAND...
#   Passes if COMMAND succeeds.
check() {
    description=$1
    shift
    checks=$((checks + 1))
    "$@" || fail "$description"
}

# expect DESCRIPTION STATUS STDOUT
#   Passes if the last run exited with STATUS and printed exactly STDOUT.
expect() {
    checks=$((checks + 1))
    if [ "$status" != "$2" ] || [ "$out" != "$3" ]; then
        fail "$1 (expected status $2, stdout '$3')"
    fi
}

contains() {
    case $1 in
    *"$2"*) return 0 ;;
    esac
    return 1
}

lines() {
    printf '%b' "$1"
}

# --- An expression on the command line --------------------------------------

run '' '2 + 3 * 4'
expect 'quoted expression' 0 14

run '' 2 + 3
expect 'expression split into several arguments' 0 5

run '' '-5 + 3'
expect 'expression starting with a minus sign' 0 -2

run '' -- '-5 + 3'
expect '"--" before the expression' 0 -2

run '' 'ans'
expect 'ans starts at 0' 0 0

run '' '1 / 0'
expect 'math error' 1 ''
check 'math error message' contains "$err" 'calci: error: division by zero'

run '' '2 + * 3'
expect 'syntax error' 1 ''
check 'error shows the expression and a caret' \
    [ "$err" = "$(lines 'calci: error: expected a number\n    2 + * 3\n        ^')" ]

run '' "$(lines '1\t/ 0')"
check 'caret lines up after a tab' \
    [ "$err" = "$(lines 'calci: error: division by zero\n    1\t/ 0\n     \t^')" ]

run '' "$(lines '1 \033 2')"
check 'control characters are not echoed' contains "$err" '    1 ? 2'

run '' ''
expect 'empty expression' 1 ''
check 'empty expression message' contains "$err" 'error: empty expression'

# --- Help -------------------------------------------------------------------

for option in --help -h; do
    run '' "$option"
    check "$option exits with 0" [ "$status" = 0 ]
    check "$option shows the usage" contains "$out" 'Usage: calci'
    check "$option shows the reference" contains "$out" 'Functions:'
done

# --- Expressions on standard input ------------------------------------------

run ''
expect 'no input' 0 ''

run '1 + 1\n2 * 3\n'
expect 'one result per line' 0 "$(lines '2\n6')"

run '\n   \n\t\n4 / 2\n'
expect 'blank lines are skipped' 0 2

run '2 + 3\nans * 2\nans - 1\n'
expect 'ans is the previous result' 0 "$(lines '5\n10\n9')"

run '6\n1 / 0\nans\n'
expect 'ans survives an error' 1 "$(lines '6\n6')"

run '1 + 1\n1 / 0\n3\n'
expect 'an error does not stop the input' 1 "$(lines '2\n3')"
check 'errors give the line number' \
    contains "$err" 'calci: line 2: error: division by zero'

run '7 * 6'
expect 'last line without a newline' 0 42

run '1 + 1\r\n2\r\n'
expect 'CRLF line endings' 0 "$(lines '2\n2')"

run '1\nquit\n2\n'
expect 'quit' 0 1

run '1\n  EXIT  \n2\n'
expect 'exit, in any case and with blanks' 0 1

run 'help\n'
check 'help exits with 0' [ "$status" = 0 ]
check 'help prints the reference' contains "$out" 'Functions:'

run "$(printf '%04095d' 0)\n"
expect 'longest line allowed' 0 0

run "$(printf '%04096d' 0)\n2\n"
expect 'overlong line is skipped' 1 2
check 'overlong line is reported' \
    contains "$err" 'line 1: error: line is longer than 4095 characters'

# --- Output errors ----------------------------------------------------------

# Linux's /dev/full makes every write fail. Git Bash on Windows has a
# /dev/full too, but native Windows programs can write to it without error.
if [ "$(uname -s)" = Linux ] && [ -w /dev/full ]; then
    "$calci" '1 + 1' >/dev/full 2>"$tmp/err"
    status=$?
    out=''
    err=$(cat "$tmp/err")
    check 'write error exits with 1' [ "$status" = 1 ]
    check 'write error is reported' contains "$err" 'cannot write'
fi

printf '%d checks, %d failed\n' "$checks" "$failures"
[ "$failures" -eq 0 ]
