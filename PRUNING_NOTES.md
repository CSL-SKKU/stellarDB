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
