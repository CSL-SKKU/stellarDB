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
#include <stdbool.h>

/* Slack kept free in the merged slab. Step 9 makes this configurable. */
static size_t prune_fit_margin;

static bool retired(centree_node n) {
  return atomic_load_explicit(&n->removed, memory_order_acquire) != 0;
}

static int child_flag(centree_node n) {
  return atomic_load_explicit(&n->child_flag, memory_order_acquire);
}

bool prune_select(centree_node leaf, struct prune_candidate *out) {
  centree_node inner, outer, sib, star;
  struct slab *ls, *is, *os;
  size_t cold_bound;
  int side;

  if (leaf == NULL)
    return false;

  inner = centree_lu_parent(leaf);
  if (inner == NULL)
    return false;
  outer = centree_lu_parent(inner);
  if (outer == NULL)
    return false;

  if (inner->lu_child[CENTREE_LU_LEFT] == leaf)
    side = CENTREE_LU_LEFT;
  else if (inner->lu_child[CENTREE_LU_RIGHT] == leaf)
    side = CENTREE_LU_RIGHT;
  else
    return false;

  /* Opposite sides: only then is the triple consecutive in-order. */
  if (outer->lu_child[!side] != inner)
    return false;

  sib = inner->lu_child[!side];
  star = outer->lu_child[side];
  if (sib == NULL || star == NULL)
    return false;

  /*
   * Both internals must be fully split -- a half-published split would leave
   * the routing splice with a node whose children are still moving. The leaf
   * must be a live, appendable leaf: child_flag 0 and a slab that is not full
   * rules out a leaf that is mid-split or about to be.
   */
  if (child_flag(inner) != 1 || child_flag(outer) != 1)
    return false;
  if (child_flag(leaf) != 0)
    return false;
  if (retired(leaf) || retired(inner) || retired(outer))
    return false;

  ls = leaf->value.slab;
  is = inner->value.slab;
  os = outer->value.slab;
  if (atomic_load_explicit(&ls->full, memory_order_acquire))
    return false;

  /*
   * nb_items is only ever decremented when an entry is invalidated, so it is
   * an upper bound on the valid entries and this test is conservative. The
   * authoritative fit test is slab_freeze()'s budget, which runs against the
   * count actually copied.
   */
  cold_bound = is->nb_items + os->nb_items;
  if (cold_bound + ls->nb_items + prune_fit_margin > ls->nb_max_items)
    return false;

  out->leaf = leaf;
  out->inner = inner;
  out->outer = outer;
  out->up = centree_lu_parent(outer); /* D, NULL at the history root */
  out->sib = sib;
  out->star = star;
  out->side = side;
  out->cold_bound = cold_bound;
  return true;
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

struct prune_src_entry {
  uint64_t key;
  uint32_t slot;
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

int prune_build_begin(const struct prune_candidate *c,
                      struct prune_build *out) {
  struct slab *leaf_slab = c->leaf->value.slab;
  tree_entry_t value = {0};
  size_t pages;

  memset(out, 0, sizeof(*out));

  /*
   * Room for what selection said could survive. The freeze budget in the
   * next step is derived from what is left of this, so the buffer size is
   * also the hard cap on the merged slab.
   */
  out->capacity = c->cold_bound + leaf_slab->nb_items + prune_fit_margin;
  if (out->capacity > leaf_slab->nb_max_items)
    out->capacity = leaf_slab->nb_max_items;

  /*
   * The throwaway build borrows outer's level and pivot; the real one takes
   * them from the routing node N replaces.
   */
  out->slab = create_slab(NULL,
                          atomic_load_explicit(&c->outer->value.level,
                                               memory_order_acquire),
                          centree_pivot_load(c->outer), 0, NULL);
  if (out->slab == NULL || out->slab->fd < 0) {
    free(out->slab);
    memset(out, 0, sizeof(*out));
    return -EIO;
  }
  out->slab->subtree = tnt_subtree_create();
  subtree_set_slab(out->slab->subtree, out->slab);

  pages = pages_for(out->slab, out->capacity);
  if (pages == 0)
    pages = 1;
  out->buffer = aligned_alloc(PAGE_SIZE, pages * PAGE_SIZE);
  if (out->buffer == NULL) {
    prune_build_discard(out);
    return -ENOMEM;
  }
  /* Unused slots stay zeroed, which is what item_is_empty() reads. */
  memset(out->buffer, 0, pages * PAGE_SIZE);

  value.key = centree_pivot_load(c->outer);
  value.seq = out->slab->seq;
  value.slab = out->slab;
  out->node = centree_node_new((void *)(uintptr_t)value.key, &value);
  atomic_store_explicit(&out->node->value.level,
                        atomic_load_explicit(&c->outer->value.level,
                                             memory_order_acquire),
                        memory_order_release);
  out->slab->centree_node = out->node;
  return 0;
}

int prune_build_add_source(struct prune_build *b, centree_node source) {
  struct slab *src = source->value.slab;
  struct slab *n = b->slab;
  struct prune_snapshot snap = {0};
  size_t items_per_page = PAGE_SIZE / src->item_size;
  size_t cached_page = (size_t)-1;
  char *page = NULL;
  int error = 0;

  snap.capacity = src->nb_max_items;
  snap.entries = malloc(snap.capacity * sizeof(*snap.entries));
  page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (snap.entries == NULL || page == NULL) {
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

  for (size_t i = 0; i < snap.nb; i++) {
    uint64_t key = snap.entries[i].key;
    size_t slot = snap.entries[i].slot;
    size_t page_idx = slot / items_per_page;
    index_entry_t existing;
    index_entry_t entry;
    struct item_metadata *meta;
    char *record;

    /* A newer source already placed this key: precedence leaf > inner > outer. */
    if (subtree_find(n->subtree, (unsigned char *)&key, sizeof(key),
                     &existing))
      continue;
    if (slot >= src->nb_max_items) {
      error = -EINVAL;
      goto out;
    }
    if (b->count == b->capacity) {
      error = -ENOSPC;
      goto out;
    }

    if (page_idx != cached_page) {
      ssize_t got = pread(src->fd, page, PAGE_SIZE,
                          (off_t)page_idx * PAGE_SIZE);

      if (got != (ssize_t)PAGE_SIZE) {
        error = -EIO;
        goto out;
      }
      cached_page = page_idx;
    }

    record = page + (slot % items_per_page) * src->item_size;
    meta = (struct item_metadata *)record;
    if (item_is_legacy(meta) || item_is_empty(meta) ||
        *(uint64_t *)(record + sizeof(*meta)) != key)
      die("Pruning found slab %lu slot %lu not holding indexed key %lu\n",
          src->seq, slot, key);

    memcpy(slot_in_buffer(b, b->count), record, n->item_size);
    entry.slab = n;
    entry.slab_idx = b->count;
    subtree_insert(n->subtree, (unsigned char *)&key, sizeof(key), &entry);
    slab_widen_range(n, key);
    b->count++;
  }

out:
  free(snap.entries);
  free(page);
  return error;
}

int prune_build_finish(struct prune_build *b) {
  struct slab *n = b->slab;
  size_t pages = pages_for(n, b->count);

  if (pages > 0) {
    ssize_t written = pwrite(n->fd, b->buffer, pages * PAGE_SIZE, 0);

    if (written != (ssize_t)(pages * PAGE_SIZE))
      return -EIO;
  }
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
  int error = prune_build_begin(c, out);

  if (error)
    return error;
  /* Newest first: inner overrides outer. The leaf joins after its freeze. */
  error = prune_build_add_source(out, c->inner);
  if (!error)
    error = prune_build_add_source(out, c->outer);
  if (!error)
    error = prune_build_finish(out);
  return error;
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
  free(b->buffer);
  memset(b, 0, sizeof(*b));
}
