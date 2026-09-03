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
report "shy flag, index slot word, write completions, recovery tie-break" $?

# Runs a binary with a throwaway /scratch0/kvell. $DBDIR is reused when set.
sandboxed() {
  local dir=${DBDIR:-$(mktemp -d /tmp/stellar-reins-XXXXXX)}
  mkdir -p "$dir"
  timeout "${TIMEOUT:-1800}" unshare -Urm bash -c \
    "mount --bind '$dir' /scratch0/kvell && exec \"\$@\"" _ "$@"
}
filter() { grep -v "SLAB WORKER\|^CORE:\|Reserving memory\|page_cache_init\|BREAKDOWN\|^GC:\|^Reinsert:\|^Prune:"; }

echo
echo "== reinsertion under load, then recovery over the shy records it left =="
# The end-to-end test with the worker running and slabs handed to it every
# round: an exact read model, zero tolerance. Then the same database is
# re-opened: recovery meets thousands of shy records and abandoned slots.
make -j"$(nproc)" test/test_prune_links >/dev/null || exit 1
DBDIR=$(mktemp -d /tmp/stellar-reins-XXXXXX)
export DBDIR
sandboxed env PRUNE_REINS=1 ./test/test_prune_links 2>&1 | filter | grep -E "FAIL|published|prunes under load"
report "shy reinsertion under load" "${PIPESTATUS[0]}"
sandboxed ./test/test_prune_links verify 2>&1 | filter | grep -E "Recovery:|FAIL|state after"
report "recover a database full of shy records" "${PIPESTATUS[0]}"
unset DBDIR

echo
echo "== $pass passed, $fail failed =="
[ "$fail" = 0 ]
