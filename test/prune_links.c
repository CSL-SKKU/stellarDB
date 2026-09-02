/*
 * History-link (lu_parent / lu_child) invariants after real splits.
 *
 * Step 0 of the pruning plan makes lu_parent atomic and adds the lu_child[]
 * back-pointers. Candidate selection reads the history tree locally instead of
 * scanning the in-order sequence, which is only equivalent while:
 *
 *   - every internal node has exactly two history children that point back
 *     at it;
 *   - routing leaves have no history children;
 *   - exactly one node (the first slab) is the history root;
 *   - history in-order == routing in-order.
 *
 * The links here come from the real pipeline (close_and_create_slab() ->
 * centree_insert()), and the checks are repeated after tnt_rebalancing(),
 * which rewires routing but must leave history untouched.
 *
 * Usage: ./test/test_prune_links [nb_keys]
 */
#include "headers.h"

#include <limits.h>
#include <linux/futex.h>
#include <stdbool.h>
#include <stdarg.h>

int print = 0;
int load = 0;
int rc_thr = 1;

#define TEST_KV_SIZE 128
#define TEST_MAX_FILE_SIZE (32LU * PAGE_SIZE) /* 1024 items per slab */

static uint64_t nb_keys = 20000;
static uint64_t failures;

static void check(bool ok, const char *fmt, ...) {
  if (ok) return;
  va_list ap;
  va_start(ap, fmt);
  printf("  FAIL ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  failures++;
}

/* ------------------------------------------------------------- requests */

struct req {
  struct slab_callback cb;
  unsigned char *item;
  _Atomic int done;
};

static void write_done(struct slab_callback *cb, void *item) {
  (void)item;
  atomic_store(&((struct req *)cb)->done, 1);
}

static unsigned char *make_item(uint64_t key, uint64_t value) {
  unsigned char *item = calloc(1, TEST_KV_SIZE);
  struct item_metadata *meta = (struct item_metadata *)item;

  item_init(meta, sizeof(uint64_t),
            TEST_KV_SIZE - sizeof(*meta) - sizeof(uint64_t));
  *(uint64_t *)(item + sizeof(*meta)) = key;
  *(uint64_t *)(item + sizeof(*meta) + sizeof(uint64_t)) = value;
  return item;
}

/*
 * Keys [lo, hi) with key % modulus == residue (modulus 0 means all of them).
 * version distinguishes the passes, so an older copy of a key differs from the
 * newest one; del writes a tombstone instead.
 */
static void run_ops(uint64_t lo, uint64_t hi, int modulus, int residue,
                    uint64_t version, int del, int invert) {
  uint64_t n = 0;
  struct req *reqs = calloc(hi - lo, sizeof(*reqs));

  for (uint64_t k = lo; k < hi; k++) {
    struct req *r;
    int selected = !modulus || (k % (uint64_t)modulus) == (uint64_t)residue;

    if (invert) selected = modulus && !selected;
    if (!selected) continue;
    r = &reqs[n++];
    r->item = make_item(k, k * 7 + version);
    r->cb.cb = write_done;
    r->cb.item = (char *)r->item;
    r->cb.fsst_idx = -1;
    atomic_store(&r->done, 0);
    if (del)
      kv_remove_async(&r->cb);
    else
      kv_upsert_async(&r->cb);
  }
  for (uint64_t i = 0; i < n; i++)
    while (!atomic_load(&reqs[i].done)) NOP10();
  for (uint64_t i = 0; i < n; i++) free(reqs[i].item);
  free(reqs);
}

static void run_upserts(uint64_t lo, uint64_t hi) {
  run_ops(lo, hi, 0, 0, 1, 0, 0);
}

/* --------------------------------------------------------------- checks */

static centree_node routing_root(void) {
  background_queue q;

  init_queue(&q);
  if (tnt_get_nodes_at_level(0, &q) != 1) return NULL;
  return dequeue_centnode(&q);
}

static void collect_routing(centree_node n, centree_node *out, size_t cap,
                            size_t *idx) {
  if (!n) return;
  collect_routing(tnt_routing_left(n), out, cap, idx);
  if (*idx < cap) out[*idx] = n;
  (*idx)++;
  collect_routing(tnt_routing_right(n), out, cap, idx);
}

static void collect_history(centree_node n, centree_node *out, size_t cap,
                            size_t *idx) {
  if (!n) return;
  collect_history(n->lu_child[CENTREE_LU_LEFT], out, cap, idx);
  if (*idx < cap) out[*idx] = n;
  (*idx)++;
  collect_history(n->lu_child[CENTREE_LU_RIGHT], out, cap, idx);
}

static void validate(const char *when) {
  size_t total = tnt_get_node_count();
  size_t cap = total + 8;
  centree_node *routing = calloc(cap, sizeof(*routing));
  centree_node *history = calloc(cap, sizeof(*history));
  centree_node history_root = NULL;
  size_t nb_routing = 0, nb_history = 0, nb_history_roots = 0;

  collect_routing(routing_root(), routing, cap, &nb_routing);
  check(nb_routing == total, "[%s] routing in-order has %zu nodes, node_count %zu",
        when, nb_routing, total);
  if (nb_routing > cap) goto out;

  for (size_t i = 0; i < nb_routing; i++) {
    centree_node n = routing[i];
    bool leaf = !tnt_routing_left(n) && !tnt_routing_right(n);
    struct slab *s = n->value.slab;

    /*
     * Every request has completed, so both reference counts must be back to
     * zero. A writer that descends past a slab has to drop the update
     * reference it took before the full check (I-8), and this is what catches
     * a missed drop.
     */
    check(__sync_fetch_and_or(&s->update_ref, 0) == 0,
          "[%s] slab seq %lu leaked %lu update refs", when, s->seq,
          s->update_ref);
    check(__sync_fetch_and_or(&s->read_ref, 0) == 0,
          "[%s] slab seq %lu leaked %lu read refs", when, s->seq, s->read_ref);

    check(leaf == (i % 2 == 0),
          "[%s] in-order position %zu is %s, expected %s", when, i,
          leaf ? "a leaf" : "internal", i % 2 == 0 ? "a leaf" : "internal");
    if (leaf) {
      check(n->lu_child[CENTREE_LU_LEFT] == NULL &&
                n->lu_child[CENTREE_LU_RIGHT] == NULL,
            "[%s] leaf at %zu has history children", when, i);
    } else {
      for (int k = 0; k < 2; k++) {
        centree_node c = n->lu_child[k];

        check(c != NULL, "[%s] internal node at %zu has no lu_child[%d]", when,
              i, k);
        if (c)
          check(centree_lu_parent(c) == n,
                "[%s] lu_child[%d] of in-order %zu does not point back", when,
                k, i);
      }
    }
    if (centree_lu_parent(n) == NULL) {
      nb_history_roots++;
      history_root = n;
    }
  }
  check(nb_history_roots == 1, "[%s] %zu nodes have no lu_parent, expected 1",
        when, nb_history_roots);

  collect_history(history_root, history, cap, &nb_history);
  check(nb_history == nb_routing,
        "[%s] history reaches %zu nodes, routing has %zu", when, nb_history,
        nb_routing);
  if (nb_history == nb_routing && nb_history <= cap) {
    for (size_t i = 0; i < nb_history; i++)
      check(history[i] == routing[i],
            "[%s] history in-order != routing in-order at %zu", when, i);
  }

  printf("  %-34s %zu nodes checked\n", when, nb_routing);
out:
  free(routing);
  free(history);
}

/* ------------------------------------------- read restart on a retired node */

/*
 * The hook marks the first node the upward walk touches as retired. A correct
 * read path must restart the descent, i.e. come back to that same node, rather
 * than skipping past it to its lu_parent -- the retired node's entries live in
 * the replacement node the published topology routes to. The second visit
 * clears the flag so the walk can finish.
 */
static centree_node trip_first;
static int trip_state; /* 0: armed, 1: marked, 2: restart seen */
static uint64_t trip_steps;

static void trip_hook(centree_node n) {
  trip_steps++;
  if (trip_state == 0) {
    trip_first = n;
    trip_state = 1;
    atomic_store(&n->removed, 1);
    return;
  }
  if (trip_state == 1) {
    check(n == trip_first,
          "walk moved on to another node instead of restarting");
    atomic_store(&n->removed, 0);
    trip_state = 2;
  }
}

static void check_read_restart(uint64_t key) {
  unsigned char *item = make_item(key, 0);
  struct slab_callback cb = {.item = (char *)item};
  struct slab *want_slab;
  uint64_t want_idx;
  index_entry_t *e;

  /* Reference: what an undisturbed lookup returns. */
  e = tnt_index_lookup(&cb, item);
  check(e != NULL, "key %lu not in the index", key);
  if (!e) {
    free(item);
    return;
  }
  want_slab = e->slab;
  want_idx = GET_SIDX(e->slab_idx);
  tnt_index_lookup_unref(e);

  trip_first = NULL;
  trip_state = 0;
  trip_steps = 0;
  tnt_set_index_lookup_step_test_hook(trip_hook);
  e = tnt_index_lookup(&cb, item);
  tnt_set_index_lookup_step_test_hook(NULL);

  check(trip_state == 2, "key %lu: no restart observed (state %d, %lu steps)",
        key, trip_state, trip_steps);
  check(e != NULL, "key %lu: lost after a restart", key);
  if (e) {
    check(e->slab == want_slab && GET_SIDX(e->slab_idx) == want_idx,
          "key %lu: restart returned a different entry", key);
    check(e->slab->read_ref == 1, "key %lu: read_ref %lu after lookup, want 1",
          key, e->slab->read_ref);
    tnt_index_lookup_unref(e);
    check(e->slab->read_ref == 0, "key %lu: read_ref %lu after unref, want 0",
          key, e->slab->read_ref);
  }
  if (trip_first)
    check(atomic_load(&trip_first->removed) == 0, "node left marked retired");
  printf("  %-34s key %lu, %lu walk steps\n", "restart on retired node", key,
         trip_steps);
  free(item);
}

/* ------------------------------------------- candidate selection */

/*
 * An independent implementation of the picking rule, written from the
 * AGENTS.md wording rather than from prune_select(): walk the in-order
 * sequence, take every consecutive internal-leaf-internal triple, and keep it
 * when it is "historically adjacent", i.e. two of the three have lu_parent
 * pointing inside the triple. prune_select() evaluates the same rule locally
 * on the history tree; the two sets must agree.
 */
static bool node_is_leaf(centree_node n) {
  return !tnt_routing_left(n) && !tnt_routing_right(n);
}

static bool in_triple(centree_node n, centree_node x, centree_node y,
                      centree_node z) {
  return n == x || n == y || n == z;
}

static bool triple_fits(centree_node x, centree_node l, centree_node y) {
  struct slab *ls = l->value.slab;

  return x->value.slab->nb_items + y->value.slab->nb_items + ls->nb_items <=
         ls->nb_max_items;
}

static size_t scan_ili(centree_node *order, size_t nb, centree_node *out,
                       size_t capacity) {
  size_t found = 0;

  for (size_t i = 1; i + 2 < nb; i += 2) {
    centree_node x, l, y;
    int inside = 0;

    x = order[i];
    l = order[i + 1];
    y = order[i + 2];

    if (node_is_leaf(x) || !node_is_leaf(l) || node_is_leaf(y)) continue;
    if (in_triple(centree_lu_parent(x), x, l, y)) inside++;
    if (in_triple(centree_lu_parent(l), x, l, y)) inside++;
    if (in_triple(centree_lu_parent(y), x, l, y)) inside++;
    if (inside != 2) continue;

    if (atomic_load(&x->child_flag) != 1 || atomic_load(&y->child_flag) != 1 ||
        atomic_load(&l->child_flag) != 0)
      continue;
    if (atomic_load(&x->removed) || atomic_load(&y->removed) ||
        atomic_load(&l->removed))
      continue;
    if (atomic_load(&l->value.slab->full)) continue;
    if (!triple_fits(x, l, y)) continue;

    if (found < capacity) out[found] = l;
    found++;
  }
  return found;
}

/* Accumulated over every phase, asserted once at the end. */
static size_t cov_candidates, cov_side[2], cov_history_root, cov_max_cold;

static void check_selection(const char *when) {
  size_t total = tnt_get_node_count();
  size_t cap = total + 8;
  centree_node *order = calloc(cap, sizeof(*order));
  centree_node *picked = calloc(cap, sizeof(*picked));
  centree_node *scanned = calloc(cap, sizeof(*scanned));
  size_t nb_order = 0, nb_picked = 0, nb_scanned;
  size_t sides[2] = {0, 0}, history_root_triples = 0;

  collect_routing(routing_root(), order, cap, &nb_order);

  for (size_t i = 0; i < nb_order; i += 2) {
    struct prune_candidate c;

    if (!prune_select(order[i], &c)) continue;
    check(c.leaf == order[i], "[%s] candidate leaf mismatch", when);
    check(c.inner->lu_child[c.side] == c.leaf,
          "[%s] side %d does not hold the leaf", when, c.side);
    check(c.outer->lu_child[!c.side] == c.inner,
          "[%s] inner is not on the opposite side of outer", when);
    sides[c.side]++;
    if (!c.up) history_root_triples++;
    if (c.cold_bound > cov_max_cold) cov_max_cold = c.cold_bound;
    if (nb_picked < cap) picked[nb_picked] = order[i];
    nb_picked++;
  }

  nb_scanned = scan_ili(order, nb_order, scanned, cap);
  check(nb_picked == nb_scanned,
        "[%s] prune_select found %zu candidates, the in-order scan %zu", when,
        nb_picked, nb_scanned);
  if (nb_picked == nb_scanned && nb_picked <= cap) {
    for (size_t i = 0; i < nb_picked; i++)
      check(picked[i] == scanned[i],
            "[%s] candidate %zu differs between the two rules", when, i);
  }
  check(nb_picked == prune_count_candidates(),
        "[%s] prune_count_candidates() disagrees with a direct sweep", when);

  cov_candidates += nb_picked;
  cov_side[0] += sides[0];
  cov_side[1] += sides[1];
  cov_history_root += history_root_triples;

  printf("  %-34s %zu candidates (side0 %zu, side1 %zu, D==NULL %zu) of %zu "
         "leaves, cold<=%zu\n",
         when, nb_picked, sides[0], sides[1], history_root_triples,
         (nb_order + 1) / 2, cov_max_cold);
  free(order);
  free(picked);
  free(scanned);
}

/* ------------------------------------------------------- read-back helper */

static _Atomic int rb_done;
static int rb_found;
static uint64_t rb_value;

static void read_back_done(struct slab_callback *cb, void *item) {
  struct item_metadata *meta = item;

  (void)cb;
  rb_found = item != NULL;
  if (item)
    rb_value =
        *(uint64_t *)((unsigned char *)item + sizeof(*meta) + sizeof(uint64_t));
  atomic_store(&rb_done, 1);
}

static void read_back_quiet(uint64_t key) {
  unsigned char *item = make_item(key, 0);
  struct slab_callback cb = {0};

  cb.cb = read_back_done;
  cb.item = (char *)item;
  cb.fsst_idx = -1;
  rb_found = 0;
  rb_value = 0;
  atomic_store(&rb_done, 0);
  kv_read_async(&cb);
  while (!atomic_load(&rb_done)) NOP10();
  free(item);
}

static void read_back(uint64_t key, uint64_t want) {
  read_back_quiet(key);
  check(rb_found, "key %lu is unreadable", key);
  if (rb_found)
    check(rb_value == want, "key %lu reads %lu, want %lu", key, rb_value, want);
}

/* --------------------------------------------- the merged slab N */

/*
 * What the merge is supposed to produce, derived straight from the sources:
 * every valid entry of inner, then every valid entry of outer whose key inner
 * does not already hold (precedence leaf > inner > outer; the leaf joins in a
 * later step). N must hold exactly that, byte for byte.
 */
struct exp_entry {
  uint64_t key;
  struct slab *src;
  uint32_t slot;
  int rank;
};

static struct exp_entry *exp_list;
static size_t nb_exp, exp_cap;
static struct slab *exp_src;
static int exp_rank;

static void exp_cb(uint64_t key, uint32_t slot, void *data) {
  (void)data;
  if (slot & (1u << 31)) return; /* stale */
  if (nb_exp == exp_cap) return;
  exp_list[nb_exp].key = key;
  exp_list[nb_exp].src = exp_src;
  exp_list[nb_exp].slot = (uint32_t)GET_SIDX(slot);
  exp_list[nb_exp].rank = exp_rank;
  nb_exp++;
}

static int cmp_exp(const void *a, const void *b) {
  const struct exp_entry *x = a, *y = b;

  if (x->key != y->key) return x->key < y->key ? -1 : 1;
  return x->rank - y->rank;
}

struct got_entry {
  uint64_t key;
  uint32_t slot;
};

static struct got_entry *got_list;
static size_t nb_got, got_cap;

static void got_cb(uint64_t key, uint32_t slot, void *data) {
  (void)data;
  if (nb_got == got_cap) return;
  got_list[nb_got].key = key;
  got_list[nb_got].slot = slot;
  nb_got++;
}

static char *slot_page;

static int read_slot(struct slab *s, size_t slot, unsigned char *out) {
  size_t items_per_page = PAGE_SIZE / s->item_size;

  if (!slot_page) slot_page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (pread(s->fd, slot_page, PAGE_SIZE,
            (off_t)(slot / items_per_page) * PAGE_SIZE) != (ssize_t)PAGE_SIZE)
    return 0;
  memcpy(out, slot_page + (slot % items_per_page) * s->item_size,
         s->item_size);
  return 1;
}

static size_t cold_tombstones, cold_read_mismatches, cold_entries,
    cold_dup_keys;

static void check_one_cold_build(const struct prune_candidate *c) {
  struct prune_build build;
  struct slab *n;
  size_t want = 0;
  int error;

  error = prune_build_cold(c, &build);
  if (error) {
    check(false, "prune_build_cold failed with %d", error);
    prune_build_discard(&build);
    return;
  }
  n = build.slab;

  /* Expected set. */
  exp_cap = c->inner->value.slab->nb_max_items +
            c->outer->value.slab->nb_max_items;
  exp_list = calloc(exp_cap, sizeof(*exp_list));
  nb_exp = 0;
  exp_src = c->inner->value.slab;
  exp_rank = 0;
  subtree_forall_entries(exp_src->subtree, exp_cb, NULL);
  exp_src = c->outer->value.slab;
  exp_rank = 1;
  subtree_forall_entries(exp_src->subtree, exp_cb, NULL);
  qsort(exp_list, nb_exp, sizeof(*exp_list), cmp_exp);

  /* What N holds. */
  got_cap = n->nb_max_items;
  got_list = calloc(got_cap, sizeof(*got_list));
  nb_got = 0;
  subtree_forall_entries(n->subtree, got_cb, NULL);

  /* Dedup the expected set in place (first of each key wins) and compare. */
  for (size_t i = 0; i < nb_exp; i++) {
    unsigned char from_src[4096], from_n[4096];
    struct item_metadata *ms, *mn;
    size_t j;

    if (i > 0 && exp_list[i].key == exp_list[i - 1].key) {
      /* Valid in both sources: only a missed invalidation produces this, and
       * it is the case the source precedence exists for. */
      cold_dup_keys++;
      continue;
    }
    j = want++;
    if (j >= nb_got) {
      check(false, "N is missing key %lu", exp_list[i].key);
      continue;
    }
    if (got_list[j].key != exp_list[i].key) {
      check(false, "N entry %zu is key %lu, expected %lu", j, got_list[j].key,
            exp_list[i].key);
      continue;
    }
    check((got_list[j].slot & (1u << 31)) == 0, "N entry %zu is marked stale",
          j);
    if (!read_slot(exp_list[i].src, exp_list[i].slot, from_src) ||
        !read_slot(n, GET_SIDX(got_list[j].slot), from_n)) {
      check(false, "could not read back key %lu", exp_list[i].key);
      continue;
    }
    ms = (struct item_metadata *)from_src;
    mn = (struct item_metadata *)from_n;
    check(memcmp(from_src, from_n, n->item_size) == 0,
          "key %lu differs between source and N", exp_list[i].key);
    check(item_is_tombstone(ms) == item_is_tombstone(mn),
          "key %lu lost its tombstone flag", exp_list[i].key);
    if (item_is_tombstone(mn)) cold_tombstones++;
    check(exp_list[i].key >= n->min && exp_list[i].key <= n->max,
          "key %lu outside N's range [%lu, %lu]", exp_list[i].key, n->min,
          n->max);

    /*
     * The plan's round trip: an entry that survived into N should be the
     * authoritative one, because a moving update invalidates the copy it
     * superseded. A mismatch means that hint was missed -- N then holds a
     * stale entry, which is shadowed by the newer copy nearer the leaf.
     */
    if (!item_is_tombstone(mn)) {
      uint64_t in_n =
          *(uint64_t *)(from_n + sizeof(*mn) + sizeof(uint64_t));

      read_back_quiet(exp_list[i].key);
      if (!rb_found || rb_value != in_n) cold_read_mismatches++;
    }
    cold_entries++;
  }
  check(want == nb_got, "N holds %zu entries, expected %zu", nb_got, want);
  check(n->nb_items == build.count && build.count == nb_got,
        "N's counters disagree (nb_items %zu, count %zu, entries %zu)",
        n->nb_items, build.count, nb_got);
  check(atomic_load(&n->full) == 1, "N is not marked full");
  check(atomic_load(&build.node->child_flag) == 1,
        "N's node is not marked fully split");

  free(exp_list);
  free(got_list);
  prune_build_discard(&build);
  check(build.slab == NULL, "discard left the build populated");
}

static void check_empty_build(const struct prune_candidate *c) {
  struct prune_build build;
  int error = prune_build_begin(c, &build);

  if (error) {
    check(false, "prune_build_begin failed with %d", error);
    return;
  }
  error = prune_build_finish(&build);
  check(error == 0, "finishing an empty N failed with %d", error);
  check(build.count == 0, "empty N holds %zu slots", build.count);
  check(build.slab->min == (uint64_t)-1 && build.slab->max == 0,
        "empty N has a range [%lu, %lu]", build.slab->min, build.slab->max);
  check(atomic_load(&build.slab->full) == 1, "empty N is not marked full");
  prune_build_discard(&build);
  printf("  %-34s ok\n", "all-stale triple (empty N)");
}

/*
 * Whether a triple is *prunable* is selection's business; the merge just
 * copies. So this exercises every leaf -> inner -> outer chain whose two
 * internal slabs could fit in one, prunable or not, which is what gets rich
 * sources (many entries, tombstones, both orientations) instead of only the
 * fully-stale triples selection happens to like.
 */
static bool chain_candidate(centree_node leaf, struct prune_candidate *c) {
  centree_node inner = centree_lu_parent(leaf);
  centree_node outer = inner ? centree_lu_parent(inner) : NULL;

  if (!inner || !outer) return false;
  if (atomic_load(&leaf->removed) || atomic_load(&inner->removed) ||
      atomic_load(&outer->removed))
    return false;
  if (atomic_load(&inner->child_flag) != 1 ||
      atomic_load(&outer->child_flag) != 1)
    return false;

  memset(c, 0, sizeof(*c));
  c->leaf = leaf;
  c->inner = inner;
  c->outer = outer;
  c->up = centree_lu_parent(outer);
  c->side = inner->lu_child[CENTREE_LU_RIGHT] == leaf ? CENTREE_LU_RIGHT
                                                      : CENTREE_LU_LEFT;
  c->sib = inner->lu_child[!c->side];
  c->star = outer->lu_child[c->side];
  c->cold_bound =
      inner->value.slab->nb_items + outer->value.slab->nb_items;
  return c->cold_bound <= leaf->value.slab->nb_max_items;
}

static void check_cold_builds(void) {
  size_t total = tnt_get_node_count();
  size_t cap = total + 8;
  centree_node *order = calloc(cap, sizeof(*order));
  size_t nb_order = 0, built = 0, rich = 0;

  collect_routing(routing_root(), order, cap, &nb_order);
  for (size_t i = 0; i < nb_order; i += 2) {
    struct prune_candidate c;

    if (!chain_candidate(order[i], &c)) continue;
    if (built == 0) check_empty_build(&c);
    check_one_cold_build(&c);
    if (c.cold_bound > 0) rich++;
    built++;
  }
  check(built > 0, "no history chain to build a merged slab from");
  check(rich > 0, "every merged slab came out empty");
  check(cold_tombstones > 0, "no tombstone was ever carried into N");
  printf("  %-34s %zu builds (%zu non-empty), %zu entries (%zu tombstones), "
         "%zu stale-but-unmarked, %zu duplicated across sources\n",
         "merged slab N", built, rich, cold_entries, cold_tombstones,
         cold_read_mismatches, cold_dup_keys);
  free(order);
}

/* --------------------------- a blocked writer released by retirement */

/*
 * A writer whose leaf is frozen parks on the leaf's child_flag waiting for the
 * split that will never come. Retirement is the second way out: the pruner
 * sets removed and wakes the waiters, and the writer must restart its descent.
 *
 * The sequence below leaves child_flag at 0 throughout, so the removed check
 * is the *only* thing that can release the writer. It also makes the slab
 * writable again before the wake-up, which is what lets the restart finish
 * here without an actual routing splice (that is a later step).
 */
static void wake_child_flag(centree_node n) {
  syscall(SYS_futex, &n->child_flag, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static centree_node leaf_for_key(uint64_t key) {
  centree_node n = routing_root();

  while (n) {
    centree_node next = key < centree_pivot_load(n) ? tnt_routing_left(n)
                                                    : tnt_routing_right(n);
    if (!next) break;
    n = next;
  }
  return n;
}

static void check_blocked_writer(uint64_t key) {
  centree_node leaf = leaf_for_key(key);
  struct req r = {0};
  struct slab *s;
  long frozen;

  if (!leaf) {
    check(false, "no routing leaf for key %lu", key);
    return;
  }
  s = leaf->value.slab;
  frozen = slab_freeze(s, s->nb_max_items);
  if (frozen < 0) {
    check(false, "freeze of leaf slab %lu returned %ld", s->seq, frozen);
    return;
  }

  r.item = make_item(key, 0xbeef);
  r.cb.cb = write_done;
  r.cb.item = (char *)r.item;
  r.cb.fsst_idx = -1;
  atomic_store(&r.done, 0);
  kv_upsert_async(&r.cb);

  usleep(100000);
  check(!atomic_load(&r.done),
        "writer completed although its leaf was frozen and childless");

  /* Writable again, but nobody has been woken: a parked writer stays parked. */
  atomic_store(&s->last_item, (size_t)frozen);
  atomic_store(&s->full, 0);
  usleep(50000);
  check(!atomic_load(&r.done), "writer was spinning on child_flag, not parked");

  /* Retire and wake, with child_flag still 0. */
  atomic_store(&leaf->removed, 1);
  wake_child_flag(leaf);

  for (int i = 0; i < 5000 && !atomic_load(&r.done); i++) usleep(1000);
  check(atomic_load(&r.done),
        "writer never restarted after its leaf was retired");
  atomic_store(&leaf->removed, 0);

  if (atomic_load(&r.done)) {
    read_back(key, 0xbeef);
    printf("  %-34s key %lu via slab %lu\n", "blocked writer restarted", key,
           s->seq);
  }
  free(r.item);
}

int main(int argc, char **argv) {
  if (argc > 1) nb_keys = strtoull(argv[1], NULL, 0);

  init_default_config(&cfg);
  cfg.kv_size = TEST_KV_SIZE;
  cfg.max_file_size = TEST_MAX_FILE_SIZE;
  cfg.page_cache_size = PAGE_SIZE * 8192; /* 32 MB */
  cfg.with_reins = 0;
  cfg.with_rebal = 0;
  cfg.nb_items_in_db = nb_keys;

  printf("== history links (%lu keys) ==\n", nb_keys);
  slab_workers_init(1, 4, 2);

  run_upserts(0, nb_keys);
  validate("after splits");
  check_selection("candidates after splits");

  check(tnt_rebalancing() >= 0, "tnt_rebalancing() failed");
  validate("after rebalance");
  check_selection("candidates after rebalance");

  /* Writes that land in the rebalanced tree must keep history consistent. */
  run_upserts(nb_keys, nb_keys + nb_keys / 2);
  validate("after post-rebalance writes");

  /*
   * Overwrites are what make a triple prunable: every moving update
   * invalidates the older copy, draining the internal slabs until three of
   * them fit in one. Only two thirds of the keys are rewritten, so the
   * internal slabs stay *partially* stale and the merge has something to
   * copy; deleting a seventh of them leaves valid tombstones behind, which
   * pruning must carry over.
   */
  run_ops(0, nb_keys, 10, 0, 2, 0, 1); /* 90% of the keys */
  /*
   * The deleted tenth is exactly the set the overwrite passes leave alone, so
   * those tombstones stay authoritative, and the splits the next pass causes
   * carry them into internal slabs.
   */
  run_ops(0, nb_keys, 10, 0, 0, 1, 0);
  run_ops(0, nb_keys, 10, 0, 3, 0, 1);
  validate("after overwrites");
  check_selection("candidates after overwrites");
  check_cold_builds();
  validate("after cold builds");

  /* An early key sits in an internal slab, a late one in its leaf. */
  check_read_restart(0);
  check_read_restart(nb_keys + nb_keys / 2 - 1);

  check_blocked_writer(nb_keys / 2);
  validate("after blocked-writer restart");

  /* The rule has two orientations and a history-root case; cover all three. */
  check(cov_candidates > 0, "no prunable triple was ever selected");
  check(cov_side[0] > 0 && cov_side[1] > 0,
        "only one orientation was covered (side0 %zu, side1 %zu)", cov_side[0],
        cov_side[1]);
  check(cov_history_root > 0, "no candidate with D == NULL was covered");

  if (failures) {
    printf("== %lu failures ==\n", failures);
    return 1;
  }
  printf("== ok ==\n");
  return 0;
}
