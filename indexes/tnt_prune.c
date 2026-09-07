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
#include <time.h>
#include <stdbool.h>

/* Per-prune phase timing, printed after the "Prune:" line. */
struct prune_timing {
  uint64_t scan, drain, snap_cold, copy_cold, flush_cold, freeze, copy_leaf,
      finish, headers, splice, retire;
  uint64_t preads, cold_entries, leaf_entries, tombstones_dropped;
};
static struct prune_timing pt;

static uint64_t now_us(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/*
 * Batched page reads for the merge. One synchronous 4 KiB read per touched
 * page was the pruner's whole cost under load: each read waited its turn
 * behind the workload's queue (~1.5 ms against ~20 us on an idle device), so a
 * source with 10k live entries took 15 s. Submitting the touched pages as
 * batches of QUEUE_DEPTH concurrent reads pays that wait once per batch. The
 * pruner is a single thread, so it owns one aio context of its own.
 */
#define PRUNE_READ_BATCH QUEUE_DEPTH

static aio_context_t prune_aio;
static struct iocb prune_iocb[PRUNE_READ_BATCH];
static struct iocb *prune_iocbs[PRUNE_READ_BATCH];
static struct io_event prune_events[PRUNE_READ_BATCH];

static int prune_aio_init(void) {
  if (prune_aio != 0)
    return 0;
  if (syscall(__NR_io_setup, PRUNE_READ_BATCH, &prune_aio) != 0)
    return -errno;
  for (int i = 0; i < PRUNE_READ_BATCH; i++)
    prune_iocbs[i] = &prune_iocb[i];
  return 0;
}

/* Reads pages[0..nb) of fd into buf, PAGE_SIZE each, in submission batches. */
static int prune_read_pages(int fd, const size_t *pages, size_t nb, char *buf) {
  int error = prune_aio_init();

  if (error)
    return error;
  for (size_t done = 0; done < nb;) {
    long batch = (long)(nb - done);
    long got = 0;

    if (batch > PRUNE_READ_BATCH)
      batch = PRUNE_READ_BATCH;
    for (long i = 0; i < batch; i++) {
      struct iocb *cb = &prune_iocb[i];

      memset(cb, 0, sizeof(*cb));
      cb->aio_fildes = (uint32_t)fd;
      cb->aio_lio_opcode = IOCB_CMD_PREAD;
      cb->aio_buf = (uint64_t)(uintptr_t)(buf + (done + (size_t)i) * PAGE_SIZE);
      cb->aio_nbytes = PAGE_SIZE;
      cb->aio_offset = (int64_t)(pages[done + (size_t)i] * PAGE_SIZE);
    }
    for (long sent = 0; sent < batch;) {
      long r = syscall(__NR_io_submit, prune_aio, batch - sent,
                       prune_iocbs + sent);

      if (r < 0)
        return -errno;
      sent += r;
    }
    pt.preads++;
    while (got < batch) {
      long r = syscall(__NR_io_getevents, prune_aio, batch - got, batch - got,
                       prune_events, NULL);

      if (r < 0)
        return -errno;
      for (long i = 0; i < r; i++)
        if (prune_events[i].res != (int64_t)PAGE_SIZE)
          return -EIO;
      got += r;
    }
    done += (size_t)batch;
  }
  return 0;
}

static bool retired(centree_node n) {
  return atomic_load_explicit(&n->removed, memory_order_acquire) != 0;
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
  PRUNE_TOO_YOUNG,
  PRUNE_NB_REASONS
};
static const char *prune_reject_name[PRUNE_NB_REASONS] = {
  "ok", "no_inner", "no_outer", "not_history_child", "same_side",
  "no_sib_star", "internal_splitting", "leaf_splitting", "retired",
  "leaf_full", "no_fit", "too_young"
};

/*
 * `overflow` (may be NULL) receives, for PRUNE_NO_FIT, how many slots the
 * triple's valid entries exceed one slab by: the distance to prunability.
 */
static enum prune_reject prune_select_why(centree_node leaf,
                                          struct prune_candidate *out,
                                          size_t *overflow) {
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
    if (overflow) {
      /* Diagnostic only: would the three fit, adjacency aside? */
      struct slab *ls_ = leaf->value.slab, *is_ = inner->value.slab,
                  *os_ = outer->value.slab;
      size_t need = is_->nb_items + os_->nb_items + ls_->nb_items + cfg.prune_margin;

      *overflow = need > ls_->nb_max_items ? need - ls_->nb_max_items : 0;
    }
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
  if (cold_bound + ls->nb_items + cfg.prune_margin > ls->nb_max_items) {
    if (overflow)
      *overflow = cold_bound + ls->nb_items + cfg.prune_margin - ls->nb_max_items;
    return PRUNE_NO_FIT;
  }

  /*
   * A leaf that was created a moment ago is where the writes are going, and
   * it is attractive to the fit test precisely because it is still nearly
   * empty. Skipping the newest slabs keeps the pruner off the hot spot.
   */
  if (cfg.prune_min_age &&
      slab_create_sequence() - ls->seq < cfg.prune_min_age)
    return PRUNE_TOO_YOUNG;

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
  return prune_select_why(leaf, out, NULL) == PRUNE_OK;
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

  for (size_t i = 0; i < nb && !found; i++)
    found = prune_select(leaves[i], out);

  free(leaves);
  return found;
}

/*
 * Diagnostic: one full scan, every leaf classified. Answers "was there ever a
 * candidate, and if not, what stopped the closest triple". Nothing mutates.
 */
void prune_scan_report(const char *phase) {
  centree tree = tnt_centree();
  struct prune_candidate candidate;
  centree_node *leaves;
  size_t capacity, nb = 0, hist[PRUNE_NB_REASONS] = {0};
  size_t overflow, min_overflow = (size_t)-1, nb_max = 0, same_side_fits = 0;

  if (tree == NULL)
    return;
  capacity =
      atomic_load_explicit(&tree->node_count, memory_order_acquire) + 8;
  leaves = malloc(capacity * sizeof(*leaves));
  if (leaves == NULL)
    return;

  centree_read_in(tree);
  collect_leaves(tree, centree_read_root(tree), leaves, capacity, &nb);
  centree_read_out(tree);

  for (size_t i = 0; i < nb; i++) {
    overflow = (size_t)-1;
    enum prune_reject why = prune_select_why(leaves[i], &candidate, &overflow);
    hist[why]++;
    if (why == PRUNE_SAME_SIDE && overflow == 0)
      same_side_fits++;
    if (why == PRUNE_NO_FIT && overflow < min_overflow) {
      min_overflow = overflow;
      nb_max = leaves[i]->value.slab->nb_max_items;
    }
  }
  free(leaves);

  printf("#R %s prune-scan: leaves=%zu candidates=%zu", phase, nb, hist[PRUNE_OK]);
  for (int r = 1; r < PRUNE_NB_REASONS; r++)
    printf(" %s=%zu", prune_reject_name[r], hist[r]);
  if (hist[PRUNE_NO_FIT])
    printf(" closest_overflow_slots=%zu slab_slots=%zu", min_overflow, nb_max);
  printf(" same_side_would_fit=%zu\n", same_side_fits);
}

/*
 * Diagnostic: where do the stale slots live? For every node (leaf and
 * internal separately): stale fraction per slab, a histogram by decile, the
 * share of all stale slots that sit in slabs above 50/80/90% stale, and how
 * the stale-heavy slabs line up along history (parent also stale-heavy, and
 * the longest such chain). Nothing mutates.
 */
static void collect_nodes(centree tree, centree_node n, centree_node *out,
                          size_t capacity, size_t *nb) {
  if (n == NULL)
    return;
  if (*nb < capacity)
    out[(*nb)++] = n;
  collect_nodes(tree, centree_read_left(tree, n), out, capacity, nb);
  collect_nodes(tree, centree_read_right(tree, n), out, capacity, nb);
}

static double node_stale_frac(centree_node n, size_t *reserved_out,
                              size_t *stale_out) {
  struct slab *s = n->value.slab;
  size_t reserved = atomic_load_explicit(&s->last_item, memory_order_acquire);
  size_t valid = s->nb_items;

  if (reserved > s->nb_max_items) reserved = s->nb_max_items;
  if (valid > reserved) valid = reserved;
  *reserved_out = reserved;
  *stale_out = reserved - valid;
  return reserved ? (double)(reserved - valid) / reserved : 0.0;
}

void prune_stale_distribution_report(const char *phase) {
  centree tree = tnt_centree();
  centree_node *nodes;
  size_t capacity, nb = 0;
  /* [0] internal, [1] leaf */
  size_t cnt[2] = {0}, hist[2][10] = {{0}}, stale_tot[2] = {0}, res_tot[2] = {0};
  size_t stale_ge50[2] = {0}, stale_ge80[2] = {0}, stale_ge90[2] = {0};
  size_t slabs_ge50[2] = {0}, slabs_ge80[2] = {0}, slabs_ge90[2] = {0};
  size_t pairs50 = 0, heavy50 = 0, longest = 0;

  if (tree == NULL)
    return;
  capacity = atomic_load_explicit(&tree->node_count, memory_order_acquire) + 8;
  nodes = malloc(capacity * sizeof(*nodes));
  if (nodes == NULL)
    return;
  centree_read_in(tree);
  collect_nodes(tree, centree_read_root(tree), nodes, capacity, &nb);
  centree_read_out(tree);

  for (size_t i = 0; i < nb; i++) {
    size_t reserved, stale;
    double f = node_stale_frac(nodes[i], &reserved, &stale);
    int k = child_flag(nodes[i]) == 1 ? 0 : 1;
    int b = (int)(f * 10); if (b > 9) b = 9;

    cnt[k]++; hist[k][b]++; stale_tot[k] += stale; res_tot[k] += reserved;
    if (f >= 0.5) { slabs_ge50[k]++; stale_ge50[k] += stale; }
    if (f >= 0.8) { slabs_ge80[k]++; stale_ge80[k] += stale; }
    if (f >= 0.9) { slabs_ge90[k]++; stale_ge90[k] += stale; }
    if (k == 0 && f >= 0.5) {
      /* history adjacency among stale-heavy internal nodes */
      centree_node p = centree_lu_parent(nodes[i]);
      size_t chain = 1, r2, s2;

      heavy50++;
      if (p && node_stale_frac(p, &r2, &s2) >= 0.5)
        pairs50++;
      while (p && node_stale_frac(p, &r2, &s2) >= 0.5) {
        chain++;
        p = centree_lu_parent(p);
      }
      if (chain > longest) longest = chain;
    }
  }
  free(nodes);

  for (int k = 0; k < 2; k++) {
    const char *kind = k ? "leaf" : "internal";
    double tot = stale_tot[k] ? (double)stale_tot[k] : 1.0;

    printf("#R %s stale-dist %s: slabs=%zu reserved=%zu stale=%zu (%.1f%%) "
           "hist_decile=%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu "
           "ge50: slabs=%zu stale_share=%.1f%% ge80: slabs=%zu stale_share=%.1f%% "
           "ge90: slabs=%zu stale_share=%.1f%%\n",
           phase, kind, cnt[k], res_tot[k], stale_tot[k],
           res_tot[k] ? 100.0 * stale_tot[k] / res_tot[k] : 0.0,
           hist[k][0], hist[k][1], hist[k][2], hist[k][3], hist[k][4],
           hist[k][5], hist[k][6], hist[k][7], hist[k][8], hist[k][9],
           slabs_ge50[k], 100.0 * stale_ge50[k] / tot,
           slabs_ge80[k], 100.0 * stale_ge80[k] / tot,
           slabs_ge90[k], 100.0 * stale_ge90[k] / tot);
  }
  printf("#R %s stale-adjacency: internal_ge50=%zu with_parent_ge50=%zu "
         "longest_ge50_chain=%zu\n", phase, heavy50, pairs50, longest);
}

/*
 * Heavy monitoring (--dump-slabs): one line per node.
 *   #S <t_s> seq kind hist_parent_seq level pivot min max reserved valid stale tombstones
 * kind: I internal, L leaf. min/max are the slab's own stored key range.
 */
void prune_dump_slabs(double t_s) {
  centree tree = tnt_centree();
  centree_node *nodes;
  size_t capacity, nb = 0;

  if (tree == NULL)
    return;
  capacity = atomic_load_explicit(&tree->node_count, memory_order_acquire) + 8;
  nodes = malloc(capacity * sizeof(*nodes));
  if (nodes == NULL)
    return;
  centree_read_in(tree);
  collect_nodes(tree, centree_read_root(tree), nodes, capacity, &nb);
  centree_read_out(tree);
  printf("#S t_s seq kind parent level pivot min max reserved valid stale tombstones (%zu nodes)\n", nb);
  for (size_t i = 0; i < nb; i++) {
    centree_node n = nodes[i], p = centree_lu_parent(n);
    struct slab *s = n->value.slab;
    size_t reserved = atomic_load_explicit(&s->last_item, memory_order_acquire);
    size_t valid = s->nb_items;

    if (reserved > s->nb_max_items) reserved = s->nb_max_items;
    if (valid > reserved) valid = reserved;
    printf("#S %.0f %lu %c %lu %lu %lu %lu %lu %zu %zu %zu %zu\n", t_s, s->seq,
           child_flag(n) == 1 ? 'I' : 'L', p ? p->value.slab->seq : 0,
           (unsigned long)atomic_load_explicit(&n->value.level, memory_order_relaxed),
           (unsigned long)centree_pivot_load(n), (unsigned long)s->min,
           (unsigned long)s->max, reserved, valid, reserved - valid,
           atomic_load_explicit(&s->nb_tombstones, memory_order_relaxed));
  }
  fflush(stdout);
  free(nodes);
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

static void sum_stale(centree tree, centree_node n, struct prune_stale *acc) {
  struct slab *s;
  size_t reserved, valid;

  if (n == NULL)
    return;
  s = n->value.slab;
  reserved = atomic_load_explicit(&s->last_item, memory_order_acquire);
  if (reserved > s->nb_max_items)
    reserved = s->nb_max_items;
  valid = s->nb_items;
  if (valid > reserved)
    valid = reserved;
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
  uint64_t t0 = now_us();
  R_LOCK(&src->tree_lock);
  if (retired(source)) {
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
  if (override) {
    pt.leaf_entries += snap.nb;
  } else {
    pt.snap_cold += now_us() - t0;
    pt.cold_entries += snap.nb;
  }
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

    if (b->drop_tombstones && item_is_tombstone(meta)) {
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
      pt.tombstones_dropped++;
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
  if (b->slab != NULL) {
    char proc[64], path[512];
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
  free(b->node);
  if (b->buffer != NULL)
    munmap(b->buffer, pages_for_capacity(b) * PAGE_SIZE);
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
  uint64_t t = now_us();
  slab_drain_updates(c->inner->value.slab);
  slab_drain_updates(c->outer->value.slab);
  pt.drain += now_us() - t;
  t = now_us();
  error = prune_build_add_source(b, c->inner, 0);
  if (!error)
    error = prune_build_add_source(b, c->outer, 0);
  pt.copy_cold += now_us() - t;
  /* Get the cold pages onto the device before the freeze window opens. */
  t = now_us();
  if (!error)
    error = prune_build_flush(b);
  pt.flush_cold += now_us() - t;
  if (error) {
    prune_build_discard(b);
    return error;
  }

  t = now_us();
  frozen = slab_freeze(leaf_slab, b->capacity - b->count);
  pt.freeze += now_us() - t;
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
  t = now_us();
  error = prune_build_add_source(b, c->leaf, 1);
  pt.copy_leaf += now_us() - t;
  t = now_us();
  if (!error)
    error = prune_build_finish(b);
  pt.finish += now_us() - t;
  if (error)
    die("Pruning could not finish the merged slab after freezing slab %lu "
        "(%d); the frozen leaf has no way back\n",
        leaf_slab->seq, error);

  prune_link_history(c, b->node);
  t = now_us();

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
  pt.headers += now_us() - t;
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

static int tnt_prune_once_timed(void);

int tnt_prune_once(void) {
  struct timeval t0, t1;
  uint64_t us;
  int status;

  RSTAT_INC(prune_calls);
  gettimeofday(&t0, NULL);
  status = tnt_prune_once_timed();
  gettimeofday(&t1, NULL);
  us = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000 +
       (uint64_t)(t1.tv_usec - t0.tv_usec);
  if (status == TNT_PRUNE_DONE) {
    RSTAT_INC(prune_done);
    RSTAT_ADD(prune_us, us);
    rstat_max(&rstats.prune_max_us, us);
  } else if (status == TNT_PRUNE_NOOP) {
    RSTAT_INC(prune_noop);
  } else if (status == -EAGAIN || status == -EBUSY || status == -ENOSPC) {
    RSTAT_INC(prune_dropped);
  } else {
    RSTAT_INC(prune_failed);
  }
  return status;
}

static int tnt_prune_once_timed(void) {
  struct prune_candidate c;
  struct prune_build b;
  int error;

  if (tnt_centree() == NULL)
    return -EINVAL;

  /*
   * Selection runs under the same lock as the rest, so the candidate cannot
   * go stale between picking it and freezing its leaf.
   */
  memset(&pt, 0, sizeof(pt));
  uint64_t t = now_us();
  tnt_maintenance_lock();
  if (!prune_scan_for_candidate(&c)) {
    tnt_maintenance_unlock();
    return TNT_PRUNE_NOOP;
  }
  pt.scan = now_us() - t;
  error = prune_freeze_and_link(&c, &b);
  if (!error) {
    t = now_us();
    prune_splice_routing(&c, &b);
    pt.splice = now_us() - t;
    t = now_us();
    prune_retire(&c);
    pt.retire = now_us() - t;
    prune_build_release_buffer(&b);
  }
  tnt_maintenance_unlock();

  if (error)
    return error;
  printf("Prune: %lu <- %lu/%lu/%lu\n", b.slab->seq, c.outer->value.slab->seq,
         c.leaf->value.slab->seq, c.inner->value.slab->seq);
  printf("Prune timing (ms): scan=%.1f drain=%.1f snapshot=%.1f copy_cold=%.1f "
         "flush_cold=%.1f freeze=%.1f copy_leaf=%.1f finish=%.1f headers=%.1f "
         "splice=%.1f retire=%.1f | read_batches=%lu cold_entries=%lu "
         "leaf_entries=%lu merged=%zu tombstones_dropped=%lu%s\n",
         pt.scan / 1e3, pt.drain / 1e3, pt.snap_cold / 1e3,
         (pt.copy_cold - pt.snap_cold) / 1e3, pt.flush_cold / 1e3,
         pt.freeze / 1e3, pt.copy_leaf / 1e3, pt.finish / 1e3, pt.headers / 1e3,
         pt.splice / 1e3, pt.retire / 1e3, pt.preads, pt.cold_entries,
         pt.leaf_entries, b.count, pt.tombstones_dropped,
         c.up == NULL ? " (root triple)" : "");
  return TNT_PRUNE_DONE;
}
