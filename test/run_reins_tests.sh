#!/bin/bash
# Builds and runs the reinsertion ("shy writes") test suite.
set -u
cd "$(dirname "$0")/.."

make -j"$(nproc)" test/test_shy >/dev/null || exit 1

pass=0; fail=0
report() { # report <name> <exit code> [expected code]
  local want=${3:-0}
  if [ "$2" = "$want" ]; then echo "  PASS  $1"; pass=$((pass+1))
  else echo "  FAIL  $1 (exit $2, expected $want)"; fail=$((fail+1)); fi
}

echo "== in-process unit tests (no slab files) =="
./test/test_shy >/dev/null 2>&1
report "shy flag helpers and index slot word" $?

echo
echo "== $pass passed, $fail failed =="
[ "$fail" = 0 ]
