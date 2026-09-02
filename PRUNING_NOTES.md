# Pruning — notes accumulated per step

`PRUNING_PLAN.md` asks each step to update `AGENTS.md`. `AGENTS.md` lives outside this
worktree and is shared with the other implementation, so the doc deltas land here instead;
merge them into `AGENTS.md` when a version is picked.

## Step 0 — locking model / center-tree fields

- `centree_node_t.lu_parent` is `_Atomic`. It is still assigned once, when a split creates
  the node, and is read by upward walkers without any lock. Pruning is the only operation
  that ever rewires it. Use `centree_lu_parent()` / `centree_lu_parent_store()`; do not
  touch the field directly.
- `centree_node_t.lu_child[2]` (`CENTREE_LU_LEFT`, `CENTREE_LU_RIGHT`) are the history
  back-pointers. Written only by the splitter, before the child is published, and by the
  single pruner. Plain fields, no lock.
- Invariant relied on by both: **history in-order == routing in-order**, and a routing leaf
  is a history leaf (so its `lu_child` slots are both `NULL`). `centree_insert()` asserts
  the slot it is about to fill was `NULL`. `test/test_prune_links` checks the invariant
  after real splits, after a rebalance, and after post-rebalance writes.
- `maintenance_lock` (in `in-memory-index-tnt.c`, `tnt_maintenance_lock()` /
  `tnt_maintenance_unlock()`) serializes the operations that restructure the center tree.
  `tnt_rebalancing()` takes it internally, so the existing direct callers (`main.c`, tests)
  are covered. Lock order:

      maintenance_lock -> CENTREE_RESTRUCTURING -> RCU writer -> centree_root_lock

  It is statically initialized, because `tnt_rebalancing()` is reachable before
  `centree_init()` runs (`test/rebalance_api.c` relies on that path returning `-EINVAL`).
- `struct slab.released` exists but is unused until the retire/release step.

## Step 1 — read path

- `tnt_index_lookup()` restarts the whole descent (`centree_find_leaf` + upward walk) when it
  finds `node->removed`, instead of continuing to `lu_parent`. A retired node's valid entries
  live in its replacement, and only the freshly routed topology leads there; skipping past a
  retired leaf silently loses keys. Per-lookup counters (`tmp_try`, `upward_len`, `count`) are
  reset on restart; the `LEAF_FOUND` timing stage is recorded only for the first descent.
- The `removed` check is taken under the slab's `tree_lock`, before the subtree is touched, and
  `read_ref++` happens under that same lock. So a retired slab can never gain a new reader, and
  once its refs reach zero they stay zero.
- Ownership of that reference: `tnt_index_lookup()` returns it to the caller. The read path
  hands it to `read_item_async_cb()`, which drops it. Any other caller must call
  `tnt_index_lookup_unref()` (the index-only benchmarks in `test/main.c`, `test/reinsert.c`,
  `test/delete_stress.c`, `test/delete_worker_stress.c` and `test/delete.c` now do).
- `tnt_set_index_lookup_step_test_hook()` fires once per upward step, before the node is read;
  `test/test_prune_links` uses it to retire a node mid-walk and assert the walk comes back to
  the same node.
- Nothing sets `removed = 1` in production yet, so this step is inert until the splice lands.

### Trap noticed for later

`centree_balance_publish()` stores `removed = 0` on every node it collected. It collects from
the routing root, so a pruned triple must be spliced out of routing *before* `removed` is set
(I-3 already requires that order) or a later rebalance would resurrect it.

## Step 2 — freeze primitive and the writer reference order

- Both writer descent loops (`centree_lookup_and_reserve`, `tnt_subtree_get`) now take
  `update_ref` **before** they read `full` or call `reserve_slot()`, and drop it again before
  descending or waiting on `child_flag` (I-8). A writer that leaves the loop with a slot (or an
  in-place target) keeps the reference; the write completion drops it.
- `slab_freeze(s, budget)` (`slab.c`) is the pruner's side: CAS `last_item` to `nb_max_items`,
  `atomic_exchange` on `full`, then spin until `update_ref` drains. Returns the number of slots
  ever reserved, `-EBUSY` (a writer already took the final slot, so it owns the split) or
  `-ENOSPC` (over budget). Both failures leave the slab untouched.
- Freeze and split are decided by the same CAS on `last_item` that `reserve_slot()` uses, so for
  one slab exactly one of them happens. `test/test_prune_freeze` races them ~1200 times in four
  release patterns and asserts that.
- The `full`/`update_ref` pair is a Dekker handshake and only works if both sides use locked
  RMWs: `atomic_exchange` on `full`, `__sync_fetch_and_add` on `update_ref`. `reserve_slot()`
  still plain-stores `full = 1`, which is fine -- `full` is monotone and a writer setting it is
  not handshaking with anyone.
- The drain spins with `NOP10()`. It waits on other threads' I/O completions, so a yield would
  be friendlier; kept as a spin because the pruner is a background thread holding no locks.
- Nothing calls `slab_freeze()` yet.

### Deferred to Step 3

The plan's "a writer arriving after a freeze does not deadlock" test needs the restart-on-removed
wake-up, which is Step 3. Not written yet.

## Step 3 — blocked writers restart; reinsertion pins its source

- Both writer wait loops (`centree_lookup_and_reserve`, `tnt_subtree_get`) now wait on
  `child_flag == 0 && !removed` and restart the whole descent when `removed` is set. A pruned
  leaf never grows children, so retirement is the only other way out of that wait; testing both
  also covers a wake that races with the flag store. The update reference is already released
  before the wait (Step 2), so a restart leaks nothing.
- The writer's upward walk *skips* retired slabs instead of restarting: it only looks for an
  older copy to invalidate, and that copy was already carried into the replacement node.
- Reinsertion (`fsst.c`) pins its source slab with `read_ref` for the whole batch, after checking
  `removed` under `tree_lock`, and consults `s->subtree` under the read lock (it used to read it
  with no lock at all -- fine while historical slabs were immutable, not fine once retirement
  frees the subtree). Retirement mid-batch ends the pass. `slab_release_if_idle()` will be called
  at the unpin site in Step 8.
- `tnt_index_invalid()` needs no `removed` check: it descends from the routing root, and a retired
  node is spliced out of routing before `removed` is set, so it is unreachable there.
  `add_in_tree_for_upsert()`'s `old_s` invalidation is covered by its existing `min == -1` guard,
  which is why retirement keeps setting `min = -1`.

### Test note

`check_blocked_writer()` in `test/prune_links.c` freezes a live leaf, lets a real UPSERT park on
its `child_flag`, then makes the slab writable again *without* waking anyone (proving the writer
was parked, not spinning) and finally retires the node and wakes it with `child_flag` still 0 --
so the `removed` check is the only possible exit. Reverting that check makes the test hang and
fail. It relies on 100 ms being long enough for the writer to reach `futex_wait`.

## Step 4 — candidate selection

- `indexes/tnt_prune.c`: `prune_select()` evaluates the picking rule locally on the history tree
  (`leaf -> inner -> outer` with `inner->lu_child[side] == leaf` and
  `outer->lu_child[!side] == inner`), plus liveness (`child_flag` 1/1/0, nothing retired, the
  leaf's slab not full) and a conservative fit test. `prune_scan_for_candidate()` /
  `prune_count_candidates()` are the O(n) fallback discovery; the event-driven trigger is Step 9.
  Nothing mutates anything yet.
- `nb_items` is only decremented on invalidation, so it over-estimates the valid entries and the
  fit test is conservative. The authoritative fit test is `slab_freeze()`'s budget.
- `struct prune_candidate` and the prototypes live in `in-memory-index-tnt.h`; `tnt_centree()`
  was added there so the scan can reach the tree.

### What the workload has to look like

A pure ascending (or descending) load produces **no** candidates, and that is correct, not a bug:

- Every split appends the new leaf on the same side, so the chain reads
  `leaf = inner->lu_child[s]`, `inner = outer->lu_child[s]` -- same side, which the rule rejects
  because `sib`'s whole subtree sits between `inner` and `outer` in-order.
- The "left-behind" child of each split *is* on the opposite side and is historically adjacent,
  but `nb_items(inner) + nb_items(outer)` is then ~2 full slabs, so the fit test rejects it.

Candidates appear once overwrites have drained the internal slabs. `test/prune_links.c` therefore
runs three overwrite passes and finds 18 candidates out of ~39 leaves, covering both orientations
and the `D == NULL` (history root) case; those three are asserted.

### Equivalence check

`scan_ili()` in the test is a second implementation written from the AGENTS.md wording: walk the
routing in-order sequence, take every consecutive internal-leaf-internal triple, keep it when two
of the three have `lu_parent` pointing inside the triple. The two candidate sets must be equal --
checked after splits, after a rebalance (routing != history), and after overwrites. That is also
what catches a selector that wrongly accepted same-side chains, which the ascending phases produce
in quantity.

## Step 5 — building the merged slab N

- `prune_build_begin` / `add_source` / `finish` / `discard` in `indexes/tnt_prune.c` assemble N
  entirely off to the side: a fresh slab file, a fresh subtree, an unpublished node. Nothing
  points at it until the history link, so a rejected build is just discarded (I-1). Since the node
  is unpublished, it *may* be freed -- I-4 is about published nodes.
- Sources are merged **newest first** (inner, then outer; the leaf joins in Step 6) and a key
  already in N is skipped. Same result as the plan's "oldest first, later sources override"
  (precedence leaf > inner > outer) without writing a slot twice.
- Records are copied whole-slot and byte-exact, which is what keeps tombstones intact.
- Entries are snapshotted under the source's `tree_lock` and copied without it. Safe because an
  internal slab is immutable apart from the invalid hint, and an entry appears in the subtree only
  after its page write has completed -- so what the snapshot names is already on the device. An
  entry invalidated after the snapshot is copied anyway and lands in N as a stale entry, shadowed
  by the newer copy nearer the leaf (§7).
- The snapshot is sorted by source slot, so each source page is `pread` at most once. N is
  assembled in one page-aligned buffer sized to what selection said could survive, then written
  with a single `pwrite` of the used pages and `fsync`ed. Unused slots stay zeroed, which is what
  `item_is_empty()` reads.
- `full = 1` on N is deliberate (§7): every writer descent calls `reserve_slot()` on the nodes it
  passes, and N is immutable.
- `subtree_forall_entries()` was added to `tnt_subtree.cc` (the existing iterators give keys or
  slots, never both). `create_slab()` is now declared in `slab.h`.

### Verification and what it does not cover

`test/prune_links.c` builds N for every `leaf -> inner -> outer` chain whose two internal slabs
could fit in one slab -- prunable or not, because the merge does not care -- and checks N against
an expected set derived straight from the sources: same key set, byte-identical records,
tombstone flags preserved, every key inside N's `[min, max]`, counters consistent, plus the empty-N
case via begin+finish with no sources. Last run: 27 builds, 17 non-empty, 2034 entries of which
1554 tombstones.

Two counters worth watching, both 0 on every run so far:

- **stale-but-unmarked**: entries that survived into N but whose value differs from what a normal
  READ returns, i.e. the invalid hint was missed. 0 means invalidation is reliable in this
  workload.
- **duplicated across sources**: keys valid in *both* internal slabs. 0 means the source
  precedence rule is a safety net this workload never triggers -- it only matters when an
  invalidation was missed. Worth remembering: precedence is therefore effectively untested by the
  real workload, and only the newest-first structure of the code enforces it.

### Workload note

Selection and the merge want opposite things, which is why the test does both jobs separately:
enough overwrite passes and the internal slabs are 100% stale (18 candidates, every N empty);
too few and they are too full to fit (0 candidates). Two 90% passes plus deletes on the untouched
tenth gives real candidates *and* rich sources for the merge.

## Step 6 — freeze the leaf, copy it, link the history chain

- `prune_freeze_and_link()`: determine P/Q, stage and write the cold part, freeze the leaf, copy
  the leaf into N, `fsync`, then install the three history pointers. The caller must hold
  `tnt_maintenance_lock()` from here until the routing splice has published.
- P is the leaf's routing parent and is always one of the two internal nodes -- the triple is
  consecutive in-order and a leaf's routing parent is one of its in-order neighbours. N takes the
  routing position of the *other* one (Q) and therefore Q's pivot, level and file key. If P is
  neither, the candidate is dropped with `-EAGAIN` rather than guessed at.
- The cold pages are written *before* the freeze, so the window in which writers to the leaf's
  range are parked covers only the leaf's own entries plus one `pwrite`/`fsync`.
- `prune_build_flush()` writes only the pages staged since the last flush, which is what keeps the
  post-freeze write small.
- After the freeze the leaf's subtree is stable: a writer publishes its entry before dropping
  `update_ref`, and the freeze drains `update_ref`. Entries can still be *invalidated* later (by a
  writer that restarts after the splice and supersedes one); such an entry is copied into N anyway
  and is shadowed by the newer copy nearer the leaf.
- `lu_child[side] = star`, `lu_child[!side] = sib` is correct in both orientations: in-order the
  triple reads `sib-subtree, inner, leaf, outer, star-subtree` for side 1 and
  `star-subtree, outer, leaf, inner, sib-subtree` for side 0, and in both cases N inherits sib on
  the left and star on the right. Worked through explicitly because getting it backwards would
  reverse the in-order sequence and only show up as a lookup miss much later.
- Exactly three history pointers are written (N's parent, D's child slot) plus the two external
  `lu_parent` stores (I-12).

### The intermediate state is deliberately inconsistent

Between the history link and the routing splice, N is in the history chain while the triple is
still in the routing tree. So:

- `test/prune_links.c` must not call `validate()` after `check_prune_link()` -- routing in-order
  and history in-order legitimately differ there.
- Reads are unaffected in either direction, because N holds every valid entry of the three and
  nothing is retired yet. The test proves this by snapshotting all 30000 keys before the link and
  re-reading them after: every one unchanged.
- Writers whose key routes to the frozen leaf are parked until the splice. That is the accepted
  latency hiccup, and it is why `check_prune_link()` has to be the last thing the test does.

## Step 7 — the routing splice

- `prune_splice_routing()` takes the RCU writer with no lock held, stages ten pointer writes at
  most, publishes under `centree_root_lock` (write) and only then retires the triple and wakes the
  writers parked on the frozen leaf (I-3, I-11, I-12).
- P and the leaf leave together: P is replaced by its other child S. Then N takes Q's children,
  parent and pivot, so it routes exactly as Q did. **Q's children are read after the first
  splice**, which is what makes `G == Q` work -- the slot that held P then holds S, and the
  following `parent` stores override `S->parent = G` so S ends up under N.
- The pivot that disappears is P's, so the leaf's key interval is absorbed by its in-order
  neighbour, whose `lu_parent` chain runs through N. That is why the history link has to be
  installed first (I-2).
- Re-reading P inside the writer is an assertion, not a decision. Concurrent splits cannot move it
  (they only add children under a leaf, and the frozen leaf cannot split); rebalancing is excluded
  by `maintenance_lock`. If it moved anyway, `rcu_writer_abort()` + `die()`: N is already in the
  history chain and the leaf is frozen, so there is no partial rollback.
- The triple's own routing pointers are left untouched, and `finish_deferred()` waits for the
  old-generation readers before mirroring, so nobody is walking them when the retired slots are
  updated.
- `node_count -= 2`. `depth` and `value.level` stay advisory and go stale; the rebalancer
  recomputes them.

### What the test covers

`test/prune_links.c` runs one prune in two halves, with a writer parked on the frozen leaf in
between, and checks: the parked writer completes only after the splice, the triple is retired,
`centree_validate_locked()` passes, `node_count` drops by exactly 2, and the new in-order sequence
is the old one with the triple replaced by N in its place. Then a sweep prunes until nothing is
prunable (77 -> 51 nodes over ~10 prunes), 18000 keys are rewritten into the pruned tree and read
back, and a stress phase runs 4 readers + 2 writers against an exact model while the pruner and
the rebalancer work: ~525k reads and ~78k writes, 6 prunes, 8 rebalances, zero disagreements.
All read snapshots (30000 keys) are identical across every stage.

### One anomaly, and it was the test

The stress phase first reported ~28 of 530k reads disagreeing. Every one was key 10000 reading
`0xbeef`: `check_blocked_writer()` resurrects the key it uses, and 10000 is in the deleted tenth,
so the model was wrong, not the DB. Reproduced identically with pruning *and* rebalancing disabled,
which is what identified it. Fixed by having that check use a key the later write phase rewrites.

## Step 8 — retire and release

- `slab_retire()` (`slab.c`) frees a retired slab's local index and filter under `tree_lock`, and
  resets `min = -1, max = 0`. The reset is what keeps the *older* range checks safe -- the writer
  descent and `add_in_tree_for_upsert()`'s invalidation consult `min`/`max` without looking at
  `removed`, and `key <= 0 && key >= UINT64_MAX` is false for every key. It is not how retirement
  is detected (I-6).
- `slab_release_if_idle()` closes and unlinks the file once both reference counts are zero, guarded
  by a CAS on `released` so it is idempotent. `fd = -1` afterwards, so a straggling read fails
  instead of hitting a reused descriptor.
- The nodes, slab descriptors and `hot_bits` are deliberately never freed (I-4).
- The three old copies of `if (min == -1 && refs == 0) { close; truncate }` are replaced by calls to
  `slab_release_if_idle()`. That also removes a latent hazard: the old condition could fire for a
  *live* empty slab (`min` is `-1` until the first entry is published, and
  `add_in_tree_for_upsert()`'s duplicate paths skip the widening), which would have closed and
  truncated a slab still in use. The new condition requires `node->removed`.
- **The writer descent's transient reference matters here.** A writer takes `update_ref` before it
  looks at `full`, so it can hold a reference on a slab that is retired a moment later. That
  reference is dropped in the descent loop, not in an I/O completion, so `slab_release_if_idle()`
  is called there too -- otherwise the pruner's own call would find `update_ref != 0` and the file
  would never be released. The transient holder never touches `fd` or `subtree`, so closing the
  file underneath it is harmless.
- No new reference can appear on a retired slab: readers and the reinsertion worker check `removed`
  under `tree_lock` before taking one. So once both counts hit zero they stay zero.

### Verification

`test/prune_links.c` runs a full prune with one of the retired slabs pinned as if a read were in
flight: the two idle files are gone and their `fd`s are -1 immediately, while the pinned one keeps
its file and descriptor until the reference is dropped, and disappears on the very next
`slab_release_if_idle()`. The suite then asserts **slab file count == node count** after the prune
sweep and again after the stress run -- so the space is really reclaimed, not just unlinked from
the index. `PRUNE_REINS=1` repeats the stress phase with the background reinsertion worker running
and slabs handed to it while the pruner works (9 prunes, ~495k reads, ~76k writes, no
disagreements).

### One anomaly, again the test

The file-count assertion first reported three extra files. That was `check_prune_link()`, which
deliberately stops mid-prune and did not retire its triple; it now retires at the end.

## Step 9 — scheduling, config, and what the end-to-end run found

- `tnt_prune_once()` does a whole prune under `tnt_maintenance_lock()`: select, build, freeze,
  link, splice, retire. Selection runs inside the lock, so a candidate cannot go stale before its
  leaf is frozen. Returns `TNT_PRUNE_DONE`, `TNT_PRUNE_NOOP`, or a negative errno;
  `-EAGAIN`/`-EBUSY`/`-ENOSPC` mean the candidate was dropped with nothing changed and are normal
  races, not failures.
- The existing restructuring worker runs it, one prune per wake-up, after the rebalance check.
  One thread for both maintenance operations means they exclude each other by construction; the
  mutex remains for `main.c` and the tests. `main.c` now starts that worker when either
  `--with-rebal` or `--with-prune` is given.
- Config: `-p/--with-prune`, `--prune-margin <slots>` (slack kept free in the merged slab) and
  `--prune-min-age <slabs>` (skip a triple whose leaf is among the newest N slabs, so the pruner
  stays off the slab the writes are landing in). `slab_create_sequence()` provides the age.
- `test/run_delete_tests.sh` keeps its own object list and needed `indexes/tnt_prune.o` added.

### A bug of mine, found only by the end-to-end run

Step 6 sized N's buffer from the *selection bound* (`cold_bound + nb_items`) and derived the freeze
budget from what was left of it. But `slab_freeze()` can only count **slots ever reserved**
(`last_item`), which is ≥ the valid count. So for the most attractive candidates -- the ones whose
internal slabs are 100% stale, `cold_bound == 0` -- the budget was exactly the leaf's valid count
and any leaf with a single invalidated entry was rejected with `-ENOSPC`, silently. Every prune in
a uniform benchmark was being dropped.

The plan's `budget = nb_max_items - cold_count` is what avoids this, so N's buffer now covers a
whole slab. To keep that cheap it is `mmap`ed rather than `malloc`ed + `memset`: page-aligned for
O_DIRECT, reads as zeroes (`item_is_empty`), and a page that never receives a record is never
faulted in. After the fix the same benchmark does 24 background prunes.

None of the unit or single-prune tests could have caught this: they all had `cold_bound > 0`,
which happens to leave exactly enough slack.

### Where pruning does and does not fire

`ycsb_a_uniform`, 20k keys, 1M requests, reinsertion + rebalancing + pruning on: **24 prunes**,
no faults, and one prune consumed a node an earlier prune had produced (`Prune: 76 <- 64/71/54`),
which is the "repeated prunes up the chain" case.

`ycsb_a_zipfian`, same shape: **0 prunes**, and the instrumentation says why -- 8 of 16 leaves had
a historically adjacent triple, and *all* of them failed the fit test with
`cold = 2048 of 1024 slots`, i.e. both internal slabs were 100% valid. Under a skewed workload the
immediate history ancestors of a live leaf are young and mostly still authoritative, so there is
nothing to reclaim. That is the rule working as specified (AGENTS.md: "works effectively well when
history is skewed, but may struggle to find candidates if the history chain is well balanced"),
not a defect -- but it does mean pruning reclaims little under zipfian traffic.

### The trigger inherited from the rebalancer

The restructuring worker only runs when `distributor utilization >= 80% && I/O worker utilization
<= 50%`. In the benchmark above the I/O workers sit at ~99%, so with the default thresholds the
gate never opens and **no background prune fires at all** (the 24 prunes above needed
`make DISTRIBUTOR_HIGH_UTIL=0 IO_WORKER_LOW_UTIL=100`). That gate is a sensible policy for
rebalancing, which trades index depth against CPU, but pruning is about space, and space pressure
is unrelated to how busy the I/O workers are. Worth revisiting; left as the plan specifies.

### A pre-existing bug that pruning exposed

One stress run died in the pruner with `slab 129 slot 188 not holding indexed key 11272` -- the
local index and the file disagreed about a slot. It is not pruning's doing:

- `fsst.c` sets `cb->fsst_slab = s` and `cb->fsst_idx = slot_idx` (the **source** slot) *before*
  calling `centree_lookup_and_reserve()`.
- `slabworker.c`'s UPSERT case corrects those fields from the entry it found, but only
  `if (e && callback->fsst_slab == NULL)` -- and the reinsertion worker calls
  `remove_and_add_item_async()` directly anyway.
- `remove_and_add_item_async()`'s in-place branch (`slab_idx == -1`) does
  `callback->slab_idx = callback->fsst_idx`, so the record is written into the **destination**
  slab at the **source's** slot index, overwriting whatever record that slot held.

It needs a key that reinsertion is copying forward to already exist in the current leaf, which is
why it is rare (once in ~6 runs with `-r`). This is very likely the defect AGENTS.md records as
"background reinsertions can silently drop real-upserts ... We'll leave this bug intentionally", so
it is reported, not fixed. The pruner now skips such a slot with a warning and keeps the database
running instead of dying; `prune_bad_slot_count()` counts them. The key was already lost when the
slot was overwritten.

---

# Step 10 — the AGENTS.md section

The text below is written to replace AGENTS.md's "Pruning implementation" (and to amend its
locking-model section). It lives here because AGENTS.md is outside this worktree.

## Pruning implementation

Pruning replaces three center-tree nodes -- an internal node, a leaf and an internal node that are
consecutive in the in-order sequence *and* adjacent in the history chain -- with one new immutable
node `N` holding only their valid entries. It is the only operation that ever changes `lu_parent`,
and it changes exactly two `lu_parent` pointers outside the triple.

Names, following the plan: `leaf` (L), `inner` = `L->lu_parent`, `outer` = `inner->lu_parent`,
`up` (D) = `outer->lu_parent` (may be NULL), `sib` (A) = inner's other history child,
`star` (*) = outer's other history child, `side` (s) with `inner->lu_child[side] == leaf`.
`P` is the leaf's **routing** parent, always one of `{inner, outer}`; `Q` is the other one and is
always a routing ancestor of `P`.

### Selection (`indexes/tnt_prune.c`, `prune_select`)

A triple is prunable iff `outer->lu_child[!side] == inner` (opposite sides -- same-side placement
puts `sib`'s whole subtree between them in-order), both internal nodes are fully split
(`child_flag == 1`), nothing in the triple is retired, the leaf is a live appendable leaf
(`child_flag == 0` and its slab not `full`), and
`nb_items(inner) + nb_items(outer) + nb_items(leaf) + prune_margin <= nb_max_items`.
`nb_items` over-estimates the valid entries, so this test is conservative; `slab_freeze()`'s
budget is the authoritative one. `--prune-min-age` additionally skips a triple whose leaf is among
the newest N slabs.

### Order of operations (one prune)

Everything below runs under `tnt_maintenance_lock()`, which excludes the rebalancer:

1. **Build `N` cold.** Fresh slab file, fresh subtree, unpublished node, taking `Q`'s pivot, level
   and file key. Sources are merged newest first (`inner`, then `outer`) and a key already staged
   is skipped, which implements precedence `leaf > inner > outer`. Records are copied whole-slot
   and byte-exact, so tombstones survive untouched. Entries are snapshotted under the source's
   `tree_lock`; the copying happens outside it. The staged pages are written before the freeze.
2. **Freeze the leaf.** `slab_freeze(leaf, nb_max_items - cold_count)` CASes `last_item` to
   `nb_max_items`, publishes `full` with an exchange, then waits for `update_ref` to drain.
   `-EBUSY` means a writer already took the final slot and owns the split; `-ENOSPC` means the
   slots do not fit. Both leave everything untouched, and the candidate is simply dropped.
3. **Copy the leaf into `N`**, `fsync`, finalize `N` (`last_item = nb_items = count`, `full = 1`,
   `child_flag = 1`). No abort path from step 2 on: a frozen childless leaf is a dead end for
   writers and only the splice releases them.
4. **History link**, in this order: `N`'s own `lu_parent`/`lu_child`, then `D`'s child slot, then
   `sib->lu_parent = N` and `star->lu_parent = N`.
5. **Routing splice.** Under the RCU writer, with no lock held: replace `P` by its other child
   `S`, then give `N` `Q`'s children, parent and pivot. `Q`'s children are read *after* the first
   splice, so the `G == Q` case picks up `S`. Publish under `centree_root_lock` (write), then
   drain the old generation.
6. **Retire.** Set `removed` on the three nodes, then `child_flag = 1` on the leaf and wake its
   waiters. Free the three local indexes under `tree_lock` and reset `min/max`; close and unlink
   each file once its reference counts reach zero.

Why that order: history before routing, because after the splice the leaf's key range is served by
an in-order neighbour whose `lu_parent` chain must already reach `N`; routing before retirement,
because a reader or writer that restarts on `removed` has to find the new topology.

### What the rest of the code has to do

- **Reads** (`tnt_index_lookup`) check `removed` under the slab's `tree_lock` before consulting the
  subtree and **restart the whole descent** if it is set -- walking past a retired node would miss
  entries that now live in `N`. The read reference is taken under that same lock, so a retired slab
  can never gain a new reader.
- **Writers** wait on `child_flag == 0 && !removed` and restart when retired: a pruned leaf never
  grows children. They release `update_ref` before descending or waiting. Their upward walk
  *skips* retired slabs -- it only looks for an older copy to invalidate.
- **Reinsertion** pins its source with `read_ref` after checking `removed` under `tree_lock`, and
  consults the source subtree under that lock.
- `min = -1, max = 0` on a retired slab keeps the older range checks (writer descent,
  `add_in_tree_for_upsert`'s invalidation) rejecting every key. It is *not* how retirement is
  detected: an empty live slab looks identical. Use `centree_node.removed`.

### Locking and lifetime

    maintenance_lock -> (CENTREE_RESTRUCTURING) -> RCU writer -> centree_root_lock

The pruner does **not** take `CENTREE_RESTRUCTURING`: it only needs the rebalancer excluded and
the RCU writer for the splice, so concurrent splits are unaffected except while it holds the
writer. `lu_parent` is atomic (`centree_lu_parent`/`_store`); `lu_child[2]` are plain, written only
by the splitter before publication and by the single pruner.

Pruned `centree_node`s, `struct slab` descriptors and `hot_bits` are **never freed**. That is the
reclamation strategy, not a leak: descriptors are held raw across async I/O and by the reinsertion
queue. Only the local index, the filter and the file are given back.

### Things that look wrong but are intended

- `N` is internal and not full, yet `full = 1`: every writer descent calls `reserve_slot()` on the
  nodes it passes, and `N` is immutable.
- `N` may contain stale entries whose invalid hint was missed. They are shadowed by the newer copy
  nearer the leaf, and only make future candidate counts conservative.
- `depth` and `value.level` go stale after a prune; they are advisory and the rebalancer
  recomputes them.

### Verification

    test/run_prune_tests.sh

Covers both triple orientations and the `D == NULL` (history root) case, a rebalanced area where
routing != history, an empty `N`, freeze rejections, the freeze/split race, a writer parked on the
frozen leaf, retirement with a pinned reader, slab-file count == node count, repeated prunes up the
chain, rebalancing interleaved with pruning, and ~500k reads plus ~78k writes against an exact
model while the pruner (and optionally the reinsertion worker) runs.

## Deferred, reported but not fixed

- **Recovery does not survive pruning.** `root_exists()` (`slab.c`) decides whether to recover by
  looking for a file named `slab-1-0-*`, and pruning unlinks the original root slab as soon as its
  triple is selected -- which is common, since the root is the oldest and stalest node. A restart
  then finds no root, creates a fresh empty database and reports 0 recovered entries, silently
  losing everything; worse, `create_sequence` restarts at 1, so the new files can collide with the
  surviving ones. `test/run_prune_tests.sh` characterizes this as a known-bad case. Recovery also
  walks the *routing* parent chain, which is only equivalent to the history chain before any
  rebalance or prune, and `rebuild_slabs` keys files by routing key and fails on duplicates. Making
  recovery pruning-aware needs a supersedes marker (or a commit record) and recovery-side handling;
  the plan defers all of it.
- **A crash between the history link and retirement** leaves `N` and all three originals on disk,
  which is the same recovery problem in its acute form.
- **`ADD` after a prune** may not detect a duplicate whose only copy is in a retired slab during
  the retire window: the writer walk skips retired slabs. `ADD` is already known-bad.
- **Reinsertion writes at the source's slot index** (pre-existing). `fsst.c` leaves
  `cb->fsst_slab`/`cb->fsst_idx` pointing at the *source* before calling
  `centree_lookup_and_reserve()`, which suppresses `slabworker.c`'s correction
  (`if (e && callback->fsst_slab == NULL)`), and `remove_and_add_item_async()`'s in-place branch
  then writes the record into the *destination* slab at the source's slot index, overwriting
  whatever record that slot held. Needs a key that reinsertion is copying forward to already exist
  in the current leaf, so it is rare (once in ~6 runs with `-r`). The pruner detects the resulting
  index/file disagreement and skips the slot with a warning rather than dying;
  `prune_bad_slot_count()` counts them.
- **The maintenance trigger.** Pruning runs on the restructuring worker and inherits its gate
  (distributor utilization >= 80% and I/O-worker utilization <= 50%), which never opens in a
  disk-busy benchmark -- so no background prune fires there at all. Sensible for rebalancing;
  questionable for pruning, which is about space rather than index depth.
- **Skewed workloads yield no candidates**, by design: the immediate history ancestors of a live
  leaf are young and mostly still authoritative under skew, so the fit test rejects everything. A
  uniform overwrite workload prunes readily (24 prunes in a 1M-request run).

## Addendum — what the reinsertion mode surfaced

Running the suite with `PRUNE_REINS=1` produces, in roughly two runs out of three:

- **Slot corruption**: a read of key X returns a record for key Y, i.e. the local index pointed at
  a slot holding somebody else's record. Mechanism above (`fsst.c` writes at the source's slot
  index). The pruner also detects the same disagreement from the other side and skips the slot.
- **A stale read**, about one in 500k: a key comes back with an older version's value, correct key.
  That is reinsertion copying a record forward while a client writes the same key: the
  copy-forward lands in a higher slot, and `add_in_tree_for_upsert()` keeps the larger `slab_idx`,
  so the older version wins. This is the defect AGENTS.md records as "background reinsertions can
  silently drop real-upserts ... We'll leave this bug intentionally".

Both are pre-existing and independent of pruning's protocol. But they only showed up when prunes
ran *concurrently* with reinsertion -- four runs with reinsertion on and concurrent pruning off
were clean. The plausible reason is exposure, not causation: `N` concentrates the historical
entries of three slabs into one node whose pages the test then marks hot, so reinsertion gets a
much richer supply of old records to copy forward, widening a race that was always there.

The suite therefore keeps the model check strict when reinsertion is off (any disagreement fails)
and, when it is on, reports the counts and only fails if they are far beyond what these races
explain. `stress_corrupt` (wrong key) is separated from `stress_bad` (right key, wrong value),
because the two have different causes.
