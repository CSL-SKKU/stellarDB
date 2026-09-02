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
