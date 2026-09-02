#!/bin/bash
# Builds and runs the pruning test suite.
#
# Everything that touches real slab files runs inside a user namespace with a
# private bind mount over /scratch0/kvell, so the real database directory is
# never touched.
set -u
cd "$(dirname "$0")/.."

make -j"$(nproc)" test/test_prune_freeze test/test_prune_links >/dev/null || exit 1

# Runs a binary with a throwaway /scratch0/kvell. $DBDIR is reused when set,
# which is how the recovery case re-opens the database it just wrote.
sandboxed() {
  local dir=${DBDIR:-$(mktemp -d /tmp/stellar-prune-XXXXXX)}
  mkdir -p "$dir"
  timeout "${TIMEOUT:-1800}" unshare -Urm bash -c \
    "mount --bind '$dir' /scratch0/kvell && exec \"\$@\"" _ "$@"
}

pass=0; fail=0
report() { # report <name> <exit code> [expected code]
  local want=${3:-0}
  if [ "$2" = "$want" ]; then echo "  PASS  $1"; pass=$((pass+1))
  else echo "  FAIL  $1 (exit $2, expected $want)"; fail=$((fail+1)); fi
}

filter() { grep -v "SLAB WORKER\|^CORE:\|Reserving memory\|page_cache_init\|BREAKDOWN\|^GC:\|^Reinsert:\|^Prune:"; }

echo "== in-process unit tests (no slab files) =="
./test/test_prune_freeze >/dev/null 2>&1
report "slab_freeze (reject paths, drain, freeze vs. split race)" $?

echo
echo "== end-to-end through the real pipeline =="
# Covers the §6 matrix: both triple orientations and the D == NULL case, a
# rebalanced area, an empty N, freeze rejections, readers and writers over all
# three ranges, a writer parked on the frozen leaf, retire with a pinned
# reader, repeated prunes up the chain, and a rebalance interleaved with
# prunes.
sandboxed ./test/test_prune_links 2>&1 | filter
report "prune end-to-end" "${PIPESTATUS[0]}"

echo
echo "== with the background reinsertion worker =="
PRUNE_REINS=1 sandboxed env PRUNE_REINS=1 ./test/test_prune_links 2>&1 | filter | tail -4
report "prune with reinsertion" "${PIPESTATUS[0]}"

echo
echo "== known-bad: recovery after pruning =="
# Recovery only runs when a file named slab-1-0-* is present (root_exists() in
# slab.c), and pruning unlinks the original root slab as soon as its triple is
# picked. A restart then finds no root, builds an empty database and reports 0
# recovered entries. See PRUNING_NOTES.md; recovery is deferred by the plan.
DBDIR=$(mktemp -d /tmp/stellar-prune-XXXXXX)
export DBDIR
sandboxed ./test/test_prune_links >/dev/null 2>&1
report "prune (populate for recovery)" $?
if ls "$DBDIR"/slab-1-0-* >/dev/null 2>&1; then
  echo "  NOTE  the root slab survived this run"
else
  echo "  NOTE  the root slab was pruned away, so recovery cannot start"
fi
sandboxed ./test/test_prune_links verify 2>&1 | filter | tail -2
report "read back after recovery (expected to fail today)" "${PIPESTATUS[0]}" 1
unset DBDIR

echo
echo "== rebalancing must still be correct =="
./test/test_rebalance >/dev/null 2>&1;     report "rebalance structural" $?
./test/test_rebalance_api >/dev/null 2>&1; report "rebalance runtime API" $?

echo
echo "== $pass passed, $fail failed =="
[ "$fail" = 0 ]
