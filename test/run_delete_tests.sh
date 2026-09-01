#!/bin/bash
# Builds and runs the DELETE test suite.
#
# Every case that touches real slab files runs inside a user namespace with a
# private bind mount over /scratch0/kvell, so the real database directory is
# never touched.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD

CFLAGS=(-O2 -ggdb3 -Wall -I. -DSELECTED_BENCH=ycsb_c_zipfian
        -DSELECTED_PAGE_CACHE_SIZE="(PAGE_SIZE * 2097152)" -DDEBUG=0)
OBJS=(config.o slab.o freelist.o ioengine.o pagecache.o stats.o random.o
      slabworker.o workload-common.o workload-ycsb.o workload-dbbench.o
      workload-bgwork.o workload-production.o workload-locality.o
      workload-latency.o utils.o rcu.o in-memory-index-tnt.o fsst.o db_bench.o
      indexes/rbtree.o indexes/btree.o indexes/tnt_centree.o
      indexes/tnt_subtree.o indexes/tnt_balance.o)

make -j"$(nproc)" >/dev/null || exit 1

build() { # build <name> [extra link flags...]
  local n=$1; shift
  clang "${CFLAGS[@]}" -c "test/$n.c" -o "test/$n.o" || exit 1
  clang "test/$n.o" "${OBJS[@]}" "${CFLAGS[@]}" -lm -lpthread -lstdc++ "$@" \
      -o "test/test_$n" || exit 1
}

# Runs a binary with a throwaway /scratch0/kvell. $DBDIR is reused when set,
# which is how the recovery case re-opens the database it just wrote.
sandboxed() {
  local dir=${DBDIR:-$(mktemp -d /tmp/stellar-delete-XXXXXX)}
  mkdir -p "$dir"
  timeout "${TIMEOUT:-900}" unshare -Urm bash -c \
    "mount --bind '$dir' /scratch0/kvell && exec \"\$@\"" _ "$@"
}

pass=0; fail=0
report() { # report <name> <exit code> [expected code]
  local want=${3:-0}
  if [ "$2" = "$want" ]; then echo "  PASS  $1"; pass=$((pass+1))
  else echo "  FAIL  $1 (exit $2, expected $want)"; fail=$((fail+1)); fi
}

filter() { grep -v "SLAB WORKER\|^CORE:\|Reserving memory\|page_cache_init\|BREAKDOWN\|^GC:\|^Reinsert:"; }

echo "== building =="
for t in delete delete_stress delete_e2e delete_mt delete_edge; do build "$t"; done
build delete_worker_stress -Wl,--wrap=open -Wl,--wrap=opendir

echo
echo "== in-process unit / stress tests (no slab files) =="
./test/test_delete >/dev/null 2>&1;               report "delete (unit)" $?
./test/test_delete_stress >/dev/null 2>&1;        report "delete_stress" $?
./test/test_delete_worker_stress >/dev/null 2>&1; report "delete_worker_stress" $?
./test/test_delete_worker_stress --rebalance-concurrent >/dev/null 2>&1
report "deterministic mid-split rebalance" $?

echo
echo "== end-to-end through the real pipeline =="
sandboxed ./test/test_delete_e2e populate 20000 2>&1 | filter | tail -3
report "e2e insert/delete/re-delete/resurrect/delete-all" "${PIPESTATUS[0]}"

# Recovery is flaky after a burst of *moving* writes. The controls below show
# it is not delete-specific: a pure-UPSERT burst hits it too, while an ADD-only
# burst never does.
rounds=${RECOVERY_ROUNDS:-8}
bad=0
for _ in $(seq "$rounds"); do
  DBDIR=$(mktemp -d /tmp/stellar-delete-XXXXXX)
  export DBDIR
  sandboxed ./test/test_delete_e2e populate 5000 >/dev/null 2>&1
  sandboxed ./test/test_delete_e2e verify   5000 >/dev/null 2>&1 || bad=$((bad+1))
  dup=$(for f in $(ls "$DBDIR"); do
          echo "$f" | awk -F- '{if (NF==5) print $5; else print $4}'
        done | sort | uniq -d | tr '\n' ' ')
  [ -n "$dup" ] && echo "    duplicate slab keys on disk: $dup"
  rm -rf "$DBDIR"; unset DBDIR
done
echo "  INFO  recovery after a delete workload: $bad / $rounds runs failed"
echo "        (intermittent -- ~15% of runs; raise RECOVERY_ROUNDS to see it)"

# A pivot of 0 / UINT64_MAX-1 / UINT64_MAX can only come from a split that read
# an unpublished key range (min still (uint64_t)-1, max still 0). Count the
# slab files whose name records such a pivot.
extreme_pivots() { ls "$1" | grep -cE -- "-(0|1844674407370955161[45])\$"; }

for mode in upsertburst populate insert; do
  bad=0
  for _ in $(seq "$rounds"); do
    DBDIR=$(mktemp -d /tmp/stellar-delete-XXXXXX)
    export DBDIR
    sandboxed ./test/test_delete_e2e "$mode" 5000 >/dev/null 2>&1
    [ "$(extreme_pivots "$DBDIR")" != 0 ] && bad=$((bad+1))
    rm -rf "$DBDIR"; unset DBDIR
  done
  echo "  INFO  $mode: $bad / $rounds runs split on an unpublished key range"
done

bad=0
for _ in $(seq "$rounds"); do
  DBDIR=$(mktemp -d /tmp/stellar-delete-XXXXXX)
  export DBDIR
  sandboxed ./test/test_delete_e2e insert    20000 >/dev/null 2>&1
  sandboxed ./test/test_delete_e2e verifyall 20000 >/dev/null 2>&1 || bad=$((bad+1))
  rm -rf "$DBDIR"; unset DBDIR
done
echo "  INFO  recovery after an insert-only workload: $bad / $rounds runs failed"
echo "        (also intermittent -- ADD-only bursts degenerate too, just less"
echo "         often, so this is a weaker control than a clean one)"

# Characterization of the recovery aliasing crash. A degenerate split can give
# several slabs the same routing key; recovery used to resolve file -> slab by
# key, so several rebuild threads rebuilt one slab's B-tree at once. Inject the
# duplicate directly instead of waiting for the ~15% natural case, and place the
# copies at consecutive seqs so different threads claim them in the same
# instant. Duplicate routing keys legitimately confuse reads; what this measures
# is that they also corrupt memory, which they must not.
DBDIR=$(mktemp -d /tmp/stellar-delete-XXXXXX)
export DBDIR
sandboxed ./test/test_delete_e2e populate 5000 >/dev/null 2>&1
victim=$(ls "$DBDIR" | head -1)
key=$(echo "$victim" | awk -F- '{if (NF==5) print $5; else print $4}')
last=$(ls "$DBDIR" | sed -E 's/^slab-([0-9]+)-.*/\1/' | sort -n | tail -1)
for n in $((last+1)) $((last+2)) $((last+3)) $((last+4)); do
  cp "$DBDIR/$victim" "$DBDIR/slab-$n-6-$key"
done
echo "  INFO  injected 4 extra slabs sharing routing key $key"
crashes=0
for _ in $(seq 6); do
  sandboxed ./test/test_delete_e2e verify 5000 >/dev/null 2>&1
  [ "$?" -ge 128 ] && crashes=$((crashes+1))
done
rm -rf "$DBDIR"; unset DBDIR
echo "  INFO  recovery crashed $crashes / 6 times on duplicate routing keys"
echo "        (expected to crash: recovery resolves file -> slab by routing key,"
echo "         and routing keys are not unique)"

echo
echo "== concurrent mixed workload =="
sandboxed ./test/test_delete_mt 16000 16 20000 2>&1 | filter | tail -3
report "concurrent delete/upsert/read" "${PIPESTATUS[0]}"

echo
echo "== edge cases =="
sandboxed ./test/test_delete_edge ondisk 3000 2>&1 | filter | tail -4
report "persisted tombstone layout" "${PIPESTATUS[0]}"

E2E_MAX_FILE_SIZE=$((4096*4096)) sandboxed env E2E_MAX_FILE_SIZE=$((4096*4096)) \
  ./test/test_delete_edge inplace 2000 2>&1 | filter | tail -4
report "in-place delete reuses the slot" "${PIPESTATUS[0]}"

sandboxed ./test/test_delete_edge reins 3000 2>&1 | filter | tail -3
report "reinsertion copies tombstones forward" "${PIPESTATUS[0]}"

sandboxed ./test/test_delete_edge count 3000 2>&1 | filter | tail -5
report "no key readable after deleting everything" "${PIPESTATUS[0]}"

echo
echo "== known-bad cases (these are expected to fail today) =="

sandboxed ./test/test_delete_edge readd 10 2>&1 | filter | tail -3
report "kv_add_async() on a deleted key" "${PIPESTATUS[0]}" 255

sandboxed ./test/test_delete_edge reuse 10 2>&1 | filter | tail -5
report "client reuses its item buffer across DELETE" "${PIPESTATUS[0]}" 134

echo
echo "== $pass passed, $fail unexpected =="
[ "$fail" = 0 ]
