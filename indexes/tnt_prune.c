/*
 * Pruning: candidate selection.
 *
 * A prune replaces three nodes -- an internal node, a leaf and an internal
 * node that are consecutive in the in-order sequence *and* adjacent in the
 * history chain -- with one new node holding only their valid entries. This
 * file picks those triples; nothing here mutates anything.
 *
 * The picking rule, in the names AGENTS.md and the plan use:
 *
 *   leaf  (L)     the leaf of the triple, a live appendable slab
 *   inner         L->lu_parent, the younger internal node
 *   outer         inner->lu_parent, the older internal node
 *   up    (D)     outer->lu_parent, NULL when outer is the history root
 *   sib   (A)     inner's other history child
 *   star  (*)     outer's other history child
 *   side  (s)     inner->lu_child[side] == L
 *
 * A triple is prunable iff it is consecutive in-order, historically local
 * (two of the three have their lu_parent inside the triple, i.e. the chain
 * segment L -> inner -> outer), and its valid entries fit in one slab.
 *
 * The test below is local to the history tree, and equivalent to scanning the
 * in-order sequence: with L on side `side` of inner and inner on the opposite
 * side of outer, history in-order reads "... sib-subtree, inner, L, outer,
 * star-subtree ...", i.e. the triple is consecutive. Same-side placement would
 * put sib's whole subtree between inner and L. History in-order equals routing
 * in-order, so consecutive there means consecutive here.
 */
#include "../headers.h"
#include "tnt_centree.h"

#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdbool.h>

/*
 * Pipelined page reads for the merge. One synchronous 4 KiB read per touched
 * page was the pruner's whole cost under load: each read waited its turn
 * behind the workload's queue (~1.5 ms against ~20 us on an idle device).
 * Batches of QUEUE_DEPTH paid that wait once per batch, but still once per
 * batch: a 64 MiB node took ~265 round trips of 2-3 ms. Now up to
 * PRUNE_READ_DEPTH reads stay in flight and every completion is refilled at
 * once, so the read phase is bound by the thread's share of device bandwidth
 * instead of by round trips. The pruner is a single thread, so it owns one
 * aio context of its own.
 */
#define PRUNE_READ_DEPTH 512

static aio_context_t prune_aio;
static struct iocb prune_iocb[PRUNE_READ_DEPTH];
static struct iocb *prune_iocbs[PRUNE_READ_DEPTH];
static struct io_event prune_events[PRUNE_READ_DEPTH];

static int prune_aio_init(void) {
  if (prune_aio != 0)
    return 0;
  if (syscall(__NR_io_setup, PRUNE_READ_DEPTH, &prune_aio) != 0)
    return -errno;
  return 0;
}

/* Reads pages[0..nb) of fd into buf, PAGE_SIZE each, keeping the queue full. */
static int prune_read_pages(int fd, const size_t *pages, size_t nb, char *buf) {
  int error = prune_aio_init();
  int free_idx[PRUNE_READ_DEPTH];
  int nfree = PRUNE_READ_DEPTH;
  size_t next = 0, done = 0, inflight = 0;

  if (error)
    return error;
  for (int i = 0; i < PRUNE_READ_DEPTH; i++)
    free_idx[i] = PRUNE_READ_DEPTH - 1 - i;
  while (done < nb) {
    long tosend = 0;

    while (next < nb && nfree > 0) {
      int i = free_idx[--nfree];
      struct iocb *cb = &prune_iocb[i];

      memset(cb, 0, sizeof(*cb));
      cb->aio_data = (uint64_t)i;
      cb->aio_fildes = (uint32_t)fd;
      cb->aio_lio_opcode = IOCB_CMD_PREAD;
      cb->aio_buf = (uint64_t)(uintptr_t)(buf + next * PAGE_SIZE);
      cb->aio_nbytes = PAGE_SIZE;
      cb->aio_offset = (int64_t)(pages[next] * PAGE_SIZE);
      prune_iocbs[tosend++] = cb;
      next++;
    }
    for (long sent = 0; sent < tosend;) {
      long r = syscall(__NR_io_submit, prune_aio, tosend - sent,
                       prune_iocbs + sent);

      if (r < 0)
        return -errno;
      sent += r;
    }
    inflight += (size_t)tosend;
    if (inflight == 0)
      break;
    {
      /*
       * Wait for a chunk of completions, not one: refilling one iocb at a
       * time costs an io_submit per page (~20 us), which capped the reader
       * near 200 MB/s of CPU-bound submits. A quarter of the depth per refill
       * keeps the queue deep and the submit count low.
       */
      long min_nr = (long)inflight < PRUNE_READ_DEPTH / 4 ? (long)inflight
                                                          : PRUNE_READ_DEPTH / 4;
      long r = syscall(__NR_io_getevents, prune_aio, min_nr, (long)inflight,
                       prune_events, NULL);

      if (r < 0)
        return -errno;
      for (long i = 0; i < r; i++) {
        if (prune_events[i].res != (int64_t)PAGE_SIZE)
          return -EIO;
        free_idx[nfree++] = (int)prune_events[i].data;
      }
      inflight -= (size_t)r;
      done += (size_t)r;
    }
  }
  return 0;
}

static bool retired(centree_node n) {
  return atomic_load_explicit(&n->removed, memory_order_acquire) != 0;
}

/* Defined further down with the build; used by the migration operations above them. */
static size_t pages_for_capacity(struct prune_build *b);
static size_t full_slab_pages(void);
static size_t full_slab_items(void);
static int rebuild_begin(centree_node n, size_t entries, struct prune_build *out);
static void node_slots(centree_node n, size_t *reserved, size_t *valid);

/* Does any history node from `from` upward hold `key` (stale copies count)? */
int prune_key_held_above(centree_node from, uint64_t key) {
  for (centree_node cur = from; cur != NULL; cur = centree_lu_parent(cur)) {
    struct slab *s = cur->value.slab;
    index_entry_t e;
    int found = 0;

    R_LOCK(&s->tree_lock);
    if (atomic_load_explicit(&s->superseded, memory_order_acquire)) {
      R_UNLOCK(&s->tree_lock);
      s = cur->value.slab;
      R_LOCK(&s->tree_lock);
    }
    if (s->min != (uint64_t)-1 && s->subtree != NULL && key >= s->min &&
        key <= s->max)
      found = subtree_find(s->subtree, (unsigned char *)&key, sizeof(key), &e);
    R_UNLOCK(&s->tree_lock);
    if (found)
      return 1;
  }
  return 0;
}

static int child_flag(centree_node n) {
  return atomic_load_explicit(&n->child_flag, memory_order_acquire);
}

/* Why a leaf was not the leaf of a prunable triple; PRUNE_OK otherwise. */
enum prune_reject {
  PRUNE_OK = 0,
  PRUNE_NO_INNER,      /* leaf has no history parent (history root) */
  PRUNE_NO_OUTER,      /* inner has no history parent */
  PRUNE_NOT_HISTORY_CHILD,
  PRUNE_SAME_SIDE,     /* triple not consecutive in-order */
  PRUNE_NO_SIB_STAR,
  PRUNE_INTERNAL_SPLITTING,
  PRUNE_LEAF_SPLITTING,
  PRUNE_RETIRED,
  PRUNE_LEAF_FULL,
  PRUNE_NO_FIT,
  PRUNE_NB_REASONS
};
static enum prune_reject prune_select_why(centree_node leaf,
                                          struct prune_candidate *out) {
  centree_node inner, outer, sib, star;
  struct slab *ls, *is, *os;
  size_t cold_bound;
  int side;

  if (leaf == NULL)
    return PRUNE_NO_INNER;

  inner = centree_lu_parent(leaf);
  if (inner == NULL)
    return PRUNE_NO_INNER;
  outer = centree_lu_parent(inner);
  if (outer == NULL)
    return PRUNE_NO_OUTER;

  if (inner->lu_child[CENTREE_LU_LEFT] == leaf)
    side = CENTREE_LU_LEFT;
  else if (inner->lu_child[CENTREE_LU_RIGHT] == leaf)
    side = CENTREE_LU_RIGHT;
  else
    return PRUNE_NOT_HISTORY_CHILD;

  /* Opposite sides: only then is the triple consecutive in-order. */
  if (outer->lu_child[!side] != inner) {
    return PRUNE_SAME_SIDE;
  }

  sib = inner->lu_child[!side];
  star = outer->lu_child[side];
  if (sib == NULL || star == NULL)
    return PRUNE_NO_SIB_STAR;

  /*
   * Both internals must be fully split -- a half-published split would leave
   * the routing splice with a node whose children are still moving. The leaf
   * must be a live, appendable leaf: child_flag 0 and a slab that is not full
   * rules out a leaf that is mid-split or about to be.
   */
  if (child_flag(inner) != 1 || child_flag(outer) != 1)
    return PRUNE_INTERNAL_SPLITTING;
  if (child_flag(leaf) != 0)
    return PRUNE_LEAF_SPLITTING;
  if (retired(leaf) || retired(inner) || retired(outer))
    return PRUNE_RETIRED;

  ls = leaf->value.slab;
  is = inner->value.slab;
  os = outer->value.slab;
  if (atomic_load_explicit(&ls->full, memory_order_acquire))
    return PRUNE_LEAF_FULL;

  /*
   * nb_items is only ever decremented when an entry is invalidated, so it is
   * an upper bound on the valid entries and this test is conservative. The
   * authoritative fit test is slab_freeze()'s budget, which runs against the
   * count actually copied.
   */
  cold_bound = is->nb_items + os->nb_items;
  if (cold_bound + ls->nb_items > ls->nb_max_items) {
    return PRUNE_NO_FIT;
  }

  out->leaf = leaf;
  out->inner = inner;
  out->outer = outer;
  out->up = centree_lu_parent(outer); /* D, NULL at the history root */
  out->sib = sib;
  out->star = star;
  out->side = side;
  out->cold_bound = cold_bound;
  return PRUNE_OK;
}

bool prune_select(centree_node leaf, struct prune_candidate *out) {
  return prune_select_why(leaf, out) == PRUNE_OK;
}

/*
 * Leaves left to right. For a tree whose in-order sequence alternates
 * leaf/internal this is in-order restricted to the leaves.
 */
static void collect_leaves(centree tree, centree_node n, centree_node *out,
                           size_t capacity, size_t *nb) {
  centree_node left, right;

  if (n == NULL)
    return;
  left = centree_read_left(tree, n);
  right = centree_read_right(tree, n);
  if (left == NULL && right == NULL) {
    if (*nb < capacity)
      out[(*nb)++] = n;
    return;
  }
  collect_leaves(tree, left, out, capacity, nb);
  collect_leaves(tree, right, out, capacity, nb);
}

/*
 * O(n) fallback discovery: the first prunable triple, scanning leaves left to
 * right. Step 9 adds an event-driven trigger. The RCU section covers only the
 * pointer reads; prune_select() runs outside it and takes no lock.
 */
bool prune_scan_for_candidate(struct prune_candidate *out) {
  centree tree = tnt_centree();
  centree_node *leaves;
  size_t capacity, nb = 0;
  bool found = false;

  if (tree == NULL)
    return false;
  capacity =
      atomic_load_explicit(&tree->node_count, memory_order_acquire) + 8;
  leaves = malloc(capacity * sizeof(*leaves));
  if (leaves == NULL)
    return false;

  centree_read_in(tree);
  collect_leaves(tree, centree_read_root(tree), leaves, capacity, &nb);
  centree_read_out(tree);

  /*
   * Cold leaves first: among the prunable triples take the one whose leaf saw
   * the fewest client writes since the scheduler's last mark, then the one
   * that frees the most slots. A hot leaf is not excluded -- when every
   * candidate is hot the busiest region still gets pruned -- it just loses to
   * any colder one.
   */
  {
    struct prune_candidate c;
    uint64_t best_hot = 0;
    size_t best_reclaim = 0;

    for (size_t i = 0; i < nb; i++) {
      struct slab *ls;
      uint64_t hot;
      size_t reclaim = 0, r, v;
      centree_node trip[3];

      if (!prune_select(leaves[i], &c))
        continue;
      ls = c.leaf->value.slab;
      hot = atomic_load_explicit(&ls->nb_writes, memory_order_relaxed) -
            atomic_load_explicit(&ls->nb_writes_mark, memory_order_relaxed);
      trip[0] = c.leaf; trip[1] = c.inner; trip[2] = c.outer;
      for (int k = 0; k < 3; k++) {
        node_slots(trip[k], &r, &v);
        reclaim += r - v;
      }
      if (!found || hot < best_hot ||
          (hot == best_hot && reclaim > best_reclaim)) {
        *out = c;
        best_hot = hot;
        best_reclaim = reclaim;
        found = true;
      }
    }
  }

  free(leaves);
  return found;
}

void prune_mark_writes(void) {
  centree tree = tnt_centree();
  centree_node *leaves;
  size_t capacity, nb = 0;

  if (tree == NULL)
    return;
  capacity = atomic_load_explicit(&tree->node_count, memory_order_acquire) + 8;
  leaves = malloc(capacity * sizeof(*leaves));
  if (leaves == NULL)
    return;
  centree_read_in(tree);
  collect_leaves(tree, centree_read_root(tree), leaves, capacity, &nb);
  centree_read_out(tree);
  for (size_t i = 0; i < nb; i++) {
    struct slab *s = leaves[i]->value.slab;

    atomic_store_explicit(&s->nb_writes_mark,
                          atomic_load_explicit(&s->nb_writes, memory_order_relaxed),
                          memory_order_relaxed);
  }
  free(leaves);
}

/* Shared traversal for maintenance candidate selection. */
static void collect_nodes(centree tree, centree_node n, centree_node *out,
                          size_t capacity, size_t *nb) {
  if (n == NULL)
    return;
  if (*nb < capacity)
    out[(*nb)++] = n;
  collect_nodes(tree, centree_read_left(tree, n), out, capacity, nb);
  collect_nodes(tree, centree_read_right(tree, n), out, capacity, nb);
}

/* ===================================================================== *
 * Migration rebuilds slabs without changing the tree topology
 * ===================================================================== */

/* Header of `s` for node `n`, with `replace` child id swapped for `with`. */
static int write_header_children(struct slab *s, centree_node n,
                                 uint64_t replace, uint64_t with) {
  uint64_t child[2] = {0, 0};

  for (int i = 0; i < 2; i++)
    if (n->lu_child[i] != NULL) {
      child[i] = n->lu_child[i]->value.slab->seq;
      if (replace && child[i] == replace)
        child[i] = with;
    }
  return slab_write_header_raw(s, centree_pivot_load(n),
                               atomic_load_explicit(&n->value.level, memory_order_relaxed),
                               child[0], child[1]);
}

/*
 * Commit point of a rebuild: the node's history parent (or ROOT) names the
 * fresh slab instead of the old one. Before it, the fresh file is unreachable
 * garbage; after it, the old file is. Recovery deletes whichever is
 * unreachable (both carry valid headers, different ids).
 */
static int rebuild_commit(centree_node n, struct slab *old, struct slab *fresh) {
  centree_node up = centree_lu_parent(n);

  if (up == NULL)
    return slab_root_write(fresh->seq) == 0 ? 0 : -EIO;
  return write_header_children(up->value.slab, up, old->seq, fresh->seq);
}

/* Publish the fresh slab on the node and retire the old one. */
static void rebuild_swap(centree_node n, struct slab *old, struct slab *fresh) {
  fresh->centree_node = n;
  n->value.seq = fresh->seq;
  __atomic_store_n(&n->value.slab, fresh, __ATOMIC_RELEASE);
  atomic_store_explicit(&old->superseded, 1, memory_order_seq_cst);
  /* Waits for readers inside the old slab's lock, then frees its index. */
  slab_retire(old);

}

static size_t slab_valid(struct slab *s) { return s->nb_items; }

static int node_is_internal(centree_node n) {
  return n != NULL && child_flag(n) == 1 && !retired(n);
}

/* Rebuild n's slab from `srcs` (newest first); n internal, lock held. */
static int rebuild_node(centree_node n, centree_node *srcs, int nsrc,
                        uint64_t child_replace, uint64_t child_with,
                        struct slab **fresh_out) {
  struct prune_build b;
  size_t entries = 0;
  int error;

  for (int i = 0; i < nsrc; i++) {
    /* Known bugs 22: in-flight writes into an internal slab publish late. */
    slab_drain_updates(srcs[i]->value.slab);
    entries += slab_valid(srcs[i]->value.slab);
  }
  /* The merged node must fit one regular slab; otherwise this move is not made. */
  if (entries > full_slab_items())
    return -ENOSPC;
  error = rebuild_begin(n, entries, &b);
  if (error)
    return error;
  b.drop_tombstones = 1;
  b.tomb_check_from = centree_lu_parent(n);
  for (int i = 0; i < nsrc && !error; i++)
    error = prune_build_add_source(&b, srcs[i], 0);
  if (error == -ENOSPC && entries < full_slab_items()) {
    /* nb_items is an upper bound, but concurrent publishes can add: retry full size. */
    prune_build_discard(&b);
    error = rebuild_begin(n, full_slab_items(), &b);
    if (!error) {
      b.drop_tombstones = 1;
      b.tomb_check_from = centree_lu_parent(n);
      for (int i = 0; i < nsrc && !error; i++)
        error = prune_build_add_source(&b, srcs[i], 0);
    }
  }
  if (!error)
    error = prune_build_finish(&b);
  if (error) {
    prune_build_discard(&b);
    return error;
  }
  if (write_header_children(b.slab, n, child_replace, child_with) != 0) {
    prune_build_discard(&b);
    return -EIO;
  }

  *fresh_out = b.slab;
  prune_build_release_buffer(&b);
  return 0;
}

/*
 * Migration: the child's valid entries move into its history parent (always
 * safe: nothing lies between adjacent nodes; the child's copy is younger and
 * wins), the child becomes an empty one-page slab. On disk, the fresh parent
 * names the fresh (empty) child, and the grandparent (or ROOT) names the fresh
 * parent: one commit. In memory the parent is swapped first, so a reader
 * between the two swaps sees the key in both, identical.
 */
int tnt_migrate_up(centree_node child) {
  centree_node parent;
  struct slab *old_c, *old_p, *fresh_c = NULL, *fresh_p = NULL;
  centree_node srcs[2];
  int error;

  if (tnt_centree() == NULL)
    return -EINVAL;
  tnt_maintenance_lock();
  parent = centree_lu_parent(child);
  if (!node_is_internal(child) || !node_is_internal(parent)) {
    tnt_maintenance_unlock();
    return TNT_MIGRATE_NOOP;
  }
  old_c = child->value.slab;
  old_p = parent->value.slab;
  /* Empty child first (nothing to copy): its id goes into the parent's header. */
  error = rebuild_node(child, NULL, 0, 0, 0, &fresh_c);
  if (error) {
    tnt_maintenance_unlock();
    return error;
  }
  srcs[0] = child;   /* younger: wins on a shared key */
  srcs[1] = parent;
  error = rebuild_node(parent, srcs, 2, old_c->seq, fresh_c->seq, &fresh_p);
  if (!error)
    error = rebuild_commit(parent, old_p, fresh_p);
  if (error) {
    /* fresh_c is unreachable garbage on disk; drop it here. */
    struct prune_build tmp = {0};

    tmp.slab = fresh_c;
    tmp.node_owned = 0;
    prune_build_discard(&tmp);
    tnt_maintenance_unlock();
    return error;
  }
  rebuild_swap(parent, old_p, fresh_p);
  rebuild_swap(child, old_c, fresh_c);
  tnt_maintenance_unlock();
  report_event(REPORT_MIGRATION);
  printf("Migrate: %lu -> %lu (parent %lu -> %lu, %zu valid)\n", old_c->seq,
         fresh_c->seq, old_p->seq, fresh_p->seq, fresh_p->nb_items);
  return TNT_MIGRATE_DONE;
}

/*
 * Pick the migration with the greatest estimated reclaim:
 * reserved(child) + stale(parent). The child's valid entries must be nonzero
 * and fit with the parent's within cfg.migrate_th * slab capacity. Require
 * at least one page of slot gain. The scan takes no lock; migration rechecks
 * the nodes and their capacity under the maintenance lock.
 */
int tnt_migrate_once(void) {
  centree tree = tnt_centree();
  centree_node *nodes, best = NULL;
  size_t capacity, nb = 0, best_gain = 0, min_gain = PAGE_SIZE / cfg.kv_size;
  int status;

  if (tree == NULL)
    return -EINVAL;

  if (cfg.migrate_th <= 0) {

    return TNT_MIGRATE_NOOP;
  }
  capacity = atomic_load_explicit(&tree->node_count, memory_order_acquire) + 8;
  nodes = malloc(capacity * sizeof(*nodes));
  if (nodes == NULL)
    return -ENOMEM;
  centree_read_in(tree);
  collect_nodes(tree, centree_read_root(tree), nodes, capacity, &nb);
  centree_read_out(tree);

  for (size_t i = 0; i < nb; i++) {
    centree_node n = nodes[i], p;
    size_t r, v;

    if (!node_is_internal(n) ||
        atomic_load_explicit(&n->value.slab->superseded, memory_order_acquire))
      continue;
    node_slots(n, &r, &v);
    p = centree_lu_parent(n);
    if (v > 0 && node_is_internal(p) &&
        !atomic_load_explicit(&p->value.slab->superseded, memory_order_acquire)) {
      size_t pr, pv;

      node_slots(p, &pr, &pv);
      if ((double)(pv + v) <= cfg.migrate_th * (double)full_slab_items()) {
        size_t gain = r + (pr - pv);

        if (gain >= min_gain && gain > best_gain) {
          best = n;
          best_gain = gain;
        }
      }
    }
  }
  free(nodes);
  if (best == NULL) {

    return TNT_MIGRATE_NOOP;
  }
  status = tnt_migrate_up(best);
  return status;
}

size_t prune_count_candidates(void) {
  centree tree = tnt_centree();
  struct prune_candidate candidate;
  centree_node *leaves;
  size_t capacity, nb = 0, count = 0;

  if (tree == NULL)
    return 0;
  capacity =
      atomic_load_explicit(&tree->node_count, memory_order_acquire) + 8;
  leaves = malloc(capacity * sizeof(*leaves));
  if (leaves == NULL)
    return 0;

  centree_read_in(tree);
  collect_leaves(tree, centree_read_root(tree), leaves, capacity, &nb);
  centree_read_out(tree);

  for (size_t i = 0; i < nb; i++)
    if (prune_select(leaves[i], &candidate))
      count++;

  free(leaves);
  return count;
}

/* ===================================================================== *
 * The stale-slot estimate (see in-memory-index-tnt.h)
 * ===================================================================== */

/* Slots handed out and slots still valid in n's slab (nb_items is an upper bound). */
static void node_slots(centree_node n, size_t *reserved, size_t *valid) {
  struct slab *s = n->value.slab;
  size_t r = atomic_load_explicit(&s->last_item, memory_order_acquire), v;

  if (r > s->nb_max_items)
    r = s->nb_max_items;
  v = s->nb_items;
  if (v > r)
    v = r;
  *reserved = r;
  *valid = v;
}

static void sum_stale(centree tree, centree_node n, struct prune_stale *acc) {
  size_t reserved, valid;

  if (n == NULL)
    return;
  node_slots(n, &reserved, &valid);
  acc->nodes++;
  acc->reserved += reserved;
  acc->valid += valid;
  sum_stale(tree, centree_read_left(tree, n), acc);
  sum_stale(tree, centree_read_right(tree, n), acc);
}

void prune_stale_measure(struct prune_stale *out) {
  centree tree = tnt_centree();

  memset(out, 0, sizeof(*out));
  if (tree == NULL)
    return;
  centree_read_in(tree);
  sum_stale(tree, centree_read_root(tree), out);
  centree_read_out(tree);
  out->stale = out->reserved - out->valid;
}

double prune_stale_ratio(const struct prune_stale *m) {
  return m->reserved ? (double)m->stale / (double)m->reserved : 0.0;
}

/* ===================================================================== *
 * Building the replacement node N
 *
 * N is assembled entirely off to the side: a fresh slab file, a fresh
 * subtree and an unpublished center-tree node. Nothing points at it until
 * the history link and the routing splice, so a build that fails or is
 * rejected is simply discarded (I-1).
 *
 * Sources are merged newest first -- leaf, then inner, then outer -- and a
 * key already present in N is skipped. That is the same result as merging
 * oldest first and letting later sources override (precedence
 * leaf > inner > outer, §2.4), without ever writing a slot twice.
 *
 * Records are copied byte-exact, whole slot: a tombstone occupies only part
 * of its slot, and copying the slot keeps it intact.
 * ===================================================================== */

/* Slots whose record did not match the index; see prune_build_add_source(). */
static uint64_t prune_bad_slots;

struct prune_src_entry {
  uint64_t key;
  uint32_t slot;
  uint32_t shy; /* the source entry was written by reinsertion */
};

struct prune_snapshot {
  struct prune_src_entry *entries;
  size_t nb;
  size_t capacity;
  int overflow;
};

static void snapshot_cb(uint64_t key, uint32_t slot, void *data) {
  struct prune_snapshot *snap = data;

  /* bit 31 is the invalid hint: the entry is stale, drop it. */
  if (slot & (1u << 31))
    return;
  if (snap->nb == snap->capacity) {
    snap->overflow = 1;
    return;
  }
  snap->entries[snap->nb].key = key;
  snap->entries[snap->nb].slot = (uint32_t)GET_SIDX(slot);
  snap->entries[snap->nb].shy = sidx_is_shy(slot);
  snap->nb++;
}

static int compare_by_slot(const void *a, const void *b) {
  const struct prune_src_entry *x = a, *y = b;

  if (x->slot < y->slot) return -1;
  if (x->slot > y->slot) return 1;
  return 0;
}

static char *slot_in_buffer(struct prune_build *b, size_t slot) {
  struct slab *n = b->slab;
  size_t items_per_page = PAGE_SIZE / n->item_size;

  return b->buffer + (slot / items_per_page) * PAGE_SIZE +
         (slot % items_per_page) * n->item_size;
}

static size_t pages_for(struct slab *n, size_t slots) {
  size_t items_per_page = PAGE_SIZE / n->item_size;

  return (slots + items_per_page - 1) / items_per_page;
}

/* The buffer's own size; kept as a function so discard can run any time. */
static size_t pages_for_capacity(struct prune_build *b) {
  size_t pages;

  if (b->slab == NULL || b->capacity == 0)
    return 1;
  pages = pages_for(b->slab, b->capacity);
  return pages ? pages : 1;
}

int prune_build_begin(const struct prune_candidate *c, centree_node pivot_from,
                      struct prune_build *out) {
  uint64_t level = atomic_load_explicit(&pivot_from->value.level,
                                        memory_order_acquire);
  uint64_t pivot = centree_pivot_load(pivot_from);
  tree_entry_t value = {0};
  size_t pages;

  memset(out, 0, sizeof(*out));
  out->dirty_lo = (size_t)-1;

  /* N routes exactly as pivot_from did. */
  out->slab = create_slab(NULL, level, pivot, 0, NULL);
  if (out->slab == NULL || out->slab->fd < 0) {
    free(out->slab);
    memset(out, 0, sizeof(*out));
    return -EIO;
  }
  out->slab->subtree = tnt_subtree_create();
  subtree_set_slab(out->slab->subtree, out->slab);

  /*
   * A whole slab's worth of slots. The freeze budget is what is left of this
   * after the cold part, so anything less would reject a leaf whose *valid*
   * entries fit but which has reserved more slots than that -- and slots are
   * what slab_freeze() can count.
   *
   * mmap, not malloc: the mapping is page-aligned for O_DIRECT and reads as
   * zeroes (item_is_empty) without touching a page that never gets a record.
   */
  out->capacity = out->slab->nb_max_items;
  pages = pages_for(out->slab, out->capacity);
  if (pages == 0)
    pages = 1;
  out->buffer = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (out->buffer == MAP_FAILED) {
    out->buffer = NULL;
    prune_build_discard(out);
    return -ENOMEM;
  }

  out->pivot_from = pivot_from;
  value.key = pivot;
  value.seq = out->slab->seq;
  value.slab = out->slab;
  out->node = centree_node_new((void *)(uintptr_t)value.key, &value);
  atomic_store_explicit(&out->node->value.level, level, memory_order_release);
  out->slab->centree_node = out->node;
  out->node_owned = 1;
  return 0;
}

/*
 * Begin a rebuild of an existing node's slab: a fresh slab (new id) sized for
 * `entries` slots (0 -> one data page), a fresh subtree, no new centree node.
 * The node keeps its routing position, pivot, level and history links; only
 * value.slab changes, at swap time.
 */
static size_t full_slab_pages(void) { return cfg.max_file_size / PAGE_SIZE; }
static size_t full_slab_items(void) {
  return full_slab_pages() * (PAGE_SIZE / cfg.kv_size);
}

static int rebuild_begin(centree_node n, size_t entries, struct prune_build *out) {
  size_t items_per_page = PAGE_SIZE / cfg.kv_size;
  size_t data_pages = entries ? (entries + items_per_page - 1) / items_per_page : 1;
  size_t pages;

  memset(out, 0, sizeof(*out));
  out->dirty_lo = (size_t)-1;
  /*
   * Never larger than a regular slab: every buffer sized from
   * cfg.max_file_size (the reinsertion read buffer, the recovery key arrays)
   * assumes that bound. A rebuild that needs more does not happen.
   */
  if (data_pages > full_slab_pages())
    return -ENOSPC;
  out->slab = create_slab_sized(NULL, atomic_load_explicit(&n->value.level, memory_order_acquire),
                                centree_pivot_load(n), 0, NULL, data_pages);
  if (out->slab == NULL || out->slab->fd < 0) {
    free(out->slab);
    memset(out, 0, sizeof(*out));
    return -EIO;
  }
  out->slab->subtree = tnt_subtree_create();
  subtree_set_slab(out->slab->subtree, out->slab);
  out->capacity = out->slab->nb_max_items;
  pages = pages_for(out->slab, out->capacity);
  if (pages == 0)
    pages = 1;
  out->buffer = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (out->buffer == MAP_FAILED) {
    out->buffer = NULL;
    prune_build_discard(out);
    return -ENOMEM;
  }
  out->pivot_from = n;
  out->node = n;               /* existing, published node: not ours to free */
  out->node_owned = 0;
  out->slab->centree_node = n; /* so slab_write_header() sees n's children */
  return 0;
}

static void mark_dirty(struct prune_build *b, size_t slot) {
  size_t items_per_page = PAGE_SIZE / b->slab->item_size;
  size_t page = slot / items_per_page;

  if (page < b->dirty_lo)
    b->dirty_lo = page;
  if (page + 1 > b->dirty_hi)
    b->dirty_hi = page + 1;
}

/* ---- dropped-tombstone key set (root triple only) ---- */

static int dead_set_add(struct prune_build *b, uint64_t key) {
  if (key == 0) {
    b->dead_has_zero = 1;
    return 0;
  }
  if (b->dead_nb * 2 >= b->dead_cap) {
    size_t cap = b->dead_cap ? b->dead_cap * 2 : 1024;
    uint64_t *bigger = calloc(cap, sizeof(*bigger));

    if (bigger == NULL)
      return -ENOMEM;
    for (size_t i = 0; i < b->dead_cap; i++) {
      uint64_t k = b->dead[i];

      if (k == 0)
        continue;
      for (size_t h = (k * 0x9E3779B97F4A7C15ULL) & (cap - 1);; h = (h + 1) & (cap - 1))
        if (bigger[h] == 0) {
          bigger[h] = k;
          break;
        }
    }
    free(b->dead);
    b->dead = bigger;
    b->dead_cap = cap;
  }
  for (size_t h = (key * 0x9E3779B97F4A7C15ULL) & (b->dead_cap - 1);;
       h = (h + 1) & (b->dead_cap - 1)) {
    if (b->dead[h] == key)
      return 0;
    if (b->dead[h] == 0) {
      b->dead[h] = key;
      b->dead_nb++;
      return 0;
    }
  }
}

static int dead_set_has(const struct prune_build *b, uint64_t key) {
  if (key == 0)
    return b->dead_has_zero;
  if (b->dead_cap == 0)
    return 0;
  for (size_t h = (key * 0x9E3779B97F4A7C15ULL) & (b->dead_cap - 1);;
       h = (h + 1) & (b->dead_cap - 1)) {
    if (b->dead[h] == key)
      return 1;
    if (b->dead[h] == 0)
      return 0;
  }
}

/*
 * Un-stage the record at slot `dst` (a superseded copy a root-triple tombstone
 * would have shadowed). The last staged record moves into the hole so N stays
 * dense: no empty slot, no stale slot, nothing for the stale estimate to
 * count.
 */
static void build_unstage(struct prune_build *b, uint64_t key, size_t dst) {
  struct slab *n = b->slab;
  size_t last = b->count - 1;

  subtree_delete(n->subtree, (unsigned char *)&key, sizeof(key));
  if (dst != last) {
    char *from = slot_in_buffer(b, last), *to = slot_in_buffer(b, dst);
    struct item_metadata *m = (struct item_metadata *)from;
    uint64_t mkey = *(uint64_t *)(from + sizeof(*m));
    index_entry_t e;
    int shy = 0;

    if (subtree_find(n->subtree, (unsigned char *)&mkey, sizeof(mkey), &e))
      shy = sidx_is_shy(e.slab_idx);
    subtree_delete(n->subtree, (unsigned char *)&mkey, sizeof(mkey));
    memcpy(to, from, n->item_size);
    e.slab = n;
    e.slab_idx = dst;
    if (shy)
      subtree_insert_shy(n->subtree, (unsigned char *)&mkey, sizeof(mkey), &e);
    else
      subtree_insert(n->subtree, (unsigned char *)&mkey, sizeof(mkey), &e);
    subtree_report_record(n->subtree, mkey, dst, item_is_tombstone((struct item_metadata *)to));
    mark_dirty(b, dst);
  }
  memset(slot_in_buffer(b, last), 0, n->item_size);
  mark_dirty(b, last);
  b->count--;
}

int prune_build_add_source(struct prune_build *b, centree_node source,
                           int override) {
  struct slab *src = source->value.slab;
  struct slab *n = b->slab;
  struct prune_snapshot snap = {0};
  size_t items_per_page = PAGE_SIZE / src->item_size;
  size_t *touched = NULL, nb_touched = 0, cur = 0;
  char *pages = NULL;
  int error = 0;

  snap.capacity = src->nb_max_items;
  snap.entries = malloc(snap.capacity * sizeof(*snap.entries));
  if (snap.entries == NULL) {
    error = -ENOMEM;
    goto out;
  }

  /*
   * Snapshot under the source's lock (I-5), copy without it: an internal
   * slab is immutable except for the invalid hint, and an entry is published
   * in the subtree only once its page write has completed, so what the
   * snapshot names is already on the device. An entry invalidated after the
   * snapshot is copied anyway; it lands in N as a stale entry, shadowed by
   * the newer copy nearer the leaf (§7).
   */
  R_LOCK(&src->tree_lock);
  if (retired(source) ||
      atomic_load_explicit(&src->superseded, memory_order_acquire)) {
    R_UNLOCK(&src->tree_lock);
    error = -ECANCELED;
    goto out;
  }
  subtree_forall_entries(src->subtree, snapshot_cb, &snap);
  R_UNLOCK(&src->tree_lock);
  if (snap.overflow) {
    error = -E2BIG;
    goto out;
  }

  /* Slot order, so each source page is read at most once. */
  qsort(snap.entries, snap.nb, sizeof(*snap.entries), compare_by_slot);
  if (snap.nb == 0)
    goto out;

  /* Distinct touched pages, ascending (entries are in slot order). */
  touched = malloc(snap.nb * sizeof(*touched));
  if (touched == NULL) {
    error = -ENOMEM;
    goto out;
  }
  for (size_t i = 0; i < snap.nb; i++) {
    size_t pg = snap.entries[i].slot / items_per_page;

    if (snap.entries[i].slot >= src->nb_max_items) {
      error = -EINVAL;
      goto out;
    }
    if (nb_touched == 0 || touched[nb_touched - 1] != pg)
      touched[nb_touched++] = pg;
  }
  pages = aligned_alloc(PAGE_SIZE, nb_touched * PAGE_SIZE);
  if (pages == NULL) {
    error = -ENOMEM;
    goto out;
  }
  error = prune_read_pages(src->fd, touched, nb_touched, pages);
  if (error)
    goto out;

  for (size_t i = 0; i < snap.nb; i++) {
    uint64_t key = snap.entries[i].key;
    size_t slot = snap.entries[i].slot;
    size_t page_idx = slot / items_per_page;
    index_entry_t existing;
    index_entry_t entry;
    struct item_metadata *meta;
    char *record;
    size_t dst;
    int replaces;

    /*
     * Precedence leaf > inner > outer. Sources arrive newest first, so a key
     * already staged wins -- unless this source outranks what is there, which
     * is the leaf joining after the two internal slabs.
     */
    replaces = subtree_find(n->subtree, (unsigned char *)&key, sizeof(key),
                            &existing);
    if (replaces && !override)
      continue;
    /* A younger source's tombstone already killed this key (cold pass). */
    if (!override && b->drop_tombstones && dead_set_has(b, key))
      continue;
    if (slot >= src->nb_max_items) {
      error = -EINVAL;
      goto out;
    }
    if (!replaces && b->count == b->capacity) {
      error = -ENOSPC;
      goto out;
    }
    dst = replaces ? GET_SIDX(existing.slab_idx) : b->count;

    /* touched[] is ascending and so are the entries: advance cur to page_idx. */
    while (touched[cur] != page_idx)
      cur++;
    record = pages + cur * PAGE_SIZE + (slot % items_per_page) * src->item_size;
    meta = (struct item_metadata *)record;
    if (item_is_legacy(meta) || item_is_empty(meta) ||
        *(uint64_t *)(record + sizeof(*meta)) != key) {
      /*
       * The local index and the file disagree about this slot, so there is
       * nothing here worth carrying over. It is not the pruner's doing:
       * reinsertion can write a record into a live slab at the *source's*
       * slot index (fsst.c leaves cb->fsst_idx pointing at the source, which
       * suppresses the correction in slabworker.c's UPSERT case), which
       * overwrites whatever record that slot held. Skipping keeps the
       * database running; the key was already lost when the slot was
       * overwritten.
       */
      if (__sync_fetch_and_add(&prune_bad_slots, 1) < 8)
        fprintf(stderr,
                "Pruning: slab %lu slot %lu does not hold indexed key %lu; "
                "skipping it\n",
                src->seq, slot, key);
      continue;
    }

    if (b->drop_tombstones && item_is_tombstone(meta) &&
        (b->tomb_check_from == NULL || !prune_key_held_above(b->tomb_check_from, key))) {
      /*
       * Root triple: nothing older than the triple exists for this key, so
       * the tombstone only has to defeat copies *inside* the triple. Those
       * are either already staged (un-stage them) or still to come from an
       * older source (block them through the dead set). The tombstone itself
       * is not written.
       */
      if (replaces)
        build_unstage(b, key, dst);
      else if (!override) {
        error = dead_set_add(b, key);
        if (error)
          goto out;
      }
      continue;
    }

    memcpy(slot_in_buffer(b, dst), record, n->item_size);
    mark_dirty(b, dst);
    if (!replaces) {
      entry.slab = n;
      entry.slab_idx = dst;
      /* The shy bit travels with the entry; the record carries it too. */
      if (snap.entries[i].shy)
        subtree_insert_shy(n->subtree, (unsigned char *)&key, sizeof(key),
                           &entry);
      else
        subtree_insert(n->subtree, (unsigned char *)&key, sizeof(key), &entry);
      slab_widen_range(n, key);
      b->count++;
    }
    subtree_report_record(n->subtree, key, dst, item_is_tombstone((struct item_metadata *)record));
  }

out:
  free(snap.entries);
  free(touched);
  free(pages);
  return error;
}

int prune_build_flush(struct prune_build *b) {
  size_t offset, length;
  ssize_t written;

  if (b->dirty_lo >= b->dirty_hi)
    return 0;
  offset = b->dirty_lo * PAGE_SIZE;
  length = (b->dirty_hi - b->dirty_lo) * PAGE_SIZE;
  written = pwrite(b->slab->fd, b->buffer + offset, length, (off_t)offset);
  if (written != (ssize_t)length)
    return -EIO;
  b->dirty_lo = (size_t)-1;
  b->dirty_hi = 0;
  return 0;
}

int prune_build_finish(struct prune_build *b) {
  struct slab *n = b->slab;
  int error = prune_build_flush(b);

  if (error)
    return error;
  if (fsync(n->fd) != 0)
    return -EIO;

  /*
   * full = 1 on an internal node is deliberate (§7): every writer descent
   * tries reserve_slot() on the nodes it passes, and N is immutable.
   */
  n->nb_items = b->count;
  atomic_store_explicit(&n->last_item, b->count, memory_order_release);
  atomic_store_explicit(&n->full, 1, memory_order_release);
  atomic_store_explicit(&b->node->child_flag, 1, memory_order_release);
  return 0;
}

int prune_build_cold(const struct prune_candidate *c,
                     struct prune_build *out) {
  int error = prune_build_begin(c, c->outer, out);

  if (error)
    return error;
  /* Newest first: inner overrides outer. The leaf joins after its freeze. */
  error = prune_build_add_source(out, c->inner, 0);
  if (!error)
    error = prune_build_add_source(out, c->outer, 0);
  if (!error)
    error = prune_build_finish(out);
  return error;
}

/*
 * N is on disk and committed: the page image is no longer needed. Only the
 * failure path released it before, so every successful prune kept a 64 MiB
 * mapping (measured: +64 MiB VmSize per prune, ~60 MiB of it resident).
 */
void prune_build_release_buffer(struct prune_build *b) {
  if (b->buffer != NULL)
    munmap(b->buffer, pages_for_capacity(b) * PAGE_SIZE);
  b->buffer = NULL;
  free(b->dead);
  b->dead = NULL;
  b->dead_cap = b->dead_nb = 0;
}

void prune_build_discard(struct prune_build *b) {
  /* Sized from the slab, so taken before the slab is freed. */
  size_t buffer_len = b->buffer != NULL ? pages_for_capacity(b) * PAGE_SIZE : 0;

  if (b->slab != NULL) {
    char proc[64], path[PATH_MAX];
    int len;

    /* The file was never referenced by anything else. */
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", b->slab->fd);
    len = readlink(proc, path, sizeof(path) - 1);
    if (len > 0) {
      path[len] = 0;
      unlink(path);
    }
    close(b->slab->fd);
    if (b->slab->subtree != NULL)
      subtree_free(b->slab->subtree);
    if (cfg.with_reins)
      free(b->slab->hot_bits);
    free(b->slab->batched_callbacks);
    free(b->slab);
  }
  /* Unpublished, so unlike a live node this one may be freed (I-4). */
  if (b->node_owned)
    free(b->node);
  if (b->buffer != NULL)
    munmap(b->buffer, buffer_len);
  free(b->dead);
  memset(b, 0, sizeof(*b));
}

/* ===================================================================== *
 * Freeze, hot copy, history link
 *
 * From the freeze on, writers whose key routes to the leaf are parked: the
 * leaf can no longer take a slot and will never grow children. Only the
 * routing splice releases them, which is why there is no abort path past a
 * successful freeze (I-9) and why the hot copy is kept as small as possible
 * -- the two internal slabs are already staged and written by then.
 *
 * The history link runs before the routing splice (I-2). Until the splice,
 * readers may follow either `sib -> inner -> outer -> D` or `sib -> N -> D`;
 * both are complete, because N holds every valid entry of the three and
 * nothing has been retired yet.
 * ===================================================================== */

static void prune_link_history(const struct prune_candidate *c,
                               centree_node n) {
  /*
   * N first: it is fully built, and nothing points at it yet (I-1).
   *
   * In-order, the triple reads "sib-subtree, inner, leaf, outer,
   * star-subtree" (or its mirror), so N inherits sib on the side the leaf sat
   * on under inner and star on the other -- which is exactly
   * lu_child[side] = star, lu_child[!side] = sib in both orientations.
   */
  centree_lu_parent_store(n, c->up);
  n->lu_child[c->side] = c->star;
  n->lu_child[!c->side] = c->sib;

  if (c->up != NULL) {
    int t = c->up->lu_child[CENTREE_LU_LEFT] == c->outer ? CENTREE_LU_LEFT
                                                         : CENTREE_LU_RIGHT;

    if (c->up->lu_child[t] != c->outer)
      die("Pruning: outer is not a history child of its own lu_parent\n");
    c->up->lu_child[t] = n;
  }

  /* The two external rewires: the whole history cost of a prune (I-12). */
  centree_lu_parent_store(c->sib, n);
  centree_lu_parent_store(c->star, n);
}

int prune_freeze_and_link(const struct prune_candidate *c,
                          struct prune_build *b) {
  struct slab *leaf_slab = c->leaf->value.slab;
  centree_node p, q;
  long frozen;
  int error;

  memset(b, 0, sizeof(*b));

  /*
   * P is the leaf's routing parent, always one of the two internal nodes: the
   * triple is consecutive in-order, and a leaf's routing parent is one of its
   * in-order neighbours. N takes the routing position of the other one, Q, so
   * it inherits Q's pivot and level. Both are fixed for the rest of the
   * prune: internals are only rewired by rebalancing (excluded through
   * maintenance_lock) or by another prune (there is one pruner), and the leaf
   * cannot split once frozen.
   */
  p = tnt_routing_parent(c->leaf);
  if (p != c->inner && p != c->outer)
    return -EAGAIN;
  q = (p == c->inner) ? c->outer : c->inner;

  error = prune_build_begin(c, q, b);
  if (error)
    return error;
  b->drop_tombstones = (c->up == NULL);
  /*
   * The two internal slabs are only immutable once every write that reserved
   * a slot in them has completed and published: a split happens when the last
   * slot is reserved, not when its write lands. Wait for that before taking
   * their snapshot, exactly as the freeze waits for the leaf.
   */
  slab_drain_updates(c->inner->value.slab);
  slab_drain_updates(c->outer->value.slab);
  error = prune_build_add_source(b, c->inner, 0);
  if (!error)
    error = prune_build_add_source(b, c->outer, 0);
  /* Get the cold pages onto the device before the freeze window opens. */
  if (!error)
    error = prune_build_flush(b);
  if (error) {
    prune_build_discard(b);
    return error;
  }

  frozen = slab_freeze(leaf_slab, b->capacity - b->count);
  if (frozen < 0) {
    prune_build_discard(b);
    return (int)frozen;
  }

  /*
   * The leaf's subtree is stable now: a writer publishes its entry before it
   * drops update_ref, and the freeze drained update_ref. Entries can still be
   * *invalidated* from here on -- by a writer that restarts after the splice
   * and supersedes one of them -- and such an entry is copied into N anyway,
   * shadowed by the newer copy nearer the leaf.
   */
  error = prune_build_add_source(b, c->leaf, 1);
  if (!error)
    error = prune_build_finish(b);
  if (error)
    die("Pruning could not finish the merged slab after freezing slab %lu "
        "(%d); the frozen leaf has no way back\n",
        leaf_slab->seq, error);

  prune_link_history(c, b->node);

  /*
   * Durable commit. N's header names its history children and carries Q's
   * pivot; until D's header (or ROOT) names N, N is unreachable on disk and a
   * crash just leaves a file for recovery to delete. D's header write is the
   * point after which the three old files are logically gone. Both are single
   * aligned page writes on O_DIRECT descriptors, like the data pages.
   */
  if (slab_write_header(b->slab) != 0)
    die("Pruning cannot write the header of slab %lu\n", b->slab->seq);
  slab_maybe_crash(CRASH_PRUNE_AFTER_N_HEADER);
  if (c->up != NULL) {
    if (slab_write_header(c->up->value.slab) != 0)
      die("Pruning cannot commit slab %lu into slab %lu\n", b->slab->seq,
          c->up->value.slab->seq);
  } else if (slab_root_write(b->slab->seq) != 0) {
    die("Pruning cannot make slab %lu the history root\n", b->slab->seq);
  }
  slab_maybe_crash(CRASH_PRUNE_AFTER_COMMIT);
  return 0;
}

/* ===================================================================== *
 * The routing splice
 *
 * P (the leaf's routing parent) and the leaf leave the tree together: P is
 * replaced by its other child S. Then N takes Q's place, with Q's children
 * and Q's pivot, so it routes exactly as Q did.
 *
 * The pivot that disappears is P's, so the leaf's key interval is absorbed by
 * its in-order neighbour: by the rightmost leaf under N's left subtree if P
 * was the triple's first node, by the leftmost leaf under its right subtree
 * if P was the last. Either way that leaf's lu_parent chain runs through N,
 * which now holds the leaf's entries -- which is why the history link has to
 * be installed first (I-2).
 *
 * Everything here is bounded pointer work: ten rcu_writer_set_ptr() calls at
 * most, no scans, and no lock held while entering the writer (I-11, I-12).
 * The triple's own routing pointers are left alone: old-generation readers
 * may still be walking them, and finish_deferred() waits for them.
 * ===================================================================== */

void prune_splice_routing(const struct prune_candidate *c,
                          struct prune_build *b) {
  centree tree = tnt_centree();
  centree_node n = b->node;
  centree_node p, q, s, g, ql, qr, gq;
  struct rcu_writer writer;

  writer = rcu_writer_in(&tree->topology_rcu);

  /*
   * Re-reading P here is an assertion, not a decision: it was fixed before
   * the freeze. Concurrent splits cannot move it (a split only adds children
   * under a leaf, and the frozen leaf can no longer split) and rebalancing is
   * excluded. If it moved anyway, N is already in the history chain and the
   * leaf is frozen, so there is nothing safe to roll back to.
   */
  p = centree_writer_parent(&writer, c->leaf);
  q = (p == c->inner) ? c->outer : c->inner;
  if ((p != c->inner && p != c->outer) || q != b->pivot_from ||
      centree_pivot_load(n) != centree_pivot_load(q)) {
    rcu_writer_abort(&tree->topology_rcu, &writer);
    die("Pruning: the routing shape changed under the maintenance lock\n");
  }

  s = centree_writer_left(&writer, p) == c->leaf
          ? centree_writer_right(&writer, p)
          : centree_writer_left(&writer, p);
  g = centree_writer_parent(&writer, p);
  /* Q is a routing ancestor of P, so P is never the root. */
  if (s == NULL || g == NULL) {
    rcu_writer_abort(&tree->topology_rcu, &writer);
    die("Pruning: the leaf's routing parent has no sibling or no parent\n");
  }

  /* P and the leaf out, S up into P's place. */
  if (centree_writer_left(&writer, g) == p)
    centree_writer_set_left(&writer, g, s);
  else
    centree_writer_set_right(&writer, g, s);
  centree_writer_set_parent(&writer, s, g);

  /*
   * Q's children are read *after* that, so if G == Q this picks up S. The
   * parent stores below then override S->parent = G, which is correct: S ends
   * up under N.
   */
  ql = centree_writer_left(&writer, q);
  qr = centree_writer_right(&writer, q);
  gq = centree_writer_parent(&writer, q);
  centree_writer_set_left(&writer, n, ql);
  centree_writer_set_right(&writer, n, qr);
  centree_writer_set_parent(&writer, n, gq);
  centree_writer_set_parent(&writer, ql, n);
  centree_writer_set_parent(&writer, qr, n);
  if (gq == NULL)
    centree_writer_set_root(&writer, tree, n);
  else if (centree_writer_left(&writer, gq) == q)
    centree_writer_set_left(&writer, gq, n);
  else
    centree_writer_set_right(&writer, gq, n);

  /* Three nodes out, one in. depth and level stay advisory and go stale. */
  centree_node_count_sub(tree, 2);

  tnt_root_wlock();
  rcu_writer_publish_deferred(&tree->topology_rcu, &writer, NULL, NULL);
  tnt_root_wunlock();
  rcu_writer_finish_deferred(&tree->topology_rcu, &writer);

  /*
   * Retire only now (I-3): a reader or writer that restarts because of these
   * flags has to find the new topology, not the old one.
   */
  atomic_store_explicit(&c->leaf->removed, 1, memory_order_seq_cst);
  atomic_store_explicit(&c->inner->removed, 1, memory_order_seq_cst);
  atomic_store_explicit(&c->outer->removed, 1, memory_order_seq_cst);
  /* Sets child_flag and wakes: the parked writers restart (step 3). */
  wakeup_subtree_get(c->leaf);
}

/* ===================================================================== *
 * Retire
 * ===================================================================== */

void prune_retire(const struct prune_candidate *c) {
  slab_retire(c->leaf->value.slab);
  slab_retire(c->inner->value.slab);
  slab_retire(c->outer->value.slab);
}

/* ===================================================================== *
 * One whole prune
 * ===================================================================== */

uint64_t prune_bad_slot_count(void) {
  return __sync_fetch_and_or(&prune_bad_slots, 0);
}

static int tnt_prune_once_impl(void);

int tnt_prune_once(void) {
  TEST_STAT_INC(prune_calls);
  int status = tnt_prune_once_impl();
  if (status == TNT_PRUNE_DONE) report_event(REPORT_PRUNING);
  else if (status == TNT_PRUNE_NOOP) TEST_STAT_INC(prune_noop);
  return status;
}

static int tnt_prune_once_impl(void) {
  struct prune_candidate c;
  struct prune_build b;
  int error;

  if (tnt_centree() == NULL)
    return -EINVAL;

  /*
   * Selection runs under the same lock as the rest, so the candidate cannot
   * go stale between picking it and freezing its leaf.
   */
  tnt_maintenance_lock();
  if (!prune_scan_for_candidate(&c)) {
    tnt_maintenance_unlock();
    return TNT_PRUNE_NOOP;
  }
  error = prune_freeze_and_link(&c, &b);
  if (!error) {
    prune_splice_routing(&c, &b);
    prune_retire(&c);
    prune_build_release_buffer(&b);
  }
  tnt_maintenance_unlock();

  if (error)
    return error;
  printf("Prune: %lu <- %lu/%lu/%lu\n", b.slab->seq, c.outer->value.slab->seq,
         c.leaf->value.slab->seq, c.inner->value.slab->seq);
  return TNT_PRUNE_DONE;
}

/* Reporting visits each reachable node once. Maintenance keeps its slab from
 * being replaced while we gather local-index snapshots. Concurrent splits and
 * writes are allowed: gauges describe the sampling pass, not a global epoch. */
void tnt_report_entries(uint64_t *total, uint64_t *stale, uint64_t *tombstones) {
  *total = *stale = *tombstones = 0;
  centree tree = tnt_centree();
  if (!tree) return;
  tnt_maintenance_lock();
  size_t capacity = 64, count = 0;
  centree_node *nodes = malloc(capacity * sizeof(*nodes));
  if (!nodes) die("Cannot allocate report snapshot\n");
  centree_read_in(tree);
  centree_node root = centree_read_root(tree);
  if (root) nodes[count++] = root;
  for (size_t i = 0; i < count; i++) {
    centree_node children[2] = {centree_read_left(tree, nodes[i]), centree_read_right(tree, nodes[i])};
    for (int j = 0; j < 2; j++) if (children[j]) {
      if (count == capacity) {
        capacity *= 2;
        centree_node *next = realloc(nodes, capacity * sizeof(*nodes));
        if (!next) die("Cannot grow report snapshot\n");
        nodes = next;
      }
      nodes[count++] = children[j];
    }
  }
  centree_read_out(tree);
  for (size_t i = 0; i < count; i++) {
    struct slab *s = nodes[i]->value.slab;
    W_LOCK(&s->tree_lock);
    if (s->subtree) subtree_report_counts(s->subtree, total, stale, tombstones);
    W_UNLOCK(&s->tree_lock);
  }
  free(nodes);
  tnt_maintenance_unlock();
}
