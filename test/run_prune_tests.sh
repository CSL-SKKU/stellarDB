#!/bin/bash
# Builds and runs the pruning test suite.
#
# Everything that touches real slab files runs inside a user namespace with a
# private bind mount over /scratch0/kvell, so the real database directory is
# never touched.
set -u
cd "$(dirname "$0")/.."

make -j"$(nproc)" test/test_prune_freeze test/test_prune_links test/test_split_skip \
  test/test_rebalance test/test_rebalance_api >/dev/null || exit 1

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
echo "== the final-slot writer must split (Known bugs 23) =="
# A moving UPSERT whose source slot index equals the destination's final slot
# index used to skip the split and strand every later writer to that range.
sandboxed ./test/test_split_skip 2>&1 | filter | tail -1
report "final-slot moving upsert splits" "${PIPESTATUS[0]}"
CONTROL=1 sandboxed env CONTROL=1 ./test/test_split_skip 2>&1 | filter | tail -1
report "final-slot moving upsert splits (control)" "${PIPESTATUS[0]}"

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
echo "== migration with ILI pruning and recovery =="
DBDIR=$(mktemp -d /tmp/stellar-prune-XXXXXX)
export DBDIR
sandboxed env PRUNE_STRESS_MIGRATE=1 ./test/test_prune_links 2>&1 | filter | grep -E "FAIL|migration|prunes under load|tests passed"
report "migration with ILI pruning under load" "${PIPESTATUS[0]}"
sandboxed ./test/test_prune_links verify 2>&1 | filter | tail -2
report "read back after migration and pruning recovery" "${PIPESTATUS[0]}"
unset DBDIR

echo
echo "== the automatic trigger (-p) =="
# Even with eligible triples, enabled pruning must wait below the threshold.
sandboxed ./test/test_prune_links auto-wait 2>&1 | filter | grep -E "FAIL|automatic|stale"
report "automatic pruning waits below the stale-ratio threshold" "${PIPESTATUS[0]}"
# The restructuring worker, woken on a timer, must bring the stale-slot ratio
# under the threshold on its own, and every key must still read correctly.
sandboxed ./test/test_prune_links auto 2>&1 | filter | grep -E "FAIL|automatic|stale"
report "automatic pruning brings the stale ratio under the threshold" "${PIPESTATUS[0]}"

echo
echo "== recovery of a pruned database =="
DBDIR=$(mktemp -d /tmp/stellar-prune-XXXXXX)
export DBDIR
sandboxed ./test/test_prune_links >/dev/null 2>&1
report "prune (populate for recovery)" $?
if [ -f "$DBDIR/ROOT" ]; then echo "  NOTE  ROOT -> slab-$(cat "$DBDIR/ROOT")"; fi
sandboxed ./test/test_prune_links verify 2>&1 | filter | tail -2
report "read back after recovery" "${PIPESTATUS[0]}"
unset DBDIR

echo
echo "== crash points =="
# Each mode _exit()s at the named point. Recovery must come back to the state
# before the interrupted operation (or after it, once it committed), delete the
# leftovers, and keep both trees consistent.
for point in 2 3; do
  DBDIR=$(mktemp -d /tmp/stellar-prune-XXXXXX)
  export DBDIR
  sandboxed ./test/test_prune_links crash-prune-$point >/dev/null 2>&1
  report "crash-prune-$point stops the process" $? 42
  sandboxed ./test/test_prune_links verify-crash 2>&1 | filter | grep -E "Recovery:|FAIL|state after"
  report "recover after crash-prune-$point" "${PIPESTATUS[0]}"
  unset DBDIR
done
DBDIR=$(mktemp -d /tmp/stellar-prune-XXXXXX)
export DBDIR
sandboxed ./test/test_prune_links crash-split >/dev/null 2>&1
report "crash-split stops the process" $? 42
sandboxed ./test/test_prune_links verify-consistent 2>&1 | filter | grep -E "Recovery:|FAIL|state after"
report "recover after crash-split" "${PIPESTATUS[0]}"
unset DBDIR

echo
echo "== a hole in a live leaf =="
DBDIR=$(mktemp -d /tmp/stellar-prune-XXXXXX)
export DBDIR
sandboxed ./test/test_prune_links hole-punch 2>&1 | filter | grep -E "hole|FAIL"
report "hole-punch" "${PIPESTATUS[0]}"
sandboxed ./test/test_prune_links hole-verify 2>&1 | filter | grep -E "Recovery:|FAIL|state after"
report "recover past the hole, append lands after it" "${PIPESTATUS[0]}"
unset DBDIR

echo
echo "== rebalancing must still be correct =="
./test/test_rebalance >/dev/null 2>&1;     report "rebalance structural" $?
./test/test_rebalance_api >/dev/null 2>&1; report "rebalance runtime API" $?

echo
echo "== $pass passed, $fail failed =="
[ "$fail" = 0 ]
